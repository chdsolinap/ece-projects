/*
 * Smart Hydroponics pH Control System
 * Final firmware - standalone version (no laptop required at runtime)
 *
 * Course:   ECE 0323.1 - Feedback and Control System (Laboratory)
 * Project:  Closed-loop pH regulation for hydroponic water
 *
 * Hardware:
 *   Arduino Uno (powered via barrel jack from 12V supply)
 *   DFRobot SEN0161 Gravity Analog pH Sensor (probe on A0)
 *   16x2 I2C LCD on SDA/SCL (A4/A5)
 *   2-channel relay module on D7 (pH-down) and D8 (pH-up)
 *   2x INTLLAB 12V peristaltic pumps switched through the relays
 *
 * Relay behavior:
 *   The relay module operates with TOGGLE pulse semantics.
 *   Each LOW->HIGH pulse on the IN pin inverts the relay state.
 *   At boot, the firmware assumes pumps are OFF (verified to be true in
 *   practice on this hardware) and tracks their state thereafter.
 *
 * Control strategy:
 *   On-off control with deadband and dead-time compensation.
 *     - When pH > setpoint + deadband/2, dose pH-down (acid).
 *     - When pH < setpoint - deadband/2, dose pH-up   (base).
 *     - After every dose, wait DOSE_WAIT_MS for mixing and sensor settling.
 *     - Minimum interval between doses is enforced as a safety floor.
 *   No control action is taken while inside the deadband.
 *
 * Calibration:
 *   Two-point linear calibration using pH 4.01 and pH 6.86 buffers.
 *   pH = PH_SLOPE * voltage + PH_OFFSET
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ====================================================================
// CONFIGURATION
// ====================================================================

LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---- Pin assignments ----
const int PH_PIN        = A0;
const int PUMP_DOWN_PIN = 7;
const int PUMP_UP_PIN   = 8;
const int LED_PIN       = 13;

// ---- Relay toggle pulse width ----
const unsigned long RELAY_PULSE_MS = 100;

// ---- pH calibration constants ----
// pH = PH_SLOPE * voltage + PH_OFFSET
const float PH_SLOPE  = 3.5;
const float PH_OFFSET = 0.06;

// ---- Sensor sampling ----
const int           PH_SAMPLES               = 40;
const unsigned long SAMPLING_INTERVAL_MS     = 20;
const unsigned long SERIAL_PRINT_INTERVAL_MS = 800;
const unsigned long LCD_UPDATE_INTERVAL_MS   = 500;

// ---- Control parameters ----
const float         SETPOINT             = 6.0;     // target pH
const float         DEADBAND             = 0.4;     // total band width
const unsigned long DOSE_MS              = 10;      // pump on time per dose
const unsigned long DOSE_WAIT_MS         = 10000;   // mixing + settle wait
const unsigned long MIN_DOSE_INTERVAL_MS = 10000;   // safety floor

// ---- Sensor sanity check ----
const float PH_MIN_VALID = 0.0;
const float PH_MAX_VALID = 14.0;

// ====================================================================
// STATE
// ====================================================================

bool pumpDownIsOn = false;
bool pumpUpIsOn   = false;

enum ControlState { IDLE, DOSING_DOWN, DOSING_UP, WAITING_AFTER_DOSE, FAULT };
ControlState state = IDLE;

unsigned long stateEnteredAt  = 0;
unsigned long lastDoseEndedAt = 0;

int   phBuffer[PH_SAMPLES];
int   phBufferIndex   = 0;
bool  phBufferFilled  = false;
float currentPh       = 7.0;
float currentVoltage  = 2.5;

unsigned long lastLcdUpdate    = 0;
unsigned long lastSerialPrint  = 0;
unsigned long lastSample       = 0;
unsigned long lastHeartbeat    = 0;

// ====================================================================
// HELPERS - pH sensor
// ====================================================================

// Trimmed-mean average: drops the highest and lowest samples to reject
// spikes, then averages the rest. Reduces ADC noise on the analog input.
float averagePhReading() {
  int n = phBufferFilled ? PH_SAMPLES : phBufferIndex;
  if (n <= 0) return 0;
  if (n < 5) {
    long sum = 0;
    for (int i = 0; i < n; i++) sum += phBuffer[i];
    return (float)sum / n;
  }
  int lo = phBuffer[0] < phBuffer[1] ? phBuffer[0] : phBuffer[1];
  int hi = phBuffer[0] < phBuffer[1] ? phBuffer[1] : phBuffer[0];
  long sum = 0;
  for (int i = 2; i < n; i++) {
    int v = phBuffer[i];
    if (v < lo)      { sum += lo; lo = v; }
    else if (v > hi) { sum += hi; hi = v; }
    else             { sum += v; }
  }
  return (float)sum / (n - 2);
}

void sampleAndUpdatePh() {
  unsigned long now = millis();
  if (now - lastSample < SAMPLING_INTERVAL_MS) return;
  lastSample = now;
  
  phBuffer[phBufferIndex++] = analogRead(PH_PIN);
  if (phBufferIndex >= PH_SAMPLES) {
    phBufferIndex = 0;
    phBufferFilled = true;
  }
  
  float avgAdc = averagePhReading();
  currentVoltage = avgAdc * 5.0 / 1024.0;
  currentPh = PH_SLOPE * currentVoltage + PH_OFFSET;
}

// ====================================================================
// HELPERS - relay (toggle semantics)
// ====================================================================

// Send one toggle pulse to a relay channel. This INVERTS the relay
// state from whatever it was before.
void pulseRelay(int pin) {
  digitalWrite(pin, LOW);
  delay(RELAY_PULSE_MS);
  digitalWrite(pin, HIGH);
}

// Drive a pump to a desired state. If the tracked state already matches
// the desired state, no pulse is sent. Prevents accidental re-toggling.
void setPumpState(int pin, bool &trackedState, bool desiredOn) {
  if (trackedState == desiredOn) return;
  pulseRelay(pin);
  trackedState = desiredOn;
}

// ====================================================================
// HELPERS - display
// ====================================================================

void lcdShow(float ph, const char* line2) {
  lcd.setCursor(0, 0);
  lcd.print("pH:");
  lcd.print(ph, 2);
  lcd.print(" SP:");
  lcd.print(SETPOINT, 1);
  lcd.print("   ");
  
  lcd.setCursor(0, 1);
  int len = 0;
  while (line2[len] != '\0' && len < 16) {
    lcd.print(line2[len]);
    len++;
  }
  while (len < 16) {
    lcd.print(' ');
    len++;
  }
}

void updateDisplay() {
  unsigned long now = millis();
  if (now - lastLcdUpdate < LCD_UPDATE_INTERVAL_MS) return;
  lastLcdUpdate = now;
  
  switch (state) {
    case IDLE:               lcdShow(currentPh, "Stable in band ");  break;
    case DOSING_DOWN:        lcdShow(currentPh, "Dosing DOWN    ");  break;
    case DOSING_UP:          lcdShow(currentPh, "Dosing UP      ");  break;
    case WAITING_AFTER_DOSE: lcdShow(currentPh, "Mixing wait    ");  break;
    case FAULT:              lcdShow(currentPh, "FAULT-bad pH   ");  break;
  }
}

void serialPrintStatus() {
  unsigned long now = millis();
  if (now - lastSerialPrint < SERIAL_PRINT_INTERVAL_MS) return;
  lastSerialPrint = now;
  
  Serial.print("t=");      Serial.print(now / 1000);
  Serial.print("s  V=");   Serial.print(currentVoltage, 3);
  Serial.print("  pH=");   Serial.print(currentPh, 2);
  Serial.print("  state=");
  switch (state) {
    case IDLE:               Serial.println("IDLE"); break;
    case DOSING_DOWN:        Serial.println("DOSING_DOWN"); break;
    case DOSING_UP:          Serial.println("DOSING_UP"); break;
    case WAITING_AFTER_DOSE: Serial.println("WAITING"); break;
    case FAULT:              Serial.println("FAULT"); break;
  }
}

void heartbeat() {
  unsigned long now = millis();
  if (now - lastHeartbeat < 1000) return;
  lastHeartbeat = now;
  digitalWrite(LED_PIN, !digitalRead(LED_PIN));
}

// ====================================================================
// SETUP
// ====================================================================

void setup() {
  // Initialize relay control pins to inactive level before pinMode so
  // the relay does not see a glitch during boot.
  digitalWrite(PUMP_DOWN_PIN, HIGH);
  digitalWrite(PUMP_UP_PIN,   HIGH);
  pinMode(PUMP_DOWN_PIN, OUTPUT);
  pinMode(PUMP_UP_PIN,   OUTPUT);
  pinMode(LED_PIN,       OUTPUT);
  digitalWrite(PUMP_DOWN_PIN, HIGH);
  digitalWrite(PUMP_UP_PIN,   HIGH);
  digitalWrite(LED_PIN, LOW);
  
  // Clear sample buffer
  for (int i = 0; i < PH_SAMPLES; i++) phBuffer[i] = 0;
  
  Serial.begin(9600);
  delay(200);
  
  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("pH Control v1.0 ");
  lcd.setCursor(0, 1);
  lcd.print("Starting...     ");
  
  Serial.println();
  Serial.println("=========================================");
  Serial.println("  Smart Hydroponics pH Control");
  Serial.println("=========================================");
  Serial.print("Setpoint:           "); Serial.println(SETPOINT, 2);
  Serial.print("Deadband (total):   "); Serial.println(DEADBAND, 2);
  Serial.print("Calibration slope:  "); Serial.println(PH_SLOPE, 4);
  Serial.print("Calibration offset: "); Serial.println(PH_OFFSET, 4);
  Serial.print("Dose duration:     ");  Serial.print(DOSE_MS);
  Serial.println(" ms");
  Serial.print("Mixing wait:       ");  Serial.print(DOSE_WAIT_MS / 1000);
  Serial.println(" s");
  Serial.println();
  
  // Pumps are assumed OFF at boot. State variables already reflect this.
  // Control starts immediately.
  state = IDLE;
  stateEnteredAt  = millis();
  lastDoseEndedAt = 0;
  
  delay(1500);
  lcd.setCursor(0, 1);
  lcd.print("Control running ");
  delay(500);
}

// ====================================================================
// MAIN LOOP
// ====================================================================

void loop() {
  unsigned long now = millis();
  
  sampleAndUpdatePh();
  updateDisplay();
  serialPrintStatus();
  heartbeat();
  
  // Sensor sanity check
  bool phInvalid = (currentPh < PH_MIN_VALID) || (currentPh > PH_MAX_VALID);
  if (phInvalid && state != FAULT) {
    Serial.println("[FAULT] pH out of valid range. Stopping pumps.");
    setPumpState(PUMP_DOWN_PIN, pumpDownIsOn, false);
    setPumpState(PUMP_UP_PIN,   pumpUpIsOn,   false);
    state = FAULT;
    stateEnteredAt = now;
    return;
  }
  if (!phInvalid && state == FAULT) {
    Serial.println("[OK] Sensor recovered. Returning to IDLE.");
    state = IDLE;
    stateEnteredAt = now;
  }
  
  float upper      = SETPOINT + DEADBAND / 2.0;
  float lower      = SETPOINT - DEADBAND / 2.0;
  bool  intervalOk = (now - lastDoseEndedAt) >= MIN_DOSE_INTERVAL_MS;
  
  switch (state) {
    case IDLE: {
      if (!phBufferFilled) break;
      if (!intervalOk)     break;
      
      if (currentPh > upper) {
        Serial.print("[CTL] pH "); Serial.print(currentPh, 2);
        Serial.print(" > ");       Serial.print(upper, 2);
        Serial.println(" -> dose pH-DOWN");
        setPumpState(PUMP_DOWN_PIN, pumpDownIsOn, true);
        state = DOSING_DOWN;
        stateEnteredAt = now;
      }
      else if (currentPh < lower) {
        Serial.print("[CTL] pH "); Serial.print(currentPh, 2);
        Serial.print(" < ");       Serial.print(lower, 2);
        Serial.println(" -> dose pH-UP");
        setPumpState(PUMP_UP_PIN, pumpUpIsOn, true);
        state = DOSING_UP;
        stateEnteredAt = now;
      }
      break;
    }
    
    case DOSING_DOWN: {
      if (now - stateEnteredAt >= DOSE_MS) {
        setPumpState(PUMP_DOWN_PIN, pumpDownIsOn, false);
        Serial.println("[CTL] pH-DOWN dose complete. Waiting for mix.");
        lastDoseEndedAt = now;
        state = WAITING_AFTER_DOSE;
        stateEnteredAt = now;
      }
      break;
    }
    
    case DOSING_UP: {
      if (now - stateEnteredAt >= DOSE_MS) {
        setPumpState(PUMP_UP_PIN, pumpUpIsOn, false);
        Serial.println("[CTL] pH-UP dose complete. Waiting for mix.");
        lastDoseEndedAt = now;
        state = WAITING_AFTER_DOSE;
        stateEnteredAt = now;
      }
      break;
    }
    
    case WAITING_AFTER_DOSE: {
      if (now - stateEnteredAt >= DOSE_WAIT_MS) {
        Serial.println("[CTL] Mix wait complete. Returning to IDLE.");
        state = IDLE;
        stateEnteredAt = now;
      }
      break;
    }
    
    case FAULT: {
      if (pumpDownIsOn) setPumpState(PUMP_DOWN_PIN, pumpDownIsOn, false);
      if (pumpUpIsOn)   setPumpState(PUMP_UP_PIN,   pumpUpIsOn,   false);
      break;
    }
  }
}
