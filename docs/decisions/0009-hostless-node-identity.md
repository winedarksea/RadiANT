# 0009 — The hostless node: one monotonic counter closes both holes

Checked by: nothing automated, deliberately. This ADR reopens a deferred
decision and specifies where a counter lives; the assertions belong to the phase
that implements it — the boot counter advancing across a simulated reboot, the
pairing counter advancing **before** the public key is transmitted, and the
`caps.has_rng` seam compiling out. What
`scripts/check_profile_registry.py` does check is the pin this ADR shares with
ADR 0008: `docs/radiant-security.md` section 7.4 states that the host-supplied
scalar is not the answer for a hostless node and names this decision.

- **Status:** Accepted.
- **Date:** 2026-08-11
- **Related:** [0006 security v1 scope](0006-security-v1-scope-and-x-priv-withdrawal.md),
  [0008 additive pages and compat security](0008-antplus-additive-pages-and-compat-security.md),
  `../radiant-security.md` sections 3.5, 4, 7.4 and 11

## Context: two decisions that are one hole

Two pinned decisions in the security design are stated separately and are the
same gap:

- **The epoch authority is the host.** No transform enables until `0xF3` has
  advanced the epoch, and `radiant_sec_set_epoch()` refuses an epoch <= the
  current one. NVM epoch ratcheting inside `radiant` is explicitly deferred
  (`../radiant-security.md` section 3.5).
- **Pairing randomness comes from the host.** `radiant_sec_pair_set_scalar()`
  takes 32 bytes over `0xF5` because "no entropy driver, no PSA, no CRACEN, on
  any target" is a pinned decision (section 7.4).

Both are correct for a dongle behind a PC. **A battery heart-rate strap has no
host.** It cannot receive `0xF3`, so it can never enable a transform; it has no
CSPRNG, so it can never generate a pairing scalar. Today's design assumes a node
behind a dongle behind a PC, and that assumption does not survive contact with
the device ADR 0008 exists to build.

Neither hole can be patched where it appears. A node that fabricates an epoch
from nothing reuses keystream after a reboot; a node with a weak on-node PRNG
produces a pairing scalar an attacker can guess, which is worse than not pairing
at all. The fix has to be *state that survives a power cycle*, and there is only
one kind of that.

## Decision

1. **A hostless node persists a monotonic boot counter in NVM**, `u32`, `+1` at
   every boot, never reset. Where no clock exists, **that counter is the epoch**.
   Where a clock exists, the epoch is coarse real time — minutes since a fixed
   RadiANT date — which is section 3.5's recommended discharge and is monotone
   for free.
2. **`K_dev` is a per-device secret provisioned at manufacture.** It is also the
   identity Tier 0 device-number source, so it is needed regardless of anything
   in this ADR.
3. **The pairing scalar is deterministic:**
   `KDF(K_dev, "pair" || pair_counter)`, where `pair_counter` is a second `u32`
   in NVM, `+1` per pairing window entered.
4. **`pair_counter` advances before the public key is transmitted**, never after
   the pairing completes. This rule is load-bearing and its reasoning is below.
5. **An optional PSA/CRACEN entropy backend sits behind its own Kconfig**, for
   parts where `nrf_security` is already linked, exposed to policy as a
   `caps.has_rng` bit. **Policy never names a backend** — the same seam
   discipline the crypto backends already keep.
6. **This is a deliberate reopening of NVM ratcheting**, which ADR 0006 and
   `../radiant-security.md` section 3.5 deferred. It is reopened as a decision,
   not smuggled in as an implementation detail. See below for why it is cheaper
   than the version that was deferred.
7. **The counters live in the node application, not in `radiant`.**
   `radiant` still only ever receives an epoch through its existing API.

```
boot_counter    u32 in NVM, +1 at every boot, never reset
epoch           = boot_counter where no clock exists;
                  coarse real time (minutes since a fixed RadiANT date) where one does
pair_counter    u32 in NVM, +1 per pairing window entered, advanced BEFORE the
                  public key goes on the air
pairing scalar  = KDF(K_dev, "pair" || pair_counter)
K_dev           per-device secret provisioned at manufacture; also the Tier 0
                  device-number source, so it is needed regardless
```

## Why the counter must advance before the pubkey is transmitted

This is the rule most likely to be written the other way round by somebody
implementing it, because "commit the counter when the pairing succeeds" is the
instinct every transactional system trains. It is wrong here, in two distinct
ways, and both close a leak this project has already paid to close once.

**A repeated `pair_counter` means a repeated X25519 ephemeral.** It does not leak
the shared secret. It does two other things:

- **It destroys forward secrecy.** Two pairings under the same scalar share a
  private key, so a later compromise of `K_dev` retroactively unlocks both,
  rather than the exchange being independent per pairing.
- **It makes the node's pairing public key a stable cross-session identifier.**
  A node that broadcasts the same 32-byte public key every time it enters a
  pairing window is broadcasting a permanent unique name — **an identity Tier 2
  leak reintroduced through the back door**, after ADR 0006 spent an entire
  decision establishing that a re-rollable identity is the point and that a
  fixed-serial broadcast defeats every tier above it. A design that removes the
  epoch from the beacon to avoid a fingerprint, and then emits an invariant
  public key during enrolment, has moved the fingerprint rather than removed it.

