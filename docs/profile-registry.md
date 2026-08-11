# The RadiANT device type and page registry

Checked by: `scripts/check_profile_registry.py` — run it with
`C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe scripts\check_profile_registry.py`.
It fails on a duplicate device type, a duplicate or overlapping page within a
type, a missing or empty required column, a device type outside 1..127, a value
outside a column's vocabulary, a page claimed here but not in
`docs/radiant-telemetry.md`'s page map (or the reverse), and a period that
disagrees with `tools/ant_pages.py`.

---

## Why this file exists

Garmin ran the ANT+ device type and page allocation until 30 June 2025, when
the membership and certification programs closed. Allocation is the one piece
of that role that genuinely needs a successor: two independent projects that
both pick device type `0x60` produce receivers that decode each other's frames
into confident nonsense, and there is no error to catch it — the CRC passes,
the channel opens, the numbers are just wrong.

A file in a public repository plus a pull-request queue is the cheapest
legitimate successor. It is not authoritative in the way Garmin's list was. It
is a **coordination point that costs nothing and works if people use it**, and
the alternative is no coordination at all.

## The collision risk, stated honestly

**Garmin has stopped allocating. "Stopped" is not "guaranteed never."**

This registry cannot reserve anything against Garmin. If Garmin resumes
allocation, or if an allocation made before the shutdown is simply not publicly
visible, a type claimed here can collide with an official one. Three things
reduce the damage and none of them eliminate it:

1. Claim from the recommended range below, which is where public evidence of
   ANT+ allocation is thinnest.
2. A receiver must treat a device type as a *hint*, not a proof. RadiANT's
   descriptor page carries an envelope version and a schema id precisely so
   that a receiver can notice it is looking at something it does not
   understand. A receiver that decodes on device type alone has no defence
   against a collision, and that is the receiver's bug.
3. Record everything here, including third-party claims, so that a collision is
   discoverable by reading one file.

The registry does **not** enumerate Garmin's full allocation list. Only the
types this project implements or decodes are recorded below as
`ant-plus-reserved`. Before claiming a type, check it against the ANT+ device
profile list as well as against this file; absence here is not evidence of
availability.

## Recommended claim range

| Range | Use |
|---|---|
| `0x01-0x2F` | Dense with ANT+ allocations. Do not claim. |
| `0x30-0x5F` | Sparse ANT+ allocations. Claim only after checking. |
| **`0x60-0x6F`** | **Recommended.** No public evidence of ANT+ allocation. |
| `0x70-0x76` | Sparse. Claim only after checking. |
| `0x77-0x7C` | Dense with ANT+ allocations (weight scale, HRM, bike speed and cadence, SDM). Do not claim. |
| `0x7D-0x7F` | Available, but adjacent to the block above. |

Device type is **7 bits — the MSB is the pairing bit — so the space is 1..127
(`0x01..0x7F`)**, and `0x00` is not a device type. That leaves a substantial
unallocated range even after Garmin's.

## How to claim

1. Open a pull request against **this file**.
2. Add one row to the device-type table and one row per page to the page table.
3. Fill every column. `scripts/check_profile_registry.py` rejects an empty one.
4. **First merged wins.** Not first opened, not first announced — first merged,
   because that is the only event with an unambiguous timestamp that both
   parties can see.
5. A claim with no public page schema is not a claim. The `Schema` column must
   point at a document, a header, or a decoder somebody else can implement
   against.

Withdrawing a claim leaves the row in place with status changed and the date
kept. A retired type is not recycled; the space is large and confused
deployments are expensive.

## Device numbers: the provisioning rule

A device type is claimed here. A device **number** is not — it is per-unit, and
this section is the rule for choosing one. It applies to **every device type in
this registry, including the ANT+ compatible ones**, which is deliberate: those
are the types with a real anonymity set, so they are where the rule buys the
most.

**Tier 0, and it is the default. A RadiANT node derives its 16-bit device number
randomly at first provisioning and persists it.** `tools/ant_identity.py
provision` generates one.

