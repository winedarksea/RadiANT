# Gate GA: power proxies without a meter, 2026-11-18

> Raw per-arm logs, JSON, images and `.config` files named below stay on the bench machine (gitignored; images rebuild
> from the commit and conf list). The committed numbers are in this README, `power_ga_summary.json`,
> `power_ga_summary.txt` and `images_md5.txt`. The method and the baseline table are in `docs/bench-runbook.md`,
> section "Power without a meter".

**Every current figure here is an ESTIMATE**: software proxies (the `radiant_power_diag` struct and `jlink_peek`
register samples) multiplied by typical datasheet currents (`tools/ant_power_est.py`). No current meter was used.

Code: local master 78864b2 (Track A merged). Branch `power/ga` adds only this folder, the runbook section and a
comment in `scripts/jlink_peek.ps1`.

## Images

| Arm | Build dir (in the `C:\wt_power_ta` worktree) | `-DEXTRA_CONF_FILE` | `zephyr.hex` md5 |
|---|---|---|---|
| A default + quiet | `build/ga_def_q` | `power_diag_quiet.conf` | 2a130085d291eaf379ebc86e25b6a602 |
| B lowpower + quiet | `build/ga_lp_q` | `lowpower.conf;power_diag_quiet.conf` | 2a130085d291eaf379ebc86e25b6a602 (same as A) |
| C lowpower + one-way + quiet | `build/ga_lp1w_q` | `lowpower.conf;one_way.conf;power_diag_quiet.conf` | 921c146a02bfa7efcd505105e30360ba |
| D default + log | `build/ga_def_log` | `power_diag.conf` | b869d171433fe0f9c5e6861efd0c10d2 |
| TI | `build/ti_hr_tx` (`build_all.ps1 -Only ti_hr_tx`) | none | f2c7d5c6bc7e301bdb34127ae6e5d9b6 |

The L15 arms are radiant/samples/hr_transmitter for `nrf54l15dk/nrf54l15/cpuapp`, built by hand with
`scripts/ga_build.ps1` (build_all's sample rows take no extra conf). `.config` read back for each:
`CONFIG_RADIANT_BACKEND_NRF=y`, `# CONFIG_MPSL is not set`, `CONFIG_RADIANT_POWER_DIAG=y`. A and B produce the
same image because `power_diag_quiet.conf` already sets the four symbols that `lowpower.conf` sets.

The L15 was flashed with `scripts/flash_sim_jlink.ps1` (no `-Erase`) and the TI with `scripts/flash_ti.ps1`.

## Rig

- nRF54L15 DK, J-Link 001057737173: the device under test. Sampled with
  `scripts/jlink_peek.ps1 -Preset nrf54l15 -Samples 200 -ElfPath <elf> -Decode -Json`.
- Pro Micro EE6986854FD72803 (apps/dongle diag image, not reflashed): the receiver. It ran
  `tools/ant_verify.py --profile heart-rate --device-number 12345 --expect-bpm 120`, 60 s per L15 arm and 300 s for
  the TI. For the L15 arms it ran at the same time as the SWD session (`scripts/ga_arm.ps1`).
- Arm D's console was captured from L15 VCOM COM3 at 115200 with DTR (`scripts/cap_com.py`). COM4 printed nothing.
- While the TI was verified, the L15 was parked on its previous idle MPSL ackps image, because every arm of the sample
  is device 12345.
- No Garmin; the nRF5340 DK was attached but not used.

## Results

Step 1 was the MPSL ackps image on the L15 before the reflash (main tree `build/ackps_l15_mpsl`, `zephyr.hex`
666f64f4b2b8a5bf2a057d35d29bcad2):

- XO running 200/200 (95 % CI 98.1–100 %).
- **CONSTLATSTAT 0/200 (0–1.9 %).**
- Liveness: running.
- The J-Link output carried no security prompt; the only matching lines were the two Cortex-M33 banners.

L15 arms (struct deltas over ~60 s; µA are ESTIMATES with the ITIMER3 HFXO row, crystal-core lower bound in
brackets):

| Arm | HFXO | CONSTLAT | TX µs/s | RX µs/s | TX/RX ops/s | CPU ‰ | Idle/s | µA | Pro Micro |
|---|---|---|---|---|---|---|---|---|---|
| A | 200/200 | 200/200 | 603.9 | 2597.5 | 4.068/4.068 | 4.14 | 25.35 | 811 (608) | 245/245 |
| B | 200/200 | 200/200 | 603.9 | 2596.8 | 4.066/4.066 | 4.14 | 25.36 | 811 (608) | 245/245 |
| C | 200/200 | 200/200 | 600.3 | 0 | 4.064/0 | 3.22 | 21.29 | 800 (597) | 244/245 |
| D | 200/200 | 200/200 | 603.1 | 2595.6 | 4.064/4.064 | 5.56 | 25.34 | 815 + UART UNKNOWN (612) | 243/245 |

The dominant terms are CONSTLAT at 549 µA and the HFXO at 236 µA (34 µA with the crystal-core row). Every missing
packet was an RX_FAIL. The `common pages` FAIL in each verify log is expected: the sample sends only page 0, with no
pages 80/81.

TI `ti_hr_tx`, 300 s:

- 1216/1219 packets received (4.053/s against 4.0605/s nominal).
- Loss 0.24 %, all three packets RX_FAIL.
- Radio-clock timing 0.007 ms off the grid; signal −43.3 dBm; 120 bpm exact.

## Traps seen

- `ant_verify` printed `loss -0.02 %` for arm B with 245/245 packets. That is its wall-clock expected count, not a
  gain.
- Arm A's first snapshot was taken 16 s after boot (the others at 33 s). Its rates match B within 0.1 %, so there was
  no boot transient.
- Python's stdout redirected by bash on Windows writes cp1252. Arm A's first `est.txt` showed a broken em-dash. Set
  `PYTHONIOENCODING=utf-8`; the files here were regenerated that way.

## Board state at the end

- L15: arm B, `2a130085` (lowpower + quiet).
- TI: `ti_hr_tx`, `f2c7d5c6`.
- Pro Micro: untouched.

Both the L15 and the TI now transmit HR as device 12345 at the same time.
