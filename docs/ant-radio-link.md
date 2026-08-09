# The ANT on-air link — clean-room reference

Checked by: **Spike A** (`ant_core/spike/rx_raw` — hears real ANT+ broadcasts and prints raw bytes,
CRC status, match index and RSSI) and **Spike B** (promiscuous capture that separates broadcast from
acknowledged from burst). Until those two spikes run, every register-level statement below is a
*prediction*, marked as one, and Spike A's job is to falsify it cheaply. Nothing else in the repo
fails if this file drifts.

This document is what every `ant_core` agent reads **instead of** anything of Garmin's. It is the
whole permitted description of the physical link. If a fact is not here and not in the free
*ANT Message Protocol and Usage Rev 5.1*, it is not available to the link layer.

## Provenance tags

Every fact carries one. A fact with no tag does not belong in this file.

| Tag | Means |
|---|---|
| `[rev5.1]` | *ANT Message Protocol and Usage* Rev 5.1, D00000652 — free, no login. Section numbers are pinned against the archived copy as an `archive`-agent follow-up; an unpinned tag means the fact is in the free spec but the section has not yet been recorded. |
| `[rtl_433]` | A **fact** learned by reading rtl_433's decoder. See *The rtl_433 boundary* at the end — facts yes, expression never. |
| `[patent US8855246B2]` | Public patent text. Patents are published teaching documents; reading one is not reverse-engineering. |
| `[nRF datasheet]` | Public Nordic product specifications — nRF52840, nRF54L15, nRF24L01+. |
| `[measured]` | Measured on this bench, naming the run. |
| `[inferred]` | Reasoning, not a source. Every one of these states how it can be falsified. |

Absent by construction: anything derived from `libant.a`, from sdk-ant's headers, or from an
adopter-gated ANT+ device profile document. See `docs/decisions/0002-clean-room-policy.md`.

## The on-air frame

```
        1        2         2        1       1       1        8         2      bytes
   +--------+---------+---------+-------+-------+--------+---------+--------+
   |preamble| net addr| dev num | dtype | ttype | length | payload |  CRC   |
   |  0x55  |  A6 C5  | lo  hi  |       |       |  0x0A  | d0..d7  |        |
   +--------+---------+---------+-------+-------+--------+---------+--------+
            |<----------------- 15 bytes CRC coverage ------------>|
            |<------------------------ 17 bytes ------------------------->|
   |<-------------------------- 18 bytes total ------------------------->|
```

- Modulation is 1 Mbps GFSK on a 1 MHz raster `[rtl_433]`. ANT+ data channels sit at RF index 57 =
  2457 MHz `[rev5.1]`; the index convention is 2400 + N MHz, N in 0..124 `[rev5.1]`.
- Frequency deviation is ~±170 kHz `[inferred]` — the nRF24L01+ 1 Mbps mode is ±160 kHz
  `[nRF datasheet]` and the original ANT part was built on that core (below), so the true figure is
  in that neighbourhood rather than BLE's ±250 kHz. Falsifiable by Spike C's Radio Configurator work,
  and it matters: an RX filter bandwidth chosen for BLE 1M is the wrong bandwidth for this PHY, which
  is why the EFR32 sensitivity number cannot simply be quoted from a datasheet.
- **Preamble is one byte**, `0x55` or `0xAA`, chosen so the alternating pattern runs continuously
  into the first bit of the address `[rtl_433]` `[patent US8855246B2]`. The nRF RADIO derives its
  preamble by the same rule, from the first address bit, with no software involvement
  `[nRF datasheet]` — which is the first sign that this frame and that peripheral share ancestry.
- **Network address is 2 bytes. ANT+ is `A6 C5`** `[rtl_433]` `[patent US8855246B2]`.
- **Device number is 2 bytes, low byte first on air** `[rtl_433]`. Device type is 1 byte, whose MSB
  is the pairing bit, leaving types 1..127 `[rev5.1]`. Transmission type is 1 byte `[rev5.1]`.
  Together those four bytes are exactly ANT's channel ID `[rev5.1]`, which is why an on-air address
  match *is* a channel-ID match and no software comparison is needed in the common case.
