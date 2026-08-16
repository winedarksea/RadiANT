# RadiANT device profiles

Checked by: `scripts/check_profile_registry.py`, run with the NCS toolchain's
Python interpreter (there is no system Python in this project). It cross-checks the device type and period tables below against
`tools/ant_pages.py` and `docs/profile-registry.md`, and the page maps of the
RadiANT device types against the registry's page rows. The ANT+ layout tables
are pinned by `tools/test_ant_pages.py`, which round-trips every encoder through
its decoder:

```
python -m unittest discover -s tools -p "test_*.py"
```

This document absorbs the former `docs/ant-plus-profiles.md`. It is the one
place to look up **what a device profile is on this project**, whether that
profile is Garmin's, this project's, or a schema published on the generic
telemetry envelope. `docs/profile-registry.md` still owns *allocation* — who
claimed which number and when — and `docs/radiant-telemetry.md` still owns the
`0x60` envelope itself.

Facts and tables only. Every ANT+ layout here is derived from the encoders and
decoders in `tools/ant_pages.py` and the cases in `tools/test_ant_pages.py`,
which are the implementations this project actually ships, except where a
section says otherwise and names its source. No prose is taken from any ANT+
device profile document.

---

## 1. The three vehicles, and how a profile earns one

A new kind of sensor can reach a receiver three ways on this project. They are
not equivalent, they cost wildly different amounts, and picking the expensive
one by default is the mistake this section exists to prevent.

| Vehicle | What it costs | When it is right |
|---|---|---|
| **A `0x60` schema recipe** | A section in this document. No allocation, no new codec, no receiver change | The default. Anything whose fields are not yet settled, and anything a receiver does not need to recognise before decoding |
| **An additive page on an ANT+ device type** | A page number that must be free in that profile, plus the ADR 0008 rules | The sensor is already an ANT+ device type and needs to say something that type has no field for |
| **A RadiANT device type** | A registry claim, a full 256-page map, the `0x20-0x2F` security reservation, a page-map table here, and a change to `check_pagemap()` | A receiver must be able to **recognise and name the node without decoding it** |

### What a RadiANT device type actually is, and it is not a byte layout

**A RadiANT profile device type is the `0x60` telemetry envelope with a pinned
mandatory schema.** Same descriptor, same data pages, same counter in byte `[1]`,
same trailing tag space, same field vocabulary. The only things a profile adds
are a device type number and a *required field set*.

That is the whole design, and it is worth stating plainly because the obvious
alternative — invent an 8-byte layout per profile, the way every ANT+ profile
does — was drafted first and is worse in four ways:

- **Zero new codec.** `src/profiles/profile_telemetry.c` and its mirror in
  `tools/ant_pages.py` already encode and decode every page a profile needs. A
  new profile adds a table, not a parser.
- **The security layer applies for free.** `X_CONF` and `X_AUTH` need byte `[1]`
  to be a counter and the trailing byte to be tag space. A bespoke layout that
  put data in either — and every draft layout did — is permanently unprotectable.
- **A receiver that has never heard of the profile still decodes it**, because
  the descriptor is self-describing. It gets typed, scaled, unit-bearing values
  and can bridge them, without a line of profile-specific code.
- **The profile can grow without a format break**, because field offsets are
  announced rather than fixed.

So the device type is a **discovery label**: it is what lets a head unit put
"Glucose monitor" in a pairing list without opening a channel and waiting a full
descriptor cycle. That is the entire test for whether a profile deserves one.

It buys one thing more, and it is the half that is easy to miss. **A profile can
pin the two schedule opt-ins that a bare `0x60` node leaves per-node**, so the
registry's `LR PHY` and `Adaptive freq` columns read `no` instead of `per-node`.
Both profiles below do. That matters because those columns are exactly what a
consumer has to know *before* it opens a channel — a long-range or
frequency-hopping node splits the receiver's merged RF 57 window the way a
separate network would — and `per-node` means "read the descriptor to find out".
A profile type is therefore cheaper to budget for than the generic type it is
built from, which is the same argument as the pairing list made about radio
scheduling instead of names.

### The rules every RadiANT profile obeys

These are `docs/radiant-telemetry.md`'s, restated because every draft profile in
this project's history has broken at least one of them:

1. **Byte `[1]` is the event counter.** Never a field.
2. **The trailing byte is tag space** whenever `X_AUTH` is on. A schema places
   no field there.
3. **Anything integrable is published as an accumulating field**, and the
   accumulator is authoritative. The instantaneous value is a convenience.
4. **A period is a number, not a rate.** "4 Hz" is not a channel period; 8192
   counts of 1/32768 s is. A receiver whose period does not match cannot open
   the channel at all, and nothing reports the period as the reason.
5. **Every field declares its invalid value**, and it is the sentinel for the
   field's width — `0xFF`, `0xFFFF`, `0xFFFFFFFF` — unless the section says
   otherwise and says why.
6. **A value a receiver can compute is not a field.** Trend arrows, composite
   indices and derived densities are the receiver's job; the node publishes what
   it measured.

---

## 2. Where each candidate profile landed

The eight profiles drafted in the former `docs/new_device_profiles.md`, and what
the evidence did to them. The "considered and not pursued" entries are in
section 7 with their reasons, because a registry that records only its successes
invites the same proposal again in a year.

| Candidate | Outcome | Why |
|---|---|---|
| E-bike / smart commuter | **Not pursued** → ANT+ LEV `0x14` | LEV already carries battery SoC, assist level, remaining range, motor and battery temperature, lights, errors, gears and a command page |
| Electronic key / immobilizer | **`0x60` recipe**, redesigned | The lock state is one vocabulary field and the command page already exists. The auto-lock-on-signal-loss premise is refused outright |
| Physiological temperature | **`0x60` recipe** | ANT+ Environment `0x19` cannot host it, and the reason is structural |
| Air quality | **`0x60` recipe** | Every field is already in the vocabulary. The composite AQI and VOC index are dropped |
| Wind / anemometer | **`0x60` recipe** | The registry already ruled this ships on `0x60` first, and the fields are still unsettled |
| Water sports / paddling | **Not pursued** | Shipping products already solve it on `0x0B` and `0x7A` |
| Smart fan | **RadiANT device type `0x62`** | A head unit must discover and name an actuator before commanding it |
| CGM | **RadiANT device type `0x61`** | Same discovery argument, plus a receiver may need to gate on the profile before displaying a reading |

---

## 3. ANT+ profiles this project implements or decodes

<!-- radiant-registry: ant-plus-types -->

| Type | Name | Period | Implemented in `tools/ant_pages.py` |
|---|---|---|---|
| `0x0B` | Bicycle Power | 8182 | pages `0x10`, `0x11`, `0x12`, `0x20` |
| `0x10` | Controls | 8192 | pages `0x10`, `0x49` (command surface only) |
| `0x11` | Fitness Equipment (FE-C) | 8192 | pages `0x10`, `0x11`, `0x12`, `0x13`, `0x30`–`0x33`, `0x36`, `0x37`, `0x46`, `0x47` |
| `0x14` | Light Electric Vehicle (LEV) | — | no |
| `0x19` | Environment | 65535 | no — decoded in C only, `src/profiles/profile_env.c` |
| `0x1E` | Running Dynamics | 4096 | pages `0x00`, `0x01`, `0x10`, `0x20`, `0x4A` |
| `0x29` | Tracker (Asset Tracker) | 2048 | pages `0x01`, `0x02`, `0x03`, `0x10`, `0x11`, `0x20` |
| `0x78` | Heart Rate | 8070 | pages `0x00`, `0x01`, `0x02`, `0x03`, `0x04` |
| `0x79` | Bike Speed and Cadence, combined | 8086 | the single page, which has no page number |
| `0x7A` | Bike Cadence | 8102 | period only |
| `0x7B` | Bike Speed | 8118 | period only |
| `0x7C` | Stride-Based Speed and Distance | 8134 | pages `0x01`, `0x02`, `0x03`, `0x10`, `0x16` |
| `0x7F` | Core Temperature | — | no |

Period is in counts of 1/32768 s: 8182 is ~4.0049 Hz, 8086 is ~4.05 Hz, 8070 is
~4.06 Hz. A `—` means this project has not implemented the type and does not
record a number it has not verified against its own code.

**`0x14` and `0x7F` carry `—` even though their periods are known**, and that is
the rule working rather than an oversight. Both numbers come from documents, not
from this project's code — LEV's is 8192, Core Temperature's is 8192 — and they
are stated in sections 3.6 and 3.8 with their sources. The column means
"verified here", and widening it to mean "read somewhere" would make every other
cell in it worth less.

**`0x11` and `0x19` used to carry `—` and no longer do.** Both are now
transcribed from their primary documents into named constants in
`src/profiles/` (`PROFILE_FEC_PERIOD`, `PROFILE_ENV_PERIOD_4_HZ`,
`PROFILE_ENV_PERIOD_0P5_HZ`) and both reach the acquisition table in
`apps/dongle_thread/src/self_channels.c`, so the numbers are this project's own
rather than something read somewhere.

**`0x11`'s last column changed with `apps/treadmill` and `0x19`'s has not.**
FE-C now has an encoder (`src/profiles/profile_fec_tx.c`) *and* a Python mirror,
so `tools/ant_pages.py` is a genuine second opinion about those bytes and the
three-copy vocabulary rule applies to `0x11` from here on. Environment `0x19`
is still C-only and still has one copy. **`0x19`'s period cell is its default
and not its whole story** — see the permitted-periods table immediately below,
and §3.7.

**`0x7C` is new and was not implemented anywhere in this tree before
`apps/treadmill`.** Its period, 8134, is close enough to `0x78`'s 8070 and
`0x11`'s 8192 to read as a typo in a diff, and a receiver told the wrong one
never opens the channel with nothing anywhere naming the period as the cause.
See §3.17.

### Permitted channel periods

The table above records each type's **default**. Three of them permit more than
one rate. The heart-rate and bicycle-power sets live in `tools/ant_pages.py` as
`HRM_PERIODS` and `BPWR_PERIODS`, cross-checked against
`docs/profile-registry.md` by `scripts/check_profile_registry.py`; Environment's
pair lives in `src/profiles/profile_env.h` and has no Python mirror to be
checked against.

| Type | Rate | Period | Approx. |
|---|---|---|---|
| `0x78` | standard (default) | 8070 | ~4.06 Hz |
| `0x78` | half | 16140 | ~2.03 Hz |
| `0x78` | quarter | 32280 | ~1.02 Hz |
| `0x0B` | standard (default) | 8182 | ~4.005 Hz |
| `0x19` | low power (profile default) | 65535 | ~0.5 Hz |
| `0x19` | full rate | 8192 | 4 Hz |

**`0x19` is the only type here where the two rates are not a display's choice**,
and it is the one that forced `apps/dongle_thread/src/self_channels.c` to search
a *table* of `{device type, period}` rows instead of one hard-wired pair. Both
rates are legal for a master, page 0's transmission-info field is where a sensor
declares which it uses — and page 0 arrives on the channel, which does not exist
until the period already matches. A slave fixed at 8192 never hears a 0.5 Hz
sensor say it is one, so the only way to acquire an unknown-rate Environment
sensor is to try both. Both `0x19` rows also carry §5.1's recommended **45 s**
search timeout rather than ANT's default 25 s, because at one transmission every
two seconds a 25 s window is a thin sample of a slow master.

**The period is not like the other settings.** A receiver skips a page number it
does not know; a receiver whose period does not match the sensor's cannot open
the channel at all, and nothing on either side reports the period as the reason.
That asymmetry is why the permitted sets are enumerated and checked rather than
left as a number in a constant somewhere. No reduced-rate variant is recorded for
`0x0B`: this project has not verified one against its own code, and the rule here
is that a number that has not been verified does not get written down.

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

### 3.1 Bicycle Power, device type `0x0B`

#### Page `0x10` — Standard Power Only

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

**Page `0x10` is now decoded in C as well as encoded**. The decoder lives
in `src/profiles/profile_power_decode.c` and **not** in `profile_power.c`, which
is a link-dependency decision rather than a filing one: `profile_power.c` is the
master — page rotation, the ANT+ compatibility layer and RadiANT beacon
insertion — so it reaches `profile_sched.c`, `profile_compat.c` and through it
`radiant_sec`. Linking that into a receive-only dongle to get one decoder would
put a power-meter *transmitter* into an image whose whole job is to listen, by a
linker decision nobody reviewed. The header is shared; only `.c` files create
link dependencies. Written from the primary independently of the encoder next
door, so that `test_power_adapter.c`'s round-trip case is a check rather than a
tautology.

**Accumulated power is watt-samples, not joules**, and that distinction is what
`radiant/src/bridge/radiant_power_adapter.c` exists to handle honestly: it
integrates instantaneous watts against the bridge's own `t_us` to produce
`0x30` energy, rather than relabelling a running sum of watt samples as joules.

#### Pages `0x11` and `0x12` — Wheel Torque and Crank Torque

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

#### Page `0x20` — Crank Torque Frequency

| Byte | Field | Encoding |
|---|---|---|
| 0 | page number | `0x20` |
| 1 | update event count | u8 accumulator |
| 2..3 | slope | u16 **BE**, 1/10 N.m/Hz |
| 4..5 | time stamp | u16 **BE** accumulator, 1/2000 s, wraps at 65536 |
| 6..7 | torque ticks stamp | u16 **BE** accumulator, wraps at 65536 |

**This page is big-endian. Every other multi-byte field in every other page
here is little-endian.** That is trap one of three.

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

### 3.2 Heart Rate, device type `0x78`

Two things about this profile shape everything else in this document and the
compat layer both.

**Byte 0's high bit is not part of the page number.** That is trap three of
three. It is the page-change toggle, flipped every four messages, which tells a
receiver the sensor has more than one page and it has now seen the set. Page
numbers here are therefore **7-bit — `0x00` to `0x7F`** — and nothing at or above
`0x80` is expressible at all. That is why the manufacturer-private range every
other profile uses (`0xF0`-`0xFF`) is unreachable here, and it is the constraint
that decided the RadiANT compat allocation in `docs/profile-registry.md`.

