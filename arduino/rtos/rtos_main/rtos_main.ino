// rtos_main.ino - Team 7 box firmware, FreeRTOS version (v1.2: relay + sync).
//
// Arduino IDE, RP2040 boxes: Tools > Board > Adafruit Feather RP2040 RFM
//                            Tools > Operating System > FreeRTOS SMP      <-- required
//              M0 box:       Tools > Board > Adafruit Feather M0 (SAMD21)  ("Adafruit SAMD Boards" package)
//                            Runs the same code as a plain loop (no FreeRTOS needed) - see minirtos.h
//                            Blue button is on D1 (LED) / D0 (button) there - see pins.h
// Libraries:   "LoRa" by Sandeep Mistry, "Adafruit SHT4x"
// Screen:      ESP32-S3 Reverse TFT Feather running arduino/display_esp32, on the I2C bus (see displink.h)
//
// Tasks (each piece of hardware has exactly one owner, they talk through queues):
//
//   io    task: buttons, switch, buzzer, button LEDs, LED bar, USB serial input   --events-->  app = loop()
//   radio task: LoRa send/receive                                                  --events-->  app
//   i2c   task: SHT41 sensor + ESP32 screen                                        --events-->  app
//   app (loop): decides everything; sends commands back to the io / radio / i2c tasks
//
// Behaviour:
//   RED button  (button 1, D5/D6)   -> HELP to everyone        (its LED blinks until someone confirms)
//   BLUE button (button 2, D24/D25) -> OK to everyone          (its LED blinks until someone confirms)
//   Confirmed -> that LED stays on 3 s + short beep, screen shows who confirmed (tick).
//   No confirmation after 5 s -> the box sends it once more. Still none at 10 s -> LED blinks fast and the
//   screen shows "no reply" (no beep).
//   Confirmations: a box confirms each text / quick message it receives, 0.7-1.5 s later (after the relay
//   copies, so they don't collide). For "everyone" messages only the first box answers - the others hear
//   that and stay quiet. A message to one box is always confirmed by that box.
//   Every 30 s each box sends a silent ping (with its role and temperature) - no beep, no message.
//   LED bar = signal strength of the strongest box heard (10 = strong). Nobody heard -> only the first (red) segment.
//   CARRY mode: when the link to the deployed boxes / base drops to ~30 % -> rapid beeping and
//               "DEPLOY A RELAY" on the screen, until the signal recovers (40 %) or you switch to DEPLOY.
//
//   SAFETY: every packet carries a tag made with the team key (key.h). Packets with a wrong tag (other
//          radios, a box with a different key, a faked HELP) are ignored. A watchdog reboots a box
//          whose radio / buttons / screen / app part stops running for 8 s.
//   RELAY: every box repeats every packet it hears for the first time (up to MAX_HOPS times along
//          the chain), so a box that can't hear the base still reaches it through the boxes in between.
//          Confirmations travel back the same way.
//   SYNC:  every box keeps the last STORE_N texts / quick messages. When a box shows up (walks back
//          into range, is switched on, or reboots) its neighbours re-send what it missed - e.g. a HELP
//          pressed out of range gets delivered once the carrier walks back into range.
//
// BASE BOX: set IS_BASE 1 and plug it into the Pi/laptop running pi/base_station.py.
//   Commands from the app:  TX,<dst>,<text>   QM,<dst>,<code>   AK,<dst>,<seq>   (dst = box id or *)
//   Debug command:          STACKS   (prints spare stack per task + free heap)

#if defined(ARDUINO_ARCH_SAMD) || defined(NO_RTOS)
#define USE_RTOS 0                 // Feather M0: tasks run one after another in loop()
#include "minirtos.h"
#else
#define USE_RTOS 1                 // RP2040: real FreeRTOS tasks
#include <FreeRTOS.h>              // "No such file": select Tools > Operating System > FreeRTOS SMP
#include <task.h>
#include <queue.h>
#include <semphr.h>
#endif
#include <SPI.h>
#include <Wire.h>
#include <LoRa.h>
#include <Adafruit_SHT4x.h>
#include "pins.h"
#include "ledbar.h"
#include "proto.h"
#include "displink.h"
#if __has_include("key.h")
#include "key.h"                   // the team key - secret, not in git
#else
#error "key.h is missing: copy key.example.h to key.h and put your team's key in it (same key on every box)"
#endif
#if defined(ARDUINO_ARCH_RP2040)
#include <hardware/watchdog.h>
#endif

// ================================================================== config
#ifndef NODE_ID
#define NODE_ID          "CHARLIE"   // different on each box, max 8 chars, no commas
#endif
#ifndef IS_BASE
#define IS_BASE          0         // 1 on the box plugged into the Pi
#endif
#define LORA_FREQ_HZ     915E6     // radio settings must match on every box
#define LORA_SF          7
#define LORA_BW_HZ       125E3
#define LORA_CR_DENOM    5
#define LORA_TX_DBM      14
#define TX_TIMEOUT_MS    3000
#define SENSOR_EVERY_MS  5000
#ifndef PING_EVERY_MS
#define PING_EVERY_MS 30000     // silent ping (also carries role + temperature)
#endif
#ifndef LINK_LOST_MS
#define LINK_LOST_MS 75000     // a box is "gone" after ~2.5 missed pings
#endif
#define WEAK_PCT         30        // CARRY alarm starts at or below this signal %
#define WEAK_CLEAR_PCT   40        // ... and stops at or above this
#ifndef ACK_WAIT_MS
#define ACK_WAIT_MS 10000     // how long to wait for a confirmation (then "no reply")
#endif
#define BTN_HELP         1         // red button  = button 1 (D5 LED / D6 button)
#define BTN_OK           2         // blue button = button 2 (D24 LED / D25 button)
#define DEPLOY_LEVEL     HIGH      // switch level that means DEPLOY (flip if yours is reversed)
#define BAR_RED_FIRST    1         // 1 if segment 1 of the LED bar is the red one; 0 if the bar is mounted reversed

// ---- relay + sync (RAM use is fixed: ~0.9 KB for all of this; shrink STORE_N first on a small MCU)
#define RELAY            1         // 0 = this box never repeats other boxes' packets
#define MAX_HOPS         3         // how many times a packet may be repeated (3 = base + 3 relays + far box)
#define RELAY_MIN_MS     100       // random wait before repeating (so two relays don't talk at once)
#define RELAY_MAX_MS     600
#define ACK_MIN_MS       700       // confirmations wait 0.7-1.5 s: after the relay copies (0.1-0.6 s), so they don't collide
#define ACK_MAX_MS       1500
#define RETRY_AFTER_MS   5000      // no confirmation after this -> send our message once more
#define RETRY_FLAG       8         // added to ttl on a retry, so boxes that already have it confirm again
#define WAVE_MS          2500      // copies of one transmission arrive within this; later = a new wave
#define STORE_N          6         // remembered messages for catch-up sync
#define SEEN_N           24        // remembered (sender, number) pairs to drop duplicates
#define SYNC_MAX_AGE_MS  600000UL  // only re-send messages younger than 10 minutes
#define BOOT_PINGS       3         // the first pings after power-on ask the neighbours for a sync
#define NBR_IN_PING      4         // how many neighbours each ping reports (for the base map)

