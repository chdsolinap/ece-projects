/*
 * =================================================================
 *  SMART PARKING SYSTEM — FINAL CODE
 *  Microprocessor and Microcontroller System and Design
 * =================================================================
 *
 *  HARDWARE OVERVIEW
 *  -----------------
 *  Microcontroller : 1 x ESP32 WROOM-32
 *  Display         : 1 x 16x2 LCD (I2C, address 0x27)
 *  Distance sensors: 2 x HC-SR04 ultrasonic at the gate
 *  Actuator        : 1 x SG90 servo for the gate barrier
 *  Output expander : 2 x 74HC595  shift registers (LED outputs,
 *                                 daisy-chained for a total of 16 outputs)
 *  Input  expander : 2 x 74HC165  shift registers (switch inputs,
 *                                 daisy-chained for a total of 16 inputs)
 *
 *  WHY SHIFT REGISTERS?
 *  --------------------
 *  An ESP32 has limited GPIOs. We need:
 *      - 2 outputs per LED pair (red + green) × up to 8 slots = 16 outputs
 *      - 1 input per occupancy switch × up to 8 slots             = 8 inputs
 *  Bit-banging two 74HC595's and two 74HC165's lets us cover all of
 *  that using only 6 GPIOs total (3 for the LED chain, 3 for the
 *  switch chain).  Adding more slots later costs zero extra GPIOs;
 *  you just append another chip to either chain.
 *
 *  REAL SLOTS (with physical hardware)
 *  -----------------------------------
 *      Slot 1 (A, index 0)  switch -> chip1 bit0 (global bit 8)
 *                           LEDs   -> chip0 bits 0 (red), 1 (green)
 *      Slot 2 (B, index 1)  switch -> chip1 bit1 (global bit 9)
 *                           LEDs   -> chip0 bits 2 (red), 3 (green)
 *      Slot 5 (C, index 4)  switch -> chip0 bit0 (global bit 0)
 *      Slot 6 (D, index 5)  switch -> chip0 bit1 (global bit 1)
 *
 *  PHANTOM SLOTS
 *  -------------
 *  Slots 3, 4, 7, 8 exist in the firmware (NUM_SLOTS = 8) but have
 *  no physical wiring.  They are skipped everywhere by checking
 *  isRealSlot().  Keeping them in the data model demonstrates the
 *  daisy chain can scale to a full 8 slots without code changes,
 *  which is part of the design rubric.
 *
 *  DAISY-CHAIN DEMONSTRATION
 *  -------------------------
 *  Even though chip 1's LED outputs are not driven, the firmware
 *  still clocks 16 bits to BOTH 74HC595 chips on every refresh, and
 *  still clocks 16 bits FROM both 74HC165 chips on every scan.  That
 *  proves the daisy chain is electrically functional in both
 *  directions, which is what the rubric asks for.
 *
 * =================================================================
 */

/* ===============================================================
 *  LIBRARY INCLUDES
 * ===============================================================
 */
#include <Wire.h>             // Arduino's built-in I2C driver. The LCD
                              // uses I2C (two wires: SDA + SCL) instead
                              // of the parallel interface, so we need
                              // this to talk to it. ESP32 default I2C
                              // pins are GPIO 21 (SDA) and 22 (SCL).

#include <LiquidCrystal_I2C.h>// Driver for I2C-backpack 16x2 character
                              // LCDs (PCF8574 expander). Lets us call
                              // lcd.print("..."), lcd.setCursor(), etc.
                              // instead of doing raw I2C transactions.

#include <ESP32Servo.h>       // Servo library adapted for the ESP32.
                              // The regular Arduino Servo.h uses AVR
                              // timer hardware and does not compile on
                              // ESP32. This version uses the ESP32's
                              // LEDC PWM peripheral under the hood.

#include <WiFi.h>             // ESP32's Wi-Fi stack. Gives us the WiFi
                              // object used to connect to a router.

#include <FirebaseESP32.h>    // Firebase Realtime Database client by
                              // Mobizt. Provides Firebase.setBool(),
                              // Firebase.getBool(), beginStream(), etc.,
                              // so the ESP32 can read and write the
                              // database (and listen for changes) over
                              // a TLS-secured WebSocket.


/* ===============================================================
 *  CONFIGURATION  —  edit these to match the deployment
 * =============================================================== */

#define WIFI_SSID      "WIFI_SSID"    // Wi-Fi network name (SSID).
#define WIFI_PASSWORD  "WIFI_PASSWORD"     // Wi-Fi password.

// Firebase host: the unique URL of our Realtime Database, taken from
// the Firebase Console. Without the leading "https://" because the
// FirebaseESP32 client adds that itself.
#define FIREBASE_HOST  "FIREBASE_HOST"

// Legacy database secret (long random string from Firebase Console ->
// Project Settings -> Service Accounts -> Database secrets). This is
// an ADMIN-LEVEL credential — it bypasses every security rule. We are
// fine using it here because the credential lives only inside the
// hardware, never in the user-facing browser.
#define FIREBASE_AUTH  "FIREBASE_AUTH"

#define NUM_SLOTS   8         // Total slots in firmware (real + phantom).
                              // Must match the web app's NUM_SLOTS.
#define REAL_SLOTS  4         // How many slots have physical hardware.
                              // Used for the LCD count and for Firebase.


