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
