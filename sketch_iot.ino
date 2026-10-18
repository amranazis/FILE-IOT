#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <NewPing.h>
#include <HTTPClient.h>
#include <time.h>

// ===== KONFIGURASI WiFi =====
const char* WIFI_SSID     = "HOME";
const char* WIFI_PASSWORD = "halow1f1";

// ===== HIVEMQ CLOUD =====
const char* MQTT_HOST   = "6d061a42c1a7425db4e6977204696d50.s1.eu.hivemq.cloud";
const int   MQTT_PORT   = 8883;
const char* MQTT_USER   = "esp32";
const char* MQTT_PASS   = "Monitoring2026";
const char* MQTT_CLIENT = "esp32-sawah-001";
const char* TOPIC_DATA   = "sawah/data";
const char* TOPIC_STATUS = "sawah/status";

// ===== FIREBASE =====
const char* FIREBASE_HOST   = "https://monitoring-petanian-default-rtdb.asia-southeast1.firebasedatabase.app";
const char* FIREBASE_SECRET = "MlX0xuTPJjjBkcmFlNavhm1cM9u7FOEWH43XxDCU";

// ===== PIN =====
#define PIN_PH_AOUT    34
#define PIN_KELEMBAPAN 32
#define PIN_TRIG        5
#define PIN_ECHO       18
#define MAX_JARAK      300

// ===== KALIBRASI =====
const int   MOIST_KERING      = 2682;
const int   MOIST_BASAH       = 1107;


// ===== INTERVAL =====
// PERBAIKAN: kembalikan ke 5 detik. 1 detik terlalu sering dan
// membebani ESP32 (SSL handshake + HTTP berat untuk loop secepat itu)
const long INTERVAL_DATA     = 5000;
const long INTERVAL_HB       = 10000;
const long INTERVAL_FIREBASE = 30000;

WiFiClientSecure secureClient;
PubSubClient     mqtt(secureClient);
NewPing          sonar(PIN_TRIG, PIN_ECHO, MAX_JARAK);

unsigned long lastData = 0, lastHB = 0, lastFirebase = 0;
bool waktuSinkron = false; // PERBAIKAN: flag supaya tau apakah NTP berhasil

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Monitoring Sawah (HiveMQ Cloud) ===");
  Serial.printf("Free heap saat boot: %d bytes\n", ESP.getFreeHeap());

  connectWiFi();

  // NTP — PERBAIKAN: retry lebih banyak & set flag waktuSinkron
  configTime(25200, 0, "pool.ntp.org", "time.google.com");
  Serial.print("Sinkron NTP");
  struct tm t;
  int retry = 0;
  while (!getLocalTime(&t) && retry < 20) {
    delay(500); Serial.print("."); retry++;
  }
  waktuSinkron = getLocalTime(&t);
  Serial.println(waktuSinkron ? "\nWaktu OK" : "\nWaktu GAGAL - timestamp akan pakai millis");

  secureClient.setInsecure();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
  connectMQTT();
}

// ==================== LOOP ====================
void loop() {
  // PERBAIKAN: cek heap memory tiap loop, print jika rendah (debug crash)
  static unsigned long lastHeapCheck = 0;
  if (millis() - lastHeapCheck > 5000) {
    lastHeapCheck = millis();
    Serial.printf("[DEBUG] Free heap: %d bytes\n", ESP.getFreeHeap());
  }

  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();

  unsigned long now = millis();

  if (now - lastData >= INTERVAL_DATA) {
    lastData = now;
    kirimData();
  }

  if (now - lastHB >= INTERVAL_HB) {
    lastHB = now;
    mqtt.publish(TOPIC_STATUS, "Online", true);
  }
}

// ==================== BACA SENSOR ====================
float bacaPH() {
  long total = 0;
  for (int i = 0; i < 5; i++) { total += analogRead(PIN_PH_AOUT); delay(5); }
  float voltage = (total / 5.0) * (3.3 / 4095.0);

  // ===== KALIBRASI 2 TITIK =====
  // pH 4.01 → ADC 438 → voltage 0.3529V
  // pH 6.89 → ADC 197 → voltage 0.1588V
  const float SLOPE  = -14.84;
  const float OFFSET =   9.248;

  float ph = (SLOPE * voltage) + OFFSET;
  ph = constrain(ph, 0.0, 14.0);

  Serial.printf("[pH DEBUG] raw=%.0f voltage=%.4fV pH=%.2f\n",
                total / 5.0, voltage, ph);
  return ph;
}
float bacaKelembapan() {
  long total = 0;
  // PERBAIKAN: kurangi dari 5x delay 10ms (50ms) jadi 3x delay 5ms (15ms)
  for (int i = 0; i < 3; i++) { total += analogRead(PIN_KELEMBAPAN); delay(5); }
  float persen = (float)(MOIST_KERING - (total / 3.0)) /
                 (float)(MOIST_KERING - MOIST_BASAH) * 90.0;
  return constrain(persen, 0.0, 100.0);
}

