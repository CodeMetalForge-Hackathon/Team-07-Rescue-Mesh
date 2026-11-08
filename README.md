

# Team 7 – Rescue Mesh (Code Metal Forge 2026)

SEE VIDEOS HERE https://drive.google.com/drive/folders/1jyw7nIakE7EcjdJ60Ef6ZI2m9zqdJ0Kz

A GPS-free, cell-free communication network for search and rescue. Responders carry LoRa **nodes**
and drop them as relays as they move into a forest, desert, mountain, building, or rubble. 
The nodes pass texts, OK / HELP alerts and sensor readings along the chain, through a network that shows chat, alerts, the network and a rough map, all worked out from the radio links themselves. 
Any phone can join a nodes's own Wi-Fi to read and send messages, with no app and no internet.

```
  [ALPHA, dropped] <--LoRa--> [CHARLIE, dropped] <--LoRa--> [BRAVO, carried]
  relays everything           relays everything           "WEAK LINK - DEPLOY A RELAY"
  phone joins Team7-ALPHA                                  phone joins Team7-BRAVO
```

## What it does
- **Texts and OK / HELP** between any nodes, from the nodes's buttons, a phone. Max 60 characters.
- **Relay over several hops:** every node repeats each packet once (up to 3 repeats), so a node that can't
  hear the repeaters still reaches it through the nodes in between.
- **Confirmations (✓):** the receiving node confirms. For "everyone" messages the first node to answer is enough.
  If no ✓ comes after 5 s, the sender tries once more.
- **Catch-up sync:** each node keeps its last 6 messages. When a node comes back into range, is switched on, or
  reboots, its neighbours re-send what it missed. A HELP pressed out of range gets delivered when you walk back.
- **CARRY / DEPLOY switch:** a carried node watches its link to the chain and shows **WEAK LINK – DEPLOY A RELAY**
  when it drops to about 30 %.
- **LED bar** = signal strength to the strongest node heard.
- **Packet key:** every packet is signed with a team key (SipHash-2-4). Packets from radios without the key, like a
  faked HELP, are ignored.
- **Watchdog:** a node whose radio, buttons, screen or main logic freezes reboots itself within 8 s.
- **Phone page:** each node's screen board makes an open Wi-Fi network `Team7-<node>`, and the chat page opens by itself.

## Hardware (per node)
- **Radio board:** Adafruit Feather RP2040 RFM95 (915 MHz), or Feather M0 RFM95 (#3178). Same firmware on both.
- **Screen + phone Wi-Fi:** Adafruit ESP32-S3 Reverse TFT Feather, connected over I2C.
- SHT41 temperature/humidity sensor, Grove LED Bar v2.1, Grove buzzer, Grove switch (CARRY / DEPLOY),
  2 Grove LED buttons (red = HELP, blue = OK). Wire antenna, 78 mm.

| Module | RP2040 Feather | M0 Feather |
|---|---|---|
| LED Bar clock / data | D9 / D10 | D9 / D10 |
| Buzzer | D11 | D11 |
| Switch | D12 | D12 |
| Red button: LED / button | D5 / D6 | D5 / D6 |
| Blue button: LED / button | D24 / D25 | **D1 / D0** |
| SHT41 + ESP32 screen (I2C) | STEMMA QT | SDA / SCL pins |

Grove modules are powered from **3.3V** and GND, never from USB or BAT.

## Repo layout
| Path | What |
|---|---|
| `arduino/rtos/rtos_main/` | **The node firmware** (Arduino IDE). FreeRTOS on the RP2040, plain loop on the M0. See `arduino/rtos/README.md` |
| `arduino/display_esp32/` | Screen + phone Wi-Fi page, for the ESP32-S3 Reverse TFT Feather |
| `arduino/rtos/rtos_test/` | Small FreeRTOS check on one node (blinking tasks, button → buzzer, LED bar) |
| `arduino/hw_test/` | Quick test of the LED bar, buzzer, switch and buttons on one node |
| `arduino/lora_sensor_node_test/` | The first (pre-RTOS) firmware, kept for reference |
| `pi/` | Base-station web app (Python 3 + pyserial). Has a `--sim` mode |
| `PROTOCOL.md` | Packet format over the air, and the serial protocol between node and laptop |
| `tests/` | Laptop tests for the packet format and the key |

## Setup
**Arduino IDE, radio boards**
1. Boards Manager URLs (Preferences):
   - `https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json`
   - `https://adafruit.github.io/arduino-board-index/package_adafruit_index.json`

   Then install **Raspberry Pi Pico/RP2040/RP2350** (Earle Philhower) and **Adafruit SAMD Boards**.
2. Libraries: **LoRa** (Sandeep Mistry), **Adafruit SHT4x**. The LED bar driver is built in.
3. **Team key:** `arduino/rtos/rtos_main/key.h` is secret and not in git. Get it from a teammate, or copy
   `key.example.h` → `key.h` and put in 16 new random bytes. **Every node needs the same key.**
4. Open `arduino/rtos/rtos_main/rtos_main.ino` and set `NODE_ID` (different for every node).
5. Board settings:
   - **RP2040:** board *Adafruit Feather RP2040 RFM*, **Tools → Operating System → FreeRTOS SMP**.
   - **M0:** board *Adafruit Feather M0 (SAMD21)*.
6. Upload. If it can't find the board:
   - **RP2040:** hold BOOT, tap RESET, upload again.
   - **M0:** double-tap RESET (a FEATHERBOOT drive appears), then upload.

**Screen board (ESP32-S3)**
1. Install the **esp32** boards (Espressif) and the **Adafruit ST7735 and ST7789** library.
2. Board *Adafruit Feather ESP32-S3 Reverse TFT*. Upload `arduino/display_esp32`.
3. If no port shows up: hold D0, tap RESET, release D0, then upload.

## Phones
Join the Wi-Fi network **`Team7-<node name>`** (for example `Team7-ALPHA`). The chat page opens by itself, or go to
**http://192.168.4.1**. On an iPhone, choose "Use Without Internet" if asked.

## Tests
```bash
g++ -std=c++17 -Wall -I arduino/rtos/rtos_main tests/test_proto.cpp -o t && ./t   # packet format + key (incl. official SipHash vectors)
g++ -std=c++17 -Wall -I arduino/lora_sensor_node_test tests/test_sensornet.cpp -o t && ./t   # the old firmware's packet code
```

## Status
Versions are git tags: `v1.0` (first LoRa link + screen), `v1.1` (pings, signal bar, confirmations),
`v1.2` (relay, sync, M0, phone page, key, watchdog).

## Hardware lessons
- Keep metal at least 10 cm from the antennas, and point the antennas straight up.
- Insulate bare antenna wire near the board.
- Don't touch boards while they transmit.
