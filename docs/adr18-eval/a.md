# Phase 1, candidate (a): held grant plus bridging extension

Branch `wip/adr18-a`, based on master 237b7c4. Measured 2026-11-10 on the
fixed rig. Nothing is merged, pushed or tagged. Raw logs are outside the
repository: see [a-raw-logs.md](a-raw-logs.md). Commits: 8ac89d0, a979ba9,
9913ec1, 56e8450, 6b6e7cb (code), then the report commits.

## Verdict in one paragraph

(a) works as a full fix **on `gate.conf`**, and the on-air A/B/A shows it.
Master found B in 12 and 13 of 17 offsets on the two A legs (1 in-band each). The (a)
image found it in 17 of 17 (5 of 5 in-band), the median time to first packet
fell from 8.9 s to 5.05 s, and every non-zero phase mask read `0x01ffffff`
(all phases) instead of the locked `0x003fcffc`. The tracked channel kept every
slot in an isolated same-instant reading: 487/487 packets, against 486/487 on
the Garmin. The fallback count was 0 on `gate.conf` (1125 bridges, 0 refused).
It took six on-air defects to get there, all invisible to the fake radio.
**Under BLE** (1 s advertiser) it works as well: 6 of 6 in-band finds at about
5 s against master's 0 of 3, all phases heard, about 4 % of bridges refused,
and tracked loss the same as master's (4.9-5.5 % against 4.9 %). **Under Thread
it fails.** MPSL refused every extension, so no bridge was ever granted, and the
scheduler, still planning held boundaries, lost 2 tracked slots in every 15
(13.4 % and 15.6 % on two runs, against 4.5 % on master between them and a
Garmin at 0.2-1.2 %). **On an R1 pair it fails rule 1 on every stack.** In the
on-air R1 walk the pair still got one slot per period between them, as on
master. (a) gave that slot to A in two periods of three, where master gave it
to A every period: A lost 33.1 % against 0 %. The mechanism is not explained.
**Verdict: (a) as built must not merge.** It fails the plan's kill criterion
(a refused extension may cost listening, never a tracked slot) under Thread and
on an R1 pair. What it proves is that one held grant removes the arm-lead bands
completely wherever the arbiter grants extensions (stack-free and BLE). If
neither (d) works, (a) is worth rebuilding only with two changes: it switches
itself off when extensions are refused, and the R1 cycling is found and fixed.
If (d2) works under Thread, (d2) is the long-term design and (a) adds little to
it (reasons in [Composition](#composition-and-whether-a-is-still-worth-building)).

## What was built

All core code is under `CONFIG_RADIANT_SCHED_HELD_GRANT` and all gate code
under `CONFIG_RADIANT_GATE_MPSL_HELD_GRANT`. The latter defaults to y when
`RADIANT_BACKEND_NRF_GATE_MPSL` is set and selects the former. At run time the
scheduler's planning is active only if the backend sets `caps.held_grant`.

**Gate** (`radiant_radio_nrf_gate_mpsl.c`):
- **Bridge.** On the elastic class, a follow-on over `FOLLOW_ON_MAX_US`
  (3000 us) is a bridge. It is left out of the request and grown in one
  extension at `bridge_at = t_to - (PLACE_LEAD_US + HEAD_MARGIN_US + 1500 us)`:
  decide at the placement deadline. A tracked window's own 4.2 ms follow-on is
  never a bridge, because tracked windows are not elastic.
- **Bridge in the request.** A fresh chunk whose decide point falls before its
  first compare (the first bite) asks for the bridge in its request. MPSL
  answers that at placement, a whole lead ahead (F2/F3: BLOCKED arrives at the
  request instant).
- **Continuing the hold.** An elastic receive that starts inside the air the
  hold has asked for is served from the held grant (`cont_ok`). A fixed-class
  receive is served only if its own air is already granted.
- **Hold cap.** The extended total is capped at `HOLD_MAX_MS` (2000 ms default)
  instead of MPSL's 100 ms request ceiling (F1). Reaching it ends the hold
  exactly as `EXTEND_FAILED` does.
- **Counters** `bridge_ok/failed/late/in_req`, `hold_capped`,
  `cont_ok/miss`, `hold_max_us`, and an optional 5 s `GSTAT`/`GHOLD` log
  (`CONFIG_RADIANT_GATE_MPSL_STATS_LOG`, `gate_stats.conf`).

**Scheduler** (`radiant_sched.c`):
- **Bridged chunk.** A scan chunk in front of a committed operation stops
  `HELD_GAP` before it (in-grant lead + dispatch slack + frame tail +
  `HELD_HANDOFF_US` 1 ms, about 1.5 ms), not a whole arm lead before it. It
  asks for a bridge covering the committed **cluster**, meaning every bounded
  operation that starts within one arm lead (plus span) of the cluster's end,
  capped at 60 ms. The R1 pair is exactly such a cluster.
- **Span rule (ADR 0013 rule 1).** A bridged chunk armed in-grant must start at
  least `arm_lead + 3 ms` before its limit, so the gate's decide point lies
  inside it. Otherwise it gets today's plan, unless the air is already held
  (the inside of a cluster).
- **Scheduler-placed boundaries.** A chunk that `max_window_us` or its dwell
  ends before the limit asks only for a 3 ms reach. If it would end inside the
  band from which the next chunk could no longer span the decide point, it is
  cut 4 ms before that band.
- **Resuming in the hold** (`held_cont()`). A continuous scan is armed into the
  live hold at in-grant lead + 500 us.
- **Reserve and preemption.** An in-grant arm never shrinks the reserve it was
  armed into. `want_preempt()` uses the in-grant reach for an operation the
  bridge covers. The expiry sweep judges such an operation against the
  in-grant floor (`held_covers()`).
- **Refused bridge.** A refused bridge (DENIED or ABORTED) gets today's plan
  for the same gap (`held_skip_until`), not a second request.
- **HAL.** `caps.held_grant`, `RADIANT_HOLD_RESERVE_MAX_US`, and
  `RADIANT_RX_ELASTIC`, which marks a short scan chunk elastic; the nRF backend
  maps it to the NORMAL class.

**Search** (`antr/ant_radio_radiant.c`): on a held-grant backend, a set that
finishes in a chunk that really ran posts the next set from the completion, at
the in-grant lead, instead of leaving it to the 50 ms pump at a fresh placement
lead. **Known race (prototype):** if this lands between the pump's slot test
and its request, the sweep skips one set.

**Tests**: 9 scheduler cases and 7 gate cases (listed in
[Tests](#tests-and-floors)); `fake_radio.c` models the held elastic grant, the
bridge, refused bridges and bridges in the request. **Tools**:
`tools/ant_disc_phase.py --r1-walk` and 9 host tests. **Bench**: `gate_stats.conf`,
`ble_load.conf` + `src/ble_load.c` (a 1 s advertiser restoring the BLE load
retired in fc04371).

## The common scorecard

### Deaf fraction (ztest, `test_sched.c` shape helpers, fake radio with the held-grant model)

Leads are the MPSL build's (24418 us, in-grant 200 us, `max_window_us` 94.2 ms),
9 periods with the first 3 discarded, N tracked channels evenly spaced: the
worst case.

| Tracked | Master (0b) | (a), predicted | (a), asserted | Longest gap | Locked? |
|---|---|---|---|---|---|
| 1 | 30.0 % | ~1.6 % | **1.6 %** (4.0 ms) | 2.6 ms | locked, but each band is under 3 ms |
| 2 | 40.2 % | ~2.5 % | **2.5 %** | 2.6 ms | locked |
| 4 | 80.3 % | ~4.1 % | **4.1 %** | 2.6 ms | locked |
| 8 | 100 % | ~8.4 % | **8.4 %** | 2.7 ms | locked |

The prediction per tracked channel is the window (0.4 ms) plus the before-gap
(~1.5 ms) plus the after-gap (frame tail + latency + in-grant lead + 500 us,
~0.9 ms), plus ~0.8 ms per `max_window_us` chunk boundary. The ceiling the test
asserts is `n x (window + 2 x 1.7 ms) + 3 x 1.7 ms`. The bands stay
phase-locked to the tracked slot (`moves=0`), but none is wider than a third of
a frame interval at the narrowest search dwell. What the old bands were (three
24.4 ms leads, 30 %) is gone, not moved. The 2 s hold cap adds one placement
lead (24.4 ms) every ~8 periods, at a drifting phase: about 1.2 % averaged,
never locked.

- **Slow handoff** (terminal delivered 1.2 ms late): 3.0 %, longest gap 3.8 ms,
  every slot bridged.
- **A pass mid-chunk** (thread tick every 10 ms): no slot lost.
- **Every bridge refused**: 10/10 fall back, 0 tracked slots lost, 11/11 slots
  received.
- **3 of 20 refused**: 0 slots lost on either of two channels.

**On air, `gate.conf`:** every non-zero `TEMPDIAG` mask in the B leg was
`0x01ffffff` (60 of 66; the rest are trial edges). The master legs read
`0x003fcffc` (71 of 104 in A). Coverage counter: `sets_advanced_uncovered`
equals `hold_capped` exactly (42/42, 48/48): the only uncovered sets are those
cut by the 2 s cap.

**Under Thread** (attached MTD, 5340 DK leader): there is no hold to measure. MPSL refused every extension (`ext=0/2964` per 150 s, both (a) runs), so no bridge was ever granted (`bridge=0/0`, `hold_max=0`), and every chunk is a fresh placement that ends at its first refused extension (`aborted` = `ext` failed). The 10 ms phase bins heard per set were **16 of 25 on (a)** (`0x00ed9b6d`, 18 of 21 masks in the second run) against **7 of 25 on master** (`0x00249124`, 32 of 36) in the same session. Both are locked, not drifting. So (a) under Thread hears more phases than master, about 36 % deaf against 72 %, but by a side effect of its scheduler-placed chunk boundaries, not by holding the grant. Bins are 10 ms wide, so these fractions are coarse. In-band discovery: master 0 of 3, (a) 1 of 4 completed trials (131 ms, found at 22 s).

**Under BLE** (1 s connectable advertiser, `ble_load.conf`): the hold works. Bridges were granted 410 of 427 and 417 of 433 times (about 4 % refused) in the two rule-1 readings, and the hold reached 0.99 s (refusals end it before the 2 s cap; `capped=0`). Every non-zero mask in both (a) discovery runs was `0x01ffffff`, all 25 bins (51 of 51). Master with the same advertiser read `0x003fcffc` or `0x003ffffc` (18-20 of 25 bins, locked; 32 of 36 masks). So under BLE, (a) closes the bands as on `gate.conf`: deaf by the coarse bins 0 % against 20-28 % on master. In-band discovery: **(a) 6 of 6, all at 5.0-5.3 s; master 0 of 3.**

### R1

| Case | Master | (a) |
|---|---|---|
| ztest, 5/12/24 ms, a search running | later slot lost 12/12 every case | **both kept 12/12** (the cluster bridge) |
| ztest, 12 ms, nothing searching | later slot lost | **later slot lost 12/12** (no elastic grant to hold) |
| on air, R1 walk (B acquired at +40 ms, walked to +12 ms, 60 s window at 12.6-9.8 ms) | **A 244/244 (0 %), B 0 (100 %)**; reference on B 0.41 % | **A 163 (33.1 % lost), B 81 (66.8 % lost)**; reference on B 3.65 % |
| on air, B found in-band next to A (B leg, 3 trials) | B never found | **A 169-196/244, B 48-75 of ~220** |

In the ztest, (a) fixes R1 only while a search runs, and in steady state, with every sensor paired, it changes nothing. **On air it does not fix R1 even with a search running** (the walk row, explained below): the pair still gets one slot per period between them. The last row is new territory: master never found B there, so it never formed the pair. With (a), B is found in about 5 s and the pair forms. After that both channels lose slots, A by 20-30 %, which master's R1 (A 100 %, B 0 %) does not. The DUT counters show ch1 losing and re-acquiring B repeatedly. That cycle (track, R1 misses, back to search, hold, re-acquire) costs A as well. It is not explained, and the fake R1 cases, which keep the search running, do not reproduce it. **This is a rule-1 finding against (a) as built, restricted to an R1 pair.**

**The R1 walk, read from the logs** (`r1walk/`, `r1walk_master/`). In both runs the DUT received 244 frames in the 60 s window, A and B together: one per period (60 s / 246.3 ms = 243.6). Master gives that one slot to A every period. (a) gives it to A two periods in three and to B one in three, so A loses a third of its slots and B gets a third of its own, which a host sees as a sensor that keeps dropping out. **(a) does not give the pair a second slot; it moves the one slot between the channels.** For A, that is a rule-1 failure and worse than master. What the logs show of the mechanism:
- A search is running all window long. The scan heard 79 frames from A (handed to ch0, `claim=0`) and 79 from B (handed to ch1, `claim=1`) in the window's ~58 s of log. That is every third period, for both.
- B is received only by the scan: 79 of its 81 packets. ch1 acquires B, loses its next tracked window to R1, and is searching again within a period or two. On master ch1 never acquires B in the window at all.
- A is received about 84 times in tracked windows and 79 times by the scan. Master's log shows A in tracked windows only (`rxpath` `tracked` +122 per 30 s, against +41 on (a)).
- No ch0 window was reported MISSED: the `HDIAG miss` instrument logged only 8 ch1 misses, all before the window. So the lost A slots end DENIED, FAILED or OK-with-nothing-heard, or are never planned, and these logs cannot tell which. The image has the health counters, but no 0xF6 reply was taken after the walk.
- Bridges ran all through (`bridge` +7 per 5 s, 0 refused, `hold_max` 0.95 s): the hold is working, and the cluster bridge should cover both A and B while the scan runs. Why it does not is not explained.
- Ruled out: the reference stack (the Pro Micro on B lost 3.65 %, not two thirds), and a frame collision on air (A and B are 9.8-12.6 ms apart, with frames under 1 ms long).

The walk is one run per image (A/B, not A/B/A), but the 33 % against 0 % gap is far outside the room floor, and the same-instant reference was under 4 %.

**R1 harness.** `ant_disc_phase.py --r1-walk` opens B at `--r1-start-ms` (40 ms, clear of every band), waits for the DUT's ch1 to claim a frame from B (a `TDIAG` line with `claim=1`), then walks B's phase by running its channel period `--r1-step-counts` (2 = 61 us a period) short or long, and restores it on arrival. The DUT's whole session is recorded (`ant_verify --record`), and each master's packets are counted only in the measuring window. A RadiANT master refuses a period change on an open channel (`radiant_channel_period_set()` requires `assigned_closed`), so master B must be a real ANT stack. Here that is the Garmin USB-m, with the Pro Micro (direct backend, no MPSL, so no R1) as the reference receiver. The walk aborts with a message if B refuses the change. 9 host tests.

### Prototype and tests

See [What was built](#what-was-built). The fake radio models the gate's held elastic grant: which arms it serves in-grant, the bridge, refusal at the decide point or at placement. The six on-air defects below show its limits. It cannot see CPU latency between `now` and the arm, a pass mid-chunk, or the search layer's re-posting. Three of those six now have a ztest; the other three are in code the unit image does not compile (`antr`) or are latency the fake does not model.

### On air (fixed rig, `gate.conf`)

A = master image (237b7c4, `gate.conf;phase_diag.conf`); B = 6b6e7cb (`gate.conf;phase_diag.conf;gate_stats.conf;health.conf`). The same 17 delays (0..240 ms by 15, shuffle seed 1), 60 s per trial, ch0 pinned to #2002 (Dongle), ch1 wildcard HR, master B = Pro Micro #5005.

| Leg | Found | In-band found | Median first packet | Mask |
|---|---|---|---|---|
| A (master) | 12/17 | 1/6 (221.0 ms, band edge, as in 0b) | 8.97 s | `0x003fcffc` 71x |
| **B ((a))** | **17/17** | **5/5** | **5.05 s** (one at 13.6 s) | `0x01ffffff` 60x |
| A' (master) | 13/17 | 1/5 (220.7 ms, band edge) | 8.97 s | `0x003fcffc` 65x |

B's in-band offsets were 4.4, 19.7, 131.3, 221.1 and 235.7 ms, all found in 5.02-5.06 s. A's in-band misses were 4.8, 20.2, 131.4, 133.7 and 235.7 ms. Pass against the ADR's criteria: 17/17, median within one sweep of the control (7.4 s in 0b), none beyond four sweeps, and the mask moved from locked to full. Gate counters for the leg: `bridge=1125/0`, `capped=88`, `cont=4639`, `miss=0`, `blocked=0`, `hold_max` 2.0 s.

**Isolated rule-1 reading** (no R1 pair): ch0 tracks #2002, ch1 searches for absent #9999 (the search never ends), and the Garmin tracks #2002 in the same window.

| Image | DUT ch0 | Garmin | `sched_missed` | Notes |
|---|---|---|---|---|
| a979ba9-era B6 (before the fixes below) | 273/366, **24.8 %** | 0 % | - | the defect |
| B7, 3 ms handoff | 17.8 % | 0 % | - | not the cause |
| B8, health counters | 17.8 % | 1.6 % | 65 | misses ~ 2 x caps (coincidence) |
| B9, cap 60 s | 21.3 % | 0 % | 76 | not the cap |
| B10, HDIAG log | 10.6 % | 1.2 % | - | every miss: bridge to that slot still running |
| B11, expiry fix | **487/487, 0.00 %** | 0.41 % | 0 | fixed |
| **6b6e7cb (final)** | **487/487, 0.00 %** | 0.41 % | **0** | `bridge=445/0` |

**Thread leg** (`apps/dongle_thread` `thread.conf;phase_diag.conf;health.conf`, plus `gate_stats.conf` on (a); MTD attached to the 5340 DK leader, `ot child table` read before and after each run; the same isolated rule-1 reading, 120 s, Garmin on #2002 in the same window; then three in-band `ant_disc_phase.py` trials at delays 195, 75 and 180 ms). Run order (a), master, (a), within about 30 minutes.

| Run | Image | DUT ch0 | Garmin | `sched_missed` / `sched_denied` | Gate | In-band found |
|---|---|---|---|---|---|---|
| `thread/` | (a) 6b6e7cb | 421, **13.4 %** | 1.23 % | **64 / 64** | `ext=0/2964 bridge=0/0 inreq=33` | 1 of 2 (third trial lost when that session ended) |
| `thread_master/` | master 237b7c4 | 465, **4.5 %** | 0.40 % | 0 / 0 | (no stats log on master) | 0 of 3 |
| `thread2/` | (a) 6b6e7cb | 410, **15.6 %** | 0.19 % | **64 / 64** | `ext=0/2964 bridge=0/0 inreq=33` | 0 of 2, then an **MPSL assert** |

Phase 0a measured master-era firmware under the same Thread rig at 0.14 % and 0.26 % (Garmin 0.59 % and 0.48 %) over 30 minutes, so the master figure here (4.5 %) is high for master; the room was not quiet. The (a) runs are worse than the master run in between them by 9-11 points, on the same-instant reference, and that loss is in the scheduler's own counters, so the comparison does not depend on the room.

**Where (a)'s Thread loss comes from.** The `HDIAG miss` instrument logged every one of the 64 misses. Each is a ch0 window reported MISSED **1.72 ms before it opens**, with no hold live (`live=0`, `hc=0`), and the last plan a bridged scan chunk ending 1.48 ms before that window (`plan=1`, bridge 8.9 ms, or `plan=2`, reach 3 ms). The misses keep a fixed cadence, alternately 5 and 10 periods apart (1.23 s and 2.46 s), which is 2 slots in every 15 (13.3 %, the measured loss). `plan=2` and `plan=1` split them about evenly (32/32 and 34/30). The scan's own count of chunks that never listened is also 64 (`TEMPDIAG other=64`), equal to `sched_denied`. So each lost tracked slot is the slot behind a bridged chunk that was denied, and the denial reached the scheduler too late to place the slot afresh. The design assumed a refusal arrives either at the decide point (a lead ahead) or at placement (also a lead ahead). Here it arrived about 1.7 ms ahead. It is not a too-near refusal (`near=0`) and not an MPSL BLOCKED (`blocked=0`); which gate path delivers it was not isolated. This is a rule-1 failure on a refusal path the fake radio does not model.

**The MPSL assert.** The second (a) run's DUT halted with `*** MPSL ASSERT 69:108` at 7133 placements, `ext=0/5613`, its last signal an extension failure. Phase 0a recorded the same assert once, on a master-based image under the same Thread rig (6575 placements, `ext=0/4894`). It is therefore not caused by (a), but (a) was not shown to leave its rate unchanged.

**BLE leg** (`coex.conf;ble_load.conf;phase_diag.conf;health.conf`, plus `gate_stats.conf` on (a); the same rule-1 reading and three in-band trials as the Thread leg). The master control is 237b7c4 plus the branch's four app-only BLE-load files and nothing in `radiant/`, built in a scratch worktree. Run order (a), master, (a).

| Run | Image | DUT ch0 | Garmin | `sched_missed` / `sched_denied` | Gate | In-band found |
|---|---|---|---|---|---|---|
| `ble/` | (a) 6b6e7cb | 460, **5.54 %** | 0.21 % | 2 / 11 | `bridge=410/17 blocked=6/207 ext=99323/119` | **3 of 3** (5.0-5.2 s) |
| `ble_master/` | master + BLE load | 463, **4.93 %** | 0.40 % | 0 / 41 | (no stats log) | 0 of 3 |
| `ble2/` | (a) 6b6e7cb | 463, **4.93 %** | 0.21 % | 3 / 42 | `bridge=417/16 blocked=24/218 ext=99376/117` | **3 of 3** (5.0-5.3 s) |

Tracked loss is the same on both images (4.9-5.5 %), well above the Garmin (0.2-0.4 %) on both, so the excess is the MPSL build's under this advertiser, not (a)'s. `sched_denied` counts scan chunks as well as tracked windows, so it is not a slot count. (a) did log 2 and 3 tracked windows MISSED per reading (`HDIAG`, all `plan=2`, a chunk with a 3 ms reach), where master logged none; that is under 1 % of slots and inside the run-to-run spread here, but it is not zero. During the discovery trials, once B was found next to A, the (a) image logged 41 and 45 such misses: that is the R1-pair behaviour above, not the BLE load. The advertiser's boot line was not captured (the log reader starts after the flash), but `blocked` and the failed extensions, both 0 on `gate.conf`, show a second stack on the schedule.

### The six on-air defects (each invisible to the fake)

1. **Resuming against the granted end** (56e8450). The chain keeps `grant_end` only about 1 ms ahead, so half of all resuming chunks were refused too-near (`cont=293 miss=313`, `bridge=0/0`). Fixed: test against the wanted end. A gate ztest covers it.
2. **Set boundaries** (6b6e7cb). The search reposted each new set from the pump at a fresh placement lead, so every set boundary ended the hold (`hold_max` 192 ms, `bridge=0/0`). Fixed in `antr` (no ztest: the unit image does not compile `antr`).
3. **A misplaced fix**, recorded because it nearly went in: 1 ms boundary slack looked like the cause of (2) and was raised to 4 ms. It was not the cause, but the larger slack is kept, with a slow-handoff ztest.
4. **ETIME at the bare in-grant lead.** Every resuming chunk failed `program_rx()` with ETIME (`TEMPDIAG other == cont`). Fixed: plan it 500 us out.
5. **Expiry sweep** (6b6e7cb). A thread tick or a frame mid-chunk reported the bridged tracked window MISSED: 18-25 % of the tracked channel's slots (table above). Fixed with `held_covers()`. A ztest (tick every 10 ms) covers it.
6. **3 ms handoff experiment.** It widened every band and was reverted to 1 ms. Recorded because the B11 image measured above has it.

## Costs

**Zero cost: PASS.** `scripts/check_zero_cost.ps1 -BaseRef 237b7c4`, run on the branch with only report commits after 6b6e7cb (so the code is 6b6e7cb's): all eight plain rows have the same allocated sections, the same defined-symbol set and a byte-identical `zephyr.bin` against the merge base. Rows: Feather, nRF52840 Dongle, Pro Micro, 5340 net core, L15 and LM20 `apps/dongle`, and TI `apps/dongle_ti` with and without `ti_gate.conf` (hal_ti present; the script fails rather than skip TI). Log `zero_cost.txt`. TI is build-only on this bench: none of this is verified on air on TI.

**Sizes** (nm/`.config` method; baselines from the brief):

| Image | Master | (a) 6b6e7cb | Delta |
|---|---|---|---|
| L15 `thread.conf;bridge.conf` FLASH | 321 736 | **324 168** | **+2 432 B** |
| L15 `thread.conf;bridge.conf` RAM | 197 928 | **198 048** | **+120 B** |
| L15 `thread.conf;matter.conf` FLASH | 817 228 | **819 660** | **+2 432 B** |
| L15 `thread.conf;matter.conf` RAM | 253 260 (8 884 B free) | **253 380** (8 764 B free) | **+120 B** |
| nRF52840 DK Matter FLASH | 809 964 | **812 396** | **+2 432 B** |
| nRF52840 DK Matter RAM | 244 696 | **244 824** (17 320 B free) | **+128 B** |

The bridge `.config` reads `RADIANT_BACKEND_NRF=y`, `GATE_MPSL=y`, `GATE_MPSL_HELD_GRANT=y`, `SCHED_HELD_GRANT=y`, and no stats log. The L15 Matter `.config` reads the same four (plus `CHIP=y`), and its ELF defines `radiant_radio_init`, `gate_init`, `gate_held`, `held_cont`, `radiant_matter_sink` and 4783 CHIP symbols, so the build that was measured is a Matter image with (a) in it. The nRF52840 Matter build reads back the same (`CHIP=y`, `GATE_MPSL_HELD_GRANT=y`, `SCHED_HELD_GRANT=y`; the same symbols in its ELF). Its first attempt stopped on a compiler crash with no error message (`size_matter52_fail1.log`, during the host's memory pressure); the rerun built clean. Why the 52840 delta is 8 B larger than the L15's was not checked (alignment is the likely reason). The RAM is the scheduler's three state fields and its debug struct, plus the gate's stats and bridge state.

**ZLI and SWI time per period** (from `GSTAT`, final image, 136 s): extensions 748/s, about 184 per period, all `TIMER0` returns of a few us each. Master's chain extends about 84 times per 94.2 ms chunk, about 170 per period. So the zero-latency work is about the same, plus one `radiant_radio_now()` per compare for the bridge test. Placements fell from about 1.8 per period on master to 86 in 136 s (0.16 per period: after caps and bridges in the request), so the `request_work_fn` round trips and `SIGNAL_START` entries mostly disappear. SWI: one in-grant arm per chunk boundary from the trampoline, as master does after every terminal, plus one `api_post_search_window()` per set from the completion (about 2 per second). Not measured in microseconds.

**HAL contract.** Two additions, both behind the symbol: `caps.held_grant`, and `RADIANT_RX_ELASTIC` with the bridge semantics of a large `follow_on_us`. The one-operation rule is **not** broken: one operation is armed at a time, and the hold is the backend's business. The TI backend needs no edit: it never sets `held_grant`, never sees the flag, and an unknown flag is ignored by both backends. TI is not verified on air.

**Tracked-slot risk, per refusal path:**

| Path | What happens | Slot lost? |
|---|---|---|
| Bridge refused at the decide point | chunk aborted ~25.7 ms before its end; committed op placed afresh with >= 24.4 ms notice | no (gate ztest, sched ztest, 0/1125 on air) |
| Bridge refused at placement (in the request) | chunk DENIED at once; committed op placed afresh | no (ztest) |
| Hold cap (2 s) | like a refusal at whatever point it falls; a bridged chunk's cap is a refused bridge | no in the isolated reading (0 misses, 42 caps) |
| Resuming chunk refused (no grant) | too-near DENIED; scan idle until the next pass | no tracked slot; costs listening (`cont_miss`, 0 on air) |
| ETIME from `program_rx()` on an in-grant arm | synchronous; MISSED; a scan slot drops until the pump re-posts it (up to 50 ms) | a tracked window planned in-grant has 1.5 ms of slack and was never seen to fail; the scan case was defect 4 |
| Extension refused mid-chunk (JIT) | `EXTEND_FAILED`: elastic chunk aborted (ADR 0013 yield) | no |
| `program_rx()` fails inside a cluster | hold released; the next op of an R1 cluster is within a lead | yes, that op, which is today's R1 outcome (no worse) |
| R1 pair cycling between track and search (on air) | A loses 20-30 % (in-band find), 33 % (R1 walk); the pair keeps one slot per period between them | **yes, unexplained** |
| Bridged chunk denied about 1.7 ms ahead (Thread, every extension refused) | the tracked window behind it is reported MISSED | **yes: 2 slots in 15 (13-16 %), measured twice** |

**Longest armed time without a terminal:** unchanged. No operation is longer than `max_window_us` (94.2 ms), and a hold of up to 2 s is a sequence of operations, each ending at its own close. The A9-3 reclaim (100 ms past an op's end) is never approached; a refused bridge or the cap ends an op early, never late.

## Composition, and whether (a) is still worth building

- **With (e)** (per-stack lead): (a) makes the lead matter only at the fallbacks and the cap. Composes freely.
- **With (d2)**: F2 says a scan extending over a tracked start before that start's request is placed blocks the request 100 %. (a)'s bridge is exactly that extension, so (a) and (d2) are alternatives for the same boundary, not layers. They could share a chunk only if (d2) owns tracked slots and (a) is limited to scan-to-scan boundaries (the reach). That keeps (a)'s set-boundary and chunk-boundary gains and drops its bridges.
- **With (d1)**: (d1) closes boundaries with `ACTION_REQUEST` at the end of the grant; (a) avoids having the boundary at all. Under BLE, F3's 0.3-1 % blocking per request is lower than the chance that a 300 ms hold meets an advertising event (F1: 5 of 20). **Under Thread the comparison depends on whether the arbiter grants a fresh request with notice where it refuses every extension.** It refused 100 % of extensions here (and 41k of 41k per 30 minutes in Phase 0a) while granting 100 % of placements (`blocked=0`). So under Thread (a) has no mechanism at all, and a (d) that works by requests, not extensions, is the only candidate that can close the bands there.
- **Worth building if a (d) is viable?** On `gate.conf` and under BLE, (a) is as good as any (d) can be: one grant, no notice at all. Its weaknesses are structural, and the Thread leg confirmed the first of them. Every boundary depends on an **extension**, which is exactly what another stack with the air scheduled refuses: under Thread, all of them. And on air an R1 pair is not fixed even while something searches. A (d) that places fresh requests with notice is judged by the arbiter against a *request*, which 802.15.4 and BLE grant with notice. **If (d2) passes under Thread, build (d2), not (a). If neither (d) passes, (a) could still serve BLE-only and stack-free builds, but only after it falls back to master's plan when extensions are refused, and after the R1 cycling is fixed. As built it is a rule-1 regression on Thread builds and on R1 pairs.**

## Kill criterion, checked

The plan's kill criteria for (a): **the fallback count must be 0 on `gate.conf`**, and **a refused extension may cost listening, never a tracked slot** (ADR 0013 rule 1).

- Fallback count on `gate.conf`: **0** (1125 bridges, 0 refused, B leg; 445/0 in the isolated reading). **Passed.**
- Rule 1, isolated: **passed** on the final image (487/487 against the Garmin's 486/487). Failed on every earlier image (defect 5), which is why that row of the table exists.
- Rule 1, R1 pair formed after an in-band find: **not passed** (A 20-30 % lost, unexplained). This kills (a) **as built** for a bridge that tracks several sensors near each other in phase, until it is explained.
- Rule 1, R1 pair formed by the walk (B acquired clear of A, then walked to 12 ms): **failed**. A lost 33.1 % against 0 % on master, with a same-instant reference at 3.65 % and 0.41 %. The pair kept one slot per period on both images.
- Under BLE: rule 1 **as good as master** in loss (4.9-5.5 % against 4.9 %), but 2-3 tracked windows per 120 s were reported MISSED, against none on master. Bridges refused: about 4 %.
- Under Thread: **failed.** The fallback count is not 0 but total: MPSL refused every extension, and no bridge was ever granted. Rule 1 fails: 64 tracked windows MISSED per 150 s on both (a) runs (13.4 % and 15.6 % lost), against none on master in the same session (4.5 % lost, every window armed). As built, (a) must not be on in a Thread build. The least it would need is for the scheduler to stop planning held boundaries once the gate reports extensions refused, and to find which denial path reports the bridged chunk only 1.7 ms ahead.

## Tests and floors

- Scheduler, held grant (9): deaf fraction by tracked count, the held-off control, all bridges refused, some refused, R1 with a search, R1 without, slow handoff, mid-chunk tick.
- Gate (7): one extension at the decide point, refused bridge at the decide point, elastic continuation, past 100 ms to the cap, tracked follow-on not a bridge, short fresh chunk asks up front, resuming tested against the wanted end.
- On the 5340 DK: `radiant\tests` all pass (`radiant_sched` 69/69), `radiant\tests\gate` 39/39, `radiant\tests\api` 65/65.
- Twister on Bluefin, from a `git archive` of 6b6e7cb: **13 scenarios, 4259/4259**, `check_test_counts` OK, observed equal to the floors.
- Floors raised in the same commits: `radiant.unit`, `.sec_modes`, `.no_pairing` and `.ack_faults` +8 each (986/986/934/993); `radiant.gate` 32 -> 39; `min_testcases` 4220 -> 4259.
- Not shown red on master: the slow-handoff case was not run against the 1 ms slack. The other new sched cases depend on `caps.held_grant`, which does not exist on master.

## Deviations and things not done

- The plan's `ant_disc_phase.py` addition needed a stack that accepts a period change on an open channel. RadiANT refuses it, so master B for the walk is the Garmin, and the Garmin is not the reference in that reading (the Pro Micro is).
- `coex.conf` no longer advertises (fc04371). The BLE leg uses a new bench-only `ble_load.conf`. The master BLE control is therefore not plain master. It is 237b7c4 plus the branch's four app-only BLE-load files (no `radiant/` change), built in a scratch worktree, `C:\wt_a_ble`.
- The B leg was rerun three times on intermediate images (B1 at 9913ec1: 12/17, master-like; B2 at 56e8450: aborted; B11: aborted) as defects were found. Only the final run is in the table; the others are in the raw logs.
- **The Thread and BLE legs are shorter than the scorecard's discovery legs.** Each run was one 120 s rule-1 reading and three in-band offsets (195, 75 and 180 ms delays), not the 17-offset rig. Deaf fractions under the stacks come from the 10 ms `TEMPDIAG` bins, not from the ztest helper, because the fake radio has no model of a second stack.
- **The stack legs are A/B/A in shape ((a), master, (a)), but the R1 walk is A/B only** (one run per image).
- **Phase 0b has no Thread loss figure**, so the master Thread control was run in this session. Phase 0a's Thread soak (0.14 %/0.26 %) is cited only for context: it used older firmware.
- **The rule-1 readings use the `ant_sim` heart-rate profile on the Dongle (#2002)**, as in the isolated `gate.conf` readings. Loss is packets against expected, not exact.
- **The first (a) Thread run's third discovery trial was lost when the previous session ended.** The second run's third trial was lost to the MPSL assert. In-band discovery under Thread therefore rests on 4 completed (a) trials, against 3 on master.
- **The R1 cycling and the Thread denial path were not root-caused.** Each was given a short look at the existing counters and `HDIAG` lines. Neither got a new instrument, because the plan does not ask for fixes in this phase.
- The DUT was left on the (a) BLE image (`img_b_ble_6b6e7cb.hex`). The 5340 DK is still the OpenThread leader. Neither master was reflashed.

## Unexplained

- R1 pair cycling costing the earlier channel 20-30 % after an in-band find and 33 % in the walk (above). Known: one slot per period between the two channels on both images; a search running all window; B received almost only by the scan; no ch0 window reported MISSED. Ruled out: the reference stack, and an on-air collision. Not known: how the lost A slots end (DENIED, FAILED or unheard), or why the cluster bridge does not keep both.
- Under Thread, which gate path denies a bridged scan chunk only about 1.7 ms before the tracked window behind it. Ruled out: too-near (`near=0`) and MPSL BLOCKED (`blocked=0`).
- Master's 4.5 % Thread loss and the 4.9-5.5 % BLE loss on both images, with the scheduler reporting no missed tracked windows on master. Both are well above the Garmin's figure, and both are outside (a)'s scope.
- The MPSL assert `69:108` (closed source), now seen once on a master-based image (Phase 0a) and once on (a), both under Thread.
- Why a hold sometimes ended before the 2 s cap on the B9 (60 s cap) image (`hold_max` 2.9 s, `inreq=31`). Set boundaries no longer end holds, and the cause of the remaining ends was not isolated.
