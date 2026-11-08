// display_esp32.ino - messaging screen + phone page for a Team 7 box, on the Adafruit ESP32-S3
// Reverse TFT Feather (v1.2). It draws what the box's main board sends over I2C (see rtos_main/displink.h).
// The ESP32 is an I2C *device* at address 0x42; the main board (RP2040 / M0) is the controller.
//
// PHONE: the ESP32 also makes a Wi-Fi network "Team7-<box name>" (open, no password). Join it with a
// phone and the chat page opens by itself (or browse to http://192.168.4.1). From the phone you can
// read the box's messages, send texts and press HELP / OK - exactly like the box's own buttons.
// The main board fetches what the phone sent by polling this board over I2C (see "phone outbox" below).
//
// Arduino IDE: Boards Manager "esp32" by Espressif -> board "Adafruit Feather ESP32-S3 Reverse TFT"
// Library:     "Adafruit ST7735 and ST7789 Library" (click Install all -> also installs Adafruit GFX)
//              WiFi / WebServer / DNSServer come with the esp32 board package - nothing to install.
// Wiring:      RP2040 SDA -- ESP32 SDA (GPIO 3), RP2040 SCL -- ESP32 SCL (GPIO 4), GND -- GND
//              (STEMMA QT to STEMMA QT works). Power: USB pin <-> USB pin, NOT 3V <-> 3V.
// Debug:       open this board's Serial Monitor (115200) - every received line is printed there.
//
// One screen:
//   header      box name, mode (colour), temperature
//   newest      sender + age (+ confirmation status for our own messages), text on 3 lines
//   previous    one line
//   bottom      boxes heard with signal bars - or a red warning banner (e.g. DEPLOY A RELAY)
// A long incoming message fills the screen for 20 s; a HELP from another box shows a red alert.

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

#define I2C_ADDR 0x42                    // must match DISPLAY_I2C_ADDR in rtos_main/displink.h
#define PHONE_WIFI   1                   // 0 = no Wi-Fi / phone page
#define WIFI_PASS    ""                  // "" = open network; or 8+ characters for a password
#define WIFI_POWER   WIFI_POWER_8_5dBm   // low power: ~10-20 m range, and less current from the box's 3.3 V
#define PHONE_READ   80                  // bytes per poll - must match PHONE_READ in rtos_main.ino

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 canvas(240, 135);            // draw here, then copy in one go: no flicker

// ---------------------------------------------------------------- colours (RGB565)
const uint16_t C_BG = 0x0000, C_TEXT = 0xFFFF, C_DIM = 0x8C71, C_LABEL = 0x07FF;
const uint16_t C_GREEN = 0x07E0, C_YELLOW = 0xFFE0, C_RED = 0xF800, C_ORANGE = 0xFD20, C_BLUE = 0x341F;

// ---------------------------------------------------------------- what we know
char myId[9] = "?", mode[8] = "", temp[8] = "--";

struct Msg {
  bool used, mine;
  char from[9];
  char text[64];         // max 60 characters
  int rssi;
  uint32_t t;
  uint16_t seq;          // our own messages: sequence number
  int8_t ack;            // our own messages: 0 waiting, 1 confirmed, -1 nobody confirmed
  char by[9];            // first box that confirmed
  uint8_t nack;          // how many boxes confirmed
};
Msg msgs[4];             // newest first

struct Node { bool used; char id[9]; int rssi; uint32_t t; };
Node nodes[8];

// Longer history for the phone page (the screen only needs the newest few)
struct PMsg { uint32_t id, t; bool mine; char from[9]; char text[64]; uint16_t seq; int8_t ack; char by[9]; uint8_t nack; };
PMsg plog[24];
uint32_t plogNext = 1;
char rh[8] = "--";

uint32_t fullMsgUntil = 0, alertUntil = 0, lastLineAt = 0;
char alertFrom[9] = "", warnText[40] = "";
bool dirty = true;

// ---------------------------------------------------------------- helpers
const int COLS = 20;                     // text size 2: 12 px per character -> 20 per line

void ago(char* out, int max, uint32_t t) {
  uint32_t a = (millis() - t) / 1000;
  if (a < 60) snprintf(out, max, "%lus", (unsigned long)a);
  else if (a < 3600) snprintf(out, max, "%lum", (unsigned long)(a / 60));
  else snprintf(out, max, "%luh", (unsigned long)(a / 3600));
}

