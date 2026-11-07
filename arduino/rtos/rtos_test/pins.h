// pins.h - Team 7 box wiring (Adafruit Feather RP2040 RFM). Same on every box.
// All Grove modules: red -> 3V, black -> GND. Never USB/BAT (5 V would reach the RP2040 pins).
#pragma once

#define PIN_BAR_CLOCK 9      // LED Bar v2.1  white
#define PIN_BAR_DATA  10     // LED Bar v2.1  yellow
#define PIN_BUZZER    11     // Buzzer        yellow
#define PIN_SWITCH    12     // Switch(P)     yellow  (CARRY / DEPLOY)
#define PIN_BTN1_LED  5      // LED Button 1  yellow / SIG1 (LED)
#define PIN_BTN1      6      // LED Button 1  white  / SIG2 (button: HIGH idle, LOW pressed)
#define PIN_BTN2_LED  24     // LED Button 2  yellow / SIG1 (LED)
#define PIN_BTN2      25     // LED Button 2  white  / SIG2

// Radio (fixed on the board): RFM95 on SPI, CS 16, RST 17, DIO0 21
// SHT41 (and an OLED later) on STEMMA QT = Wire (SDA 2, SCL 3)
