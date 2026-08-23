<!-- SPDX-License-Identifier: Apache-2.0 -->
# Spike B results - it is a control byte

Checked by: `radiant/spike/promisc/spike_b_analyse.py`, which refuses to report
a capture it cannot show to be complete. Run with the NCS toolchain's Python
interpreter (there is no system Python in this project):

```powershell
python radiant\spike\promisc\spike_b_analyse.py `
    archive\captures\radio\2026-08-09-spike-b2-runA-burst-seq.log `
    archive\captures\radio\2026-08-09-spike-b2-runB-burst-seq-advburst.log `
    archive\captures\radio\2026-08-09-spike-b2-runC-master-control.log --strict
```

and it exits 0. The three-radio runs above are what this document is built
from; six earlier two-radio runs
(`archive/captures/radio/2026-08-09-spike-b-nrf54l15-run{1,2,6}.log`,
`...-run3-bursts.log`, `...-run4-burst-lengths.log`, `...-run5-timed.log`,
`...-config.log`) are the source for *Readings that stand from the two-radio
runs* near the bottom. If this document and any of those logs disagree, the
logs are right.

Provenance: `[measured]` throughout unless a line says otherwise. Measured on
this bench on 2026-08-09, with three radios on the air at once for the first
time, after six earlier two-radio runs. The register semantics referenced are
`[nRF datasheet]`; the prediction being tested is the `[inferred]` *Spike B
gap* section of `docs/ant-radio-link.md`. Nothing here derives from
`sdk-ant`, from `libant.a`, or from a non-redistributable ANT+ device profile
document.

---

## Verdict

**The control byte is six independent fields, and there is no three-bit burst
sequence number in it.** The burst sequence is **one bit**, alternating -
which is why the two-radio runs, which could only ever see the first packet
of a burst, could not have found it and were right to refuse to guess.

**Eleven** values were observed across the four three-radio runs, and every
one of them decomposes. Nine came from the two burst-focused runs; the third,
the master-control run, adds `0x8A` and `0xAA`:

```
   bit    7      6      5      4      3     2 1 0
        +------+------+------+------+------+-------+
        | xchg | ack  | last | tog  | slot | 0 1 0 |
        +------+------+------+------+------+-------+
