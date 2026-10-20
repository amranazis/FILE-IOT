// ==========================================================
//  MONITORING SAWAH (ESP32) - versi Firebase Auth
//  Membaca sensor pH, kelembapan tanah, dan level air (ultrasonik),
//  lalu mengirim data ke:
//   - HiveMQ Cloud (MQTT)  -> data realtime tiap 5 detik
//   - Firebase RTDB (HTTP) -> data terbaru + riwayat tiap 30 detik
//     (login memakai email/password Firebase Auth, bukan database secret)
// ==========================================================

#include <WiFi.h>             // Koneksi WiFi ESP32
#include <PubSubClient.h>     // Library client MQTT
#include <WiFiClientSecure.h> // Koneksi TLS/SSL (dipakai MQTT 8883 & HTTPS)
#include <ArduinoJson.h>      // Membuat & membaca JSON
#include <NewPing.h>          // Library sensor ultrasonik (HC-SR04)
#include <HTTPClient.h>       // Request HTTP/HTTPS ke Firebase
#include <time.h>             // Fungsi waktu (NTP)

// ===== KONFIGURASI WiFi =====
const char* WIFI_SSID     = "HOME";
const char* WIFI_PASSWORD = "halow1f1";

// ===== HIVEMQ CLOUD =====
const char* MQTT_HOST   = "6d061a42c1a7425db4e6977204696d50.s1.eu.hivemq.cloud";
const int   MQTT_PORT   = 8883;              // Port MQTT over TLS
const char* MQTT_USER   = "esp32";
const char* MQTT_PASS   = "Monitoring2026";
const char* MQTT_CLIENT = "esp32-sawah-001"; // ID unik perangkat di broker
const char* TOPIC_DATA   = "sawah/data";     // Topik data sensor
const char* TOPIC_STATUS = "sawah/status";   // Topik status Online/Offline

// ===== FIREBASE =====
//URL Realtime Database ada di build - realtime database - data
const char* FIREBASE_HOST = "https://monitoring-petanian-default-rtdb.asia-southeast1.firebasedatabase.app";

// FIREBASE_API_KEY sama dengan firebaseConfig.apiKey di web (firebase-config.js)

const char* FIREBASE_API_KEY   = "AIzaSyDePAftAaQ8nFzkCcq-tG7PeGJ0VZx2Sh8";
const char* DEVICE_EMAIL       = "esp32@monitoring.com";  // Akun khusus perangkat di Firebase Auth
const char* DEVICE_PASSWORD    = "12345678"; // isi dengan pasword user yang ditetapkan di console firebase

String        firebaseIdToken   = "";   // Token hasil login (dipakai di ?auth=...)
unsigned long lastLogin         = 0;    // Waktu login terakhir yang berhasil
const long    INTERVAL_LOGIN    = 50UL * 60UL * 1000UL; // re-login tiap 50 menit (token expire ~1 jam)

// ===== PIN =====
#define PIN_PH_AOUT    34   // Output analog sensor pH
#define PIN_KELEMBAPAN 32   // Output analog sensor kelembapan tanah
#define PIN_TRIG        5   // Trigger sensor ultrasonik
#define PIN_ECHO       18   // Echo sensor ultrasonik
#define MAX_JARAK      300  // Jarak maksimum baca ultrasonik (cm)

// ===== KALIBRASI =====
const int   MOIST_KERING      = 2682; // Nilai ADC saat sensor di udara/kering
const int   MOIST_BASAH       = 1107; // Nilai ADC saat sensor di air/basah


// ===== INTERVAL (ms) =====
const long INTERVAL_DATA     = 5000;   // Kirim data ke MQTT
const long INTERVAL_HB       = 10000;  // Heartbeat status "Online"
const long INTERVAL_FIREBASE = 30000;  // Simpan ke Firebase

WiFiClientSecure secureClient;          // Client TLS untuk MQTT
PubSubClient     mqtt(secureClient);    // Client MQTT memakai koneksi TLS di atas
NewPing          sonar(PIN_TRIG, PIN_ECHO, MAX_JARAK); // Objek sensor ultrasonik

// Penanda waktu eksekusi terakhir (timer non-blocking berbasis millis)
unsigned long lastData = 0, lastHB = 0, lastFirebase = 0;
bool waktuSinkron = false; // true jika sinkronisasi NTP berhasil

