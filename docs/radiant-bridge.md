# The RadiANT bridge: ANT to Thread, Matter and BLE

Checked by: nothing — treat as narrative. Every byte layout this document
mentions is owned by [`radiant-telemetry.md`](radiant-telemetry.md) and checked
there; what is *here* is architecture. The one claim in it a machine can settle
is the coexistence gate of section 7, and that is a bench measurement which has
not been taken.

The normative decision this document implements is
[`decisions/0010-bridge-two-planes.md`](decisions/0010-bridge-two-planes.md).
Where the two disagree, the ADR wins.

---

## 1. What this is, and the one decision it turns on

[`radiant-telemetry.md`](radiant-telemetry.md) section 1 names the motivating
case: **a heart-rate strap driving a smart home** — elevated BPM turns on the
AC, resting HR dims the lights. That document specifies how the number gets on
the air. This one specifies what happens after it comes off.

The commercial extension of the same shape is workers or event attendants
wearing ordinary ANT+ straps, with a building responding to the population
rather than to a person. That is section 10, and it is deferred rather than
dropped, because two of its constraints have to be in the design from v1 or they
cannot be added later.

### The decision everything else follows from

Section 7 of the envelope states the gap honestly: **Matter has no heart-rate
cluster.** There is no wearable device type, no vitals cluster, and no
attribute whose units are bpm. That is not a gap in our mapping table; it is a
gap in the Matter Device Library, and it is not ours to close.

The tempting move is to close it anyway with a manufacturer-extension cluster
and declare the problem solved. That is wrong as a *primary* path, because a
custom cluster is invisible to every controller until that controller's code
learns it — which for Home Assistant means a change to `python-matter-server`
or a custom component. "Your heart rate is in Matter, you just cannot see it"
is not a feature.

So the bridge is **two planes over one Thread network**:

| Plane | Carries | Transport | Consumer |
|---|---|---|---|
| **Data** | the numbers: bpm, beat accumulator, RR interval, RSSI, battery | MQTT over Thread | Home Assistant via MQTT Discovery — no custom code |
| **Control** | the *derived* semantics: elevated, worn, cohort state — plus the actuator commands | Matter | HA / Apple / Google, or a direct device binding with no hub in the path |

**The split is about what Matter can express, not about numbers versus
booleans.** Where Matter *does* have the cluster — temperature and humidity,
`0x10` and `0x11` — the number takes the Matter plane too, at tier 1, correctly
labelled. That is not an exception to the rule above so much as the rule stated
precisely: heart rate travels MQTT because Matter has nowhere to put it, and the
moment a quantity has somewhere to go, it goes there. Section 8.1a is the
mechanism and section 8.1 is the table.

This is not a workaround. Matter is a **device** model: it wants to be told
"this endpoint is a thermostat". Heart rate is not a device, it is a
measurement stream, and MQTT is the model that fits a measurement stream. The
envelope's own opening analogy — *MQTT with the broker deleted* — makes the
bridge the exact place where the broker gets put back:

| Envelope | MQTT |
|---|---|
| field ID | topic suffix |
| descriptor page, re-broadcast every 121 messages | retained discovery message |
| accumulating field | a monotone counter with `state_class: total_increasing` |
| `0x60` node, self-describing | a discovered device, self-describing |

The descriptor **is** the discovery message. Section 9 is mostly a transcription.

### Thread has no device model, and this is the most load-bearing fact here

**Thread is IPv6 over 802.15.4 plus mesh routing. That is the whole of it.** It
is the peer of Wi-Fi and Ethernet, not of Zigbee. Joining a Thread network
yields an IPv6 address and a route — no device identity, no capability
description, no registry, nothing that could "appear" in a controller.

| Layer | Thread | Matter |
|---|---|---|
| What it is | network: IPv6, mesh routing, 802.15.4 | application: device types, clusters, attributes, commissioning |
| What joining gets you | an address | an entry in the controller's device registry |
| Does Home Assistant list it? | **no** — HA's Thread panel shows border routers and datasets, not nodes | **yes**, and this is the only thing that does |

Two consequences that between them decide the whole product shape:

1. **A Thread node is invisible without Matter.** Home Assistant surfaces only
   discoveries it has an integration manifest for, so a device advertising a
   service HA does not recognise is not an "unknown device" a user could choose
   to ignore — there is no inbox. It is simply absent.
2. **You cannot join a Thread network unaddressed.** Thread is
   cryptographically closed: a node needs the operational dataset — network key,
   channel, PAN ID — before it may transmit at all. There is no "permit join"
   that admits an unknown node. Thread is closer to Wi-Fi than to Zigbee.

**SRP is the part that genuinely is automatic, and it is findability rather than
identity.** A Thread 1.2+ node registers its services with the border router's
SRP server, which republishes them as DNS-SD on the LAN. That is how a Matter
device announces itself to a commissioner. It makes a node *findable*; only
Matter makes it *meaningful*.

### So Matter commissioning is mandatory, not an optimisation

Of the three ways onto a Thread network — Matter commissioning, Thread's native
joiner flow, and out-of-band provisioning — only the first is something a
household will complete, and Home Assistant exposes no UI for commissioning
non-Matter devices at all. **A build whose data plane is entirely MQTT still
needs the Matter stack, because Matter commissioning is how it reaches the
network its MQTT runs over.**

This used to be written as "Matter earns its flash", which was too weak. It is
not an argument for keeping Matter; it is the reason Matter is load-bearing
rather than additive.

**One escape exists and it is deliberate.** Out-of-band provisioning — the
WebSerial page of section 9.0a writing the Thread operational dataset directly,
which Home Assistant will export from its Thread panel — produces a **Matterless
MQTT build**. That build is not household-friendly: it trades a QR scan for a
technical setup step, and it forfeits tier 1 entirely, since without Matter
there is nothing for Home Assistant to list. What it buys is a very large amount
of flash, which is what makes it the nRF52840's escape hatch in section 11.
Named here so that "Matter is mandatory" is understood as a statement about
product experience rather than about physics.

### The two tiers, and the first one is the default

The plane split above is about *fidelity*. The tier split is about *friction*,
and it is what a user actually experiences:

| Tier | User does | What appears | Needs |
|---|---|---|---|
| **1 — Matter** | scan the QR code | four `binary_sensor`s: *heart rate monitor worn*, *bike in use*, *at rest*, *training zone 2 or above* — plus a correctly-labelled temperature or humidity `sensor` for any bound source that reports one (§8.1a) | **nothing further.** Ships with HA |
| **2 — MQTT values** | tier 1, then install the Mosquitto add-on and provision credentials | `sensor`, bpm, correct units, device registry, history | one first-party add-on |

**Tier 1 is the out-of-box experience and the headline feature works entirely
inside it.** One QR scan and those four are in Home Assistant with nothing
installed — enough to drive the fan or the AC, which is the whole motivating
case of `radiant-telemetry.md` section 1. Tier 2 is the opt-in "I want the
number in bpm" step — **bpm specifically, not numbers in general**, because the
numbers Matter can carry are already in tier 1.

Two of the four are **correct by construction** — they ask only whether an
accumulator is advancing, so they need no calibration and cannot be wrong about
a stranger. The other two use a standard-human prior that personalises itself if
storage allows. Section 6 is the whole of it, and section 8.1 is the endpoint
model.

That ordering is a change of emphasis, not of architecture.
[`decisions/0010-bridge-two-planes.md`](decisions/0010-bridge-two-planes.md)
makes MQTT normative **for values** and that is unchanged; what this adds is
that Matter is normative **for first run**, and the two were never in tension.
Survivability points the same way: the derived booleans reach an automation with
the broker down, and with the hub down entirely if client binding (section 8.4)
is in use.

---

## 2. The structural rule this inherits, and how it was amended

[`radiant-telemetry.md`](radiant-telemetry.md) section 2 rule 3 used to read
"the Matter/Zigbee bridge runs **on the host or the gateway**, never over the
air." That conflated two separate things, and this document is why the
distinction now matters:

- **"Never over the air" is the invariant, and it is unchanged.** A Matter
  cluster ID, an attribute ID, a ZCL identifier or an MQTT topic string never
  appears in an ANT frame. A RadiANT field ID does, and something translates.
  This is what protects a Garmin head unit, and it is absolute.
- **"On the host or the gateway" was never the invariant.** It was a statement
  about where the translation happened to run in v1. A combo node running
  `radiant` + OpenThread on one chip translates in exactly the same place
  in the *logical* stack — above the profile decoder, below the network — and
  the fact that it is the same die changes nothing on the air.

The amended rule 3 separates them. Everything in this document sits strictly
above the profile decoder, and nothing in it may add a byte to an ANT frame.

---

## 3. The sample bus

**Every radio input decodes into the section 7 vocabulary before anything else
sees it.** This is the keystone, and it is what makes each new sink cost one
file instead of one file per profile per sink.

```
ANT+ 0x78 heart rate ──┐                              ┌── USB / ANT serial
ANT+ 0x0B, 0x11, ...  ─┤                              ├── MQTT over Thread
RadiANT 0x60          ─┼──► sample bus ──► rules ──►  ├── Matter
  (descriptor-driven)  │                     │        ├── BLE
derived / rule output ─┘◄────────────────────┘        └── local GPIO
```

The record is what the envelope already defines, made concrete:

```c
struct radiant_sample {
	uint32_t source;      /* binding index — NOT a device number; see §5 */
	uint8_t  field_id;    /* stable within source */
	uint8_t  field_type;  /* the §7 vocabulary: 0x26 heart rate, 0x36 count */
	uint8_t  flags;       /* ACCUMULATING | DERIVED | STALE */
	int8_t   exp;         /* value_SI = raw * 10^exp, in the type's unit */
	int64_t  raw;
	uint64_t t_us;        /* the t_sync that produced it, not the queue time */
};
```

Three properties are load-bearing.

### 3.1 ANT+ profiles are vocabulary producers too

Section 7's claim that the Matter mapping is "mechanical rather than bespoke"
currently holds only for RadiANT device types, because only those carry a
descriptor. Routing the ANT+ compatibility profiles through the same vocabulary
is the single change that extends the claim to the profiles people actually
own, and it costs one adapter per profile — not one per profile per sink.

**Environmental fields ride the same mechanism and are the shortest adapters in
the set.** An ANT+ Environment `0x19` page 1 is one `0x10` temperature record
plus a `0x36` event count (`radiant_env_adapter.c`); common page 84 — legal in
any profile, which is what makes it the interesting one — is up to two records
per message, drawn from `0x10` temperature, `0x11` humidity, `0x12` pressure,
`0x16` speed, `0x18` angle and `0x36` event count
(`radiant_common_adapter.c`). Both are now implemented and both are derived from
their primary documents; `device-profiles.md` §3.7 and §3.4 carry the layouts
and the one limitation that remains, which is that no environmental sensor is on
the bench and both are vector-tested rather than hardware-verified. Section 8.1a
is where that status is set against what the Matter side does with the types.

An ANT+ heart rate page 0 becomes three bus records, not one:

| ANT+ field | Vocabulary type | Kind |
|---|---|---|
| computed heart rate (byte 7) | `0x26` heart rate, bpm | instantaneous |
| heart beat count (byte 6) | `0x36` event count | **accumulating**, u8, wraps |
| heart beat event time (bytes 4–5) | `0x37` duration, s | **accumulating**, u16, 1/1024 s |

That third row is the interesting one, and it exposes a real trap.

### 3.2 The 1/1024 s trap, stated because it will otherwise be found twice

The vocabulary's scale is **decimal** — `value_SI = raw * 10^exp` — and ANT+
event times are in **1/1024 s**. There is no integer `exp` that expresses
1/1024. So an adapter cannot simply relabel the ANT+ accumulator and hand it
over; it has to convert, and converting an accumulator is where the mistake
lives.

The wrong construction is to convert each reading and then difference:
`(raw_t * 1000 / 1024) - (raw_prev * 1000 / 1024)` accumulates a truncation
error per event and drifts without bound.

The right one is to difference in the field's own width first — exactly as
section 5 of the envelope requires and `ant_pages.delta_u16()` already does —
accumulate exactly, and convert only at publication:

```
acc_1024 += delta_u16(t, t_prev);            /* exact, u64, never converted */
sample.raw = (acc_1024 * 1000) / 1024;       /* computed fresh each time */
sample.exp = -3;                             /* milliseconds */
```

