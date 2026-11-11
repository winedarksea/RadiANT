# Phase 3 Stage B: acceptance and the scan tail

Measured 2026-11-11 on the nRF54L15 DK (J-Link `1057737173`), branch
`wip/adr18-impl`, NCS v3.4.0. Part 1 ran the images built at 8b357d7, with master
c4e1ef8 as the control. Design:
[ADR 0021](../decisions/0021-tracked-slots-are-booked-a-period-ahead.md). Raw logs:
[phase-3b-raw-logs.md](phase-3b-raw-logs.md).

Nothing here was run on TI.

## Part 1: acceptance

### 1. Discovery on the 17-offset rig, `gate.conf`

`tools/ant_disc_phase.py`, delays 0..240 by 15, seed 1, 60 s a trial. Master A the
Dongle #2002, master B the Pro Micro #5005, A/B/A in one sitting, with the 5-offset
control (A off air, channel 0 on absent #9999) after each of A and B.

| Leg | Found | In-band | Missed at (ms) | Median first packet | Slowest |
|---|---|---|---|---|---|
| A, master | 13/17 | 1/5 | 4.6, 20.2, 132.1, 236.2 | 9.0 s | 9.25 s |
| A control | 5/5 | | | 7.5 s | 7.5 s |
| **B, branch** | **15/17** | 4/5 | **5.6, 101.3** | 9.0 s | 9.0 s |
| B control | 5/5 | | | 6.6 s | 6.8 s |
| A', master | 13/17 | 1/5 | 5.0, 19.6, 131.8, 236.8 | 9.0 s | 40.5 s |

**The B leg fails the plan's 17/17.** The rest of the bar holds: the median is within
one sweep of the control (9.0 s against 6.6-7.5 s; a sweep is 7-9 s) and nothing is
beyond four. The mask does not meet the plan's wording either: it read `0x00ffffff` in
49 of 63 non-zero reports. It is full rather than locked and narrow, but bins are
7.7 ms wide and these gaps are narrower than that, so a constant full mask does not
show the bands have moved. They have not: both misses repeat the design's own
prediction of fixed bands.

- **5.6 ms** is the after-slot band, which Stage A's report says is still there. The
  earliest ambient frame heard in the B leg was at 9.475 ms (#52233 and #8265 frames,
  95 of them), against 27.1 ms on master.
- **101.3 ms** is new to the record. It is where the first chunk ends: opened about
  9.4 ms after the slot and capped by `max_window_us`, it closes around 101-103 ms and
  the next chunk waits an elastic lead. The ambient frames leave 99.2-108.2 ms empty,
  which fits, though other gaps of similar width are chance. So this band's position
  is inferred. Zero #5005 frames were logged in either failing trial.

Master's misses all fall in the three bands of ADR 0018. Its 221 ms trial is the band
edge again: 9.0 s on A and 40.5 s on A'.

Tracked work was unharmed on the branch: `hi 7221/7221/0/7204`, every window armed
into a booking (`hit` 7221), `ext=122216/0`, `blk`, `unpl`, `busy`, `near`, `nosess`
all 0, `eo` 0, `sched_missed` 0. `early` 32 are channel-close orphans (`en` 32).

**A voided first attempt.** The first A leg read 17/17 with first packets near 5 s. A
heart-rate strap nobody here owns, #8265 (transmission type 241, -68 dBm), had come
on air, and channel 1's wildcard bound it within 5 s of every trial. Every figure in
that leg looked valid. Those logs are kept as `void_ambient8265_*`. Commit ecc461a
makes channel 1 pair on transmission type 5, which only the harness's masters send,
and refuses to count a bind to anything but B.

### 2. R1 pair on air

`ant_disc_phase.py --r1-walk`: B (the Garmin, which takes a period change on an open
channel) is acquired at about 40 ms after A, walked to the target, and read for 60 s.
The Pro Micro is the reference on B. `s`: a third DUT channel searching for absent
#9999 throughout (`--r1-search-dev`, added in ecc461a).

| Run | B-A in the window (ms) | DUT A | DUT B | Reference on B |
|---|---|---|---|---|
| master, 12 | 12.6 -> 11.0 | 244 (0 %) | **0 (100 %)** | 0.0 % |
| master, 12 s | 12.7 -> 5.1 | 244 (0 %) | **0 (100 %)** | 0.4 % |
| branch, 12 | 12.5 -> 12.0 | 244 (0 %) | 223 (8.5 %) | 0.8 % |
| branch, 12 s | 12.5 -> 8.1 | 244 (0 %) | 221 (9.3 %) | 0.4 % |
| branch, 20 | 20.6 -> 19.4 | 244 (0 %) | 224 (8.1 %) | 0.0 % |
| **branch, 5** | 5.7 -> 4.7 | **162 (33.5 %)** | **73 (70.1 %)** | 0.0 % |

**At 8-20 ms the branch keeps both slots, and at 5 ms it fails.** Master never hears B
in the window. At 12 and 20 ms the branch counted every tracked window booked, hit and
granted, with `blk` 0. Its ~8 % on B is the same at 12 and 20 ms, so it is not R1: B is
the Garmin, and the DUT hears the Garmin a little worse than the reference does.

At 5 ms the gate's bookings were refused by MPSL with no other stack on the air: `blk`
rose by about 13 every 5 s from the moment B arrived (`hi` 40 acquires, 20 refused,
20 granted every 5 s). **And the earlier channel lost a third of its slots, which master
never does.** That is a rule-1 regression against master in the 2.6-~6 ms band.

Partly explained. A tracked slave's booking holds its window plus the 4150 us
acknowledged-reply reserve plus 1.25 ms of margins, about 6.5 ms. A pair closer than
that overlaps, and MPSL refuses whichever booking was placed second, first come first
served (Phase 0c F2). Which one is placed second depends on the order of completions,
so either channel can lose. A ztest with the real follow-on
(`test_d2_r1_with_a_real_follow_on`) reproduces the refusals below about 4.5 ms in the
fake, which models no margins. It does not reproduce the loss on channel A. Also
unexplained: in the 12 ms run the same refusals began about 110 s after the window,
when B had drifted to about 11.2 ms, further than 6.5 ms. With a search running, 8.1 ms
held with no refusals.

ADR 0021's "two sessions fix R1 pairs" holds from about 12 ms up to the lead. Below
about 6 ms, and possibly below 11 ms, it does not, and there it costs the earlier slot
as well. One way to close it is for the earlier booking to take in the later window,
whose arm then rides the held grant as a follow-on does. That was not built here.

### 3. ch0 loss, `sched_missed`, fallback and extension counters, `gate.conf`

Interleaved, 180 s each, ch0 #2002 and ch1 searching for absent #9999, with the Garmin
on #2002 in the same window.

| Run | DUT ch0 | Garmin | `sched_missed` | ext ok/refused | Booking fallbacks |
|---|---|---|---|---|---|
| master 1 | 731/731 | 0.27 % | 0 | 87 480/0 | n/a |
| branch 1 | 731/731 | 0.82 % | 0 | 84 969/0 | 0 |
| master 2 | 713/713 | 1.37 % | 0 | 88 428/0 | n/a |
| branch 2 | 731/731 | 1.64 % | 0 | 84 970/0 | 0 |

**PASS.** No loss on either image, at or under the Garmin's. Every tracked window was
booked and hit (`pl` 732, `hit` 731). Fallback means a tracked window that did not go
through its booking: `unpl`, `busy`, `near`, `nosess`, `den` and `blk` were all 0. The
extension-failed counter was 0. `early` 2 per run are the channel-close orphans.

### 5. BLE, interleaved A/B against master

`coex.conf;ble_load.conf` (a 1 s connectable advertiser), six 60 s readings an image,
alternating. Each tracked slot is classed from the gate's own counters against the
packets heard: heard, granted-unheard (granted minus heard), and ungranted (slots
minus granted). The control's counters come from a bench-only 5 s log line added to
c4e1ef8 in the control worktree (`ctl_patch.py`), not committed anywhere.

| | Slots | Heard | Granted-unheard | Ungranted | Garmin |
|---|---|---|---|---|---|
| master | 1444 | 99.10 % | 1.52 % | -0.62 % | 0.61 % |
| branch | 1464 | 98.91 % | **0.07 %** | 1.02 % | 0.41 % |

**Not a merge blocker: granted-unheard on the branch is 1.45 points under master, not
over it.** Master's figure is too high, because its high-priority grant counter also
counts windows that are not tracked slots. Its granted total exceeds its slots by about
1 % on every reading, hence the negative ungranted. The branch's ungranted 1.0 % is
bookings refused at placement by the advertiser (`blk` 1-4 a reading), the same path a
master tracked request takes. DUT loss was about 1 % on both images. Phase 1's higher
(d2) loss under BLE (11.9 and 18.1 % against 8.8 %) did not reproduce.

**The lead policy under BLE stayed OPEN** in all six readings: `el=3000 [3000..3000]`,
while 54-165 elastic requests a reading were refused. Extension refusals ran 13-35 a
reading, never three in a row long enough to tip it.

### 4. Thread leg, attached peer

The 5340 DK is the OpenThread leader, with the DUT in its child table (RLOC16 `0x1002`
before and after every run). Interleaved master, branch, master, branch. Each run is
one boot: a 120 s rule-1 reading with the Garmin on #2002, then five in-band
`ant_disc_phase.py` trials of 60 s (delays 195, 210, 75, 180, 165, which put B in the
three master bands and on the 221 ms edge).

| Run | DUT ch0 | Garmin | `sched_missed` | Fallbacks | Lead | In-band found (phase ms: first packet) |
|---|---|---|---|---|---|---|
| master 1 | 486/486 | 0.82 % | 0 | n/a | n/a | 1/5 (221: 27.2 s) |
| branch 1 | 477/477 | 1.23 % | 0 | 0 | CONTENDED 16 ms | 2/5 (20: 17.6 s, 221: 17.9 s) |
| master 2 | 484/485 | 1.02 % | 0 | n/a | n/a | 1/5 (221: 26.5 s) |
| branch 2 | 485/485 | 0.40 % | 0 | 0 | CONTENDED 16 ms | 2/5 (21: 17.6 s, 221: 17.9 s) |

**The fallback rate under Thread is 0.** In both branch rule-1 readings every tracked
window went through its booking (`pl` 478/486, `hit` 477/485). `den`, `blk`, `unpl`,
`busy`, `near` and `nosess` were 0, and so was `eo`. Every extension was refused (0/162,
0/159) and the policy sat in CONTENDED.

**Discovery beside Thread is 2 of 5 in-band against master's 1 of 5.** The branch found
the 20 ms phase, which master misses, and found the 221 ms edge 9 s sooner. It missed
5, 131 and 236 ms on both runs. Measured against the ~26 % listening cap from Stage A,
this is what the cap allows: every grant is one 10 ms first bite at a 16 ms lead, so in
60 s a phase may not be listened to at all. Phase 1's (d2) prototype found 4 of 6 under
Thread at 18-21 s, a different delay set. Nothing here contradicts the cap, and the
gate.conf bar does not apply.

### 7. Thread's own traffic during a search

From the same boots: 60 one-second pings from the leader to the DUT's RLOC address
while the DUT was idle, then 120 while it tracked #2002 and searched for absent #9999
(the start of the 140 s reading after the rule-1 one).

| Run | Idle: received, RTT min/avg/max (ms) | Tracking + search: received, RTT min/avg/max (ms) |
|---|---|---|
| master 1 | 60/60, 10/13.8/25 | 120/120, 10/16.8/33 |
| branch 1 | 60/60, 10/13.0/19 | 120/120, 10/18.2/42 |
| master 2 | 60/60, 10/12.7/17 | 120/120, 10/16.0/27 |
| branch 2 | 60/60, 10/13.2/27 | 120/120, 10/18.3/44 |

**No ping was lost on either image, and the child never re-attached.** The RLOC16 was
the same in every child table, before and after. With a search running, the branch
adds about 1.5-2 ms to the mean round trip over master (18.2-18.3 against 16.0-16.8 ms)
and about 10 ms to the worst (42-44 against 27-33 ms). Idle, the two are the same. This
fits the branch taking more grants per period (a booking for each tracked slot plus
elastic chunks), each of which the 802.15.4 driver has to wait out. It is a cost, not a
failure, at n = 2 a side.
