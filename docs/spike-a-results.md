<!-- SPDX-License-Identifier: Apache-2.0 -->
# Spike A results - the premise holds

> ## Superseded in part by `docs/spike-b-part2-results.md`
>
> **This is a primary record and is not rewritten. Everything it observed is
> confirmed.** What is superseded is one *inference* Spike A could not have
> scoped correctly, and the distinction is the reusable lesson here.
>
> **Byte 3 is a control byte, and there is no length field in an ANT frame.**
> Its low bits are not a length either: `0x0A` reads 10 there and `0xA2` reads
> 2, and both carry an eight-byte payload. Measured across 3,104 CRC-valid
> frames in ten Spike B runs. See `docs/spike-b-part2-results.md`, and
> `docs/ant-radio-link.md` for the whole six-field structure.
>
> Spike A did not get this wrong so much as get it *narrowly right*. On a bench
> carrying nothing but broadcasts, `PCNF0.LFLEN = 8` with `CRCINC = 1` genuinely
> parses `0x0A` and genuinely receives every frame - the 40/40 and 2,164-frame
> figures below all stand. **Spike A never saw an acknowledged or a burst frame**,
> because there was no second radio to provoke one, and every frame it did see
> was a slot-opening broadcast from a master, in which byte 3 is constant for
> reasons that have nothing to do with length. The observation was sound; the
> inference generalised from a sample that could not contain a counterexample.
>
> The lesson, which is about spike scope rather than about this byte: **a
> configuration that receives 100 % of the traffic on the bench is evidence
> about the bench's traffic.** A tag is only upgraded by a run that could have
> failed. Spike A's own run could not have falsified the length reading, so that
> row should never have been graded **confirmed** - and the two rows where it
> was are annotated in place below rather than edited away.
>
> Nothing else in this file has moved. The three rows affected are
> `PCNF0.LFLEN = 8`, *Fact one*, and *Length byte `0x0A`* in the frame-layout
> table; a broadcast-only receiver is what the first of them describes, and
> `docs/ant-radio-link.md` keeps it on those terms.

Checked by: `radiant/spike/rx_raw` and the captures it produced,
`archive/captures/radio/2026-08-09-nrf54l15-run1.log` and
`...-run2.log`. Re-running the spike against a transmitter with a different
device number requires rebuilding it with a different `SPIKE_DEVNUM`; if this
document and those logs disagree, the logs are right.

Provenance: `[measured]` throughout unless stated. Measured on this bench on
2026-08-09. The register semantics referenced are `[nRF datasheet]`; the
predictions being tested are the `[inferred]` rows of `docs/ant-radio-link.md`.
Nothing here derives from `sdk-ant`, from `libant.a`, or from a non-redistributable
ANT+ device profile document.

---

## Verdict

**PASS, on the nRF54L15.** A bare nRF RADIO, configured from
`docs/ant-radio-link.md` alone, receives real ANT+ broadcasts with no software
in the bit path.

The pass criterion was **≥50 CRC-valid frames in 60 s whose decoded device
number and device type exactly match `tools/ant_scan.py`'s ground truth**. The
measured figure is **240–241 CRC-valid frames in 60 s, 100 % of them matching,
reproduced over six 60 s windows across two independent runs.** The transmitter
sends 240.3 frames in 60 s, so this is essentially every frame it emitted.

The predicted register mapping was right in every row that this spike could
test, including the one `docs/ant-radio-link.md` calls its least certain claim.
Seven of the eight address permutations heard nothing at all - not a degraded
signal, *nothing* - so the result is not a marginal preference between readings.

### Read this before quoting the verdict

**The nRF54L15's RADIO is not the nRF52840's, and every register prediction in
`docs/ant-radio-link.md` was written for the nRF52840.** The two peripherals
share register names, field positions and the MDK bitfield macros used here, and
the packet engine is the same lineage - but they are different silicon, with
different ramp-up behaviour, a different `TIMING`/`RXGAIN` block, and no
`MODECNF0`.

So:

