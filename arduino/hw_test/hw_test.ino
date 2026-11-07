// hw_test.ino - checks the Grove modules on one box, without the radio.
// Board: Adafruit Feather RP2040 RFM. No extra libraries needed (LED bar driver is built in).
//
// Wiring (red -> 3V, black -> GND on every module):
//   LED Bar v2.1 : yellow (data) -> D10, white (clock) -> D9
//   Buzzer v1.3  : yellow -> D11
//   Switch(P)    : yellow -> D12
//   LED Button 1 : yellow/SIG1 (LED) -> D5,  white/SIG2 (button) -> D6
//   LED Button 2 : yellow/SIG1 (LED) -> D24, white/SIG2 (button) -> D25
//
// What it does:
//   - LED bar fills 1..10 once at startup, then shows the switch position
//   - both button LEDs blink while idle
//   - hold a button: its LED stays on and the buzzer sounds while held
//   - slide the switch: prints HIGH/LOW
// Open the Serial Monitor at 115200.

#define PIN_BAR_CLOCK 9
#define PIN_BAR_DATA  10
#define PIN_BUZZER    11
#define PIN_SWITCH    12
#define PIN_BTN1_LED  5      // button 1 LED (SIG1)
#define PIN_BTN1      6      // button 1 signal (SIG2): HIGH = idle, LOW = pressed
#define PIN_BTN2_LED  24     // button 2 LED (SIG1)
#define PIN_BTN2      25     // button 2 signal (SIG2)

#define BAR_BRIGHTNESS 0.3f  // 0..1. Keep it dim: the full-bright bar draws >100 mA
#define BLINK_MS       400   // idle blink speed of the button LEDs

// ---- Grove LED Bar v2 driver (MY9221 chip) -------------------------------------
// Seeed's Library Manager version doesn't compile on this board, so this is the
// same protocol written out: 16-bit command word, 12 x 16-bit channel words
// (10 LEDs + 2 unused), then a latch. Deliberately slower than the chip's limit.
void barWord(uint16_t w) {
  for (int i = 0; i < 16; i++) {
    digitalWrite(PIN_BAR_DATA, (w & 0x8000) ? HIGH : LOW);
    delayMicroseconds(1);
    digitalWrite(PIN_BAR_CLOCK, (i & 1) ? LOW : HIGH);   // data is taken on every clock edge
    delayMicroseconds(1);
    w <<= 1;
  }
}
void barShow(const uint8_t* v) {          // v[0..9] = brightness 0..255 per segment
  barWord(0x0000);                        // command: 8-bit grayscale mode
  for (int i = 0; i < 10; i++) barWord(v[i]);
  barWord(0); barWord(0);                 // the chip has 12 channels; 2 unused
  digitalWrite(PIN_BAR_DATA, LOW);        // latch: hold still > 220 us, then 4 data pulses
  delayMicroseconds(300);
  for (int i = 0; i < 4; i++) {
    digitalWrite(PIN_BAR_DATA, HIGH); delayMicroseconds(1);
    digitalWrite(PIN_BAR_DATA, LOW);  delayMicroseconds(1);
  }
  delayMicroseconds(10);
}
void barBegin() {
  pinMode(PIN_BAR_CLOCK, OUTPUT); pinMode(PIN_BAR_DATA, OUTPUT);
  digitalWrite(PIN_BAR_CLOCK, LOW); digitalWrite(PIN_BAR_DATA, LOW);
  delayMicroseconds(500);
}
void showLevel(int n) {                   // light the first n segments, dimmed
  uint8_t level = (uint8_t)(255 * BAR_BRIGHTNESS * BAR_BRIGHTNESS);
  if (level < 4) level = 4;
  uint8_t v[10];
  for (int i = 0; i < 10; i++) v[i] = (i < n) ? level : 0;
  barShow(v);
}

// ---- buzzer ----------------------------------------------------------------------
void buzzer(bool on) { digitalWrite(PIN_BUZZER, on ? HIGH : LOW); }
void beep(int ms) { buzzer(true); delay(ms); buzzer(false); }

// ---- state -----------------------------------------------------------------------
int lastSwitch = -1;
int lastB1 = HIGH, lastB2 = HIGH;

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 5000) delay(10);

  pinMode(PIN_BUZZER, OUTPUT);   buzzer(false);
  pinMode(PIN_SWITCH, INPUT);    // Grove Switch(P) drives its own HIGH/LOW
  pinMode(PIN_BTN1, INPUT);      // Grove LED Button has its own pull-up
  pinMode(PIN_BTN2, INPUT);
  pinMode(PIN_BTN1_LED, OUTPUT);
  pinMode(PIN_BTN2_LED, OUTPUT);
  barBegin();

  Serial.println(F("\n=== Team 7 hardware test ==="));
  Serial.println(F("1) LED BAR: segments should light 1 -> 10 one at a time."));
  for (int n = 0; n <= 10; n++) { showLevel(n); delay(150); }
  delay(400);
  showLevel(0);
  Serial.println(F("   -> Did 1..10 light in order?"));

  delay(500);
  Serial.println(F("2) BUZZER: short beep now..."));
  beep(200);
  Serial.println(F("   -> Did you hear it?"));

  Serial.println(F("3) BUTTONS: both LEDs should be blinking now."));
  Serial.println(F("   Hold a button: its LED stays on and the buzzer sounds while you hold it."));
  Serial.println(F("4) SWITCH: slide it - each change prints here. Bar shows 10 for HIGH, 2 for LOW."));
  Serial.println(F("   -> Tell Claude which position (HIGH or LOW) means DEPLOY."));
}

void loop() {
  // --- buttons: LOW = pressed
  int b1 = digitalRead(PIN_BTN1);
  int b2 = digitalRead(PIN_BTN2);
  if (b1 != lastB1) { Serial.println(b1 == LOW ? F("   BUTTON 1 pressed") : F("   BUTTON 1 released")); lastB1 = b1; }
  if (b2 != lastB2) { Serial.println(b2 == LOW ? F("   BUTTON 2 pressed") : F("   BUTTON 2 released")); lastB2 = b2; }

  // --- button LEDs: blink alternately while idle, solid while pressed
  bool blinkPhase = (millis() / BLINK_MS) % 2;
  digitalWrite(PIN_BTN1_LED, (b1 == LOW || blinkPhase)  ? HIGH : LOW);
  digitalWrite(PIN_BTN2_LED, (b2 == LOW || !blinkPhase) ? HIGH : LOW);

  // --- buzzer sounds while any button is held
  buzzer(b1 == LOW || b2 == LOW);

  // --- switch
  int sw = digitalRead(PIN_SWITCH);
  if (sw != lastSwitch) {
    Serial.print(F("   SWITCH is now: "));
    Serial.println(sw == HIGH ? "HIGH" : "LOW");
    showLevel(sw == HIGH ? 10 : 2);
    lastSwitch = sw;
  }
  delay(10);
}
