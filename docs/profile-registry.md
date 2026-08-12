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
| `0x7D-0x7E` | Available, but adjacent to the block above. |
| `0x7F` | **Taken.** ANT+ Core Temperature. Do not claim. |

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

- **LR PHY** — the type uses the RadiANT long-range PHY: **Bluetooth LE Coded**,
  with the coding rate named in the cell. A long-range channel cannot share the
  merged RF 57 RX window, so every one of them splits the receiver's radio
  schedule the way a separate network would have. This is why it is a per-type
  opt-in and not a mode. See [ADR 0007](decisions/0007-long-range-phy.md).
- **Adaptive freq** — the type's nodes may operate off RF 57, moving to a
  quieter index on a countdown announced in page `0x13`. Same window-splitting
  cost: an off-57 channel cannot join the merged RF 57 window.

  **Corrected 2026-08-11 by [ADR 0012](decisions/0012-adaptive-frequency.md).**
  This bullet previously read "Discovery and pairing stay on RF 57 so a
  searching receiver still finds every node, but data lives wherever the node's
  descriptor says", copied from ADR 0005 axis 5. The first half is true and
  permanent — **the search sweep is 1 M on RF 57 forever** and no frequency axis
  multiplies it. The second half claims more than the mechanism delivers: a node
  that has moved is not transmitting on RF 57 at all, so a *wildcard* sweep does
  not find it. It is re-acquired from the descriptor a receiver already holds,
  from the sync handoff of page `0x12`, or from the bounded retry list ADR 0012
  publishes — at most five named indices, which is not a sweep and does not
  touch the sweep's "certain within one sweep" guarantee.

**`Adaptive freq`** is `no`, `yes`, or `per-node` (the node announces it in a
descriptor, so a consumer must be prepared for either).

**`LR PHY` carries a coding rate, not a yes.** The previous line of this
document predicted that `yes` would not be enough and named the vocabulary that
would replace it; ADR 0007 built the PHY and this is that replacement, using
those same names rather than a second set:

| Cell | Meaning |
|---|---|
| `no` | 1 M GFSK only. Every ANT+ compatibility type, permanently. |
| `s8` | LE Coded S=8, 125 kbit/s. The one rate this project implements. |
| `s2` | LE Coded S=2, 500 kbit/s. Defined in the vocabulary, deliberately **not** implemented — no build will transmit it. |
| `per-node` | The node announces its rate in the descriptor's schedule block; a consumer must read it before budgeting a window. |

The reason the cell is not `yes` is that a consumer cannot size a receive window
from "long range": at eight bytes an S=8 frame is ~1.3 ms against ~150 µs at
1 M, which is the difference between a slot that fits and one that does not.

On the wire the same vocabulary is three bits in the schedule block — `0`
uncoded (1 M), `1` LE Coded S=8, `2` LE Coded S=2, `3..7` reserved — see
`docs/radiant-telemetry.md` section 6, `TLM_CODING_*` in `tools/ant_pages.py`
and `enum profile_sched_coding` in `src/profiles/profile_schedule.h`. Those are
one vocabulary and not three, and `scripts/check_profile_registry.py` enforces
the column against it.

A consumer reading `no` in both columns knows the type is on the merged RF 57
window and costs nothing extra.

---

## Device types

<!-- radiant-registry: device-types -->

