# 0014 — A second vendor's silicon: what the HAL held, and what it did not

Date: 2026-08-13
Status: accepted

## Context

`radiant_radio_hal.h` was written to be portable and has been argued about at
length on the strength of one implementation and a reading of a second vendor's
headers. `docs/backends.md` §"EFR32: accommodated in the shape, not built" is
explicit that the EFR32 column was reasoned from RAIL's documentation and that
nothing had been compiled, let alone run.

This ADR records the first time the seam was actually tested: a working
backend, on a Texas Instruments CC2652R, receiving and transmitting real ANT+
through the unmodified core.

The port is `radiant_core/src/radiant_radio_cc26xx.c`, on a
LAUNCHXL-CC26X2R1. It was written against the HAL contract and against the
measurements in `radiant_core/spike/ti_phy`, deliberately not against
`radiant_radio_nrf.c` — the two radios disagree about nearly everything that
matters and a port-by-analogy would have inherited the wrong shape.

## Decision

Keep the HAL's shape. Add exactly one capability field. Fix one core policy.

## The seam held, and this is the evidence

Everything above `radiant_radio_hal.h` ran unchanged: the frame codec, the
scheduler, the channel state machines, the search policy, the event queue, the
transfer engine and the serial bridge. `tools/ant_probe.py`,
`tools/ant_scan.py`, `tools/ant_verify.py` and `tools/ant_session.py` were
pointed at the board without modification and behaved as they do against an
nRF. The eight-channel session — the one that mimics what Zwift actually does —
passes: 8/8 channels open simultaneously, 80 broadcasts, four sensors
identified, acknowledged data transmitted and burst bridged.

No `#ifdef` on a part number was added above the HAL. That was the claim, and
it survived.

Two things the HAL had anticipated correctly and that a lesser contract would
have broken on:

* **`addr_len_hw_max` being informational.** This part matches at most 4 bytes
  of address in hardware and the backend uses 3; the remaining bytes are
  compared in software. The HAL had already documented that a shorter hardware
  match is legal and costs spurious wakeups, so the boundary was already drawn
  in the right place.
* **`crc_in_hw` being a capability rather than an assumption.** The sync-word
  matcher consumes the address bytes before the CRC engine sees them and ANT's
  CRC covers them, so the CRC is computed in software here. The core did not
  care.

## What it cost: one new capability field

`caps.min_filter_hamming_bits`, and it is not a nicety.

The nRF matches addresses with a comparator. The CC26x2 matches them with a
**correlator**, which scores a sliding window against a template and fires
above a threshold. Two templates one bit apart score nearly equally against
every arriving frame, and the part resolves that by firing on neither.
Measured, against a transmitter at −47 dBm sending a known 4.0049 frames per
second, out of ~30 frames per point:

| separation | 1 bit | 2 bits | 4 bits | 8 bits | single word |
|---|---|---|---|---|---|
| frames received | **0** | 3 | 18 | 20 | 21–24 |

`radiant_search.c` paired `devnum_lo` `2k` with `2k+1` in set `k` — consecutive
integers, one bit apart, all 128 sets. That choice was free on the nRF and had
a good reason: it makes "which set would catch device X" a division, which the
seen cache and a named-device search both need. On this part it made the
receiver deaf for an entire sweep **and produced no error anywhere**: the
window armed, the receiver ran, the command reported an ordinary timeout, and
the scheduler calmly re-armed. Forever.

The fix is complement pairing — set `k` holds `k` and `k ^ 0xFF` — chosen
because it is 8 bits apart, because it is an involution so the 128 sets still
partition all 256 values exactly once with no duplicate filter, and because
`min(X, X ^ 0xFF)` keeps the lookup arithmetic rather than a table. It is gated
on the new capability, so the nRF's layout and its A/B baseline do not move.
Three tests in `test_search.c` pin all of it, including the default.

**The general lesson is worth more than the field.** A capability contract can
only express constraints somebody thought to ask about. This one is not
"how many addresses" or "how long an address" — both of which the HAL had — but
"how *different* must two addresses be", which nobody would ask having only
ever seen a comparator. A second vendor is how you find those.

## What it cost: PHY parameters cannot be inherited

Two of the settings copied from SmartRF's `tc901` override block were actively
harmful at ANT's PHY, and both failed silently:

* **The AGC reference level.** `tc901` sets `0x22`; TI's own default is `0x2E`.
  `tc901` is a 250 kbps PHY with a 530 kHz receive bandwidth, and at ANT's
  1 Mbps the borrowed value lost **better than half of every transmission** —
  37–47 % of frames received against 87–100 % once corrected.
