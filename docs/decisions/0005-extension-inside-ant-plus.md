# 0005 — RadiANT extensions live inside the ANT+ network, not beside it

Checked by: the Tier 4 interop capture — a shipping sdk-ant dongle and a Garmin
head unit both continue to find and hold every standard sensor while a RadiANT
telemetry node transmits alongside them, recorded as an `.antcap` and added to
the `ant_verify.py --replay` fixtures so the claim is regression-tested rather
than remembered. The adaptive-frequency clause is checked by its own bench
gate: a node moved to a quiet RF channel holds `loss (exact)` ≈ 0 over 300 s.

- **Status:** Accepted. **Amended 2026-08-09 — extension axis 3 is withdrawn.**
  **Amended 2026-08-11 by [ADR 0007](0007-long-range-phy.md) — axis 4 is built
  and is LE Coded rather than lower-rate GFSK; axis 3 is partially unblocked for
  that PHY only, and its withdrawal otherwise stands.**
  **Amended 2026-08-11 by [ADR 0012](0012-adaptive-frequency.md) — axis 5 is
  built, and one clause of it ("so a searching receiver still finds every node")
  is corrected in place.**
  The decision itself stands; see *Amendment* immediately below.
- **Date:** 2026-08-08
- **Amended:** 2026-08-09
- **Related:** [0001 backend selection](0001-backend-selection-and-release-default.md),
  `../radiant-telemetry.md`, `../profile-registry.md`, `../radiant-security.md`,
  `../spike-b-part2-results.md`

## Amendment, 2026-08-09 — axis 3 is withdrawn

**Status of the amendment:** Accepted. **What changed:** extension axis 3,
*longer frames via the length byte*, has no mechanism behind it and is
withdrawn. **What did not change:** the decision this ADR records — that RadiANT
extensions live on ANT+ network `A6 C5`, RF channel 57, rather than on a network
of their own.

**Why.** This ADR was written on the reading that byte 3 of an ANT frame is a
length: `0x0A` = 8 payload + 2 CRC. `docs/spike-b-part2-results.md` measured
that reading false. **Byte 3 is a control byte of six independent fields, and
its low bits are not a length either** — `0x0A` reads 10 in bits 4:0 and `0xA2`
reads 2, and both carry an eight-byte payload, across 3,104 CRC-valid frames.
**There is no length field in an ANT frame.** The two bits that move are the
slot-opening bit and a one-bit sequence number; bits 2:0 are `010` on every
frame ever captured and their meaning is unknown.

This ADR also staked its own falsifiable prediction on that reading — that an
advanced-burst frame would show `0x1A` in byte 3. **That prediction has been
tested and has failed.** Advanced burst was enabled, the dongle accepted 24-byte
blocks, every transfer completed, and each block was fragmented into three
8-byte packets. No frame with a payload other than eight bytes has ever been on
this bench's air. The only `0x1A` ever seen failed its CRC and was an ordinary
bit error.

**The size of the damage, stated in both directions so it is neither inflated
nor minimised.**

- **Axis 3 is gone, not merely unproven.** It is not "an extension path that
  needs testing"; it is an extension path with nothing underneath it. A stated
  path that cannot work is worse than no path, because a profile will be
  designed against it. Anything that cited axis 3 as an available capability was
  citing a mechanism that does not exist — `../radiant-telemetry.md` did, in two
  places, and both are corrected.

  > **Partially unblocked 2026-08-11 by [ADR 0007](0007-long-range-phy.md), for
  > the long-range PHY only. THIS WITHDRAWAL STANDS.**
  >
  > Axis 3 asked whether a length could be *inferred from an ANT frame*, and the
  > answer is still no and always will be: byte 3 reads 10 on a broadcast and 2
  > on an in-slot frame for the same eight payload bytes. That is a fact about
  > ANT and nothing changes it.
  >
  > ADR 0007 authors a length field in a *RadiANT-written* format, at an offset
  > it chose, on a PHY a stock ANT receiver cannot demodulate at all. Nothing is
  > inferred and no receiver is confused, so the two reasons for this withdrawal
  > — no mechanism, and the cost to every merged RX window — are both absent:
  > the mechanism is authored rather than discovered, and a coded channel could
  > never join the merged 1 M window in the first place.
  >
  > The permission is stated narrowly on purpose: **length extension is
  > permitted exactly where the PHY already makes us invisible.** Not on RadiANT
  > device types, and not off RF 57 — a device type is a byte anyone can read
  > and a frequency is a place anyone can tune to, and neither makes a malformed
  > frame unheard. Whether the same authorship argument extends to a
  > RadiANT-authored **1 M** format is a *different claim*, is explicitly open
  > in ADR 0007, and is owed a Tier 4 capture nobody has taken.
