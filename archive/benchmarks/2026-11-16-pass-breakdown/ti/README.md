# C2 (TI part) and C3: TI LaunchXL pass breakdown and CRC fast path (2026-11-16)

Plan: `C:\Users\Colin\.claude\plans\a-reviewer-had-some-shiny-prism.md`, steps C2 (TI legs) and C3. Draft by
the bench agent; not yet result-checked. **The C2-TI A/B/A was NOT completed. The run stopped on a bench
failure that needs a decision (below).**

## Result in one paragraph

**No TI leg completed the C2 workload.** On both the B1 image (3292198) and the pre-B1 image (dd17fc1), the
TI stops answering host commands 5-40 s into the 300 s window. This happens during the 4.5 s close/unassign/
reassign/reopen cycle of the scan channel, while its 9 tracked channels keep running and keep sending events.
`ant_scan_rate`'s TI side then fails with "-> no answer". Four attempts failed this way: three on B1 -Os and one
on pre-B1. A 150 s diagnostic with a 15 s command timeout showed an ASSIGN unanswered for the full 15 s.

The stall is sometimes transient: in 3 of 4 legs the post-window `ant_diag` read worked. Sometimes it is not:
in `aborted_leg1c_cmd_stall/` the TI ignored every host command, including the reset, for more than 2 min
after the window. The radio kept streaming events throughout.

Every affected TI run also shows 0xF6 `host_csum` 0 → 1 and `host_desync` 0 → 5-7. That fits host RX bytes
being dropped while the command path is blocked (my inference, not shown). The Pro Micro, on the same workload
in the same window, completed every leg.

So the TI timing below comes from legs whose scan workload ended early and at different times. It is
**indicative, not an A/B**. The -O2 leg was never run.

**C3 reading (with that caveat):**
- `crc_max_us` (page 8) is **32 and 19 µs** on B1 -Os (two partial legs) against **70 µs** on pre-B1 (one partial
  leg).
- **That is above the plan's 10 µs threshold, so Phase D triggers as written.** The trigger is the main
  session's and the owner's call. Read the caveat in "C3 verdict" before acting on it.
- FE-C acked TX against the real trainer: TI completes 74.0 % (pre-B1) and 84.0 % (B1 -Os). The Pro Micro
  control right after each run completed 73.5 % and 66.0 %. That is not worse, but the run was not same-window,
  and `fec_ack_sweep.py` was not run (below).

## Bench and substitutions

| Role | Board | Image (md5 of .hex) |
|---|---|---|
| DUT | TI LaunchXL CC2652R, XDS110 `L1100TY2`, ANT UART COM11 @ 57600 | B1 -Os `images/ti_b1_Os.hex` **fa85be8bd120702af71aed4a91958b24**; B1 -O2 `images/ti_b1_O2.hex` **991ab67c39efd951904a032d4eb5d495** (built, never run on air); pre-B1 `images/ti_preB1_dd17fc1.hex` **9725606d5bdc916ba9b457add5174fc1** |
| Control + master counter | Pro Micro `EE6986854FD72803` | C2 -Os image 81dfb8be… (not reflashed) |
| Masters | L15 COM4 @ 57600 (21001-21003), 5340 net COM5 (24001-24003), Dongle `3D55F77818BE772A` (23001-23002) | unchanged from the nRF legs (`../README.md`) |
| Air | trainer #52233 (heard: 0x0B, 0x11, 0x7B; `pre/trainer_check_pm.txt`) | tracked as 52233:0x11 |

- **The Garmin USB-m is no longer on the bench** (the owner swapped the TI in for it). The loss reference is now
  a same-window control: the Pro Micro tracks the same 8 masters plus the trainer, with the same 4.5 s reopen
  scan, from the same start time. It also does the 20 s master counts before and after each leg (8/8 both
  times in every leg).
- Masters, device numbers, periods, the 480 s sims and the 300 s window are the nRF legs'. The runner is
  `leg_ti.py`, adapted from `../leg.py`.
- `ant_scan_rate.py` multi-device mode was used in the first attempt. After that the TI and the Pro Micro run
  as two single-device processes with one shared `--start-at`, so that each side keeps its own log. The TI side
  runs through `scan_ti_retry.py`, which resends an unanswered command up to 4 times, 0.5 s apart. It did not
  rescue any leg.
- Each leg starts with a TI flash (`flash_ti_leg.ps1` → `scripts/flash_ti.ps1 -Serial L1100TY2`: dslite,
  verify, xds110reset, then ant_probe PASS). The TI therefore starts from a fresh boot, and its before-read is
  all zero.

