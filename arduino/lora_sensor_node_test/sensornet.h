// sensornet.h - packet protocol for the Team 7 LoRa sensor network.
// Plain C++ (no Arduino calls) so it can be unit-tested on a laptop.
//
// On air:  [FF FF 00 00] + "SN1,<src>,<seq>,<ttl>,<via>,<temp_c>,<rh>,<flag>"
//   The 4 leading bytes are the RadioHead header (to=broadcast, from=broadcast,
//   id=0, flags=0). It makes these packets readable by the CircuitPython
//   version of this node (adafruit_rfm9x) too, so both kinds of board can mix.
//
//   src    node that took the reading        seq   0..65535 counter from src
//   ttl    hops left (relays decrement it)   via   node that sent THIS copy
//   temp_c deg C, 2 decimals                 rh    % relative humidity, 2 decimals
//   flag   'R' real sensor, 'S' simulated (sensor missing)
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

namespace sensornet {

const int ID_MAX = 8;          // max chars in a node name
const int MAX_PACKET = 80;     // our packets are ~40 bytes
const uint8_t RH_BROADCAST = 0xFF;
const int HEADER_LEN = 4;

struct Reading {
  char src[ID_MAX + 1];
  uint16_t seq;
  uint8_t ttl;
  char via[ID_MAX + 1];
  float tempC;
  float rh;
  char flag;  // 'R' or 'S'
};

inline void copyId(char* dst, const char* s) {
  strncpy(dst, s, ID_MAX);
  dst[ID_MAX] = '\0';
}

// "23.46" / "-3.05" without relying on printf float support.
inline int fmt2(char* out, int max, float v) {
  long c = lroundf(v * 100.0f);
  const char* sign = c < 0 ? "-" : "";
  if (c < 0) c = -c;
  return snprintf(out, max, "%s%ld.%02ld", sign, c / 100, c % 100);
}

// Reading -> bytes ready for LoRa. Returns length, or 0 on error.
inline int encode(const Reading& r, uint8_t* out, int max) {
  if (max < HEADER_LEN + 1) return 0;
  out[0] = RH_BROADCAST; out[1] = RH_BROADCAST; out[2] = 0; out[3] = 0;
  char t[16], h[16];
  fmt2(t, sizeof t, r.tempC);
  fmt2(h, sizeof h, r.rh);
  int n = snprintf((char*)out + HEADER_LEN, max - HEADER_LEN, "SN1,%s,%u,%u,%s,%s,%s,%c",
                   r.src, (unsigned)r.seq, (unsigned)r.ttl, r.via, t, h, r.flag);
  if (n <= 0 || n >= max - HEADER_LEN) return 0;
  return HEADER_LEN + n;  // no trailing NUL on air
}

// Split s in place on ',' keeping empty fields. Returns field count.
inline int splitFields(char* s, char** f, int maxf) {
  int n = 0;
  f[n++] = s;
  for (char* p = s; *p; p++) {
    if (*p == ',') {
      *p = '\0';
      if (n == maxf) return maxf + 1;  // too many
      f[n++] = p + 1;
    }
  }
  return n;
}

inline bool parseUInt(const char* s, unsigned long maxv, unsigned long& v) {
  if (!*s) return false;
  char* end;
  v = strtoul(s, &end, 10);
  return *end == '\0' && v <= maxv && s[0] != '-';
}

inline bool parseFloat(const char* s, float& v) {
  if (!*s) return false;
  char* end;
  v = strtof(s, &end);
  return *end == '\0';
}

// bytes (including 4-byte header) -> Reading. False if it isn't ours / garbled.
inline bool decode(const uint8_t* buf, int len, Reading& r) {
  if (len < HEADER_LEN + 5 || len > MAX_PACKET) return false;
  if (buf[0] != RH_BROADCAST) return false;  // not addressed to everyone
  char s[MAX_PACKET + 1];
  int n = len - HEADER_LEN;
  for (int i = 0; i < n; i++) {
    uint8_t c = buf[HEADER_LEN + i];
    if (c < 0x20 || c > 0x7E) return false;   // printable ASCII only
    s[i] = (char)c;
  }
  s[n] = '\0';
  char* f[8];
  if (splitFields(s, f, 8) != 8) return false;
  if (strcmp(f[0], "SN1") != 0) return false;
  if (!*f[1] || !*f[4] || strlen(f[1]) > ID_MAX || strlen(f[4]) > ID_MAX) return false;
  unsigned long seq, ttl;
  if (!parseUInt(f[2], 65535, seq) || !parseUInt(f[3], 15, ttl)) return false;
  if (!parseFloat(f[5], r.tempC) || !parseFloat(f[6], r.rh)) return false;
  if (strlen(f[7]) != 1 || (f[7][0] != 'R' && f[7][0] != 'S')) return false;
  copyId(r.src, f[1]);
  copyId(r.via, f[4]);
  r.seq = (uint16_t)seq;
  r.ttl = (uint8_t)ttl;
  r.flag = f[7][0];
  return true;
}

// Remembers the last N (src, seq) pairs so each reading is shown/relayed once.
template <int N>
class SeenCache {
 public:
  SeenCache() : next_(0) { memset(used_, 0, sizeof used_); }
  bool seen(const char* src, uint16_t seq) const {
    for (int i = 0; i < N; i++)
      if (used_[i] && seq_[i] == seq && strcmp(src_[i], src) == 0) return true;
    return false;
  }
  void add(const char* src, uint16_t seq) {
    if (seen(src, seq)) return;
    copyId(src_[next_], src);
    seq_[next_] = seq;
    used_[next_] = true;
    next_ = (next_ + 1) % N;  // overwrite the oldest
  }
  // true if new (and records it), false if duplicate
  bool checkAndAdd(const char* src, uint16_t seq) {
    if (seen(src, seq)) return false;
    add(src, seq);
    return true;
  }
 private:
  char src_[N][ID_MAX + 1];
  uint16_t seq_[N];
  bool used_[N];
  int next_;
};

}  // namespace sensornet
