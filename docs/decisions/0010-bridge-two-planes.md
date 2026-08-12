# 0010 — The bridge is two planes: MQTT for values, Matter for semantics

Date: 2026-08-11
Status: accepted

## Context

The motivating case for the whole telemetry envelope
(`docs/radiant-telemetry.md` section 1) is a heart-rate strap driving a smart
home. The receiver is an nRF52840 that already speaks ANT; the target is a
Home Assistant / Thread network with a border router the user already owns.

**Matter has no heart-rate cluster.** There is no wearable device type, no
vitals cluster, and no attribute whose units are bpm. Section 7 of the envelope
records this as an honest gap and stops there, leaving the shape of the bridge
undecided. This ADR decides it.

Three options were considered.

1. **Matter only.** Carry raw bpm in a manufacturer-extension (MEI) cluster.
   One protocol, no broker.
2. **MQTT over Thread only.** Carry everything as MQTT topics with Home
   Assistant's MQTT Discovery creating the entities. No Matter stack at all.
3. **Two planes.** MQTT carries the values; Matter carries the derived
   semantics and the actuator control.

## Decision

**Option 3, with MQTT as the normative path for values.**

- The **data plane** is MQTT over Thread. Raw bpm, beat accumulators, RSSI and
  battery are MQTT topics, discovered by Home Assistant through retained
  discovery messages.
- The **control plane** is Matter. It carries the *derived* booleans —
  "elevated", "worn" — on standard clusters, and it is how the bridge actuates,
  either through a hub or through a direct client binding.
- Raw bpm is **also** published on an MEI cluster, so a no-broker deployment is
  possible. That path is explicitly secondary.
- Matter is implemented **even in an MQTT-only build**, because Matter's
  BLE→Thread commissioning is how the device obtains the Thread operational
  dataset.

## Amended 2026-08-11 — normative for values, but not the default first run

The decision above is unchanged. What it did not say, and should have, is that
**"normative" was a statement about fidelity and was read as one about
priority.** Two findings force the distinction:

1. **Thread carries no device model.** A Thread node is invisible to Home
   Assistant without Matter — HA surfaces only discoveries it has an integration
   manifest for, so there is no "unknown device" inbox a user could ignore.
2. **Matter commissioning is the only household-viable way onto the Thread
   network** the MQTT plane runs over. The reason above called this a reason to
   keep Matter; it is stronger than that.

So the tiers are: **tier 1, Matter booleans — one QR scan, nothing installed,
and the headline automation already works. Tier 2, MQTT values — one first-party
add-on plus credential provisioning, and the number arrives in bpm.**

MQTT remains normative for values. Matter is normative for first run. These were
never in tension; the document simply led with the wrong one, and the phasing in
`docs/radiant-bridge.md` section 12 inherited an ordering that could not have
been built in the stated sequence.

Two smaller consequences recorded with it: **MQTT-SN is rejected** — it needs a
gateway daemon to reach any broker, and nothing in Thread, a border router or HA
performs that translation, so plain MQTT over TCP is used instead. And a
**Matterless MQTT build** exists via out-of-band Thread provisioning; it is the
small-flash escape hatch, not the entry product, because it forfeits tier 1
entirely.

## Consequences

### Why MQTT is normative rather than the fallback

A custom Matter cluster is invisible to every controller until that
controller's code learns it. For Home Assistant that means a change to
`python-matter-server` or a custom component — someone else's repository, on
someone else's schedule. "The heart rate is in Matter, you just cannot see it"
is not a shippable feature, so the path that works today is the normative one.

MQTT Discovery, by contrast, creates a fully-formed entity from a retained JSON
message with no code anywhere.

### The analogy becomes the implementation

`docs/radiant-telemetry.md` section 1 frames the envelope as *MQTT with the
broker deleted*. Option 3 makes the bridge the place where the broker is put
back, and the mapping is close to a transcription: field ID to topic,
descriptor page to retained discovery message, accumulating field to
`state_class: total_increasing`. That the analogy holds this far is evidence the
envelope was designed right, and it is a cost saved rather than an aesthetic
point.

### What this costs

- **Two stacks on one chip.** Flash on nRF52840 is tight enough that
  `docs/radiant-bridge.md` section 11 splits the SKUs: nRF52840 for the
  MQTT-only build, nRF54L15 or nRF5340 for the full Matter build.
- **A broker in the household path.** A Home Assistant user who has not
  installed Mosquitto has to. Mitigated by the Matter plane keeping the
  automation alive without it, and by the MEI cluster existing.
- **A device type that is a deliberate lie.** The Matter endpoint carrying the
  derived boolean declares Contact Sensor or Occupancy Sensor because there is
  no honest option. Recorded in `docs/radiant-bridge.md` section 8.2 as a known
  cost, revisitable by recommissioning if the Device Library ever grows a
  vitals type.

### What it does not change

**Nothing goes over the air.** `docs/radiant-telemetry.md` section 2's
prohibition on Matter or Zigbee semantics in an ANT frame is untouched and
absolute. See ADR-adjacent amendment in that document's rule 3: the invariant
is "never on the air", and the clause that said "on the host or the gateway" was
describing where v1 happened to run, not stating a rule. An on-device bridge
sits in the same place in the logical stack and adds no byte to any frame.

### Rejected: Matter only

Rejected because raw heart rate would be unreachable from Home Assistant on
day one, and the timeline for it becoming reachable is not ours.

### Rejected: MQTT only

Rejected for one reason that is not about data at all: **commissioning**.
Thread's native joiner flow is not something to ask a household to drive, and
Matter's QR-code commissioning is. A build that dropped Matter entirely would
have to invent its own provisioning path for the network its data plane depends
on.

## Prior art this leans on

Home Assistant's MQTT Discovery is a stable, documented contract that has
carried a large ecosystem of self-describing devices. Adopting it means the
bridge's HA integration is *no integration* — which is the same reasoning that
put the BLE clock-accuracy ladder into the sync-handoff page rather than a
ladder of our own (`docs/radiant-telemetry.md` section 12).

The one part of it we do not own is the discovery schema itself, which Home
Assistant versions and will change. `docs/radiant-bridge.md` section 9.2 keeps
the generator table-driven off the field-type vocabulary so a schema change is a
table edit.