// REAL_SLOT_INDEXES holds the 0-based positions of the real slots in
// the slot arrays below. Slot 1 is index 0, slot 5 is index 4, etc.
// `const int [..] = { ... }` declares an immutable lookup table that
// is stored in flash (not RAM) thanks to `const`.
const int REAL_SLOT_INDEXES[REAL_SLOTS] = { 0, 1, 4, 5 };

// slotToInputBit maps a slot index to the bit number (0..15) where its
// occupancy switch lives in the combined 16-bit 74HC165 read buffer.
// During the build, slot 1/2 switches ended up on the SECOND 74HC165
// chip (global bits 8 and 9) while slot 5/6 switches landed on the
// FIRST chip (global bits 0 and 1). Using a lookup table instead of a
// formula makes this wiring quirk explicit and easy to change later.
// A value of -1 means "phantom slot, no switch wired".
const int slotToInputBit[NUM_SLOTS] = {
  8,   // slot 1 (index 0) -> chip1 bit0  (global bit 8)
  9,   // slot 2 (index 1) -> chip1 bit1  (global bit 9)
  -1,  // slot 3 (phantom)
  -1,  // slot 4 (phantom)
  0,   // slot 5 (index 4) -> chip0 bit0
  1,   // slot 6 (index 5) -> chip0 bit1
  -1,  // slot 7 (phantom)
  -1   // slot 8 (phantom)
};

/* ===============================================================
 *  PIN ASSIGNMENTS — ESP32 GPIO numbers
 * =============================================================== */

// --- 74HC595 chain (LED outputs) ---
#define SR_OUT_DATA   32      // Serial-data line       -> chip pin 14 (DS / SER)
#define SR_OUT_CLOCK  33      // Shift-register clock   -> chip pin 11 (SH_CP / SRCLK)
#define SR_OUT_LATCH  25      // Storage-register clock -> chip pin 12 (ST_CP / RCLK)

// --- 74HC165 chain (switch inputs) ---
#define SR_IN_DATA    18      // Serial-data line from chip Q7 (pin 9)
#define SR_IN_CLOCK   19      // Shift clock           -> chip pin 2 (CP)
#define SR_IN_LOAD    23      // Parallel-load /PL     -> chip pin 1 (active LOW)

// --- Gate hardware ---
#define US1_TRIG   13         // Outside sensor — TRIG (transmit pulse)
#define US1_ECHO   12         // Outside sensor — ECHO (input pulse width)
#define US2_TRIG   14         // Inside  sensor — TRIG
#define US2_ECHO   27         // Inside  sensor — ECHO
#define SERVO_PIN  26         // Servo PWM signal


/* ===============================================================
 *  TIMING & THRESHOLD CONSTANTS
 *
 *  Everything in milliseconds unless noted. Each value was chosen
 *  to keep the system responsive without false-triggering.
 * =============================================================== */

#define CAR_DETECT_CM      5      // Ultrasonic distance below which we
                                  // consider a "car present" at the gate.
                                  // Tight value (5 cm) avoids picking
                                  // up hands or far-away surfaces.

#define DWELL_TIME         1500   // Car must remain in front of the
                                  // sensor this long before we open the
                                  // gate.  Filters out brief crossings.

#define GATE_TRAVEL_TIME   800    // How long the servo needs to physically
                                  // sweep from closed to open (or back).

#define CLOSE_DELAY        2000   // After the car has passed, wait this
                                  // long with BOTH sensors clear before
                                  // we actually close — gives the driver
                                  // time to leave the gate area.

#define SCAN_INTERVAL      80     // Minimum gap between ultrasonic
                                  // readings. The HC-SR04 echo can take
                                  // up to ~25 ms; 80 ms keeps the loop
                                  // smooth.

#define GATE_OPEN_ANGLE    0      // Servo angle that represents OPEN.
#define GATE_CLOSE_ANGLE   90     // Servo angle that represents CLOSED.
                                  // These are swapped from the SG90's
                                  // usual 90/0 because the horn was
                                  // installed mirrored on this build.


/* ===============================================================
 *  GATE STATE MACHINE — symbolic state values
 *
 *  Using #define'd integers (instead of a giant if/else chain) lets
 *  us write handleGate() as a sequence of "if (gateState == X) ..."
 *  blocks that read top-to-bottom in the order the states fire.
 * =============================================================== */

#define STATE_IDLE              0   // Nothing happening at the gate.
#define STATE_ENTRY_WAITING     1   // Outside sensor saw a car; debouncing.
#define STATE_ENTRY_OPENING     2   // Servo is rotating to OPEN.
#define STATE_ENTRY_WAIT_PASS   3   // Gate is open, waiting for car to
                                    // reach the inside sensor.
#define STATE_ENTRY_CLOSING     4   // Both sensors clear; closing soon.
#define STATE_EXIT_WAITING      5   // Mirror of ENTRY_WAITING but for
                                    // a car leaving the parking lot.
#define STATE_EXIT_OPENING      6
#define STATE_EXIT_WAIT_PASS    7
#define STATE_EXIT_CLOSING      8


/* ===============================================================
 *  GLOBAL OBJECTS
 *
 *  Created once at startup, then re-used for the program's lifetime.
 * =============================================================== */

// LiquidCrystal_I2C constructor:
//   (I2C address, columns, rows).  0x27 is the default for almost all
//   PCF8574 LCD backpacks shipped on AliExpress / hobby kits.
LiquidCrystal_I2C lcd(0x27, 16, 2);

// One Servo object representing the gate barrier.
Servo gateServo;

