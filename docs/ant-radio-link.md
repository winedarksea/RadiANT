# The ANT on-air link — clean-room reference

Checked by: **Spike A** (`ant_core/spike/rx_raw` — hears real ANT+ broadcasts and prints raw bytes,
CRC status, match index and RSSI), which **has run and passed**: `docs/spike-a-results.md`, from the
logs `archive/captures/radio/2026-08-09-nrf54l15-run1.log` and
`archive/captures/radio/2026-08-09-nrf54l15-run2.log`. **Spike B**
(promiscuous capture that separates broadcast from acknowledged from burst) **has not run**, and
everything it gates is still a prediction and still marked as one. Nothing else in the repo fails if
this file drifts.

> ### Read the register tables with this in mind
>
> **Spike A ran on an nRF54L15 DK. Every prediction it confirmed was written for the nRF52840, and
> the nRF52840 confirmation has not happened** — it needs one Feather flash, and the Feather's UF2
> bootloader window has closed, so that flash needs a human.
>
> So `[measured]` in this document means *"measured on one of the two v1 parts"*, never *"measured on
> the part you are targeting"*. The nRF54L15 is a v1 target in its own right (Phase 6), and a
> configuration derived from nRF52840 documentation working unmodified on different silicon is
> genuine evidence that the mapping is a property of the **frame** rather than of one peripheral —
> but the two RADIOs differ in ramp-up, in the `TIMING`/`RXGAIN` block, and in the absence of
> `MODECNF0`. Treat a first-run failure on the nRF52840 as a porting question, not as a refutation.

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
| `[measured]` | Measured on this bench, naming the run. Unqualified, it means **Spike A, 2026-08-09, nRF54L15 DK** — `docs/spike-a-results.md`. A tag is only upgraded to `[measured]` by a run that could have failed and did not; a claim the run did not exercise keeps `[inferred]`, however confident the reasoning behind it. |
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

Every field below was read off real frames in Spike A, in the positions drawn above
`[measured]`; the annotated capture is in `docs/spike-a-results.md` under *The frame, byte for byte*.

- Modulation is 1 Mbps GFSK on a 1 MHz raster `[rtl_433]`. ANT+ data channels sit at RF index 57 =
  2457 MHz `[rev5.1]`; the index convention is 2400 + N MHz, N in 0..124 `[rev5.1]`. Confirmed:
  `MODE=Nrf_1Mbit`, `FREQUENCY=57`, 2,164 CRC-valid frames `[measured]`.
- Frequency deviation is ~±170 kHz `[inferred]` — the nRF24L01+ 1 Mbps mode is ±160 kHz
  `[nRF datasheet]` and the original ANT part was built on that core (below), so the true figure is
  in that neighbourhood rather than BLE's ±250 kHz. Falsifiable by Spike C's Radio Configurator work,
  and it matters: an RX filter bandwidth chosen for BLE 1M is the wrong bandwidth for this PHY, which
  is why the EFR32 sensitivity number cannot simply be quoted from a datasheet.
- **Preamble is one byte**, `0x55` or `0xAA`, chosen so the alternating pattern runs continuously
  into the first bit of the address `[rtl_433]` `[patent US8855246B2]`. The nRF RADIO derives its
  preamble by the same rule, from the first address bit, with no software involvement
  `[nRF datasheet]` — which is the first sign that this frame and that peripheral share ancestry.
  **The preamble byte has still never been directly observed** `[inferred]`: `PLEN=8bit` receives
  every frame the transmitter sends, which is consistent with a one-byte preamble, but the
  demodulator does not report what it locked on to. The `0x55` printed in Spike A's `raw18` lines is
  the one assumption in that reconstruction. Falsifiable only with a receiver that reports pre-sync
  bits — an SDR, not the nRF.
- **Network address is 2 bytes. ANT+ is `A6 C5`** `[rtl_433]` `[patent US8855246B2]` `[measured]` —
  matched in hardware on every frame, and read out of RAM in the search configuration.
