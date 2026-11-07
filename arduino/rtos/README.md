# FreeRTOS firmware (work in progress)

The box firmware is moving to **FreeRTOS**, using the copy built into the Arduino core for
this board. You don't need to download FreeRTOS, and the separate FreeRTOS-LTS download should
**not** be added to this repo: it targets the Pico SDK/CMake toolchain and is 115 MB.

## Arduino IDE settings
- **Tools → Board →** Adafruit Feather RP2040 RFM
- **Tools → Operating System → FreeRTOS SMP** ← required, or it won't compile or run
- Libraries: **LoRa** (Sandeep Mistry), **Adafruit SHT4x**. The LED bar driver is built in (`ledbar.h`).

## Sketches
| Folder | What it is |
|---|---|
| `rtos_test/` | Small check of FreeRTOS on one box: two LEDs blinking at different rates (2 tasks), button → queue → buzzer, LED bar animation, and a per-task core/stack report every 5 s. No radio. |
| `rtos_main/` | Skeleton of the real firmware. One task per piece of hardware, joined by queues. It already does a PING/HELP button demo between boxes, CARRY/DEPLOY on the bar, signal strength on the bar, and sensor logging. Change `NODE_ID` for each box. |

`pins.h` and `ledbar.h` are copied into each sketch folder, because Arduino sketches can't
share files across folders. **When you change them, change both copies.**

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
| `sensor` | Wire (SHT41, OLED later) | a reading every 5 s; falls back to simulated values if the sensor is missing |
| `radio` | SPI + RFM95 | sends queued packets (3 s TX timeout, then radio restart) and reports received packets with RSSI/SNR |
| app (`loop`) | nothing directly | decides what happens. Roles, relay, link meter and protocol will be added here. |

## Rules
1. **Each piece of hardware is used by one task only.** Arduino libraries aren't thread-safe.
   An OLED on the STEMMA QT port must be drawn from the **sensor task**, because it shares Wire with the SHT41.
2. **Tasks talk through queues**, not shared variables.
3. **Wait with `vTaskDelay()` / `delay()`**, which let other tasks run. Never use busy loops.
   (`delayMicroseconds()` doesn't yield, so keep it to microseconds.)
4. **Print with `logf()`.** It takes a mutex so lines from different tasks don't get mixed together.

## Plan: features to move over from `../lora_sensor_node`, one at a time
- [ ] sensor packets + relay + dedupe (`sensornet.h`)
- [ ] base-station serial protocol (`ME`, `DATA`, `MSG`, `QM`, `NBR`…, see `PROTOCOL.md`)
- [ ] CARRY/DEPLOY roles, chain numbering, beacons
- [ ] LED bar link meter and buzzer range warning
- [ ] quick messages with acknowledgement, and the base → box message panel
- [ ] watchdog
