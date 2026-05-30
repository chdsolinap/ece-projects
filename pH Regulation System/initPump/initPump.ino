// Simple priming sketch – fills both tubes
const int PUMP_DOWN = 7;
const int PUMP_UP   = 8;
const unsigned long PULSE_MS = 100;      // latching pulse
const unsigned long PRIME_MS  = 5000;    // 5 seconds of pumping

void setup() {
  Serial.begin(9600);
  pinMode(PUMP_DOWN, OUTPUT);
  pinMode(PUMP_UP, OUTPUT);
  digitalWrite(PUMP_DOWN, HIGH);
  digitalWrite(PUMP_UP, HIGH);
  
  Serial.println("Priming pH‑down pump for 5 seconds...");
  digitalWrite(PUMP_DOWN, LOW); delay(PULSE_MS); digitalWrite(PUMP_DOWN, HIGH); // ON
  delay(PRIME_MS);
  digitalWrite(PUMP_DOWN, LOW); delay(PULSE_MS); digitalWrite(PUMP_DOWN, HIGH); // OFF
  
  delay(1000);
  
  Serial.println("Priming pH‑up pump for 5 seconds...");
  digitalWrite(PUMP_UP, LOW); delay(PULSE_MS); digitalWrite(PUMP_UP, HIGH); // ON
  delay(PRIME_MS);
  digitalWrite(PUMP_UP, LOW); delay(PULSE_MS); digitalWrite(PUMP_UP, HIGH); // OFF
  
  Serial.println("Done. Disconnect USB, then re‑upload normal code.");
}

void loop() {}