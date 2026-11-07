# Device-number-dependent receive loss (TODO.md item 1), 2026-11-07

Plan: `~/.claude/plans/our-testing-found-a-validated-church.md`. Rig in every
window: stock-RadiANT nRF52840 Dongle (`3D55F77818BE772A`) transmits via
`ant_sim` (except Phase 1); receivers are the Garmin USB-m (`006`, reference),
the stock Feather (`755972D7183A6183`, control), the 54L15 DK (COM4, log COM3)
and the 5340 DK net core (COM5). `run_window.ps1` / `run_sweep.ps1` drive it,
`summarize.py` and `analyze_sweep.py` read it. Loss below is
RX_FAIL / (packets + RX_FAIL).

The per-window directories (`p1_*` ... `p5aba_*`: every receiver's JSON and
log, the 54L15's log VCOM) are gitignored and stay on the bench PC.
`windows.txt` holds what they show: each window's per-receiver counts with
Wilson intervals, and every raw `rxdiag` capture quoted below.

## Result in one paragraph

The nRF RADIO in `Nrf_1Mbit` mode **drops one bit** when a long alternating
run (0101...) sits inside the address it is matching, or within about a byte
after the end of that address. If the slip lands inside the hardware address
the frame never matches (a "silent" window). If it lands just after, the frame
arrives intact apart from that one missing bit. The Garmin (a different
demodulator) is unaffected. Shortening the hardware match (ADR-0019 idea, N=2)
fixes numbers whose run is far enough from `[A6 C5]` (43634) but **cannot fix
#54100**, whose run touches `C5`'s trailing `0101`. The slipped frames are
recoverable: re-inserting the one bit gives a CRC-valid frame (8/8 tested).

**Fix (Phase 5, ADR 0019):** addresses with a run of 9 or more bits get the
2-byte match plus a CRC-verified one-bit repair; all others keep the full
match. A/B/A on #54100 with the Garmin receiving: 2.08 → 0.17 → 2.33 %, against
the Garmin's 0.17/0.17/0.33. A signal-dependent residual at -50..-60 dBm is
still open.

## Phase 1: Garmin as transmitter (300 s)

| dev # | Feather | Dongle | 54L15 | 5340 |
|---|---|---|---|---|
| 54100 | 1.91 | 2.08 | 1.66 | 1.83 |
| 54900 | 0.00 | 0.00 | 0.00 | 0.08 |

Not RadiANT's transmitter. Receiver-side.

## Phase 2: classify the misses (54L15 diag build, 300 s)

#54100: 1203 tracked windows, 21 missed, **all 21 `silent`** (no ADDRESS, no
END), 0 CRC failures, 0 cut off; host RX_FAIL = 21 exactly. #54900: 1203 more
windows, 0 new silent. Frame offset in window 104..419 us either way.

## Phase 3: sweep (120 s, 5-byte stock match)

| dev # | alt run 5B/3B | 5340 | 54L15 | Feather | Garmin |
|---|---|---|---|---|---|
| 21845 | 21/12 | 3.53 | 2.71 | 2.90 | 0.00 |
| 54101 | 12/12 | 1.88 | 2.50 | 2.07 | 0.21 |
| 54100 | 11/11 | 4.79 | 2.29 | 3.53 | 0.00 |
| 43634 | 11/6  | 2.29 | 2.91 | 3.12 | 0.00 |
| 54102 | 10/10 | 2.49 | 2.49 | 3.12 | 0.00 |
| 54096 | 9/9   | 2.08 | 3.12 | 1.24 | 0.21 |
| 54186 | 9/8   | 1.04 | 0.83 | 0.00 | 0.21 |
| 54058 | 8/7   | 0.00 | 0.21 | 0.00 | 0.00 |
| 54080 | 7/7   | 0.00 | 0.00 | 0.00 | 0.83 |
| 54132 | 6/6   | 0.00 | 0.00 | 0.00 | 0.62 |