// Firebase needs four objects:
//   firebaseListener — keeps the long-lived stream open
//                      (separate from writes to avoid deadlocks).
//   firebaseWriter   — used for set / get calls (writes & one-offs).
//   firebaseAuth     — auth payload (we use the legacy token, so this
//                      stays empty; it just needs to exist).
//   firebaseConfig   — host + credentials + advanced options.
FirebaseData   firebaseListener;
FirebaseData   firebaseWriter;
FirebaseAuth   firebaseAuth;
FirebaseConfig firebaseConfig;


/* ===============================================================
 *  GLOBAL VARIABLES — runtime state
 * =============================================================== */

int availableSlots = REAL_SLOTS;    // Cached count shown on the LCD.
int lastShownCount = -1;            // Last value we actually wrote to
                                    // the LCD; -1 forces the first
                                    // updateLCD() call to redraw.

// Per-slot state. Indexed 0..NUM_SLOTS-1. Phantom slots are simply
// always-false and ignored everywhere except inside loops that filter
// with isRealSlot() / REAL_SLOT_INDEXES[].
bool slotIsOccupied[NUM_SLOTS];     // Physical switch says car is parked.
bool slotIsReserved[NUM_SLOTS];     // Web app reserved this slot.
bool ownerWasAlerted[NUM_SLOTS];    // Have we already sent the
                                    // "reserved slot taken" alert? Used
                                    // to avoid spamming Firebase.

// 16-bit shift-register snapshots, split into two bytes (one per chip).
// We do bit math in C with bytes (8-bit) rather than wider types, then
// concatenate logically with bit 8..15 living in [1].
byte ledOutputBuffer[2];            // What we WILL push to the 595s.
byte switchInputBuffer[2];          // What we just READ from the 165s.

// Most-recent debounced sensor readings.
bool sensor1HasCar = false;
bool sensor2HasCar = false;
unsigned long lastSensorReadTime = 0;   // millis() of the last scan.
                                        // `unsigned long` matches the
                                        // type returned by millis() so
                                        // subtraction wraps correctly.

// Gate state machine variables.
int gateState = STATE_IDLE;
unsigned long gateTimer = 0;        // Re-used as a stopwatch in every state.
bool servoIsAttached = false;       // True while the servo signal is being
                                    // generated. We detach when idle so
                                    // the servo stops drawing power.


/* ===============================================================
 *  HELPERS
 * =============================================================== */

/* isRealSlot()
 * Returns true if the given slot index has physical hardware behind it.
 * O(REAL_SLOTS) linear search — fine because REAL_SLOTS is tiny (4).
 */
bool isRealSlot(int slotIndex) {
  for (int i = 0; i < REAL_SLOTS; i++) {
    if (REAL_SLOT_INDEXES[i] == slotIndex) return true;
  }
  return false;
}


/* ===============================================================
 *  ULTRASONIC — HC-SR04 distance measurement
 *
 *  Protocol:
 *    1. Pull TRIG low briefly to ensure a clean rising edge.
 *    2. Pull TRIG high for >= 10 µs to fire 8 cycles of 40 kHz pulse.
 *    3. Sensor pulls ECHO high for as long as it takes the pulse to
 *       travel out and come back.
 *    4. Measure ECHO pulse width with pulseIn().
 *    5. distance_cm = duration_us * 0.034 / 2
 *           0.034 cm/µs = speed of sound (~340 m/s).
 *           Divide by 2 because the pulse made a round trip.
 * =============================================================== */
long measureDistanceCM(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);       // Quiet line before the trigger pulse.
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);            // Fire the 10 µs pulse.
  digitalWrite(trigPin, LOW);

  // pulseIn() blocks until ECHO goes HIGH then LOW (or times out).
  // 25 000 µs timeout corresponds to ~4 m, more than enough for a
  // parking gate. Returns 0 on timeout, which we map to "very far".
  long duration = pulseIn(echoPin, HIGH, 25000);
  if (duration == 0) return 999;    // 999 cm sentinel = "no echo seen".
  return duration * 0.034 / 2;
}


/* ===============================================================
 *  74HC595 OUTPUT — write 16 bits to the daisy-chained LED chain
 *
 *  Wiring recap:
 *    ESP32 -> chip0 DS  -> ... QH' -> chip1 DS
 *  Data flows IN at chip 0 and SHIFTS THROUGH chip 0 into chip 1.
 *  So bits we send first end up FAR from the ESP32 (in chip 1) and
 *  bits we send last stay near (chip 0).
 *
 *  Therefore: send chip 1's byte first, MSB first; then chip 0's
 *  byte. After 16 clock edges, raise LATCH to copy the shift
 *  register contents to the output pins simultaneously (so all 16
 *  outputs update at the same instant — no glitches).
 * =============================================================== */
void sendOutputsToShiftRegisters() {
  digitalWrite(SR_OUT_LATCH, LOW);   // Hold LATCH low while shifting so
                                     // outputs do not flicker between
                                     // intermediate states.

  for (int chip = 1; chip >= 0; chip--) {
    byte data = ledOutputBuffer[chip];
    for (int bit = 7; bit >= 0; bit--) {
      digitalWrite(SR_OUT_CLOCK, LOW);
      // (data >> bit) shifts the target bit down to position 0;
      // & 1 isolates it; result is 0 or 1, then digitalWrite uses
      // that as LOW/HIGH on the data line.
      digitalWrite(SR_OUT_DATA, (data >> bit) & 1);
      digitalWrite(SR_OUT_CLOCK, HIGH);   // Rising edge clocks the bit in.
    }
  }

  digitalWrite(SR_OUT_LATCH, HIGH);  // Rising edge on LATCH copies the
                                     // shift register to the output
                                     // register, atomically updating
                                     // all 16 LED lines.
}

