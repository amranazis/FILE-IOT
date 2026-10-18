//==========================================
// ESP32 - Kalibrasi Capacitive Soil Moisture
// Sensor : Capacitive Soil Moisture V2.0
// Pin AO -> GPIO32
//==========================================

#define SOIL_PIN 32

const int samples = 20;
  
void setup() {
  Serial.begin(115200);

  analogReadResolution(12);          // ADC 0-4095
  analogSetAttenuation(ADC_11db);    // Rentang hingga ±3.3V
}

void loop() {

  long total = 0;

  // Ambil beberapa sampel agar stabil
  for(int i = 0; i < samples; i++) {
    total += analogRead(SOIL_PIN);
    delay(20);
  }

  float adc = total / (float)samples;

  float voltage = adc * 3.3 / 4095.0;

  Serial.println(" ");
  Serial.print("ADC      : ");
  Serial.println(adc, 2);

  Serial.print("Voltage  : ");
  Serial.print(voltage, 3);
  Serial.println(" V");

  delay(1000);
}