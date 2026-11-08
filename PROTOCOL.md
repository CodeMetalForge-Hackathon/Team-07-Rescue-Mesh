# Team 7 protocol reference

Two layers: what the boxes send each other **over LoRa**, and what a box and the
Pi/laptop say to each other **over USB serial** (115200 baud, one line per message).

## 1. Over the air (LoRa, between boxes)

Every packet = 4-byte RadioHead header `FF FF 00 00` + ASCII, comma-separated.
Fields 1–6 are the same for every type so relaying/dedupe works the same way.
`tag` = 16 hex digits, SipHash-2-4 with the team key (`key.h`) over `<TYPE>,<src>,<seq>,<rest>`.
Packets with a wrong tag are ignored. `ttl`/`via` are not covered, because relays change them:

```
<TYPE>,<src>,<seq>,<ttl>,<via>,<tag>,...type-specific fields...
```
| Type | Rest of fields | Purpose |
|---|---|---|
| `SN1` | `temp_c,rh,flag` | sensor reading (flag R=real, S=simulated) |
| `BC1` | `role,temp,rh,flag,boot,nbrs` | silent ping every ~30 s from every box. `boot` = 1 for the first 3 pings after power-on (asks neighbours to re-sync). `nbrs` = `id:rssi;id:rssi` of up to 4 boxes heard directly (feeds the map). Relayed like everything else. |
| `MS1` | `dst,text` | text message; `dst` = node id or `*` for everyone; text is last so it may contain commas |
| `QM1` | `dst,code` | quick message: `OK`, `HELP`, `FOUND`, `RETURN` |
| `AK1` | `dst,acked_seq,R\|O` | confirmation sent back to `dst`: `R` = received by a box (0.7–1.5 s after receiving; for `*` messages a box that hears another box's `R` first stays quiet), `O` = acknowledged by the base operator |

`role`: `B` base, `D` deployed relay, `C` carried. `chain` (serial lines only): `0` for the base, `-1` for every other box.
`(src, seq)` identifies a packet; duplicates are dropped.
`ttl` = how many more times it may be repeated (starts at 3). A box repeating it sets `via` to itself and `ttl - 1`.
A sender's **retry** (no confirmation after 5 s) adds 8 to `ttl` (so 8–11). Boxes that already have that message
don't show it again, but confirm it again and pass it on. Catch-up re-sends don't have the 8, so they don't trigger that.
`via == src` means the packet came straight from the box that wrote it.
Boxes keep the last 6 `MS1`/`QM1` and re-send them (same `src`, `seq`) when a neighbour returns or reboots.

## 2. Box → Pi/laptop (serial lines the box prints)

Lines that start with one of these tags are machine-readable; anything else is a human log line.

| Line | Meaning |
|---|---|
| `ME,<id>,<role>,<chain>,<temp>,<rh>,<flag>` | who the attached box is + its own reading (boot and every ~5 s) |
| `DATA,<src>,<seq>,<temp>,<rh>,<flag>,<rssi>,<snr>,<via>,<heard_by>` | sensor reading received |
| `BCN,<src>,<role>,<chain>,<rssi>,<snr>` | ping heard **directly** from `src` |
| `LINK,<tail_id>,<margin_db>,<state>` | (first firmware only, not sent by v1.2) carried box's link meter |
| `MSG,<src>,<seq>,<dst>,<rssi>,<snr>,<via>,<text>` | text received (text may contain commas) |
| `QM,<src>,<seq>,<dst>,<code>,<rssi>,<snr>,<via>` | quick message received |
| `ACK,<src>,<acked_seq>` | `src` confirmed receiving our message `acked_seq` |
| `NBR,<src>,<role>,<chain>,<id>:<rssi>;...` | neighbour report (who `src` hears, in dBm) — also tells the Pi `src`'s role |
| `SENT,<seq>,<dst>,<kind>,<text_or_code>` | the box transmitted something the Pi asked for |

## 3. Pi/laptop → box (commands, one per line)

| Command | Effect |
|---|---|
| `TX,<dst>,<text>` | send a text (`dst` = id or `*`); max 60 printable ASCII chars |
| `QM,<dst>,<code>` | send a quick message |
| `AK,<dst>,<seq>` | the operator acknowledges message `seq` from `dst` (e.g. a HELP): that box shows "HELP acknowledged - help is coming" |

## 4. Box ↔ its screen board (I2C, address 0x42)

The box sends text lines (`D,STATUS`, `D,MSG`, `D,SENT`, `D,ACK`, `D,NODE`, `D,WARN`; see `arduino/rtos/rtos_main/displink.h`).
Every 0.25 s it also reads 80 bytes back: `[id][len][line]`, where `line` is a `TX,...` or `QM,...` command typed on the
phone page (id 0 = nothing). It confirms with the two bytes `0x06,<id>`.
