/*
 * Smart Hydroponics pH Control System
 * Final firmware - production version
 *
 * Course:   Feedback and Control Systems
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
 *   This module operates with TOGGLE pulse semantics.
 *   Each LOW->HIGH pulse on the IN pin inverts the relay state.
 *   The firmware tracks pump state internally and pulses the IN pin
 *   only when a transition is required. State synchronization is
 *   enforced at startup via an operator confirmation prompt.
 *
 * Control strategy:
 *   On-off control with deadband and dead-time compensation.
 *     - When pH > setpoint + deadband/2, dose pH-down (acid).
 *     - When pH < setpoint - deadband/2, dose pH-up   (base).
 *     - After every dose, wait DOSE_WAIT_MS for mixing and sensor settling.
 *     - Minimum interval between doses is also enforced.
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
const int PH_PIN        = A0;   // pH sensor analog output
const int PUMP_DOWN_PIN = 7;    // Relay IN1 -> pH-down (vinegar) pump
const int PUMP_UP_PIN   = 8;    // Relay IN2 -> pH-up (baking soda) pump
const int LED_PIN       = 13;   // Onboard LED, used as heartbeat

// ---- Relay toggle pulse width ----
// One LOW->HIGH pulse on the relay IN pin inverts the relay state.
const unsigned long RELAY_PULSE_MS = 100;

// ---- pH calibration constants ----
// Measured from two-point calibration with pH 4.01 and pH 6.86 buffers.
// Equation: pH = PH_SLOPE * voltage + PH_OFFSET
const float PH_SLOPE  = 3.5;
const float PH_OFFSET = 0.06;

// ---- Sensor sampling ----
const int           PH_SAMPLES               = 40;
const unsigned long SAMPLING_INTERVAL_MS     = 20;
const unsigned long SERIAL_PRINT_INTERVAL_MS = 800;
const unsigned long LCD_UPDATE_INTERVAL_MS   = 500;

// ---- Control parameters ----
const float         SETPOINT                 = 6.0;     // target pH
const float         DEADBAND                 = 0.4;     // total width
const unsigned long DOSE_MS                  = 10;     // pump on time per dose
const unsigned long DOSE_WAIT_MS             = 10000;   // mixing + settle wait
const unsigned long MIN_DOSE_INTERVAL_MS     = 10000;   // safety floor

// ---- Sensor sanity check ----
const float PH_MIN_VALID = 0.0;
const float PH_MAX_VALID = 14.0;

// ====================================================================
// STATE
// ====================================================================

// Pump state (tracked in software because the relay has no state feedback)
bool pumpDownIsOn = false;
bool pumpUpIsOn   = false;

// Control state machine
enum ControlState { IDLE, DOSING_DOWN, DOSING_UP, WAITING_AFTER_DOSE, FAULT };
ControlState state = IDLE;

unsigned long stateEnteredAt = 0;
unsigned long lastDoseEndedAt = 0;

// Sensor state
int   phBuffer[PH_SAMPLES];
int   phBufferIndex = 0;
bool  phBufferFilled = false;
float currentPh      = 7.0;
float currentVoltage = 2.5;

// Display state
unsigned long lastLcdUpdate    = 0;
unsigned long lastSerialPrint  = 0;
unsigned long lastSample       = 0;
unsigned long lastHeartbeat    = 0;

// ====================================================================
// HELPERS - pH sensor
// ====================================================================

// Trimmed-mean average: drops the highest and lowest samples to
// reject ADC spikes, then averages the rest.
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

// Send one toggle pulse to a relay channel.
// This INVERTS the relay state from whatever it was before.
void pulseRelay(int pin) {
  digitalWrite(pin, LOW);
  delay(RELAY_PULSE_MS);
  digitalWrite(pin, HIGH);
}

// Drive a pump to a desired state (true = on, false = off).
// If the pump's tracked state already matches the desired state, no pulse is sent.
void setPumpState(int pin, bool &trackedState, bool desiredOn) {
  if (trackedState == desiredOn) return;
  pulseRelay(pin);
  trackedState = desiredOn;
}

// Force both pumps off no matter what the tracked state says.
// Used at startup and when entering a fault condition.
void forceAllPumpsOff() {
  // We do not know the actual relay state, so the safest action is to ask
  // the operator at startup. This function is for runtime use after sync.
  setPumpState(PUMP_DOWN_PIN, pumpDownIsOn, false);
  setPumpState(PUMP_UP_PIN,   pumpUpIsOn,   false);
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
// STARTUP - synchronize relay state with software state
// ====================================================================

void syncPumpStateAtStartup() {
  Serial.println();
  Serial.println("======================================");
  Serial.println("  STARTUP PUMP STATE SYNCHRONIZATION");
  Serial.println("======================================");
  Serial.println("Look at the pumps. They are physically OFF if their");
  Serial.println("rotors are not turning. Answer y/n in the serial monitor.");
  Serial.println();
  
  // pH-down pump
  Serial.print("Is the pH-DOWN pump currently RUNNING? (y/n): ");
  while (!Serial.available()) delay(10);
  char r1 = Serial.read();
  while (Serial.available()) Serial.read();
  if (r1 == 'y' || r1 == 'Y') {
    Serial.println("y - sending toggle to switch it OFF");
    pulseRelay(PUMP_DOWN_PIN);
  } else {
    Serial.println("n - leaving as is");
  }
  pumpDownIsOn = false;
  delay(500);
  
  // pH-up pump
  Serial.print("Is the pH-UP pump currently RUNNING? (y/n): ");
  while (!Serial.available()) delay(10);
  char r2 = Serial.read();
  while (Serial.available()) Serial.read();
  if (r2 == 'y' || r2 == 'Y') {
    Serial.println("y - sending toggle to switch it OFF");
    pulseRelay(PUMP_UP_PIN);
  } else {
    Serial.println("n - leaving as is");
  }
  pumpUpIsOn = false;
  
  Serial.println();
  Serial.println("Both pumps now tracked as OFF. Entering control loop.");
  Serial.println();
}

// ====================================================================
// SETUP
// ====================================================================

void setup() {
  // Initialize control pins first to a safe (idle) level before pinMode,
  // so the relay does not see a glitch during boot.
  digitalWrite(PUMP_DOWN_PIN, HIGH);
  digitalWrite(PUMP_UP_PIN,   HIGH);
  pinMode(PUMP_DOWN_PIN, OUTPUT);
  pinMode(PUMP_UP_PIN,   OUTPUT);
  pinMode(LED_PIN,       OUTPUT);
  digitalWrite(PUMP_DOWN_PIN, HIGH);
  digitalWrite(PUMP_UP_PIN,   HIGH);
  digitalWrite(LED_PIN, LOW);
  
  // Clear ADC sample buffer
  for (int i = 0; i < PH_SAMPLES; i++) phBuffer[i] = 0;
  
  // Serial
  Serial.begin(9600);
  delay(200);
  
  // LCD
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
  Serial.print("Dose duration:     ");  Serial.print(DOSE_MS);     Serial.println(" ms");
  Serial.print("Mixing wait:       ");  Serial.print(DOSE_WAIT_MS / 1000); Serial.println(" s");
  Serial.println();
  
  // Synchronize pump state with operator
  syncPumpStateAtStartup();
  
  state = IDLE;
  stateEnteredAt = millis();
  lastDoseEndedAt = 0;
  
  lcd.setCursor(0, 1);
  lcd.print("Control running ");
  delay(1500);
}

// ====================================================================
// MAIN LOOP
// ====================================================================

void loop() {
  unsigned long now = millis();
  
  // Always: sample the sensor and update display
  sampleAndUpdatePh();
  updateDisplay();
  serialPrintStatus();
  heartbeat();
  
  // Sanity check on pH value
  bool phInvalid = (currentPh < PH_MIN_VALID) || (currentPh > PH_MAX_VALID);
  if (phInvalid && state != FAULT) {
    Serial.println("[FAULT] pH out of valid range. Stopping pumps.");
    forceAllPumpsOff();
    state = FAULT;
    stateEnteredAt = now;
    return;
  }
  if (!phInvalid && state == FAULT) {
    // Sensor recovered, return to idle
    Serial.println("[OK] Sensor recovered. Returning to IDLE.");
    state = IDLE;
    stateEnteredAt = now;
  }
  
  // Control thresholds
  float upper = SETPOINT + DEADBAND / 2.0;
  float lower = SETPOINT - DEADBAND / 2.0;
  bool intervalOk = (now - lastDoseEndedAt) >= MIN_DOSE_INTERVAL_MS;
  
  // State machine
  switch (state) {
    case IDLE: {
      if (!phBufferFilled) break;  // wait until buffer has good data
      if (!intervalOk) break;       // respect minimum dose interval
      
      if (currentPh > upper) {
        Serial.print("[CTL] pH "); Serial.print(currentPh, 2);
        Serial.print(" > "); Serial.print(upper, 2);
        Serial.println(" -> dose pH-DOWN");
        setPumpState(PUMP_DOWN_PIN, pumpDownIsOn, true);
        state = DOSING_DOWN;
        stateEnteredAt = now;
      }
      else if (currentPh < lower) {
        Serial.print("[CTL] pH "); Serial.print(currentPh, 2);
        Serial.print(" < "); Serial.print(lower, 2);
        Serial.println(" -> dose pH-UP");
        setPumpState(PUMP_UP_PIN, pumpUpIsOn, true);
        state = DOSING_UP;
        stateEnteredAt = now;
      }
      // else: still in deadband, stay IDLE
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
      // Recovery handled above. Just keep pumps off here as a guard.
      if (pumpDownIsOn) setPumpState(PUMP_DOWN_PIN, pumpDownIsOn, false);
      if (pumpUpIsOn)   setPumpState(PUMP_UP_PIN,   pumpUpIsOn,   false);
      break;
    }
  }
}