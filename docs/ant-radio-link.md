# The ANT on-air link — clean-room reference

Checked by: **Spike A** (`radiant_core/spike/rx_raw` — hears real ANT+ broadcasts and prints raw bytes,
CRC status, match index and RSSI), which **has run and passed**: `docs/spike-a-results.md`, from the
logs `archive/captures/radio/2026-08-09-nrf54l15-run1.log` and
`archive/captures/radio/2026-08-09-nrf54l15-run2.log`. And by **Spike B**
(`radiant_core/spike/promisc` — promiscuous capture that separates broadcast from acknowledged from
burst), which **has run twice**, and which **refuted this document's reading of byte 3 and then
refuted the replacement reading as well**:

- **Part 1**, two radios: `docs/spike-b-results.md`, from the seven logs
  `archive/captures/radio/2026-08-09-spike-b-nrf54l15-run{1,2,6}.log`, `...-run3-bursts.log`,
  `...-run4-burst-lengths.log`, `...-run5-timed.log` and `...-config.log`.
- **Part 2**, three radios, which **supersedes part 1 wherever they differ**:
  `docs/spike-b-part2-results.md`, from the four logs
  `archive/captures/radio/2026-08-09-spike-b2-run{0-pacing-bug,A-burst-seq,B-burst-seq-advburst,C-master-control}.log`.
  Those four are checked by `radiant_core/spike/promisc/spike_b_analyse.py --strict`, which refuses a
  capture it cannot show to be complete.

Nothing else in the repo fails if this file drifts. **If this file and those logs disagree, the logs
are right.**

> ### Read the register tables with this in mind
>
> **Every frame in this document was received on an nRF54L15 DK or, in Spike B part 2, on an
> nRF5340 DK's network core. Every prediction they confirmed was written for the nRF52840, and the
> nRF52840 confirmation has not happened** — the Feather has been a transmitter throughout and never
> a receiver. It needs one Feather flash, and the Feather's UF2 bootloader window has closed, so that
> flash needs a human.
>
> So `[measured]` in this document means *"measured on one of the two v1 parts"*, never *"measured on
> the part you are targeting"*. The nRF54L15 is a v1 target in its own right (Phase 6), and a
> configuration derived from nRF52840 documentation working unmodified on different silicon is
> genuine evidence that the mapping is a property of the **frame** rather than of one peripheral —
> but the two RADIOs differ in ramp-up, in the `TIMING`/`RXGAIN` block, and in the absence of
> `MODECNF0`. Treat a first-run failure on the nRF52840 as a porting question, not as a refutation.

This document is what every `radiant_core` agent reads **instead of** anything of Garmin's. It is the
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
| `[measured]` | Measured on this bench, naming the run. Unqualified, it means **Spike A or Spike B, 2026-08-09** — `docs/spike-a-results.md`, `docs/spike-b-results.md`, `docs/spike-b-part2-results.md`. A tag is only upgraded to `[measured]` by a run that could have failed and did not; a claim the run did not exercise keeps `[inferred]`, however confident the reasoning behind it. **Spike B is why that rule is written down, and it earned the point twice.** Part 1 upgraded two rows for free and demoted one this document had called settled. Part 2 then demoted the *replacement* — the length reading of bits 4:0 and the three-bit type field — which had survived 750 frames only because every one of them was a slot-opening frame from a master. A tag that survives a run is not the same as a tag the run could have falsified. |
| `[inferred]` | Reasoning, not a source. Every one of these states how it can be falsified. |

Absent by construction: anything derived from `libant.a`, from sdk-ant's headers, or from an
adopter-gated ANT+ device profile document. See `docs/decisions/0002-clean-room-policy.md`.

## The on-air frame

```
        1        2         2        1       1       1        8         2      bytes
   +--------+---------+---------+-------+-------+--------+---------+--------+
   |preamble| net addr| dev num | dtype | ttype |control | payload |  CRC   |
   |  0x55  |  A6 C5  | lo  hi  |       |       |6 fields| d0..d7  |        |
   +--------+---------+---------+-------+-------+--------+---------+--------+
            |<----------------- 15 bytes CRC coverage ------------>|
            |<------------------------ 17 bytes ------------------------->|
   |<-------------------------- 18 bytes total ------------------------->|
```

Every field below was read off real frames in Spike A, in the positions drawn above
`[measured]`; the annotated capture is in `docs/spike-a-results.md` under *The frame, byte for byte*.
Byte 3 was called a *length* in every earlier version of this drawing; Spike B measured it varying
while the payload did not, and it is a **control byte**.

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
- **Byte 3 is a control byte, not a length** `[measured]`, and it is **six independent fields**
  `[measured]`. Eleven values, over 3,104 CRC-valid frames in ten Spike B runs, with the payload
  eight bytes throughout. The full table and the bit structure are in *Fact one*, below.

  The decisive evidence for "not a length" is not a histogram. An ANT master leaves its last payload
  in its broadcast buffer, so the **same eight payload bytes** go out again one slot later with byte
  3 changing `0xAA` → `0x0A` — same device, same length, 249.7 ms apart, differing in exactly the
  byte under investigation and in nothing else `[measured]`. **A length field cannot do that.** Part
  2 then removed the last hiding place: `0x0A` has `01010` = 10 in bits 4:0 and `0xA2` has `00010`
  = 2, and **both carry eight payload bytes** `[measured]`.

  rtl_433 called it a length because a passive ANT+ sniffer watching heart-rate straps and power
  meters sees nothing but broadcasts, where the byte is constant for a reason that has nothing to do
  with length. What it costs to get this wrong is in the register table below; see *Fact one* for
  the bit structure and *The Spike B gap* for what is still open.
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
rather than asserted. Assert both forms in `radiant_frame.c`'s tests; they catch different mistakes.

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
3. ~~Byte 3 reads 10 for an 8-byte payload, i.e. it counts the CRC — a ShockBurst convention.~~
   **This tell is withdrawn** `[measured]`. A *broadcast's* byte 3 reads 10 and `CRCINC=1` receives
   it, which is why the coincidence was so convincing; but part 2 measured an in-slot frame reading
   2 in the same five bits with the same eight-byte payload, so the byte is not a length in any
   sense and the ten is `0b01010` — the slot bit, a clear sequence bit and `010`. The other two
   tells stand on their own, and the ancestry claim does not need this one.

