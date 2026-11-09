# Phase 0a raw-log index

All raw logs are at `C:\adr18_logs\phase-0a\` on the bench PC, outside the repo
(3.5 MB). They can go away; copy anything worth keeping into
`archive/benchmarks/`. `phase-0a-soak-instrument.patch`, beside this file, is the
one artefact needed to rebuild the soak images.

| Files | What |
|---|---|
| `ztest-gate-C5-1-RED-no-fix.txt` | Gate suite with the C5-1 test but not the fix: 31/32 |
| `ztest-gate-C5-1-GREEN-with-fix.txt` | Same, with the fix: 32/32 |
| `ztest-main-final.txt`, `ztest-api-final.txt`, `ztest-gate-final.txt` | The three suites on branch code: 965, 65, 32 passed |
| `zero-cost-first-pass.txt` | The eight-row zero-cost comparison against 7df6ada |
| `zc_build_base_*.log`, `zc_build_head_*.log` | Build logs for those rows |
| `p4peer-build.log` | The OpenThread leader image for the 5340 DK |
| `sk_build_*.log` | Soak-arm builds, with the `.config` readback in the task output |
| `soak-dry-ctrl-20261109-113912-*` | 90 s rig check on the control image |
| `soak-ctrl1-20261109-114306-*` | Control arm, cut at 1031 s uptime by the L15 probe dropping off USB. Kept as extra exposure |
| `soak-ctrl-20261109-121806-*` | **Control arm, full 30 min** |
| `soak-fixed1-20261109-125002-*` | Fixed arm, halted at about 298 s by MPSL assert 69:108 |
| `soak-fixed-20261109-132255-*` | **Fixed arm, full 30 min** |
| `soak-hammer_ctrl-20261109-141933-*` | 10 min stress, `irq_lock`: `clkback` 5931 |
| `soak-hammer_fixed-20261109-143119-*` | 10 min stress, `CLOCK_LOCK`: `clkback` 0 |
| `void/` | Two stress attempts where the L15 probe was off USB and nothing ran. No data |

Each soak prefix holds: `-console.log` (DUT log VCOM with host timestamps,
including the 30 s `soak1`/`soak2` counter lines), `-dut.json` and `-dut.log`
(`ant_verify` on the DUT), `-ref.json` and `-ref.log` (Garmin reference, absent on
the stress runs), `-sim.log` (the `ant_sim` master), `-peer.log` (OpenThread leader
`ot state` and `ot child table` before and after), and `-summary.json`.