**Bytes `[4..7]` are the same on every page.** The page number only says what
the three bytes in between mean. A page that changed those four bytes would not
be a new page; it would be a broken sensor.

| Byte | Field | Encoding |
|---|---|---|
| 0 | bits 6..0 page number, bit 7 page-change toggle | toggle flips every 4 messages |
| 1..3 | page-specific | see below |
| 4..5 | heartbeat event time | u16 LE accumulator, 1/1024 s, wraps at 65536 |
| 6 | heartbeat count | u8 accumulator, wraps at 256 |
| 7 | computed heart rate | u8 bpm, **`0` = no reading** |

**`0` and not `0xFF` means "no reading" here**, which is the one place in this
document where the invalid-value table below does not apply. A decoder that
reached for `INVALID_U8` reports a plausible 255 bpm, and `0xFF` on this byte is
a real 255 bpm rather than a sentinel.

Bytes `[1..3]`, per page:

| Page | Kind | `[1]` | `[2]` | `[3]` |
|---|---|---|---|---|
| `0x00` | main | `0xFF` | `0xFF` | `0xFF` |
| `0x01` | background, **optional** | cumulative operating time, u24 LE, 2 s units | | |
| `0x02` | background, **required** | manufacturer id, u8 | serial number, **upper** 16 bits, u16 LE | |
| `0x03` | background, **required** | hardware version, u8 | software version, u8 | model number, u8 |
| `0x04` | main | manufacturer-specific, `0xFF` when unused | previous heartbeat event time, u16 LE, 1/1024 s | |

**Pages 2 and 3 are the two pages this profile document makes mandatory**
("must be implemented by all manufacturers"); page 1 is left to the
manufacturer's discretion. Nothing else in the profile carries a `shall` this
strong, which is why it is worth stating as a table property rather than prose
a reader can skim past.

Page `0x02`'s manufacturer id is **8 bits**, where common page 80's is 16. They
are different fields of different widths and a decoder that shares one struct
between them truncates silently.

**Page 2's serial field is the *upper* 16 bits of a 32-bit serial number, and
the ANT device number supplies the *lower* 16 bits — not the other way
round.** The primary spec (§5.3.4.3) is explicit about which half is which:
"The 32-bit serial number [is] comprised of the upper serial number (most
significant 16 bits) and the device number (least significant 16 bits)." A
field named or documented as the serial's low half, or code that concatenates
`(device_number << 16) | page2_field`, produces a serial number that is
byte-swapped at the 16-bit level — plausible-looking and wrong. The correct
construction is `(page2_field << 16) | device_number`.

Page `0x04` carries two event times on one page, which is what lets a receiver
compute an R-R interval from a single packet instead of differencing two. That
is why a modern strap sends it as its main page and `0x00` in the background
rotation rather than the other way round.

```
bpm = delta_beats * 1024 * 60 / delta_event_time
```

The computed-heart-rate byte is the sensor's own answer and is a convenience;
the accumulator pair is what survives a lost packet. Same division of labour as
accumulated versus instantaneous power.

---

### 3.3 Bike Speed and Cadence combined, device type `0x79`

| Byte | Field | Encoding |
|---|---|---|
| 0..1 | cadence event time | u16 LE accumulator, 1/1024 s, wraps at 65536 |
| 2..3 | cumulative crank revolutions | u16 LE accumulator, wraps at 65536 |
| 4..5 | speed event time | u16 LE accumulator, 1/1024 s, wraps at 65536 |
| 6..7 | cumulative wheel revolutions | u16 LE accumulator, wraps at 65536 |

**There is no page-number byte.** Byte 0 is the low half of the cadence event
time. That is trap two of three, and it is why `ant_pages.decode()` takes a
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

### 3.4 Common pages

#### Page `0x50` (80) — Manufacturer's Information

| Byte | Field | Encoding |
|---|---|---|
| 0 | page number | `0x50` |
| 1..2 | reserved | `0xFF` |
| 3 | hardware revision | u8 |
| 4..5 | manufacturer id | u16 LE |
| 6..7 | model number | u16 LE |

#### Page `0x51` (81) — Product Information

| Byte | Field | Encoding |
|---|---|---|
| 0 | page number | `0x51` |
| 1 | reserved | `0xFF` |
| 2 | supplemental software revision | u8, `0xFF` = not used |
| 3 | main software revision | u8 |
| 4..7 | serial number | u32 LE, `0xFFFFFFFF` = not supplied |

#### Page `0x52` (82) — Battery Status

| Byte | Field | Encoding |
|---|---|---|
| 0 | page number | `0x52` |
| 1 | reserved | `0xFF` |
| 2 | battery identifier | u8, `0x00` when there is only one battery |
| 3..5 | cumulative operating time | u24 LE |
| 6 | fractional battery voltage | u8, 1/256 V |
| 7 | coarse voltage (bits 3..0), status (bits 6..4), time resolution (bit 7) | bit 7: 0 = 16 s units, 1 = 2 s units |

Voltage is `coarse + fractional/256`.

**Page 82 is implemented and every profile in this document gets it for free.**
`profile_common_82()` in `src/profiles/profile_common.c`, scheduled through
`common_82_every`, mirrored as `ant_pages.encode_common_82`. **No profile in
this document spends a payload byte on battery state of charge**, because a page
every ANT+ receiver already understands carries it.

#### Page `0x54` (84) — Subfield Data

Decoded. `profile_common_decode_84()` in `src/profiles/profile_common.c`,
surfaced onto the sample bus by `radiant/src/bridge/radiant_common_adapter.c`.
Derived from the **primary** document — `ANT+ Common Data Pages`, D00001198
Rev 3.1, §4 Table 4-1, §6.13 Tables 6-16 and 6-17, and the §6.13.1 worked
example — which is a different provenance from pages 80/81/82 above, and the
two are kept separate on purpose so a clean-room audit does not have to guess
which claim covers which function.

##### The keying rule, which is the architectural point

**Common pages are keyed on the channel's transmission type, not on the device
type.** Table 4-1 splits the page-number byte into ranges, and says of
`0x40`–`0x5D` verbatim:

> Common Data Pages
> The common data pages have their formats defined. Use of these pages is
> defined by the transmission type of the ANT channel parameter. All unused
> pages in this section are undefined and are not to be used.

| Page number | Row | Keyed on |
|---|---|---|
| `0x00`–`0x3F` (0–63) | ANT+ Alliance Device Type Specific | device type |
| `0x40`–`0x5D` (64–93) | **Common Data Pages** | **transmission type** |
| `0x5E`–`0x6F` (94–111) | Reserved for future use | — |
| `0x70`–`0x7F` (112–127) | Manufacturer specific, toggle-bit profiles | device type |
| `0x80`–`0xDF` (128–223) | Reserved for future use | — |
| `0xE0`–`0xFF` (224–255) | Manufacturer specific | device type |

So page 84 is legal on a heart-rate strap, a power meter or a bike light, and a
decoder chosen from the binding's device type would never see it. That single
sentence decides the shape of the receive path:
`apps/dongle_thread/src/rx_tap.c` tests the common range **before** it
dispatches on device type, hands the payload to the common adapter and
*returns* — it does not fall through, because
`radiant_hr_adapter_decode()` reads bytes 4..7 of any page it is given and
would publish page 84's humidity as a heart rate. It is the one exception to
that file's "never dispatch on the payload" rule, and it is an exception
because the specification says the payload is what carries the keying here.
The range test is the whole range and not `== 0x54`: what Table 4-1 removes
from device-type keying is `0x40`–`0x5D`, so a profile adapter must not see any
of it. `PROFILE_COMMON_PAGE_RANGE_LAST` is `0x5D` and not `0x5E` — the table's
own decimal column says 64–93.

The consequence is that **every bound channel now feeds a second decoder on
every broadcast**, so one source has two producers. That is why
`radiant/src/bridge/radiant_bridge.h` reserves a `field_id` block: `0x00`–`0x1F`
for the bound profile's own adapter, `0x20`–`0x3F` for common pages (one id per
subpage, at `RADIANT_FIELD_ID_COMMON_BASE + subpage`), `0x40`–`0xFF`
unallocated. A sink keys stored history on `(source, field_id)`, so without the
split the two producers would silently merge two different series.

##### The heart-rate toggle, which breaks the rule on the most common sensor there is

Found during implementation, not in any of the documents above, and it is the
trap in this page. **Device type `0x78` sets bit 7 of the page byte as a
page-change toggle on *every* page it sends, including the common ones**
(§3.2 above, and `ant_pages.decode_hr()` masks the same bit for the same
reason). So on a strap, page 84 arrives as `0x54` or `0xD4` depending on where
the toggle happens to be — and `0xD4` is inside Table 4-1's `0x80`–`0xDF`
"reserved, do not use" row, i.e. in no range that means anything.

Without masking, the common path works on every device type **except** the
single most common ANT+ sensor there is, and then only on roughly half the
broadcasts — the worst failure shape there is to find on a bench. `rx_tap.c`
therefore masks `page &= 0x7F` when `b->devtype == 0x78`, before the range
test, and hands the adapter a body whose byte 0 is already de-toggled. That is
masking a bit the *profile* defines, not choosing a decoder from the payload:
the page number underneath is still the common page number, still keyed on
transmission type, and still handled by the device-type-independent path.

##### Table 6-16 — the byte layout

| Byte | Field | Encoding |
|---|---|---|
| 0 | page number | `0x54` |
| 1 | reserved | `0xFF` |
| 2 | subpage 1 — the page value for bytes 4..5 | 1–254, `0xFF` = invalid |
| 3 | subpage 2 — the page value for bytes 6..7 | 1–254, `0xFF` = invalid |
| 4..5 | data field 1 | u16 LE, meaning per subpage 1 |
| 6..7 | data field 2 | u16 LE, meaning per subpage 2 |

Byte 1 is deliberately **not** validated by the decoder: a master that puts
something else there is out of spec in a way that says nothing about bytes
2..7, and rejecting the page over it would discard good weather data to enforce
a byte nobody reads.

##### Table 6-17 — the subpage catalogue, and what this project does with each

Sign is the subpage's business: 1, 7 and 8 are two's complement and the rest are
unsigned, which is why the decoder hands back an uninterpreted `uint16_t` and
the adapter casts.

| Sub | Field | Wire unit | Spec valid range | Vocabulary type | Conversion | `field_id` |
|---|---|---|---|---|---|---|
| 1 | temperature | 0.01 °C signed | −326.67 … +326.67 | `0x10` temperature, K | `raw = centi_C + 27315`, `exp = -2` — **exact** | `0x21` |
| 2 | barometric pressure | 0.01 kPa | 0 … 655.35 kPa | `0x12` pressure, Pa | `raw = wire`, `exp = +1` — **exact** | `0x22` |
| 3 | humidity | 0.01 % | max 100 % | `0x11` humidity, % | `raw = wire`, `exp = -2` — **exact** | `0x23` |
| 4 | wind speed | 0.01 km/h | max 655.35 km/h | `0x16` speed, m/s | `raw = (v x 1000000 + 180) / 360`, `exp = -6` — **lossy** | `0x24` |
| 5 | wind direction | 0.05 ° | max 7199 (359.95°) | `0x18` angle, rad | `raw = (v x 872665 + 500000) / 1000000`, `exp = -6` — **lossy** | `0x25` |
| 6 | charging cycles | count | 0 … 65535 | `0x36` event count, accumulating | delta at u16 wire width, `exp = 0` | `0x26` |
| 7 | minimum operating temperature | 0.01 °C signed | −326.67 … +326.67 | `0x10` | as subpage 1 | `0x27` |
| 8 | maximum operating temperature | 0.01 °C signed | −326.67 … +326.67 | `0x10` | as subpage 1 | `0x28` |
| 9–254 | reserved for future use | — | — | — | declined and counted | — |

**Declining a subpage is a normal answer, not an error**, and the same is true
of the other common pages: 80/81/82 are identity and battery, 83 is time, 85–87
are memory, pairing and errors — none of them a measurement — so
`radiant_common_adapter_decode()` returning 0 for page `0x50` is the answer
rather than a gap. Subpage 6 is the only accumulating quantity on the page and
obeys `radiant-bridge.md` §3.2's differencing rule in full: differenced at its
own u16 width, first sighting establishes `prev` and posts nothing.

**The two lossy conversions are a first for this codebase, and §3.2 is the
wrong lens for them.** Nothing accumulates — wind speed and direction are
instantaneous, each sample independent of the last — so the rounding error is
bounded by half a unit in the last place on that sample alone and does not
compound. What is lossy is the *scale*: 0.01 km/h is exactly 1/360 m/s and
`10^k / 360` is never an integer, because 360 has a factor of 3 and `10^k` has
none; 0.05° is 872.66462… µrad, irrational, so no `(raw, exp)` pair is exact at
any scale. Both are integer round-half-up, and both errors are orders of
magnitude below the sensor's own quantisation.

##### The golden vector, §6.13.1

```
ANT message payload = 54 FF 01 03 6B 0A EA 19
```

Subpage 1 = temperature, `0x0A6B` = 2667 → **26.67 °C**; subpage 3 = humidity,
`0x19EA` = 6634 → **66.34 % RH**. A conformance vector from the primary
document, which is exactly what this section previously said the page lacked.
It is the acceptance test for the decoder and is transcribed as
`PROFILE_COMMON_84_GOLDEN_TEMP_CENTI` / `_HUMID_CENTI` in
`profile_common.h`, so the C test, the Python test and the header cannot drift
apart.

##### Three defects in Table 6-17, recorded rather than silently fixed

Same register as §3.14's note on Controls page `0x49`'s length column: the
source table is internally inconsistent, the self-consistent half is treated as
authoritative, and the other half is written down so nobody "discovers" it
again and flips the code.

1. **Subpages 7 and 8 have their descriptions transposed.** Subpage 7 is
   *named* "Minimum Operating Temperature LSB/MSB" and *described* as "A signed
   value using two's complement of the maximum recorded temperature in °C";
   subpage 8 is the mirror image. The names are self-consistent with each other
   and with the row order, and the two description cells are plainly swapped.
   **Trust the names**: 7 is the minimum, 8 is the maximum. Both decode
   identically, so this costs nothing at the wire and everything at the label.
