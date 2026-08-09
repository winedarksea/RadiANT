<!-- SPDX-License-Identifier: Apache-2.0 -->
# Spike B results - it is a control byte

> ## Superseded in part by `docs/spike-b-part2-results.md`
>
> **This is a primary record and is not rewritten.** Everything it *measured* is
> confirmed. Two things it *inferred* are now falsified, and each is annotated
> in place below with a `SUPERSEDED` note; nothing else in this file has moved.
>
> The two:
>
> 1. **Bits 7:5 are not a three-bit type-and-sequence field.** They are three
>    independent flags - exchange, acknowledgement, last - and the burst
>    sequence is a single bit, bit 4. See *The bit structure*.
> 2. **`0xAA` is burst-last.** This document says explicitly not to read it that
>    way, and the serial-layer observation behind that was correct; the
>    conclusion was not. See *The one-packet burst is not a burst*.
>
> Also gone: the length reading of bits 4:0, and with it the `0x9A`
> advanced-burst prediction this document restates. `0x0A` reads 10 there and
> `0xA2` reads 2, both with eight payload bytes.
>
> Part 2 also reached six of this document's *what remains untested* rows - the
> burst sequence, burst-last, the reply frame, the turnaround, the `(0,1)`
> combination and the nRF5340 as a capture platform.

Checked by: `radiant_core/spike/promisc` and the captures it produced,
`archive/captures/radio/2026-08-09-spike-b-nrf54l15-run{1,2,6}.log`,
`...-run3-bursts.log`, `...-run4-burst-lengths.log`, `...-run5-timed.log` and
`...-config.log`. Re-running the spike against a different transmitter requires
rebuilding it with a different `SPIKE_DEVNUM`; if this document and those logs
disagree, the logs are right.

Provenance: `[measured]` throughout unless stated. Measured on this bench on
2026-08-09. The register semantics referenced are `[nRF datasheet]`; the
prediction being tested is the `[inferred]` *Spike B gap* section of
`docs/ant-radio-link.md`. Nothing here derives from `sdk-ant`, from `libant.a`,
or from an adopter-gated ANT+ device profile document.

---

## Verdict

**The byte rtl_433 calls "length" is ANT's control byte. The guess was right.**

Three values were seen, with the payload staying **eight bytes** throughout:

| On air | Message | Frames | Runs |
|---|---|---|---|
| `0x0A` | broadcast | 718 | 6 of 6 |
| `0xAA` | acknowledged data | 22 | 6 of 6 |
| `0x8A` | burst data, first packet of a multi-packet burst | 10 | 6 of 6 |

The clinching observation is not the histogram, it is what happens **after** an
acknowledged message. An ANT master leaves the last payload it sent in its
broadcast buffer, so the eight bytes that went out as acknowledged data go out
again, unchanged, as broadcasts in the following slots:

```
P t=3460366761 dt= +249707 OK m=5 rssi=-17 dn=14871 ty=0B tt=5 B3=AA pl=A0005AA500112233 crc=B7DC air=87
P t=3460616456 dt= +249695 OK m=5 rssi=-17 dn=14871 ty=0B tt=5 B3=0A pl=A0005AA500112233 crc=AF92 air=87
P t=3460866150 dt= +249694 OK m=5 rssi=-17 dn=14871 ty=0B tt=5 B3=0A pl=A0005AA500112233 crc=AF92 air=89
```

Same device, same 8 payload bytes, same frame length, 249.7 ms apart - and byte
3 changes from `0xAA` to `0x0A`. **A length field cannot do that.** No timing
argument, no correlation across two clocks, and no appeal to the histogram is
needed; the two frames differ in exactly the byte under investigation and in
nothing else. That pair recurs 22 times across the six runs.

### What this costs, immediately, and it is not small

`docs/ant-radio-link.md`'s tracking configuration sets `PCNF0.LFLEN = 8` and
`PCNF0.CRCINC = 1`, which tells the nRF RADIO to read byte 3 **as a length**.
Spike A confirmed that works, because everything Spike A heard was a broadcast
carrying `0x0A`.

Point that receiver at an acknowledged frame and it reads `LENGTH = 0xAA = 170`,
overruns `MAXLEN`, and throws the packet away with a CRC error. **A backend
built from that table would receive broadcasts perfectly and drop every
acknowledged and every burst frame, silently**, and would look like a
sensitivity problem rather than a configuration error. Acknowledged data is how
Zwift sets trainer resistance, so the symptom would have been "ERG mode does not
work" discovered somewhere in Phase 5, months from here.