int wrap(const char* text, char lines[][COLS + 1], int maxLines) {    // word-wrap; '~' marks a cut
  int n = 0;
  const char* p = text;
  while (*p && n < maxLines) {
    while (*p == ' ') p++;
    int len = strlen(p), take = len <= COLS ? len : COLS;
    if (len > COLS) for (int i = COLS; i > 0; i--) if (p[i] == ' ') { take = i; break; }
    memcpy(lines[n], p, take); lines[n][take] = 0;
    p += take; n++;
  }
  while (*p == ' ') p++;
  if (*p && n > 0) { int l = strlen(lines[n - 1]); if (l >= COLS) l = COLS - 1; lines[n - 1][l] = '~'; lines[n - 1][l + 1] = 0; }
  return n;
}
int wrappedLines(const char* text) { char tmp[8][COLS + 1]; return wrap(text, tmp, 8); }

void text(int x, int y, const char* s, uint16_t color, int size = 2) {
  canvas.setTextSize(size); canvas.setTextColor(color); canvas.setCursor(x, y); canvas.print(s);
}

int barsFor(int rssi) { int m = rssi + 123; return m > 45 ? 4 : m > 30 ? 3 : m > 15 ? 2 : m > 5 ? 1 : 0; }
void signalBars(int x, int yBottom, int rssi) {
  int b = barsFor(rssi);
  uint16_t col = b >= 3 ? C_GREEN : b == 2 ? C_YELLOW : C_RED;
  for (int i = 0; i < 4; i++) {
    int h = 4 + i * 3;
    if (i < b) canvas.fillRect(x + i * 5, yBottom - h, 4, h, col);
    else canvas.drawRect(x + i * 5, yBottom - h, 4, h, C_DIM);
  }
}

uint16_t modeColor() {
  if (!strcmp(mode, "BASE")) return C_BLUE;
  if (!strcmp(mode, "DEPLOY")) return C_GREEN;
  if (!strcmp(mode, "CARRY")) return C_ORANGE;
  return C_DIM;
}

// Confirmation status of one of our own messages, drawn right-aligned on line y.
void ackStatus(const Msg& m, int y) {
  char s[24];
  uint16_t col;
  if (m.ack > 0) {                                                        // \xFB = check mark
    if (m.nack > 1) snprintf(s, sizeof s, "\xFB%.5s+%d", m.by, m.nack - 1);  // e.g. "√BASE+1"
    else snprintf(s, sizeof s, "\xFB%s", m.by);
    col = C_GREEN;
  }
  else if (m.ack < 0) { strcpy(s, "no reply"); col = C_RED; }
  else                { strcpy(s, "..."); col = C_YELLOW; }
  s[10] = 0;                                                              // keep it short
  text(240 - 12 * strlen(s), y, s, col);
}

// ---------------------------------------------------------------- drawing
void drawMain() {
  char buf[48], a[8];
  canvas.fillRect(0, 0, 240, 18, modeColor());
  snprintf(buf, sizeof buf, "%s %s", myId, mode);
  text(3, 1, buf, C_BG);
  if (strcmp(temp, "--")) { snprintf(buf, sizeof buf, "%sC", temp); text(240 - 3 - 12 * strlen(buf), 1, buf, C_BG); }

  if (!msgs[0].used) {
    text(0, 40, "No messages yet", C_DIM);
  } else {
    const Msg& m = msgs[0];
    ago(a, sizeof a, m.t);
    text(0, 21, m.from, m.mine ? C_ORANGE : C_LABEL);
    text(12 * (strlen(m.from) + 1), 21, a, C_DIM);
    if (m.mine) ackStatus(m, 21);
    char lines[3][COLS + 1];
    int n = wrap(m.text, lines, 3);
    for (int i = 0; i < n; i++) text(0, 38 + 16 * i, lines[i], C_TEXT);
    if (msgs[1].used && !warnText[0]) {                                   // the warning banner needs this row
      const Msg& o = msgs[1];
      snprintf(buf, sizeof buf, "%s: %s", o.from, o.text);
      buf[o.mine ? 10 : COLS] = 0;                                        // room for the status
      text(0, 88, buf, C_DIM);
      if (o.mine) ackStatus(o, 88);
    }
  }

  if (warnText[0]) {                       // warning replaces the strip: 1-2 lines, '|' = line break
    bool blink = (millis() / 400) % 2;
    canvas.fillRect(0, 102, 240, 33, blink ? C_RED : 0x8000);
    char l1[COLS + 1] = "", l2[COLS + 1] = "";
    const char* bar = strchr(warnText, '|');
    if (bar) { snprintf(l1, sizeof l1, "%.*s", (int)(bar - warnText), warnText); snprintf(l2, sizeof l2, "%s", bar + 1); }
    else snprintf(l1, sizeof l1, "%s", warnText);
    if (l2[0]) { text((240 - 12 * strlen(l1)) / 2, 103, l1, C_TEXT); text((240 - 12 * strlen(l2)) / 2, 119, l2, C_TEXT); }
    else text((240 - 12 * strlen(l1)) / 2, 111, l1, C_TEXT);
    return;
  }
  canvas.drawFastHLine(0, 107, 240, C_DIM);
  int shown = 0;
  for (int i = 0; i < 8 && shown < 3; i++) {
    if (!nodes[i].used) continue;
    char name[5]; strncpy(name, nodes[i].id, 4); name[4] = 0;
    bool stale = millis() - nodes[i].t > 75000;
    text(80 * shown, 114, name, stale ? C_DIM : C_TEXT);
    signalBars(80 * shown + 12 * strlen(name) + 3, 129, stale ? -200 : nodes[i].rssi);
    shown++;
  }
  if (!shown) text(0, 114, "no boxes heard", C_DIM);
}

