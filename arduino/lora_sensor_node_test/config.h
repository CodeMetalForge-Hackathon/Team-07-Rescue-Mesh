// config.h - per-team settings. Only NODE_ID should differ between boards.
#pragma once

// Give each board its own name (max 8 chars, no commas): "ALPHA", "BRAVO", "BASE"...
// Leave as "" to use an automatic ID made from the chip's unique ID.
#ifndef NODE_ID
#define NODE_ID ""
#endif

// ---- Radio: MUST be identical on every node ----
#define LORA_FREQ_HZ      915E6   // US ISM band (868E6 in Europe)
#define LORA_SF           7       // 7 = fast/short ... 12 = slow/longest range
#define LORA_BW_HZ        125E3
#define LORA_CR_DENOM     5       // coding rate 4/5
#define LORA_TX_DBM       14      // 2..20. 14 is plenty indoors; 20 only for outdoor range tests.

// ---- Behaviour ----
#define SEND_EVERY_MS     5000    // own reading interval
#define SEND_JITTER_MS    1500    // +/- random so nodes don't keep colliding
#define MAX_HOPS          2       // how many times a reading may be relayed
#define RELAY_ENABLED     true    // false = this node sends/receives but never forwards
#define STALE_AFTER_MS    30000   // mark a node STALE in the table after this
#define MAX_NODES         12      // size of the table of known nodes