The fix is already in the document as an afterthought and needs promoting to the
rule: **`PCNF0 = 0` with `STATLEN = 10` is the correct receive and transmit
configuration, not an alternative to it.** It is identical on air - Spike A
measured 40/40 CRC-valid frames both ways - it puts byte 3 in RAM where software
can read it, and on transmit it lets software *choose* that byte, which is
exactly what `radiant_ack.c` and `radiant_burst.c` need to do. The `CRCINC` form is a
broadcast-only receiver and should be described as one.

### What is still open, and it is half the table

| On air | Message | Status |
|---|---|---|
| `0x0A` | broadcast | **measured** |
| `0xAA` | acknowledged | **measured** |
| `0x8A` | burst, sequence 0, not the last packet | **measured** |
| ? | burst, sequences 1..7 | **not observed** |
| ? | burst, last packet | **not observed** |
| ? | advanced burst / any payload length other than 8 | **not observed** |

The reason is one sentence: **a burst needs somebody to receive it, and this
bench could not put a master, a slave and a sniffer on the air at once.** See
*The rig that could not be built*. The pass criterion asked for the full table
across two runs; this is three of its rows across six.

---

## The rig

| Role | Board | Why |
|---|---|---|
| Capture | nRF54L15 DK, `nrf54l15dk/nrf54l15/cpuapp`, log on COM7 | J-Link, so it reflashes unattended; and Spike A already proved this exact promiscuous configuration works on this silicon, which removes every variable except the one under test |
| Transmitter | Adafruit Feather nRF52840, already running the shipping dongle firmware (`0FCF:1009`), driven as an ANT+ master by `radiant_core/spike/promisc/spike_b_drive.py` | **Zero Feather flashes were used**, as in Spike A |

Device under test: **#14871 (`0x3A17`), type `0x0B`, transmission type 5, period
8182** - the same transmitter Spike A characterised, so its channel ID is
independently known rather than assumed.

Every payload the host sent is marked in byte 0 - `B0` broadcast, `A0`
acknowledged, `C0` burst - with the host's own sequence number in byte 1. So a
captured frame maps back to a line of the driver's output by identity, not by
timing. That is what makes single-frame results usable: there is exactly one
`0x8A` frame per burst, and its payload says which burst it was.

### The radio configuration, as programmed

From `archive/captures/radio/2026-08-09-spike-b-nrf54l15-config.log`:

```
[cfg] BALEN=2 BASE0=BASE1=0xA3650000 PREFIX0=0x22CC4488 PREFIX1=0x11EEE8AA
[cfg] RXADDRESSES=0xFF PCNF0=0x00000000 PCNF1=0x01020C1C CRCCNF=0x00000002
[cfg] on-air prefixes (slot:byte): 0:11 1:22 2:33 3:44 4:55 5:17* 6:77 7:88
```

This is Spike A's *search* configuration - `PCNF0 = 0`, `BALEN = 2`,
`STATLEN = 12`, the `BASE0 = ... << 8 * (4 - BALEN)` packing rule - widened to
all eight logical addresses. `MAXLEN` is 28 here against Spike A's 60 purely
because this program's buffer is smaller.

Using the search configuration rather than the tracking one was the single
design decision that had to be right in advance, and the reason is the section
above: a receiver that parses byte 3 as a length cannot report that byte 3 was
not a length. It would have thrown away every frame that mattered.

---

## Every value, and how it was provoked

### Broadcast - `0x0A`

718 frames over six runs, every one CRC-valid, from a master transmitting at
4.005 Hz. Nothing new against Spike A except that the byte is now known to be
constant *because the message type is constant*, not because the byte cannot
vary.

### Acknowledged - `0xAA`

18 acknowledged messages were sent (`MESG_ACKNOWLEDGED_DATA`, 0x4F) across the
six runs. **18 frames carrying `0xAA` appeared, one per message.** The stack then
reported `EVENT_TRANSFER_TX_FAILED` for every one, correctly - there was no
slave to acknowledge them.

Two facts fall out that `radiant_ack.c` needs:

- **One on-air attempt per acknowledged message.** 18 requests, 18 frames, no
  retransmissions. This stack, as a master with no peer, does not retry an
  acknowledged message on air before failing it. The failure arrived ~250 ms
  after the request in every case, which is one channel period.
- **The payload persists into the following broadcasts.** The buffer is not
  cleared by the acknowledged transmission; see the three-line capture above.

