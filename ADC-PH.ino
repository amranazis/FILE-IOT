 
#define PH_PIN 34

const int numSamples = 20;

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
}

void loop() {
  long totalADC = 0;

  // Membaca ADC sebanyak 20 kali
  for (int i = 0; i < numSamples; i++) {
    totalADC += analogRead(PH_PIN);
    delay(20);
  }

  float adc = totalADC / (float)numSamples;
  float voltage = adc * 3.3 / 4095.0;

  Serial.print("ADC: ");
  Serial.print(adc, 2);

  Serial.print(" | Voltage: ");
  Serial.print(voltage, 3);
  Serial.println(" V");

  delay(1000);
}
