// siphash.h - SipHash-2-4 (Aumasson & Bernstein, 2012): a keyed hash made for authenticating short
// messages. 128-bit key -> 64-bit tag. Without the key you can't make a valid tag for a packet, so a
// fake HELP from someone else's radio is rejected. Written from the reference description and checked
// against the official test vectors (see tests in the repo's README). Plain C++, no Arduino calls.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace sip {

inline uint64_t rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }
inline uint64_t le64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

#define SIP_ROUND(v0, v1, v2, v3)                                   \
  do {                                                              \
    v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);       \
    v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;                          \
    v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;                          \
    v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);       \
  } while (0)

// SipHash-2-4 of `len` bytes at `in` with the 16-byte `key`.
inline uint64_t hash(const uint8_t key[16], const uint8_t* in, size_t len) {
  uint64_t k0 = le64(key), k1 = le64(key + 8);
  uint64_t v0 = 0x736f6d6570736575ULL ^ k0, v1 = 0x646f72616e646f6dULL ^ k1;
  uint64_t v2 = 0x6c7967656e657261ULL ^ k0, v3 = 0x7465646279746573ULL ^ k1;
  size_t full = len - (len % 8);
  for (size_t i = 0; i < full; i += 8) {
    uint64_t m = le64(in + i);
    v3 ^= m; SIP_ROUND(v0, v1, v2, v3); SIP_ROUND(v0, v1, v2, v3); v0 ^= m;
  }
  uint64_t b = ((uint64_t)len) << 56;
  for (size_t i = 0; i < len % 8; i++) b |= ((uint64_t)in[full + i]) << (8 * i);
  v3 ^= b; SIP_ROUND(v0, v1, v2, v3); SIP_ROUND(v0, v1, v2, v3); v0 ^= b;
  v2 ^= 0xff;
  SIP_ROUND(v0, v1, v2, v3); SIP_ROUND(v0, v1, v2, v3); SIP_ROUND(v0, v1, v2, v3); SIP_ROUND(v0, v1, v2, v3);
  return v0 ^ v1 ^ v2 ^ v3;
}
#undef SIP_ROUND

}  // namespace sip