// ==================== SETUP ====================
// Dijalankan sekali saat ESP32 menyala/restart
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Monitoring Sawah (HiveMQ Cloud) ===");
  Serial.printf("Free heap saat boot: %d bytes\n", ESP.getFreeHeap());

  connectWiFi(); // Sambungkan ke WiFi

  // Sinkron waktu via NTP, zona WIB (UTC+7 = 25200 detik)
  configTime(25200, 0, "pool.ntp.org", "time.google.com");
  Serial.print("Sinkron NTP");
  struct tm t;
  int retry = 0;
  // Coba hingga 20x (maks ~10 detik) sampai waktu berhasil didapat
  while (!getLocalTime(&t) && retry < 20) {
    delay(500); Serial.print("."); retry++;
  }
  waktuSinkron = getLocalTime(&t);
  Serial.println(waktuSinkron ? "\nWaktu OK" : "\nWaktu GAGAL - timestamp akan pakai millis");

  // Login ke Firebase Auth SEBELUM mulai kirim data (maks 5x percobaan)
  Serial.println("Login ke Firebase Auth...");
  int retryLogin = 0;
  while (!loginFirebase() && retryLogin < 5) {
    delay(2000);
    retryLogin++;
  }
  if (firebaseIdToken == "") {
    Serial.println("[Firebase Auth] PERINGATAN: login gagal setelah beberapa percobaan. Data sensor tetap dikirim via MQTT, tapi TIDAK akan tersimpan ke Firebase sampai login berhasil.");
  }

  secureClient.setInsecure();          // Lewati verifikasi sertifikat (praktis, tapi kurang aman)
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);             // Perbesar buffer MQTT
  connectMQTT();                       // Sambungkan ke broker
}

// ==================== LOOP ====================
// Dijalankan berulang; mengatur jadwal login ulang, kirim data & heartbeat
void loop() {
  // Cetak sisa memori heap tiap 5 detik (membantu mendeteksi memory leak/crash)
  static unsigned long lastHeapCheck = 0;
  if (millis() - lastHeapCheck > 5000) {
    lastHeapCheck = millis();
    Serial.printf("[DEBUG] Free heap: %d bytes\n", ESP.getFreeHeap());
  }

  if (!mqtt.connected()) connectMQTT(); // Sambung ulang jika terputus
  mqtt.loop();                          // Jaga koneksi MQTT tetap hidup

  unsigned long now = millis();

  // Re-login berkala supaya idToken tidak pernah expire (Firebase idToken
  // umumnya valid 1 jam, kita refresh tiap 50 menit untuk jaga-jaga)
  if (now - lastLogin >= INTERVAL_LOGIN) {
    loginFirebase();
  }

  // Kirim data sensor tiap INTERVAL_DATA
  if (now - lastData >= INTERVAL_DATA) {
    lastData = now;
    kirimData();
  }

  // Kirim status "Online" tiap INTERVAL_HB (retained)
  if (now - lastHB >= INTERVAL_HB) {
    lastHB = now;
    mqtt.publish(TOPIC_STATUS, "Online", true);
  }
}

// ==================== FIREBASE AUTH ====================
// Login ke Firebase Auth lewat REST API (email + password), lalu menyimpan
// idToken di firebaseIdToken. Return true jika login berhasil.
// PERBAIKAN (keamanan): login pakai email/password Firebase Auth biasa,
// menggantikan Legacy Database Secret yang bypass Security Rules.
bool loginFirebase() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  // Endpoint Firebase Auth untuk sign-in dengan email/password
  String url = "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=" + String(FIREBASE_API_KEY);

  // Susun body JSON: email, password, dan minta token dikembalikan
  StaticJsonDocument<256> reqDoc;
  reqDoc["email"]            = DEVICE_EMAIL;
  reqDoc["password"]         = DEVICE_PASSWORD;
  reqDoc["returnSecureToken"] = true;
  String body;
  serializeJson(reqDoc, body);

  bool ok = false;
  if (http.begin(client, url)) {
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);

    if (code == 200) { // 200 = login sukses
      String resp = http.getString();
      StaticJsonDocument<1024> respDoc;
      DeserializationError err = deserializeJson(respDoc, resp);
      if (!err && respDoc.containsKey("idToken")) {
        firebaseIdToken = respDoc["idToken"].as<String>(); // Simpan token
        lastLogin = millis();                              // Catat waktu login
        Serial.println("[Firebase Auth] Login OK, token diperbarui");
        ok = true;
      } else {
        Serial.println("[Firebase Auth] Gagal parse response login");
      }
    } else {
      Serial.printf("[Firebase Auth] Login gagal, HTTP code: %d\n", code);
      Serial.println(http.getString()); // Tampilkan pesan error dari Firebase
    }
    http.end();
  } else {
    Serial.println("[Firebase Auth] GAGAL begin() untuk login");
  }
  return ok;
}

