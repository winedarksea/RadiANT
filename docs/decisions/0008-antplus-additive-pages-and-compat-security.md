# 0008 — Additive pages on ANT+ device types, and a two-tier compat security layer

Checked by: `scripts/check_profile_registry.py`, which asserts that the pins
this decision creates are still stated in `docs/radiant-security.md` section 11
and `docs/radiant-telemetry.md` section 2 — the compat domain byte `0x04` and
its subtypes, the 40-bit and 48-bit tag lengths, `N in {4, 8, 16, 32}`,
`K in {16, 32, 64, 128}`, the derived-locator formula, the policy default and
its precedence rule, the two-page allocation, and `0x79`'s permanent exclusion —
and which checks the shape of any compat page rows in
`docs/profile-registry.md`. The 2.0%-of-slots claim is checked by a Tier 4
capture, not by this document.

- **Status:** Accepted.
- **Date:** 2026-08-11
- **Related:** [0002 clean-room policy](0002-clean-room-policy.md),
  [0005 extension inside ANT+](0005-extension-inside-ant-plus.md),
  [0006 security v1 scope](0006-security-v1-scope-and-x-priv-withdrawal.md),
  [0009 hostless node identity](0009-hostless-node-identity.md),
  `../radiant-security.md` section 11, `../radiant-telemetry.md` section 2,
  `../profile-registry.md`

## Context

`radiant_sec` secures **RadiANT-envelope channels**: device type `0x60`, the
telemetry envelope, pages `0x01..0x1F`, a spread tag in byte `[7]` of every data
page. A secured channel is by construction not an ANT+ channel, and
`CONFIG_RADIANT_SEC`'s help text says so.

This decision adds the other half: **a sensor that is a byte-exact ANT+ heart
rate monitor or power meter to every legacy receiver, and a verifiable — and on
demand, private — RadiANT sensor to a receiver holding its key.**

Four facts constrain everything below.

1. **No spare payload byte exists on a standard ANT+ page.** Power `0x10` ends in
   instantaneous power, `0x11`/`0x12` in accumulated torque, `0x20` in torque
   ticks. The existing `X_AUTH` spread tag takes byte `[7]` of every data page
   and therefore **cannot be used on a compat channel at all**. The tag needs its
   own page.
2. **Device type `0x79` can never carry an additional page.** Its payload has no
   page-number byte; byte 0 is the low half of the cadence event time. An
   inserted page decodes as speed and cadence and steps four accumulators.
3. **ADR 0005 axis 1 already permits additive pages** — *"new pages within
   existing device types. Zero risk; invisible to receivers that skip unknown
   page numbers."* No new axis is needed.
4. **But `../radiant-telemetry.md` section 2 clause 2 forbade them.** "Nothing in
   this document applies to them" was written to protect existing page layouts
   and, as written, blocked additive pages too. That clause is the one line
   standing in front of this design, and amending it is decision 1 below.

A fifth fact is a scope correction rather than a constraint: **ANT+ device
profiles are open spec and may be implemented anywhere in this library**
(maintainer ruling, 2026-08-10, amending ADR 0002). A page layout is a fact about an
interoperation target, which is the reasoning ADR 0002 already applied to
`src/profiles/`; extending it to `radiant_core/**` is a correction of scope, not
a relaxation. The clean room that remains is `libant.a` and sdk-ant, and nothing
here touches it.

## Decision

1. **`../radiant-telemetry.md` section 2 clause 2 is amended to permit additive
   pages on ANT+ device types**, while keeping the prohibition on modifying
   existing page layouts intact and unweakened, and while permanently excluding
   `0x79`. The amended clause is in that document; the split it makes is between
   *adding* a page number and *changing* one, which have nothing in common.
2. **The compat layer is three layers plus an operation, specified in
   `../radiant-security.md` section 11**: A a capability beacon, B attested clear
   broadcast in two tiers, C a private-mode switch, D adding a receiver to an
   existing network.
3. **Confidentiality on a compat channel is a switch, not an addition** — the
   governing insight below.