The practical consequence is the premise of the whole rebuild: **the nRF RADIO can emit and receive a
byte-exact ANT frame with no software assistance in the bit path.** No soft modem, no bit-banging,
no per-byte CPU work. Spike A is four days spent confirming that before four months are spent
assuming it — and on the receive side, on nRF54L15 silicon, it is now confirmed: 240–241 CRC-valid
frames per 60 s against 240.3 transmitted, six windows, zero CRC errors `[measured]`. **Transmit
remains entirely untested** `[inferred]`; the same mapping is predicted to emit a frame the shipping
dongle hears, and `tools/ant_scan.py` against a spike-driven master is the cheap way to find out.

## Register mapping — measured, on nRF54L15

This was written as a prediction so that Spike A could *fail*. It did not — but Spike B did move one
row, and it is the row a backend is built from. This is the tracking / transmit configuration on the
nRF RADIO. It appears here rather than in `radiant_core/include/radiant_core/radiant_radio_hal.h` on purpose: the HAL
contains no register semantics at all, and this table is what one backend does to satisfy it.

**The measurement was on an nRF54L15 DK; these values were derived for the nRF52840 and have never
been run on one.** See the caveat at the top of this file. Read `[measured]` in this table as "this
row is no longer a guess about ANT", not as "this row is proven on your part".

> ## Required form: `PCNF0 = 0`, `STATLEN = 10`. For receive **and** for transmit.
>
> **This is the highest-value line in this document, and it is a Spike B correction to the table
> below.** `[measured]`
>
> Byte 3 is a control byte. A receiver configured with `PCNF0.LFLEN = 8` and `PCNF0.CRCINC = 1` tells
> the nRF RADIO to read it **as a length**. Point that receiver at an acknowledged frame and it reads
> `LENGTH = 0xAA = 170`, overruns `MAXLEN`, and throws the packet away as a CRC error.
>
> **A backend built from the `LFLEN = 8` rows below would receive every broadcast perfectly and
> silently drop every acknowledged and every burst frame.** It would look like a sensitivity problem,
> not a configuration error. Acknowledged data is how Zwift sets trainer resistance, so the symptom
> is "ERG mode does not work", discovered somewhere in Phase 5, months from here. That is the single
> most expensive failure this project could ship, and it costs one register bit to avoid.
>
> **Part 2 closes even the theoretical escape.** `0x0A`'s low five bits read 10 and `0xA2`'s read 2,
> and both frames carry eight payload bytes: there is no `LFLEN`/`CRCINC` combination that parses
> this byte, because no length field reads 10 and 2 for the same length. And `0xA2` is not an exotic
> case — **it is what every frame a slave sends looks like.**
>
> The static form is **identical on air** — Spike A measured 40/40 CRC-valid frames both ways, and
> all 3,104 of Spike B's frames were captured in the static form. It puts byte 3 in RAM where software reads
> it, and on transmit it lets software *choose* it, which is exactly what `radiant_ack.c` and
> `radiant_burst.c` need to do; `LFLEN = 8` on transmit would try to send 170 bytes.
>
> The `LFLEN = 8 / CRCINC = 1` form is a **broadcast-only receiver**. It is kept in the table because
> it is measured and because it is the shape of a length field on this part, not because it is an
> alternative to choose between.

| Setting | Value | Status | Why |
|---|---|---|---|
| **`PCNF0`** | **`0`, with `STATLEN = 10`** | `[measured]` | **The required form, for RX and TX.** No S0, no LENGTH, no S1; the body is a fixed 10 bytes `[ttype][control][d0..d7]` and byte 3 is a body byte software reads and writes. See the box above. |
| `PCNF0.S0LEN` | `1` | `[measured]` | *Broadcast-only form.* The transmission-type byte sits between the address and byte 3, which is precisely the S0 slot. Transmission type 5 came out of the S0 slot correctly. |
| `PCNF0.LFLEN` | `8` | `[measured]`, **broadcast-only** | *Broadcast-only form.* Byte 3 is 8 bits and a broadcast's `0x0A` was parsed as a length. **On an acknowledged frame it parses `0xAA` as 170, overruns `MAXLEN` and drops the frame silently.** Do not build a backend from this row. |
| `PCNF0.S1LEN` | `0` | `[measured]` | Works with S1 absent. See *Fact one* — S1 is a trap here, and the trap was not walked into. |
| `PCNF0.CRCINC` | `1` | `[measured]`, **broadcast-only** | *Broadcast-only form.* A broadcast's byte 3 **includes** the 2 CRC bytes. 40/40 CRC-valid in every phase B cycle. Meaningless without `LFLEN`, and inherits its defect. |
| `PCNF0.PLEN` | `8bit` | `[measured]` | One-byte preamble is enough for the receiver to lock. The preamble *byte value* is still unobserved — see the frame section. |
| `PCNF1.BALEN` | `4` | `[measured]` | 5-byte on-air address: `A6 C5 devnum_lo devnum_hi device_type`. |
| `PCNF1.ENDIAN` | `Big` | `[measured]` | Governs S0/LENGTH/S1/PAYLOAD ordering — **not** the address; see *Fact two*. Payload bytes arrive in transmit order. |
| `PCNF1.WHITEEN` | `Disabled` | `[measured]` | No whitening. |
| `CRCCNF` | `LEN=Two \| SKIPADDR=Include` | `[measured]` | 2-byte CRC covering the address bytes too — the 15-byte coverage. |
| `CRCPOLY` | `0x11021` | `[measured]` | Same polynomial as `0x1021`; this register carries the implicit x^16 term explicitly. |
| `CRCINIT` | `0xFFFF` | `[measured]` | |
| `RXADDRESSES` | `1` for one filter, `0xFF` for eight | `[measured]` both ways | Logical address 0 only in Spike A; Spike B ran all eight logical addresses in one window for 750 frames with no degradation. |
| Packet buffer | `[trans_type][control][d0..d7]` | `[measured]` | 10 bytes, exactly what appears in RAM. Byte 3 is the control byte — one of the eleven values in *Fact one* — and under the required `PCNF0 = 0` form it is a body byte like any other. The address is in the address registers, the CRC is generated; neither appears there. |