### Burst, first packet - `0x8A`

Ten multi-packet bursts were requested. **Each put exactly one frame on the air,
carrying `0x8A` and the first eight payload bytes**, followed by
`EVENT_TRANSFER_TX_START` and then `EVENT_TRANSFER_TX_FAILED`. A burst is
link-acknowledged and there was no peer, so it stopped after the first packet.
Requesting 2, 3 or 12 blocks made no difference to what reached the air
(`...-run4-burst-lengths.log`).

### The one-packet burst is not a burst - `0xAA`

A burst of a single block, i.e. sequence 0 with the last-packet flag set, was
sent four times. It produced `0xAA`, not a fourth distinct value.

**Do not read that as "burst-last is encoded as `0xAA`".** The stack raised
`EVENT_TRANSFER_TX_START` for the multi-block bursts and **not** for the
one-block ones - three of four in run 3, three of four in run 4, one of two in
runs 5 and 6. A one-packet burst is being turned into a single acknowledged
transmission before it reaches the radio, so the `0xAA` says something about the
stack's serial layer, not about the on-air burst encoding. That is why the
burst-last row above is marked *not observed* rather than filled in with a value
that six runs appear to support. `[inferred]`

> **SUPERSEDED - `docs/spike-b-part2-results.md`.** It says both, and the
> paragraph above is half right in a way that could not be told apart from here.
> The serial-layer observation stands: `EVENT_TRANSFER_TX_START` still does not
> fire for a one-block burst. But **`0xAA` *is* the burst-last encoding** -
> exchange set, acknowledgement clear, last set, sequence bit 0, slot bit set -
> and acknowledged data is byte-for-byte a one-packet burst on air. These four
> one-block bursts were measuring the right thing for a reason two radios could
> not see. Part 2's run C is the control: the same board, stack, script and
> payload with only the channel role changed moved `0xA2` to `0xAA`, one bit.

### Advanced burst - not achieved

Advanced burst was enabled successfully (`MESG_CONFIG_ADV_BURST`, 24-byte
packets accepted) and 24-byte blocks were sent. **They did not reach the air as
24-byte frames.** The frames that appeared were CRC-valid at `STATLEN = 12`,
which can only happen with an 8-byte payload, and carried only the first eight
bytes of the block. With no peer to negotiate an advanced-burst size with, the
stack fragmented to the standard 8-byte packet.

So the payload length never moved, and the falsifiable prediction in
`docs/ant-radio-link.md` - that an advanced-burst frame shows `0x1A` - **remains
untested.** The capture would have caught it if it had happened: a frame with a
different payload length reads its header correctly and then fails its CRC,
which `radiant_core/spike/promisc` counts and prints separately as `longframe`.
**`longframe` was zero in all six runs**, which is the positive statement that no
frame of a non-standard length was ever on the air.

The prediction is also now stated wrongly and should be restated. If the low
bits of the control byte are a length in the ShockBurst sense, a 24-byte
advanced-burst packet would carry `24 + 2 = 26 = 0b11010` in **bits 4:0**, with
the message-type bits still set to whatever a burst uses - i.e. `0x9A`, not
`0x1A`. `0x1A` would require the type bits to be zero, which is the broadcast
encoding. See below.

---

## The bit structure - where the evidence stops

The three observed values, in binary:

```
broadcast              0x0A   0000 1010
burst, not last        0x8A   1000 1010
acknowledged           0xAA   1010 1010
```

**Measured**, over all 750 CRC-valid frames of all six runs:

- **Bits 4:0 held `0b01010` = 10 on every frame.** Ten is 8 payload + 2 CRC.
- **Bit 6 was zero on every frame.**
- **Bits 7 and 5 carry the message type**: `(0,0)` broadcast, `(1,0)` burst
  non-final, `(1,1)` acknowledged. `(0,1)` was never seen.

**Inferred, and it is inference** `[inferred]`: the natural reading is that
**bits 4:0 remain a length** - the ShockBurst convention Spike A confirmed, which
also explains why the value is 10 rather than 8 - and that **bits 7:5 are a
three-bit type-and-sequence field**. That reading is consistent with everything
above and with `PCNF0.CRCINC` receiving broadcasts correctly, but it is not
tested, for one blunt reason: **the payload length never changed, so nothing
distinguishes "bits 4:0 are a length" from "bits 4:0 are a constant that happens
to equal 10".** Falsify it by getting any frame with a payload other than 8 bytes
on the air and reading bits 4:0.