- **Device number is 2 bytes, low byte first on air** `[rtl_433]` `[measured]`. Device type is 1
  byte, whose MSB is the pairing bit, leaving types 1..127 `[rev5.1]`. Transmission type is 1 byte
  `[rev5.1]`. Together those four bytes are exactly ANT's channel ID `[rev5.1]`, which is why an
  on-air address match *is* a channel-ID match and no software comparison is needed in the common
  case. Spike A recovered `#14871 / 0x0B / 5` from the air on 241 of 241 frames per window, matching
  `tools/ant_scan.py`'s independent ground truth exactly `[measured]`.
- **Length byte is `0x0A`** for a standard 8-byte data message `[rtl_433]` `[measured]` — parsed as a
  length by `LFLEN=8`, so in broadcast it does behave as one. Ten, not eight — see *Fact one* below,
  where the discrepancy turns out to be the most useful thing in the frame. What that byte does
  *above* broadcast is still open; see *The Spike B gap*.
- **CRC is CRC-16/CCITT-FALSE**: width 16, polynomial `0x1021`, init `0xFFFF`, no reflection, no
  final XOR `[rtl_433]` `[measured]`.
- **No whitening** `[rtl_433]` `[measured]`. This matters more than it sounds: whitening is the one
  frame feature that cannot be reproduced by a general-purpose radio without matching the vendor's
  LFSR seed rule, and ANT does not use it.

### Where the CRC starts and stops — the wording trap

The frame after the preamble is **17 bytes**, of which the CRC **covers the first 15** and occupies
the last 2 `[rtl_433]` `[measured]`. Prose that says "CRC over the 17 bytes after the preamble" is
not wrong so much as compressed, and both readings are usable:

- compute over the 15 bytes and compare against the received 2, or
- compute over all 17 and check the result is **zero**.

The second works because CCITT-FALSE has no final XOR and no reflection, so appending a correct CRC
to a message drives the register to zero `[measured]` — arithmetic, and now verified on traffic
rather than asserted. Assert both forms in `ant_frame.c`'s tests; they catch different mistakes.

**Both forms held on every one of the 2,164 CRC-valid frames Spike A recorded** `[measured]`: a
software CCITT-FALSE over the 15 covered bytes equalled `RADIO->RXCRC` every time, and recomputing
over message-plus-CRC gave zero every time — in the tracking configuration (5 address + 10 body) and
in the search configuration (3 address + 12 body) alike. That the same CRC validates under both
splits is the 15-byte coverage invariant being exercised rather than assumed.

A synthetic golden vector for that test, computed not captured, and re-derived by the spike's own
implementation at boot before the radio is touched `[measured]`:

```
message (15 bytes)  A6 C5 34 12 78 01 0A 00 01 02 03 04 05 06 07
CRC-16/CCITT-FALSE  0x1B12
same 15 bytes + 1B 12, recomputed  ->  0x0000
```

The CRC also chains: running the last 13 bytes with the register pre-loaded to the state after
`A6 C5` — which is **`0x233E`** — gives the same `0x1B12` `[measured]`. That is not a curiosity. It is
what lets a radio whose CRC engine cannot cover its own sync word still produce ANT's CRC in
hardware, by baking `0x233E` in as the CRC initial value; see the EFR32 notes in `docs/backends.md`.
The confirmation is arithmetic, from the boot self-test. The one attempt to exercise `0x233E` in a
receiver's CRC hardware was phase E below, which failed for an unrelated reason and so says nothing
either way about the constant.

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
ShockBurst layout; it is a descendant of it. Three tells, which Spike A was built to falsify:

1. The address is a short base plus a distinguishing trailing byte — exactly the nRF24 pipe model,
   in which pipes share the high address bytes and differ in the one transmitted **last**
   `[nRF datasheet]`. **Confirmed** `[measured]`: the prefix goes out last, and all four
   prefix-first permutations heard nothing at all.
2. The preamble-from-first-address-bit rule is identical `[nRF datasheet]` `[rtl_433]`. Consistent
   with a receiver that locks on `PLEN=8bit`, but not directly observed `[inferred]`.
3. The length byte counts the CRC (below), which is a ShockBurst convention, not a general one.
   **Confirmed** `[measured]`: `CRCINC=1` receives, so the on-air length really does include the CRC
   bytes.

