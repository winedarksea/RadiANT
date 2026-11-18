# B4 gate pair: -O2 vs -O2 + B4 arm cache, Pro Micro (2026-11-16)

Plan: `C:\Users\Colin\.claude\plans\a-reviewer-had-some-shiny-prism.md`, B4 "Gate: one more C2 pair with and
without it". **Result-checked 2026-11-16 by an independent agent: every number recomputes from the raw
files (five small slips, corrected below); timing gate met.** This run reuses the C2 nRF runner and
workload (`../2026-11-16-pass-breakdown/`). There is one change: the Garmin is off the bench, so the TI LaunchXL
does the master counts and there is no reference receiver in the window.

**What this measures.** Timing (pages 8 and 9) on the Pro Micro under the C2 Zwift-style load. The CPU sets
these numbers, not the room, so sequential legs are valid for them. Loss is context only. With no second
receiver in the window, each loss figure is compared only with the other legs, so room drift is not
controlled.

## Result in one paragraph

Four 300 s legs, A/B/A/B, each from a fresh flash. All 8 simulated masters were on air before and after every
leg (TI count 8/8), the trainer was tracked, and each leg had 67 scan opens. **Worst-case page 9 [5] was
A 855 / 254 and B 178 / 183 µs.** A leg 1 is a refusal-retry outlier: 3 early-ETIME refusals and
`pass_steps_max` 4. The clean A legs (leg 3: 254; C2 leg 2: 255, same image on another run) put B4's saving
at **~71–77 µs on [5]**. The C2 estimate, a loose upper bound, was 105. **Page 9 [2] (backend arm call) halved,
144 → 74 µs in both B legs**, and that accounts for almost all of the [5] drop. Pooled tracked loss on the
8 sims was 1.99 / 2.04 / 2.05 / 2.04 %. Scan sightings/s were 7.80 / 7.84 / 7.34 / 7.00, falling with time,
not with the image. Neither B leg had early ETIME, a refusal or a start-missed. **The C4 candidate on B is
250 µs, which is not < 240**: it misses by one 10 µs step, and it would need a worst [5] ≤ 168 µs. C4 was not
applied.

## Rig

| Role | Board | Image | Masters / channels |
|---|---|---|---|
| DUT | Pro Micro nRF52840 clone, USB `EE6986854FD72803` | A: `C:\wt_c2\build\pm_o2\dongle\zephyr\zephyr.uf2` c4b4ca99fc52deec40add6ba97b312c9 (C2 -O2 image, master 3292198). B: `C:\wt_b4\build\r2_pmdiago2\dongle\zephyr\zephyr.uf2` ba0a7084da330fbf71f1ced903d61a24 (branch perf/nrf-arm-cache f90fff9) | scan ch0 reopened every 4.5 s + 9 tracked (8 sims + trainer FE-C) |
| Master | nRF54L15 DK, COM4 @ 57600 | unchanged 3b486c3 Zwift MPSL image (not reflashed; reset by ant_probe before each leg) | 21001 power 8182, 21002 HR 8070, 21003 CSC 8086 |
| Master | nRF5340 DK net core, COM5 @ 115200 | unchanged from C2 (3292198 cpunet) | 24001 HR, 24002 CSC, 24003 power |
| Master | nRF52840 Dongle `3D55F77818BE772A` | unchanged from C2 (3292198 direct) | 23001 CSC, 23002 power |
| Master counter | TI LaunchXL, XDS110 `L1100TY2`, COM11 @ 57600 | `ti_fix.hex`, not reflashed | receive only: `ant_scan_rate` 20 s before and after each window, then an ant_probe reset |
| Air | trainer #52233 (heard: 0x0B, 0x11, 0x7B; `pre/ti_trainer_check.txt`) | - | tracked by the DUT as 52233:0x11 |

**A vs B source.** `git log 3292198..f90fff9` shows only the two B4 commits, e67fdb5 and f90fff9. The diff
touches `radiant_radio_nrf.c`, `radiant_radio_hal.h`, backend tests and two lines of `docs/backends.md`. The `C:\wt_b4` worktree is clean.

