// ledbar.h - Grove LED Bar v2 (MY9221) driver, no library needed.
// Protocol: 16-bit command word, 12 x 16-bit channel words (10 LEDs + 2 unused), latch.
// Call these from ONE task only (it's not thread-safe, like every Arduino driver).
#pragma once
#include <Arduino.h>
#include "pins.h"

inline void barWord(uint16_t w) {
  for (int i = 0; i < 16; i++) {
    digitalWrite(PIN_BAR_DATA, (w & 0x8000) ? HIGH : LOW);
    delayMicroseconds(1);
    digitalWrite(PIN_BAR_CLOCK, (i & 1) ? LOW : HIGH);   // data is taken on every clock edge
    delayMicroseconds(1);
    w <<= 1;
  }
}

// v[0..9] = brightness 0..255 per segment, v[0] = green end
inline void barShow(const uint8_t* v) {
  barWord(0x0000);                          // command: 8-bit grayscale
  for (int i = 0; i < 10; i++) barWord(v[i]);
  barWord(0); barWord(0);                   // 2 unused channels
  digitalWrite(PIN_BAR_DATA, LOW);          // latch: hold > 220 us, then 4 data pulses
  delayMicroseconds(300);
  for (int i = 0; i < 4; i++) {
    digitalWrite(PIN_BAR_DATA, HIGH); delayMicroseconds(1);
    digitalWrite(PIN_BAR_DATA, LOW);  delayMicroseconds(1);
  }
  delayMicroseconds(10);
}

inline void barBegin() {
  pinMode(PIN_BAR_CLOCK, OUTPUT); pinMode(PIN_BAR_DATA, OUTPUT);
  digitalWrite(PIN_BAR_CLOCK, LOW); digitalWrite(PIN_BAR_DATA, LOW);
  delayMicroseconds(500);
}

// Light the first n segments (0..10). Dim by default: the full-bright bar draws >100 mA.
inline void barLevel(int n, uint8_t brightness = 60) {
  uint8_t v[10];
  for (int i = 0; i < 10; i++) v[i] = (i < n) ? brightness : 0;
  barShow(v);
}