## Images and build

The B1 images were built from `C:\wt_c2` (branch `bench/c2-nrf` at **3292198**, worktree clean) with NCS
v3.4.0 and toolchain dcbdc366a1 (Zephyr SDK 1.0.1). That is the same bundle as the pre-B1 image, whose
CMakeCache shows `ZEPHYR_BASE=C:/ncs/v3.4.0/zephyr` and SDK dcbdc366a1. The script is `images/ti_builds.ps1`.
It reproduces the build_all row `ti_launchxl_diag` by hand: `apps/dongle_ti -b cc26x2r1_launchxl --
-DANT_RADIO=core -DRADIANT_BACKEND=cc26xx -DEXTRA_ZEPHYR_MODULES=C:/ncs/v3.4.0/modules/hal/ti
-DEXTRA_CONF_FILE=ti_health.conf;ti_diag.conf` (plus `;C:/wt_c2/build/o2.conf` for -O2).

- `.config` read-back (`images/*.config`):
  - **B1 -Os and -O2 both have:** `RADIANT_DIAG_COUNTERS=y`, `ANT_DONGLE_DIAG=y`, `ANT_DONGLE_HEALTH_COUNTERS=y`,
    `RADIANT_BACKEND_CC26XX=y`, `RADIANT_CRC_REPAIR=y`, `RADIANT_PROFILE_SANITY=y`, `RADIANT_SEARCH_SHORT_MATCH=y`,
    `CRC=y`, `ANT_DONGLE_UART_BAUD=57600`, `LOG_MODE_DEFERRED=y`, and `# RADIANT_ACK_FAULTS is not set`.
  - `RADIANT_HOT_PATH_O2` is not set on -Os and `=y` on -O2.
  - **pre-B1** (`C:\wt_diag\build\ti_diag`, branch perf/diag-pages at **dd17fc1**, clean, built 2026-11-15
    15:48 after the commit; 46b79cc is not an ancestor) has the same symbols, except that `CRC`, `CRC_REPAIR` and
    `PROFILE_SANITY` are absent and the O2 symbol does not exist yet.
  - **So pre-B1 vs B1 also differs by CRC repair being on** (C1). Page 10 [0-3] read "not measured" on pre-B1.
- `-O2` placement (`images/compile_flags_check.txt`, from compile_commands.json):
  - on -O2, `radiant_sched.c`, `ant_radio_radiant.c` and `radiant_radio_cc26xx.c` compile with `-Os -O2`;
  - `radiant_search.c` keeps `-Os` only;
  - on -Os and on pre-B1, all four files compile with `-Os`.
- B1 is in: `nm` shows `crc16_itu_t` and `crc_fast_ok` in both B1 ELFs and in neither pre-B1 ELF.
- Size (text/data/bss):
  - B1 -Os: 85784/1350/47013;
  - B1 -O2: 90844/1350/47021 (+5060 B text);
  - pre-B1: 84032/1349/46972.
- `images/md5sums.txt` lists the hex and config md5s.

## What happened, attempt by attempt

| Dir | Image | Masters pre/post | TI side of the window | TI 0xF6 after |
|---|---|---|---|---|
| `aborted_leg1_scan_csum/` | B1 -Os | 8/8 / 8/8 | stock multi-device `ant_scan_rate`: "open scan -> no answer" (time not logged; child log was in a temp dir) | host_csum 1, host_desync 5 |
| `aborted_leg1b_scan_csum/` | B1 -Os | 8/8 / 8/8 | "scan period -> no answer" at t=39.3 s (process time; window opened ~15 s in), 1 resend also unanswered | host_csum 1, host_desync 6 |
| `aborted_leg1c_cmd_stall/` | B1 -Os | 8/8 / 8/8 | stalls at 21 s and 40 s, each answered on the 2nd resend; then "scan frequency" unanswered for 4 resends from 45 s | not readable: diag, health and the final ant_probe reset all unanswered (`ti_reset_end.txt`: events still streaming on ch7-9) |
| `leg4_preB1/` (the C3 leg) | pre-B1 | 8/8 / 8/8 | ASSIGN unanswered at t=24.9 s; the resend gets code 21 (wrong state), so the first ASSIGN was processed but its answer never arrived | host_csum 1, host_desync 7 |
| `diag_latency_preB1/` (diagnostic, 150 s, 15 s timeout) | pre-B1 | sims started, no count | 3 clean reopens (each command answered in ≤ 16 ms), then the ASSIGN after the 4th close was unanswered for 15 s (`latency.json`) | 0xF6 unanswered 3 s later; ant_probe reset passed ~9 s after that |