This is not a security feature. It is a correction: on an ANT+ sensor the device
number is typically a **factory serial**, so it ties the node to a purchase
record, a warranty, and a previous owner, and anyone in range can follow it
across sessions and locations. Encryption never touched this — the number is
outside the payload because the receiver's hardware filter has to match on it.
Sell or gift a Tier 0 sensor and it is genuinely a different device.

**Zero UX change: no receiver ever loses a sensor**, which is the whole reason
this can be the default. The number is chosen once, at provisioning, and never
moves while a channel is open.

Two further tiers are **opt-in and off by default** — re-roll on explicit user
action, and re-roll at every power-up. `docs/radiant-security.md` section 4 is
normative for them, including the cost Tier 2 imposes on standard receivers.
`docs/decisions/0006-security-v1-scope-and-x-priv-withdrawal.md` records why
continuous per-epoch rotation was rejected rather than deferred.

**The 16-bit birthday bound is real and is not new.** Collisions become likely
around 300 devices in range. ANT+ already lives with that — device type and
transmission type have to match as well — and a random number is no worse than a
sequential serial here, only differently distributed.

**And the rule that matters more than any of this:** a node with a privacy
posture must emit page 81 with `serial = 0xFFFFFFFF`, suppress page 82 or zero
its operating time, and report a generic manufacturer, model and revision in
page 80. A 32-bit globally unique serial broadcast in the clear every 30 seconds
is strictly more identifying than the device number, and it is unaffected by any
re-roll. A node that re-rolls its device number and keeps broadcasting its
serial has not changed identity; it has added a field.

## The two schedule-splitting opt-ins

Two columns exist because they are the things a *consumer* of a device type has
to know before it opens a channel, and neither is visible from the frame:

- **LR PHY** — the type uses the RadiANT long-range GFSK variant. A long-range
  channel cannot share the merged RF 57 RX window, so every one of them splits
  the receiver's radio schedule the way a separate network would have. This is
  why it is a per-type opt-in and not a mode.
- **Adaptive freq** — the type's nodes may operate off RF 57. Discovery and
  pairing stay on RF 57 so a searching receiver still finds every node, but
  data lives wherever the node's descriptor says. Same window-splitting cost.

Values are `no`, `yes`, or `per-node` (the node announces it in a descriptor,
so a consumer must be prepared for either). A consumer reading `no` in both
columns knows the type is on the merged RF 57 window and costs nothing extra.

---

## Device types

<!-- radiant-registry: device-types -->

| Type | Name | Status | Claimant | Date | Period | LR PHY | Adaptive freq | Notes |
|---|---|---|---|---|---|---|---|---|
| `0x0B` | Bicycle Power | ant-plus-reserved | Garmin / ANT+ | 2025-06-30 | 8182 | no | no | Implemented in `tools/ant_pages.py`; byte-exact to Garmin's profile |
| `0x11` | Fitness Equipment (FE-C) | ant-plus-reserved | Garmin / ANT+ | 2025-06-30 | — | no | no | Compatibility target; not yet implemented in `tools/ant_pages.py`, so no period is recorded rather than a guessed one |
| `0x60` | RadiANT Generic Telemetry | radiant | RadiANT project | 2026-08-08 | per-node | no | per-node | The envelope in `docs/radiant-telemetry.md`; period is announced in descriptor frame 0. Implemented in `src/profiles/` and mirrored in `tools/ant_pages.py` |
| `0x78` | Heart Rate | ant-plus-reserved | Garmin / ANT+ | 2025-06-30 | — | no | no | Compatibility target; not yet implemented in `tools/ant_pages.py` |
| `0x79` | Bike Speed and Cadence, combined | ant-plus-reserved | Garmin / ANT+ | 2025-06-30 | 8086 | no | no | Implemented in `tools/ant_pages.py`. **No page-number byte** |
| `0x7A` | Bike Cadence | ant-plus-reserved | Garmin / ANT+ | 2025-06-30 | 8102 | no | no | Period defined in `tools/ant_pages.py`; pages not implemented there |
| `0x7B` | Bike Speed | ant-plus-reserved | Garmin / ANT+ | 2025-06-30 | 8118 | no | no | Period defined in `tools/ant_pages.py`; pages not implemented there |