// ==================== BACA SENSOR ====================

// Membaca pH: rata-rata 5 sampel ADC -> tegangan -> pH (kalibrasi 2 titik)
float bacaPH() {
  long total = 0;
  for (int i = 0; i < 5; i++) { total += analogRead(PIN_PH_AOUT); delay(5); }
  float voltage = (total / 5.0) * (3.3 / 4095.0); // Konversi ADC 12-bit ke volt

  // Konstanta garis kalibrasi: pH = SLOPE * tegangan + OFFSET
  const float SLOPE  = -11.06;
  const float OFFSET =   7.90;

  float ph = (SLOPE * voltage) + OFFSET;
  ph = constrain(ph, 0.0, 14.0); // Batasi hasil ke rentang pH 0-14

  Serial.printf("[pH DEBUG] raw=%.0f voltage=%.4fV pH=%.2f\n",
                total / 5.0, voltage, ph);
  return ph;
}

// Membaca kelembapan tanah (%): rata-rata 3 sampel, dipetakan dari nilai kalibrasi
float bacaKelembapan() {
  long total = 0;
  for (int i = 0; i < 3; i++) { total += analogRead(PIN_KELEMBAPAN); delay(5); }
  // Semakin kecil nilai ADC = semakin basah; dipetakan ke skala 0-100%
  float persen = (float)(MOIST_KERING - (total / 3.0)) /
                 (float)(MOIST_KERING - MOIST_BASAH) * 100.0;
  return constrain(persen, 0.0, 100.0); // Batasi 0-100%
}

// Membaca jarak permukaan air (cm) dengan sensor ultrasonik
float bacaAir() {
  unsigned int jarak = sonar.ping_median(5); // Median 5x ping untuk mengurangi noise
  float cm = sonar.convert_cm(jarak);        // Konversi waktu pantul ke cm
  if (cm == 0) cm = MAX_JARAK;               // 0 = tidak ada pantulan -> anggap jarak maksimum
  return cm;
}

// Mengambil timestamp dalam milidetik (epoch).
// Jika NTP gagal, pakai epoch perkiraan + millis agar tidak jadi tahun 1970.
long long getTimestamp() {
  struct tm t;
  if (getLocalTime(&t)) {
    time_t epoch = mktime(&t);
    long long ts = (long long)epoch * 1000LL; // Cast dulu agar tidak overflow 32-bit
    Serial.printf("[TIME DEBUG] tahun=%d bulan=%d hari=%d jam=%d:%d epoch=%ld ts=%lld\n",
                  t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min,
                  (long)epoch, ts);
    return ts;
  }
  Serial.println("[TIME DEBUG] getLocalTime GAGAL saat dipanggil - pakai fallback");
  return 1750000000000LL + (long long)millis(); // Fallback waktu perkiraan
}

// ==================== KIRIM DATA ====================
// Baca semua sensor, kirim ke MQTT, dan (tiap 30 detik) simpan ke Firebase
void kirimData() {
  float ph         = bacaPH();
  float kelembapan = bacaKelembapan();
  float air        = bacaAir();

  Serial.printf("[DATA] pH: %.1f | Kelembapan: %.0f%% | Air: %.1f cm\n",
                ph, kelembapan, air);

  // Susun payload JSON: nilai dibulatkan 1 desimal (kelembapan jadi integer)
  StaticJsonDocument<200> doc;
  doc["ph"]         = round(ph * 10) / 10.0;
  doc["kelembapan"] = (int)kelembapan;
  doc["air"]        = round(air * 10) / 10.0;

  char buffer[200];
  serializeJson(doc, buffer); // Ubah JSON menjadi string

  bool ok = mqtt.publish(TOPIC_DATA, buffer); // Publish ke broker
  Serial.printf("[MQTT] %s\n", ok ? "OK" : "GAGAL");

  unsigned long now = millis();
  Serial.printf("[CEK] now=%lu lastFirebase=%lu selisih=%lu (butuh >=%ld)\n",
                now, lastFirebase, now - lastFirebase, INTERVAL_FIREBASE);

  // Simpan ke Firebase hanya jika sudah lewat INTERVAL_FIREBASE
  if (now - lastFirebase >= INTERVAL_FIREBASE) {
    lastFirebase = now;
    Serial.println("[CEK] Saatnya simpan ke Firebase...");
    simpanFirebase(ph, kelembapan, air);
  }
}

