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
constexpr uint32_t SBUS_STARTUP_DELAY_MS = 5000;
constexpr uint32_t SBUS_FRAME_INTERVAL_MS = 10;
constexpr uint16_t SBUS_MIN_VALUE = 172;
constexpr uint16_t SBUS_NEUTRAL_VALUE = 992;
constexpr uint16_t SBUS_MAX_VALUE = 1811;
// Reserved analog input for a future divided battery-voltage signal. The
// firmware reports only the raw ADC reading; never connect a battery directly.
static constexpr int8_t BATTERY_SENSE_PIN = 3;
bfs::SbusTx sbus(&Serial1, -1, SBUS_TX_PIN, true);

// ==================== RECEIVER VALUE TEST ====================
// Comment out the next line after testing to disable continuous test output.
// #define RECEIVER_VALUE_TEST

#ifdef RECEIVER_VALUE_TEST
static constexpr uint32_t RECEIVER_VALUE_TEST_INTERVAL_MS = 250;
#endif
// ================== END RECEIVER VALUE TEST ==================

// ESP-NOW changed its receive callback's first parameter in Arduino-ESP32 3.x.
// A single version-selected alias also keeps Arduino's .ino prototype generator
// from exposing the 3.x-only type when PlatformIO builds with core 2.x.
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
using EspNowRecvInfo = esp_now_recv_info_t;
#else
using EspNowRecvInfo = uint8_t;
#endif

uint16_t sbusChannels[16];

// Link-state, updated from the ESP-NOW receive callback
volatile uint32_t lastPacketMs = 0;
volatile bool linkActive = false;
volatile bool remoteFailsafe = true;
volatile bool packetReceived = false;
volatile uint32_t packetsReceived = 0;

// A valid forward packet tells the receiver which transmitter MAC should get
// the unicast telemetry response. Peer setup and sending happen in loop(), not
// inside ESP-NOW's receive callback.
portMUX_TYPE peerMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t pendingTransmitterMac[6] = {};
volatile bool transmitterPeerPending = false;
uint8_t transmitterMac[6] = {};
bool transmitterPeerReady = false;
uint32_t lastStatusSendMs = 0;

// Safe channel values: used at startup and when the link is lost.
void applyFailsafe() {
  for (int i = 0; i < 16; i++) {
    sbusChannels[i] = SBUS_NEUTRAL_VALUE;
  }
  sbusChannels[2] = SBUS_MIN_VALUE;  // CH3: throttle to minimum
  sbusChannels[7] = SBUS_MIN_VALUE;  // CH8: throttle lock disarmed
}

uint16_t channelValueToSbus(uint16_t channelValue) {
  const uint16_t clamped = constrain(channelValue, 1000, 2000);
  const uint32_t scaled =
      static_cast<uint32_t>(clamped - 1000) *
      (SBUS_MAX_VALUE - SBUS_MIN_VALUE);
  return SBUS_MIN_VALUE + (scaled + 500) / 1000;
}

bool handlePacket(const uint8_t *data, int len) {
  if (len != sizeof(SbusPacket)) {
    return false;  // ignore malformed / foreign packets
  }
  SbusPacket packet;
  memcpy(&packet, data, sizeof(packet));

  for (int i = 0; i < 16; i++) {
    sbusChannels[i] = channelValueToSbus(packet.ch[i]);
  }
  remoteFailsafe = packet.failsafe != 0;
  lastPacketMs = millis();
  linkActive = true;
  packetReceived = true;
  packetsReceived++;
  return true;
}

const uint8_t *sourceMac(const EspNowRecvInfo *info) {
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  return info->src_addr;
#else
  return info;
#endif
}

void onDataRecv(const EspNowRecvInfo *info, const uint8_t *data, int len) {
  if (!handlePacket(data, len)) {
    return;
  }

  const uint8_t *mac = sourceMac(info);
  portENTER_CRITICAL(&peerMux);
  memcpy(pendingTransmitterMac, mac, 6);
  transmitterPeerPending = true;
  portEXIT_CRITICAL(&peerMux);
}

void configureTransmitterPeer() {
  uint8_t candidateMac[6];
  bool pending = false;

  portENTER_CRITICAL(&peerMux);
  if (transmitterPeerPending) {
    memcpy(candidateMac, pendingTransmitterMac, 6);
    transmitterPeerPending = false;
    pending = true;
  }
  portEXIT_CRITICAL(&peerMux);

  if (!pending) {
    return;
  }
  if (transmitterPeerReady) {
    if (memcmp(candidateMac, transmitterMac, 6) == 0) {
      return;
    }
  }

  if (!esp_now_is_peer_exist(candidateMac)) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, candidateMac, 6);
    peer.channel = ESPNOW_WIFI_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
      return;
    }
  }

  memcpy(transmitterMac, candidateMac, 6);
  transmitterPeerReady = true;
}