// ---- safety
#define AUTH_CHECK       1         // 1 = ignore packets whose tag doesn't match our key. 0 = only log them (debugging)
#define WATCHDOG_MS      8000      // reboot if a part of the firmware is stuck this long
#define ALIVE_MS         4000      // ... i.e. a part hasn't checked in for this long

// ---- phone (through the ESP32 screen board's Wi-Fi page)
#define PHONE_POLL_MS    250       // how often we ask the screen board if the phone sent something
#define PHONE_READ       80        // bytes per poll - must match PHONE_READ in display_esp32.ino

#ifndef PIN_RFM_CS
#define PIN_RFM_CS   16
#define PIN_RFM_RST  17
#define PIN_RFM_DIO0 21
#endif

// ================================================================== messages between tasks
enum EvType : uint8_t { EV_BUTTON, EV_SWITCH, EV_RADIO_RX, EV_SENSOR, EV_SERIAL, EV_PHONE };
struct Event {
  EvType type;
  uint8_t a;            // button number / switch level / 1 = real sensor
  int16_t rssi;
  float snr, temp, rh;
  uint8_t len;
  uint8_t data[proto::MAX_PKT];   // received packet (with header) or a serial command line
};

enum UiType : uint8_t { UI_BEEP, UI_BAR, UI_LED, UI_ALARM };
enum LedMode : uint8_t { LED_OFF, LED_ON, LED_BLINK, LED_FAST };
struct UiCmd { UiType type; uint8_t a, b; uint16_t ms; };
// BEEP: a = count, ms = length | BAR: a = level 0..10 | LED: a = 1/2, b = mode, ms = duration (0 = stays)
// ALARM: a = 1 on / 0 off (rapid beep bursts)

struct TxPacket { uint8_t len; uint16_t delayMs; uint8_t data[proto::MAX_PKT]; };

// Types used in function arguments must be defined up here: the Arduino IDE puts its automatic
// function prototypes above the first function, and they need to know these types.
struct Seen { uint16_t src, seq; uint32_t at; };   // (sender, number) of a packet already handled, and when
enum Part : uint8_t { P_IO, P_RADIO, P_I2C, P_APP, P_COUNT };   // parts that check in with the watchdog
struct Stored {                                 // a message kept for catch-up sync
  bool used; char type[4]; char src[proto::ID_MAX + 1]; uint16_t seq;
  uint32_t t;                                  // when we got it
  uint32_t overheard;                          // last time another box sent it (skip it in a sync after that)
  char rest[proto::ID_MAX + 2 + proto::MAX_TEXT];   // "dst,text"
};

QueueHandle_t qEvents, qUi, qTx, qDisp;
SemaphoreHandle_t logMutex;
TaskHandle_t hIo, hRadio, hI2c;

// ================================================================== output (any task)
// logf(): human-readable lines, prefixed with the time.  protoLine(): exact lines for base_station.py.
void logf(const char* fmt, ...) {
  char buf[160];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  xSemaphoreTake(logMutex, portMAX_DELAY);
  Serial.print('['); Serial.print(millis()); Serial.print("] "); Serial.println(buf);
  xSemaphoreGive(logMutex);
}
void protoLine(const char* fmt, ...) {
  char buf[160];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  xSemaphoreTake(logMutex, portMAX_DELAY);
  Serial.println(buf);
  xSemaphoreGive(logMutex);
}

void ui(UiType t, uint8_t a, uint8_t b = 0, uint16_t ms = 0) {
  UiCmd c = {t, a, b, ms};
  xQueueSend(qUi, &c, 0);
}

void show(DispType t, const char* from, const char* text, int16_t rssi = 0, uint16_t seq = 0) {
  static DispMsg m;                  // app task only: keep the struct off the stack
  memset(&m, 0, sizeof m);
  m.type = t;
  strncpy(m.from, from, 8);
  strncpy(m.text, text, sizeof m.text - 1);
  m.rssi = rssi; m.seq = seq;
  xQueueSend(qDisp, &m, 0);
}

void fmt2(char* out, int max, float v) {           // "23.46" without printf float support
  long c = lroundf(v * 100.0f);
  const char* sign = c < 0 ? "-" : "";
  if (c < 0) c = -c;
  snprintf(out, max, "%s%ld.%02ld", sign, c / 100, c % 100);
}

#if defined(ARDUINO_ARCH_RP2040)
inline uint32_t rnd() { return rp2040.hwrand32(); }     // hardware random generator
#else
inline uint32_t rnd() { return ((uint32_t)random(0x7FFFFFFF) << 1) ^ micros(); }   // seeded in setup()
#endif
#if defined(ARDUINO_ARCH_SAMD)
extern "C" char* sbrk(int incr);
#endif
int freeRam() {
#if defined(ARDUINO_ARCH_RP2040)
  return rp2040.getFreeHeap();
#elif defined(ARDUINO_ARCH_SAMD)
  char top;
  return &top - sbrk(0);                                // gap between heap and stack
#else
  return -1;
#endif
}

int signalPct(float rssi) {                        // -120 dBm = 0 %, -60 dBm or better = 100 %
  int p = (int)((rssi + 120.0f) * 100.0f / 60.0f);
  return p < 0 ? 0 : p > 100 ? 100 : p;
}

// ================================================================== watchdog
// Each part checks in every time it runs. housekeeping() feeds the watchdog only while ALL of them are
// alive - a timer alone would keep feeding it even if the radio part were stuck.
const char* const partName[P_COUNT] = {"io (buttons/LEDs)", "radio", "i2c (sensor/screen)", "app"};
volatile uint32_t aliveAt[P_COUNT];
inline void alive(Part p) { aliveAt[p] = millis(); }

void wdtStart() {
#if defined(ARDUINO_ARCH_RP2040)
  rp2040.wdt_begin(WATCHDOG_MS);
#elif defined(ARDUINO_ARCH_SAMD)
  // SAMD21: watchdog clock = GCLK2 = 32 kHz ULP oscillator / 32 = 1.024 kHz, 8192 cycles = 8 s
  GCLK->GENDIV.reg = GCLK_GENDIV_ID(2) | GCLK_GENDIV_DIV(4);
  GCLK->GENCTRL.reg = GCLK_GENCTRL_ID(2) | GCLK_GENCTRL_GENEN | GCLK_GENCTRL_SRC_OSCULP32K | GCLK_GENCTRL_DIVSEL;
  while (GCLK->STATUS.bit.SYNCBUSY) {}
  GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID_WDT | GCLK_CLKCTRL_CLKEN | GCLK_CLKCTRL_GEN_GCLK2;
  WDT->CTRL.reg = 0;
  while (WDT->STATUS.bit.SYNCBUSY) {}
  WDT->CONFIG.reg = WDT_CONFIG_PER_8K;
  WDT->CTRL.reg = WDT_CTRL_ENABLE;
  while (WDT->STATUS.bit.SYNCBUSY) {}
#endif
}
void wdtStop() {
#if defined(ARDUINO_ARCH_SAMD)
  WDT->CTRL.reg = 0;
  while (WDT->STATUS.bit.SYNCBUSY) {}
#endif
}
void wdtFeed() {
#if defined(ARDUINO_ARCH_RP2040)
  rp2040.wdt_reset();
#elif defined(ARDUINO_ARCH_SAMD)
  if (!WDT->STATUS.bit.SYNCBUSY) WDT->CLEAR.reg = WDT_CLEAR_CLEAR_KEY;
#endif
}
bool wdtCausedReboot() {
#if defined(ARDUINO_ARCH_RP2040)
  return watchdog_enable_caused_reboot();
#elif defined(ARDUINO_ARCH_SAMD)
  return (PM->RCAUSE.reg & PM_RCAUSE_WDT) != 0;
#else
  return false;
#endif
}
void watchdogCheck() {                          // called from housekeeping()
  static int stuck = -1;
  uint32_t now = millis();
  for (int i = 0; i < P_COUNT; i++) {
    if (now - aliveAt[i] > ALIVE_MS) {
      if (stuck != i) { stuck = i; logf("!! %s part stuck for %lu ms - watchdog will reboot the box", partName[i], (unsigned long)(now - aliveAt[i])); }
      return;                                   // don't feed: the watchdog reboots us
    }
  }
  stuck = -1;
  wdtFeed();
}

