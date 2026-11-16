# C2 load run, nRF part: pass breakdown, -Os vs -O2 (2026-11-16)

Plan: `C:\Users\Colin\.claude\plans\a-reviewer-had-some-shiny-prism.md`, step C2 (nRF legs only; the TI is
off the bench, owner ruling "nRF legs first"). Draft by the bench agent. **Result-checked 2026-11-16 by an
independent agent: every number recomputes from the raw files; its wording corrections are applied below.**
Owner decisions at the checkpoint: cut the pass first (B4 plus removing the duplicate format/filter work,
lead stays 328), and HOT_PATH_O2 on in the shipping apps.

**What this measures.** Timing (pages 8 and 9) on the Pro Micro under a Zwift-style load. The CPU sets these
numbers, not the room, so sequential legs are valid for them. Loss is context only, read against the Garmin in
the same window.

## Result in one paragraph

Every leg ran 300 s with all 8 simulated masters on air before and after (Garmin count 8/8), the trainer
tracked, and 67 scan opens. **The worst-case page 9 [5] (pass `now` to backend `now`) was 289 / 255 / 557 µs
(-Os / -O2 / -Os).** Leg 3's 557 is one pass that made two arm calls: an RX arm refused by the early ETIME
check (**page 9 [7] `early_etime` = 1, page 11 `arm_rejected` = 1**), then a retry from the same `now`. So the
clean -Os/-O2 pair is legs 1/2 (289 vs 255, one worst case each). [5] is CPU time inside `pass()`'s irq_lock
and exceeds the 240 µs budget (328 lead − 88 air lead) in every leg: the review's 25 µs margin is already
negative, and a refusal happens when that coincides with a floor-lead window (once in 900 s).
**B4 trigger: met in every leg** (estimate 113 / 105 / 253 µs, threshold 20 µs). **C4 candidate: 390 µs from
leg 1, 720 µs from the worst -Os leg, 340 µs on -O2. All are at or above 240**, so C4 changes nothing and goes
to the owner checkpoint (plan rule).

## Rig

| Role | Board | Image (md5) | Masters / channels |
|---|---|---|---|
| DUT | Pro Micro nRF52840 clone, USB `EE6986854FD72803` | -Os `images/promicro_Os.uf2` 81dfb8be7df12b9d628ecfe1132a914f; -O2 `images/promicro_O2.uf2` c4b4ca99fc52deec40add6ba97b312c9 | scan ch0 reopened every 4.5 s + 9 tracked (8 sims + trainer FE-C) |
| Master | nRF54L15 DK, COM4 @ 57600 (J-Link 001057737173) | unchanged 3b486c3 Zwift MPSL image (`build/cu_zwift_mpsl`, md5 59b84375 per memory; not reflashed) | 21001 power 8182, 21002 HR 8070, 21003 CSC 8086 |
| Master | nRF5340 DK net core, COM5 @ 115200 (J-Link 001050006310) | `images/nrf5340_cpunet.hex` ec3ac1be75d84a92a34054fdb8826234 + app core `images/nrf5340_appcore.hex` 45bd2a9fcb8739bd6286dada17e618c0 | 24001 HR 8070, 24002 CSC 8086, 24003 power 8182 |
| Master | nRF52840 Dongle, USB `3D55F77818BE772A` | `images/radiant_dongle_nrf52840dongle_c2.zip` f681ab248f80539f47eb92bc76210529 (hex 6732ed8cfc4b1975899f348f31876862) | 23001 CSC 8086, 23002 power 8182 |
| Reference | Garmin USB-m `006` | stock | one channel: 21001 power, `ant_verify.py` |
| Air | trainer #52233 (powered, heard: 0x0B, 0x11, 0x7B) | - | tracked by the DUT as 52233:0x11 (FE-C, 8192) |

