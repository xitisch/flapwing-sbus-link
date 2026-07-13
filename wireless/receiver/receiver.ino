/*
 * receiver.ino
 *
 * ESP32-C3 SuperMini firmware for the SBUS *receiver* node (mounted on the
 * robot).
 * Receives channel values from the transmitter over ESP-NOW and forwards
 * them to the robot as an SBUS signal on Serial1 (GPIO4, inverted, 100k 8E2).
 *
 * Failsafe: if no packet arrives within LINK_TIMEOUT_MS the output drops to a
 * safe state (throttle minimum, SBUS failsafe flag set) until the link recovers.
 */

#include <esp_now.h>
#include <esp_arduino_version.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include "sbus.h"
#include "esp_now_link.h"

#if !defined(CONFIG_IDF_TARGET_ESP32C3)
#error "Select an ESP32-C3 board (for example, ESP32C3 Dev Module)."
#endif

// The C3 has UART0 and UART1 only. Keep UART0/native USB available for debug
// output and dedicate UART1 to the inverted SBUS signal. GPIO4 is exposed on
// the SuperMini and avoids its boot, LED, and native-USB pins.
static constexpr int8_t SBUS_TX_PIN = 4;
bfs::SbusTx sbus(&Serial1, -1, SBUS_TX_PIN, true);

// ESP-NOW changed its receive callback's first parameter in Arduino-ESP32 3.x.
// A single version-selected alias also keeps Arduino's .ino prototype generator
// from exposing the 3.x-only type when PlatformIO builds with core 2.x.
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
using EspNowRecvInfo = esp_now_recv_info_t;
#else
using EspNowRecvInfo = uint8_t;
#endif

uint16_t userChannels[16];

// Link-state, updated from the ESP-NOW receive callback
volatile uint32_t lastPacketMs = 0;
volatile bool linkActive = false;
volatile bool packetReceived = false;

// Safe channel values: used at startup and when the link is lost.
void applyFailsafe() {
  for (int i = 0; i < 16; i++) {
    userChannels[i] = 1500;
  }
  userChannels[2] = 1000;  // CH3: throttle to minimum
  userChannels[7] = 1000;  // CH8: throttle lock disarmed
}

void handlePacket(const uint8_t *data, int len) {
  if (len != sizeof(SbusPacket)) {
    return;  // ignore malformed / foreign packets
  }
  SbusPacket packet;
  memcpy(&packet, data, sizeof(packet));

  for (int i = 0; i < 16; i++) {
    userChannels[i] = constrain(packet.ch[i], 1000, 2000);
  }
  lastPacketMs = millis();
  linkActive = true;
  packetReceived = true;
}

void onDataRecv(const EspNowRecvInfo *, const uint8_t *data, int len) {
  handlePacket(data, len);
}

void setup() {
  Serial.begin(115200);

  // Configures UART1 for 100kbaud, 8E2, and inverted output on SBUS_TX_PIN.
  sbus.Begin();

  // Start in the failsafe state until the first packet arrives
  applyFailsafe();

  // ESP-NOW runs on Wi-Fi in station mode, disconnected from any AP
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (esp_wifi_set_channel(ESPNOW_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
    Serial.println("Unable to set ESP-NOW Wi-Fi channel");
  }

  uint8_t mac[6] = {};
  if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
    Serial.printf("Receiver MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  } else {
    Serial.println("Unable to read receiver MAC");
  }
  Serial.printf("SBUS output: GPIO%d\n", SBUS_TX_PIN);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_register_recv_cb(onDataRecv);
  Serial.println("ESP-NOW receiver ready");
}

void loop() {
  // Failsafe: if the link goes quiet, force the safe channel values
  if (linkActive && (millis() - lastPacketMs > LINK_TIMEOUT_MS)) {
    linkActive = false;
    applyFailsafe();
    Serial.println("Link lost - failsafe engaged");
  }

  // Debug: print channels when a new packet arrives and a value changed
  if (packetReceived) {
    packetReceived = false;

    static uint16_t lastPrinted[16] = {0};
    int dbg[] = {0, 1, 2, 4, 5, 7};  // CH1,CH2,CH3,CH5,CH6,CH8
    bool changed = false;
    for (int k = 0; k < 6; k++)
      if (userChannels[dbg[k]] != lastPrinted[dbg[k]]) changed = true;

    if (changed) {
      Serial.print("RX  yaw(CH1):");    Serial.print(userChannels[0]);
      Serial.print("  pitch(CH2):");    Serial.print(userChannels[1]);
      Serial.print("  thr(CH3):");      Serial.print(userChannels[2]);
      Serial.print("  trim1(CH5):");    Serial.print(userChannels[4]);
      Serial.print("  trim2(CH6):");    Serial.print(userChannels[5]);
      Serial.print("  thr_lock(CH8):"); Serial.println(userChannels[7]);
      for (int k = 0; k < 6; k++) lastPrinted[dbg[k]] = userChannels[dbg[k]];
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
  sbusData.failsafe   = !linkActive;
  sbusData.ch17       = false;
  sbusData.ch18       = false;

  sbus.data(sbusData);
  sbus.Write();

  delay(14);  // ~14ms matches the standard SBUS frame interval
}