- **Length byte is `0x0A`** for a standard 8-byte data message `[rtl_433]`. Ten, not eight — see
  *Fact one* below, where the discrepancy turns out to be the most useful thing in the frame.
- **CRC is CRC-16/CCITT-FALSE**: width 16, polynomial `0x1021`, init `0xFFFF`, no reflection, no
  final XOR `[rtl_433]`.
- **No whitening** `[rtl_433]`. This matters more than it sounds: whitening is the one frame feature
  that cannot be reproduced by a general-purpose radio without matching the vendor's LFSR seed rule,
  and ANT does not use it.

### Where the CRC starts and stops — the wording trap

The frame after the preamble is **17 bytes**, of which the CRC **covers the first 15** and occupies
the last 2 `[rtl_433]`. Prose that says "CRC over the 17 bytes after the preamble" is not wrong so
much as compressed, and both readings are usable:

- compute over the 15 bytes and compare against the received 2, or
- compute over all 17 and check the result is **zero**.

The second works because CCITT-FALSE has no final XOR and no reflection, so appending a correct CRC
to a message drives the register to zero `[inferred]` — arithmetic, and verified here rather than
asserted. Assert both forms in `ant_frame.c`'s tests; they catch different mistakes.

A synthetic golden vector for that test, computed not captured `[inferred]`:

```
message (15 bytes)  A6 C5 34 12 78 01 0A 00 01 02 03 04 05 06 07
CRC-16/CCITT-FALSE  0x1B12
same 15 bytes + 1B 12, recomputed  ->  0x0000
```

The CRC also chains: running the last 13 bytes with the register pre-loaded to the state after
`A6 C5` — which is **`0x233E`** — gives the same `0x1B12` `[inferred]`. That is not a curiosity. It is
what lets a radio whose CRC engine cannot cover its own sync word still produce ANT's CRC in
hardware, by baking `0x233E` in as the CRC initial value; see the EFR32 notes in `docs/backends.md`.

### One 16-bit constant depends on the network key

The CRC seed is a constant `0xFFFF`, **not** key-derived `[rtl_433]`. Everything key-dependent in the
frame is therefore the 2-byte network address, and for ANT+ that address is published: network key
`B9 A5 21 FB BD 72 C3 45` (already in `tools/ant_scan.py:55`) corresponds to `A6 C5`
`[rtl_433]` `[patent US8855246B2]`.

The **key → address algorithm is unknown to anyone outside Garmin and is not needed**. `ant_net.c`
holds a small table seeded with that one pair and returns an invalid-parameter error for any other
key. That is a permanent, documented limitation, not a to-do. Do **not** attempt to fit the function
from observed samples — that is the single activity here most likely to be characterised as
reverse-engineering, and it buys a capability nothing in the plan needs.

## Why the mapping works at all

The nRF24AP2 — the original ANT chip — was an ANT MCU bonded to an **nRF24L01+ radio core**
`[nRF datasheet]`. The frame above is therefore not merely *compatible with* Nordic's Enhanced
ShockBurst layout; it is a descendant of it. Three tells `[inferred]`, all falsifiable by Spike A:

1. The address is a short base plus a distinguishing trailing byte — exactly the nRF24 pipe model,
   in which pipes share the high address bytes and differ in the one transmitted **last**
   `[nRF datasheet]`.
2. The preamble-from-first-address-bit rule is identical `[nRF datasheet]` `[rtl_433]`.
3. The length byte counts the CRC (below), which is a ShockBurst convention, not a general one.

The practical consequence is the premise of the whole rebuild: **the nRF RADIO can emit and receive a
byte-exact ANT frame with no software assistance in the bit path.** No soft modem, no bit-banging,
no per-byte CPU work. Spike A is four days spent confirming that before four months are spent
assuming it.

## Register mapping to validate in Spike A — a prediction

Stated as a prediction so that Spike A can *fail*. This is the tracking / transmit configuration on
the nRF RADIO. It appears here rather than in `ant_core/include/ant_radio_hal.h` on purpose: the HAL
contains no register semantics at all, and this table is what one backend does to satisfy it.

