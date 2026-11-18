# C2 (TI part) and C3 rerun: TI LaunchXL pass breakdown and CRC fast path, after the UART fix (2026-11-16)

Plan: `C:\Users\Colin\.claude\plans\a-reviewer-had-some-shiny-prism.md`, steps C2 (TI legs) and C3. This reruns
`../ti/`, which stopped on the TI UART overrun defect (`../../2026-11-16-ti-cmd-stall/`, now fixed and merged).
Draft by the bench agent. **Result-checked 2026-11-16 by an independent agent: every number recomputes from
the raw files. The check's corrections and additions are in the next section and override the draft below
where they differ.**

## Result check (2026-11-16)

- **C3 verdict:** `crc_max_us` = 7 µs in all four legs, over ~13 400 CRC computations per leg (every in-window
  frame that matches its address: sync_entries − sw_addr_mismatch − window_drops; not ~1 500), timed under
  `irq_lock` with CYCCNT and floored, so < 8 µs. ≤ 10 µs → **Phase D does not trigger.** 7 µs also rules out
  the generic loop (~60 µs), so the fast path's init self-check passed on the board. The TX CRC is not timed
  separately (it sits inside p9 [2], 71–78 µs); there is no pre-B1 figure under masked timing (not needed).
- **Leg 4:** the accepted retry is the [5] = 444 arm, a cold arm: slack 600 − 444 − 150 = 6 = [6]. The refused
  arm was an in-grant (follow-on, 400 µs lead) arm; a cold arm needs [5] > 450 to be refused. Its backend lead
  was p4 min_lead 144 < 150, which puts its [5] at ~256 against the 250 µs in-grant budget. Same stale-`now`
  retry shape as nRF C2 leg 3.
- **Margin — the binding TI budget is the in-grant one, 400 − 150 = 250 µs**, not the cold 450. The tightest
  accepted arms in every clean leg are in-grant arms: [6] 21/22 µs at -Os and 72 µs on -O2 leg 2, each
  confirmed by min_lead − 3 µs. The cold -Os passes (319–323 µs) already exceed 250 µs, so any pass that long
  made in a follow-on context would be refused. The draft's "slack far below 450 − [5]" is wrong for leg 4.
- **-Os vs -O2:** per-frame fields are settled (rx_cb 491/488 → 423/421, drain 568/566 → 496/496, ~70 µs, two
  legs each). The pass reduction (~75–85 µs) rests on one clean -O2 leg, and -O2 does not remove refusals.
- **Pro Micro on the 5340 masters (14–22 %):** the same ba0a7084 image read 0.7–1.6 % on these masters in the
  B4 pair, so the image alone is not the cause. The Pro Micro's 20 s counts on the 5340 masters also fall
  (76–80 → 60–63 sightings). The extra ~600–750 lost frames per leg appear as empty windows and missed slots
  (term_empty +600..+1050, slots_missed up), not CRC failures, sw_addr_mismatch or collisions — frames not
  detected (inference). Investigated separately: `../../2026-11-16-pm-5340-anomaly/`.
- **Pro Micro tx_drops:** the before/after reads bracket the scan process plus ~1 s after it exits, so these
  files cannot place the drops after the window (in-bracket rises +51/+45/+46/+45 per leg).

## Result

- **All four legs completed: A/B/A/B, 300 s each. The TI answered every command.**
  - The TI ran the stock `ant_scan_rate.py`, with no resend wrapper. It exited rc 0 after 315 s in every leg
    and printed no "no answer" line.
  - Every diag and health read before and after the window answered.
  - TI 0xF6 after each leg: `host_csum` 0, `host_desync` 0, `uart_rx_overruns` 0, `tx_high_water` 0,
    `tx_drops` 0, `tx_timeout` 0, `event_overflows` 0.
- **C3: `crc_max_us` = 7 µs in all four legs (both -Os and -O2), ≤ 10 µs. So Phase D does not trigger.**
  - This is page 8 [2], timed with interrupts masked in this diag build (c49fafb).
  - The first attempt's 32/19 µs readings included ISR preemption.