A second inference, weaker and named so it is not mistaken for a finding: with
three of eight possible values of bits 7:5 used, and burst sequence numbers
needing somewhere to live, the remaining values `001`, `011`, `110`, `111` are
the plausible home for burst sequence and burst-last. Nothing here tests that.
Do not build `radiant_burst.c` on it.

> **SUPERSEDED - `docs/spike-b-part2-results.md`.** Both inferences in this
> section are falsified, and the caution attached to them was right.
>
> - **Bits 4:0 are not a length.** `0x0A` reads 10 there and `0xA2` reads 2, and
>   both carry eight payload bytes. The two bits that move are bit 3 (slot
>   opener) and bit 4 (sequence); bits 2:0 are `010` on every frame ever
>   captured and their meaning is unknown. The `0x9A` advanced-burst prediction
>   restated in *Advanced burst - not achieved* above rests on this reading and
>   is **withdrawn**, not corrected.
> - **Bits 7:5 are not a three-bit type-and-sequence field.** They are three
>   independent flags: b7 exchange, b6 acknowledgement, b5 last. `110` and `111`
>   are the acknowledgement direction, `001`/`010`/`011` never occur, and **the
>   burst sequence is one bit - bit 4 - so sequences 2 through 7 do not exist.**
>   A 17-packet and a 51-packet burst were captured end to end with no drops and
>   bit 4 alternated on every packet.
>
> The instruction not to build `radiant_burst.c` on the guess was correct, and the
> guess was wrong. `radiant_burst.c` can be written now, from part 2.

**What `radiant_core` may rely on today**: `0x0A` broadcast, `0xAA` acknowledged,
`0x8A` for a burst packet that is not the last. Anything about sequence numbers
or the final packet of a burst is unknown, and a table of three rows is enough to
implement broadcast and acknowledged data - which is the whole of trainer
control - and not enough to implement burst.

---

## The rig that could not be built, and exactly what stopped it

A multi-packet burst needs a receiver, so the experiment needs three radios at
once: a master, a slave, and a sniffer. The bench has three boards and only two
of them can be an ANT endpoint.

- **Feather nRF52840** - ANT endpoint (the shipping dongle firmware). No flash
  available; none was needed.
- **nRF54L15 DK** - ANT endpoint *or* sniffer, not both.
- **nRF5340 DK** - sniffer only. It cannot be an ANT endpoint: the shipping
  firmware's radio backend does not run on it (that is the `CONFIG_ANT_NP_HOST`
  dual-core path the plan drops from v1).

So the nRF5340 had to be the sniffer. The plan's objection to the nRF5340 does
not apply to a bare capture app - it needs no RPC subsystem - and that part was
right: **`radiant_core/spike/promisc` builds and programs cleanly for
`nrf5340dk/nrf5340/cpunet`** (23,216 B flash, 22,800 B RAM), and J-Link
programmed the network core at 122 KB/s with no complaint. The image is at
`build/promisc_net/merged.hex`.

**It cannot print.** Two things stand between the network core and its console,
and the second one is the blocker:

1. The network core is held in reset by `NETWORK.FORCEOFF` until an
   application-core image releases it. That turned out to be already satisfied -
   J-Link found the network core running, so whatever is on the application core
   already releases it.
2. **Every GPIO on an nRF5340 belongs to the application core until the
   application core hands it over** (`P1.PIN_CNF[n].MCUSEL = NetworkMCU`). The
   network core's console is `uart0` on P1.00/P1.01, wired to the DK's second
   VCOM. Nothing grants those pins, so the console has nowhere to go. COM10 is
   silent; that is not a bug in the capture app.

Zephyr's answer is `nrf/samples/nrf5340/empty_app_core`, which both releases the
network core and grants it every pin. It was built (`build/promisc_appcore/`) and
**not flashed**, because of what the application core turned out to be running.

### The application core belongs to a different project

COM9 carries a Zephyr shell (`dk:~$`) from an application with log modules
`st_health` and `st_ble_hr` - a BLE heart-rate peripheral - which had been up for
about four hours. It is **not** this repository: an exhaustive search of the
machine outside `C:/Users/Colin/sdk-ant/` (which the clean-room policy forbids
opening) found no source containing those module names and no build tree that
could have produced that image. The only two applications on the machine are
`ant_dongle` and `zephyr_aerosense`, whose modules are `ant_*` and `aero_*`.

Overwriting the application core would therefore have destroyed an image with no
recovery path on this machine. That was not done.