```

| Bit | 0 means | 1 means | Both values seen |
|---|---|---|---|
| **7** | plain broadcast | part of an acknowledged exchange (acknowledged data or burst) | yes |
| **6** | this is the data packet | this is the **acknowledgement** of one | yes |
| **5** | more packets follow | **last** packet of the transfer | yes |
| **4** | - | - | yes; it is a one-bit alternating sequence number, see below |
| **3** | a frame sent inside a slot somebody else opened | the frame that **opens the channel slot** | yes |
| **2:0** | always `010` | 0 exceptions in 2,354 CRC-valid frames here, and the two-radio runs' 750 all had bits 4:0 = `01010` | no |

The byte sits at **offset 3 of the body** - equivalently, immediately before
the eight payload bytes, and the seventh byte of the frame after the preamble
if you count the two network-address bytes and `devnum_lo` as part of the
frame, which the nRF RADIO does not because they are its address field.

### The table the pass criterion asked for

Every row measured, in at least two independent runs unless the *Runs* column
says otherwise.

| On air | Direction | Meaning | Runs |
|---|---|---|---|
| `0x0A` | slot opener | **broadcast** | 0, A, B, C, and all 6 two-radio runs |
| `0xAA` | slot opener | **acknowledged data**, and identically a **one-packet burst** | C, and all 6 two-radio runs |
| `0x8A` | slot opener | burst packet, sequence bit 0, not the last | C, and all 6 two-radio runs |
| `0x82` | in-slot | burst packet, sequence bit **0**, not the last | 0, A, B |
| `0x92` | in-slot | burst packet, sequence bit **1**, not the last | A, B |
| `0xA2` | in-slot | **acknowledged data**, and identically a one-packet burst; also burst-**last** with sequence bit 0 | 0, A, B |
| `0xB2` | in-slot | burst **last** packet, sequence bit 1 | A, B |
| `0xC2` | in-slot | **acknowledgement** of a non-final packet, next sequence bit 0 | A, B |
| `0xD2` | in-slot | **acknowledgement** of a non-final packet, next sequence bit 1 | 0, A, B |
| `0xE2` | in-slot | **acknowledgement of the final packet** - the transfer is complete - sequence bit 0 | A, B |
| `0xF2` | in-slot | acknowledgement of the final packet, sequence bit 1 | 0, A, B |

**Burst sequence 2 through 7 do not exist.** That is a positive statement, not
an absence: a seventeen-packet burst and a fifty-one-packet burst were both
captured end to end with a drop counter proving no frame was lost, and bit 4
alternated `0,1,0,1,...` across every one of them while bits 7:5 stayed
constant. There is no field in the frame that could hold a number larger than
one.

Bits 7:5 took the values `000`, `100`, `101`, `110`, `111` and **never**
`001`, `010` or `011`. `b7 = 0` only ever occurred with `b6 = b5 = 0`, which
is to say the only broadcast encoding is `0x0A`.

---

## The rig

| Role | Board | Image |
|---|---|---|
| Master | nRF54L15 DK | `sim/`, ANT+ bicycle power, device **14871** / type `0x0B` / transmission type 5, period 8182 |
| Slave | Adafruit Feather nRF52840 | the shipping dongle firmware, `0FCF:1009`, driven by `spike_b_drive.py --role slave`. **Zero Feather flashes were used** |
| Sniffer | nRF5340 DK **network core** | `radiant/spike/promisc`, console on the network core's VCOM |

A burst needs a receiver, so this needs three radios at once: a master, a
slave and a sniffer. The bench has three boards and only two of them can be
an ANT endpoint outright, so the **nRF5340 DK's network core is the
sniffer**. Getting it printing needed two register writes granting it its
own console pins (`radiant/spike/promisc/appcore`, `P1.PIN_CNF[0..1].MCUSEL =
NetworkMCU`) - not a silicon limitation: every GPIO on an nRF5340 belongs to
the application core until the application core hands it over, and the
companion image grants only P1.00/P1.01 rather than the whole port. It is a
stock DK. The RTT fallback (`radiant/spike/promisc/net_rtt.conf`,
`build/promisc_net_rtt`) is kept because it is the only diagnostic that tells
"the capture program is not running" apart from "the pins were never
granted", but it was never needed.

The first three-radio run still failed before the data below was captured:
burst packets after the first died with `EVENT_TRANSFER_TX_FAILED`, because
`spike_b_drive.py` paced on `EVENT_TRANSFER_NEXT_DATA_BLOCK` - an event
`src/ant_serial_bridge.c` consumes and never forwards to the host, on the
stated grounds that a real ANT stick frames bursts itself. Streaming instead
of waiting for that event fixed it: the bridge already blocks in
`k_sem_take(&burst_block_free, K_MSEC(1000))` before copying each block, so it
throttles the host on its own. `2026-08-09-spike-b2-run0-pacing-bug.log`
keeps that failed run because it captured a measurement nothing else does -
see *What a receiver does when a burst stops*, below.

---

## How each bit was established

### Bit 4, the sequence bit - the headline

A nine-packet burst, in full, from
`2026-08-09-spike-b2-runA-burst-seq.log`. `burst#NN` is the payload marker
`spike_b_drive.py` wrote into bytes 0-1 of that block, so each line maps to a
host block by identity; `page10` is the master's bicycle-power page.

```
page10/0A | burst#00/82 +2195 | page10/D2 +1581 | burst#01/92 +1501 | page10/C2 +1581
          | burst#02/82 +1562 | page10/D2 +1521 | burst#03/92 +1592 | page10/C2 +1520
          | burst#04/82 +1562 | page10/D2 +1551 | burst#05/92 +1562 | page10/C2 +1551
          | burst#06/82 +1562 | page10/D2 +1551 | burst#07/92 +1562 | page10/C2 +1551
          | burst#08/A2 +1562 | page10/F2 +1550
```

Nine data packets, nine acknowledgements, strictly interleaved. The data
packets run `82 92 82 92 82 92 82 92 A2` - bit 4 alternates with the block
index and nothing else moves until the last packet sets bit 5. The
seventeen-packet burst in the same run does the same thing seventeen times
and wraps eight times; the fifty-one-packet advanced burst in run B does it
fifty-one times.

If bits 7:5 were a three-bit sequence field, block 2 would differ from block
0. It does not: they are the same byte, `0x82`.

