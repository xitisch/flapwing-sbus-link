/*
 * esp_now_link.h
 *
 * Shared definition of the ESP-NOW payload exchanged between the SBUS
 * transmitter and receiver nodes.
 *
 * NOTE: Arduino sketch folders are self-contained, so an identical copy of
 * this file lives in both firmware/transmitter/ and firmware/receiver/.
 * If you change the payload, update BOTH copies so the structs stay binary
 * compatible.
 */

#ifndef ESP_NOW_LINK_H
#define ESP_NOW_LINK_H

#include <stdint.h>

// Payload sent over ESP-NOW. Channel values are in microseconds [1000, 2000].
// The failsafe byte lets the transmitter report loss of its host GUI while the
// ESP-NOW radio link itself is still working.
typedef struct __attribute__((packed)) {
  uint16_t ch[16];
  uint8_t failsafe;
} SbusPacket;

// Both ESP-NOW nodes must use the same 2.4 GHz Wi-Fi channel.
static const uint8_t ESPNOW_WIFI_CHANNEL = 1;

// If no packet is received within this window the receiver engages failsafe.
static const uint32_t LINK_TIMEOUT_MS = 500;

#endif  // ESP_NOW_LINK_H
