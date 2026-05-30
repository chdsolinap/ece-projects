/*
 * Debug version: scanAllSwitches() prints what it reads every cycle.
 * This is a focused test to find where in the loop the read fails.
 *
 * To use: replace the scanAllSwitches() function in your SmartParking.ino
 * with this debug version. Don't replace anything else.
 *
 * Then watch Serial Monitor while pressing switches.
 *
 * Expected output every ~1 second:
 *   [DEBUG] bits: 11111111 11111111
 * When pressing slot 1:
 *   [DEBUG] bits: 11111110 11111111
 *   [SWITCH 1] occupied
 *
 * If you see bits change but no [SWITCH] message, scanAllSwitches has a bug.
 * If bits never change, the read is being corrupted by something in the loop.
 */

void scanAllSwitches() {
  static unsigned long lastDebug = 0;

  // Read fresh data from the shift registers
  readInputsFromShiftRegisters();

  // DEBUG: print bits every 500ms
  if (millis() - lastDebug > 500) {
    lastDebug = millis();
    Serial.print("[DEBUG] bits: ");
    for (int b = 7; b >= 0; b--) Serial.print((switchInputBuffer[0] >> b) & 1);
    Serial.print(" ");
    for (int b = 7; b >= 0; b--) Serial.print((switchInputBuffer[1] >> b) & 1);
    Serial.println();
  }

  for (int i = 0; i < NUM_SLOTS; i++) {
    bool switchIsPressed = !readOneInputBit(i);

    if (switchIsPressed != slotIsOccupied[i]) {
      bool wasReserved = slotIsReserved[i];
      slotIsOccupied[i] = switchIsPressed;

      if (switchIsPressed) {
        if (wasReserved) {
          if (!ownerWasAlerted[i] && WiFi.status() == WL_CONNECTED) {
            String slotPath = "/slots/slot" + String(i + 1);
            Firebase.setBool(firebaseWriter, slotPath + "/notifyOwner", true);
            Firebase.setString(firebaseWriter, slotPath + "/notifyMessage",
              "Reserved slot " + String(i + 1) + " has been taken.");
            ownerWasAlerted[i] = true;
            Serial.print("[ALERT] Reserved slot ");
            Serial.print(i + 1);
            Serial.println(" was taken — owner notified");
          }
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
