// rtos_main.ino - Team 7 box firmware, FreeRTOS version.
//
// Arduino IDE: Tools > Board > Adafruit Feather RP2040 RFM
//              Tools > Operating System > FreeRTOS SMP      <-- required
// Libraries:   "LoRa" by Sandeep Mistry, "Adafruit SHT4x"
// Screen:      ESP32-S3 Reverse TFT Feather running arduino/display_esp32, on the I2C bus (see displink.h)
//
// One task per piece of hardware, joined by queues:
//
//   buttons task  --events--> app = loop() --UI commands------> UI task    (buzzer, button LEDs, LED bar)
//   serial  task  --events-->      |       --packets----------> radio task (LoRa)  --events--> app
//   i2c     task  --events-->      |       --display messages-> i2c task (SHT41 + ESP32 screen share Wire)
//
// What it does now:
//   button 1 short  -> quick message OK      (base app shows it; your screen shows "YOU: OK sent")
//   button 2 short  -> quick message HELP    (base app shows a red alert; other boxes beep 3x)
//   button 2 hold   -> next screen page (MAIN / NEARBY / STATUS)
//   button 1 hold   -> placeholder beep (FOUND / calibrate later)
//   switch          -> CARRY / DEPLOY (LED bar 2 / 10, shown on the screen)
//   every 10 s      -> broadcasts its temperature/humidity (SN1) so others see it in NEARBY
//   texts from the base (or any box) -> shown on the screen, beep, and confirmed back automatically
//
// BASE BOX: set IS_BASE 1, plug it into the Pi/laptop running pi/base_station.py.
// It takes these commands over USB and prints the lines the web app understands (PROTOCOL.md):
//   TX,<dst>,<text>   QM,<dst>,<code>   AK,<dst>,<seq>        (dst = box id or *)
// Not yet: relaying through other boxes, roles/chain numbers, link meter, map reports.

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
#define NODE_ID          "ALPHA"   // different on each box, max 8 chars, no commas
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
#define SHORT_PRESS_MS   800       // released before this = short press
#define LONG_PRESS_MS    1500      // held this long = long press (fires while still held)
#define DEPLOY_LEVEL     HIGH      // switch level that means DEPLOY (flip if yours is reversed)

#ifndef PIN_RFM_CS
#define PIN_RFM_CS   16
#define PIN_RFM_RST  17
#define PIN_RFM_DIO0 21
#endif

// ================================================================== messages between tasks
enum EvType : uint8_t { EV_BTN_SHORT, EV_BTN_LONG, EV_SWITCH, EV_RADIO_RX, EV_SENSOR, EV_SERIAL };
struct Event {
  EvType type;
  uint8_t a;            // button number / switch level / 1 = real sensor
  int16_t rssi;
  float snr, temp, rh;
  uint8_t len;
  uint8_t data[proto::MAX_PKT];   // received packet (with header) or a serial command line
};

enum UiType : uint8_t { UI_BEEP, UI_BAR, UI_LED };
enum LedMode : uint8_t { LED_OFF, LED_ON, LED_BLINK, LED_FAST };
struct UiCmd { UiType type; uint8_t a, b; uint16_t ms; };

struct TxPacket { uint8_t len; uint16_t delayMs; uint8_t data[proto::MAX_PKT]; };

QueueHandle_t qEvents;    // everything -> app
QueueHandle_t qUi;        // app -> UI task
QueueHandle_t qTx;        // app -> radio task
QueueHandle_t qDisp;      // app -> i2c task (ESP32 screen)
SemaphoreHandle_t logMutex;

// ================================================================== output (any task)
// logf(): human-readable lines, prefixed with the time.
// protoLine(): machine lines for base_station.py - exact format, no prefix.
void logf(const char* fmt, ...) {
  char buf[220];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  xSemaphoreTake(logMutex, portMAX_DELAY);
  Serial.printf("[%7lu] %s\n", (unsigned long)millis(), buf);
  xSemaphoreGive(logMutex);
}
void protoLine(const char* fmt, ...) {
  char buf[240];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  xSemaphoreTake(logMutex, portMAX_DELAY);
  Serial.println(buf);
  xSemaphoreGive(logMutex);
}