- **The Pro Micro side ran all 300 s in every leg:** 67 opens, rc 0, about 315 s elapsed per process.
- After the TI child fails, the TI's tracked channels keep running for the rest of the window. Whether its scan
  channel stays open depends on where the sequence broke: page 5 `windows` reads 105 / 407 / 60. So the TI's
  load differs between legs.
- **Both images show it, so it is not caused by B1, B3 or C1.**
- Not established: whether the command thread is blocked or the replies are lost (leg4's code 21 says a reply
  was lost or very late), and whether it needs the scan reopen. A tracked-only TI leg would answer the second
  question; it was not run.

## TI timing from the partial legs (diff over the leg; levels are the after value, since boot)

| Field | leg1 B1 -Os | leg1b B1 -Os | leg4 pre-B1 |
|---|---|---|---|
| p8 `crc_max_us` | **32** | **19** | **70** |
| p8 `frame_max_us` | 101 | 111 | 135 |
| p8 `rx_cb_max_us` | 594 | 585 | 566 |
| p8 `pass_max_us` | 412 | 691 | 390 |
| p8 `pass_steps_max` | 2 | 3 | 2 |
| p8 `pass_over_100us` / p0 `sched_dones` | 12304 / 12442 | 14432 / 14493 | 11644 / 11849 |
| p8 `drain_over_250us` | 10241 | 9387 | 10268 |
| p4 `drain_max_us` (RAT) | 673 | 672 | 678 |
| p4 `min_lead_us` | 175 | 75 | 198 |
| p9 [0] sweep | 98 | 133 | 90 |
| p9 [1] decide | 174 | 338 | 155 |
| p9 [2] arm call | 78 | 85 | 84 |
| p9 [3] done callbacks | 149 | 149 | 69 (pre-fix, not comparable) |
| p9 [4] book | not measured | not measured | not measured |
| p9 [5] pass→arm | 336 | 615 | 309 |
| p9 [6] min slack | 22 | 18 | 44 |
| p9 [7] early ETIME | 0 | **1** | 0 |
| p10 [0-3] repaired / unrepairable / refuted / implausible | 24 / 53 / 1 / 0 | 31 / 50 / 0 / 0 | not measured (no repair) |
| p10 [6] `t_sync_max_us` | 14 | 14 | 14 |
| p10 [7] `match_max_us` | 22 | 31 | 33 |
| p7 `trk_open_late` / max µs | 43 / 235 | 77 / 586 | 46 / 235 |
| p7 `trk_missed_behind_scan` | 0 | 0 | 0 |
| p11 merged / arm_rejected / stale / collided | 237 / 0 / 5 / 13 | 414 / 1 / 5 / 38 | page absent |
| p12 `stale_rx_end` / others | 5 / 0 | 5 / 0 | page absent |
| p0 `sched_missed` / `slots_missed` | 13 / 884 | 38 / 799 | 12 / 764 |
| 0xF6 `tx_high_water` / `tx_drops` | 0 / 0 | 0 / 0 | 0 / 0 |

Pages 7 [0-2], 7 [6-7] and 10 [4-5] are nRF-only fields and read "not measured" on TI. leg1c has no
after-read. Raw data is in `summary_partial.json` (`summarise_partial.py`) and in each leg's
`ti_diag_{before,after}.json` and `ti_diag_diff.txt`.

- **The per-frame fields are the best comparison here,** because each covers one frame regardless of how long
  the scan ran. `crc_max_us` 70 → 32/19 and `frame_max_us` 135 → 101/111 both drop with B1. `t_sync` (14) and
  `match` (22-33) do not move.
- **The pass fields ([0], [1], [5], `pass_max`) track the scan load, which differed between legs.** leg1b ran
  its scan channel longest (407 windows) and has the largest pass values. Do not read -Os vs pre-B1 from them.
- leg1b had page 9 [7] early ETIME = 1 and page 11 `arm_rejected` = 1. That is the same pattern as nRF leg 3.
- **-Os vs -O2 on TI: no data.** The -O2 image was built and checked but never ran on air, because the stall
  stopped the A/B/A first.

## C3 verdict

