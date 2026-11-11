# Phase 4: the review's F1 and F2, and corrections F3-F6

Branch `wip/adr18-f1` from `wip/adr18-impl` at 9c35fcb. Fixes two defects an
independent review found in Stage C ([phase-3c.md](phase-3c.md)) and corrects
four statements. Design: [ADR 0021](../decisions/0021-tracked-slots-are-booked-a-period-ahead.md).
Raw logs: `C:\adr18_logs\phase-4\`.

## The mechanism

Written down from the code, the reviewer's ztest and the Stage C logs before
anything was changed.

### F1: the earlier channel of a close R1 pair is never booked

Two tracked channels, A at tA and B at tA + d, no scan running (the steady state
of every Stage C R1 walk: `TEMPDIAG chunks ok=17` over the whole 355 s session of
`r1_after_8`, all in acquisition). One period, as the code runs it:

1. **A completes.** `pass()` arms B's current slot (booked, or joined to A's
   air). `book_ahead()` finds A's next slot first and asks `join_defer()`, which
   says wait: B starts within `RADIANT_SCHED_JOIN_DEFER_US` (12 ms) of now and
   B's next slot is not posted yet. `book_ahead()` then **returns**, so nothing
   else is booked in that pass either.
2. **B completes.** B's channel posts B's next slot from its callback. `pass()`
   then runs `arm_next()`, whose rule with no scan is "commit the radio early,
   even to something far away" (there is no timer of our own). The earliest
   committed operation is A's next slot, a period ahead, not booked, so it is
   armed as a **fresh request**. Only then does `book_ahead()` run, and
   `next_committed()` skips A because it is in flight.
3. Inside the join gap (d under about 6.6 ms) that fresh request is stretched to
   carry B (`follow_on = j - close`, up to `RADIANT_SCHED_JOIN_MAX_US`), and B is
   covered by a `ride` entry and never booked: 0 bookings. Outside it (8-12 ms) B
   is booked by that `book_ahead()`, A never: 13 of 25.

The comment on `join_defer()` ("the neighbour's completion books c instead") is
true only when a scan is running: then step 2 arms a scan chunk, A is not in
flight, and `book_ahead()` books A carrying B. The red ztest below shows exactly
that split: every scan case booked, every no-scan case under 14 ms not.

**What this costs, and what it does not.** With no scan, `arm_next()` arms A a
period ahead, and the gate places a fresh request at once as a NORMAL request
against the anchor (`gate_acquire()`, "placed like any ordinary future
reservation"). So the fresh request has about a period of notice, the same as a
booking. It is not a 24.4 ms-lead request, and on `gate.conf` both images read
`den 0 blk 0`. What does differ:

- **F2 (below):** the stretched fresh request is the one place a refusal costs
  the earlier slot.
- The core cannot tell A's air from a booking (`is_booked()` is false), so
  anything that keys on a booking (the scan's book gap, a scan tail) does not
  apply to A. With no scan there is nothing to use it.
- Whether a fresh NORMAL request a period ahead is refused more than a booking
  under BLE is not known; that is what the `ble_load.conf` R1 pair measures.

**Why the deferral exists, and why it stays.** Without it, with a scan running,
A is booked at its own completion, before B's next slot exists, so A's booking
cannot carry B; at B's completion B's own booking overlaps A's and is refused
(first come, first served): B loses about half its slots (Stage C's mutation
check). The deferral is right; what is wrong is that with no scan nothing books A
at the neighbour's completion, and the early `return`.

### F2: a refused joined fresh request costs the earlier slot

The gate places a refused request again at plain length only for `ROLE_BOOK`
(`radiant_radio_nrf_gate_mpsl.c`, BLOCKED handler). A stretched fresh request is
`ROLE_OP`: refused, it is denied, and the earlier window with it. ADR 0021's "a
refusal costs the joiner, never the earlier slot" holds only for bookings.

A second route to the same gap appears with the F1 fix below: a booking that the
core arms into before MPSL has answered becomes `ROLE_OP` too, so a refusal that
arrives after the arm would skip the re-place as well.

### The fix, as designed before building it

1. **Book at arm time.** When `arm_rx_leader()` is about to arm a bounded
   receive as a fresh request (not booked, not in a held reserve, at least the
   arm lead ahead), it books it first with `book_slot()` and arms it into that
   booking. `book_slot()` already joins the neighbour behind it. No deferral is
   needed here: an operation is armed only when it is the earliest committed one,
   so the neighbour behind it has completed and posted its next slot.
2. **No stretched fresh request.** The fresh join in `arm_rx_window()` (and the
   `ride` entry that recorded it) goes. A window that still has to be armed fresh
   (booking busy) asks for its own air only; a neighbour close behind books or
   asks for its own and, if refused, costs only itself.
3. **`book_ahead()` skips a deferred operation instead of returning.** Later
   operations are still booked, but not any that start within the deferred
   one's possible joined air (`JOIN_MAX` + `JOIN_GAP` after it), so a later
   booking can never take air the earlier one will ask for.
4. **The gate re-places a refused joined request at plain length whether it is
   still a booking or already armed** (`ROLE_OP`), keeping the staged operation
   and its backstop.

TX operations are left as they are (a transmit never leads a join, F4).

## What was built

Commits on `wip/adr18-f1` (9c35fcb..):

- `66fb0da` sched: `arm_rx_leader()` books a bounded window before arming it
  fresh (`book_slot()`, which joins the neighbour); the fresh join in
  `arm_rx_window()` and the `ride` entry are gone; `book_ahead()` skips a
  deferred operation, and anything starting within `JOIN_MAX + JOIN_GAP` after
  it, instead of returning.
- `19f3934` gate: the BLOCKED handler places a refused booking that carries
  `book_tail_us` again at plain length for `ROLE_OP` as well as `ROLE_BOOK`,
  keeping the staged operation and its backstop; a fresh request clears
  `book_tail_us`, so a stale value from an earlier booking in that session cannot
  shorten it.

No third session, no HAL change. TX is unchanged (F4). FLASH on the gate bench
image is 240 B smaller (111 612 -> 111 372), RAM 32 B smaller.

## ztests

Bluefin, native_sim. Logs: `ztest_red_before_9c35fcb.txt`,
`ztest_green_after.txt`, `ztest_mutation_no_defer.txt`, `twister.txt`.

- **`test_d2_r1_pair_is_booked_in_steady_state`** (strict arbiter, 4150 us
  follow-on, 12 periods; 2, 3, 5, 8, 10, 12, 14 and 20 ms, with and without a
  scan). Asserts no tracked loss on either channel and at most **2 fresh fixed
  arms in the whole run** (`R1_FRESH_MAX`, one acquisition arm per channel).
  Red on 9c35fcb: every no-scan case under 14 ms, 13 fresh arms (2-5 ms: 0
  bookings; 8-12 ms: 13 of 25). Green after: all 16 cases, 0 fresh arms.
- **`test_d2_a_refused_joined_request_keeps_the_earlier_slot`** (3 and 5 ms,
  with and without a scan; the fake refuses the next 4 requests longer than one
  window plus its reply reserve, re-placing a booking at plain length as the gate
  does). Red on 9c35fcb with no scan: A lost 4 (its stretched fresh request was
  refused whole). Green after: A lost 0 in all four cases; B lost 4 at 5 ms (one
  per refusal) and 0 at 3 ms (inferred: B then lies inside A's own reply
  reserve, which a refusal does not take).
- Gate: **`test_d2_a_refused_joined_booking_still_serves_the_armed_window`** and
  **`..._refused_twice_denies_the_armed_window`**, red on 9c35fcb (the armed
  window was denied at the first refusal); **`test_d2_a_refused_fresh_request_is_not_placed_again`**
  (the stale-tail guard).
- **The deferral is still load-bearing** (mutation: `join_defer()` returning
  false): the later channel loses 6 of 12 at 3 and 5 ms, now with and without a
  scan, and Stage C's own R1 test fails.
- Twister: **4343 of 4343** in 14 scenarios, counts OK; floor 4332 -> 4343.

The fake gained `fresh_fixed` (fixed arms neither booked nor riding held air),
`fake_radio_book_refuse_long()`, and, under the strict arbiter only, a too-near
DENIED (not ETIME) for a fixed arm at the in-grant lead that no held air covers,
as the gate answers it.

## Bench

nRF54L15 DK, `C:\adr18_logs\phase-4\`, Stage C's harness (`ant_disc_phase.py
--r1-walk`, A the Dongle #2002, B the Garmin #5005, reference the Pro Micro on
B, 60 s window), before and after interleaved in one sitting. Before:
`phase-3c\img_c3_gate.hex`, `phase-3c\img_c3_thread.hex`, and `img_c3_ble.hex`
built here from 9c35fcb. After: `img_f1_*.hex` (1362d1f). **Fresh** = `hi`
pending minus `D2BOOK hit`, as a share of `hi` acquires.

| Run | B-A in window (ms) | A lost | B lost | Ref on B | Gate | Fresh | Log |
|---|---|---|---|---|---|---|---|
| **after, 5** | 5.4 -> 5.0 | 0.0 % | 1.1 % | 0.4 % | den 0 blk 0, join 878 | **0 (0 %)** | `r1_after_5` |
| before, 5 | 5.6 -> 4.9 | 1.1 % | 1.9 % | 1.6 % | den 0 blk 0, join 886 | 974 (34 %) | `r1_before_5` |
| **after, 8** | 8.5 -> 1.8 | 0.7 % | 0.0 % | 0.0 % | den 0 blk 0, join 811 | **0 (0 %)** | `r1_after_8` |
| before, 8 | 8.6 -> 7.8 | 0.3 % | 0.3 % | 0.4 % | den 0 blk 0 | 940 (33 %) | `r1_before_8` |
| **after, 12** | 12.3 -> 11.9 | 1.1 % | 0.7 % | 0.4 % | den 0 blk 0 | **0 (0 %)** | `r1_after_12` |
| before, 12 | 12.6 -> 11.8 | 0.7 % | 0.3 % | 0.0 % | den 0 blk 0 | 703 (25 %) | `r1_before_12` |
| **BLE after, 5** | 5.5 -> 4.9 | 1.9 % | 2.3 % | 2.5 % | den 22 blk 22 | **1 (0 %)** | `r1ble_after_5` |
| BLE before, 5 | 5.6 -> 5.2 | 2.3 % | 2.7 % | 0.0 % | den 27 blk 27 | 969 (34 %) | `r1ble_before_5` |
| **BLE after, 10** | 10.6 -> 9.9 | 3.6 % | 1.9 % | 1.2 % | den 16 blk 16 | **0 (0 %)** | `r1ble_after_10` |
| BLE before, 10 | 10.9 -> 6.1 | 2.7 % | 1.1 % | 0.4 % | den 15 blk 15 | 1036 (36 %) | `r1ble_before_10` |
| **Thread after, 5** | 5.6 -> 4.8 | 1.1 % | 0.3 % | 0.4 % | den 0 blk 0, join 876 | **0 (0 %)** | `r1th_after_5` |
| Thread before, 5 | 5.6 -> 3.7 | 2.3 % | 1.1 % | 0.8 % | den 0 blk 0 | 916 (32 %) | `r1th_before_5` |

- **F1 is fixed on air:** every after run has 0 or 1 fresh tracked arm in the
  session; every before run has 25-36 %.
- **F1/F2's real cost was small, and nothing here shows the fix buying loss.**
  Every run on both images is within a couple of points of the reference, and the
  before image lost no slot to the arbiter on `gate.conf` or beside Thread. Under
  BLE both images had refusals (`den` 22/27 at 5 ms, 16/15 at 10 ms); A's loss
  after and before differs by under one point either way, inside run-to-run spread
  (one run each, so a difference that small is not resolved). A's 3.6 % in
  `r1ble_after_10` is one reading; before read 2.7 %.
- The `after, 8` walk drifted from 8.5 to 1.8 ms during the window (the Garmin's
  period), so it crossed into the join gap and the merge span; both channels held.
- The Thread leader was attached for both runs (RLOC16 `0x2001`). No MPSL ASSERT
  in any log of this sitting.
- The room was quiet: B read 0-2.7 % here, where Stage C read 8-10 % from the same
  Garmin. F5 stands as corrected.

### Regression checks

| Check | After | Before | Log |
|---|---|---|---|
| One tracked channel, `gate.conf`, 180 s (ch1 searching for absent #9999) | 722 heard (1.2 %), Garmin 0.4 %, `sched_missed` 0, `sched_denied` 2 | 716 (2.0 %), Garmin 0.1 %, 0, 3 | `gateab_after`, `gateab_before` |
| Ack pair 1 (slave / master) | 58/60, 59/60 (DUT ch0 loss 3.9 %, 1.7 %) | 60/60, 60/60 | `ackab_after`, `ackab_before` |
| Ack pair 2 | 60/60, 60/60 | 60/60, 59/60 | `ackab_after2`, `ackab_before2` |
| Ack pair 3 | 59/60, 60/60 | 60/60, 60/60 | `ackab_after3`, `ackab_before3` |

**Not resolved:** the acknowledged transfers read 356/360 after against 359/360
before. Three of the four after failures are in the first pair, which also read
3.9 % receive loss on the DUT's channel; the two pairs after it read 1 failure on
each image. Stage C's first after pair read the same (59/60, 58/60) and did not
repeat. No mechanism is known: an acknowledged reply is armed inside its own
grant's reserve, which this change does not touch (`!in_reserve`), and a single
tracked channel's next window was a fresh request a period ahead before and is a
booking a period ahead now. It needs a longer interleaved series to settle.

## Close-out

| Check | Result | Log |
|---|---|---|
| Twister, Bluefin | **PASS**, 4343 of 4343 in 14 scenarios, counts OK (at 1860ea1; later commits are docs only) | `twister.txt`, `twister_counts.txt` |
| Zero cost against c4e1ef8 (local, -j4) | **PASS**, all eight rows identical | `zero_cost.txt` |
| Gate suite, nRF5340 DK (COM6) | **PASS**, 4 suites, 0 failures, one boot; `radiant_gate_d2` 34/34 | `ztest_gate_5340.txt` |
| Main suite, nRF5340 DK | **PASS**, 50 suites, 0 failures, one boot; `radiant_sched` 79/79 | `ztest_main_5340.txt` |
| API suite | not run: the change is in `radiant_sched.c`, the gate and the test fake, which the main and gate suites cover | |
| Sizes (L15 bridge/Matter, nRF52840 Matter) | not run; bench images: gate 111 372 / 59 888 (before 111 612 / 59 920), Thread 299 884 / 158 320 (before 300 140 / 158 352), BLE 200 056 / 72 972 (before 200 296 / 73 004) | `build_f1_*.log`, `build_c3_ble.log` |

## Open

- The acknowledged-transfer difference above (356/360 against 359/360) is not
  resolved.
- F3 and F4 are documented, not fixed ([phase-3c.md](phase-3c.md), open list).
- The `book_ahead()` skip (later operations still booked past a deferred one) has
  no test of its own: with two channels it changes nothing observable, and the R1
  helper drives only two. A three-channel case is untested.
- The F2 path was exercised in ztests only. On air under BLE, `den` 16-27 a
  session were refusals of some booking; which channel, and whether any was a
  joined one re-placed at plain length, is not instrumented.