- **The other four axes are untouched.** New pages, new device types, a
  per-type long-range PHY and per-type adaptive frequency never depended on the
  length reading. Axis 5 in particular, which is what actually removes the last
  argument for a second network, rests on the descriptor and the scheduler's
  per-operation radio state.
- **The load-bearing argument for the decision is untouched.** A second network
  splits the radio schedule, permanently and structurally, and the merged RX
  window is what makes 32 tracked channels affordable. That reasoning never
  mentioned byte 3.
- **One argument for a second network is genuinely restored, and it is new.**
  The rejected-alternatives section below claimed a second network buys *exactly
  one* thing — escape from 2457 MHz — partly because longer payloads were
  already available in place. They are not available anywhere now. If a longer
  payload ever becomes necessary it will need a **new framing**, and a fresh
  network address is a cheaper place to introduce new framing than a network
  stock receivers are listening on. That is a real argument and it is recorded
  here rather than argued away. It does not currently change the decision,
  because the mechanism it would need is not merely unbuilt but **unknown** —
  nobody has put a non-eight-byte ANT frame on the air — and because nothing in
  v1 needs one. Revisit this ADR if that changes, not before.

**Where longer payloads stand now: unknown.** Not "later", not "reserved" —
unknown, and not currently achievable by any measured mechanism. Bits 2:0 =
`010` have a *disproved* meaning rather than a measured one. What would settle
it is a frame with a payload other than eight bytes actually on the air, decoded
with its control byte read: two ends that both negotiate advanced burst would do
it, or a RadiANT transmitter emitting one deliberately with a receiver that can
decode it. Until such a frame exists, **do not invent a replacement mechanism
and do not write a profile that assumes one.**

One consequence for the test plan: the Tier 4 interop capture's
longer-than-8-byte sub-case has nothing to emit. `../testing.md` still asks for
it. The rest of Tier 4 — a stock dongle and a head unit holding every standard
sensor while a RadiANT node transmits — is unaffected and remains this ADR's
check.

The specific claims below are annotated in place rather than edited away, so
that what this ADR argued from remains readable.

## Context

`radiant_core` is a superset, not a clone. Beyond byte-exact ANT+ compatibility it
is meant to carry the things Garmin stopped developing: 32 simultaneous
channels instead of 8, background scan mode, a generic telemetry envelope, new
device types, and optional per-channel security. All of that needs somewhere to
live on the air.

The natural-looking answer is a second network — a different 2-byte network
address, arguably a different RF channel, a clean space with no legacy in it
and no risk of confusing a stock receiver. It is the answer most protocol
extensions reach for and it is the one this ADR rejects.

Some facts that constrain the choice:

- **The ANT+ network is address `A6 C5` on RF channel 57 (2457 MHz).** Every
  ANT+ device is there. Discovery, pairing and all standard traffic happen
  there.
- ~~**The on-air length byte already expresses payload size.** `0x0A` = 8 payload
  + 2 CRC. It is a field, not a constant, and the nRF radio's `PCNF0.CRCINC`
  exists for exactly this ShockBurst-style arrangement. A frame with a longer
  payload is expressible today with no new framing and no new network — and a
  falsifiable prediction of that reading is that an advanced-burst frame should
  carry `0x1A`.~~
  **FALSIFIED, 2026-08-09** — `../spike-b-part2-results.md`. Byte 3 is a control
  byte; its low bits read 10 on `0x0A` and 2 on `0xA2` with the same eight-byte
  payload. It is a constant, not a field, and the prediction it offered was
  tested and failed. See *Amendment*.
- ~~**A stock ANT receiver's `MAXLEN` rejects an over-long frame harmlessly.** It
  does not confuse the receiver; it fails length validation and is dropped.~~
  **MOOT, 2026-08-09.** This constrained the choice only because an over-long
  frame was thought to be emittable. None can be emitted, so nothing reaches a
  stock receiver's `MAXLEN` to be rejected, and the claim itself was never
  measured against a real one.
- **A receiver never opens a channel for a device type it does not recognise.**
  Device type is 7 bits (the MSB is the pairing bit), so 1–127, with a
  substantial unallocated range. New device types are invisible by
  construction.