| Setting | Value | Why |
|---|---|---|
| `PCNF0.S0LEN` | `1` | The transmission-type byte sits between the address and the length byte, which is precisely the S0 slot. |
| `PCNF0.LFLEN` | `8` | The length byte is 8 bits. |
| `PCNF0.S1LEN` | `0` | See *Fact one* — S1 is a trap here. |
| `PCNF0.CRCINC` | `1` | The on-air length **includes** the 2 CRC bytes. |
| `PCNF0.PLEN` | `8bit` | One-byte preamble. |
| `PCNF1.BALEN` | `4` | 5-byte on-air address: `A6 C5 devnum_lo devnum_hi device_type`. |
| `PCNF1.ENDIAN` | `Big` | Governs S0/LENGTH/S1/PAYLOAD ordering — **not** the address; see *Fact two*. |
| `PCNF1.WHITEEN` | `Disabled` | No whitening. |
| `CRCCNF` | `LEN=Two \| SKIPADDR=Include` | 2-byte CRC covering the address bytes too. |
| `CRCPOLY` | `0x11021` | Same polynomial as `0x1021`; this register carries the implicit x^16 term explicitly. |
| `CRCINIT` | `0xFFFF` | |
| Packet buffer | `[trans_type][0x0A][d0..d7]` | 10 bytes. The address is in the address registers, the CRC is generated; neither appears in RAM. |

All rows `[nRF datasheet]` for the register semantics, `[inferred]` for the mapping onto ANT.

## Fact one — the length byte is 10 because 10 = 8 + 2

`0x0A` with an 8-byte payload looks like an off-by-two until you notice it is **exactly ShockBurst
semantics**: the length counts the CRC. `PCNF0.CRCINC` exists for precisely this case
`[nRF datasheet]`, so the mapping costs one bit, not a workaround.

This is also a **falsifiable prediction**: an advanced-burst frame carrying 24 payload bytes should
show `0x1A` on air `[inferred]`. If Spike B sees `0x1A`, the length byte really is a length byte and
this section stands. If it sees something else, see *The Spike B gap* — the field may not be a length
at all.

Two consequences worth writing down before someone rediscovers them on the bench:

- **Do not use `S1LEN=8`** to absorb the extra byte. S1 is transmitted *after* LENGTH
  `[nRF datasheet]`, so a byte placed there lands in the wrong position on air and every receiver
  rejects the frame. The symptom is a perfectly healthy-looking transmitter that nothing hears.
- **Fallback if `CRCINC` misbehaves**: `PCNF0 = 0`, `STATLEN = 10`, buffer unchanged. Identical on
  air, one line of difference, no dynamic length parsing. Keep it in the spike so the decision takes
  minutes rather than a day.

## Fact two — address bits go out LSBit-first, whatever `ENDIAN` says

`PCNF1.ENDIAN` governs the bit order of S0, LENGTH, S1 and PAYLOAD. **The address is always
transmitted least-significant-bit first** `[nRF datasheet]`. So every address byte must be
bit-reversed on its way into `BASE`/`PREFIX`; this is what Nordic's own ESB layer has a
`bytewise_bit_swap` helper for.

On-air byte order is the lowest used base byte first and the prefix **last** `[nRF datasheet]`,
matching the nRF24 pipe model. Giving `rev8(0xA6) = 0x65` and `rev8(0xC5) = 0xA3`, the prediction is:

```
BASE0   = (rev8(devnum_hi) << 24) | (rev8(devnum_lo) << 16) | 0x0000A365
PREFIX0 = rev8(device_type)
```

which puts `A6 C5 devnum_lo devnum_hi device_type` on the air, in that order.

**This is the least certain row in the whole document, and it is why Spike A sweeps permutations.**
Two documented details are easy to read the wrong way round — whether a base address shorter than
4 bytes is truncated from the low end or the high end, and which end of `BASE` goes out first. Spike
A therefore sweeps the four bit/byte-order permutations at boot, 5 s each, and reports which one
produces CRC-valid frames. The invariant that must hold whichever way it lands is the on-air order
above; the register arithmetic that produces it is a backend's business and belongs nowhere near
`ant_core`.

## Search needs a different packet configuration

With a 3-byte on-air address `[A6 C5 devnum_lo]`, **three** bytes precede the length field, and the
nRF's fixed `S0 | LENGTH | S1` layout has no slot for a length field there `[nRF datasheet]`. Search
therefore runs static-length:

