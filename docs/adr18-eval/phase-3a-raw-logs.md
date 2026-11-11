# Phase 3 Stage A raw logs

All files are in `C:\adr18_logs\phase-3\`, outside the repository, unless a path says
otherwise. Measured 2026-11-10 on branch `wip/adr18-impl`. Report:
[phase-3a.md](phase-3a.md).

The host rebooted at 20:39 that day. `build_final_thread.log` (0 bytes) and the
`final` images are from the interrupted run and are not evidence. Files named `a2_*`
or marked below were produced after the reboot, at 8b357d7.

## Images

| File | What it is |
|---|---|
| `img_a2_gate.hex`, `..._config.txt`, `build_a2_gate.log` | **final `gate.conf` image**, 8b357d7, with `phase_diag`, `d2_stats`, `health` (SHA-256 `2a3eda98...`) |
| `img_a2_thread.hex`, `..._config.txt`, `build_a2_thread.log` | **final `thread.conf` image**, same confs (`ac69f659...`), `SESSION_COUNT=3` |
| `img_a2_noskip_thread.hex`, ... | 8b357d7 with `EXT_FAIL_RUN_OFF` 0xFFFFFFFF (`89907d9f...`) |
| `img_a2_norelease_thread.hex`, ... | as noskip, and the two early-release calls removed (`ef389529...`) |
| `img_skip_thread.hex`, `img_noskip_thread.hex` (+ configs) | single-session arms at 6742cf8 and f696729 (no-skip patched). The code differs only in the skip constant |
| `img_sweep_thread.hex` (+ config) | `LEAD_SWEEP` image at b79114e |
| `img_final_gate.hex`, `img_v_*.hex`, `img_fe2_gate.hex` (+ configs) | the bisect at b05bcff and its variants |
| `C:\adr18_logs\phase-1-d2\img_d2_gate_79ce02f.hex` | the prototype control on `gate.conf` |
| `C:\adr18_logs\phase-1-d2\img_d2_thread_s14.hex` | the Phase 1 image that asserted twice; positive control |

The `_config.txt` files hold `.config` read-back, the commit and the hash. The commit
line of the `a2_*` ones is UTF-16 from a PowerShell append; decoded it reads
`8b357d7b5534...`.

## Final readings at 8b357d7

| Path | What it shows |
|---|---|
| `twister_a2.txt` | Bluefin, first line the commit under test. 4297 of 4297, 14 scenarios |
| `twister_head.txt`, `zero_cost_head.txt` | both repeated at the commit carrying this report, which changes comments and documents only |
| `ztest_dk_gate.txt`, `ztest_dk_main.txt`, `ztest_dk_api.txt` | 5340 DK: 69, 986, 65 cases, no failures |
| `zero_cost_a2.txt` | every row identical against c4e1ef8 (mixed encoding: strip NULs to read it) |
| `size_{l15_bridge,l15_matter,n52840_matter}_a2.txt` | FLASH, RAM, `.config` and symbol checks |
| `a2_gate1/`, `a2_gate2/` | `gate.conf`, 120 s, ch0 #2002, ch1 absent #9999, Garmin on #2002. DUT 0.0 %, Garmin 0.21 and 0.41 %, `hi 487/487/0/487`, `t0fix rel=487 bs=0` |
| `a2_d2ctrl/` | prototype between them. DUT 0.0 %, Garmin 0.82 %, `t0fix rel=0 bs=487` |
| `a2_ack/` | 59/60 slave, 60/60 master. Ran beside an image build |
| `a2_ack2/` | 59/60, 58/60, quiet host |
| `a2_ack_ctrl/` | prototype: 58/60, 57/60 |
| `a2_ack3/` | 60/60, 60/60 |

## Soaks

Each has `-console.log`, `-dut.log`/`.json`, `-ref.log`/`.json`, `-peer.log`,
`-sim.log`, `-summary.json`. `dut_rc` 1 is `ant_verify` failing the absent channel, on
every arm.

| Prefix | Image | Length | Refusals | Asserts | Tracked | DUT / Garmin loss |
|---|---|---|---|---|---|---|
| `soak-skipA-20261110-161142` | single session, skip | 30 min | 1809 | 0 | `hi 9166/9166/0/9166` | 0.23 / 0.30 % |
| `soak-noskipA-20261110-164329` | single session, no skip | 30 min | 41 821 | 0 | `hi 14415/14415/0/14415` | 0.27 / 0.20 % |
| `soak-skipB-20261110-171514` | single session, skip | 30 min | 1809 | 0 | `hi 9165/9165/0/9165` | 0.37 / 0.25 % |
| `soak-noskipB-20261110-174702` | single session, no skip | 30 min | 41 821 | 0 | `hi 14415/14415/0/14415` | 0.41 / 0.19 % |
| `soak-d2skipA-20261110-212019` | `img_a2_thread` | 30 min | 2367 | 0 | `hi 7199/7199/0/7199` | 0.08 / 0.23 % |
| `soak-d2noskipA-20261110-215208` | `img_a2_noskip_thread` | 30 min | 59 495 | 0 | `hi 7199/7199/0/7199` | 0.04 / 0.36 % |
| `soak-ctrl_s14-20261110-224858` | `img_d2_thread_s14` | 15 min | 30 411 | 0 | `hi 3601/3601/0/3601` | 0.05 / 0.52 % |
| `soak-norelease-20261110-230547` | `img_a2_norelease_thread` | 15 min | 22 069 + 5022 | 1 at 668 s | second segment shown | Garmin 0.47 % |

The 5340 DK was the OpenThread leader for all of these (`ot_leader_flash.txt`,
`ot_leader_flash_a2.txt`), with the DUT in its child table.

## Lead sweep

`sweep_attached/`, `sweep_detached/`, `sweep2_attached/`, `sweep2_detached/`: `dut_log.txt`
(`D2SWEEP`, `D2EL`, `D2BOOK`, `TEMPDIAG`), `dut.json`, `ref.json`, `health.txt`, `ot.txt`
(`leader` or `disabled`). `sweep_corrected.txt` in each is `sweep_sum_corrected.py` run
over its log; use that, since the original tool mis-attributed 5 s of each step. Steps
with `tracked_grants` of 0 ran before the tracked channel was up. The last step or two
of each are cut short by the run's end.

## Early release and the regression bisect

| Dir | Image | ch0 / Garmin | What it showed |
|---|---|---|---|
| `fix1_ack`, `ack_fix2`, `ack_fix3` | early release, single session | 59+60, 59+60, 60+59 | |
| `ack_ctrl1`, `ack_ctrl2` | dee9135 control | 59+59, 58+59 | |
| `gate_final1`, `gate_final2` | b05bcff, with e7749c2 | 6.1 / 1.2 %, 8.2 / 0.4 % | the regression |
| `gate_d2ctrl` | prototype | 0.2 / 0.2 % | same sitting |
| `ack_final` | b05bcff | 60+60 | |
| `v_nofixend` | b05bcff without the `fix_end` reset | 0.2 / 0.2 % | isolates e7749c2 |
| `v_nodue` | b05bcff without the booking-due pass | 3.5 / 0.2 % | |
| `v_nofix1` | b05bcff without the early release | 0.2 / 0.2 % | `late_drop` 486 |
| `fe2_1`, `fe2_2` | e7749c2 on real completions only | 2.3 / 1.0 %, 2.1 / 0.4 % | the unexplained remainder |

## Tests

| File | What it shows |
|---|---|
| `ztest_backend_red.txt`, `ztest_signal_context_red.txt`/`_green.txt`, `ztest_atomic_red.txt`/`_green.txt`, `ztest_policy_red.txt`/`_green.txt`, `ztest_knee_red.txt`, `ztest_knee.txt` | red then green for the first session's fixes |
| `ztest_review_red.txt`, `ztest_review_green.txt` | the review's seven tests before and after the fixes |
| `ztest_review_mutations.txt` | one-line mutations on Bluefin. Two of the first five failed to build and are not evidence; the two scheduler mutations are recorded twice, the first run's output being cut off |
| `twister_final.txt`, `twister_mid.txt`, `zero_cost.txt` | earlier runs, at 6128da5, 6ca1fe6 and before b05bcff; superseded |
