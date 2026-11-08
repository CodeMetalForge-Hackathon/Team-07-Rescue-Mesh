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
- **Pages** (long press **button 2** on the RP2040 box to switch):
  - **MAIN**: header (name, mode in colour, temperature), the newest message (3 lines), the previous message, and the boxes heard with signal bars
  - **NEARBY**: every box heard, with bars, RSSI and age (grey when nothing has been heard for 30 s)
  - **STATUS**: name, mode, temperature, humidity, boxes heard, message count
- A **long message** fills the whole screen for 20 s.
- A **HELP** from another box shows a blinking **red screen** for 20 s, or until the next message.
- A red **"WAITING FOR BOX" / "NO LINK TO BOX"** bar means nothing has arrived from the RP2040 for 15 s. Check the SDA/SCL/GND wires and that the RP2040 is running `rtos_main`.
- Text uses size 2 (about 1.6 mm tall). That's the smallest that's readable on a 1.14" screen.

## Buttons and switch
| Input | Does |
|---|---|
| Button 1 short | quick message **OK** |
| Button 2 short | quick message **HELP**: the base app shows a red alert and other boxes beep 3× |
| Button 2 hold (1.5 s) | next screen page |
| Button 1 hold | placeholder (FOUND / calibrate later) |
| Switch | CARRY / DEPLOY (flip `DEPLOY_LEVEL` if it's backwards) |

## Base box
Set `IS_BASE 1` and plug the box into the Pi or a laptop running `pi/base_station.py`.
- The box accepts `TX,<dst>,<text>`, `QM,<dst>,<code>` and `AK,<dst>,<seq>` from the app.
- It prints `ME`, `DATA`, `MSG`, `QM`, `ACK` and `SENT` lines (see `PROTOCOL.md`).
- Texts to `*` go to everyone; texts to a box ID go to that box only.
- A field box confirms every text it receives, so the app shows ✓✓ delivered.
- When the base acknowledges a HELP, that box shows "BASE got your msg".
- The Serial Monitor can't be open while `base_station.py` is running, because both need the port.

## Task layout (`rtos_main`)
```
buttons task (prio 3) --events--> app = loop() --UI commands--> UI task (prio 2): buzzer, button LEDs, LED bar
sensor  task (prio 1) --events-->      |        --packets-----> radio task (prio 2): LoRa send/receive
                                       ^------------events------------------------------'
```
| Task | Owns | Does |
|---|---|---|
| `buttons` | both buttons, switch | 10 ms polling, debounce, short press / long press (1.5 s) / switch events |
| `ui` | buzzer, button LEDs, LED bar | beeps, blink modes, bar levels; temporary states time out back to the base state |
| `i2c` | Wire: SHT41 + ESP32 screen | a reading every 5 s (simulated if the sensor is missing); sends screen messages to the ESP32 |
| `serial` | USB serial input | reads command lines from the Pi app and hands them to the app |
| `radio` | SPI + RFM95 | sends queued packets (3 s TX timeout, then radio restart) and reports received packets with RSSI/SNR |
| app (`loop`) | nothing directly | decides what happens. Roles, relay, link meter and protocol will be added here. |

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
- [x] screen on the ESP32-S3 TFT: main / nearby / status pages, full-screen long messages, HELP alert
- [ ] relay through other boxes + dedupe (for chains longer than one hop)
- [ ] send each box's role over the radio (the app shows field boxes as role "?" for now) and `NBR` reports for the map
- [ ] CARRY/DEPLOY roles, chain numbering, beacons
- [ ] LED bar link meter and buzzer range warning
- [ ] quick messages with acknowledgement, and the base → box message panel
- [ ] watchdog
