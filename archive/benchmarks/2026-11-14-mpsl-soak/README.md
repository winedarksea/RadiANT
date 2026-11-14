# MPSL assert 69:108 soak (TODO "Left open by the 2026-11-08 clearance"), 2026-11-14

Plan: run the step 1 + step 2 image (L15 `apps/dongle_thread`,
`thread.conf;phase_diag.conf;d2_stats.conf;health.conf`) beside an attached
Thread peer for at least 2 h and read the new counters. **It never got past 15
minutes: 69:108 fired in every leg, on HEAD as well as on the fix.** The 2 h
single run was replaced by short alternating legs to answer "is it ours?".

## Rig

- DUT: nRF54L15 DK (J-Link 1057737173), ANT on COM4, log on COM3 (DTR).
- Thread peer: nRF5340 DK running the NCS OpenThread CLI with `scripts/p4_peer.conf`
  (`scripts/flash_p4_peer.ps1 -Port COM6`), `leader`; the DUT attached as a
  child in every leg (`<leg>-peer.log`, child table after boot).
- ANT master: Pro Micro `EE6986854FD72803` driven by `ant_sim.py --profile power
  --device-number 2002` (its shipping image, not reflashed).
- DUT channels: ch0 tracks #2002, ch1 searches an absent #43210 (sweep always
  busy). Garmin `006` tracks #2002 as a same-window reference.
- Runner: `soak_run.py` (copied here; adapted from the phase-0a
  `soak_arm.py`), one flash per leg.
- Images: fix = this branch (`f_soak`), HEAD = dd38b26 (`h_soak`), long match =
  fix + `apps/dongle/long_match.conf` (`f_soak_lm`, short match off). Every image
  is D2 (two sessions) with the 2026-11-13 short-match default unless noted.

## Result

| leg | image | assert after boot | placed | ext ok/fail |
|---|---|---|---|---|
| fix1 | fix | 1.5 min (~4 s after the ANT channels opened) | 128 | 0/6 |
| head1 | HEAD | 6.3 min | 10770 | 0/429 |
| fix2 | fix | 1.6 min (~4 s after open) | 236 | 0/9 |
| head2 | HEAD | 3.9 min | 5426 | 0/216 |
| fix3 | fix | 3.3 min | 4024 | 0/162 |
| head3 | HEAD | 2.2 min | 1599 | 0/64 |
| fix4 | fix | 3.8 min | 5232 | 0/210 |
| lm1 | fix, long match | 14.6 min | 28728 | 0/1143 |
| lm2 | fix, long match | 7.6 min | 13538 | 0/540 |

Every assert line reads `blocked=16 deadline=0 over=0 invalid=0`; the boot-time
16 BLOCKED are the Thread attach and are identical in every leg. The 16-entry
signal traces (in each `<leg>-console.log`) end on TIMER0 returning END (`1:2`)
in all but head3, which ends `1:1 3:2` (TIMER0 asked to extend, EXTEND_FAILED,
END) like the 2026-11-09 event. Grants before it are almost all `0:0 1:2 7:0`:
START, TIMER0 END, SESSION_IDLE - the elastic scan's 10 ms first bites with
extensions skipped (`skip` counts thousands).

- **Not caused by this pass:** HEAD asserts as often as the fix (3 of 3 legs,
  2.2-6.3 min). ARMDBG (abort-cleared FAILED, early releases, tx/rx start
  misses) read 0/0/0/0 in every fix leg up to its assert.
- **Far more frequent than on record:** once in ~111k failed extensions on
  2026-11-09; here within minutes, at 6-1143 failed extensions. What differs
  from 11-09: the short match is on by default since 2026-11-13, and the master
  is the Pro Micro (before: the nRF52840 Dongle).
- **The short match may raise the rate but is not the cause:** the long-match
  image also asserted (14.6 and 7.6 min against 1.5-6.3 min, mean 3.2, for the
  seven short-match legs). Two legs suggest a lower rate; they do not show one.
- C5-3 applies: the assert halts the image for good (no watchdog), so on a
  Thread or Matter MPSL image this is a field hang within minutes of attaching,
  not a rare event.

Not done: the 2 h run (impossible while it asserts in minutes), the
`gate.conf` (no Thread) soak, a BLE-peer soak.
