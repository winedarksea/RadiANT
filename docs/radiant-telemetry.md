# The RadiANT telemetry envelope

Checked by: `scripts/check_profile_registry.py` — it cross-checks the page map
below against `docs/profile-registry.md` and the common-page cadence against
`tools/ant_pages.py`. Nothing yet checks the byte layouts; the encoders in
`src/profiles/` and their mirror in `tools/ant_pages.py` are what will, through
`tools/test_ant_pages.py`, and until they exist treat every layout table here
as a specification rather than as a description.

RadiANT is an open-source, clean-room implementation compatible with the ANT+
specifications. This document specifies **one new device type** and the pages
inside it. It changes nothing about the ANT+ compatibility profiles.

---

## 1. What this is, and the MQTT parallel

This is not a sensor definition. It is an **envelope**: a way for an arbitrary
node to publish typed values over ANT at a period far below 4 Hz, on a coin
cell that should last a decade, to a receiver that has never heard of it
before.

The design is easiest to hold in your head as MQTT with the broker deleted:

| MQTT | RadiANT telemetry | Why the analogy holds |
|---|---|---|
| topic | **field ID** (a byte in the descriptor) | a stable name for one stream of values, chosen by the publisher |
| retained message | **the descriptor page, interleaved on a timer** | a late subscriber learns the schema without asking; MQTT stores it in the broker, we re-publish it every 121 messages |
| QoS 0, fire and forget | **ANT broadcast** | no delivery guarantee, no retransmission, no back-channel |
| broker | *(none)* | this is the whole point: there is no server to hold state, so the state is re-broadcast instead |

The one place the analogy breaks is worth stating because it is the design's
best property: an MQTT subscriber that misses a message has lost that value
forever, whereas a RadiANT accumulating field recovers a lost packet from the
*next* packet. See section 5.

### Why this exists at all

The motivating case is **a heart-rate strap driving a smart home** — elevated
BPM turns on the AC, resting HR dims the lights. In v1 that bridge runs
host-side (dongle -> daemon -> Matter) and needs no new radio work at all. A
combo node running `radiant_core` + Thread + Matter directly on one chip is what the
MPSL backend eventually enables, and is the concrete product that justifies
building it.

The envelope exists so that the *second* such node, and the fiftieth, cost a
descriptor edit rather than a new profile document, a new device type
allocation, and a new decoder on every receiver.

### The rate ceiling that forces sparse mode

The ANT channel period field is 16 bits in units of 1/32768 s, so the slowest
strictly-periodic ANT master is **2.0 s (0.5 Hz)**, and 4 Hz sensors sit at
8182 counts. There is no such thing as a 0.05 Hz ANT master. Anything slower
than 0.5 Hz is therefore **sparse mode by necessity, not by preference** — the
node keeps a channel period configured and simply declines to transmit in most
of its slots. That constraint, not a power estimate, is why the sparse flag is
in the descriptor from v1.

---

## 2. Structural constraint: this cannot break Garmin or Zwift

**Normative, and the reason the envelope is safe to ship.**

1. The envelope governs **only RadiANT device types and the pages inside
   them**. A receiver that does not recognise device type `0x60` never opens a
   channel for it and never sees one of these pages.
2. The ANT+ compatibility profiles — `0x0B` bicycle power, `0x78` heart rate,
   `0x79`/`0x7A`/`0x7B` bike speed and cadence, `0x11` fitness equipment — stay
   **byte-exact implementations of Garmin's specifications**. Nothing in this
   document applies to them.
3. The Matter/Zigbee bridge runs **on the host or the gateway**, never over the
   air. A Matter attribute never appears in an ANT frame; a RadiANT field ID
   does, and the host translates.

**Prohibited: encoding Matter or Zigbee semantics into an existing ANT+ page.**
Not "discouraged" — prohibited. Adding a Matter cluster id, an attribute id, or
a RadiANT field descriptor to page `0x10` of device type `0x0B`, or to any
other page of any ANT+-allocated device type, is the single change that would
break a Garmin head unit or Zwift, and it is the only one. If a mapping needs a
value that an ANT+ page does not carry, the node publishes it on a RadiANT
device type alongside, on its own channel.

---

## 3. Channel parameters

| Parameter | Value | Note |
|---|---|---|
| Network | ANT+ public, `A6 C5` | Extensions live inside the ANT+ network by decision; see `docs/decisions/0005-extension-inside-ant-plus.md` |
| RF channel | 57 (2457 MHz) by default | A node may announce another in the descriptor; see section 8 |
| Device type | `0x60` | Claimed in `docs/profile-registry.md` |
| Transmission type | `0x05` | Independent channel, global data pages. Fixed: a searching receiver matches on it |
| Device number | 1..65535 | Random at first provisioning and stable thereafter (identity Tier 0); re-rolled only on explicit user action or, opt-in, at power-up. It never changes while a channel is open. See `docs/radiant-security.md` section 4 |
| Channel period | node's choice, 1..65535 counts of 1/32768 s | Announced in the descriptor |
| Payload | 8 bytes | The length-byte extension (>8 bytes) is **not** used by this envelope in v1; it costs every merged RX window |