Register semantics throughout are `[nRF datasheet]`; the mapping onto ANT is `[measured]` on
nRF54L15 and `[inferred]` for the nRF52840.

The literal register words that received real traffic, for the ground-truth device `#14871 / 0x0B /
5`, on-air address `A6 C5 17 3A 0B` `[measured]`. **This is the broadcast-only form** — `PCNF0 =
0x04000108` is `S0LEN=1, LFLEN=8, CRCINC=1` — and it is recorded because it is what Spike A ran, not
because it is what to program:

```
MODE      = Nrf_1Mbit      FREQUENCY = 57
BASE0     = 0x5CE8A365     PREFIX0   = 0xD0      TXADDRESS = 0   RXADDRESSES = 1
PCNF0     = 0x04000108     PCNF1     = 0x0104003C            (MAXLEN=60, STATLEN=0, BALEN=4)
CRCCNF    = 0x00000002     CRCPOLY   = 0x11021   CRCINIT     = 0xFFFF
SHORTS    = READY_START | END_START | ADDRESS_RSSISTART
```

For the required static form, set `PCNF0 = 0` and put the body length in `PCNF1.STATLEN` — 10 for
tracking, 12 for search. The literal words Spike B ran, in the search geometry, are in
`archive/captures/radio/2026-08-09-spike-b-nrf54l15-config.log`:

```
BALEN=2  BASE0=BASE1=0xA3650000  PREFIX0=0x22CC4488  PREFIX1=0x11EEE8AA
RXADDRESSES=0xFF  PCNF0=0x00000000  PCNF1=0x01020C1C  CRCCNF=0x00000002
```

Using the search configuration rather than the tracking one was the one design decision Spike B had
to get right in advance, and the reason is the box above: **a receiver that parses byte 3 as a length
cannot report that byte 3 is not a length.** It would have thrown away every frame that mattered.

`MAXLEN=60` is a spike detail — its buffer is 64 bytes — and a backend sizes it to the format.
`SHORTS` is what keeps RX armed with no software between packets, which is the property the whole
premise rests on.

### Still untested after Spikes A and B

Named so their absence is a visible decision rather than an oversight. Each of these keeps
`[inferred]` wherever it appears in this file, and none of them may be quoted as measured.

- **The nRF52840.** One Feather flash, not yet spent.
- **Transmit.** Both spikes are receive-only.
- **`t_sync` capture and its calibration constant.** Spike B timestamps in software at the top of the
  interrupt handler, which bounds the jitter but does not calibrate anything.
- **Sensitivity.** −17 dBm across a desk says nothing about range.
- **`S1LEN=8`**, deliberately never attempted; see *Fact one*.
- **The preamble byte itself**, and **frequency deviation** — both need an instrument the nRF is not.
- **Any payload length other than 8 bytes.** Advanced burst has now been enabled twice and
  fragmented both times. Bits 2:0 = `010` therefore still has no measured meaning — only a disproved
  one. See *Fact one*.
- **A master-originated multi-packet burst with a real slave.** The one experiment that would turn
  bit 3 from `[inferred]` into `[measured]`, and it needs a scriptable ANT master.
- **Bit 5 set on a broadcast.** Always 0, and no reason is known why it could not be otherwise.
- **Retry behaviour of the data sender.** The *receiver's* is measured — 21 retransmissions at
  3143 µs; what the sender does when an acknowledgement goes missing is not, because none went
  missing in a completed transfer.
- **Whether an acknowledgement can carry a payload other than the acknowledger's broadcast buffer.**
  Every one observed carried the buffer.

Four rows that were on this list until 2026-08-09 and are **closed** by Spike B part 2 — the burst
sequence encoding, the burst last-packet flag, the slave→master reply frame, and the reply
turnaround. See *Fact one* and *Timing*.

### Two rows Spike B closed for free

Both were on the list above until 2026-08-09. They are `[measured]` now and may be quoted as
measured: Spike B exercised them because it needed them, not because it set out to.

- **Eight logical addresses in one window.** `RXADDRESSES = 0xFF`, `BASE1 = BASE0`, the real
  `devnum_lo` at slot 5 and seven decoys: 750 CRC-valid frames across six runs, no degradation
  against Spike A's single-filter figures. The 32-set sweep now rests on a measurement.