- A pass on the nRF54L15 is **strong** evidence for the premise, and the
  nRF54L15 is a v1 target in its own right (Phase 6). It is not a substitute for
  the nRF52840 confirmation, which still needs **one Feather flash**.
- Had this **failed**, the result would have been genuinely ambiguous - premise
  wrong, or nRF54L RADIO difference - and would have had to be reported that
  way rather than as a dead premise. It did not fail, which is the one outcome
  that does not carry that ambiguity: a configuration derived from nRF52840
  documentation worked unmodified on a different part, which is weak evidence
  that the mapping is a property of the *frame*, not of one peripheral.

---

## The rig

| Role | Board | Why |
|---|---|---|
| Transmitter | Adafruit Feather nRF52840, already running the shipping dongle firmware (`0FCF:1009`) | Driven as an ANT+ master from the host by `tools/ant_sim.py`. **Zero Feather flashes were used.** |
| Receiver (the spike) | nRF54L15 DK, `nrf54l15dk/nrf54l15/cpuapp` | J-Link, so it reflashes unattended and the permutation sweep costs nothing to iterate |
| Log transport | The DK's first VCOM (`uart20`), **COM7**, 115200 | Chosen over RTT: no SEGGER DLL needed on the host, and the capture archives as plain text |

The plan puts Spike A on the Feather. It was moved because the Feather's UF2
bootloader window has closed, so flashing it now needs a human to double-tap
RESET - and the varying half of a bench experiment belongs on the board that can
be flashed unattended.

### Ground truth, established first

`tools/ant_sim.py --serial 6183 --seconds 720 -q` on the Feather. The receiver
for this step was the nRF54L15 DK itself, temporarily flashed with
`dist/ant_dongle_nrf54l15dk.hex` over J-Link so that establishing ground truth
also cost no Feather flash, then `tools/ant_scan.py --port COM8 --seconds 45`:

```
  found: #14871 - power meter
Heard 21 broadcasts from 1 sensor(s)
```

Device number **14871** (`0x3A17`), device type **11** (`0x0B`), transmission
type **5**, channel period 8182 (4.005 Hz). Those exact values are the pass
criterion, and the spike checks against them rather than against anything it
derived itself.

The scan heard 21 of ~180 frames, which is the known artefact and not a radio
problem: `tools/ant_scan.py:145` opens its wildcard channel on period `0x1F86`
(8070, the heart-rate period) while the transmitter runs 8182.

---

## The winning configuration

Phase A swept eight permutations with a 5-byte address and a static 10-byte
body. Deliberately static: it makes the address question independent of whether
the length byte is a length byte at all, so exactly one unknown moves at a time.

```
[res] P0 rev,a0-at-lsb,pfx-last  end=24 crc_ok=24 crc_err=0 id_ok=24 rssi_avg=-17dBm
[res] P1 rev,a0-at-msb,pfx-last  end=0  crc_ok=0
[res] P2 raw,a0-at-lsb,pfx-last  end=0  crc_ok=0
[res] P3 raw,a0-at-msb,pfx-last  end=0  crc_ok=0
[res] P4 rev,a0-at-lsb,pfx-1st   end=0  crc_ok=0
[res] P5 rev,a0-at-msb,pfx-1st   end=0  crc_ok=0
[res] P6 raw,a0-at-lsb,pfx-1st   end=0  crc_ok=0
[res] P7 raw,a0-at-msb,pfx-1st   end=0  crc_ok=0
```

P0 won identically in all six cycles across both runs: 24 CRC-valid frames in
6 s, which is every frame the transmitter sent in the window.

**P0 is exactly the prediction in `docs/ant-radio-link.md`.** Address bytes are
bit-reversed, the first on-air base byte sits in the *least* significant byte of
`BASE0`, and the prefix goes out last.

### Tracking / TX - the exact register values that worked

For on-air address `A6 C5 17 3A 0B` and body `[05][0A][d0..d7]`:

