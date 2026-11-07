// hw_test.ino - checks the Grove modules on one box, without the radio.
// Board: Adafruit Feather RP2040 RFM. Library: "Grove LED Bar" by Seeed Studio.
//
// Wiring (red -> 3V, black -> GND on every module):
//   LED Bar v2.1 : yellow (data) -> D10, white (clock) -> D9
//   Buzzer v1.3  : yellow -> D11
//   Switch(P)    : yellow -> D12
//
// Open the Serial Monitor at 115200 and follow the printed steps.

#include <Grove_LED_Bar.h>

#define PIN_BAR_CLOCK 9
#define PIN_BAR_DATA  10
#define PIN_BUZZER    11
#define PIN_SWITCH    12

#define BAR_BRIGHTNESS 0.5f   // 0..1. Keep it dim: the full-bright bar draws >100 mA

Grove_LED_Bar* bar = nullptr;   // created in setup(), not at global scope

void showLevel(int n) {           // light the first n segments, dimmed
  for (int i = 1; i <= 10; i++) bar->setLed(i, i <= n ? BAR_BRIGHTNESS : 0.0f);
}

void beepDigital(int ms) {        // for an "active" buzzer: HIGH = sound
  digitalWrite(PIN_BUZZER, HIGH);
  delay(ms);
  digitalWrite(PIN_BUZZER, LOW);
}

void beepTone(int ms) {           // for a "passive" buzzer: needs a frequency
  tone(PIN_BUZZER, 2700, ms);
  delay(ms);
  noTone(PIN_BUZZER);
  digitalWrite(PIN_BUZZER, LOW);
}

const char* switchName(int v) { return v == HIGH ? "HIGH" : "LOW"; }

int lastSwitch = -1;

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 5000) delay(10);

  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_SWITCH, INPUT);     // Grove Switch(P) drives its own HIGH/LOW
  bar = new Grove_LED_Bar(PIN_BAR_CLOCK, PIN_BAR_DATA, false, LED_BAR_10);
  bar->begin();

  Serial.println(F("\n=== Team 7 hardware test ==="));

  Serial.println(F("1) LED BAR: segments should light 1 -> 10 one at a time, then all off."));
  for (int n = 0; n <= 10; n++) { showLevel(n); delay(150); }
  delay(400);
  showLevel(0);
  Serial.println(F("   -> Did 1..10 light in order? (if random segments light, tell Claude)"));

  delay(800);
  Serial.println(F("2) BUZZER test A: plain ON/OFF beep now..."));
  beepDigital(300);
  delay(1000);
  Serial.println(F("   BUZZER test B: tone beep now..."));
  beepTone(300);
  Serial.println(F("   -> Which one made a clear beep: A, B, or both?"));

  Serial.println(F("3) SWITCH: slide it back and forth - each change prints here."));
  Serial.println(F("   -> Tell Claude which position (HIGH or LOW) you want to mean DEPLOY."));
  Serial.println(F("   (While running, the bar shows 10 segments for HIGH, 2 for LOW.)"));
}

void loop() {
  int sw = digitalRead(PIN_SWITCH);
  if (sw != lastSwitch) {
    Serial.print(F("   SWITCH is now: "));
    Serial.println(switchName(sw));
    showLevel(sw == HIGH ? 10 : 2);
    beepDigital(60);
    lastSwitch = sw;
  }
  delay(20);
}
