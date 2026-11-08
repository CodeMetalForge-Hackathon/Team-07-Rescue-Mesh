# FreeRTOS firmware (work in progress)

The box firmware is moving to **FreeRTOS**, using the copy built into the Arduino core for
this board. You don't need to download FreeRTOS, and the separate FreeRTOS-LTS download should
**not** be added to this repo: it targets the Pico SDK/CMake toolchain and is 115 MB.

## Arduino IDE settings
**RP2040 boxes (ALPHA, BRAVO, BASE)**
- **Tools → Board →** Adafruit Feather RP2040 RFM
- **Tools → Operating System → FreeRTOS SMP** ← required, otherwise you get `FreeRTOS.h: No such file`

**M0 box (CHARLIE: Feather M0 RFM95, #3178)**
- Install **Adafruit SAMD Boards** in the Boards Manager (needs the Adafruit board URL in Preferences).
- **Tools → Board → Adafruit SAMD → Adafruit Feather M0 (SAMD21)**
- The same `rtos_main` sketch runs here **without FreeRTOS**: the tasks run one after another in `loop()`,
  and `minirtos.h` provides the few FreeRTOS calls. The features are the same: buttons, switch, LED bar, buzzer, screen, sensor, relay and sync.
  It uses ~11 KB of the M0's 32 KB RAM.
- Wiring differences (handled in `pins.h`):
  - Blue button: LED on **D1**, button on **D0** (D24 is the radio's SPI clock on the M0).
  - No STEMMA QT plug: the SHT41 and the ESP32 screen go on the **SDA / SCL** pins (plus 3V / GND).
  - Radio pins are CS 8, RST 4, IRQ 3. They're on the board already, nothing to wire.
- Typing `STACKS` prints the free RAM.

**All boxes**
- Libraries: **LoRa** (Sandeep Mistry), **Adafruit SHT4x**. The LED bar driver is built in (`ledbar.h`).
- Screen: a separate **ESP32-S3 Reverse TFT Feather** running `../display_esp32` (see below).

## Sketches
| Folder | What it is |
|---|---|
| `rtos_test/` | Small check of FreeRTOS on one box: two LEDs blinking at different rates (2 tasks), button → queue → buzzer, LED bar animation, and a per-task core/stack report every 5 s. No radio. |
| `rtos_main/` | The real firmware, being built up step by step. One task per piece of hardware, joined by queues. It already does OK/HELP quick messages, texts from the base shown on the screen (confirmed back automatically), screen pages, CARRY/DEPLOY, sensor broadcasts, and the base-station serial protocol. Set `NODE_ID` (and `IS_BASE` on the Pi's box) for each box. |

`pins.h` and `ledbar.h` are copied into each sketch folder, because Arduino sketches can't
share files across folders. **When you change them, change both copies.** (`rtos_test` is RP2040-only.)

## Screen (ESP32-S3 Reverse TFT Feather)
The TFT on that board is wired inside the board to its own ESP32-S3, so the RP2040 can't drive it.
Instead, the ESP32 runs `../display_esp32` as an **I2C device at address 0x42**. The RP2040 (I2C controller,
the same bus as the SHT41) sends it short text lines, and the ESP32 draws them. The line format is in `displink.h`.

```
RP2040 SDA (GPIO 2) -- ESP32 SDA (GPIO 3)      (STEMMA QT to STEMMA QT works,
RP2040 SCL (GPIO 3) -- ESP32 SCL (GPIO 4)       but don't use its red 3V wire)
GND ------------------ GND
power: RP2040 USB pin -- ESP32 USB pin  (one USB cable for both; NOT 3V to 3V)
```
- TX/RX (GPIO 0/1) stay free for your own digital I/O.
- **The RP2040 logs whether the screen answers:** `screen connected (I2C 0x42)` or `screen not answering…`.
  A box without a screen works normally.
- **The ESP32 prints every line it receives** (`rx: D,...`) on its own USB Serial Monitor (115200), which helps when debugging.
- **ESP32 IDE setup:** in Boards Manager install **esp32** by Espressif, then select the board **Adafruit Feather ESP32-S3 Reverse TFT**.
  Install the library **Adafruit ST7735 and ST7789 Library** (click Install all).
- **One messaging screen:**
  - **Header:** box name, mode (orange CARRY / green DEPLOY / blue BASE), temperature.
  - **Newest message:** sender, age and 3 lines of text. For your own messages it shows the
    confirmation status: `...` while waiting, `√BASE` or `√BASE+1` once confirmed, `no reply` after 10 s.
  - **Previous message**, on one line.
  - **Bottom:** boxes heard, with signal bars. In CARRY mode with a weak link, this becomes a blinking red **WEAK LINK / DEPLOY A RELAY** banner.
- A **long incoming message** fills the whole screen for 20 s.
- A **HELP** from another box shows a blinking **red screen** for 20 s, or until the next message.
- A red **"WAITING FOR BOX" / "NO LINK TO BOX"** bar means nothing has arrived from the RP2040 for 15 s. Check the SDA/SCL/GND wires and that the RP2040 is running `rtos_main`.
- Text uses size 2 (about 1.6 mm tall). That's the smallest that's readable on a 1.14" screen.

## Safety: packet key + watchdog (v1.2)
**Key (authentication).** Every packet carries a 16-hex-digit tag made with the team key (SipHash-2-4, a standard
algorithm for authenticating short messages; checked against its official test vectors). Packets with a wrong tag are
ignored, and the Serial Monitor says `!! ... failed the key check`. This stops a fake HELP or a fake
"all clear" from someone else's radio. The text itself is **not** hidden: it's authenticated, not encrypted.
- The key lives in `rtos_main/key.h`. **It is secret and is NOT in git** (`.gitignore`).
  - Your laptop already has one. Every box flashed from this folder gets the same key automatically.
  - A teammate cloning the repo: ask for `key.h`, or copy `key.example.h` → `key.h`, put in new random bytes, and re-flash **every** box.
- A box with a different key (or old firmware) is cut off, and its neighbours log it every 10 s. `AUTH_CHECK 0` logs bad tags but accepts them (debugging only).
- Relays may change `ttl` and `via`, so those two fields aren't covered by the tag. Duplicates are still dropped, so an
  immediate replay of a recorded packet does nothing.

**Watchdog.** The radio, io (buttons/LEDs), i2c (sensor/screen) and app parts each check in every time they run.
The watchdog is only fed while **all** of them are alive. If one is stuck for 4 s, the log names it (`!! radio part stuck...`),
and the box reboots at 8 s. After the reboot it says `this start was a WATCHDOG REBOOT`, and the catch-up sync restores
messages it missed. RP2040: built-in watchdog. M0: the SAMD21 watchdog, set up directly (no library).

## Phone page (v1.2)
Each box's ESP32 screen board makes an **open Wi-Fi network called `Team7-<box name>`** (e.g. `Team7-ALPHA`).
1. On the phone: Settings → Wi-Fi → join `Team7-ALPHA`. The chat page opens by itself.
   If it doesn't, open the browser and go to **http://192.168.4.1** (any `http://` address works too).
   - **iPhone:** if a sheet pops up, use it, or tap *Cancel → Use Other Network / Use Without Internet* to stay connected and use Safari.
   - **Android:** tap *Sign in to network*. If it asks, choose *Stay connected / Use this network as is*.
2. The page shows the box's messages (with ✓ confirmations), the boxes it hears, temperature, and the DEPLOY A RELAY warning.
   You can send a text (to everyone or one box, 60 characters max) and press **OK** or **HELP**. HELP needs two taps so it can't go by accident.
   **HELP / OK from the phone are exactly the box's red / blue buttons**: same LED, beep and confirmations.
- How it works: the main board asks the screen board every 0.25 s "did the phone send something?" over the I2C wires
  that already connect them, so there's no new wiring. Each line has a number and is confirmed, so it is never lost or sent twice.
- Range: about 10–20 m. Wi-Fi power is set low (`WIFI_POWER` in `display_esp32.ino`) because the screen board runs off the box's 3.3 V.
  If the screen board keeps restarting while the phone is connected, that 3.3 V supply is too weak: power the ESP32 from USB instead.
- `PHONE_WIFI 0` in `display_esp32.ino` turns the Wi-Fi off. `WIFI_PASS` (8+ characters) adds a password.
- Upload **both** the new `display_esp32` (ESP32 board) and the new `rtos_main` (main board). An old main board never collects the phone's messages,
  and the page will say "Waiting for the box to take N message(s)".

## Buttons, switch, LED bar, buzzer
| Input / output | Does |
|---|---|
| **Red** button (button 1, D5/D6) | **HELP** to everyone. Its LED blinks until someone confirms, then stays on 3 s. Fast blinking = nobody confirmed in 10 s. |
| **Blue** button (button 2, D24/D25) | **OK** to everyone, with the same confirmation feedback |
| Switch | CARRY / DEPLOY (flip `DEPLOY_LEVEL` if it's backwards) |
| LED bar | **Signal strength** of the strongest box heard (10 = strong). Nobody heard = only the first (red) segment. Set `BAR_RED_FIRST 0` if your bar is mounted the other way round. |
| Buzzer | 1 beep sent / confirmed · 2 beeps text received, or no reply · 3 beeps HELP received · **rapid bursts = CARRY mode with a weak link: deploy a relay** |

- **Silent ping:** every 30 s each box sends a short ping with its role and temperature. There's no beep or
  message. It keeps the LED bar, the screen's box list and the base app's network view up to date.
- **Confirmations:** every box automatically confirms each text and quick message it receives.
  When the base operator presses *Acknowledge* in the app, the box shows "HELP acknowledged - help is coming".
- **CARRY alarm:** when the link to the deployed boxes or base falls to 30 % or below, the box beeps rapidly and shows DEPLOY A RELAY.
  It stops at 40 %, or when you switch to DEPLOY. Signal readings are averaged, and with 30 s pings
  the alarm can lag up to about a minute behind someone walking. `PING_EVERY_MS` is the setting that controls this.

## Relay and catch-up sync (v1.2)
- **Relay:** every box repeats each packet the first time it hears it, after a random 0.1–0.6 s wait.
  A packet can be repeated up to `MAX_HOPS` (3) times, so BASE – relay – relay – relay – far box works.
  Confirmations come back the same way. Duplicates are dropped by (sender, number).
  Set `RELAY 0` on a box that should never repeat.
- **Catch-up sync:** every box remembers the last `STORE_N` (6) texts and quick messages.
  When a box comes into range, is switched on, or reboots, its neighbours re-send anything younger than 10 minutes.
  - A HELP pressed out of range shows "no reply" after 10 s. It is delivered automatically once the carrier
    walks back into range, and the confirmation still turns up on the screen.
  - A text the base sent while a box was away reaches that box when it returns.
- **Map data:** each ping lists up to 4 boxes it hears and how strongly. The base box turns these into
  `NBR` lines, so the app can draw the whole chain, including boxes the base can't hear.
- RAM: the relay and sync add about 0.9 KB, all fixed-size (no `malloc`). On the M0, lower `STORE_N` first if it's tight.
- Limits: there's no listen-before-talk, so two boxes sending at the exact same moment can still collide.
  The random waits make this rare, but a collided HELP is only re-sent on the next sync.

## Base box
Set `IS_BASE 1` and plug the box into the Pi or a laptop running `pi/base_station.py`.
- The box accepts `TX,<dst>,<text>`, `QM,<dst>,<code>` and `AK,<dst>,<seq>` from the app.
- It prints `ME`, `DATA`, `BCN`, `NBR`, `MSG`, `QM`, `ACK` and `SENT` lines (see `PROTOCOL.md`). Messages that came through a relay show the relay in the `via` field.
- Texts to `*` go to everyone; texts to a box ID go to that box only.
- A field box confirms every text it receives, so the app shows ✓✓ delivered.
- When the base operator acknowledges a HELP, that box shows "HELP acknowledged - help is coming".
- Type `STACKS` in the Serial Monitor to print the spare stack of each task and the free heap. We use this to size the M0 version.
- The Serial Monitor can't be open while `base_station.py` is running, because both need the port.

## Task layout (`rtos_main`)
```
buttons task (prio 3) --events--> app = loop() --UI commands--> UI task (prio 2): buzzer, button LEDs, LED bar
sensor  task (prio 1) --events-->      |        --packets-----> radio task (prio 2): LoRa send/receive
                                       ^------------events------------------------------'
```
| Task | Owns | Does |
|---|---|---|
| `io` | buttons, switch, buzzer, button LEDs, LED bar, USB serial input | 10 ms loop: debounced button/switch events, beeps/alarm bursts, LED modes, bar level, command lines |
| `i2c` | Wire: SHT41 + ESP32 screen | a reading every 5 s (simulated if the sensor is missing); sends screen messages to the ESP32 |
| `radio` | SPI + RFM95 | sends queued packets (3 s TX timeout, then radio restart) and reports received packets with RSSI/SNR |
| app (`loop`) | nothing directly | decides everything: messages, confirmations, pings, link meter, CARRY alarm |

## Rules
1. **Each piece of hardware is used by one task only.** Arduino libraries aren't thread-safe.
   Screen content goes through the **i2c task**, because the screen shares Wire with the SHT41. Other tasks send it `DispMsg`s.
2. **Tasks talk through queues**, not shared variables.
3. **Wait with `vTaskDelay()` / `delay()`**, which let other tasks run. Never use busy loops.
   (`delayMicroseconds()` doesn't yield, so keep it to microseconds.)
4. **Print with `logf()`.** It takes a mutex so lines from different tasks don't get mixed together.

## Plan: features to move over from `../lora_sensor_node`, one at a time
- [x] sensor packets (SN1), text (MS1), quick messages (QM1), acknowledgements (AK1)
- [x] base-station serial protocol: `ME`, `DATA`, `MSG`, `QM`, `ACK`, `SENT`, and the `TX`/`QM`/`AK` commands
- [x] screen on the ESP32-S3 TFT: messaging screen, confirmation status, full-screen long messages, HELP alert, warning banner
- [x] v1.1: silent ping (30 s), LED bar = signal strength, confirmations for every message, red = HELP / blue = OK, CARRY weak-link alarm, fewer tasks and smaller memory use
- [x] v1.2: relay through other boxes + dedupe, confirmations over several hops, catch-up sync after out-of-range / reboot
- [x] each box's role is sent in its ping (the app shows C / D / B)
- [x] `NBR` neighbour reports for the map (sent inside the ping)
- [x] M0 (SAMD21) port: same sketch, plain loop instead of FreeRTOS (`minirtos.h`)
- [ ] CARRY/DEPLOY roles, chain numbering, beacons
- [ ] LED bar link meter and buzzer range warning
- [ ] quick messages with acknowledgement, and the base → box message panel
- [ ] watchdog