void drawFullMessage() {
  char buf[48], a[8];
  ago(a, sizeof a, msgs[0].t);
  canvas.fillRect(0, 0, 240, 18, C_LABEL);
  snprintf(buf, sizeof buf, "%s  %s", msgs[0].from, a);
  text(3, 1, buf, C_BG);
  char lines[7][COLS + 1];
  int n = wrap(msgs[0].text, lines, 7);
  for (int i = 0; i < n; i++) text(0, 21 + 16 * i, lines[i], C_TEXT);
}

void drawAlert() {
  char a[8], buf[32];
  canvas.fillScreen((millis() / 500) % 2 ? C_RED : 0x8000);
  text(72, 12, "HELP", C_TEXT, 4);
  snprintf(buf, sizeof buf, "from %s", alertFrom);
  text((240 - 12 * strlen(buf)) / 2, 60, buf, C_TEXT);
  ago(a, sizeof a, msgs[0].t);
  snprintf(buf, sizeof buf, "%s ago", a);
  text((240 - 12 * strlen(buf)) / 2, 84, buf, C_TEXT);
  if (msgs[0].rssi) { snprintf(buf, sizeof buf, "%d dBm", msgs[0].rssi); text((240 - 12 * strlen(buf)) / 2, 108, buf, C_TEXT); }
}

void draw() {
  canvas.fillScreen(C_BG);
  uint32_t now = millis();
  if (alertUntil && now < alertUntil) drawAlert();
  else if (fullMsgUntil && now < fullMsgUntil) drawFullMessage();
  else drawMain();
  if (lastLineAt == 0 || now - lastLineAt > 15000) {          // nothing from the radio box (it sends every 5 s)
    canvas.fillRect(0, 117, 240, 18, C_RED);
    const char* m = lastLineAt ? "NO LINK TO BOX" : "WAITING FOR BOX";
    text((240 - 12 * strlen(m)) / 2, 118, m, C_TEXT);
  }
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 240, 135);
  dirty = false;
}

// ---------------------------------------------------------------- parsing the lines
int fields(char* s, char** f, int n) {      // split at the first n commas; the last field keeps the rest
  int k = 0;
  f[k++] = s;
  for (char* p = s; *p && k <= n; p++) if (*p == ',') { *p = 0; f[k++] = p + 1; }
  return k;
}

PMsg& newPMsg() {                          // overwrite the oldest
  PMsg* o = &plog[0];
  for (auto& p : plog) if (p.id < o->id) o = &p;
  memset(o, 0, sizeof *o);
  o->id = plogNext++; o->t = millis();
  return *o;
}

Msg& newMsg() {
  for (int i = 3; i > 0; i--) msgs[i] = msgs[i - 1];
  memset(&msgs[0], 0, sizeof msgs[0]);
  msgs[0].used = true;
  msgs[0].t = millis();
  return msgs[0];
}