The alternative - leaving the application core alone and writing
`P1.PIN_CNF[0..1].MCUSEL` live over SWD, a two-word poke that reverts on the next
reset - **was refused by this environment's command sandbox**, twice. That is a
reasonable guardrail on a board running someone else's firmware, and it is where
the route ended.

### What was damaged, and what has to happen about it

**The nRF5340 DK's network core was reprogrammed before any of this was known,
and its previous contents are gone.** A BLE application-core image implies a
Bluetooth controller on the network core - `hci_ipc` or NCS's `ipc_radio` - and
that is what was overwritten with the capture app. It cannot be recovered
byte-for-byte; it existed only in that board's flash.

A functionally standard replacement has been **built but not flashed**, because
flashing the nRF5340 was also refused by the sandbox:

```
build/ipc_radio_net/merged.hex
  nrf/applications/ipc_radio, -DEXTRA_CONF_FILE=overlay-bt_hci_ipc.conf,
  board nrf5340dk/nrf5340/cpunet, NCS v3.2.4
```

That is what NCS sysbuild produces as the network-core image for a BLE
application-core app, so it has a good chance of restoring the `st_*`
application's Bluetooth. It is a replacement, not the original, and the original
may have carried a non-default configuration.

**Action required by a human**, and it is the one thing in this document that
does not wait:

```powershell
.\scripts\flash_sim_jlink.ps1 -HexPath build\ipc_radio_net\merged.hex `
    -JLinkExe <shim injecting -SelectEmuBySN 1050006310> -Device nRF5340_xxAA_NET
