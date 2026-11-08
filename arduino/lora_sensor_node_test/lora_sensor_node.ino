// lora_sensor_node.ino - Team 7 LoRa sensor network node (Arduino / C++)
//
// Board : Adafruit Feather RP2040 RFM95 (Tools > Board > Raspberry Pi RP2040 Boards
//         > "Adafruit Feather RP2040 RFM")
// Sensor: Adafruit SHT41 on the STEMMA QT port (I2C 0x44)
// Libs  : "LoRa" by Sandeep Mistry, "Adafruit SHT4x", "Adafruit NeoPixel"
//
// Flash the SAME sketch on every board; only NODE_ID in config.h changes.
// Each node: reads its SHT41 every ~5 s and broadcasts it, listens the rest
// of the time, prints a table of every node it hears, and relays readings so
// A can reach C through B. A missing sensor sends simulated data ('*').

#include <SPI.h>
#include <Wire.h>
#include <LoRa.h>
#include <Adafruit_SHT4x.h>
#include <Adafruit_NeoPixel.h>
#include "config.h"
#include "sensornet.h"

#ifdef ARDUINO_ARCH_RP2040
#include <pico/unique_id.h>
#include <hardware/watchdog.h>
#endif

#define WATCHDOG_MS   8000   // reboot if loop() ever stops running for 8 s
#define TX_TIMEOUT_MS 3000   // a ~40-byte packet takes 60 ms at SF7, ~1.5 s at SF12

using sensornet::Reading;

// ---------------------------------------------------------------- hardware
#ifndef PIN_RFM_CS          // these come from the Feather RP2040 RFM board variant
#define PIN_RFM_CS   16
#define PIN_RFM_RST  17
#define PIN_RFM_DIO0 21
#endif
#ifndef PIN_NEOPIXEL
#define PIN_NEOPIXEL 4
#endif

Adafruit_SHT4x sht4;
Adafruit_NeoPixel pixel(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
bool haveSensor = false;

char myId[sensornet::ID_MAX + 1];

// ---------------------------------------------------------------- state
struct NodeInfo {
  bool used;
  char src[sensornet::ID_MAX + 1];
  char via[sensornet::ID_MAX + 1];
  float tempC, rh;
  char flag;
  int rssi;
  float snr;
  bool self;
  unsigned long at;
};
NodeInfo nodes[MAX_NODES];

struct PendingRelay {
  bool used;
  unsigned long due;
  uint8_t len;
  uint8_t buf[sensornet::MAX_PACKET];
};
PendingRelay relays[8];

sensornet::SeenCache<64> seen;
uint16_t seqNo;
unsigned long nextSendAt;
float simT, simH;
unsigned long txTimeouts = 0;   // how many times the radio got stuck and was reset

// ---------------------------------------------------------------- freeze tracker
// The sketch records which step it is on in a watchdog scratch register.
// Those registers survive a watchdog reboot, so after a freeze the next boot
// prints exactly where the board was stuck.
enum Step : uint32_t {
  ST_LOOP = 1, ST_RX_POLL, ST_RX_HANDLE, ST_RX_PRINT, ST_LED, ST_RELAY_SEND,
  ST_SENSOR, ST_TX_START, ST_TX_WAIT, ST_TX_RESTART, ST_TX_PRINT, ST_TABLE_PRINT
};
const char* stepName(uint32_t s) {
  switch (s) {
    case ST_LOOP:        return "top of loop()";
    case ST_RX_POLL:     return "polling radio for packets (LoRa.parsePacket)";
    case ST_RX_HANDLE:   return "decoding a received packet";
    case ST_RX_PRINT:    return "printing DATA line over USB serial";
    case ST_LED:         return "flashing the NeoPixel";
    case ST_RELAY_SEND:  return "sending a relay packet";
    case ST_SENSOR:      return "reading the SHT41 sensor (I2C)";
    case ST_TX_START:    return "starting a radio send (beginPacket/write/endPacket)";
    case ST_TX_WAIT:     return "waiting for radio TX done";
    case ST_TX_RESTART:  return "restarting the radio after a TX timeout";
    case ST_TX_PRINT:    return "printing TX line over USB serial";
    case ST_TABLE_PRINT: return "printing the node table over USB serial";
    default:             return "unknown";
  }
}
inline void step(uint32_t s) {
#ifdef ARDUINO_ARCH_RP2040
  watchdog_hw->scratch[0] = 0xB00C0000u | s;   // tag + step number
#endif
}

// ---------------------------------------------------------------- helpers
void flash(uint8_t r, uint8_t g, uint8_t b) {
  step(ST_LED);
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
  delay(25);
  pixel.setPixelColor(0, 0);
  pixel.show();
}

void makeNodeId() {
  if (strlen(NODE_ID) > 0) {
    sensornet::copyId(myId, NODE_ID);
    for (char* p = myId; *p; p++) if (*p == ',') *p = '_';
    return;
  }
#ifdef ARDUINO_ARCH_RP2040
  pico_unique_board_id_t uid;
  pico_get_unique_board_id(&uid);
  snprintf(myId, sizeof myId, "%02X%02X",
           uid.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 2], uid.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 1]);