- **`RXMATCH` recovery of `devnum_lo`.** `RADIO->RXMATCH` read **5** on every one of the 750 frames,
  and **slot 5 was chosen deliberately** so that a register stuck at zero could not masquerade as
  success the way slot 0 would have.

## Fact one — the control byte is six independent fields

**Retitled twice, and the second time is the one that stuck.** It began as *the length byte is 10
because 10 = 8 + 2*, about a length field. Part 1 made it *the control byte's low bits read 10*,
still about a length field in five of the eight bits. Part 2 measured the whole byte, and there is
no length field anywhere in it.

```
   bit    7      6      5      4      3     2 1 0
        +------+------+------+------+------+-------+
        | xchg | ack  | last | seq  | slot | 0 1 0 |
        +------+------+------+------+------+-------+
```

| Bit | 0 means | 1 means | Status |
|---|---|---|---|
| **7** | plain broadcast | part of an acknowledged exchange — acknowledged data or burst, either direction | `[measured]` |
| **6** | this is the data packet | this is the **acknowledgement** of one | `[measured]` |
| **5** | more packets follow | **last** packet of the transfer | `[measured]` |
| **4** | — | a **one-bit alternating sequence number** | `[measured]` |
| **3** | a frame sent inside a slot somebody else opened | the frame that **opens the channel slot** | `[inferred]` |
| **2:0** | always `010` | — | `[measured]`; meaning unknown |

**The eleven values that have been on the air, and nothing else has** `[measured]`:

| On air | Direction | Meaning | Runs |
|---|---|---|---|
| `0x0A` | slot opener | **broadcast** — the only encoding with bit 7 clear | A, B, C and all 6 of part 1 |
| `0x8A` | slot opener | burst packet, sequence bit 0, not the last | C and all 6 of part 1 |
| `0xAA` | slot opener | **acknowledged data**, and identically a **one-packet burst** | C and all 6 of part 1 |
| `0x82` | in-slot | burst packet, sequence bit **0**, not the last | 0, A, B |
| `0x92` | in-slot | burst packet, sequence bit **1**, not the last | A, B |
| `0xA2` | in-slot | acknowledged data, and identically burst-**last** with sequence bit 0 | 0, A, B |
| `0xB2` | in-slot | burst **last** packet, sequence bit 1 | A, B |
| `0xC2` | in-slot | **acknowledgement** of a non-final packet, next sequence bit 0 | A, B |
| `0xD2` | in-slot | acknowledgement of a non-final packet, next sequence bit 1 | 0, A, B |
| `0xE2` | in-slot | **acknowledgement of the final packet** — transfer complete — sequence bit 0 | A, B |
| `0xF2` | in-slot | acknowledgement of the final packet, sequence bit 1 | 0, A, B |

Bits 7:5 took `000`, `100`, `101`, `110`, `111` and **never** `001`, `010` or `011` `[measured]`.

### The burst sequence is one bit, and sequences 2 through 7 do not exist

`[measured]`, and it is a positive statement rather than an absence. Nineteen bursts of 1, 2, 3, 6,
9, 17, 27 and 51 on-air packets were captured **end to end**, with the sniffer's ring-drop counter
`dr = 0` on every line proving no frame was lost, and bit 4 alternated `0,1,0,1,…` across every
packet of every one of them while bits **7:6** held still. Block 2 of a nine-packet burst is `0x82`,
the same byte as block 0; if bits 7:5 were a sequence field it could not be. There is no field left
in the frame that could hold a number larger than one.

Bit 5 is **not** part of that still set, and saying "7:5" here was wrong. Re-tabulated from
`archive/captures/radio/2026-08-09-spike-b2-runA-burst-seq.log`: the 17-packet burst at
`t=0427271085` runs `82 92 82 92 …` for packets 0–15 and `A2` on packet 16, so bit 5 is 0
throughout and goes to 1 **exactly once, on the final packet** — the same shape in the 2-, 3- and
9-packet bursts. That is bit 5 doing precisely what the table above says it does, and it is why
"last" is `[measured]` rather than inferred. What holds still across a burst is bits 7:6.

That also settles what part 1 could not: `ANTW_BURST_HEADER_SEQ_MASK`, the serial protocol's own
2-bit sequence, is a **serial-side concept that must be translated, not forwarded**.

### Acknowledged data *is* a one-packet burst

`[measured]`, byte for byte. `0xAA` is exchange set, acknowledgement clear, **last set**, sequence
bit 0, slot bit set — which is "sequence 0, last packet" and nothing else. A receiver dispatching on
this byte cannot distinguish acknowledged data from a one-packet burst, because on air there is
nothing to distinguish; the difference between them is a serial-layer difference only. `radiant_ack.c`
and `radiant_burst.c` should share one encoder.

This retro-explains part 1's puzzle. Four one-block bursts produced `0xAA` and part 1 refused to
read that as burst-last, on the correct observation that `EVENT_TRANSFER_TX_START` does not fire for
a one-block burst. The serial-layer observation was right and the conclusion was wrong: it says
both.

### The reply frame — the row part 1 listed as never observed

`[measured]`, and it is **not** a short acknowledgement. It is a full 8-byte frame with the same
channel ID, and the acknowledger sends **whatever is in its own broadcast buffer at that moment** —
in every capture the master's next bicycle-power page, which then goes out again unchanged as the
next scheduled broadcast one channel period later.

```
page10/0A  t=0096677448          <- master's scheduled broadcast
ack#01/A2  t=0096679622  +2174   <- slave's acknowledged data
page10/F2  t=0096681224  +1602   <- master's acknowledgement
page10/0A  t=0096927139 +245915  <- the same 8 bytes again, as the next broadcast
```