**`.config` checked on both images** (`images/A_pm_o2.config`, `images/B_b4_o2.config`):
`RADIANT_DIAG_COUNTERS=y`, `ANT_DONGLE_DIAG=y`, `ANT_DONGLE_HEALTH_COUNTERS=y`, `RADIANT_HOT_PATH_O2=y`,
`RADIANT_CRC_REPAIR=y`, `RADIANT_PROFILE_SANITY=y`, `RADIANT_SEARCH_SHORT_MATCH=y`, `RADIANT_BACKEND_NRF=y`,
`RADIANT_BACKEND_NRF_GATE_MPSL` not set, `MPSL` not set, `BT` not set, `K32SRC_XTAL=y`. The two lists are
identical. `md5sums.txt` lists the archived copies and the raw JSON.

**Master count source: the TI.** It is receive-only (`ant_scan_rate.py --port COM11 --baud 57600 --seconds
20`), followed by `ant_probe.py --port COM11 --baud 57600` to reset it. It saw 8/8 in every count, every sim
first heard within 0.25 s. (In the aborted attempt the L15's three masters were first heard at 18 s.)

## Procedure (per leg)

Driver `run_legs.ps1`. For each leg, in order:

1. Check the image md5.
2. `flash_pm.ps1`, unchanged from C2: reboot to bootloader, UF2 copy, re-enumerate, ant_probe PASS.
3. `leg_b4.py <leg> --trainer`. This is C2's `leg.py` with three changes:
   - The Garmin count becomes the TI count.
   - The Garmin `ant_verify` window is removed.
   - The outdir is made absolute (see the aborted attempt below).

Everything else is identical: master resets, 480 s sims with the same device numbers and seeds, diag and 0xF6
before, the 300 s `ant_scan_rate.py --serial EE6986854FD72803 --seconds 300 --reopen 4.5` with 9 `--track`,
diag and 0xF6 after, `ant_diag --diff`, post-count, and the sims running out.

| Leg | Image | Flash md5 | Window | Opens | Masters pre / post | Sims (s) |
|---|---|---|---|---|---|---|
| 1 | A | c4b4ca99 | 301.6 s, rc 0 | 67 | 8/8 / 8/8 | 480.7–481.7 |
| 2 | B | ba0a7084 | 301.5 s, rc 0 | 67 | 8/8 / 8/8 | 480.7–481.8 |
| 3 | A | c4b4ca99 | 301.6 s, rc 0 | 67 | 8/8 / 8/8 | 480.8–481.8 |
| 4 | B | ba0a7084 | 301.7 s, rc 0 | 67 | 8/8 / 8/8 | 480.7–481.6 |

- **Fresh boot confirmed** on every before-read: page 9 [5] = 0, [6] no_sample, `pass_max_us` 21,
  `sched_dones` 0.
- The UART sims (L15, 5340) exit rc 1 in every leg, as in C2. That is ant_sim's strict pacing check: 5–6
  messages per run went out on the wall-clock fallback. The post-counts are 8/8 and the in-window accounting
  closes, so no channel was lost.

## Timing (pages 8 and 9): the gate

Levels are the worst case since boot, and each leg starts from a fresh boot. Fields [0]–[3] are per-pass sums
(`radiant_sched.c` `pb`), so a pass that arms more than once adds all its arm calls into [2] and [5].

| Field | leg 1 A | leg 2 B | leg 3 A | leg 4 B | C2 leg 2 (A image) |
|---|---|---|---|---|---|
| p8 `pass_max_us` | **872** | 197 | 273 | 202 | 274 |
| p8 `pass_steps_max` | **4** | 2 | 2 | 2 | 2 |
| p8 `pass_over_100us` (count) | 21041 | 21025 | 21005 | 20657 | 20972 |
| p9 [0] `sweep_max_us` | 77 | 43 | 45 | 32 | 44 |
| p9 [1] `decide_max_us` | 355 | 104 | 104 | 106 | 106 |
| **p9 [2] `arm_call_max_us`** | 327 | **74** | 144 | **74** | 144 |
| p9 [3] `done_cb_max_us` | 115 | 115 | 115 | 115 | 115 |
| p9 [4] `book_max_us` | - | - | - | - | - |
| **p9 [5] `pass_to_arm_max_us`** | **855** | **178** | **254** | **183** | 255 |
| p9 [6] `arm_slack_min_us` | 52 | 83 | 52 | 83 | 52 |
| p9 [7] `early_etime` | **3** | 0 | 0 | 0 | 0 |

Per image (two legs each; C2 leg 2 is a third A point from the same image on the same day, in a different
run and room state):

| | A worst | A mean | A clean (leg 3 / C2 leg 2) | B worst | B mean |
|---|---|---|---|---|---|
| [5] | 855 | 554.5 | 254 / 255 | 183 | 180.5 |
| [2] | 327 | 235.5 | 144 / 144 | 74 | 74 |

- **[5] drops by ~71–77 µs** (clean A 254/255 → B 178/183). The C2 estimate was [5] − [0] − [1] = 105 µs on
  -O2 (105 again in leg 3 here), a loose upper bound that also includes up to 115 µs of callbacks ([3]). The
  direct measure is the [2] drop, which accounts for the whole [5] drop. Main session: gate met in substance.
- **[2] (B4's target) drops 144 → 74 µs in both B legs**, about −70 µs, which is almost all of the [5] drop.
  [0], [1] and [3] do not move (104–106 decide, 115 callbacks). **Mechanism (result check):** the old code
  called `rx_sw_addr_len()` three times per receive arm (twice in `rx_arm()`, once in `program_rx()`, all
  before the backend's `now`); with SLIP_GUARD it runs `radiant_addr_alt_run()`, a 39-iteration bit loop in
  `radiant_frame.c` (compiled -Os, outside the O2 hot set), ~16–19 µs per filter per call. The A ELF has 3
  call sites of `radiant_addr_split_rx_hw_len`, the B ELF 1, reached only on a filter-memo miss. Two saved
  calls on a 2-filter merged window ≈ 64–75 µs. The worst cases are deterministic (A 144/144/143, B 74/74).
- B delivered normally: every sim channel 1157–1207 frames on both images, `sw_addr_mismatch` 0, `crc_fail`
  and terminal counts in A's range, scan page 5 equal across images.
- `pass_max_us` falls with [5]: clean A 273/274, B 197/202.
- **[6] min slack**: A 52 µs in every clean leg, B 83 µs in both legs (31 µs more slack).
- **Leg 1 A is a refusal-retry outlier.** Three early-ETIME refusals: page 9 [7] = 3, page 11 `arm_rejected` 3,
  page 2 `sched_arm_rejected` 3, page 3 `rx_refused` 3. `pass_steps_max` was 4 and every per-pass sum is
  inflated ([1] 355, [2] 327, [5] 855). That fits one pass with three refused RX arms and a retry from the same
  `now`, the leg-3 C2 mechanism repeated three times. The counters cannot show whether all three were in one
  pass, but [5] only grows within a pass and 855 is about 3.4 × 254, so that reading fits. Same image as legs
  3 and C2-2, which saw none. A refusal needs a pass over the 240 µs budget to coincide with a floor-lead
  window, so how often it happens depends on the phases between masters, not on the image. **B never refused
  in 600 s.** No B pass reached the floor-lead refusal condition: worst [5] 183, 57 µs under 240. Refusals
  are rare on A too (1 event in 900 s on the -O2 image), so the rates cannot be told apart statistically.
- **Refusal cascade:** each retry plans from the stale pass `now` (`pass()` reads `now` once), so every
  refusal pushes the backend's `now′` further past it and makes the next arm likelier to be refused. That is
  how one pass reached 3 refusals and [5] = 855 ([0]+[1]+[2] = 759 ≤ `pass_max` 872).
- **Not covered by this gate:** `tx_armed` was 0 on every leg, so the transmit path and `apply_tx_address()`'s
  shadow invalidation were not exercised on hardware; the conformance transcripts are a separate item
  (`gate_extra/`).

### B4 trigger estimate per leg ([5] − [0] − [1], same loose bound as C2)

423 / 31 / 105 / 45. On B it is 31–45 µs, so a further B4-style cut of the arm call alone has at most that
left to find.

### C4 candidate (⌈1.25 × worst [5] + 20⌉ to 10 µs). Not applied.

| Source | worst [5] | candidate | < 240? |
|---|---|---|---|
| A, worst leg (leg 1, refusal chain) | 855 | ⌈1088.75⌉ → **1090** | no |
| A, clean leg (leg 3) | 254 | ⌈337.5⌉ → **340** | no |
| B, worst leg (leg 4) | 183 | ⌈248.75⌉ → **250** | **no** (by 10) |
| B, leg 2 | 178 | ⌈242.5⌉ → 250 | no |

To land below 240 (230 or less), the worst [5] would have to be ≤ 167 µs. Under the plan's rule B's 250 is
"≥ 240: change nothing, bring to the checkpoint". It is now within one rounding step.

## Other pages (diff over the leg; levels are after values)

| Field | leg 1 A | leg 2 B | leg 3 A | leg 4 B |
|---|---|---|---|---|
| p7 `srch_cut_close` | 35 | 17 | 19 | 19 |
| p7 `srch_end_near_close` | 65 | 68 | 67 | 61 |
| p7 `trk_open_late` | 41 | 40 | 41 | 48 |
| p7 `trk_open_late_max_us` (level) | 188 | 148 | 161 | 186 |
| p7 `trk_missed_behind_scan` | 0 | 0 | 0 | 0 |
| p7 `tx_start_missed` | 0 | 0 | 0 | 0 |
| p7 `rx_start_missed` | 0 | 0 | 0 | 0 |
| p10 `crc_repaired` | 5 | 14 | 14 | 15 |
| p10 `crc_unrepairable` | 26 | 25 | 24 | 42 |
| p10 `crc_repair_refuted` | 0 | 0 | 0 | 0 |
| p10 `crc_repair_implausible` | 0 | 0 | 0 | **1** |
| p10 [4] `late_isr_drops` | 0 | 0 | 0 | 0 |
| p10 [5] `end_late_max_us` (level) | 39 | 40 | 38 | 39 |
| p11 `windows_merged` | 148 | 148 | 153 | 129 |
| p11 `preempted` / `replans` | 0 / 0 | 0 / 1 | 0 / 0 | 0 / 0 |
| p11 `arm_rejected` | **3** | 0 | 0 | 0 |
| p11 `stale_events` | 65 | 63 | 62 | 63 |
| p11 `slots_collided` | 4 | 3 | 3 | 2 |
| p0 `sched_missed` | 5 | 3 | 3 | 2 |
| p0 `slots_missed` | 493 | 519 | 587 | 607 |
| p3 `rx_posted` | 21042 | 21026 | 21005 | 20658 |
| p4 `crc_fail` | 3776 | 3809 | 3576 | 3595 |

- `rx_start_missed`, `tx_start_missed` and `trk_missed_behind_scan` are 0 in every leg. B4 opens no
  start-missed path.
- `late_isr_drops` is 0 everywhere, and END-ISR lateness is 38–40 µs.
- Leg 4 B has one `crc_repair_implausible` (the sanity check rejected a repair). It is a single event; read
  it as room CRC noise unless it repeats.
- Leg 4 has ~370 fewer `rx_posted` / `pass_over_100us` than the other legs, and 129 merged windows against
  148–153. The master phase layout differs between legs. Every leg's in-window accounting closes (below).
- `stale_events` 62–65 again, all `stale_rx_end`, matching the ~67 scan closes (C2 explanation).
- `pass_over_100us` is `rx_posted − 1` in legs 1, 2 and 4, and equal to `rx_posted` in leg 3, even on B.
  Every arming pass still takes over 100 µs.
- Pages 5 and 6 are not diffed (ant_scan_rate's reset zeroes them). The raw JSON has the values.

## 0xF6

| | leg 1 A | leg 2 B | leg 3 A | leg 4 B |
|---|---|---|---|---|
| `tx_high_water` before → after | 0 → 32 | 0 → 32 | 0 → 32 | 0 → 32 |
| `tx_drops` before → after | 0 → 258 | 0 → 112 | 0 → 119 | 0 → 101 |
| `tx_timeout` before → after | 0 → 2 | 0 → 2 | 0 → 2 | 0 → 2 |

Same post-window artefact as C2: the tracked channels stay open after ant_scan_rate exits, with no reader
until ant_diag and ant_health. It says nothing about item 6. Leg 1's 258 is the longer post-window reader
gap in that leg (ant_health took 4.3 s against 1.5 s elsewhere; `leg1_A/leg.log`), not a window loss.

## Loss and scan (context only; no same-window reference)

Pro Micro tracked loss = RX_FAIL / (received + RX_FAIL):

| Master | leg 1 A | leg 2 B | leg 3 A | leg 4 B |
|---|---|---|---|---|
| 21001 power (L15) | 3.41 % | 3.41 % | 3.58 % | 3.50 % |
| 21002 HR (L15) | 4.35 % | 4.69 % | 4.35 % | 4.27 % |
| 21003 CSC (L15) | 3.46 % | 3.37 % | 3.37 % | 3.79 % |
| 24001 HR (5340) | 0.99 % | 0.99 % | 1.64 % | 1.15 % |
| 24002 CSC (5340) | 0.82 % | 0.99 % | 0.66 % | 0.99 % |
| 24003 power (5340) | 1.17 % | 1.17 % | 1.00 % | 1.00 % |
| 23001 CSC (Dongle) | 1.07 % | 1.23 % | 1.40 % | 1.23 % |
| 23002 power (Dongle) | 0.67 % | 0.42 % | 0.33 % | 0.33 % |
| **8 sims pooled** | **1.99 %** (193/9676) | **2.04 %** (197/9671) | **2.05 %** (198/9679) | **2.04 %** (197/9673) |
| 52233 FE-C (trainer) | 24.83 % | 26.94 % | 32.03 % | 34.15 % |
| **Scan sightings/s** | 7.80 | 7.84 | 7.34 | 7.00 |

- **Tracked loss is unchanged after removing the drift; there is no same-window reference, so this gate item
  is weaker than the plan asks.** The pooled sim loss is flat at 1.99–2.05 %, a spread of 5 frames out of
  ~9,700 (binomial SE ~0.14 pp per leg, so only a change ≥ ~0.4 pp is detectable). A time-trend plus image
  fit gives an image effect of +0.004 pp on loss and +0.18/s on sightings (B better), trend −0.33/s per leg. Per-master figures match C2: the L15 is the weak transmitter at 3.4–4.7 %.
- The trainer FE-C loss rises steadily with time (24.8 → 26.9 → 32.0 → 34.2 %) whatever the image: A, B, A, B
  all go up. That is room or trainer drift, the known FE-C problem, not B4. Without a Garmin it cannot be
  separated further.
- Scan sightings/s are mostly the trainer's 0x0B and 0x7B channels (heard in 67/67 opens). They also fall
  with time: 0x0B 5.10 / 5.18 / 4.88 / 4.64 per s. So 7.80 / 7.84 / 7.34 / 7.00 is drift, not the image
  (A mean 7.57, B mean 7.42; the B legs fall either side of A leg 3). Each sim was sighted on scan in about
  1/67 opens, as in C2.
- In-window accounting (`summary.json` → `scan.accounting`): each sim channel is within −0.5 to +6.7 slots of
  window × 32768 / period. The trainer is 12–23 short, from its search periods, as in C2.

## Bench events and deviations

- **Aborted first attempt (`aborted_leg1_relative_json_path/`).** `leg_b4.py` was given a relative outdir, and
  the tools run with cwd = the tools folder. So the TI count's `--json` path did not resolve and the count
  parsed as empty: "short" twice, then ABORT. The TI's printed table in that attempt shows 8/8.
  - Fix: `outdir = os.path.abspath(...)`. (C2 had passed an absolute path.)
  - The Pro Micro had only been probed, with no channels opened, in that attempt. It was reflashed anyway
    before the real leg 1.
- No Garmin: no reference window, no Garmin loss row, no Garmin resets. The TI was reset after every count.
  It was never flashed.
- L15, 5340 and Dongle were not reflashed. The L15 transmitted in every leg (3/3 pre and post); the
  silent-L15 case seen in C2 did not recur.

## Files

- `run_legs.ps1` is the driver; `leg_b4.py` the leg runner (C2 `leg.py` adapted); `flash_pm.ps1` is C2's,
  unchanged; `analyse.py` is C2's without the Garmin section and with pages 2 and 3 added; `table.py` renders
  `table.txt` from `summary.json`.
- `leg{1_A,2_B,3_A,4_B}/` hold the raw data per leg:
  - `diag_before.json`, `diag_after.json`, `diag_diff.txt`;
  - `health_before.txt`, `health_after.txt`;
  - `scan_pm.json` / `.txt`;
  - `count_pre_try0.*` and `count_post.*` (TI), sim logs `try0_sim_*.txt`;
  - `flash.log` (image md5 and probe), `leg.log`, `meta.json` (every command with rc, start and duration),
    `runner_stdout.txt`.
- `pre/` holds the TI probe and the trainer check (20 s, sims off).
- `images/` holds copies of both UF2s and their `.config`. `md5sums.txt` covers the images and the raw JSON.

## Final board state (2026-11-16 ~14:00)

- Pro Micro: **B image** (B4 -O2 diag, ba0a7084…), from the leg 4 flash. Its tracked channels were left
  open by ant_scan_rate's exit, as in C2.
- TI LaunchXL: `ti_fix.hex`, unchanged, reset by ant_probe after the last count.
- nRF54L15 DK: unchanged 3b486c3 Zwift MPSL image, COM4 @ 57600. Its sims ran out normally.
- nRF5340 DK, nRF52840 Dongle: unchanged from C2. Their sims ran out normally.