* **The deviation.** `docs/ant-radio-link.md` records ~170 kHz and the backend
  was configured to expect it. 250 kHz measures at least as well everywhere and
  better in places; it is also the value the modulation index argues for. The
  earlier sweeps had been choosing a *narrower* filter to compensate for a
  deviation that was wrong.

## What it cost: a measurement discipline, learned the hard way

P1's gate reported **40 of 40 CRC-valid** and read as a clean pass. Its
denominator was *the frames it had already detected*. A PHY that silently drops
half of everything scores exactly the same as a perfect one on that metric —
and this one was dropping better than half.

Nothing measured against a known transmitted count until a paced transmitter
was put in front of it, at which point the gap was obvious in one run.

**A PHY gate needs a denominator the receiver does not choose.** Related: the
first two `rxBw`/`aaFilter` sweeps used whatever was transmitting in the room,
which measures the room; their winners disagreed with each other by more than
the margin either won by.

## What it cost: a scheduler assumption about window edges

The HAL defines a window as `[t_open, t_close]` in `t_sync` terms, so the
obvious end trigger is `t_close` plus one byte. That is correct arithmetic and
it measured **79 % loss**; `t_close + 500 µs` measured **8.3 %**.

The frames were never late — with the slop in place, 543 of 554 landed *inside*
the core's own window. What the arithmetic misses is that the end trigger stops
**sync search**, not merely packet reception: a ~400 µs tracked window with an
edge placed to the microsecond leaves the correlator no run-up. And a miss
there is not a lost packet, it is a lost *anchor* — the channel free-runs,
drifts, and misses the next four. That is a per-backend constant
(`RX_END_SLOP_US`), not a core change, but it is the kind of thing the HAL's
prose should warn about and now does.

## Consequences

**Accepted, and recorded rather than tuned:**

* **Acquisition is 4× slower.** `max_filters = 2` makes the wildcard sweep 128
  sets against the nRF's 32 — about 36 s to cover all 256 `devnum_lo` values
  against about 9 s. `docs/testing.md`'s acquisition gate fails by
  construction. This was predicted before any hardware ran and the instruction
  was to record it, not to fit the gate to it. That still holds.
* **Loss is 3.7 % against the nRF backend's 0.14 %**, measured in one A/B/A
  sitting — see the table below —
  against a 1.5 % gate. The residual is the
  part's own detection rate: continuous receive at −48 dBm measures 91–98 % of
  frames sent, and `nRxNok` was **zero in every row of every sweep** — every
  frame the correlator locked onto demodulated and passed its CRC. So this is
  sync detection, not demodulation, and closing it further means MCE/RFE work
  (custom radio-core RAM patches), which is out of scope here.
* **Timing is not the problem** — the TI part is measurably *better* at it.

### The A/B/A sitting, 2026-08-13

One transmitter, running continuously and never touched; receivers alternated.
Recorded as `archive/benchmarks/2026-08-13-radiant-nrf.json` and
`2026-08-13-radiant-cc26xx-ab.json`.

| leg | receiver | packets | loss | timing off-grid (radio) | mean RSSI |
|---|---|---|---|---|---|
| A1 | nRF54L15 DK, `nrf` backend | 706 | **0.139 %** | 0.0120 ms | −35.0 dBm |
| B | LAUNCHXL-CC26X2R1, `cc26xx` | 1156 | **3.744 %** | 0.0093 ms | −47.9 dBm |
| A2 | nRF54L15 DK, `nrf` backend | 663 | **0.150 %** | 0.0120 ms | −35.0 dBm |

**A1 and A2 agree to 0.011 percentage points.** That is the whole purpose of
the repeat and it says the difference measured is the backend, not the
afternoon.

* **`loss` gate: FAILS.** 3.74 % against a 1.5 % limit, and against 0.14 % for
  the backend it has to match. This is the real cost and it is not close.
* **`timing` gate: PASSES, and the TI part is the better of the two** — 0.0093 ms
  off the slot grid by the radio's own clock against 0.0120 ms. The RAT is
  4 MHz where the nRF's timer is 1 MHz, and it shows.
* **One caveat recorded rather than argued away:** the two receivers did not see
  the same signal. The L15 read −35 dBm and the LaunchXL −47.9 dBm on the same
  transmitter — about 13 dB, from board and antenna, not from the backend. Both
  are far above either part's sensitivity, and the spike measured the TI part
  losing frames at −47 dBm in *continuous* receive, so the loss is not an SNR
  artefact. But an equal-power comparison would be better and this was not one.