The relation is **bit 6 set, bit 5 echoed, bit 4 complemented**, everything else unchanged:
`82 → D2`, `92 → C2`, `A2 → F2`, `B2 → E2`, on every one of **165 adjacent CRC-valid
data/acknowledgement pairs** across runs 0, A and B, with no counterexample. Runs 0, A and B carry
171 data packets in total: 165 immediately followed by a CRC-valid acknowledgement, three followed
by an acknowledgement that **failed CRC at the sniffer** (excluded, because a CRC-rejected frame is
not evidence of anything), and three followed by the next data packet at a doubled ~3.08 ms gap
where ~1.55 ms is expected. 165 + 3 + 3 = 171, and zero counterexamples under every adjacency
definition tried — within-exchange, whole-file, CRC-valid-only, and all-frames. (An earlier revision
of this file said 168, which was 165 plus the three CRC-failed acknowledgements. Recounted directly
from the logs; the logs are right.)

Read as "the sequence bit I expect next" — which is what a stop-and-wait protocol acknowledges with
— the complement is exactly right; read as "an echo of what I just received" it is exactly wrong.
`[inferred]`: the arithmetic is measured, the reading of it is not.

**The receive path must therefore transmit.** A tracking channel that receives acknowledged data or
a burst packet has ~1.55 ms to put a full 8-byte frame on the air. That is the tightest deadline in
the link layer.

### Bit 3, the slot-opening flag — the one `[inferred]` left in the byte

Part 1 saw `0x0A`, `0x8A`, `0xAA` on 750 frames, all with bit 3 set. Part 2 sees the `0x_2` family
with bit 3 clear. What is **measured** is that bit 3 is *not* master-versus-slave, and one pair of
frames settles that without argument: the master's own broadcast carries it set and the master's
acknowledgement 1.6 ms later, same slot, same transmitter, carries it clear.

Run C is the other half and it is a strong control: **the same Feather, the same stack, the same
driver script and the same payload bytes**, with only the channel role changed from slave to master.
As a slave its acknowledged data is `0xA2`; as a master it is `0xAA`. Its burst packet 0 is `0x82`
as a slave and `0x8A` as a master. One bit moves, and what changed is whether the frame opened the
channel slot or followed inside one.

*Slot opener* is the reading that fits every frame in both parts `[inferred]`. **Falsify it** by
getting a **master** to send a multi-packet burst to a real slave: under this reading its first
packet carries bit 3 and packets 1..N do not, and if instead all of them carry it then bit 3 means
"sent by the channel master" and the master's acknowledgement frames are the anomaly to explain.
That needs a scriptable ANT master, which `sim/` — a bicycle-power sensor that never bursts — is
not.

### Bits 2:0, and the end of the length hypothesis

**Withdrawn** `[measured]`. Part 1 could only say that bits 4:0 held `01010` on every frame and that
nothing distinguished "a length" from "a constant that happens to be 10". The way to settle it was
supposed to be a frame with a payload other than eight bytes. It turned out not to be needed:

- `0x0A` has bits 4:0 = `01010` = **10**, with an eight-byte payload.
- `0xA2` has bits 4:0 = `00010` = **2**, with an eight-byte payload.

Both CRC-valid at `STATLEN = 12`, which is only possible with exactly eight payload bytes. **No
length field reads 10 and 2 for the same length.** The two bits that moved are bit 3 and bit 4, both
accounted for above, and what is left is `010` in bits 2:0 on every frame ever captured in either
part. Its meaning is unknown; it is not a length, because it never changed while the two bits above
it did.

> ### The `0x9A` advanced-burst prediction is WITHDRAWN, not restated
>
> This section once predicted that a 24-byte advanced-burst frame shows `0x1A` on air, and a later
> pass corrected that to `0x9A` — `24 + 2 = 26 = 0b11010` in bits 4:0 with the burst type bits still
> set.
>
> **Its premise is falsified.** The correction assumed bits 4:0 are a length that counts the CRC,
> and they are not; they are the slot bit, the sequence bit and `010`. There is nothing left to
> compute a prediction *from*, so there is no prediction. Neither `0x1A` nor `0x9A` is a candidate
> encoding, and no constant for either appears in `radiant_core` any more.
>
> Nor is the frame any closer to being seen. Part 2 enabled advanced burst again, the dongle
> accepted 24-byte blocks, every transfer completed — and **the air still only ever carried
> eight-byte packets**, each block fragmented into three, with the sequence bit alternating across
> **on-air packets** rather than host blocks. Advanced burst is negotiated between the two ends and
> only one end had it enabled. So **no frame with a payload other than eight bytes has ever been on
> this bench's air**, across both parts.
>
> (`radiant_core/spike/promisc` prints a frame that reads its header and then fails CRC as `longframe`,
> which is what a non-standard length would look like. Runs A, B and C produced 5, 1 and 2 of them
> and every one is an ordinary bit error, printed with a control byte one or two bits from a value
> the run was already producing: `0x0C`, `0x0E`, `0x1A` against `0x0A`. In particular **the only
> `0x1A` this bench has ever seen failed its CRC.**)

Two consequences worth writing down before someone rediscovers them on the bench:

- **Do not use `S1LEN=8`** to absorb the extra byte. S1 is transmitted *after* LENGTH
  `[nRF datasheet]`, so a byte placed there lands in the wrong position on air and every receiver
  rejects the frame. The symptom is a perfectly healthy-looking transmitter that nothing hears.
  `[inferred]`, and deliberately left that way: Spike A did not spend a window proving a
  known-bad configuration, and the datasheet's field order is not in doubt.