The error is then bounded at one millisecond *total*, forever, instead of one
truncation *per beat*. Any adapter converting a non-decimal ANT+ unit into the
vocabulary follows this shape, and the ones that need it today are event time
(1/1024 s), and the 1/2048 s variants some profiles use.

**The vocabulary has no instantaneous time-interval type.** RR interval — the
per-beat spacing that HRV is computed from — is an instantaneous duration, and
class `0x10-0x2F` has no entry for it; `0x37` is in the accumulating class and
section 7 requires the accumulate bit there. A type such as `0x27` time
interval, canonical unit s, would close it. **It is proposed here, not
allocated**: the vocabulary table in `radiant-telemetry.md` is mirrored in
`tools/ant_pages.py`, and a type allocated in one and not the other is exactly
the drift the mirror exists to prevent. Allocate both together or neither.

### 3.3 The bus is a queue, and it drops rather than blocks

**No sink ever runs in the radio callback.** A Thread retransmission, a TLS
handshake or a flash write that delays an ANT receive window turns into packet
loss on a link whose real floor is around 0.4 % — a number this bench has
characterised precisely enough that a regression would be visible and
misattributed.

So the decoder writes into a ring buffer and returns. A bridge work queue
drains it. The buffer **drops oldest and counts the drops**, and the drop
counter is itself published as a `0x36` event count on a reserved
diagnostics binding. A sink that cannot keep up must be visible as a number,
not as a gap.

`t_us` is captured at `t_sync`, not at dequeue, so a sink can still report when
a sample was *heard* however long it waited.

---

## 4. Sinks

A sink is a registration, not a case in a switch:

```c
struct radiant_sink {
	const char *name;
	bool (*want)(const struct radiant_sample *s);
	void (*publish)(const struct radiant_sample *s);
	void (*binding_changed)(uint32_t source, const struct radiant_binding *b);
};

RADIANT_SINK_DEFINE(mqtt_sink, ...);   /* STRUCT_SECTION_ITERABLE under the hood */
```

This is the same shape `profile_sched.c` already uses for its client seam — a
registered callback getting first refusal — and the consistency is deliberate:
one plugin idiom in the tree, not two.

Consequences worth naming:

- **Adding the BLE sink touches zero lines of ANT code.** That is the test of
  whether the seam is in the right place.
- **A build without Thread does not link the MQTT sink.** Kconfig selects the
  sink set per image, and an image that selects no sink but USB is exactly
  today's dongle firmware with a bus in the middle — which is why phase 1 of
  section 12 is a pure refactor with no radio risk.
- **`binding_changed` exists so a sink can be declarative.** The MQTT sink
  publishes its retained discovery message from it; the Matter sink adds or
  removes an endpoint. Neither polls.

---

## 5. Binding and identity

**Nothing downstream may key on an ANT device number.** It is 16 bits, it is
re-rollable by design (identity Tier 0, `docs/radiant-security.md` section 4),
and an MQTT topic or a Matter endpoint has to survive a battery change and a
re-pair. Worse, a device number collision across two straps in the same room is
a one-in-65535 event that will happen at a trade show.

The binding table is the pivot, and it **is specified to be persisted and is
not yet**:

> **Corrected 2026-08-15.** This sentence read "and it is persisted", and the
> code has never done that. `radiant/src/bridge/radiant_binding.h` says so in as
> many words — "RAM-backed for now" — and the code is the truth. The consequence
> is concrete and it is the reason this correction is a paragraph rather than a
> word: **every reboot loses all bindings, and re-pairing mints a new `uuid`**,
> hence new MQTT topics and a fresh set of Home Assistant entities. A dashboard
> accumulates orphans on every power cut. For a bench that is a nuisance; for
> the gym deployment of `docs/treadmill-reference-design.md` §8 it is a
> prerequisite rather than a nicety, because the whole point of the `uuid` is
> that it survives exactly this.

```
binding[i] = {
	devnum, devtype, trans_type,   /* what to track */
	label,                         /* what a human calls it */
	uuid,                          /* stable across a re-roll and a re-pair */
	policy,                        /* thresholds, publish intervals, consent */
}
```

- `i` is what the bus carries as `source`. Every topic, endpoint and rule hangs
  off it, and none of them ever see a device number.
- `uuid` is generated at first bind and **survives a re-pair of the same
  physical strap**, which is how a HA entity keeps its history when the user
  replaces a CR2032 and the strap re-rolls. Matching a re-paired strap back to
  its `uuid` is a user action, not an inference — the bridge offers the last
  binding with the same label and lets a human confirm.
- Table capacity sizes the Matter endpoint count, the ANT tracked-channel count
  and the coexistence budget of section 7 **together**. Eight for the household
  SKU. Raising it is a radio decision before it is a memory decision.

### Binding is explicit and opt-in, always

A bridge never ingests a sensor it was not told to. This is stated here as an
architecture rule rather than a privacy footnote because it is also what makes
the radio budget hold: promiscuous ingest means continuous scan, and section 7
shows continuous scan is the one mode that cannot coexist with Thread.

The privacy rule and the radio rule are the same rule, which is a good sign.

---

## 6. The rule evaluator

Ship the raw number *and* a derived boolean, and evaluate the boolean **on the
dongle**. Three reasons:

1. Matter has no other way to carry the signal at all (section 1).
2. The automation keeps working when the hub reboots, the broker restarts, or
   Wi-Fi drops. "The AC did not come on because Home Assistant was updating" is
   the failure this design is supposed to make impossible.
3. The commercial case needs aggregation to happen *before* the data leaves the
   device, which means the aggregating step cannot live in the hub.

**Derived outputs re-enter the bus** as ordinary `struct radiant_sample`
records with `DERIVED` set, so they reach every sink through the same path and
no sink needs to know a rule engine exists. All four outputs below are type
`0x02` occupancy.

### 6.1 Activity: is an accumulator advancing?

The two activity outputs are one rule applied twice, not two rules:

> **A sensor is *active* when its accumulating field is advancing.**

| Output | Accumulator | Active means |
|---|---|---|
| heart-rate monitor worn | `0x36` beat count | beats are arriving — worn, and reading a pulse |
| bike in use | `0x30` energy | work is being done — pedalling |

Cadence (`0x35`) and speed (`0x34`) fall out of the same rule for free and are
**not** enabled in v1: power and heart rate are the deployment, and an output
nobody consumes is an endpoint to maintain.

**Use the accumulator, never the instantaneous value.** "Instantaneous power
> 0" seems equivalent and is not: instantaneous power is zero every time the
rider coasts, so that boolean drops out at every descent and every traffic
light. `delta(acc_energy) > 0` over a window does not. This is
`radiant-telemetry.md` section 5's "the accumulator is authoritative" earning
itself a third time.

**The dwell is asymmetric**: ~2 s to assert, ~20 s to clear. Many power meters
and trainers keep transmitting zeros for some time after the rider stops, and a
symmetric dwell either chatters on the leading edge or lags badly on the
trailing one.

These two are **correct by construction**. They need no calibration, no prior
and no knowledge of the person, which is why they are what tier 1 ships
(section 1) and why they are the defaults rather than any intensity threshold.

Liveness follows the envelope's own rule: a heartbeat interval exists precisely
so that "no data" is never silent (`radiant-telemetry.md` section 8). A sink
publishes `STALE` after `3 x` the expected interval, which clears the activity
output and — because 6.2's zone outputs are gated on it — clears those too.

### 6.2 Zones: a standard-human prior, optionally personalised

The two zone outputs answer "is this person working hard, or clearly resting?"

| Output | Occupied when |
|---|---|
| at rest | monitor worn **and** HR below the rest threshold |
| training, zone 2 or above | monitor worn **and** HR at or above the Z2 threshold |

**Both require the monitor to be worn.** A strap on a table reads no pulse, and
a bridge that reported "at rest" for it would switch the air conditioning off in
the middle of a workout. That is the single most likely real-world failure of
the headline feature and it costs one `&&`.

Thresholds come from Karvonen (heart-rate reserve) rather than percent-of-max,
because the estimator produces both endpoints anyway and HRR is the less
sensitive of the two models to differences in resting heart rate:

```
HRR       = HR_max - HR_rest
rest      < HR_rest + 0.15 x HRR
zone 2+   >= HR_rest + 0.60 x HRR
```

With the population prior for an adult of unknown age and fitness —
`HR_max ~ N(180, 15^2)`, `HR_rest ~ N(65, 10^2)` — that is **HRR = 115, rest
below 82 bpm, zone 2 and above at 134 bpm**. Those two numbers are what a
never-personalised bridge ships with, and they are good enough to separate a
hard workout from clear rest, which is the whole use case. Nothing here claims
to be a training tool.

`hysteresis` and `dwell_s` still apply and are not polish: a 4 Hz stream
crossing a bare threshold produces a boolean that chatters several times a
second, and a Matter report or an MQTT publish per chatter is how a bridge
becomes the noisiest device on the network.

**There is no confidence gate.** An earlier draft proposed withholding the
personalised thresholds until an observation count cleared a floor. It is not
worth the state or the branch: the prior alone already separates heavy work from
rest, so a half-converged posterior is never worse than the thing it replaces,
and the failure mode a gate would prevent — a wrong threshold flipping a
boolean — is already handled by hysteresis and dwell.

### 6.3 The optional personalisation, and why it is 76 bytes

A per-binding histogram of observed non-zero heart rates, from which the two
endpoints are estimated:

| Property | Value |
|---|---|
| Range | 30–220 bpm |
| Bin width | 5 bpm → **38 bins** |
| Counter | `uint16`, saturating |
| **Cost** | **76 bytes per personalised binding** |

```
HR_max estimate  = p99(histogram)
HR_rest estimate = p01(histogram)
```

each folded into the prior by a conjugate normal update — two scalars per
parameter, no matrix, no floating point required:

```
posterior = (prior/prior_var + obs/obs_var) / (1/prior_var + 1/obs_var)
```

Quantile interpolation within a bin gives sub-5-bpm threshold resolution, so the
coarse bins do not quantise the answer.

**Forgetting is free.** When any bin reaches its ceiling, halve every bin. That
is exponential decay with no extra state and one pass over 76 bytes. At ~1 Hz
decimated sampling a `uint16` saturates after ~18 hours in a single bin, which
puts the forgetting timescale at weeks-to-months of sessions — about right for
tracking real fitness change, and slow enough that a single bad session does not
move anything.

**A histogram rather than a running min/max, and the reason is artifacts.**
Chest straps produce spurious readings routinely: dry electrodes at the start of
a session, EMI, and cadence-lock where the reading latches onto pedalling rate.
One 240 bpm artifact destroys a running maximum permanently; a p99 does not
notice it. This is a failure that would otherwise be found months after
shipping.

**Prior-only is `n = 0` of this same path, not a second implementation.** That
is the whole reason the estimator is shaped this way. It means the output
semantics never change, no Matter endpoint moves when personalisation is
enabled, cold start needs no special case, and — see below — running out of
storage is a graceful degradation rather than an error.

### 6.4 The histogram store is capped, and eviction is not a failure

**A gym may see thousands of heart-rate monitors.** The binding table is capped
and opt-in (section 5), so thousands of *bound* sensors is not the concern; the
concern is that any deployment which raises that cap would otherwise raise the
histogram store with it, at 76 bytes a time, with no ceiling stated anywhere.

So the store is capped independently:

- `CONFIG_RADIANT_BRIDGE_HR_PROFILES` slots, default **8** — 608 bytes.
- Allocation is **least-recently-active eviction**. A binding that has not been
  active longest gives up its slot.
- A binding with no slot uses the prior. It does not fail, does not error and
  does not change any endpoint's meaning.
- **Zero slots is a legal configuration**, and it is the default in
  aggregate-only builds (section 10), where the bridge is deliberately not
  modelling individuals at all.

That graceful path exists only because 6.3 made prior-only the `n = 0` case. A
design with two implementations would have had to choose between a hard cap and
an allocation failure.

### 6.5 Persistence, and what it is keyed to

The histogram is worthless if it does not survive a reboot, so it reaches NVM —
but **written on session end**, when the activity output of 6.1 clears, not
continuously. That is a few writes a day rather than thousands, which is what
keeps it kind to the part.

It keys on `binding_uuid` (section 5) and **never** on the ANT device number, or
a coin-cell change discards months of convergence. Two people sharing one strap
corrupt each other's estimate; the answer is a per-binding reset, not an
inference.

### 6.6 One honest bias, recorded rather than corrected