- **New pages within an existing device type are zero-risk.** Receivers skip
  page numbers they do not know.
- **The scheduler's cost model is windows, not bytes.** All ANT+ traffic being
  on RF 57 is what allows overlapping tracked RX windows to be *merged* into a
  single hardware window. That merge is the highest-value item in the
  scheduler: it is why 32 tracked sensors do not cost 32 windows, and why
  slave-side collisions between tracked channels drop to zero.

  **Corrected 2026-08-10 — "up to 8 filters" was true of the search format and
  false of the tracking one, and the number is 2.** The nRF's eight logical
  addresses are one `BASE0+AP0` and seven `BASE1+AP1..AP7`, so a window carries
  two distinct bases; on the 5-byte tracking address the device number is
  *inside* the base, so two tracked channels share a window only if they are the
  same sensor. Thirty-two tracked sensors therefore cost **sixteen** windows,
  not four. The claim survives — sixteen is not thirty-two — but it was
  measured against `fake_radio.c`, which advertised eight filters and modelled
  no base at all, so CI could not see that the real backend refused every set
  the scheduler built. `caps.max_addr_groups` is the repair; see
  `docs/backends.md`, "the caps table".
- **The residual loss floor is real and understood.** ~0.4%, characterised at
  0.26–0.60%, is memoryless per-slot collision with Wi-Fi channel 11 over
  2457 MHz, with an `RX_FAIL` for every hole. Frequency agility would fix it;
  ANT+ forbids it, so a stock sensor eats that floor forever.

That last point is the only genuine argument for a second network, and it is
worth stating in its strongest form: 2457 MHz sits in a bad neighbourhood, and
a second network could simply not be there.

## Decision

**Every RadiANT extension lives inside the ANT+ network — address `A6 C5`, RF
channel 57 — and is expressed through the existing extension axes rather than
through a new network.**

The axes, in increasing order of compatibility risk. **Five were stated; four
survive.** Axis 3 is withdrawn and is kept in place, struck through, so that
later numbering still means what it meant and so that nothing quietly
renumbers itself:

1. **New pages within existing device types.** Zero risk; invisible to
   receivers that skip unknown page numbers.
2. **New device types.** A receiver that does not recognise the type never
   opens a channel for it. Every type and page claimed is recorded in
   `docs/profile-registry.md`, which is **public with a process**: third
   parties claim by PR, first merged wins, each entry recording claimant, date
   and page schema. Allocation is the one part of Garmin's role that actually
   needs a successor, and a file plus a PR queue is the cheapest legitimate
   one. The collision risk is stated honestly there — Garmin has stopped
   allocating, but "stopped" is not "guaranteed never".
3. ~~**Longer frames via the length byte.** No new network, no new framing.
   Reserved for profiles that genuinely cannot fit 8 bytes, because it is not
   free: a longer frame lengthens the RX window for every channel merged into
   it.~~

   **WITHDRAWN, 2026-08-09 — this axis has no mechanism.** There is no length
   byte. Byte 3 is a control byte of six independent fields and none of them is
   a length; `0x0A` reads 10 in bits 4:0 and `0xA2` reads 2, both with an
   eight-byte payload, over 3,104 CRC-valid frames
   (`../spike-b-part2-results.md`). **Do not design a profile against this
   axis.** How a longer ANT payload would be expressed on air is **unknown**,
   and no measured mechanism produces one: advanced burst was enabled twice and
   fragmented both times, so no frame with a payload other than eight bytes has
   ever been on this bench's air. Bits 2:0 = `010` therefore have a *disproved*
   meaning, not a measured one. What would settle it is such a frame on the air
   with its control byte decoded — two ends that both negotiate advanced burst,
   or a deliberate RadiANT transmitter and a receiver that can decode it. The
   RX-window cost noted above was the only part of this axis that was ever
   about scheduling rather than framing, and it still applies to whatever
   mechanism eventually replaces it. Nothing replaces it today.