float bacaAir() {
  unsigned int jarak = sonar.ping_median(5);
  float cm = sonar.convert_cm(jarak);
  if (cm == 0) cm = MAX_JARAK;
  return cm;
}

// PERBAIKAN: jika NTP gagal, gunakan epoch perkiraan + millis
// supaya tidak selalu tahun 1970
long long getTimestamp() {
  struct tm t;
  if (getLocalTime(&t)) {
    time_t epoch = mktime(&t);
    // PERBAIKAN: cast ke (long long) SEBELUM dikali 1000
    // agar tidak overflow 32-bit. epoch x 1000 > 2 miliar = overflow di 'long'
    long long ts = (long long)epoch * 1000LL;
    Serial.printf("[TIME DEBUG] tahun=%d bulan=%d hari=%d jam=%d:%d epoch=%ld ts=%lld\n",
                  t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min,
                  (long)epoch, ts);
    return ts;
  }
  Serial.println("[TIME DEBUG] getLocalTime GAGAL saat dipanggil - pakai fallback");
  return 1750000000000LL + (long long)millis();
}

// ==================== KIRIM DATA ====================
void kirimData() {
  float ph         = bacaPH();
  float kelembapan = bacaKelembapan();
  float air        = bacaAir();

  Serial.printf("[DATA] pH: %.1f | Kelembapan: %.0f%% | Air: %.1f cm\n",
                ph, kelembapan, air);

  StaticJsonDocument<200> doc;
  doc["ph"]         = round(ph * 10) / 10.0;
  doc["kelembapan"] = (int)kelembapan;
  doc["air"]        = round(air * 10) / 10.0;

  char buffer[200];
  serializeJson(doc, buffer);

  bool ok = mqtt.publish(TOPIC_DATA, buffer);
  Serial.printf("[MQTT] %s\n", ok ? "OK" : "GAGAL");

  unsigned long now = millis();
  Serial.printf("[CEK] now=%lu lastFirebase=%lu selisih=%lu (butuh >=%ld)\n",
                now, lastFirebase, now - lastFirebase, INTERVAL_FIREBASE);

  if (now - lastFirebase >= INTERVAL_FIREBASE) {
    lastFirebase = now;
    Serial.println("[CEK] Saatnya simpan ke Firebase...");
    simpanFirebase(ph, kelembapan, air);
  }
}

// ==================== SIMPAN FIREBASE ====================
void simpanFirebase(float ph, float kelembapan, float air) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Firebase] SKIP - WiFi tidak terhubung");
    return;
  }

  long long ts = getTimestamp();
  Serial.printf("[Firebase] Timestamp: %lld\n", ts);

  StaticJsonDocument<200> doc;
  doc["ph"]         = round(ph * 10) / 10.0;
  doc["kelembapan"] = (int)kelembapan;
  doc["air"]        = round(air * 10) / 10.0;
  doc["timestamp"]  = ts;

  char json[200];
  serializeJson(doc, json);

  // PERBAIKAN: pakai WiFiClientSecure terpisah untuk HTTPS Firebase
  // supaya tidak konflik dengan koneksi SSL MQTT yang sedang aktif
  WiFiClientSecure httpsClient;
  httpsClient.setInsecure();

  HTTPClient http;

  String urlTerbaru = String(FIREBASE_HOST) + "/sensor/terbaru.json?auth=" + FIREBASE_SECRET;
  if (http.begin(httpsClient, urlTerbaru)) {
    http.addHeader("Content-Type", "application/json");
    int code1 = http.PUT(json);
    Serial.printf("[Firebase] Terbaru: %d\n", code1);
    if (code1 < 0) Serial.printf("[Firebase] Error: %s\n", http.errorToString(code1).c_str());
    http.end();
  } else {
    Serial.println("[Firebase] GAGAL begin() untuk /terbaru");
  }

  delay(100); // jeda kecil antar request HTTPS

  // PERBAIKAN: String(long long) tidak didukung langsung di Arduino,
  // pakai snprintf manual untuk konversi yang aman
  char tsStr[20];
  snprintf(tsStr, sizeof(tsStr), "%lld", ts);

  String urlRiwayat = String(FIREBASE_HOST) + "/sensor/riwayat/" +
                      String(tsStr) + ".json?auth=" + FIREBASE_SECRET;
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

void connectMQTT() {
  Serial.print("MQTT ke HiveMQ Cloud");
  int coba = 0;
  while (!mqtt.connected() && coba < 5) {
    if (mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS,
                     TOPIC_STATUS, 1, true, "Offline")) {
      Serial.println("\nMQTT OK!");
      mqtt.publish(TOPIC_STATUS, "Online", true);
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