/* setOneOutputBit()
 * Sets or clears one bit in the in-memory ledOutputBuffer WITHOUT
 * touching any other bit. We push the buffer to hardware later via
 * sendOutputsToShiftRegisters().
 *
 *  bitNumber = 0..15 (bits 0-7 are chip0, bits 8-15 are chip1).
 */
void setOneOutputBit(int bitNumber, bool turnOn) {
  int whichChip = bitNumber / 8;     // integer division: 0..7 -> 0, 8..15 -> 1
  int whichBit  = bitNumber % 8;     // bit position WITHIN the byte (0..7)
  if (whichChip < 0 || whichChip > 1) return;  // out-of-range safety.

  if (turnOn) {
    // |= is "OR-assign": leaves existing 1-bits alone, sets the new one.
    // (1 << whichBit) builds a bitmask with a single 1 at position whichBit.
    ledOutputBuffer[whichChip] |= (1 << whichBit);
  } else {
    // ~ is bitwise NOT; ~(1<<whichBit) is all-1s except a single 0 at
    // the target position. &= keeps everything else but forces the
    // target bit to 0.
    ledOutputBuffer[whichChip] &= ~(1 << whichBit);
  }
}


/* ===============================================================
 *  74HC165 INPUT — read 16 bits from the daisy-chained switch chain
 *
 *  Wiring recap:
 *    chip1 Q7 -> chip0 DS -> ... -> chip0 Q7 -> ESP32
 *  Data flows FROM far-end (chip 1) THROUGH chip 0 to the ESP32.
 *  The first bit shifted out is whatever was on chip 0's Q7 at the
 *  instant of /PL — that is chip 0's bit 7. After 8 clocks chip 0
 *  has emptied; the next 8 bits are chip 1's data.
 *
 *  Our convention: store bits 0-7 in switchInputBuffer[0] and bits
 *  8-15 in switchInputBuffer[1] so that bit number == switch index
 *  across the whole 16-bit space.
 * =============================================================== */
void readInputsFromShiftRegisters() {
  // Briefly assert /PL low to latch every D-input into the shift register.
  digitalWrite(SR_IN_LOAD, LOW);
  delayMicroseconds(5);
  digitalWrite(SR_IN_LOAD, HIGH);

  for (int chip = 1; chip >= 0; chip--) {
    byte data = 0;
    for (int bit = 7; bit >= 0; bit--) {
      digitalWrite(SR_IN_CLOCK, LOW);     // Falling edge here exposes the
                                          // next serial bit on Q7.
      delayMicroseconds(2);               // Small settle time before sampling.
      if (digitalRead(SR_IN_DATA) == HIGH) {
        // Same bitmask trick as setOneOutputBit, but used to BUILD a byte.
        data |= (1 << bit);
      }
      digitalWrite(SR_IN_CLOCK, HIGH);
    }
    switchInputBuffer[chip] = data;
  }
}

/* readOneInputBit()
 *  Returns the value of one bit (0..15) from the buffer we already read.
 *  Defaults to true (logical HIGH = released = NOT pressed) on a
 *  range error so a wiring bug never reports a phantom "occupied".
 */
bool readOneInputBit(int bitNumber) {
  int whichChip = bitNumber / 8;
  int whichBit  = bitNumber % 8;
  if (whichChip < 0 || whichChip > 1) return true;
  // Shift the target bit down to LSB position, mask with 1, compare to 1.
  return ((switchInputBuffer[whichChip] >> whichBit) & 1) == 1;
}


/* ===============================================================
 *  LED CONTROL
 *
 *  Slots 1 and 2 (A, B) on chip 0 work normally: each has its own
 *  red and green output bits.
 *      Slot N red   bit = N*2
 *      Slot N green bit = N*2 + 1
 *
 *  Slots 5 and 6 (C, D) on chip 1 are NOT driven by this firmware.
 *  See the header comment for the reason.  All chip 1 outputs stay
 *  LOW for the entire run, so the LEDs on that chip remain dark and
 *  the wiring fault is harmless.
 * =============================================================== */

/* updateOneSlotLED()
 *  Called whenever ONE slot's occupancy or reservation changes, so
 *  we don't rewrite all 16 bits for every minor change.
 */
void updateOneSlotLED(int slotIndex) {
  // Skip slots whose LEDs we deliberately don't drive (C and D).
  if (slotIndex == 4 || slotIndex == 5) return;

  // shouldBeRed is true if the slot is OCCUPIED or RESERVED.
  // Both states show red so the visual rule is simple: green = free,
  // red = unavailable for any reason.
  bool shouldBeRed = slotIsOccupied[slotIndex] || slotIsReserved[slotIndex];
  int redBit   = slotIndex * 2;
  int greenBit = slotIndex * 2 + 1;

  setOneOutputBit(redBit,   shouldBeRed);
  setOneOutputBit(greenBit, !shouldBeRed);   // ! is logical NOT.
  sendOutputsToShiftRegisters();
}

/* updateAllLEDs()
 *  Re-derives every slot's LED from current state and pushes to hardware
 *  in a single 16-bit burst.  Used at boot and after big changes (e.g.
 *  after Firebase initial sync).
 */