// ================================================================== io task
// Buttons + switch in, buzzer + LEDs + LED bar out, USB serial command lines in. Steps every 10 ms.
namespace io {
  const int btnPin[2] = {PIN_BTN1, PIN_BTN2}, ledPin[2] = {PIN_BTN1_LED, PIN_BTN2_LED};
  int stable[2] = {HIGH, HIGH}, cnt[2] = {0, 0};
  int swStable, swCnt = 0;
  LedMode mode[2] = {LED_OFF, LED_OFF}, baseMode[2] = {LED_OFF, LED_OFF};
  TickType_t modeUntil[2] = {0, 0};
  int barNow = -1;
  int beepsLeft = 0; uint16_t beepMs = 80; bool beepOn = false; TickType_t beepNext = 0;
  bool alarmOn = false; TickType_t alarmNext = 0;
  char line[proto::MAX_PKT]; int lineLen = 0;
  Event ev;                                         // serial lines are big: keep off the stack
}

void ioInit() {
  using namespace io;
  pinMode(PIN_BTN1, INPUT);            // Grove LED Button has its own pull-up: LOW = pressed
  pinMode(PIN_BTN2, INPUT);
  pinMode(PIN_SWITCH, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);   digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_BTN1_LED, OUTPUT); pinMode(PIN_BTN2_LED, OUTPUT);
  barBegin();
  swStable = digitalRead(PIN_SWITCH);
  Event e = {}; e.type = EV_SWITCH; e.a = swStable; xQueueSend(qEvents, &e, portMAX_DELAY);
}

void ioStep() {
  using namespace io;
  alive(P_IO);
  TickType_t now = xTaskGetTickCount();
  // ---- inputs: buttons (debounced, event on press) and switch
  for (int i = 0; i < 2; i++) {
    int raw = digitalRead(btnPin[i]);
    if (raw != stable[i]) {
      if (++cnt[i] >= 3) {                        // 30 ms of the new value = real change
        stable[i] = raw; cnt[i] = 0;
        if (raw == LOW) { Event e = {}; e.type = EV_BUTTON; e.a = i + 1; xQueueSend(qEvents, &e, 0); }
      }
    } else cnt[i] = 0;
  }
  int sw = digitalRead(PIN_SWITCH);
  if (sw != swStable) {
    if (++swCnt >= 5) { swStable = sw; swCnt = 0; Event e = {}; e.type = EV_SWITCH; e.a = sw; xQueueSend(qEvents, &e, 0); }
  } else swCnt = 0;

  // ---- USB serial command lines (from base_station.py or the Serial Monitor)
  while (Serial.available()) {
    int c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineLen) {
        memset(&ev, 0, sizeof ev);
        ev.type = EV_SERIAL; ev.len = lineLen; memcpy(ev.data, line, lineLen); ev.data[lineLen] = 0;
        xQueueSend(qEvents, &ev, 0);
        lineLen = 0;
      }
    } else if (lineLen < (int)sizeof line - 1) line[lineLen++] = (char)c;
  }

  // ---- commands from the app
  UiCmd c;
  while (xQueueReceive(qUi, &c, 0) == pdTRUE) {
    if (c.type == UI_BEEP) { beepsLeft = c.a; beepMs = c.ms ? c.ms : 80; beepOn = false; beepNext = now; }
    else if (c.type == UI_ALARM) { alarmOn = c.a; alarmNext = now; }
    else if (c.type == UI_BAR && c.a != barNow) {
      int lvl = c.a > 10 ? 10 : c.a;
      uint8_t v[10];
      for (int i = 0; i < 10; i++) v[BAR_RED_FIRST ? i : 9 - i] = (i < lvl) ? 60 : 0;
      barShow(v);
      barNow = c.a;
    } else if (c.type == UI_LED && c.a >= 1 && c.a <= 2) {
      int i = c.a - 1;
      mode[i] = (LedMode)c.b;
      if (c.ms) modeUntil[i] = now + pdMS_TO_TICKS(c.ms);
      else { baseMode[i] = (LedMode)c.b; modeUntil[i] = 0; }
    }
  }

  // ---- outputs
  for (int i = 0; i < 2; i++) {
    if (modeUntil[i] && now >= modeUntil[i]) { modeUntil[i] = 0; mode[i] = baseMode[i]; }
    bool on = mode[i] == LED_ON ||
              (mode[i] == LED_BLINK && (now / pdMS_TO_TICKS(300)) % 2) ||
              (mode[i] == LED_FAST  && (now / pdMS_TO_TICKS(100)) % 2);
    digitalWrite(ledPin[i], on ? HIGH : LOW);
  }
  if (alarmOn && beepsLeft == 0 && now >= alarmNext) {         // rapid burst: 5 beeps every 1.5 s
    beepsLeft = 5; beepMs = 60; beepOn = false; beepNext = now;
    alarmNext = now + pdMS_TO_TICKS(1500);
  }
  if (beepsLeft > 0 && now >= beepNext) {
    beepOn = !beepOn;
    digitalWrite(PIN_BUZZER, beepOn ? HIGH : LOW);
    if (!beepOn) beepsLeft--;
    beepNext = now + pdMS_TO_TICKS(beepMs);
  }
}

#if !USE_RTOS
void idleWork() {                                   // M0: keep buttons / buzzer / LEDs alive while waiting
#if defined(ARDUINO_ARCH_SAMD)
  // The Arduino IDE starts an upload by opening the port at 1200 baud; the board then resets into its
  // bootloader 250 ms later. Turn the watchdog off first, so it can't reboot the board in the middle of the upload.
  static bool uploading = false;
  if (!uploading && Serial.baud() == 1200) { uploading = true; wdtStop(); }
#endif
  static uint32_t last = 0;
  if (millis() - last >= 10) { last = millis(); ioStep(); }
}
#endif

// ================================================================== I2C task (SHT41 + screen)
namespace i2c {
  Adafruit_SHT4x sht4;
  bool have = false;
  float simT = 21.0f, simH = 45.0f;
  int screen = -1;                                  // -1 unknown, 0 not answering, 1 ok
  TickType_t nextSensor = 0;
  DispMsg m;
  char line[128];
  TickType_t nextPoll = 0;
  int lastPhoneId = -1;
  Event pe;
}