### Bit 5, the last-packet flag

The cleanest pair is the two-packet burst, because it differs from the
nine-packet one in exactly one host-side decision:

```
page10/0A | burst#00/82 +2194 | page10/D2 +1583 | burst#01/B2 +1498 | page10/E2 +1584
```

Block 1 is `0xB2`, and `0xB2` is `0x92` with bit 5 set. Block 1 of the
nine-packet burst was `0x92`. Same sequence bit, same direction, same
everything except that this one is the last - and bit 5 is what changed.

### Bit 6, the acknowledgement flag, and the reply frame

It is not a short ack. It is a full frame with the same channel ID and eight
payload bytes, and the acknowledger simply sends whatever is in its own
broadcast buffer at that moment - in every capture here the master's next
bicycle-power page, which then goes out again unchanged as the next
scheduled broadcast one channel period later.

```
page10/0A  t=0096677448          <- master's scheduled broadcast
ack#01/A2  t=0096679622  +2174   <- slave's acknowledged data
page10/F2  t=0096681224  +1602   <- master's acknowledgement
page10/0A  t=0096927139 +245915  <- same 8 bytes again, as the next broadcast
```

Bit 6 is 1 on every acknowledgement and 0 on every data packet, in both
directions of the exchange. `0xC2`/`0xD2` acknowledge a non-final packet and
`0xE2`/`0xF2` acknowledge the final one, so **bit 5 is echoed** by the
acknowledgement.

The acknowledgement's bit 4 is the **complement** of the packet it answers,
and its bit 5 is an echo, in **all 165** data/acknowledgement pairs across
runs 0, A and B, with no exceptions: `82 -> D2`, `92 -> C2`, `A2 -> F2`,
`B2 -> E2`. Read as "the sequence bit I expect next", which is what a
stop-and-wait protocol acknowledges with, that is exactly right; read as "an
echo of what I just received" it is exactly wrong. `[inferred]` - the
arithmetic is measured, the reading of it is not.

**Where 165 comes from, since a nearby number is easy to reach for.** Runs 0,
A and B carry **171** data packets between them. 165 are immediately followed
by a CRC-valid acknowledgement, and those 165 are the pairs the relation is
asserted on. Three more are followed by an acknowledgement that **failed CRC
at the sniffer** - visible in the analyser's `(ER)` column - and a frame the
CRC rejected is not evidence of anything, so those three are excluded rather
than counted; 165 + 3 = 168 is the number to *avoid* quoting as measured
pairs. The last three are followed by the next data packet, where the
acknowledgement was missed entirely, visible as a doubled ~3.08 ms gap where
~1.55 ms is expected. 165 + 3 + 3 = 171, and there is no counterexample
anywhere in the 171.

### Bit 3, the slot-opening flag - and the control that pins it

The two-radio runs saw `0x0A`, `0x8A`, `0xAA` on 750 frames, all with bit 3
set. The three-radio runs also see `0x02`-family values with bit 3 clear. The
difference is not master versus slave, and one pair of frames settles that
without any argument:

```
page10/0A  ...  <- master, bit 3 SET
page10/F2  ...  <- master, 1.6 ms later, same slot, bit 3 CLEAR
```

Same transmitter, same channel, 1.6 ms apart. So bit 3 does not identify the
master.

Run C is a strong control: **the same Feather, the same stack, the same
driver script and the same payload bytes**, with only the channel role
changed from slave to master. As a slave its acknowledged data is `0xA2`; as
a master it is `0xAA`. Its burst packet 0 is `0x82` as a slave and `0x8A` as
a master. The single differing bit is 3, and the thing that changed is
whether the frame opened the channel slot or followed inside one.

`[inferred]`, and named as such: *slot opener* is the reading that fits every
frame in both sets of runs. Falsify it by getting a **master** to send a
multi-packet burst to a real slave - under this reading its first packet
carries bit 3 and packets 1..N do not, and if instead all of them carry it,
bit 3 means "sent by the channel master" and the master's acknowledgement
frames are the anomaly to explain. That experiment needs a master this bench
cannot script; `sim/` is a bicycle-power sensor and never bursts.

### Bits 2:0, and the end of the length hypothesis

