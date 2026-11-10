# Phase 0b raw logs

All files are in `C:\adr18_logs\phase-0b\`, outside the repository. Measured
2026-11-09 on branch `wip/adr18-foundation`. Report: [phase-0b.md](phase-0b.md).

## Bench identity

| File | What it is |
|---|---|
| `dongle_probe.txt` | `ant_probe --serial 3D55F77818BE772A` on master A (nRF52840 Dongle), before any bench work. Not reflashed. |
| `dut_probe.txt` | `ant_probe --port COM4` on the L15 DK after flashing the leg image |
| `flash_dut_gate.txt` | `flash_sim_jlink.ps1` output for the DUT image (Cortex-M33, verify O.K.) |
| `build_dut_gate.log`, `build_dut_gate_config.txt` | DUT build (`apps/dongle_thread`, `gate.conf;phase_diag.conf`, L15) and its `.config` read-back, source commit `0771cc3` and the worktree status at build time |
| `build_pm_head.log`, `build_pm_head_config.txt` | Pro Micro image (`apps/dongle`, `promicro_nrf52840/nrf52840/uf2`) from `ccfa2b4`: `.config` read-back (USB_LEGACY, UF2 reboot request, nRF HAL), UF2 target address and family id, SHA-256 |
| `promicro_flash.txt` | Pro Micro: probe before, reboot to bootloader, `INFO_UF2.TXT`, copy, probe after (`RADIANT0.01B00`) |

## Step 6: the 17-offset leg

| File | What it is |
|---|---|
| `leg_a_offsets_console.txt` | the harness's console, including the ant_verify bring-up lines |
| `leg_a_offsets/run.txt` | the harness's timeline: delays (shuffle seed 1), per-trial host phase, the per-trial JSON row, the final tally and mask list |
| `leg_a_offsets/results.jsonl` | one row per trial: host phase at start and end, TDIAG frames per device with the DUT's own phase, TEMPDIAG masks, ant_verify channel packets and time to first packet |
| `leg_a_offsets/summary.json` | the found/not-found tally the report quotes |
| `leg_a_offsets/dut_log.txt` | the DUT's whole log (COM3, DTR) for the leg: 98 `TDIAG frame` and 224 `TEMPDIAG chunks` lines |
| `leg_a_offsets/tNN.json`, `tNN_verify.txt` | ant_verify per trial (ch0 pinned to #2002, ch1 wildcard HR) |

The controls (5 offsets, A off air) and the R1 reading were not run: the leg
did not reproduce 12/17, which is the plan's stop condition. See the report.

## Tests

| File | What it is |
|---|---|
| `ztest_dev1.txt`, `ztest_dev2.txt`, `ztest_dev3.txt` | earlier `radiant\tests` runs on the 5340 DK while the tests were written: a build error (missing include), then three, then one failing shape test. Kept as the record of what changed |
| `ztest_dev4.txt`, `ztest_unit_5340_console.txt` | final `radiant\tests` run on the 5340 DK: one boot, PROJECT EXECUTION SUCCESSFUL, and the `deaf-shape` / `r1-shape` lines |
| `ztest_api.txt`, `ztest_api_5340_console.txt` | `radiant\tests\api` on the 5340 DK: successful, one boot |
| `ztest_gate.txt`, `ztest_gate_5340_console.txt` | `radiant\tests\gate` on the 5340 DK: successful, one boot |
| `twister_bluefin.txt` | native_sim twister on the Bluefin server over a `git archive` of the branch: 13 scenarios, 4220/4220, `check_test_counts` OK |
| `branch_snapshot.tar` | the archive that was run there |
| `conformance_list_base.txt`, `conformance_list_head.txt` | `ant_conformance.py --list` from 42475c6 and from the branch: identical (286 lines) |
| `conformance_replay_nrf.txt` | `--replay` of `archive/captures/serial/conformance-nrf-radiant.antser` with the branch's tools: parses, 1273 records |

## Zero cost and sizes

| File | What it is |
|---|---|
| `zero_cost.txt` (UTF-16), `zero_cost_exit.txt` | `scripts\check_zero_cost.ps1 -BaseRef 42475c6`. The exit file has one line per attempt: `0`, `1`, `0`. The first attempt died on a script bug before building anything and still exited 0, because the error was not trapped; the second died on a missing row directory (exit 1). Both bugs are fixed in `ccfa2b4`, which also adds the `trap`. The third run is the result: PASS, 8/8 rows identical. `zero_cost.txt` holds only the third run |
| `zero_cost_no_hal_ti.txt` | the same script at `-NcsVersion v3.2.4`, where hal_ti is absent: FAILED, exit 1, no skip |
| `sizes_base.txt`, `size_build_base_l15_bridge.log`, `size_build_base_l15_matter.log` | size baseline on 42475c6 as is: the L15 `thread.conf;bridge.conf` build FAILS to link (logs are UTF-16) |
| `sizes_base_patched.txt`, `size_build_base_patched_*.log` | the same on 42475c6 with two duplicate source lines removed from `apps/dongle_thread/CMakeLists.txt` (scratch worktree `build\zsz`, never committed): bridge links; both Matter arms still fail to link |