The practical consequence is the premise of the whole rebuild: **the nRF RADIO can emit and receive a
byte-exact ANT frame with no software assistance in the bit path.** No soft modem, no bit-banging,
no per-byte CPU work. Spike A is four days spent confirming that before four months are spent
assuming it — and on the receive side, on nRF54L15 silicon, it is now confirmed: 240–241 CRC-valid
frames per 60 s against 240.3 transmitted, six windows, zero CRC errors `[measured]`. **Transmit
remains entirely untested** `[inferred]`; the same mapping is predicted to emit a frame the shipping
dongle hears, and `tools/ant_scan.py` against a spike-driven master is the cheap way to find out.

## Register mapping — measured, on nRF54L15

This was written as a prediction so that Spike A could *fail*. It did not: every row below was
exercised on the air and none of them moved. This is the tracking / transmit configuration on the
nRF RADIO. It appears here rather than in `ant_core/include/ant_radio_hal.h` on purpose: the HAL
contains no register semantics at all, and this table is what one backend does to satisfy it.

**The measurement was on an nRF54L15 DK; these values were derived for the nRF52840 and have never
been run on one.** See the caveat at the top of this file. Read `[measured]` in this table as "this
row is no longer a guess about ANT", not as "this row is proven on your part".

| Setting | Value | Status | Why |
|---|---|---|---|
| `PCNF0.S0LEN` | `1` | `[measured]` | The transmission-type byte sits between the address and the length byte, which is precisely the S0 slot. Transmission type 5 came out of the S0 slot correctly. |
| `PCNF0.LFLEN` | `8` | `[measured]` | The length byte is 8 bits, and `0x0A` was parsed as a length. |
| `PCNF0.S1LEN` | `0` | `[measured]` | Works with S1 absent. See *Fact one* — S1 is a trap here, and the trap was not walked into. |
| `PCNF0.CRCINC` | `1` | `[measured]` | The on-air length **includes** the 2 CRC bytes. 40/40 CRC-valid in every phase B cycle. |
| `PCNF0.PLEN` | `8bit` | `[measured]` | One-byte preamble is enough for the receiver to lock. The preamble *byte value* is still unobserved — see the frame section. |
| `PCNF1.BALEN` | `4` | `[measured]` | 5-byte on-air address: `A6 C5 devnum_lo devnum_hi device_type`. |
| `PCNF1.ENDIAN` | `Big` | `[measured]` | Governs S0/LENGTH/S1/PAYLOAD ordering — **not** the address; see *Fact two*. Payload bytes arrive in transmit order. |
| `PCNF1.WHITEEN` | `Disabled` | `[measured]` | No whitening. |
| `CRCCNF` | `LEN=Two \| SKIPADDR=Include` | `[measured]` | 2-byte CRC covering the address bytes too — the 15-byte coverage. |
| `CRCPOLY` | `0x11021` | `[measured]` | Same polynomial as `0x1021`; this register carries the implicit x^16 term explicitly. |
| `CRCINIT` | `0xFFFF` | `[measured]` | |
| `RXADDRESSES` | `1` | `[measured]` for one filter | Logical address 0 only. The eight-address form the wildcard sweep needs is untested — see below. |
| Packet buffer | `[trans_type][0x0A][d0..d7]` | `[measured]` | 10 bytes, exactly what appears in RAM. The address is in the address registers, the CRC is generated; neither appears there. |

Register semantics throughout are `[nRF datasheet]`; the mapping onto ANT is `[measured]` on
nRF54L15 and `[inferred]` for the nRF52840.

The literal register words that received real traffic, for the ground-truth device `#14871 / 0x0B /
5`, on-air address `A6 C5 17 3A 0B` `[measured]`:

```
MODE      = Nrf_1Mbit      FREQUENCY = 57
BASE0     = 0x5CE8A365     PREFIX0   = 0xD0      TXADDRESS = 0   RXADDRESSES = 1
PCNF0     = 0x04000108     PCNF1     = 0x0104003C            (MAXLEN=60, STATLEN=0, BALEN=4)
CRCCNF    = 0x00000002     CRCPOLY   = 0x11021   CRCINIT     = 0xFFFF
SHORTS    = READY_START | END_START | ADDRESS_RSSISTART
```

`MAXLEN=60` is a spike detail — its buffer is 64 bytes — and a backend sizes it to the format.
`SHORTS` is what keeps RX armed with no software between packets, which is the property the whole
premise rests on.

### Still untested after Spike A