**A chest strap is worn during exercise, not all day.** The observed
distribution is therefore truncated, and `p01` is not resting heart rate — it is
the lowest heart rate seen during a warm-up. It will read high, plausibly by
10–20 bpm.

This is accepted rather than corrected. Sampling the low quantile only from the
first 60 s of a session would reduce it and would add session-boundary state to
do so. What the biased estimate actually produces is "calm, for this person,
during a session", which is what an automation wants from a boolean called *at
rest* anyway. The bias is one-directional and known, which makes it a
documented property rather than an error.

### Publish pacing

Never at the rate the strap transmits. Every sink obeys a
change-or-heartbeat contract — `publish if (changed by more than deadband) or
(max_interval elapsed)`, floored by `min_interval`. ~4 Hz into MQTT is 345,000
messages a day per strap, and Matter's attribute reporting has its own
minimum-interval negotiation that a 4 Hz writer simply violates.

---

## 7. Radio coexistence — the constraint that decides whether this exists

This section is the go/no-go, and it is independent of everything above it.

### 7.1 The MPSL backend is a hard prerequisite

On nRF, OpenThread's `nrf_802154` driver and `radiant_radio_nrf.c` both want
RADIO exclusively. That is the same collision [`backends.md`](backends.md)
already guards for Bluetooth with `BUILD_ASSERT(!IS_ENABLED(CONFIG_BT))`, and
802.15.4 is not different in kind. **There is no shortcut**: a Thread-capable
bridge builds on `radiant_radio_nrf_mpsl.c`, which is Phase 6b.

`backends.md` should carry the matching build assert for
`CONFIG_NET_L2_OPENTHREAD` on the direct backend, for the same reason it
carries the one for `CONFIG_BT` — two stacks each believing they own RADIO do
not announce themselves, they just lose packets.

### 7.2 The insight that makes single-chip survivable

The instinct is that an always-on ANT receiver plus an always-on Thread device
cannot share one radio. The arithmetic says otherwise, and the distinction is
not between ANT and Thread — it is between **scanning** and **tracking**.

A tracked ANT slot is a narrow window:

```
window = 2 x guard  +  frame airtime  +  ramp-up
       = 2 x 400 us +  ~150 us        +  ~40 us    ~=  1.0 ms
```

`RADIANT_CHANNEL_GUARD_MAX_US` is 400 µs (`radiant_channel.h`), and that is the
*widest* the guard ever gets — the residual estimator narrows it on a
well-behaved link. ~150 µs is the 8-byte airtime at 1 M that
`radiant-telemetry.md` section 6 already uses to budget a coding rate. So, at
the ~4 Hz ANT+ profiles run (8182 counts, ~250 ms, the one period this project
records because it is the one it has implemented):

| Mode | Radio duty | Coexists with Thread? |
|---|---|---|
| Wildcard **scan**, 32-set sweep | ~100 % | **No.** This is the one that kills it |
| **Tracking** 8 sensors at ~4 Hz | 8 x 1.0 ms / 250 ms ≈ **3.2 %** | Comfortably |
| Tracking 16 sensors | ≈ 6.4 % | Comfortably |

The arithmetic above is derived from compiled-in constants, **not measured**.
Section 7.4 is the measurement.

**Read that table with section 7.3 next to it.** Aggregate duty is the wrong
metric on its own, and taken alone it makes coexistence look far more
comfortable than it is.

So the architectural rule was:

> **Scanning is a bounded pairing state, not a running state.** Once a sensor is
> bound the bridge tracks it, and gives ~95 % of the radio back to Thread.

Enforced, not merely intended: the sink layer refuses to arm a wildcard sweep
while a Thread session is up, except inside a user-initiated pairing window
with a hard timeout. That is the same rule as "binding is opt-in" from section
5, arrived at from the other direction.

> ### ⚠ AMENDED BY ADR 0013 FOR THE COMBINED USB + THREAD BUILD
>
> **The rule above does not survive contact with Zwift, and it is kept here
> rather than rewritten because the premise it rests on is the interesting
> part.**
>
> It rests on scanning being a state a *user* enters. The USBPcap capture
> analysed on this project shows that is false for the hostless bridge's
> nearest sibling: **Zwift tears down and rebuilds a background-scan channel
> every 4–5 s, cycling networks 0/1/2, for the entire session.** A combined USB
> + Thread dongle therefore has a permanent scanner in it from the moment the
> ride starts, put there by the host rather than by the user — and under the
> rule above, Matter would be dead for the whole ride.
>
> The amendment inverts which side is elastic:
>
> - **A tracked slot is inviolate.** Tracked RX, master TX, and the
>   acknowledged-data reply that may follow either are reserved as a unit and
>   are never shortened or displaced by anything `radiant` decides.
> - **The second stack's slice comes out of the sweep**, which yields when the
>   arbiter says so and takes the whole cost of coexistence.
> - **The yield point is discovered, not configured.** A scan chunk is granted a
>   short timeslot and extended repeatedly; MPSL grants an extension only when
>   it has nothing else scheduled in the extended region. A fixed slice yields
>   when it need not and overruns when it must not.
>
> The rule as written above **still stands for the hostless bridge of this
> document**, which has no host putting a scanner in it. It is the *combined*
> build that needs the amendment. ADR 0013 has the full reasoning, including
> the two measured dead ends it fences.

### 7.3 Two consequences that have to be said out loud

**Sparse RadiANT nodes cannot be bridged by a combo device.**
`radiant-telemetry.md` section 8 is explicit that a sparse node *requires* a
scanning receiver, and section 7.2 above is that a continuous scanner has no
radio left for Thread. These are both true, so a sparse node and Thread cannot
share one chip today. A bridge that reads the sparse flag must refuse the node
and say so — which is section 8's existing rule, reached for a new reason.

The escape is already specified and not yet built: the **sync handoff page
`0x12`** lets a second, ANT-only receiver hand over every parameter, so the
bridge never sweeps at all. That turns sparse-plus-Thread from impossible into
a two-box deployment, and it is the strongest argument yet for building the
handoff sender.

### 7.3a Duty cycle is the wrong metric: blackout rate against frame duration

An 802.15.4 frame is slow. Max PSDU is 127 bytes at 250 kbps — **~4.25 ms on
air** with preamble and SFD. An ANT blackout is the ~1.0 ms window of section
7.2 plus a PHY switch on each edge: ANT is 1 Mbps GFSK and 802.15.4 is 250 kbps
O-QPSK DSSS, so every slot is a disable, a MODE change and a re-ramp on both
sides. Call it **~1.3 ms**, which is `phy_switch_us` in the HAL capability
table earning its place.

A frame of duration `F` is clipped if any blackout of duration `B` begins within
`F + B` of its start. With `N` blackouts per period `P`:

```
P(clipped) = N x (F + B) / P
```

| Tracked sensors | Aggregate duty | Max-size 15.4 frame clipped | ~40-byte frame clipped |
|---|---|---|---|
| 8 | ~4 % | **18 %** | 9 % |
| 16 | ~8 % | **36 %** | 18 % |
| 32 | ~17 % | **71 %** | 36 % |

Derived, not measured — same status as section 7.2, and section 7.4 is still
the measurement that settles it.

**4 % duty produces 18 % frame loss**, because the blackouts are frequent and
short while the frames are long. Worse than uniform suggests, too: the blackout
phases come from N independent masters' clocks, so clustering is guaranteed
rather than avoidable.

> ### ⚠ THE ARITHMETIC ABOVE IS UNDERSTATED IN TWO PLACES
>
> **The PHY switch is 8–20× larger than the 20 µs assumed here.** Nordic
> publishes measured multiprotocol switching tables
> (`nrf_802154/doc/multiprotocol_switching_tables.rst`): **164–303 µs mean,
> 414 µs worst case**, not the ~20 µs this section's `B ≈ 1.3 ms` was built on.
> The real blackout is nearer **1.6–1.9 ms**, and every figure in the table
> above scales roughly with it.
>
> **And the blackout table is not the only cost.** The arbitrated backend
> reserves air for the acknowledged-data reply a tracked frame may turn into
> (`struct radiant_rx_req::follow_on_us`), because MPSL cannot be asked for more
> air from inside a granted timeslot without closing it — and there is no ANT
> retry. That takes the tracked **reservation** at eight sensors from
> 32 × 0.94 ms ≈ 3.2 % to 32 × 2.9 ms ≈ **9.3 %**, and re-running this section
> against it puts a max-size 802.15.4 frame at ~**24 %** rather than 18 %.
>
> **Two different numbers, and this document currently records neither
> separately.** The reserve costs Thread's **scheduler** — deferral, latency,
> refused extensions — and *not* Thread's **air**, provided the backend returns
> the grant the moment an empty window closes. It does, and that is a required
> line of the design rather than an optimisation. So:
>
> | | Air blackout | Scheduler reservation |
> |---|---|---|
> | 8 sensors, tracked | ~1.6–1.9 ms per slot, ~4–6 % | ~9.3 % |
>
> P3.5 measures the first directly — 802.15.4 frame loss *caused by* ANT
> blackouts, against a stub 15.4 receiver — and it was moved ahead of the Matter
> work for a specific reason: **if the real figure is 24 %, the Matter data
> model is being written against a link that does not work.**
>
> **P3.5 IS NOW TAKEN AND THE ANSWER IS 21.8 %** — see §7.3c. The table above is
> confirmed, not merely plausible: the prediction was 18 % here and 24 % with
> the reserve, and the measurement lands inside that bracket, 2.2 pp short of
> the pessimistic end. Read §7.3c's three caveats before quoting it.
>
> **MEASURED 2026-08-13, AND AT ONE TRACKED SENSOR NEITHER COLUMN COSTS
> ANYTHING.** The scheduler column now has numbers (section 7.4.2) and it is
> close to free: Thread's worst send latency was 431 µs against a 296 µs
> unloaded worst case, zero CCA failures, and not a single failed transmit in
> 2400 sends per arm. The air column is likewise free in the other direction —
> ANT measured 0.09 % loss in MED and 0.30 % in SED against 0.30 % with no
> second stack. The claim that the reserve costs Thread's scheduler rather than
> Thread's air is consistent with this, but it has been tested only at **one**
> tracked sensor, and the table above is about 8, 16 and 32. Read the paragraph
> above as answered-at-N=1, not as answered.

One asymmetry matters. MPSL knows the ANT schedule ahead of time, so a
well-behaved 802.15.4 driver **defers its own transmissions** that would not
finish before the next slot — our TX is protected, at the cost of latency. But
**RX cannot be deferred**: an inbound frame arrives when it arrives. The
degradation therefore presents as *other devices cannot reach us*, which is the
worst way for infrastructure to fail, because every symptom points elsewhere.

### 7.3b The rule that decides a role: blast radius, not duty

A Thread MED and a Thread Router both keep the receiver nominally on, and both
lose the same fraction of frames to the table above. What separates them is not
radio time. It is:

> **Does anyone else's radio schedule depend on mine, and who finds out when I
> miss?**

A leaf's misses are absorbed by MAC retries and by TCP, cost some latency, and
are invisible outside the device. A router's misses orphan other people's
children, churn routes and fail SRP registrations for devices that have no idea
this one exists. Same packet loss, two unrelated blast radii.

| Role | Who picks our RX phases | Who notices a miss | Verdict |
|---|---|---|---|
| BLE broadcaster, advertisements only | us — TX only, no listening obligation | nobody | **Safest available** |
| Thread SSED / CSL child | us — we publish the phase | us only | Safe |
| Thread SED, fast poll | us | us only | Safe |
| Thread MED | nobody; always-on by default | us only, MAC retries cover most | Acceptable |
| BLE peripheral in a connection | the central picked the anchor | our link only | Acceptable |
| BLE mesh relay / observer | continuous scan | others' traffic | **Refuse** |
| Thread Router / Leader / Border Router | everyone | children, routes, SRP, the partition | **Refuse** |

**Preference order for the bridge: CSL child, then fast-poll SED, then MED.** A
sleepy role is *better* for coexistence, not worse, because it hands us our own
receive phases — which turns "we lose 18 % of frames at random" into "we own the
schedule and interleave it with the ANT slots deliberately". Thread 1.2 CSL is
built for exactly this: the child publishes a synchronised receive window and
the parent transmits into it, so downlink latency is one CSL period at a duty we
choose. CSL needs a Thread 1.2+ parent, which a household cannot be assumed to
have, hence the fallbacks.