---

## 4. Page map

<!-- radiant-registry: pagemap -->

| Page | Name | Summary |
|---|---|---|
| `0x00` | Descriptor | The self-describing schema, sent as a set of consecutive frames |
| `0x01-0x0F` | Data | Packed field values against the announced schema |
| `0x10` | Reliable command | Command + sequence + inline tag, receiver -> node |
| `0x11` | Command acknowledge | Result + sequence + inline tag, node -> receiver |
| `0x12-0x1F` | Reserved | Unassigned; a receiver ignores these |
| `0x20-0x2F` | Reserved for the security envelope | Epoch and key-generation announcement, v2 TESLA key disclosure |
| `0x30-0x4F` | Reserved | Unassigned |
| `0x50` | Common page 80 | Manufacturer information, byte-exact ANT+ |
| `0x51` | Common page 81 | Product information, byte-exact ANT+ |
| `0x52` | Common page 82 | Battery status, byte-exact ANT+ |
| `0x53-0xEF` | Reserved | Other ANT+ common pages and future RadiANT pages |
| `0xF0-0xFF` | Vendor-private | Never registered, never bridged, never assumed to mean anything |

Every one of the 256 page numbers is accounted for on purpose, and
`scripts/check_profile_registry.py` enforces that for any RadiANT device type.
"Unassigned" should be a decision, not an oversight, because a page number that
nobody claimed is exactly the one that gets used twice.

### The two positional invariants

These are the format decisions that cannot be retrofitted, and they hold for
**every** RadiANT page:

- **Byte `[1]` is a counter.** An event counter on a data page, a sequence
  number on a command page. Never a field.
- **The trailing byte(s) are tag space.** No RadiANT page may place an
  authentication tag anywhere but at the end, and no page may place a field in
  a byte the descriptor has declared to be tag space.

Everything the security switches need follows from those two, which is why they
are stated here in Phase 4 rather than discovered in Phase 7.

**Two documented exceptions to the counter invariant, and there are no others.**
They are named here because an earlier draft asserted the invariant without
them, and an invariant with unadmitted exceptions is worse than one with stated
ones:

- **The descriptor's byte `[1]` is a frame index**, `(index << 4) | (count - 1)`.
  It is not a counter and never was.
- **The ANT+ common pages `0x50`/`0x51`/`0x52` are byte-exact ANT+ layouts** and
  carry no counter at all. Changing that would break every ANT+ receiver, which
  is the one thing this envelope must not do.

Neither exception is inside the secured page range, neither is ever encrypted,
and neither carries a spread tag — which is exactly why
`docs/radiant-security.md` bounds the secured page range to `0x01..0x1F` rather
than trusting a host to remember. The counter invariant holds where the security
switches rely on it, and the two places it does not hold are the two places
nothing relies on it.

---

## 5. Field kinds, and why accumulating is the default

ANT+ profiles survive packet loss because their values are **cumulative sums**,
not instantaneous readings. Page `0x10` of bicycle power carries
`acc_power` alongside `inst_power`: lose a packet and the next one still tells
you the total energy that went through the cranks, because the accumulator
absorbed the missing sample. This is the single most valuable thing to copy
from ANT+'s design, and it is the reason `tools/ant_verify.py` checks
accumulator continuity at all.

Two field kinds:

- **Accumulating** (`accumulate` bit set). The transmitted value is a running
  total in the field's declared width, and it is *meant* to wrap. A receiver
  differences two readings **in the field's own width**, exactly as
  `ant_pages.delta_u8()` / `delta_u16()` do. A receiver that widens to `int`
  before subtracting is correct for hours and then reports one absurd sample
  per wrap, which is easy to dismiss as radio noise.
- **Instantaneous**. The value at the moment of transmission. A lost packet is
  lost.

**Rule: anything integrable is published as an accumulating field.** Concretely,
if the field's quantity has an integral in the accumulating block of the type
vocabulary (section 7) — power -> energy, speed -> distance, flow -> volume,
current -> charge, revolutions per minute -> revolutions — the node publishes
the accumulating partner, and may publish the instantaneous form alongside as a
convenience. The accumulator is authoritative. That is precisely the shape of
ANT+ page `0x10`.

**Choosing a width.** Pick the width so that a full wrap takes at least ten
times the worst outage the deployment tolerates, and never less than 60 s.
Two readings separated by more than one wrap are indistinguishable from two
readings separated by less, and no receiver can detect the difference — so the
width is the only thing standing between a plausible number and a wrong one.

---

## 6. The descriptor, page `0x00`

The descriptor is a **set of consecutive frames**, all bearing page number
`0x00`, all 8 bytes. It is the retained message: a receiver joining mid-stream
learns the schema from it with no request, no back-channel, and no registry
lookup.

```
[0] 0x00                       page number
[1] (index << 4) | (count - 1) frame index, and how many frames in the set
[2..7]                         6 bytes of frame body
```