4. **Attestation is two tiers, roman-numbered. Tier I (identity) is on by
   default; Tier II (data) is off.** This is the centrepiece and it has its own
   section.
5. **The default configuration of a RadiANT compat sensor spends 2.0% of its
   slots on RadiANT** — 0.8% beacon + 1.2% Tier I — against the **1.65%** ANT+
   itself already spends on common pages 80 and 81.
6. **`private_policy` has four states — `never`, `physical`, `command`,
   `always` — and defaults to `never`.** A shipped strap is a plain ANT+ sensor
   unless somebody configures it otherwise, out of band.
7. **`CONFIG_RADIANT_SEC_COMPAT` is default n and depends on
   `CONFIG_RADIANT_SEC`**, with the zero-cost discipline of ADR 0006 inherited
   unchanged: no ops struct, no init call, no-op static inlines when off, a CI
   job rather than an intention.

## The governing insight, which decides the shape

> **Advertise, authenticate and encrypt are three different things with three
> different compatibility costs.** Advertising is free. Authenticating a clear
> stream is free to legacy receivers and is most of the value. Encrypting is not
> compatible with anything, ever, because a ciphertext page beside the plaintext
> page carrying the same value is theatre. So confidentiality is a **switch**,
> not an addition.

The theatre argument is worth stating as a mechanism rather than a slogan,
because "encrypted ANT+ heart rate" is what somebody will propose next. A compat
channel exists to be read by a Garmin head unit. Its data pages are therefore in
the clear. Adding an encrypted copy of the same value on another page protects
nothing — the plaintext is still on the air, one page number away, and the
"protected" reading is recoverable by reading the unprotected one. The only way
confidentiality means anything is if the clear stream **stops**, and a node whose
clear stream has stopped is no longer an ANT+ sensor. That is Layer C, done
deliberately and announced, rather than an encryption flag that lies.

## The two-tier split is the centrepiece

**The governing insight extends one level down: authenticating an identity and
authenticating a data stream are two different things with two different costs.**
They were fused in the first draft, at the price of the second one, and
separating them is what lets the cheap half be on by default.

The threat model decides which is which:

- *Accidental* — a stranger's sensor on an overlapping device number feeding a
  false heart rate into your ride — is an **identity** question and it is the
  mainstream case.
- *Deliberate eavesdropping* is a **confidentiality** question and it is Layer C.
- **Injection of forged values is in neither.** Shipping the expensive mechanism
  that defends against it, on by default, on every strap, was answering a
  question nobody in the threat model asked.

| | **Tier I — identity** | **Tier II — data** |
|---|---|---|
| Default | **on** | **off** |
| Question answered | is this stream from the holder of `K_auth`, now? | were these exact bytes sent by that holder? |
| Covers | nothing outside its own page | `N-1` preceding transmitted messages |
| Page | `[1..2]` counter, `[3..7]` `trunc40( CMAC(K_auth, nonce_block) )` | `[1]` window index, `[2..7]` `trunc48( CMAC(K_auth, nonce_block \|\| p_1 .. p_{N-1}) )` |
| Cadence | every `T` seconds, **decoupled from the data rate**, default 20 s | one page in `N` transmitted messages |
| Slot cost at 4 Hz | **1.2%** | 12.5% at N=8 |
| Verified on receipt | **yes** | no — the window must close |
| Loss behaviour | **verification rate = page delivery rate**, independent of `T` | one lost packet unverifies the whole window |
| DoS amplification | **none** | `N-1` |
| Forgery bound | 2^-40, one attempt per `T` | 2^-48 |

Three consequences worth writing down so nobody re-fuses them:

- **Decoupling `T` from the data rate is what removes the compatibility
  objection.** At 1.2% of slots, **the backwards-compatibility cost of
  authentication is smaller than a mechanism ANT+ itself ships** — 2 common pages
  in 121 is 1.65%, and every deployed receiver demonstrably tolerates it. That
  sentence is the whole argument for the tier.
