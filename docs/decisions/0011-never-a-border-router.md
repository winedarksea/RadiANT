# 0011 — The bridge rebroadcasts; it is never a router

Date: 2026-08-11
Status: accepted

## Context

`docs/radiant-bridge.md` puts ANT and a Thread stack on one nRF radio through
MPSL timeslots. The obvious next product is a **2-in-1**: one USB dongle that is
both the ANT receiver and the household's Thread border router. It would be
cheap, it would sell, and it is the first thing anyone asks for.

Section 7.2 of that document appeared to support it. Tracking eight ANT sensors
at ~4 Hz is about 4 % radio duty, which leaves 96 % for Thread and reads as
ample.

That reading is wrong, and the correction is what this ADR records.

## Decision

**The bridge may rebroadcast. It may never route.**

On one radio, the bridge is refused the Thread Router, Leader and Border Router
roles, and the BLE mesh relay and observer roles. It takes a leaf role only:
CSL child by preference, then fast-poll SED, then MED.

A 2-in-1 border router requires **a second 802.15.4 part on the board**. That is
a BOM and layout decision, not a firmware configuration, and it carries its own
constraint: ANT's RF 57 is 2457 MHz, between 802.15.4 channels 21 and 22, so the
Thread radio is pinned away from it (channel 15, 25 or 26) with budgeted antenna
isolation.

The three configurations — dongle, bridge, border-router RCP — are mutually
exclusive and enforced with `BUILD_ASSERT`, in the shape `docs/backends.md`
already uses for `CONFIG_BT` on the direct backend.

## Consequences

### Duty cycle was the wrong metric

An 802.15.4 frame at 250 kbps is slow: 127 bytes is ~4.25 ms on air. An ANT
blackout is ~1.3 ms once the PHY switch is counted — ANT is 1 Mbps GFSK,
802.15.4 is 250 kbps O-QPSK DSSS, so every slot costs a disable, a MODE change
and a re-ramp on both edges.

A frame of duration `F` is clipped if a blackout of duration `B` starts within
`F + B` of its start, so with `N` blackouts per period `P` the loss is
`N (F + B) / P`. At eight tracked sensors that is **18 % of full-size frames on
4 % duty**. The blackout phases come from N independent masters' clocks, so
clustering is guaranteed rather than avoidable.

Derived, not measured. `docs/radiant-bridge.md` section 7.4 is the gate that
settles it.

### The rule that decides a role is blast radius, not radio time

A Thread MED and a Thread Router both keep the receiver nominally on and both
lose the same fraction of frames. The question that separates them is **does
anyone else's radio schedule depend on ours, and who finds out when we miss.**

A leaf's misses are absorbed by MAC retries and TCP: some tail latency, nothing
visible outside the device. A router's misses orphan other people's children,
churn routes and fail SRP registrations for devices that have never heard of
ANT. Identical packet loss, unrelated consequences.

There is an asymmetry worth keeping: MPSL knows the ANT schedule, so a
well-behaved 802.15.4 driver defers **its own** transmissions around the slots.
RX cannot be deferred. So the failure presents as *other devices cannot reach
us* — the worst way for infrastructure to fail, because every symptom points
somewhere else.

### The purpose collision settles it without the radio argument

A border router is routing, discovery proxy, SRP and NAT64. In a USB-dongle form
factor the dongle is an RCP and the border router proper is a daemon on a capable
host. But **if a capable host is present, the ANT data should go to it over
USB** — faster, lossless, already built, no Thread involved. The Thread data
plane of ADR 0010 exists for the *hostless* case.

**Amended 2026-08-11 — this third argument is weaker than first written, and
the decision does not rest on it.** "Send it over USB" is right about transport
and misleading about friction: USB into Home Assistant requires an *integration*
— code somebody writes and maintains — whereas the Thread path uses protocols HA
already speaks. Under a no-custom-code constraint the network path is the
low-friction one **even when a host is present**, which is the opposite of what
this paragraph implies. `docs/radiant-bridge.md` section 1 has the tier table.

Reasons 1 and 2 above are unaffected and are sufficient on their own. This one
is retained because the purpose collision is real — a border router still
requires a capable host, and that host still changes what the best data path
is — but it is a supporting argument, not a load-bearing one.

So the two configurations are mutually exclusive by purpose rather than by
radio: in every deployment where the 2-in-1 could work, half of it is
unnecessary. A separate product-shape argument points the same way — a USB
dongle is mobile by design and a border router must be fixed.

### This supersedes the MED recommendation

`docs/radiant-bridge.md` section 7.3 previously read "Thread role is MED, not
Router", and dismissed SED as "wrong, because it polls and sub-second actuation
is the feature". The role conclusion was right for the wrong reason and the SED
dismissal was wrong outright: a sleepy role is *better* for coexistence, because
it hands us our own receive phases and turns random loss into a schedule we
interleave deliberately. A 200 ms poll period is comfortably sub-second, and
Thread 1.2 CSL does better still.

The fallback chain exists because CSL needs a Thread 1.2+ parent, which a
household cannot be assumed to have.

### What this costs

- **No 2-in-1 SKU on one radio**, which is a real product we are declining to
  ship. Mitigated by the two-dongle deployment, where the sync handoff page
  `0x12` already lets two ANT receivers cooperate.
- **The bridge depends on infrastructure it does not provide.** It needs
  somebody else's border router to exist. That was already the framing of ADR
  0010, and this ADR makes it permanent rather than provisional.

### What it does not cost

Rebroadcast is *contained*, not degraded into uselessness. Outbound publishes
are nearly unaffected, because the bridge chooses when to transmit and MPSL
defers around the ANT slots. The loss lands on the inbound direction, where TCP
retransmits it and the cost is tail latency — invisible for telemetry, a few
hundred milliseconds for an actuator command.

"Safe" is not "free", and it is not "measured" either. Section 7.4 still has to
be run, in the MED role as well as the intended one, because MED is the
always-on worst case.
