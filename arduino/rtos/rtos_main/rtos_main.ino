// rtos_main.ino - Team 7 box firmware, FreeRTOS version (v1.1).
//
// Arduino IDE: Tools > Board > Adafruit Feather RP2040 RFM
//              Tools > Operating System > FreeRTOS SMP      <-- required
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
//   Nobody confirms in 10 s -> that LED blinks fast + two beeps, screen shows "no reply".
//   Every box confirms (ACKs) every text and quick message it receives, automatically.
//   Every 30 s each box sends a silent ping (with its role and temperature) - no beep, no message.
//   LED bar = signal strength of the strongest box heard (10 = strong). Nobody heard -> only the first (red) segment.
//   CARRY mode: when the link to the deployed boxes / base drops to ~30 % -> rapid beeping and
//               "DEPLOY A RELAY" on the screen, until the signal recovers (40 %) or you switch to DEPLOY.
//
// BASE BOX: set IS_BASE 1 and plug it into the Pi/laptop running pi/base_station.py.
//   Commands from the app:  TX,<dst>,<text>   QM,<dst>,<code>   AK,<dst>,<seq>   (dst = box id or *)
//   Debug command:          STACKS   (prints spare stack per task + free heap)
// Not yet: relaying through other boxes, message sync between boxes (see plan in README).

#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <semphr.h>
#include <SPI.h>
#include <Wire.h>
#include <LoRa.h>
#include <Adafruit_SHT4x.h>
#include "pins.h"
#include "ledbar.h"
#include "proto.h"
#include "displink.h"

// ================================================================== config
#ifndef NODE_ID
#define NODE_ID          "BRAVO"   // different on each box, max 8 chars, no commas
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
#define ACK_WAIT_MS 10000     // how long to wait for a confirmation
#endif
#define BTN_HELP         1         // red button  = button 1 (D5 LED / D6 button)
#define BTN_OK           2         // blue button = button 2 (D24 LED / D25 button)
#define DEPLOY_LEVEL     HIGH      // switch level that means DEPLOY (flip if yours is reversed)
#define BAR_RED_FIRST    1         // 1 if segment 1 of the LED bar is the red one; 0 if the bar is mounted reversed

#ifndef PIN_RFM_CS
#define PIN_RFM_CS   16
#define PIN_RFM_RST  17
#define PIN_RFM_DIO0 21
#endif

// ================================================================== messages between tasks
enum EvType : uint8_t { EV_BUTTON, EV_SWITCH, EV_RADIO_RX, EV_SENSOR, EV_SERIAL };
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

int signalPct(float rssi) {                        // -120 dBm = 0 %, -60 dBm or better = 100 %
  int p = (int)((rssi + 120.0f) * 100.0f / 60.0f);
  return p < 0 ? 0 : p > 100 ? 100 : p;
}

