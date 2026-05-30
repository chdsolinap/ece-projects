/*
 * ============================================================
 *  GATE DIAGNOSTIC SKETCH
 *  Upload this first to test sensors and servo individually.
 *  Open Serial Monitor at 115200 baud.
 * ============================================================
 *
 *  COMMANDS (type in Serial Monitor and press Enter):
 *    s1    → read US1 distance once
 *    s2    → read US2 distance once
 *    scan  → continuously print both sensors (Ctrl+Z to stop, re-send idle)
 *    idle  → stop continuous scan
 *    open  → rotate servo to 90° (open)
 *    close → rotate servo to 0°  (close)
 *    sweep → sweep servo 0→90→0 slowly so you can see it move
 *
 * ============================================================
 */

#include <ESP32Servo.h>

#define US1_TRIG  13
#define US1_ECHO  12
#define US2_TRIG  14
#define US2_ECHO  27
#define SERVO_PIN 26

Servo gateServo;
bool scanning = false;

long measureDistance(uint8_t trig, uint8_t echo) {
  digitalWrite(trig, LOW);
  delayMicroseconds(2);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);
  long dur = pulseIn(echo, HIGH, 30000);
  if (dur == 0) return -1;  // timeout = nothing detected
  return dur * 0.034 / 2;
}

void setup() {
  Serial.begin(115200);
  pinMode(US1_TRIG, OUTPUT); pinMode(US1_ECHO, INPUT);
  pinMode(US2_TRIG, OUTPUT); pinMode(US2_ECHO, INPUT);
  gateServo.attach(SERVO_PIN);
  gateServo.write(0);
  Serial.println("=== Gate Diagnostic Ready ===");
  Serial.println("Commands: s1, s2, scan, idle, open, close, sweep");
}

void loop() {
  // Continuous scan mode
  if (scanning) {
    long d1 = measureDistance(US1_TRIG, US1_ECHO);
    long d2 = measureDistance(US2_TRIG, US2_ECHO);
    Serial.print("US1: ");
    if (d1 < 0) Serial.print("no echo"); else { Serial.print(d1); Serial.print(" cm"); }
    Serial.print("   |   US2: ");
    if (d2 < 0) Serial.print("no echo"); else { Serial.print(d2); Serial.print(" cm"); }
    Serial.println();
    delay(300);
  }

  // Command handler
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd == "s1") {
      long d = measureDistance(US1_TRIG, US1_ECHO);
      Serial.print("US1 distance: ");
      if (d < 0) Serial.println("no echo (check wiring)");
      else { Serial.print(d); Serial.println(" cm"); }
    }
    else if (cmd == "s2") {
      long d = measureDistance(US2_TRIG, US2_ECHO);
      Serial.print("US2 distance: ");
      if (d < 0) Serial.println("no echo (check wiring)");
      else { Serial.print(d); Serial.println(" cm"); }
    }
    else if (cmd == "scan") {
      scanning = true;
      Serial.println("Scanning... send 'idle' to stop.");
    }
    else if (cmd == "idle") {
      scanning = false;
      Serial.println("Scan stopped.");
    }
    else if (cmd == "open") {
      gateServo.write(90);
      Serial.println("Servo → 90° (open)");
    }
    else if (cmd == "close") {
      gateServo.write(0);
      Serial.println("Servo → 0° (close)");
    }
    else if (cmd == "sweep") {
      Serial.println("Sweeping 0 → 90 → 0...");
      for (int a = 0; a <= 90; a++) { gateServo.write(a); delay(15); }
      delay(500);
      for (int a = 90; a >= 0; a--) { gateServo.write(a); delay(15); }
      Serial.println("Sweep done.");
    }
    else {
      Serial.println("Unknown command. Try: s1, s2, scan, idle, open, close, sweep");
    }
  }
}