- **`crc_max_us` after B1 = 32 and 19 µs (> 10).** By the plan's wording this **triggers Phase D**. Before
  acting on it:
  - `crc_max_us` is a single worst case since boot over one ~15-byte `crc16_itu_t()` call. It is timed on the
    cycle clock inside `rf_callback` → `rx_drain()`, with interrupts enabled, so an ISR that preempts that span
    lands in the worst case.
  - The typical fast-path cost is likely a few µs. I infer that from the code; it was not measured, since there
    is no histogram.
  - The two B1 legs disagree by 13 µs. So the number above 10 may be preemption, not CRC work.
  - A hardware CRC would not remove preemption either.
  - This is for the main session and the owner. It is not decided here.
- **pre-B1 vs B1 `crc_max_us`: 70 → 32 / 19.** pre-B1 is one partial leg; its page 8 is already on the cycle
  clock (A3).
- **Tracked loss, TI minus Pro Micro, same window: not available.** No TI leg produced a scan result, because
  the TI child died. Pro Micro loss is in `summary_partial.json` (per master 0.5-4.9 %, trainer 28-44 %).
- **FE-C acked TX:** see below.

## FE-C acked TX (`fec/`)

`tools/fec_ack_sweep.py` was **not run**, for two reasons:
- It requires apps/trainer on a DK (`--sim-port` is mandatory), and this step may flash only the TI.
- Its originator opens a UART at 115200 only, while the TI runs at 57600.

Substitute: `fec_acked_real.py` reuses fec_ack_sweep's own `open_originator()`, `send_one()` and
`page51_tagged()` unchanged, with the baud bound to 57600. It runs against the **real trainer #52233**:
200 tagged page 51 commands at 1 Hz, and it counts `TRANSFER_TX_COMPLETED` against `FAILED`. Without the
sim's ground truth there is no window_missed / not_heard split. The Pro Micro ran the same command
immediately after each TI run, as a room reference. Each pair is adjacent in time, not in the same window: two
originators commanding one trainer would collide. No sims were running.

| Run | Start | Completed | Failed | % |
|---|---|---|---|---|
| TI pre-B1 | 11:33:44 | 148 | 52 | 74.0 |
| Pro Micro (after it) | 11:37:31 | 147 | 53 | 73.5 |
| TI B1 -Os | 11:42:27 | 168 | 32 | 84.0 |
| Pro Micro (after it) | 11:46:06 | 132 | 68 | 66.0 |

- TI minus Pro Micro: +0.5 pp on pre-B1, +18 pp on B1 -Os.
- The Pro Micro moved 7.5 pp between its two runs, which shows how much the room moves on its own. So this
  says "not worse", nothing finer.
- `page71_seen` is 0 everywhere: the tool saw no page 71 from this trainer, so there is no command-applied
  count.

## Bench events and deviations

- Four aborted or partial legs and one diagnostic, listed above. Nothing was retuned, and no firmware or repo
  file was changed. The new scripts live only in this folder.
- The TI was flashed 5 times: B1 -Os before each of the three B1 attempts, pre-B1 before leg4, and B1 -Os at
  the end. All went through `scripts/flash_ti.ps1`, and every one verified and probed PASS. No other
  board was flashed.
- The L15 kept its 3b486c3 Zwift MPSL image. Masters were reset before every leg, and the pre-count was 8/8 on
  the first try every time.

## Files

- `images/`: the hex files, .configs, `md5sums.txt`, `ti_builds.ps1`, `o2.conf`, `check_flags.py` and
  `compile_flags_check.txt`.
- `leg_ti.py` (runner), `run_leg.ps1` (flash, then leg), `flash_ti_leg.ps1`, `scan_ti_retry.py`,
  `diag_cmd_latency.py`, `fec_acked_real.py`, `summarise_partial.py` → `summary_partial.json`. `analyse_ti.py`
  was written for complete legs and was not used.
- Per leg dir:
  - `flash.log` (md5, dslite verify, probe);
  - `leg.log` and `meta.json` (every command with rc, start and duration);
  - `ti_`/`pm_` `diag_before/after.json`, `diag_diff.txt` and `health_*.txt`;
  - `scan*.json`/`.txt`, `count_*` and the sim logs.
- `pre/trainer_check_pm.*` holds the trainer heard check (Pro Micro, 20 s, no sims).
- `fec/`: the four FE-C runs (JSON + txt), `flash_b1_Os.log`, `b1_times.txt`.

## Final board state (2026-11-16 ~11:50)

- TI: **B1 -Os** `images/ti_b1_Os.hex` fa85be8b…, flashed 11:41, ant_probe PASS at the end.
- Pro Micro: unchanged, C2 -Os image 81dfb8be…, ant_probe PASS.
- Dongle, 5340, L15: unchanged from the nRF legs. Their sims exited or were reset.
