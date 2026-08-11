# 0012 — Adaptive frequency: leaving 2457 MHz, slowly and out loud

- **Status:** accepted, with its bench gate **owed** — see *What is not measured*
- **Date:** 2026-08-11
- **Builds:** [0005](0005-extension-inside-ant-plus.md) axis 5, and **corrects one sentence of it**
- **Depends on:** [0002](0002-clean-room-policy.md) (read scope), [0005](0005-extension-inside-ant-plus.md) (the extension axes and the merged RX window), RF-4's channel-quality map (`radiant_core/src/radiant_chanmap.c`), RF-5a's descriptor schedule block
- **Related:** [0007](0007-long-range-phy.md) — the other extension axis, which crossed the same compatibility boundary and set the discipline this record follows

> **Provenance.** Clean-room. Written from this project's own RF plan, its prior
> decision records and its own `docs/radiant-telemetry.md`, and from the
> published Bluetooth core specification's advertising-channel placement
> (public). Nothing here derives from sdk-ant, from `libant.a`, from disassembly
> of any binary, or from any adopter-gated ANT+ device profile document. See
> [0002](0002-clean-room-policy.md).

---

## Context

The characterised loss floor on this bench is **~0.4 %**, bounded at
0.26–0.60 %, and it is fully accounted for: memoryless per-slot collision with
Wi-Fi channel 11 over 2457 MHz, with an `RX_FAIL` for every hole. It is the only
remaining loss, every other source having been closed, and **no link-layer change
can touch it** — the interferer is not ours and the frequency is fixed by ANT+.

Frequency agility is the only fix. ADR 0005 reserved it as axis 5 and gated it
on "loss ≈ 0 on a quiet RF channel". RF-4 built the evidence it needs — the
channel-quality map, binned on the same 1 dB scale as the opportunistic
noise histogram so that a deliberate figure and an opportunistic one are
comparable. RF-5a put an RF index in the descriptor. This record spends both.

## Decision

1. **A node moves once, to a quiet index, and stays.** Blind frequency-diverse
   repetition and fast per-event hopping (CSA #2) are **declined, not deferred**;
   the arithmetic for both is in the RF plan and is not re-litigated here.
2. **The candidate set is BLE's advertising channels: RF 2, 26 and 80** (2402,
   2426, 2480 MHz). A node is **not restricted to them** — the descriptor carries
   a full 0..124 index and the encoder will announce any of them.
3. **Selection is driven by RF-4's channel-quality map**, through one ranking
   function that takes the map as an argument, so that "node-side if it can
   measure, receiver-side if not" is two callers and not two rules.
4. **A move is announced in-band before it happens**, on page `0x13`, with a
   countdown in transmitted messages. **Receivers act on countdown expiry, not on
   receipt.**
5. **Moves are rate-limited to minutes-scale**, as a structural refusal.
6. **An off-57 channel cannot join the merged RX window.** The accepted price of
   axis 5, scoped to the types that opted in.
7. **Discovery never moves.** The search sweep is 1 M on RF 57, the three-byte
   search format, forever.
8. **ANT+ compatibility channels are permanently excluded.** Not deferred —
   excluded.

---

## The candidate set is BLE's, and that is an argument

RF 2, 26 and 80 sit in the gaps between the three non-overlapping Wi-Fi channels
(1, 6, 11). That placement is the outcome of the same survey this project would
otherwise have to repeat with worse instruments, and it has a decade of shipped
deployment behind it. Adopting it costs nothing and inventing an alternative
would cost a measurement campaign to arrive at the same three numbers.

They are defaults in **two** places, and the second one is the reason they are a
list rather than a single choice:

- what the selector considers when a caller supplies no list of its own;
- **what a receiver that has lost an off-57 node tries first.**

Nothing refuses an index outside them. A deployment whose interference does not
look like Wi-Fi should measure and choose, and the map reaches all 125 indices
precisely so that it can.

## Selection: the map is the argument

`profile_freq_select()` is pure and is the only ranking function in the project.
It takes evidence — `{rf_index, have, busy_dbm, floor_dbm}` — and returns an
index or a refusal. The node-side path feeds it `radiant_chanmap_get()`; a
receiver feeds it its own map. **Two receivers given the same evidence must
reach the same answer, tie-breaks included**, or a node takes whichever asked
last; so the tie-break order (busy, then floor, then the lower index) is part of
the contract rather than an implementation detail.

Three rules that are all refusals, because each one is a way this could quietly
choose wrongly:

- **Rank on `busy_dbm`, which is the map's maximum over per-dwell MEANS.** A
  single loud sample is a burst a 250 ms period usually steps around; a whole
  dwell that averaged loud is a transmitter that lives there. Ranking on the
  floor instead would choose the index with the quietest microsecond, which is
  the wrong question. `floor_dbm` is the tie-break.
- **No evidence for the incumbent, no move.** A quiet candidate is not evidence
  that here is loud, and without both halves there is no margin to compute. A
  node that moved anyway would look, on a bench, like adaptive frequency
  choosing at random.