2. **Table 6-17 prints "Wind Direction LSB" twice** for subpage 5, where the
   second row is plainly the MSB — every other two-byte subpage in the table
   pairs LSB with MSB and the field is stated as 2 bytes. Read as LSB then MSB,
   i.e. the same u16 little-endian as every other subpage.
3. **The temperature valid range is narrower than the field at both ends, with
   no sentinel in the gap.** The table states −326.67 … +326.67 °C for subpages
   1, 7 and 8, while a signed 0.01 °C count in a u16 expresses
   −327.68 … +327.67. Contrast Environment `0x19` (§3.7), where the range stops
   one count short *on the negative side only*, precisely so the `0x8000`
   sentinel is not also a legal reading. Here the gap is symmetric and no
   sentinel is defined inside it, so the decoder does not invent one: it hands
   back the raw two's-complement value and lets the caller decide. Inventing an
   invalid value the specification does not state is how a real −327.00 °C
   reading becomes a silent drop.

##### What "decoded" does and does not claim here

**No ANT+ environmental sensor is on this bench.** Page 84 is verified against
the §6.13.1 golden vector, against `tools/ant_sim.py` replay, and in ztest —
not against a real weather-reporting sensor. A primary-spec-derived,
vector-tested decoder is materially better than the third-party-evidence
position this section used to record, and it is **not** the same thing as
verified on hardware. The same limitation applies to §3.7.

There is deliberately **no encoder**. Nothing in this tree is a weather station,
and an encoder with no transmitter is an untested page that looks shipped.

---

### 3.5 The pages that are not ANT+: `0x70`-`0x72`

A capture from a RadiANT compat sensor carries three page numbers no ANT+
document defines: `0x70` the capability beacon, `0x71` Tier I identity
attestation, `0x72` Tier II data attestation. They are **added** to the profile
and nothing existing is touched, which is the whole mechanism — a legacy
receiver skips a page number it does not know, and the data pages above stay
byte-exact.

They are not specified here. `docs/radiant-security.md` section 11 is normative
for the bytes, `docs/profile-registry.md` registers the numbers and the residual
collision risk, and
`docs/decisions/0008-antplus-additive-pages-and-compat-security.md` records why
the allocation is what it is. `tools/ant_pages.py` encodes and decodes them and
`tools/ant_verify.py` reports `verified` / `unverified` / `clear` over them —
where **`clear` means "no key here" and not "unprotected"**.

The cost, because it is the number the compatibility claim rests on: **2.0 % of
slots** in the default configuration, 0.8 % beacon plus 1.2 % Tier I, against
the **1.65 %** ANT+ itself already spends on common pages 80 and 81 in the
cadence below.

---

### 3.6 Light Electric Vehicle, device type `0x14` — reference only

**Not implemented by this project. Recorded in full because it is the reason
the e-bike profile of section 7.1 was not pursued**, and because a claimant
needs the whole interoperability contract — both channels, every page, the
travel-mode mapping rule — not just a field list to check against.

#### Channel configuration

| Parameter | Display (slave) | LEV (master) |
|---|---|---|
| Channel type | Slave `0x00` | Master `0x10`, bidirectional |
| Network key | ANT+ managed network key | same |
| RF channel | 57 (`0x39`, 2457 MHz) | same |
| Transmission type | `0`, for a pairing search | `5` (`0x05`) |
| Device type | `20` (`0x14`) | `20` (`0x14`) |
| Device number | 1..65535, `0` for wildcard search | 1..65535, **must not be `0x0000`** |
| Channel period | 8192 (4.00 Hz) | 8192 (4.00 Hz) |
| Search timeout | default 30 s, implementation-specific | — |

Every ANT+ message has an 8-byte payload: byte 0 is the page number, bytes
1..7 are page-specific.

#### Data pages and the 4-page rotation

Pages 1, 2 (or 34), and 3 fill the first three channel periods of a 4-page
rotation, one page per period — each therefore at 1 Hz. The fourth period
rotates among page 4, page 5, a common page, or a manufacturer-specific page.
Because LEV speed appears on pages 1, 2 and 3, it updates at an average 3 Hz;
because travel mode / system state / gear state appear on pages 1 and 3, they
update at an average 2 Hz. Page 2 carries remaining range and page 34 carries
fuel consumption instead — they are the same page in every other byte, and a
node sending 34 by default must still send page 2 **at least once every 30
seconds** (120 messages) so a receiver that only knows page 2 stays current.

Either common page 80 or 81 (alternating) must appear roughly every 20th
channel period (~5 s at 4 Hz); Table 7-1 states each individual page's own
cadence as every 40th message. A display's request (common page 70) gets an
immediate reply on the next channel period, after which the LEV **resets its
4-page rotation** back to page 1 — a receiver stays within 0.5 s of current
state even after a request interrupts the pattern.

Pages 1, 2 (or 34), 3, and 5 are required of every LEV; page 4 is optional;
page 16 is required to *decode* (every LEV) but optional to *send* (a display
may be receive-only).

#### Page `0x01` — Speed & System Information 1

| Byte | Field | Encoding |
|---|---|---|
| 0 | page number | `0x01` |
| 1 | temperature state | u8 bit field, Table 5-3 below. Optional, `0x00` = unknown |
| 2 | travel mode state | u8 bit field, Table 5-4 below |
| 3 | system state | u8 bit field, Table 5-5 below |
| 4 | gear state | u8 bit field, Table 5-6 below |
| 5 | error message | u8 code, Table 5-7 below. Optional |
| 6 | LEV speed, low byte | u8, low byte of a 12-bit value |
| 7 bits 3..0 | LEV speed, high nibble | 12-bit total: 0.1 km/h, max 409.5 km/h |
| 7 bits 7..4 | reserved | `0xF` |

**Table 5-3 — Temperature state:**

| Bits | Field | Value |
|---|---|---|
| 7 | motor temperature alert | 0 no alert/unknown, 1 overheating |
| 6..4 | motor temperature | 000 unknown, 001 cold, 010 cold/warm, 011 warm, 100 warm/hot, 101 hot, 110..111 reserved |
| 3 | battery temperature alert | 0 no alert/unknown, 1 overheating |
| 2..0 | battery temperature | same 6-level scale as motor temperature |

**Table 5-4 — Travel mode state** (shared by pages 1 and 3, and by page 16's
travel-mode byte):

| Bits | Field | Value |
|---|---|---|
| 7..6 | reserved | `00` |
| 5..3 | current assist level | `000` off, `001`..`111` = assist 1..7 |
| 2..0 | current regenerative level | `000` off, `001`..`111` = regen 1..7 |

**Table 5-5 — System state** (shared by pages 1 and 3): every bit defaults to
0 when the LEV doesn't support the feature, so "off" and "unsupported" share
one code.

| Bits | Field | Value |
|---|---|---|
| 7..5 | reserved | `000` |
| 4 | manual throttle | 0 off/unsupported, 1 on |
| 3 | light on/off | 0 off/unsupported, 1 on |
| 2 | light beam | 0 low/unsupported, 1 high |
| 1 | turn signal, left | 0 off/unsupported, 1 blinking |
| 0 | turn signal, right | 0 off/unsupported, 1 blinking |

**Table 5-6 — Gear state** (shared by pages 1 and 3):

| Bits | Field | Value |
|---|---|---|
| 7 | gear exists | 0 no, 1 yes |
| 6 | manual / automatic | 0 automatic or no gears, 1 manual |
| 5..2 | current rear gear | `0001`..`1111` = gear 1..15, `0000` = none |
| 1..0 | current front gear | `01`..`11` = gear 1..3, `00` = none |

**Table 5-7 — Error message:** a single code, not a fault bit field.

| Code | Meaning |
|---|---|
| 0 | no error |
| 1 | battery error |
| 2 | drive train error |
| 3 | battery end of life |
| 4 | overheating |
| 5..15 | reserved |
| 16..255 | manufacturer-specific |

#### Page `0x02` — Speed & Distance Information

| Byte | Field | Encoding |
|---|---|---|
| 0 | page number | `0x02` |
| 1..3 | odometer | u24 LE accumulator, 0.01 km, wraps at 16,777,216 (~167,772 km) |
| 4 | remaining range, low byte | u8, low byte of a 12-bit value |
| 5 bits 3..0 | remaining range, high nibble | 12-bit total: 1 km, max 4095 km. Optional |
| 5 bits 7..4 | reserved | `0xF` |
| 6 | LEV speed, low byte | as page 1 |
| 7 bits 3..0 | LEV speed, high nibble | as page 1 |
| 7 bits 7..4 | reserved | `0xF` |

**Remaining range's own field row prints `0x00` as its "unknown" sentinel**,
not the `0x000`/`0xFFF` pattern the rest of the profile uses for a 12-bit
field split across a nibble (fuel consumption, charging cycle count, wheel
circumference all use `0x000`). Transcribed as printed; almost certainly the
same `0x000` sentinel, since nothing in the spec explains a genuine exception.

#### Page `0x22` (34) — Alternative Speed & Distance Information

Optional; replaces page 2's remaining range with fuel consumption. Every
other byte is identical to page 2, including the odometer and LEV speed
fields.

| Byte | Field | Encoding |
|---|---|---|
| 0 | page number | `0x22` (34) |
| 1..3 | odometer | as page 2 |
| 4 | fuel consumption, low byte | u8, low byte of a 12-bit value |
| 5 bits 3..0 | fuel consumption, high nibble | 12-bit total: 0.1 Wh/km, max 409.5 Wh/km, `0x000` = unknown |
| 5 bits 7..4 | reserved | `0xF` |
| 6 | LEV speed, low byte | as page 1 |
| 7 bits 3..0 | LEV speed, high nibble | as page 1 |
| 7 bits 7..4 | reserved | `0xF` |

#### Page `0x03` — Speed & System Information 2

| Byte | Field | Encoding |
|---|---|---|
| 0 | page number | `0x03` |
| 1 | battery state of charge | bits 6..0: 1 %, max 100 %. Bit 7: battery-empty warning |
| 2 | travel mode state | Table 5-4, as page 1 |
| 3 | system state | Table 5-5, as page 1 |
| 4 | gear state | Table 5-6, as page 1 |
| 5 | % assist | u8, %, max 100 %. Optional, `0xFF` = unknown |
| 6 | LEV speed, low byte | as page 1 |
| 7 bits 3..0 | LEV speed, high nibble | as page 1 |
| 7 bits 7..4 | reserved | `0xF` |

% assist is `motor power / (motor power + user power)` — the LEV's own
computed answer, not something a receiver could derive from anything else on
the wire.

#### Page `0x04` — Battery Information (optional page)

| Byte | Field | Encoding |
|---|---|---|
| 0 | page number | `0x04` |
| 1 | reserved | `0xFF` |
| 2 | charging cycle count, low byte | u8, low byte of a 12-bit value |
| 3 bits 3..0 | charging cycle count, high nibble | 12-bit total: 1 count, max 4095, `0x000` = unknown |
| 3 bits 7..4 | fuel consumption, high nibble | 12-bit total: 0.1 Wh/km, max 409.5 Wh/km, `0x000` = unknown |
| 4 | fuel consumption, low byte | u8, low byte of the field above |
| 5 | battery voltage | u8, 1/4 V, max 63.75 V (raw 255). `0x00` = unknown |
| 6..7 | distance on current charge | u16 LE, 0.1 km, max 6553.5 km, `0x0000` = unknown. LEV resets this after every charge |

**Byte 3 packs the high nibble of two unrelated fields**: bits 3..0 finish
charging cycle count (whose low byte is byte 2), bits 7..4 finish fuel
consumption (whose low byte is byte 4). It is the only byte in this profile
that belongs to two different multi-byte fields at once, and a decoder that
reads byte 3 as one field gets a number that is neither.

#### Page `0x05` — LEV Capabilities

Required of every LEV; sent in the fourth rotation slot at ~1 Hz or less, or
on request (common page 70).

| Byte | Field | Encoding |
|---|---|---|
| 0 | page number | `0x05` |
| 1 | reserved | `0xFF` |
| 2 | travel modes supported | Table 5-13 below |
| 3 | wheel circumference, low byte | u8, low byte of a 12-bit value |
| 4 bits 3..0 | wheel circumference, high nibble | 12-bit total: 1 mm, max 4095 mm. Optional |
| 4 bits 7..4 | reserved | `0xF` |
| 5..7 | reserved | `0xFF` each |

**Table 5-13 — Travel modes supported:**

| Bits | Field | Value |
|---|---|---|
| 7..6 | reserved | `00` |
| 5..3 | assist modes supported | `0` (none) to `7` |
| 2..0 | regenerative modes supported | `0` (none) to `7` |

#### Pages `0x06`-`0x0F` — reserved for future main data pages

Spec's own words: reserved for future main data page definitions. No content
defined.

#### Page `0x10` (16) — Display Data (display → LEV, acknowledged)

The one page in this profile that runs the other direction: a display sends
it to report a user-requested state change, as an **acknowledged** message so
the display can confirm the LEV received it. Optional for a display to send;
every LEV must be able to decode it.

| Byte | Field | Encoding |
|---|---|---|
| 0 | page number | `0x10` |
| 1 | wheel circumference, low byte | u8, low byte of a 12-bit value |
| 2 bits 3..0 | wheel circumference, high nibble | 12-bit total: 1 mm, max 4095 mm. Optional |
| 2 bits 7..4 | reserved | `0xF` |
| 3 | travel mode | u8, same bit layout as Table 5-4 bits 5..0. `0xFF` = not supported/not set |
| 4..5 | display command | u16 LE bit field, Table 5-15 below |
| 6..7 | manufacturer id | u16 LE, the *display's* ANT+ manufacturer id — informational, not a setting |

**Wheel circumference's "not set" sentinel is printed as `0xFF` in the field
row but `0xFFF` in the prose** (section 5.10.1) — the same table/prose
sentinel mismatch as page 2's remaining range and page 5's wheel
circumference, and the third field to carry it.

**Table 5-15 — Display command**, a 16-bit little-endian bit field (byte 4 =
low byte, byte 5 = high byte), reflecting the state the user requested through
the display:

