# ANT+ profiles this project implements or decodes

Checked by: `scripts/check_profile_registry.py` — it cross-checks the device
type and period table below against `tools/ant_pages.py` and against
`docs/profile-registry.md`. The layout tables themselves are pinned by
`tools/test_ant_pages.py`, which round-trips every encoder through its decoder:

```
C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe -m unittest discover -s tools -p "test_*.py"
```

Facts and tables only. Every layout here is derived from the encoders and
decoders in `tools/ant_pages.py` and the cases in `tools/test_ant_pages.py`,
which are the implementations this project actually ships. No prose is taken
from any ANT+ device profile document.

---

## Device types

<!-- radiant-registry: ant-plus-types -->

| Type | Name | Period | Implemented in `tools/ant_pages.py` |
|---|---|---|---|
| `0x0B` | Bicycle Power | 8182 | pages `0x10`, `0x11`, `0x12`, `0x20` |
| `0x11` | Fitness Equipment (FE-C) | — | no |
| `0x78` | Heart Rate | — | no |
| `0x79` | Bike Speed and Cadence, combined | 8086 | the single page, which has no page number |
| `0x7A` | Bike Cadence | 8102 | period only |
| `0x7B` | Bike Speed | 8118 | period only |

Period is in counts of 1/32768 s: 8182 is ~4.0049 Hz, 8086 is ~4.05 Hz. A `—`
means this project has not implemented the type and does not record a number it
has not verified against its own code.

RF channel for all of them is `0x39` = 57 = 2457 MHz, network 0, ANT+ public
network key.

### Two device-type numbers worth not confusing

- **`0x11` is the Fitness Equipment device type and also the Wheel Torque page
  number under device type `0x0B`.** Device types and page numbers are separate
  namespaces that happen to overlap numerically, and a decoder that dispatches
  on the wrong one produces a valid-looking result.
- **`0x79`, `0x7A` and `0x7B` are three different sensors**, not three pages of
  one. The combined sensor is the only one that reports cadence and speed
  together.

---

## Bicycle Power, device type `0x0B`

### Page `0x10` — Standard Power Only

| Byte | Field | Encoding |
|---|---|---|
| 0 | page number | `0x10` |
| 1 | update event count | u8 accumulator, +1 per power event, wraps at 256 |
| 2 | pedal power balance | u8, `0xFF` = not reported |
| 3 | instantaneous cadence | u8 rpm, `0xFF` = not reported |
| 4..5 | accumulated power | u16 LE accumulator, watts, wraps at 65536 |
| 6..7 | instantaneous power | u16 LE, watts |

The accumulated/instantaneous pairing on one page is the canonical example of
ANT+'s loss tolerance: the instantaneous value is a convenience, the
accumulator is what survives a lost packet.

### Pages `0x11` and `0x12` — Wheel Torque and Crank Torque

| Byte | Field | Encoding |
|---|---|---|
| 0 | page number | `0x11` (wheel) or `0x12` (crank) |
| 1 | update event count | u8 accumulator, +1 per revolution |
| 2 | wheel or crank ticks | u8 accumulator, wraps at 256 |
| 3 | instantaneous cadence | u8 rpm, `0xFF` = not reported |
| 4..5 | accumulated period | u16 LE accumulator, 1/2048 s, wraps at 65536 |
| 6..7 | accumulated torque | u16 LE accumulator, 1/32 N.m, wraps at 65536 |

Four accumulators, all wrapping independently.

Average power between two samples:

```
avg_torque_Nm  = delta_torque / 32 / delta_event
avg_omega_rads = 2*pi * delta_event * 2048 / delta_period
power_W        = avg_torque_Nm * avg_omega_rads
               = delta_torque * 2*pi * 64 / delta_period
```

`delta_event` cancels. It is still needed, but only to tell "no new event" from
"a new event with zero torque"; both otherwise present as
`delta_period == 0`. `ant_pages.power_from_torque()` raises rather than
returning zero for that case.

**Pages `0x11` and `0x12` are independent accumulator series that share a
channel.** A sensor emitting both advances two unrelated sets. Differencing one
against the other produces a plausible number rather than an error, which is
why `tools/ant_verify.py` keeps one baseline per page number rather than one
per channel.

### Page `0x20` — Crank Torque Frequency

| Byte | Field | Encoding |
|---|---|---|
| 0 | page number | `0x20` |
| 1 | update event count | u8 accumulator |
| 2..3 | slope | u16 **BE**, 1/10 N.m/Hz |
| 4..5 | time stamp | u16 **BE** accumulator, 1/2000 s, wraps at 65536 |
| 6..7 | torque ticks stamp | u16 **BE** accumulator, wraps at 65536 |

**This page is big-endian. Every other multi-byte field in every other page
here is little-endian.** That is trap one of two.

```
elapsed_s = delta_time / 2000
torque_Hz = delta_ticks / elapsed_s - offset_Hz
torque_Nm = torque_Hz / (slope / 10)
omega     = 2*pi * delta_event / elapsed_s
power_W   = torque_Nm * omega
```

Unlike the torque pages, `delta_event` does **not** cancel here: torque comes
from a tick frequency and angular velocity from the event count, so both are
needed.

---

## Bike Speed and Cadence combined, device type `0x79`