void ui(UiType t, uint8_t a, uint8_t b = 0, uint16_t ms = 0) {
  UiCmd c = {t, a, b, ms};
  xQueueSend(qUi, &c, 0);
}

void show(DispType t, const char* from, const char* text, int16_t rssi = 0) {
  DispMsg m = {};
  m.type = t;
  strncpy(m.from, from, 8);
  strncpy(m.text, text, sizeof m.text - 1);
  m.rssi = rssi;
  xQueueSend(qDisp, &m, 0);
}

// "23.46" without relying on printf float support
void fmt2(char* out, int max, float v) {
  long c = lroundf(v * 100.0f);
  const char* sign = c < 0 ? "-" : "";
  if (c < 0) c = -c;
  snprintf(out, max, "%s%ld.%02ld", sign, c / 100, c % 100);
}

// ================================================================== buttons task
void buttonsTask(void*) {
  const int pins[2] = {PIN_BTN1, PIN_BTN2};
  pinMode(PIN_BTN1, INPUT);           // Grove LED Button has its own pull-up: LOW = pressed
  pinMode(PIN_BTN2, INPUT);
  pinMode(PIN_SWITCH, INPUT);
  int stable[2] = {HIGH, HIGH}, cnt[2] = {0, 0};
  TickType_t downAt[2] = {0, 0};
  bool longSent[2] = {false, false};
  int swStable = digitalRead(PIN_SWITCH), swCnt = 0;
  Event e = {};
  e.type = EV_SWITCH; e.a = swStable;
  xQueueSend(qEvents, &e, portMAX_DELAY);          // report the starting position once

  for (;;) {
    TickType_t now = xTaskGetTickCount();
    for (int i = 0; i < 2; i++) {
      int raw = digitalRead(pins[i]);
      if (raw != stable[i]) {
        if (++cnt[i] >= 3) {                        // 30 ms of the new value = real change
          stable[i] = raw; cnt[i] = 0;
          if (raw == LOW) {
            downAt[i] = now; longSent[i] = false;
          } else if (!longSent[i] && (now - downAt[i]) < pdMS_TO_TICKS(SHORT_PRESS_MS)) {
            Event ev = {}; ev.type = EV_BTN_SHORT; ev.a = i + 1;
            xQueueSend(qEvents, &ev, 0);
          }
        }
      } else {
        cnt[i] = 0;
      }
      if (stable[i] == LOW && !longSent[i] && (now - downAt[i]) >= pdMS_TO_TICKS(LONG_PRESS_MS)) {
        longSent[i] = true;
        Event ev = {}; ev.type = EV_BTN_LONG; ev.a = i + 1;
        xQueueSend(qEvents, &ev, 0);
      }
    }
    int sw = digitalRead(PIN_SWITCH);
    if (sw != swStable) {
      if (++swCnt >= 5) {
        swStable = sw; swCnt = 0;
        Event ev = {}; ev.type = EV_SWITCH; ev.a = sw;
        xQueueSend(qEvents, &ev, 0);
      }
    } else {
      swCnt = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ================================================================== UI task
void uiTask(void*) {
  pinMode(PIN_BUZZER, OUTPUT);   digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_BTN1_LED, OUTPUT); pinMode(PIN_BTN2_LED, OUTPUT);
  barBegin();
  barLevel(0);

  const int ledPin[2] = {PIN_BTN1_LED, PIN_BTN2_LED};
  LedMode mode[2] = {LED_OFF, LED_OFF}, baseMode[2] = {LED_OFF, LED_OFF};
  TickType_t modeUntil[2] = {0, 0};
  int barNow = -1, barBase = 0;
  TickType_t barUntil = 0;
  int beepsLeft = 0; uint16_t beepMs = 0; bool beepOn = false; TickType_t beepNext = 0;

  for (;;) {
    UiCmd c;
    while (xQueueReceive(qUi, &c, 0) == pdTRUE) {
      TickType_t now = xTaskGetTickCount();
      if (c.type == UI_BEEP) {
        beepsLeft = c.a; beepMs = c.ms ? c.ms : 80; beepOn = false; beepNext = now;
      } else if (c.type == UI_BAR) {
        if (c.ms) { barUntil = now + pdMS_TO_TICKS(c.ms); }
        else { barBase = c.a; barUntil = 0; }
        if (c.a != barNow) { barLevel(c.a); barNow = c.a; }
      } else if (c.type == UI_LED && c.a >= 1 && c.a <= 2) {
        int i = c.a - 1;
        mode[i] = (LedMode)c.b;
        if (c.ms) modeUntil[i] = now + pdMS_TO_TICKS(c.ms);
        else { baseMode[i] = (LedMode)c.b; modeUntil[i] = 0; }
      }
    }
    TickType_t now = xTaskGetTickCount();
    if (barUntil && now >= barUntil) { barUntil = 0; if (barNow != barBase) { barLevel(barBase); barNow = barBase; } }
    for (int i = 0; i < 2; i++) {
      if (modeUntil[i] && now >= modeUntil[i]) { modeUntil[i] = 0; mode[i] = baseMode[i]; }
      bool on = mode[i] == LED_ON ||
                (mode[i] == LED_BLINK && (now / pdMS_TO_TICKS(400)) % 2) ||
                (mode[i] == LED_FAST  && (now / pdMS_TO_TICKS(100)) % 2);
      digitalWrite(ledPin[i], on ? HIGH : LOW);
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
// Owns Wire. On the bus: SHT41 (0x44) and the ESP32-S3 screen board (0x42).
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
  for (;;) {
    DispMsg m;
    while (xQueueReceive(qDisp, &m, 0) == pdTRUE) {
      char line[220];
      displinkFormat(m, NODE_ID, line, sizeof line);
      int ok = displinkSend(line) ? 1 : 0;
      if (ok != screen) {                           // log only when it changes
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

// ================================================================== serial task
// Reads command lines from the Pi/laptop (base box) and hands them to the app.
void serialTask(void*) {
  char line[proto::MAX_PKT];
  int n = 0;
  for (;;) {
    while (Serial.available()) {
      int c = Serial.read();
      if (c == '\n' || c == '\r') {
        if (n > 0) {
          Event e = {}; e.type = EV_SERIAL;
          e.len = n; memcpy(e.data, line, n); e.data[n] = 0;
          xQueueSend(qEvents, &e, 0);
          n = 0;
        }
      } else if (n < (int)sizeof line - 1) {
        line[n++] = (char)c;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
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
  struct Pending { bool used; TickType_t due; TxPacket p; };
  static Pending pend[6];
  for (;;) {
    TxPacket p;
    while (xQueueReceive(qTx, &p, 0) == pdTRUE) {          // park packets until their send time
      for (auto& s : pend) if (!s.used) {
        s.used = true; s.due = xTaskGetTickCount() + pdMS_TO_TICKS(p.delayMs); s.p = p; break;
      }
    }
    for (auto& s : pend) {
      if (s.used && xTaskGetTickCount() >= s.due) {
        if (!radioSend(s.p.data, s.p.len)) logf("TX FAILED (%d bytes)", s.p.len);
        s.used = false;
      }
    }
    int size = LoRa.parsePacket();
    if (size > 0) {
      Event e = {}; e.type = EV_RADIO_RX;
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

// ================================================================== app (setup + loop)
uint16_t seqNo;
bool deployed = false;
float lastTemp = 0, lastRh = 0; bool lastReal = false; int sensorCount = 0;

const char* myRole() { return IS_BASE ? "B" : (deployed ? "D" : "C"); }
const char* modeName() { return IS_BASE ? "BASE" : (deployed ? "DEPLOY" : "CARRY"); }

// Queue a packet for the radio. Returns the sequence number used.
unsigned sendPkt(const char* type, const char* rest, uint16_t delayMs = 0) {
  TxPacket p = {};
  unsigned seq = seqNo++;
  p.len = proto::build(p.data, sizeof p.data, type, NODE_ID, seq, 0, NODE_ID, rest);
  p.delayMs = delayMs;
  if (p.len) xQueueSend(qTx, &p, 0);
  return seq;
}

void pushStatus() {
  DispMsg m = {};
  m.type = D_STATUS;
  strncpy(m.text, modeName(), 7);
  m.temp = lastTemp; m.rh = lastRh;
  m.flag = sensorCount ? lastReal : 2;           // 2 = no reading yet
  xQueueSend(qDisp, &m, 0);
}

void quick(const char* code) {
  char rest[40];
  snprintf(rest, sizeof rest, "*,%s", code);
  unsigned seq = sendPkt("QM1", rest);
  char t[40]; snprintf(t, sizeof t, "%s sent (#%u)", code, seq);
  show(D_MSG, "YOU", t);
}

// ---- commands from the Pi / laptop (base box) ----
void handleSerial(char* line) {
  char cmd[4] = {0}, dst[proto::ID_MAX + 1];
  const char* rest;
  if (!proto::split1(line, cmd, sizeof cmd, &rest)) return;          // not a command: ignore
  if (!proto::split1(rest, dst, sizeof dst, &rest)) return;
  char arg[proto::MAX_TEXT + 1];
  proto::cleanText(rest, arg, sizeof arg);
  if (!*arg) return;
  char body[proto::MAX_PKT];
  if (!strcmp(cmd, "TX")) {
    snprintf(body, sizeof body, "%s,%s", dst, arg);
    unsigned seq = sendPkt("MS1", body);
    protoLine("SENT,%u,%s,TX,%s", seq, dst, arg);
    show(D_MSG, "YOU", arg);
  } else if (!strcmp(cmd, "QM")) {
    snprintf(body, sizeof body, "%s,%s", dst, arg);
    unsigned seq = sendPkt("QM1", body);
    protoLine("SENT,%u,%s,QM,%s", seq, dst, arg);
  } else if (!strcmp(cmd, "AK")) {
    snprintf(body, sizeof body, "%s,%s", dst, arg);
    sendPkt("AK1", body);
    logf("ack sent to %s for #%s", dst, arg);
  }
}

// ---- packets from other boxes ----
void handleRadio(const Event& e) {
  proto::Pkt p;
  if (!proto::parse(e.data, e.len, p)) {
    logf("RX %d bytes, not ours (RSSI %d)", e.len, e.rssi);
    return;
  }
  if (!strcmp(p.src, NODE_ID)) return;                               // our own, echoed
  char snr[12]; fmt2(snr, sizeof snr, e.snr);
  show(D_NODE, p.via, "", e.rssi);
  int level = (e.rssi + 120) / 6;
  ui(UI_BAR, level < 0 ? 0 : level > 10 ? 10 : level, 0, 2000);     // signal strength for 2 s

  char dst[proto::ID_MAX + 1];
  const char* rest;
  bool forMe = false;
  if (!strcmp(p.type, "SN1")) {
    char t[12], h[12], f[4];
    if (sscanf(p.rest, "%11[^,],%11[^,],%3s", t, h, f) == 3) {
      if (IS_BASE) protoLine("DATA,%s,%u,%s,%s,%s,%d,%s,%s,%s", p.src, p.seq, t, h, f, e.rssi, snr, p.via, NODE_ID);
      logf("RX %s sensor %s C %s %%RH (RSSI %d)", p.src, t, h, e.rssi);
    }
    return;
  }
  if (!proto::split1(p.rest, dst, sizeof dst, &rest)) return;
  forMe = !strcmp(dst, "*") || !strcmp(dst, NODE_ID);

  if (!strcmp(p.type, "MS1")) {
    if (IS_BASE) protoLine("MSG,%s,%u,%s,%d,%s,%s,%s", p.src, p.seq, dst, e.rssi, snr, p.via, rest);
    if (!forMe) return;
    logf("RX text from %s: %s", p.src, rest);
    show(D_MSG, p.src, rest, e.rssi);
    ui(UI_BEEP, 2, 0, 60);
    char ack[24]; snprintf(ack, sizeof ack, "%s,%u", p.src, p.seq);
    sendPkt("AK1", ack, 100 + (uint16_t)(rp2040.hwrand32() % 400));   // random wait: many boxes may answer
  } else if (!strcmp(p.type, "QM1")) {
    if (IS_BASE) protoLine("QM,%s,%u,%s,%s,%d,%s,%s", p.src, p.seq, dst, rest, e.rssi, snr, p.via);
    if (!forMe) return;
    logf("RX quick message from %s: %s", p.src, rest);
    char t[40]; snprintf(t, sizeof t, "[%s]", rest);
    show(D_MSG, p.src, t, e.rssi);
    if (!strcmp(rest, "HELP")) { ui(UI_BEEP, 3, 0, 120); ui(UI_LED, 1, LED_FAST, 5000); ui(UI_LED, 2, LED_FAST, 5000); }
    else if (!strcmp(rest, "RETURN")) { ui(UI_BEEP, 5, 0, 200); ui(UI_BAR, 10, 0, 5000); }
    else ui(UI_BEEP, 1, 0, 60);
  } else if (!strcmp(p.type, "AK1")) {
    if (strcmp(dst, NODE_ID)) return;                                // acks for someone else
    if (IS_BASE) protoLine("ACK,%s,%s", p.src, rest);
    logf("%s confirmed our message #%s", p.src, rest);
    char t[40]; snprintf(t, sizeof t, "got your msg #%s", rest);
    show(D_MSG, p.src, t, e.rssi);
    ui(UI_LED, 1, LED_ON, 3000); ui(UI_LED, 2, LED_ON, 3000); ui(UI_BEEP, 1, 0, 40);
  }
}

void handle(Event& e) {
  switch (e.type) {
    case EV_BTN_SHORT:
      if (e.a == 1) { logf("button 1 -> OK");   quick("OK");   ui(UI_LED, 1, LED_ON, 1000);   ui(UI_BEEP, 1, 0, 40); }
      else          { logf("button 2 -> HELP"); quick("HELP"); ui(UI_LED, 2, LED_FAST, 3000); ui(UI_BEEP, 2, 0, 60); }
      break;
    case EV_BTN_LONG:
      if (e.a == 2) { DispMsg m = {}; m.type = D_PAGE; xQueueSend(qDisp, &m, 0); ui(UI_BEEP, 1, 0, 20); }
      else { logf("button 1 long press (placeholder)"); ui(UI_BEEP, 1, 0, 400); }
      break;
    case EV_SWITCH:
      deployed = (e.a == DEPLOY_LEVEL);
      logf("switch -> %s", modeName());
      ui(UI_BAR, deployed ? 10 : 2);
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
      lastTemp = e.temp; lastRh = e.rh; lastReal = e.a;
      char t[12], h[12];
      fmt2(t, sizeof t, e.temp); fmt2(h, sizeof h, e.rh);
      protoLine("ME,%s,%s,%d,%s,%s,%s", NODE_ID, myRole(), IS_BASE ? 0 : -1, t, h, e.a ? "R" : "S");
      if (++sensorCount % 2 == 0) {                    // every 10 s: tell the others
        char rest[40]; snprintf(rest, sizeof rest, "%s,%s,%s", t, h, e.a ? "R" : "S");
        sendPkt("SN1", rest, (uint16_t)(rp2040.hwrand32() % 300));
      }
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
  logMutex = xSemaphoreCreateMutex();
  qEvents = xQueueCreate(16, sizeof(Event));
  qUi     = xQueueCreate(16, sizeof(UiCmd));
  qTx     = xQueueCreate(6,  sizeof(TxPacket));
  qDisp   = xQueueCreate(10, sizeof(DispMsg));

  logf("=== Team 7 node %s (FreeRTOS)%s ===", NODE_ID, IS_BASE ? " BASE" : "");
  // stack sizes are in 32-bit words; priority: higher number runs first
  xTaskCreate(buttonsTask, "buttons", 1024, nullptr, 3, nullptr);
  xTaskCreate(uiTask,      "ui",      1024, nullptr, 2, nullptr);
  xTaskCreate(radioTask,   "radio",   2048, nullptr, 2, nullptr);
  xTaskCreate(i2cTask,     "i2c",     2048, nullptr, 1, nullptr);
  xTaskCreate(serialTask,  "serial",  1024, nullptr, 1, nullptr);
}

void loop() {
  static Event e;                        // static: ~230 bytes, keep it off loop()'s stack
  if (xQueueReceive(qEvents, &e, portMAX_DELAY) == pdTRUE)
    handle(e);
}