| Bits | Field | Value |
|---|---|---|
| 15..10 | reserved | `000000` |
| 9..6 | requested rear gear | `0001`..`1111` = gear 1..15, `0000` = none |
| 5..4 | requested front gear | `01`..`11` = gear 1..3, `00` = none |
| 3 | light on/off | 0 off, 1 on |
| 2 | light beam | 0 low, 1 high |
| 1 | turn signal, left | 0 off, 1 blinking |
| 0 | turn signal, right | 0 off, 1 blinking |

Travel mode is a separate byte (3), not part of this bit field — a display
sets gear and light/turn-signal state through the command field and travel
mode through byte 3 in the same message.

#### Common page `0x46` (70) — Request Data Page

Sent display → LEV only, as an acknowledged message; the LEV must never
request a page from the display. Descriptor bytes 1 and 2 are always invalid
for LEV (`0xFF`, `0xFF`) — this profile has no page variants to select with
them.

| Byte | Field | Encoding |
|---|---|---|
| 0 | page number | `0x46` (70) |
| 1..2 | reserved | `0xFF` |
| 3 | descriptor byte 1 | `0xFF` = invalid, always invalid for LEV |
| 4 | descriptor byte 2 | `0xFF` = invalid, always invalid for LEV |
| 5 | requested transmission response | bits 6..0: number of transmissions requested, max 4 (one second of data). Bit 7: request an acknowledged reply if possible. `0x80` = transmit until acknowledged, `0x00` = invalid |
| 6 | requested page number | the page to transmit |
| 7 | command type | `0x01` = request data page (the only value LEV uses); `0x02` = request ANT-FS session |

Only broadcast message types may be requested — not acknowledged, not burst.
The LEV must be able to answer a request for any page this document defines,
but may silently ignore a page number it doesn't support; a display using
this page has to handle that "no response" case rather than assume every
request lands. On a valid response the LEV resets its 4-page rotation (see
above).

#### Common pages 80 and 81

Byte-exact against the ANT+ common pages already documented in section 3.4
above — manufacturer's information and product information, same layout, no
LEV-specific variant. **LEV has no use for common page 82 (battery
status)**: page 4 above already carries a richer, LEV-specific battery
picture (voltage, charge-cycle count, distance since charge) than page 82's
generic voltage/percentage fields, so nothing in this profile spends the byte
twice.

#### Travel mode mapping (section 6)

A display and an LEV can support different numbers of travel modes — up to 7
assist and 7 regenerative each — and Table 6-1 is the rule that keeps them
talking about the same setting anyway. Mode numbers are grouped so that
whichever count a device supports, its lowest and highest available numbers
still mean "least" and "most":

| Modes supported | Grouping of modes 1..7 | Recommended settings |
|---|---|---|
| 1 | all of 1..7 | 7 |
| 2 | 1,2,3 / 4,5,6,7 | 3, 7 |
| 3 | 1,2 / 3,4 / 5,6,7 | 2, 4, 7 |
| 4 | 1 / 2,3 / 4,5 / 6,7 | 1, 3, 5, 7 |
| 5 | 1 / 2 / 3 / 4,5 / 6,7 | 1, 2, 3, 5, 7 |
| 6 | 1 / 2 / 3 / 4 / 5 / 6,7 | 1, 2, 3, 4, 5, 7 |
| 7 | 1 / 2 / 3 / 4 / 5 / 6 / 7 | 1, 2, 3, 4, 5, 6, 7 |

The same table applies to assist and regenerative levels independently — a
mode *number* is just a position in this grouping, not a fixed percentage.
When a display supports more modes than the LEV, it hides the modes the LEV
can't reach rather than showing its own full range. When a display supports
fewer modes than the LEV, a user with manual control on the LEV can still
reach the modes the display cannot show, and the display simply interprets
whatever mode number it receives via this same table.

#### Minimum requirements (section 7)

| Required page | Transmission requirement |
|---|---|
| Page 1 | 1 Hz |
| Page 2 (or page 34) | 1 Hz; page 2 itself at least once per 30 s |
| Page 3 | 1 Hz |
| Page 5 | on request |
| Page 16 | on system update or on display request; sent acknowledged |
| Common page 80 | every 40th message |
| Common page 81 | every 40th message |

A display must be able to decode all of the above, and must itself transmit
common pages 80 and 81 at roughly the same every-40th-message cadence,
sending its first common page immediately on detecting the LEV.

**What LEV does not carry, in full despite the above:** rider power in watts,
motor power in watts, and walk assist. Those three are the entire gap, and
section 7.1 is why the gap does not justify a profile of its own.

---

### 3.7 Environment, device type `0x19`

Decoded. `src/profiles/profile_env.c` (page codec, decode only) and
`radiant/src/bridge/radiant_env_adapter.c` (the sample-bus mapping);
`apps/dongle_thread/src/self_channels.c` carries two acquisition rows for it.

**The "not derived from a primary spec" caveat this section used to carry is
lifted.** — §5.1 channel configuration, §6.3 data page 0, §6.3.1
transmission info, §6.3.2 supported pages, §6.4 data page 1, §6.5 the reserved
range. The four converging open-source implementations previously cited here
agreed with Table 6-4 **byte for byte**, and none of them had a placement
field, so the earlier position was right and is now sourced rather than merely
corroborated.