**Byte 3's low bits are not a length, and that is now measured rather than
argued.** The two-radio runs could only say that bits 4:0 held `01010` on
every frame and that nothing distinguished "a length" from "a constant that
happens to be 10". The way to settle it was supposed to be a frame with a
payload other than eight bytes. It turned out not to be needed:

- `0x0A` has bits 4:0 = `01010` = 10, with an eight-byte payload.
- `0xA2` has bits 4:0 = `00010` = 2, with an eight-byte payload.

Both CRC-valid at `STATLEN = 12`, which is only possible with exactly eight
payload bytes. A length field cannot read 10 and 2 for the same length. The
two bits that moved are bit 3 (slot) and bit 4 (sequence), both accounted for
above, and what is left is `010` in bits 2:0 on every frame ever captured.
Its meaning is unknown. It is not a length, because it never changed while
the two bits above it did.

The prediction in `docs/ant-radio-link.md` that an advanced-burst frame shows
`0x9A` is therefore **not merely untested, it is built on a reading that is
now falsified**, and it should be withdrawn rather than restated.

### Advanced burst, again, and why it still proves nothing about length

Run B enabled advanced burst, the dongle accepted 24-byte blocks, and every
transfer completed. **The air still only ever carried eight-byte packets.**
Each 24-byte block was fragmented into three, visible by identity because
`spike_b_drive.py` fills the second and third octets of a block with a
distinguishable marker:

```
burst#00/82 | D2 | burst+08/92 | C2 | burst+10/82 | D2 | burst#01/92 | ...
```

The sequence bit alternates across **on-air packets**, not across host
blocks - fifty-one alternations for a seventeen-block advanced burst.
Advanced burst is negotiated between the two ends and only the slave had it
enabled; `sim/` is an ordinary ANT+ sensor, so there was nothing to negotiate
with and the stack fragmented.

So: **no frame with a payload other than eight bytes has ever been on this
bench's air**, across either the two-radio or three-radio runs. `longframe`
counted 5, 1 and 2 frames in runs A, B and C, and all eight were ordinary bit
errors - each one is printed in the analyser's output with a control byte
one bit away from a value the run was already producing (`0x0E`, `0x0C` and
`0x1A` against `0x0A`).

---

## The timing numbers

All ADDRESS-interrupt to ADDRESS-interrupt, so both ends of every interval
carry one interrupt entry and the offset cancels. Software timestamps, not
(D)PPI, so every figure below carries interrupt-entry jitter; the offset
cancels in all of them, but the 1.55 ms turnaround in particular deserves a
hardware-captured measurement before a transmit deadline is designed around
it.

| Interval | Run A | Run B |
|---|---|---|
| Master broadcast -> slave's reply | n=7, **2186 us**, sd 8, range 2179-2195 | n=12, **2191 us**, sd 8, range 2181-2201 |
| Slave packet -> master's acknowledgement | n=33, **1567 us**, sd 21, range 1520-1599 | n=128, **1559 us**, sd 19, range 1515-1595 |
| Master's acknowledgement -> slave's next packet | n=26, **1546 us**, sd 26, range 1485-1592 | n=116, **1550 us**, sd 21, range 1488-1598 |

Three numbers `radiant_sched.c` and `radiant_burst.c` both need:

1. **A slave answers its master 2.19 ms after the master's ADDRESS**, which
   is about 2.08 ms after the master's frame ends. That is the reply
   turnaround the spike was asked for.
2. **A burst runs at one packet per ~3.11 ms**, as a strict data/ack
   alternation at ~1.55 ms each way. Eight payload bytes per 3.11 ms is about
   20.6 kbit/s of user data, which is what an ANT burst is worth on this
   link.
3. **The first packet of a burst costs the full 2.19 ms slot turnaround;
   every packet after it costs 1.55 ms.** The two are visibly different
   populations - sd 8 us against sd 21 us - and averaging them, as an earlier
   draft of the analyser did, produces a number that describes neither.
   `spike_b_analyse.py` reports them separately for that reason.

