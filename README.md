# Team 7 – Rescue Mesh (Code Metal Forge 2026)

A GPS-free, cell-free communication network for search and rescue. A responder places
LoRa **drop boxes** as they move into a building or rubble. The boxes relay
messages and sensor readings back to a **base station**, and a phone web app shows chat,
alerts, the chain of boxes and a rough map of where everyone is, all worked out from
the radio links themselves.

```
 [BASE + Pi 5] <--LoRa--> [RELAY #1] <--LoRa--> [CARRIED box]
   phones join the Pi's      dropped box,           buzzes when the link to the
   Wi-Fi hotspot -> web app  silent relay           last drop gets weak -> drop the next box
```

## Hardware (per box)
- Adafruit Feather RP2040 RFM95 (LoRa, 915 MHz) with a 78 mm wire antenna
- SHT41 temperature/humidity sensor on the STEMMA QT port
- Grove LED Bar v2.1 (shows link strength), Grove Buzzer, Grove Switch (CARRY / DEPLOY)
- Base box: plugged into a Raspberry Pi 5 over USB

| Module | Feather pin |
|---|---|
| LED Bar data / clock | D10 / D9 |
| Buzzer | D11 |
| Switch | D12 |
| Quick-message buttons (planned) | D5, D6, D24 |

Every module is powered from **3V** and GND. Never from USB or BAT.

## Repo layout
| Path | What |
|---|---|
| `arduino/lora_sensor_node/` | Box firmware (Arduino IDE): sensor readings, multi-hop relay, dedupe, radio TX timeout, watchdog and freeze-location report |
| `arduino/hw_test/` | Quick test for the LED bar, buzzer and switch on one box |
| `pi/` | Base-station web app (Python 3 + pyserial): chat, network view, minimap, alerts. Includes `--sim` mode |
| `PROTOCOL.md` | Over-the-air packet format and the serial protocol between box and Pi |
| `tests/` | Laptop unit tests for the packet code |

## Arduino setup
1. Add the boards URL `https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json`, then install **Raspberry Pi Pico/RP2040/RP2350** (Earle Philhower).
2. Select the board **Adafruit Feather RP2040 RFM**.
3. Install these libraries: **LoRa** (Sandeep Mistry), **Adafruit SHT4x**, **Adafruit NeoPixel**, **Grove LED Bar**.
4. Set a unique `NODE_ID` in `config.h` for each box, then upload. If the upload hangs, hold BOOT, tap RESET, and upload again.

## Base station
```bash
cd pi && python3 base_station.py --sim   # try without hardware -> http://localhost:8000
python3 base_station.py                  # with the base box plugged in
```
For the Pi hotspot and auto-start, see `pi/README.md`.

## Tests
```bash
g++ -std=c++17 -I arduino/lora_sensor_node tests/test_sensornet.cpp -o t && ./t
```

## Status
- Working on hardware: two-box link, sensor readings, relay logic, freeze protection.
- Tested in simulation: 3-box relay chain and the full web app.
- In progress: CARRY/DEPLOY roles, LED-bar link meter, buzzer warnings, texting and quick messages in the firmware.

## Hardware lessons
- Keep metal at least 10 cm from the antennas, and point the antennas straight up.
- Insulate bare antenna wire near the board.
- Don't touch boards while they transmit.