Device type 25, RF 57. §5.1 permits **two** channel periods — 65535 counts
(0.5 Hz, what the Garmin tempe uses and the profile's low-power default) and
8192 counts (4 Hz) — and page 0's transmission-info field declares which one a
sensor defaults to. §5.1 also **recommends a 45 second receiver search
timeout**, in as many words, "to allow sufficient time for a 0.5 Hz master to
be found"; that is where `PROFILE_ENV_SEARCH_TIMEOUT_S` comes from, against
ANT's own 25 s default.

| Page | Byte | Field |
|---|---|---|
| 0 | 1 | reserved `0xFF` |
| 0 | 2 | reserved `0xFF` |
| 0 | 3 | transmission info: bits 1..0 default rate (`00` = 0.5 Hz, `01` = 4 Hz, `10`/`11` reserved), bits 3..2 UTC time support, bits 5..4 local time support, bits 7..6 reserved |
| 0 | 4..7 | supported pages, u32 LE bitmap — bit position *is* the page number; bit 0 always set, a temperature sensor also sets bit 1, bits 2..31 shall be 0 |
| 1 | 1 | reserved `0xFF` |
| 1 | 2 | event count, u8 accumulator |
| 1 | 3 + 4 bits 7..4 | 24-hour low, 12-bit signed, 0.1 °C, `0x800` invalid |
| 1 | 4 bits 3..0 + 5 | 24-hour high, 12-bit signed, 0.1 °C, `0x800` invalid |
| 1 | 6..7 | current temperature, int16 LE, 0.01 °C, `0x8000` invalid |

**Page 0 reserves both bytes 1 and 2; page 1 reserves only byte 1** and puts
the event count in byte 2. Two pages, two shapes, one byte apart — an encoder
or a test vector that reuses one shape for the other is wrong in a way that
still parses.

**§6.4's stated ranges stop one count short of the field minimum**, and that is
not sloppiness: −204.7 °C (not −204.8) for the two 12-bit fields and
−327.67 °C (not −327.68) for the 16-bit current reading, which is precisely
what keeps the `0x800` and `0x8000` sentinels from also being legal readings.
Contrast common page 84 (§3.4), whose temperature range is narrowed at *both*
ends with no sentinel in the gap at all.

The two 24-hour fields are packed in **opposite directions** and share byte 4,
they are twelve-bit signed and need explicit sign extension from bit 11, and
they use a different scale *and* a different sentinel from the current reading
in the same page. `profile_env.c` keeps all of that in two helpers so there is
exactly one place to be wrong about the packing.

**It is a thermometer, and that is the fact the rest of the design turns on.**
Pages 0 and 1 are the whole profile and pages 2–63 are reserved (§6.5): there
is **no humidity, no barometric pressure, no wind and no air quality anywhere
in device type `0x19`**, however much the name "Environment" suggests
otherwise. That is what forces common page 84 (§3.4) to exist as a separate
path — everything environmental other than temperature reaches this project
through a page keyed on transmission type, arriving on any profile's channel,
rather than through a device type of its own.

**There is also no sensor placement, sensor location or sensor kind field
anywhere in it.** That single fact is what decides section 6.3, and ANT+ itself
reached the same conclusion: when it needed physiological temperature it minted
Core Temperature `0x7F` (§3.8) rather than extending this.

**No ANT+ environmental sensor is on this bench.** Like page 84, this decoder is
verified against the specification, against `tools/ant_sim.py` and in ztest, and
not against a real sensor. Primary-spec-derived and vector-tested is not the
same as verified on hardware.

---

### 3.8 Core Temperature, device type `0x7F` — reference only

**Not implemented by this project. Recorded because it is direct prior art for
section 6.3's design** — when ANT+ itself needed to carry physiological
temperature, it did not extend Environment `0x19`. It minted a separate device
type, which is the same structural conclusion section 6.3 reaches independently
(there, a `0x60` recipe rather than a device type, but "not an Environment page"
either way).

Evidence is vendor code from an ANT+-certified manufacturer rather than a
primary spec: greenTEG/CoreBodyTemp's own published Connect IQ source
(`CoreSensor.mc`, `const DEVICE_TYPE = 0x7F`) and an independent `openant`
decoder agree byte-for-byte. Device type 127 (`0x7F`), period 8192 (4 Hz), RF 57.

| Page | Byte | Field |
|---|---|---|
| 0 | 2 | data quality: 0 poor, 1 fair, 2 good, 3 excellent, `0xFF` unused |
| 1 | 1 | heat strain index, u8, ×0.1 |
| 1 | 2 | event count, u8 accumulator |
| 1 | 3 + 4 bits 3..0 | skin temperature, 12-bit signed, ×0.05 °C, `0x800` invalid |
| 1 | 4 bits 7..4 + 5 | reserved |
| 1 | 6..7 | core temperature, int16 LE, ×0.01 °C, `0x8000` invalid |
| `0x50`/`0x51`/`0x52` | — | common pages 80/81/82, standard |

Two things worth noting against this project's own choices rather than just
recording the layout:

- **Page 0's data-quality field validates section 6.3's measurement-quality
  field.** ANT+'s own certified solution to "a calculated core temperature is a
  model output that can be wrong during warm-up" was to add a quality field,
  independently of this project reaching the same conclusion for the same
  reason.
- **It is less flexible than section 6.3's placement enum, not more.** Rather
  than one temperature field with a placement code, it hard-codes two fixed
  fields — skin and core — as separate quantities on one page. That works for a
  device that always has both sensors (the CoreBodyTemp strap does), and does
  not generalise to a single-sensor device reporting one placement of several.
  Worth the sentence; not a reason to change section 6.3's design.

---

### 3.9 The common-page cadence trap

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

### 3.10 Accumulator semantics

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

### 3.11 Invalid-value sentinels

| Width | Sentinel | Constant |
|---|---|---|
| u8 | `0xFF` | `INVALID_U8` |
| u16 | `0xFFFF` | `INVALID_U16` |
| u32 | `0xFFFFFFFF` | `INVALID_U32` |

A receiver that treats these as numbers reports 255 rpm or 65535 W. The
encoders in `tools/ant_pages.py` take `None` for "not reported" so a caller
never has to remember which sentinel a given page uses.

### 3.12 Capture file format

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

### 3.13 Running Dynamics, device type `0x1E`

Numbered last rather than beside the other implemented profiles because 3.4
through 3.12 are already referenced from `docs/profile-registry.md` and from
several source headers; a renumbering to put this in tidy order would break
those citations to buy nothing. It is an implemented profile, not a
reference-only one.

Implemented in `src/profiles/profile_rd.c` and mirrored in
`tools/ant_pages.py`; the two are checked against the same vectors by
`tools/test_ant_pages.py` and `radiant/tests/src/test_profile_rd.c`.

#### The metrics

| Metric | Page | Wire scale | Range | Invalid |
|---|---|---|---|---|
| Cadence | `0x00` | 1/32 strides/min | 0–255.97 | integer part 0 |
| Vertical oscillation | `0x00` | 1/4 mm | 0–2047.75 mm | integer part 0 |
| Ground contact time | `0x00` | 1 ms | 0–2047 ms | 0 |
| Stance time | `0x00` | 1/4 % | 0–100 % | integer part 0 |
| Step count | `0x00` | 1 step | 7-bit rollover | none — 0 is a real value |
| Ground contact balance | `0x01` | 1/32 % | 0–100 % | integer part 0 |
| Vertical ratio | `0x01` | 1/32 % | 0–100 % | none stated |
| Step length | `0x01` | 1 mm | 0–8191 mm | none — 0 is a real value |
| Walking flag, module orientation | `0x00`, `0x01` | 1 bit | — | — |
| Horizontal speed (display → sensor) | `0x10` | 1/256 m/s | 0–14.996 m/s | `0x0F` / `0xFF` |

**Every measurement spells invalid as ZERO, not as `0xFF`.** This is the one
implemented profile where section 3.11's sentinels do not apply at all, and a
decoder that reached for `INVALID_U8` reports a stationary runner's stance time
as 63.75 %.

Percentage fields reserve 101–127 rather than leaving them undefined, so a
sensor clamping a computed percentage clamps to 100 and not to the field
maximum. `profile_rd.c` refuses to encode a reserved value rather than
transmitting it.

#### Three layout traps

- **There is no page-change toggle.** Byte `[0]` is the whole page number, all
  eight bits. That is the opposite of heart rate `0x78` (section 3.2), and a
  decoder that masked with `0x7F` out of habit would work by accident on every
  page this profile currently defines and break when 33–63 are allocated.
- **Ground contact time is split low-bits-first**: three low bits in byte `[4]`
  bits 5–7, eight high bits in byte `[5]`. Every other split field on the
  profile puts its low byte first and its high bits in the shared byte.
- **Two fractions are split across a byte boundary, low bit first.** Stance
  time's 2 bits (byte `[6]` bit 7, then byte `[7]` bit 0) and ground contact
  balance's 5 bits (byte `[1]` bit 7, then byte `[2]` bits 0–3). Reading either
  the other way round is wrong by a factor of two *only when the low bit is
  set*, so half of a naive test set passes.

Page `0x00` sets its one reserved bit to `0`; page `0x01` sets its two reserved
bits to `0b11`. Both are transcribed from the tables and neither is a typo.

#### The two channel configurations, which is the part that is not a layout

A **standalone RD pod** is one channel: device type `0x1E`, RF 57 (2457 MHz),
period 4096 (8 Hz), transmission type 5.

An **HR-RD strap** — the Garmin HRM-Run / HRM-Pro shape — is **two channels**,
and this is the fact that decides how a receiver finds the running dynamics:

1. Its heart-rate channel is an ordinary ANT+ HRM channel (`0x78`, period 8070)
   and **carries no running dynamics at all**. Section 3.2's page set is the
   whole of it.
2. The running dynamics ride a **second channel**, device type `0x1E` again but
   period 8070 and transmission type 1, on one of four permitted RF indices
   (3 / 39 / 61 / 75) rather than the ANT+ 57.
3. A display opens it by sending page `0x4A` as an acknowledged message **on
   the heart-rate channel** after pairing, naming the RF index, the device type
   and the period. A strap with no session leader opens the RD channel where it
   was told.
4. The strap then advertises where it went in the byte HRM page `0x04` reserves
   as manufacturer-specific — byte `[1]` becomes an RF enumeration — so a
   receiver that did *not* become session leader can still find the channel.

**The enumeration is not sorted**: code 1 is 2403 MHz, 2 is 2439, **3 is 2475
and 4 is 2461**. The document states it twice and both statements agree, so it
is transcribed as a table rather than computed. Code 0 means the RD channel is
not open, and — critically — **any value outside the enumeration must not be
interpreted**, because on a strap without running dynamics that byte is
ordinary manufacturer-specific data. `profile_rd_rf_enum_index()` returns 0 for
both cases so that "do not interpret" is the default rather than something a
caller remembers. This is also why `profile_hr.c` writes `0xFF` there.

#### The page numbers do not collide with the RadiANT compat pages

Checked rather than assumed, because this document raised it as an open
question before the specification was in hand. Section 3.5 allocates
`0x70`–`0x72` in the **heart rate** page space. This profile uses `0x00`,
`0x01`, `0x10`, `0x20` and `0x4A` in the **running dynamics** page space, whose
reserved ranges are 2–15, 17–31 and 33–63. The two allocations are disjoint
twice over: different device types, and no shared number. No compat client is
attached to this profile.

#### What RadiANT does with it

`src/bridge/radiant_rd_adapter.c` decodes both main pages onto the sample bus.
Six of the eight metrics map onto the section 7 vocabulary exactly and are
published today; **cadence and ground contact time are not**, and that is a
vocabulary gap rather than a decoder gap — see `docs/radiant-bridge.md` and the
note in that adapter's header. Nothing is silently dropped: the decoder parses
every field, and the two unpublished ones are readable from
`struct profile_rd_metrics` by any caller that wants them before the vocabulary
question is settled.

#### Independent corroboration: the HRM 600's BLE side, and why it does not touch this section

Two community sources reverse-engineer the Garmin HRM 600's data path: a blog post
(`dropbars.be/blog/reverse-engineering-garmin-hrm600-running-dynamics`) and its
companion repository `codeberg.org/samdumont/openrd-ble-running-dynamics`
(an ESP32/NimBLE simulator of the sensor, referencing Gadgetbridge's public
Garmin BLE protocol notes as its own foundation). Recorded here as
`[community, reverse-engineered]` per this project's own provenance-tagging
convention — corroboration, not a source this section's page layout was
written from.

**The transport is a different, proprietary stack, not ANT+ pages carried over
BLE.** The HRM 600 talks to a watch over Bluetooth using Garmin's own
Multi-Link service, GFDI framing, COBS byte-stuffing, and a protobuf payload
exchanged through an `EventSharing` mechanism — four nested layers with no
relationship to an ANT+ data page at all. This settles a question this
document's HR-RD section otherwise left open: whatever a Garmin watch does
over BLE, it is not "the RD pages, GATT-wrapped." The ANT+ side — this
section's own scope — is a separate, simpler, openly-interoperable path; the
blog post's own words are that "over ANT+ a non-Garmin sensor can feed \[RD]
to a Garmin watch today. The lock is specifically the Bluetooth path."

**The physiological scale factors match this section's exactly**, recovered
independently from a decompiled protobuf schema on a transport with no
ANT+ ancestry:

| Field (protobuf name) | Scale | Matches this section's |
|---|---|---|
| `vertical_oscillation_1_4ths_mm` | 0.25 mm | §3.13 vertical oscillation, 1/4 mm |
| `stance_time_1_4ths_percent` | 0.25 % | §3.13 stance time, 1/4 % |
| `ground_contact_balance_1_32nds_percent` | 1/32 % | §3.13 ground contact balance, 1/32 % |
| `vertical_ratio_1_32nds_percent` | 1/32 % | §3.13 vertical ratio, 1/32 % |
| `cadence_1_32_strides_per_min` | 1/32 strides/min | §3.13 cadence, 1/32 strides/min |
| `ground_contact_time_ms`, `step_length_mm`, `step_count` | whole units | §3.13, same |

Two independent reverse-engineering efforts against two unrelated transports
recovering the same seven scale factors is strong corroboration that
`profile_rd.c`'s wire-scale constants are right, without being a second source
for anything this section states about ANT+ specifically — the BLE schema
names its fields differently and carries them through protobuf, not through an
8-byte ANT+ payload, so there is no byte offset or bit-packing fact to import
from it.

**One field this project's ANT+ source does not define at all**: protobuf field
11, `step_speed_loss_data`, is present in the schema but — per the blog post —
"never observed in captures." Not implemented here; recorded so a future reader
does not wonder whether it was missed.

Full protocol notes — UUIDs, envelope framing, the field table, the
twelve-step handshake — are kept separately in
[`docs/ble-running-dynamics-notes.md`](ble-running-dynamics-notes.md) rather
than here, because they describe a BLE application that does not exist in
this repository yet; nothing above depends on that document.

### 3.14 Controls, device type `0x10`
**Scoped to the command surface, not the full 64-page document.** Rev 2.0 also
defines a peripheral-enumeration model — pages 1, 2, 5, 7, 8, 17, 20, 70 and 72,
through which one "generic controllable device" announces a set of child
devices — and a text-transfer sub-protocol for now-playing metadata. Neither is
implemented. What is implemented is page `0x10` (Audio/Video Command) and page
`0x49` (Generic Command): a remote sending transport and menu/stopwatch
commands to a controllable master, which is what "control a device over ANT+"
means in the overwhelming majority of real deployments. The channel this
profile opens is unaffected by the scope decision — this implementation
interoperates with any Rev 2.0 controllable device's command pages regardless
of whether that device also implements peripheral enumeration.

**Channel.** Device type `0x10`, RF 57 (2457 MHz), period 8192 (4 Hz),
transmission type ending in nibble 5. A controllable device's periodic
broadcast has no required main-page content beyond common pages 80/81/82 — the
command pages are slave-initiated (`0x49`, acknowledged) or issued by the
master in response to a local event (`0x10`), not a periodic report.

**The sequence number is shared across both page types, not one per type.**
Table 9-1 and table 9-9 each carry a "Sequence #" byte, and the document is
explicit both draw from the SAME counter: an audio command, a generic command,
then another audio command are sequence numbers N, N+1, N+2. A node emitting
both page types owns one counter and both encoders draw from it —
`profile_controls_send_av()` and `_send_generic()` share
`struct profile_controls::sequence`, and a node with two independent counters
would silently disagree with the profile the moment it used both page types in
one session.

**Page `0x49`'s command field is 16 bits, despite what the source table's own
length column says.** Table 9-9 lists byte range "6-7" against a length column
reading "1 Byte" — internally inconsistent, since table 9-8's value range is
0..65535 (65535 = "No Command"), which needs both bytes. The byte range and the
value table are treated as authoritative; the length column is recorded here as
a likely transcription artifact of the source document rather than silently
"corrected" without comment.

**"No Command" is a page a sender must be able to emit, not just decode.**
When a generic-command exchange has an odd number of real commands, the
padding packet's command field must be `0xFFFF` and its serial/manufacturer
bytes must equal the PRECEDING packet's — a "No Command" packet must not itself
look like a new command to a receiver differencing sequence numbers, so it also
does not advance the sequence counter. `profile_controls_encode_generic()`
enforces this: sequence increments on every command except `0xFFFF`.

**What is deliberately not modelled**: audio-device-class support tiers
(table 9-2's "required for audio / optional for video recorder" columns) are a
receiver-conformance policy question, not a wire fact, and are not transcribed.

### 3.15 Tracker (Asset Tracker), device type `0x29`
**Channel.** Device type `0x29`, RF 57 (2457 MHz), period 2048 (16 Hz),
transmission type ending in nibble 5.

**One tracker reports MANY assets, each named by a 5-bit ASSET INDEX — not a
device number.** Pages 1, 2, 16 and 17 all carry it in byte `[1]` bits 0-4
(reserved top 3 bits `0b111`). Indices are assigned sequentially from 0 and are
**never reused or shifted** when an asset is removed: the document's own rule
is that a removed index's pages simply stop being sent, leaving a hole rather
than a renumbering. `struct profile_tracker` therefore holds 32 independent
asset slots, addressed by index, not one sensor's worth of state.

**Location is split across TWO pages, and the split is not symmetric.** Page 1
(`0x01`) carries distance, bearing, status and latitude's LOW 16 bits; page 2
(`0x02`) carries latitude's HIGH 16 bits **and all 32 bits of longitude**. A
receiver holding only page 1 does not have half a position report — it has a
quarter of one, since it is also missing the entirety of longitude. The
document's transmission pattern (page 1 always immediately followed by page 2
for the same asset) is what makes this safe in practice; this project's decode
API enforces the pairing by taking both pages together rather than exposing two
independent half-decoders a caller could invoke out of order.

**Units are semicircles and bradians, matched pairs of integer mappings chosen
so neither device needs π:**

| Unit | Formula | Range |
|---|---|---|
| Semicircles (lat/lon) | `value = degrees * 2^31 / 180` | signed 32-bit, two's complement |
| Bradians (bearing) | `value = degrees * 256 / 360` | unsigned 8-bit, 0-255 = 0-360° |

Both are stored as the raw wire integer by this module; degree conversion is
the caller's, the same split `profile_rd.h` draws between wire scale and
physical units.

**The asset situation enum is asset-type-dependent.** Table 7-5 defines five
values for a dog (`Sitting`/`Moving`/`Pointed`/`Treed`/`Unknown`) and states
`0xFF` "Undefined" for a plain Asset Tracker and for every asset type this
document does not separately enumerate. `PROFILE_TRACKER_SITUATION_*` carries
the dog values because they are the document's own worked case; any other
asset type's situation values are not defined here because the document does
not define them either.

**What is deliberately not modelled**: the higher-rate temporary-RF-channel
text exchange the sibling Controls profile's page 20 can trigger is out of
scope for the same reason page 20 itself is out of scope in §3.14 — it belongs
to Controls, not to Tracker, and neither document's temporary-channel mechanism
is implemented here.

### 3.16 Fitness Equipment (FE-C), device type `0x11`

Decoded **and** encoded since `apps/treadmill`; the two halves are separate
translation units and the sub-section below covers the transmit one. The
receive half is `src/profiles/profile_fec.c`, surfaced by
`radiant/src/bridge/radiant_power_adapter.c`. `tools/ant_pages.py` has no FE-C pages, so this is currently the only
copy and the three-copy vocabulary rule does not yet apply to it.

**Channel.** Device type `0x11` (17 decimal), RF 57, period **8192** counts —
exactly 4 Hz, the one rate the profile defines. Note §3's warning about `0x11`
being both this device type and Bicycle Power's Wheel Torque *page* number.

**Why it is here at all.** A smart trainer speaks FE-C, not just Bicycle Power,
and page 16 carries an explicit **equipment state** — a more direct "bike in
use" signal than inferring one from an accumulator.

#### Page `0x10` (16) — General FE Data

| Byte | Field | Encoding |
|---|---|---|
| 0 | page number | `0x10` |
| 1 | equipment type bit field | bits 0..4 type (Table 8-8), bits 5..7 "do not interpret" |
| 2 | elapsed time | u8 accumulator, 0.25 s, rolls at 64 s |
| 3 | distance traveled | u8 accumulator, 1 m, rolls at 256 m — **no invalid value** |
| 4..5 | instantaneous speed | u16 LE, 0.001 m/s, `0xFFFF` invalid |
| 6 | instantaneous heart rate | bpm, `0xFF` invalid |
| 7 | bits 0..3 capabilities (Table 8-9), bits 4..7 FE state (Table 8-10) |

Every FE type must send this page (§8.5.2), so a treadmill or rower still yields
elapsed time, distance, speed, heart rate and state through it. Pages 19–24, the
per-equipment-type pages, are not decoded.

#### Page `0x19` (25) — Specific Trainer Data

| Byte | Field | Encoding |
|---|---|---|
| 0 | page number | `0x19` |
| 1 | update event count | u8 accumulator |
| 2 | instantaneous cadence | rpm, `0xFF` invalid |
| 3..4 | accumulated power | u16 LE accumulator, 1 W, wraps at 65536 |
| 5 + 6 bits 0..3 | instantaneous power | 12 bits LE, 1 W, `0xFFF` invalid |
| 6 bits 4..7 | trainer status bit field | Table 8-27 |
| 7 bits 0..3 | flags bit field | Table 8-28 |
| 7 bits 4..7 | FE state bit field | Table 8-10 |

**The accumulated-power field starts at byte 3, one earlier than Bicycle Power
page `0x10`'s, which puts it at bytes 4..5.** Two profiles, two offsets, the
same quantity in the same units.

Page 26 (`0x1A`) Specific Trainer *Torque* Data is deliberately absent: power
must be computed from it, every trainer that sends it also sends `0x19`, and a
second differently-scaled route to the same watts is a second thing to get
wrong.

#### Four traps in these two pages

1. **The FE state field is a nibble inside a nibble.** Both tables give byte 7
   as "FE State Bit Field, 4 Bits (4:7)", and Table 8-10 then numbers the bits
   *within* that nibble: bits 0–2 are the state, bit 3 is the lap toggle. So the
   state is `(byte7 >> 4) & 0x07` and the lap toggle is bit 7 of byte 7.
   `(byte7 >> 4) & 0x0F` folds the toggle into the state and turns `IN_USE` (3)
   into 11 on every other lap. States: 1 asleep, 2 ready, 3 in use, 4 finished;
   0 and 5–7 are reserved and are passed through rather than remapped, because
   "reserved" here means a future state and not an error.
2. **`0xFFF` instantaneous power invalidates the accumulator too.** Table 8-25
   says so in as many words. This is the only sentinel in this project where one
   field's invalid value condemns a *different* field, and a receiver honouring
   only the instantaneous half keeps integrating a frozen accumulator.
3. **Distance has no invalid value — it has an enable bit.** §8.5.2.3 states
   this outright: all 256 values of byte 3 are legal distances, and whether byte
   3 means anything is capabilities bit 2. The general rule (§8.5.2.6.1) is that
   *accumulating* optional fields are gated by capabilities bits while
   *instantaneous* ones carry an all-ones sentinel. Reaching for a sentinel on
   distance reads 255 m as "absent". This is the opposite convention from
   §3.11's, and it is the profile's, not an error.
4. **Speed may be a lie, on purpose.** Capabilities bit 3 (§8.5.2.6.2, §6.4.2)
   says bytes 4..5 are *virtual* speed — what the rider would have been doing
   had the trainer been able to apply negative resistance. Not an error, not
   invalid, simply not a measurement of anything physical, so it is reported as
   a flag and the consumer decides.

**Elapsed time is session time, not wall time.** §8.5.2.2: the accumulator
"shall only increment when the fitness equipment is in the `IN_USE` state", so
differencing it across a pause understates the interval by the pause. Correct
for a workout timer, wrong as a basis for integrating power over time — which is
why `radiant_power_adapter.c` integrates against the bridge's own `t_us`
instead.

#### The transmit half, and the treadmill pages — `src/profiles/profile_fec_tx.c`

`apps/treadmill` made this project fitness equipment as well as a receiver, so
the paragraph that used to stand here ("there is no encoder either — nothing in
this tree emulates fitness equipment") is retired. What replaced it is a
**second translation unit**, deliberately not an extension of
`profile_fec.c`: a receiver links the decoder and a treadmill links the
encoder, which is the same split `profile_power_decode.c` makes against
`profile_power.c`, and the linker is what enforces it.

| Page | Name | What it carries |
|---|---|---|
| `0x11` (17) | General Settings | Cycle length (stride length, 0.01 m); **incline, sint16 LE, 0.01 %, invalid `0x7FFF`**; resistance level |
| `0x12` (18) | General FE Metabolic | METs (0.01), caloric burn rate (0.1 kcal/h), accumulated calories. Optional |
| `0x13` (19) | Specific Treadmill Data | Cadence in **strides**/min; negative and positive vertical distance, both unsigned accumulators in 0.1 m |
| `0x36` (54) | FE Capabilities | Maximum resistance; **byte 7 bit 2 = Simulation mode** |
| `0x47` (71) | Command Status | Last command id, sequence, status, and a **four-byte echo of the command's own bytes 4–7** |
| `0x30`–`0x33` (48–51) | Basic Resistance, Target Power, Wind Resistance, **Track Resistance** | Decoded. They arrive as acknowledged data from a controller |
| `0x37` (55) | User Configuration | Decoded, user weight only |
| `0x46` (70) | Request Data Page | Decoded; drives on-request delivery of 54, 71, 80 and 81 |

**Track Resistance is the incline command, and the specification says so.**
§8.8.4.1 (p.58): *"Controllable fitness equipment that is capable of adjusting
the incline directly should apply the grade simulation parameter in this way.
Other fitness equipment that is not capable of adjusting the incline (e.g.
controllable trainers) shall use the grade field to calculate gravitational
resistance to apply to the user."* Page 51 is written treadmill-first and the
trainer reading is the fallback, so **no spec extension and no RadiANT-private
incline page is needed** — see
[ADR 0017](decisions/0017-fec-treadmill-control.md).

**Grade and incline are the same quantity with different encodings, different
ranges and different sentinels.** This is the trap of the transmit half:

| | Page 51 grade | Page 17 incline |
|---|---|---|
| Type | **unsigned** u16, biased | **two's complement** sint16 |
| Scale | `Grade% = raw × 0.01 − 200.00` | 0.01 % directly |
| Zero | `0x4E20` (20000) | `0x0000` |
| Range | ±200.00 % | ±100.00 % (§10.1.2.1) |
| Invalid | `0xFFFF` → *assume flat* | `0x7FFF` → *cannot report* |

`0xFFFF` read as an incline is −0.01 %, and `0x7FFF` read as a grade is
+127.11 %. Each is a legal-looking reading of the other's sentinel, and neither
produces an error anywhere.

**The interleave is a spec requirement, and it is not the 119/120/121 cadence
`profile_sched.c` implements.** §10.1 (p.68): page 16 twice consecutively every
4 messages *or* once every 5th; the equipment-specific page (19 for a
treadmill) at least once every 5; pages 17 and 18 at least once every 20;
common 80 and 81 as **two consecutive** background pages every **66**. 66 is not
121 and the engine's placement is hard-coded, so FE-C leaves `common_80`/
`common_81` NULL and claims those two slots through the client seam instead.
`radiant/tests/src/test_profile_fec_tx.c` measures the worst gap of each rule
over 264 messages rather than trusting the schedule to be right by
construction.

**Certification, recorded because it is a process fact and not a wire fact.**
§10.1.3.1 (p.69) says *"At this time, only trainer fitness equipment types may
support the controllable fitness equipment use case."* The certification
programme closed on 2025-06-30 and this project does not certify, so the note
constrains nothing technically. It is written down in ADR 0017 so a future
reader finds it rather than rediscovering it.

**Still out of scope:** calibration pages `0x01`/`0x02`, the torque page `0x1A`,
and the equipment-specific pages for the other machine types (20–24).

### 3.17 Stride-Based Speed and Distance (SDM), device type `0x7C`

Implemented (encode) in `src/profiles/profile_sdm.c`, mirrored in
`tools/ant_pages.py`.

**Why a treadmill emits this beside FE-C.** FE-C is what a control-capable head
unit pairs with; SDM is what everything that only knows foot pods pairs with,
which includes Zwift Run and most watches. Two device types, two periods, no
shared page space — so `apps/treadmill` runs two independent masters rather
than one channel with a wider page set.

**Channel.** Device type `0x7C` (124), transmission type 5, RF 57, period
**8134** (~4.03 Hz).

| Page | Name | Encoding |
|---|---|---|
| `0x01` (1) | Default Data | `[1]` time 1/200 s, `[2]` time s, `[3]` distance m, `[4]` distance 1/16 m **high nibble** + speed m/s **low nibble**, `[5]` speed 1/256 m/s, `[6]` **stride count**, `[7]` update latency 1/32 s |
| `0x02` (2) | Base | `[3]` cadence strides/min, `[4]` cadence 1/16 high + speed integer low, `[5]` speed 1/256, `[7]` status. `[1]`, `[2]`, `[6]` reserved **`0x00`** |
| `0x03` (3) | Calories | Page 2's template with accumulated kcal in `[6]` |
| `0x10` (16 dec) | Distance and Strides Summary | `[1..3]` strides, 24-bit LE; `[4..7]` distance, 32-bit LE, 1/256 m. Request-only |
| `0x16` (22 dec) | Capabilities | `[1]` flag bits for time / distance / speed / latency / cadence / calories. **Required on request** |

**Four things here are the opposite of every other profile in this document.**

1. **The period is 8134**, not 8070 and not 8192. A wrong channel period does
   not fail loudly — the channel simply never opens, and nothing on either side
   names the period as the reason.
2. **There is no `0xFF` invalid convention.** An unused field is `0x00` and
   validity is out of band, in page 22's capability bits. `0xFF` in a cadence
   byte is a real 255 strides/min. This is the reverse of §3.16's FE-C rule and
   of §3.11's, and it is the profile's, not an error.
3. **"Page 16" is `0x10` and "page 22" is `0x16`** — the same two digits. The
   document names pages in decimal and the wire carries hex, so the summary
   page and the capabilities page swap places for anybody who reads one number
   in the other base.
4. **Strides are not steps.** One stride is two footfalls, so a runner at 170
   steps per minute is at 85 strides per minute here. FE-C page 19 also counts
   strides; **BLE RSC counts steps** and every real client expects steps there.
   One conversion, one direction, one helper —
   `treadmill_rsc_cadence_from_strides()` in `apps/treadmill` — or exactly one
   of the three outputs this project emits will be wrong by a factor of two.

**The interleave** is `1, 1, X, X` repeating for 64 messages, then pages 80 and
81 twice consecutively: a 66-message cycle, where X alternates between page 2
and page 3 group by group. As with FE-C, 66 is not 121, so the common pair
rides `profile_sched.c`'s client seam rather than its built-in cadence.

**Status byte** (`[7]` of pages 2 and 3): bits 7..6 location, 5..4 battery,
3..2 health, 1..0 use state. A treadmill reports location **Other** — it is not
on anybody's shoe — and battery **New**, because it is on mains.

---

## 4. Two vocabulary types this document allocates

Both recipes and both device types below need quantities the section 7
vocabulary of `docs/radiant-telemetry.md` does not have. They are allocated
**here and in `tools/ant_pages.py` together**, which is that document's stated
rule: a type that exists in one and not the other is exactly the drift the
mirror exists to prevent.

| Type | Quantity | Canonical unit | Matter cluster | Why |
|---|---|---|---|---|
| `0x27` | time interval | s | — | An **instantaneous** duration. Class `0x30-0x3F` requires the accumulate bit, so `0x37` duration cannot express "this reading is 210 seconds old" or an R-R interval |
| `0x28` | substance concentration | mol/m^3 | — | Blood glucose, and any other molar concentration. **1 mmol/L is exactly 1 mol/m^3**, so the canonical SI unit is the clinical unit and no conversion constant is needed |

**`0x27` is the gap `docs/radiant-telemetry.md` section 7 named and deliberately
left open**, for the RR interval an ANT+ heart rate page 4 carries. The CGM
profile's reading-age field needs the same type, so the gap closes here. Its
first two users are unrelated, which is the evidence that it belongs in the
vocabulary rather than in a profile.

**`0x28` is deliberately not "blood glucose".** A type is a quantity, not an
application; naming it for its first user is how a vocabulary ends up with three
concentration types that differ only in what measured them. `0x21` gas
concentration (ppm) and `0x22` mass concentration (µg/m³) already exist and are
different quantities, not different names for this one.

---

## 5. RadiANT device types

Both are the `0x60` envelope with a pinned schema, per section 1. Both partition
all 256 page numbers, because `scripts/check_profile_registry.py` requires it of
any type with status `radiant` and because an unassigned page number should be a
recorded decision.

### 5.1 Continuous glucose monitor, device type `0x61`

| Parameter | Value |
|---|---|
| Device type | `0x61` |
| Channel period | **65535** (0.5 Hz), the slowest strictly-periodic ANT master |
| RF channel | 57 |
| Transmission type | `0x05` |
| Envelope | `0x60`, version 1 |
| Transforms | **`X_AUTH` mandatory.** See below |
| Schema id | `0x01` for the field set below |

<!-- radiant-registry: pagemap-0x61 -->

| Page | Name | Summary |
|---|---|---|
| `0x00` | Descriptor | The self-describing schema, sent as a set of consecutive frames |
| `0x01-0x0F` | Data | Packed field values against the announced schema |
| `0x10` | Reliable command | Command + sequence + inline tag, receiver -> node |
| `0x11` | Command acknowledge | Result + sequence + inline tag, node -> receiver |
| `0x12` | Sync handoff | One receiver tells another where and when a node it already tracks transmits |
| `0x13` | Frequency move | Target RF index and a countdown; receivers act on expiry, not receipt |
| `0x14-0x1F` | Reserved | Unassigned; a receiver ignores these |
| `0x20-0x2F` | Reserved for the security envelope | Epoch and key-generation announcement, v2 TESLA key disclosure |
| `0x30-0x4F` | Reserved | Unassigned |
| `0x50` | Common page 80 | Manufacturer information, byte-exact ANT+ |
| `0x51` | Common page 81 | Product information, byte-exact ANT+ |
| `0x52` | Common page 82 | Battery status, byte-exact ANT+ |
| `0x53-0xEF` | Reserved | Other ANT+ common pages and future RadiANT pages |
| `0xF0-0xFF` | Vendor-private | Never registered, never bridged, never assumed to mean anything |

**The mandatory schema.** Field area is bits 0..39 of page `0x01`, because
`X_AUTH` claims byte `[7]`.

| Field id | Quantity | Type | Acc | Signed | Width | Exp | Page | Bit offset | Invalid |
|---|---|---|---|---|---|---|---|---|---|
| `0x01` | glucose concentration | `0x28` | no | no | 12 | -2 | `0x01` | 0 | `0xFFF` |
| `0x02` | reading age | `0x27` | no | no | 12 | 0 | `0x01` | 12 | `0xFFF` |
| `0x03` | trend | `0x40` | no | no | 4 | 0 | `0x01` | 24 | `0xF` |
| `0x04` | sensor status | `0x40` | no | no | 4 | 0 | `0x01` | 28 | `0xF` |

32 of 40 bits used; 8 remain for a profile revision to claim.

- **Glucose is 0.01 mmol/L over 0..40.95 mmol/L.** That is 0.18 mg/dL, which is
  already finer than any sensor's accuracy, and it is *deliberately* not the
  drafted 0.1 mg/dL: a CGM's MARD is around 9 %, so a tenth of a mg/dL is three
  significant figures of false precision. mol/m³ is the canonical unit and is
  numerically identical to mmol/L; a receiver displaying mg/dL multiplies by
  18.016.
- **Reading age is 12 bits of seconds, 0..4094.** The drafted u8 capped at 255 s
  and **a CGM updates every 5 minutes**, so the field overflowed during normal
  operation and reported a fresh reading as stale. This is the one drafted field
  that was outright broken rather than merely arguable.
- **Trend** is the sensor's own smoothed answer over ~15 minutes and is *not*
  receiver-derivable from a few 0.5 Hz samples, which is why it survives rule 6
  of section 1 where the drafted temperature profile's trend field does not.

Trend code space, `0x40` generic enum declared here:

| Code | Meaning |
|---|---|
| 0 | falling rapidly, faster than 2 mg/dL/min |
| 1 | falling, 1 to 2 mg/dL/min |
| 2 | falling slowly |
| 3 | stable |
| 4 | rising slowly |
| 5 | rising, 1 to 2 mg/dL/min |
| 6 | rising rapidly, faster than 2 mg/dL/min |
| 7..14 | reserved |
| 15 | not available |

Sensor status code space:

| Code | Meaning |
|---|---|
| 0 | OK |
| 1 | warming up |
| 2 | expired |
| 3 | error, replace sensor |
| 4 | calibration required |
| 5..14 | reserved |
| 15 | not available |

#### Why `X_AUTH` is mandatory here and nowhere else

Every other profile in this document treats the security transforms as an
opt-in, because a wrong wind reading costs an aero estimate and a wrong PM2.5
reading costs a decision about a window. **A forged glucose reading causes
someone to eat, or not eat.** That is the only profile in this project where an
injected packet has a direct physiological consequence, and a broadcast on an
open network with no authentication is trivially injectable by anyone in range.

So, normative:

- A `0x61` node **must** set `X_AUTH` in descriptor frame 0 byte `[7]`.
- A receiver **must not display a reading it has not verified.** Not as a number
  with a warning icon, not greyed out — an unverified or unverifiable reading is
  reported as **no reading**, the same way heart rate's `0` byte is.
- The same applies to a *stale* reading: a receiver **must** compare the
  reading-age field against its own tolerance and report no reading past it,
  rather than displaying an old number as a current one.

The consequence is stated rather than hidden: **this profile cannot ship until
the security layer does.** `docs/radiant-security.md` owns that layer, the
descriptor authentication frame is specification-only today, and a `0x61` node
built before it exists would be one that this document says must not be
displayed. That is the honest position and it is preferable to a profile that
ships sooner and is wrong in the field.

**This rule is CGM's because CGM is the profile in this document that needs
it, not because CGM is special.** Any future `0x60` recipe whose reading has a
direct physiological consequence — blood pressure (ANT+ device type `0x12`,
which this project does not implement or decode; no reference section exists
for it because its page layout was not found anywhere verifiable) is the
obvious next candidate — inherits this section's reasoning and its rules
without needing its own restatement. A recipe that meets the "wrong reading,
physiological consequence" test and skips `X_AUTH` has not made a different
engineering choice; it has not read this section.

#### The collision risk, recorded rather than solved

**An ANT+ Glucose Device Profile existed as a members-only beta** — ANT+ tech
bulletin, 5 March 2013, "New revision of CGM Profile 1.0_Beta.009". It never
reached a public 1.0, it is absent from the public device profile list and from
the Android ANT+ plugin's `DeviceType` enum, and **its device type number could
not be verified**.

So `0x61` is claimed knowing that an unpublished 2013 allocation could be
anywhere in the space, including here. Nothing reduces that risk to zero. What
bounds it is the registry's standing rule: a receiver treats a device type as a
hint, the descriptor carries an envelope version and a schema id, and a receiver
that decodes on device type alone has no defence against a collision — which is
the receiver's bug. A foreign profile's bytes will not parse as a valid
descriptor set, and with `X_AUTH` mandatory they will not verify either.

### 5.2 Smart fan, device type `0x62`

| Parameter | Value |
|---|---|
| Device type | `0x62` |
| Channel period | **8192** (4.00 Hz) |
| RF channel | 57 |
| Transmission type | `0x05` |
| Envelope | `0x60`, version 1 |
| Transforms | Optional. `X_AUTH` recommended wherever an unauthenticated actuator is a nuisance |
| Schema id | `0x01` for the field set below |

The period is 4 Hz **because commands arrive near the node's own transmit slot**,
so the period is the command latency: 8192 counts gives 250 ms. A slower fan
would be cheaper on battery, and a fan is mains-powered.

<!-- radiant-registry: pagemap-0x62 -->

| Page | Name | Summary |
|---|---|---|
| `0x00` | Descriptor | The self-describing schema, sent as a set of consecutive frames |
| `0x01-0x0F` | Data | Packed field values against the announced schema |
| `0x10` | Reliable command | Command + sequence + inline tag, receiver -> node |
| `0x11` | Command acknowledge | Result + sequence + inline tag, node -> receiver |
| `0x12` | Sync handoff | One receiver tells another where and when a node it already tracks transmits |
| `0x13` | Frequency move | Target RF index and a countdown; receivers act on expiry, not receipt |
| `0x14-0x1F` | Reserved | Unassigned; a receiver ignores these |
| `0x20-0x2F` | Reserved for the security envelope | Epoch and key-generation announcement, v2 TESLA key disclosure |
| `0x30-0x4F` | Reserved | Unassigned |
| `0x50` | Common page 80 | Manufacturer information, byte-exact ANT+ |
| `0x51` | Common page 81 | Product information, byte-exact ANT+ |
| `0x52` | Common page 82 | Battery status, byte-exact ANT+ |
| `0x53-0xEF` | Reserved | Other ANT+ common pages and future RadiANT pages |
| `0xF0-0xFF` | Vendor-private | Never registered, never bridged, never assumed to mean anything |

**The mandatory schema**, on page `0x01`:

| Field id | Quantity | Type | Acc | Signed | Width | Exp | Page | Bit offset | Invalid |
|---|---|---|---|---|---|---|---|---|---|
| `0x01` | current fan speed | `0x24` | no | no | 8 | 0 | `0x01` | 0 | `0xFF` |
| `0x02` | target fan speed | `0x24` | no | no | 8 | 0 | `0x01` | 8 | `0xFF` |
| `0x03` | fan mode | `0x42` | no | no | 4 | 0 | `0x01` | 16 | `0xF` |

Both speeds are 0..100 %. Publishing target alongside current is what lets a UI
show a fan spooling up rather than reporting a lag as an error.

Commands use pages `0x10` and `0x11` unchanged: **`0x02` set level** with target
field `0x02`, and **`0x04` set mode / enum** with target field `0x03`. No command
id is added, and no bespoke command page exists.

#### The auto-mode design hole, and its resolution

The drafted profile had the head unit send the fan an HR threshold — "start at
120 bpm, full at 170" — and never said where the fan got heart rate from. There
are only two possibilities and the draft specified neither: the fan opens its own
ANT channel to an HR strap (and then needs that strap's device number, which was
in no page), or the head unit runs the loop (and then the thresholds are dead
bytes).

**Resolved in favour of the head unit owning the control loop.** The fan is a
0..100 % actuator with a mode. It does not receive heart rate, it does not
implement a controller, and it has no threshold fields. The head unit already has
every input — HR, power, speed, and any `0x60` core-temperature node — and is the
only place all of them exist at once.

That deletes four bytes from the control page and makes the profile
implementable by a fan vendor with no ANT receiver in the product. The `0x42` fan
mode field remains because a fan may have modes of its own (off, manual, sleep,
oscillate) that are not a speed.

#### This profile is for a fan running RadiANT, and the Matter path is a different fan

`docs/radiant-bridge.md` §8.4 also describes driving a fan from a heart rate,
over Matter, and the two are not competing designs. **This profile is the case
where RadiANT firmware runs on the fan itself** — the fan is an ANT node with a
descriptor, a schema and pages `0x10`/`0x11` as its whole control surface, and a
head unit commands it directly with no Thread, no Matter and no bridge anywhere
in the path. The bridge's case is an ordinary smart fan that speaks Matter and
has never heard of RadiANT, which cannot use this profile because it has no ANT
radio.

`radiant-bridge.md` §8.4a is the comparison in full. Nothing in this section
depends on it.

---

## 6. `0x60` schema recipes

A recipe is a **published field set on device type `0x60`**: the same envelope,
no device type of its own, no registry claim, no receiver change. A node
implements one by putting these fields in its descriptor. A receiver that
recognises the schema id can name the node; a receiver that does not still
decodes every value, because the descriptor says what they are.

**Recipes are not normative in the way sections 3 and 5 are.** They are the
settled-enough form of a field set, published so that two implementers of the
same sensor produce the same descriptor. Section 7's promotion bar is how one
becomes a device type.

### 6.1 Apparent wind

The registry already recorded this one under "not claimed, and why" — it ships on
`0x60` first, and a device type is worth claiming once its fields have settled.
This recipe is that first ship, not a claim.

| Field id | Quantity | Type | Acc | Signed | Width | Exp | Page | Bit offset |
|---|---|---|---|---|---|---|---|---|
| `0x01` | apparent wind speed | `0x16` | no | no | 12 | -2 | `0x01` | 0 |
| `0x02` | apparent wind yaw angle | `0x18` | no | **yes** | 12 | -3 | `0x01` | 12 |
| `0x03` | differential (pitot) pressure | `0x12` | no | **yes** | 16 | -1 | `0x01` | 24 |
| `0x04` | sensor status | `0x40` | no | no | 4 | 0 | `0x01` | 40 |
| `0x05` | static pressure | `0x12` | no | no | 20 | 0 | `0x02` | 0 |
| `0x06` | air temperature | `0x10` | no | no | 12 | -1 | `0x02` | 20 |
| `0x07` | relative humidity | `0x11` | no | no | 8 | 0 | `0x02` | 32 |
| `0x08` | air distance | `0x34` | **yes** | no | 24 | 0 | `0x03` | 0 |

- **Speed is 0.01 m/s over 0..40.95 m/s**, and the unit is m/s and not "0.01 m/s
  or kph" — the drafted line offered a choice, and a unit a sender picks is not a
  unit.
- **Yaw is 0.001 rad**, about 0.057°, signed over ±4.095 rad. The drafted 1° in an
  int16 was wrong twice: ±180° needs 9 bits, and CdA work needs far better than a
  degree.
- **Air density is deleted.** It is computed from static pressure, temperature and
  humidity, all three of which are now published. A node that publishes only the
  density it computed has thrown away the inputs and given the receiver a number
  it cannot check or recompute — rule 6 of section 1.
- **Differential pressure is published raw**, signed, 0.1 Pa. Dynamic pressure at
  15 m/s is about 138 Pa. This is the field that makes field recalibration and
  zero-offset correction possible, and the drafted profile had no equivalent.
- **Air distance is the accumulating partner** section 5 of
  `docs/radiant-telemetry.md` requires: speed `0x16` integrates to distance
  `0x34`, and the accumulator is what survives a lost packet.

Sensor status: 0 calibrating, 1 ready, 2 blocked or obstructed, 3 fault,
4..14 reserved, 15 not available.

### 6.2 Air quality

| Field id | Quantity | Type | Acc | Signed | Width | Exp | Page | Bit offset |
|---|---|---|---|---|---|---|---|---|
| `0x01` | PM2.5 mass concentration | `0x22` | no | no | 12 | 0 | `0x01` | 0 |
| `0x02` | PM10 mass concentration | `0x22` | no | no | 12 | 0 | `0x01` | 12 |
| `0x03` | CO2 concentration | `0x21` | no | no | 16 | 0 | `0x01` | 24 |
| `0x04` | air temperature | `0x10` | no | no | 12 | -1 | `0x02` | 0 |
| `0x05` | relative humidity | `0x11` | no | no | 8 | 0 | `0x02` | 12 |

Every one of these is an existing vocabulary type, so this recipe adds nothing to
any table. Three drafted fields are deliberately gone:

- **The composite AQI score and the AQI standard indicator.** AQI is *derived*
  from the concentrations, and the breakpoints change — the US EPA revised the
  PM2.5 breakpoints in 2024. A firmware-computed AQI is a number that goes stale
  in a deployed sensor and can never be corrected, where a receiver-computed one
  is fixed by a software update. The node publishes what it measured.
- **The VOC index.** It is explicitly sensor-vendor-specific and therefore not
  comparable between two sensors, which is the one property a core interoperable
  field must have. A node that wants to publish it uses a vendor-private type in
  `0xF0..0xFF`, which the bridge drops by rule.
- **Ozone and nitrogen dioxide.** Real sensors for both are rare and expensive.
  Nothing stops a node adding them as `0x21` fields — the envelope does not need
  permission — but they are not core.

**Two fields are added, and they matter more than anything dropped.** CO2 is the
most-wanted indoor air number and the draft omitted it entirely. Relative
humidity is not a courtesy: PM mass readings from an optical sensor are
humidity-dependent through hygroscopic particle growth, so **a PM2.5 number
without an RH number alongside it is not comparable to another sensor's**, and a
receiver cannot correct what it was not told.

Rate: an air quality node is the archetypal sparse-mode node. The drafted 2 Hz is
roughly a hundred times what the sensors' own averaging windows support.

### 6.3 Physiological temperature

| Field id | Quantity | Type | Acc | Signed | Width | Exp | Page | Bit offset |
|---|---|---|---|---|---|---|---|---|
| `0x01` | body temperature | `0x10` | no | no | 16 | -2 | `0x01` | 0 |
| `0x02` | sensor placement | `0x40` | no | no | 4 | 0 | `0x01` | 16 |
| `0x03` | measurement quality | `0x24` | no | no | 8 | 0 | `0x01` | 20 |

Temperature is in kelvin at 0.01 K, the vocabulary's canonical unit — 310.15 K is
37.00 °C. The drafted encoding was a u16 at 0.01 °C with a -20 °C offset, which
spans -20 to +635 °C for a quantity whose entire physiological range is 20 to
45 °C, and which invents a second temperature representation for a vocabulary
that already has one.

Placement: 0 core, ingestible; 1 core, calculated from a wearable; 2 skin;
3 tympanic; 4 axillary; 5 oral; 6 generic; 7..14 reserved; 15 not available.

**Measurement quality is the field the draft was missing and the one that
matters most.** A calculated core temperature — the wearable kind, not the pill —
is a model output, and during warm-up or under poor skin contact it is wrong by
degrees while looking entirely plausible. Devices in this class report a
confidence, users are told to ignore readings below it, and a profile without it
publishes a number a receiver cannot qualify.

**The drafted trend field is gone.** Unlike the CGM's, it is a receiver-derivable
smoothing of a 1 Hz signal, and rule 6 of section 1 applies.

#### Why this is not an additive page on ANT+ Environment `0x19`

ADR 0008 permits adding a page number to an ANT+ device type, and `0x19` has a
page-number byte, so the mechanism is available. It is still refused, for a
structural reason rather than a preference:

**Environment page 1 *is* the ambient temperature reading, and the profile has no
placement field of any kind** (section 3.7). So a body-temperature sensor
claiming device type `0x19` has exactly two options, and both are wrong:

- **Emit page 1 with body temperature.** Every existing tempe receiver in range
  then displays 37 °C as the air temperature. The additive-page mechanism
  protects a legacy receiver from pages it does not know; it does nothing about a
  page it *does* know carrying a different quantity than it expects.
- **Omit page 1.** The node is then an Environment sensor that never sends the
  Environment profile's only data page — a broken sensor to every receiver, which
  will report it as a fault rather than as a different kind of device.

There is no third option, and no additive page fixes either. A quantity that
needs a placement field cannot live in a profile that has none.

**ANT+'s own Core Temperature profile, device type `0x7F` (section 3.8), is
prior art for this reasoning rather than a counter-example.** When ANT+ needed
physiological temperature, it did not add a page to Environment either — it
minted a separate device type. That is the same structural answer this section
reaches by a different vehicle (a `0x60` recipe instead of a device type,
because section 5's promotion bar has not been met by anything here yet). Its
own page 0 carries a data-quality field, independently arriving at the same
field this recipe carries for the same reason: a calculated core temperature is
a model output that can be wrong during warm-up, and a receiver needs to be told
when to distrust it.

### 6.4 Presence lock

This is the drafted immobilizer profile with its premise and its cryptography
both removed. What survives is small, and that is the finding.

| Field id | Quantity | Type | Acc | Signed | Width | Exp | Page | Bit offset |
|---|---|---|---|---|---|---|---|---|
| `0x01` | lock state | `0x05` | no | no | 2 | 0 | `0x01` | 0 |
| `0x02` | vehicle speed | `0x16` | no | no | 12 | -2 | `0x01` | 2 |

Lock state: 0 unlocked, 1 locked, 2 alarm triggered, 3 not available. Commands
use page `0x10` command **`0x04` set mode / enum** against field `0x01`, with the
existing sequence number, epoch-covered tag, idempotency rule and acknowledge
page. **No rolling code, no key id, no challenge nonce, no bespoke command page.**

Three normative rules, and they are the whole profile:

1. **A node changes lock state only on an authenticated command.** It **must
   not** change state on the loss of a receiver, a beacon, or any other presence
   signal. The drafted design auto-locked when an ANT signal went away, and this
   project has *measured* its own link rather than assuming it: a **~0.4 % loss
   floor** characterised in `docs/spike-b-part2-results.md`, plus the drop and
   re-acquire behaviour of a tracking receiver in `docs/radiant-telemetry.md`
   section 8. An immobilizer built on "the signal stopped" engages on a normal
   dropout, and the dropouts are not rare.
2. **A node must refuse to engage while the vehicle is moving.** Field `0x02`
   exists for this and is required on any node that can immobilize. The command
   is rejected with result code `0x06` (busy) while speed is non-zero. A motor
   cut-off on a moving bicycle is an injury, not a security feature.
3. **Presence-based unlocking is the receiver's job, not the node's.** A watch or
   phone that wants to unlock on approach issues a real, authenticated,
   idempotent command. That keeps the entire trust decision in a place where a
   user can see it and a log can record it, and it means a dropout unlocks
   nothing and locks nothing.

**The privacy cost is stated because it is the profile's worst property.** A node
that broadcasts "I am locked" at 1 Hz on a stable device number lets anyone with
a receiver scan a bike rack for locked bikes and follow one across sessions. The
device-number rules of `docs/profile-registry.md` and the page 80/81/82 privacy
rules apply here with more force than anywhere else in this document, and a
deployment that cannot accept a discoverable beacon should not run this recipe at
all.

---

## 7. Considered and not pursued

### 7.1 E-bike / smart commuter

**ANT+ LEV, device type `0x14`, already is this profile.** Section 3.6 has the
field list from the primary specification. Of the drafted profile's fields, LEV
carries battery state of charge, assist level in two forms, remaining range,
motor and battery temperature, light status with high beam, and error codes —
plus odometer, battery voltage, charge cycles, gear state, throttle, turn signals
and regen level, none of which the draft had. It has an acknowledged command page.

The gap is three fields: **rider power in watts, motor power in watts, and walk
assist.** That gap does not justify a profile:

- **Rider power is already a complete ANT+ profile.** An e-bike that wants to
  publish it broadcasts ANT+ Bicycle Power `0x0B` on a second channel, which
  every head unit and every training app already decodes, with accumulators this
  project already implements.
- **Motor power** is partly served by LEV's % assist, which is
  `motor / (motor + rider)`. A true watt figure is a real gap.
- **Walk assist** is absent from LEV entirely, including from its command page.

**If this is revived, it is an additive page on `0x14` and nothing else.** The
route is open: LEV has a page-number byte, pages 6..15 are reserved for future
use, and the RadiANT compat allocation `0x70`-`0x72` is unused by LEV, whose only
high page numbers are 16, 34 and the common pages. The conditions ADR 0008
imposes are met. What is missing is a builder — the fields would be motor power,
rider power and a walk-assist command, and this project does not have an e-bike
to verify them against.

Two drafted encodings are recorded because they were errors rather than choices:
`int8` cannot represent "-40 °C to +215 °C" (that is a u8 with a -40 offset, and
the line contradicted itself), and an unsigned u16 for motor power cannot express
regenerative braking.

### 7.2 Water sports / rowing and paddling

**No ANT+ profile covers on-water rowing or paddling, and shipping products
already solved it without one.** The Vaaka canoe and kayak sensor reports stroke
rate as an **ANT+ Bike Cadence `0x7A`** sensor. The PerformanceBlade kayak power
meter presents as an **ANT+ Bicycle Power `0x0B`** meter. Both work with every
existing head unit today.

Indoor rowing is covered: **FE-C `0x11`, equipment type 22, page 22**, carrying
stroke count as a u8 accumulator, cadence, and instantaneous power. **The "22,
22" is confirmed against Concept2's own engineering documentation for the
byte layout, but not against a primary ANT+ spec, and it should not be read as
"page number equals equipment-type code" being a general FE-C rule** — a
separate cross-check (GoldenCheetah's decoder) shows FE-C's general page 16
carries the equipment-type code as a data field while equipment-specific pages
are independently numbered elsewhere (stationary bike at `0x15`, trainer at
`0x19`), so 22-and-22 here is Concept2's own coincidence rather than a pattern
to lean on. Confidence: medium, single vendor source, worth a primary-spec
check before this claim is load-bearing for anything.

So the honest recommendation is the one the market already reached: a paddle
sensor broadcasts `0x0B`, where a stroke is a crank revolution and the torque
accumulators are real, and puts anything paddling-specific on a `0x60` channel
alongside. This project blesses no field set for the latter, because it has no
paddle sensor to settle one against.

Three drafted fields are recorded as *not* recommended to anyone who does build
one. **Distance per stroke** is not measurable by a paddle-mounted sensor, which
has no distance reference at all. **Stroke rate** is derivable from the
accumulating stroke count the same profile already carries, which is the
convenience-versus-accumulator pattern applied backwards. **Slip / wash as a
percentage of wasted energy** has no vendor-independent definition, so two
implementations would produce different numbers under one field name — the exact
failure a shared vocabulary exists to prevent.

### 7.3 The promotion bar

A recipe becomes a device type when, and only when:

1. A real node ships it on `0x60` and the field set has not changed for two
   further releases.
2. A receiver can state what it gains from recognising the type **before**
   decoding a descriptor. "It would be tidier" is not that.
3. The registry's collision check has been repeated at claim time, not reused
   from the proposal.

`0x61` and `0x62` are claimed against criterion 2 alone, which is a departure
recorded here rather than hidden: neither has shipped. If either fails to ship,
the registry's rule is that the row stays with its status changed and the number
is not recycled.

---

## 8. Where the code lives

| File | What it is |
|---|---|
| `src/profiles/profile_hr.c` | ANT+ heart rate `0x78` |
| `src/profiles/profile_power.c` | ANT+ bicycle power `0x0B`, transmit side |
| `src/profiles/profile_power_decode.c` | ANT+ bicycle power page `0x10`, receive side — its own translation unit so a receiver does not link a transmitter (§3.1) |
| `src/profiles/profile_fec.c` | ANT+ fitness equipment `0x11`, pages 16 and 25, decode only (§3.16) |
| `src/profiles/profile_fec_tx.c` | ANT+ fitness equipment `0x11`, the **transmit** half: a treadmill's pages 16, 17, 18, 19, 54 and 71, plus decoders for control pages 48–51, 55 and 70 (§3.16) |
| `src/profiles/profile_sdm.c` | ANT+ stride-based speed and distance `0x7C`, pages 1, 2, 3, 16 and 22 (§3.17) |
| `src/profiles/profile_env.c` | ANT+ Environment `0x19`, pages 0 and 1, decode only (§3.7) |
| `src/profiles/profile_common.c` | Common pages 80, 81, 82 (encode) and 84 (decode) — two provenances, kept separate (§3.4) |
| `src/profiles/profile_telemetry.c` | The `0x60` envelope, and therefore every profile in sections 5 and 6 |
| `src/profiles/profile_bits.c` | The MSB-first packer whose vectors are the shared contract |
| `tools/ant_pages.py` | All of the above, mirrored, plus the field-type vocabulary |

A capture of the C stream is committed as `tools/vectors/compat-hr.antcap` and
`tools/vectors/compat-power.antcap`, and `tools/test_compat_capture.py` decodes
every message with the Python encoders and re-encodes it, asserting the bytes
come back identical. A table in section 3 that disagreed with either
implementation therefore fails a test rather than sitting in a document.

Nothing in sections 5 and 6 has an implementation yet. They are schemas for a
codec that already exists, which is the point of section 1.

---

## 9. The rest of the official ANT+ profile list

**Not implemented, not decoded, not registered, not machine-checked.** This
table exists so that a reader of this document sees the whole landscape in one
place rather than concluding — wrongly — that a profile's absence from
sections 1–8 means this project overlooked it or considers it out of scope. It
did not, and it does not. Nothing here is a claim: `docs/profile-registry.md`'s
own stated policy is that it records only device types this project
implements, decodes, or has a documented reason to depend on, and that policy
is correct and unchanged. Before building against or claiming any of these,
verify independently against `thisisant.com/developer/ant-plus/device-profiles`
and the registry's own claim process — a number below is "what research turned
up," not "what is safe to build on."

The profiles this project actually has depth on — implemented, decoded, or with
their own reference-only section because a design decision depends on them —
are in the table for completeness of the picture, cross-referenced rather than
repeated. Two rows moved out of "not implemented" in the package that added the
Environment, FE-C and common-page-84 decoders; **the confidence column moved
with them**, from converging open-source evidence to the primary document, which
is the change that matters more than the depth column did.

| Device type | Name | Depth here | Confidence |
|---|---|---|---|
| `0x0B` | Bicycle Power | Implemented (encode) | — |
| `0x10` | Controls | Implemented (command surface only), §3.14 | — |
| `0x11` | Fitness Equipment (FE-C) | Decoded | community |
| `0x12` | Blood Pressure | Not implemented; forward-noted §5.1 as the next profile that would inherit CGM's mandatory-`X_AUTH` rule | community + open-source, type only — no page layout found anywhere |
| `0x13` | Geocache | Not implemented | community |
| `0x14` | Light Electric Vehicle | Not implemented, reference-only §3.6 | primary spec |
| `0x19` | Environment | Decoded (pages 0 and 1), §3.7 | primary spec |
| `0x1E` | Running Dynamics | Implemented, §3.13 | — |
| `0x29` | Tracker (Asset Tracker) | Implemented, §3.15 | — |
| `0x1F` | Muscle Oxygen | Not implemented |  |
| `0x22` | Shifting | Not implemented | open-source only |
| `0x23` | Bike Lights | Not implemented | primary spec (publicly mirrored PDF) |
| `0x28` | Radar | Not implemented | open-source only |
| `0x30` | Tire Pressure Monitor | Not implemented | open-source only |
| `0x73` | Dropper Seatpost | Not implemented | open-source, name matches the official list exactly |
| `0x77` | Weight Scale | Not implemented. `docs/profile-registry.md`'s claim-range table already excludes this as part of a dense band — this row is cross-reference, not new information | community + open-source |
| `0x78` | Heart Rate | Implemented, §3.2 | — |
| `0x79`/`0x7A`/`0x7B` | Bike Speed and Cadence family | Implemented (combined) / period-only (cadence, speed), §3.3 | — |
| `0x7C` | Stride-Based Speed and Distance | Implemented (encode)|
| `0x7F` | Core Temperature | Not implemented, reference-only §3.8 | vendor code |
| unverified | Multi-Sport Speed and Distance | Real profile, confirmed via the official list; **device type number could not be verified from any source worth trusting** — a single low-quality search result claimed `15` and is explicitly not transcribed here | unverified |
| unverified | Racquet | Real profile, device type not found | unverified |
| unverified | Extended Display (marketed as "Varia Vision") | Real profile, device type not found | unverified |
| unverified | Suspension | Real profile, device type not found | unverified |
| n/a | Sync | Governs ANT-FS file-transfer session behaviour rather than broadcasting sensor data; not a device-type profile in the sense every other row is | — |

**Reading the confidence column.** "Primary spec" and "vendor code" mean a
claim traceable to the manufacturer or to Garmin's own document — the same
standard section 3.6 and 3.8 hold themselves to. "Open-source only" means a
third-party decoder (`openant` and similar) asserts the number with no
corroborating second source — real enough to avoid colliding with by accident,
not real enough to build a receiver against without independent verification.
"Unverified" means exactly what it says: the profile is real, its device type
is not confirmed here, and this document is not the place that number will
come from.
