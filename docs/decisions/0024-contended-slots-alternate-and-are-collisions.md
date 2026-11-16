# 0024 — Contended tracked slots alternate the loser, and a scheduler drop is a collision

- **Status:** accepted (owner decisions, 2026-11-15). Implemented and verified in
  ztest. **The root cause of the 2026-11-15 FE-C crossing loss is not
  identified**; see *What this does not settle*.
- **Date:** 2026-11-16
- **Builds on:** [0016](0016-merge-reach-is-the-arm-lead.md) (merge reach),
  [0013](0013-sweep-is-the-elastic-consumer.md) (a denial is not a miss)
- **Related:** [0018](0018-search-listens-through-the-arm-lead.md),
  [0021](0021-tracked-slots-are-booked-a-period-ahead.md) (the booking path, which
  this record does not change), [0023](0023-radiant-diag-message.md) (page 11)

> **Provenance.** Clean-room. Written from this project's own source, its
> captures and its tests. Nothing here derives from sdk-ant, from `libant.a`,
> from disassembly of any binary, or from any ANT+ device profile document. See
> [0002](0002-clean-room-policy.md).

---

## Context

In every 2026-11-15 Zwift ride on RadiANT (TI, Pro Micro, MPSL Dongle), FE-C
#52233 (period 8192) lost most of its frames each time power #20329 (period
8182) drifted through it, once every ~204 s (TODO.md, "From the 2026-11-15 Zwift
captures", item 1). `tools/decode_pcap.py --phase 3 2` on capture `215313` shows
FE-C missing **8 slots in a row** while power was 0.57–2.67 ms ahead of it; the
eighth miss is `RX_FAIL_GO_TO_SEARCH`, and FE-C carries Zwift's commands.

Two scheduler properties turn any unservable pair into that shape, whatever
makes the pair unservable:

1. **The loser is always the same channel.** `arm_next()` serves the earliest
   bounded receive. When two windows can neither merge nor both be armed in
   sequence, the later one loses every period until the geometry drifts apart.
   At 305 µs of drift per period, a 2.1 ms band is seven periods.
2. **A scheduler drop counts as a sensor miss.** `api_sched_done()` handled
   `RADIANT_SCHED_DONE_MISSED` (never armed, or expired behind another window)
   exactly like an armed window that heard nothing: miss count, `RX_FAIL`, guard
   growth, and after eight, search. Only `DENIED` was spared (ADR 0013).

## Decision

### 1. Alternate the loser

`radiant_sched.c` keeps `s.lost_mask`: bit *c* is set when channel *c*'s bounded
receive expires while a bounded operation holds the radio or has just ended (the
pass after its terminal), or when an open bounded receive is cut short for
someone else. It is cleared when the channel is next armed, or cancelled. It
lives outside `struct sched_slot` because `slot_clear()` wipes the slot when the
request is consumed.

In `arm_next()`, `turn_candidate()` lets the earliest bounded receive give way
to a later one that carries the bit, when the earliest does not, and when
serving the earliest first would leave the later one's expected sync (its window
centre) behind the earliest's close plus the arm lead. The flagged channel is
then the leader. No merge prediction is needed: if the two could share a window
after all, `arm_rx_window()` still merges the earlier one in.

`want_preempt()`'s "an unopened bounded receive gives way to one that starts
earlier" rule is skipped for the channel let go (`s.turn_yield`), or the choice
would flip straight back. Counted in `radiant_sched_stats.turn_yields`.

**Result:** neither channel loses two contended slots in a row, so
self-collision can never reach `RADIANT_CHANNEL_RX_FAIL_TO_SEARCH`.

#### Rule 4 stays one-sided, deliberately

ADR 0016's rule 4 caps the union at `RADIANT_SCHED_MERGE_SPAN_MAX_US` past the
*leader's* open. A member opening before the leader extends the union backwards
within the reach. Alternation makes that common: the later channel leads and the
earlier one merges in behind it. A symmetric cap was tried and rejected on the
evidence:

| test (12 periods) | one-sided cap | symmetric cap |
|---|---|---|
| r1 pair, MPSL lead, 5–24 ms apart | ch0 0 lost, ch1 6 | 6 and 6 |
| d2 three tracked, booking, 5 ms gap | 12 / 6 / 6 served | 6 / 6 / 6 |

The one-sided cap serves both channels in the periods the later one leads. The
only cost is a reply transmit at +2190 µs cutting a long window short, and that
transmit conflicts with the other channel's window either way.

### 2. A scheduler drop is a collision

In `api_sched_done()`, a tracked window that ends `MISSED` or `ABORTED` and
heard nothing takes the denial's accounting (`radiant_channel_on_slot_denied()`:
the clock advances and the guard grows, the miss run does not) and raises
`EVENT_CHANNEL_COLLISION` (0x09) instead of `RX_FAIL`, as Garmin's stick does.
Counted in `radiant_api_stats.slots_collided`. Only a window that was armed and
ran to its close empty (`DONE_OK`, not heard) counts toward search.

`ABORTED` is included because it too is the scheduler's doing: another operation
took the radio from an open window. Collisions share the denial path's bound
(`RADIANT_CHANNEL_DENY_TO_SEARCH`, 16), because the guard is finite either way.

### 3. Page 11

0xF7 page 11 carries the scheduler counters that never reached the host
(`windows_merged`, `preempted`, `replans`, `arm_rejected`, `tx_armed`,
`stale_events`) plus `slots_collided` and `turn_yields`. Against page 0's
`slots_missed`, it splits a lost tracked slot into "our scheduling" and "the
air". Version stays 1 (beta).

## What this does not settle

**The mechanism of the 0.57–2.67 ms band is still unknown.**
`test_two_masters_drifting_through_each_other` (radiant/tests/api) opens the two
channels exactly as Zwift does and drifts the masters through each other on the
nRF-direct and TI presets, with and without the 0x33 command, frame-terminal
latency (`fake_radio_set_rx_frame_delay_us()`), ±150 µs jitter and 20 % scattered
FE-C loss. It never loses more than one FE-C slot in a row, on master or after
this change. So the cause is either backend timing that `fake_radio` does not
model, or something about the real masters. The next step is the bench rig
(plan step 3: `apps/trainer` at 8192 plus `ant_sim.py --profile power` at 8182)
with the page 11 counters and `decode_pcap.py --phase`.

What this record guarantees regardless: whatever makes the pair unservable,
the cost now alternates and is reported as a collision, so it can no longer
send FE-C to search on its own.

**The booking path (MPSL, ADR 0021) is unchanged.** `book_slot()` and
`book_ahead()` still book earliest-first. Two tracked operations inside one lead
are carried by the R1 join (`test_d2_r1_both_slots_are_kept`); a third still
alternates through `arm_next()` but its bookings do not.

## Verification

- `test_two_unservable_slots_alternate_the_loser` (radiant/tests/src/test_sched.c):
  two slots 300 µs apart on different RF channels, twelve periods, misses
  strictly alternate, 6 each.
- `test_r1_two_tracked_channels_inside_the_lead_today`: rewritten; the later
  channel loses 5–7 of 12, never 10+, and the earlier none.
- `test_no_separation_between_two_tracked_slots_drops_a_slot` (the ADR 0016
  sweep) and `test_three_tracked_slots_inside_one_lead_still_drop_the_third`
  unchanged and passing.
- `test_a_scheduler_dropped_slot_is_a_collision_not_an_rx_fail` and
  `test_scheduler_collisions_never_send_a_channel_to_search` (radiant/tests/api).
- Full native_sim suite on the Bluefin server: 6260 cases in 16 scenarios, all
  passed.
- **Not yet on hardware.** Bench and Zwift acceptance are plan steps 3 and the
  verification list in TODO.md.