void sendReceiverStatus() {
  if (!transmitterPeerReady) {
    return;
  }

  ReceiverStatusPacket status = {};
  status.magic = RECEIVER_STATUS_MAGIC;
  status.packets_received = packetsReceived;
  status.battery_adc_raw = analogRead(BATTERY_SENSE_PIN);
  status.version = RECEIVER_STATUS_VERSION;
  status.link_active = linkActive ? 1 : 0;
  status.failsafe = (!linkActive || remoteFailsafe) ? 1 : 0;
  esp_now_send(transmitterMac, reinterpret_cast<uint8_t *>(&status),
               sizeof(status));
}

void writeSbusFrame() {
  bfs::SbusData sbusData = {};
  for (int i = 0; i < 16; i++) {
    sbusData.ch[i] = sbusChannels[i];
  }
  sbusData.lost_frame = false;
  sbusData.failsafe = !linkActive || remoteFailsafe;
  sbusData.ch17 = false;
  sbusData.ch18 = false;

  sbus.data(sbusData);
  sbus.Write();
}

void setup() {
  const uint32_t startupMs = millis();

  // GPIO4 is high-impedance until the startup guard expires. Do not call
  // sbus.Begin() before then, because that enables UART1 on the pin.
  pinMode(SBUS_TX_PIN, INPUT);
  applyFailsafe();

  Serial.begin(115200);
  delay(2000);
  Serial.println("Receiver booted");

  pinMode(BATTERY_SENSE_PIN, INPUT);
  analogReadResolution(12);

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

  bool espNowReady = false;
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
  } else {
    espNowReady = true;
  }

  while (millis() - startupMs < SBUS_STARTUP_DELAY_MS) {
    delay(1);
  }

  // Enable the inverted 100 kbaud, 8E2 UART only after the guard interval.
  // The first frame is written before ESP-NOW reception is enabled, ensuring
  // that the first values placed on the SBUS wire are the safe raw values.
  applyFailsafe();
  linkActive = false;
  remoteFailsafe = true;
  sbus.Begin();
  writeSbusFrame();

  if (espNowReady) {
    esp_now_register_recv_cb(onDataRecv);
    Serial.println("ESP-NOW receiver ready");
  }
}

void loop() {
  configureTransmitterPeer();

  // Failsafe: if the link goes quiet, force the safe channel values
  if (linkActive && (millis() - lastPacketMs > LINK_TIMEOUT_MS)) {
    linkActive = false;
    remoteFailsafe = true;
    applyFailsafe();
    Serial.println("Link lost - failsafe engaged");
  }

  // Debug: handle a newly received ESP-NOW packet.
  if (packetReceived) {
    packetReceived = false;

// ==================== RECEIVER VALUE TEST ====================
#ifdef RECEIVER_VALUE_TEST
    // Print the latest received values at 4 Hz. Printing from loop() keeps
    // serial I/O out of the high-priority ESP-NOW receive callback.
    static uint32_t lastTestPrintMs = 0;
    if (millis() - lastTestPrintMs >= RECEIVER_VALUE_TEST_INTERVAL_MS) {
      lastTestPrintMs = millis();
      Serial.print("[RECEIVER TEST] CH1="); Serial.print(sbusChannels[0]);
      Serial.print(" CH2=");               Serial.print(sbusChannels[1]);
      Serial.print(" CH3=");               Serial.print(sbusChannels[2]);
      Serial.print(" CH5=");               Serial.print(sbusChannels[4]);
      Serial.print(" CH6=");               Serial.print(sbusChannels[5]);
      Serial.print(" CH8=");               Serial.print(sbusChannels[7]);
      Serial.print(" FAILSAFE=");          Serial.println(remoteFailsafe ? "YES" : "NO");
    }
#else
    // Normal debug behavior: print only when one of the used channels changes.
    static uint16_t lastPrinted[16] = {0};
    int dbg[] = {0, 1, 2, 4, 5, 7};  // CH1,CH2,CH3,CH5,CH6,CH8
    bool changed = false;
    for (int k = 0; k < 6; k++)
      if (sbusChannels[dbg[k]] != lastPrinted[dbg[k]]) changed = true;

    if (changed) {
      Serial.print("RX  yaw(CH1):");    Serial.print(sbusChannels[0]);
      Serial.print("  pitch(CH2):");    Serial.print(sbusChannels[1]);
      Serial.print("  thr(CH3):");      Serial.print(sbusChannels[2]);
      Serial.print("  trim1(CH5):");    Serial.print(sbusChannels[4]);
      Serial.print("  trim2(CH6):");    Serial.print(sbusChannels[5]);
      Serial.print("  thr_lock(CH8):"); Serial.println(sbusChannels[7]);
      for (int k = 0; k < 6; k++) lastPrinted[dbg[k]] = sbusChannels[dbg[k]];
    }
#endif
// ================== END RECEIVER VALUE TEST ==================
  }

  writeSbusFrame();

  if (millis() - lastStatusSendMs >= RECEIVER_STATUS_INTERVAL_MS) {
    lastStatusSendMs = millis();
    sendReceiverStatus();
  }

  delay(SBUS_FRAME_INTERVAL_MS);
}