- **Decoupling is also what forced the construction.** A window CMAC at a 20 s
  interval would be unverifiable roughly 28% of the time at the characterised
  ~0.4% loss floor, because the window would span ~81 messages. So the tag had to
  cover *no payload* — and that turned out to be exactly the mechanism the threat
  model needed. The cheap answer and the correct answer coincided; that is luck
  worth recording, not a design principle.
- **40 bits, not 48, for Tier I.** The counter needs two in-page bytes now that
  no window index is implied. 2^-40 per attempt, against a mechanism
  rate-limited to one attempt per `T` by construction, is not the weak link in
  this system, and ADR 0006's D16 reasoning survives the reduction.

**Naming, pinned here to prevent a collision that has already nearly happened:
attestation tiers are roman (Tier I, Tier II); ADR 0006's identity and
provisioning tiers are arabic (Tier 0, Tier 1, Tier 2).** They are unrelated
axes — a node can run attestation Tier I with identity Tier 0, or Tier II with
Tier 2 — and no document may use one numeral system for both.

## What is pinned

The byte-level spec is `../radiant-security.md` section 11. The pins this ADR
creates, in one list, are:

- **The beacon layout, with no epoch field.** Two frames on the descriptor's
  `(index << 4) | (count - 1)` convention: version and capability bits, the
  2-bit private policy, the attestation window `N` and mode, the key-group hint
  `trunc24( CMAC(K_id, epoch) )`, and the private-mode locator (device type,
  device number, period). `private-available` and the policy field must agree or
  the beacon is malformed; the locator fields are zero on a `never` node and on
  any node with `announce = silent`.
- **The SWITCH/RETURN layout**, as frames 2 and 3 of the beacon page's set:
  frame A the locator, reason and countdown; frame B
  `trunc48( CMAC(K_auth, nonce_block || frame_A) )` under subtype `0x03`.
- **Both attestation layouts**, above, sharing `dom = 0x04`
  (`RADIANT_SEC_DOM_COMPAT_MAC`, beside `_CTR`/`_SPREAD_MAC`/`_DESC_MAC`) with
  the subtype **inside the MAC'd nonce at position 9**, not merely in the page.
  Without the domain byte a compat tag and a spread tag can coincide; without the
  subtype in the block, a Tier I and a Tier II tag over the same counter are
  separated only by a page byte an attacker chooses.
- **The cost/loss table for both tiers**, in `../radiant-security.md` section
  7.7, next to the spread MAC's.
- **The four policy states with `never` as the default**, and the `announce`,
  `enrol`, `devnum` and `period` settings with their defaults.
- **The policy precedence rule: NVM if provisioned, else Kconfig; the host
  message writes NVM** rather than shadowing it.
- **The key dependency and its one exemption.** `private_policy != never`
  requires `K_auth` and Tier I; **`physical` + `silent` is the one exemption**,
  permitted with both tiers off, because with no announcement there is nothing to
  forge and no over-air trigger to authenticate.
- **The derived locator:** `trunc16( CMAC(K_id, "priv" || epoch) )`, `0x0000`
  excluded because it is the ANT wildcard, collisions rederived with a 1-byte
  suffix `0x00, 0x01, ...` of which a searching keyholder tries the first four
  before falling back to a wildcard search.
- **The epoch-recovery procedure and its two paths:** forward search against the
  key-group hint when `advertise = on`; trial-verification of candidate epochs
  against the attestation tag when `advertise = off`. Plus the bounded absolute
  scan for a re-provisioned sensor.
- **The private-mode state machine including the countdown:** COMPAT ->
  ANNOUNCING -> PRIVATE -> RETURNING -> COMPAT, `K in {16, 32, 64, 128}` with a
  default of 64 messages, the beacon promoted to 1 in 8 for the duration,
  receivers acting on countdown expiry rather than on receipt.
- **The page allocation: exactly two page numbers, not three**, and the same two
  in every compat profile.

### The epoch is not broadcast

The epoch was in the beacon as a joining receiver's freshness anchor. It comes
out, and this is a hard constraint rather than a simplification: for a hostless
strap **the epoch is the boot counter** (ADR 0009), so a slowly incrementing
32-bit number broadcast every 30 s is a device fingerprint that survives every
other privacy measure here — and it sat directly beside a key-group hint that was
made epoch-derived specifically to avoid that class of leak. It would also defeat
per-boot device-number rotation on its own. The same rule already governs the
sync-handoff page.