// Ask the screen board for a line typed on the phone: it answers [id][len][text][padding].
// We confirm with 0x06,<id>; an id we already handled is only confirmed again, never used twice.
void phonePoll() {
  using namespace i2c;
  uint8_t b[PHONE_READ];
  int n = Wire.requestFrom((uint8_t)DISPLAY_I2C_ADDR, (uint8_t)PHONE_READ);
  int k = 0;
  while (Wire.available() && k < PHONE_READ) b[k++] = (uint8_t)Wire.read();
  while (Wire.available()) Wire.read();
  if (n < 2 || k < 2) return;
  int id = b[0], len = b[1];
  if (id == 0 || id == 255 || len == 0 || len > k - 2 || len >= (int)sizeof pe.data) return;   // nothing / old screen code
  if (id != lastPhoneId) {
    memset(&pe, 0, sizeof pe);
    pe.type = EV_PHONE; pe.len = len;
    memcpy(pe.data, b + 2, len); pe.data[len] = 0;
    for (int i = 0; i < len; i++) if (pe.data[i] < 32 || pe.data[i] > 126) return;   // garbled read: try again
    if (xQueueSend(qEvents, &pe, 0) != pdTRUE) return;        // app busy: leave it, ask again next time
    lastPhoneId = id;
  }
  Wire.beginTransmission(DISPLAY_I2C_ADDR);
  Wire.write((uint8_t)0x06); Wire.write((uint8_t)id);
  Wire.endTransmission();
}

void i2cInit() {
  using namespace i2c;
  Wire.begin();
  Wire.setClock(100000);
  have = sht4.begin(&Wire);
  if (have) { sht4.setPrecision(SHT4X_HIGH_PRECISION); sht4.setHeater(SHT4X_NO_HEATER); }
  logf("SHT41 %s", have ? "found" : "not found - sending simulated values");
  nextSensor = xTaskGetTickCount();
}

void i2cStep() {
  using namespace i2c;
  alive(P_I2C);
  while (xQueueReceive(qDisp, &m, 0) == pdTRUE) {
    displinkFormat(m, NODE_ID, line, sizeof line);
    int ok = displinkSend(line) ? 1 : 0;
    if (ok != screen) {
      logf(ok ? "screen connected (I2C 0x42)" : "screen not answering on I2C 0x42 - check SDA/SCL/GND and that the ESP32 is running");
      screen = ok;
    }
  }
  TickType_t now = xTaskGetTickCount();
  if (screen == 1 && now >= nextPoll) { nextPoll = now + pdMS_TO_TICKS(PHONE_POLL_MS); phonePoll(); }
  if (now >= nextSensor) {
    nextSensor = now + pdMS_TO_TICKS(SENSOR_EVERY_MS);
    Event e = {}; e.type = EV_SENSOR;
    sensors_event_t hum, temp;
    if (have && sht4.getEvent(&hum, &temp)) {
      e.a = 1; e.temp = temp.temperature; e.rh = hum.relative_humidity;
    } else {
      simT += (int)(rnd() % 41 - 20) / 100.0f;
      simH += (int)(rnd() % 101 - 50) / 100.0f;
      e.a = 0; e.temp = simT; e.rh = simH;
    }
    xQueueSend(qEvents, &e, 0);
  }
}

// ================================================================== radio task
uint8_t rfmRead(uint8_t reg) {
  SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_RFM_CS, LOW);
  SPI.transfer(reg & 0x7F);
  uint8_t v = SPI.transfer(0x00);
  digitalWrite(PIN_RFM_CS, HIGH);
  SPI.endTransaction();
  return v;
}
void rfmWrite(uint8_t reg, uint8_t v) {
  SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_RFM_CS, LOW);
  SPI.transfer(reg | 0x80);
  SPI.transfer(v);
  digitalWrite(PIN_RFM_CS, HIGH);
  SPI.endTransaction();
}
bool radioStart() {
  LoRa.setPins(PIN_RFM_CS, PIN_RFM_RST, PIN_RFM_DIO0);
  if (!LoRa.begin(LORA_FREQ_HZ)) return false;
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW_HZ);
  LoRa.setCodingRate4(LORA_CR_DENOM);
  LoRa.setTxPower(LORA_TX_DBM);
  LoRa.enableCrc();
  return true;
}
bool radioSend(const uint8_t* buf, int len) {
  const uint8_t REG_IRQ_FLAGS = 0x12, IRQ_TX_DONE = 0x08;
  LoRa.beginPacket();
  LoRa.write(buf, len);
  LoRa.endPacket(true);
  TickType_t t0 = xTaskGetTickCount();
  while ((rfmRead(REG_IRQ_FLAGS) & IRQ_TX_DONE) == 0) {
    if (xTaskGetTickCount() - t0 > pdMS_TO_TICKS(TX_TIMEOUT_MS)) {
      logf("!! TX timeout - restarting radio (check antenna / power)");
      LoRa.end();
      radioStart();
      return false;
    }
    vTaskDelay(1);
  }
  rfmWrite(REG_IRQ_FLAGS, IRQ_TX_DONE);
  return true;
}

bool radioOk = false;
struct Parked { bool used; TickType_t due; TxPacket p; };
Parked parked[4];

bool radioInit() {
  if (!radioStart()) {
    logf("!! LoRa init FAILED - check the board selection");
    ui(UI_LED, 1, LED_FAST); ui(UI_LED, 2, LED_FAST);
    return false;
  }
  logf("radio ready: %ld MHz SF%d %d dBm", (long)(LORA_FREQ_HZ / 1e6), LORA_SF, LORA_TX_DBM);
  radioOk = true;
  return true;
}

void radioStep() {
  alive(P_RADIO);
  if (!radioOk) {                                         // radio didn't start: try again every 5 s
    static uint32_t lastTry = 0;
    if (millis() - lastTry > 5000) { lastTry = millis(); radioInit(); }
    return;
  }
  static TxPacket p;
  static Event e;
  for (auto& s : parked) {                                // park packets until their send time
    if (s.used) continue;                                 // (only take one when there is room for it)
    if (xQueueReceive(qTx, &p, 0) != pdTRUE) break;
    s.used = true; s.due = xTaskGetTickCount() + pdMS_TO_TICKS(p.delayMs); s.p = p;
  }
  for (auto& s : parked) {
    if (s.used && xTaskGetTickCount() >= s.due) {
      if (!radioSend(s.p.data, s.p.len)) logf("TX FAILED (%d bytes)", s.p.len);
      s.used = false;
    }
  }
  int size = LoRa.parsePacket();
  if (size > 0) {
    memset(&e, 0, sizeof e);
    e.type = EV_RADIO_RX;
    while (LoRa.available()) {
      int b = LoRa.read();
      if (e.len < sizeof e.data - 1) e.data[e.len++] = (uint8_t)b;
    }
    e.rssi = LoRa.packetRssi();
    e.snr = LoRa.packetSnr();
    xQueueSend(qEvents, &e, 0);
  }
}

#if USE_RTOS
// RP2040: each part runs in its own FreeRTOS task
void ioTask(void*)    { ioInit(); for (;;) { ioStep(); vTaskDelay(pdMS_TO_TICKS(10)); } }
void i2cTask(void*)   { i2cInit(); for (;;) { i2cStep(); vTaskDelay(pdMS_TO_TICKS(20)); } }
void radioTask(void*) { radioInit(); for (;;) { radioStep(); vTaskDelay(pdMS_TO_TICKS(2)); } }
#endif

// ================================================================== app state
uint16_t seqNo;
bool deployed = false;
float lastTemp = 0, lastRh = 0; bool lastReal = false; int sensorCount = 0;