All three master boards run at once, so cross-board close pairs occur (3x3 + 3x2 + 3x2 = 21 cross-board pairs;
the same-board pairs do not count, per memory `radiant-multichannel-bench-traps`). Device numbers were checked
with `ant_identity.slip_warning`: 22001-22003 trip SLIP_GUARD and were replaced by 24001-24003.

### Images and build

All built from `C:\wt_c2`, branch `bench/c2-nrf` at master **3292198**, NCS v3.4.0, worktree clean.
Script: `images/c2_builds.ps1`.

- Pro Micro (A, -Os): `apps/dongle -b promicro_nrf52840/nrf52840/uf2 -- -DANT_RADIO=core -DRADIANT_BACKEND=nrf -DEXTRA_CONF_FILE=diag_counters.conf`.
- Pro Micro (B, -O2): same plus `images/o2.conf` (`CONFIG_RADIANT_HOT_PATH_O2=y`), as `-DEXTRA_CONF_FILE=diag_counters.conf;C:/wt_c2/build/o2.conf`.
- `.config` read back on both (`images/promicro_O*.config`): `RADIANT_DIAG_COUNTERS=y`, `ANT_DONGLE_DIAG=y`,
  `ANT_DONGLE_HEALTH_COUNTERS=y`, `RADIANT_CRC_REPAIR=y`, `RADIANT_PROFILE_SANITY=y`, `RADIANT_SEARCH_SHORT_MATCH=y`,
  `RADIANT_BACKEND_NRF=y`, `RADIANT_BACKEND_NRF_GATE_MPSL` not set, `MPSL` not set, `K32SRC_XTAL=y` (LFXO).
  `RADIANT_HOT_PATH_O2` is not set on A and `=y` on B.
- `compile_commands.json` (`images/compile_flags_check.txt`): on B, `radiant_sched.c`, `ant_radio_radiant.c` and
  `radiant_radio_nrf.c` compile with `-Os ... -O2` (the -O2 comes last). On A they have `-Os` only.
- Size: -Os 98984 B flash / 60228 B RAM; -O2 104344 B / 60228 B (+5360 B flash).
- Dongle: `apps/dongle -b nrf52840dongle/nrf52840 -- -DANT_RADIO=core -DRADIANT_BACKEND=nrf`. This is a direct
  build: no MPSL gate, no diag. `scripts/package_dfu.ps1`, then
  `nrfutil nrf5sdk-tools dfu usb-serial -pkg ... -p COM7` from the SDFU bootloader (`images/dg_dfu.log`).
  ant_probe PASS afterwards.
- 5340: `apps/dongle -b nrf5340dk/nrf5340/cpunet -- -DANT_RADIO=core -DRADIANT_BACKEND=nrf` and
  `radiant/spike/promisc/appcore -b nrf5340dk/nrf5340/cpuapp`, flashed with the guarded
  `scripts/flash_sim_jlink.ps1 -Serial 001050006310 -Device nRF5340_xxAA_NET` / `_APP`
  (`images/n53*_flash.log`). ant_probe `--port COM5` PASS (RADIANT0.01B00).
- `images/md5sums.txt` lists every archived image and config.

## Procedure (per leg, `leg.py`)

1. Pro Micro flashed (`flash_pm.ps1`: `ant_probe --reboot-to-bootloader`, UF2 copy to NICENANO, re-enumerate,
   ant_probe PASS). **Every leg starts from a fresh boot**, so the since-boot worst cases cover the leg only.
   Before-reads confirm this: page 9 [5] = 0, `arm_slack_min` = no_sample, `pass_max_us` 21-22, `sched_dones` 0.
2. ant_probe resets each master board, then `ant_sim.py` runs on all three (480 s, seed = base device number).
3. Master count: a 20 s Garmin `ant_scan_rate` must see 8/8 sim devices (per board 3/3, 3/3, 2/2). Then the
   Garmin is reset with ant_probe.