The requirement it must keep satisfying is occasional contact, 1:N — receiver A
this week, receiver B next month, each many boots out of date and neither able to
ask the sensor anything. It survives as a forward search: 50 boots is 50 CMAC
operations, 1 000 is ~3 ms on a Cortex-M4 with AES hardware, 65 536 is ~200 ms
once. **This is cheaper than the field it replaces**, and it is a one-off cost at
re-acquisition rather than a per-message one.

The consequence to build for is the `advertise = off` path: with no hint to
search against, the receiver trial-verifies candidate epochs against the
attestation tag directly. That path is the one that will otherwise be written and
never exercised, so both are asserted.

## The threat model this serves

> **The user sets up at home, where there are no snoopers, and needs the result
> to hold up everywhere else.** What they are defending against later is two
> things: **accidental** — a stranger's sensor on an overlapping device number
> feeding a false heart rate into their ride — and **deliberate** — someone in a
> gym, a race paddock or a hotel lobby reading a rider's data because it happened
> to be in the air.

- **The trusted setup moment is the design's foundation, and it is a realistic
  one.** Nothing here needs a PKI, a CA or a server, because there is a moment
  when the sensor and the receiver are in the same room and nobody hostile is.
  That is why `closed` and `physical` enrolment are the recommended paths and
  `open-window` is the reluctant third.
- **The accidental case is solved outright, and cheaply.** It is an identity
  problem, so Tier I answers it at 1.2% of slots with no confidentiality cost. A
  keyed receiver rejects a colliding stranger's sensor cryptographically instead
  of by device number. This is the most valuable thing here and it works in the
  default configuration, on an otherwise perfectly ordinary ANT+ strap.
- **The deliberate case is solved only where the user controls the receiver.**
  **RadiANT gives integrity everywhere and confidentiality wherever you own the
  other end.** That belongs in the README, not buried in an appendix, because it
  is the first question a buyer will ask.

Three costs go in this section rather than being discovered later:

- **Switch-time linkage.** A passive observer present at a switch learns that
  compat device number X and private device number Y are the same node. It would
  learn the same from the timing of one disappearing as the other appears, so the
  announcement does not create the linkage; it makes it cheap. Stated as a
  consequence: **ADR 0006's identity Tier 2 unlinkability does not survive a
  switch that an observer watches.** `announce = silent` narrows it to timing
  correlation; it does not remove the fact that the node was visibly an ANT+
  sensor beforehand.
- **The `broadcast`/`silent` availability trade.** `broadcast` costs
  unlinkability at the switch and buys a gap bounded by retune time for every
  keyed receiver, including the ones that did not ask. `silent` buys
  unlinkability and pays a gap bounded by search time, for every listener
  including the one that asked. **Neither is "more secure"**, and the docs quote
  the measured gap rather than "a short search".
- **Add is cheap, remove is not.** Layer D adds a receiver in sixty seconds
  because every keyholder holds the same root; removing one means re-provisioning
  every remaining receiver with a new root. Per-receiver revocation stays out of
  scope for the reasons in `../radiant-security.md` section 7.3, and Layer D
  makes that gap *more* visible rather than less. Anyone shipping a product
  should read the sentence before choosing `open-window`.

And one thing not solvable at this layer: **jamming and channel flooding remain
unfixable.** Attestation lets a receiver ignore a forged sensor; it cannot stop a
hostile transmitter from occupying the RF channel.

## Rejected alternatives

- **A third page number, for Tier II.** Rejected on the 7-bit namespace alone —
  heart rate's byte 0 carries a page-change toggle in bit 7, so its page space is
  0x00..0x7F and is shared with every vendor's private pages. The subtype nibble
  in byte `[0]` separates the tiers within one contiguous attestation claim.
  Nothing else about the design would change; the number is the whole objection.
