# 0021 — On MPSL, tracked slots are booked a period ahead in a second session

- **Status:** Accepted as the design to build. Decisions 1-4 are built on
  `wip/adr18-impl` ([Stage A report](../adr18-eval/phase-3a.md)). The scan tail
  (decision 5) is untested and conditional. Stage B
  ([report](../adr18-eval/phase-3b.md)) built decision 5 and keeps it, and found R1
  pairs under ~6 ms still lost, now on both channels. Stage C
  ([report](../adr18-eval/phase-3c.md)) fixed that: both slots kept at 5 ms.
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

> **Corrections, 2026-11-10, from the Stage A bench
> ([phase-3a.md](../adr18-eval/phase-3a.md)).** The text below is left as first
> written, so read it with these:
>
> - **Decision 4's figure is wrong.** Thread did not refuse "only about 2 %" at
>   8 ms or more, and the 16-24 ms lead was not an artefact of the backoff. With
>   the lead held fixed, under an attached peer and detached alike, Thread refused
>   99-100 % at 4 and 6 ms, 84-89 % at 8 ms, 75-80 % at 12 ms, and none at 16 or
>   24 ms. The "2.1 %" was the `>= 8 ms` notice bin of a run whose lead was mostly
>   16-24 ms, so nearly everything in that bin had enough notice. The backoff had
>   landed about where the knee is. CONTENDED is 16 ms.
> - **Decision 3's evidence is weaker than stated, and one sentence is false.**
>   69:108 has not "only ever followed extension refusals": `gate_disc_master` on
>   Phase 1's master image asserted with `ext=87/0` and no other stack. On the
>   two-session image at HEAD the arm with the skip and the arm without it both
>   ran 30 minutes with no assert, the second through 59 495 refusals. The skip is
>   therefore not shown to prevent the assert. All six asserts on record came from
>   images without decision 2's early release, and none has come from an image
>   with it, which is suggestive and also unproven. The skip stays because it
>   costs nothing measurable, not because it is known to be the cure.
> - **Decision 2 gives the air back to the other stack, and not to the scan.** The
>   evaluation's ~5.5 ms after-slot band is not achieved. Opening the chunk at the
>   window's end (e7749c2) lost tracked slots on `gate.conf` and was reverted
>   (6128da5), so the chunk still waits out the follow-on reserve.
> - **Left out, (d1).** The sweep supports dropping it on the original ground:
>   requests at 8 ms of notice or less are mostly refused beside Thread. It still
>   was not measured as `ACTION_REQUEST`.
> - **Consequences.** RAM is +448 B on Matter and bridge images at this branch's
>   head, with the bench counters out.
> - **Listening under Thread is about 26 % of the air** at the 16 ms lead, measured
>   over 30 minutes. Every grant ends at its first compare, so a grant is at most
>   one 10 ms first bite.

> **Corrections, 2026-11-11, from the Stage B bench
> ([phase-3b.md](../adr18-eval/phase-3b.md)).**
>
> - **"Two sessions fix R1 pairs" holds from about 12 ms to the lead, not below about
>   6 ms.** Each booking holds its window plus the 4150 us reply reserve and margins,
>   about 6.5 ms, and MPSL refuses whichever of two overlapping bookings was placed
>   second. At 5 ms both channels lost slots (33.5 % and 70 %), **including the one
>   master keeps**. Once the scan tail lets discovery find a sensor that close to a
>   tracked one, the tracked one loses about a quarter of its slots. Refusals were also
>   seen near 11 ms, unexplained. Not fixed: the earlier booking taking in the later
>   window is one route.
> - **Decision 5 is built and kept**: 10 ms, dithered, with a refused tailed booking
>   placed once more at plain length (BLE refused longer bookings more). It gave 17/17
>   on the 17-offset rig and raised listening beside Thread from about 27 % to 35 %. No
>   third session was needed. The tail is mostly not asked for when two or more
>   channels are tracked, so its gain is a single-channel one for now.
> - **69:108 occurred on an image with decision 2's early release** (Stage A's final
>   Thread image, eight channels configured, seven searching). Its trace ended on a
>   TIMER0 asking for an extension, not on an EXTEND_FAILED. The early release is not
>   shown to prevent it.
> - **Late arms:** `eo` 0 on the Matter image in 300 s, but `eo` 1 twice in
>   multi-channel runs. A live slot lost to a late arm is still possible and rare.
> - **Thread's own traffic** lost no pings and never re-attached. The round trip
>   during a search is a few ms longer than master's.
> - **Eight tracked channels** were not measured on air. In a ztest at the 16 ms lead
>   the scan is 69 % deaf.

> **Corrections, 2026-11-11, from Stage C
> ([phase-3c.md](../adr18-eval/phase-3c.md)).** These replace the first Stage B
> bullet above.
>
> - **R1 below ~6.6 ms had two causes, and both are fixed.** Two tracked requests
>   overlap at the arbiter when B - A is under about 6.2-6.6 ms (window, the 4150 us
>   reply reserve, the gate's margins, MPSL's guard), so one of the two was refused.
>   The earlier channel lost too because the gate placed waiting requests in session
>   index order rather than the order the core made them, which put the earlier
>   channel's fresh request behind the later channel's booking every third period.
>   The "refusals near 11 ms" were a misread phase: by the DUT's own timestamps they
>   began at 5-6 ms.
> - **The fix:** requests reach MPSL oldest first; and a committed operation that
>   starts within 2.5 ms of the air an earlier one holds joins that one's request
>   (booking tail or follow-on) and is armed into its grant from the earlier
>   completion. No third session and no HAL change.
> - **On air at 5 ms:** A 0.7 %, B 4.0 % on `gate.conf` (before, interleaved: 36 % and
>   68 %), and 0.3 % / 2.3 % beside an attached Thread leader (before: 33.5 % /
>   67 %). The `fin_B` discovery case keeps the tracked channel (241/244 against
>   176/244).
> - **Physical floor:** a later window inside the earlier channel's acknowledged
>   reply exchange (when one is in use) cannot be served, and a sensor within about
>   1 ms of a tracked one is not found by the search. A refused joined booking is
>   placed again at plain length, so under another stack's refusal it costs the
>   joiner, never the earlier slot.

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
- Two sessions fix R1 pairs from about 6.6 ms to the lead; below that the later
  operation rides the earlier one's grant (Stage C), so both are kept down to the
  physical floor. Three tracked operations within one lead are still not handled:
  the join can chain (code), but that is not tested, and spread wider than the join
  gap inside one lead a third has nowhere to book (ztest characterization).

## Open before this is complete

The acceptance bench is in the Phase 3 plan. The questions that can still change
this design: whether the scan tail survives, whether 69:108 stays away over a
long soak, whether (d2)'s higher tracked loss under BLE is real, whether a live
slot is lost to a late arm, and whether Thread's own traffic suffers during a
search.
