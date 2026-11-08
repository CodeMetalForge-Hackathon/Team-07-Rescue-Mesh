// proto.h - over-the-air packet format (see PROTOCOL.md). Plain C++, no Arduino calls,
// so it can be unit-tested on a laptop.
//
//   [FF FF 00 00] + "<TYPE>,<src>,<seq>,<ttl>,<via>,<rest...>"
//     MS1 rest = dst,text        (text is last, may contain commas)
//     QM1 rest = dst,code        (OK / HELP / FOUND / RETURN)
//     AK1 rest = dst,acked_seq
//     SN1 rest = temp_c,rh,flag
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace proto {

const int HDR = 4;          // RadioHead-style header: to=FF from=FF id=0 flags=0
const int ID_MAX = 8;
const int MAX_TEXT = 60;    // max message length (= 3 lines on the screen)
const int MAX_PKT = 112;    // longest packet: 4 header + 100 (MS1 with 60-char text) + spare

struct Pkt {
  char type[4];
  char src[ID_MAX + 1];
  unsigned seq;
  unsigned ttl;
  char via[ID_MAX + 1];
  char rest[MAX_PKT];       // everything after the 5 common fields
};

// Build a packet. Returns total length (with header) or 0 if it doesn't fit.
inline int build(uint8_t* out, int max, const char* type, const char* src, unsigned seq,
                 unsigned ttl, const char* via, const char* rest) {
  if (max < HDR + 8) return 0;
  out[0] = 0xFF; out[1] = 0xFF; out[2] = 0; out[3] = 0;
  int n = snprintf((char*)out + HDR, max - HDR, "%s,%s,%u,%u,%s,%s", type, src, seq % 65536u, ttl, via, rest);
  if (n <= 0 || n >= max - HDR) return 0;
  return HDR + n;
}

// Parse the header + 5 common fields. False if it isn't one of ours.
inline bool parse(const uint8_t* buf, int len, Pkt& p) {
  if (len < HDR + 10 || len > HDR + MAX_PKT - 1 || buf[0] != 0xFF) return false;
  char s[MAX_PKT];
  int n = len - HDR;
  for (int i = 0; i < n; i++) {
    uint8_t c = buf[HDR + i];
    if (c < 32 || c > 126) return false;
    s[i] = (char)c;
  }
  s[n] = 0;
  char* f[6];
  int k = 0;
  f[k++] = s;
  for (char* q = s; *q && k < 6; q++)
    if (*q == ',') { *q = 0; f[k++] = q + 1; }
  if (k < 6) return false;
  if (strlen(f[0]) != 3 || !*f[1] || strlen(f[1]) > ID_MAX || !*f[4] || strlen(f[4]) > ID_MAX) return false;
  char* end;
  unsigned long seq = strtoul(f[2], &end, 10); if (*end || !*f[2] || seq > 65535) return false;
  unsigned long ttl = strtoul(f[3], &end, 10); if (*end || !*f[3] || ttl > 15) return false;
  strcpy(p.type, f[0]); strcpy(p.src, f[1]); strcpy(p.via, f[4]);
  p.seq = (unsigned)seq; p.ttl = (unsigned)ttl;
  strncpy(p.rest, f[5], sizeof p.rest - 1); p.rest[sizeof p.rest - 1] = 0;
  return true;
}

// Split "a,b..." into first field (a) and the remainder (b..., may contain commas).
inline bool split1(const char* rest, char* first, int firstMax, const char** remainder) {
  const char* c = strchr(rest, ',');
  if (!c || c == rest || c - rest >= firstMax) return false;
  memcpy(first, rest, c - rest); first[c - rest] = 0;
  *remainder = c + 1;
  return true;
}

// Make text safe to send: printable ASCII only, trimmed, max MAX_TEXT chars.
inline void cleanText(const char* in, char* out, int max) {
  int n = 0;
  for (const char* p = in; *p && n < max - 1 && n < MAX_TEXT; p++)
    if (*p >= 32 && *p <= 126) out[n++] = *p;
  while (n && out[n - 1] == ' ') n--;
  out[n] = 0;
}

}  // namespace proto