- **Keeping the epoch in the beacon.** Rejected above; a fingerprint that
  survives every other measure, replaced by a search nobody will notice.
- **One fused attestation mechanism.** The original single window CMAC at N=8
  costs 12.5% of a legacy receiver's data pages — 30x the bench loss floor the RF
  plan worked three phases to reach — to defend against a threat the model does
  not contain. Split, not tuned.
- **Encrypting a compat page.** There is no such thing; see the theatre argument.
- **An over-air policy-change command.** Not deferred, *refused*. A policy a
  remote party can rewrite is not a policy, and downgrading `command` -> `never`
  over the air would be a mute attack wearing a safety hat.
- **Per-receiver switching** ("go private for just this receiver"). The sensor
  has one stream and N listeners; it cannot exist, and the SWITCH announcement is
  the answer rather than a workaround for it.
- **A mode flip on the same channel** instead of a close. Keeping device type
  `0x0B` alive while emitting only unknown page numbers leaves a head unit
  showing a connected sensor with no data — a failure mode users report as a bug.
  A close looks like walking out of range, which users understand.

## Consequences

**Good.**

- The mainstream failure — a colliding stranger's sensor — is solved
  cryptographically in the default configuration, at a compatibility cost smaller
  than one ANT+ already ships.
- Attestation is profile-agnostic. `radiant_sec_compat.c` authenticates bytes and
  contains **no page number and no field name**, which is what makes it testable
  against synthetic payloads with no profile in the loop and usable unchanged by
  a profile nobody has written yet. Same discipline as "`radiant_sec.c` contains
  no call to `radiant_sec_aes_ecb()`", and worth a grep in CI.
- The private-mode switch is not pure loss. Switching to device type `0x60` *is*
  the act of becoming RadiANT-native, which under ADR 0005 is the population
  eligible for the coded PHY, frequency agility, length extension and response
  slots of the RF plan. The honest pitch for Layer C is **"leave the
  compatibility constraint and gain range, reliability, battery life, latency and
  privacy — all five of which exist only because you left it"**. The switch
  itself lands on 1 M GFSK / RF 57 and moves afterwards through the ordinary
  descriptor mechanism: one change at a time.

**Costs, accepted.**

- **While private, a Garmin head unit and Zwift see nothing at all.** That is the
  price of confidentiality and there is no version of it that is cheaper. It is
  in the Kconfig help text, not only here.
- Two page numbers are consumed in every targeted profile's 7-bit namespace, and
  a manufacturer-specific page number is only unique per manufacturer id, so a
  collision with a vendor's private page on `0x0B` is possible. Mitigated by the
  beacon's version field and by the attestation MAC failing closed.
- **The heart-rate toggle-bit sequence must not be disturbed by inserted pages,
  and that is a bench question against a real head unit, not a documentation
  question.** It is answered in the interop phase or it is not answered.
- Tier II, when enabled, keeps the amplification property the spread MAC has:
  `N` is simultaneously the airtime cost, the verification latency and the DoS
  amplification factor. Deliver-as-unverified covers it; off-by-default is the
  real answer.
- One more surface for a policy value to disagree with itself. The precedence
  rule exists because three sources with an unstated precedence is a bug report
  waiting six months.

## Deliberately not in v1

- **Opportunistic attestation substitution.** The beacon's format bit and `N`
  field are specified so it is a later configuration change rather than a break;
  the implementation and its data-dependent test matrix are not in v1.
- **Speed and cadence.** `0x79` is excluded permanently and structurally.
  `0x7A`/`0x7B` are excluded from v1 pending a per-profile check, not by
  construction.
- **Fitness equipment `0x11`.** A compatibility target, unimplemented, and a much
  larger profile.
- **A printed key or QR code**, which would genuinely defeat MITM rather than
  merely mitigating it. It stays the recommendation for a product with a
  manufacturing step and is not a v1 bench answer.
- **A screen-free pairing fingerprint that actually solves MITM.** Not solved
  here, and must not be claimed as solved.
- **Per-receiver revocation, descriptor encryption, TESLA** — unchanged from ADR
  0006, still out, still for the same reasons.