Named so their absence is a visible decision rather than an oversight. Each of these keeps
`[inferred]` wherever it appears in this file, and none of them may be quoted as measured.

- **The nRF52840.** One Feather flash, not yet spent.
- **Transmit.** Spike A is receive-only.
- **`RXMATCH` recovery of `devnum_lo`.** Only one filter was ever programmed, so `RXMATCH` was
  always 0. The value being *readable* is datasheet; the recovery scheme is untried.
- **Eight logical addresses in one window** (`BASE1`, `AP1..AP7`, `RXADDRESSES=0xFF`) — which the
  32-set sweep depends on entirely.
- **`t_sync` capture and its calibration constant.** The spike timestamps nothing.
- **Sensitivity.** −17 dBm across a desk says nothing about range.
- **`S1LEN=8`**, deliberately never attempted; see *Fact one*.
- **The preamble byte itself**, and **frequency deviation** — both need an instrument the nRF is not.
- **Everything above plain broadcast.** That is Spike B, and it is still the gating unknown.

## Fact one — the length byte is 10 because 10 = 8 + 2

`0x0A` with an 8-byte payload looks like an off-by-two until you notice it is **exactly ShockBurst
semantics**: the length counts the CRC. `PCNF0.CRCINC` exists for precisely this case
`[nRF datasheet]`, so the mapping costs one bit, not a workaround. **`CRCINC=1` works**
`[measured]` — 40 CRC-valid frames out of 40 in every phase B cycle, both runs.

This is also a **falsifiable prediction**: an advanced-burst frame carrying 24 payload bytes should
show `0x1A` on air `[inferred]`. If Spike B sees `0x1A`, the length byte really is a length byte and
this section stands. If it sees something else, see *The Spike B gap* — the field may not be a length
at all.

Two consequences worth writing down before someone rediscovers them on the bench:

- **Do not use `S1LEN=8`** to absorb the extra byte. S1 is transmitted *after* LENGTH
  `[nRF datasheet]`, so a byte placed there lands in the wrong position on air and every receiver
  rejects the frame. The symptom is a perfectly healthy-looking transmitter that nothing hears.
  `[inferred]`, and deliberately left that way: Spike A did not spend a window proving a
  known-bad configuration, and the datasheet's field order is not in doubt.
- **`PCNF0 = 0`, `STATLEN = 10`, buffer unchanged is a known-good alternative**, not a fallback to
  be reached for in trouble. It is **identical on air** — 40/40 CRC-valid in the same phase B, same
  cycle, as `CRCINC` `[measured]` — and it costs one line and no dynamic length parsing. Use
  whichever suits the backend: a radio without a `CRCINC` equivalent loses nothing by using the
  static form, and a format change that alters the payload length costs an extra line rather than a
  redesign. `CRCINC=1` is the recommended form only because it keeps the length byte a length byte.

## Fact two — address bits go out LSBit-first, whatever `ENDIAN` says

**This was the least certain claim in the document. It is now the best-evidenced one.** `[measured]`

`PCNF1.ENDIAN` governs the bit order of S0, LENGTH, S1 and PAYLOAD. **The address is always
transmitted least-significant-bit first** `[nRF datasheet]`. So every address byte must be
bit-reversed on its way into `BASE`/`PREFIX`; this is what Nordic's own ESB layer has a
`bytewise_bit_swap` helper for.

On-air byte order is the lowest used base byte first and the prefix **last** `[nRF datasheet]`,
matching the nRF24 pipe model. Giving `rev8(0xA6) = 0x65` and `rev8(0xC5) = 0xA3`:

```
BASE0   = (rev8(devnum_hi) << 24) | (rev8(devnum_lo) << 16) | 0x0000A365
PREFIX0 = rev8(device_type)
```

which puts `A6 C5 devnum_lo devnum_hi device_type` on the air, in that order. For `#14871 / 0x0B`
that is `BASE0 = 0x5CE8A365`, `PREFIX0 = 0xD0`, and those exact words received 240–241 frames per
minute `[measured]`.

Spike A swept all eight bit-order / byte-order / prefix-position permutations at boot, 5 s each, and
the result is not a marginal preference: **the formula above caught every frame the transmitter
sent, and the other seven permutations heard literally nothing** — not degraded reception, zero
address matches, zero CRC-valid frames, in both runs. The four non-reversed packings settle the bit
order and the four prefix-first packings settle the prefix position, independently. A wrong answer
here does not produce a weak link; it produces silence, which is worth knowing when a backend port
comes up dead.

