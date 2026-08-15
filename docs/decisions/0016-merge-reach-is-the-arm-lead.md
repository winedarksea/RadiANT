# 0016 — The merge reach is the arm lead, because the arm lead is an exclusion radius

- **Status:** accepted, **verified deterministically in ztest and reproduced on
  air** — 12 tracked channels across two independent master boards, `sched_missed`
  145/172 pre-fix → 5. The `RX_FAIL_GO_TO_SEARCH` half is still owed a quiet room.
  See *Verification status*, including why the control must be built from the
  pre-fix revision and not from `CONFIG_RADIANT_PHY_LR_CODED`.
- **Date:** 2026-08-15
- **Builds:** [0005](0005-extension-inside-ant-plus.md) — its "32 tracked sensors
  do not cost 32 windows" claim rests entirely on the merge rules this record
  changes
- **Depends on:** [0002](0002-clean-room-policy.md) (read scope),
  [0007](0007-long-range-phy.md) (whose accepted arm-lead cost this record
  re-assesses, and which it amends)
- **Amends:** [0007](0007-long-range-phy.md) — *Consequences → Paid*, the
  `min_arm_lead_us` bullet
- **Related:** [0013](0013-sweep-is-the-elastic-consumer.md) — the other place
  where a scheduler policy is stated as a rule about which work gives way;
  [0014](0014-second-vendor-port-what-it-cost.md), whose `max_addr_groups`
  finding sets the residual below

> **Provenance.** Clean-room. Written from this project's own source, its prior
> decision records and its own bench logs, and from Nordic's public nRF52840 /
> nRF54L15 product specifications and the MDK headers shipped in NCS v3.4.0.
> Nothing here derives from sdk-ant, from `libant.a`, from disassembly of any
> binary, or from any non-redistributable ANT+ device profile document. See
> [0002](0002-clean-room-policy.md).

---

## Context

`radiant_sched.c`'s `arm_rx_window()` builds one hardware receive window around
a leader and merges every other channel that can share it. Membership rule 3
required each member to **literally overlap the leader's own window**. A tracked
window is the prediction ± `radiant_channel_guard_us()`, which is 100 µs once
the channel is locked, so literal overlap reached only about 200 µs of channel
separation.

That number was chosen against nothing. The number it needed to be chosen
against is `min_arm_lead_us`, and the reason is one line elsewhere in the same
file: `pass_step()` declares a receive window `DONE_MISSED` as soon as
`t_end < now + arm_lead`. **`min_arm_lead_us` is therefore an exclusion radius,
not slack** — no second window can be armed within `arm_lead` of one already
committed.