**`tools/ant_ab.py` refuses to gate this pair, and it is right to.** It compares
`rig` dicts wholesale and the receiver board genuinely differs. The harness was
designed for the single-vendor case the project had — same board, alternating
*firmware* — and a cross-vendor A/B cannot satisfy that by construction: you
cannot put the nRF backend on a CC2652R. Nothing was forced to make the tool
agree. If cross-vendor comparison is to be gated, the same-rig rule needs a
deliberate rule for which rig fields may differ, and that is a change to the
harness's contract rather than to a baseline file.

### Sensitivity: NO VERDICT, and the tool is the reason it is honest about that

`tools/ant_sens.py` removes the attenuator problem — it walks the *transmitter's*
power down until the receiver misses 5 %, which is the same attenuation applied
at the other end of the link. So this should have been measurable, and the
attempt was made properly. It produced no figure, for two separate reasons,
**both caught by the tool's own checks rather than by inspection**:

**One — with the USB stick as the transmitter, the dial is not connected.**
Across the full commanded ladder the received signal did not move:

| commanded | +8 | +4 | 0 | −4 | −12 | −20 |
|---|---|---|---|---|---|---|
| RSSI at the nRF receiver | −59.0 | −59.1 | −59.0 | −59.0 | −59.0 | −59.0 |

Flat to a tenth of a dB over 28 dB, and flat on the TI receiver too (−74/−75
throughout). Two independent receivers cannot be wrong in the same direction.
The rungs are *accepted* — `OK: tx power level 0x05 custom 0x00` — so the
message goes out and is answered. The tool's verdict:

> `FAIL dial: RSSI moved 0.00 dB per commanded dB, not ~1. The transmitter is
> not transmitting at the powers it was told to.`

That is the RSSI slope check this file's own header calls "not decoration",
doing exactly its job.

**Two — the dial is NOT broken, and an earlier draft of this ADR said it was.**
The obvious hypothesis was the third bullet in that header — *"the transmitter's
firmware predates the custom byte"* — and when a fine ladder (the `LVL_CUSTOM`
raw-register escape, +8 down to −40 dBm, fourteen rungs) and a second
transmitter built from this tree were *both* flat, the conclusion drawn was that
the shipping nRF transmit-power path was defective.

**That was wrong, and instrumenting the transmitter is what showed it.** A
temporary `LOG_INF` in `apply_power()` on an nRF54L15 driving a ladder recorded
five distinct register writes:

| commanded | +8 | +4 / 0 | −4 | −12 | −20 |
|---|---|---|---|---|---|
| `RADIO.TXPOWER` | `0x3F` | `0x28` | `0x0F` | `0x06` | `0x02` |

Those are the nRF54L15 encodings, reached through the part's own MDK symbols
exactly as `radiant_txp_table[]` intends. The value leaves the host, survives
the bridge, the channel store, `api_power_of()`, the scheduler and the arm, and
lands in the register. **The transmit path is correct end to end.** (The `+4`
and `0` rungs share a value because `CONFIG_ANT_DONGLE_TX_POWER_BOOST` folds
those two levels, which the tool's header already warns about.)

So the nRF54L15's dial is provably fine, and the flat readings on *that*
transmitter are explained by its links being marginal into either receiver
(25–57 % loss at every rung, non-monotonic): mean-RSSI-over-*received*-packets
pins near the detection threshold whatever the transmitter does, because those
are the only packets it can average.

That does not explain the stick, which is where the strong links are — see
below.

Two smaller things fell out and are worth keeping:

* **The nRF54L15's reported RSSI looks constant.** Exactly −59.0 dBm on all
  fourteen fine rungs, and exactly −35.0 dBm as the mean of both 700-packet A/B
  legs. The CC26x2's moves (−47.9 mean, −53 min, −47 max, and 13 dB between
  sittings), so this is not the transmitter. Not chased further, and not a
  conclusion — but a receiver that always reports the same number would defeat
  `ant_sens.py`'s dial check on every future ladder, so it is the first thing to
  check when this is picked up.
* **A stale comment.** `radiant_channel.c` says "radiant_sched.c clamps against
  caps.tx_power_max_dbm when it arms". No such clamp exists; `radiant_sched.c`
  carries `power` verbatim.

