// display_esp32.ino - messaging screen for a Team 7 box, on the Adafruit ESP32-S3 Reverse TFT Feather (v1.1).
// It draws what the box's Feather RP2040 sends over I2C (see rtos_main/displink.h).
// The ESP32 is an I2C *device* at address 0x42; the RP2040 is the controller.
//
// Arduino IDE: Boards Manager "esp32" by Espressif -> board "Adafruit Feather ESP32-S3 Reverse TFT"
// Library:     "Adafruit ST7735 and ST7789 Library" (click Install all -> also installs Adafruit GFX)
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

#define I2C_ADDR 0x42                    // must match DISPLAY_I2C_ADDR in rtos_main/displink.h

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
  } else if (!strncmp(body, "MSG,", 4)) {
    if (fields(body, f, 3) < 4) return;                         // MSG,from,rssi,text
    Msg& m = newMsg();
    strncpy(m.from, f[1], 8); m.rssi = atoi(f[2]); strncpy(m.text, f[3], sizeof m.text - 1);
    if (!strcmp(m.text, "[HELP]")) { strncpy(alertFrom, m.from, 8); alertUntil = millis() + 20000; }
    else { alertUntil = 0; fullMsgUntil = wrappedLines(m.text) > 3 ? millis() + 20000 : 0; }
  } else if (!strncmp(body, "SENT,", 5)) {
    if (fields(body, f, 2) < 3) return;                         // SENT,seq,text
    Msg& m = newMsg();
    m.mine = true; strcpy(m.from, "YOU"); m.seq = atoi(f[1]); strncpy(m.text, f[2], sizeof m.text - 1);
    fullMsgUntil = alertUntil = 0;
  } else if (!strncmp(body, "ACK,", 4)) {
    if (fields(body, f, 2) < 3) return;                         // ACK,seq,by
    uint16_t seq = atoi(f[1]);
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

void onI2CReceive(int len) {
  static char buf[128];
  static int n = 0;
  while (Wire.available()) {
    int c = Wire.read();
    if (c == 0x02) { n = 0; continue; }                         // start of a new line
    if (c == '\n') {
      if (n) { Line l; memcpy(l.s, buf, n); l.s[n] = 0; xQueueSend(lineQueue, &l, 0); }
      n = 0;
    } else if (n < (int)sizeof buf - 1) buf[n++] = (char)c;
  }
}

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
  Wire.onReceive(onI2CReceive);
  Wire.begin((uint8_t)I2C_ADDR);                                 // I2C device on SDA 3 / SCL 4
  Serial.printf("Team 7 display v1.1 ready - I2C device at 0x%02X\n", I2C_ADDR);
}

void loop() {
  Line l;
  while (xQueueReceive(lineQueue, &l, 0) == pdTRUE) {
    Serial.printf("rx: %s\n", l.s);                              // debug: everything that arrives
    handleLine(l.s);
  }
  static uint32_t lastDraw = 0;
  bool animating = (alertUntil && millis() < alertUntil) || warnText[0];   // blinking screens
  if (dirty || millis() - lastDraw > (animating ? 250u : 1000u)) { draw(); lastDraw = millis(); }
  delay(10);
}