#else
  snprintf(myId, sizeof myId, "N%03ld", random(1000));
#endif
}

uint32_t idHash() {
  uint32_t h = 2166136261u;  // FNV-1a
  for (const char* p = myId; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  return h;
}

float frand(float lo, float hi) { return lo + (hi - lo) * (random(10001) / 10000.0f); }

void scheduleNextSend() {
  nextSendAt = millis() + SEND_EVERY_MS + random(-SEND_JITTER_MS, SEND_JITTER_MS + 1);
}

NodeInfo* nodeSlot(const char* src) {
  NodeInfo* oldest = &nodes[0];
  for (int i = 0; i < MAX_NODES; i++) {
    if (nodes[i].used && strcmp(nodes[i].src, src) == 0) return &nodes[i];
  }
  for (int i = 0; i < MAX_NODES; i++) {
    if (!nodes[i].used) return &nodes[i];
    if (!nodes[i].self && nodes[i].at < oldest->at) oldest = &nodes[i];
  }
  return oldest;  // table full: reuse the least recently heard node
}

void remember(const Reading& r, int rssi, float snr, bool self) {
  NodeInfo* n = nodeSlot(r.src);
  n->used = true;
  sensornet::copyId(n->src, r.src);
  sensornet::copyId(n->via, r.via);
  n->tempC = r.tempC; n->rh = r.rh; n->flag = r.flag;
  n->rssi = rssi; n->snr = snr; n->self = self;
  n->at = millis();
}

bool readSensor(float& t, float& h) {
  if (haveSensor) {
    sensors_event_t hum, temp;
    if (sht4.getEvent(&hum, &temp)) {
      t = temp.temperature;
      h = hum.relative_humidity;
      return true;
    }
    Serial.println("SHT41 read failed -> simulated value");
  }
  simT += frand(-0.2f, 0.2f);
  simH += frand(-0.5f, 0.5f);
  if (simH < 0) simH = 0;
  if (simH > 100) simH = 100;
  t = simT; h = simH;
  return false;
}

void feedWatchdog() {
#ifdef ARDUINO_ARCH_RP2040
  rp2040.wdt_reset();
#endif
}

// Direct access to two RFM95 registers, so we can wait for "TX done" WITH a
// timeout. (The LoRa library's blocking endPacket() waits forever, so a radio
// glitch during a send would freeze the whole board.)
const uint8_t REG_IRQ_FLAGS = 0x12, IRQ_TX_DONE = 0x08;

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
  LoRa.enableCrc();   // drop corrupted packets
  return true;
}

void radioSend(const uint8_t* buf, int len) {
  step(ST_TX_START);
  LoRa.beginPacket();
  LoRa.write(buf, len);
  LoRa.endPacket(true);                      // start sending, don't block
  unsigned long t0 = millis();
  step(ST_TX_WAIT);
  while ((rfmRead(REG_IRQ_FLAGS) & IRQ_TX_DONE) == 0) {
    if (millis() - t0 > TX_TIMEOUT_MS) {     // radio stuck: reset it and carry on
      txTimeouts++;
      Serial.printf("!! TX timeout #%lu - resetting radio (check antenna joint / power)\n", txTimeouts);
      flash(255, 0, 255);                    // purple = radio had to be reset
      step(ST_TX_RESTART);
      LoRa.end();
      if (!radioStart()) {
        Serial.println("!! radio restart FAILED - rebooting via watchdog");
        while (true) {}                      // stop feeding the watchdog -> clean reboot
      }
      return;
    }
    feedWatchdog();
    delay(1);
  }
  rfmWrite(REG_IRQ_FLAGS, IRQ_TX_DONE);      // clear the flag
}

