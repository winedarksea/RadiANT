<!-- SPDX-License-Identifier: Apache-2.0 -->
# Spike B part 2 - the whole control byte

Checked by: `radiant/spike/promisc/spike_b_analyse.py`, which refuses to report
a capture it cannot show to be complete. The gate is run with the NCS toolchain's
Python interpreter (there is no system Python in this project):

```powershell
python radiant\spike\promisc\spike_b_analyse.py `
    archive\captures\radio\2026-08-09-spike-b2-runA-burst-seq.log `
    archive\captures\radio\2026-08-09-spike-b2-runB-burst-seq-advburst.log `
    archive\captures\radio\2026-08-09-spike-b2-runC-master-control.log --strict
```

and it exits 0. If this document and those logs disagree, the logs are right.

Provenance: `[measured]` throughout unless a line says otherwise. Measured on
this bench on 2026-08-09, with three radios on the air at once for the first
time. Nothing here derives from `sdk-ant`, from `libant.a`, or from an
non-redistributable ANT+ device profile document.

Part 1 is `docs/spike-b-results.md`. It established that byte 3 is a control
byte and measured three of its values. It is right about everything it measured
and wrong about two things it inferred; both corrections are below, and neither
was avoidable from a two-radio bench.

---

## Verdict

**The control byte is six independent fields, and there is no three-bit burst
sequence number in it.** The burst sequence is **one bit**, alternating - which
is why part 1, which could only ever see the first packet of a burst, could not
have found it and was right to refuse to guess.

**Eleven** values were observed across the four runs, and every one of them
decomposes. Nine is the runs-A-and-B figure - run C, the master control, is what
adds `0x8A` and `0xAA`:

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
| **2:0** | always `010` | 0 exceptions in 2,354 CRC-valid frames here, and part 1's 750 all had bits 4:0 = `01010` | no |

The byte sits at **offset 3 of the body** - equivalently, immediately before the
eight payload bytes, and the seventh byte of the frame after the preamble if you
count the two network-address bytes and `devnum_lo` as part of the frame, which
the nRF RADIO does not because they are its address field.

### The table the pass criterion asked for

Every row measured, in at least two independent runs unless the *Runs* column
says otherwise.

| On air | Direction | Meaning | Runs |
|---|---|---|---|
| `0x0A` | slot opener | **broadcast** | 0, A, B, C, and all 6 of part 1 |
| `0xAA` | slot opener | **acknowledged data**, and identically a **one-packet burst** | C, and all 6 of part 1 |
| `0x8A` | slot opener | burst packet, sequence bit 0, not the last | C, and all 6 of part 1 |
| `0x82` | in-slot | burst packet, sequence bit **0**, not the last | 0, A, B |
| `0x92` | in-slot | burst packet, sequence bit **1**, not the last | A, B |
| `0xA2` | in-slot | **acknowledged data**, and identically a one-packet burst; also burst-**last** with sequence bit 0 | 0, A, B |
| `0xB2` | in-slot | burst **last** packet, sequence bit 1 | A, B |
| `0xC2` | in-slot | **acknowledgement** of a non-final packet, next sequence bit 0 | A, B |
| `0xD2` | in-slot | **acknowledgement** of a non-final packet, next sequence bit 1 | 0, A, B |
| `0xE2` | in-slot | **acknowledgement of the final packet** - the transfer is complete - sequence bit 0 | A, B |
| `0xF2` | in-slot | acknowledgement of the final packet, sequence bit 1 | 0, A, B |

**Burst sequence 2 through 7 do not exist.** That is a positive statement, not an
absence: a seventeen-packet burst and a fifty-one-packet burst were both captured
end to end with a drop counter proving no frame was lost, and bit 4 alternated
`0,1,0,1,...` across every one of them while bits 7:5 stayed constant. There is
no field in the frame that could hold a number larger than one.

Bits 7:5 took the values `000`, `100`, `101`, `110`, `111` and **never** `001`,
`010` or `011`. `b7 = 0` only ever occurred with `b6 = b5 = 0`, which is to say
the only broadcast encoding is `0x0A`.

---

## The rig that could finally be built

| Role | Board | Image |
|---|---|---|
| Master | nRF54L15 DK | `sim/`, ANT+ bicycle power, device **14871** / type `0x0B` / transmission type 5, period 8182 |
| Slave | Adafruit Feather nRF52840 | the shipping dongle firmware, `0FCF:1009`, driven by `spike_b_drive.py --role slave`. **Zero Feather flashes were used**, as in Spike A and part 1 |
| Sniffer | nRF5340 DK **network core** | `radiant/spike/promisc`, console on the network core's VCOM |

Part 1 got two of these three working and stopped at the third. What it missed
is one sentence long.

### The nRF5340 network core prints perfectly well

Part 1's finding was that every GPIO on an nRF5340 belongs to the application
core until it writes `PIN_CNF[n].MCUSEL = NetworkMCU`, that the network core's
`uart0` is P1.00/P1.01, and that therefore the capture had nowhere to print.
All of that is true. The conclusion drawn from it - that the network core
"cannot print" - is not: **the application core hands the pins over in two
register writes**, and `radiant/spike/promisc/appcore` is those two writes plus
a heartbeat, 25 KB of flash and about eighty lines.

What actually blocked part 1 was not the silicon, it was that the application
core was believed to hold an image belonging to another project. It did not -
the board is a stock DK - and once that was settled the route took twenty
minutes. The RTT fallback that part 1's successor brief proposed exists
(`radiant/spike/promisc/net_rtt.conf`), builds clean - `build/promisc_net_rtt`,
22,988 B flash and 31,176 B RAM - and was **never needed and never flashed**. It
is kept because it is the only diagnostic that separates "the capture program is
not running" from "the pins were never granted", and those have completely
different fixes.

The companion grants **only** P1.00 and P1.01, unlike
`nrf/samples/nrf5340/empty_app_core`, which grants everything and then powers off
application-core RAM. Keeping the application core's own console alive is what
made the bring-up a reading rather than a guess: COM9 said `[appcore] alive 1
(FORCEOFF=0)` while COM10 was still silent, which named the fault immediately.

### The other thing part 1 could not have known

The first three-radio run still failed. Acknowledged data worked, and every
multi-block burst died after packet 0 with `EVENT_TRANSFER_TX_FAILED` -
precisely part 1's symptom, now with a peer present, which was supposed to be the
cure.

The cause is in this repository. `spike_b_drive.py` paced burst packets by
waiting for `EVENT_TRANSFER_NEXT_DATA_BLOCK`, which is the event
`src/ant_radio.h`'s buffer-ownership contract turns on. But
`src/ant_serial_bridge.c` **consumes that event and returns**, on the stated
grounds that a real ANT stick frames bursts itself and never shows the host its
internal flow control. The wait could not be satisfied by construction. The host
sat for its timeout, the transfer starved, and the radio sent one packet.

Back-pressure is not lost by streaming instead: the bridge blocks in
`k_sem_take(&burst_block_free, K_MSEC(1000))` before copying each block, so it
simply stops draining the endpoint. Seventeen packets now leave the host in under
0.1 ms and the transfer completes.

**So part 1's `a burst needs somebody to receive it` was necessary and not
sufficient.** Both were true at once, and a two-radio bench could not tell them
apart, because with no peer the transfer fails for the first reason before the
second one can show.

`archive/captures/radio/2026-08-09-spike-b2-run0-pacing-bug.log` keeps that
failure, because it contains a measurement nothing else does - see *What a
receiver does when a burst stops*.

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

Nine data packets, nine acknowledgements, strictly interleaved. The data packets
run `82 92 82 92 82 92 82 92 A2` - bit 4 alternates with the block index and
nothing else moves until the last packet sets bit 5. The seventeen-packet burst
in the same run does the same thing seventeen times and wraps eight times; the
fifty-one-packet advanced burst in run B does it fifty-one times.

If bits 7:5 were a three-bit sequence field, block 2 would differ from block 0.
It does not: they are the same byte, `0x82`.

### Bit 5, the last-packet flag

The cleanest pair is the two-packet burst, because it differs from the
nine-packet one in exactly one host-side decision:

```
page10/0A | burst#00/82 +2194 | page10/D2 +1583 | burst#01/B2 +1498 | page10/E2 +1584
```

Block 1 is `0xB2`, and `0xB2` is `0x92` with bit 5 set. Block 1 of the
nine-packet burst was `0x92`. Same sequence bit, same direction, same everything
except that this one is the last - and bit 5 is what changed.

### Bit 6, the acknowledgement flag, and the reply frame

**This is the frame part 1 listed as never observed.** It is not a short ack. It
is a full frame with the same channel ID and eight payload bytes, and the
acknowledger simply sends whatever is in its own broadcast buffer at that moment
- in every capture here the master's next bicycle-power page, which then goes out
again unchanged as the next scheduled broadcast one channel period later.

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

The acknowledgement's bit 4 is the **complement** of the packet it answers, and
its bit 5 is an echo, in **all 165** data/acknowledgement pairs across runs 0, A
and B, with no exceptions: `82 -> D2`, `92 -> C2`, `A2 -> F2`, `B2 -> E2`. Read as
"the sequence bit I expect next", which is what a
stop-and-wait protocol acknowledges with, that is exactly right; read as "an echo
of what I just received" it is exactly wrong. `[inferred]` - the arithmetic is
measured, the reading of it is not.

**Where 165 comes from, since a nearby number is easy to reach for.** Runs 0, A
and B carry **171** data packets between them. 165 are immediately followed by a
CRC-valid acknowledgement, and those 165 are the pairs the relation is asserted
on. Three more are followed by an acknowledgement that **failed CRC at the
sniffer** - visible in the analyser's `(ER)` column - and a frame the CRC
rejected is not evidence of anything, so those three are excluded rather than
counted; 165 + 3 = 168 is the number to *avoid* quoting as measured pairs. The
last three are followed by the next data packet, where the acknowledgement was
missed entirely, visible as a doubled ~3.08 ms gap where ~1.55 ms is expected.
165 + 3 + 3 = 171, and there is no counterexample anywhere in the 171.

### Bit 3, the slot-opening flag - and the control that pins it

Part 1 saw `0x0A`, `0x8A`, `0xAA` on 750 frames, all with bit 3 set. Part 2 sees
`0x02`-family values with bit 3 clear. The difference is not master versus slave,
and one pair of frames settles that without any argument:

```
page10/0A  ...  <- master, bit 3 SET
page10/F2  ...  <- master, 1.6 ms later, same slot, bit 3 CLEAR
```

Same transmitter, same channel, 1.6 ms apart. So bit 3 does not identify the
master.

Run C is the other half of the control and it is a strong one: **the same
Feather, the same stack, the same driver script and the same payload bytes**,
with only the channel role changed from slave to master. As a slave its
acknowledged data is `0xA2`; as a master it is `0xAA`. Its burst packet 0 is
`0x82` as a slave and `0x8A` as a master. The single differing bit is 3, and the
thing that changed is whether the frame opened the channel slot or followed
inside one.

`[inferred]`, and named as such: *slot opener* is the reading that fits every
frame in both parts. Falsify it by getting a **master** to send a multi-packet
burst to a real slave - under this reading its first packet carries bit 3 and
packets 1..N do not, and if instead all of them carry it, bit 3 means "sent by
the channel master" and the master's acknowledgement frames are the anomaly to
explain. That experiment needs a master this bench cannot script; `sim/` is a
bicycle-power sensor and never bursts.

### Bits 2:0, and the end of the length hypothesis

**Byte 3's low bits are not a length, and that is now measured rather than
argued.** Part 1 could only say that bits 4:0 held `01010` on every frame and
that nothing distinguished "a length" from "a constant that happens to be 10".
The way to settle it was supposed to be a frame with a payload other than eight
bytes. It turned out not to be needed:

- `0x0A` has bits 4:0 = `01010` = 10, with an eight-byte payload.
- `0xA2` has bits 4:0 = `00010` = 2, with an eight-byte payload.

Both CRC-valid at `STATLEN = 12`, which is only possible with exactly eight
payload bytes. A length field cannot read 10 and 2 for the same length. The two
bits that moved are bit 3 (slot) and bit 4 (sequence), both accounted for above,
and what is left is `010` in bits 2:0 on every frame ever captured in either
part. Its meaning is unknown. It is not a length, because it never changed while
the two bits above it did.

The prediction in `docs/ant-radio-link.md` that an advanced-burst frame shows
`0x9A` is therefore **not merely untested, it is built on a reading that is now
falsified**, and it should be withdrawn rather than restated.

### Advanced burst, again, and why it still proves nothing about length

Run B enabled advanced burst, the dongle accepted 24-byte blocks, and every
transfer completed. **The air still only ever carried eight-byte packets.** Each
24-byte block was fragmented into three, visible by identity because
`spike_b_drive.py` fills the second and third octets of a block with a
distinguishable marker:

```
burst#00/82 | D2 | burst+08/92 | C2 | burst+10/82 | D2 | burst#01/92 | ...
```

The sequence bit alternates across **on-air packets**, not across host blocks -
fifty-one alternations for a seventeen-block advanced burst. Advanced burst is
negotiated between the two ends and only the slave had it enabled; `sim/` is an
ordinary ANT+ sensor, so there was nothing to negotiate with and the stack
fragmented.

So: **no frame with a payload other than eight bytes has ever been on this
bench's air**, across both parts of this spike. `longframe` counted 5, 1 and 2
frames in runs A, B and C, and all eight were ordinary bit errors - each one is
printed in the analyser's output with a control byte one bit away from a value
the run was already producing (`0x0E`, `0x0C` and `0x1A` against `0x0A`).

---

## The timing numbers

All ADDRESS-interrupt to ADDRESS-interrupt, so both ends of every interval carry
one interrupt entry and the offset cancels. Software timestamps, not (D)PPI - see
part 1's note, which still applies.

| Interval | Run A | Run B |
|---|---|---|
| Master broadcast -> slave's reply | n=7, **2186 us**, sd 8, range 2179-2195 | n=12, **2191 us**, sd 8, range 2181-2201 |
| Slave packet -> master's acknowledgement | n=33, **1567 us**, sd 21, range 1520-1599 | n=128, **1559 us**, sd 19, range 1515-1595 |
| Master's acknowledgement -> slave's next packet | n=26, **1546 us**, sd 26, range 1485-1592 | n=116, **1550 us**, sd 21, range 1488-1598 |

Three numbers `radiant_sched.c` and `radiant_burst.c` both need:

1. **A slave answers its master 2.19 ms after the master's ADDRESS**, which is
   about 2.08 ms after the master's frame ends. That is the number part 1 called
   *not measured* and is the reply turnaround the spike was asked for.
2. **A burst runs at one packet per ~3.11 ms**, as a strict data/ack alternation
   at ~1.55 ms each way. Eight payload bytes per 3.11 ms is about 20.6 kbit/s of
   user data, which is what an ANT burst is worth on this link.
3. **The first packet of a burst costs the full 2.19 ms slot turnaround; every
   packet after it costs 1.55 ms.** The two are visibly different populations -
   sd 8 us against sd 21 us - and averaging them, as an earlier draft of the
   analyser did, produces a number that describes neither. `spike_b_analyse.py`
   reports them separately for that reason.

The master's slot period and jitter are unchanged from part 1 and were not
re-derived; run C's `dt` column reproduces 249691 us to the microsecond.

## What a receiver does when a burst stops

From the failed run, and worth keeping because no successful run can show it.
The slave sent burst packet 0 and then nothing. The master answered `0xD2` and
then **retransmitted that identical frame - same payload, same CRC - twenty more
times at 3143 us**, spanning 66 ms, before giving up.

So a receiver in the middle of a burst re-sends its acknowledgement rather than
waiting quietly, and it does so at **3143 us** against the **3113 us** a running
burst actually takes per packet - near enough that it is plainly the same timer
and far enough apart, at 1 %, that it is worth recording as two numbers rather
than one. 21 attempts is what this stack does
before abandoning the transfer. `radiant_burst.c`'s receive path needs a retry limit
and this is a measured value for it, on one stack. `[measured, n=2]` - it
happened twice, in the two 16-block attempts of run 0, with 21 and 21
retransmissions.

---

## Was the capture complete? - the question that had to be answered first

Part 1's result was a histogram, where a lost frame costs a count. This one is a
sequence, and a silently dropped frame in the middle of a burst reads as an
encoding that skips a value. So the sniffer now stamps every line with `dr=`,
its ring-drop counter as it stood when that record was made, and
`spike_b_analyse.py` fails the run if it ever increments.

`dr = 0` on every line of every run. Peak ring occupancy was 29 of 512 in run A
and 88 of 512 in run B - the advanced-burst run, where 51 packets arrive 1.5 ms
apart and the console needs about 10 ms a line. `printk` is synchronous and
cannot lose a line once it starts, so the ring is the only place a frame can
vanish, and it did not.

The `n=` counter is *not* the check: it counts every END event including noise
through the eight three-byte matchers, so gaps in the printed `n` are expected.
It is there to catch two captures concatenated by accident, which the analyser
also tests for.

---

## Consequences for `radiant`

1. **`radiant_burst.c` can be written now.** The whole encoding is known: bit 7 set,
   bit 6 clear on data and set on the acknowledgement, bit 5 on the final packet,
   bit 4 alternating per on-air packet, bit 3 set only if this frame opens the
   slot, bits 2:0 `010`. A transmitter alternates bit 4 and expects the
   acknowledgement to carry its complement.
2. **Acknowledged data is a one-packet burst**, on air and byte for byte.
   `radiant_ack.c` and `radiant_burst.c` should share one encoder rather than two, and a
   receiver that dispatches on the control byte cannot distinguish them - which
   means the serial layer's distinction between them is a serial-layer
   distinction only. That also retro-explains part 1's puzzle: a one-block burst
   produced `0xAA` not because the stack downgraded it to an acknowledged
   message, but because that *is* the encoding of "sequence 0, last packet".
3. **The receive path must transmit.** A tracking channel that receives
   acknowledged data or a burst packet has 1.55 ms to put an acknowledgement on
   the air, and that acknowledgement is a full 8-byte frame carrying whatever is
   in the broadcast buffer. This is the tightest deadline in the link layer and
   it is what the direct radio backend's timer arming has to hit.
4. **`radiant_frame.h` needs the field, not just the byte.** See the change request
   below: a `ctrl_byte` that software has to mask by hand is how bit 3 gets
   forgotten.
5. **Nothing in the frame can carry a burst sequence above 1**, so any design
   that assumed a 3-bit on-air sequence - including the serial protocol's own
   2-bit `ANTW_BURST_HEADER_SEQ_MASK` - is a serial-side concept that must be
   translated, not forwarded.

## Corrections to part 1

Both are inferences, both were reasonable, and neither survives.

- *"bits 7:5 are a three-bit type-and-sequence field ... the remaining values
  001, 011, 110, 111 are the plausible home for burst sequence and burst-last."*
  Bits 7:5 are three independent flags. `110` and `111` are the acknowledgement
  direction, `001`/`010`/`011` never occur, and the burst sequence lives in bit 4.
- *"Do not read `0xAA` as burst-last ... the `0xAA` says something about the
  stack's serial layer, not about the on-air burst encoding."* It says both. The
  serial-layer observation was correct - `EVENT_TRANSFER_TX_START` still does not
  fire for a one-block burst - but `0xAA` **is** the burst-last encoding for
  sequence bit 0 from a slot opener, and part 1's four one-block bursts were
  measuring the right thing for a reason it could not see.

Part 1's measured rows are all confirmed: `0x0A`, `0x8A` and `0xAA` reappeared
in run C from the same board, and `0x0A` from a different board in every run.

---

## The bench, as left

Stated because a spike that leaves a board in an undocumented state has spent
something it did not account for.

| Board | Core | Image now on it | Evidence it works |
|---|---|---|---|
| nRF54L15 DK (probe 1057737173) | app | `sim/`, unchanged - `build/sim_l15/merged.hex` | transmitting bicycle-power page 16 on COM7 at 4 Hz |
| nRF5340 DK (probe 1050006310) | app | Zephyr `samples/bluetooth/peripheral_hr`, NCS v3.2.4, sysbuild - `build/dk5340_restore/peripheral_hr/zephyr/zephyr.hex` | COM9: `Bluetooth initialized`, `Advertising successfully started`, LED blinking |
| nRF5340 DK | net | Zephyr `hci_ipc`, the standard Bluetooth controller sysbuild pairs with it - `build/dk5340_restore/hci_ipc/zephyr/zephyr.hex` | the application core's own log reports `HCI transport: IPC`, `Firmware: Standard Bluetooth controller`, and an identity address - which it could only get by talking to that controller |
| Feather nRF52840 | - | the shipping dongle firmware, **not reflashed** | enumerates as `0FCF:1009` and ran every driver script here |

**Nothing is left needing a human.** The nRF5340 DK is a working BLE peripheral
with a working controller, which is a stock-like arrangement and functionally
close to what part 1 recorded finding on it. The capture images
(`build/promisc_net2`, `build/promisc_appcore2`) are still on disk and the two
`flash_sim_jlink.ps1` invocations that put them back are in
`radiant/spike/promisc/README.md`, so the rig is twenty minutes from being
rebuilt, not a day.

The nRF54L15 was halted over SWD before run C to clear the channel. J-Link
resumed it when `JLink.exe` exited, which is why run C has two transmitters in
it. That is worth knowing before anyone plans a run that depends on silencing a
board: `h` followed by `q` does not leave a core halted.

## What remains untested

Named so their absence is visibly a decision.

1. **Any payload length other than eight bytes.** Advanced burst was enabled and
   fragmented; nothing on this bench has ever put a different length on the air.
   Bits 2:0 = `010` therefore still has no measured meaning, only a disproved one.
2. **A master-originated multi-packet burst with a real slave.** This is the one
   experiment that would turn bit 3 from `[inferred]` into `[measured]`, and it
   needs a scriptable ANT master, which `sim/` is not.
3. **Bit 5 on a broadcast.** Always 0, and no reason is known why it could not be
   something else.
4. **Whether an acknowledgement can carry a *different* payload** from the
   acknowledger's broadcast buffer. Every one observed carried the buffer.
5. **Retry behaviour of the data sender.** The receiver's 21 retransmissions were
   measured; what the *sender* does when an acknowledgement goes missing was not,
   because no acknowledgement went missing in a completed transfer.
6. **The nRF52840 as a receiver.** Everything in both parts of this spike was
   received on an nRF54L15 or an nRF5340. One Feather flash, still unspent.
7. **Hardware ((D)PPI) timestamps**, so every microsecond figure here carries
   interrupt-entry jitter. The differences are what is reported and the offset
   cancels in all of them, but the 1.55 ms turnaround in particular deserves a
   hardware-captured measurement before a transmit deadline is designed around it.
8. **Frames from any other vendor's ANT+ device.** `foreign` was zero throughout.
