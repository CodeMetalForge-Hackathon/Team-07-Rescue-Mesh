// displink.h - sends screen content to the ESP32-S3 Reverse TFT over I2C.
// The ESP32 runs arduino/display_esp32 as an I2C *device* at address 0x42 and draws what it gets.
// This box (the RP2040) is the I2C controller - the same bus as the SHT41.
//
// Wiring (STEMMA QT to STEMMA QT, or 3 jumpers):
//   RP2040 SDA (GPIO 2) -- ESP32 SDA (GPIO 3)
//   RP2040 SCL (GPIO 3) -- ESP32 SCL (GPIO 4)
//   GND                 -- GND
//   Do NOT join the boards' 3V pins (the red STEMMA QT wire): power both from USB (USB pin <-> USB pin).
//
// Addresses on this bus: SHT41 0x44, ESP32 screen 0x42, the ESP board's own battery monitor 0x36.
//
// Lines (text always last so it may contain commas):
//   D,STATUS,<id>,<mode>,<temp|->,<rh|->,<flag R/S/->     header
//   D,MSG,<from>,<rssi|0>,<text>                         message received
//   D,SENT,<seq>,<text>                                  we sent something (waits for ACKs)
//   D,ACK,<seq>,<by>                                     <by> confirmed our <seq>; "-" = nobody did
//   D,NODE,<id>,<rssi>                                   heard a box (bottom strip)
//   D,WARN,<text>                                        warning banner ('|' = line break); empty clears it
// Each line is sent as: 0x02 (start marker) + text + '\n', split into 30-byte I2C writes
// (the ESP32's I2C receive buffer is 128 bytes). Only the task that owns Wire may call displinkSend().
#pragma once
#include <Arduino.h>
#include <Wire.h>

#define DISPLAY_I2C_ADDR 0x42
#define DISPLAY_CHUNK    30

enum DispType : uint8_t { D_MSG, D_SENT, D_ACK, D_NODE, D_STATUS, D_WARN };
struct DispMsg {
  DispType type;
  char from[9];       // MSG: sender | ACK: who confirmed ("-" = nobody) | NODE: box id
  char text[64];      // MSG/SENT/WARN: text (max 60) | STATUS: mode ("CARRY"/"DEPLOY"/"BASE")
  int16_t rssi;       // MSG / NODE (0 = not applicable)
  uint16_t seq;       // SENT / ACK
  float temp, rh;     // STATUS
  uint8_t flag;       // STATUS: 1 = real reading, 0 = simulated, 2 = none yet
};

// Turn a DispMsg into one text line (no newline).
inline void displinkFormat(const DispMsg& m, const char* myId, char* line, int max) {
  switch (m.type) {
    case D_MSG:  snprintf(line, max, "D,MSG,%s,%d,%s", m.from, m.rssi, m.text); break;
    case D_SENT: snprintf(line, max, "D,SENT,%u,%s", m.seq, m.text); break;
    case D_ACK:  snprintf(line, max, "D,ACK,%u,%s", m.seq, m.from); break;
    case D_NODE: snprintf(line, max, "D,NODE,%s,%d", m.from, m.rssi); break;
    case D_WARN: snprintf(line, max, "D,WARN,%s", m.text); break;
    case D_STATUS:
      if (m.flag == 2) {
        snprintf(line, max, "D,STATUS,%s,%s,-,-,-", myId, m.text);
      } else {
        long t10 = lroundf(m.temp * 10), h = lroundf(m.rh);
        snprintf(line, max, "D,STATUS,%s,%s,%s%ld.%ld,%ld,%s", myId, m.text,
                 t10 < 0 ? "-" : "", labs(t10) / 10, labs(t10) % 10, h, m.flag ? "R" : "S");
      }
      break;
  }
}

// Send one line to the screen. Returns false if the screen didn't answer (not connected / not running).
inline bool displinkSend(const char* line) {
  uint8_t buf[128];
  size_t n = strlen(line);
  if (n > sizeof buf - 2) n = sizeof buf - 2;
  buf[0] = 0x02;                       // start marker: the screen drops any half-received line
  memcpy(buf + 1, line, n);
  buf[n + 1] = '\n';
  n += 2;
  for (size_t off = 0; off < n; off += DISPLAY_CHUNK) {
    size_t k = (n - off < DISPLAY_CHUNK) ? n - off : DISPLAY_CHUNK;
    Wire.beginTransmission(DISPLAY_I2C_ADDR);
    Wire.write(buf + off, k);
    if (Wire.endTransmission() != 0) return false;
  }
  return true;
}