void handleLine(char* line) {
  char* f[7];
  if (strncmp(line, "D,", 2)) return;
  lastLineAt = millis();
  char* body = line + 2;
  if (!strncmp(body, "STATUS,", 7)) {
    if (fields(body, f, 5) < 6) return;                         // STATUS,id,mode,temp,rh,flag
    strncpy(myId, f[1], 8); strncpy(mode, f[2], 7);
    if (strcmp(f[3], "-")) strncpy(temp, f[3], 7);
    if (strcmp(f[4], "-")) strncpy(rh, f[4], 7);
  } else if (!strncmp(body, "MSG,", 4)) {
    if (fields(body, f, 3) < 4) return;                         // MSG,from,rssi,text
    Msg& m = newMsg();
    strncpy(m.from, f[1], 8); m.rssi = atoi(f[2]); strncpy(m.text, f[3], sizeof m.text - 1);
    PMsg& p = newPMsg(); strcpy(p.from, m.from); strcpy(p.text, m.text);
    if (!strcmp(m.text, "[HELP]")) { strncpy(alertFrom, m.from, 8); alertUntil = millis() + 20000; }
    else { alertUntil = 0; fullMsgUntil = wrappedLines(m.text) > 3 ? millis() + 20000 : 0; }
  } else if (!strncmp(body, "SENT,", 5)) {
    if (fields(body, f, 2) < 3) return;                         // SENT,seq,text
    Msg& m = newMsg();
    m.mine = true; strcpy(m.from, "YOU"); m.seq = atoi(f[1]); strncpy(m.text, f[2], sizeof m.text - 1);
    PMsg& p = newPMsg(); p.mine = true; strcpy(p.from, "YOU"); p.seq = m.seq; strcpy(p.text, m.text);
    fullMsgUntil = alertUntil = 0;
  } else if (!strncmp(body, "ACK,", 4)) {
    if (fields(body, f, 2) < 3) return;                         // ACK,seq,by
    uint16_t seq = atoi(f[1]);
    for (auto& p : plog) {                                       // phone history
      if (!p.id || !p.mine || p.seq != seq) continue;
      if (!strcmp(f[2], "-")) { if (p.ack == 0) p.ack = -1; }
      else { if (p.ack <= 0) { p.ack = 1; p.nack = 0; strncpy(p.by, f[2], 8); p.by[8] = 0; } p.nack++; }
      break;
    }
    for (auto& m : msgs) {
      if (!m.used || !m.mine || m.seq != seq) continue;
      if (!strcmp(f[2], "-")) { if (m.ack == 0) m.ack = -1; }
      else {
        if (m.ack <= 0) { m.ack = 1; m.nack = 0; strncpy(m.by, f[2], 8); m.by[8] = 0; }
        m.nack++;
      }
      break;
    }
  } else if (!strncmp(body, "WARN,", 5)) {
    strncpy(warnText, body + 5, sizeof warnText - 1); warnText[sizeof warnText - 1] = 0;
  } else if (!strncmp(body, "NODE,", 5)) {
    if (fields(body, f, 2) < 3) return;                         // NODE,id,rssi
    int slot = -1, oldest = 0;
    for (int i = 0; i < 8; i++) {
      if (nodes[i].used && !strcmp(nodes[i].id, f[1])) { slot = i; break; }
      if (!nodes[i].used && slot < 0) slot = i;
      if (nodes[i].t < nodes[oldest].t) oldest = i;
    }
    if (slot < 0) slot = oldest;
    nodes[slot].used = true; strncpy(nodes[slot].id, f[1], 8); nodes[slot].id[8] = 0;
    nodes[slot].rssi = atoi(f[2]); nodes[slot].t = millis();
  } else {
    return;
  }
  dirty = true;
}

// ---------------------------------------------------------------- I2C receive
// The RP2040 sends each line as 0x02 + text + '\n' in 30-byte pieces. onReceive() runs in the
// I2C driver's task, so it only reassembles lines and hands complete ones to loop() via a queue.
struct Line { char s[128]; };
QueueHandle_t lineQueue;