**An earlier draft of this section said MED, and dismissed SED as "wrong,
because it polls and sub-second actuation is the feature."** That reasoning does
not survive the table above, and the latency objection was weak on its own
terms: a 200 ms poll period is comfortably sub-second.

Rebroadcast is *contained*, not free. A leaf still loses inbound frames during
ANT blackouts; it just shows up as retries and tail latency rather than as
someone else's outage. Outbound publishes are nearly unaffected, because we
choose when to transmit and MPSL defers around ANT slots.

**The blast-radius argument above is unaffected by measurement.** Both roles
were measured 2026-08-13 (section 7.4.2) and both pass the coexistence gate:
0.09 % loss in MED, 0.30 % in SED, against 0.30 % with no second stack. SED is
marginally the busier of the two on the arbiter — 1.6 % of requests blocked
against 0.4 % — which is the direction a sleepy child's self-scheduled receive
windows would predict, but the magnitude is negligible and it does not bear
weight. An earlier reading of that difference as large came from a bug since
fixed. Neither role is a worst case at one tracked sensor; the preference order
here rests on who else suffers when we miss, which measurement does not touch.

### 7.3c P3.5 measured — 21.8 %, and §7.3a's arithmetic holds

The reverse-direction measurement §7.3a asks for: 802.15.4 frame loss *caused by*
ANT blackouts, taken with `coex154` on an nRF54L15 DK against a generator on an
nRF5340 DK, 150 s per arm at 50 frames/s, loss over the received sequence span.

| Arm | PSDU 127 (~4.25 ms) | PSDU 40 (~1.28 ms) |
|---|---|---|
| Bench floor — `radiant` not linked | 4.16 % / 5.42 % (repeat) | 2.05 % |
| **1** — arbiter up, 0 ANT channels | 4.88 % / 4.71 % (repeat) | 1.86 % |
| **2** — 8 masters, offsets spread | **21.78 %** | **10.47 %** |
| 3 — 8 masters, offsets bunched | 7.23 % — **confounded, do not use** | not taken |

**Attributable cost (arm 2 − arm 1): 16.9 pp at 127 bytes, 8.6 pp at 40 bytes.**
Arm 1 moved 0.17 pp across an A/B/A and the floor moved 1.26 pp, so bench noise
is ≈ ±1.3 pp and the 16.9 pp is about thirteen times it.

**§7.3a predicted 18 % for this row, or 24 % with the follow-on reserve.
Measured 21.8 % absolute — inside the bracket, 2.2 pp short of the pessimistic
figure.** The arithmetic in that section is confirmed rather than merely
plausible. Whether 21.8 % trips its "revisit the Matter data model before P7"
trigger is a judgement call and is deliberately left open here: it is far closer
to the pessimistic figure than to the headline one, and the 24 % threshold is
not literally cleared.

**An arbiter with no competitor costs nothing measurable.** Arm 1 against the
unlinked floor is ~0.5 pp, inside the noise. That is the closest thing to an
arbitration-only measurement available, and it agrees with the forward-direction
result in §7.4.2.

**What these numbers actually measure, stated because it is easy to overclaim.**
On nRF54L there is no build of the 15.4 driver without MPSL, so arm 1 is
15.4-*with*-an-arbiter-and-no-competitor. The 16.9 pp is therefore the cost of a
second stack's **demand**, not the cost of arbitration.

Three caveats that bound this result:

- **Arm 3 does not measure clustering, so §7.3a's "clustering is guaranteed
  rather than avoidable" assertion is still untested.** With all offsets at
  zero the scheduler can only place one window per period: gate counters over
  the same window show `granted=2089` (~34/s ≈ 8 masters × 4.06 Hz) for spread
  against `granted=285` (~4.6/s ≈ **one** master) for bunched. Bunched is not
  "same duty, clustered" — it is one-eighth the duty. Testing it needs a
  scheduler that can stack coincident windows, or a different way to force
  clustering.
- **The instrument perturbs the measurement, badly, and the figures above are
  the quiet build.** With `CONFIG_RADIANT_SWEEP_DEBUG=y` the same arm reads
  27.9 % against 21.8 % — 6 pp of instrument — because `prj.conf` sets
  `LOG_MODE_IMMEDIATE=y` and the gate's ~1 Hz multi-hundred-byte dump is written
  synchronously in context. Recorded in `coex154/rx_ant.conf`.
- **The raw console captures were not retained**, unlike the P4 arms in
  `build/p4logs`. The numbers above are as reported from live readings and have
  not been re-derived from a saved artefact. Re-take before quoting them
  anywhere load-bearing.

**Why P3.5 was blocked for so long: channel 26 is empty.** The blocker was
`foreign=0` on the receiver — not one ambient 802.15.4 frame — read as evidence
of a deaf receiver. It was nothing of the kind. `coex154/Kconfig` picks channel
26 precisely *because* it is furthest from BLE advertising and most Wi-Fi, so
there is no ambient traffic there to hear. `foreign=0` was a correct reading of
a quiet channel. Proven by pointing traffic at it: with the receiver flashed and
an OpenThread leader in the room, `ot ping ff02::1 100 60 0.3` moved `foreign`
from 5 to 123, +62 for 60 multicast pings. The link then worked with the code
exactly as committed — no fix was required. The plan's leading suspect,
`nrf_802154_promiscuous_set()` never being called, was already false on
inspection: it is called in `role_run()`.

> ### ⚠ A REAL COEXISTENCE DEFECT FOUND WHILE TAKING THIS, AND IT IS NOT
> ### `coex154`'s
>
> Under contention the 15.4 driver panics:
> `NRF_802154_ASSERT(radio_is_disabled)` at `nrf_802154_trx.c:380`, from
> `wait_until_radio_is_disabled()` inlined into `rxframe_finish()`. The driver
> finishes an RX, triggers `TASKS_DISABLE`, spins `MAX_RAMPDOWN_CYCLES` waiting
> for `RADIO->STATE == DISABLED` — and never sees it, because the gate has taken
> the RADIO and ramped it back up.
>
> It was made survivable **inside `coex154` only**, by overriding the `__weak`
> `nrf_802154_assert_handler()` to count instead of `k_panic()`; that is what
> allowed the A/B to be taken at all, and `drv_assert` is printed beside every
> loss figure. **A run with `drv_assert > 0` is a lower bound on a broken
> configuration, not a clean measurement.** The figures in the table have
> `drv_assert = 0`.
>
> The underlying race is untouched and is latent even in the quiet build —
> zero hits across ~20 minutes of quiet runs is absence of evidence, not a
> guarantee. It is the same seam as the sweep-window routing leak in §7.4.2:
> both are the 15.4 driver and the gate disagreeing about who holds the RADIO
> at an instant MPSL thinks is settled.

### 7.4 The coexistence gate

Mirrors the BLE gate [`backends.md`](backends.md) already defines, because the
question is the same and a second acceptance metric would be a second thing to
argue about:

> A build with `CONFIG_NET_L2_OPENTHREAD=y`, attached in the role section 7.3b
> selects and carrying continuous traffic, with `tools/ant_verify.py` showing
> `loss (exact)` no worse than **+0.5 pp** against the same board running ANT
> alone.

Run it in the **MED** role as well as the intended one, even though MED is the
last preference. MED is the always-on case, so it is the worst case, and a gate
measured only in the role that schedules its own windows would be measuring the
mitigation rather than the problem. Record both numbers.

Read it against the characterised floor rather than against zero — this bench's
real ANT loss floor is ~0.4 %, and half of an earlier "1 %" turned out to be the
tool. [`testing.md`](testing.md) is normative for how to read the number; the
trap it documents is the one this gate would otherwise walk into.

The gate is a Tier 2 A/B and it is cheap. Run it before writing a line of the
Matter data model.

#### 7.4.1 First measurement, 2026-08-13 — the gate FAILS

Six arms in one sitting on an nRF54L15 DK, in the plan's order, each preceded by
a J-Link reset and bracketed by an `ot state` check on the peer. Baselines:
[`2026-08-13-radiant-coex-thread-med.json`](../archive/benchmarks/2026-08-13-radiant-coex-thread-med.json),
[`2026-08-13-radiant-coex-thread-sed.json`](../archive/benchmarks/2026-08-13-radiant-coex-thread-sed.json).

| Arm | ANT delivered | Thread send latency, worst | MAC retries | Failed sends |
|---|---|---|---|---|
| Control (no OpenThread) | **961 / 961**, loss −0.01 %, twice, 25 min apart | — | — | — |
| MED, ANT loaded | **0 / 961** | 270 µs (260 µs idle) | 96 (144 idle) | 0 of 2400 |
| SED, ANT loaded | **0 / 961** | 347 µs (296 µs idle) | 330 (76 idle) | 0 of 2400 |

Against the **+0.5 pp** bar this is a decisive fail, and the delta is not a loss
figure in any useful sense: 100 % here means *the channel never acquired*, not
that packets were lost in flight. The mechanism is in the sweep counters, not
the loss counters — with the Thread load transmitting, 22 796 scan chunks were
placed and **none completed**, so device #777 is never found and nothing is ever
tracked. The loaded arm is not a degraded control arm; it is a receiver that
never starts.

Four things this sitting does establish, and they matter for what to fix next:

- **Arbitration itself is not the cause.** Same gate, same board, same rig
  without OpenThread: 961/961 twice, A/B/A delta 0.00 pp against a 0.35 pp bar.
  What costs the link everything is the *demand* of an attached, transmitting
  OpenThread node.
- **The grant-abort invariant held throughout**, under harder conditions than
  the soak it was written for: `grant_end_calls` tracked `granted` exactly
  (15 449/15 449 MED, 13 687/13 687 SED) with `late_disarm`, `idle_disarm`,
  `overstayed` and `invalid_return` all zero, across 131 `EXTEND_FAILED`s and
  an arbiter refusing 39–47 % of everything asked of it.
- **SED is harder on the gate than MED, which is the opposite of the assumption
  above.** 47 % of requests blocked against MED's 39 %, and 102 `EXTEND_FAILED`s
  against 29. A sleepy child schedules its own radio windows and those windows
  are what our reservations collide with; an always-on role has no such
  structure to collide with. "Measure MED because it is the worst case" did not
  hold here — keep measuring both, and do not lean on that reasoning again
  without re-measuring.
- **The sitting found and fixed a permanent gate wedge** (`g.mpsl_owes` raised
  after `mpsl_timeslot_request()` rather than before, so a synchronously-refused
  request left a flag nothing could clear). It only bites where a request is
  refused *synchronously*, which needs a genuinely contending stack — which is
  why BLE bring-up never saw it. Figures above are from the fixed build; the
  previously recorded MED figure of 973/974, 0.10 % predates this and two other
  fixes and is not comparable.

**Root cause of the 0 % was found the same day, and the gate now passes — see
§7.4.2.** It was not the arbiter, not the grant-abort path and not the deadline
backstop. It was five separate defects in series, each hiding the ones behind
it, which is why every counter in the table above looked self-consistent while
nothing worked. **Everything in this section is superseded as a result and is
kept only as the record of how it was found.**

The formal verdict, from `ant_ab.py` against `[gates.coexistence]`:

```
| coexistence (second stack on) | 0.000 % | 100.000 % | <= second-stack-off + 0.5 pp | FAIL |
  second stack: thread_med; 100.000 % is over the 1.5 % ceiling on its own
```

Identical for `thread_sed`. Two mechanical notes for whoever re-runs it:
`gate_coexistence()` reads one baseline, not an A/B pair, so the invocation is
`ant_ab.py <file> <file>` — passing the MED and SED files together is refused,
correctly, because they are different rigs. And the other gates in that table
report FAIL against these files only because a coexistence-only baseline carries
no `radio_runs`, `conformance` or `usb_runs` block; those verdicts are an
artefact of the invocation and say nothing about the build. `arbiter cost` and
`sweep rate under contention` SKIP for the reasons given above — no
`loss_direct_pct` arm was taken, and no sweep set ever completed.

Two corrections to the P4 recipe, both learned the hard way and both now in
`scripts/build_p4.ps1` / `scripts/p4_arm.ps1`:

- **Use the `power` profile, not `heart-rate`.** `ant_verify.py` only computes
  `loss (exact)` when the transmitter's event counter never stands still, and a
  heart-rate master's counter steps per *beat*, not per message.
- **`CONFIG_RADIANT_SWEEP_DEBUG=y` is required on the build line.** Without
  it there is no 1 Hz gate dump and no sweep counters, and a perfectly healthy
  board looks like a hang.