struct Nbr { bool used; char id[9]; char role; float rssi; uint32_t last; };
Nbr nbrs[8];                                   // boxes heard directly

struct Pend { bool used, retried; uint16_t seq; uint8_t btn; char what[12]; uint32_t sent; uint8_t acks; };
Pend pend[4];                                  // our messages waiting for confirmation

bool warned = false, everLinked = false;
uint32_t nextPing = 0;
int barShown = -1, bootPings = 0;

const char* myRole() { return IS_BASE ? "B" : (deployed ? "D" : "C"); }
const char* modeName() { return IS_BASE ? "BASE" : (deployed ? "DEPLOY" : "CARRY"); }

// ---- duplicate filter: (sender, number) pairs seen recently. Two lists so frequent pings and
//      confirmations can't push messages out (messages must be remembered as long as they can be re-synced).
Seen seenMsg[SEEN_N], seenShort[SEEN_N];
uint8_t seenMsgPos = 0, seenShortPos = 0;

uint16_t idHash(const char* s) {               // 16-bit FNV-1a of the box name
  uint32_t h = 2166136261u;
  while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
  return (uint16_t)(h ^ (h >> 16)) | 1;        // never 0, so 0 = empty slot
}
Seen* seenFind(Seen* list, const char* src, unsigned seq) {
  uint16_t h = idHash(src);
  for (int i = 0; i < SEEN_N; i++) if (list[i].src == h && list[i].seq == (uint16_t)seq) return &list[i];
  return nullptr;
}
// true if already seen; otherwise remembers it and returns false
bool seenBefore(Seen* list, uint8_t& pos, const char* src, unsigned seq) {
  if (seenFind(list, src, seq)) return true;
  list[pos].src = idHash(src); list[pos].seq = (uint16_t)seq; list[pos].at = millis();
  pos = (pos + 1) % SEEN_N;
  return false;
}

// ---- confirmations we still have to send. For "everyone" messages only one box needs to answer:
//      if we hear another box confirm the same message first, we drop ours.
struct AckJob { bool used, toAll; char src[proto::ID_MAX + 1]; uint16_t seq; uint32_t due; };
AckJob ackJobs[4];
void scheduleAck(const char* src, unsigned seq, bool toAll) {
  AckJob* j = &ackJobs[0];
  for (auto& a : ackJobs) {
    if (a.used && a.seq == (uint16_t)seq && !strcmp(a.src, src)) return;   // already planned
    if (!a.used) { j = &a; break; }
    if (a.due < j->due) j = &a;
  }
  memset(j, 0, sizeof *j);
  j->used = true; j->toAll = toAll; j->seq = (uint16_t)seq;
  strncpy(j->src, src, proto::ID_MAX);
  j->due = millis() + ACK_MIN_MS + rnd() % (ACK_MAX_MS - ACK_MIN_MS);
}
void ackHeard(const char* src, unsigned seq) {        // another box confirmed (src, seq)
  for (auto& a : ackJobs)
    if (a.used && a.toAll && a.seq == (uint16_t)seq && !strcmp(a.src, src)) a.used = false;
}

// ---- message store for catch-up sync (oldest is overwritten first)
Stored store[STORE_N];
uint8_t storeHead = 0;                         // next slot to write = the oldest
uint32_t syncAt = 0, syncAsked = 0;            // syncAt != 0: a sync round is running / due
uint8_t syncIdx = 0;

void storeAdd(const char* type, const char* src, unsigned seq, const char* rest) {
  Stored& s = store[storeHead];
  memset(&s, 0, sizeof s);
  s.used = true;
  strncpy(s.type, type, 3); strncpy(s.src, src, proto::ID_MAX); s.seq = (uint16_t)seq;
  strncpy(s.rest, rest, sizeof s.rest - 1);
  s.t = millis();
  storeHead = (storeHead + 1) % STORE_N;
}
Stored* storeFind(const char* src, unsigned seq) {
  for (auto& s : store) if (s.used && s.seq == (uint16_t)seq && !strcmp(s.src, src)) return &s;
  return nullptr;
}
uint32_t lastAsk = 0;
void askSync(const char* why) {                // a neighbour appeared: re-send what it may have missed
  if (lastAsk && millis() - lastAsk < 15000) return;     // one round is enough (boot pings repeat the ask)
  bool any = false;
  for (auto& s : store) if (s.used && millis() - s.t < SYNC_MAX_AGE_MS) any = true;
  if (!any) return;
  lastAsk = syncAsked = millis();
  syncIdx = 0;
  if (!syncAt) syncAt = millis() + 500 + rnd() % 2500;    // random: several neighbours may answer
  logf("sync: %s - will re-send recent messages", why);
}

// ---- sending. Every packet starts with MAX_HOPS repeats allowed.
const uint8_t meshKey[16] = MESH_KEY;

bool sendRaw(const char* type, const char* src, unsigned seq, unsigned ttl, const char* rest, uint16_t delayMs) {
  static TxPacket p;
  memset(&p, 0, sizeof p);
  char tag[proto::TAG_HEX + 1];
  proto::sign(meshKey, type, src, seq, rest, tag);           // same tag whoever sends it (we all have the key)
  p.len = proto::build(p.data, sizeof p.data, type, src, seq, ttl, NODE_ID, tag, rest);
  p.delayMs = delayMs;
  if (!p.len) return false;
  if (xQueueSend(qTx, &p, 0) != pdTRUE) { logf("TX queue full - dropped %s from %s", type, src); return false; }
  return true;
}
unsigned sendPkt(const char* type, const char* rest, uint16_t delayMs = 0) {
  unsigned seq = seqNo++;
  sendRaw(type, NODE_ID, seq, MAX_HOPS, rest, delayMs);
  return seq;
}

void pushStatus() {
  static DispMsg m;
  memset(&m, 0, sizeof m);
  m.type = D_STATUS;
  strncpy(m.text, modeName(), 7);
  m.temp = lastTemp; m.rh = lastRh;
  m.flag = sensorCount ? lastReal : 2;
  xQueueSend(qDisp, &m, 0);
}

// A packet arrived directly from box `id`. Returns true if `id` is new or was gone (-> sync).
bool heard(const char* id, char role, int rssi) {
  int slot = -1, oldest = 0;
  for (int i = 0; i < 8; i++) {
    if (nbrs[i].used && !strcmp(nbrs[i].id, id)) { slot = i; break; }
    if (!nbrs[i].used && slot < 0) slot = i;
    if (nbrs[i].last < nbrs[oldest].last) oldest = i;
  }
  bool fresh = false;
  if (slot < 0) slot = oldest;
  Nbr& n = nbrs[slot];
  if (!n.used || strcmp(n.id, id)) { memset(&n, 0, sizeof n); strncpy(n.id, id, 8); n.rssi = rssi; fresh = true; }
  bool back = !fresh && millis() - n.last > LINK_LOST_MS;
  if (back) n.rssi = rssi;                                 // old value is meaningless after a gap
  n.used = true;
  n.rssi = (fresh || back) ? rssi : 0.6f * n.rssi + 0.4f * rssi;   // smooth out single-packet noise
  if (role) n.role = role;
  n.last = millis();
  show(D_NODE, id, "", (int16_t)n.rssi);
  return fresh || back;
}