```

Until that runs, the nRF5340 DK's network core holds the Spike B sniffer and the
board's Bluetooth does not work.

### Was there a two-board arrangement that would have done better?

No, and the reason is structural rather than a matter of effort. The missing rows
all require a burst that runs past its first packet, a burst is
link-acknowledged, and an acknowledgement requires a second ANT endpoint. Two
endpoints plus a sniffer is three radios. The only way around it is to make the
sniffer itself acknowledge - which requires knowing what an ANT reply frame looks
like, and that is the question, not the method.

---

## The three timing numbers

### Master slot period and jitter - **measured**

From the `dt` column, which is ADDRESS-interrupt to ADDRESS-interrupt between
consecutive frames of the same master, over 744 intervals in six runs:

| Run | n | mean (us) | stdev (us) | peak-to-peak (us) |
|---|---|---|---|---|
| 1 | 168 | 249696.3 | 5.5 | 50 |
| 2 | 145 | 249696.3 | 6.5 | 50 |
| 3 | 120 | 249696.5 | 5.9 | 46 |
| 4 | 104 | 249696.3 | 5.4 | 48 |
| 5 | 103 | 249696.6 | 6.4 | 45 |
| 6 | 104 | 249696.3 | 6.0 | 34 |

Nominal is 8182 / 32768 s = **249694.8 us**. The measured mean is **+1.6 us,
i.e. +6.4 ppm** - that is the two boards' crystals disagreeing, not the master
drifting, and it is the right order for two 50 ppm-rated parts.

**Slot jitter is at most 6 us rms and 50 us peak-to-peak, and that figure is
dominated by the measurement, not by the master.** Timestamps are captured in
software at the top of the interrupt handler, so both ends of an interval carry
one interrupt entry. The true master jitter is somewhere at or below 6 us rms.
Measuring it properly needs (D)PPI hardware capture, which this spike does not
do and which `radiant_radio_hal.h`'s `t_sync` calibration will need anyway.

For `radiant_sched.c` the usable number is: **a real ANT master holds its slot to
well inside +/-25 us over minutes, so a receive window guard of tens of
microseconds is about drift and clock error, not about the master being
sloppy.**

### Master open-time - **measured, with a stated ambiguity**

Time from the host's `MESG_OPEN_CHANNEL` to the first `EVENT_TX`, over eight
runs: **250, 250, 250, 250, 250, 265, 266, 266 ms.** The 16 ms quantisation is
the Windows clock, not the radio. That is **one channel period (249.7 ms)** in
every run.

Cross-checked against the host-timestamped capture (run 5) two ways. The
correlation between the host clock and the capture clock is consistent with the
first frame appearing one period after `OPEN_CHANNEL`. And the payload sequence
settles it independently: every `EVENT_TX` produces exactly one new payload on
the air one slot later, with no repeats, which is only consistent with
`EVENT_TX` being raised immediately after each transmission - so the first
`EVENT_TX` arriving at +250 ms means the first *transmission* was at +250 ms.

**A master does not transmit when you open it. It transmits one channel period
later.** `radiant_channel.c` should reproduce that, and a Zwift-style host that
expects data immediately after opening a master channel is going to wait a slot.

The ambiguity, and it is not resolvable from outside: **a sniffer cannot see a
receive window.** Whether that period is a collision probe (the master listening
before it talks) or simply alignment to the next slot boundary of an internal
timebase is not observable over the air by any receiver. Both produce exactly
this measurement. Distinguishing them needs the master's own instrumentation.

### Slave-to-master reply turnaround - **not measured**

This is the number that needed the third radio. There was no slave, so no reply
was ever transmitted. `spike_b_drive.py --role slave` exists, is written for
exactly this, and has never been run.

---

## Two rows of `docs/ant-radio-link.md` closed for free

Both were on Spike A's *still untested* list, and this spike exercised them
because it needed them anyway.

- **Eight logical addresses in one window.** `RXADDRESSES = 0xFF`, `BASE1 =
  BASE0`, the real `devnum_lo` at slot 5 and seven decoys. It works: 750
  CRC-valid frames across six runs, no degradation against Spike A's
  single-filter figures. The 32-set wildcard sweep rests on this and it now
  rests on a measurement.
- **`RXMATCH` recovers `devnum_lo`.** `RADIO->RXMATCH` read **5** - the slot the
  real prefix was written into - on **every one of the 750 frames**, and 5 was
  chosen precisely because slot 0 cannot distinguish a working `RXMATCH` from a
  stuck one. `devnum_lo` is recoverable by indexing the prefix table with
  `RXMATCH`, which is what `radiant_search.c` will do.

**The cost of eight filters, so `radiant_search.c` can budget it**: with the
transmitter off, eight three-byte matchers produced **19, 22 and 27 CRC failures
per 15 s window** on this bench - of order 1.4 per second. Spike A saw 3 to 5 per
*60 s* window with one filter. Every one was rejected by the CRC and none ever
decoded to a plausible channel ID. Rank and gate on CRC, never on match count -
Spike A's advice, now with eight times the reason.

---

## Consequences for `radiant_core`

1. **`PCNF0 = 0, STATLEN = 10` is mandatory for tracking, not optional.** The
   `LFLEN = 8 / CRCINC = 1` form is a broadcast-only receiver. This is the
   highest-value line in this document.
2. **Rename the field everywhere.** It is a control byte. `radiant_frame.h`'s
   `len_byte` becomes `ctrl_byte`; the cross-check against `payload_len` becomes
   a check on bits 4:0 only.
3. **`radiant_ack.c` can be written now.** Set `0xAA`, transmit in the slot, expect
   a reply. What the reply looks like is still unknown.
4. **`radiant_burst.c` cannot.** Three of the four things it needs to encode are
   unmeasured. Write the first-packet case, leave the sequence encoding behind a
   clearly marked gap, and get a three-radio rig before finishing it.
5. **`radiant_channel.c`: a master's first transmission is one period after open.**
6. **The receive path must not assume `0x0A`.** A tracking channel will now see
   at least three values and must dispatch on the byte rather than validate it.

---

## What remains untested

Named so their absence is visibly a decision.

1. **Burst sequence numbers and the last-packet encoding.** The headline gap.
   Needs three radios.
2. **Slave-to-master reply turnaround.** Same cause.
3. **Any payload length other than 8 bytes**, and therefore whether bits 4:0 are
   really a length. Advanced burst was enabled and still produced 8-byte frames.
4. **The reply frame itself** - what a slave sends back to acknowledge. Never
   observed, because no slave ever transmitted.
5. **The `(0,1)` combination of bits 7 and 5**, and bit 6 being anything but
   zero. Never seen; no reason to think they are unused.
6. **The nRF52840, again.** Everything here is nRF54L15, like Spike A. One
   Feather flash, still unspent.
7. **Hardware ((D)PPI) timestamps**, and therefore a real jitter figure and the
   `t_sync` calibration constant.
8. **A slave-side capture.** `spike_b_drive.py --role slave` is written and has
   never been run.
9. **The nRF5340 network core as a capture platform.** Builds, programs, cannot
   print. See above.
10. **Frames from any other ANT+ device.** `foreign` was zero throughout - the
    bench was otherwise quiet - so nothing here says what a real sensor from
    another vendor puts in that byte.