| Register | Value | Derivation |
|---|---|---|
| `MODE` | `Nrf_1Mbit` (0) | |
| `FREQUENCY` | `57` | 2457 MHz, `MAP_Default` |
| `BASE0` | `0x5CE8A365` | `(rev8(dnh)<<24) \| (rev8(dnl)<<16) \| 0x0000A365`, with `rev8(0x3A)=0x5C`, `rev8(0x17)=0xE8`, `rev8(0xC5)=0xA3`, `rev8(0xA6)=0x65` |
| `PREFIX0` | `0xD0` | `rev8(device_type)` = `rev8(0x0B)` |
| `TXADDRESS` / `RXADDRESSES` | `0` / `1` | logical address 0 |
| `PCNF0` | `0x04000108` | `S0LEN=1`, `LFLEN=8`, `S1LEN=0`, `PLEN=8bit`, `CRCINC=Include` |
| `PCNF1` | `0x0104003C` | `MAXLEN=60`, `STATLEN=0`, `BALEN=4`, `ENDIAN=Big`, `WHITEEN=Disabled` |
| `CRCCNF` | `0x00000002` | `LEN=Two`, `SKIPADDR=Include` |
| `CRCPOLY` | `0x11021` | |
| `CRCINIT` | `0xFFFF` | |
| `SHORTS` | `READY_START \| END_START \| ADDRESS_RSSISTART` | keeps RX armed with no software between packets |

`MAXLEN` is a spike detail (the buffer is 64 bytes); a backend would size it to
the format.

The `BASE0` formula in `docs/ant-radio-link.md` can be quoted verbatim. It is
now `[measured]`.

### Search - the correction the document needs

The 3-byte search configuration works, but **not with the naive truncation**,
and this is the one place where following the document literally would have cost
bench time.

With `BALEN=2` the used bytes of `BASE0` are the **high** two, not the low two.
Nordic's wording is that the base address is truncated from its least
significant byte, and under the byte order phase A established - first on-air
base byte at the *low* end - that means the first on-air bytes are the ones
discarded. So the two surviving base bytes have to be shifted up into the top of
the register while keeping the same relative order:

```
BASE0   = rev8(a1) << 24 | rev8(a0) << 16        (a0 is first on air)
PREFIX0 = rev8(a2)
```

For `[A6 C5 17]`: `BASE0 = 0xA3650000`, `PREFIX0 = 0xE8`. Equivalently, and this
is the form worth writing into a backend because it collapses to the tracking
case at `BALEN=4`:

```
BASE0 = (lsb-first packing of the base bytes) << (8 * (4 - BALEN))
```

Phase C swept all four packings and only this one heard the transmitter:

```
[res] K0 a0-at-lsb,low-bytes   end=9..20 crc_ok=0            (noise triggers, -54..-101 dBm)
[res] K1 a0-at-lsb,high-bytes  end=24    crc_ok=24 id_ok=24  rssi_avg=-17dBm
[res] K2 a0-at-msb,low-bytes   end=11..18 crc_ok=0           (noise triggers)
[res] K3 a0-at-msb,high-bytes  end=0     crc_ok=0
```

Search registers: `PCNF0 = 0x00000000`, `PCNF1 = 0x01020C3C`
(`MAXLEN=60`, `STATLEN=12`, `BALEN=2`, `ENDIAN=Big`, `WHITEEN=Disabled`),
`CRCCNF`/`CRCPOLY`/`CRCINIT` unchanged from tracking.

A note for whoever writes `radiant_search.c`: with only three matched bytes the
address matcher fires on noise several times a second on this bench. Rank and
gate on CRC, never on match count. The failing packings above are that effect,
not weak reception - the RSSI column separates them by ~70 dB.

---

## What the pass actually measured

### Phase D, tracking (5-byte address, `CRCINC` config)

```
run 1  [res] D-tracking  end=241 crc_ok=241 crc_err=0 id_ok=241 sw_crc_ok=241 zero_ok=241 rssi_avg=-17dBm
run 1  [res] D-tracking  end=240 crc_ok=240 crc_err=0 id_ok=240 sw_crc_ok=240 zero_ok=240 rssi_avg=-17dBm
run 1  [res] D-tracking  end=240 crc_ok=240 crc_err=0 id_ok=240 sw_crc_ok=240 zero_ok=240 rssi_avg=-17dBm
run 2  [res] D-tracking  end=240 crc_ok=240 crc_err=0 id_ok=240 sw_crc_ok=240 zero_ok=240 rssi_avg=-17dBm
run 2  [res] D-tracking  end=240 crc_ok=240 crc_err=0 id_ok=240 sw_crc_ok=240 zero_ok=240 rssi_avg=-17dBm
run 2  [res] D-tracking  end=240 crc_ok=240 crc_err=0 id_ok=240 sw_crc_ok=240 zero_ok=240 rssi_avg=-17dBm
```