4. `ant_diag.py --serial PM --json diag_before.json` (all pages) and `ant_health.py --json` (0xF6).
5. **Window, 300 s, same start:** `ant_scan_rate.py --serial EE6986854FD72803 --seconds 300 --reopen 4.5` with 9
   `--track` (8 sims + 52233:0x11) on the Pro Micro, and `ant_verify.py --serial 006 --profile power
   --device-number 21001 --trans-type 5 --seconds 300` on the Garmin.
6. ant_diag after, ant_health after, `ant_diag.py --diff`.
7. Master count again (Garmin 20 s, then reset).
8. The sims run out. Then `analyse.py leg1_Os leg2_O2 leg3_Os > summary.json`; `table.txt` is the rendered table.

| Leg | Image | Pro Micro window | Garmin window | Masters pre / post | Sims (s) |
|---|---|---|---|---|---|
| 1 | -Os | 301.3 s, rc 0, 67 opens | 301.6 s | 8/8 / 8/8 | 480.5-481.4 |
| 2 | -O2 | 301.2 s, rc 0, 67 opens | 301.4 s | 8/8 / 8/8 | 480.6-481.3 |
| 3 | -Os | 301.3 s, rc 0, 67 opens | 301.5 s | 8/8 / 8/8 | 480.6-481.4 |

The sims on the two UART boards exit rc 1 in every leg. That is ant_sim's strict pacing check: 1-17 of ~1900
messages per channel went out on the wall-clock fallback. It is not a channel loss: the post-window count is 8/8
and every sim channel accounts for its slots in-window (below). The Garmin's rc 1 is its 1.5 % loss limit.

## Timing (pages 8 and 9): the result

Levels are the worst case since boot. Each leg starts from a fresh boot.

| Field | leg 1 -Os | leg 2 -O2 | leg 3 -Os |
|---|---|---|---|
| p8 `pass_max_us` | 311 | 274 | **578** |
| p8 `pass_steps_max` | 2 | 2 | 2 |
| p8 `pass_over_100us` (count) | 21010 | 20972 | 21029 |
| p0 `sched_dones` (count) | 21086 | 21055 | 21110 |
| p9 [0] `sweep_max_us` | 47 | 44 | 65 |
| p9 [1] `decide_max_us` | 129 | 106 | 239 |
| p9 [2] `arm_call_max_us` | 143 | 144 | 220 |
| p9 [3] `done_cb_max_us` | 120 | 115 | 120 |
| p9 [4] `book_max_us` | not measured | not measured | not measured |
| p9 [5] `pass_to_arm_max_us` | **289** | **255** | **557** |
| p9 [6] `arm_slack_min_us` | 21 | 52 | **18** |
| p9 [7] `early_etime` (count) | 0 | 0 | **1** |

- **Every arming pass is over 100 µs.** `sched_dones` is not a pass count; `pass_over_100us` equals arm calls
  − 1 in every leg (rx_posted 21011 vs 21010, 20973 vs 20972; leg 3: 21030 posted + 1 refused vs 21029, the
  refusal-plus-retry pass holding two). Housekeeping passes stay under 100 µs.
- Also not in the table: 0xF6 `tx_timeout` 0→2/2/1, and in leg 3 page 2 `sched_arm_rejected` 1 and page 3
  `rx_refused` 1 (the same refusal).
- **Page 9 [4] reads 0xFFFFFFFF on this direct build**, so book_ahead() is not timed here. It was already "-" at
  boot.
- **-Os vs -O2.** Leg 2 is lower than leg 1 on [1] (106 vs 129), [5] (255 vs 289) and `pass_max` (274 vs 311),
  and level on [0], [2] and [3]. But the A/A repeat is not stable: leg 3 (the same -Os image) reads [5] 557 and
  `pass_max` 578. The difference between the two -Os legs (268 µs) is eight times the -Os to -O2 difference
  (34 µs), but leg 3 differs because of the refusal path, not -Os noise. The clean comparison is leg 1 vs
  leg 2 (289 vs 255), one maximum each, so it is still not resolved statistically. [1] decide was lower on
  -O2 than on either -Os leg, and [2] arm_call (backend) did not move with -O2 (143/144).