void printTable() {
  step(ST_TABLE_PRINT);
  unsigned long now = millis();
  char t[12], h[12], route[16];
  Serial.println(F("----------------------------------------------------------------"));
  Serial.println(F("NODE      TEMP_C    RH_%    RSSI   SNR   ROUTE        AGE"));
  for (int i = 0; i < MAX_NODES; i++) {
    NodeInfo& n = nodes[i];
    if (!n.used) continue;
    unsigned long age = (now - n.at) / 1000;
    sensornet::fmt2(t, sizeof t, n.tempC);
    sensornet::fmt2(h, sizeof h, n.rh);
    if (n.self) snprintf(route, sizeof route, "self");
    else if (strcmp(n.via, n.src) == 0) snprintf(route, sizeof route, "direct");
    else snprintf(route, sizeof route, "via %s", n.via);
    char line[100];
    if (n.self) {
      snprintf(line, sizeof line, "%-8s %7s%c %6s     -      -   %-11s %4lus",
               n.src, t, n.flag == 'S' ? '*' : ' ', h, route, age);
    } else {
      char snr[12];
      sensornet::fmt2(snr, sizeof snr, n.snr);
      snprintf(line, sizeof line, "%-8s %7s%c %6s  %5d %6s  %-11s %4lus%s",
               n.src, t, n.flag == 'S' ? '*' : ' ', h, n.rssi, snr, route, age,
               (now - n.at) > STALE_AFTER_MS ? " STALE" : "");
    }
    Serial.println(line);
  }
  Serial.println(F("(* = simulated sensor)"));
  Serial.println(F("----------------------------------------------------------------"));
}

void sendOwnReading() {
  Reading r;
  sensornet::copyId(r.src, myId);
  sensornet::copyId(r.via, myId);
  r.seq = seqNo++;
  r.ttl = MAX_HOPS;
  step(ST_SENSOR);
  bool real = readSensor(r.tempC, r.rh);
  r.flag = real ? 'R' : 'S';

  uint8_t buf[sensornet::MAX_PACKET];
  int len = sensornet::encode(r, buf, sizeof buf);
  if (len == 0) return;
  seen.add(r.src, r.seq);  // ignore our own echoes coming back via relays
  radioSend(buf, len);
  remember(r, 0, 0, true);

  char t[12], h[12];
  sensornet::fmt2(t, sizeof t, r.tempC);
  sensornet::fmt2(h, sizeof h, r.rh);
  step(ST_TX_PRINT);
  Serial.printf("TX %s seq=%u  %s C  %s %%RH  [%c]\n", myId, (unsigned)r.seq, t, h, r.flag);
  if (real) flash(0, 0, 255); else flash(255, 80, 0);
}