Six 60 s windows, all 240 or 241 against 240.3 expected. **Zero CRC errors and
zero missed frames**, at -17 dBm on a desk. Run 1 and run 2 are separate builds,
separate flashes and separate transmitter sessions.

### Phase D, search (3-byte address) - why the pass is not circular

A 5-byte address match proves the device number and device type were on the air,
but it cannot *print* them: those bytes are consumed by the hardware matcher and
never reach RAM. The search configuration puts them in the buffer instead, so
they are read rather than assumed:

```
[cfg] D-search  addr=A6 C5 17 BALEN=2 BASE0=0xA3650000 PREFIX0=0xE8 PCNF0=0x00000000 PCNF1=0x01020C3C
  PKT crc=OK  rxmatch=0 rssi=-17dBm len=0x0A dev=14871 type=0x0B trans=5
      raw18 55 A6 C5 17 3A 0B 05 0A 10 BD FF 50 DE 11 64 00 19 9A
      rxcrc=0x199A sw_crc=0x199A match  recompute_over_17=0x0000 zero
[res] D-search  end=246 crc_ok=241 crc_err=5 id_ok=241 sw_crc_ok=241 zero_ok=241 rssi_avg=-17dBm
[res] D-search  end=244 crc_ok=241 crc_err=3 id_ok=241 sw_crc_ok=241 zero_ok=241 rssi_avg=-17dBm
[res] D-search  end=245 crc_ok=241 crc_err=4 id_ok=241 sw_crc_ok=241 zero_ok=241 rssi_avg=-17dBm
```

**241 CRC-valid frames in 60 s, 241 of them decoding to `#14871 / 0x0B / 5`, in
all three windows.** The three to five CRC errors per window are the noise
triggers a 3-byte address invites; every one was rejected by the CRC, which is
the behaviour the HAL contract promises.

The `raw18` line is a reconstruction and the document should say so: only the 12
body bytes and `RADIO->RXCRC` are observed. The address bytes printed are the
ones that were programmed - the match is the evidence they were on the air - and
the leading `0x55` is the single genuine assumption, because the demodulator
does not report what it locked on to.

### The frame, byte for byte

```
55  A6 C5  17 3A  0B  05  0A  10 BD FF 50 DE 11 64 00  19 9A
|   |      |      |   |   |   |                        |
|   |      |      |   |   |   +- 8 payload bytes        +- CRC
|   |      |      |   |   +----- length 0x0A
|   |      |      |   +--------- transmission type 5
|   |      |      +------------- device type 0x0B
|   |      +-------------------- device number 0x3A17 = 14871, low byte first
|   +--------------------------- ANT+ network address
+------------------------------- preamble (assumed)
```

Every field of `docs/ant-radio-link.md`'s frame diagram is where the document
says it is. The payload decodes as ANT+ page `0x10` at ~100 W, which is what
`ant_sim.py` was asked to transmit.

The bytes above are the record and are not touched. **One label in it is a
reading, not an observation**: the byte annotated `length 0x0A` is byte 3, and
Spike B part 2 measured it as a **control byte** with no length field in it -
`0x0A` is the broadcast encoding. See the box at the top of this file.

### CRC

Three things, all confirmed:

- **On real frames.** A software CRC-16/CCITT-FALSE computed over the
  reconstructed 15 covered bytes equalled `RADIO->RXCRC` on **every one of the
  2,164 CRC-valid frames** recorded in phase D across both runs (`sw_crc_ok`),
  and recomputing over message+CRC gave zero on every one (`zero_ok`). Both
  forms, both configurations, no exceptions.
