// test_proto.cpp - laptop tests for the box firmware's packet format and packet key.
//   g++ -std=c++17 -Wall -I arduino/rtos/rtos_main tests/test_proto.cpp -o t && ./t
// (proto.h and siphash.h are plain C++, so they compile on a laptop.)
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include "proto.h"

// Official SipHash-2-4 test vectors (Aumasson & Bernstein): key = 00 01 .. 0f, message = 00 01 .. (n-1), n = 0..63.
static const uint64_t VECTORS[64] = {
  0x726fdb47dd0e0e31ULL, 0x74f839c593dc67fdULL, 0x0d6c8009d9a94f5aULL, 0x85676696d7fb7e2dULL,
  0xcf2794e0277187b7ULL, 0x18765564cd99a68dULL, 0xcbc9466e58fee3ceULL, 0xab0200f58b01d137ULL,
  0x93f5f5799a932462ULL, 0x9e0082df0ba9e4b0ULL, 0x7a5dbbc594ddb9f3ULL, 0xf4b32f46226bada7ULL,
  0x751e8fbc860ee5fbULL, 0x14ea5627c0843d90ULL, 0xf723ca908e7af2eeULL, 0xa129ca6149be45e5ULL,
  0x3f2acc7f57c29bdbULL, 0x699ae9f52cbe4794ULL, 0x4bc1b3f0968dd39cULL, 0xbb6dc91da77961bdULL,
  0xbed65cf21aa2ee98ULL, 0xd0f2cbb02e3b67c7ULL, 0x93536795e3a33e88ULL, 0xa80c038ccd5ccec8ULL,
  0xb8ad50c6f649af94ULL, 0xbce192de8a85b8eaULL, 0x17d835b85bbb15f3ULL, 0x2f2e6163076bcfadULL,
  0xde4daaaca71dc9a5ULL, 0xa6a2506687956571ULL, 0xad87a3535c49ef28ULL, 0x32d892fad841c342ULL,
  0x7127512f72f27cceULL, 0xa7f32346f95978e3ULL, 0x12e0b01abb051238ULL, 0x15e034d40fa197aeULL,
  0x314dffbe0815a3b4ULL, 0x027990f029623981ULL, 0xcadcd4e59ef40c4dULL, 0x9abfd8766a33735cULL,
  0x0e3ea96b5304a7d0ULL, 0xad0c42d6fc585992ULL, 0x187306c89bc215a9ULL, 0xd4a60abcf3792b95ULL,
  0xf935451de4f21df2ULL, 0xa9538f0419755787ULL, 0xdb9acddff56ca510ULL, 0xd06c98cd5c0975ebULL,
  0xe612a3cb9ecba951ULL, 0xc766e62cfcadaf96ULL, 0xee64435a9752fe72ULL, 0xa192d576b245165aULL,
  0x0a8787bf8ecb74b2ULL, 0x81b3e73d20b49b6fULL, 0x7fa8220ba3b2eceaULL, 0x245731c13ca42499ULL,
  0xb78dbfaf3a8d83bdULL, 0xea1ad565322a1a0bULL, 0x60e61c23a3795013ULL, 0x6606d7e446282b93ULL,
  0x6ca4ecb15c5f91e1ULL, 0x9f626da15c9625f3ULL, 0xe51b38608ef25f57ULL, 0x958a324ceb064572ULL
};

int main() {
  using namespace proto;
  // 1. SipHash matches the official vectors
  uint8_t k[16], m[64];
  for (int i = 0; i < 16; i++) k[i] = i;
  for (int i = 0; i < 64; i++) m[i] = i;
  for (int n = 0; n < 64; n++) assert(sip::hash(k, m, n) == VECTORS[n]);
  printf("SipHash-2-4: all 64 official test vectors match\n");

  const uint8_t key[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}, other[16] = {9};
  char tag[TAG_HEX + 1], want[TAG_HEX + 1];
  uint8_t buf[MAX_PKT];
  Pkt p;

  // 2. the longest text packet fits: 60-char text, 8-char names, 5-digit number
  std::string sixty(60, 'A');
  char rest[MAX_PKT]; snprintf(rest, sizeof rest, "%s,%s", "ABCDEFGH", sixty.c_str());
  sign(key, "MS1", "ABCDEFGH", 65535, rest, tag);
  int n = build(buf, sizeof buf, "MS1", "ABCDEFGH", 65535, 11, "ABCDEFGH", tag, rest);
  assert(n > 0 && parse(buf, n, p));
  const char* text; char dst[9];
  assert(split1(p.rest, dst, sizeof dst, &text) && std::string(text) == sixty && std::string(dst) == "ABCDEFGH");
  printf("longest text packet: %d bytes (buffer %d, LoRa max 255)\n", n, MAX_PKT);

  // 3. the right key accepts, another key rejects
  sign(key, p.type, p.src, p.seq, p.rest, want);   assert(tagEqual(want, p.tag));
  sign(other, p.type, p.src, p.seq, p.rest, want); assert(!tagEqual(want, p.tag));

  // 4. a relay changes ttl and via: still valid
  n = build(buf, sizeof buf, "MS1", "ABCDEFGH", 65535, 2, "RELAY1", tag, rest);
  assert(parse(buf, n, p));
  sign(key, p.type, p.src, p.seq, p.rest, want); assert(tagEqual(want, p.tag));

  // 5. someone changes "OK" to "HELP" but keeps the tag: rejected
  sign(key, "QM1", "ALPHA", 5, "*,OK", tag);
  n = build(buf, sizeof buf, "QM1", "ALPHA", 5, 3, "ALPHA", tag, "*,HELP");
  assert(parse(buf, n, p));
  sign(key, p.type, p.src, p.seq, p.rest, want); assert(!tagEqual(want, p.tag));
  printf("key check: right key accepted, wrong key / edited packet rejected, relay changes allowed\n");

  // 6. a ping listing 4 neighbours fits
  char ping[MAX_PKT];
  snprintf(ping, sizeof ping, "D,-12.34,100.00,R,1,%s", "CHARLIE1:-120;CHARLIE2:-120;CHARLIE3:-120;CHARL:-99");
  sign(key, "BC1", "ABCDEFGH", 65535, ping, tag);
  n = build(buf, sizeof buf, "BC1", "ABCDEFGH", 65535, 3, "ABCDEFGH", tag, ping);
  assert(n > 0 && parse(buf, n, p));
  printf("longest ping: %d bytes\n", n);

  // 7. text is cleaned: printable only, max 60 characters
  std::string longIn(120, 'x'); longIn[5] = '\n';
  char clean[200]; cleanText(longIn.c_str(), clean, sizeof clean);
  assert(strlen(clean) == (size_t)MAX_TEXT && !strchr(clean, '\n'));

  // 8. junk is not ours
  const uint8_t junk[] = {0xFF, 0xFF, 0, 0, 'h', 'e', 'l', 'l', 'o', ',', 'w', 'o', 'r', 'l', 'd'};
  assert(!parse(junk, sizeof junk, p));
  printf("all packet tests passed\n");
}
