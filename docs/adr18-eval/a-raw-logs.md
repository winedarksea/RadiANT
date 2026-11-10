# Phase 1 (a) raw logs

All files are in `C:\adr18_logs\phase-1-a\`, outside the repository. Measured
2026-11-10 on branch `wip/adr18-a`. Report: [a.md](a.md).

## Images

| File | What it is |
|---|---|
| `img_a_gate.hex`, `build_a_gate.log` (+`.cmd`) | Master A image: 237b7c4 (detached worktree `C:\wt_a_base`), `apps/dongle_thread` `gate.conf;phase_diag.conf`, L15. SHA-256 `311914D1...BBE74D4`. `.config` NRF=y, GATE_MPSL=y, PHASE_DIAG=y |
| `img_b_gate_9913ec1.hex` (B1), `img_b_gate_56e8450.hex` (B2), `img_b3_gate.hex` ... `img_b11.hex`, `build_b*_gate.log` | The intermediate (a) images, one per on-air defect (see a.md, "The six on-air defects"). `img_b7_handoff3ms.hex` is the reverted 3 ms handoff, `img_b9_nocap.hex` the 60 s cap, `img_b10_dbg.hex` the HDIAG build |
| `img_b_6b6e7cb.hex`, `img_b_6b6e7cb_config.txt`, `build_b_final.log` | **Final (a) image**: 6b6e7cb, `gate.conf;phase_diag.conf;gate_stats.conf;health.conf`. SHA-256 `12A94FD6...BFDCD1B` |
| `img_b_thread_6b6e7cb.hex`, `build_b_thread.log` | (a) under Thread: `thread.conf;phase_diag.conf;gate_stats.conf;health.conf` |
| `img_b_ble_6b6e7cb.hex`, `build_b_ble.log` | (a) under BLE: `coex.conf;ble_load.conf;phase_diag.conf;gate_stats.conf;health.conf` (1 s advertiser) |
| `flash_dut_*.txt` | `flash_sim_jlink.ps1` output for every flash of the L15 (`1057737173`) |
| `identity.txt` | `ant_probe` on the Dongle, the Pro Micro, the DUT (COM4) and the Garmin before the legs |
| `ot_leader_flash.txt` | the 5340 DK flashed as the OpenThread leader (Phase 0a's `build\p4peer` images in `C:\wt_timing`), `peer state: leader` |

## Discovery legs (`tools/ant_disc_phase.py`, delays 0..240 by 15, seed 1, 60 s)

Each directory has `run.txt` (timeline and per-trial row), `results.jsonl`, `summary.json`, `dut_log.txt` (COM3, DTR; `TDIAG`, `TEMPDIAG`, and on (a) images `GSTAT`/`GHOLD` every 5 s) and `tNN.json`/`tNN_verify.txt`; the console is beside it.

| Dir | Image | Result |
|---|---|---|
| `leg_a1/` | master | 12/17, in-band 1/6 |
| `leg_b/` | **6b6e7cb** | **17/17, in-band 5/5**, `bridge=1125/0` |
| `leg_a2/` | master | 13/17, in-band 1/5 |
| `leg_b1/` | 9913ec1 | 12/17 (master-like: `bridge=0/0 cont=293 miss=313`, defect 1) |
| `leg_b2/` | 56e8450 | aborted after one trial (`bridge=0/0`, `hold_max` 192 ms, defect 2) |
| `leg_b11_aborted/` | B11 (3 ms handoff) | aborted: the image was superseded |
| `b3_sanity/` .. `b6_sanity/` | B3..B6 | one in-band trial each (delay 195 ms), the debugging trail; B6 is the first found in-band |

## Isolated rule-1 readings (`rule1_*`)

ch0 pinned to #2002 (Dongle, `ant_sim`), ch1 searching for absent #9999, the Garmin (`006`) on #2002 in the same window. Each has `dut.json`/`dut_verify.txt`, `ref.json`/`ref_verify.txt`, `dut_log.txt`, `sim.txt`, `a_reset.txt`, and where taken `health.txt` (0xF6).

| Dir | Image | DUT ch0 | Garmin |
|---|---|---|---|
| `rule1_b6/` | B6 | 24.8 % | 0 % |
| `rule1_b7/` | B7 (3 ms handoff) | 17.8 % | 0 % |
| `rule1_b8/` | B8 (health) | 17.8 %, `sched_missed` 65 | 1.6 % |
| `rule1_b9_nocap/` | B9 (60 s cap) | 21.3 %, missed 76 | 0 % |
| `rule1_b10_dbg/` | B10 (HDIAG: each miss logged with the planner state) | 10.6 % | 1.2 % |
| `rule1_b11/` | B11 (expiry fix) | 0.00 %, missed 0 | 0.41 % |
| `rule1_b_final/` | **6b6e7cb** | **0.00 %, missed 0** | 0.41 % |

## Stacks and R1

| Dir | What it is |
|---|---|
The stack legs were run by a scratchpad script (`stackleg.ps1`: flash, wait for the Thread attach, the 120 s rule-1 reading, a 0xF6 read, then three in-band `ant_disc_phase.py` trials at delays 195, 75 and 180 ms). Each directory has `flash.txt`, `rule1/` (laid out as the `rule1_*` directories, with `health.txt`) and `disc/` (laid out as the discovery legs); the console is beside it.

| Dir | What it is |
|---|---|
| `thread/`, `thread_console.txt` | (a) under Thread, first run (`img_b_thread_6b6e7cb.hex`), plus `ot_child_table.txt` (child `0xe801` attached before and after). ch0 13.4 %, Garmin 1.23 %, missed/denied 64/64, `ext=0/2964`, `bridge=0/0`. The third disc trial was lost when that session ended |
| `thread_master/`, `thread_master_console.txt` | master control under Thread: `img_a_thread_237b7c4.hex` (SHA-256 `69E786E9...6250DC`), `build_a_thread.log` (+`.cmd`), `img_a_thread_config.txt`; `thread.conf;phase_diag.conf;health.conf` from `C:\wt_a_base`. ch0 4.5 %, Garmin 0.40 %, missed/denied 0/0, in-band 0/3, mask `0x00249124` |
| `thread2/`, `thread2_console.txt` | (a) under Thread, second run, same image. ch0 15.6 %, Garmin 0.19 %, missed/denied 64/64. `disc/dut_log.txt` ends in `MPSL ASSERT 69:108`; the third trial's `t02_verify.txt` is the host finding no startup after it |
| `ble/`, `ble_console.txt` | (a) under BLE: `img_b_ble_6b6e7cb.hex` (`coex.conf;ble_load.conf;phase_diag.conf;gate_stats.conf;health.conf`). ch0 5.54 %, Garmin 0.21 %, missed/denied 2/11, `bridge=410/17`, in-band 3/3 |
| `ble_master/`, `ble_master_console.txt` | master control under BLE: `img_a_ble_237b7c4.hex`, `build_a_ble.log` (+`.cmd`), `img_a_ble_config.txt`. Built in the scratch worktree `C:\wt_a_ble`: 237b7c4 plus the branch's four app-only BLE-load files (`apps/dongle_thread/CMakeLists.txt`, `Kconfig`, `ble_load.conf`, `src/ble_load.c`), no `radiant/` change; `coex.conf;ble_load.conf;phase_diag.conf;health.conf`. ch0 4.93 %, Garmin 0.40 %, missed/denied 0/41, in-band 0/3 |
| `ble2/`, `ble2_console.txt` | (a) under BLE, second run, same image. ch0 4.93 %, Garmin 0.21 %, missed/denied 3/42, `bridge=417/16`, in-band 3/3 |
| `r1walk/`, `r1walk_console.txt` | the R1 walk on the final (a) gate image (`img_b_6b6e7cb.hex`, flashed by `flash_dut_b_final2.txt`): `run.txt`, `r1w_summary.json`, `r1w_dut.antser` (the DUT's whole session), `r1w_dut.json`/`r1w_dut_verify.txt`, `r1w_ref.json`/`r1w_ref_verify.txt` (Pro Micro on B), `dut_log.txt`. A 163 (33.1 %), B 81 (66.8 %), ref 3.65 % |
| `r1walk_master/`, `r1walk_master_console.txt` | the same walk on the master gate image (`img_a_gate.hex`, flashed by `flash_dut_a3.txt`). A 244 (0 %), B 0 (100 %), ref 0.41 % |

## Tests

| File | What it is |
|---|---|
| `ztest_gate_1..4.txt`, `ztest_gate_final.txt` | `radiant\tests\gate` on the 5340 DK (COM6): 36/37 (a test bug), then 37/37, 38/38, 39/39, 39/39 |
| `ztest_unit_1..9.txt`, `ztest_unit_5340_console.txt`, `ztest_unit_6_console.txt`, `ztest_unit_final_console.txt` | `radiant\tests` runs while the scheduler was developed; the failures that found the span, bootstrap, refusal-storm and slow-handoff issues; the final console with every `deaf-shape`, `held-` and `r1-held` line |
| `ztest_api_final.txt` | `radiant\tests\api`: 65/65 |
| `twister_bluefin.txt`, `branch_snapshot_6b6e7cb.tar` | native_sim twister on Bluefin over the archive: 13 scenarios, 4259/4259, counts OK |

## Sizes and zero cost

| File | What it is |
|---|---|
| `size_bridge15.log` | L15 `thread.conf;bridge.conf` at 6b6e7cb: FLASH 324 168, RAM 198 048 |
| `size_matter15.log` | three failed attempts (an orphaned compiler from a stopped build held the directory) |
| `size_matter15b.log` (+`.cmd`) | L15 `thread.conf;matter.conf` at 6b6e7cb: FLASH 819 660, RAM 253 380 |
| `size_matter52_fail1.log` | the first nRF52840 Matter attempt: a compiler crash with no error message, during the host's memory pressure |
| `size_matter52.log` (+`.cmd`) | nRF52840 DK `thread.conf;matter.conf` at 6b6e7cb, the rerun: FLASH 812 396, RAM 244 824 |
| `zero_cost.txt` | `scripts/check_zero_cost.ps1 -BaseRef 237b7c4` (code at 6b6e7cb): all eight rows same sections, same symbols, identical `zephyr.bin`; PASS, exit 0. Builds under `C:\wt_adr18_a\build\zc\` |