| Setting | Tracking | Search |
|---|---|---|
| `PCNF0` | `S0LEN=1, LFLEN=8, CRCINC=1` | `0` (no S0, no LENGTH, no S1) |
| `BALEN` | `4` (5-byte address) | `2` (3-byte address) |
| Static length | — | `STATLEN = 12` |
| RAM buffer | 10 bytes `[trans_type][0x0A][d0..d7]` | 12 bytes `[devnum_hi][dtype][ttype][0x0A][d0..d7]` |
| `devnum_lo` | in the matched address | recovered from `RADIO->RXMATCH` |

`[nRF datasheet]` for the mechanisms, `[inferred]` for the mapping.

**CRC coverage is 15 bytes either way** — tracking is 5 address + 10 body, search is 3 address + 12
body `[inferred]`. That is a genuinely useful invariant: assert it in `ant_frame.c`'s unit tests, and
a future format that quietly breaks it announces itself in CI rather than on the air.

**The consequence is a HAL decision, not a bench detail.** Two configurations, both needed
simultaneously by a dongle that is tracking some channels and searching for others, means radio
configuration is **per-operation state, not global**. `ant_radio_hal.h` takes a
`struct ant_pkt_format` with every arm call for this reason and no other.

## Wildcard search: a 32-set sweep, shared by every searching channel

The nRF's minimum on-air address is 3 bytes — `BALEN` ranges 2..4 plus a mandatory prefix byte
`[nRF datasheet]` — so the third matched byte is unavoidably `devnum_lo`, and a true "any device
number" match is not available. Eight logical addresses (`BASE1 = BASE0`, prefixes `AP1..AP7`) cover
8 of the 256 possible values in one window `[nRF datasheet]`, so **32 sets cover all of them**
`[inferred]`.

**Dwell = one channel period (~260 ms with guards) is provably optimal** `[inferred]`. A shorter
dwell does not raise the per-transmission acquisition probability — that is `1/32` either way,
because a shorter window simply misses more transmissions of the set it is on. But a dwell equal to
the channel period *guarantees* the sensor transmits at least once while its set is selected, so
acquisition is certain within one full sweep rather than merely likely. Worst case is 32 × 260 ms =
**8.3 s**, average ~4 s, comfortably inside ANT's 25 s default search timeout `[rev5.1]`.

Three things about the sweep are **not optional**:

1. **One sweep serves every searching channel.** All ANT+ traffic is on RF 57, network 0, so a single
   window that accepts all eight logical addresses catches any matching packet; the resulting channel
   ID is then offered *in software* to every channel in SEARCHING. Without this, eight simultaneous
   searches take ~64 s and `tools/ant_session.py` fails outright.
2. **A recently-seen cache** — 16-entry ring, 60 s lifetime. A dropped sensor is re-acquired in
   ~250 ms instead of ~4 s by trying its own set first. About 30 lines of code, and dropout recovery
   is the thing a rider actually notices.
3. **Merge overlapping tracked RX windows** into one multi-filter receive. Since every ANT+ channel
   shares RF 57, merged windows drop slave-side collisions *between our own tracked channels* to
   zero, and 32 tracked sensors do not cost 32 windows. This is the highest-value item in the
   scheduler.

None of the three is a HAL feature. `ant_radio_hal.h` exposes `caps.max_filters` and
`caps.filter_wildcard_dev`, and all of the above is core policy reading those numbers — which is what
lets a backend with two sync words instead of eight run the same code with a different sweep length.

## The preamble-as-address alternative — a 30-minute experiment

ANT's preamble is deterministic, so it can be treated as part of the address: `BALEN = 2` with the
on-air address `[preamble][A6][C5]`, matching every ANT+ packet in a single window with **no sweep at
all** `[inferred]`.

The honest caveat is in the same sentence: the demodulator then gets **zero** preamble to lock on,
because what would have been the preamble is being matched as address. Bit-clock recovery and AGC
settling both suffer, and the loss is a sensitivity loss, which is exactly the kind that looks like
"it works on the desk" and fails across a room.

So it is a measurement, not an argument. Run CRC-valid-packets-per-minute against the 32-set sweep on
the same rig at the same attenuation. **Decision rule: if it lands within ~6 dB of the sweep, take
it** — it collapses search from 32 windows to one, and 6 dB of search-only sensitivity is a fair
price for that. RX-only either way; a transmitter still emits a real preamble.