- **Leg 3's outlier, reconciled:** one pass made two arm calls. The first RX arm was refused by the early
  ETIME check; `arm_failed()` ran its MISSED `notify_done` and returned STEP_RETRY, and a second arm followed
  from the same `now`. Within one pass [5] only grows, so 557 is the retry arm (accepted, its window ≥ 645 µs
  out); the refused arm's own [5] lay between 240 and 557. [0]+[1]+[2] = 524 ≤ `pass_max` 578. Real CPU time,
  not a measurement artefact.
- [5] is above the 240 µs budget (lead 328 − air lead 88; not ARM_SETUP_US as such) in every leg. An arm is
  refused when [5] > 240 *at the floor lead*. [6] min slack 21/52/18 means the tightest accepted arms had
  [5] ≈ 219/188/222.

### B4 trigger (plan B4: page 9 [5] − [0] − [1] ≥ 20 µs)

| | leg 1 -Os | leg 2 -O2 | leg 3 -Os |
|---|---|---|---|
| [5] − [0] − [1] | 289−47−129 = **113** | 255−44−106 = **105** | 557−65−239 = **253** |
| triggers (≥ 20) | yes | yes | yes |

This is a loose upper bound: each field is a separate worst case, and [5] − [0] − [1] also includes done
callbacks ([3]) and earlier backend calls in the same pass (subtracting [3] gives −7 / −10 / 133). Better,
still indirect, evidence: [2] is ~143 µs per arm call, and every arm ran `apply_format`/`apply_filters` twice
(validation in `rx_arm()`, then again in `program_rx()`), both before the backend's `now`. **B4 is
triggered**; its own gate pair measures the saving.

### C4 candidate (⌈1.25 × worst [5] + 20⌉ to 10 µs)

| Source of worst [5] | [5] | candidate | < 240? |
|---|---|---|---|
| leg 1 -Os | 289 | ⌈381.25⌉ → **390** | no |
| leg 2 -O2 | 255 | ⌈338.75⌉ → **340** | no |
| leg 3 -Os (worst -Os) | 557 | ⌈716.25⌉ → **720** | no |

Every candidate is at or above 240. Per the plan, C4 changes nothing and the numbers go to the checkpoint. The
owner chooses between raising the lead and cutting the pass first (for example B4). C4 was not run.

## Other pages (diff over the leg; levels are after values)

| Field | leg 1 -Os | leg 2 -O2 | leg 3 -Os |
|---|---|---|---|
| p7 `srch_cut_close` | 20 | 33 | 25 |
| p7 `srch_cut_abort` | 0 | 0 | 0 |
| p7 `srch_end_near_close` | 88 | 86 | 62 |
| p7 `trk_open_late` | 46 | 41 | 37 |
| p7 `trk_open_late_max_us` (level) | 197 | 195 | 153 |
| p7 `trk_missed_behind_scan` | 0 | 0 | 0 |
| p7 `tx_start_missed` | 0 | 0 | 0 |
| p7 `rx_start_missed` | 0 | 0 | 0 |
| p10 `crc_repaired` | 15 | 16 | 9 |
| p10 `crc_unrepairable` | 26 | 19 | 27 |
| p10 `crc_repair_refuted` / `implausible` | 0 / 0 | 0 / 0 | 0 / 0 |
| p10 [4] `late_isr_drops` | 0 | 0 | 0 |
| p10 [5] `end_late_max_us` (level) | 60 | 37 | 59 |
| p11 `windows_merged` | 136 | 142 | 142 |
| p11 `preempted` / `replans` | 0 / 0 | 0 / 0 | 0 / 0 |
| p11 `arm_rejected` | 0 | 0 | **1** |
| p11 `tx_armed` | 0 | 0 | 0 |
| p11 `stale_events` | 62 | 62 | 62 |
| p11 `slots_collided` | 2 | 3 | 1 |
| p11 `turn_yields` | 0 | 0 | 0 |
| p12 `stale_rx_end` | 62 | 62 | 62 |
| p12 `stale_rx_frame` / `filter_index` / `tx` | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |
| p0 `sched_missed` | 2 | 3 | 1 |
| p0 `slots_missed` | 554 | 478 | 509 |
| p4 `crc_fail` | 3517 | 3767 | 3588 |

