// rtos_main.ino - Team 7 box firmware, FreeRTOS version (skeleton).
//
// Arduino IDE: Tools > Board > Adafruit Feather RP2040 RFM
//              Tools > Operating System > FreeRTOS SMP      <-- required
// Libraries:   "LoRa" by Sandeep Mistry, "Adafruit SHT4x"
//
// This is the FOUNDATION for the real firmware: one task per piece of hardware,
// connected by queues. It does a few small things end-to-end so each part is
// testable; the full features (relay/dedupe, roles, link meter, map reports,
// base-station protocol) get moved in one at a time.
//
//   buttons task --events--> app (loop) --commands--> UI task (buzzer, button LEDs, LED bar)
//   sensor  task --events-->     |      --packets---> radio task (LoRa) --events--> app
//
// What it does right now (two boxes with this sketch):
//   button 1 short  -> sends "PING"   : your LED 1 lights 1 s, other box beeps once
//   button 2 short  -> sends "HELP"   : your LED 2 blinks fast, other box beeps 3x + both LEDs blink
//   button 1 long   -> beep (placeholder for FOUND / calibrate later)
//   switch          -> CARRY / DEPLOY shown on the LED bar (2 segments / 10 segments)
//   packet received -> LED bar shows its signal strength for 3 s
//   every 5 s       -> temperature/humidity printed
// Serial Monitor at 115200 shows everything.

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

// ================================================================== config
#define NODE_ID          "ALPHA"   // different on each box, max 8 chars
#define LORA_FREQ_HZ     915E6     // radio settings must match on every box
#define LORA_SF          7
#define LORA_BW_HZ       125E3
#define LORA_CR_DENOM    5
#define LORA_TX_DBM      14
#define TX_TIMEOUT_MS    3000
#define SENSOR_EVERY_MS  5000
#define SHORT_PRESS_MS   800       // released before this = short press
#define LONG_PRESS_MS    1500      // held this long = long press (fires while still held)

#ifndef PIN_RFM_CS
#define PIN_RFM_CS   16
#define PIN_RFM_RST  17
#define PIN_RFM_DIO0 21
#endif

// ================================================================== messages between tasks
enum EvType : uint8_t { EV_BTN_SHORT, EV_BTN_LONG, EV_SWITCH, EV_RADIO_RX, EV_SENSOR };
struct Event {
  EvType type;
  uint8_t a;            // button number / switch level / 1 = real sensor
  int16_t rssi;
  float snr, temp, rh;
  uint8_t len;
  uint8_t data[64];     // received packet (incl. 4-byte RadioHead header)
};

enum UiType : uint8_t { UI_BEEP, UI_BAR, UI_LED };
enum LedMode : uint8_t { LED_OFF, LED_ON, LED_BLINK, LED_FAST };
struct UiCmd {
  UiType type;
  uint8_t a, b;         // BEEP: a = count | BAR: a = level 0..10 | LED: a = led 1/2, b = mode
  uint16_t ms;          // BEEP: length of each beep | BAR/LED: how long before going back (0 = stay)
};

struct TxPacket { uint8_t len; uint8_t data[64]; };

QueueHandle_t qEvents;    // everything -> app
QueueHandle_t qUi;        // app -> UI task
QueueHandle_t qTx;        // app -> radio task
SemaphoreHandle_t logMutex;

// ================================================================== logging (any task)
void logf(const char* fmt, ...) {
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  xSemaphoreTake(logMutex, portMAX_DELAY);
  Serial.printf("[%7lu] %s\n", (unsigned long)millis(), buf);
  xSemaphoreGive(logMutex);
}

void ui(UiType t, uint8_t a, uint8_t b = 0, uint16_t ms = 0) {
  UiCmd c = {t, a, b, ms};
  xQueueSend(qUi, &c, 0);
}

// "23.46" without relying on printf float support
void fmt2(char* out, int max, float v) {
  long c = lroundf(v * 100.0f);
  const char* sign = c < 0 ? "-" : "";
  if (c < 0) c = -c;
  snprintf(out, max, "%s%ld.%02ld", sign, c / 100, c % 100);
}