- **`PCNF0 = 0`, `STATLEN = 10`, buffer unchanged is the required form**, on receive and on transmit.
  It used to be described here as a known-good alternative, on the reading that `CRCINC=1` was
  preferable because "it keeps the length byte a length byte". There is no length byte. It is
  **identical on air** — 40/40 CRC-valid in the same phase B, same cycle, as `CRCINC` `[measured]`,
  and it is the configuration all 3,104 of Spike B's frames were captured in, across both parts — it
  costs one line and no dynamic length parsing, and it is the only form that can receive an
  acknowledged frame or transmit one. See the box in the register table.

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
produces it is a backend's business and belongs nowhere near `radiant_core`.

## Search needs a different packet configuration

With a 3-byte on-air address `[A6 C5 devnum_lo]`, **three** bytes precede byte 3, and the nRF's fixed
`S0 | LENGTH | S1` layout has no slot for a length field there `[nRF datasheet]`. Search therefore
runs static-length — and since Spike B, so does tracking, for a different reason:

| Setting | Tracking | Search | Status |
|---|---|---|---|
| `PCNF0` | `0` (no S0, no LENGTH, no S1) | `0` (no S0, no LENGTH, no S1) | `[measured]` |
| `BALEN` | `4` (5-byte address) | `2` (3-byte address) | `[measured]` |
| Static length | `STATLEN = 10` | `STATLEN = 12` | `[measured]` |
| RAM buffer | 10 bytes `[trans_type][control][d0..d7]` | 12 bytes `[devnum_hi][dtype][ttype][control][d0..d7]` | `[measured]` |
| `devnum_lo` | in the matched address | recovered from `RADIO->RXMATCH` | `[measured]` |

Tracking's `PCNF0` row is the Spike B correction; the `S0LEN=1, LFLEN=8, CRCINC=1` form it used to
carry is a broadcast-only receiver. The two configurations now differ only in address length and
body length, which is a simplification the bench bought rather than one anybody designed.

`[nRF datasheet]` for the mechanisms. The mapping is `[measured]`: the search configuration
(`PCNF0 = 0x00000000`, `PCNF1 = 0x01020C3C`) decoded **241 CRC-valid frames per 60 s, all 241 of them
`#14871 / 0x0B / 5`, across three windows**, with the channel ID read out of RAM rather than assumed.
That is what makes the pass non-circular: a 5-byte address match proves the channel ID was on the
air but cannot print it, because those bytes are consumed by the matcher.

**`RXMATCH` is now `[measured]`.** Only one filter was ever programmed in Spike A, so `RXMATCH` was 0
on every frame and the recovery scheme was untried. Spike B armed all eight logical addresses,
**wrote the real `devnum_lo` into slot 5 and seven decoys into the others**, and `RADIO->RXMATCH`
read **5** on every one of its 750 CRC-valid frames. Slot 5 rather than slot 0 was the point: at slot
0 a register stuck at zero is indistinguishable from a working one. Indexing the prefix table with
`RXMATCH` is what `radiant_search.c` will do, and it now rests on a measurement that could have failed.

**The cost of eight filters, so `radiant_search.c` can budget it** `[measured]`: with the transmitter
off, eight three-byte matchers produced **19, 22 and 27 CRC failures per 15 s window** on this bench
— of order 1.4 per second, against Spike A's 3 to 5 per *60 s* window with one filter. Every one was
rejected by the CRC and none ever decoded to a plausible channel ID. Rank and gate on CRC, never on
match count — Spike A's advice, now with eight times the reason.

**CRC coverage is 15 bytes either way** — tracking is 5 address + 10 body, search is 3 address + 12
body `[measured]`, confirmed in both configurations on 2,164 frames. That is a genuinely useful
invariant: assert it in `radiant_frame.c`'s unit tests, and a future format that quietly breaks it
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

A note for `radiant_search.c` from the same measurement: with only three matched bytes the address
matcher fires on noise several times a second on a quiet bench. **Rank and gate on CRC, never on
match count** `[measured]`. Even the correct packing showed 3 to 5 CRC errors per 60 s window
alongside its 241 good frames — every one of them rejected by the CRC, which is exactly the
behaviour the HAL contract promises.

**The consequence is a HAL decision, not a bench detail.** Two configurations, both needed
simultaneously by a dongle that is tracking some channels and searching for others, means radio
configuration is **per-operation state, not global**. `radiant_radio_hal.h` takes a
`struct radiant_pkt_format` with every arm call for this reason and no other.

## Wildcard search: a 32-set sweep, shared by every searching channel

The nRF's minimum on-air address is 3 bytes — `BALEN` ranges 2..4 plus a mandatory prefix byte
`[nRF datasheet]` — so the third matched byte is unavoidably `devnum_lo`, and a true "any device
number" match is not available. Eight logical addresses (`BASE1 = BASE0`, prefixes `AP1..AP7`) cover
8 of the 256 possible values in one window `[nRF datasheet]`, so **32 sets cover all of them**
`[inferred]`.

**Spike A did not exercise this**: it ran one filter, `RXADDRESSES = 1`, throughout. **Spike B did,
because it needed it** `[measured]`: `RXADDRESSES = 0xFF`, `BASE1 = BASE0`, eight prefixes, 750
CRC-valid frames across six runs with no degradation against the single-filter figures. The
eight-addresses-at-once mechanism the sweep is built on is a measurement now, not a prediction. What
`radiant_search.c`'s own bench test still has to establish is the *sweep* — 32 sets, dwell, and the
recently-seen cache — none of which any spike has run.

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
   zero, and 32 tracked sensors do not cost 32 windows — they cost sixteen, because the nRF matches
   **two device numbers per tracking window** and not eight (`caps.max_addr_groups`; see
   `docs/backends.md`). This is still the highest-value item in the scheduler.