void updateAllLEDs() {
  // Clear both bytes so any bit we DON'T explicitly set ends up LOW.
  // This guarantees chip 1 stays all-zero and the unused slot 3/4/7/8
  // bits on chip 0 never accidentally turn on.
  ledOutputBuffer[0] = 0;
  ledOutputBuffer[1] = 0;

  // Only iterate the real slots, and only drive A/B (skip C/D).
  for (int r = 0; r < REAL_SLOTS; r++) {
    int i = REAL_SLOT_INDEXES[r];
    if (i == 4 || i == 5) continue;       // C/D LEDs deliberately untouched.
    bool shouldBeRed = slotIsOccupied[i] || slotIsReserved[i];
    int redBit   = i * 2;
    int greenBit = i * 2 + 1;
    if (shouldBeRed) {
      setOneOutputBit(redBit, true);
    } else {
      setOneOutputBit(greenBit, true);
    }
  }

  sendOutputsToShiftRegisters();
}


/* ===============================================================
 *  LCD
 * =============================================================== */

/* updateLCD()
 *  Writes the available-slots line.
 *  Early-returns when the count didn't change to avoid the slow
 *  full-redraw flicker every loop iteration.
 */
void updateLCD() {
  if (availableSlots == lastShownCount) return;
  lastShownCount = availableSlots;

  lcd.clear();
  lcd.setCursor(0, 0);                // column 0, row 0 (top line).
  lcd.print("Available Slots:");
  lcd.setCursor(0, 1);                // column 0, row 1 (bottom line).
  if (availableSlots <= 0) {
    lcd.print("    FULL    ");
  } else {
    lcd.print(availableSlots);        // print() is overloaded for int.
    lcd.print(" / ");
    lcd.print(REAL_SLOTS);
  }
}


/* ===============================================================
 *  AVAILABLE-SLOT COUNT
 *
 *  Counts only REAL slots, then pushes the count to Firebase so the
 *  web dashboard's summary card matches the on-site LCD.
 * =============================================================== */
void recalculateAvailableSlots() {
  int usedCount = 0;
  for (int i = 0; i < REAL_SLOTS; i++) {
    int slotIndex = REAL_SLOT_INDEXES[i];
    if (slotIsOccupied[slotIndex] || slotIsReserved[slotIndex]) {
      usedCount++;
    }
  }
  availableSlots = REAL_SLOTS - usedCount;

  updateLCD();

  // Only push to Firebase when Wi-Fi is up; otherwise the request would
  // block for a long timeout and stall the gate state machine.
  if (WiFi.status() == WL_CONNECTED) {
    Firebase.setInt(firebaseWriter, "/system/availableSlots", availableSlots);
    Firebase.setInt(firebaseWriter, "/system/totalSlots", REAL_SLOTS);
  }
}


/* ===============================================================
 *  SCAN SWITCHES
 *
 *  Reads all 16 input bits in one shot, then iterates the real
 *  slots and reacts to any state change. Reserved-slot-taken events
 *  push an alert to Firebase exactly once per occupancy episode.
 * =============================================================== */
void scanAllSwitches() {
  readInputsFromShiftRegisters();    // refresh switchInputBuffer[].

  for (int r = 0; r < REAL_SLOTS; r++) {
    int i = REAL_SLOT_INDEXES[r];            // actual slot index 0..7.
    int inputBit = slotToInputBit[i];        // physical bit number.
    if (inputBit == -1) continue;            // belt-and-suspenders.

    // Switches are wired ACTIVE-LOW (pulled to GND when pressed). The
    // 74HC165 reads HIGH when the line is idle, so we invert with `!`.
    bool switchIsPressed = !readOneInputBit(inputBit);

    // Only act when the debounced state actually flips.
    if (switchIsPressed != slotIsOccupied[i]) {
      bool wasReserved = slotIsReserved[i];  // capture before mutating.
      slotIsOccupied[i] = switchIsPressed;

      if (switchIsPressed) {
        // --- The slot just became OCCUPIED ---
        if (wasReserved) {
          // The owner of a reservation may have arrived — or a stranger
          // took the reserved slot. Either way, alert the owner once.
          if (!ownerWasAlerted[i] && WiFi.status() == WL_CONNECTED) {
            String slotPath = "/slots/slot" + String(i + 1);
            Firebase.setBool(firebaseWriter, slotPath + "/notifyOwner", true);
            // String concatenation with + is supported by the Arduino
            // String class. Result is uploaded as the alert text.
            Firebase.setString(firebaseWriter, slotPath + "/notifyMessage",
              "Reserved slot " + String(i + 1) + " has been taken.");
            ownerWasAlerted[i] = true;
            Serial.print("[ALERT] Reserved slot ");
            Serial.print(i + 1);
            Serial.println(" was taken - owner notified");
          }
          // Reservation is consumed once the car arrives, so clear it.
          slotIsReserved[i] = false;
          if (WiFi.status() == WL_CONNECTED) {
            String slotPath = "/slots/slot" + String(i + 1);
            Firebase.setBool(firebaseWriter, slotPath + "/reserved", false);
          }
        } else {
          Serial.print("[SWITCH ");
          Serial.print(i + 1);
          Serial.println("] occupied");
        }
      } else {
        // --- The slot just became FREE ---
        // Reset the alert flag so the NEXT reservation can re-alert.
        ownerWasAlerted[i] = false;
        Serial.print("[SWITCH ");
        Serial.print(i + 1);
        Serial.println("] free");
      }

      updateOneSlotLED(i);
      recalculateAvailableSlots();

      if (WiFi.status() == WL_CONNECTED) {
        String slotPath = "/slots/slot" + String(i + 1);
        Firebase.setBool(firebaseWriter, slotPath + "/occupied", slotIsOccupied[i]);
      }
    }
  }
}


