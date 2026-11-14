# MPSL ACK-window reserve (TODO "ACK-window reserve on MPSL"), 2026-11-14

Question: since e042a10 (2026-11-12) the originator's ACK window carries
`follow_on_us = RADIANT_TRANSFER_NEXT_PACKET_US` (1550 us). The data packet's
grant is sized for REPLY + GUARD + frame tail only, so the window's own air fits
that grant but its follow-on overruns it by about 1.5 ms. `gate_acquire()` served
an in-grant arm only if the follow-on fitted too, so the window fell through to
placement and was refused as too near. Prediction: every acknowledged message an
MPSL build originates fails.

Fix: `gate_acquire()` serves an in-grant arm when its own air (plus the tail)
fits and clips its follow-on at `grant_end`. A later arm into the clipped part
(a retry, a burst's next packet) is still refused, as before e042a10.

Ztest first: `test_an_ack_window_is_served_in_grant_with_its_follow_on_clipped`
(radiant/tests/gate) failed on HEAD dd38b26 (5340 DK: 33/34, "gate: too near -
start +840 us, need +18000") and passes with the fix (34/34; all four gate suites
77/77, one boot).

## Rig

- DUT: nRF54L15 DK (`apps/dongle_thread`, UART on COM4), flashed per leg with
  `scripts/flash_sim_jlink.ps1`.
- Partner: Garmin USB-m `006`, `--tx-power 3` (0 dBm on the Garmin; level 4 is
  its weakest).
- `tools/ant_ack_pair.py`, 60 sends, 1 s interval, 90 s; `--master-acked 60` for
  master-originated acknowledged data. Garmin reset with `ant_probe` after each run.
- Images (NCS v3.4.0): HEAD = dd38b26 `gate.conf` (`h_gate`, md5 E0B339F1...)
  and direct (`h_dir`, + `apps/dongle/diag_counters.conf`); fix = this branch
  `gate.conf` (`f_gate`, md5 89597463...) and direct (`f_dir`, same fragment).
- Same-window control: the Pro Micro (`EE6986854FD72803`, shipping direct image,
  not reflashed) as the originating slave against the same Garmin.

## Result

Order run: h_gate_1, f_gate_1 (+ control), h_gate_2, f_gate_2, f_dir_1, h_dir_1.

| leg | DUT originates as slave (Zwift's path) | DUT as master receives acked | DUT master originates acked |
|---|---|---|---|
| h_gate_1 (HEAD, MPSL) | **0/60** completed, Garmin received 57 | 58/60 | 60/60 |
| f_gate_1 (fix, MPSL) | **58/60**, Garmin received 58 | 57/60 | 60/60 |
| Pro Micro control, same window | 60/60 | | |
| h_gate_2 (HEAD, MPSL) | **0/60**, Garmin received 59 | | |
| f_gate_2 (fix, MPSL) | **59/60**, Garmin received 59 | | |
| f_dir_1 (fix, direct) | 60/60 | 57/60 | 59/60 |
| h_dir_1 (HEAD, direct) | 57/60, Garmin received 57 | | 59/60 |

- HEAD on MPSL failed every slave-originated acknowledged message in both of its
  legs while the Garmin received 57 and 59 of the data packets: the packets went
  out, the ACK window was never opened. With the fix every message the Garmin
  received completed (58/58, 59/59).
- The direct gate ignores `follow_on_us`, so the direct build is unaffected
  either way (57-60/60; every failure there is a data packet the Garmin did not
  receive).
- Master-originated acknowledged data was NOT broken on HEAD (60/60). Not traced:
  the master's opener has a longer TX reserve (OPENER_REPLY), but by the same
  arithmetic its window should still overrun; the master slot's grant must be
  sized by something larger (the tracked broadcast reserve). Recorded, not chased.
- ADR 0021's "acked 356/360 against 359/360" predates e042a10 (2026-11-11), so it
  is not this defect.

Files: `<leg>-<direction>.json/.txt`, `<leg>-flash.txt`.