`Period` is the channel period in counts of 1/32768 s, `per-node` when the type
does not fix one, and `—` when this project has not implemented the type and
will not record a number it has not verified. Where a number is present and the
type appears in `tools/ant_pages.py`, the checker asserts they agree.

## Pages

One row per page or contiguous page range, per device type. Ranges are written
`0xNN-0xMM` and are expanded before the duplicate check, so two rows that
overlap are an error.

For any type with status `radiant`, the rows must **partition `0x00..0xFF`
exactly** — no gaps, no overlaps. That is stricter than it needs to be and it
is deliberate: an unassigned page number should be a recorded decision, because
the unclaimed one is the one two implementations both reach for.

<!-- radiant-registry: pages -->

| Type | Page | Name | Schema | Notes |
|---|---|---|---|---|
| `0x0B` | `0x10` | Standard Power Only | `ant_pages.encode_power_std` | acc_power and inst_power, LE |
| `0x0B` | `0x11` | Wheel Torque | `ant_pages.encode_power_torque` | Four independent accumulators |
| `0x0B` | `0x12` | Crank Torque | `ant_pages.encode_power_torque` | Four independent accumulators |
| `0x0B` | `0x20` | Crank Torque Frequency | `ant_pages.encode_power_torque_freq` | **Big-endian**, unlike every other page here |
| `0x0B` | `0x50` | Common page 80, manufacturer | `ant_pages.encode_common_80` | Interleaved at message 119 of 121 |
| `0x0B` | `0x51` | Common page 81, product | `ant_pages.encode_common_81` | Interleaved at message 120 of 121 |
| `0x0B` | `0x52` | Common page 82, battery | `ant_pages.encode_common_82` | Optional |
| `0x79` | none | Combined speed and cadence | `ant_pages.encode_bsc_combined` | This device type has **no page-number byte**; byte 0 is the low half of the cadence event time |
| `0x60` | `0x00` | Descriptor | `ant_pages.encode_tlm_descriptor` | Frame set; the retained message. C: `profile_desc_encode` |
| `0x60` | `0x01-0x0F` | Data | `ant_pages.encode_tlm_data` | Field area packed MSB-first against the descriptor. C: `profile_data_encode` |
| `0x60` | `0x10` | Reliable command | `ant_pages.encode_tlm_command` | Sequence + inline 16-bit tag, idempotent. **Layout only** - the idempotency rule and the tag need a key, and land with the command path |
| `0x60` | `0x11` | Command acknowledge | `ant_pages.encode_tlm_command_ack` | Result code + inline 16-bit tag. Layout only, as above |
| `0x60` | `0x12` | Sync handoff | `ant_pages.encode_tlm_handoff` | Two frames under one page number. C: `profile_handoff_encode`. Carries **no epoch**, and that is a constraint rather than an omission |
| `0x60` | `0x13-0x1F` | Reserved | — | Unassigned; a receiver ignores these |
| `0x60` | `0x20-0x2F` | Reserved for the security envelope | `docs/radiant-security.md` | Epoch and key-generation announcement, v2 TESLA key disclosure |
| `0x60` | `0x30-0x4F` | Reserved | — | Unassigned |
| `0x60` | `0x50` | Common page 80 | `ant_pages.encode_common_80` | Byte-exact ANT+ |
| `0x60` | `0x51` | Common page 81 | `ant_pages.encode_common_81` | Byte-exact ANT+ |
| `0x60` | `0x52` | Common page 82 | `ant_pages.encode_common_82` | Byte-exact ANT+, optional |
| `0x60` | `0x53-0xEF` | Reserved | — | Other ANT+ common pages and future RadiANT pages |
| `0x60` | `0xF0-0xFF` | Vendor-private | — | Never registered, never bridged |

The page-map table in `docs/radiant-telemetry.md` and the `0x60` rows here are
cross-checked against each other, page number and name, so the two cannot
drift.

---

## Not claimed, and why

- **A dedicated CdA (aerodynamic drag) device type** for the sibling
  `zephyr_aerosense` sensor. It ships on `0x60` first. A device type is worth
  claiming once its fields have settled, and a registry that fills with
  aspirational entries is worth less than one that does not.
- **A long-range PHY type.** The column exists, nothing uses it, and the axis
  is deliberately not in v1.