void handlePacket(const uint8_t* buf, int len, int rssi, float snr) {
  Reading r;
  step(ST_RX_HANDLE);
  if (!sensornet::decode(buf, len, r)) return;          // not ours / garbled
  if (strcmp(r.src, myId) == 0) return;                  // our own reading, relayed back
  if (!seen.checkAndAdd(r.src, r.seq)) return;           // duplicate
  remember(r, rssi, snr, false);

  // Machine-readable line for laptop/serial_logger.py
  char t[12], h[12], s[12];
  sensornet::fmt2(t, sizeof t, r.tempC);
  sensornet::fmt2(h, sizeof h, r.rh);
  sensornet::fmt2(s, sizeof s, snr);
  step(ST_RX_PRINT);
  Serial.printf("DATA,%s,%u,%s,%s,%c,%d,%s,%s,%s\n",
                r.src, (unsigned)r.seq, t, h, r.flag, rssi, s, r.via, myId);
  flash(0, 255, 0);

  if (RELAY_ENABLED && r.ttl > 0) {
    Reading c = r;
    c.ttl = r.ttl - 1;
    sensornet::copyId(c.via, myId);
    for (int i = 0; i < 8; i++) {
      if (!relays[i].used) {
        int n = sensornet::encode(c, relays[i].buf, sizeof relays[i].buf);
        if (n == 0) break;
        relays[i].len = n;
        relays[i].due = millis() + random(50, 400);   // random wait avoids collisions
        relays[i].used = true;
        break;
      }
    }
  }
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);   // don't hang on battery

  pixel.begin();
  pixel.setBrightness(40);
  makeNodeId();

  // Random seed: chip/name hash plus ADC noise, so seq numbers differ after reboots
  uint32_t seed = idHash() ^ micros();
  for (int i = 0; i < 16; i++) seed = (seed << 1) ^ (analogRead(A0) & 1) ^ (analogRead(A1) & 1);
  randomSeed(seed);
  seqNo = (uint16_t)random(65536);
  simT = 21.0f + frand(-1, 1);
  simH = 45.0f + frand(-5, 5);

  // Sensor (STEMMA QT = Wire on GP2/GP3 for this board)
  Wire.begin();
  if (sht4.begin(&Wire)) {
    sht4.setPrecision(SHT4X_HIGH_PRECISION);
    sht4.setHeater(SHT4X_NO_HEATER);
    haveSensor = true;
    Serial.printf("SHT41 found, serial 0x%08lX\n", (unsigned long)sht4.readSerial());
  } else {
    Serial.println("SHT41 not found -> sending SIMULATED data");
  }

  // Radio (RFM95 is on the board's default SPI: GP8/14/15)
  if (!radioStart()) {
    Serial.println("LoRa init FAILED - check board selection. Halting.");
    while (true) { flash(255, 0, 0); delay(500); }
  }

  Serial.println(F("================================================================"));
  Serial.printf("Team 7 LoRa sensor node %s | %ld MHz SF%d %d dBm relay=%s\n",
                myId, (long)(LORA_FREQ_HZ / 1e6), LORA_SF, LORA_TX_DBM,
                RELAY_ENABLED ? "on" : "off");
  Serial.println(F("================================================================"));

#ifdef ARDUINO_ARCH_RP2040
  if (watchdog_enable_caused_reboot()) {
    uint32_t crumb = watchdog_hw->scratch[0];
    uint32_t count = watchdog_hw->scratch[1] + 1;
    watchdog_hw->scratch[1] = count;
    Serial.println("!! Last reset was the WATCHDOG - the board had frozen and rebooted itself");
    if ((crumb & 0xFFFF0000u) == 0xB00C0000u)
      Serial.printf("!! Froze at: %s  (step %lu, freeze #%lu since power-up)\n",
                    stepName(crumb & 0xFFFF), (unsigned long)(crumb & 0xFFFF), (unsigned long)count);
  } else {
    watchdog_hw->scratch[1] = 0;   // normal power-up / reset button: clear the counter
  }
  watchdog_hw->scratch[0] = 0;
  rp2040.wdt_begin(WATCHDOG_MS);
#endif

  nextSendAt = millis() + random(500, 2000);
}

void loop() {
  feedWatchdog();
  step(ST_LOOP);

  // 1. Receive (polling - no interrupts needed)
  step(ST_RX_POLL);
  int size = LoRa.parsePacket();
  if (size > 0) {
    uint8_t buf[sensornet::MAX_PACKET];
    int len = 0;
    while (LoRa.available()) {
      int b = LoRa.read();
      if (len < (int)sizeof buf) buf[len++] = (uint8_t)b;
    }
    if (size <= (int)sizeof buf) handlePacket(buf, len, LoRa.packetRssi(), LoRa.packetSnr());
  }

  unsigned long now = millis();

  // 2. Forward any relays that are due
  for (int i = 0; i < 8; i++) {
    if (relays[i].used && (long)(now - relays[i].due) >= 0) {
      step(ST_RELAY_SEND);
      radioSend(relays[i].buf, relays[i].len);
      relays[i].used = false;
    }
  }

  // 3. Our own reading
  if ((long)(now - nextSendAt) >= 0) {
    sendOwnReading();
    printTable();
    scheduleNextSend();
  }
}