// Strongest box heard recently. chainOnly: only deployed boxes / base (what a carrier needs).
int bestPct(bool chainOnly) {
  int best = -1;
  for (auto& n : nbrs) {
    if (!n.used || millis() - n.last > LINK_LOST_MS) continue;
    if (chainOnly && n.role != 'D' && n.role != 'B') continue;
    int p = signalPct(n.rssi);
    if (p > best) best = p;
  }
  return best;                                  // -1 = nobody
}

// "ID:rssi;ID:rssi" of the boxes heard directly (for the base map). Stops before `max` chars.
void nbrList(char* out, int max) {
  int n = 0, k = 0;
  out[0] = 0;
  for (auto& b : nbrs) {
    if (!b.used || millis() - b.last > LINK_LOST_MS || k >= NBR_IN_PING) continue;
    char item[16];
    int len = snprintf(item, sizeof item, "%s%s:%d", k ? ";" : "", b.id, (int)lroundf(b.rssi));
    if (n + len >= max) break;
    memcpy(out + n, item, len + 1);
    n += len; k++;
  }
}

// ---- sending with confirmation tracking
unsigned sendTracked(const char* type, const char* dst, const char* body, const char* what, uint8_t btn) {
  char rest[proto::MAX_PKT];
  snprintf(rest, sizeof rest, "%s,%s", dst, body);
  unsigned seq = sendPkt(type, rest);
  storeAdd(type, NODE_ID, seq, rest);                      // so it can be re-sent to boxes that missed it
  int slot = 0;
  for (int i = 0; i < 4; i++) if (!pend[i].used) { slot = i; break; } else if (pend[i].sent < pend[slot].sent) slot = i;
  pend[slot].used = true; pend[slot].retried = false; pend[slot].seq = seq; pend[slot].btn = btn; pend[slot].acks = 0;
  strncpy(pend[slot].what, what, 11); pend[slot].what[11] = 0;
  pend[slot].sent = millis();
  show(D_SENT, "YOU", what, 0, seq);
  if (btn) ui(UI_LED, btn, LED_BLINK, ACK_WAIT_MS);          // blinks until confirmed or timeout
  return seq;
}

void quick(const char* code, uint8_t btn) {
  unsigned seq = sendTracked("QM1", "*", code, code, btn);
  logf("button %d -> %s (#%u)", btn, code, seq);
  ui(UI_BEEP, 1, 0, 40);
}

void gotAck(const char* by, unsigned seq, bool byOperator, int rssi) {
  for (auto& p : pend) {
    if (!p.used || p.seq != seq) continue;
    if (byOperator) {                                          // the base operator acknowledged
      char t[48]; snprintf(t, sizeof t, "%s acknowledged - help is coming", p.what);
      show(D_MSG, by, t, rssi);
      ui(UI_BEEP, 2, 0, 120);
      if (p.btn) ui(UI_LED, p.btn, LED_ON, 5000);
      logf("#%u: operator at %s acknowledged", seq, by);
      return;
    }
    p.acks++;
    show(D_ACK, by, "", 0, seq);
    if (p.acks == 1) {                                         // first confirmation
      if (p.btn) ui(UI_LED, p.btn, LED_ON, 3000);
      ui(UI_BEEP, 1, 0, 30);
    }
    logf("#%u confirmed by %s", seq, by);
    return;
  }
  Stored* s = storeFind(NODE_ID, seq);                         // late: delivered by a sync after the timeout
  if (!s) return;
  const char* what = strchr(s->rest, ',');
  what = what ? what + 1 : s->rest;
  if (byOperator) {
    char t[48]; snprintf(t, sizeof t, "%.11s acknowledged - help is coming", what);
    show(D_MSG, by, t, rssi);
    ui(UI_BEEP, 2, 0, 120);
    logf("#%u delivered later: operator at %s acknowledged", seq, by);
  } else {
    show(D_ACK, by, "", 0, seq);
    ui(UI_BEEP, 1, 0, 30);
    logf("#%u delivered later, confirmed by %s", seq, by);
  }
}

// ---- commands from the Pi / laptop (base box), plus STACKS for debugging.
//      The phone page sends the same TX / QM lines (fromPhone = true).
void handleCommand(char* line, bool fromPhone) {
  if (!strcmp(line, "STACKS")) {
#if USE_RTOS
    protoLine("# spare stack (words): io %u  radio %u  i2c %u  app %u   free heap %d bytes",
              (unsigned)uxTaskGetStackHighWaterMark(hIo), (unsigned)uxTaskGetStackHighWaterMark(hRadio),
              (unsigned)uxTaskGetStackHighWaterMark(hI2c), (unsigned)uxTaskGetStackHighWaterMark(nullptr),
              freeRam());
#else
    protoLine("# plain loop (no RTOS)   free RAM %d bytes", freeRam());
#endif
    return;
  }
  char cmd[4] = {0}, dst[proto::ID_MAX + 1];
  const char* rest;
  if (!proto::split1(line, cmd, sizeof cmd, &rest)) return;
  if (!proto::split1(rest, dst, sizeof dst, &rest)) return;
  static char arg[proto::MAX_TEXT + 1];
  proto::cleanText(rest, arg, sizeof arg);
  if (!*arg) return;
  if (fromPhone) logf("phone -> %s,%s,%s", cmd, dst, arg);
  if (!strcmp(cmd, "TX")) {
    unsigned seq = sendTracked("MS1", dst, arg, arg, 0);
    if (!fromPhone) protoLine("SENT,%u,%s,TX,%s", seq, dst, arg);   // base_station.py matches these to its sends
  } else if (fromPhone && !strcmp(cmd, "QM") && !strcmp(dst, "*") && !strcmp(arg, "HELP")) {
    quick("HELP", BTN_HELP);                                   // same as pressing the red button
  } else if (fromPhone && !strcmp(cmd, "QM") && !strcmp(dst, "*") && !strcmp(arg, "OK")) {
    quick("OK", BTN_OK);                                       // same as pressing the blue button
  } else if (!strcmp(cmd, "QM")) {
    unsigned seq = sendTracked("QM1", dst, arg, arg, 0);
    if (!fromPhone) protoLine("SENT,%u,%s,QM,%s", seq, dst, arg);
  } else if (fromPhone) {
    return;                                                    // the phone may only send texts and OK / HELP
  } else if (!strcmp(cmd, "AK")) {                             // operator acknowledges (e.g. a HELP)
    char body[32]; snprintf(body, sizeof body, "%s,%s,O", dst, arg);
    sendPkt("AK1", body);
    logf("operator ack sent to %s for #%s", dst, arg);
  }
}

// ---- packets from other boxes
// Split "a,b,c" into at most n fields (in place). The last field keeps any further commas.
int fields(char* s, char** f, int n) {
  int k = 0;
  f[k++] = s;
  for (char* q = s; *q && k < n; q++) if (*q == ',') { *q = 0; f[k++] = q + 1; }
  return k;
}