Up to 16 frames: two fixed header frames plus up to 14 field frames — or up to
13 field frames plus the **descriptor authentication frame**, which is mandatory
whenever any transform bit is set and always occupies the last slot in the set.

### Frame 0 — identity and timing

```
[2] envelope version (bits 7..4) | field count (bits 3..0)
[3] schema id (u8)
[4..5] channel period, counts of 1/32768 s (LE u16); 0x0000 = asynchronous
[6] RF channel index, 0..124 meaning 2400 + N MHz; 57 is the ANT+ default
[7] flags (below)
```

- **envelope version** is 1. A receiver that does not implement the version
  **must reject the node** rather than guess, because the version governs the
  frame layout itself.
- **schema id** is a node-chosen byte that changes whenever any field
  descriptor changes. It is what lets a receiver cache a schema and notice a
  change after reading a single frame instead of the whole set. It is not a
  checksum and carries no integrity claim.
- **field count** 0..14. 15 is reserved for an extended descriptor.

### Frame 0 flags, byte `[7]`

```
bit 7  TRANSFORM  X_CONF        data pages are AES-128-CTR ciphertext
bit 6  TRANSFORM  X_AUTH        the trailing byte of each data page is a spread-MAC tag
bit 5  TRANSFORM  reserved, must be 0 in v1 (descriptor set encryption; refused,
                  because the descriptor has no counter and therefore no nonce)
bit 4  TRANSFORM  reserved, must be 0 in v1 (v2: TESLA delayed key disclosure)
bit 3  INFO       reserved, must be 0
bit 2  INFO       sparse mode
bit 1  INFO       the node is not on RF 57; byte [6] is authoritative
bit 0  INFO       long-range PHY in use
```

**Bit 3 was `X_PRIV`, "the device number rotates per epoch", and it is now
reserved-must-be-zero.** Continuous rotation is withdrawn — see
`docs/radiant-security.md` section 3.7 for the reasons, which are worth reading
before proposing it again. The bit stays *reserved* rather than being reused,
for two reasons: announcing a privacy posture in the clear is itself a leak, and
keeping the bit free makes revisiting rotation a format-compatible change rather
than a break. The same applies to frame 1's epoch field.

**The forward-compatibility rule, and it is the most important line in this
document:**

- Bits 7..4 are **transform** flags: they change how the bytes must be
  interpreted. A receiver that sees a transform bit it does not implement
  **must not decode the node's data pages**. Fail closed.
- Bits 3..0 are **informational**: they describe the node, not the byte layout.
  An unknown informational bit is ignored. Fail open.

Bit 4 exists so that the v2 tier can be added without a v1 receiver silently
decoding transformed bytes as plaintext. That is exactly the retrofit this
document was written in Phase 4 to avoid.

Further informational flags, when they are needed, come from the reserved bits
of frame 1 byte `[3]` — not from this byte, which is full.

### Frame 1 — mode and reserved security space

```
[2] sparse heartbeat interval in seconds (u8); 0 when the node is not sparse
[3] transmit control:
      bits 7..6  MAC window W: 0 = reserved (W=1, not in v1), 1 = W=2,
                 2 = W=4, 3 = W=8
      bits 5..4  sparse repeat count: 0 = 1x, 1 = 2x, 2 = 3x, 3 = 5x
      bits 3..0  reserved, must be 0 -- the informational-flag extension space
[4..7] epoch (u32 LE); zero only on a node that enables no transform AND sends
       no reliable-command page. A command-only node carries a non-zero epoch
```

**The epoch's "zero when unused" rule and section 9's "a command-only node still
needs the epoch" were, in an earlier draft, a direct contradiction.** Resolved
in favour of section 9: the command page's inline tag covers the epoch, and that
coverage is the only thing that stops a command captured yesterday from being
adopted as a fresh sequence after a reboot. So the epoch is zero only when
neither a transform nor the command page is in use — which is the ordinary
telemetry node, and is the case the zero was written for.

**This frame is the reserved space, and it is reserved from v1 even though
nothing populates it until the security phase.** The epoch is four bytes wide
from the first shipped node because a receiver with no real-time clock has to
get the epoch from somewhere, and the only place it can come from is a page that
a node already sends. Adding it later means a format break for every deployed
node — which is the entire reason this document was written ahead of the code.

**`W in {2, 4, 8}` in v1**; encoding 0 (W=1, an inline 16-bit tag) is reserved
for the reliable-command page of section 9 and is refused on a data page. The
window set is not arbitrary: the spread MAC derives its window boundary and its
tag-byte index from the counter, `window = [c - (c mod W), +W)` and
`tag byte = tag[c mod W]`, and that is self-synchronising across packet loss
**only because W divides both 256 and 65536** — so a byte-counter wrap and a
16-bit counter wrap both land on a window boundary. A future W of 3, 5 or 6
would break resynchronisation silently, one lost packet at a time.

