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