4. **A long-range PHY, per device type, not in v1.** ~~A lower-rate GFSK variant
   on the same band~~, opted into per device type and marked as such in the
   registry. ANT+ compatibility channels never change PHY, so this axis cannot
   touch a standard device by construction. The HAL accommodates it already —
   PHY is a backend/caps property, so `radiant_radio_caps` grows a supported-PHY
   list rather than needing a redesign.

   > **Amended 2026-08-11 by [ADR 0007](0007-long-range-phy.md), which builds
   > this axis.** It is **Bluetooth LE Coded at S=8**, not lower-rate GFSK. The
   > enum reserved for it is now `RADIANT_PHY_LR_CODED` — it was
   > `RADIANT_PHY_LR_GFSK`, keeping the same numeric value — and the registry's
   > `LR PHY` column now carries a coding rate rather than a yes.
   >
   > The substitution is not a detail. Narrowing the modulation buys ~3 dB per
   > halving of bandwidth and needs a soft-decision receiver nobody here is
   > going to write; the coded PHY buys **~8 dB** from FEC that is already in
   > the silicon on every part in view, for one register write. The rest of this
   > paragraph is unchanged and turned out to be exactly right: the HAL needed a
   > table entry and not a redesign, and compatibility channels are untouched by
   > construction — ADR 0007 hardens that from "cannot" to "permanently
   > excluded, not deferred".
5. **Adaptive frequency, per device type.** The descriptor announces the node's
   RF channel; discovery and pairing stay on RF 57 so a searching receiver
   still finds every node; data lives wherever the descriptor says; and an
   in-band announcement ("next epoch on RF N") lets a deployed pair move away
   from fresh interference. Standard channels never move and only RadiANT
   receivers follow.

   > **Amended 2026-08-11 by [ADR 0012](0012-adaptive-frequency.md), which
   > builds this axis — and corrects one clause of it.**
   >
   > Everything above is delivered: the candidate set is BLE's advertising
   > channels (RF 2/26/80, defaults rather than a restriction), selection is
   > driven by the channel-quality map, the announcement is page `0x13` with a
   > countdown that every receiver acts on at expiry rather than on receipt, and
   > moves are rate-limited to minutes-scale. "Next epoch on RF N" turned out to
   > be figurative: the boundary is a **message count**, not the security epoch,
   > which for a hostless node is a boot counter and must not be spent on this.
   >
   > **"So a searching receiver still finds every node" claims more than the
   > mechanism delivers.** A node that has moved is not transmitting on RF 57 at
   > all, so a wildcard sweep does not find it. What is true and permanent is
   > the first half: **the sweep is 1 M on RF 57 forever and no frequency axis
   > multiplies it.** A moved node is re-acquired from the descriptor a receiver
   > already holds, from the sync handoff of page `0x12`, or from a bounded retry
   > list of at most five named indices — which is not a sweep and does not touch
   > "certain within one sweep". ADR 0012 records why the alternative reading
   > (descriptor on 57, data elsewhere) was rejected: it doubles the receiver's
   > windows where this ADR costed one, and it breaks the countdown's clock.
   >
   > The rest of this ADR is unaffected. The window-splitting cost below is
   > exactly right, is now asserted by a scheduler test rather than inherited,
   > and remains scoped to the types that opted in — which is still what removes
   > the last argument for a second network.

### Why not a second network

**Because it buys exactly one thing, and axis 5 already provides that thing
without the cost.**

> **Amended 2026-08-09.** "Exactly one" was true while axis 3 existed. With axis
> 3 withdrawn the honest count is **one thing now and a second thing
> conditionally**: a fresh address is a cheaper place to introduce a *new
> framing*, which is what a longer payload would require. The condition is that
> nobody knows what a longer ANT frame looks like, so there is no framing to
> introduce. The scheduling argument below is unchanged and is what carries the
> decision. See *Amendment*.

Enumerate what a second network would actually deliver over extending in place:

- ~~*Longer payloads?* No — the length byte already carries them (axis 3).~~
  **Amended 2026-08-09.** *Longer payloads?* **Not from either option, today.**
  There is no length byte, axis 3 is withdrawn, and no measured mechanism puts a
  payload other than eight bytes on the air on any network. What a second
  network would offer is not the payload but the *freedom to define new framing
  for it* without a stock receiver listening — a real advantage, currently over
  a capability that does not exist. It is recorded in *Amendment* as the one
  argument this falsification restored.
- *New device types and pages?* No — the existing type and page space has room,
  and unknown types are ignored by construction (axes 1 and 2).
- *Isolation from stock receivers?* No, and this is the one people expect to be
  yes. Unknown device types are already invisible; ~~over-long frames are already
  rejected on `MAXLEN`~~ — and there are no over-long frames, so there is nothing
  for `MAXLEN` to reject (*Amendment*, 2026-08-09). The conclusion is unchanged
  and is now reached with one clause rather than two: there is nothing left for
  a separate address to protect against.