void handleRadio(const Event& e) {
  static proto::Pkt p;
  if (!proto::parse(e.data, e.len, p)) { logf("RX %d bytes, not ours (RSSI %d)", e.len, e.rssi); return; }
  if (!strcmp(p.via, NODE_ID)) return;                       // cannot be: we never hear ourselves
  {
    char want[proto::TAG_HEX + 1];
    proto::sign(meshKey, p.type, p.src, p.seq, p.rest, want);
    if (!proto::tagEqual(want, p.tag)) {                       // wrong key, old firmware, or a fake
      static uint32_t lastWarn = 0;
      if (!lastWarn || millis() - lastWarn > 10000) {
        lastWarn = millis();
        logf("!! %s from %s (via %s) failed the key check - %s", p.type, p.src, p.via,
             AUTH_CHECK ? "ignored (different key.h? old firmware? fake?)" : "ACCEPTED because AUTH_CHECK is 0");
      }
      if (AUTH_CHECK) return;
    }
  }
  bool retry = p.ttl & RETRY_FLAG;                           // the sender's second try (see RETRY_FLAG)
  unsigned left = p.ttl & (RETRY_FLAG - 1);                  // repeats left
  if (left > MAX_HOPS || p.ttl > (RETRY_FLAG | MAX_HOPS)) return;   // nobody of ours sends that
  bool ping = !strcmp(p.type, "BC1") || !strcmp(p.type, "SN1");
  bool direct = !strcmp(p.src, p.via);                       // straight from the box that wrote it
  bool mine = !strcmp(p.src, NODE_ID);                       // our own packet, repeated by a neighbour

  // ping fields: role,temp,rh,flag,boot,neighbours (SN1: temp,rh,flag)
  static char pf[proto::MAX_PKT];
  char* f[6] = {0};
  char role = 0;
  if (ping) {
    strcpy(pf, p.rest);
    if (p.type[0] == 'B') { int k = fields(pf, f, 6); for (int i = k; i < 6; i++) f[i] = (char*)""; role = f[0][0]; }
    else { int k = fields(pf, f + 1, 3); for (int i = k + 1; i < 4; i++) f[i] = (char*)""; f[0] = f[4] = f[5] = (char*)""; }
  }

  // link strength: every packet tells us `via` is in range (its role only if it wrote the ping itself)
  bool newNbr = heard(p.via, direct ? role : 0, e.rssi);
  if (newNbr) { char why[32]; snprintf(why, sizeof why, "%s in range", p.via); askSync(why); }
  if (mine) return;
  if (ping && direct && f[4][0] == '1') { char why[32]; snprintf(why, sizeof why, "%s just switched on", p.src); askSync(why); }

  bool msg = !strcmp(p.type, "MS1") || !strcmp(p.type, "QM1");
  bool again = false;                                        // a retry of a message we already have
  if (msg) {
    Seen* sn = seenFind(seenMsg, p.src, p.seq);
    if (sn) {
      Stored* s = storeFind(p.src, p.seq); if (s) s->overheard = millis();
      if (!retry || millis() - sn->at < WAVE_MS) return;     // duplicate (another relay's copy)
      sn->at = millis();                                     // first copy of the retry: pass it on + confirm again
      again = true;
    } else seenBefore(seenMsg, seenMsgPos, p.src, p.seq);
  } else if (seenBefore(seenShort, seenShortPos, p.src, p.seq)) return;
  char snr[12]; fmt2(snr, sizeof snr, e.snr);

  // ---- repeat it for boxes further away (not if it was meant only for us)
  char dst[proto::ID_MAX + 1] = "*";
  const char* rest = p.rest;
  if (!ping && !proto::split1(p.rest, dst, sizeof dst, &rest)) return;
  bool onlyForMe = !strcmp(dst, NODE_ID);
  if (RELAY && left > 0 && !onlyForMe)
    sendRaw(p.type, p.src, p.seq, (p.ttl & RETRY_FLAG) | (left - 1), p.rest, RELAY_MIN_MS + rnd() % (RELAY_MAX_MS - RELAY_MIN_MS));

  if (ping) {                                                // silent ping / sensor broadcast
    const char *t = f[1], *h = f[2], *fl = f[3];
    if (IS_BASE) {
      if (direct && role) protoLine("BCN,%s,%c,%d,%d,%s", p.src, role, role == 'B' ? 0 : -1, e.rssi, snr);
      if (role) protoLine("NBR,%s,%c,%d,%s", p.src, role, role == 'B' ? 0 : -1, f[5]);
      if (*t && strcmp(t, "-")) protoLine("DATA,%s,%u,%s,%s,%s,%d,%s,%s,%s", p.src, p.seq, t, h, fl, e.rssi, snr, p.via, NODE_ID);
    }
    return;
  }

  bool forMe = !strcmp(dst, "*") || onlyForMe;
  if (again) {                                               // already shown: only confirm again
    if (forMe) { scheduleAck(p.src, p.seq, !onlyForMe); logf("%s #%u from %s again (retry) - confirming again", p.type, p.seq, p.src); }
    return;
  }
  char hops[16] = "";
  if (!direct) snprintf(hops, sizeof hops, " (via %s)", p.via);

  if (!strcmp(p.type, "MS1")) {
    storeAdd(p.type, p.src, p.seq, p.rest);
    if (IS_BASE) protoLine("MSG,%s,%u,%s,%d,%s,%s,%s", p.src, p.seq, dst, e.rssi, snr, p.via, rest);
    if (!forMe) return;
    logf("RX text from %s%s: %s", p.src, hops, rest);
    show(D_MSG, p.src, rest, e.rssi);
    ui(UI_BEEP, 2, 0, 60);
    scheduleAck(p.src, p.seq, !onlyForMe);
  } else if (!strcmp(p.type, "QM1")) {
    storeAdd(p.type, p.src, p.seq, p.rest);
    if (IS_BASE) protoLine("QM,%s,%u,%s,%s,%d,%s,%s", p.src, p.seq, dst, rest, e.rssi, snr, p.via);
    if (!forMe) return;
    logf("RX quick message from %s%s: %s", p.src, hops, rest);
    char t[24]; snprintf(t, sizeof t, "[%s]", rest);
    show(D_MSG, p.src, t, e.rssi);
    if (!strcmp(rest, "HELP")) { ui(UI_BEEP, 3, 0, 120); ui(UI_LED, BTN_HELP, LED_FAST, 5000); }
    else if (!strcmp(rest, "RETURN")) ui(UI_BEEP, 5, 0, 200);
    else ui(UI_BEEP, 1, 0, 60);
    scheduleAck(p.src, p.seq, !onlyForMe);
  } else if (!strcmp(p.type, "AK1")) {
    unsigned seq = 0; char flag = 'R';
    sscanf(rest, "%u,%c", &seq, &flag);
    if (flag == 'R') ackHeard(dst, seq);                       // someone already confirmed it: we needn't
    if (!onlyForMe) return;                                    // confirmation for someone else
    if (IS_BASE) protoLine("ACK,%s,%u", p.src, seq);
    gotAck(p.src, seq, flag == 'O', e.rssi);
  }
}

