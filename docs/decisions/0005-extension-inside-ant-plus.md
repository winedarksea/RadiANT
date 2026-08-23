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
  **Related 2026-08-15: [ADR 0016](0016-merge-reach-is-the-arm-lead.md) changes
  the merge rule this record's "32 sensors do not cost 32 windows" depends on.**
  The claim and the corrected sixteen-window count below are unaffected; what
  changed is that the merge now fires in a band of channel separations where it
  previously could not, and where nothing else could serve the pair either.
  The decision itself stands; see *Amendment* immediately below.
- **Date:** 2026-08-08
- **Amended:** 2026-08-09
- **Related:** [0001 backend selection](0001-backend-selection-and-release-default.md),
  `../radiant-telemetry.md`, `../profile-registry.md`, `../radiant-security.md`,
  `../spike-b-results.md`

## Amendment, 2026-08-09 — axis 3 is withdrawn

**Status of the amendment:** Accepted. Extension axis 3, *longer frames via the
length byte*, has no mechanism behind it and is withdrawn. The decision this
ADR records — that RadiANT extensions live on ANT+ network `A6 C5`, RF channel
57, rather than on a network of their own — is untouched.

**Why.** This ADR was written on the reading that byte 3 of an ANT frame is a
length: `0x0A` = 8 payload + 2 CRC. `docs/spike-b-results.md` measured
that reading false. **Byte 3 is a control byte of six independent fields, and
its low bits are not a length either** — `0x0A` reads 10 in bits 4:0 and `0xA2`
reads 2, and both carry an eight-byte payload, across 3,104 CRC-valid frames.
**There is no length field in an ANT frame.** The two bits that move are the
slot-opening bit and a one-bit sequence number; bits 2:0 are `010` on every
frame ever captured and their meaning is unknown. This ADR also staked a
falsifiable prediction on the length reading — an advanced-burst frame showing
`0x1A` in byte 3 — and that failed too: advanced burst fragmented every 24-byte
block into three ordinary 8-byte packets, and the only `0x1A` ever seen failed
its CRC.

**Axis 3 is gone, not merely unproven** — a stated path that cannot work is
worse than no path, because a profile will be designed against it.
`../radiant-telemetry.md` cited it as available in two places; both are
corrected. The other four axes, and the load-bearing scheduling argument for
this decision (below), never depended on the length reading and are untouched.
One argument for a second network is genuinely restored: a fresh network
address would be a cheaper place to introduce a *new* framing than a network
stock receivers listen on, if a longer payload ever becomes necessary — but the
mechanism it would need is not merely unbuilt, it is **unknown** (nobody has
put a non-eight-byte ANT frame on the air), and nothing in v1 needs one.
Revisit this ADR if that changes, not before.

**Where longer payloads stand now: unknown, not "later" or "reserved."** Bits
2:0 = `010` have a *disproved* meaning, not a measured one. What would settle
it is a frame with a payload other than eight bytes actually on the air,
decoded with its control byte read. **Do not invent a replacement mechanism and
do not write a profile that assumes one** until such a frame exists.

**Partially unblocked 2026-08-11 by [ADR 0007](0007-long-range-phy.md), for
the long-range PHY only — this withdrawal otherwise stands.** ADR 0007 authors
a length field in a RadiANT-*written* format on a PHY a stock ANT receiver
cannot demodulate at all, so neither reason for this withdrawal (no mechanism;
cost to every merged RX window) applies there. Whether the same authorship
argument extends to a RadiANT-authored 1 M format is a different, still-open
claim — see that record.

One consequence for the test plan: the Tier 4 interop capture's
longer-than-8-byte sub-case has nothing to emit; `../testing.md` still asks for
it. The rest of Tier 4 is unaffected and remains this ADR's check.

## Context

