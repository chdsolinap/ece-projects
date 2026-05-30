# Smart Parking System — Step-by-Step Build Guide

---

## Overview

The system is built in five stages. Follow them in order, testing each before moving on.

---

## Stage 1 — Hardware Setup

### What you need

**Per parking slot (×N):**
- 1× Lever-type limit switch
- 1× Bi-color LED (common cathode) or 2× separate LEDs (red + green)

**Shared hardware:**
- 1× ESP32 (WROOM-32 or WROVER)
- 1× 16×2 LCD with I2C backpack (address 0x27)
- 2× Servo motors (one for entry gate, one for exit gate)
- 4× HC-SR04 ultrasonic sensors (2 per gate)
- Jumper wires, breadboard or PCB, 5V power supply

### Wiring the limit switches
- One terminal to a GPIO pin, other terminal to GND.
- Enable INPUT_PULLUP in code (done in setup()).
- When car presses switch: pin reads LOW = occupied.

### Wiring the bi-color LEDs
- Common cathode to GND.
- Red anode → 220Ω resistor → GPIO (ledRed pin).
- Green anode → 220Ω resistor → GPIO (ledGreen pin).

### Wiring the ultrasonic sensors (HC-SR04)
- VCC → 5V, GND → GND
- TRIG → GPIO (output), ECHO → GPIO (input, use voltage divider 1kΩ+2kΩ for 3.3V ESP32)

### Wiring the servos
- Signal → GPIO, VCC → external 5V (not ESP32's 3.3V), GND shared with ESP32.

### Wiring the LCD (I2C)
- SDA → GPIO 21, SCL → GPIO 22, VCC → 5V, GND → GND.

---

## Stage 2 — Configuring the Code for Your Hardware

Open `SmartParking.ino`. The **only section you need to edit** is:

```cpp
#define NUM_SLOTS 10   // change this to your slot count

const SlotConfig SLOT_CONFIGS[NUM_SLOTS] = {
  // { switchPin, redPin, greenPin }
  {  4,  5,  6  },  // Slot 1
  ...
};
```

Set the pin numbers to match your actual wiring. That's it — everything else adapts.

Also fill in your WiFi and Firebase credentials at the top of the file.

---

## Stage 3 — Firebase Setup

1. Go to https://console.firebase.google.com
2. Create a new project (e.g. "SmartParking").
3. Go to **Realtime Database** → Create Database → Start in test mode.
4. Copy the database URL (e.g. `yourproject-default-rtdb.firebaseio.com`).
5. Go to **Project Settings** → **Service Accounts** → copy the **Database secret** (legacy token).
6. Paste both into your `.ino` file:
   ```cpp
   #define FIREBASE_HOST  "yourproject-default-rtdb.firebaseio.com"
   #define FIREBASE_AUTH  "your_database_secret_here"
   ```
7. Import `firebase_rules.json` under **Realtime Database → Rules** to secure your data.

### Firebase data structure (auto-created on first boot):
```
/system/
  totalSlots: 10
  availableSlots: 8

/slots/
  slot1/
    occupied: false
    reserved: false
    notifyOwner: false
    notifyMessage: ""
  slot2/ ...

/events/
  (push log of entry_confirmed / exit_confirmed)
```

---

## Stage 4 — Installing Arduino Libraries

In Arduino IDE → Library Manager, install:

| Library | Purpose |
|---|---|
| `LiquidCrystal_I2C` by Frank de Brabander | LCD display |
| `ESP32Servo` by Kevin Harrington | Servo on ESP32 |
| `Firebase ESP32 Client` by Mobizt | Firebase integration |

Board: Select **ESP32 Dev Module** under ESP32 Arduino.

---

## Stage 5 — Testing Checklist

Work through these in order:

- [ ] Upload code, open Serial Monitor at 115200 baud.
- [ ] Confirm "WiFi connected" and "System ready" messages appear.
- [ ] LCD shows "Available Slots: 10 / 10".
- [ ] Press each limit switch manually → LED turns RED, count decrements.
- [ ] Release switch → LED turns GREEN, count increments.
- [ ] Open Firebase console — confirm `/slots/slot1/occupied` changes in real time.
- [ ] Hold object in front of Entry US1 for 2 seconds → entry gate servo opens.
- [ ] Trigger Entry US2 → gate closes after delay.
- [ ] Repeat for exit gate (mirror logic).
- [ ] In Firebase, set `/slots/slot3/reserved = true` → LED 3 turns RED even with no car.
- [ ] Place car on slot 3's limit switch → `/notifyOwner` flag appears in Firebase.

---

## Scalability Notes

The entire system is designed around `NUM_SLOTS` and `SLOT_CONFIGS[]`.

- To add 5 more slots: change `#define NUM_SLOTS 15` and add 5 entries to `SLOT_CONFIGS`.
- No logic, no gate code, no Firebase sync code needs to change.
- The ESP32 has enough GPIO pins for ~15–20 slots before you need an I2C GPIO expander (e.g. PCF8574). If you need more, use a `PCF8574` library and wrap `digitalRead`/`digitalWrite` calls with your expander's equivalent.

---

## Wiring a Second ESP32 (for very large deployments)

If you exceed ~20 slots, use a second ESP32 as a "slot node" that only reads switches and controls LEDs, and communicates its state to the main ESP32 via Firebase. The main ESP32 handles gates and LCD only. The slot node code is a simplified version of the main sketch with no gate logic.

---

## Owner App

For the app, use either:

- **MIT App Inventor** (easiest) — drag-and-drop UI, direct Firebase integration.
- **FlutterFlow** (polished) — generates Flutter code, Firebase built in.
- **React Native + Firebase SDK** (most control) — full custom app.

In Firebase, your app reads `/slots/*` to show the dashboard and writes `reserved: true/false` to toggle reservations. The ESP32 picks this up on its next poll cycle (every 2 seconds by default).