- *A different PHY?* No — that is a per-type property (axis 4) and does not
  need a network address to express.
- *Escape from 2457 MHz?* **Yes.** This is the entire remaining benefit.

And the cost is concrete and permanent: **a second network splits the radio
schedule.** Channels not on RF 57 cannot join the merged RX window. Every
off-57 channel is a separate hardware window with its own guard time, competing
for the same radio against tracked ANT+ channels. The 32-channel design works
because merging collapses shared-frequency channels into one window with up to
8 filters; a parallel network on another frequency dismantles that for
everything that uses it, and does so *structurally* — not as a tuning problem
but as a property of having two frequencies.

So the trade is: pay a permanent, project-wide scheduling cost, to obtain
frequency escape.

**Axis 5 obtains the same frequency escape without paying it project-wide.**
Adaptive frequency is a per-device-type opt-in. A node that needs to escape
2457 MHz escapes it; every other channel stays merged on RF 57. The
window-splitting cost is real for that node — an off-57 channel cannot join the
merged window, exactly as with a second network — but it is *scoped to the
types that asked for it* rather than imposed on the design. That is the same
benefit at a fraction of the cost, which is what removes the last argument for
a second network.

The scheduler already treats radio configuration as **per-operation state**
rather than global — a consequence of search needing a different packet
configuration from tracking (`PCNF0=0`, `BALEN=2`, `STATLEN=12`, with
`devnum_lo` recovered from `RADIO->RXMATCH`) — so per-channel frequency costs
the scheduler nothing structural. That property is what makes axis 5 cheap and
is worth noting: the design decision that made search work is the same one that
makes adaptive frequency affordable.

### When to revisit

Only if measurement shows 2457 MHz is genuinely the limiting factor across the
board rather than for particular deployments. **The existing bench data does not
support that**, and the surrounding investigation is closed: a 16 dB
transmit-power spread stays inside one setting's own run-to-run spread, no
frequency beat the ANT+ default, and the residual loss is accounted for
per-packet with an `RX_FAIL` for every hole. Do not re-run the frequency or
transmit-power sweep on the strength of this ADR.

**A second trigger, added 2026-08-09.** Also revisit if a frame with a payload
other than eight bytes is ever put on the air and decoded, because that is the
mechanism axis 3 turned out not to have, and because a *new framing* is the one
thing a separate network address would deliver more cheaply than extending in
place. Neither half of that is true today. Do **not** revisit on the strength of
axis 3's withdrawal alone: the scheduling cost of a second network is unchanged
and is what decided this ADR.

## Consequences

**Good.**

- No second radio schedule, no second network to time-share, no second
  discovery mechanism for tools to learn.
- Extensions are invisible to receivers that do not want them, by construction
  rather than by convention — which is a claim the Tier 4 capture can actually
  test.
- The merged RX window survives, which is what makes 32 tracked channels
  affordable at all.
- Frequency escape is still available where it is genuinely needed, scoped to
  the device types that opt in.
- The registry gives the ecosystem an allocation process, which is the piece of
  the ANT+ Alliance's function that its shutdown actually removed.

**Costs, accepted.**

- Type and page collisions with any future third-party allocation are possible.
  Mitigated by a public registry with a process, not eliminated. Written into
  `docs/profile-registry.md` honestly.
- ~~Over-long frames must be verified against real stock receivers rather than
  reasoned about — `MAXLEN` handling is the case most likely to upset one, and
  it is the specific case the Tier 4 capture repeats.~~
  **Amended 2026-08-09: this cost is not currently payable, and that is worse
  than paying it.** With axis 3 withdrawn there is no over-long frame to emit,
  so the Tier 4 sub-case has nothing to transmit and the `MAXLEN` question stays
  open indefinitely rather than being answered. If a longer-payload mechanism is
  ever found, this cost returns unchanged and unverified, and it must be paid
  before any profile ships against it. `../testing.md`'s Tier 4 still asks for
  the longer-than-8-byte capture; the rest of Tier 4 is unaffected.
- An adaptive-frequency node costs its own RX window, the same as a
  second-network channel would. The saving is that only nodes that opted in pay
  it.
- Sharing RF 57 means RadiANT traffic contends with ANT+ traffic for airtime.
  At 4 Hz with ~400 µs windows, 32 channels is ~19% duty — comfortable — but it
  is not free, and profile design should still choose the slowest period the
  data tolerates rather than treating airtime as unlimited.