**The epoch is no longer `floor(t / 128 s)`.** Nothing rotates continuously, so
the epoch is a key-rotation clock that advances on reboot, on host command, and
on a counter wrap. `docs/radiant-security.md` section 3.5 is normative for it;
the parts that constrain this format are that **a counter wrap advances the
epoch by 1** on both sides, and that no transform enables until the epoch has
been advanced after a reset.

### Frames 2..N — one per field

```
[2] field id (u8)
[3] field type (u8) -- the vocabulary in section 7
[4] encoding:
      bit 7     accumulate (1) / instantaneous (0)
      bit 6     signed
      bits 5..2 width code
      bits 1..0 reserved, must be 0
[5] scale exponent, int8: value = raw * 10^exp, in the type's canonical unit
[6] data page number, 0x01..0x0F
[7] bit offset of the field within that page's field area, 0..47
```

Width codes:

| Code | Bits | Code | Bits |
|---|---|---|---|
| 0 | 1 | 8 | 20 |
| 1 | 2 | 9 | 24 |
| 2 | 4 | 10 | 32 |
| 3 | 6 | 11 | 40 |
| 4 | 8 | 12 | 48 |
| 5 | 10 | 13..15 | reserved |
| 6 | 12 | | |
| 7 | 16 | | |

### The last frame — the descriptor authentication frame

Mandatory whenever any transform bit in frame 0 byte `[7]` is set; absent
otherwise, and its slot is a field frame instead.

```
[0]    0x00                        page number, like every descriptor frame
[1]    (index << 4) | (count - 1)  frame index, like every descriptor frame
[2..7] 48-bit CMAC(K_auth, epoch[4 LE] || devnum[2 LE] || schema_id || bodies)
       "bodies" is bytes [2..7] of every preceding frame of this set, in frame
       order. The MAC uses domain byte 0x03; see docs/radiant-security.md 3.3.
```

**Without it, the MAC on the data pages protects the wrong thing.** The
descriptor carries the epoch, the window W, the transform flags, and every
field's offset and scale — so an attacker who forges one descriptor frame gets
**wrong readings out of correctly authenticated packets**. The tag verifies and
the numbers are still a lie.

It costs **one slot in 121** and **zero data-page bytes**, which is why it is
mandatory rather than a further opt-in. It is specified here and implemented
when `src/profiles/` exists; there is no descriptor encoder to authenticate
until then, and `docs/radiant-security.md` section 7.6 states the limit that
leaves open in the meantime.

### Data pages `0x01..0x0F`

```
[0]    page number, 0x01..0x0F
[1]    event counter (u8), +1 per transmitted data page, wraps at 256
[2..7] field area, 48 bits, bit offset 0 = MSB of byte [2]
```

- The **event counter counts data pages only**, not descriptor or common
  pages. It is what a receiver counts holes with, and it is the low half of the
  `X_CONF` nonce counter. It is mandatory in v1 whether or not any security
  switch is ever enabled, because adding it later would renumber every field
  offset in every deployed schema.
- **It counts transmissions, not application writes.** A master retransmits its
  current body every slot whether or not the application supplied a new one, so
  a counter that advanced per write would repeat across retransmissions — and a
  repeated counter under one `(epoch, device number)` is keystream reuse. On a
  secured channel the transform layer owns this byte outright.
- **It is the packet index within the current epoch and resets to zero when the
  epoch advances.** A loss detector built on it has to handle that reset; the
  receiver is told the epoch, so this is cheap. The 16-bit nonce counter's high
  byte is reconstructed from time rather than from arrival history, and a wrap
  of it advances the epoch by 1 on both sides.
- **With `X_AUTH` on**, byte `[7]` is the spread-MAC tag byte and the field
  area is bits 0..39. The descriptor's explicit bit offsets are what make this
  a schema change rather than a format break: the node re-publishes a
  descriptor with a new schema id and every receiver picks it up within one
  interleave cycle.
- **With `X_CONF` on**, bytes `[2..7]` (or `[2..6]` when `X_AUTH` is also on)
  are ciphertext. Bytes `[0]` and `[1]` stay in the clear, because a receiver
  needs the page number to tell a data page from a descriptor and needs the
  counter to build the nonce. This is what makes `X_CONF` cost zero payload
  bytes.
- **With both on the order is encrypt-then-MAC**, and the tag covers bytes
  `[0..6]` of every packet in the window — including the page number, so that
  flipping it cannot reinterpret authenticated bits against a different schema.

**Bit packing is MSB-first**, and the value itself is stored MSB-first within
its width. This is the opposite of the little-endian byte order every ANT+ page
in `tools/ant_pages.py` uses, and the difference is deliberate: those fields
are byte-aligned, where little-endian is unambiguous; these are not, where
little-endian bit order is a reliable source of two implementations that each
work alone. `src/profiles/` and `tools/ant_pages.py` must share one pack/unpack
helper each, tested against the same vectors, for exactly that reason.

### Interleave cadence: every 121 messages

