# Team 7 protocol reference

Two layers: what the boxes send each other **over LoRa**, and what a box and the
Pi/laptop say to each other **over USB serial** (115200 baud, one line per message).

## 1. Over the air (LoRa, between boxes)

Every packet = 4-byte RadioHead header `FF FF 00 00` + ASCII, comma-separated.
Fields 1–4 are the same for every type so relaying/dedupe works the same way:

```
<TYPE>,<src>,<seq>,<ttl>,<via>,...type-specific fields...
```
| Type | Rest of fields | Purpose |
|---|---|---|
| `SN1` | `temp_c,rh,flag` | sensor reading (flag R=real, S=simulated) |
| `BC1` | `role,chain` | beacon every ~2 s from deployed/base boxes (ttl=0, never relayed) — used for the link meter |
| `MS1` | `dst,text` | text message; `dst` = node id or `*` for everyone; text is last so it may contain commas |
| `QM1` | `dst,code` | quick message: `OK`, `HELP`, `FOUND`, `RETURN` |
| `AK1` | `dst,acked_seq` | "message received" confirmation sent back to `dst` |
| `NB1` | `role,chain,id:rssi;id:rssi;...` | neighbour report every ~10 s: who this box hears directly and how strongly (relayed to base, feeds the minimap) |

`role`: `B` base, `D` deployed relay, `C` carried. `chain`: base = 0, each drop = previous + 1, carried = -1.
`(src, seq)` identifies a message; duplicates are dropped; relays decrement `ttl`.

## 2. Box → Pi/laptop (serial lines the box prints)

Lines that start with one of these tags are machine-readable; anything else is a human log line.

| Line | Meaning |
|---|---|
| `ME,<id>,<role>,<chain>,<temp>,<rh>,<flag>` | who the attached box is + its own reading (boot and every ~5 s) |
| `DATA,<src>,<seq>,<temp>,<rh>,<flag>,<rssi>,<snr>,<via>,<heard_by>` | sensor reading received |
| `BCN,<src>,<role>,<chain>,<rssi>,<snr>` | beacon heard |
| `LINK,<tail_id>,<margin_db>,<state>` | carried box's link meter: state `OK` / `WARN` / `LOST` |
| `MSG,<src>,<seq>,<dst>,<rssi>,<snr>,<via>,<text>` | text received (text may contain commas) |
| `QM,<src>,<seq>,<dst>,<code>,<rssi>,<snr>,<via>` | quick message received |
| `ACK,<src>,<acked_seq>` | `src` confirmed receiving our message `acked_seq` |
| `NBR,<src>,<role>,<chain>,<id>:<rssi>;...` | neighbour report (who `src` hears, in dBm) — also tells the Pi `src`'s role |
| `SENT,<seq>,<dst>,<kind>,<text_or_code>` | the box transmitted something the Pi asked for |

## 3. Pi/laptop → box (commands, one per line)

| Command | Effect |
|---|---|
| `TX,<dst>,<text>` | send a text (`dst` = id or `*`); max 140 printable ASCII chars |
| `QM,<dst>,<code>` | send a quick message |
| `AK,<dst>,<seq>` | confirm we received message `seq` from `dst` |