| Type | Name | Status | Claimant | Date | Period | LR PHY | Adaptive freq | Notes |
|---|---|---|---|---|---|---|---|---|
| `0x0B` | Bicycle Power | ant-plus-reserved | Garmin / ANT+ | 2025-06-30 | 8182 | no | no | Implemented in `tools/ant_pages.py`; byte-exact to Garmin's profile |
| `0x11` | Fitness Equipment (FE-C) | ant-plus-reserved | Garmin / ANT+ | 2025-06-30 | — | no | no | Compatibility target; not yet implemented in `tools/ant_pages.py`, so no period is recorded rather than a guessed one |
| `0x14` | Light Electric Vehicle (LEV) | ant-plus-reserved | Garmin / ANT+ | 2025-06-30 | — | no | no | Recorded so a claimant does not repeat the search. Spec D00001390 Rev 1.1 states period 8192 (4.00 Hz); the column stays `—` because this project has not implemented the type. Fields in `docs/device-profiles.md` 3.6. **This is why no e-bike type is claimed** |
| `0x19` | Environment | ant-plus-reserved | Garmin / ANT+ | 2025-06-30 | — | no | no | Temperature only, and it has **no sensor placement field**, which is why physiological temperature is a `0x60` recipe rather than an additive page here. Default period 65535 (0.5 Hz), alternate 8192, from third-party implementations rather than the spec. `docs/device-profiles.md` 3.7 |
| `0x60` | RadiANT Generic Telemetry | radiant | RadiANT project | 2026-08-08 | per-node | per-node | per-node | The envelope in `docs/radiant-telemetry.md`; period is announced in descriptor frame 0. Implemented in `src/profiles/` and mirrored in `tools/ant_pages.py`. `LR PHY` is `per-node` from ADR 0007: a `0x60` node may run LE Coded S=8, and announces the rate in its schedule block. `Adaptive freq` is `per-node` from ADR 0012: a `0x60` node may move to a quieter RF index, announcing it in page `0x13` first. A node that switched to private mode is exactly this type |
| `0x61` | RadiANT Continuous Glucose Monitor | radiant | RadiANT project | 2026-08-11 | 65535 | no | no | The `0x60` envelope with a pinned schema; `docs/device-profiles.md` 5.1. **`X_AUTH` is mandatory** — a forged reading has a physiological consequence, so an unverified reading is reported as no reading. Not implemented. **Claimed knowing an ANT+ Glucose profile existed as a 2013 members-only beta whose device type could not be verified** |
| `0x62` | RadiANT Smart Fan | radiant | RadiANT project | 2026-08-11 | 8192 | no | no | The `0x60` envelope with a pinned schema; `docs/device-profiles.md` 5.2. Period is 4 Hz because commands land near the node's own slot, so the period is the command latency. Not implemented |
| `0x78` | Heart Rate | ant-plus-reserved | Garmin / ANT+ | 2025-06-30 | 8070 | no | no | Implemented in `tools/ant_pages.py`; pages `0x00`-`0x04`. Half and quarter rates below. **Byte 0 bit 7 is a page-change toggle, so page numbers here are 7-bit** |
| `0x79` | Bike Speed and Cadence, combined | ant-plus-reserved | Garmin / ANT+ | 2025-06-30 | 8086 | no | no | Implemented in `tools/ant_pages.py`. **No page-number byte** |
| `0x7A` | Bike Cadence | ant-plus-reserved | Garmin / ANT+ | 2025-06-30 | 8102 | no | no | Period defined in `tools/ant_pages.py`; pages not implemented there |
| `0x7B` | Bike Speed | ant-plus-reserved | Garmin / ANT+ | 2025-06-30 | 8118 | no | no | Period defined in `tools/ant_pages.py`; pages not implemented there |
| `0x7F` | Core Temperature | ant-plus-reserved | Garmin / ANT+ | 2025-06-30 | — | no | no | Recorded so a claimant does not repeat the search, and because it corroborates `docs/device-profiles.md` 6.3's design decision. Vendor code states period 8192 (4 Hz); the column stays `—` because this project has not implemented the type. Confirmed via two independent vendor/open-source implementations (greenTEG/CoreBodyTemp's own code, `openant`) rather than a primary spec. Fields in `docs/device-profiles.md` 3.8 |

`Period` is the channel period in counts of 1/32768 s, `per-node` when the type
does not fix one, and `—` when this project has not implemented the type and
will not record a number it has not verified. Where a number is present and the
type appears in `tools/ant_pages.py`, the checker asserts they agree.

### Permitted channel periods, per compat profile

**A period is the one setting in the compat design that a receiver cannot skip
past.** An unknown page number is ignored; a wrong period means the channel
never opens at all, with no error anywhere that names the period as the cause.
`period` is a manufacturer setting under
`docs/decisions/0008-antplus-additive-pages-and-compat-security.md`, so the
choice is from a **closed set** rather than a rate somebody picks, and the set
is enumerated here and in `tools/ant_pages.py` where
`scripts/check_profile_registry.py` cross-checks the two.

<!-- radiant-registry: compat-periods -->