The master's slot period and jitter reproduce the two-radio runs' figures
below to the microsecond (run C's `dt` column: 249691 us).

## What a receiver does when a burst stops

From the failed run, and worth keeping because no successful run can show it.
The slave sent burst packet 0 and then nothing. The master answered `0xD2`
and then **retransmitted that identical frame - same payload, same CRC -
twenty more times at 3143 us**, spanning 66 ms, before giving up.

So a receiver in the middle of a burst re-sends its acknowledgement rather
than waiting quietly, and it does so at **3143 us** against the **3113 us** a
running burst actually takes per packet - near enough that it is plainly the
same timer and far enough apart, at 1 %, that it is worth recording as two
numbers rather than one. 21 attempts is what this stack does before
abandoning the transfer. `radiant_burst.c`'s receive path needs a retry limit
and this is a measured value for it, on one stack. `[measured, n=2]` - it
happened twice, in the two 16-block attempts of run 0, with 21 and 21
retransmissions.

---

## Was the capture complete? - the question that had to be answered first

A histogram loses a count when a frame is dropped; a sequence reads as an
encoding that skips a value instead, which is worse and harder to see. So the
sniffer stamps every line with `dr=`, its ring-drop counter as it stood when
that record was made, and `spike_b_analyse.py` fails the run if it ever
increments.

`dr = 0` on every line of every run. Peak ring occupancy was 29 of 512 in run
A and 88 of 512 in run B - the advanced-burst run, where 51 packets arrive
1.5 ms apart and the console needs about 10 ms a line. `printk` is
synchronous and cannot lose a line once it starts, so the ring is the only
place a frame can vanish, and it did not.

The `n=` counter is *not* the check: it counts every END event including
noise through the eight three-byte matchers, so gaps in the printed `n` are
expected. It is there to catch two captures concatenated by accident, which
the analyser also tests for.

---

## Consequences for `radiant`

1. **`PCNF0 = 0, STATLEN = 10` is mandatory for tracking, not optional.** The
   `LFLEN = 8 / CRCINC = 1` form is a broadcast-only receiver.
2. **`radiant_burst.c` can be written now.** The whole encoding is known: bit
   7 set, bit 6 clear on data and set on the acknowledgement, bit 5 on the
   final packet, bit 4 alternating per on-air packet, bit 3 set only if this
   frame opens the slot, bits 2:0 `010`. A transmitter alternates bit 4 and
   expects the acknowledgement to carry its complement.
3. **Acknowledged data is a one-packet burst**, on air and byte for byte.
   `radiant_ack.c` and `radiant_burst.c` should share one encoder rather than
   two, and a receiver that dispatches on the control byte cannot
   distinguish them - which means the serial layer's distinction between
   them is a serial-layer distinction only.
4. **The receive path must transmit.** A tracking channel that receives
   acknowledged data or a burst packet has 1.55 ms to put an acknowledgement
   on the air, and that acknowledgement is a full 8-byte frame carrying
   whatever is in the broadcast buffer. This is the tightest deadline in the
   link layer and it is what the direct radio backend's timer arming has to
   hit.
5. **`radiant_frame.h` needs the field, not just the byte.** A `ctrl_byte`
   that software has to mask by hand is how bit 3 gets forgotten.
6. **Nothing in the frame can carry a burst sequence above 1**, so any design
   that assumed a 3-bit on-air sequence - including the serial protocol's
   own 2-bit `ANTW_BURST_HEADER_SEQ_MASK` - is a serial-side concept that
   must be translated, not forwarded.
7. **A master's first transmission is one channel period after open** (see
   *Readings that stand from the two-radio runs* below) - `radiant_channel.c`
   should reproduce that.
8. **The receive path must not assume `0x0A`.** A tracking channel sees at
   least eleven values and must dispatch on the byte rather than validate it.

---

## Readings that stand from the two-radio runs

