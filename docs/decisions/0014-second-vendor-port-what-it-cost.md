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

The port is `radiant/src/radiant_radio_cc26xx.c`, on a
LAUNCHXL-CC26X2R1. It was written against the HAL contract and against the
measurements in `radiant/spike/ti_phy`, deliberately not against
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

### Sensitivity: NO VERDICT

`tools/ant_sens.py` walks the *transmitter's* power down until the receiver
misses 5 %, which should have made this measurable. It produced no figure, for
two reasons, both caught by the tool's own checks:

**The USB stick's dial did not move.** Across a 28 dB commanded ladder the
RSSI at the nRF receiver held flat (−59.0 dBm ±0.1), and flat on the TI
receiver too (−74/−75 throughout) — `FAIL dial: RSSI moved 0.00 dB per
commanded dB, not ~1.` Instrumenting `apply_power()` on an nRF54L15 with a
temporary `LOG_INF` confirmed the transmit path is correct end to end
(`RADIO.TXPOWER` moves through five distinct register values across the
ladder); the flat readings there are explained by both links being marginal
(25–57 % loss at every rung), which pins mean-RSSI-over-received-packets near
the detection threshold regardless of commanded power. That leaves the stick's
flat readings unexplained by link margin — see next.

**The CC26x2 port has no working transmit-power control.** `caps` advertised
`tx_power_min_dbm = -20 … tx_power_max_dbm = +5`, but `radiant_radio_tx()`
never read `req->power`: `setup_cmd.txPower` was the fixed `0x7217` `RF_open()`
consumed, so the backend has exactly one operating point and silently ignored
every commanded value. Fixed: `caps` now reports `min == max == 5`, and the arm
site documents why it ignores the field. A real range needs `RF_setTxPower()`
(available in `RFCC26X2.h`) plus a per-part `RF_TxPowerTable_Entry` table from
SmartRF Studio — not derivable from this tree, named future work rather than
guessed.

A fine ladder into the CC26x2 receiver — the only receiver on this bench whose
RSSI is known to move (−47.9 dBm in the A/B vs −74.5 dBm under the ladder, same
transmitter, same distance, because `ant_sim.py` never sets a transmit power
and `ant_sens.py` does) — held constant to ±0.15 dB across the full 48 dB
commanded span, confirming the stick's output is pinned regardless of what is
asked for. The stick cannot be reflashed from this bench (needs its DFU
button), and the nRF54L15 (which can be) does not have a strong enough link to
either receiver to carry a ladder. So `[gates.sensitivity]` has **no verdict
for either backend** — the instrument ran to completion and correctly refused
to interpolate a dB figure out of readings it could not justify.

One tooling defect fixed along the way: `MasterDriver` bound
`self._stop = threading.Event()` over `Thread._stop()`, a private *method*
`join()` calls — every ladder ended in `TypeError` inside `threading.py`'s
`finally` block, after results printed but before the JSON was written.
Renamed to `_stop_evt`; `tools/test_ant_sens.py` is 30/30.

**Still owed:** whether the CC26x2's `RF_setTxPower()` register write actually
moves (needs a logic probe on the RF core, or a receiver with more headroom);
an equal-power A/B, or a harness rule for which rig fields may legitimately
differ across vendors; sensitivity itself, blocked on both of the above;
`ack_data` (`tools/ant_session.py --attempts N` is confirmed working, but the
intended responder never transmitted a detectable signal on this bench, so the
100-attempt run measured an unrelated ambient sensor at 0/100 instead); and the
seeded timing constants (`T_SYNC_CAL_US`, `TX_RAMP_UP_US`, `RX_RAMP_UP_US`,
`RX_TO_TX_US`, `TX_TO_RX_US`), which want the wired two-board trigger the HAL's
`t_sync` section describes.

**Closed since the first sitting:** the transmit-power table
(`apply_power()` + `radiant_cc26xx_txp[]`); conformance against `sdk-ant`'s
baseline (byte-identical to a plain `radiant/nrf` build, so every difference
from `sdk-ant` is shared rather than port-specific —
`archive/benchmarks/2026-08-13-radiant-cc26xx.json`); coexistence design
([ADR 0015](0015-cc26xx-coexistence-design.md) — all four build arms compile,
still missing a live 802.15.4 peer to exercise the re-post branch); and the
rxBw/AGC re-sweep (`RX_BW_CODE` 98 confirmed a local optimum, both neighbours
several times worse; defaults unchanged).

## A note on the bench

Every `dslite` flash of the LaunchXL leaves the XDS110's ANT backchannel
(COM14) silent — zero bytes, LED blinking, JTAG healthy, which reads as a
firmware hang. It is not: `xds110reset.exe` clears it, and `scripts/flash_ti.ps1`
now runs that itself. A bare reset into an already-closed port does nothing,
which is why an earlier attempt concluded a physical replug was required.

A bare `JLink.exe -CommanderScript` without `exec DisableAutoUpdateFW` put the
nRF54L15 DK's onboard J-Link into its bootloader during this sitting.
`scripts/flash_sim_jlink.ps1` exists for exactly this and was not used;
recovery is a physical replug.