None of the three is a HAL feature. `radiant_radio_hal.h` exposes `caps.max_filters`,
`caps.max_addr_groups` and `caps.filter_wildcard_dev`, and all of the above is core policy reading
those numbers — which is what lets a backend with two sync words instead of eight run the same code
with a different sweep length.

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

## The Spike B gap — closed

**This section used to say "an open question, not an answer", and it was the single most important
unknown in the public record. It has run twice.** `docs/spike-b-results.md`,
`docs/spike-b-part2-results.md`.

The question was where ANT signals *broadcast* vs *acknowledged* vs *burst sequence 3* vs
*burst-last*, given that the frame has no spare byte: preamble 1, network address 2, device number 2,
device type 1, transmission type 1, byte 3, payload 8, CRC 2 = **18 bytes, every one accounted for**
`[rtl_433]` `[measured]`. The guessed resolution — that the byte rtl_433 calls "length" is ANT's
control byte, and that rtl_433 only ever saw broadcasts — was right. The answer to *where the
sequence lives* was not guessed and could not have been: **there is no burst sequence 3.**

| Row | Status |
|---|---|
| broadcast | `[measured]` — `0x0A`, the only encoding with bit 7 clear |
| acknowledged data | `[measured]` — `0xAA` slot-opening, `0xA2` in-slot, **and it is a one-packet burst** |
| burst data, not last | `[measured]` — `0x8A` / `0x82` / `0x92`, sequence bit alternating |
| burst, last packet | `[measured]` — `0xA2` / `0xB2`, bit 5 |
| burst sequences 2..7 | `[measured]` — **they do not exist.** The sequence is one bit |
| the slave→master reply frame | `[measured]` — a full 8-byte frame, bit 6 set, bit 5 echoed, bit 4 complemented |
| any payload length other than 8 | **not observed**, and still the last open row |
| bit 3 as "slot opener" | `[inferred]` — measured only to be *not* master-versus-slave |

The full table and its evidence are in *Fact one*.

**What `radiant_core` may rely on today**: the whole encoding. **`radiant_burst.c` can be written now** —
bit 7 set, bit 6 clear on data and set on the acknowledgement, bit 5 on the final packet, bit 4
alternating per on-air packet, bit 3 set only if this frame opens the slot, bits 2:0 `010`. A
transmitter alternates bit 4 and expects the acknowledgement to carry its complement.

`radiant_ack.c` and `radiant_burst.c` should **share one encoder**, because acknowledged data and a
one-packet burst are the same bytes. And **the receive path must transmit**: a tracking channel has
~1.55 ms to answer a data packet with a full 8-byte frame.

Two things neither part saw, named so nobody reads their absence as a negative result: **any frame
whose payload is not eight bytes**, so bits 2:0 have a disproved meaning rather than a measured one;
and **frames from any other ANT+ device**, since `foreign` was zero throughout both parts, so
nothing here says what a real sensor from another vendor puts in that byte.

## Timing — all three numbers measured, and two more

Spike B part 1 was supposed to yield three numbers the scheduler needs and no document provides. It
yielded two. Part 2 got the third and two the brief had not asked for.

All figures are ADDRESS-interrupt to ADDRESS-interrupt, so both ends of every interval carry one
interrupt entry and the offset cancels. Software timestamps, not (D)PPI — see the jitter note below,
which still applies to every number here.

### Master slot period and jitter — `[measured]`

ADDRESS-interrupt to ADDRESS-interrupt between consecutive frames of the same master, over **744
intervals in six runs**: mean **249,696.4 µs**, standard deviation **5.4–6.5 µs** per run,
peak-to-peak 34–50 µs. Nominal is 8182 / 32768 s = 249,694.8 µs, so the measured mean is +1.6 µs =
**+6.4 ppm** — two boards' crystals disagreeing, which is the right order for two 50 ppm-rated parts,
not the master drifting.

**That jitter figure is an upper bound and is dominated by the measurement, not by the master.**
Timestamps are captured in software at the top of the interrupt handler, so both ends of every
interval carry one interrupt entry. The true master jitter is at or below 6 µs rms. Measuring it
properly needs (D)PPI hardware capture, which `radiant_radio_hal.h`'s `t_sync` calibration will need
anyway.

For `radiant_sched.c` the usable number is: **a real ANT master holds its slot to well inside ±25 µs over
minutes**, so a receive-window guard of tens of microseconds is about drift and clock error, not
about the master being sloppy.

### Master open-time — `[measured]`, with an ambiguity that is not resolvable from the air

Time from `MESG_OPEN_CHANNEL` to the first `EVENT_TX`, over eight runs: 250, 250, 250, 250, 250, 265,
266, 266 ms — the 16 ms quantisation is the Windows clock, not the radio. That is **one full channel
period (249.7 ms) in 8 of 8 runs**. Cross-checked against the host-timestamped capture two ways,
including one that does not depend on correlating two clocks: every `EVENT_TX` produces exactly one
new payload on the air one slot later with no repeats, which is only consistent with `EVENT_TX` being
raised immediately after each transmission.

**A master does not transmit when you open it. It transmits one channel period later.**
`radiant_channel.c` should reproduce that, and a Zwift-style host that expects data immediately after
opening a master channel is going to wait a slot.