- `stale_events` is exactly 62 in all three legs, and all of it is `stale_rx_end`. Candidate mechanism: 67
  host scan closes per leg reach the firmware; each goes `radiant_sched_cancel()` → `drop_slot()` →
  `abort_armed()` on the in-flight scan chunk, and the backend's later ABORTED terminal carries a retired op.
  A close that lands while a tracked window holds the radio produces none, so ~62-64 is expected. The exact 62
  in all three legs is unexplained; a run with a different `--reopen` would settle it.
- `late_isr_drops` 0 everywhere. The B2 guard never fired under this load, and END-ISR lateness peaked at 60 µs.
- `rx_start_missed`/`tx_start_missed` 0 and `trk_missed_behind_scan` 0 in every leg.
- Pages 5/6 are not diffed: ant_scan_rate's reset zeroes them (memory, plan). The raw JSON has their after
  values.

## 0xF6 (item 6: `tx_high_water`)

| | leg 1 | leg 2 | leg 3 |
|---|---|---|---|
| `tx_high_water` before → after | 0 → **32** | 0 → **32** | 0 → **32** |
| `tx_drops` before → after | 0 → 89 | 0 → 84 | 0 → 52 |

**The queue filled to 32 of 32 and dropped events, but after the window, not in it.**
- When ant_scan_rate exits, the 9 tracked channels keep running. Two reader gaps follow: ant_diag reopens the
  device 0.21-0.32 s after the scan exits, ant_health 1.30-1.40 s after. Firmware (`usb_ant_class.c`): with no
  IN polling a write times out after 1 s (`tx_timeout` 2/2/1), then the 32-deep queue fills at ~36/s.
- Control after leg 3 (`leg3_Os/txdrops_control.txt`): with the channels open and no reader, `tx_drops` went
  506 → 924 in 11.7 s. That is 36/s = 9 channels × 4 Hz.
- In-window accounting agrees. Per sim channel, received + RX_FAIL is within 3.5 slots of window × 32768 /
  period in every leg (`summary.json` → `scan.accounting`; resolution ~±2 slots per channel), so at most a few
  events per channel could have been lost in the window.
- **So this run neither confirms nor refutes item 6's "high water 0, once 1" under load.** ant_health reads the
  level since boot, and the post-window gap pins it at 32. A clean read needs the 0xF6 read inside the same host
  session as the window, or the channels closed before the host lets go. That is a tool change, not done here.

## Loss (context only; same window, Garmin on 21001)

Pro Micro tracked loss = RX_FAIL / (received + RX_FAIL):

| Master | leg 1 -Os | leg 2 -O2 | leg 3 -Os |
|---|---|---|---|
| 21001 power (L15) | 3.34 % | 3.75 % | 3.42 % |
| 21002 HR (L15) | 4.19 % | 4.28 % | 4.61 % |
| 21003 CSC (L15) | 3.05 % | 3.71 % | 3.62 % |
| 24001 HR (5340) | 0.99 % | 0.57 % | 1.07 % |
| 24002 CSC (5340) | 0.74 % | 0.82 % | 0.99 % |
| 24003 power (5340) | 1.33 % | 0.75 % | 0.75 % |
| 23001 CSC (Dongle) | 1.48 % | 1.73 % | 1.73 % |
| 23002 power (Dongle) | 1.50 % | 0.33 % | 1.08 % |
| 52233 FE-C (trainer) | 29.30 % | 23.56 % | 24.75 % |
| **Garmin, 21001 (wall-clock basis)** | **3.90 %** (1156/1203) | **3.99 %** (1154/1202) | **5.83 %** (1132/1202) |