Threshold ~9 alternating bits inside the 5-byte address. (The `silent` column
`analyze_sweep.py` prints is contaminated by the tail after `ant_verify` exits;
use RX_FAIL.)

## Phase 4: hardware address length (120 s; DKs only, Feather stock)

| N | dev # | 5340 | 54L15 | Feather (5B) | Garmin |
|---|---|---|---|---|---|
| 5 | 54100 | 4.16 | 2.49 | 2.29 | 0.00 |
| 5 | 43634 | 3.53 | 2.49 | 3.12 | 0.00 |
| 5 | 54132 | 0.00 | 0.00 | 0.21 | 0.42 |
| 3 | 54100 | 2.29 | 2.49 | 3.11 | 0.21 |
| 3 | 43634 | 4.58 | 2.50 | 3.74 | 0.00 |
| 3 | 54132 | 0.21 | 0.21 | 0.00 | 0.21 |
| 2 | 54100 | 3.75 | 1.67 | 2.08 | 0.21 |
| 2 | **43634** | **0.00** | **0.00** | 3.12 | 0.62 |
| 2 | 54132 | 0.00 | 0.42 | 0.00 | 0.62 |

The diag build counted the misses at N=3/N=2 as software-compare rejections
(`fr_foreign` rose one-for-one with `silent`). Raw payloads (`p4_n2_dn54100`,
`p4_n3b_dn43634` logs):

- 54100, N=2: expected `54 d3 0b ...`, got `49 a6 16 0a ...` = the stream with
  one bit deleted in the `0101` run, intact after it (4/4 frames).
- 43634, N=3: expected `aa 0b 05 0a 10 ...`, got `a4 16 0a 14 21 ...` = same
  signature, one bit deleted in `0xAA` right after the hardware address (8/8).
- Re-inserting that bit (position 4) makes every one of the 8 43634 frames
  CRC-valid, uniquely, as a plausible power page. The 54100 captures held 12 of
  13 payload bytes, so they could not be CRC-checked.

So the slip zone is "the hardware address plus about a byte after it". N=2
moves the zone's end to bit 16; 43634's run starts at bit 24 (clean), 54100's
run spans bits 11-22 (still lossy).

ztest on the 54L15 DK: 46/46 suites pass, incl. new `radiant_addr_split` 8/8.

## Phase 5: the fix (`CONFIG_RADIANT_NRF_SLIP_GUARD`, ADR 0019)

