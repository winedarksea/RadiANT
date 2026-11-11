# Phase 3 Stage B: acceptance and the scan tail

Measured 2026-11-11 on the nRF54L15 DK (J-Link `1057737173`), branch
`wip/adr18-impl`, NCS v3.4.0. Part 1 ran the images built at 8b357d7, with master
c4e1ef8 as the control. Design:
[ADR 0021](../decisions/0021-tracked-slots-are-booked-a-period-ahead.md). Raw logs:
[phase-3b-raw-logs.md](phase-3b-raw-logs.md).

Nothing here was run on TI.

## Verdicts

> **Fixed in Stage C ([phase-3c.md](phase-3c.md)).** The blocker below is closed:
> both slots are kept at 5 ms. The "refusals near 11 ms" were a misread phase (they
> began at 5-6 ms by the DUT's own timestamps), and the earlier channel's loss came
> from the gate's session-index placement order.

**Merge blocker for the user to rule on (not the BLE criterion, which passes): an R1
pair closer than about 6 ms costs the slot of the channel that was already tracked.**
Master in the same position loses the later channel only. Measured on air with the
R1 walk (item 2: A lost 33.5 %, B 70 % at 5 ms) and, once the scan tail lets discovery
find a sensor 4.7-5.9 ms after a tracked one, in ordinary discovery (Part 2: the
tracked channel lost 26-28 % in those trials, on `gate.conf` and beside Thread). The
cause is that each booking holds its window plus the 4150 us reply reserve and
margins, so MPSL refuses whichever of the two was placed second.

| Check | Branch (8b357d7 in Part 1; ff88a6a for the tail rows) | Control (c4e1ef8) | Log |
|---|---|---|---|
| 1. 17-offset, `gate.conf` | **FAIL** at 8b357d7: 15/17 (5.6, 101.3 ms missed). **17/17** with the tail, one find at 40.2 s | 13/17 and 13/17, in-band 1/5 | `aba_*`, `fin_B` |
| 2. R1 pair | 12-20 ms: both kept. **5 ms: FAIL, A 33.5 % and B 70 % lost** | B 100 % lost, A kept | `r1_*`, `r1t_*` |
| 3. ch0 loss, `sched_missed`, fallbacks, ext refused, `gate.conf` | PASS: 0 loss, 0, 0, 0 | 0 loss, 0 | `gate_*`, `gatetail_*` |
| 4. Thread: fallback rate, in-band | fallback 0; in-band 2/5 (4/5 with the tail) | in-band 1/5 (2/5) | `thread_*`, `threadtail_*` |
| 5. BLE granted-unheard | **PASS**: 0.07 % (master 1.52 %), lead stayed OPEN | 1.52 % | `ble_*`, `bletail_*` |
| 6. Bridge second-sensor bind | 5/14 bound (64 % missed) | 6/14 (57 % missed) | `bind_*`, `bridge_head1` |
| 7. Thread's own traffic | 0 ping lost, no re-attach; mean RTT +1.5-2 ms under search (+5 ms with the tail) | 0 lost | `thread_*` |
| 8. Late arms, Matter image | `eo` 0, `hl` 0, `late` 0 in 300 s; `eo` 1 seen in two multi-channel runs elsewhere | n/a | `matter1`, `r1_head12s`, `eight2_head_2` |
| 9. Eight channels beside Thread | **not measured on air** (masters could not be phased); ztest: 69 % deaf at the 16 ms lead | n/a | `eight2_*`, ztest |
| Scan tail | **kept**, 10 ms with re-place at plain length | n/a | `tsw_*` |
| MPSL 69:108 | **one assert on `img_a2_thread`** (with early release), new trace shape | none this session | `eight_head_1` |


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
heart-rate strap of unknown owner, #8265 (transmission type 241, -68 dBm), had come
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

### 8. Late-arm exposure on a Matter image

L15 `thread.conf;matter.conf;d2_stats.conf;health.conf` at 8b357d7, with
`CONFIG_RADIANT_LOG_LEVEL_INF=y` (CHIP silences radiant's log otherwise). It was not
commissioned, so CHIP was advertising over BLE and the Thread stack was detached. One
300 s reading, ch0 #2002 and ch1 absent, the Garmin on #2002:

- ch0 1209/1209, Garmin 0.35 %, `sched_missed` 0.
- `hit` 1209 of 1209 tracked acquires, `early` 2, both `en` (channel-close orphans),
  **`eo` 0**, `hl` 0 (no hit with under 1 ms to go), `due saved` 0, `late` 0,
  `orphan` 1. START error -84..+19 us. CONTENDED lead, `ext=0/399`.

**No late arm was seen on the Matter image.** Phase 1's `eo=1` question is not closed,
though, because **`eo` 1 appeared twice elsewhere** with no channel closing:

- the `gate.conf` R1 walk with a search running (`r1_head12s`), between 66.7 and 71.7 s
  of the log, while B was crossing about 22 ms. The DUT capture shows B missing two
  slots at 69.7 s, but B misses about 8 % throughout (it is the Garmin), so a lost
  live slot cannot be told from loss on air;
- the eight-channel run beside Thread (`eight2_head_2`, `eo` 1, `early` 4, `en` 3,
  `mis` 14, `nosess` 22).

In both, other tracked channels or a search were contending for the sessions. `eo`
counts a booking granted while the other session had an operation staged, so it is
the counter of a live slot lost to a late arm. It is rare (1 in about 1000-2900
bookings in those runs) and has not been seen with one tracked channel.

### 9. Eight tracked channels beside Thread

**Not measured on air: the rig could not put eight masters at distinct phases.** A new
driver (`multi_master.py`, scratchpad) opened four channels on each of the Dongle and
the Pro Micro, one period plus period/8 apart. Each stack opened every channel (8 of
8), but all eight transmitted within about a millisecond of one phase (the dry run's
`phase_ms` read 0.0-0.5 and 243.8-245.7). The DUT then tracked one or two of them
(`#3001` alone, or `#3003` and `#3006`), and the rest were never acquired. Both stacks
evidently align a new master channel to their existing ones, so phase cannot be set
from the host this way. What the four readings do show, all with Thread attached:
master's search heard `0x00249124` (7 of 25 bins, locked) and the branch's moved
between `0x00694a52` and `0x0096a44b` (about 11 bins). One earlier attempt with the
first driver (`eight_*`, only two masters on air) is kept for its assert (below).

What was measured instead is the scheduler alone, in a ztest
(`test_d2_deaf_fraction_at_the_contended_lead`): the deaf fraction at 1/2/4/8 evenly
spaced tracked channels at the 16 ms lead the gate uses beside Thread. The fake radio
has no other stack, so it neither refuses extensions nor caps a grant at its first
bite. These are upper bounds on listening, not Thread figures.

| Tracked | Deaf, 3.4 ms lead (Phase 1) | Deaf, 16 ms lead | 16 ms lead + 10 ms tail |
|---|---|---|---|
| 1 | 6.1 % | 21.9 % | 12.3 % |
| 2 | 9.5 % | 23.5 % | 23.5 % |
| 4 | 13.4 % | 34.5 % | 34.5 % |
| 8 | 26.9 % | **69.1 %** | 69.1 % |

At eight channels beside Thread, the scheduler alone leaves about 70 % of phases
unlistened, before Thread takes its share. The summary's "almost deaf" inference
stands, now with a number under it.

### An MPSL assert on an image with the early release

`*** MPSL ASSERT 69:108` halted `img_a2_thread` (8b357d7) in `eight_head_1`, beside an
attached leader, with two masters on air, two tracked channels and seven channels
searching for absent devices: `placed=2588 granted=2554 blocked=33 ext=0/90 skip=1866`,
`last_sig=1`, trace ending `... 7:0 0:0 1:2` (IDLE, START, TIMER0 returning EXTEND).

**This is the first assert on record from an image with decision 2's early release,**
which Stage A noted as suggestive. It also has a different shape: every earlier
assert followed an `EXTEND_FAILED` (`1:1 3:2`). This one came on a TIMER0 that asked
for an extension, with no refusal after it in the trace. The skip had run 1866 times,
so this extension may be the first request after a skip ran out. The suggestion that
the early release stops 69:108 does not hold.

### 6. Bridge: a second sensor binds while the first is tracked

L15 `thread.conf;bridge.conf` with the pairing window open 40 s every 100 s, MQTT off,
`PROFILES_EXTRA=n`, the leader stopped (a detached MTD, as in the baseline). Both images
carry a one-line bench patch (`bridge_tt.py`): discovery pairs on transmission type 5,
because the ambient strap #8265 would otherwise take the free channel. The Dongle's
#1001 is bound and tracked throughout. Before each window the Pro Micro restarts as a
new device at a fresh phase, and the attempt passes if it binds in that window
(`bind3.py`, scratchpad).

| Run | Bound | Missed |
|---|---|---|
| master 1 | 3/7 | 4 |
| branch 1 | 3/7 | 4 |
| master 2 | 3/7 | 4 |
| branch 2 | 2/7 | 5 |
| **master** | **6/14** | **57 %** |
| **branch** | **5/14** | **64 %** |

A first run with the eviction harness (`evict3.py`) read 5 binds in 16 attempts on
the branch (69 % missed). Its two other runs stopped at the harness's initial-bind
step, because the second initial sensor did not bind within 230 s, which is the same
miss. #1001 stayed TRACKING in every window on both images.

**No improvement: the branch misses as often as master, within the 50-70 % baseline.**
This image runs beside a detached Thread stack, where the gate sits in CONTENDED and
listening is capped near 26 %. A 40 s window then holds about one sweep. The discovery
gains measured on `gate.conf` do not carry over to this configuration.

## Part 2: the scan tail (ADR 0021 decision 5)

### What was built

| Commit | What |
|---|---|
| 0d385b6 | While a scan runs, a tracked RX booking asks for a tail past its follow-on, dithered 0.5-1.5x `caps.scan_tail_us`. When the window completes, the scheduler arms the next chunk into the held grant at the in-grant lead. No tail when another committed operation is near the booked air or the slot's phase now. Six scheduler ztests and one gate ztest; three were red first (`ztest_tail_red.txt`). Fake radio: deaf 6.1 % -> 2.0 % at one channel, and the gap moves |
| 320b0b9 | R1 characterization with a real follow-on (item 2) |
| d95a0c7 | A refused tailed booking is placed once more at plain length. Mutation-checked on Bluefin |
| ff88a6a | Tail on by default at 10 ms |
| 5579440 | Contended-lead deaf characterization (item 9) |

Each can be dropped. The floors commits ddbaf96 and 9edb970 go with them.

### The sweep: one boot per stack, 120 s a step, tail 5/10/20/0/5/10/20 ms

Image `CONFIG_RADIANT_GATE_MPSL_TAIL_SWEEP` at 0d385b6, ch0 #2002 and ch1 absent
#9999, about 1000 s per stack. Per step, from the gate's counters (`tail_sum.py`):

| Stack | Tail | Bookings refused (`blk`) | `mis` / `busy` / `nosess` / `eo` | Tail chunks per window | Listening credit |
|---|---|---|---|---|---|
| `gate.conf` | 0 / 5 / 10 / 20 ms | 0 / 0 / 0 / 0 | all 0 | 0.04 / 0.95 / 1.0 / 1.0 | 53.0 / 53.1 / 53.1 / 52.9 % |
| BLE | 0 / 5 / 10 / 20 ms | **1.24 / 1.55 / 2.09 / 3.14 %** | 20 ms: `mis` 4, `early` 1 | 0.04 / 0.93 / 0.96 / 0.95 | 52.4 / 52.5 / 52.1 / 51.9 % |
| Thread | 0 / 5 / 10 / 20 ms | 0 / 0 / 0 / 0 | all 0 | 0.05 / 0.87 / 0.98 / 1.0 | **26.8 / 34.7 / 34.9 / 35.1 %** |

(Credit is the search's own `credited_ms`. Compare it within a row only.)

- **Beside Thread the tail is the gain**: listening up from 26.8 to about 35 %, and
  phase coverage from a locked 5 masks to 12-16 moving ones at 5-10 ms and a full
  `0x00ffffff` at 20 ms. A booked grant is fixed-length and never extends, so its
  tail is air Thread has already granted.
- **On `gate.conf` the tail costs nothing and closes the after-slot band**: bin 24 is
  heard (`0x01ffffff`).
- **BLE refused more of the longer bookings.** Each refusal is a tracked slot, so
  without a remedy the tail breaks rule 1 under BLE. d95a0c7 places a refused tailed
  booking again at plain length.
- **The e7749c2 failure did not appear** on `gate.conf` or Thread (`mis`, `busy`,
  `nosess` 0, `eo` 0). BLE at 20 ms had `mis` 4 and `early` 1 in one step.

### At the final image (ff88a6a, tail 10 ms with the re-place)

| Check | Plain branch / master | Tail | Log |
|---|---|---|---|
| 17-offset B leg, `gate.conf` | 15/17 (misses 5.6, 101.3) | **17/17**, in-band 5/5, median 8.9 s; one at 40.2 s (117.5 ms), one at 25.0 s | `fin_B`, `fin_Bc` (control 5/5) |
| BLE, ungranted / granted-unheard (6 x 60 s, interleaved with the plain branch) | 1.23 % / -0.07 % | **1.03 % / -0.14 %** | `bletail_*` |
| `gate.conf` rule 1 (2 x 180 s against master) | master 99.93 % heard | 99.93 % heard, `sched_missed` 0, ext refused 0 | `gatetail_*` |
| Thread in-band (one run each, same boot design as item 4) | 2/5 | **4/5** (5, 132, 222, 237 ms) | `threadtail_*` |
| Thread ping under search, RTT avg/max | 18.2-18.3 / 42-44 ms (plain branch) | 21.3 / 67 ms, 0 lost | `threadtail_head_1` |
| R1 at 12 / 12 s / 20 s | B kept | B kept (A 0 %, B 8.9-10.1 %, as the Garmin always reads) | `r1t_*` |
| R1 walked to 8.7 -> 4.6 ms | A 33.5 %, B 70 % lost (5 ms run) | **A 13 %, B 36 % lost** | `r1t_8` |

With the re-place, BLE ungranted is back at the plain branch's level (blk 5-9 a
reading, but `den`, the refusals that reached an arm, 1-4). **The tail is kept.**

**But the tail exposes item 2's R1 defect in discovery.** In `fin_B`, B at 5.9 ms was
found in 8.8 s, which the plain branch never managed. Once B was tracked, the pair sat
inside the sub-6 ms band, and **channel 0, the sensor already tracked, lost 69 of 244
slots (28 %)** in that trial. The tail run beside Thread did the same at 4.7 ms
(180/243). Master in the same position never finds B, so it never loses A. The 17/17
is real, but at offsets under ~6 ms it comes at the cost of the sensor already being
tracked.

The 40.2 s find is one trial past the plan's "about four sweeps".

The gate tail run 1 also read `eo` 1, `early` 3, `busy` 4 with every slot heard; `r1t_12s`
read `mis` 156 and `nosess` 4285 with a search running and no tracked slot lost in the
window. These are the e7749c2 counters at low rates, with no loss attached that could
be measured.

### Extras

- **Re-placing a refused booking once:** built only for tailed bookings (d95a0c7).
  For plain bookings it was not built, and Stage A's question stays open.
- **A longer first bite at the 16 ms CONTENDED lead:** not tried. The tail's ~8 points
  of listening beside Thread came instead, and there was no time left for a second
  variable.

## Part 3: close-out at the final head

| Check | Result | Log |
|---|---|---|
| Twister, Bluefin | **PASS**, 4327 of 4327 in 14 scenarios at 9edb970, counts OK (floors raised: `radiant.unit` and its three variants +7, `radiant.gate` +2) | `twister_final.txt` |
| Three suites, 5340 DK | **PASS**: gate 71, unit 993 (50 suites), API 65, one boot each | `ztest_{gate,main,api}_final.txt` |
| Zero cost against c4e1ef8 | **PASS**, 8 of 8 rows identical, TI included (head 191285e; later commits are tests and docs) | `zero_cost.txt` |
| Sizes at 191285e, counters out | see below | `size_*.txt` |
| Ack regression, 60 sends each way | 60/60 and 60/60 | `ack_fin/` |
| `ant_ack_pair.py` acceptance, DUT slave | **PASS**: 300 sent, 297 completed, peer 299, 3 `TRANSFER_TX_FAILED`, 340 s | `ack_fin/accept_slave_dut.*` |
| 0xF6, structure version 1 | coverage build (`fin_gate`): the 32-byte field present; plain `apps/dongle` `health.conf`: field absent, so a 30-byte body (`ant_health.py` decodes `[30..31]` only from a 32-byte body) | `f6_*.txt` |

| Image | FLASH | RAM | Against Stage A |
|---|---|---|---|
| L15 `thread.conf;bridge.conf` | 327 920 | 198 440 | +1088 / +64 B |
| L15 `thread.conf;matter.conf` | 823 320 | 253 772 (8372 free) | +1088 / +64 B |
| nRF52840 DK Matter | 816 092 | 245 208 | +1088 / +64 B |

Against master's baselines in d2.md, RAM is +512 B on all three. Every `.config`
reads `RADIANT_BACKEND_NRF=y`, `GATE_MPSL_D2=y` and `SCHED_BOOKING=y`, with no stats
log. Both Matter ELFs carry 3599 `_ZN4chip` symbols and `radiant_matter_sink`.

TI: build and binary comparison only. Nothing in this effort was verified on air on TI.