The two quantities have to meet, and they did not. Between the ~200 µs the merge
rule reached and the advertised lead there was a band in which two tracked
channels could **neither merge nor be armed in sequence**, and the scheduler
dropped the later one deterministically, every period, until the two masters'
crystals drifted apart. With the coded PHY's lead advertised on every nRF build
(456 µs on nRF54L15, 616 µs on nRF52840 — see
[ADR 0007's amendment](0007-long-range-phy.md#amendment-2026-08-15--the-lead-this-adr-accepted-was-measured-wrong-and-the-phy-is-now-kconfig-gated))
that band was **`max(0, L − 2G)`** wide, where `L` is the advertised lead and
`G` the 100 µs locked guard: **256 µs on nRF54L15** (456 − 200) and **416 µs on
nRF52840** (616 − 200). At eight tracked sensors that is worth roughly **1 % of
slots per channel**.

(The proposal this record implements quoted a single ≈344 µs figure for the
band. It is stated per part here instead, because the band is a difference of
two per-part numbers and no single figure is right for both.)

**It is exactly invisible at one channel**, which is the whole reason it lived
this long: every loss figure this project has recorded is single-channel —
0.139 % / 0.150 % on nRF, 0.257 % for sdk-ant, all three P4 coexistence arms —
and `tools/ant_verify.py` was structurally single-channel. The one contradicting
measurement was already in the repo and unconnected to any of this: three
tracked channels at **0.43 / 1.24 / 0.41 %** against 0.139 % for one.

## Decision

**Merge rule 3's reach is `arm_lead()`.** A member must lie within the leader's
own window *extended by that reach* — not merely within the union accumulated so
far, which would let a chain of barely-touching windows walk the merged span
arbitrarily far.

The rule follows from the exclusion radius rather than sitting beside it: below
`arm_lead` of separation a second arm is not inconvenient, it is **physically
impossible**, so merging is the only way to hear both — which is precisely when
the rule must say yes. The two now agree by construction, and neither can be
retuned without the other.

Three supporting points, each load-bearing:

1. **`could_join_armed()` mirrors the identical test.** The two must agree
   exactly. If the mirror said "no" where the rebuild would say "yes", a
   joinable channel would never trigger the rebuild that would take it; if it
   said "yes" where the rebuild says "no", the request stays pending and asks
   again — the one-wasted-rebuild-per-request case `s.replan` bounds, which
   would then be paid on every tracked pair inside a lead of each other rather
   than never.
2. **Rule 4 is unchanged and is what actually bounds the merged span.**
   `RADIANT_SCHED_MERGE_SPAN_MAX_US` (2000 µs) keeps a merged window from
   outliving the measured 2.19 ms master-to-slave turnaround. A merged pair at
   Δ = 168 µs spans 368 µs, well inside it. The chain-walking that rule 3's own
   comment worries about is rule 4's job, not rule 3's.
3. **Background scans need no new condition.** The existing
   `m->continuous != lead->continuous` test already excludes them.

## The alternative that was rejected: lower the lead and leave rule 3 alone

Gating the coded PHY (ADR 0007's amendment) drops the advertised lead to 168 µs
on nRF54L15 and 328 µs on nRF52840. Put through `max(0, L − 2G)` that narrows the
band from 256 µs to **zero on nRF54L15** — 168 µs is *narrower* than the 200 µs
two windows already overlap across, so on that part the pairwise band closes by
geometry and the reach never has to fire — and from 416 µs to **128 µs on
nRF52840**, which is the part that still has one.

That asymmetry is itself the argument against stopping there: the same source
change leaves one supported part correct and another defective, for no reason
either part knows about. `radiant/tests/src/test_sched.c` pins the derivation
next to `TEST_NRF52840_ARM_LEAD_US`, and the three merge-reach cases set the
lead to nRF52840's 328 µs precisely because the mock's own preset (200 µs, exactly
`2G`) has no band and would have made all three tests vacuous.

**It was rejected because it shrinks the defect rather than closing it, and it
leaves the mechanism intact.** The band is the gap between two independently
chosen numbers; making one of them smaller makes the gap smaller and keeps the
gap. Worse, it makes the residual proportional to the lead, so the defect would
come back in full the day any build turns the coded PHY on — which ADR 0007
explicitly intends to be a supported configuration — or the day a new part
advertises a longer lead for reasons of its own. Both changes are made: the lead
is now PHY-correct *and* the reach is derived from it, so the band is closed by
construction at whatever the lead happens to be.

## What it costs

**The merged window now spans the dead gap between the pair**, not merely their
overlap: up to `arm_lead` of extra receive per merged pair per period. At 4 Hz
that is about **0.07 % duty**. Receive current only — no airtime, no transmit,
nothing on the air that was not there before — and rule 4 still bounds the total
span.

Against that, the pair being merged is one that was previously losing a whole
window every period, so the arm that this buys back is not a marginal one.

## The residual, which is real and is not "eliminated"

`caps.max_addr_groups` is **2** on the nRF backend — logical address 0 is
`BASE0 + AP0` and 1..7 are `BASE1 + AP1..AP7`, so a window expresses at most two
distinct bases, and on the 5-byte tracking format the device number lives inside
the base. Rule 5 caps a window at two distinct tracked device numbers
accordingly.

So this change **rescues pairs, which is exactly the pairwise bad band, and
three tracked channels piled inside one lead still cost the third its window.**
That is a hardware limit expressed through the capability query, not a policy
choice, and closing it needs either a part with more address groups or a
different tracking-format address layout. It is written here so it is asserted
by a test and recorded, rather than rediscovered as a mystery loss on a
multi-sensor bench.

## Consequences

**Gained**

- The deterministic component of multi-channel tracked loss — a pair inside one
  arm lead — goes to zero rather than to a smaller number.
- `min_arm_lead_us` acquires a stated meaning that the scheduler and the backend
  now share: it is the distance below which two pieces of receive work are one
  piece of work. Both `radiant_radio_nrf.c`'s lead comment and
  `arm_rx_window()`'s rule 3 point at each other, so neither can be re-derived
  in isolation.
- [ADR 0005](0005-extension-inside-ant-plus.md)'s "32 sensors do not cost 32
  windows" is on firmer ground than it was: the merge now fires in the cases it
  was always assumed to fire in. The corrected count from
  `caps.max_addr_groups` — sixteen windows rather than four — is unchanged by
  this record.

**Paid**

- ~0.07 % duty at 4 Hz of extra receive per merged pair, receive current only.
- A merge rule whose behaviour now depends on a backend capability, so two
  backends with different leads merge differently. This is correct — the
  capability query is how every other such difference is expressed
  ([0014](0014-second-vendor-port-what-it-cost.md)) — but it means a
  `fake_radio.c` preset with an unrealistic `min_arm_lead_us` certifies the
  wrong geometry, the same trap `max_addr_groups` fell into.

**Refused**

- Lowering the lead alone (above).
- Any change to rule 4's 2000 µs span cap. It bounds a different failure and
  the merged pair fits inside it comfortably.
- Any claim that three-way pile-ups are fixed.

## Verification status

**Deterministic** (`radiant/tests/src/test_sched.c`, on the fake radio's virtual
clock; `fake_radio.c` already exposes a mutable `min_arm_lead_us` and an nRF
preset, so no new harness was needed):

- two tracked slots Δ = 300 µs apart — inside `arm_lead`, outside twice the
  tracked guard — merge instead of the later one reporting `DONE_MISSED`;
- **the bad-band sweep**: Δ stepped across 0–600 µs with `missed == 0` asserted
  at every Δ where a merge is geometrically possible. This is the test that
  would have caught the original defect, and it is the one to keep;
- a three-way pile-up still drops one, asserted, so the `max_addr_groups == 2`
  residual is recorded by the suite rather than rediscovered.

**On air, and the defect reproduced.** 2026-08-15, nRF54L15 DK receiver, 12
tracked channels, 600 s per arm under identical load. Two independent master
boards — an nRF52840 dongle (6 masters) and a CC26x2 LaunchPad (6 masters) —
giving **36 cross-board pairs**, with deliberately mixed channel periods (8182 /
8070 / 8086 counts) so that 18 of those pairs beat through the whole 250 ms
separation space every 18–128 s rather than waiting on crystal drift:

| arm | `min_arm_lead_us` (read from the ELF) | merge reach | `sched_missed` | failed | denied |
|---|---|---|---|---|---|
| pre-fix, run 1 | 456 | 200 (literal overlap) | **145** | 0 | 0 |
| pre-fix, run 2 | 456 | 200 | **172** | 0 | 0 |
| this record | 168 | 168 (`arm_lead()`) | **5** | 0 | 0 |

A ~30× reduction. **The control repeat is the load-bearing evidence**, not the
ratio: between the two pre-fix runs the room got much quieter (28.5 % → 13.1 %
aggregate loss) while `sched_missed` went *up*, 145 → 172. The counter is
decoupled from the air, which is exactly the property that lets it be gated in a
room at 5–8 %. Absolute per-channel loss moved by 15 points between two runs of
the *same* image and is recorded but not gated.

**THE CONTROL MUST BE BUILT FROM THE PRE-FIX REVISION, NOT FROM
`CONFIG_RADIANT_PHY_LR_CODED=y`.** The first attempt at this sitting used the
Kconfig knob to raise the lead back to 456 µs and got `sched_missed = 15` — a
near-null result that reads as "no defect". The knob cannot reopen the band, and
the reason is this record's own decision: the reach *is* `arm_lead()`, so raising
the lead raises the exclusion radius and the merge reach together and the band
stays `max(0, L − L) = 0` for every `L`. Only a build whose rule 3 is literal
overlap has a band at all. (The 15 is not noise either — it is the
`max_addr_groups` residual below, which does scale with the lead.)

**Still owed a quiet room:** the `RX_FAIL_GO_TO_SEARCH` half. It did not
discriminate here (79 / 48 pre-fix vs 106 for this record, no ordering) because
at 13–29 % ambient loss eight consecutive *RF* misses swamp eight consecutive
*scheduling* misses; the arms differed by 12 dB of RSSI. The ~2 s artefact
dropout is therefore **neither supported nor refuted on air**, and the fixed
arm's 106 must not be read as a regression.

One honest limitation of the counter: `RADIANT_RADIO_ETIME` funnels through
`arm_rejected` → `DONE_MISSED` → `sched_missed`, so it is not separable from
bad-band misses by this counter alone. The fixed arm's 5 misses over ~29,000
tracked windows (0.017 %) bounds any ETIME contribution to negligible.