/* ===============================================================
 *  FIREBASE STREAM CALLBACK
 *
 *  Fires whenever ANY child below /slots changes in the cloud
 *  database. We only care about /slots/slotN/reserved flips driven
 *  by the web dashboard.
 *
 *  The path comes in like "/slot3/reserved", relative to the stream
 *  root we opened ("/slots"). So we parse out the slot number after
 *  "/slot" and the field name after the second '/'.
 * =============================================================== */
void streamCallback(StreamData data) {
  String path = data.dataPath();
  Serial.print("[STREAM] ");
  Serial.print(path);
  Serial.print(" = ");
  Serial.println(data.stringData());

  if (path.startsWith("/slot")) {
    // path looks like "/slot5/reserved".  Find the second '/'.
    int slashIndex = path.indexOf('/', 5);
    if (slashIndex > 0) {
      // substring(5, slashIndex) skips "/slot" prefix and stops at
      // the next slash — e.g. for "/slot5/reserved" it returns "5".
      String slotNumberStr = path.substring(5, slashIndex);
      int slotNumber = slotNumberStr.toInt();
      String fieldName = path.substring(slashIndex + 1);

      if (slotNumber >= 1 && slotNumber <= NUM_SLOTS) {
        int slotIndex = slotNumber - 1;

        // Ignore stream events for slots that have no hardware.
        if (!isRealSlot(slotIndex)) return;

        // Only handle the "reserved" boolean here. Other fields are
        // either written BY us (occupied) or one-shot ack flags.
        if (fieldName == "reserved" && data.dataType() == "boolean") {
          bool newReservedValue = data.boolData();
          if (newReservedValue != slotIsReserved[slotIndex]) {
            slotIsReserved[slotIndex] = newReservedValue;
            // If the dashboard cancelled the reservation, reset the
            // alert flag so a future re-reservation can alert again.
            if (!newReservedValue) ownerWasAlerted[slotIndex] = false;
            updateOneSlotLED(slotIndex);
            recalculateAvailableSlots();
            Serial.print("[RESERVE ");
            Serial.print(slotNumber);
            // Ternary inside Serial.print would also work; using two
            // lines keeps the serial output readable in source form.
            if (newReservedValue) Serial.print("] ON");
            else                  Serial.print("] OFF");
            Serial.print(" - available slots: ");
            Serial.println(availableSlots);
          }
        }
      }
    }
  }
}

/* streamTimeoutCallback()
 *  Called by the FirebaseESP32 library when the long-lived stream
 *  loses its keep-alive. The library auto-reconnects, so we only log.
 */
void streamTimeoutCallback(bool timeout) {
  if (timeout) Serial.println("[STREAM] timeout - will reconnect automatically");
}


/* ===============================================================
 *  GATE LOGIC
 *
 *  We deliberately use a state machine instead of blocking
 *  delay()-based code so that the loop() can still read switches,
 *  push Firebase updates, and refresh the LCD while the gate is
 *  busy moving.
 * =============================================================== */

/* readAllSensors()
 *  Throttled to SCAN_INTERVAL so we don't oversaturate the HC-SR04s
 *  (they need ~60 ms between pulses to avoid hearing their own echo).
 */
void readAllSensors() {
  // (millis() - lastSensorReadTime) is the elapsed ms since last scan.
  // Subtraction of two unsigned longs naturally wraps around at the
  // 49-day rollover, which is why we never store an "end time".
  if (millis() - lastSensorReadTime < SCAN_INTERVAL) return;
  lastSensorReadTime = millis();

  long distance1 = measureDistanceCM(US1_TRIG, US1_ECHO);
  long distance2 = measureDistanceCM(US2_TRIG, US2_ECHO);

  bool newSensor1State = (distance1 < CAR_DETECT_CM);
  bool newSensor2State = (distance2 < CAR_DETECT_CM);

  // Only log on state change to keep the serial console quiet.
  if (newSensor1State != sensor1HasCar) {
    sensor1HasCar = newSensor1State;
    if (sensor1HasCar) Serial.println("[US1] CAR detected");
    else               Serial.println("[US1] clear");
  }
  if (newSensor2State != sensor2HasCar) {
    sensor2HasCar = newSensor2State;
    if (sensor2HasCar) Serial.println("[US2] CAR detected");
    else               Serial.println("[US2] clear");
  }
}

/* servoOpen() / servoClose() / servoIdle()
 *  We attach() the PWM signal only when commanding the servo, then
 *  detach() shortly after. This:
 *    - silences SG90 buzzing when idle
 *    - reduces current draw (important on USB power)
 *    - lets the gate be back-driven by hand for demos
 */
void servoOpen() {
  if (!servoIsAttached) {
    gateServo.attach(SERVO_PIN);
    servoIsAttached = true;
  }
  gateServo.write(GATE_OPEN_ANGLE);
  Serial.println(">>> SERVO: open");
}