| Byte | Field | Encoding |
|---|---|---|
| 0..1 | cadence event time | u16 LE accumulator, 1/1024 s, wraps at 65536 |
| 2..3 | cumulative crank revolutions | u16 LE accumulator, wraps at 65536 |
| 4..5 | speed event time | u16 LE accumulator, 1/1024 s, wraps at 65536 |
| 6..7 | cumulative wheel revolutions | u16 LE accumulator, wraps at 65536 |

**There is no page-number byte.** Byte 0 is the low half of the cadence event
time. That is trap two of two, and it is why `ant_pages.decode()` takes a
`device_type` argument that looks redundant: dispatching on `payload[0]` the way
every other profile does will decode this page as whatever page that byte
happens to name. A cadence event time of `0x2010` puts `0x10` in byte 0 and
decodes as a Standard Power page.

The second consequence is quieter: bytes `[4..7]` alone are a valid-looking
speed-only page, so a receiver that reads only those silently drops the cadence
half and reports no error.

```
cadence_rpm = delta_revs * 1024 * 60 / delta_event_time
speed_mps   = delta_revs * wheel_circumference_m * 1024 / delta_event_time
```

---

## Common pages

### Page `0x50` (80) — Manufacturer's Information

| Byte | Field | Encoding |
|---|---|---|
| 0 | page number | `0x50` |
| 1..2 | reserved | `0xFF` |
| 3 | hardware revision | u8 |
| 4..5 | manufacturer id | u16 LE |
| 6..7 | model number | u16 LE |

### Page `0x51` (81) — Product Information

| Byte | Field | Encoding |
|---|---|---|
| 0 | page number | `0x51` |
| 1 | reserved | `0xFF` |
| 2 | supplemental software revision | u8, `0xFF` = not used |
| 3 | main software revision | u8 |
| 4..7 | serial number | u32 LE, `0xFFFFFFFF` = not supplied |

### Page `0x52` (82) — Battery Status

| Byte | Field | Encoding |
|---|---|---|
| 0 | page number | `0x52` |
| 1 | reserved | `0xFF` |
| 2 | battery identifier | u8, `0x00` when there is only one battery |
| 3..5 | cumulative operating time | u24 LE |
| 6 | fractional battery voltage | u8, 1/256 V |
| 7 | coarse voltage (bits 3..0), status (bits 6..4), time resolution (bit 7) | bit 7: 0 = 16 s units, 1 = 2 s units |

Voltage is `coarse + fractional/256`.

---

## The common-page cadence trap

Generic ANT+ guidance says common pages must appear at least once per **65**
messages. That number is not what a certified sensor does, and a simulator
built on it sends common pages roughly twice as often as the profile requires —
spending airtime and battery to be less realistic.

The value this project uses comes from sdk-ant's own certified bicycle power
profile, recorded in `tools/ant_pages.py` at `COMMON_PAGE_INTERVAL`:

| Constant | Value |
|---|---|
| `COMMON_PAGE_80_INTERVAL` (sdk-ant) | 119 |
| `COMMON_PAGE_81_INTERVAL` (sdk-ant) | 120 |
| stated minimum | interleave every **121 messages** |
| `ant_pages.COMMON_PAGE_INTERVAL` | 120 |

So the pattern is: 119 data messages, page 80, page 81, repeat — a 121-message
cycle carrying two common pages. `docs/radiant-telemetry.md` puts the RadiANT
descriptor set on the same cadence for the same reason.

---

## Accumulator semantics

Every field marked "accumulator" above is a running total in a fixed width that
is **meant** to wrap. The rules, which `tools/test_ant_pages.py` pins:

1. Difference two readings **in the accumulator's own width**:
   `delta_u8(now, before) = (now - before) mod 256`,
   `delta_u16(now, before) = (now - before) mod 65536`.
   A receiver that promotes to a wider signed type before subtracting is
   correct for hours and then produces one large negative delta per wrap.
2. `65500 -> 100` in 16 bits is a delta of **136**, not `-65400`.
3. One baseline per page number per device number. Two power meters share
   device type `0x0B`; merging their streams produces one analysis of two
   unrelated series.
4. An accumulator that advances with no new event is a violation, not a data
   point. `tools/ant_verify.py` counts those and fails on a non-zero count.

## Invalid-value sentinels

| Width | Sentinel | Constant |
|---|---|---|
| u8 | `0xFF` | `INVALID_U8` |
| u16 | `0xFFFF` | `INVALID_U16` |
| u32 | `0xFFFFFFFF` | `INVALID_U32` |

A receiver that treats these as numbers reports 255 rpm or 65535 W. The
encoders in `tools/ant_pages.py` take `None` for "not reported" so a caller
never has to remember which sentinel a given page uses.

---

## Capture file format

`tools/ant_pages.py` also owns the `.antcap` line format, because the captures
are replayed through the C decoders in the sibling `zephyr_aerosense` project:

```
# ant capture v1: <seconds> <device_type> <device_number> <8 payload bytes>
12.345678 0B 3A17 1006ff5039300064
```

Deliberately not JSON: one `sscanf` on the C side against a parser dependency.
The device number is not decoration — two sensors can share a device type, and
merging their streams is the same mistake as rule 3 above, made in the analysis
tool instead of the firmware.