// ==================== SIMPAN FIREBASE ====================
// Menyimpan data ke 2 lokasi: /sensor/terbaru (ditimpa) dan /sensor/riwayat/<timestamp>,
// memakai idToken sebagai autentikasi (?auth=<token>)
void simpanFirebase(float ph, float kelembapan, float air) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Firebase] SKIP - WiFi tidak terhubung");
    return;
  }

  // Kalau belum/gagal login, jangan kirim (idToken kosong = auth != null gagal,
  // request akan ditolak Security Rules dan cuma buang-buang request).
  if (firebaseIdToken == "") {
    Serial.println("[Firebase] SKIP - belum login (idToken kosong), coba login ulang...");
    loginFirebase();
    if (firebaseIdToken == "") return;
  }

  long long ts = getTimestamp();
  Serial.printf("[Firebase] Timestamp: %lld\n", ts);

  // Susun JSON data + timestamp
  StaticJsonDocument<200> doc;
  doc["ph"]         = round(ph * 10) / 10.0;
  doc["kelembapan"] = (int)kelembapan;
  doc["air"]        = round(air * 10) / 10.0;
  doc["timestamp"]  = ts;

  char json[200];
  serializeJson(doc, json);

  // Client HTTPS terpisah agar tidak bentrok dengan koneksi SSL MQTT
  WiFiClientSecure httpsClient;
  httpsClient.setInsecure();

  HTTPClient http;

  // 1) PUT ke /sensor/terbaru -> selalu berisi data paling baru
  String urlTerbaru = String(FIREBASE_HOST) + "/sensor/terbaru.json?auth=" + firebaseIdToken;
  if (http.begin(httpsClient, urlTerbaru)) {
    http.addHeader("Content-Type", "application/json");
    int code1 = http.PUT(json);
    Serial.printf("[Firebase] Terbaru: %d\n", code1); // 200 = sukses
    if (code1 == 401 || code1 == 403) {
      // Token ditolak/kedaluwarsa -> kosongkan agar login ulang
      Serial.println("[Firebase] Token ditolak, akan re-login di siklus berikutnya");
      firebaseIdToken = ""; // paksa login ulang di percobaan berikutnya
    }
    if (code1 < 0) Serial.printf("[Firebase] Error: %s\n", http.errorToString(code1).c_str());
    http.end();
  } else {
    Serial.println("[Firebase] GAGAL begin() untuk /terbaru");
  }

  delay(100); // Jeda kecil antar request HTTPS

  // Konversi long long ke string manual (String(long long) tidak didukung langsung)
  char tsStr[20];
  snprintf(tsStr, sizeof(tsStr), "%lld", ts);

  // 2) PUT ke /sensor/riwayat/<timestamp> -> arsip data (tiap data punya key unik)
  String urlRiwayat = String(FIREBASE_HOST) + "/sensor/riwayat/" +
                      String(tsStr) + ".json?auth=" + firebaseIdToken;
  if (http.begin(httpsClient, urlRiwayat)) {
    http.addHeader("Content-Type", "application/json");
    int code2 = http.PUT(json);
    Serial.printf("[Firebase] Riwayat: %d\n", code2);
    if (code2 < 0) Serial.printf("[Firebase] Error: %s\n", http.errorToString(code2).c_str());
    http.end();
  } else {
    Serial.println("[Firebase] GAGAL begin() untuk /riwayat");
  }
}

// ==================== KONEKSI ====================

// Menyambung ke WiFi (maks 20x coba ~10 detik); restart ESP32 jika gagal
void connectWiFi() {
  Serial.printf("WiFi: %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int coba = 0;
  while (WiFi.status() != WL_CONNECTED && coba < 20) {
    delay(500); Serial.print("."); coba++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi OK! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi GAGAL"); ESP.restart();
  }
}

// Menyambung ke broker MQTT (maks 5x coba); restart ESP32 jika gagal.
// Mengatur Last Will: broker otomatis menerbitkan "Offline" jika perangkat terputus.
void connectMQTT() {
  Serial.print("MQTT ke HiveMQ Cloud");
  int coba = 0;
  while (!mqtt.connected() && coba < 5) {
    if (mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS,
                     TOPIC_STATUS, 1, true, "Offline")) { // topic LWT, QoS 1, retained, pesan
      Serial.println("\nMQTT OK!");
      mqtt.publish(TOPIC_STATUS, "Online", true); // Tandai perangkat online
    } else {
      Serial.printf(" gagal, error: %d\n", mqtt.state());
      delay(3000); coba++;
    }
  }
  if (!mqtt.connected()) {
    Serial.println("MQTT gagal - restart...");
    ESP.restart();
  }
}