// ---- phone outbox: lines for the main board ("TX,<dst>,<text>" or "QM,<dst>,<code>").
// Each poll (main board reads PHONE_READ bytes) returns [id][len][text][zero padding]; id 0 = nothing.
// The main board then writes 0x06,<id> ("got it") and only then do we move to the next line. So a line is
// never lost, and the main board ignores an id it has already handled - it can't be sent twice either.
struct Out { uint8_t id; char s[PHONE_READ - 2]; };
QueueHandle_t outQueue;
Out outCur = {0, ""};                      // only touched in the I2C callbacks (same driver task)
uint8_t outNextId = 1;
volatile int outWaiting = 0;               // lines not yet taken by the main board (for the phone page)

void onI2CRequest() {
  if (!outCur.id) {
    Out o;
    if (xQueueReceive(outQueue, &o, 0) == pdTRUE) outCur = o;
  }
  uint8_t b[PHONE_READ];
  memset(b, 0, sizeof b);
  if (outCur.id) {
    size_t n = strlen(outCur.s);
    if (n > PHONE_READ - 2) n = PHONE_READ - 2;
    b[0] = outCur.id; b[1] = (uint8_t)n; memcpy(b + 2, outCur.s, n);
  }
  Wire.write(b, sizeof b);                 // always exactly PHONE_READ bytes
}

void onI2CReceive(int len) {
  static char buf[128];
  static int n = 0;
  if (Wire.peek() == 0x06 && len == 2) {                        // main board: "got line <id>"
    Wire.read();
    int id = Wire.read();
    if (outCur.id && id == outCur.id) { outCur.id = 0; if (outWaiting > 0) outWaiting--; }
    return;
  }
  while (Wire.available()) {
    int c = Wire.read();
    if (c == 0x02) { n = 0; continue; }                         // start of a new line
    if (c == '\n') {
      if (n) { Line l; memcpy(l.s, buf, n); l.s[n] = 0; xQueueSend(lineQueue, &l, 0); }
      n = 0;
    } else if (n < (int)sizeof buf - 1) buf[n++] = (char)c;
  }
}

// ---------------------------------------------------------------- phone: Wi-Fi + web page
#if PHONE_WIFI
#include "phone_page.h"                   // the HTML page (PHONE_PAGE)
WebServer web(80);
DNSServer dns;
bool wifiOn = false;

bool phoneQueue(const char* line) {
  Out o; memset(&o, 0, sizeof o);
  o.id = outNextId; outNextId = outNextId >= 254 ? 1 : outNextId + 1;   // 1..254
  strncpy(o.s, line, sizeof o.s - 1);
  if (xQueueSend(outQueue, &o, 0) != pdTRUE) return false;
  outWaiting++;
  Serial.printf("phone: %s\n", line);
  return true;
}

void jsonStr(String& j, const char* s) {     // add "s" with quotes escaped
  j += '"';
  for (; *s; s++) { if (*s == '"' || *s == '\\') j += '\\'; j += *s; }
  j += '"';
}

void webState() {
  String j; j.reserve(3500);
  j += "{\"id\":"; jsonStr(j, myId);
  j += ",\"mode\":"; jsonStr(j, mode);
  j += ",\"temp\":"; jsonStr(j, temp);
  j += ",\"rh\":"; jsonStr(j, rh);
  j += ",\"warn\":"; jsonStr(j, warnText);
  j += ",\"link\":"; j += (lastLineAt && millis() - lastLineAt < 15000) ? "true" : "false";
  j += ",\"waiting\":"; j += (int)outWaiting;
  j += ",\"boxes\":[";
  bool first = true;
  for (auto& n : nodes) {
    if (!n.used) continue;
    if (!first) j += ','; first = false;
    j += "{\"id\":"; jsonStr(j, n.id);
    j += ",\"rssi\":"; j += n.rssi;
    j += ",\"age\":"; j += (millis() - n.t) / 1000; j += '}';
  }
  j += "],\"msgs\":[";
  first = true;
  for (uint32_t id = plogNext > 24 ? plogNext - 24 : 1; id < plogNext; id++) {     // oldest first
    for (auto& p : plog) {
      if (p.id != id) continue;
      if (!first) j += ','; first = false;
      j += "{\"mine\":"; j += p.mine ? "true" : "false";
      j += ",\"from\":"; jsonStr(j, p.from);
      j += ",\"text\":"; jsonStr(j, p.text);
      j += ",\"age\":"; j += (millis() - p.t) / 1000;
      j += ",\"ack\":"; j += (int)p.ack;
      j += ",\"by\":"; jsonStr(j, p.by);
      j += ",\"n\":"; j += (int)p.nack; j += '}';
    }
  }
  j += "]}";
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", j);
}