The descriptor set and the ANT+ common pages ride the cadence a real certified
sensor uses, not the one the generic guidance suggests. `tools/ant_pages.py`
documents the trap directly, at `COMMON_PAGE_INTERVAL`:

> 120 rather than the 64 this used to use, because 64 is not what a real sensor
> does: sdk-ant's certified bicycle power profile interleaves page 80 at 119
> and page 81 at 120, commented "Minimum: Interleave every 121 messages".

So the numbers are **119 / 120, cycle length 121** — not the 65 the generic
ANT+ guidance claims. A simulator or a node that sends common pages twice as
often as the profile requires is not simulating anything, and it spends radio
energy to do it.

The node keeps a message counter `m` over a 121-message cycle:

| `m` | Page |
|---|---|
| 0 .. D | Descriptor frames 0..D, consecutive |
| 119 | Common page 80 (manufacturer) |
| 120 | Common page 81 (product) |
| everything else | Data pages, in the node's own rotation |

The descriptor set is sent **consecutively**, not one frame per cycle. A node
with 8 fields has D = 9; spreading those frames one per cycle would make a
receiver wait 10 x 121 messages — over eight minutes at 2 Hz, over an hour at
0.25 Hz — before it could decode anything. Consecutive costs 10 slots out of
121 and makes a mid-stream join take one cycle.

Common page 82 (battery) is optional and the node places it in any data slot at
whatever cadence it likes. It is not announced in the descriptor; it is a page
every ANT+ receiver already understands.

**In sparse mode the cadence rule changes**: see section 8.

### The common pages leak more than the device number ever did

Normative, and **independent of every security switch and every identity
tier** — it applies to a node with all transforms off, and it is the single
highest-value privacy rule in this envelope because it is also the cheapest:

> A node with a privacy posture emits page 81 with **`serial = 0xFFFFFFFF`**,
> the sentinel `tools/ant_pages.py` already documents as "not supplied";
> **page 82 is suppressed**, or emitted with `operating_time` zeroed; and page
> 80 reports a **generic** manufacturer, model and hardware revision.

The reasoning is arithmetic rather than judgement. Page 81 broadcasts a **32-bit
globally unique serial number in the clear** every 30 seconds or so — strictly
more identifying than the 16-bit device number, unaffected by any device-number
re-roll, and therefore capable of defeating the identity tiers of
`docs/radiant-security.md` section 4 outright. Page 82's operating-time counter
is monotone: it survives an identity change *and* fingerprints a battery swap.

A node that re-rolls its device number and keeps broadcasting its serial number
has not changed identity. It has added a field.

---

## 7. The field-type vocabulary

This is the part that makes the Matter and Zigbee mapping **mechanical rather
than bespoke**, and it is the reason the vocabulary is decided at K4 instead of
per-profile. A descriptor already carries a typed value with a unit and a scale
factor, which is exactly a Matter attribute model. What turns that from a
resemblance into a bridge is fixing the *quantity* namespace so the host has a
table lookup rather than a per-node adapter.

The mapping rule, in full:

```
value_SI      = raw * 10^exp                 in the type's canonical unit
matter_value  = f_type(value_SI)             one fixed function per type
```

`f_type` is a per-quantity unit conversion and nothing else — no per-node code,
no per-vendor code. Adding a field to a node adds zero lines to the bridge.

Canonical units are SI wherever an SI unit exists. There are exactly two
exceptions, both noted in the table, both because every other system in the
chain (ANT+, BLE, Matter) already uses the non-SI unit and converting twice
would only create rounding.

**SI is the contract; Matter is an alignment.** A field type owes the vocabulary
a canonical unit and a scale — that is what makes it a field type. Where a
quantity also has an obvious Matter cluster and attribute the table names it, and
`f_type` is the conversion between them; where it does not the column reads `—`,
the type is a first-class member of the vocabulary regardless, and a bridge drops
the field rather than inventing a mapping. Matter has no heart-rate quantity, and
the motivating case in section 1 is a heart-rate strap: a vocabulary admitting
only bridgeable quantities would have excluded its own headline example. The
bridge itself runs host-side either way and is still future work — section 2,
rule 3.

### Class 0x00-0x0F — boolean and state

| Type | Quantity | Canonical unit | Matter cluster / attribute | ZCL |
|---|---|---|---|---|
| `0x01` | boolean state | 0/1 | Boolean State / StateValue | Binary Input |
| `0x02` | occupancy | 0/1 | Occupancy Sensing / Occupancy | Occupancy Sensing |
| `0x03` | contact (open/closed) | 0/1 | Boolean State / StateValue | IAS Zone |
| `0x04` | on/off actuator state | 0/1 | On/Off / OnOff | On/Off |
| `0x05` | lock state | enum 0..3 | Door Lock / LockState | Door Lock |

### Class 0x10-0x2F — instantaneous scalars