The invariant that must hold on any radio is the on-air order above; the register arithmetic that
produces it is a backend's business and belongs nowhere near `ant_core`.

## Search needs a different packet configuration

With a 3-byte on-air address `[A6 C5 devnum_lo]`, **three** bytes precede the length field, and the
nRF's fixed `S0 | LENGTH | S1` layout has no slot for a length field there `[nRF datasheet]`. Search
therefore runs static-length:

| Setting | Tracking | Search | Status |
|---|---|---|---|
| `PCNF0` | `S0LEN=1, LFLEN=8, CRCINC=1` | `0` (no S0, no LENGTH, no S1) | `[measured]` |
| `BALEN` | `4` (5-byte address) | `2` (3-byte address) | `[measured]` |
| Static length | — | `STATLEN = 12` | `[measured]` |
| RAM buffer | 10 bytes `[trans_type][0x0A][d0..d7]` | 12 bytes `[devnum_hi][dtype][ttype][0x0A][d0..d7]` | `[measured]` |
| `devnum_lo` | in the matched address | recovered from `RADIO->RXMATCH` | `[inferred]` — untested |

`[nRF datasheet]` for the mechanisms. The mapping is `[measured]`: the search configuration
(`PCNF0 = 0x00000000`, `PCNF1 = 0x01020C3C`) decoded **241 CRC-valid frames per 60 s, all 241 of them
`#14871 / 0x0B / 5`, across three windows**, with the channel ID read out of RAM rather than assumed.
That is what makes the pass non-circular: a 5-byte address match proves the channel ID was on the
air but cannot print it, because those bytes are consumed by the matcher.

**`RXMATCH` remains `[inferred]`.** Only one filter was ever programmed in Spike A, so `RXMATCH` was
0 on every frame. Recovering `devnum_lo` from it is the eight-prefix policy in `ant_search.c`, and
belongs in that module's own test rather than being read as settled here.

**CRC coverage is 15 bytes either way** — tracking is 5 address + 10 body, search is 3 address + 12
body `[measured]`, confirmed in both configurations on 2,164 frames. That is a genuinely useful
invariant: assert it in `ant_frame.c`'s unit tests, and a future format that quietly breaks it
announces itself in CI rather than on the air.

### The `BASE0` packing rule when `BALEN < 4` — get this wrong and you hear noise

**This is a stated rule, not a footnote. The naive reading receives nothing but noise triggers, and
it costs a bench day to discover.** `[measured]`

Nordic truncates a short base address **from its least significant byte** `[nRF datasheet]`. Under
the byte order *Fact two* establishes — first on-air base byte at the *low* end of `BASE0` — the
bytes discarded by truncation are exactly the ones you want to keep. So the surviving base bytes
must be shifted up into the top of the register, keeping their relative order:

```
BASE0 = (lsb-first packing of the base bytes) << (8 * (4 - BALEN))
```

Write it in that form: it collapses to the tracking case at `BALEN = 4`, so a backend has one
expression rather than two. For search, on-air address `A6 C5 devnum_lo`, the base bytes are `A6 C5`
and the prefix is `devnum_lo`:

```
BASE0   = (rev8(0xC5) << 24) | (rev8(0xA6) << 16)  =  0xA3650000   (the same for every device)
PREFIX0 = rev8(devnum_lo)
```

which is the tracking packing `0x0000A365` shifted left by `8 * (4 - 2)`. For the ground-truth
device, `devnum_lo = 0x17`, so `PREFIX0 = 0xE8`.

Spike A swept all four packings of this and **only this one heard the transmitter** — 24 CRC-valid
frames in the window, at −17 dBm. The naive low-bytes packing (`BASE0 = 0x0000A365`) produced 9 to 20
address matches per window with **zero** CRC-valid frames, at −54 to −101 dBm: that is the matcher
firing on noise, not weak reception, and the RSSI column separates the two by about 70 dB.