The two-radio runs (`docs/spike-b-results.md`'s original six) measured
everything above and inferred two things that the three-radio runs falsified
- both corrected in place above. Everything else those runs measured stands
and is not repeated in full here; the numbers that are not restated
elsewhere in this document are kept below.

**Master slot period and jitter**, from the `dt` column (ADDRESS-interrupt to
ADDRESS-interrupt between consecutive frames of the same master), over 744
intervals in six runs:

| Run | n | mean (us) | stdev (us) | peak-to-peak (us) |
|---|---|---|---|---|
| 1 | 168 | 249696.3 | 5.5 | 50 |
| 2 | 145 | 249696.3 | 6.5 | 50 |
| 3 | 120 | 249696.5 | 5.9 | 46 |
| 4 | 104 | 249696.3 | 5.4 | 48 |
| 5 | 103 | 249696.6 | 6.4 | 45 |
| 6 | 104 | 249696.3 | 6.0 | 34 |

Nominal is 8182 / 32768 s = **249694.8 us**. The measured mean is **+1.6 us,
i.e. +6.4 ppm** - two boards' crystals disagreeing, not the master drifting,
and the right order for two 50 ppm-rated parts. Slot jitter is at most 6 us
rms and 50 us peak-to-peak, and that figure is dominated by the software
timestamp measurement, not by the master: the true master jitter is
somewhere at or below 6 us rms. For `radiant_sched.c` the usable number is: a
real ANT master holds its slot to well inside +/-25 us over minutes, so a
receive window guard of tens of microseconds is about drift and clock error,
not about the master being sloppy.

**Master open-time**, from the host's `MESG_OPEN_CHANNEL` to the first
`EVENT_TX`, over eight runs: **250, 250, 250, 250, 250, 265, 266, 266 ms**
(the 16 ms quantisation is the Windows clock, not the radio) - **one channel
period (249.7 ms)** in every run. **A master does not transmit when you open
it. It transmits one channel period later.** Whether that period is a
collision probe or simple alignment to a slot boundary is not observable
over the air by any receiver; both produce this measurement.

**Acknowledged data, retry behaviour of the sender**: 18 acknowledged
messages were sent across the six two-radio runs (no slave present). **18
frames carrying `0xAA` appeared, one per message** - one on-air attempt per
acknowledged message, no retransmissions, failure (`EVENT_TRANSFER_TX_FAILED`)
arriving ~250 ms after the request in every case, one channel period. The
payload persists unchanged into the following broadcasts, because the
transmit buffer is not cleared by an acknowledged send.

**Two rows of `docs/ant-radio-link.md` closed for free:**

- **Eight logical addresses in one window.** `RXADDRESSES = 0xFF`, `BASE1 =
  BASE0`, the real `devnum_lo` at slot 5 and seven decoys. It works: 750
  CRC-valid frames across six runs, no degradation against the single-filter
  figures. The 32-set wildcard sweep rests on this measurement.
- **`RXMATCH` recovers `devnum_lo`.** `RADIO->RXMATCH` read **5** - the slot
  the real prefix was written into - on **every one of the 750 frames**, and
  5 was chosen precisely because slot 0 cannot distinguish a working
  `RXMATCH` from a stuck one. `devnum_lo` is recoverable by indexing the
  prefix table with `RXMATCH`.
- **The cost of eight filters**, so `radiant_search.c` can budget it: with
  the transmitter off, eight three-byte matchers produced **19, 22 and 27
  CRC failures per 15 s window** on this bench - of order 1.4 per second,
  against 3 to 5 per *60 s* window with one filter. Every one was rejected by
  the CRC and none ever decoded to a plausible channel ID. Rank and gate on
  CRC, never on match count.

---

## What remains untested

Named so their absence is visibly a decision.

1. **Any payload length other than eight bytes.** Advanced burst was enabled
   and fragmented; nothing on this bench has ever put a different length on
   the air. Bits 2:0 = `010` therefore still has no measured meaning, only a
   disproved one.
2. **A master-originated multi-packet burst with a real slave.** This is the
   one experiment that would turn bit 3 from `[inferred]` into `[measured]`,
   and it needs a scriptable ANT master, which `sim/` is not.
3. **Bit 5 on a broadcast.** Always 0, and no reason is known why it could
   not be something else.
4. **Whether an acknowledgement can carry a *different* payload** from the
   acknowledger's broadcast buffer. Every one observed carried the buffer.
5. **Retry behaviour of the data sender.** The receiver's 21 retransmissions
   were measured; what the *sender* does when an acknowledgement goes missing
   was not, because no acknowledgement went missing in a completed transfer.
6. **The nRF52840 as a receiver.** Everything measured across both sets of
   runs was received on an nRF54L15 or an nRF5340. One Feather flash, still
   unspent.
7. **Hardware ((D)PPI) timestamps**, so every microsecond figure here carries
   interrupt-entry jitter, and `radiant_radio_hal.h`'s `t_sync` calibration
   will need one anyway.
8. **Frames from any other vendor's ANT+ device.** `foreign` was zero
   throughout, on every run of both sets.