**One real defect fixed here.** `MasterDriver` subclasses `threading.Thread` and
bound `self._stop = threading.Event()` over `Thread._stop()`, which is a private
*method* that `join()` calls from `_wait_for_tstate_lock()`. Every ladder
therefore ended in `TypeError: 'Event' object is not callable` raised from
inside `threading.py` — in the `finally` block, after the measurements printed
but before the JSON was written, so the one artefact the run existed to produce
was always lost. Renamed to `_stop_evt`; `tools/test_ant_sens.py` is 30/30.

**And chasing the dial found a defect in this port.** `caps` advertised
`tx_power_min_dbm = -20 … tx_power_max_dbm = +5`, and `radiant_radio_tx()` never
read `req->power` at all: `setup_cmd.txPower` is the fixed `0x7217` that
`RF_open()` consumed, so the backend has exactly one operating point. A host
setting a transmit power was answered cheerfully and ignored, and the capability
query was the only thing claiming otherwise — the same shape of silent lie the
HAL's own comments keep warning about, in a field nobody had reason to exercise
until a tool tried to use this radio as an instrument.

`caps` now reports `min == max == 5`, so the core clamps to the one value that
exists, and the arm site says why it ignores the field rather than leaving it
looking forgotten. Making the range real needs `RF_setTxPower()` — which
`RFCC26X2.h` does provide — plus an `RF_TxPowerTable_Entry` table of raw
register values for this part, band and front end. Those come out of SmartRF
Studio and are not derivable from anything in this tree, so the table is named
future work rather than guessed.

**Three — the one measurement that isolates it.** The CC26x2 receiver's RSSI is
the only one on this bench known to move: it read −47.9 dBm in the A/B and
−74.5 dBm under the ladder, on **the same transmitter at the same distance**.
The difference between those two sittings is not the boards and not the room. It
is that `tools/ant_sim.py` never sets a transmit power and `tools/ant_sens.py`
does.

A fine ladder into that receiver then held **−74.4 to −74.7 dBm — constant to
±0.15 dB — across the full 48 dB commanded span**, +8 dBm down to −40 dBm, with
loss 29–58 % and non-monotonic.

So on the USB stick, *setting* a transmit power at all costs about 27 dB and
then pins the output regardless of what is asked for. The equivalent path on an
nRF54L15 built from this tree was instrumented at the register and is correct,
so this is specific to the image the stick is running — which is the hypothesis
this ADR started with, now resting on a measured 27 dB step rather than on a
plausible story about firmware age.

**And that is where it stops, because the stick cannot be reflashed from here.**
It is the only transmitter on the bench with a strong link to both receivers;
instrumenting or fixing it needs its DFU button. The nRF54L15, which can be
reflashed freely, does not have a strong enough link to either receiver to carry
a ladder.

So `[gates.sensitivity]` has **no verdict for any backend, and I could not
obtain one.** The instrument now runs to completion and writes its JSON, and it
refused to interpolate a dB figure out of readings it could not justify — which
is the outcome its own header argues for, and which is why the wrong diagnosis
above got caught rather than shipped.

**The lesson is the one this whole port keeps repeating.** "Two transmitters,
two code paths, three pairings, all flat" felt like overwhelming evidence for a
defect in the shipping transmit path, and an earlier draft of this ADR said so.
One `LOG_INF` at the register disproved it in ninety seconds and showed that one
of those two transmitters was simply on a link too marginal to measure anything.
Four ladders of black-box evidence pointed confidently at the wrong component;
a single white-box observation at the boundary settled it. When a measurement
disagrees with the code, instrument the boundary before rewriting the diagnosis.

**The CC26x2 cannot be the transmitter in that measurement either**, until the
power table exists; that is now recorded in `caps` rather than discovered again.

**Still owed:**

* **An equal-power A/B**, or a harness rule for which rig fields may legitimately
  differ across vendors.
* **Sensitivity**, once the transmit-power path works.
* **The seeded timing constants.** `T_SYNC_CAL_US`, `TX_RAMP_UP_US`,
  `RX_RAMP_UP_US`, `RX_TO_TX_US`, `TX_TO_RX_US` are seeded, marked as seeded in
  the source, and want the wired two-board trigger the HAL's `t_sync` section
  describes.

## A note on the bench

The nRF54L15 DK's onboard J-Link was put into its bootloader during this
sitting by running a bare `JLink.exe -CommanderScript` without
`exec DisableAutoUpdateFW`. `scripts/flash_sim_jlink.ps1` exists for exactly
this reason, documents exactly this failure, and was not used. Recovery is a
physical replug. Recorded here because the note already existed and was not
enough on its own.