One limitation bounds the absolute figures: the nRF52840 Feather that normally
carries the reference master would not enumerate, so the master was a Dongle
driven by `tools/ant_sim.py` — which `baseline.schema.json` deliberately refuses
as a baseline transmitter. An ANT master re-broadcasts its current payload until
the host loads the next, so over 240 s the receiver catches a repeated event
count (9 repeats in 944 pairs, measured) and `loss (exact)` is absent from every
arm. Arm-to-arm deltas are unaffected — every arm used the identical rig — but
these absolutes cannot be read against the ~0.4 % floor. Re-take against
`sim_firmware` when the Feather is back.

#### 7.4.2 Five defects in series — and the gate now PASSES

All found and fixed the same day. Numbers below are independently re-measured on
the fixed tree, not carried over from the work that produced the fixes.
Baselines: [`…-med-fixed.json`](../archive/benchmarks/2026-08-13-radiant-coex-thread-med-fixed.json),
[`…-sed-fixed.json`](../archive/benchmarks/2026-08-13-radiant-coex-thread-sed-fixed.json).

| arm | before | after | vs control |
|---|---|---|---|
| p4ctrl, 240 s | 961/961 | 959/962, **0.30 %** | — |
| p4med, 240 s | **0 of 961** | 960/961, **0.09 % — PASS** | **−0.21 pp** |
| p4sed, 240 s | **0 of 919** | 957/960, **0.30 % — PASS** | **0.00 pp** |
| ztest, 3 apps | — | **672 pass, 0 fail** (core 627, api 22, gate 23) | — |

`ant_ab.py` against `[gates.coexistence]`, both roles:

```
| coexistence (second stack on) | 0.300 % | 0.090 % | <= second-stack-off + 0.5 pp | PASS |   (MED)
| coexistence (second stack on) | 0.300 % | 0.300 % | <= second-stack-off + 0.5 pp | PASS |   (SED)
```

An ANT+ channel and an attached, transmitting OpenThread node now share one
radio at no measurable cost to either: the contended MED arm measured very
slightly *better* than the control, which is scatter rather than a real
advantage, and the Thread side completed 2400 sends per arm with zero failures.
The arbiter is close to free — 4 of 970 requests blocked in MED, 16 of 986 in
SED.

**The pre-fix sitting's "SED is harder than MED" correction does not survive**,
and §7.3b's caveat should be read with that in mind: on the fixed build the
direction is the same but the magnitude is negligible (1.6 % of requests blocked
against 0.4 %), so that finding was mostly measuring bug 23 rather than the
role. Neither role is a worst case at this load. Do not lean on either claim
without re-measuring at higher sensor counts.

#### 7.4.2a The scheduler cost, from the Thread side

§7.4.2 reports what coexistence costs **ANT+**. This is the other direction, and
it is the column the §7.4 tables never had: what arbitration costs **Thread**.
It is not a new sitting — every number is read out of the consoles
`scripts/p4_arm.ps1` already captured on 2026-08-13 (`thread lat us:` and
`thread mac d:`), which nothing had ever tabulated.

| | p4ctrl (control) | p4med (Thread MED) | p4sed (Thread SED) |
|---|---|---|---|
| MAC frames sent, cumulative | — (no load) | 2 407 | 4 935 |
| MAC retries | — | 20 (0.83 %) | 32 (0.65 %) |
| CCA failures / aborts / busy | — | **0 / 0 / 0** | **0 / 0 / 0** |
| Send latency ≤ 128 µs | — | 2 397 | 2 384 |
| Send latency 128–256 µs | — | 3 | 16 |
| Send latency > 256 µs | — | **0** | **0** |
| Worst single send | — | **311 µs** | **431 µs** |
| Gate requests blocked | 0 of 965 | 4 of 970 | 16 of 986 |
| Gate `eagain` | 7 | 49 | 82 |
| Grant skew | 0 | 8 548 | 6 055 |

**Reading it.** Arbitration is close to invisible to Thread. Better than 99.8 %
of sends complete inside 128 µs in both roles, nothing at all lands beyond
256 µs, and the worst single send in either arm is 431 µs — against an ANT+
window that recurs every 250 ms. There were **no** CCA failures, aborts or busy
returns in either role, which is the result that matters most: the 802.15.4 MAC
never found the air taken when it went to use it. The retry rate (0.65–0.83 %)
is ordinary 802.15.4 behaviour at this scale and does not differ meaningfully
between the roles.

The SED arm is the slightly more contended one on every column that moves
(16 blocked against 4, 82 `eagain` against 49, a 431 µs worst case against
311 µs), which is the same direction §7.4.2 found from the ANT+ side and the
same negligible magnitude. Neither role is a worst case at this load.

**The ANT+ loss figures from that sitting are deliberately NOT repeated here.**
They were taken against `tools/ant_sim.py` driving a Dongle, which
`archive/benchmarks/baseline.schema.json` refuses as a baseline transmitter, so
they cannot be read against the ~0.4 % floor — that is §7.4.1's own recorded
limitation and it is what §7.4.3 below is for. The scheduler-cost numbers in
this table are unaffected by it: they are the *Thread* stack's own account of
its own sends, and the transmitter on the ANT+ side is not in that measurement
at all.

#### 7.4.3 Re-taking the three arms against an admissible transmitter — OUTSTANDING

§7.4.1's limitation stands and is the reason the loss column above is missing.
The fix is to re-take all three arms with `apps/sim` **firmware** as the master
rather than a host script driving a dongle.

**That transmitter now exists and is proven** (2026-08-15): `apps/sim` builds on
the v3.2.4 pairing and runs on an nRF54L15 DK, and a Feather DUT decoded it at
100.06 W against 100 and 80.09 rpm against 80 with clean accumulator continuity.
`apps/sim` also builds for `nrf52840dk` and the Feather, so the rig is
Feather-as-master / L15-as-DUT / nRF5340-DK-as-Thread-peer.

**It is nevertheless blocked, and not on equipment.** Three back-to-back A-leg
repeats on that rig measured 7.94 %, 4.87 % and 7.04 % loss — a 3.07 pp spread
against `repeat_a_max_delta_pp = 0.35`, whose own rule voids a sitting. A
coexistence delta of 0.5 pp cannot be read out of a bench with 3 pp of run-to-run
scatter, so re-taking the arms today would produce numbers that look like
results and are not. See the header of `tools/ab_gates.toml` for the full
measurement and the likely cause (a real trainer broadcasting three device types
continuously on the ANT+ frequency). Power that down, confirm the A1/A2 delta is
inside 0.35 pp, and this becomes one sitting.

**Bug 19 — the endpoint snapshot was taken at the wrong moment.**
`radio_endpoints_save()` ran twice in `radiant_radio_init()`: once correctly,
before the init-time hand-back, then again unconditionally afterwards. The
second call overwrote the contended entries with what the hand-back had just
restored, so the per-grant swap re-applied *the other stack's* routing for
exactly the two registers it exists to move. The file's own comment warns
against this and was defeated by the later call.

**Bug 20 — the swap set was decided by a boot race, and this was the root cause
of the 0 %.** The set was "registers another stack already held when
`radiant_radio_init()` ran". Two consecutive boots of the *same image* gave that
mask 2 and then 0. When RadiANT initialises first the mask is empty, the swap is
compiled in but permanently inert, and the 802.15.4 driver then writes its own
`PUBLISH_ADDRESS`/`SUBSCRIBE_RXEN` over ours — directly, not through
`nrfx_gppi_conn_alloc()`, so nothing refuses it and nothing is logged. Our
`CC_START` compare no longer reaches `TASKS_RXEN`, the receiver never ramps, and
because `TASKS_DISABLE` on an already-DISABLED radio raises no
`EVENTS_DISABLED`, the window never produces a terminal either — so every
granted timeslot ran to its TIMER0 end with the operation still armed and was
reported `DONE_DENIED`. That is the "13 001 grants delivered, 0 windows ended
OK" accounting in §7.4.1. The set is now "registers this backend programmed",
and the foreign value is re-read at **every** grant entry rather than
snapshotted at init — correct under any init order, and it survives the other
stack changing its routing between grants, which it does constantly.