A note for `ant_search.c` from the same measurement: with only three matched bytes the address
matcher fires on noise several times a second on a quiet bench. **Rank and gate on CRC, never on
match count** `[measured]`. Even the correct packing showed 3 to 5 CRC errors per 60 s window
alongside its 241 good frames — every one of them rejected by the CRC, which is exactly the
behaviour the HAL contract promises.

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

**Spike A did not exercise this**: it ran one filter, `RXADDRESSES = 1`, throughout. The 3-byte
address itself is measured and works; the eight-addresses-at-once mechanism the sweep is built on is
still `[inferred]`, and it is the first thing `ant_search.c`'s own bench test should establish.

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

## The preamble-as-address alternative — tested, refuted, keep the sweep

**Recommendation: do not use it on the nRF backend. Keep the 32-set sweep.** `[measured]`

The idea was worth the half hour and is kept here because a refuted hypothesis with evidence is
worth more than a deleted one. ANT's preamble is deterministic, so it can be treated as part of the
address: `BALEN = 2` with the on-air address `[preamble][A6][C5]`, matching every ANT+ packet in a
single window with **no sweep at all** `[inferred]`. That would have collapsed search from 32 windows
to one.

The honest caveat was in the same sentence as the idea: the demodulator then gets **zero** preamble
to lock on, because what would have been the preamble is being matched as address. Bit-clock
recovery and AGC settling both suffer, and the loss is a sensitivity loss, which is exactly the kind
that looks like "it works on the desk" and fails across a room. The stated decision rule was *take it
if it lands within ~6 dB of the 32-set sweep on the same rig*.

**It did not land. Zero CRC-valid frames, across all four base packings, in both runs** — while the
normal configuration caught 100 % of the transmitter's frames at −17 dBm in the same minute on the
same rig. The handful of `end` events it did produce were noise triggers 70 dB down; the real signal
was never matched at all. That is the predicted failure mode in its strongest form, and it is
consistent with the nRF address matcher needing valid preamble bits ahead of the address rather than
merely preferring them.

Getting the CRC right was half the experiment and that part is reusable: ANT's CRC does not cover the
preamble, so `SKIPADDR=Include` would have covered one byte too many. The configuration ran
`SKIPADDR=Skip` with `CRCINIT` preloaded to `0x233E` over the 13 body bytes — the same chaining trick
`docs/backends.md` needs for the EFR32.

Two caveats on the refutation, so nobody re-runs it expecting a different answer or over-reads this
one:

- It is **refuted as configured**, not refuted in principle. The 13-byte body assumes a one-byte
  preamble, which has never been directly observed. A longer preamble would offset the frame and no
  demodulator behaviour could rescue it.
- It was refuted on nRF54L15, like everything else here. A radio with a separate correlator might
  behave differently — but the sweep already meets its timing budget (8.3 s worst case against a 25 s
  search timeout), so there is nothing to buy back. This is not worth a second bench day.

## The Spike B gap — an open question, not an answer

**This is the single most important unknown in the public record, and it is stated here as unresolved
on purpose.**

Spike A did not narrow this by one bit, and it is worth being clear why. Every frame it heard was a
sensor broadcast, so `0x0A` was constant in exactly the way rtl_433 saw it constant; that the byte
parses as a length in broadcast is `[measured]`, and it is also what a control byte encoding
"broadcast, 8-byte payload" would look like. Only Spike B discriminates.

Count the frame again: preamble 1, network address 2, device number 2, device type 1, transmission
type 1, length 1, payload 8, CRC 2 = **18 bytes, every one accounted for** `[rtl_433]` `[measured]`.
There is
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
- **Measured sensitivity figures.** Still none. `[measured]` now appears throughout this file and
  means only that a configuration received real traffic across a desk at −17 dBm; that says nothing
  about range. Phase 4 records the sdk-ant baseline curve and Spike C adds the EFR32 comparison, on
  the same rig, and until then no sensitivity claim in this project has a number behind it.
- **Anything measured on the nRF52840.** One Feather flash, still unspent. See the caveat at the top:
  this file's `[measured]` tags all read "nRF54L15, 2026-08-09".
- **Anything about transmit.** Spike A is receive-only. The mapping is predicted to transmit as well
  as it receives, and that prediction is untested.
- **Timing.** `t_sync`, slot phase, jitter and turnaround are unmeasured; Spike B yields the last
  three for free and the HAL's `t_sync` contract needs its own calibration.