`radiant` is a superset, not a clone. Beyond byte-exact ANT+ compatibility it
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
- **There is no on-air length byte.** Byte 3 is a control byte; its low bits
  read 10 on `0x0A` and 2 on `0xA2` with the same eight-byte payload — a
  constant, not a field. **Falsified 2026-08-09**, `../spike-b-results.md`;
  see *Amendment*. (A stock receiver's `MAXLEN` handling is therefore moot: no
  over-long frame can be emitted to reach it.)
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
3. ~~**Longer frames via the length byte.**~~ **WITHDRAWN, 2026-08-09 — see
   *Amendment* above; no mechanism exists.** Kept here, struck through, only so
   the later axis numbers do not shift.
4. **A long-range PHY, per device type, not in v1.** Opted into per device type
   and marked as such in the registry. ANT+ compatibility channels never change
   PHY, so this axis cannot touch a standard device by construction. The HAL
   accommodates it already — PHY is a backend/caps property, so
   `radiant_radio_caps` grows a supported-PHY list rather than needing a
   redesign. **Built by [ADR 0007](0007-long-range-phy.md) as Bluetooth LE
   Coded at S=8** — not the lower-rate GFSK originally reserved here, because
   coded buys ~8 dB from FEC already in the silicon for one register write
   against ~3 dB per halving of bandwidth and a soft-decision receiver nobody
   here would write; see that record. The paragraph above otherwise turned out
   right: a table entry, not a redesign, and compatibility channels untouched.
5. **Adaptive frequency, per device type.** The descriptor announces the node's
   RF channel; discovery and pairing stay on RF 57; data lives wherever the
   descriptor says; and an in-band announcement lets a deployed pair move away
   from fresh interference. Standard channels never move and only RadiANT
   receivers follow. **Built by [ADR 0012](0012-adaptive-frequency.md)**, which
   also corrects this axis's claim that a searching receiver "still finds every
   node": a node that has moved is not on RF 57 at all, so only the first half
   is true and permanent — the sweep stays 1 M / RF 57 forever and no frequency
   axis multiplies it. A moved node is re-acquired from its descriptor, the
   sync handoff, or a bounded retry list, none of which touch "certain within
   one sweep". See that record for why the alternative (descriptor on 57, data
   elsewhere) was rejected.

### Why not a second network

**Because it buys exactly one thing that matters, and axis 5 already provides
it without the cost.** ("Exactly one" was true before axis 3's withdrawal; the
honest count now is one thing plus a second, conditional thing — a fresh
address would be a cheaper place to introduce a *new framing* for a longer
payload, if one is ever found. The scheduling argument below is what carries
the decision either way.)

Enumerate what a second network would actually deliver over extending in place:

- *Longer payloads?* **Not from either option, today.** There is no length
  byte, axis 3 is withdrawn, and no measured mechanism puts a payload other
  than eight bytes on the air on any network. A second network's real
  advantage — freedom to define new framing without a stock receiver listening
  — is over a capability that does not currently exist.
- *New device types and pages?* No — the existing type and page space has room,
  and unknown types are ignored by construction (axes 1 and 2).
- *Isolation from stock receivers?* No. Unknown device types are already
  invisible, and there are no over-long frames for a stock receiver's `MAXLEN`
  to reject — nothing is left for a separate address to protect against.
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
- **Not currently payable, which is worse than paying it.** With axis 3
  withdrawn there is no over-long frame to emit, so the Tier 4 sub-case that
  would verify `MAXLEN` handling against a real stock receiver has nothing to
  transmit and the question stays open indefinitely. If a longer-payload
  mechanism is ever found, this cost returns unchanged and unverified, and must
  be paid before any profile ships against it. `../testing.md`'s Tier 4 still
  asks for the longer-than-8-byte capture; the rest of Tier 4 is unaffected.
- An adaptive-frequency node costs its own RX window, the same as a
  second-network channel would. The saving is that only nodes that opted in pay
  it.
- Sharing RF 57 means RadiANT traffic contends with ANT+ traffic for airtime.
  At 4 Hz with ~400 µs windows, 32 channels is ~19% duty — comfortable — but it
  is not free, and profile design should still choose the slowest period the
  data tolerates rather than treating airtime as unlimited.
