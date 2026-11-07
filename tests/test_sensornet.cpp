// Laptop unit tests for sensornet.h:
//   g++ -std=c++17 -Wall -I arduino/lora_sensor_node tests/test_sensornet.cpp -o t && ./t
#include <cassert>
#include <cstdio>
#include "sensornet.h"
using namespace sensornet;

static Reading mk(const char* src, uint16_t seq, uint8_t ttl, const char* via, float t, float h, char f) {
  Reading r; copyId(r.src, src); copyId(r.via, via);
  r.seq = seq; r.ttl = ttl; r.tempC = t; r.rh = h; r.flag = f; return r;
}

static int raw(const char* s, uint8_t* out) {  // header + ascii body
  out[0] = 0xFF; out[1] = 0xFF; out[2] = 0; out[3] = 0;
  int n = strlen(s); memcpy(out + 4, s, n); return n + 4;
}

int main() {
  uint8_t b[MAX_PACKET]; Reading d;

  // round trip + exact wire format (must match the CircuitPython node)
  int n = encode(mk("ALPHA", 65535, 2, "ALPHA", 23.456f, 45.0f, 'R'), b, sizeof b);
  assert(n == 4 + (int)strlen("SN1,ALPHA,65535,2,ALPHA,23.46,45.00,R"));
  assert(b[0] == 0xFF && b[1] == 0xFF && b[2] == 0 && b[3] == 0);
  assert(memcmp(b + 4, "SN1,ALPHA,65535,2,ALPHA,23.46,45.00,R", n - 4) == 0);
  assert(decode(b, n, d));
  assert(!strcmp(d.src, "ALPHA") && d.seq == 65535 && d.ttl == 2 && d.flag == 'R');
  assert(d.tempC > 23.45f && d.tempC < 23.47f && d.rh == 45.0f);

  // negatives and small values
  n = encode(mk("B", 1, 0, "C", -3.05f, 0.04f, 'S'), b, sizeof b);
  assert(memcmp(b + 4, "SN1,B,1,0,C,-3.05,0.04,S", n - 4) == 0);
  assert(decode(b, n, d) && d.tempC < -3.04f && d.tempC > -3.06f && !strcmp(d.via, "C"));

  // a packet produced by the CircuitPython node decodes here
  n = raw("SN1,PYNODE,812,2,PYNODE,22.65,40.30,R", b);
  assert(decode(b, n, d) && !strcmp(d.src, "PYNODE") && d.seq == 812);

  // garbage is rejected
  const char* bad[] = {"", "hello", "SN1,a,b,c", "XX1,A,1,2,A,1.0,2.0,R", "SN1,A,1,-1,A,1.0,2.0,R",
                       "SN1,A,1,2,A,1.0,2.0,Q", "SN1,,1,2,A,1.0,2.0,R", "SN1,A,1,2,A,1.0,2.0,R,extra",
                       "SN1,A,70000,2,A,1.0,2.0,R", "SN1,A,1,2,A,abc,2.0,R", "SN1,TOOLONGNAME,1,2,A,1,2,R"};
  for (const char* s : bad) { n = raw(s, b); assert(!decode(b, n, d)); }
  n = raw("SN1,A,1,2,A,1.0,2.0,R", b); b[0] = 0x05; assert(!decode(b, n, d));  // addressed elsewhere
  n = raw("SN1,A,1,2,A,1.0,2.0,R", b); b[6] = 0x01; assert(!decode(b, n, d));  // binary junk

  // dedupe cache
  SeenCache<3> c;
  assert(c.checkAndAdd("A", 1) && !c.checkAndAdd("A", 1) && c.checkAndAdd("B", 1));
  c.add("A", 2); c.add("A", 3);
  assert(!c.seen("A", 1) && c.seen("A", 3));   // oldest evicted

  puts("all sensornet.h tests passed");
}