| Type | Quantity | Canonical unit | Matter cluster / attribute | Integral partner |
|---|---|---|---|---|
| `0x10` | temperature | K | Temperature Measurement / MeasuredValue (0.01 degC) | — |
| `0x11` | relative humidity | % | Relative Humidity Measurement / MeasuredValue | — |
| `0x12` | pressure | Pa | Pressure Measurement / MeasuredValue | — |
| `0x13` | illuminance | lx | Illuminance Measurement / MeasuredValue | — |
| `0x14` | length | m | — | — |
| `0x15` | mass | kg | — | `0x33` |
| `0x16` | speed | m/s | — | `0x34` |
| `0x17` | acceleration | m/s^2 | — | — |
| `0x18` | angle | rad | — | — |
| `0x19` | angular rate | rad/s | — | `0x35` |
| `0x1A` | force | N | — | — |
| `0x1B` | torque | N.m | — | `0x30` |
| `0x1C` | active power | W | Electrical Power Measurement / ActivePower (mW) | `0x30` |
| `0x1D` | voltage | V | Electrical Power Measurement / Voltage (mV) | — |
| `0x1E` | current | A | Electrical Power Measurement / ActiveCurrent (mA) | `0x31` |
| `0x1F` | frequency | Hz | — | `0x36` |
| `0x20` | volumetric flow | m^3/s | — | `0x32` |
| `0x21` | gas concentration | ppm | Concentration Measurement (CO2 etc.) / MeasuredValue | — |
| `0x22` | mass concentration | ug/m^3 | Concentration Measurement (PM2.5 etc.) | — |
| `0x23` | sound pressure level | dB SPL | — | — |
| `0x24` | percentage | % | Level Control / CurrentLevel (0..254) | — |
| `0x25` | battery state of charge | % | Power Source / BatPercentRemaining (0.5%) | — |
| `0x26` | heart rate | **bpm** (non-SI, exception) | *(no cluster; see below)* | — |

**Diagnostic use of `0x10`, beyond a sensor node.** `radiant_radio_nrf.c` samples
RSSI raw - nRF52840 errata 153 says the reading has a temperature-dependent
error, which `radiant_radio_nrf.c` now corrects on nRF52840/nRF52833 using
Nordic's own open-source 802.15.4 driver rather than an invented curve. This
bench's own nRF54L15 has no equivalent correction anywhere in Nordic's tree,
so the confound is still live there. `radiant_radio_nrf_die_temp_c()`
(`radiant_core/include/radiant_core/radiant_radio_nrf_diag.h`) exists so a
bench capture can publish die temperature as an ordinary `0x10` field
alongside the RSSI it is already recording, deconfounding the Phase 4
sensitivity baseline and the Tier 2 A/B gate ([`testing.md`](testing.md)) on
whichever part is under test. This is a diagnostic use of the vocabulary, not
a new device type: the envelope already says what `0x10` means, and a bench
rig is exactly the "arbitrary node" section 1 describes.

### Class 0x30-0x3F — accumulating quantities

Everything in this class **must** carry the `accumulate` bit. That is what the
class is for, and a descriptor that clears the bit on one of these types is
malformed.

| Type | Quantity | Canonical unit | Matter cluster / attribute |
|---|---|---|---|
| `0x30` | energy | J | Electrical Energy Measurement / CumulativeEnergyImported (mWh) |
| `0x31` | charge | C | — |
| `0x32` | volume | m^3 | — |
| `0x33` | mass, cumulative | kg | — |
| `0x34` | distance | m | — |
| `0x35` | revolutions | count | — |
| `0x36` | event count | count | — |
| `0x37` | duration | s | — |

### Class 0x40-0x4F — enumerations

| Type | Quantity | Matter cluster / attribute |
|---|---|---|
| `0x40` | generic enum, code space declared out of band | — |
| `0x41` | HVAC system mode | Thermostat / SystemMode |
| `0x42` | fan mode | Fan Control / FanMode |

### Class 0x50-0x5F — opaque, and 0xF0-0xFF — vendor-private

`0x50` is raw bytes: no unit, no scale, no Matter mapping, and the bridge is
required to drop it rather than invent one. `0xF0..0xFF` are vendor-private
types that are never registered and never bridged.

### Worked examples

| Field | Type | Width / exp | Raw | SI | Matter |
|---|---|---|---|---|---|
| Room temperature | `0x10` | 16 bits, exp -2 | 29315 | 293.15 K | `round((293.15 - 273.15) * 100)` = 2000 |
| Trainer power | `0x1C` | 12 bits, exp 0 | 250 | 250 W | `250 * 1000` = 250000 mW |
| Cumulative energy | `0x30` | 32 bits, exp 0, accumulate | 3600000 | 3.6 MJ | `3600000 / 3600` = 1000 mWh |
| Door contact | `0x03` | 1 bit, exp 0 | 1 | open | StateValue = true |

### Honest gap: not every type maps

Heart rate is the motivating case for this whole envelope and Matter has **no
heart-rate cluster**. The vocabulary says so in the table rather than inventing
a mapping. The bridge therefore exposes `0x26` as a *rule input* — the thing an
automation triggers on — while the Matter side of the smart-home case is the
actuator: On/Off for the AC, Level Control for the lights. That asymmetry is
the real shape of the feature, and pretending otherwise would mean shipping a
vendor cluster that no Matter controller knows what to do with.