**Advance-after leaves a window where both happen for free.** A pairing that is
abandoned — the user walks away, the peer never answers, the window times out,
the battery dies mid-exchange — never "completes", so an advance-on-completion
implementation reuses the same scalar on the next attempt. Abandoned pairings are
the *common* case in a room where somebody is holding a button on a strap, so the
repeated-ephemeral condition would be the normal path rather than the rare one.

**The rule, stated so it can be asserted:** the counter is incremented and the
new value is durably written **before** the derived public key leaves the radio.
If the write fails, the node does not enter the pairing window. Fail closed; a
node that cannot advance its counter has nothing safe to say.

The same fail-closed rule applies to the boot counter: **if the boot counter
cannot be advanced and persisted, no transform enables.** That is exactly the
`0xF3` refusal, relocated to the node, and it keeps section 3.5's invariant —
never reissue an epoch — enforceable by the party that actually owns it.

## Why this is cheaper than the ratcheting that was deferred

The deferred proposal was an NVM ratchet inside `radiant`, written per
epoch. Two things make this one different, and they are the reason it is
acceptable now when that one was not:

| | Deferred version | This decision |
|---|---|---|
| Write frequency | per epoch | **once per boot, once per pairing window** |
| Where it lives | inside `radiant` | **in the node application** |
| What `radiant` learns | a persistence policy | nothing — it still receives an epoch through the existing API |
| Flash wear | proportional to session length | proportional to power cycles |

Write frequency is the whole argument. A strap that is worn twice a day for ten
years boots on the order of 7 000 times; a per-epoch ratchet on the same device
writes orders of magnitude more often, and the number is unbounded in session
length rather than bounded by user behaviour. The actual endurance headroom
against the storage backend's wear levelling is a number to measure in the
implementation phase, not to assert here — but the ratio between the two designs
is not in doubt.

Location is the second argument and it protects an architectural property rather
than a budget. `radiant` has one job at this boundary: it is told an epoch
and it refuses one that has not advanced. Putting persistence *inside* it would
make every consumer inherit a flash dependency, a storage backend and a failure
mode, to serve one class of node. The node application already owns its own NVM
for the Tier 0 device number, so the counter costs it a key, not a subsystem.

## The entropy backend, and the seam it must not break

A deterministic scalar is a **replacement** for a CSPRNG, not a preference over
one. Where real entropy exists it should be used, and on nRF54L it does exist —
`psa_rng`/CRACEN — for builds that already link `nrf_security`.

- The backend sits behind its own Kconfig, off unless selected.
- Its presence is visible to policy as a **`caps.has_rng` bit** and nothing else.
  **Policy never names a backend.** This is the same discipline the crypto
  backends keep, and the reason `radiant_sec.c` contains no call to
  `radiant_sec_aes_ecb()`.
- With `has_rng` set, the pairing scalar comes from the RNG and `pair_counter`
  advances anyway, so a build that flips the bit does not change the counter's
  meaning or its monotonicity.
- The host path is unchanged: `0xF5` still supplies a scalar for a
  host-attached node, and it still takes precedence where it is used.

Three sources of one scalar means a precedence rule, stated once so it is not
discovered later: **host-supplied (`0xF5`) if present, else the RNG when
`caps.has_rng`, else `KDF(K_dev, "pair" || pair_counter)`.** All three advance
`pair_counter` before transmission.

## Consequences

- **The strap ADR 0008 exists to build is now buildable.** A hostless node can
  hold an epoch across reboots, refuse a stale one, enable a transform, and
  complete an over-air pairing without a weak PRNG or a host.
- **`K_dev` becomes load-bearing at manufacture.** It was already the Tier 0
  device-number source; it is now also the pairing-scalar root, so a device
  shipped without one has neither a stable identity nor a pairing path. Its
  provisioning is a manufacturing step, and a factory reset that re-rolls the
  node's identity re-provisions `K_dev` with it. **The counters are never reset
  independently of `K_dev`** — a reset counter under an unchanged key is a
  repeated scalar and a replayed epoch, which is the exact failure this ADR
  exists to prevent.
- **The epoch is a boot counter, which is why it is not broadcast.** ADR 0008's
  removal of the epoch field from the beacon and from the SWITCH frame depends on
  this decision: a slowly incrementing number tied to power cycles is a
  fingerprint. The two ADRs land together for that reason.
- **A hostless node's epoch does not survive re-provisioning by itself.** A
  receiver that has seen a sensor before recovers a stale epoch by forward search
  (ADR 0008); a re-provisioned sensor whose counter moved backward is recovered
  by a **bounded** absolute scan, never an unbounded loop.
- **`../radiant-security.md` section 7.4's limit is amended rather than
  deleted.** The unauthenticated-Diffie-Hellman fingerprint problem is untouched
  by anything here; only the "a hostless node cannot pair at all" half is closed.

## What is still not solved

- **Rollback of the whole NVM image.** An attacker with physical access who can
  restore an old flash image restores an old counter, and everything above
  depends on the counter being monotone. Physical possession of the sensor is
  outside this threat model, and a node with a secure element would place the
  counter there; v1 does not have one.
- **A node with no NVM at all.** It cannot do any of this, and it is not a
  RadiANT security target. Say so rather than degrading gracefully into a scheme
  that looks secure.
- **Clock-free coarse time.** Where no clock exists the epoch counts boots, not
  seconds, so nothing in the system may infer elapsed real time from the epoch.
  The attestation counter is the time-derived quantity; the epoch is not.