## The Spike B gap — an open question, not an answer

**This is the single most important unknown in the public record, and it is stated here as unresolved
on purpose.**

Count the frame again: preamble 1, network address 2, device number 2, device type 1, transmission
type 1, length 1, payload 8, CRC 2 = **18 bytes, every one accounted for** `[rtl_433]`. There is
**nowhere** for the thing ANT obviously must signal: *broadcast* vs *acknowledged* vs *burst sequence
3* vs *burst-last*.

The likely resolution `[inferred]`: the byte rtl_433 calls "length" is really ANT's **control byte**,
and rtl_433 only ever observed sensor broadcasts — a passive ANT+ sniffer watching heart-rate straps
and power meters sees nothing else — so a field that varies in general looked constant in every
sample it had. Under that reading `0x0A` encodes "broadcast, 8-byte payload" and the other message
types live in the same byte.

**That is a guess.** It is not marked `[rtl_433]` because rtl_433 does not say it, and it is not
marked `[rev5.1]` because the free spec documents the host↔dongle serial protocol, not the air
interface. It is `[inferred]`, and the two predictions it makes are:

- an advanced-burst frame shows `0x1A` in that byte (consistent with a length reading too), and
- an acknowledged frame with an 8-byte payload shows something **other than** `0x0A`.

The second one discriminates. Spike B's method needs no new hardware: promiscuous capture
(`BALEN=2`, `STATLEN=12`, all eight prefixes covering a known `devnum_lo`) with microsecond
timestamps; `sim/` on the nRF54L15 DK as the master; the shipping dongle running
`tools/ant_session.py` to send acknowledged data and a burst. Diff broadcast against acknowledged
against each burst packet, byte for byte, and reproduce across two runs. The pass condition is a
table giving the offset and encoding of {broadcast, acknowledged, burst seq 0–7, burst-last}.

**This gates the critical path.** Acknowledged data is how Zwift sets trainer resistance, so an
`ant_core` that can only broadcast is an `ant_core` that cannot run an ERG workout. Nothing above
plain broadcast should be designed in detail until this table exists.

Spike B also yields, for free, three numbers the scheduler needs and no document provides: the
slave→master reply turnaround, the master's slot phase and jitter, and the master's open-time
collision-probe duration `[inferred]`.

## The rtl_433 boundary

rtl_433 is GPL. **RadiANT is Apache-2.0.** The distinction that matters is not "did you look" but
"what did you take":

- **Facts are fine and they live here.** Centre frequency, modulation, the CRC polynomial and seed,
  the absence of whitening, the preamble rule, the field order. Facts are not copyrightable, and this
  document is where they are recorded so that no `ant_core` agent needs to open rtl_433 at all.
- **Expression is not, ever.** No rtl_433 code, no near-verbatim translation of its decoder, no
  transliteration of its structure into C, in any Apache-2.0 file in this repo. There is no cheap way
  to unwind that later, and the whole clean-room defence rests on it.
- If some future file genuinely must derive from rtl_433's expression, it lives in a separate
  GPL-headered directory and is not linked into the shipped image. Nothing currently planned needs
  that.

The same boundary, in the other direction, applies to adopter-gated ANT+ device profile documents:
usable for `src/profiles/`, `tools/ant_pages.py` and the profile docs, and **never** for
`ant_core/**` or for this file. See `docs/decisions/0002-clean-room-policy.md`.

## What this document does not contain

Named so that their absence is visibly a decision:

- **The key → network address function.** Unknown, not needed, and not to be fitted from samples.
- **Anything from `libant.a`.** Not disassembled, not inspected, not by any means.
- **Message-type encoding above broadcast.** Open — see *The Spike B gap*.
- **Channel period, timeout and message-rate constants.** Those are `[rev5.1]` material and belong in
  `docs/ant-serial-protocol.md` with the rest of the serial protocol, not in the link-layer reference.
- **Measured sensitivity figures.** None yet: `[measured]` appears nowhere above. Phase 4 records the
  sdk-ant baseline curve and Spike C adds the EFR32 comparison, on the same rig.