---

## 8. Sparse (event-driven) mode

A descriptor flag (frame 0 bit 2) declaring that the node transmits **on
meaningful change plus a slow heartbeat**, rather than in every slot.

For a door sensor or a thermostat this cuts radio energy another 10-100x below
what any legal channel period reaches. That is the difference between a battery
lasting years and one lasting a decade, and it is unreachable by tuning the
period because of the 0.5 Hz floor in section 1.

### The rule, stated explicitly

> **A sparse node requires a scanning receiver.**

An ANT master must normally be periodic because a *tracking* receiver opens a
narrow RX window predicted from the master's slot phase, and drops the channel
when the search timeout expires with nothing in it. A node that skips most of
its slots will be dropped, re-acquired, and dropped again, and the receiver
will report it as terrible link quality rather than as a configuration
mismatch.

A **scan-mode** receiver does not care: it listens continuously and takes
whatever arrives. The 32-channel RadiANT dongle has background scan on, which
is what makes sparse mode practical rather than theoretical.

Therefore:

- A receiver that reads the sparse flag and **cannot** scan must refuse the
  node and say so, rather than track it and report intermittent loss.
- A receiver that can scan must not apply a search timeout to a sparse node.
- The flag is in the descriptor precisely so this is a decision the receiver
  makes from data rather than from configuration.

### Two sub-modes

| Channel period field | Behaviour | Cost |
|---|---|---|
| non-zero | **Slot-aligned sparse.** The node keeps its period configured, so the transmissions it does make land on a predictable phase. | Keeps a timer running; a scan receiver's dwell can be aligned |
| `0x0000` | **Asynchronous.** No slot discipline at all; the node wakes, transmits, sleeps. | Lowest energy possible; requires a *continuous* scan receiver |

### Heartbeat and repetition

- The heartbeat interval is frame 1 byte `[2]`, in seconds. Recommended 30-60.
  **Zero is invalid** for a sparse node: without a heartbeat a receiver cannot
  distinguish a quiet node from a dead one, and "no data" is the one reading a
  telemetry system must never produce silently.
- Because there is no retransmission and a scanning receiver may be mid-dwell
  elsewhere, a sparse node sends each event **k times** (frame 1 byte `[3]`
  bits 5..4; default 3x) spaced by roughly one channel period. The event
  counter in byte `[1]` is what lets the receiver deduplicate the repeats, and
  it is the same counter that detects loss in periodic mode — one mechanism,
  two jobs.

### Descriptor cadence in sparse mode

The 121-message interleave is useless to a node that sends 40 messages a day.
So in sparse mode:

- The node emits the **whole descriptor set with every heartbeat**, and
  immediately after any schema change.
- The node emits the descriptor set on receipt of command `0x06` (section 9),
  which is how a receiver that just joined gets a schema without waiting for
  the next heartbeat.

---

## 9. The reliable-command page

ANT's acknowledged data tells the sender that *a* packet arrived. It carries no
sequence number and is single-shot, so on failure the sender cannot distinguish
"not delivered" from "delivered, and the acknowledgement was lost". Retry the
second case and a "toggle the AC" command runs twice. This page is the missing
half of the smart-home case.

Commands travel **receiver -> node as ANT acknowledged data** on the node's own
channel; the acknowledgement travels back as a broadcast page `0x11`, repeated
k times.

### Page `0x10` — reliable command

```
[0]    0x10
[1]    sequence number (u8), wraps at 256          <- the counter invariant
[2]    command id
[3]    target field id; 0xFF = the node as a whole
[4..5] argument (u16 LE), scaled by the target field's exponent
[6..7] inline tag (u16) = trunc16(MAC(K_cmd, epoch || seq || cmd || target || arg))
```

### Page `0x11` — command acknowledge

```
[0]    0x11
[1]    sequence number being acknowledged
[2]    result code
[3]    command id, echoed
[4..5] resulting value of the target field (u16, target field's scaling)
[6..7] inline tag (u16) over (epoch || seq || result || cmd || value)
```

Result codes:

| Code | Meaning |
|---|---|
| `0x00` | accepted and executed |
| `0x01` | accepted; already executed (idempotent repeat) |
| `0x02` | rejected: sequence outside the accept window |
| `0x03` | rejected: bad tag |
| `0x04` | rejected: unknown command |
| `0x05` | rejected: unsupported argument |
| `0x06` | rejected: busy |

### Command vocabulary

| Id | Command | Matter counterpart |
|---|---|---|
| `0x00` | no-op / ping | — |
| `0x01` | set boolean | On/Off Off, On |
| `0x02` | set level | Level Control MoveToLevel |
| `0x03` | set setpoint | Thermostat setpoint |
| `0x04` | set mode / enum | Thermostat SystemMode, Fan Control FanMode |
| `0x05` | identify | Identify |
| `0x06` | send the descriptor set now | — |
| `0x07` | report schema id | — |
| `0x08-0x7F` | reserved | |
| `0x80-0xFF` | vendor-private | |