// ---- every ~250 ms: ping, LED bar, carry alarm, confirmation timeouts, sync
void housekeeping() {
  uint32_t now = millis();
  alive(P_APP);
  watchdogCheck();
  if ((int32_t)(now - nextPing) >= 0) {
    nextPing = now + PING_EVERY_MS - 2000 + rnd() % 4000;     // jitter: avoid collisions
    if (bootPings < BOOT_PINGS) nextPing = now + 2000 + rnd() % 2000;   // quick pings right after power-on
    char rest[100], t[12] = "-", h[12] = "-", nl[52];
    if (sensorCount) { fmt2(t, sizeof t, lastTemp); fmt2(h, sizeof h, lastRh); }
    nbrList(nl, sizeof nl);
    snprintf(rest, sizeof rest, "%s,%s,%s,%s,%d,%s", myRole(), t, h, sensorCount ? (lastReal ? "R" : "S") : "-",
             bootPings < BOOT_PINGS ? 1 : 0, nl);
    if (bootPings < BOOT_PINGS) bootPings++;
    sendPkt("BC1", rest);
    if (IS_BASE) protoLine("NBR,%s,B,0,%s", NODE_ID, nl);
  }

  if (syncAt && (int32_t)(now - syncAt) >= 0) {               // sync round: one stored message at a time
    syncAt = 0;
    while (syncIdx < STORE_N) {
      Stored& s = store[(storeHead + syncIdx++) % STORE_N];     // oldest first
      if (!s.used || now - s.t > SYNC_MAX_AGE_MS) continue;
      if ((int32_t)(s.t - syncAsked) > 0) continue;                         // arrived during this sync: they have it
      if (s.overheard && (int32_t)(s.overheard - syncAsked) > 0) continue;   // another box already re-sent it
      sendRaw(s.type, s.src, s.seq, MAX_HOPS, s.rest, 0);
      logf("sync: re-sent %s #%u from %s", s.type, s.seq, s.src);
      syncAt = now + 300 + rnd() % 400;                       // next one a bit later
      break;
    }
  }

  int best = bestPct(false);
  if (best >= 0) everLinked = true;
  int level = best < 0 ? 1 : (best + 5) / 10;                  // nobody heard -> only the red segment
  if (level < 1) level = 1;
  if (level != barShown) { ui(UI_BAR, level); barShown = level; }

  if (!IS_BASE && !deployed && everLinked) {                   // CARRY: watch the link to the chain
    int chain = bestPct(true);
    if (chain < 0) chain = best;                               // no roles known yet: use any box
    if (!warned && chain <= WEAK_PCT) {
      warned = true;
      //ui(UI_ALARM, 1);
      show(D_WARN, "", chain < 0 ? "NO SIGNAL|DEPLOY A RELAY" : "WEAK LINK|DEPLOY A RELAY");   // '|' = line break
      logf("weak link (%d %%) - deploy a relay", chain);
    } else if (warned && chain >= WEAK_CLEAR_PCT) {
      warned = false;
      ui(UI_ALARM, 0);
      show(D_WARN, "", "");
      logf("link ok again (%d %%)", chain);
    }
  } else if (warned) {
    warned = false; ui(UI_ALARM, 0); show(D_WARN, "", "");
  }

  for (auto& a : ackJobs) {                                  // confirmations that are due
    if (!a.used || (int32_t)(now - a.due) < 0) continue;
    char ack[32]; snprintf(ack, sizeof ack, "%s,%u,R", a.src, a.seq);
    sendPkt("AK1", ack);
    a.used = false;
  }

  for (auto& p : pend) {
    if (!p.used) continue;
    if (!p.acks && !p.retried && now - p.sent > RETRY_AFTER_MS) {     // no confirmation yet: try once more
      p.retried = true;
      Stored* s = storeFind(NODE_ID, p.seq);
      if (s) { sendRaw(s->type, NODE_ID, s->seq, RETRY_FLAG | MAX_HOPS, s->rest, 0); logf("#%u: no confirmation yet - sending it once more", p.seq); }
    }
    if (now - p.sent > ACK_WAIT_MS) {
      if (!p.acks) {                                           // quiet: screen + fast LED only, no beep
        show(D_ACK, "-", "", 0, p.seq);
        if (p.btn) ui(UI_LED, p.btn, LED_FAST, 3000);
        logf("#%u: nobody confirmed yet (will retry when a box comes in range)", p.seq);
      }
      p.used = false;
    }
  }
}

void handle(Event& e) {
  switch (e.type) {
    case EV_BUTTON:
      if (e.a == BTN_HELP) quick("HELP", BTN_HELP);
      else if (e.a == BTN_OK) quick("OK", BTN_OK);
      break;
    case EV_SWITCH:
      deployed = (e.a == DEPLOY_LEVEL);
      logf("switch -> %s", modeName());
      ui(UI_BEEP, deployed ? 2 : 1, 0, 50);
      pushStatus();
      break;
    case EV_RADIO_RX:
      handleRadio(e);
      break;
    case EV_SERIAL:
      handleCommand((char*)e.data, false);
      break;
    case EV_PHONE:
      handleCommand((char*)e.data, true);
      break;
    case EV_SENSOR: {
      lastTemp = e.temp; lastRh = e.rh; lastReal = e.a; sensorCount++;
      char t[12], h[12];
      fmt2(t, sizeof t, e.temp); fmt2(h, sizeof h, e.rh);
      protoLine("ME,%s,%s,%d,%s,%s,%s", NODE_ID, myRole(), IS_BASE ? 0 : -1, t, h, e.a ? "R" : "S");
      pushStatus();
      break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);

#if !defined(ARDUINO_ARCH_RP2040)
  uint32_t seed = micros();                        // no hardware random generator: mix analog noise
  for (int i = 0; i < 32; i++) seed = (seed << 3) ^ (seed >> 29) ^ (uint32_t)analogRead(A0 + (i % 4)) ^ micros();
  randomSeed(seed);
#endif
  seqNo = (uint16_t)rnd();   // random start so numbers don't repeat after a reboot
  nextPing = millis() + 2000 + rnd() % 3000;
  logMutex = xSemaphoreCreateMutex();
  qEvents = xQueueCreate(8, sizeof(Event));
  qUi     = xQueueCreate(8, sizeof(UiCmd));
  qTx     = xQueueCreate(4, sizeof(TxPacket));
  qDisp   = xQueueCreate(6, sizeof(DispMsg));

  logf("=== Team 7 node %s (v1.2 relay, %s)%s ===", NODE_ID, USE_RTOS ? "FreeRTOS" : "plain loop", IS_BASE ? " BASE" : "");
  if (wdtCausedReboot()) logf("!! this start was a WATCHDOG REBOOT - something was stuck (see the log before it)");
  for (int i = 0; i < P_COUNT; i++) aliveAt[i] = millis();
#if USE_RTOS
  // stack sizes in 32-bit words (sized from measurements - type STACKS to check); higher priority runs first
  xTaskCreate(ioTask,    "io",    512, nullptr, 3, &hIo);
  xTaskCreate(radioTask, "radio", 512, nullptr, 2, &hRadio);
  xTaskCreate(i2cTask,   "i2c",   640, nullptr, 1, &hI2c);
#else
  ioInit();
  radioInit();
  i2cInit();
#endif
  for (int i = 0; i < P_COUNT; i++) aliveAt[i] = millis();   // (init can take a moment)
  wdtStart();
}

void loop() {
  static Event e;                        // static: ~140 bytes, keep it off loop()'s stack
#if USE_RTOS
  if (xQueueReceive(qEvents, &e, pdMS_TO_TICKS(250)) == pdTRUE) handle(e);
#else
  // M0: one pass = radio, buttons/buzzer/LEDs (every 10 ms), sensor/screen (every 20 ms), then the app
  static uint32_t lastI2c = 0;
  radioStep();
  idleWork();
  if (millis() - lastI2c >= 20) { lastI2c = millis(); i2cStep(); }
  while (xQueueReceive(qEvents, &e, 0) == pdTRUE) handle(e);
#endif
  housekeeping();
}
