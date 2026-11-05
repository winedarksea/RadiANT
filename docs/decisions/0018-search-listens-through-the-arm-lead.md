# 0018 — Under MPSL, a search must listen through the arm lead, or it is deaf for three of them every tracked period

- **Status:** proposed. The measurement is done; the fix is not designed in
  code and nothing here has been built. It becomes its own plan.
- **Date:** 2026-11-05
- **Builds on:** [0013](0013-sweep-is-the-elastic-consumer.md) (the sweep is the
  elastic consumer; tracked slots are inviolate),
  [0016](0016-merge-reach-is-the-arm-lead.md) (`min_arm_lead_us` is an
  exclusion radius, not slack)
- **Related:** `review/2026-10-31-timing` (R1, C5-1, A14-1; unmerged and
  unverified), review 2026-11-05 item 1

> **Provenance.** Clean-room. Written from this project's source, its own bench
> measurements and earlier decision records. Nothing here derives from sdk-ant,
> `libant.a` or any ANT+ profile document. See [0002](0002-clean-room-policy.md).

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

## Candidates, judged against the measurement

**(a) Hold the elastic grant through the boundary, and arm in-grant.** A scan
chunk is already the elastic class: it is granted short and extended, and
the gate handles `SIGNAL_EXTEND`. A grant still held when the next operation
is due can serve it at `min_arm_lead_in_grant_us` (168 µs) instead of 24.4 ms.
The burst path already does this (`radiant_burst.c:331`), and
`gate_acquire()` already serves a request that fits inside `g.grant_end`. If
the scan's grant is extended to cover the tracked window, and the scheduler
plans that window and the chunk after it against the in-grant lead, all three
bands shrink to the tracked window plus 2 × 168 µs. That is under 1 % of the
period, and it is the part a second master at that phase would collide on
anyway. It is the only candidate that closes all three bands with one
mechanism, and the mid-period band closes for free because chunk 2 is
in-grant too.

The risk is ADR 0013 rule 1. The tracked slot would now depend on an
extension MPSL may refuse when the other stack has the air scheduled. **The
rule that keeps it safe:** decide at T − `min_arm_lead_us`, the last instant a
fresh request for the tracked window can still be placed. If the extension
that covers the tracked window has already succeeded by then, keep listening
to T − in-grant lead and arm the tracked window inside the grant. Otherwise
stop the chunk there and place the tracked request exactly as today. A
refused extension then costs listening, never a tracked slot. Under real
802.15.4 traffic some periods will fall back, and the band then reappears in
those periods only, which is no worse than today.

**(b) Let the 3-byte search window cover the tracked slot and route the
tracked master's frame to its channel by channel ID.** This works only when
the tracked channel shares the search's RF index and network. It costs a
search filter per tracked device, out of 8 per window. It turns every tracked
window into a merge across address formats, which ADR 0016's rule 1
deliberately forbids. Its contention exposure is the same as (a)'s, because
the tracked slot again lives inside the search's grant, and it is far more
invasive in the channel layer, which would have to accept timing sync from a
search frame. It solves the same three bands as (a) at a much higher cost.
**Rejected.**

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

## Phase-aware dwell credit, and its stall escape

With (a) working, the never-listened band shrinks to the tracked window
itself. Crediting time is then honest to within a few tenths of a millisecond
per period, so credit needs no change for correctness.

It is still owed for honesty in the periods where (a) falls back, which are
exactly the periods of real coexistence pressure. Credit alone stalls the
sweep, as last session found: a set whose phases cannot all be covered never
advances, and the denial escape fires only on denials, not on uncredited
listening. If it is added, it needs its own escape. Advance after a wall-clock
cap of about 3 × dwell regardless of coverage, and count that as
`advanced_uncovered`. The cheaper first step is that counter alone. Record per
set what fraction of the period it listened to before advancing, and decide
on credit once the A/B under Thread shows how often (a) falls back.

## Recommendation

**(a)**, with the decide-at-the-placement-deadline rule above, the coverage
counter, and no credit change in the first iteration.

**The A/B/A that decides it** uses the same rig and harness (a host process
driving both masters and stepping B's start offset; the bridge's per-frame
log for calibration):

1. **A** (master): the 17-offset sweep plus the 5-offset control, exactly as
   above. Expect 12/17 and 5/5, and mask `0x003fcffc`.
2. **B** (fix): the same. Pass means 17/17 found, each within about one
   sweep. The chunk mask covers every 10 ms bin except, at most, the one
   holding the tracked window. ch0's loss and `sched_missed` stay within the
   room's floor: a two-receiver concurrent reading, since the room has been
   at 5–8 %. And the gate's extend-failed and fallback counts are recorded.
3. **A'** (master again): back to 12/17. Without this leg a room that changed
   between A and B reads as a fix.
4. **Then under Thread**, with an attached 802.15.4 peer (the 5340 as OT
   leader through `scripts/flash_p4_peer.ps1`, so B needs another board).
   Count how often (a) falls back. That number decides whether phase-aware
   credit is needed.

Deterministic coverage comes first, before any of that: a `test_sched` case on
the fake radio with `min_arm_lead_in_grant_us` and the held-reserve model
(`fake_radio.c` already has both), tracking one channel and searching on
another. It asserts that the listened phases cover the period except the
tracked window, and that a refused extension at T − lead still arms the
tracked window on time.
