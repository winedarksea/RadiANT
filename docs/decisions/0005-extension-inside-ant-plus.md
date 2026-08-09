# 0005 — RadiANT extensions live inside the ANT+ network, not beside it

Checked by: the Tier 4 interop capture — a shipping sdk-ant dongle and a Garmin
head unit both continue to find and hold every standard sensor while a RadiANT
telemetry node transmits alongside them, recorded as an `.antcap` and added to
the `ant_verify.py --replay` fixtures so the claim is regression-tested rather
than remembered. The adaptive-frequency clause is checked by its own bench
gate: a node moved to a quiet RF channel holds `loss (exact)` ≈ 0 over 300 s.

- **Status:** Accepted
- **Date:** 2026-08-08
- **Related:** [0001 backend selection](0001-backend-selection-and-release-default.md),
  `../radiant-telemetry.md`, `../profile-registry.md`, `../radiant-security.md`

## Context

`ant_core` is a superset, not a clone. Beyond byte-exact ANT+ compatibility it
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
- **The on-air length byte already expresses payload size.** `0x0A` = 8 payload
  + 2 CRC. It is a field, not a constant, and the nRF radio's `PCNF0.CRCINC`
  exists for exactly this ShockBurst-style arrangement. A frame with a longer
  payload is expressible today with no new framing and no new network — and a
  falsifiable prediction of that reading is that an advanced-burst frame should
  carry `0x1A`.
- **A stock ANT receiver's `MAXLEN` rejects an over-long frame harmlessly.** It
  does not confuse the receiver; it fails length validation and is dropped.
- **A receiver never opens a channel for a device type it does not recognise.**
  Device type is 7 bits (the MSB is the pairing bit), so 1–127, with a
  substantial unallocated range. New device types are invisible by
  construction.
- **New pages within an existing device type are zero-risk.** Receivers skip
  page numbers they do not know.
- **The scheduler's cost model is windows, not bytes.** All ANT+ traffic being
  on RF 57 is what allows overlapping tracked RX windows to be *merged* into a
  single hardware window with up to 8 filters. That merge is the highest-value
  item in the scheduler: it is why 32 tracked sensors do not cost 32 windows,
  and why slave-side collisions between tracked channels drop to zero.
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

The five axes, in increasing order of compatibility risk:

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
3. **Longer frames via the length byte.** No new network, no new framing.
   Reserved for profiles that genuinely cannot fit 8 bytes, because it is not
   free: a longer frame lengthens the RX window for every channel merged into
   it.
4. **A long-range PHY, per device type, not in v1.** A lower-rate GFSK variant
   on the same band, opted into per device type and marked as such in the
   registry. ANT+ compatibility channels never change PHY, so this axis cannot
   touch a standard device by construction. The HAL accommodates it already —
   PHY is a backend/caps property, so `ant_radio_caps` grows a supported-PHY
   list rather than needing a redesign.
5. **Adaptive frequency, per device type.** The descriptor announces the node's
   RF channel; discovery and pairing stay on RF 57 so a searching receiver
   still finds every node; data lives wherever the descriptor says; and an
   in-band announcement ("next epoch on RF N") lets a deployed pair move away
   from fresh interference. Standard channels never move and only RadiANT
   receivers follow.

### Why not a second network

**Because it buys exactly one thing, and axis 5 already provides that thing
without the cost.**

Enumerate what a second network would actually deliver over extending in place:

- *Longer payloads?* No — the length byte already carries them (axis 3).
- *New device types and pages?* No — the existing type and page space has room,
  and unknown types are ignored by construction (axes 1 and 2).
- *Isolation from stock receivers?* No, and this is the one people expect to be
  yes. Unknown device types are already invisible; over-long frames are already
  rejected on `MAXLEN`. There is nothing left for a separate address to protect
  against.
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
- Over-long frames must be verified against real stock receivers rather than
  reasoned about — `MAXLEN` handling is the case most likely to upset one, and
  it is the specific case the Tier 4 capture repeats.
- An adaptive-frequency node costs its own RX window, the same as a
  second-network channel would. The saving is that only nodes that opted in pay
  it.
- Sharing RF 57 means RadiANT traffic contends with ANT+ traffic for airtime.
  At 4 Hz with ~400 µs windows, 32 channels is ~19% duty — comfortable — but it
  is not free, and profile design should still choose the slowest period the
  data tolerates rather than treating airtime as unlimited.
