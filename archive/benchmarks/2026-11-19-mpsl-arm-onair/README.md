# MPSL arm-guard fix on air (default MPSL build, LP option off), 2026-11-19

**Verdict: PASS.** No MPSL/SDC assert and no reset. Tracked loss matches the
same-window reference and is lower than it. Acked data completes at the
2026-11-14 rate. The EDENIED-type counters are small (`sched_denied` 6,
`slots_denied` 2).

This is the on-air check of merge 88134ca (fix/mpsl-arm-handback: dd28bde +
446b9a8). That merge applies the grant-generation arm guard on every MPSL build
and owes a denial while an arm stages.

## Image and rig

- DUT: nRF54L15 DK (J-Link 001057737173). The image is
  `C:\wt_mpsl_arm\build\ackps_l15_mpsl\dongle\zephyr\zephyr.hex`, **md5
  41f6e951d698f8aafc04cee8385b4a18**. It is `apps/dongle_thread`
  `gate.conf;uart_57600.conf` + `apps/dongle/diag_counters.conf`, built at
  dd28bde. That commit is code-identical to master 47bdf80 for this row.
  - It is the MPSL gate with no second stack. The image has no SDC/BLE (gate.conf
    is the coexistence "A" arm), so "search under BLE" in the strict sense was
    not run here.
  - The BLE-contended MPSL case was run in Gate G4 (hrm_ble with BLE connected).
    That run has the LP option on, so it is not this row:
    `../2026-11-19-power-g4/`.
- ANT on COM4 @57600, console on COM3 (DTR), captured for the whole run.
- Partner and reference: the TI LaunchXL `crcfix_ti_rx` (apps/dongle_ti, md5
  a3862fbf) on COM11 @57600.
- On-air source: the Pro Micro `hrtx_promicro_lp_pd` (875a1aee), HR #12345 at
  0 dBm. It is fixed on that image.
- Flashed with `scripts/flash_sim_jlink.ps1 -HexPath <hex> -Serial 001057737173`.
  There was no other J-Link use.

## Commands

`scripts/onair_run.py runs/` (about 9 min) ran these steps:
1. `ant_diag` on both boards (pre).
2. Phase 1, 300 s:
   - `ant_verify` on the L15: ch0 tracks #12345 and ch1 searches the absent
     #43210, so the sweep is always busy, as in the 2026-11-14 soak.
   - At the same time, `ant_verify` on the TI tracking #12345.
3. `ant_diag` on the L15 (mid).
4. Phase 2, `ant_ack_pair` with the TI on device 2002, power level 4 on both ends:
   - a) The L15 is master and receives slave-originated acked data (gated
     runner, 60 sends, 1 s).
   - b) The L15 is slave and originates acked data to the TI master. This is
     Zwift's FE-C path, the one e042a10 broke on MPSL.
   - c) `--master-acked 60`: the L15 master originates.
5. `ant_diag` (post).

## Results

| check | result | earlier MPSL sessions |
|---|---|---|
| asserts / resets | **0 / 0**: console uptime continuous 5:39 to 14:39, no boot banner, no ASSERT line | 2026-11-15 fix legs: 0/0 |
| tracked loss, L15 ch0 (search on ch1) | **4.59 %** (56 of 1218; 48 RX_FAIL) | 2026-11-15: 0.29 to 0.36 % in a quieter room |
| same window, TI reference | 5.57 % (68 of 1219, all RX_FAIL) | Garmin then: 0.39 to 0.86 % |
| a) L15 master receives acked | **58/60** completed (TI saw 59 data packets) | 2026-11-14 f_gate: 57/60 |
| b) L15 slave originates acked | **59/60** (TI received 60) | f_gate: 58/60, 59/60 |
| c) L15 master originates acked | **60/60**, slave got 60 distinct, 0 duplicates | 60/60 |
| search, phase 1 | 611 windows, 608 sweeps, 1 acquire, 0 timeouts | |

Room note: loss on both receivers is 10 to 15 times the 2026-11-15 level. The
L15 is better than the same-window TI, so per bench-gates-at-current-room this
is the room, not the image.

### EDENIED-type refusals (0xF7, cumulative since the first host open)

| counter | after phase 1 | after phase 2 |
|---|---|---|
| sched_denied | 4 | 6 |
| slots_denied | 2 | 2 |
| sched_missed / sched_failed | 0 / 0 | 0 / 0 |
| slots_missed | 48 | 51 |
| sched_arm_rejected, tx_expired | 0, 0 | 0, 0 |
| xfer_no_ack | 0 | 1 |
| term_error | 3 | 3 |

For scale: `archive/benchmarks/2026-11-17-ack-pending-search/l15mpsl_diag_after.json`
(the pre-fix ackps image) had `sched_denied` 1 and `slots_missed` 114.

Other counters after phase 2:
- Arm slack minimum 43 us; pass-to-arm max 67 us.
- CRC: 6 repaired, 8 unrepairable.

Counter trap: the search pages (5/6) read 0 again after phase 2, so they were
reset by a host reset somewhere in `ant_ack_pair`. Page 0 kept counting. The
search numbers above are the mid read.

Console: one `gate: too near - start +-200 us, need +2250` at the first channel
open, which is the normal placement refusal. Nothing else is unusual.

`runs/` holds every raw output: the diag JSON, the verify/ack text and JSON,
the console log and the runner log. They stay on the bench machine only (gitignored; see `docs/preservation.md`). The committed numbers are this README and `onair_summary.json`.