void webSend() {                               // POST to=<id or *>&text=<text>
  String to = web.arg("to"), t = web.arg("text");
  char clean[61]; int n = 0;
  size_t i = 0;
  while (i < t.length() && t[i] == ' ') i++;                    // trim leading spaces
  for (; i < t.length() && n < 60; i++) { char c = t[i]; if (c >= 32 && c <= 126) clean[n++] = c; }
  while (n && clean[n - 1] == ' ') n--;
  clean[n] = 0;
  if (!n) { web.send(400, "text/plain", "empty"); return; }
  if (to.length() == 0 || to.length() > 8 || to.indexOf(',') >= 0) to = "*";
  char line[PHONE_READ];
  snprintf(line, sizeof line, "TX,%s,%s", to.c_str(), clean);
  web.send(phoneQueue(line) ? 200 : 503, "text/plain", "ok");
}

void webQuick() {                              // POST code=HELP|OK
  String c = web.arg("code");
  if (c != "HELP" && c != "OK") { web.send(400, "text/plain", "bad code"); return; }
  char line[16]; snprintf(line, sizeof line, "QM,*,%s", c.c_str());
  web.send(phoneQueue(line) ? 200 : 503, "text/plain", "ok");
}

void webRedirect() {                           // any other address -> our page (makes the phone open it)
  web.sendHeader("Location", "http://192.168.4.1/", true);
  web.send(302, "text/plain", "");
}

void wifiStart() {
  char ssid[24]; snprintf(ssid, sizeof ssid, "Team7-%s", myId);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, strlen(WIFI_PASS) >= 8 ? WIFI_PASS : nullptr);
  WiFi.setTxPower(WIFI_POWER);
  dns.start(53, "*", WiFi.softAPIP());         // every name -> us (captive portal)
  web.on("/", HTTP_GET, [] { web.sendHeader("Cache-Control", "no-store"); web.send(200, "text/html", PHONE_PAGE); });
  web.on("/api/state", HTTP_GET, webState);
  web.on("/api/send", HTTP_POST, webSend);
  web.on("/api/quick", HTTP_POST, webQuick);
  web.onNotFound(webRedirect);
  web.begin();
  wifiOn = true;
  Serial.printf("Wi-Fi \"%s\" up - phone page at http://%s\n", ssid, WiFi.softAPIP().toString().c_str());
}
#endif

// ---------------------------------------------------------------- setup / loop
void setup() {
  Serial.begin(115200);                                          // USB, for debugging
  pinMode(TFT_BACKLITE, OUTPUT);  digitalWrite(TFT_BACKLITE, HIGH);
  pinMode(TFT_I2C_POWER, OUTPUT); digitalWrite(TFT_I2C_POWER, HIGH);   // also powers the I2C pull-ups
  delay(10);
  tft.init(135, 240);
  tft.setRotation(3);
  canvas.setTextWrap(false);
  canvas.cp437(true);                                            // so \xFB draws the check mark
  draw();
  lineQueue = xQueueCreate(8, sizeof(Line));
  outQueue = xQueueCreate(4, sizeof(Out));
  outNextId = 1 + esp_random() % 254;                            // random start: never repeats an id after a restart
  Wire.onReceive(onI2CReceive);
  Wire.onRequest(onI2CRequest);
  Wire.begin((uint8_t)I2C_ADDR);                                 // I2C device on SDA 3 / SCL 4
  Serial.printf("Team 7 display v1.2 ready - I2C device at 0x%02X\n", I2C_ADDR);
}

void loop() {
  Line l;
  while (xQueueReceive(lineQueue, &l, 0) == pdTRUE) {
    Serial.printf("rx: %s\n", l.s);                              // debug: everything that arrives
    handleLine(l.s);
  }
#if PHONE_WIFI
  if (!wifiOn && strcmp(myId, "?")) wifiStart();               // start once we know the box's name
  if (wifiOn) { dns.processNextRequest(); web.handleClient(); }
#endif
  static uint32_t lastDraw = 0;
  bool animating = (alertUntil && millis() < alertUntil) || warnText[0];   // blinking screens
  if (dirty || millis() - lastDraw > (animating ? 250u : 1000u)) { draw(); lastDraw = millis(); }
  delay(2);
}