- **Against the golden vector**, at boot, before the radio is touched:
  ```
  CRC self-test: golden=0x1B12 (expect 0x1B12), over-17=0x0000 (expect 0x0000),
                 chained-from-0x233E=0x1B12 (expect 0x1B12)
  ```
- **The chaining constant.** `0x233E` - the CCITT-FALSE state after `A6 C5` - is
  arithmetically confirmed. It is the constant `docs/backends.md` needs for a
  radio whose CRC engine cannot cover its own sync word.

---

## Every claim in `docs/ant-radio-link.md`, graded

Rows marked **confirmed** may have their `[inferred]` tag upgraded to
`[measured]`, qualified by "on nRF54L15" where noted.

### Register mapping (the prediction table)

| Claim | Verdict | Note |
|---|---|---|
| `PCNF0.S0LEN = 1` (transmission type in S0) | **confirmed** | trans type 5 read correctly out of the S0 slot |
| `PCNF0.LFLEN = 8` | **confirmed**, **SUPERSEDED** | `0x0A` was parsed as a length and every frame was received. On broadcasts only - see the note under this table |
| `PCNF0.S1LEN = 0` | **confirmed** | works with S1 absent; the S1 trap was not walked into |
| `PCNF0.CRCINC = 1` | **confirmed** | 40/40 CRC-valid in phase B, every cycle. `CRCINC` behaves; **the fallback was not needed** |
| `PCNF0.PLEN = 8bit` | **confirmed** | one-byte preamble is enough for the receiver to lock |
| `PCNF1.BALEN = 4` | **confirmed** | 5-byte on-air address |
| `PCNF1.ENDIAN = Big` | **confirmed** | payload bytes come out in transmit order |
| `PCNF1.WHITEEN = Disabled` | **confirmed** | no whitening |
| `CRCCNF = LEN=Two \| SKIPADDR=Include` | **confirmed** | 15-byte coverage including the address |
| `CRCPOLY = 0x11021`, `CRCINIT = 0xFFFF` | **confirmed** | |
| Packet buffer `[trans_type][0x0A][d0..d7]` | **confirmed** | exactly the 10 bytes that appear in RAM |

> **SUPERSEDED - `docs/spike-b-part2-results.md`, the `PCNF0.LFLEN = 8` row.**
> The observation is exactly right and is not withdrawn: the radio was told to
> read byte 3 as a length, it did, and 100 % of the transmitter's frames were
> received. What does not follow is that byte 3 *is* a length. **Every frame in
> this run was a broadcast** - one master, no peer, so nothing on the air could
> carry any other value - and on a broadcast byte 3 is always `0x0A`. Point the
> same configuration at an acknowledged frame and it reads `LENGTH = 0xAA = 170`,
> overruns `MAXLEN` and drops the packet as a CRC error. This row therefore
> grades a **broadcast-only receiver**, which is how `docs/ant-radio-link.md`
> now labels it; the required form is `PCNF0 = 0` with `STATLEN = 10`, which the
> *Fallback* row two tables down measured as identical on air.

### The two facts

| Claim | Verdict | Note |
|---|---|---|
| **Fact one** - length byte 10 = 8 payload + 2 CRC, ShockBurst semantics, `CRCINC` is the right mechanism | **confirmed**, **SUPERSEDED** | the `CRCINC` half stands as measured; the length half does not - see below |
| Fallback `PCNF0=0, STATLEN=10` is identical on air | **confirmed** | 40/40 as well; the two are interchangeable, so the fallback is real and costs one line |
| Do not use `S1LEN=8` | **untested** | not attempted. The reasoning is sound and there is no reason to spend a window proving a known-bad configuration |
| **Fact two** - address bits go out LSBit-first regardless of `ENDIAN`; bit-reverse every address byte | **confirmed** | the four non-reversed permutations heard nothing whatsoever |
| On-air order is lowest used base byte first, prefix last | **confirmed** | all four prefix-first permutations heard nothing |
| `BASE0 = (rev8(dh)<<24)\|(rev8(dl)<<16)\|0x0000A365`, `PREFIX0 = rev8(dtype)` | **confirmed** | `0x5CE8A365` / `0xD0` |

