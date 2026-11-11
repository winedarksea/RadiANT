# Phase 3 Stage C: R1 pairs closer than the reply reserve

Measured 2026-11-11 on the nRF54L15 DK (J-Link `1057737173`), branch
`wip/adr18-impl`, NCS v3.4.0. Fixes a merge blocker found in Stage B
([phase-3b.md](phase-3b.md), item 2): an R1 pair closer than about 6 ms cost
slots on both channels, including the one already tracked, which master keeps.
Design: [ADR 0021](../decisions/0021-tracked-slots-are-booked-a-period-ahead.md).
Raw logs: `C:\adr18_logs\phase-3c\` (index at the end). Nothing here ran on TI.

## The mechanism

Written down from the Stage B logs and the code before anything was changed.

### Why the pair overlaps at all

A tracked slave's request, booked or fresh, is its window (guard up to 400 us
each side) plus the 4150 us acknowledged-reply reserve, plus the gate's 250 us
head margin, 250 us tail margin and 750 us end margin. MPSL keeps 0.2-0.4 ms
between two timeslots (Phase 0c F2). So A's timeslot ends by tA + 5.55 ms and B's
starts at tB - 0.65 ms: **two tracked requests overlap when B - A is under about
6.2-6.6 ms.** MPSL is first come, first served on overlap, and priority does not
rank our two sessions (Phase 0c F2).

### There is no separate 11-12 ms mechanism

Stage B reported refusals "near 11 ms" in `r1_head12`. That figure was a linear
extrapolation of the walker's phase after the window. The DUT's own packet
timestamps (`r1w_dut.antser`, mean B - A per 20 s) read 12.0 ms in the window,
then 10.5, 7.9, 7.2 and **5.2 ms** at the 20 s bucket where the refusals began
(the DUT's packet count per 20 s drops from 72 to 40 there). The refusals started
at 5-6 ms, inside the geometry above, and showed the same cycle as `r1_head5`
(below). Nothing reaches 11 ms.

### Why the earlier channel loses

Shown, from `r1_head5`:

- The per-slot pattern in the 60 s window (gaps between received packets) is a
  strict three-period cycle: **A ok, ok, miss; B ok, miss, miss.** A 33.5 % and
  B 70 % are 1/3 and 2/3 to within one packet.
- The gate counters per 5 s (about 40.6 slots): bookings placed 28 (hit 14,
  denied 14), fresh tracked requests 14 (granted 7, blocked 7, delivered as async
  denials, `sent bl` +7), refusals by session `s0` 7 and `s1` 14.

The cause, from the code: **`request_work_fn()` placed waiting requests in session
index order, not in the order the core made them.** After B's booking is refused,
B's arm is denied at A's completion. The pass then arms A's next slot as a fresh
request (it has no booking, and nothing else is armed), which goes to the idle
session, and `book_ahead()` books B's next slot into the session A's grant is
just releasing. When that session is `s0`, the work queue runs after the grant has
ended and places `s0` (B's booking, made second) before `s1` (A's fresh request,
made first). MPSL refuses A.

Traced with sessions, that is the cycle exactly: A's fresh request blocked, B ok;
A ok on a booking placed first, B denied; A ok on a fresh request in `s0`, B
denied; then the first state again. Per cycle: 2 booking hits, 2 booking denials,
2 fresh requests of which 1 is blocked; `s1` refused twice, `s0` once. Every
counter above matches. Inferred, not instrumented: which session was live at each
step, and that the work queue runs after the grant ends (the only order that
reproduces the counts).

So ADR 0013 rule 1 depended on which session the established channel happened to
be in, which nothing in the core can see. And even with the order right, two
overlapping requests cannot both be granted: the later channel loses every slot,
as on master.

### The physical floor

With A's acknowledged reply unused, A's air is its window and a frame tail, about
0.8 ms; B's window can be served from about 1.3 ms after A, and below 2 ms on the
same RF channel and format the two merge into one window anyway. When A's reply
exchange is in use (the host queued data for A's master), A's air runs to
tA + guard + 4150 us, and a B window inside that span cannot be served: the radio
does one operation at a time. That is the floor; master has the same one.

## The fix

Two commits, each separately revertible.

- **40a2089, the gate places requests in the order the core made them** (oldest
  first). This alone restores the minimum bar: the earlier channel's request
  always reaches MPSL first. Gate ztest
  `test_d2_requests_reach_mpsl_in_the_order_they_were_made`, red before (the
  booking made second reached MPSL first).
- **c0ee88f, an R1 neighbour rides the earlier window's grant.** A committed
  operation that starts within `RADIANT_SCHED_JOIN_GAP_US` (2.5 ms) of the air an
  earlier one holds joins that one's request: a booking carries it as its tail
  (`book_tail_us`), a fresh arm as its follow-on. The joiner is armed into the held
  grant from the earlier completion at the booked lead, as a follow-on is, and
  makes no request of its own. The grant handoff already keeps a grant into which
  the completion programmed an operation. An operation is not booked while a
  neighbour close behind it is still in flight, so the neighbour's next slot is
  known when the booking is made (`RADIANT_SCHED_JOIN_DEFER_US`); without that
  deferral the ztest loses half the later channel's slots (mutation-checked). A
  joiner loses its cover when the earlier operation is denied, aborted outside a
  completion, or fails. The gate already places a refused tailed booking again at
  plain length, so a refused joined booking costs the joiner, never the earlier
  slot. Leaders are receive windows only; a transmit can join one but does not
  lead a join.
- 1090d52: the bench log prints the join counters (`join`, `jdefer`, bench images
  only).

No third session, no HAL change: `book_tail_us` and `follow_on_us` already exist.
The scheduler constants are not taken from the gate (that would be a caps field);
2.5 ms covers the gate's 1.65 ms with 0.85 ms over.

The fake radio gained a strict arbiter mode (`fake_radio_book_arbiter()`): every
fixed request, fresh or booked, holds its air plus the gate's margins, first come
first served. `test_d2_r1_pair_with_the_reply_reserve_keeps_both_slots` runs 2, 5,
8, 11, 12 and 15 ms with the 4150 us follow-on, with and without a scan. Before:
at 5 ms the later channel was denied 13 of 13. After: all twelve cases keep both
slots, 12 of 12 each.

## Verdicts

**Fixed.** On air the earlier channel no longer loses slots to the later one, and
both are kept at 5 ms on `gate.conf` and beside an attached Thread leader, where
the image before the fix lost a third and two thirds, interleaved in the same
sitting. Every after-fix R1 run read `den=0 blk=0`: every tracked slot was booked
or joined and granted.

### R1 walk, `gate.conf`

`ant_disc_phase.py --r1-walk`, as Stage B (A the Dongle #2002, B the Garmin as
#5005 walked to the target, the Pro Micro the reference on B, 60 s window).
Before-fix image: `img_fin_gate.hex` from Stage B (ff88a6a; no firmware change up
to 285f94a). After: `img_c3_gate.hex` (1090d52). Run in the order listed.

| Run | B-A in the window (ms) | DUT A lost | DUT B lost | Reference on B | Gate (whole session) | Log |
|---|---|---|---|---|---|---|
| **after, 5** | 5.6 -> 3.5 | **0.7 %** (242) | **4.0 %** (234) | 3.3 % | `den 0 blk 0`, hi 2231/1885/0/1884, join 415 | `r1_after_5` |
| before, 5 | 5.6 -> 4.8 | 36.0 % (156) | 68.0 % (78) | 2.5 % | `den 314 blk 314`, hi 2395/2081/470/1925 | `r1_before_5` |
| after, 8 | 8.6 -> 8.0 | 0.7 % | 9.7 % | 1.2 % | `den 0 blk 0`, hi 2851/2851/0/2850, join 0 | `r1_after_8` |
| before, 8 | 8.5 -> 8.2 | 0.3 % | 4.8 % | 2.0 % | `den 0 blk 0` | `r1_before_8` |
| after, 12 | 12.3 -> 10.9 | 1.1 % | 8.5 % | 0.8 % | `den 0 blk 0`, hi 2855/2855/0/2854 | `r1_after_12` |
| after, 20 | 20.5 -> 18.0 | 1.9 % | 11.4 % | 10.2 % | `den 0 blk 0`, hi 2855/2855/0/2853 | `r1_after_20` |
| master, 12 (Stage B) | 12.6 -> 11.0 | 0 % | 100 % | 0.0 % | n/a | `phase-3b\r1_ctl12` |
| branch, 5 (Stage B) | 5.7 -> 4.7 | 33.5 % | 70.1 % | 0.0 % | `den 238 blk 238` | `phase-3b\r1_head5` |

- At 5 ms the join carried B in A's grant (`join` 415 in the session, 346 tracked
  arms served from the held air, `hi` acquires minus pending), and both channels
  read at the reference's level.
- 8, 12 and 20 ms need no join (`join` 0) and the gate granted every tracked slot
  on both images. B's 5-11 % there is on air: the gate counters show no refusal,
  B's misses are single packets spread over the whole session including the walk
  at 30-50 ms, and Stage B read the same 8-10 % from this Garmin at 12 and 20 ms on
  every image. At 20 ms the reference itself read 10.2 %.
- `eo` 1 once (`r1_after_5`): one booking granted while the other session had an
  operation staged, the rare late-arm counter Stage B also saw in multi-channel
  runs. It cannot be told from on-air loss here.

### Discovery near a tracked channel, scan tail on (`fin_B` case)

`ant_disc_phase.py`, delays 190/195/200 ms (195 put B at 5.9 ms in Stage B's
`fin_B`), 60 s a trial, Pro Micro as B.

| Image | B-A (ms) | B found | Tracked channel A | B packets | Log |
|---|---|---|---|---|---|
| **after** | 4.7 | 8.7 s | **241/244** | 204 | `disc195_after` |
| before | 4.7 | 9.0 s | 176/244 (28 % lost) | 70 | `disc195_before` |
| after | 10.0 | 8.7 s | 244/244 | 209 | `disc195_after` |
| before | 9.8 | 8.7 s | 244/244 | 209 | `disc195_before` |
| after | -0.6 (sd 81) | not found | 243/244 | 0 | `disc195_after` |
| before | 0.3 | not found | 0 (see below) | 0 | `disc195_before` |

The after image's gate read `den 0 blk 2` over the three trials (the two refusals
are not attributed; inferred to be B's first booking at acquisition, before a join
could be made), the before image `den 139 blk 141`. A sensor within about 1 ms of a
tracked one was not found on either image: the tracked window holds the radio at
that instant with A's address only. **Unexplained:** the before image's trial at
0.3 ms counted no packets on channel 0 at all; not chased, it is the image being
replaced. A first pair at delay 0 (`disc5_*`) landed B at 56 ms, not 5 ms (the
delay-to-phase offset had moved since Stage B), and is not an R1 reading; both
images were clean there.

### Thread, attached leader, 5 ms

The 5340 DK reflashed as the OpenThread leader (`flash_p4_peer.ps1`, `leader`),
the DUT in its child table (RLOC16 `0x0c01`) before and after each run.
`thread.conf;phase_diag.conf;d2_stats.conf;health.conf`, after `img_c3_thread`
(9f5e594), before `img_fin_thread` (ff88a6a).

| Image | B-A (ms) | DUT A lost | DUT B lost | Reference on B | Gate | Log |
|---|---|---|---|---|---|---|
| **after** | 5.6 -> 1.3 | **0.3 %** (243) | **2.3 %** (238) | 0.8 % | `den 0 blk 0`, join 383, CONTENDED 16 ms | `r1th_after_5` |
| before | 5.6 -> 3.3 | 33.5 % (162) | 67.2 % (80) | 0.4 % | `den 277 blk 277` | `r1th_before_5` |

**An MPSL ASSERT 69:108 halted the after image at 348.8 s**, after the window and
the reading: `placed=6266 granted=6249 blocked=16 ext=0/201 skip=3969`, trace
`1:2 7:0 0:0` repeating and ending on TIMER0 returning END, `len=10000/10238`
(an elastic first bite with the extension skip active). This is the open assert
Stage A and B recorded beside Thread (Stage B's on `img_a2_thread`), on the elastic
path this change does not touch. One occurrence in one ~6 min Thread boot does not
say whether the rate changed; it needs the long Thread soak that 69:108 has
always needed.

### Nothing regressed: one tracked channel, acknowledged transfers

`gate.conf`, ch0 #2002 and ch1 searching for absent #9999, the Garmin on #2002 in
the same window, 180 s a reading (`rule1_3.ps1`), interleaved before/after.

| Reading | DUT ch0 heard | Garmin | Gate | `t0fix bs` | Log |
|---|---|---|---|---|---|
| after (first, alone) | 715/731 | 0.0 % | hi 731/731/0/731, `den 0 blk 0`, `sched_missed` 0 | 0 | `gate_after` |
| before 1 | 728 | 0.3 % | hi 731/731/0/731 | 0 | `gateab_before_1` |
| after 1 | **730** | 0.7 % | hi 731/731/0/730 | 0 | `gateab_after_1` |
| before 2 | 729 | 1.1 % | hi 730/730/0/729 | 0 | `gateab_before_2` |
| after 2 | **730** | 0.8 % | hi 731/731/0/731 | 0 | `gateab_after_2` |

The first after-fix reading heard 715 of 731 with every window granted and run
(`sched_missed` 0) and `join`/`jdefer` both 0, so nothing new had acted. The
interleaved pairs that followed read 730 and 730 against 728 and 729: the 715 was
the room.

`ant_ack_pair.py --seconds 90 --sends 60 --interval 1 --json`, both directions:

| Image | DUT slave | DUT master | Log |
|---|---|---|---|
| after (first, alone) | 59/60 (peer 60) | 58/60 (peer 59) | `ack_after` |
| before | 60/60 | 60/60 | `ackab_before` |
| after | **60/60** | **60/60** | `ackab_after` |
<!-- ACK2 -->

One tracked channel never joins anything, and every follow-on is armed inside its
own grant's reserve, which the join leaves alone (`!in_reserve`). The first
after-fix pair's 1 and 2 failures did not repeat interleaved.

### Tests and close-out

| Check | Result | Log |
|---|---|---|
| Gate ztest, placement order | red before, green after | Bluefin, this session |
| Sched ztest, R1 with the reply reserve | red before (5 ms: later channel denied 13/13), green after, 12 of 12 cases; deferral mutation-checked (removing it: later channel 6/12 denied) | Bluefin |
| Twister, Bluefin | **PASS**, 4332 of 4332 in 14 scenarios at 1090d52, counts OK; total floor raised 4327 -> 4332 | `twister.txt` |
<!-- CLOSEOUT -->

## Unexplained, and open

- The before image's discovery trial at 0.3 ms counted no packets on channel 0.
- Two booking refusals in `disc195_after` (`blk 2`, `den 0`) are not attributed.
- `eo` 1 in `r1_after_5`: a late arm, rare, cannot be scored against on-air loss.
- MPSL ASSERT 69:108 on the after Thread image, after the window (see above): open,
  as it was before this work.
- Not tested: three tracked operations chained inside the join gap; an R1 pair where
  the earlier channel is a master (a transmit does not lead a join; the placement
  order fix still keeps the earlier one); a joined booking refused by BLE on air
  (the re-place at plain length is the Stage B mechanism, ztested there).

## Raw logs

All in `C:\adr18_logs\phase-3c\`. Images: `img_c3_gate.hex` (1090d52,
`gate.conf;phase_diag.conf;d2_stats.conf;health.conf`), `img_c3_thread.hex`
(9f5e594, `thread.conf;phase_diag.conf;d2_stats.conf;health.conf`), each with
`img_*_config.txt` and `build_*.log`. Before-fix images are Stage B's
`img_fin_gate.hex` and `img_fin_thread.hex` (ff88a6a).

| Path | What |
|---|---|
| `r1_{after,before}_{5,8}`, `r1_after_{12,20}` | R1 walks: `r1w_summary.json`, `r1w_dut.antser`, `dut_log.txt`, `run.txt` |
| `disc195_{after,before}`, `disc5_{after,before}` | discovery near A (delays 190/195/200; and 0, which landed at 56 ms) |
| `r1th_{after,before}_5`, `ot_leader_flash.txt`, `ot_thread.txt` | Thread R1 and the leader |
| `gate_after`, `gateab_{before,after}_{1,2}` | single-channel rule-1 |
| `ack_after`, `ackab_*` | acknowledged-transfer pairs |
| `twister.txt`, `zero_cost.txt`, `size_*.txt`, `ztest_*_final.txt` | close-out |
| `chain*.out`, `b_c3_*.out`, `zc.out`, `ztests.out` | driver consoles; scripts in the session scratchpad (`phase3c_chain*.ps1`, `b4c.ps1`, `zc3c.ps1`, `sizes3c.ps1`, `ztests_3c.ps1`, `bf_iter3c.sh`) |
