// pins.h - Team 7 box wiring. Same on every box except the blue button on the M0 box.
// All Grove modules: red -> 3V, black -> GND. Never USB/BAT (5 V would reach the chip's pins).
#pragma once

#define PIN_BAR_CLOCK 9      // LED Bar v2.1  white
#define PIN_BAR_DATA  10     // LED Bar v2.1  yellow
#define PIN_BUZZER    11     // Buzzer        yellow
#define PIN_SWITCH    12     // Switch(P)     yellow  (CARRY / DEPLOY)
#define PIN_BTN1_LED  5      // LED Button 1 (red)  yellow / SIG1 (LED)
#define PIN_BTN1      6      // LED Button 1 (red)  white  / SIG2 (button: HIGH idle, LOW pressed)

#if defined(ARDUINO_ARCH_SAMD)
// ---- Adafruit Feather M0 RFM95 (#3178)
// D24 is the radio's SPI clock on this board, so the blue button moved to D1 / D0 (TX / RX pins).
#define PIN_BTN2_LED  1      // LED Button 2 (blue) yellow / SIG1 (LED)
#define PIN_BTN2      0      // LED Button 2 (blue) white  / SIG2
// Radio (fixed on the board): RFM95 on SPI, CS 8, RST 4, IRQ 3
#define PIN_RFM_CS    8
#define PIN_RFM_RST   4
#define PIN_RFM_DIO0  3
// I2C = Wire on the SDA / SCL pins (no STEMMA QT plug): SHT41 0x44, ESP32-S3 screen board 0x42
#else
// ---- Adafruit Feather RP2040 RFM
#define PIN_BTN2_LED  24     // LED Button 2 (blue) yellow / SIG1 (LED)
#define PIN_BTN2      25     // LED Button 2 (blue) white  / SIG2
// Radio (fixed on the board): RFM95 on SPI, CS 16, RST 17, DIO0 21  (set in rtos_main.ino)
// I2C (STEMMA QT) = Wire (SDA 2, SCL 3): SHT41 0x44, ESP32-S3 screen board 0x42
// TX/RX (GPIO 0/1) are left free for your own digital I/O
#endif