- On the same master in the same window, the Pro Micro beat or matched the Garmin in every leg.
- The L15 masters are the weak link for both receivers. The L15 sims themselves report 2.9-3.6 % fewer
  EVENT_TX than slots (5340 0.3-0.5 %, Dongle 0-1 %), HR worst on both sides, which accounts for most of the
  3-4.6 % both receivers see on 21001-21003. So the cause is the transmitter (the L15 runs the MPSL Zwift
  image), not placement.
- Trainer FE-C loss of 23-29 % is in line with the known FE-C problem (memory `zwift-captures-2026-11-15`,
  `fec-slot-crossing-2026-11-16`). Its accounting is 12-26 slots short, because its search periods
  (ch9 EVENT_RX_FAIL_GO_TO_SEARCH x1-3) do not raise RX_FAIL.
- EVENT_CHANNEL_COLLISION (0x09) on tracked channels: 2 / 3 / 1, matching page 11 `slots_collided`.
- Scan: 7.10 / 8.08 / 7.73 sightings/s. Each tracked device was sighted on the scan channel exactly once
  (1/67 opens), so the rate is mostly the trainer's three channels: 52233:0x0B and 0x7B heard in 67/67 opens, 0x11 in 2-4/67.

## Bench events and deviations

- **Aborted first attempt (`aborted_leg1_l15_silent/`).**
  - The L15 sim's three master channels opened OK, but no EVENT_TX came back and the Garmin heard none of them
    (pre-count 0/3 on the L15, 3/3 and 2/2 elsewhere). The run was stopped about 1 min into the window.
  - After an ant_probe reset, the L15 transmitted with 1, 2 and 3 masters in short tests (`pre/l15_*`), so the
    fault did not reproduce. Cause unknown: possibly state left from the Zwift session on that image.
  - `leg.py` then gained a reset of every master board before the sims, and an abort if the pre-count was
    short twice. All three real legs passed on the first try.
  - The Pro Micro had seen ~1 min of load in the aborted run, so it was reflashed before leg 1.
- The L15 was not reflashed, as instructed: it still carries the 3b486c3 Zwift MPSL image at 57600, and it
  served as a master host.
- The trainer was powered and heard (`pre/trainer_check_garmin.txt`), so it is in the workload as a tracked
  FE-C channel in every leg.
- Garmin reset with ant_probe after every ant_scan_rate run (pre-count, post-count, and the trainer check).

## Files

Only this README is in git. The files listed below live on the bench machine.

- `images/` holds the images, .configs, build script, O2 fragment, compile-flag check, flash logs and
  `md5sums.txt`.
- `leg{1_Os,2_O2,3_Os}/` holds the raw data per leg:
  - diag `diag_before.json`, `diag_after.json`, `diag_diff.txt`;
  - 0xF6 `health_before.txt`, `health_after.txt`;
  - DUT window `scan_pm.json` / `.txt`, Garmin window `garmin_verify.json` / `.txt`;
  - master counts `count_pre_try0.*` and `count_post.*`, sim logs `try0_sim_*.txt`;
  - `flash.log` (Pro Micro md5 and probe), `leg.log` and `meta.json` (every command with rc, start and duration).
- `leg.py`, `flash_pm.ps1` and `analyse.py` are the runner, the flash helper and the summariser.
  `summary.json` and `table.txt` are the derived numbers.

## Final board state (2026-11-16 ~10:37)

- Pro Micro: **-Os C2 image** (`images/promicro_Os.uf2`, 81dfb8be…), stack reset.
- nRF52840 Dongle: 3292198 direct apps/dongle (no diag, no MPSL), DFU zip f681ab24….
- nRF5340 DK: net core = 3292198 apps/dongle cpunet (ec3ac1be…); app core = promisc/appcore (45bd2a9f…).
- nRF54L15 DK: unchanged 3b486c3 Zwift MPSL image, COM4 @ 57600.
- Garmin: stock, reset.