// ================================================================== buttons task
// Polls both buttons + the switch every 10 ms, debounces, turns them into events.
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
          if (raw == LOW) {                         // pressed
            downAt[i] = now; longSent[i] = false;
          } else if (!longSent[i] && (now - downAt[i]) < pdMS_TO_TICKS(SHORT_PRESS_MS)) {
            Event ev = {}; ev.type = EV_BTN_SHORT; ev.a = i + 1;   // released quickly
            xQueueSend(qEvents, &ev, 0);
          }
        }
      } else {
        cnt[i] = 0;
      }
      if (stable[i] == LOW && !longSent[i] && (now - downAt[i]) >= pdMS_TO_TICKS(LONG_PRESS_MS)) {
        longSent[i] = true;                         // long press fires while still held
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
// Owns the buzzer, both button LEDs and the LED bar. Never blocks on a beep:
// everything is a little state machine stepped every 10 ms.
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
    // temporary bar/LED states expire back to their base state
    if (barUntil && now >= barUntil) { barUntil = 0; if (barNow != barBase) { barLevel(barBase); barNow = barBase; } }
    for (int i = 0; i < 2; i++) {
      if (modeUntil[i] && now >= modeUntil[i]) { modeUntil[i] = 0; mode[i] = baseMode[i]; }
      bool on = mode[i] == LED_ON ||
                (mode[i] == LED_BLINK && (now / pdMS_TO_TICKS(400)) % 2) ||
                (mode[i] == LED_FAST  && (now / pdMS_TO_TICKS(100)) % 2);
      digitalWrite(ledPin[i], on ? HIGH : LOW);
    }
    // beeps: on for beepMs, off for beepMs, repeat
    if (beepsLeft > 0 && now >= beepNext) {
      beepOn = !beepOn;
      digitalWrite(PIN_BUZZER, beepOn ? HIGH : LOW);
      if (!beepOn) beepsLeft--;
      beepNext = now + pdMS_TO_TICKS(beepMs);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ================================================================== sensor task
// Owns Wire (STEMMA QT). If an OLED is added later, it must be drawn from THIS task too
// (or Wire gets a mutex), because two tasks using Wire at once corrupts both.
void sensorTask(void*) {
  Adafruit_SHT4x sht4;
  Wire.begin();
  bool have = sht4.begin(&Wire);
  if (have) {
    sht4.setPrecision(SHT4X_HIGH_PRECISION);
    sht4.setHeater(SHT4X_NO_HEATER);
    logf("SHT41 found");
  } else {
    logf("SHT41 not found - sending simulated values");
  }
  float simT = 21.0f, simH = 45.0f;
  TickType_t last = xTaskGetTickCount();
  for (;;) {
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
    vTaskDelayUntil(&last, pdMS_TO_TICKS(SENSOR_EVERY_MS));   // exact 5 s period
  }
}

// ================================================================== radio task
// Owns SPI + the RFM95. Sends whatever the app queues, reports what it receives.
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
  LoRa.endPacket(true);                       // start sending, don't block
  TickType_t t0 = xTaskGetTickCount();
  while ((rfmRead(REG_IRQ_FLAGS) & IRQ_TX_DONE) == 0) {
    if (xTaskGetTickCount() - t0 > pdMS_TO_TICKS(TX_TIMEOUT_MS)) {
      logf("!! TX timeout - restarting radio (check antenna / power)");
      LoRa.end();
      radioStart();
      return false;
    }
    vTaskDelay(1);                            // let other tasks run while the packet goes out
  }
  rfmWrite(REG_IRQ_FLAGS, IRQ_TX_DONE);
  return true;
}

void radioTask(void*) {
  if (!radioStart()) {
    logf("!! LoRa init FAILED - check the board selection");
    ui(UI_LED, 1, LED_FAST); ui(UI_LED, 2, LED_FAST);
    vTaskDelete(nullptr);                     // the rest of the box keeps working
  }
  logf("radio ready: %ld MHz SF%d %d dBm", (long)(LORA_FREQ_HZ / 1e6), LORA_SF, LORA_TX_DBM);
  for (;;) {
    TxPacket p;
    while (xQueueReceive(qTx, &p, 0) == pdTRUE) {
      bool ok = radioSend(p.data, p.len);
      logf("TX %d bytes %s", p.len, ok ? "ok" : "FAILED");
    }
    int size = LoRa.parsePacket();
    if (size > 0) {
      Event e = {}; e.type = EV_RADIO_RX;
      while (LoRa.available()) {
        int b = LoRa.read();
        if (e.len < sizeof e.data) e.data[e.len++] = (uint8_t)b;
      }
      e.rssi = LoRa.packetRssi();
      e.snr = LoRa.packetSnr();
      xQueueSend(qEvents, &e, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// ================================================================== app (setup + loop)
// The "brain": reacts to events and tells the other tasks what to do.
// This is where roles, relay, link meter, protocol etc. will go.
uint16_t seqNo = 0;

void sendText(const char* kind) {
  TxPacket p;
  p.data[0] = 0xFF; p.data[1] = 0xFF; p.data[2] = 0; p.data[3] = 0;   // RadioHead-style broadcast header
  int n = snprintf((char*)p.data + 4, sizeof p.data - 4, "%s,%s,%u", kind, NODE_ID, (unsigned)seqNo++);
  p.len = 4 + n;
  xQueueSend(qTx, &p, 0);
}

void handle(const Event& e) {
  switch (e.type) {
    case EV_BTN_SHORT:
      if (e.a == 1) {
        logf("button 1 -> PING");
        sendText("PING");
        ui(UI_LED, 1, LED_ON, 1000); ui(UI_BEEP, 1, 0, 40);
      } else {
        logf("button 2 -> HELP");
        sendText("HELP");
        ui(UI_LED, 2, LED_FAST, 3000); ui(UI_BEEP, 2, 0, 60);
      }
      break;
    case EV_BTN_LONG:
      logf("button %d long press (placeholder)", e.a);
      ui(UI_BEEP, 1, 0, 400);
      break;
    case EV_SWITCH: {
      bool deploy = (e.a == HIGH);          // flip if your switch is the other way round
      logf("switch -> %s", deploy ? "DEPLOY" : "CARRY");
      ui(UI_BAR, deploy ? 10 : 2);
      ui(UI_BEEP, deploy ? 2 : 1, 0, 50);
      break;
    }
    case EV_RADIO_RX: {
      char text[65] = {0};
      int n = 0;
      for (int i = 4; i < e.len && n < 64; i++)                     // skip the 4-byte header
        text[n++] = (e.data[i] >= 32 && e.data[i] < 127) ? (char)e.data[i] : '.';
      char snr[12]; fmt2(snr, sizeof snr, e.snr);
      logf("RX \"%s\"  RSSI %d dBm  SNR %s", text, e.rssi, snr);
      int level = (e.rssi + 120) / 6;                                // rough: -120 dBm = 0, -60 = 10
      ui(UI_BAR, level < 0 ? 0 : level > 10 ? 10 : level, 0, 3000);
      if (strncmp(text, "HELP", 4) == 0) {
        ui(UI_BEEP, 3, 0, 120); ui(UI_LED, 1, LED_FAST, 5000); ui(UI_LED, 2, LED_FAST, 5000);
      } else if (strncmp(text, "PING", 4) == 0) {
        ui(UI_BEEP, 1, 0, 40);
      }
      break;
    }
    case EV_SENSOR: {
      char t[12], h[12];
      fmt2(t, sizeof t, e.temp); fmt2(h, sizeof h, e.rh);
      logf("sensor: %s C  %s %%RH%s", t, h, e.a ? "" : "  (simulated)");
      break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);

  logMutex = xSemaphoreCreateMutex();
  qEvents = xQueueCreate(16, sizeof(Event));
  qUi     = xQueueCreate(16, sizeof(UiCmd));
  qTx     = xQueueCreate(8,  sizeof(TxPacket));

  logf("=== Team 7 node %s (FreeRTOS) ===", NODE_ID);
  // stack sizes are in 32-bit words; priority: higher number runs first
  xTaskCreate(buttonsTask, "buttons", 1024, nullptr, 3, nullptr);
  xTaskCreate(uiTask,      "ui",      1024, nullptr, 2, nullptr);
  xTaskCreate(radioTask,   "radio",   2048, nullptr, 2, nullptr);
  xTaskCreate(sensorTask,  "sensor",  1536, nullptr, 1, nullptr);
}

void loop() {
  Event e;
  if (xQueueReceive(qEvents, &e, portMAX_DELAY) == pdTRUE)   // sleeps until something happens
    handle(e);
}