- **A 6 dB margin, or stay.** Two indices within a decibel of each other trade
  places as the room changes, and a node that re-announced every time would be
  hopping slowly rather than adapting. Six is chosen against the effect being
  chased: a Wi-Fi carrier on 2457 MHz is tens of dB above the floor, not six. So
  this admits the move the feature exists for and refuses the ones that are noise
  in the measurement.

With `CONFIG_RADIANT_CORE_ED_SCAN` off, the map's accessor is a no-op inline that
reports nothing, so the node-side path returns "no data" and the node never
moves. That is the receiver-side case arriving at the right answer by
construction rather than by a special case.

## The announcement is a page, and here is why it is not a descriptor frame

The descriptor is where the RF index lives, so a frame in the descriptor set is
the obvious home for a pending change to it. **It is the wrong home, and the
reason was already written down before this phase started.**
`src/profiles/profile_schedule.h` records that `profile_sched.c` encodes the
descriptor set **once at init** and retransmits the bytes — which is exactly why
RF-5a could put a downlink window on the wire but could not announce a *live*
one, and recorded that as the first thing a later phase must fix. A countdown
has the identical shape: it must be recomputed every time the frame goes out. It
hits the same wall for the same reason.

So the announcement is **page `0x13`, a one-frame set**, inserted through
`profile_sched.h`'s existing client seam and present in the rotation only while a
countdown is running. The descriptor keeps saying where the node **is**; the page
says where it is **going**; when the countdown expires the node re-encodes its
descriptor and the page goes away. A node that never moves emits nothing extra
and is byte for byte the node it was.

### The countdown is Layer C's arithmetic and deliberately only that

The private-mode switch ([0008](0008-antplus-additive-pages-and-compat-security.md),
`profile_private.h`) solved the identical problem — announce a move to N
listeners so all of them act on one message — and its arithmetic is **reused
rather than re-derived**:

```
c       = ceil( (move_at - msgs_after_this_one) / 8 )
move_at = msgs_after_this_one + c * 8
```

The node moves its own target to whatever it just said, every time it says
anything. Rounded up, so a receiver holding an older copy retunes **early** —
losing the tail of the old channel rather than the head of the new one, which is
where the descriptor is.

**One sub-case is this record's own, and it is the bug this mechanism ships with
if nobody looks for it.** The re-anchor must not run on the last announcement
slot before the move: applied there it pushes the move one quantum further out,
and the next announcement does it again, and the node announces forever and never
leaves. On a bench that is "adaptive frequency does not work" with nothing in any
log. `profile_freq.c` declines to announce once `move_at <= msgs + 1`, and both
`radiant_core/tests/src/test_freq.c` and `tools/test_ant_pages.py` pin it.

**What is deliberately not borrowed:**

- **No tag, no key, no policy states.** Layer C's announcement *must* be
  authenticated: it takes the node off the air entirely, so an unauthenticated
  "everybody follow me" is a one-packet herding attack strictly worse than
  muting. Here the recovery path is the retry list and then the sweep on RF 57,
  which never moves and needs no key — **so a forged announcement costs a
  receiver one re-acquisition, which is exactly what a *missed* announcement
  already costs.** A defence is worth its complexity when it buys more than the
  failure it prevents, and this one would not. Stated here rather than left
  implicit, so that "we did not authenticate it" is a finding in a document
  rather than a discovery in a review. **Revisit if a RadiANT-native control type
  ever carries an actuator on an off-57 channel**, where a herding attack costs
  availability of something that matters rather than one re-acquisition.
- **No beacon promotion.** One slot in eight from the ordinary data rotation.
- **No bounded duration and no automatic return.** A node on a quiet index has no
  reason to go back on a timer. It returns the way it left, by announcing it, if
  the map ever says 57 is better.

### A receiver counts slots, not messages

The countdown is in **transmitted messages**. A receiver counting only the
messages it *heard* would fall behind by its own loss rate — which is the ~0.4 %
this whole phase exists to remove, so the feature would be least reliable exactly
where it is most needed. A tracked channel opens a window every period whether or
not a frame lands in it, so a slot count matches the node's message count by
construction. The API takes one call per slot carrying the message or `NULL`, so
there is no ordering rule between two functions for a caller to get wrong.

**The same fact bounds where this works: a sparse node is refused.** It does not
transmit in every slot, so its message count and a receiver's slot count are
unrelated and no countdown either end could agree on exists. Refused with a
counter rather than served with something that half works.

---

## What this corrects in ADR 0005, and it is one sentence

ADR 0005 axis 5 reads:

> discovery and pairing stay on RF 57 **so a searching receiver still finds every
> node**; data lives wherever the descriptor says

**The first clause is true and permanent. The emphasised one claims more than
any mechanism here delivers**, and it is corrected rather than quietly
reinterpreted.

A node that has moved is not transmitting on RF 57 at all, so a *wildcard sweep*
does not find it. There is a reading in which it would — the node keeps its
descriptor on 57 and puts only its data pages elsewhere — and that reading is
rejected on three grounds:

1. It costs the node a frequency switch per rotation and the receiver **two**
   windows per node, where ADR 0005 costed axis 5 at "its own RX window",
   singular.
2. It breaks the countdown's clock. With the rotation split across two
   frequencies, a receiver watching one plane cannot count the slots on the
   other, so the one number both ends agree on stops existing.
3. **ADR 0005's own text contradicts it.** The RF plan says the three candidates
   "are the defaults a receiver tries first" — and a receiver only ever *tries* a
   frequency if the node is not on 57. Under the split reading the descriptor
   states the answer exactly and nothing is ever tried, which would make the
   candidate set pointless.

So the honest statement, and the one every consumer should read:

> **The search sweep is 1 M on RF 57 forever, and no frequency axis multiplies
> it.** A node that has moved is outside that sweep's certainty and is
> re-acquired from the descriptor a receiver already holds, from the sync handoff
> of page `0x12`, or from a **bounded retry list of at most five named indices**
> — the last announced target, the three defaults, then RF 57.

This is the same shape of answer [0007](0007-long-range-phy.md) gives one axis
over ("an LR node is found by its 1 M descriptor or by the sync handoff, never by
sweeping the coded PHY"), and the reason is identical: the sweep covers all 256
values of `devnum_lo` and is *certain* to find any transmitting device within one
sweep — a property seven tests in `test_search.c` defend by name. A second PHY
would not lengthen that sweep, it would **multiply** it; a frequency axis would
multiply it again. **The retry list is not a sweep and must not become one:** it
is a bounded walk over named indices, it does not change the sweep's set count,
and nothing in `profile_freq.c` reaches `radiant_search.c` — a property of the
include graph rather than a rule somebody remembers.

## The merged window, and the price

`radiant_sched.c` refuses to merge two windows whose `rf_index` differs, which it
already did. That is not incidental: **a scheduler that merged across
frequencies would arm one window on one of the two and silently drop every packet
from the other**, which on a bench is a sensor that stops working when an
unrelated node moves. It is now asserted rather than inherited —
`test_an_off_57_window_never_joins_the_merged_one` in `test_sched.c` puts two
channels on RF 57 and one on RF 26 with all three overlapping, and checks that
the two merge, the third does not, and the third is served on its own frequency
immediately afterwards rather than lost.

That third window **is** the price, and it is the one ADR 0005 already accepted:
"an adaptive-frequency node costs its own RX window, the same as a second-network
channel would. The saving is that only nodes that opted in pay it."

## The compatibility boundary

**ANT+ compatibility channels are permanently excluded from adaptive frequency.**
Not deferred — excluded, by the same byte-exactness constraint that forbids
touching a page layout, and in the same words ADR 0007 used for the coded PHY. A
compat channel is 1 M on RF 57 for its whole life. No capture, no bench result
and no future ADR changes that without also abandoning the compatibility claim.

**A private-mode switch lands on 1 M / RF 57**, and any frequency move happens
afterwards through the ordinary announcement. One change at a time: a switch that
arrived private, coded *and* off-57 would force a keyholder that missed the
announcement to search PHYs and frequencies as well as device numbers, turning a
solved re-acquisition problem into an unsolved one.

---

## What is not measured

**The bench gate of this phase has not been run, and no number in this record
stands in for it.**

The gate is: loss ≈ 0 on the selected quiet channel with a single copy per event,
against ~0.4 % on RF 57, in one sitting — with the map's verdict agreeing
independently with `tools/ant_sens.py` and the noise histogram. It needs the
Feather that stands in for a stock dongle, which is in its UF2 bootloader and is
reserved for the combined hardware session that also owes ADR 0007 its ≥6 dB
sensitivity ladder and its two `t_sync` calibrations. **No dB figure and no loss
figure appears anywhere in this record, in `profile_freq.c`, or in
`test_freq.c`.**

What *is* verified, and what it is worth:

- Every rule above, as C assertions on real hardware through
  `scripts/run_ztest_hw.ps1` (nRF5340 DK over J-Link) — including the two
  failures a bench would find last: the countdown terminating, and node and
  receiver landing on the same slot when the receiver joined late or lost most of
  the countdown.
- The wire format, byte for byte, against `tools/ant_pages.py` — two
  implementations rather than one checked against itself.
- Both nRF SoC branches compiling with `CONFIG_RADIANT_CORE_BACKEND_NRF=y`
  confirmed in `.config`.

**Until that session runs, this record is a mechanism with an argument and not a
measurement.** The status line says so on purpose. If the gate fails — if a quiet
index does not remove the floor — the mechanism is not thereby wrong, but the
*reason for it* is, and this record should be amended rather than quietly kept.

## What this does not do

- No fast hopping, no per-event frequency diversity, no channel map exchange.
- No move on an ANT+ compatibility channel, ever.
- No move for a sparse node.
- No authentication of the announcement (see above, with its revisit trigger).
- No automatic return to RF 57, and no timer that would produce one.
- No change to the search sweep, in any form.