Why not N=2 for everyone: on the clean #54132 at N=2 the 54L15 rejected 2 of
481 frames, and both captures were **noise** matches on `[A6 C5]` whose
payload held the real frame's preamble and address (`... aa a6 c5 74 d3 ...`).
A 16-bit match costs about that much on any number. So the guard gives the
2-byte match only to addresses with an alternating run of 9 or more bits
(~5 % of numbers, any dtype). It repairs the slipped bit when the received
address bytes are the filter's with exactly one deletion and the CRC over the
reconstruction verifies. Every other address keeps the full match, unchanged.
Offline first: all 21 Phase 4 captures repair to exactly one CRC-valid power
page (the truncated #54100 ones: 1 of 256 fills each). RXCRC holds the first
received CRC bit in bit 15.

Images: `build/fix_dongle` (DFU), `build/fix_l15` (+ `rxdiag.conf`),
`build/fix53`, all with `SLIP_GUARD=y RUN=9`. ztest 46/46 suites,
`radiant_addr_split` 13/13. Feather stays stock as the same-window control.

### Sweep, Garmin transmitting (120 s; `p5_*`)

| dev # | run | guard | Feather (stock) | Dongle | 54L15 | 5340 | RSSI 5340 |
|---|---|---|---|---|---|---|---|
| 54100 | 11 | 2 B | 2.07 | 0.00 | 0.42 | 3.12 | -60 |
| 43634 | 11 | 2 B | 1.46 | 0.41 | 0.62 | 3.12 | -59 |
| 21845 | 21 | 2 B | 1.04 | 0.21 | 0.83 | 3.12 | -58 |
| 54101 | 12 | 2 B | 2.07 | 0.00 | 0.21 | 0.42 | -51 |
| 54102 | 10 | 2 B | 1.87 | 0.00 | 0.00 | 1.25 | -51 |
| 54096 | 9  | 2 B | 0.83 | 0.00 | 0.00 | 0.62 | -50 |
| 54186 | 9  | 2 B | 0.62 | 0.21 | 0.00 | 0.42 | -50 |
| 54058 | 8  | 5 B | 0.00 | 0.00 | 0.00 | 0.83 | -50 |
| 54080 | 7  | 5 B | 0.00 | 0.00 | 0.00 | 0.00 | -40 |
| 54132 | 6  | 5 B | 0.21 | 0.00 | 0.00 | 0.00 | -40 |
| 54900 | 6  | 5 B | 0.00 | 0.21 | 0.00 | 0.00 | -40 |

The first three windows ran with the signal 8-20 dB down on every receiver
(54L15 -55 vs -35, Feather -56 vs -40), which confounds them. 54L15 diag over
the whole sweep: `fr_repaired` 52, `fr_foreign` 1 (one noise match in ~6700
windows), and the `hw=` the guard chose was right in every window (2 for runs
>= 9, 5 below; the last line of each log is the next window's start). Its
unrecovered frames at the weak windows were not single deletions: address
intact with body errors, and one deletion followed by an insertion a few bits
later (`0b` arrived as `0d`).

### Interleaved rerun at normal signal (120 s; `p5b_*`, all RSSI -37..-40)

| dev # | Feather (stock) | Dongle | 54L15 | 5340 |
|---|---|---|---|---|
| 54132 | 0.00 | 0.00 | 0.00 | 0.00 |
| 54100 | 2.28 | 0.00 | 0.00 | 0.00 |
| 54900 | 0.00 | 0.00 | 0.00 | 0.00 |
| 43634 | 1.87 | 0.21 | 0.00 | 0.21 |
| 21845 | 2.08 | 0.00 | 0.00 | 0.00 |

So the 5340's 3.12 % above went with the weak-signal windows, not with the
fix.

### A/B/A on #54100, Garmin receiving (300 s; `p5aba_*`, `run_aba.ps1`)

TX the Dongle (stock TX path). Only the 54L15 changes between arms: A = stock
5-byte match (`build/rx_l15_n5`), B = slip guard (`build/fix_l15`).

| arm | 54L15 | Garmin | Feather (stock) | 5340 (fix) |
|---|---|---|---|---|
| A  | 2.08 [1.41, 3.05] | 0.17 [0.05, 0.60] | 2.33 | 0.08 |
| B  | **0.17 [0.05, 0.60]** | 0.17 [0.05, 0.60] | 2.41 | 0.08 |
| A2 | 2.33 [1.62, 3.35] | 0.33 [0.13, 0.85] | 2.58 | 0.08 |

Arm A's diag: 25 missed windows, all 25 `silent`, as in Phase 2. TODO item 1's
Done gate (RadiANT within the Garmin's Wilson CI, 0.5 pp margin) holds for both
fixed receivers. RSSI -21..-30 dBm.

**Open, signal-dependent residual.** In the middle `p5_*` windows the 5340 saw
-50 dBm (the others -35..-41). There it still lost 0.4-1.25 % on numbers with
runs of 8-10, guarded or not: 54102 1.25, 54096 0.62, 54186 0.42, and the
unguarded run-8 #54058 0.83. At -40 dBm it lost nothing anywhere. With the
54L15's weak-window captures (above) that points to the slip getting more
frequent at lower signal, reaching shorter runs, and sometimes being followed
by a re-slip the single-deletion repair rightly refuses. Next steps if it
matters: a 5340 run at a controlled -50..-60 dBm with `SLIP_GUARD_RUN=8`, and
captures (the 5340 image has no log console, so RTT or the 54L15 at the same
level).
