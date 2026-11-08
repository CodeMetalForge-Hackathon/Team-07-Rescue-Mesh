# FreeRTOS firmware (work in progress)

The box firmware is moving to **FreeRTOS**, using the copy built into the Arduino core for
this board. You don't need to download FreeRTOS, and the separate FreeRTOS-LTS download should
**not** be added to this repo: it targets the Pico SDK/CMake toolchain and is 115 MB.

## Arduino IDE settings
- **Tools → Board →** Adafruit Feather RP2040 RFM
- **Tools → Operating System → FreeRTOS SMP** ← required, or it won't compile or run
- Libraries: **LoRa** (Sandeep Mistry), **Adafruit SHT4x**. The LED bar driver is built in (`ledbar.h`).
- Screen: a separate **ESP32-S3 Reverse TFT Feather** running `../display_esp32` (see below).

## Sketches
| Folder | What it is |
|---|---|
| `rtos_test/` | Small check of FreeRTOS on one box: two LEDs blinking at different rates (2 tasks), button → queue → buzzer, LED bar animation, and a per-task core/stack report every 5 s. No radio. |
| `rtos_main/` | The real firmware, being built up step by step. One task per piece of hardware, joined by queues. It already does OK/HELP quick messages, texts from the base shown on the screen (confirmed back automatically), screen pages, CARRY/DEPLOY, sensor broadcasts, and the base-station serial protocol. Set `NODE_ID` (and `IS_BASE` on the Pi's box) for each box. |

`pins.h` and `ledbar.h` are copied into each sketch folder, because Arduino sketches can't
share files across folders. **When you change them, change both copies.**

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

## Base box
Set `IS_BASE 1` and plug the box into the Pi or a laptop running `pi/base_station.py`.
- The box accepts `TX,<dst>,<text>`, `QM,<dst>,<code>` and `AK,<dst>,<seq>` from the app.
- It prints `ME`, `DATA`, `MSG`, `QM`, `ACK` and `SENT` lines (see `PROTOCOL.md`).
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
- [ ] relay through other boxes + dedupe (for chains longer than one hop)
- [x] each box's role is sent in its ping (the app shows C / D / B)
- [ ] `NBR` neighbour reports for the map
- [ ] M0 (SAMD21) port
- [ ] CARRY/DEPLOY roles, chain numbering, beacons
- [ ] LED bar link meter and buzzer range warning
- [ ] quick messages with acknowledgement, and the base → box message panel
- [ ] watchdog
