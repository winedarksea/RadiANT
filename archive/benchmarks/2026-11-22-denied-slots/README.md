# Denied tracking slots on air, 2026-11-22

> The full console log stays on the bench machine (`bench-logs/2026-11-22-denied-slots/`, gitignored). The committed numbers are in this README, `legs.tsv`, `den_coexble-summary.json`, the two ant_verify JSONs and `den_coexble-counters.txt` (the first and last `rxpath` and gate lines).

The bench check for the ADR 0024 amendment (a gate-denied tracked slot raises
`EVENT_CHANNEL_COLLISION` 0x09) and its 2026-11-22 follow-up (a run of
`DENY_TO_SEARCH` denials raises one `RX_FAIL_GO_TO_SEARCH`, not a 0x09 as
well). It also settles the "4 unexplained misses" from `acc_coex_ble`
(`archive/benchmarks/2026-11-15-assert-69108/`): 92 misses with no event,
88 of them `book_denied`.

## Result

**PASS.** Every missing packet reached the host as an event, and every gate
refusal of a tracked slot ended `DENIED`.

| | DUT (L15, MPSL gate + BLE load) | Garmin `006` (same window) |
|---|---|---|
| loss (exact), ch0 #2002 | 1.54 % (37/2401, -44.2 dBm) | 0.33 % (8/2403) |
| `EVENT_RX_FAIL` | 5 | 8 |
| `EVENT_CHANNEL_COLLISION` (0x09) | 32 | 0 |
| `EVENT_RX_FAIL_GO_TO_SEARCH` | 0 | 0 |
| `unaccounted` / `allowed_unaccounted` | 0 / 9 | 0 |
| reboots / asserts | 0 / 0 (clean at 630.8 s) | |

- **(a)** `unaccounted` 0 <= 9, and the verdict lists the collisions.
- **(b)** The host's 0x09 count is 32, and `rxpath` reads `denied=32
  collided=0` at the end (`missed=5`, the 5 `RX_FAIL`s). The counters start
  at 0 when the channel opens, which is when the run starts. The first
  `rxpath` line (13 s in) already shows `denied=2`, so a first-to-last delta
  would read 30. Measured from the open, the sum is exact.
- **(d)** No `GO_TO_SEARCH` at all, so no run of 16 consecutive denials.
- **The 4 unexplained misses.** The gate's tracked counters (`D2BOOK ... hi
  acq/pending/blocked/granted`) end at `2402/2373/83/2370`, from 0 before
  the channel opened: acquired - granted = 32, which equals `slots_denied`'s
  32 over the same span (both final lines are the 10:24:24 print). Every
  refusal ends `DENIED` and is now reported. No late-refusal path that posts
  nothing was seen, so no new item was opened.
- The DUT's ant_verify prints FAILED only because channel 1 searches the
  absent #43210 by design (no packets). Channel 0 is the measured one.
- DUT loss is higher than the Garmin's because the SoftDevice takes slots
  (`blocked` 445 of 7688 placed); that is the cost the amendment reports,
  not a fault.

## Rig

- DUT: nRF54L15 DK (J-Link 1057737173), ANT on COM4 @115200, log COM3 (DTR).
  ch0 tracks #2002, ch1 searches the absent #43210. No master channel on the
  DUT, which the 4-misses comparison requires.
- Master: TI LaunchXL (COM11 @57600, image unchanged) via `ant_sim.py --port
  COM11 --baud 57600 --profile power --device-number 2002`.
- Control: Garmin USB-m `006` tracking #2002.

## Image

`apps/dongle_thread` from master c58ca20, the `acc_coex_ble` recipe, sha256
`3a1f68be37dcd6941810ff779c98988ded278cd3759e57a17cdb6d954ed3b83a`
(`.config` lines in `image-config.txt`):

    west build -p always -b nrf54l15dk/nrf54l15/cpuapp -s C:\wt_den\apps\dongle_thread -d C:\wt_den\build\coexble -- ^
        -DANT_RADIO=core -DRADIANT_BACKEND=nrf "-DEXTRA_CONF_FILE=coex.conf;ble_load.conf;phase_diag.conf;d2_stats.conf"

Runner: `archive/benchmarks/2026-11-20-window-drops/soak_run.py --no-peer
--continue-after-assert --master-port COM11 --master-baud 57600 --seconds 600`.
The 0xF7 before/after reads return rc=2 on this image (no diag page), which
is expected.