> **SUPERSEDED - `docs/spike-b-part2-results.md`, the *Fact one* row.** Read the
> row as three claims, because they did not survive together.
>
> - *`CRCINC` is the right mechanism* - **stands** for a broadcast-only
>   receiver, and 40/40 CRC-valid is the measurement that says so.
> - *10 = 8 payload + 2 CRC, ShockBurst semantics* - **withdrawn.** Byte 3 is a
>   control byte of six independent fields. The ten is `0b01010`: the slot bit,
>   the sequence bit and `010`, and its arithmetic agreement with 8 + 2 is a
>   coincidence, which is exactly what made it so convincing. `0xA2` reads **2**
>   in the same five bits with the same eight-byte payload, and no length field
>   reads 10 and 2 for one length.
> - The **ancestry** argument this row supported - that ANT's frame descends
>   from Enhanced ShockBurst - does not depend on it. `docs/ant-radio-link.md`
>   withdraws this tell and keeps the other two.
>
> Spike A could not have found this. It saw 2,164 frames and every one was a
> master's broadcast; a value other than `0x0A` was not available to be seen.

### Search configuration

| Claim | Verdict | Note |
|---|---|---|
| Search needs `PCNF0=0`, `BALEN=2`, `STATLEN=12`, 12-byte buffer | **confirmed** | |
| Buffer is `[devnum_hi][dtype][ttype][0x0A][d0..d7]` | **confirmed** | read straight out of RAM |
| CRC coverage is 15 bytes either way | **confirmed** | 3 address + 12 body validates against the same CRC as 5 + 10 |
| `devnum_lo` recoverable from `RXMATCH` | **untested** | only one filter was programmed, so `RXMATCH` was always 0. The eight-prefix sweep is core policy and belongs in `radiant_search.c`'s own test |
| The `BASE0` packing for `BALEN=2` | **refuted as written** | the document does not state it; the naive reading is wrong. See *Search - the correction the document needs* above |

### Frame layout

| Claim | Verdict |
|---|---|
| Network address is `A6 C5`, 2 bytes | **confirmed** |
| Device number 2 bytes, low byte first on air | **confirmed** |
| Device type 1 byte, then transmission type 1 byte | **confirmed** |
| Length byte `0x0A` for a standard 8-byte data message | **confirmed** as a byte value, **SUPERSEDED** as a name - byte 3 is the control byte, and `0x0A` is its broadcast encoding |
| CRC is CRC-16/CCITT-FALSE, init `0xFFFF`, over 15 bytes | **confirmed** |
| Appending a correct CRC drives the register to zero | **confirmed** on 2,164 real frames |
| CRC chains: state after `A6 C5` is `0x233E` | **confirmed** arithmetically |
| No whitening | **confirmed** |
| Preamble is one byte `0x55`/`0xAA` | **not directly observed** - `PLEN=8bit` receives correctly, which is consistent, but the demodulator does not report the preamble it locked on to |
| 1 Mbps GFSK on RF index 57 | **confirmed** |
| Frequency deviation ~±170 kHz | **untested** - `MODE=Nrf_1Mbit`'s deviation was not measured; Spike C's job |

---

## Phase E - preamble-as-address is refuted on this part

The 30-minute bonus: `BALEN=2` with the on-air address `[55 A6 C5]`, which would
match every ANT+ packet in one window and collapse wildcard search from 32
windows to one.

It needs a different CRC setup, and getting that right is half the experiment:
ANT's CRC does not cover the preamble, so `SKIPADDR=Include` would cover one byte
too many. The spike therefore ran `SKIPADDR=Skip` with `CRCINIT` preloaded to
`0x233E`, covering the 13 body bytes - which is also a hardware test of the
chaining constant.

**Result: zero CRC-valid frames, across all four base packings, in both runs.**

```
[res] K0 a0-at-lsb,low-bytes   end=5 crc_ok=0   (-87..-101 dBm, noise)
[res] K1 a0-at-lsb,high-bytes  end=1 crc_ok=0
[res] K2 a0-at-msb,low-bytes   end=5 crc_ok=0
[res] K3 a0-at-msb,high-bytes  end=0 crc_ok=0
```

