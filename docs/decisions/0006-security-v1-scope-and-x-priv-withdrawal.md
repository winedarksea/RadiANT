# 0006 — Security v1 is `X_AUTH` + `X_CONF`; `X_PRIV` is withdrawn, not deferred

Checked by: `scripts/check_profile_registry.py`, which asserts that the pins
this decision creates are still stated in `docs/radiant-security.md` and
`docs/radiant-telemetry.md` — the nonce's domain byte, the legal window set, the
tag length, the epoch-advance rules, the descriptor authentication frame, the
common-page privacy rules, and descriptor INFO bit 3 as reserved-must-be-zero.
The zero-cost claim is checked by the `zero-cost` CI job rather than by this
document.

- **Status:** Accepted.
- **Date:** 2026-08-10
- **Related:** [0002 clean-room policy](0002-clean-room-policy.md),
  [0005 extension inside ANT+](0005-extension-inside-ant-plus.md),
  `../radiant-security.md`, `../radiant-telemetry.md`, `../profile-registry.md`

## Decision

1. **v1 security is two per-channel payload transforms: `X_AUTH` and `X_CONF`.**
   Both are confined to two hook points in `radiant_core/src/radiant_api.c`.
   Neither touches the address path, the search policy, the RX filters, the
   scheduler or the device ID lists.
2. **`X_PRIV` — continuous per-epoch device-number rotation — is rejected.** Not
   deferred, not "v2": rejected, with the reasons recorded below so that it is
   not re-proposed from first principles by somebody who has only read the
   original three-switch draft.
3. **What replaces it is a provisioning rule, in three tiers**, of which only
   the free one is on by default. See `../radiant-security.md` section 4.
4. **The reserved space `X_PRIV` used stays reserved**: descriptor frame 0 INFO
   bit 3 is reserved-must-be-zero rather than reused, and the descriptor's
   4-byte epoch field stays, because `X_CONF` still needs the epoch as a
   key-rotation clock. Revisiting rotation later is therefore a
   format-compatible change rather than a break.
5. **Everything except identity Tier 0 is off by default**, and the zero-cost
   claim is a CI job rather than an intention.

## Why `X_PRIV` cannot work here

The scheme was `devnum_e = trunc16(HMAC(K_id, epoch))` with
`epoch = floor(t / 128 s)`. It fails on its own terms, before any cost argument.

**The anonymity set is empty by construction.** Rotation breaks standard
receivers — a Garmin head unit or Zwift cannot follow a device number that
changes every 128 seconds — so it is usable only on RadiANT-only device types,
whose deployed population is approximately zero. A rotating node is a category
of one, and an anonymity set of one is not an anonymity set. This is the
argument that decides it; the rest are confirmations.

**Five leaks survive it.**

| Leak | Why rotation does not close it |
|---|---|
| Device type and transmission type | Must stay fixed, because a searching receiver matches on them |
| Period, RF channel, schema id | Announced in the descriptor and stable across a boundary |
| Event counter | Links across a boundary by `+1` |
| Timing phase | Continuous through a boundary |
| Descriptor INFO bit 3 | Announced "I rotate" **in the clear** |

And a sixth that is structural rather than incidental: `floor(t / 128 s)` makes
**every node rotate on the same global tick**, which is simultaneously a
correlated demand spike on the receiver and a linkage signal.

**It cannot work below about 1 Hz at all.** A clockless receiver's only epoch
source is the descriptor, which arrives every 121 messages — 3.8 epochs at
0.25 Hz. The envelope exists to serve nodes slower than 1 Hz.

**Its entire cost sits in the part that delivers least.** The cost curve of
identity rotation has one cliff in it, and the cliff is *mid-session* rotation:

| Rotation trigger | Link-layer cost | Receiver re-pairs? |
|---|---|---|
| Never (ANT+ today: a factory serial) | — | no |
| **First boot only — random, then stable** | **none** | **no** |
| Explicit user action | none | yes, and the user asked for it |
| Every power-up | none | yes, every session |
| Every 128 s (`X_PRIV`) | the whole address path | yes, constantly |

Everything above the last row is free, because a rotation never happens while a
channel is open. Crossing that line buys, all at once: the dual-filter guard
window consuming both nRF address BASEs and blocking merged RX windows; the
device ID inclusion/exclusion list never matching again; a stale device number
snapshotted into every transfer; one dead seen-cache entry per epoch; roughly
675 phantom devices per node per day in `tools/ant_scan.py`; and a monotone
real-time-clock requirement that a coin cell can only meet with flash writes.

## What is gained, and what is honestly lost

**Gained, for free:** a device number that is random at first provisioning
rather than a factory serial, so it no longer ties a node to a purchase record,
a warranty or a previous owner — including on ANT+ compatible device types,
which is the population with a real anonymity set. Zero UX change; no receiver
ever loses a sensor.

**Gained, opt-in:** re-roll on user action (Tier 1) and re-roll at power-up
(Tier 2). Tier 2 answers the stalking case — a worn strap power-cycles between
rides, so a receiver planted near your house sees a different node each time —
and is tolerable only because a **keyed RadiANT receiver re-acquires
cryptographically**: it wildcard-searches its device type and asks whether
`X_AUTH` verifies under its key. Resolution at power-up granularity costs
nothing beyond `X_AUTH` and the ordinary search path; resolution at 128-second
granularity costs the address path. That price difference is the entire
argument, and it is the same one BLE settled with resolvable rather than merely
random private addresses.

**Lost, and stated in `../radiant-security.md` section 7.1:** an observer who
sees you twice inside one session links those two sightings — a fleet catching
you at km 0 and km 100 of one ride, or on entry and exit from one gym visit.
`X_PRIV` would have covered that. Nothing here does.

Worth recording because it bounds the loss: an always-on mains-powered node — a
thermostat, a door sensor — never reboots, so Tier 2 gives it one identity for
months. But a stationary node is trivially linkable by RSSI and by simply always
being there, so continuous rotation buys it nothing either. **The case where the
cheap scheme is weak is the case where the expensive scheme was also useless.**

## The highest-value privacy change was not a switch at all

Page 81 broadcasts a **32-bit globally unique serial number in the clear** every
30 seconds or so, and page 82 a monotone operating-time counter that survives an
identity change and fingerprints a battery swap. Each leaks strictly more than
the 16-bit device number ever did, and no amount of device-number rotation
touches either.

The fix is one normative rule, independent of every switch and every tier: page
81 carries `serial = 0xFFFFFFFF` — the "not supplied" sentinel `ant_pages.py`
already defines — page 82 is suppressed or zeroed, and page 80 reports generic
manufacturer, model and revision. A node that re-rolls its device number and
keeps broadcasting its serial number has not changed identity; it has added a
field.

## Consequences

- `docs/radiant-security.md` describes two switches and an identity rule. Its
  section 3.7 carries the short form of this ADR so a reader of that document
  alone does not re-derive the withdrawal.
- Descriptor INFO bit 3 and frame 1's epoch field are reserved, not reclaimed.
- The address-group defect that `X_PRIV`'s guard window would have collided with
  is **independently a correctness bug** and is fixed on its own merits: the nRF
  backend matches at most two distinct address BASEs per RX window, and nothing
  in the scheduler knew that. It survives this withdrawal.
- Unlinkability is not claimed anywhere. The claim is narrower and true: *an
  identity that is not a factory serial and can be re-rolled at will.*
