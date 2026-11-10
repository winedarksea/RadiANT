# 0018 — Under MPSL, a search must listen through the arm lead, or it is deaf for three of them every tracked period

- **Status:** proposed. The measurement is done; the fix is not designed in
  code and nothing here has been built. It becomes its own plan.
- **Date:** 2026-11-05. **Revised 2026-11-07:** the candidate (a) arithmetic
  was wrong (it assumed one grant could span a whole period; the gate caps a
  grant at MPSL's 100 ms); the self-inflicted/inherent split, the 100 ms
  consequence, the bridging-extension requirement, candidate (d) and the
  corrected pass criteria were added. The measurement sections are unchanged.
- **Builds on:** [0013](0013-sweep-is-the-elastic-consumer.md) (the sweep is the
  elastic consumer; tracked slots are inviolate),
  [0016](0016-merge-reach-is-the-arm-lead.md) (`min_arm_lead_us` is an
  exclusion radius, not slack)
- **Related:** `review/2026-10-31-timing` (R1, C5-1, A14-1; unmerged and
  unverified), review 2026-11-05 item 1, `docs/p4-coexistence-bugs.md` bug 26
  (where the 24 ms came from)

> **Provenance.** Clean-room. Written from this project's source, its own bench
> measurements, nrfxlib's public `mpsl_timeslot.h` and earlier decision
> records. Nothing here derives from sdk-ant, `libant.a` or any ANT+ profile
> document. See [0002](0002-clean-room-policy.md).

---

## Context

On an MPSL build, a wildcard search cannot discover a second master while
another channel tracks a first master, for as long as the second master's
packets fall at certain phases of the tracked channel's period. It was first
seen on the nRF54L15 bridge (`apps/dongle_thread`, self channels) and then
narrowed. It does not occur on a non-MPSL image, it does not need Thread
(`gate.conf` reproduces it), and it does not occur when nothing is tracked.

Last session's temporary counters showed the sweep was not starved. About 52 %
of wall time was credited and sets advanced about 2/s. Instead it was
**phase-blind**: binning every credited chunk by its phase against the tracked
slot gave the same mask every 5 s, with ~26 % of the period never listened to.
Two explanations were left open. One was "never heard": the master sits in the
unlistened band. The other was "heard but not acquired": the counters showed
`frames_ok > 0` across 5/5 failures, including a run whose band had been
narrowed to ~16 %, and that seemed unlikely under random phases. This record
settles which one it is.

## The measurement

**Rig** (no Feather). Master A was an nRF52840 dongle on `apps/dongle`, HR
#2002. Master B was the nRF5340 DK net core on `apps/dongle` with the
`promisc/appcore` companion, HR #5005; it transmits up to 8 dB weaker, which
matters for loss but not for found or not found. Both masters ran at 8070
counts on RF 57. The bridge under test was the nRF54L15 DK on
`apps/dongle_thread` `gate.conf` (MPSL, no Thread), with `.config` read back
for `RADIANT_BACKEND_NRF=y` and `GATE_MPSL=y`. The host channels went over the
ANT VCOM: ch0 pinned to #2002, ch1 wildcard HR, 60 s per trial.

**Controlling and measuring B's phase.** One host process drove both masters
and timestamped every `EVENT_TX` of each on one clock. B was reopened a chosen
delay after one of A's `EVENT_TX`, stepped across 0–240 ms in shuffled order.
A temporary per-frame log on the bridge recorded every CRC-valid frame the
search heard: device number, phase against ch0's slot, and which channel
claimed it. Wherever the bridge heard B, its own phase calibrates the host's:
**bridge phase = host phase − 25.2 ms**, with a spread of 25.0–25.3 over 12
points. For failures, where B is never heard, the phase is that calibration
applied to the host measurement.

**Chunk mask while ch0 tracks:** `0x003fcffc` in 84 five-second reports.
10 ms bins 2–11 and 14–21 are listened to. Bins **0–1, 12–13 and 22–24**
never are. 18 other non-zero masks occur, about one per trial. They fall in
the reports straddling a trial boundary, where the host has just re-opened the
channels and ch0 is not yet tracking (an inference from their timing; the
per-report channel state was not logged).

| B's phase after the tracked slot (ms) | how known | ch1 outcome |
|---|---|---|
| 2.3 | host − 25.2 | **not found in 60 s** |
| 9.2 | host − 25.2 | **not found** |
| 23.5 | host − 25.2 | **not found** |
| 38.9 | bridge | found, 11.3 s |
| 54.0 | bridge | found, 11.3 s |
| 69.3 | bridge | found, 11.3 s |
| 84.1 | bridge | found, 11.3 s |
| 99.4 | bridge | found, 11.6 s |
| 114.1 | bridge | found, 11.3 s |
| 128.6 | host − 25.2 | **not found** |
| 144.2 | bridge | found, 8.8 s |
| 159.3 | bridge | found, 8.8 s |
| 174.4 | bridge | found, 8.8 s |
| 189.0 | bridge | found, 8.8 s |
| 204.0 | bridge | claimed by ch1 (host capture lost to a COM port fault at 5 s) |
| 211.2 | bridge | found, 11.3 s |
| 225.4 | host − 25.2 | **not found** |

**5 of 17 failed, and every failure is inside the never-listened bins; all 12
successes are outside them.** The predicted deaf fraction is ≈28 %; the
observed one is 29 %.

**Control:** the same five failing offsets, with B's phase set against A, then
A taken off air and ch0 pinned to an absent #9999, so nothing is tracked.
**B was found 5/5, in 7.3–7.4 s.**

**"Heard but not acquired" is refuted.** In every failing trial the bridge
heard **zero** frames from #5005. In every success, B's first CRC-valid frame
was claimed by ch1 at once. The `frames_ok` that kept rising through the
failures were **121 frames from an ambient FE-C trainer (#52233)** that no
channel was searching for (type 0x11 against a wildcard-HR channel). They
count as CRC-valid and are irrelevant.

The same failure showed on the bridge image (`thread.conf;bridge.conf`, self
channels), though without a phase measurement. With A tracked, B was not
bound across two pairing windows. One second into the first window after A
went absent, B bound.

## What it is: one arm lead before every fresh grant request

There are three bands, and all three are the same quantity: `min_arm_lead_us`,
24.4 ms on this build (`RADIANT_GATE_MPSL_PLACE_LEAD_US` 24000 +
`HEAD_MARGIN_US` 250 + ARM_LEAD 168). Without a grant held, no operation may
arm nearer than that, and **every scan chunk under MPSL starts with no grant
held**.

- **After the tracked slot, ≈0–25 ms.** The tracked window's done is the
  pump's `now`, and `pass_step()`'s `earliest = now + arm_lead()` forbids the
  first chunk for another 24.4 ms.
- **Mid-period, ≈119–144 ms.** Chunk 1 runs from ≈25 ms to the
  `MAX_WINDOW_US` cap (100 ms − 5.8 ms of grant overhead = 94.2 ms), so it
  ends at ≈119 ms. Chunk 2 is a fresh request and pays another 24.4 ms, so it
  starts at ≈143.6 ms. The success at 144.2 ms sits 0.6 ms after that
  predicted start; the one at 114.1 ms sits 5 ms before chunk 1's predicted
  end.
- **Before the tracked slot, ≈221–246 ms.** `arm_next()` stops a background
  scan `lead` before the next committed window, so the tracked request can
  still be placed.

3 × 24.4 ms plus the tracked window is ≈30 % of a 246.3 ms period. Without
MPSL the lead is 168 µs and the same three gaps total under 1 %, which is why
the non-MPSL image finds B in one sweep.

**Credit is the second half of the defect.** `radiant_search_on_done()`
credits time, not phase. Each set therefore advances after its full 260 ms of
dwell while missing the same 30 % of phases on every visit, and "certain within
one sweep" is false without any counter moving. That is the over-credit
ADR 0013 warned of, reached through phase rather than through a shortened
grant.

**What it is not.** It is not Thread. It is not the tracking channel's own
behaviour: ch0 tracked throughout, at 240–244 packets a minute against 243.6
expected. It is
not C5-1: that branch fixed which class a BLOCKED answer backs off, and it
does not touch the lead. And it is not the mid-period stagger tried last
session: rotating chunk 1's ceiling moves one of the three gaps around (which
is why it covered every bin over 5 s) and leaves the two fixed at the tracked
slot. Last session's failing master sat in one of those two.

**A derived prediction, not measured:** every additional tracked channel at a
distinct phase adds two more arm leads per period. At four tracked channels
spaced evenly, each 61.6 ms gap keeps ≈12.8 ms of listening. Discovery would
then be blind to ~80 % of phases. The bridge's two self channels, and Zwift's
four, are exactly that case.

## Why this is not the shared-radio tradeoff

Reviewers have asked whether this is just what sharing a radio costs. It is
not, and the record should say so plainly, because the two costs scale with
different things.

**The inherent cost** is that when BLE or 802.15.4 actually owns the air,
RadiANT cannot listen. It scales with the other stack's duty cycle and no
design removes it.

**The measured deafness is not that.** `gate.conf` runs MPSL with no other
stack. Bug 26's sweep is the reference: at a 24 ms placement lead MPSL
refused nothing (`blocked` 0 across 8563 grants). During all three bands the
radio is off and the air is free. The band is RadiANT's own choreography: it
hands the grant back, and then asks for a new one with 24 ms of notice.

The 24 ms is **notice, not occupancy.** It is `PLACE_LEAD_US`, a Kconfig value
swept until the *other* stack's scheduler stopped refusing requests (bug 26).
It is paid once per request MPSL receives, and it is coupled to listening
only through one API rule: `mpsl_timeslot_request()` answers `-NRF_EAGAIN`
unless the session is IDLE (`mpsl_timeslot.h`; the gate's rule 1 at the top
of `radiant_radio_nrf_gate_mpsl.c`). A request cannot be queued behind a live
timeslot in its own session. So every boundary between two grants is a full
lead with the radio idle, and the deaf fraction is

> (notice) × (grant boundaries per period), with the boundaries phase-locked
> to the tracked slot because the tracked window is its own grant.

That is why it scales with the number of tracked channels (two boundaries
each) and not with the other stack's load. The inherent cost scales the other
way round. A design that leaves only the inherent cost is the target.

## The 100 ms ceiling, which the first draft missed

MPSL's `MPSL_TIMESLOT_LENGTH_MAX_US` is 100 ms, and the gate enforces it on
the *extended* total: the extension chain stops at
`g.granted_len_us + step <= MPSL_TIMESLOT_LENGTH_MAX_US`
(`radiant_radio_nrf_gate_mpsl.c`, the `SIGNAL_TIMER0` case), and
`caps.max_window_us` is 94.2 ms for the same reason. nrfxlib's header does not
say whether MPSL itself accepts an extension past 100 ms total; the gate does
not try. A 246.3 ms period therefore needs **at least three grants**, so at
least three boundaries per period cyclically, and with one session each
boundary is a full lead. The first draft's claim that holding one grant
would close all three bands to "the tracked window plus 2 × 168 µs" assumed a
grant spanning the whole period. It cannot.

What a single held grant *can* do is stop the tracked window from being a
grant of its own. Today the tracked window is one grant and the two chunks
are two more: three boundaries, all locked to the tracked phase. If the
tracked window sits inside a scan grant, the scan chain free-runs at one
grant plus one lead per cycle (94.2 + 24.4 ≈ 118.6 ms), which is about 2.08
boundaries per period, and, because 118.6 does not divide 246.3, the gaps
**drift ≈9 ms per period against the tracked slot** instead of sitting on it.
A master at a fixed phase is then deaf on some visits rather than on every
visit.

So the honest arithmetic for candidate (a) below is: about 21 % of each
period unlistened instead of 30 %, no longer phase-locked, and independent of
how many channels are tracked for as long as a 24.4 ms gap fits between
neighbouring tracked instants (it does at four). Closing the remaining
≈2 leads per period needs the next request to reach MPSL *before* the current
grant ends, which is candidate (d), and that is a core change this ADR scopes
but does not take.

## Candidates, judged against the measurement

**(a) Hold the elastic grant through the tracked boundary, and arm in-grant.**
A scan chunk is already the elastic class: it is granted short and extended,
and the gate handles `SIGNAL_EXTEND`. A grant still held when the next
operation is due can serve it at `min_arm_lead_in_grant_us` (168 µs) instead
of 24.4 ms: `gate_acquire()` already serves a request that fits inside
`g.grant_end`, and the acknowledged-data reply path already relies on it
(`radiant_burst.c`, the `min_arm_lead_in_grant_us` check). If the scan grant
is extended to cover the tracked window, and the scheduler plans that window
and the chunk's continuation against the in-grant lead, the two bands at the
tracked slot shrink to the tracked window plus two in-grant handoffs. The
mid-period boundary stays, as above, but it stops being pinned.

Three things the first draft left implicit, which the plan has to build:

1. **A bridging extension, not the chain.** The chain grows just in time, one
   `EXTEND_STEP_US` (1 ms) at the compare before the current end, so at
   T − `min_arm_lead_us` the grant's end is only ~2 ms ahead and nothing is
   known about T. The gate must arm a compare at T − lead − margin and ask
   there for one extension of (lead + window + tail) in a single step. The API
   allows any length ≥ 200 µs; the 1 ms step is this file's own drift policy
   and a 30 ms step costs 15 µs of drift at the 500 ppm it already budgets.
   The answer arrives within `MPSL_TIMESLOT_EXTENSION_PROCESSING_TIME_MAX_US`
   (25 µs), so the decision is made at the compare.
2. **The handoff is an operation boundary, not just a grant boundary.** The
   HAL allows one operation in flight. The scan chunk must *end as an
   operation* at T − handoff, the tracked window be armed in-grant, and the
   scan continuation be armed in-grant after it. Each handoff goes through
   `deliver_terminal()` and a scheduler pass on the gate's work queue, so
   budget a handoff of 1–2 ms, not 168 µs, until it is measured.
3. **The scheduler places the boundaries.** With the tracked window inside a
   grant, `arm_next()` no longer stops the scan `lead` early. The chain will
   sometimes land a boundary across T on its own; rather than fall back in
   those periods, the scheduler should shorten the preceding chunk so that
   the next grant starts inside (T − 94.2 ms + window, T − lead − margin).
   It knows T a period ahead. The same freedom lets it rotate the mid-period
   gap deliberately instead of relying on drift.

The risk is ADR 0013 rule 1. The tracked slot would now depend on an
extension MPSL may refuse when the other stack has the air scheduled. **The
rule that keeps it safe:** decide at T − `min_arm_lead_us`, the last instant a
fresh request for the tracked window can still be placed. If the bridging
extension has succeeded by then, keep listening to T − handoff and arm the
tracked window inside the grant. Otherwise end the chunk there and place the
tracked request exactly as today: the session goes IDLE, the staged request is
placed, and the notice MPSL gets is the lead minus the hand-back. A refused
extension then costs listening, never a tracked slot. Under real 802.15.4
traffic some periods will fall back, and the band then reappears in those
periods only, which is no worse than today.

**(b) Let the 3-byte search window cover the tracked slot and route the
tracked master's frame to its channel by channel ID.** This works only when
the tracked channel shares the search's RF index and network. It costs a
search filter per tracked device, out of 8 per window. It turns every tracked
window into a merge across address formats, which ADR 0016's rule 1
deliberately forbids. Its contention exposure is the same as (a)'s, because
the tracked slot again lives inside the search's grant, and it is far more
invasive in the channel layer, which would have to accept timing sync from a
search frame. It solves the same two bands as (a) at a much higher cost, and
nothing about the mid-period one. **Rejected.**

**(c) Yield the tracked slot to the search one period in K, only while a
search is active.** This is the fallback, and the measurement shows how poor
it is. It opens the ±25 ms band once every K periods, so a master sitting
there is heard only when its set's dwell lands on a yielded period. That is
roughly 1 in K visits: about K sweeps, or minutes at K = 8, against one sweep
today outside the band. It costs a deliberate 1/K of the tracked channel's
packets for as long as anything searches, and the bridge's pinned channels
search whenever a paired sensor is absent. That breaks ADR 0013 rule 1 by
design. It does nothing for the mid-period band. **Kept only as a last
resort** if (a) proves unbuildable on MPSL.

**(d) Pipeline the next request so the notice runs while the current grant
is live.** This is the only way past the ≈2 leads per period the 100 ms
ceiling leaves, and it is what would take the self-inflicted cost from about
a fifth of the period to the grant latency per boundary (≈1.7–2.8 ms
measured, so ≈2–3 % per period). The API offers two routes:

- **`SIGNAL_ACTION_REQUEST` returned from the scan grant's end.** MPSL closes
  the timeslot and takes the next request in the same return, with
  `distance_us` from this grant's start. The gate's split rejected this
  action because it closes the timeslot; at a chunk boundary that is exactly
  what is wanted. MPSL receives the request with no notice beyond the
  distance, so whether it is granted at short notice with the other stack
  idle, and how often it is BLOCKED under 802.15.4 continuous receive and
  under a BLE advertiser, is a measurement, not a design question.
- **A second timeslot session** (`mpsl_timeslot_session_count_set()`,
  `CONFIG_MPSL_TIMESLOT_SESSION_COUNT=2`). A request in the idle session is
  placed with the full lead while the other session's grant is live. It keeps
  the 24 ms notice the other stack was swept to need, and MPSL arbitrates the
  two sessions by priority, so a tracked window in the second session never
  depends on an extension at all: the scan's extension simply fails where it
  would overlap, which is ADR 0013's yield point. The gate would carry two
  anchors, since `distance_us` is per session.

Both routes need the next operation to be *known* before the current one
ends: a two-deep arm in the HAL and scheduler, which breaks the one-operation
rule that the gate's rule 1 mirrors 1:1, or a gate that holds a speculative
elastic continuation and tells the core the air is held. Either is a larger
change than (a) and is not taken here. **(d) is the follow-on decision**, to
be opened after (a) is measured, and its first step is the `mpsl_arb` spike
answering the short-notice question above on `gate.conf`, under Thread, and
under BLE, before any core change.

## Phase-aware dwell credit, and its stall escape

With (a) working, each visit still leaves about a fifth of the period
unlistened, so time credit is not honest per visit. What changes is that the
unlistened fifth moves between visits, so a set's coverage accumulates across
sweeps instead of repeating. The expected cost to a master at a fixed phase is
then about 1.3 sweeps rather than never, and it is bounded by how many periods
the drift (or the scheduler's deliberate rotation) takes to cover the band:
three to four at 9 ms per period.

Credit is still owed for honesty in the periods where (a) falls back, which
are exactly the periods of real coexistence pressure. Credit alone stalls the
sweep, as last session found: a set whose phases cannot all be covered never
advances, and the denial escape fires only on denials, not on uncredited
listening. If it is added, it needs its own escape. Advance after a wall-clock
cap of about 3 × dwell regardless of coverage, and count that as
`advanced_uncovered`. The cheaper first step is that counter alone. Record per
set what fraction of the period it listened to before advancing, and decide
on credit once the A/B under Thread shows how often (a) falls back. If (d) is
built later the question mostly goes away, since the unlistened fraction
falls to the grant latency.

## What (a) does not promise

- **R1 of the 2026-10-31 review** (the tracked channel's own loss under a
  live BLE stack, 9.5 % measured against 8.7 % predicted from the exclusion
  radius) is the inherent kind of cost. (a) is not aimed at it. The lever
  there, if any, is whether `PLACE_LEAD_US` is larger than the BLE stack
  needs, which is a Kconfig sweep per other-stack, not a design change.
- **Under real 802.15.4 or BLE load** the bridging extension will fail in
  some periods and those periods look like today. The A/B under Thread
  (step 4 below) measures that fraction; it is the number that says how much
  of the remaining deafness is inherent.
- **One sweep.** With one session the mid-period boundary remains, so "found
  in one sweep for every phase" is not the pass criterion for (a). It is the
  criterion for (d).

## Recommendation

**(a)**, with the decide-at-the-placement-deadline rule, the bridging
extension, the scheduler-placed boundaries, the coverage counter, and no
credit change in the first iteration. Then open (d) as its own record once
(a)'s fallback rate under Thread is known.

**The A/B/A that decides it** uses the same rig and harness (a host process
driving both masters and stepping B's start offset; the bridge's per-frame
log for calibration). The harness `disc_phase.py` lived in a session
scratchpad and may be gone; if so it is rebuilt into `tools/` first.

1. **A** (master): the 17-offset sweep plus the 5-offset control, exactly as
   above. Expect 12/17 and 5/5, and mask `0x003fcffc` in every report.
2. **B** (fix): the same. Pass means **17/17 found**, with a median
   time-to-found within about one sweep of the control's and no offset
   beyond about four sweeps. The chunk mask is **no longer constant** across
   the five-second reports (a mask that moves is the signature of the fix;
   a mask that is constant and merely wider means the gaps are still
   locked), and over any 1 s window every 10 ms bin except the tracked
   window's is listened to at least once. ch0's loss and `sched_missed` stay
   within the room's floor: a two-receiver concurrent reading, since the
   room has been at 5–8 %. The gate's bridging-extension failed and fallback
   counts are recorded and should be zero on `gate.conf`, where nothing else
   is on the air: a non-zero count there is a defect in the fix, not
   contention.
3. **A'** (master again): back to 12/17. Without this leg a room that changed
   between A and B reads as a fix.
4. **Then under Thread**, with an attached 802.15.4 peer (the 5340 as OT
   leader through `scripts/flash_p4_peer.ps1`, so B needs another board).
   Count how often (a) falls back. That number decides whether phase-aware
   credit is needed, and it is the baseline (d) is judged against.

Deterministic coverage comes first, before any of that: a `test_sched` case on
the fake radio with `min_arm_lead_in_grant_us` and the held-reserve model
(`fake_radio.c` already has both), tracking one channel and searching on
another over several periods. It asserts that the tracked window is armed
inside the scan grant when the bridging extension succeeds; that a refused
extension at T − lead still arms the tracked window on time; that no scan
grant exceeds `max_window_us`; and that the union of listened phases over the
run covers the period except the tracked window, with the unlistened band of
any single period not at the same phase as the previous period's.

## Phase 0c facts (2026-11-10)

Measured with `radiant/spike/mpsl_arb` (commands `a`, `b`, `c`, code at
`b1e76ad`) on the nRF54L15 DK, NCS v3.4.0. No radio is driven; the second
stack is the spike's BLE advertiser (1.0-1.2 s). No Thread. Full report and
logs: `docs/adr18-eval/phase-0c.md`, `phase-0c-raw-logs.md`. No MPSL assert,
OVERSTAYED or INVALID_RETURN in any run.

**F1: one timeslot extended past 100 ms. PASS.**

| Run (20 grants each) | No stack: reached 300 ms | BLE run 1 | BLE run 2 |
|---|---|---|---|
| 1 ms just-in-time chain | 20/20 | 15/20 | 15/20 |
| plus 30 ms step at 40 ms | 20/20 (step 20/20) | 15/20 (step 20/20) | 11/15 visible |
| plus 30 ms step 85 -> 115 ms | 20/20 (step 20/20) | 15/20 (step 16/20) | 15/20 (step 15/20) |
| back-to-back 1 ms chain | 20/20 | 4/20 | 3/20 |

Free TIMER21 against MPSL_TIMER0 over each 298 ms hold: 0-2 us. MPSL_TIMER0
counts through 100 ms without reset. Every BLE failure is a clean
EXTEND_FAILED at an advertising event (refused at 79-193 ms). The
back-to-back row hits one event repeatedly: it asks for 300 ms in under
0.5 ms, so successive grants fail 7 ms nearer the same event. **The 100 ms
ceiling is on the requested length, not on the extended total.** The
"100 ms ceiling" section above is therefore wrong about MPSL; it remains
true of the gate as written (`granted_len_us + step <= LENGTH_MAX_US`).

**F2: two sessions in one client. Works; first come, first served.**

| Layout (30 iterations, S2 at S1 + 40 ms) | No stack: S2 granted / blocked | BLE run 1 | BLE run 2 |
|---|---|---|---|
| A: S2 placed while S1 ends at 5 ms, S2 HIGH | 30 / 0 | 29 / 1 | 29 / 1 |
| A, S2 NORMAL | 30 / 0 | 30 / 0 | 30 / 0 |
| A, S1 steps of 200 us, S2 HIGH | 30 / 0 | 30 / 0 | 30 / 0 |
| B: S2 placed after S1's 60 ms step covers it, HIGH | 0 / 30 | 0 / 24 (6 skipped) | 0 / 26 (4 skipped) |
| B, S2 NORMAL | 0 / 30 | 0 / 26 (4 skipped) | 0 / 25 (5 skipped) |

Both sessions open together (rc 0). In A, session 1's extension is refused
0.2-0.4 ms before session 2's start; handover from session 1's END to
session 2's start is 3.0 ms with 1 ms steps and the 2 ms end margin, 2.4 ms
with 200 us steps. Session 2 starts within 17 us of its asked-for instant.
In B, session 2 is BLOCKED about 28 us after the request. Session 1 is never
cut. HIGH and NORMAL behave the same. The BLE blocks and skips are
advertising events, not session 1.

**F3: ACTION_REQUEST at a grant's end. Short notice is not refused.**

| Gap after close | No stack: blocked (xtal / noguar, of 300 each) | BLE: blocked, 2 runs x 2 modes (of 300 each) |
|---|---|---|
| 50, 100, 200, 300, 500, 750, 1000, 1500, 2000, 3000 us | 0 / 0 at every gap | 1-3 at every gap (0.3-1.0 %) |

Start error 0-41 us. Under BLE the rate does not depend on the gap: each
blocked grant is an advertising event, and BLOCKED arrives at the request
instant. Not measured under Thread, where bug 26 saw about 80 % blocked at
2.5 ms notice.

**Pruning (the plan's rules):**

- F1 passed: **(a) becomes a full fix and moves first.** One grant can cover
  the whole period, so the "about 21 % deaf, drifting" estimate for (a) no
  longer applies.
- F2 did not show the tracked request blocked or cut in (d2)'s layout (A):
  **(d2) is kept**, with one rule: the scan must not extend over a tracked
  start before that start's request is placed (layout B blocks 100 %).
  Priority cannot help, and a big bridging extension breaks (d2).
- F3 is not mostly BLOCKED with no stack (0/6000): **(d1) is kept**; its open
  question is Thread.