**Bug 21 — the NVIC line was enabled once, at startup.**
[`mpsl.rst`](https://docs.nordicsemi.com/) puts RADIO in the "no interrupts" set
on nRF54L: if the Timeslot API is used for RADIO access, the application must
enable and disable the RADIO interrupt itself. `radiant_radio_enable()` did that
once — enough beside the SoftDevice Controller, not enough beside OpenThread,
because the 802.15.4 driver NVIC-disables the same vector in its own teardown.
After that MPSL has nothing to forward and `MPSL_TIMESLOT_SIGNAL_RADIO` never
arrives. The line is now taken for the grant and restored on the way out, like
the endpoint registers.

**Bug 22 — `elastic_skew_us` moved the reservation without moving the
operation.** It added the skew to the timeslot's `start`/`end` while the staged
operation kept the core's absolute `t_open`/`t_close`, so the grant began up to
11.4 ms after the window was due and `program_rx()` refused `ETIME` — the right
answer to the wrong question. Every refusal fed the skew another step, so it was
self-sustaining. Removed rather than repaired: the reservation must cover the
operation, and shifting the operation is not the gate's to do.
`BLOCKED_RUN_REANCHOR` already changes phase by construction. **Not proven:** the
skew was introduced against a 100 ms BLE advertiser and this removal has only
been measured against OpenThread. Re-measure the BLE arm before treating the
advertiser case as settled.

**Bug 23 — the grant ended 14 µs after it started, and this was the whole of
the remaining 96 %.** MPSL restarts TIMER0 from zero each timeslot, but CC0
still holds the *previous* grant's value until `SIGNAL_START` rewrites it — and
that rewrite happens ~31 µs in, after the receive window has been programmed.
Anything the counter walks past in those 31 µs latches
`EVENTS_COMPARE0`, and `nrf_timer_int_enable()` then raises `SIGNAL_TIMER0`
immediately. The gate does the correct thing with a TIMER0 signal: it ends the
timeslot — 14 µs in, against a window not due to open until 231 µs.
Self-sustaining, too, because `gate_release()` sets CC0 from the *ending*
grant's own count, so each short grant guarantees the next one. The fix is a
single `nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0)` between
the `cc_set` and the `int_enable` — the exact three-step ordering
`gate_release()` has had all along, in the one other place that arms the same
compare.

Why it only ever appeared beside a second stack, measured rather than argued:
the stale-compare counter reads **0** in the control arm and **963 of 965**
grants in a contended one. With nothing else asking for the radio, grants follow
each other closely enough that the previous CC0 is never passed during that
31 µs window. That is also why every control arm in this document always
passed.

**A hypothesis that the evidence killed, recorded because it was convincing.**
The 802.15.4 driver *does* reset the RADIO inside our grants — `nrf_radio_reset()`
zeroes every `SUBSCRIBE_*`/`PUBLISH_*`, writes `INTENCLR00 = 0xffffffff` and
triggers `TASKS_SOFTRESET`, and it is delivered from the MPSL work-queue
*thread*, so it lands wherever that thread is next scheduled. It was the
obvious culprit and it was the wrong one. A cross-tab over the tracked windows
the loss figure is actually computed from recorded **`wipe=0`** — before the fix
as well as after. The `ramp=1080/1532` that pointed at it was dominated by long
sweep windows and by anchor bootstraps that program nothing. An in-grant
detect-and-repair was built against this hypothesis and then removed, since it
had nothing to fix on the path that mattered.

**Still open, and neither costs tracked packets:**

- **Long sweep/ED windows can still lose their routing to that reset**, because
  their grants are tens of milliseconds — wide enough to contain the MPSL work
  queue being scheduled. That is a discovery-*speed* cost beside a second stack
  and is unmeasured as a rate. Whether the handover ordering is Nordic's to fix
  is arguable, but it is not costing packets on the tracked path, so there is no
  case for raising it yet.
- **`DONE_DENIED` credits zero dwell, which is a livelock rather than a slow
  path.** A chunk that is always denied leaves `dwell_remaining` unchanged, so
  `radiant_sched_rechunk()` re-arms the identical chunk forever and the sweep
  never leaves the set — observed frozen on set 3 re-arming ~110×/s. It needs a
  bounded-denial escape regardless of what caused the denials.
- **`radiant/tests/CMakeLists.txt` never listed `src/profiles/profile_rd.c`.**
  The RD phase added the decoder, the adapter, the test and the adapter's own
  CMake entry, but not the implementation's — so the core ztest application
  failed to link and `profile_rd`'s 25 tests had never run once. Fixed; they
  pass.


### 7.5 The bridge is never a border router

Normative, and decided in
[`decisions/0011-never-a-border-router.md`](decisions/0011-never-a-border-router.md).
The "2-in-1 dongle" — an ANT receiver that is also the household's Thread border
router — is the obvious product and it must not be built on one radio.

Three reasons, in ascending order of how completely they settle it:

1. **The role is not tunable.** A border router's Thread interface is a Router
   and usually the Leader: `rx-on-when-idle` always, because it parents sleepy
   children, answers their data polls within milliseconds, relays for other
   routers and runs the SRP server Matter devices register against.
2. **Section 7.3b puts it in the Refuse row**, and section 7.3a says why the
   loss is not small: ~18 % of full-size frames at eight tracked sensors, landing
   on traffic that belongs to devices which have never heard of ANT.
3. **The purpose collision, which settles it without any radio argument.** A
   border router is not just a radio — it is routing, discovery proxy, SRP and
   NAT64, which in a USB-dongle form factor means the dongle is an **RCP** and
   the border router proper is a daemon on a capable host. But *if a capable
   host is present, the ANT data should go to it over USB* — faster, lossless,
   already built, and needing no Thread at all. The Thread data plane of this
   document exists for the **hostless** case. So the two are mutually exclusive
   by purpose, not merely by radio: in every deployment where the 2-in-1 could
   work, half of it is unnecessary.

There is a product-shape argument in the same direction. A USB dongle is mobile
— that is the point of it — and a border router must be fixed. "I moved the
dongle to my laptop and the house lost its Thread route" is a support burden
designed in rather than encountered.

#### The configurations, and which combinations are refused

| Config | Thread role | ANT role | Host | ANT data path |
|---|---|---|---|---|
| **A. Dongle** (today) | none | full: scan and track | yes | USB |
| **B. Bridge** (this document) | CSL child / SED / MED | track only; scan is a bounded pairing state | **no** | MQTT over Thread, plus Matter |
| **A+B. Combined dongle** | CSL child / SED | full: scan and track, both permanent | yes | **USB only.** Matter carries the control plane; **no MQTT** |
| **C. Border router RCP** | Router / Leader, via a host daemon | none on this radio | yes | USB, if ANT at all |

**A+C and B+C are refused at compile time**, in the `BUILD_ASSERT` shape
[`backends.md`](backends.md) already uses for `CONFIG_BT` on the direct gate.

**A+B was in this row as "mutually exclusive" and is not.** It is the combined
USB + Thread dongle: a rider's heart rate drives a fan over Matter while Zwift
is served over USB. Three things make it a different configuration rather than a
merge of the two above, and all three are decisions rather than consequences:

- **No MQTT plane.** A host is present, so values go over USB and Matter carries
  the control plane only. MQTT stays in the hostless bridge build, where it is
  the *normative* path for values — see ADR 0010, which this does not overturn.
- **The scanner is permanent**, put there by the host rather than by a user.
  That is what forced the amendment to section 7.2 above, and it is why the
  combined build depends on ADR 0013 in a way the hostless bridge does not.
- **Never all three at once.** BLE + RadiANT + Thread is out of scope by
  decision, not by arithmetic.

The Thread-free dongle build (A) remains the safe Zwift default. A+B ships
beside it, not instead of it.

#### If the 2-in-1 is wanted anyway, it is a BOM decision

Not two roles on one radio — **a second 802.15.4 part on the same board**. One
design note belongs here rather than being found on a bench: **ANT's RF 57 is
2457 MHz, which sits between 802.15.4 channels 21 (2455) and 22 (2460)**. Two
radios that close on one board desense each other whatever the time-sharing
does. Pin Thread to channel 15 (2425) or 25/26 (2475/2480) and budget antenna
isolation.

The cheaper answer is two USB devices — a border-router-only dongle and an
ANT-only dongle, neither compromised — and the sync handoff page `0x12` already
exists to let two ANT receivers cooperate.

---

## 8. The Matter plane

### 8.1 Endpoint model — one table, keyed on the vocabulary type

**An endpoint is instantiated per announced field, not per binding kind.** The
Matter sink holds one table keyed on the section 7 field type;
`binding_changed` (section 4) walks the binding's fields and instantiates an
endpoint for every type that has a row. A type with no row is not an error — it
is a field the Matter plane declines and the MQTT plane carries.

```c
struct radiant_matter_attr_conv {
	uint32_t attribute;
	int32_t  mul;                 /* 0 = CONSTANT: write `offset` once, at creation */
	int32_t  div, offset;
};

struct radiant_matter_type_map {
	uint8_t  field_type;          /* the §7 vocabulary */
	uint16_t device_type;         /* Matter Device Library, 0 = cluster only */
	uint32_t cluster, attribute;
	int32_t  mul, div, offset;    /* SI -> the cluster's unit */

	uint8_t  n_extra;             /* further attributes in the same cluster */
	struct radiant_matter_attr_conv extra[RADIANT_MATTER_MAX_EXTRA];
};
```

**A row is a list of attributes, not one attribute**, and §8.1a is where the two
things that forced that are set out.

| Field type | Matter device type | Cluster / attribute | SI -> cluster unit |
|---|---|---|---|
| `0x02` occupancy | Occupancy Sensor `0x0107` | Occupancy Sensing `0x0406` / `Occupancy` | boolean |
| `0x10` temperature | Temperature Sensor `0x0302` | Temperature Measurement `0x0402` / `MeasuredValue` | int16, 0.01 °C: `K x 100 - 27315` |
| `0x11` relative humidity | Humidity Sensor `0x0307` | Relative Humidity Measurement `0x0405` / `MeasuredValue` | u16, 0.01 %: `% x 100` |
| `0x12` barometric pressure | Pressure Sensor `0x0305` | Pressure Measurement `0x0403` / `MeasuredValue`, **plus `ScaledValue` and `Scale`** | int16, whole kPa: `Pa / 1000`; scaled: `Pa / 10` at `Scale = 2` (§8.1a) |
| `0x25` battery state of charge | — cluster only, on the source's **existing primary** endpoint | Power Source `0x002F` / `BatPercentRemaining` | 0.5 %: `% x 2` |
| `0x16` speed, `0x18` angle | **none, and it is section 1's rule for the second time** | — | MQTT plane only (§8.1a). Wind is what forced the decision; `0x16` also carries FE-C's trainer speed, and it has no cluster either |
| `0x26` heart rate | **none, and section 1 is the whole reason** | — | — |

The derived booleans of section 6 are all type `0x02`, so they are instances of
the first row rather than cases in a switch:

| Name | Occupied when | From |
|---|---|---|
| **Heart rate monitor worn** | the beat accumulator is advancing | §6.1 |
| **Bike in use** | the energy accumulator is advancing | §6.1 |
| **At rest** | worn, and HR below `HR_rest + 0.15 x HRR` | §6.2 |
| **Training, zone 2 or above** | worn, and HR at or above `HR_rest + 0.60 x HRR` | §6.2 |
| **Equipment in use** | FE-C page 16 byte 7 reports `IN_USE` | added 2026-08-15 |

**The fifth one is the only one that is not an inference, and it is worth the
sentence.** §8.2 below admits the first four stretch the Occupancy Sensor
definition. Fitness equipment is where the stretch disappears: FE-C page 16
carries an explicit FE state — `READY` (2), `IN_USE` (3), `FINISHED` (4) —
which is the machine's own report rather than an answer to §6.1's "is an
accumulator advancing?" heuristic.

It also closes a gap that was invisible until a treadmill existed. **"Bike in
use" is driven by the `0x30` energy accumulator, which the FE-C adapter posts
only from page 25 (Specific Trainer Data).** A treadmill sends page 19 instead,
which nothing in this tree decodes — so before this rule a bound treadmill
published a speed and a state enum and asserted no activity at all. The state
enum had been on the sample bus since the FE-C decoder landed and
`rules_want()` accepted only `ACCUMULATING` samples and
`RADIANT_FIELD_HEART_RATE`, so nothing read it.

**No vocabulary addition was needed**, which matters because §13 forbids adding
a `RADIANT_FIELD_*` until it is allocated in `docs/radiant-telemetry.md` and
`tools/ant_pages.py` in the same change: occupancy `0x02` was already in all
three copies, and this is a new `field_id` beside the existing four.

**The dwell is asymmetric the other way from §6.1's.** There is no assert dwell
— waiting two seconds to believe a direct report only adds latency to a fact
already established — and there is a short (5 s) clear dwell, so that a runner
stepping off for a drink does not flicker a gym display. `STALE` still
overrides both, immediately, which is what stops an unplugged machine reading
"in use" forever.

**Do not casually also enable the FE-C adapter's reserved distance and
elapsed-time field ids.** They are reserved deliberately
(`radiant_power_adapter.h`): §6.1's ACTIVE slot is *single-occupancy*, and a
source posting `0x30`, `0x37` and `0x34` would have all three fight over one
`prev_raw` and one dwell. Per-machine utilisation needs a per-field-type slot
map first — a real change, not a free one.

Two additions carry raw bpm and neither is part of tier 1:

| Cluster | Attribute | Carries |
|---|---|---|
| `0x0404` Flow Measurement | `MeasuredValue` | raw bpm, the default-off passenger cluster of §8.3b |
| `0xFFF1_xxxx` (MEI) | `HeartRateMeasurement` | raw bpm, standards-clean, §8.3 |

So a power meter binding instantiates one endpoint, a heart-rate binding three,
and a household with one strap and one trainer shows four entities in Home
Assistant rather than sixteen — unchanged from the per-binding-kind version this
replaces, because the *outcome* was never the problem.

**The earlier draft hardcoded the endpoint set per binding kind** — "a
heart-rate binding instantiates 1, 3 and 4; a power meter instantiates 2" — with
endpoint numbers as fixed identities. That is a switch statement per sink per
profile, which is the exact cost section 3's sample bus exists to abolish, and
one layer up from where the bus already solved it. The tell was that adding
temperature to it needed a design discussion rather than a table row.

### 8.1a The environmental quantities are the clean case, and it is worth saying out loud

Every other quantity in this document reaches Matter by a compromise: occupancy
is a stretch (§8.2), the MEI cluster is invisible until someone else's code
learns it (§8.3), the passenger cluster is deliberately mislabelled (§8.3b).
**Temperature, humidity and pressure are the case where none of that applies.**
Matter has a device type, a cluster and an attribute for each, with the right
units and the right semantics, and `radiant-telemetry.md` section 7 already
allocated `0x10`, `0x11` and `0x12` against them. Nothing is stretched, nothing
is hidden, and nothing is mislabelled.

They are therefore **tier 1**: they appear on the QR scan with nothing
installed, correctly named, in every controller — the same tier as the derived
booleans and by a much shorter argument.

Two quantities page 84 also carries do **not** get that treatment, and the two
refusals are different in kind: **wind** has no Matter cluster at all and takes
the MQTT plane, while **pressure** has one whose mandatory attribute is too
coarse to be useful. Both are below.

**The one trap is that temperature is the only row with an offset.** The
vocabulary's canonical unit is kelvin and Matter wants 0.01 °C, so the
conversion is `K x 100 - 27315` — a scale *and* a shift. Every other type in the
vocabulary is a pure decimal scale, which is why `exp` alone was sufficient
everywhere until now, and why a map struct with `mul`/`div` but no `offset`
would look complete and ship a 273.15 K error on exactly one row.
`radiant-telemetry.md` section 7's worked example already states the arithmetic;
the map has to be able to express it.

**Where the values come from, and the three sources are no longer unequal:**

| Source | Status |
|---|---|
| A RadiANT `0x60` node whose descriptor announces `0x10`, `0x11` or `0x12` | **Free by construction.** The descriptor names the type, the bus carries it, the table maps it. No adapter, no per-profile code |
| ANT+ Environment `0x19` | **Implemented** — `profile_env.c` + `radiant_env_adapter.c`, temperature ; `device-profiles.md` §3.7 |
| ANT+ common page 84 | **Implemented** — `profile_common_decode_84()` + `radiant_common_adapter.c`. `device-profiles.md` §3.4. This is the "any device type" path and it is now the *widest* of the three, not the weakest |

The three rows used to differ in evidence, and they no longer do: all three are
primary-derived. **What they still differ in is bench exposure.** No ANT+
environmental sensor is on this bench, so the two ANT rows are verified against
the spec vector, `tools/ant_sim.py` and ztest, and not against a real sensor.
That is a conformance question rather than an architecture one, and it is a
materially better position than the one this section used to record — but
"vector-tested" is not "verified", and this section should not be read as either
more or less than that.

#### The battery row now does what it always said it did

`device_type = 0` means "cluster only, on the binding's own endpoint", and until
this change the endpoint allocator called through unconditionally, so a battery
field allocated its **own** endpoint with `device_type = 0`. In Matter that is a
bridged node with no device type and a single Power Source cluster — a phantom
entity in every controller, sitting beside the real sensor and reporting its
battery as though it were a separate thing in the house. Harmless in a table
nobody rendered; not harmless once a CHIP stack is behind the seam.

A `device_type == 0` row now resolves to the source's **existing primary
endpoint** — the first one instantiated for that source that carries a device
type of its own. If there is no primary yet, the row is **deferred, not
dropped**, and at `LOG_DBG` rather than `LOG_WRN`: a battery page arriving
before any measurement is ordinary for a sensor whose common page 82 leads its
first data page, and the next battery sample after the primary exists lands on
it.

**Endpoint budget.** Section 5 sizes the binding table against the Matter
endpoint count and the coexistence budget together, and this is the change most
likely to push on it. It pushes gently: an environment binding is one or two
endpoints, against three for a heart-rate binding, so the worst case per binding
does not move. Battery no longer costs an endpoint at all. **The worst case
still does not close**, and that is accepted rather than unnoticed:
`RADIANT_BINDING_MAX` is 8 and one heart-rate binding alone announces four
derived booleans, against `RADIANT_MATTER_MAX_ENDPOINTS` 16 — a number that is
not this table's to pick unilaterally, since it must equal the CHIP side's
`BRIDGE_MAX_DYNAMIC_ENDPOINTS_NUMBER` and
`CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT`. Overrunning it logs a warning and
the field does not appear; it is never a silent drop.

### 8.2 Occupancy is a stretch, not a lie, and that is why it was chosen

**This section is scoped to the derived booleans, and §8.1a is why that scope
needs stating.** Where a vocabulary type has a real Matter device type — `0x10`
temperature, `0x11` humidity — the bridge uses it and no stretch is involved.
Occupancy is what the `0x02` row does, and the argument below is about that row
alone. A reader who takes it for the bridge's general posture will conclude the
design is more compromised than it is.

Matter requires each endpoint to declare a device type from the Device Library,
and there is no wearable, vitals or heart-rate type. **Occupancy Sensor is the
closest honest fit, and it is closer than it first looks** — read each endpoint
as "something is occupying a space":

- *Heart rate monitor worn* — a body is occupying the strap.
- *Bike in use* — a rider is occupying the trainer.
- *At rest* / *Training, zone 2 or above* — the person is occupying a
  heart-rate zone.

That last one is the stretch, and it is a stretch rather than a fabrication: a
zone genuinely is a region, and being in it genuinely is occupancy. Compare the
alternative this replaces — **an earlier draft declared Contact Sensor
(`0x0015`) carrying Boolean State**, which is not a stretch of anything. A heart
rate is not a door.

Three practical reasons the choice is also the cheap one:

1. **One device type across every derived output.** All four are type `0x02`,
   so they are four instances of one row in §8.1's map rather than four rows.
   No per-boolean decision, and a reader of the code never has to ask why the
   *at rest* endpoint differs in kind from the *worn* one.
2. **Home Assistant renders it as `binary_sensor` with `device_class:
   occupancy`**, which is a sensible label for all four, where `contact` would
   have read as a door or window on every card.
3. **Apple Home and Google both render occupancy today.** Tier 1 works outside
   Home Assistant with no extra work.

If the Device Library ever grows a vitals type, moving to it is a
recommissioning rather than a format break.

### 8.3 The MEI cluster carries the number, and is honestly second

A manufacturer-extension cluster under our own MEI carries raw bpm. It is
standards-clean and it costs almost nothing to define. It is **not** the
primary path to Home Assistant, and the doc should not imply it is: a custom
cluster reaches HA only once `python-matter-server`'s custom-cluster table or a
custom component learns it, and that is a change in someone else's repository
on someone else's schedule.

Its actual jobs are the no-broker deployment, and being the thing that already
exists if a controller ever does add support. Ship it; do not lead with it.

### 8.3b The numeric passenger cluster — off by default, and here is the cost

Tier 1 of section 1 gets a user booleans with zero installation. Some users will
want the *number* with zero installation too, and there is exactly one way to
give it to them: publish bpm on a standard numeric cluster that Home Assistant
already renders, and accept that the label is wrong.

**Flow Measurement (`0x0404`) is the least harmful carrier.** Almost nobody has
a flow sensor, so the namespace is empty, and — unlike the alternatives — it
pollutes nothing the user cares about. Temperature would land in climate
dashboards; Electrical Power would land in the energy dashboard and corrupt
statistics that HA computes long-term sums from. Illuminance is log-encoded in
Matter and would need un-mangling. A wrong number in the energy dashboard is a
support ticket; a wrong number in a flow sensor nobody has is a renamed entity.

**§8.1a strengthens this, and it is the one place the two sections interact.**
Now that `0x10` temperature is a real row in §8.1's map, a bpm-as-temperature
passenger would not merely be mislabelled — it would sit in the same controller,
under the same bridge, alongside genuine temperature endpoints, at a plausible
value and with no way for a user to tell which reading is the real one. That
turns a renamed entity into a wrong one. Flow Measurement was already the choice;
it is now the choice by a wider margin.

**It is off by default**, and the reason is worth stating rather than assumed:
shipping deliberately mislabelled entities as a default is how a product feels
untrustworthy, and a user who never enables it is never lied to. A user who
enables it has opted into a known trade with a documented reason.

Three tiers of honesty, then, and they should be presented to the user in this
order: booleans (correct and free), MQTT (correct and one add-on), passenger
cluster (free and mislabelled).

### 8.4 The dongle as a Matter client — the latency floor

For "elevated HR → fan on", the dongle does not need to represent heart rate in
Matter at all. It needs the **Binding cluster** and client-side `On/Off` and
`Level Control`, so a commissioner binds it to the fan once and the rule
thereafter runs entirely on the nRF with no hub in the path. This is the Matter
light-switch pattern, and it is the lowest-latency and most robust form of the
headline feature.

Caveat, stated because it decides whether this can be the only path:
controller-side support for *configuring* bindings is thin in practice. So ship
8.1 as well and let a hub route when binding is unavailable. Client binding is
the good path, not the only path.

### 8.4a Two fans, and they are not alternatives

This document and `device-profiles.md` §5.2 both describe turning a fan on from
a heart rate, and a reader can easily take them for two competing designs with
one of them redundant. They are not. **They are the same rule evaluator driving
two different fans, and which one applies is decided by the fan, not by us.**

| The fan speaks | Where RadiANT firmware runs | Last hop | Specified in |
|---|---|---|---|
| **RadiANT** — device type `0x62` | **on the fan itself.** The fan is an ANT node with a descriptor, a schema and a command surface | ANT page `0x10` reliable command: `0x02` set level, `0x04` set mode | `device-profiles.md` §5.2 |
| **Matter, but not RadiANT** — an ordinary smart fan | **on the bridge only.** The fan has no ANT radio and never hears of RadiANT | Matter `On/Off` and `Level Control`, client-side, over Thread | this section |

Two consequences worth stating, because each is easy to get backwards:

- **The `0x62` path does not need this document at all.** A head unit with an
  ANT radio commands a `0x62` fan directly — no Thread, no Matter, no bridge, no
  hub. That profile exists so a fan *vendor* can ship an ANT product, and
  `device-profiles.md` §5.2's resolution of the auto-mode hole is what keeps it
  implementable by a vendor with no ANT receiver in the product.
- **The Matter path cannot use `0x62`, and no amount of profile work changes
  that.** The fan has no ANT radio. The only thing the bridge can send it is
  Matter, which is precisely why §8.4's client binding is the mechanism rather
  than a convenience.

The two share everything above the last hop: the same sample bus (§3), the same
binding table (§5), and the same rule evaluator (§6) producing the same derived
booleans. Only the final carrier differs, which is what the sink registry of
section 4 is for — a `0x62` fan and a Matter fan are two sinks, not two
architectures.

**Neither is the generic case, and v1 does not attempt one.** An ANT+ device
sending a generic command that the bridge relays to an arbitrary Matter actuator
— a remote button mapped to whatever the user likes — is a third thing, and it
is out of scope here: it would need ANT+ Controls `0x10`, which
`device-profiles.md` §9 records as unimplemented on community and open-source
evidence, and it raises a mapping-registration question neither path above has
to answer. Section 13.

### 8.5 Commissioning is what pays for the Matter stack

Commissioning provisions the Thread operational dataset over BLE. That is how
the device joins the network the MQTT plane runs over, and it is why a build
whose data plane is entirely MQTT still wants a Matter stack. Section 1 states
the argument; it is repeated here only because it is the thing most likely to
be optimised away by someone counting flash.

---

## 9. The MQTT plane

### 9.0 Plain MQTT over TCP. Not MQTT-SN, and here is why

**MQTT-SN is rejected.** It is a UDP protocol that requires an **MQTT-SN
gateway** to translate into real MQTT before any broker will see it, and nothing
in a Thread network, a border router or Home Assistant performs that
translation. Adopting it would mean shipping a gateway daemon — which is
precisely the class of thing section 1's tier table exists to avoid.

The constraint MQTT-SN was designed for does not apply here. It targets networks
with no TCP — Zigbee, raw 802.15.4. **Thread is IPv6 with a real TCP stack**;
in Zephyr, OpenThread is an L2 beneath the native IP stack, so `CONFIG_NET_TCP`
plus `CONFIG_MQTT_LIB` is an ordinary configuration. Plain MQTT to Mosquitto, no
gateway, no translation layer, nothing to maintain.

Two routing facts make this less fragile than it sounds, and both are worth
knowing before someone proposes a workaround for a problem that is not there:

- **OTBR provisions its own ULA prefix** and advertises it on the LAN, so a
  Thread node reaches the Home Assistant host over IPv6 **even where the user's
  ISP and router have no IPv6 at all**. No NAT64 required. This is the same
  mechanism that already makes Matter-over-Thread work for everyone.
- **On the common HA hardware the border router and the broker are the same
  box.** The path is Thread → border router → localhost. There is very little
  routing left to go wrong.

### 9.0a Provisioning: address is solvable, credentials are not

Section 1 tier 2 costs a provisioning step. It splits into two halves with
different answers, and conflating them makes the problem look worse than it is:

- **Address — solvable in principle.** Once on the network the bridge can
  resolve `homeassistant.local` through OTBR's DNS-SD Discovery Proxy and assume
  a broker on that host at 1883, which is true of very nearly every HA install.
  **Two things to verify on real hardware before depending on it:** whether HA's
  OTBR add-on enables the Discovery Proxy, and the expectation that the
  Mosquitto add-on does *not* advertise `_mqtt._tcp` — so a clean DNS-SD browse
  for a broker finds nothing and the `homeassistant.local` heuristic is doing
  the work.
- **Credentials — irreducible.** The Mosquitto add-on requires a username and
  password unless anonymous access is deliberately enabled. No discovery
  mechanism produces a password. Something must carry it to the device.

So provisioning shrinks to credentials rather than disappearing. The intended
vehicle is a **browser-based setup page over WebSerial** — the pattern ESPHome
users already recognise, supported by Chrome and Edge on every desktop platform,
and installing nothing. The dongle already carries MS OS 2.0 descriptors for
WinUSB binding, so either WebSerial over a CDC interface or WebUSB against the
existing vendor interface is reachable.

**It is an optional step for tier 2, not a first-run flow.** Matter
commissioning already handles the network join (section 1), so a tier 1 user
never sees this page at all.

### 9.1 Topics

```
radiant/<bridge_id>/status                        retained, LWT
radiant/<bridge_id>/<binding_uuid>/status         retained, per-sensor liveness
radiant/<bridge_id>/<binding_uuid>/<field_id>     the value
radiant/<bridge_id>/<binding_uuid>/cmd/<field_id> inbound; becomes page 0x10
```

`binding_uuid`, not a device number — section 5.

### 9.2 Discovery is the descriptor, transcribed

The retained discovery message is generated from `binding_changed`, and every
field in it comes from somewhere the envelope already defines:

| Discovery key | Source |
|---|---|
| `unit_of_measurement` | the vocabulary's canonical unit, converted by `f_type` |
| `state_class` | `total_increasing` if the `accumulate` bit is set, else `measurement` |
| `device_class` | the vocabulary type, where one maps; omitted where none does |
| `suggested_display_precision` | derived from the field's `exp` and width |
| `expire_after` | the sparse heartbeat interval, or the channel period x 3 |
| `availability_topic` | the per-binding status topic |
| `unique_id` | `binding_uuid` + field id |

`expire_after` is the one to get right. It is the MQTT expression of the
envelope's rule that **"no data" must never be produced silently**
(`radiant-telemetry.md` section 8), and without it a strap that walks out of
range leaves its last reading on screen indefinitely — which for a
heart-rate-driven AC is not a stale number, it is a wrong actuator.

**This is the one contract in the bridge that we do not own.** Home Assistant
versions its discovery schema, and it will move. Keep the generator in one
place, keep it table-driven off the vocabulary, and treat a schema change as a
table edit.

### 9.3 Availability, three levels

Bridge (LWT), sensor (per-binding status), and value (`expire_after`). All
three are needed and none substitutes for another: the bridge can be up while a
strap is gone, and a strap can be present while one field has stopped
advancing.

---

## 10. Privacy, and the commercial cohort case

### 10.1 Normative from v1, even though the cohort sinks are not

Three rules, in the design from the first commit because none can be retrofitted
onto a bus that was not built for them:

1. **Binding is explicit opt-in.** No promiscuous ingest, ever. Section 5, and
   it is also the radio rule of section 7.2.
2. **Bounded, non-reconstructable per-person state only.** The bridge holds current value
   and rule state. It is not a logger. A device that keeps a history is a device
   that can be seized, subpoenaed or stolen with the history in it.

   **Amended for section 6.3.** This rule read "**zero** per-person retention",
   and the heart-rate histogram would have violated it silently — which is
   exactly the kind of quiet breach the rule exists to catch, so it is amended
   openly instead. The amended test has three parts, and the histogram passes
   all three:

   - **Bounded.** Fixed size, known at compile time — 76 bytes, and the store
     itself is capped at `CONFIG_RADIANT_BRIDGE_HR_PROFILES` slots (§6.4).
     Nothing grows with time or with session count.
   - **Non-reconstructable.** A marginal distribution over 38 bins carries no
     ordering and no timestamps. No workout, no session, no event and no
     sequence can be recovered from it. It is a shape, not a record.
   - **Forgetting.** The halve-on-saturation rule of §6.3 means old observations
     decay out rather than accumulating indefinitely.

   A raw sample log passes none of the three, which is the distinction the rule
   was written to draw in the first place. The residual risk is that ~76 bytes
   of coarse distribution is weakly identifying; it is small, it is stated, and
   rule 3 is what a deployment uses when small is not small enough.
3. **Aggregate-only is a build-time mode**, not a runtime setting. In that
   build the per-person sinks are not linked, so "it was configured wrongly" is
   not a possible incident. **`CONFIG_RADIANT_BRIDGE_HR_PROFILES` defaults to 0
   in that build**: no histograms, no personalisation, prior-only thresholds
   everywhere, which §6.3 makes a supported configuration rather than a
   degraded one.

The honest framing for a product page, and it is defensible: ANT+ heart rate is
**unencrypted on air already**, so anyone in range has it whether this bridge
exists or not. This design does not widen that, and it declines several
opportunities to.

That said — continuously broadcast individual heart rate is biometric data, and
in some jurisdictions and some employment contexts it is a regulated category.
That is a legal question this document does not answer and should not pretend
to; what it does is make the aggregate-only build the one a deployment can
choose without asking us for a feature.

### 10.2 Deferred to a later version

- **Cohort aggregate sinks** — `count_elevated`, `max_hr_band`, `any_critical`
  as a small fixed Matter endpoint set, so N people do not become N endpoints.
  Per-person commissioning, endpoint counts and hub entity explosion all bite
  well before the interesting deployment sizes.
- **The broker ACL model** for keeping individual streams separated.
- **Binding tables above ~8**, which is a radio budget question (section 7.2)
  before it is a memory one.

---

## 11. Silicon, because this is where the plan meets a wall

**The two columns are not "with and without a feature" — they are two different
products**, because Matter is what makes tier 1 exist at all (section 1). The
left column is the Matterless MQTT build reached by out-of-band provisioning;
it has no QR scan, no Home Assistant device entry, and no booleans.

| Target | Matterless MQTT (out-of-band provisioning) | Full build: Matter + MQTT |
|---|---|---|
| nRF52840 (1 MB flash / 256 KB RAM) | Fits | **Very tight.** OpenThread plus a Matter stack already runs near the flash limit before `radiant`, MPSL and USB are added. Expect to trade away OTA, or need external flash the dongle form factor does not have |
| nRF5340 | Comfortable | Comfortable — the network core runs 802.15.4 and MPSL, the application core runs Matter |
| nRF54L15 (1.5 MB NVM / 256 KB RAM) | Comfortable | Workable, and it is the part already on this bench |

Recommendation: **the full build is the product and it targets nRF54L15 or
nRF5340; the Matterless build is the nRF52840's escape hatch, not the entry
SKU.** Shipping the escape hatch as the entry product would mean the cheapest
dongle is the one that needs the most technical setup and shows the least in
Home Assistant, which is exactly backwards.

Board configs already exist for all three (`boards/`). Establish the flash
budget with a linked image before writing the Matter data model, not after —
this is a wall you hit at 95 % complete.

---

## 12. Phasing

Ordered so that the two risky things — the radio and the flash — are settled
before anything expensive is built on top of them.

| Phase | What | Risk retired |
|---|---|---|
| 1 | Sample bus + sink registry + the ANT+ `0x78` adapter. Sinks = USB only | None. A pure refactor, host-testable, mirrors into `tools/ant_pages.py`, no radio change |
| 2 | Binding table + rule evaluator §6.1–6.2 (activity outputs, prior-only zones), still USB-only | Fully testable on the mock radio: hysteresis, the asymmetric dwell, and the worn-gates-resting rule |
| 3 | **MPSL backend + the 802.15.4 coexistence gate (§7.4)** | The go/no-go. Independent of everything above and everything below |
| 4 | **Matter: commissioning and the §8.1 type map** — the four occupancy endpoints, plus `0x10` temperature and `0x11` humidity for any `0x60` source that announces them | The flash ceiling of §11, and it delivers tier 1 — the first end-to-end demo is strap → QR scan → four HA `binary_sensor`s, no host and nothing installed |
| 5 | MQTT-over-Thread sink + discovery generator + the WebSerial provisioning page | Tier 2. Adds the number in bpm |
| 6 | Personalisation §6.3–6.5: histogram, conjugate update, capped store, NVM on session end | Pure addition. No endpoint moves, because prior-only was already `n = 0` of the same path |
| 7 | Matter remainder: the MEI cluster, client binding, page `0x10` as the reverse path, and the §8.3b passenger cluster behind its default-off switch | — |
| 8 | BLE sink | — |

**Phases 4 and 5 used to be the other way round, and that was an ordering
bug rather than a preference.** MQTT runs over Thread, and Matter commissioning
is how the device gets onto Thread (section 1) — so the old phase 4 could not
have been demonstrated without the old phase 5 already existing. The only way to
have shipped MQTT first would have been to build the out-of-band provisioning
path first, which is the escape hatch and not the product.

Putting Matter first also front-loads the flash ceiling, which is the second of
the two risks this ordering exists to retire early, and it means the first
demo is the zero-install one.

**Temperature, humidity and pressure add no phase, and that is the test of
§8.1.** They are three rows in a table phase 4 has to build anyway, reachable
from any `0x60` node whose descriptor announces them, so the Matter half costs a
table edit. The prediction held: the ANT+ side of §8.1a — the Environment `0x19`
adapter and the common page 84 decoder — was built afterwards, from the primary
documents, and needed **no** change to the type map beyond adding the pressure
row. What did change the map was a Matter conformance detail (pressure's kPa
resolution) and a Matter storage detail (constants on external-storage
attributes), not anything about the ANT decode.

**What remains outstanding in the Matter plane is the CHIP integration itself.**
`apps/dongle_thread/matter.conf` exists and builds `CONFIG_CHIP=y` beside
RadiANT, which is where the flash, RAM, `CONFIG_BT` and timeslot risks get
retired as a build.

> **Corrected 2026-08-15.** This paragraph used to say there was "no Matter
> source in that image", and listed three things as not built. **Two of the
> three are now false**: `bridge.zap` and `bridge.matter` **are** vendored at
> `apps/dongle_thread/src/matter/default_zap/`, and `radiant_matter.c` **is**
> compiled under `CONFIG_ANT_DONGLE_MATTER_MAP`. What genuinely remains is
> `radiant_matter_attr_write()` still being a `__weak` no-op that has never
> written an attribute, plus packages E4 (an ANT `BridgedDeviceDataProvider`),
> E5 (an occupancy `MatterBridgedDevice` type) and E6 (the CHIP event-loop
> handoff).

Everything §8.1 and §8.1a describe is the data model behind that seam, driven
directly by ztest. The nRF5340 as a second target is separately outstanding —
see §11 and `docs/backends.md`.

**For the gym-occupancy use case the data model is already right**, which is
worth stating because it is the part that usually is not: `radiant_matter.c`
maps `RADIANT_FIELD_OCCUPANCY` to the Occupancy Sensor type, and endpoints are
allocated per `(source, field_id)`, so N treadmills naturally become N occupancy
endpoints — capped at `RADIANT_MATTER_MAX_ENDPOINTS` = **16**, which must stay
equal to `CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT` and
`CONFIG_BRIDGE_MAX_DYNAMIC_ENDPOINTS_NUMBER` (static-asserted). **For "a number
on the gym's website", though, MQTT is the path**: `mqtt_sink.c` works today,
emits HA MQTT Discovery, and implements §9.3's three availability levels
including `expire_after` — which is what stops an unplugged treadmill reading
"in use" forever and is the single most important correctness property there.
Its hard limits are real and none is a one-liner: no username, no password, no
TLS, and `CONFIG_ANT_DONGLE_MQTT_BROKER` must be a **numeric IPv6 literal**,
because there is no resolver and the code states that a hostname is "not a
supported input, not merely an unimplemented one".

**BLE is deliberately last.** Most heart-rate straps are already ANT+/BLE dual,
so re-broadcasting HR over BLE is the *least* valuable sink for the headline
case. Its real value is ANT-only sensors and range aggregation. Note also that
the BLE Heart Rate Service is connection-oriented and 1:1, so N sensors means N
advertising identities; broadcast-with-manufacturer-data is the better fit and
should be specified as such when its turn comes.

---

## 13. Explicitly not in v1

- **A vendor-specific Zigbee path.** Matter and MQTT over Thread cover the same
  network; a third application protocol on the same PHY earns nothing.
- **The bridge as a Thread Router, Leader or Border Router, or as a BLE mesh
  relay.** Not deferred — refused, on one radio, permanently. Section 7.5 and
  `decisions/0011-never-a-border-router.md`. A second 802.15.4 part on the board
  is the only route to a 2-in-1, and it is a BOM decision rather than a firmware
  one.
- **Sparse-node bridging on a combo device.** Section 7.3, and it is a radio
  fact rather than a scheduling decision.
- **`0x27` time interval** or any other vocabulary addition, until the type is
  allocated in `radiant-telemetry.md` and `tools/ant_pages.py` in the same
  change. Section 3.2.
- **Cohort aggregation.** Section 10.2.
- **A generic ANT-command-to-Matter-actuator relay**, and the two fan paths of
  §8.4a are not it. Relaying an arbitrary ANT+ command to an arbitrary Matter
  device needs ANT+ Controls `0x10` decoded — unimplemented, community and
  open-source evidence only (`device-profiles.md` §9) — and it needs an answer
  to "which command drives which actuator" that neither fan path has to give.
  Deferred rather than refused: if it is built, the bridge should describe
  itself as a switch and let the commissioner own the mapping, because a bridge
  that owns a command-mapping registry has acquired a configuration UI, and
  `radiant-telemetry.md`'s command vocabulary is explicit that the bridge gets
  no command vocabulary of its own.
- **Any security switch.** The bridge inherits the envelope's posture and adds
  nothing; a bridged deployment that needs `X_AUTH` waits for Phase 7 exactly
  like an unbridged one.
