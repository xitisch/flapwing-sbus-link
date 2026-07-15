/*
 * SBUS.ino
 *
 * ESP32 firmware for transmitting SBUS signals to a flapping-wing robot.
 * Channel values are received over USB serial as comma-separated packets
 * and forwarded to the SBUS output on Serial2.
 *
 * Serial packet format (host -> ESP32):
 *   <CH1,CH2,CH3,CH5,CH6,CH8>
 *   All values in the range [1000, 2000] microseconds.
 *
 * SBUS hardware: GPIO17 (TX), inverted logic, 100kbaud, 8E2.
 *
 * Dependencies: bolderflight/sbus  (provides sbus.h / bfs::SbusTx)
 */

#include "sbus.h"

// SBUS TX on Serial2, GPIO17, inverted signal (SBUS uses active-low logic)
bfs::SbusTx sbus(&Serial2, 16, 17, true);

uint16_t userChannels[16];

void setup() {
  Serial.begin(115200);

  // SBUS requires 100kbaud, 8-bit, Even parity, 2 stop bits, inverted
  Serial2.begin(100000, SERIAL_8E2, 16, 17, true);

  // Default all channels to neutral
  for (int i = 0; i < 16; i++) {
    userChannels[i] = 1500;
  }

  // Override channels that need non-neutral safe defaults
  userChannels[0] = 1500;  // CH1: yaw     (neutral)
  userChannels[1] = 1500;  // CH2: pitch   (neutral)
  userChannels[2] = 1000;  // CH3: throttle (minimum — do not arm at neutral)
  userChannels[4] = 1500;  // CH5: trim 1  (neutral)
  userChannels[5] = 1500;  // CH6: trim 2  (neutral)
  userChannels[7] = 1000;  // CH8: throttle lock (locked / disarmed)
}

void loop() {
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    command.trim();

    // Packet format: <v0,v1,v2,v3,v4,v5>
    if (command.startsWith("<") && command.endsWith(">")) {
      String data = command.substring(1, command.length() - 1);

      // Map the 6 incoming values to their respective (non-contiguous) SBUS indices
      int targetChannels[] = {0, 1, 2, 4, 5, 7};
      int commaIndex = 0;
      int startIndex = 0;
      int idx = 0;

      while ((commaIndex = data.indexOf(',', startIndex)) != -1 && idx < 5) {
        int val = data.substring(startIndex, commaIndex).toInt();
        userChannels[targetChannels[idx]] = constrain(val, 1000, 2000);
        startIndex = commaIndex + 1;
        idx++;
      }
      // Parse the final value after the last comma
      if (idx <= 5 && startIndex < data.length()) {
        int val = data.substring(startIndex).toInt();
        userChannels[targetChannels[idx]] = constrain(val, 1000, 2000);
      }

      // Echo parsed values for debugging
      Serial.print("yaw(CH1):");      Serial.println(userChannels[0]);
      Serial.print("pitch(CH2):");    Serial.println(userChannels[1]);
      Serial.print("throttle(CH3):"); Serial.println(userChannels[2]);
      Serial.print("trim1(CH5):");    Serial.println(userChannels[4]);
      Serial.print("trim2(CH6):");    Serial.println(userChannels[5]);
      Serial.print("thr_lock(CH8):"); Serial.println(userChannels[7]);
      Serial.println();
    }
  }

  // Build and send the SBUS frame
  bfs::SbusData sbusData;
  for (int i = 0; i < 16; i++) {
    // Map RC microseconds [1000, 2000] to SBUS counts [172, 1811], matching a
    // standard SBUS receiver. CH8 throttle lock: 2000 -> 1811 (unlocked),
    // 1000 -> 172 (locked). This mapping is the tested-correct configuration.
    sbusData.ch[i] = map(userChannels[i], 1000, 2000, 172, 1811);
  }
  sbusData.lost_frame = false;
  sbusData.failsafe   = false;
  sbusData.ch17       = false;
  sbusData.ch18       = false;

  sbus.data(sbusData);
  sbus.Write();

  delay(14);  // ~14ms matches the standard SBUS frame interval
}
