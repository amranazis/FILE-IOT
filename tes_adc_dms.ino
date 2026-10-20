// ==========================================================
//  TES PEMBACAAN ADC MODUL DMS (pin D34 dan D13)
//  Tujuan: memastikan sinyal apa yang keluar dari kabel
//  kuning (D34) dan hijau (D13) pada modul DMS.
//
//  PENTING: sketch ini SENGAJA tanpa WiFi. D13 adalah pin ADC2,
//  dan ADC2 tidak bisa dibaca analog saat WiFi aktif.
// ==========================================================

#define PIN_DMS_34 34   // Kabel kuning/ungu  (GPIO34, ADC1, input-only)
#define PIN_DMS_13 13   // Kabel hijau/biru   (GPIO13, ADC2)

const int JUMLAH_SAMPEL = 10; // Jumlah sampel untuk rata-rata

// Menghitung rata-rata ADC dari satu pin
float bacaRata(int pin) {
  long total = 0;
  for (int i = 0; i < JUMLAH_SAMPEL; i++) {
    total += analogRead(pin);
    delay(2);
  }
  return total / (float)JUMLAH_SAMPEL;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  analogReadResolution(12);        // ADC 12-bit: 0 - 4095
  analogSetAttenuation(ADC_11db);  // Rentang baca sekitar 0 - 3.3 V

  pinMode(PIN_DMS_34, INPUT);
  pinMode(PIN_DMS_13, INPUT);

  Serial.println("=== TES ADC DMS (tanpa WiFi) ===");
  Serial.println("Format: D34 raw | volt   ||   D13 raw | volt | digital");
}

void loop() {
  float raw34 = bacaRata(PIN_DMS_34);
  float raw13 = bacaRata(PIN_DMS_13);

  float v34 = raw34 * (3.3 / 4095.0); // Konversi ADC ke volt
  float v13 = raw13 * (3.3 / 4095.0);

  int dig13 = digitalRead(PIN_DMS_13); // Status HIGH/LOW pin 13 (untuk cek apakah sinyal digital)

  Serial.printf("D34: %7.1f | %.3f V   ||   D13: %7.1f | %.3f V | dig=%d\n",
                raw34, v34, raw13, v13, dig13);

  delay(500);
}
