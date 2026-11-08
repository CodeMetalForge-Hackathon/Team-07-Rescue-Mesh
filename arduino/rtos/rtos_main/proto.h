// proto.h - over-the-air packet format (see PROTOCOL.md). Plain C++, no Arduino calls,
// so it can be unit-tested on a laptop.
//
//   [FF FF 00 00] + "<TYPE>,<src>,<seq>,<ttl>,<via>,<tag>,<rest...>"
//   tag = 16 hex digits: SipHash-2-4 with the team key over "<TYPE>,<src>,<seq>,<rest>" (see sign()).
//         ttl and via are left out on purpose: relays change them, the rest can't be changed without the key.
//     MS1 rest = dst,text        (text is last, may contain commas)
//     QM1 rest = dst,code        (OK / HELP / FOUND / RETURN)
//     AK1 rest = dst,acked_seq,R|O
//     BC1 rest = role,temp,rh,flag,boot,id:rssi;id:rssi   (silent ping)
//     SN1 rest = temp_c,rh,flag
//   ttl = repeats left (relay sets via = itself, ttl - 1); (src, seq) identifies a packet for dedupe.
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "siphash.h"

namespace proto {

const int HDR = 4;          // RadioHead-style header: to=FF from=FF id=0 flags=0
const int ID_MAX = 8;
const int MAX_TEXT = 60;    // max message length (= 3 lines on the screen)
const int MAX_PKT = 128;    // longest packet: 4 header + 117 (MS1 with 60-char text and the tag) + spare
const int TAG_HEX = 16;     // 64-bit tag as hex

struct Pkt {
  char type[4];
  char src[ID_MAX + 1];
  unsigned seq;
  unsigned ttl;
  char via[ID_MAX + 1];
  char tag[TAG_HEX + 1];
  char rest[MAX_PKT];       // everything after the 6 common fields
};

// The packet's tag: SipHash-2-4 with the team key over "<TYPE>,<src>,<seq>,<rest>", as 16 hex digits.
inline void sign(const uint8_t key[16], const char* type, const char* src, unsigned seq, const char* rest, char out[TAG_HEX + 1]) {
  char m[MAX_PKT + 24];
  int n = snprintf(m, sizeof m, "%s,%s,%u,%s", type, src, seq % 65536u, rest);
  if (n < 0) n = 0;
  if (n >= (int)sizeof m) n = sizeof m - 1;
  uint64_t h = sip::hash(key, (const uint8_t*)m, (size_t)n);
  // two 32-bit halves: the small printf on the M0 has no 64-bit numbers
  snprintf(out, TAG_HEX + 1, "%08lx%08lx", (unsigned long)(h >> 32), (unsigned long)(h & 0xFFFFFFFFu));
}
// Constant-time compare (so timing doesn't leak how many digits were right).
inline bool tagEqual(const char* a, const char* b) {
  uint8_t d = 0;
  for (int i = 0; i < TAG_HEX; i++) d |= (uint8_t)(a[i] ^ b[i]);
  return d == 0;
}

// Build a packet. Returns total length (with header) or 0 if it doesn't fit.
inline int build(uint8_t* out, int max, const char* type, const char* src, unsigned seq,
                 unsigned ttl, const char* via, const char* tag, const char* rest) {
  if (max < HDR + 8) return 0;
  out[0] = 0xFF; out[1] = 0xFF; out[2] = 0; out[3] = 0;
  int n = snprintf((char*)out + HDR, max - HDR, "%s,%s,%u,%u,%s,%s,%s", type, src, seq % 65536u, ttl, via, tag, rest);
  if (n <= 0 || n >= max - HDR) return 0;
  return HDR + n;
}

// Parse the header + 6 common fields. False if it isn't one of ours. (Check the tag separately.)
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
  char* f[7];
  int k = 0;
  f[k++] = s;
  for (char* q = s; *q && k < 7; q++)
    if (*q == ',') { *q = 0; f[k++] = q + 1; }
  if (k < 7) return false;
  if (strlen(f[0]) != 3 || !*f[1] || strlen(f[1]) > ID_MAX || !*f[4] || strlen(f[4]) > ID_MAX) return false;
  if (strlen(f[5]) != TAG_HEX) return false;
  char* end;
  unsigned long seq = strtoul(f[2], &end, 10); if (*end || !*f[2] || seq > 65535) return false;
  unsigned long ttl = strtoul(f[3], &end, 10); if (*end || !*f[3] || ttl > 15) return false;
  strcpy(p.type, f[0]); strcpy(p.src, f[1]); strcpy(p.via, f[4]); strcpy(p.tag, f[5]);
  p.seq = (unsigned)seq; p.ttl = (unsigned)ttl;
  strncpy(p.rest, f[6], sizeof p.rest - 1); p.rest[sizeof p.rest - 1] = 0;
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