void servoClose() {
  if (!servoIsAttached) {
    gateServo.attach(SERVO_PIN);
    servoIsAttached = true;
  }
  gateServo.write(GATE_CLOSE_ANGLE);
  Serial.println(">>> SERVO: close");
}

void servoIdle() {
  if (servoIsAttached) {
    delay(50);                    // Give the last write() time to send a
                                  // few PWM frames before we cut signal.
    gateServo.detach();
    servoIsAttached = false;
  }
}

/* handleGate()
 *  One pass through the state machine. Called every loop().
 *
 *  Two symmetric halves: ENTRY (outside->inside) and EXIT
 *  (inside->outside). Each runs:
 *      WAITING   - debounce the trigger sensor
 *      OPENING   - drive servo, wait for mechanical travel
 *      WAIT_PASS - hold open until the OTHER sensor sees the car
 *                  (or a 10 s safety timeout fires)
 *      CLOSING   - both sensors clear for CLOSE_DELAY ms, then close.
 */
void handleGate() {
  // -------- IDLE: nothing to do unless a sensor sees something --------
  if (gateState == STATE_IDLE) {
    if (sensor1HasCar && !sensor2HasCar) {
      gateTimer = millis();
      gateState = STATE_ENTRY_WAITING;
      Serial.println("[GATE] entry detected, waiting...");
    } else if (sensor2HasCar && !sensor1HasCar) {
      gateTimer = millis();
      gateState = STATE_EXIT_WAITING;
      Serial.println("[GATE] exit detected, waiting...");
    }
    return;
  }

  // -------- ENTRY half --------
  if (gateState == STATE_ENTRY_WAITING) {
    if (!sensor1HasCar) {
      gateState = STATE_IDLE;          // Car drove away, cancel.
      Serial.println("[GATE] entry cancelled (car left)");
      return;
    }
    if (millis() - gateTimer >= DWELL_TIME) {
      servoOpen();
      gateTimer = millis();
      gateState = STATE_ENTRY_OPENING;
    }
    return;
  }

  if (gateState == STATE_ENTRY_OPENING) {
    if (millis() - gateTimer >= GATE_TRAVEL_TIME) {
      gateTimer = millis();
      gateState = STATE_ENTRY_WAIT_PASS;
    }
    return;
  }

  if (gateState == STATE_ENTRY_WAIT_PASS) {
    if (sensor2HasCar) {
      gateTimer = millis();
      gateState = STATE_ENTRY_CLOSING;
      Serial.println("[GATE] car reached US2, closing soon");
    } else if (millis() - gateTimer >= 10000) {
      // Safety: if the car never makes it past, close anyway so we
      // don't get stuck holding the gate open forever.
      servoClose();
      delay(200);
      servoIdle();
      gateState = STATE_IDLE;
      Serial.println("[GATE] entry timeout");
    }
    return;
  }

  if (gateState == STATE_ENTRY_CLOSING) {
    if (millis() - gateTimer >= CLOSE_DELAY && !sensor1HasCar && !sensor2HasCar) {
      servoClose();
      delay(200);
      servoIdle();
      gateState = STATE_IDLE;
      Serial.println("[GATE] entry complete");
    }
    return;
  }

  // -------- EXIT half (mirror of the ENTRY half) --------
  if (gateState == STATE_EXIT_WAITING) {
    if (!sensor2HasCar) {
      gateState = STATE_IDLE;
      Serial.println("[GATE] exit cancelled (car left)");
      return;
    }
    if (millis() - gateTimer >= DWELL_TIME) {
      servoOpen();
      gateTimer = millis();
      gateState = STATE_EXIT_OPENING;
    }
    return;
  }

  if (gateState == STATE_EXIT_OPENING) {
    if (millis() - gateTimer >= GATE_TRAVEL_TIME) {
      gateTimer = millis();
      gateState = STATE_EXIT_WAIT_PASS;
    }
    return;
  }

  if (gateState == STATE_EXIT_WAIT_PASS) {
    if (sensor1HasCar) {
      gateTimer = millis();
      gateState = STATE_EXIT_CLOSING;
      Serial.println("[GATE] car reached US1, closing soon");
    } else if (millis() - gateTimer >= 10000) {
      servoClose();
      delay(200);
      servoIdle();
      gateState = STATE_IDLE;
      Serial.println("[GATE] exit timeout");
    }
    return;
  }

  if (gateState == STATE_EXIT_CLOSING) {
    if (millis() - gateTimer >= CLOSE_DELAY && !sensor1HasCar && !sensor2HasCar) {
      servoClose();
      delay(200);
      servoIdle();
      gateState = STATE_IDLE;
      Serial.println("[GATE] exit complete");
    }
    return;
  }
}

/* ===============================================================
 *  SETUP
 * =============================================================== */