The few `end` events are noise triggers 70 dB below the transmitter; the real
signal was never matched at all. `docs/ant-radio-link.md`'s decision rule was
"take it if it lands within ~6 dB of the sweep". It did not land: the normal
configuration caught 100 % of frames at -17 dBm in the same minute on the same
rig, and this caught none.

**Recommendation: drop the preamble-as-address alternative for the nRF backend
and keep the 32-set sweep.** Two honest caveats on that recommendation:

- The failure mode is the one the document itself predicted - with the preamble
  consumed as address, the demodulator gets nothing to lock on. Zero rather than
  degraded is a stronger version of the same effect, and it is consistent with
  the nRF address matcher requiring valid preamble bits ahead of the address.
- The 13-byte body assumes the preamble is exactly one byte. If the transmitter
  emits a longer preamble, the frame would be offset and this configuration
  could not work whatever the demodulator did. That was not separately measured,
  so "refuted **as configured**" is the correct strength of claim. It is not
  worth a second bench day: the sweep already meets its timing budget.

---

## What remains nRF52840-specific, and what is still untested

Named so their absence is visibly a decision.

1. **The nRF52840 confirmation. This needs one Feather flash and was not done.**
   Every register value above was measured on nRF54L15 silicon. The mapping is
   expected to port unchanged - the field positions are identical and the MDK
   macros are the same names - but "expected" is what this spike exists to stop
   people saying. Build `radiant/spike/rx_raw` for
   `adafruit_feather_nrf52840/nrf52840/uf2`, or better for the nRF5340 DK, which
   flashes unattended and whose network core is a closer relative of the
   nRF52840 RADIO than the nRF54L15 is.
2. **Transmit was not tested at all.** Spike A is receive-only. The same mapping
   is predicted to transmit a frame the shipping dongle will hear, and
   `tools/ant_scan.py` against a spike-driven master is the cheap way to check
   it.
3. **`RXMATCH` recovery of `devnum_lo`** - one filter only, so `RXMATCH` was
   always 0.
4. **Eight logical addresses in one window** (`BASE1`, `AP1..AP7`,
   `RXADDRESSES=0xFF`), which the 32-set sweep depends on. Not exercised.
5. **`t_sync` capture and its calibration constant.** The spike does not
   timestamp anything. The HAL's `t_sync` contract remains entirely unmeasured,
   and its failure mode is silent - see the paragraph in `radiant_radio_hal.h`.
6. **Sensitivity.** -17 dBm on a desk says nothing about range. Phase 4's
   baseline curve and Spike C's comparison are where that lives.
7. **Anything above plain broadcast.** That is Spike B, and it is still the
   gating unknown.
8. **`S1LEN=8`** was not tried, deliberately.

---

## Practical notes for whoever runs this next

- **Zero Feather flashes were used**, and none are needed to repeat any of the
  above. `tools/ant_sim.py` turns the already-flashed dongle firmware into an
  ANT+ master over USB, and the DK reflashes over J-Link.
- **Two J-Link probes are attached** and non-interactive J-Link cannot choose
  between them; every command fails with "Cannot connect to the
  probe/programmer" unless one is selected. Point
  `scripts/flash_sim_jlink.ps1 -JLinkExe` at a `.cmd` shim that injects
  `-SelectEmuBySN <serial>` and forwards `%*`, so the script's own
  `exec DisableAutoUpdateFW` still runs first. Never let the probe firmware
  auto-update.
- **Do not pipe `scripts/flash_sim_jlink.ps1` into `Select-Object -First N`.**
  PowerShell stops the upstream pipeline once it has the objects it asked for,
  which kills `JLink.exe` mid-program; the symptom is exit 255 with no
  explanation.
- **The nRF54L15 DK's `sim/` image was overwritten** by the spike.
  `scripts\flash_sim_jlink.ps1` restores it unattended.
- The spike loops forever, so a capture started at any moment gets a complete
  cycle, and two cycles are a reproducibility check for free. One cycle is about
  five minutes.
