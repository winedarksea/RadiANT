# TI shipping image rebuild and on-air acked check (2026-11-16)

TODO item "Rebuild the TI image before it ships": rebuild from master with
e66ae02 and scheduler rule 6 (fix/ti-sync-separation) in it, then check
acknowledged transfers on air in both roles.

## Images

| file (local only in `build\ti_acked_rebuild_497030a\`, not in git: archive budget) | md5 | built from |
|---|---|---|
| `radiant_dongle_cc26x2r1_launchxl.hex` | a8960111c4ab2484bcca704704246101 | 497030a (fix/ti-sync-separation; merged to master as b92236f), shipping config |
| `radiant_dongle_cc26x2r1_launchxl_diag.hex` | 6c5dc98528aef2e76d0d93a4d5bfd912 | same commit, `ti_health.conf;ti_diag.conf` |
| `ti2_o2.hex` (not copied; `2026-11-16-pass-breakdown/ti_rerun/images/`) | c4e5eb81 | 5584dea, before rule 6 |

Built with `scripts/build_all.ps1 -Only "radiant_dongle_cc26x2r1_launchxl*.hex"`
(NCS v3.4.0). Flashed with `scripts/flash_ti.ps1`. TI on COM11 @ 57600; the peer
is the Garmin USB-m `006` at its default level (3, 0 dBm). The control is the Pro
Micro `EE6986854FD72803` (B4 O2 diag image ba0a7084) against the same Garmin.

## Commands

    # arm 1: TI is the slave and originates acked data (Garmin master)
    ant_ack_pair.py --slave-port COM11 --slave-baud 57600 --master-serial 006 --sends 100
    # arm 2: TI is the master and originates acked data (Garmin slave at level 3)
    ant_ack_pair.py --master-port COM11 --master-baud 57600 --slave-serial 006 \
        --master-acked 100 --burst-slave-tx-power 3 --json-bursts a2_ti_master_acked_r1.json
    # arm 3: TI is the master and answers the Garmin slave's acked data
    ant_ack_pair.py --master-port COM11 --master-baud 57600 --slave-serial 006 --sends 100
    # control for arm 1
    ant_ack_pair.py --slave-serial EE6986854FD72803 --master-serial 006 --sends 100

## Results (COMPLETED of 100, in run order)

| run | image | arm | result | pattern |
|---|---|---|---|---|
| a1_ti_slave_orig_r1 | shipping | 1 | **52** | first 47 FAILED, then 52 of 53 |
| a1_pm_slave_orig_r1 | Pro Micro | 1 | 100 | |
| a1_ti_slave_orig_r2 | shipping | 1 | **88** | 87 OK, then 12 of the last 13 FAILED |
| a2_ti_master_acked_r1 | shipping | 2 | 100 | slave saw 100 distinct, rx_fail 1 |
| a3_ti_master_ackrx_r1 | shipping | 3 | 100 | |
| a1_tidiag_slave_orig_r1 | diag | 1 | 97 | 3 scattered |
| a1_tidiag_slave_orig_r2 | diag | 1 | **6** | 5 OK then nearly all FAILED |
| a1_pm_slave_orig_r2 | Pro Micro | 1 | 99 | |
| a1_pm_slave_orig_r3 | Pro Micro | 1 | 98 | |
| a1_tio2old_slave_orig_r1 | ti2_o2 | 1 | 100 | |
| a1_tio2old_slave_orig_r2 | ti2_o2 | 1 | **0** | |
| a1_tio2old_slave_orig_r3 | ti2_o2 | 1 | **0** | |
| a1_tio2old_slave_orig_r4 | ti2_o2 | 1 | **47** | first 53 FAILED, then 47 of 47 |

## Verdict

- **Arms 2 and 3 pass** (100/100 each). The TI as master originates acked data
  and answers it.
- **Arm 1 fails, but it is not this rebuild.** The TI as a slave loses whole
  stretches, from a few seconds to the entire 30 s run. Inside a stretch every
  acked message fails, and the Garmin never receives it (`acknowledged
  received` at the master's host tracks the COMPLETED count within one). Outside one,
  the TI is as good as the control. The Pro Micro in the same window went
  100, 99 and 98.
- **`ti2_o2.hex` (5584dea) shows the same pattern** (100, 0, 0, 47), so the
  fault predates rule 6. Rule 6 also cannot reach a one-filter window:
  `filters_fit()` has nothing to compare, and the cc26xx check loops from the
  second filter.
- **What the TI sees in a bad stretch** (0xF7 diff, ti2_o2 run r4, in
  `a1_tio2old_r4_{before,after}.json`): `tx_armed` 100 (every reply was armed,
  none refused, `tx_expired` 0), `term_empty` 61 against `term_ok` 186. About
  60 slots of Garmin frames fall in the 53-send failing stretch, so almost every
  receive window in it ended empty. In that stretch the TI's slave timing is not
  where the Garmin is, in either direction. The tracking channel does not drop
  to search (search page: one acquisition per run).
- Good versus dead is decided per session (whole runs at 100 or 0), with
  changes partway through some runs. That points at a phase or timing condition
  set when the channel opens and occasionally changed by drift, not at random
  RF loss. The Pro Micro -> TI 8-13 % loss (TODO, 2026-11-16 benches) may be
  the same defect seen in a smaller dose.

The rebuilt image is therefore **not** staged into `build\final_*`. The
"rebuild before it ships" item stays open behind the new TI slave-tracking item.

## Bench state afterwards

TI left on `radiant_dongle_cc26x2r1_launchxl_diag.hex` (6c5dc985), for
the slave-tracking investigation. Probe PASS after the flash.
