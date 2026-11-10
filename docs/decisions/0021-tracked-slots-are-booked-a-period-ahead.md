# 0021 — On MPSL, tracked slots are booked a period ahead in a second session

- **Status:** Accepted as the design to build. Implementation is in progress on
  `wip/adr18-impl`. The scan tail (decision 5) is untested and conditional.
- **Date:** 2026-11-10
- **Resolves:** [0018](0018-search-listens-through-the-arm-lead.md)
- **Builds on:** [0013](0013-sweep-is-the-elastic-consumer.md) (tracked slots
  are inviolate), [0016](0016-merge-reach-is-the-arm-lead.md)
- **Evidence:** [evaluation](../adr18-eval/summary.md),
  [(d2) report](../adr18-eval/d2.md), [(a) report](../adr18-eval/a.md),
  [Phase 0c facts](../adr18-eval/phase-0c.md)

> **Provenance.** Clean-room. Written from this project's source, its own bench
> measurements and nrfxlib's public `mpsl_timeslot.h`. See
> [0002](0002-clean-room-policy.md).

---

## Context

Each fresh MPSL grant costs a 24.4 ms arm lead, so a search is deaf about 30 %
of the time, locked to the tracked channel's phase. Two tracked operations
2.6-24.4 ms apart also cannot both be placed, and the later one is lost every
period (R1).

Under Thread, MPSL refused every extension on every image measured, and granted
requests placed with a full lead. A tracked slot must therefore depend on a
request placed with notice, and never on an extension.

## Decision

1. **Book each committed operation about a period ahead, in a second MPSL
   session.** The gate keeps a pool of two sessions. The scan runs up to a small
   gap before the booked instant. A booking has no op id, callback or terminal,
   so the HAL's one-operation rule stands.
2. **Release a frame-bearing tracked window's grant early.** The receive path's
   `stop_on_first` branch retires the operation without owing a release, so the
   grant runs to its backstop. It goes through the existing grant handoff, which
   keeps the grant when the completion arms a follow-on.
3. **Stop asking for extensions the arbiter keeps refusing.** After a run of
   refusals the gate skips extensions for a while. MPSL ASSERT 69:108 has only
   ever followed extension refusals, and the skip stopped it in two runs. That
   is suggestive, not proof.
4. **A two-state stack policy replaces the doubling backoff.** OPEN or
   CONTENDED, keyed on the extension-refusal run, with the CONTENDED scan lead
   set from a measured sweep. The doubling backoff settled at 16-24 ms because
   of its own arithmetic: at 8 ms of notice or more, Thread refused only about
   2 % of requests.
5. **A scan tail inside the booked grant (untested).** Lengthen the booking's
   reserve by a dithered ~10 ms and arm the scan into it at the in-grant lead,
   closing the band after each tracked window on every stack. If longer bookings
   are refused more often, even after re-placing once at the plain length, a
   third session is built instead.

## Left out

- **(a) held grant**, in any form. Its gains need extensions, it failed rule 1
  under Thread, and its R1 cycling is unexplained.
- **(d1) `ACTION_REQUEST` chain.** Not disproven, but the scan tail closes the
  same boundary with no new request path.
- **A third session**, unless the scan tail fails.

## HAL contract

Additions only, all compiled out unless `RADIANT_BACKEND_NRF_GATE_MPSL` is set:
`caps.booking`, `caps.min_arm_lead_elastic_us`, `RADIANT_RX_ELASTIC` and
`radiant_radio_book()`. The TI backend needs no edit and is build-only here.

## Consequences

- One more MPSL session per app, and about +640 B RAM on Matter images before
  bench counters are removed.
- A booked slot depends on the core arming within a few milliseconds of the
  chunk ending, where master stages it 24 ms ahead. The build must bound this.
- Two sessions fix R1 pairs. Three tracked operations within one lead are
  not yet handled.

## Open before this is complete

The acceptance bench is in the Phase 3 plan. The questions that can still change
this design: whether the scan tail survives, whether 69:108 stays away over a
long soak, whether (d2)'s higher tracked loss under BLE is real, whether a live
slot is lost to a late arm, and whether Thread's own traffic suffers during a
search.