| Type | Rate | Period | Approx. | Constant in `tools/ant_pages.py` |
|---|---|---|---|---|
| `0x78` | standard (default) | 8070 | ~4.06 Hz | `HRM_PERIOD` |
| `0x78` | half | 16140 | ~2.03 Hz | `HRM_PERIOD_HALF` |
| `0x78` | quarter | 32280 | ~1.02 Hz | `HRM_PERIOD_QUARTER` |
| `0x0B` | standard (default) | 8182 | ~4.005 Hz | `BPWR_PERIOD` |

**Bicycle power has one rate and that is a recorded fact, not an omission.** No
reduced-rate variant is registered for `0x0B` because this project has not
verified one against its own code, and the registry's rule is that it does not
write down a number it has not verified. A compat power node uses 8182.

The Period column above records each profile's **default**, which is what the
device-type table carries; `HRM_PERIODS` and `BPWR_PERIODS` hold the full sets
and the checker asserts the default is a member of its own set. A node running
at a reduced rate spends **proportionally less** of its slots on RadiANT, not
more: Tier I's interval `T` is in seconds and is decoupled from the data rate,
so halving the message rate halves the number of data pages an attestation page
displaces.

## Pages

One row per page or contiguous page range, per device type. Ranges are written
`0xNN-0xMM` and are expanded before the duplicate check, so two rows that
overlap are an error.

For any type with status `radiant`, the rows must **partition `0x00..0xFF`
exactly** — no gaps, no overlaps. That is stricter than it needs to be and it
is deliberate: an unassigned page number should be a recorded decision, because
the unclaimed one is the one two implementations both reach for.

**Compat pages carry the token `RadiANT compat` in their Name column**, plus
`beacon` or `attestation` — for example `RadiANT compat beacon`. That is a
convention `scripts/check_profile_registry.py` looks for rather than a
decoration: it is how the checker finds the rows whose arity it has to police.
The allocation is **two page numbers, not three** — SWITCH and RETURN are frame
indices inside the beacon page's frame set — it is the same numbers in every
compat profile, every number is `<= 0x7F` because heart rate's byte 0 carries a
page-change toggle in bit 7, and **none of it may ever land on `0x79`**, which
has no page-number byte at all. See
`docs/decisions/0008-antplus-additive-pages-and-compat-security.md`.

### The compat allocation, and why these numbers

**Beacon `0x70`. Attestation `0x71`–`0x72`.** The same numbers on `0x0B` and on
`0x78`, so a receiver has one rule.

- **`0x70` is the beacon and also the nibble base of the attestation claim.**
  Byte `[0]` of an attestation page is `0x70 | subtype`, and that subtype is the
  same nibble the MAC'd nonce carries at position 9 — so the page byte is
  *derived* from the subtype rather than being a second, independent statement
  of it.
- **Why the attestation claim is two numbers and the allocation is still two,
  not three.** ADR 0008 pins the subtype nibble into byte `[0]`, and byte `[0]`
  is the page number. Neither tier has a spare bit anywhere else: Tier I spends
  `[1..2]` on the counter and `[3..7]` on a 40-bit tag, Tier II spends `[1]` on
  the window index and `[2..7]` on a 48-bit tag. So the two tiers **cannot**
  share one byte-`[0]` value, and the checker's rule is the one that reflects
  this — the attestation claim is *one contiguous, nibble-aligned claim* of one
  or two numbers. What ADR 0008 rejected was a **third independent claim**: a
  Tier II page allocated separately, elsewhere in the namespace, verified on its
  own terms. That is still rejected. The count that matters to the 7-bit
  namespace is the nibble block, and this design occupies one.
- **`0x73` is not used.** It is what subtype `0x03`, the SWITCH/RETURN
  announcement, would be under the same rule — and the announcement rides frames
  2 and 3 of the beacon page's own frame set instead, which is exactly how the
  third page number is avoided.
- **Why the `0x70` block.** ANT+'s own common pages sit at or below `0x57`
  within the 7-bit space, and the manufacturer-private range every other profile
  uses (`0xF0`–`0xFF`) is **unreachable on heart rate**, whose byte 0 cannot
  express anything `>= 0x80`. `0x70`–`0x72` is the high, sparse end of the space
  a heart-rate sensor can actually express.

