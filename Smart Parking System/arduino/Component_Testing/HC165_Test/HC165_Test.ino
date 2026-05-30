/*
 * 74HC165 Raw Read Test
 * Just reads the chip and prints all 16 bits every 200ms.
 * No LEDs, no Firebase, no anything else.
 *
 * With nothing pressed: should print all 1s
 * With slot 1 switch pressed: bit 0 should turn 0
 */

#define SR_IN_DATA    18
#define SR_IN_CLOCK   19
#define SR_IN_LOAD    23

byte buf[2];

void readChain() {
  digitalWrite(SR_IN_LOAD, LOW);
  delayMicroseconds(5);
  digitalWrite(SR_IN_LOAD, HIGH);

  for (int chip = 1; chip >= 0; chip--) {
    byte data = 0;
    for (int bit = 7; bit >= 0; bit--) {
      digitalWrite(SR_IN_CLOCK, LOW);
      delayMicroseconds(2);
      if (digitalRead(SR_IN_DATA) == HIGH) {
        data = data | (1 << bit);
      }
      digitalWrite(SR_IN_CLOCK, HIGH);
    }
    buf[chip] = data;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(SR_IN_DATA, INPUT);
  pinMode(SR_IN_CLOCK, OUTPUT);
  pinMode(SR_IN_LOAD, OUTPUT);
  digitalWrite(SR_IN_LOAD, HIGH);
  digitalWrite(SR_IN_CLOCK, LOW);

  Serial.println("=== 74HC165 Raw Test ===");
  Serial.println("Format: [chip0 bits 7..0] [chip1 bits 7..0]");
  Serial.println("All 1s = nothing pressed (good)");
  Serial.println("Press a switch — its bit should turn 0");
  Serial.println();
}

void loop() {
  readChain();
  Serial.print("Chip0: ");
  for (int i = 7; i >= 0; i--) Serial.print((buf[0] >> i) & 1);
  Serial.print("  Chip1: ");
  for (int i = 7; i >= 0; i--) Serial.print((buf[1] >> i) & 1);
  Serial.println();
  delay(200);
}