**Whether that period is a collision probe or simple slot alignment is not observable from the air**,
and the reason is structural rather than a gap in the experiment: **a sniffer cannot see a receive
window.** A master listening before it talks and a master aligning to the next boundary of an
internal timebase produce exactly this measurement. Distinguishing them needs the master's own
instrumentation. This document does not claim which it is.

### The three turnarounds — `[measured]`, part 2

The numbers that needed the third radio. Two independent runs, reported separately rather than
averaged, because the first packet of a transfer and every packet after it are visibly different
populations — sd 8 µs against sd 19–26 µs — and one mean describes neither.

| Interval | Run A | Run B |
|---|---|---|
| Master broadcast → **slave's reply** | n=7, **2186 µs**, sd 8, range 2179–2195 | n=12, **2191 µs**, sd 8, range 2181–2201 |
| Data packet → **master's acknowledgement** | n=33, **1567 µs**, sd 21, range 1520–1599 | n=128, **1559 µs**, sd 19, range 1515–1595 |
| Acknowledgement → **next data packet** | n=26, **1546 µs**, sd 26, range 1485–1592 | n=116, **1550 µs**, sd 21, range 1488–1598 |

Three things the scheduler and `radiant_burst.c` both need:

1. **A slave answers its master 2.19 ms after the master's ADDRESS**, which is about 2.08 ms after
   the master's frame ends. This is the reply turnaround part 1 called *not measured*.
2. **A burst runs at one packet per ~3.11 ms**, a strict data/acknowledgement alternation at
   ~1.55 ms each way. Eight payload bytes per 3.11 ms is about **20.6 kbit/s** of user data, which
   is what an ANT burst is worth on this link.
3. **The first packet of a burst costs the full 2.19 ms slot turnaround; every packet after it costs
   1.55 ms.**

### What a receiver does when a burst stops — `[measured, n=2]`

From the run that failed, kept because no successful run can show it. The slave sent burst packet 0
and then nothing. The master answered `0xD2` and then **retransmitted that identical frame — same
payload, same CRC — twenty more times at 3143 µs**, spanning 66 ms, before abandoning the transfer.

So a receiver in the middle of a burst re-sends its acknowledgement rather than waiting quietly, and
it does so at **3143 µs** against the **3113 µs** a running burst actually takes per packet — near
enough that it is plainly the same timer, and far enough apart, at 1 %, to be worth recording as two
numbers. **21 attempts** is what this stack does before giving up; `radiant_burst.c`'s receive path needs
a retry limit and this is a measured value for it, on one stack, twice.

The sender's side of that is **not measured**: no acknowledgement went missing in a completed
transfer, so what the *data sender* does when one does is unknown.

## The rtl_433 boundary

rtl_433 is GPL. **RadiANT is Apache-2.0.** The distinction that matters is not "did you look" but
"what did you take":

- **Facts are fine and they live here.** Centre frequency, modulation, the CRC polynomial and seed,
  the absence of whitening, the preamble rule, the field order. Facts are not copyrightable, and this
  document is where they are recorded so that no `radiant_core` agent needs to open rtl_433 at all.
- **Expression is not, ever.** No rtl_433 code, no near-verbatim translation of its decoder, no
  transliteration of its structure into C, in any Apache-2.0 file in this repo. There is no cheap way
  to unwind that later, and the whole clean-room defence rests on it.
- If some future file genuinely must derive from rtl_433's expression, it lives in a separate
  GPL-headered directory and is not linked into the shipped image. Nothing currently planned needs
  that.

The same boundary, in the other direction, applies to adopter-gated ANT+ device profile documents:
usable for `src/profiles/`, `tools/ant_pages.py` and the profile docs, and **never** for
`radiant_core/**` or for this file. See `docs/decisions/0002-clean-room-policy.md`.

## What this document does not contain

Named so that their absence is visibly a decision:

- **The key → network address function.** Unknown, not needed, and not to be fitted from samples.
- **Anything from `libant.a`.** Not disassembled, not inspected, not by any means.
- **Any frame whose payload is not eight bytes.** The last open row of the control byte, and the
  reason bits 2:0 = `010` has a disproved meaning rather than a measured one — see *Fact one*. The
  burst sequence, the last-packet flag and the reply frame are all closed as of 2026-08-09.
- **Channel period, timeout and message-rate constants.** Those are `[rev5.1]` material and belong in
  `docs/ant-serial-protocol.md` with the rest of the serial protocol, not in the link-layer reference.
- **Measured sensitivity figures.** Still none. `[measured]` now appears throughout this file and
  means only that a configuration received real traffic across a desk at −17 dBm; that says nothing
  about range. Phase 4 records the sdk-ant baseline curve and Spike C adds the EFR32 comparison, on
  the same rig, and until then no sensitivity claim in this project has a number behind it.
- **Anything measured on the nRF52840.** One Feather flash, still unspent. See the caveat at the top:
  this file's `[measured]` tags all read "nRF54L15, 2026-08-09".
- **Anything about transmit.** Both spikes are receive-only. The mapping is predicted to transmit as
  well as it receives, and that prediction is untested.
- **A real jitter figure, and the `t_sync` calibration constant.** Slot phase, open-time and all
  three turnarounds are measured, but every one of them is a software timestamp taken at the top of
  an interrupt handler, so every microsecond figure here carries interrupt-entry jitter. The
  differences are what is reported and the offset cancels in all of them — but **the 1.55 ms
  turnaround in particular deserves a hardware-captured measurement before a transmit deadline is
  designed around it**, because that deadline is the tightest thing in the link layer. The HAL's
  `t_sync` contract still needs its own (D)PPI calibration. See *Timing*.