**The residual risk, recorded rather than solved.** A manufacturer-specific page
number is only unique per manufacturer id, so a vendor's private page on `0x0B`
or `0x78` may already use `0x70`, `0x71` or `0x72`. This project has **no
authoritative list of vendors' private page usage and does not claim one**;
these numbers are unclaimed by ANT+'s own common and background pages and by
every profile in this registry, and that is the whole of the evidence. What
bounds the damage is that a collision fails closed rather than quietly: the
beacon carries a version nibble that a foreign page will not match, and an
attestation tag computed over a foreign page's bytes does not verify, so the
receiver reports `unverified` instead of believing it. A receiver that decodes
on page number alone has no defence, and that is the receiver's bug — the same
rule this registry already states for device types.

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
| `0x0B` | `0x70` | RadiANT compat beacon | `ant_pages.encode_compat_beacon` | Two frames, four during a switch countdown. Carries **no epoch** |
| `0x0B` | `0x71-0x72` | RadiANT compat attestation | `ant_pages.encode_compat_attest_tier1` | The base `0x70` bitwise-or the subtype nibble: `0x71` Tier I identity, `0x72` Tier II data. One contiguous nibble-aligned claim |
| `0x78` | `0x00` | Default heart rate | `ant_pages.encode_hr_default` | Bytes `[4..7]` are the same on every page of this type |
| `0x78` | `0x01` | Cumulative operating time | `ant_pages.encode_hr_cumulative_time` | Background page; u24 LE, 2 s units |
| `0x78` | `0x02` | Manufacturer information | `ant_pages.encode_hr_manufacturer` | Background page; 8-bit manufacturer id, not common page 80's 16 |
| `0x78` | `0x03` | Product information | `ant_pages.encode_hr_product` | Background page |
| `0x78` | `0x04` | Previous heartbeat time | `ant_pages.encode_hr_previous_beat` | Main page; two event times on one page give an R-R interval from one packet |
| `0x78` | `0x50` | Common page 80, manufacturer | `ant_pages.encode_common_80` | Byte-exact ANT+, and it carries the toggle in bit 7 like every page here |
| `0x78` | `0x51` | Common page 81, product | `ant_pages.encode_common_81` | Byte-exact ANT+ |
| `0x78` | `0x70` | RadiANT compat beacon | `ant_pages.encode_compat_beacon` | The same number as on `0x0B`, so a receiver has one rule |
| `0x78` | `0x71-0x72` | RadiANT compat attestation | `ant_pages.encode_compat_attest_tier1` | The same numbers as on `0x0B` |
| `0x79` | none | Combined speed and cadence | `ant_pages.encode_bsc_combined` | This device type has **no page-number byte**; byte 0 is the low half of the cadence event time, so it can never carry an additional page and gets no compat row, permanently |
| `0x60` | `0x00` | Descriptor | `ant_pages.encode_tlm_descriptor` | Frame set; the retained message. C: `profile_desc_encode` |
| `0x60` | `0x01-0x0F` | Data | `ant_pages.encode_tlm_data` | Field area packed MSB-first against the descriptor. C: `profile_data_encode` |
| `0x60` | `0x10` | Reliable command | `ant_pages.encode_tlm_command` | Sequence + inline 16-bit tag, idempotent. **Layout only** - the idempotency rule and the tag need a key, and land with the command path |
| `0x60` | `0x11` | Command acknowledge | `ant_pages.encode_tlm_command_ack` | Result code + inline 16-bit tag. Layout only, as above |
| `0x60` | `0x12` | Sync handoff | `ant_pages.encode_tlm_handoff` | Two frames under one page number. C: `profile_handoff_encode`. Carries **no epoch**, and that is a constraint rather than an omission |
| `0x60` | `0x13` | Frequency move | `ant_pages.encode_tlm_freq_move` | One-frame set; target RF index and a countdown in units of eight transmitted messages. C: `profile_freq_encode`. Receivers act on countdown EXPIRY, not on receipt, so every one of them retunes on the same message |
| `0x60` | `0x14-0x1F` | Reserved | — | Unassigned; a receiver ignores these |
| `0x60` | `0x20-0x2F` | Reserved for the security envelope | `docs/radiant-security.md` | Epoch and key-generation announcement, v2 TESLA key disclosure |
| `0x60` | `0x30-0x4F` | Reserved | — | Unassigned |
| `0x60` | `0x50` | Common page 80 | `ant_pages.encode_common_80` | Byte-exact ANT+ |
| `0x60` | `0x51` | Common page 81 | `ant_pages.encode_common_81` | Byte-exact ANT+ |
| `0x60` | `0x52` | Common page 82 | `ant_pages.encode_common_82` | Byte-exact ANT+, optional |
| `0x60` | `0x53-0xEF` | Reserved | — | Other ANT+ common pages and future RadiANT pages |
| `0x60` | `0xF0-0xFF` | Vendor-private | — | Never registered, never bridged |
| `0x61` | `0x00` | Descriptor | `ant_pages.encode_tlm_descriptor` | The `0x60` envelope unchanged. A profile type pins a schema, not a codec |
| `0x61` | `0x01-0x0F` | Data | `ant_pages.encode_tlm_data` | Schema `0x01` is the four fields of `docs/device-profiles.md` 5.1, all on page `0x01` |
| `0x61` | `0x10` | Reliable command | `ant_pages.encode_tlm_command` | Layout only, as on `0x60` |
| `0x61` | `0x11` | Command acknowledge | `ant_pages.encode_tlm_command_ack` | Layout only, as on `0x60` |
| `0x61` | `0x12` | Sync handoff | `ant_pages.encode_tlm_handoff` | As on `0x60`; carries no epoch |
| `0x61` | `0x13` | Frequency move | `ant_pages.encode_tlm_freq_move` | Allocated for one rule across every RadiANT type. This profile pins `Adaptive freq` to `no`, so a conforming node never sends it |
| `0x61` | `0x14-0x1F` | Reserved | — | Unassigned; a receiver ignores these |
| `0x61` | `0x20-0x2F` | Reserved for the security envelope | `docs/radiant-security.md` | Epoch and key-generation announcement, v2 TESLA key disclosure |
| `0x61` | `0x30-0x4F` | Reserved | — | Unassigned |
| `0x61` | `0x50` | Common page 80 | `ant_pages.encode_common_80` | Byte-exact ANT+ |
| `0x61` | `0x51` | Common page 81 | `ant_pages.encode_common_81` | Byte-exact ANT+ |
| `0x61` | `0x52` | Common page 82 | `ant_pages.encode_common_82` | Byte-exact ANT+, optional. Carries battery, so the schema spends no field on it |
| `0x61` | `0x53-0xEF` | Reserved | — | Other ANT+ common pages and future RadiANT pages |
| `0x61` | `0xF0-0xFF` | Vendor-private | — | Never registered, never bridged |
| `0x62` | `0x00` | Descriptor | `ant_pages.encode_tlm_descriptor` | The `0x60` envelope unchanged |
| `0x62` | `0x01-0x0F` | Data | `ant_pages.encode_tlm_data` | Schema `0x01` is the three fields of `docs/device-profiles.md` 5.2, all on page `0x01` |
| `0x62` | `0x10` | Reliable command | `ant_pages.encode_tlm_command` | The profile's whole control surface: command `0x02` set level and `0x04` set mode. No bespoke command page |
| `0x62` | `0x11` | Command acknowledge | `ant_pages.encode_tlm_command_ack` | Layout only, as on `0x60` |
| `0x62` | `0x12` | Sync handoff | `ant_pages.encode_tlm_handoff` | As on `0x60`; carries no epoch |
| `0x62` | `0x13` | Frequency move | `ant_pages.encode_tlm_freq_move` | Allocated for one rule across every RadiANT type. This profile pins `Adaptive freq` to `no`, so a conforming node never sends it |
| `0x62` | `0x14-0x1F` | Reserved | — | Unassigned; a receiver ignores these |
| `0x62` | `0x20-0x2F` | Reserved for the security envelope | `docs/radiant-security.md` | Epoch and key-generation announcement, v2 TESLA key disclosure |
| `0x62` | `0x30-0x4F` | Reserved | — | Unassigned |
| `0x62` | `0x50` | Common page 80 | `ant_pages.encode_common_80` | Byte-exact ANT+ |
| `0x62` | `0x51` | Common page 81 | `ant_pages.encode_common_81` | Byte-exact ANT+ |
| `0x62` | `0x52` | Common page 82 | `ant_pages.encode_common_82` | Byte-exact ANT+, optional. A mains-powered fan omits it |
| `0x62` | `0x53-0xEF` | Reserved | — | Other ANT+ common pages and future RadiANT pages |
| `0x62` | `0xF0-0xFF` | Vendor-private | — | Never registered, never bridged |

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