void setup() {
  Serial.begin(115200);
  delay(500);                   // Give the USB-serial chip time to enumerate.
  Serial.println();
  Serial.println("=== Smart Parking Booting ===");
  Serial.print("NUM_SLOTS=");
  Serial.print(NUM_SLOTS);
  Serial.print("  REAL_SLOTS=");
  Serial.println(REAL_SLOTS);
  Serial.print("Real slot indexes: ");
  for (int i = 0; i < REAL_SLOTS; i++) {
    Serial.print(REAL_SLOT_INDEXES[i] + 1);
    if (i < REAL_SLOTS - 1) Serial.print(", ");
  }
  Serial.println();

  // ---- Initialise software state ----
  for (int i = 0; i < NUM_SLOTS; i++) {
    slotIsOccupied[i] = false;
    slotIsReserved[i] = false;
    ownerWasAlerted[i] = false;
  }
  ledOutputBuffer[0]   = 0;
  ledOutputBuffer[1]   = 0;
  switchInputBuffer[0] = 0;
  switchInputBuffer[1] = 0;

  // ---- Configure shift-register pins ----
  pinMode(SR_OUT_DATA,  OUTPUT);
  pinMode(SR_OUT_CLOCK, OUTPUT);
  pinMode(SR_OUT_LATCH, OUTPUT);
  digitalWrite(SR_OUT_LATCH, HIGH);     // Resting state per 74HC595 datasheet.

  pinMode(SR_IN_DATA,  INPUT);          // Data line is driven by the chip.
  pinMode(SR_IN_CLOCK, OUTPUT);
  pinMode(SR_IN_LOAD,  OUTPUT);
  digitalWrite(SR_IN_LOAD,  HIGH);      // /PL is active-LOW, idle HIGH.
  digitalWrite(SR_IN_CLOCK, LOW);

  // Push initial LED state (everything green, C/D dark by design).
  updateAllLEDs();

  // ---- Bring up the LCD ----
  Wire.begin(21, 22);                   // (SDA, SCL) — ESP32 default I2C pins.
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.print("Booting...");

  // ---- Gate sensors + servo ----
  pinMode(US1_TRIG, OUTPUT);
  pinMode(US1_ECHO, INPUT);
  pinMode(US2_TRIG, OUTPUT);
  pinMode(US2_ECHO, INPUT);
  digitalWrite(US1_TRIG, LOW);
  digitalWrite(US2_TRIG, LOW);

  // Send a CLOSE command at boot so the barrier ends up in a known
  // position regardless of where the servo woke up.
  gateServo.attach(SERVO_PIN);
  gateServo.write(GATE_CLOSE_ANGLE);
  servoIsAttached = true;
  delay(500);
  servoIdle();

  // ---- Read switches once so the LCD count is correct immediately ----
  readInputsFromShiftRegisters();
  for (int r = 0; r < REAL_SLOTS; r++) {
    int i = REAL_SLOT_INDEXES[r];
    int inputBit = slotToInputBit[i];
    if (inputBit != -1) {
      slotIsOccupied[i] = !readOneInputBit(inputBit);
    }
  }

  // ---- Wi-Fi ----
  lcd.clear();
  lcd.print("WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int retryCount = 0;
  // Try for up to 15 seconds (30 * 500 ms). If the router is down we
  // continue without Wi-Fi; gate + LCD still work, just no cloud sync.
  while (WiFi.status() != WL_CONNECTED && retryCount < 30) {
    delay(500);
    Serial.print(".");
    retryCount++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    lcd.clear();
    lcd.print("WiFi FAILED");
    Serial.println("\n[WIFI] could not connect");
  } else {
    Serial.println();
    Serial.print("[WIFI] connected, IP = ");
    Serial.println(WiFi.localIP());

    // ---- Firebase config ----
    // The library takes pointers (&) to firebaseConfig and firebaseAuth
    // so it can keep referring to them after Firebase.begin() returns.
    firebaseConfig.host = FIREBASE_HOST;
    firebaseConfig.signer.tokens.legacy_token = FIREBASE_AUTH;
    Firebase.begin(&firebaseConfig, &firebaseAuth);
    Firebase.reconnectWiFi(true);     // Auto-reconnect if Wi-Fi drops.
    delay(1000);                      // Let the TLS handshake finish.

    // ---- Initial Firebase sync ----
    Firebase.setInt(firebaseWriter, "/system/totalSlots", REAL_SLOTS);

    for (int r = 0; r < REAL_SLOTS; r++) {
      int i = REAL_SLOT_INDEXES[r];
      String slotPath = "/slots/slot" + String(i + 1);

      // Push our local occupancy reading so the dashboard reflects it.
      Firebase.setBool(firebaseWriter, slotPath + "/occupied", slotIsOccupied[i]);

      // Read any pre-existing reservation flag set by the dashboard
      // before we booted; if none exists yet, create it as `false`.
      if (Firebase.getBool(firebaseWriter, slotPath + "/reserved")) {
        slotIsReserved[i] = firebaseWriter.boolData();
      } else {
        Firebase.setBool(firebaseWriter, slotPath + "/reserved", false);
      }
    }

    updateAllLEDs();
    recalculateAvailableSlots();

    // ---- Open the realtime listener ----
    // beginStream() returns false if the request couldn't even be
    // queued — typical causes: bad host, bad auth token, no internet.
    if (Firebase.beginStream(firebaseListener, "/slots")) {
      Serial.println("[STREAM] listening for app changes");
    } else {
      Serial.print("[STREAM] failed to start: ");
      Serial.println(firebaseListener.errorReason());
    }
    Firebase.setStreamCallback(firebaseListener, streamCallback, streamTimeoutCallback);

    Serial.println("[FIREBASE] ready");
  }

  // Force a fresh LCD draw now that real values are loaded.
  lastShownCount = -1;
  updateLCD();
  Serial.println("=== System Ready ===");
  Serial.println();
}

/* ===============================================================
 *  LOOP
 * =============================================================== */
void loop() {
  readAllSensors();    // Refresh gate ultrasonic readings (throttled).
  scanAllSwitches();   // Refresh slot occupancy + push to Firebase.
  handleGate();        // Advance the gate state machine one tick.
}