### Idempotency, stated as a rule

The node keeps `last_seq` and the `result` it produced.

1. Verify the tag first. A bad tag is rejected with `0x03` and **changes no
   state**, including `last_seq`.
2. If `seq == last_seq`, **re-send the stored acknowledgement and do not
   execute again.** Same sequence, same effect. This is what makes a retried
   "AC on" safe.
3. Otherwise accept if `delta_u8(seq, last_seq)` is in 1..64 — a window wide
   enough to survive a run of loss, narrow enough that a stale command cannot
   walk back into range. Anything else is rejected with `0x02`.
4. On power-up `last_seq` is unknown, and the node adopts the sequence of the
   first command that passes the tag check **in the current epoch**.

Step 4 is where the epoch earns its four bytes even on a node with no
confidentiality and no privacy: the tag covers the epoch, so a command captured
yesterday and replayed after a reboot fails verification instead of being
adopted as a fresh sequence. A command-only node still needs the epoch.

### The honest limit on a 16-bit inline tag

Two bytes is what fits. A forgery attempt succeeds with probability 2^-16, and
the attacker must guess a tag for a sequence number inside the accept window,
which the idempotency rule caps. The node must therefore **rate-limit *failed*
tag verifications, with exponential backoff**, and log rejections — at which
point a forging attacker is a detectable flood rather than a quiet success.

Rate-limiting *accepted* commands, which an earlier draft asked for, does
nothing: an attacker brute-forcing a 16-bit tag produces 65535 rejections for
every acceptance, so 99.998% of the traffic being throttled is the traffic the
accepted-command limiter never sees. Throttle the failures.

A node that needs a full-strength inline tag
would need payloads longer than 8 bytes, and **there is no longer a mechanism
for those**: this used to name "the length-byte extension (extension axis 3)",
and byte 3 is not a length — see the amendment in
`docs/decisions/0005-extension-inside-ant-plus.md` and section 11 below. Two
bytes is therefore not a trade-off a node can decline; **it is the limit**,
until some measured way of putting a longer frame on the air exists. Rate
limiting and logged rejections are the whole mitigation, and a deployment that
cannot accept 2^-16 per attempt has no answer inside this envelope today.

The command page is also why a spread MAC is not enough on its own: a spread
MAC lets an attacker inject up to one window of forged data before detection,
which is acceptable for telemetry and wrong for anything that actuates. See
`docs/radiant-security.md`.

---

## 10. Where the code lives

Encode and decode go in `src/profiles/` and are mirrored into
`tools/ant_pages.py`. That is the same split the ANT+ pages already use, and
it is what makes the envelope **host-testable in CI with no board**: the
Python encoders and decoders check each other in `tools/test_ant_pages.py`, and
the same vectors feed the C decoders. Given that `native_sim` does not build on
Windows and every C unit test runs only in CI on Linux, the Python mirror is
the only layer a developer can iterate against locally.

`tools/ant_pages.py` is also the shared contract with the sibling
`zephyr_aerosense` project, so the envelope's encoders arriving there is how
that project gets a second consumer for free.

---

## 11. Explicitly not in v1

- **A dedicated CdA profile for the aero sensor.** The generic envelope is how
  it ships first. A device type is worth claiming only once its fields have
  settled, and claiming one early is how a registry fills with entries nobody
  implements.
- **Payloads longer than 8 bytes.** This bullet used to say the on-air length
  byte already expresses them. **There is no length byte** — Spike B part 2
  measured byte 3 as a control byte whose low bits are not a length either
  (`0x0A` reads 10 there and `0xA2` reads 2, both carrying eight payload bytes),
  across 3,104 CRC-valid frames. See `docs/spike-b-part2-results.md` and
  `docs/ant-radio-link.md`.

  So the honest position is: **how a longer payload would be expressed on air is
  unknown, and no measured mechanism achieves it.** No frame with a payload other
  than eight bytes has ever been on this bench's air — advanced burst was
  enabled, the dongle accepted 24-byte blocks, and every one fragmented into
  three 8-byte packets. Bits 2:0 = `010` therefore has a *disproved* meaning
  rather than a measured one. Settling it needs a frame with a non-eight-byte
  payload actually on the air — two ends that both negotiate advanced burst, or a
  RadiANT transmitter emitting one deliberately and a receiver that can decode
  it — and until one exists, no replacement mechanism should be invented here.

  None of that moves the envelope, which stays at 8 in v1 for a reason that never
  depended on the length reading: a longer frame lengthens the RX window for
  every channel merged into it, not just the node that asked for it. What changes
  is that "longer payloads later" is now an **open question**, not a reserved
  capability the registry can hand out.
- **Any security switch turned on.** Every reservation in this document is
  populated with zeros in v1. Phase 7 turns them on; Phase 4 makes sure there
  is somewhere for them to go.