// ================================================================== io task
// Buttons + switch in, buzzer + LEDs + LED bar out, USB serial command lines in. Steps every 10 ms.
void ioTask(void*) {
  const int btnPin[2] = {PIN_BTN1, PIN_BTN2}, ledPin[2] = {PIN_BTN1_LED, PIN_BTN2_LED};
  pinMode(PIN_BTN1, INPUT);            // Grove LED Button has its own pull-up: LOW = pressed
  pinMode(PIN_BTN2, INPUT);
  pinMode(PIN_SWITCH, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);   digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_BTN1_LED, OUTPUT); pinMode(PIN_BTN2_LED, OUTPUT);
  barBegin();

  int stable[2] = {HIGH, HIGH}, cnt[2] = {0, 0};
  int swStable = digitalRead(PIN_SWITCH), swCnt = 0;
  { Event e = {}; e.type = EV_SWITCH; e.a = swStable; xQueueSend(qEvents, &e, portMAX_DELAY); }

  LedMode mode[2] = {LED_OFF, LED_OFF}, baseMode[2] = {LED_OFF, LED_OFF};
  TickType_t modeUntil[2] = {0, 0};
  int barNow = -1;
  int beepsLeft = 0; uint16_t beepMs = 80; bool beepOn = false; TickType_t beepNext = 0;
  bool alarm = false; TickType_t alarmNext = 0;
  static char line[proto::MAX_PKT]; int lineLen = 0;
  static Event ev;                                  // serial lines are big: keep off the stack

  for (;;) {
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
      else if (c.type == UI_ALARM) { alarm = c.a; alarmNext = now; }
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
    if (alarm && beepsLeft == 0 && now >= alarmNext) {         // rapid burst: 5 beeps every 1.5 s
      beepsLeft = 5; beepMs = 60; beepOn = false; beepNext = now;
      alarmNext = now + pdMS_TO_TICKS(1500);
    }
    if (beepsLeft > 0 && now >= beepNext) {
      beepOn = !beepOn;
      digitalWrite(PIN_BUZZER, beepOn ? HIGH : LOW);
      if (!beepOn) beepsLeft--;
      beepNext = now + pdMS_TO_TICKS(beepMs);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ================================================================== I2C task (SHT41 + screen)
void i2cTask(void*) {
  Wire.begin();
  Wire.setClock(100000);
  Adafruit_SHT4x sht4;
  bool have = sht4.begin(&Wire);
  if (have) { sht4.setPrecision(SHT4X_HIGH_PRECISION); sht4.setHeater(SHT4X_NO_HEATER); }
  logf("SHT41 %s", have ? "found" : "not found - sending simulated values");

  float simT = 21.0f, simH = 45.0f;
  int screen = -1;                                  // -1 unknown, 0 not answering, 1 ok
  TickType_t nextSensor = xTaskGetTickCount();
  static DispMsg m;
  static char line[128];
  for (;;) {
    while (xQueueReceive(qDisp, &m, 0) == pdTRUE) {
      displinkFormat(m, NODE_ID, line, sizeof line);
      int ok = displinkSend(line) ? 1 : 0;
      if (ok != screen) {
        logf(ok ? "screen connected (I2C 0x42)" : "screen not answering on I2C 0x42 - check SDA/SCL/GND and that the ESP32 is running");
        screen = ok;
      }
    }
    TickType_t now = xTaskGetTickCount();
    if (now >= nextSensor) {
      nextSensor = now + pdMS_TO_TICKS(SENSOR_EVERY_MS);
      Event e = {}; e.type = EV_SENSOR;
      sensors_event_t hum, temp;
      if (have && sht4.getEvent(&hum, &temp)) {
        e.a = 1; e.temp = temp.temperature; e.rh = hum.relative_humidity;
      } else {
        simT += (int)(rp2040.hwrand32() % 41 - 20) / 100.0f;
        simH += (int)(rp2040.hwrand32() % 101 - 50) / 100.0f;
        e.a = 0; e.temp = simT; e.rh = simH;
      }
      xQueueSend(qEvents, &e, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
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

void radioTask(void*) {
  if (!radioStart()) {
    logf("!! LoRa init FAILED - check the board selection");
    ui(UI_LED, 1, LED_FAST); ui(UI_LED, 2, LED_FAST);
    vTaskDelete(nullptr);
  }
  logf("radio ready: %ld MHz SF%d %d dBm", (long)(LORA_FREQ_HZ / 1e6), LORA_SF, LORA_TX_DBM);
  struct Parked { bool used; TickType_t due; TxPacket p; };
  static Parked parked[4];
  static TxPacket p;
  static Event e;
  for (;;) {
    while (xQueueReceive(qTx, &p, 0) == pdTRUE) {          // park packets until their send time
      for (auto& s : parked) if (!s.used) {
        s.used = true; s.due = xTaskGetTickCount() + pdMS_TO_TICKS(p.delayMs); s.p = p; break;
      }
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
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// ================================================================== app state
uint16_t seqNo;
bool deployed = false;
float lastTemp = 0, lastRh = 0; bool lastReal = false; int sensorCount = 0;

struct Nbr { bool used; char id[9]; char role; float rssi; uint32_t last; };
Nbr nbrs[8];                                   // boxes heard directly

struct Pend { bool used; uint16_t seq; uint8_t btn; char what[12]; uint32_t sent; uint8_t acks; };
Pend pend[4];                                  // our messages waiting for confirmation

bool warned = false, everLinked = false;
uint32_t nextPing = 0;
int barShown = -1;

const char* myRole() { return IS_BASE ? "B" : (deployed ? "D" : "C"); }
const char* modeName() { return IS_BASE ? "BASE" : (deployed ? "DEPLOY" : "CARRY"); }

unsigned sendPkt(const char* type, const char* rest, uint16_t delayMs = 0) {
  static TxPacket p;
  memset(&p, 0, sizeof p);
  unsigned seq = seqNo++;
  p.len = proto::build(p.data, sizeof p.data, type, NODE_ID, seq, 0, NODE_ID, rest);
  p.delayMs = delayMs;
  if (p.len) xQueueSend(qTx, &p, 0);
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

void heard(const char* id, char role, int rssi) {
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
  n.used = true;
  n.rssi = fresh ? rssi : 0.6f * n.rssi + 0.4f * rssi;   // smooth out single-packet noise
  if (role) n.role = role;
  n.last = millis();
  show(D_NODE, id, "", (int16_t)n.rssi);
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

// ---- sending with confirmation tracking
unsigned sendTracked(const char* type, const char* dst, const char* body, const char* what, uint8_t btn) {
  char rest[proto::MAX_PKT];
  snprintf(rest, sizeof rest, "%s,%s", dst, body);
  unsigned seq = sendPkt(type, rest);
  int slot = 0;
  for (int i = 0; i < 4; i++) if (!pend[i].used) { slot = i; break; } else if (pend[i].sent < pend[slot].sent) slot = i;
  pend[slot].used = true; pend[slot].seq = seq; pend[slot].btn = btn; pend[slot].acks = 0;
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
}

// ---- commands from the Pi / laptop (base box), plus STACKS for debugging
void handleSerial(char* line) {
  if (!strcmp(line, "STACKS")) {
    protoLine("# spare stack (words): io %u  radio %u  i2c %u  app %u   free heap %d bytes",
              (unsigned)uxTaskGetStackHighWaterMark(hIo), (unsigned)uxTaskGetStackHighWaterMark(hRadio),
              (unsigned)uxTaskGetStackHighWaterMark(hI2c), (unsigned)uxTaskGetStackHighWaterMark(nullptr),
              rp2040.getFreeHeap());
    return;
  }
  char cmd[4] = {0}, dst[proto::ID_MAX + 1];
  const char* rest;
  if (!proto::split1(line, cmd, sizeof cmd, &rest)) return;
  if (!proto::split1(rest, dst, sizeof dst, &rest)) return;
  static char arg[proto::MAX_TEXT + 1];
  proto::cleanText(rest, arg, sizeof arg);
  if (!*arg) return;
  if (!strcmp(cmd, "TX")) {
    unsigned seq = sendTracked("MS1", dst, arg, arg, 0);
    protoLine("SENT,%u,%s,TX,%s", seq, dst, arg);
  } else if (!strcmp(cmd, "QM")) {
    unsigned seq = sendTracked("QM1", dst, arg, arg, 0);
    protoLine("SENT,%u,%s,QM,%s", seq, dst, arg);
  } else if (!strcmp(cmd, "AK")) {                             // operator acknowledges (e.g. a HELP)
    char body[32]; snprintf(body, sizeof body, "%s,%s,O", dst, arg);
    sendPkt("AK1", body);
    logf("operator ack sent to %s for #%s", dst, arg);
  }
}

// ---- packets from other boxes
void handleRadio(const Event& e) {
  static proto::Pkt p;
  if (!proto::parse(e.data, e.len, p)) { logf("RX %d bytes, not ours (RSSI %d)", e.len, e.rssi); return; }
  if (!strcmp(p.src, NODE_ID)) return;                       // our own, echoed
  char snr[12]; fmt2(snr, sizeof snr, e.snr);

  if (!strcmp(p.type, "BC1") || !strcmp(p.type, "SN1")) {    // silent ping / sensor broadcast
    char role = 0, t[12] = "", h[12] = "", f[4] = "";
    if (p.type[0] == 'B') {
      char r[4] = "";
      sscanf(p.rest, "%3[^,],%11[^,],%11[^,],%3s", r, t, h, f);
      role = r[0];
    } else {
      sscanf(p.rest, "%11[^,],%11[^,],%3s", t, h, f);
    }
    heard(p.via, role, e.rssi);
    if (IS_BASE) {
      if (role) protoLine("BCN,%s,%c,%d,%d,%s", p.src, role, role == 'B' ? 0 : -1, e.rssi, snr);
      if (*t && strcmp(t, "-")) protoLine("DATA,%s,%u,%s,%s,%s,%d,%s,%s,%s", p.src, p.seq, t, h, f, e.rssi, snr, p.via, NODE_ID);
    }
    return;
  }
  heard(p.via, 0, e.rssi);

  char dst[proto::ID_MAX + 1];
  const char* rest;
  if (!proto::split1(p.rest, dst, sizeof dst, &rest)) return;
  bool forMe = !strcmp(dst, "*") || !strcmp(dst, NODE_ID);
  char ack[32]; snprintf(ack, sizeof ack, "%s,%u,R", p.src, p.seq);

  if (!strcmp(p.type, "MS1")) {
    if (IS_BASE) protoLine("MSG,%s,%u,%s,%d,%s,%s,%s", p.src, p.seq, dst, e.rssi, snr, p.via, rest);
    if (!forMe) return;
    logf("RX text from %s: %s", p.src, rest);
    show(D_MSG, p.src, rest, e.rssi);
    ui(UI_BEEP, 2, 0, 60);
    sendPkt("AK1", ack, 100 + (uint16_t)(rp2040.hwrand32() % 500));   // random wait: many boxes may answer
  } else if (!strcmp(p.type, "QM1")) {
    if (IS_BASE) protoLine("QM,%s,%u,%s,%s,%d,%s,%s", p.src, p.seq, dst, rest, e.rssi, snr, p.via);
    if (!forMe) return;
    logf("RX quick message from %s: %s", p.src, rest);
    char t[24]; snprintf(t, sizeof t, "[%s]", rest);
    show(D_MSG, p.src, t, e.rssi);
    if (!strcmp(rest, "HELP")) { ui(UI_BEEP, 3, 0, 120); ui(UI_LED, BTN_HELP, LED_FAST, 5000); }
    else if (!strcmp(rest, "RETURN")) ui(UI_BEEP, 5, 0, 200);
    else ui(UI_BEEP, 1, 0, 60);
    sendPkt("AK1", ack, 100 + (uint16_t)(rp2040.hwrand32() % 500));
  } else if (!strcmp(p.type, "AK1")) {
    if (strcmp(dst, NODE_ID)) return;                          // confirmation for someone else
    unsigned seq = 0; char flag = 'R';
    sscanf(rest, "%u,%c", &seq, &flag);
    if (IS_BASE) protoLine("ACK,%s,%u", p.src, seq);
    gotAck(p.src, seq, flag == 'O', e.rssi);
  }
}

// ---- every ~250 ms: ping, LED bar, carry alarm, confirmation timeouts
void housekeeping() {
  uint32_t now = millis();
  if ((int32_t)(now - nextPing) >= 0) {
    nextPing = now + PING_EVERY_MS - 2000 + rp2040.hwrand32() % 4000;     // jitter: avoid collisions
    char rest[40], t[12] = "-", h[12] = "-";
    if (sensorCount) { fmt2(t, sizeof t, lastTemp); fmt2(h, sizeof h, lastRh); }
    snprintf(rest, sizeof rest, "%s,%s,%s,%s", myRole(), t, h, sensorCount ? (lastReal ? "R" : "S") : "-");
    sendPkt("BC1", rest);
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
      ui(UI_ALARM, 1);
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

  for (auto& p : pend) {
    if (p.used && now - p.sent > ACK_WAIT_MS) {
      if (!p.acks) {
        show(D_ACK, "-", "", 0, p.seq);
        if (p.btn) ui(UI_LED, p.btn, LED_FAST, 3000);
        ui(UI_BEEP, 2, 0, 150);
        logf("#%u: nobody confirmed", p.seq);
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
      handleSerial((char*)e.data);
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

  seqNo = (uint16_t)rp2040.hwrand32();   // random start so numbers don't repeat after a reboot
  nextPing = millis() + 2000 + rp2040.hwrand32() % 3000;
  logMutex = xSemaphoreCreateMutex();
  qEvents = xQueueCreate(8, sizeof(Event));
  qUi     = xQueueCreate(8, sizeof(UiCmd));
  qTx     = xQueueCreate(4, sizeof(TxPacket));
  qDisp   = xQueueCreate(6, sizeof(DispMsg));

  logf("=== Team 7 node %s (FreeRTOS v1.1)%s ===", NODE_ID, IS_BASE ? " BASE" : "");
  // stack sizes in 32-bit words (sized from measurements - type STACKS to check); higher priority runs first
  xTaskCreate(ioTask,    "io",    512, nullptr, 3, &hIo);
  xTaskCreate(radioTask, "radio", 512, nullptr, 2, &hRadio);
  xTaskCreate(i2cTask,   "i2c",   640, nullptr, 1, &hI2c);
}

void loop() {
  static Event e;                        // static: ~140 bytes, keep it off loop()'s stack
  if (xQueueReceive(qEvents, &e, pdMS_TO_TICKS(250)) == pdTRUE) handle(e);
  housekeeping();
}