- **-Os vs -O2 on TI.** In the clean pair of legs, -O2 shortens the pass:
  - page 9 [5] 319/323 → 244 µs;
  - `pass_max` 394/399 → 313 µs;
  - `rx_cb_max` ~490 → ~422 µs;
  - `drain_max` (RAT) ~567 → 496 µs.
  - Leg 4 (-O2) contains one early-ETIME refusal plus its retry in one pass: [5] 444, `pass_max` 514,
    `pass_steps_max` 3, slack 6, `arm_rejected` 1. This is the refusal pattern of nRF C2 leg 3, so leg 4's worst
    case is not a clean -O2 figure.
- **Tracked loss, TI vs Pro Micro in the same window:**
  - L15 masters: TI is +0.1 to +0.6 pp worse.
  - Dongle masters: −0.2 to +0.6 pp.
  - 5340 masters: the Pro Micro control is the outlier (14–22 % against the TI's 0.8–1.6 %).
  - Trainer FE-C: TI +16 to +18 pp worse.

## Bench

| Role | Board | Image |
|---|---|---|
| DUT | TI LaunchXL CC2652R, XDS110 `L1100TY2`, ANT UART COM11 @ 57600 | A = `images/ti2_os.hex` **d6f0421947cf2bcaff80c4aae9c621e6**; B = `images/ti2_o2.hex` **c4e5eb81765eed9412a633603ece3040** |
| Control + master counter | Pro Micro `EE6986854FD72803` | B4 -O2 diag image ba0a7084 (not reflashed; ant_probe PASS) |
| Masters | L15 COM4 @ 57600 (21001 power, 21002 HR, 21003 CSC), 5340 net COM5 (24001 HR, 24002 CSC, 24003 power), Dongle `3D55F77818BE772A` (23001 CSC, 23002 power) | unchanged: the L15 keeps the 3b486c3 Zwift MPSL image; nothing reflashed |
| Air | trainer #52233 (heard on 0x0B/0x11/0x7B, `pre/trainer_check_pm.*`, and in every pre/post count) | tracked as 52233:0x11 |

**Workload.** It is identical to the nRF C2 legs and to `../ti/`:
- 8 `ant_sim` masters on three boards with mixed periods (power 8182, HR 8070, CSC 8086). The sims run for 480 s.
- A 300 s window with the scan channel reopened every 4.5 s.
- 9 tracked channels: the 8 masters plus 52233:0x11.

**Control.** The TI and the Pro Micro each run that workload in their own process, from one shared `--start-at`.

**Leg sequence.** For each leg:
1. Flash the TI fresh (`flash_ti_leg.ps1` → `C:\wt_c2\scripts\flash_ti.ps1 -Serial L1100TY2`: dslite verify,
   xds110reset, then ant_probe PASS).
2. Reset all three master boards (ant_probe).
3. Start the sims.
4. Take the **master count: a Pro Micro 20 s `ant_scan_rate`, before and after.**
5. Read TI and Pro Micro `ant_diag` (all pages) and `ant_health`.
6. Run the window.
7. Read diag and health again, then take the post-count.

**Runner.** `leg_ti2.py` is `../ti/leg_ti.py` with one change: the TI side uses the stock `ant_scan_rate.py`, so an
unanswered command fails the leg instead of being resent. `run_leg2.ps1` drives one leg (flash, then leg). The
exact commands are in `commands.txt` and each leg's `meta.json`.

## Images

- **Source.** Built in `C:\wt_c2`, branch `bench/c2-nrf` at **5584dea** (= local master). `git status` was clean
  before and after.
- **Toolchain.** NCS v3.4.0 with toolchain dcbdc366a1. The script is `images/ti2_builds.ps1`.
- **Configuration.** The build is the build_all row `ti_launchxl_diag`: `apps/dongle_ti`, `-DANT_RADIO=core
  -DRADIANT_BACKEND=cc26xx`, hal_ti module, and `EXTRA_CONF_FILE=ti_health.conf;ti_diag.conf`.
  - A adds `images/os.conf` (`CONFIG_RADIANT_HOT_PATH_O2=n`).
  - B is as shipped (`prj.conf` has O2=y).
- **`.config` read-back** (`images/ti2_*.config`).
  - Both images have:
    - `RADIANT_DIAG_COUNTERS=y`, `ANT_DONGLE_DIAG=y`, `ANT_DONGLE_HEALTH_COUNTERS=y`;
    - `RADIANT_CRC_REPAIR=y`, `RADIANT_PROFILE_SANITY=y`, `RADIANT_SEARCH_SHORT_MATCH=y`, `CRC=y`;
    - `RADIANT_BACKEND_CC26XX=y`, `ANT_DONGLE_UART_BAUD=57600`, `LOG_MODE_DEFERRED=y`;
    - `RADIANT_ACK_FAULTS` not set.
  - `RADIANT_HOT_PATH_O2`: not set on A, `=y` on B.
- **`compile_commands.json`** (`images/compile_flags_check.txt`).
  - A has no file with `-O2`.
  - B has exactly `radiant_sched.c`, `ant_radio_radiant.c` and `radiant_radio_cc26xx.c` at `-Os -O2`.
  - `radiant_search.c` is `-Os` on both.
- **B1 is in.** `nm` finds `crc16_itu_t` and `crc_fast_ok` in both ELFs.
- **Size** (text/data/bss):
  - A: 86168/1350/47022;
  - B: 91228/1350/47030 (+5060 B text).
- **md5s** of the hex, ELF and .config are in `images/md5sums.txt`. The flash logs record the hex md5 before each
  flash, and dslite reported "Program verification successful" each time.

## Per-leg TI numbers (diff over the leg; level fields are the since-boot value at the after-read, boot = the leg's flash)

| Field | leg1 A -Os | leg2 B -O2 | leg3 A -Os | leg4 B -O2 |
|---|---|---|---|---|
| TI flash done | 15:44:44 | 15:53:21 | 16:02:05 | 16:10:38 |
| p8 [2] **crc_max_us** | **7** | **7** | **7** | **7** |
| p8 frame_max_us | 102 | 102 | 108 | 96 |
| p8 rx_cb_max_us | 491 | 423 | 488 | 421 |
| p8 drain_max_frames | 1 | 1 | 1 | 1 |
| p8 pass_max_us | 394 | 313 | 399 | 514 |
| p8 pass_steps_max | 2 | 2 | 2 | 3 |
| p8 pass_over_100us / p0 sched_dones | 22145 / 21195 | 21404 / 21109 | 22074 / 21110 | 21371 / 21132 |
| p8 drain_over_250us | 10256 | 10249 | 10232 | 10216 |
| p4 drain_max_us (RAT) | 568 | 496 | 566 | 496 |
| p4 min_lead_us | 175 | 225 | 174 | 144 |
| p9 [0] sweep | 95 | 68 | 103 | 90 |
| p9 [1] decide | 176 | 143 | 176 | 224 |
| p9 [2] arm call | 78 | 71 | 78 | 76 |
| p9 [3] done callbacks | 148 | 137 | 149 | 138 |
| p9 [4] book | n/m | n/m | n/m | n/m |
| p9 [5] pass→arm | 319 | 244 | 323 | 444 |
| p9 [6] min slack | 22 | 72 | 21 | 6 |
| p9 [7] early ETIME | 0 | 0 | 0 | **1** |
| p10 [0-3] repaired / unrepairable / refuted / implausible | 9 / 27 / 0 / 0 | 12 / 35 / 0 / 0 | 15 / 43 / 0 / 0 | 6 / 32 / 0 / 0 |
| p10 [4]/[5] | n/m (nRF only) | n/m | n/m | n/m |
| p10 [6] t_sync_max_us | 2 | 2 | 2 | 2 |
| p10 [7] match_max_us | 29 | 32 | 29 | 10 |
| p7 trk_open_late / max µs | 33 / 213 | 34 / 222 | 41 / 232 | 34 / 213 |
| p7 trk_missed_behind_scan | 0 | 0 | 0 | 0 |
| p7 [0-2], [6-7] | n/m (nRF only) | n/m | n/m | n/m |
| p11 merged / preempted / replans / arm_rejected | 226 / 0 / 0 / 0 | 241 / 0 / 0 / 0 | 226 / 0 / 0 / 0 | 232 / 0 / 0 / 1 |
| p11 tx_armed / stale / collided / turn_yields | 0 / 66 / 12 / 0 | 0 / 61 / 11 / 0 | 0 / 64 / 12 / 0 | 0 / 65 / 11 / 0 |
| p12 stale_rx_frame / rx_end / filter_index / tx | 0 / 66 / 0 / 0 | 0 / 61 / 0 / 0 | 0 / 64 / 0 / 0 | 0 / 65 / 0 / 0 |
| p0 sched_missed / slots_missed | 12 / 783 | 11 / 738 | 12 / 805 | 11 / 777 |
| p4 crc_fail | 1260 | 1176 | 1312 | 1264 |
| p5 windows / frames_ok / acquired | 996 / 1535 / 15 | 970 / 1630 / 18 | 992 / 1626 / 16 | 1001 / 1603 / 20 |
| 0xF6 host_csum / host_desync / **uart_rx_overruns** | 0 / 0 / **0** | 0 / 0 / **0** | 0 / 0 / **0** | 0 / 0 / **0** |
| 0xF6 tx_high_water / tx_drops / tx_timeout / event_overflows | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |

- Raw data:
  - `<leg>/ti_diag_{before,after}.json`, `ti_diag_diff.txt` and `ti_health_*.txt`;
  - the same files with the `pm_` prefix for the Pro Micro;
  - `summary.json` and `table.txt` (`analyse_ti2.py`).
- The TI's `uart_rx_overruns` reads a real 0 before and after each leg. It is not the 0xFFFF sentinel.
- Stale events (61–66) ≈ the scan channel's host closes (66 TI opens per leg). This matches the nRF C2 reading.

## -Os vs -O2 on TI (per image: worst / mean of its two legs)

| Field | A -Os worst / mean | B -O2 worst / mean | B, clean leg 2 only |
|---|---|---|---|
| crc_max_us | 7 / 7 | 7 / 7 | 7 |
| frame_max_us | 108 / 105 | 102 / 99 | 102 |
| rx_cb_max_us | 491 / 489.5 | 423 / 422 | 423 |
| drain_max_us (p4, RAT) | 568 / 567 | 496 / 496 | 496 |
| pass_max_us | 399 / 396.5 | 514 / 413.5 | 313 |
| p9 [0] sweep | 103 / 99 | 90 / 79 | 68 |
| p9 [1] decide | 176 / 176 | 224 / 183.5 | 143 |
| p9 [2] arm call | 78 / 78 | 76 / 73.5 | 71 |
| p9 [3] done callbacks | 149 / 148.5 | 138 / 137.5 | 137 |
| p9 [5] pass→arm | 323 / 321 | 444 / 344 | 244 |
| p9 [6] min slack (smaller is worse) | 21 / 21.5 | 6 / 39 | 72 |
| early ETIME | 0 / 0 | 1 / 0.5 | 0 |

- **Per-frame fields (one frame each, no load dependence).**
  - `rx_cb_max` drops ~68 µs at -O2 (491/488 → 423/421).
  - `drain_max` drops ~71 µs (568/566 → 496/496).
  - `crc_max` and `t_sync` do not move. They are already at the floor.
- **Pass fields.**
  - The two A legs agree within 5 µs ([5] 319/323, `pass_max` 394/399). So, unlike the nRF C2 legs, the TI's
    -Os worst case repeats.
  - B leg 2 is 75 µs lower on [5] and 81–86 µs lower on `pass_max`.
  - B leg 4's worst case is one refused arm (early ETIME 1, `arm_rejected` 1) re-armed inside the same pass
    (`pass_steps_max` 3). It is the same shape the nRF result check found in nRF leg 3: a retry plans from the
    stale pass `now`. Its sub-fields that are not part of that retry stay at or below A: [0] 90, [2] 76, [3] 138.
  - My reading, not established: -O2 shortens the TI pass by ~75–85 µs. Leg 4 shows that a refusal and its
    retry can still occur at -O2, which is the open "re-read now on STEP_RETRY" item.

## C3 verdict

**`crc_max_us` after B1 = 7 µs in every leg (4/4, both optimisation levels), which is ≤ 10 µs. Phase D does not
trigger.**
- The value is page 8 [2] from the honest-timing diag build: the CRC is timed under `irq_lock`. The single worst
  case over ~21 000 scheduler dones and ~1 500 scan frames per leg is 7 µs, and identical across four fresh boots.
- For the record, the earlier figures were 70 µs (pre-B1, preemptible timing) and 32/19 µs (B1, preemptible
  timing).
- Per the plan, the main session records in `ti-rf-core-facts` that the fast path closed item 4.
- **Not done in this step:** the plan's C3 also lists "FE-C acked TX unchanged (`fec_ack_sweep.py`)" and a
  pre-B1 leg.
  - The pre-B1 reference stays `../ti/leg4_preB1` (70 µs). It was not rerun: its image lacks the UART fix, so
    it would stall again.
  - FE-C acked TX was not rerun. `fec_ack_sweep.py` still needs apps/trainer on a DK, and this step may flash
    only the TI. The substitute against the real trainer is in `../ti/fec/`.

## TI vs Pro Micro, same window: tracked loss per master (%, TI / PM / TI−PM)

| Master | leg1 A | leg2 B | leg3 A | leg4 B |
|---|---|---|---|---|
| 21001 power (L15) | 4.14 / 3.75 / +0.39 | 3.54 / 3.42 / +0.12 | 4.06 / 3.66 / +0.40 | 4.22 / 3.74 / +0.48 |
| 21002 HR (L15) | 4.67 / 4.11 / +0.56 | 4.23 / 3.94 / +0.29 | 5.00 / 4.52 / +0.48 | 4.47 / 3.94 / +0.53 |
| 21003 CSC (L15) | 3.72 / 3.46 / +0.26 | 4.25 / 3.87 / +0.38 | 3.88 / 3.30 / +0.58 | 3.89 / 3.62 / +0.27 |
| 23001 CSC (Dongle) | 2.00 / 1.40 / +0.60 | 1.41 / 1.56 / −0.15 | 1.75 / 1.23 / +0.52 | 1.25 / 1.23 / +0.01 |
| 23002 power (Dongle) | 0.84 / 0.42 / +0.43 | 0.67 / 0.83 / −0.16 | 0.34 / 0.17 / +0.17 | 0.59 / 0.33 / +0.26 |
| 24001 HR (5340) | 1.41 / 17.60 / −16.19 | 1.08 / 16.10 / −15.02 | 1.33 / 21.79 / −20.46 | 1.58 / 20.63 / −19.06 |
| 24002 CSC (5340) | 0.83 / 13.80 / −12.96 | 1.08 / 14.21 / −13.13 | 1.17 / 20.85 / −19.68 | 1.50 / 21.07 / −19.57 |
| 24003 power (5340) | 1.01 / 16.84 / −15.83 | 1.18 / 14.35 / −13.17 | 1.35 / 20.67 / −19.32 | 1.34 / 20.65 / −19.31 |
| 52233:0x11 trainer FE-C | 44.81 / 26.55 / +18.26 | 43.27 / 27.48 / +15.80 | 46.54 / 30.18 / +16.36 | 44.49 / 27.20 / +17.29 |

Loss is context only (plan C2: the timing is the result).
- **On the L15 and Dongle masters**, the TI and the Pro Micro agree within 0.6 pp. The TI is the worse of the two
  in 18 of 20 cells.
- **On the 5340 net-core masters**, the Pro Micro control loses 14–22 %, rising over the session, while the TI
  loses 0.8–1.6 %. This is a Pro Micro ↔ 5340 link effect: the control is the outlier, not the TI. In the first
  attempt the Pro Micro read 0.6–3.6 % on these masters. What changed is not established. One candidate is the
  Pro Micro's image, which changed from the C2 -Os image to the B4 -O2 image. I did not investigate this; it may
  need a look.
- **On the trainer**, the TI loses 16–18 pp more than the Pro Micro, consistently, on both images. TI
  sensitivity to this one sensor is a link property, unchanged between -Os and -O2.
- The Pro Micro's 0xF6 `tx_drops` rises across the session (53 → 121 → 192 → 262, since its boot). Its
  `tx_high_water` is 32. These match the known after-window drops (channels left open with no host reading),
  not an in-window effect. The TI's `tx_high_water` is 0 in every leg, so item 6 needs nothing on TI.

## Master counts and elapsed time

- **Master counts.** The Pro Micro took a 20 s count before and after every leg: **8/8 each time** (l15 3/3,
  n53 3/3, dongle 2/2). The trainer was present in every count.
- **Elapsed time.** TI and Pro Micro scan processes: rc 0 and 315.1–315.2 s in every leg. The scan duration was
  300.0 s. Opens: TI 66, Pro Micro 67 per leg.
- **Commands.** No step in any leg returned a non-zero rc (`meta.json`).
  - The exceptions are the L15 and 5340 `ant_sim` processes, which exit rc 1 with "EVENT_TX pacing did not hold
    ... wall-clock fallback". That exit is identical in every earlier nRF and TI leg (`../leg*/`, `../ti/*/`).
    The payloads still went out.
- **TI after the run.** `ti_reset_end` (ant_probe) passed at the end of each leg.

## TI margin, C4 style (report only)

- **The TI's constants** (`radiant/src/radiant_radio_cc26xx.c`):
  - `MIN_ARM_LEAD_US` 600 is the scheduler's lead (caps.min_arm_lead_us);
  - `ARM_FLOOR_US` 150 is the refusal floor at the backend's early check (`at < now + 150` → ETIME);
  - `IN_GRANT_LEAD_US` 400 is the follow-on lead.
- **The analogue of the nRF's 240 µs budget** (328 − 88) is therefore 600 − 150 = **450 µs** for a cold arm.
  For an in-grant arm it is 400 − 150 = 250 µs.
- **page 9 [5] worst against the 450 µs budget:**
  - A: 319 / 323 µs, margin ≥ 127;
  - B: 244 µs (margin 206) on the clean leg;
  - B leg 4: 444 µs, margin 6. That is the refusal-plus-retry pass.
- **page 9 [6], the smallest slack at the 150 µs floor on accepted arms:**
  - A: 22 / 21 µs;
  - B: 72 / 6 µs.
  - In every leg the slack is far below 450 − [5]. So the tightest accepted arms were not planned at the full
    600 µs lead; they are probably follow-on or merged arms nearer the floor (inference, not shown). A slack
    of 6 µs on B leg 4 means one accepted arm came within 6 µs of refusal in that leg.
- **Refusals.** early ETIME stays 0 in three of four legs and is 1 in leg 4. `trk_missed_behind_scan` is 0
  everywhere.

## Bench events and deviations

- No aborted leg, stall or retry. The TI was flashed 4 times, once per leg, and nothing else was flashed.
- No firmware or repo source was changed, and nothing was committed. The only file written in `C:\wt_c2` is
  `build/os_off.conf`, under `build/`, which is gitignored; `git status` is clean.
- The masters were reset before each leg, and the pre-count was 8/8 on the first try every time.

## Final board state

- **TI:** B image (-O2, shipping config + diag) `images/ti2_o2.hex` c4e5eb81…, flashed 16:10, ant_probe PASS at
  the end of leg 4.
- **Pro Micro:** unchanged, B4 -O2 diag ba0a7084.
- **L15, 5340, Dongle:** unchanged. Their sims exited on their 480 s timer.

## Files

- `images/`: the two hex/ELF/.config sets, `md5sums.txt`, `sizes.txt`, `compile_flags_check.txt`,
  `ti2_builds.ps1` and `os.conf`.
- `leg_ti2.py`, `run_leg2.ps1`, `flash_ti_leg.ps1`, `analyse_ti2.py` → `summary.json`, `table.txt`, and
  `commands.txt`.
- `pre/`: board probes and the trainer check.
- `leg{1..4}_*/`: `flash.log`, `leg.log`, `meta.json`, `runner.out`, diag/health before/after and diff (TI and
  PM), `scan_{ti,pm}.{json,txt}`, `count_*`, and the sim logs. `leg*.run.txt` holds each leg's driver output.
