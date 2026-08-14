# RadiANT security: two payload switches and an identity rule, all off by default

Checked by: `scripts/check_profile_registry.py` — it asserts that the reserved
page range `0x20-0x2F`, the descriptor's epoch field, the data page's mandatory
counter and the trailing tag space stay claimed in `docs/profile-registry.md`
and `docs/radiant-telemetry.md`, and that the pins in this document (the nonce's
domain byte, the v1 window set, the tag length, the epoch-advance-on-wrap rule,
the descriptor authentication frame and the common-page privacy rules) are still
stated here. It also checks section 11's pins — the compat domain byte and its
subtypes, the two tag lengths, the legal `N` and `K` sets, the derived-locator
formula, the policy default and precedence, the two-page allocation and the
`0x79` exclusion — and the shape of any compat page rows in the registry.
Everything else in this document is narrative until the phase that implements it
lands.

This document is written ahead of the code on purpose. The switches below
constrain the page envelope — room for an epoch, a counter and a MAC byte — and
discovering that after the first profile ships means a format break for every
deployed node.

**Scope of v1: `X_AUTH` and `X_CONF`.** Both are payload transforms. Neither
touches the address path, the search policy, the RX filters or the scheduler.
`X_PRIV` — continuous per-epoch device-number rotation — was specified in the
first draft of this document and is **withdrawn**, not deferred; section 3.7
records why, so that it is not re-proposed. What replaces it is a provisioning
rule, section 4.

**Sections 1 to 10 secure RadiANT device type `0x60`. Section 11 is the other
half** — the same primitives on an ANT+ compatibility profile, where the node
stays byte-exact to a Garmin head unit and is verifiable, and on demand private,
to a receiver holding its key. It is a separate Kconfig, off by default, and its
decision record is
`docs/decisions/0008-antplus-additive-pages-and-compat-security.md`.

---

## 1. Open broadcast is the default, and that is a feature

An ANT+ sensor broadcasts in the clear and anything in range can read it.
RadiANT keeps that, unchanged, as the default for every channel.

It is not a gap that we have not got round to closing. It is:

- what lets `tools/ant_scan.py` open a wildcard channel and enumerate every
  sensor in the room with no keys and no pairing ceremony;
- what lets a head unit, a phone app, or somebody else's tool consume a sensor
  it has never seen before;
- the reason the ANT+ ecosystem is interoperable at all.

At household and gym range, "anyone in the room can read my heart rate" is an
acceptable and usually desirable property. ANT+ shipped open by default and
that choice was right.

**What ANT+ got wrong was not the default. It was the thing you got when you
turned it on.**

## 2. What ANT+ encryption actually gives you

| | ANT+ single-channel encryption |
|---|---|
| Confidentiality | AES-128-CTR |
| Authentication | **none** |
| Replay protection | **none** |
| Unlinkability | **none** |
| Reachable from a Windows host | **no** |

Four problems, in increasing order of how much they matter:

1. **CTR is malleable.** Flip a bit in the ciphertext and the same bit flips in
   the plaintext. With no MAC, an attacker who knows the layout of a power page
   can inject arbitrary wattage into an encrypted stream without holding the
   key. Encryption without authentication is, for a broadcast telemetry
   protocol, worse than no encryption — it makes a forged reading look
   protected.
2. **No replay protection.** A captured frame is a valid frame forever.
3. **It does not solve the problem that actually matters.** An ANT+ sensor
   broadcasts a **fixed 16-bit device number forever**, and that number is
   typically a factory serial. Turning encryption on does not change it; it is
   outside the encrypted payload because the receiver's hardware filter has to
   match on it. So anyone can track a rider across sessions, across gyms, and
   across locations, by device number alone, whether or not the payload is
   encrypted.
4. **No Windows host can use it.** `ANT_DLL.dll` exports no encryption call at
   all — not `ANT_EncryptedChannelEnable`, not `ANT_SetCryptoKey`, none of
   them; Zwift resolves none of them either. The feature is dead code that
   costs stack RAM shared with the plain channels. `CONFIG_ANT_DONGLE_ENCRYPTION`
   in this repo is off by default for exactly that reason, and its Kconfig help
   explains it.

RadiANT does not implement ANT+'s scheme. It replaces it.

---

## 3. Two independently selectable switches, not a ladder

The correction that matters most is structural: these are **two orthogonal
per-channel switches**, and all four combinations are legal. A ladder
(none -> encrypted -> encrypted+authenticated) never reaches the combination
that turns out to be the useful one, which is `X_AUTH` alone — see section 4.

| Switch | Property | Payload cost |
|---|---|---|
| `X_CONF` | confidentiality | **0 bytes** |
| `X_AUTH` | authenticity | **1 byte/packet** |
| *(v2)* | non-forgeability among receivers | v2 |

Identity is no longer in this table. It is not a switch and it is not a payload
transform; it is a provisioning rule, and it lives in section 4.

**One primitive.** AES-128 is the only cryptographic primitive in v1: CTR for
`X_CONF`, **AES-CMAC (RFC 4493)** for every MAC in the system, and the SP 800-108
counter-mode KDF with CMAC as its PRF for every derived key. No SHA is linked
anywhere. That saves roughly 1.4 KB, collapses the test surface to four
published vector sets (FIPS-197, RFC 4493, SP 800-38A, SP 800-108), and means
every construction here runs identically on `native_sim` and on a coin cell.

The first draft priced AES at "about 7 us from the nRF ECB peripheral". That
figure holds on one SoC family and is not a design input: **there is no ECB
peripheral on nRF54L at all** — `nordic,nrf-ecb` appears in the nRF51/52/5340-
cpunet/54H/9280 devicetrees and in no `nrf54l*.dtsi` — and reaching CRACEN means
PSA/`nrf_security`. So the v1 backend is **software AES**, encrypt-only, one
shared key schedule, roughly 1.0 KB, working on every board in the matrix. The
seam above it is built for hardware from the first line (see
`radiant/include/radiant/radiant_sec.h`); no hardware backend ships in
v1.

### 3.1 `X_CONF` — confidentiality

```
K_enc  = KDF(K_root, "enc" || epoch)
stream = AES-128-CTR(K_enc, nonce_block)
ct[2..6] = pt[2..6] XOR stream[2..6]
```

Bytes `[0]` (page number) and `[1]` (counter) stay in the clear, because a
receiver needs the page number to tell a data page from a descriptor and needs
the counter to build the nonce. With `X_AUTH` also on, byte `[7]` is the tag
byte and is likewise not ciphertext. That is what makes `X_CONF` cost **zero
payload bytes**.

**`radiant_sec` owns byte `[1]` on a secured master channel, and the host does
not.** This is not a stylistic choice. A master retransmits its current
broadcast body every slot whether or not the application wrote a new one, so a
host-maintained counter repeats across retransmissions — and a repeated counter
under one `(epoch, devnum)` is keystream reuse, which is the one failure that
turns CTR from weak into catastrophic. The counter therefore counts
*transmissions*, not host writes.

**The counter is reconstructed from time, not from arrival history.** The
receiver knows the epoch, the epoch's time anchor and the channel period, so it
computes the packet index it *expects* now and picks the 16-bit rollover that
lands nearest to the counter byte(s) actually on the air. This is what makes the
scheme work on the three cases an arrival-history counter cannot handle at all:

- a receiver joining mid-epoch, which is the *normal* case, not an edge case;
- a gap longer than 255 packets;
- sparse mode, where most slots carry nothing.

Drift budget: nearest-rollover resolution needs combined clock error below
±128 packet periods — ±32 s at 4 Hz, against two 50 ppm crystals diverging about
8.6 s/day — and the receiver **re-anchors its phase on every accepted packet**,
so drift only ever accumulates across a gap in which nothing was accepted.

### 3.2 `X_AUTH` — authenticity, via a spread MAC

A tag computed over a window of **W** consecutive packets, transmitted **one tag
byte per packet** in byte `[7]`, verified a whole window at a time.

**The message is pinned, because the natural reading of "MAC the payload" leaves
byte `[0]` unauthenticated — and an attacker who can flip the page number
reinterprets the same authenticated bits against a different schema.**

```
encrypt-then-MAC, always, in that order

tag = trunc_{8*W bits}( CMAC(K_auth, nonce_block || ct_0[0..6] || ct_1[0..6] || ... || ct_{W-1}[0..6]) )

where ct_i is packet i of the window as it appears on the air, including
byte [0] and byte [1], and excluding only byte [7], which carries the tag.
On a channel with X_CONF off, "ct" is the plaintext — the MAC covers what is
on the air either way.
```

`nonce_block` uses the counter of the **first packet of the window** and the
domain byte `0x02`; see section 3.3.

**Tag length is `8*W` bits** — 16 bits at W=2, 32 at W=4, 64 at W=8. It is not a
fixed 32; a W-byte tag spread one byte per packet over W packets cannot be
anything else. Section 7.5 states what that buys at each W.

**The tag transmitted in window *k* authenticates window k-1, and this is
forced rather than chosen.** Encrypt-then-MAC over a window means the tag
depends on all W packets of that window, so a sender cannot know the tag byte
belonging in the *first* packet of a window until it has built the *last* one.
The only alternatives are to delay every packet by up to W periods — which makes
telemetry stale to save a byte that was never at stake — or to build the whole
window before transmitting, which a host writing one page at a time cannot do.
So the tag lags by exactly one window:

```
window   = [ c - (c mod W), c - (c mod W) + W )
tag byte = tag_of_previous_window[ c mod W ]    where c is the packet's counter
```

Consequences, stated rather than left to be discovered:

- **Detection latency is W+1 to 2W packet periods**, not W. Section 7.5's table
  carries the numbers.
- **The first window of an epoch is never verified**, because nothing
  authenticates it, and the last window before a node goes quiet is never
  verified either. A receiver reports both as `unverified`; neither is evidence
  of an attack.
- A window's own tag is still computed at its own boundary, so nothing about
  the self-synchronising property below changes.

**Tag byte index and window boundary are derived from the counter, not from
arrival order.** By arrival order, one lost packet — against a measured ~0.4%
bench loss floor — desynchronises every window after it, permanently, with no
resynchronisation procedure anywhere in the design.

Self-synchronising, with no resync state and no resync message. This works
because **W divides 256 and divides 65536**, so a byte-counter wrap and a
16-bit-counter wrap both land on a window boundary. That is the reason
`W in {2, 4, 8}` and not, say, 3 or 5, and it is written down here because it is
exactly the kind of constraint a later "let's allow W=6" would violate silently.

**W=1 is not supported in v1.** An inline 16-bit tag needs bytes `[6..7]`, no
text defines that layout, and a descriptor declaring a field at bit offsets
32..39 would silently overlap it. W=1 stays reserved for the reliable-command
page, which is not v1 either.

**An unverifiable window is delivered, marked `unverified`. It is not
discarded.** Discarding costs W packets for every one lost — a 3.2% delivered-
data loss at W=8 against a 0.4% floor — and hands an attacker a W-for-1 denial
of service, where one injected frame destroys W legitimate ones. Delivering with
a verdict costs nothing and keeps the choice with the receiver's application.
The verdict surface is not optional and not ignorable: **every secured RX
delivery carries `verified` / `unverified` / `clear`**, `0xF1` carries a
per-channel drop-vs-deliver policy bit (default: deliver), and `0xF4` reports
window verdict counters, so a host that ignores per-message flags still has an
auditable stream.

**`X_AUTH` is not bypassable by message type.** The transform governs broadcast,
and on a secured channel a *non-broadcast* frame — acknowledged data, a burst —
carrying a page in the secured range is **dropped and counted**, never
delivered. Without that rule an injector simply sends a secured-range page as
acknowledged data, skips the MAC machinery entirely, and is delivered as
implicitly trusted data.

### 3.3 The pinned block layouts

Two implementations that differ here do not interoperate and the difference is
invisible until it is a field failure, so both blocks are pinned to the byte.

```
nonce_block = epoch[4 LE] || devnum[2 LE] || counter[2 LE] || dom || 0x00 x7
                dom = 0x01 CTR keystream | 0x02 spread MAC | 0x03 descriptor MAC

kdf_block   = 0x01 || label || 0x00 || epoch[4 LE] || base_devnum[2 LE] || 0x0080
                label = "enc" | "auth" | "id" | "cmd"
```

- **The domain byte at position 8 is what stops a keystream block and a MAC
  block ever coinciding.** The first draft omitted it, and the omission is the
  kind that produces a scheme which passes every test and leaks the tag key.
- The nonce is a **full 16 bytes**, because AES-CTR consumes 16-byte blocks.
  Stating "the nonce is 8 bytes" and leaving the rest to the implementer is how
  two implementations end up with different keystreams.
- `counter` in `nonce_block` is the 16-bit packet index within the epoch. Byte
  `[1]` on the air is its low 8 bits; the high 8 bits are reconstructed from
  time (section 3.1).
- **`base_devnum` is the device number fixed at provisioning time**, not the
  current on-air one. On a Tier 2 node (section 4) it does *not* re-roll, and a
  receiver knows it from pairing rather than from the air. Binding it into the
  KDF gives each sensor under a shared household root its own keys. The nonce
  already carries the current on-air `devnum`, so this is a defence against a
  16-bit device-number collision inside a shared-root household, not the primary
  uniqueness mechanism.
- The epoch-less `"id"` label uses **`epoch = 0`** in `kdf_block`.
- `0x0080` is the SP 800-108 output length in bits, big-endian, as that
  specification requires.

### 3.4 The key hierarchy: one root per channel

The first draft named `K_id`, `K_master` and `K_cmd` plus two derived keys, with
no stated hierarchy, and no sentence anywhere saying how many bytes a pairing
transfers. Pinned:

```
one 16-byte root per channel, K_root

K_enc  = KDF(K_root, "enc"  || epoch)      X_CONF keystream
K_auth = KDF(K_root, "auth" || epoch)      X_AUTH spread tag, descriptor auth frame
K_id   = KDF(K_root, "id")                 identity resolution (epoch = 0)
K_cmd  = KDF(K_root, "cmd"  || epoch)      reliable-command page, not v1
```

`0xF2 SET_KEY` installs **exactly one 16-byte value**, and everything else is
derived. Keys are write-only: there is no read arm for `0xF2` anywhere, and
`MESG_REQUEST` for `0xF2` answers `ANTW_INVALID_MESSAGE`. Keys and every derived
value are wiped on reset.

### 3.5 The epoch, the reboot rule, and the counter wrap

The epoch is a 32-bit key-rotation clock. It is no longer `floor(t / 128 s)`:
nothing rotates continuously any more, so the epoch advances only when something
makes it advance.

**A reboot must advance the epoch, and this gates both switches.** The nonce is
`epoch || devnum || counter`, and a node that reboots mid-epoch and restarts its
counter from zero **reuses keystream**. With the plaintext schema broadcast in
the clear on the descriptor, that is not a weakening — it is total recovery of
both plaintexts. The same reboot also makes `K_auth` and every recorded
`(packet, tag)` pair from the previous session valid again, which is a full
replay against an `X_AUTH`-only channel. So the rule covers both switches, not
just `X_CONF`:

> **No transform enables until the host has set an epoch with `0xF3`, and `0xF3`
> rejects any epoch <= the current one.**

That rejection is only enforceable within a power cycle, because keys and state
are wiped on reset. The obligation therefore sits with the **epoch authority** —
the host, or the node application — and is normative: *persist the last epoch
issued and never reissue it.* The recommended discharge, wherever a clock
exists, is to define the epoch as **coarse real time**, minutes since a fixed
RadiANT date: monotone for free, and it composes with the time-derived counter
of section 3.1 because `0xF3` carries microseconds-into-epoch as well as the
ordinal. Raw `0xF3` stays available for clockless hosts. NVM epoch ratcheting
inside `radiant` is deferred.

**A counter wrap advances the epoch by 1, on both sides.** The 16-bit counter
was sized against 128-second epochs, where it could not wrap. With the epoch
advancing only on reboot and on host command, it wraps after about 4.5 hours at
4 Hz and about 2.3 hours at 8 Hz — reintroducing, inside one session, exactly
the two-time pad the reboot rule exists to prevent. The receiver absorbs this
for free, because its time-derived index already carries the high bits:

```
epoch_delta = expected_index >> 16
counter     = expected_index & 0xFFFF
```

`65536` is divisible by every legal W, so window alignment survives the wrap
untouched. `0xF3` rejects epochs close to `0xFFFFFFFF` so that a wrap always has
headroom.

### 3.6 What the counter is, exactly, and where the invariant does not hold

Two definitions of "the counter" were in circulation — a packet index within the
epoch, and a free-running loss detector — and they are not compatible. Pinned:
**the counter is the packet index within the current epoch, and it resets to
zero when the epoch advances.** A loss detector built on it must handle that
reset, which is cheap because the receiver is told the epoch.

And the byte-`[1]`-is-a-counter invariant has two documented exceptions, which
the first draft did not admit: **the descriptor's byte `[1]` is a frame index**,
and the ANT+ common pages `0x50`/`0x51`/`0x52` are byte-exact ANT+ layouts with
no counter at all. Neither is in the secured page range, neither is encrypted,
and neither carries a spread tag — which is why the secured page range is
bounded away from both (section 5.5).

### 3.7 `X_PRIV` is withdrawn, and here is why it must not be re-proposed

The first draft specified a third switch: `devnum_e = trunc16(HMAC(K_id, epoch))`
with `epoch = floor(t / 128 s)`, rotating the device number continuously. It is
withdrawn as a **decision**, not a deferral.

It could not deliver the unlinkability it claimed:

- **Rotation breaks standard receivers**, so it is usable only on RadiANT-only
  device types — whose population is approximately zero. A rotating node is a
  category of one, and an anonymity set of one is not an anonymity set.
- Device type and transmission type **must stay fixed** for hardware filtering.
- Period, RF channel and schema are announced in the descriptor and are stable
  across a boundary.
- The event counter links across a boundary by `+1`, and timing phase is
  continuous through it.
- `floor(t / 128 s)` makes **every node rotate on the same global tick**, which
  is both a correlated demand spike and a linkage signal.
- Descriptor INFO bit 3 announced "I rotate" **in the clear**.
- Below about 1 Hz it cannot work at all: a clockless receiver's only epoch
  source is the descriptor, which arrives every 121 messages — 3.8 epochs at
  0.25 Hz.

And its cost was the whole of the address path: the dual-filter guard window
consuming both nRF address BASEs and blocking merged RX windows, the device ID
inclusion/exclusion list never matching again, a stale device number snapshotted
into every transfer, one dead seen-cache entry per epoch, roughly 675 phantom
devices per node per day in `tools/ant_scan.py`, and a monotone-clock
requirement a coin cell can only meet with flash writes.

**Descriptor INFO bit 3 stays reserved-must-be-zero** rather than being
documented as a rotation flag, and the descriptor's epoch field stays reserved.
Announcing a privacy posture is itself a leak, and reserving the bit keeps the
door open at zero format cost: revisiting this later is a format-compatible
change rather than a break.

### 3.8 v2, named and not specified: non-forgeability among receivers

TESLA-style delayed key disclosure. The sensor MACs packet *i* with `K_i`, and
discloses `K_i` *d* packets later; `K_i` comes from a hash chain so a receiver
can verify the disclosure against a previously authenticated element. Receivers
verify retroactively and **cannot forge**, because they learn the key only after
the window in which it would have been useful has closed.

Named here so the reserved transform-flag bit (descriptor frame 0, bit 4) and
the reserved page range `0x20-0x2F` are visibly for something. Not specified in
v1, and it is the eventual fix for limit 7.2.

---

## 4. Identity: what replaces `X_PRIV`

The cost curve of identity rotation has a cliff in it, and the cliff is
**mid-session rotation**:

| Rotation trigger | Link-layer cost | Receiver re-pairs? |
|---|---|---|
| Never (ANT+ today: a factory serial) | — | no |
| **First boot only — random, then stable** | **none** | **no** |
| Explicit user action (button, host command, factory reset) | none | yes, and the user asked for it |
| Every power-up | none | yes, every session |
| Every 128 s (`X_PRIV`) | address path, search, RX filters, scheduler, ID lists, seen cache, transfer snapshot, epoch clock, NVM ratchet | yes, constantly |

Everything above the last row is free, because **a rotation never happens while
a channel is open.** Section 3.7 lists what crossing that line costs.

### Tier 0 — random at first boot, stable thereafter. On by default

A RadiANT node derives its 16-bit device number randomly at first provisioning
and persists it. This is not a security feature; it is a correction. The number
stops being a factory serial that ties the node to a purchase record, a
warranty, or a previous owner. Sell or gift the sensor and it is genuinely a
different device.

**Zero UX change — no receiver ever loses a sensor**, which is the whole reason
this is the default. It is also not `radiant` code: device numbers are set
by the host (`MESG_SET_CHANNEL_ID`) or by the node application, so this is a
provisioning rule in `docs/profile-registry.md` plus `tools/` and `sim/`
support.

It applies to **ANT+ compatible device types too**, which is the population with
a real anonymity set. Note the 16-bit birthday bound: collisions become likely
around 300 devices in range, which is a constraint ANT+ already lives with.

### Tier 1 — re-roll on explicit user action. Opt-in, off by default

A button, a host command, or a factory reset. Receivers must re-pair, but the
user just asked for a new identity, so the loss is expected rather than
mysterious. This is the tier that covers "I am selling this" and "I am going
somewhere I would rather not be logged".

### Tier 2 — re-roll at every power-up. Opt-in, off by default, RadiANT device types only

This is the tier that answers the stalking case: a worn strap power-cycles
between rides, so a receiver planted near your house sees a different node each
time. It is also the tier that **silently loses sensors**, and that cost is
written down here rather than discovered:

- A **standard** receiver — Zwift, a Garmin head unit — must re-pair every
  session. This is stated plainly in the Kconfig help and in `docs/testing.md`.
  It is the reason Tier 2 is off by default and will stay a minority setting.
- A **RadiANT** receiver holding the key does **not** re-pair, and that is what
  makes Tier 2 tolerable at all. It wildcard-searches its device type, hears any
  candidate node, and confirms identity *cryptographically*: does `X_AUTH`
  verify under my key? "Which sensor is this" is answered by the MAC rather than
  by a number.

  Strictly, it is answered by the MAC's **key group**. A household sharing one
  root across two same-type sensors cannot tell them apart this way, so provision
  a per-sensor root where that distinction matters (section 3.3's `base_devnum`
  binding is what makes per-sensor roots cheap).

  This costs **nothing new**. It reuses `X_AUTH` and the ordinary search path,
  with no epoch, no guard window, no filter pressure and no scheduler changes,
  because resolution happens **once at acquisition** instead of every 128
  seconds.

  The implementation is one entry point, `radiant_sec_try_devnum()`: it sets the
  candidate's on-air device number and clears the receive window state, because
  a window half filled under the previous candidate and closed under this one
  produces an unverified verdict that says nothing about either. It re-derives
  **no key** — `base_devnum` is fixed at provisioning and does not re-roll — so
  a trial costs one nonce block per packet. The caller drives the ordinary
  search, calls it per candidate, and watches for `VERIFIED`.

  **The trial must be able to answer no.** A receiver that verified whatever it
  heard would "re-acquire" the first stranger to walk past, which is worse than
  re-pairing because it is silent. Both directions are asserted in
  `radiant/tests/src/test_sec.c`, along with a refusal to start a trial
  before the epoch is known — with no epoch nothing can verify, so such a trial
  would report "not my sensor" about every candidate including the right one.

This is the same insight BLE reached with *resolvable* private addresses rather
than merely random ones — but resolution at power-up granularity is vastly
cheaper than at epoch granularity, and that price difference is the entire
argument.

---

## 5. Rules that follow from open-by-default

### 5.1 Per channel, not per device

A node may broadcast heart rate in the clear on one channel and something
protected on another. The switches are per-channel state.

ANT already has per-channel encryption semantics
(`ant_crypto_channel_enable(channel, ...)`), so this matches the existing API
shape rather than inventing one — which also means the host-side messages in
section 9 take a channel number, like every other configuration message.

### 5.2 The descriptor page stays in the clear

Even when data pages do not.

Otherwise a protected node is indistinguishable from noise: it cannot be
enumerated, `tools/ant_scan.py` reports a silent hole rather than a device it
cannot read, and the operator has no way to tell a working encrypted node from
a dead one.

**Descriptor encryption is refused in v1.** It is underspecified rather than
merely unbuilt: the descriptor has no counter — byte `[1]` is a frame index —
so it has no nonce, and section 3.1's time-derived reconstruction has nothing to
reconstruct. `0xF1` bit 3 returns `ANTW_INVALID_PARAMETER_PROVIDED`. The
descriptor's transform bit 5 stays reserved for the version that specifies it,
and the cost it will have to state is unchanged: **the node becomes
undiscoverable to anything without the key.**

### 5.3 The descriptor is authenticated, or the MAC protects the wrong thing

The descriptor is unauthenticated in every configuration of the first draft, and
it carries the epoch, W, the transform flags, and every field's offset and
scale. Forging one descriptor frame yields **wrong readings from correctly
MAC'd packets** — the tag verifies and the numbers are still a lie.

So: a **descriptor authentication frame**, the last frame of the descriptor set,
mandatory whenever any transform bit is set.

```
[0]    0x00                        page number, like every descriptor frame
[1]    (index << 4) | (count - 1)  frame index, like every descriptor frame
[2..7] 48-bit CMAC(K_auth, epoch[4 LE] || devnum[2 LE] || schema_id || bodies)
       where "bodies" is bytes [2..7] of every preceding frame of this set,
       in frame order, and the MAC uses domain byte 0x03 in nonce_block.
```

It costs **one slot in 121** and **zero data-page bytes**. It is specified here
and implemented when `src/profiles/` arrives; until then there is no descriptor
encoder to authenticate. Section 7.6 states the limit that leaves open in the
meantime.

### 5.4 The common pages leak more than the device number ever did, and that is fixable in one rule

**This is the highest-value item in the whole privacy discussion, it is
independent of every switch and every identity tier, and it is one rule.**

- **Page 81 broadcasts a 32-bit globally unique serial number in the clear,
  every 30 seconds or so.** A 32-bit serial is strictly more identifying than
  the 16-bit device number, it is not affected by any device-number re-roll, and
  it defeats Tier 1 and Tier 2 outright.
- **Page 82 carries a monotone operating-time counter** that survives an
  identity change *and* fingerprints a battery swap.
- Page 80 carries manufacturer, model and hardware revision.

Normative, for any node with a privacy posture — any tier, any switch setting,
including all three off:

> Page 81 is emitted with **`serial = 0xFFFFFFFF`**, the sentinel that
> `tools/ant_pages.py` already documents as "not supplied". **Page 82 is
> suppressed**, or emitted with `operating_time` zeroed. Page 80 reports a
> **generic** manufacturer, model and revision.

A node that re-rolls its device number and keeps broadcasting its serial number
has not changed identity at all; it has added a field.

### 5.5 The secured page range is bounded, and the host cannot unbound it

The transform applies to a host-set contiguous page range, **bounded to
`0x01..0x1F`**, default `0x01..0x0F`. That keeps the descriptor (`0x00`) and the
ANT+ common pages (`0x50`/`0x51`/`0x52`) in the clear **mechanically rather than
by memory**, and it stops a host setting the low bound to `0x00` and making its
own node undiscoverable by accident. A range outside `0x01..0x1F` is refused
with `ANTW_INVALID_PARAMETER_PROVIDED`.

### 5.6 Zero cost when off, asserted rather than intended

All of it compiles out behind Kconfig, mirroring the existing
`CONFIG_ANT_DONGLE_ENCRYPTION` pattern. A plain ANT+ dongle built without the
switches carries no AES code, no key material, no key storage and no
per-channel crypto state competing for RAM with the plain channels.

"Byte-identical on air" is too weak a gate to be worth stating alone: it needs
two boards to observe, it says nothing about flash, and it passes a build
carrying 4 KB of dead security code. The gate is a CI job that builds the same
board twice in one run — at `HEAD` and at the merge base — and asserts that the
`size -A` output is identical section by section, that the defined-symbol set is
unchanged, that no `radiant_sec` symbol exists, and that `.config` contains
`# CONFIG_RADIANT_SEC is not set`. That last check is the one people forget, and
without it the job goes green on the day someone renames the symbol.

The on-air claim is a ztest assertion against the recorded `fake_radio` TX
bodies and RX filters, not a shell script — `radiant/tests/api/src/test_api_sec.c`,
which runs in both a feature-absent and a feature-present scenario so the
comparison means something.

#### What it actually costs when it is on

Measured on `adafruit_feather_nrf52840`, against the same `-DANT_RADIO=core`
build the CI job uses as its reference:

| | text | bss |
|---|---|---|
| `X_AUTH` + `X_CONF` + the `0xF1`–`0xF4` host surface | **+5084 B** | **+5277 B** |
| X25519 and pairing, on top of that | **+2812 B** | +705 B |

The largest single contributors to text are `radiant_sec_pump` (760 B, the RX
policy engine), `encrypt_block` (344 B), `radiant_sec_tx_transform` (308 B),
`rx_close_window` (290 B) and the AES `sbox` (256 B of rodata). Nothing else
exceeds 200 B. The largest single contributor to bss is the ISR-to-pump queue
at 1536 B — `RADIANT_SEC_MAX_CHANNELS` × `RADIANT_SEC_W_MAX` entries.

**This section used to claim a 4096 B ceiling, and the feature does not fit in
it.** That number was a starting figure with no measurement behind it, asserted
from the day the Kconfig symbol existed precisely so it would be watched as it
grew. It was watched, it grew, and at the end of the work it was 988 B short.

Raising a ceiling because you hit it is normally the exact failure the gate
exists to prevent, so the reasoning is written down rather than the number
quietly edited: the breakdown above is a block cipher, two modes, a KDF, a
windowing state machine and a serial surface, with no fat in it. The guess was
low; the code is not bloated. The ceilings are now 6144 B of text and 6144 B of
bss for the transforms, and a separate 3584 B of text for pairing — measured
figures with about 20% headroom, and the per-symbol breakdown above is what the
next change gets compared against.

**RAM is now asserted too, and never was before.** "Costs nothing when disabled"
was only ever checked in flash, and this feature turns out to cost more bss than
text. On a 256 KB part that is affordable; a budget nobody checks is how it
stops being affordable.

**`CONFIG_RADIANT_SEC_COMPAT` gets its own budget from the day the symbol
exists**, which is the correction the paragraphs above bought applied to the
next feature rather than only recorded about the last one. Section 11.4's
primitives are three pure functions over the same CMAC seam:

| symbol | text |
|---|---|
| `radiant_sec_compat_tier2_tag` | 132 B |
| `radiant_sec_compat_tier1_tag` | 66 B |
| `radiant_sec_compat_nonce_block` | 30 B |
| **total** | **228 B**, ceiling 288 |

**The linked delta is 0 B today and the CI job says so out loud**, because
nothing calls these yet and the linker never pulls the archive member in. A
ceiling on the linked delta alone would therefore be a gate that cannot fail
until somebody writes a caller, so the compiled object is measured too and that
is the number the 288 B applies to. When the emit and verify entry points land
the ceiling moves — with a per-symbol breakdown and a measurement behind the new
figure, in this table, or it does not move.

---

## 6. Threat model

Stated as such, and stated to be **not the household case**. In a house, the
default open broadcast is correct and none of this should be enabled.

**In scope:**

| Adversary | Capability |
|---|---|
| Passive eavesdropper | Receives every frame in range and reads the payload |
| Passive tracker | Correlates device numbers and serial numbers across sessions, days and locations to follow a person |
| Active injector | Transmits well-formed frames impersonating a sensor, to spoof a reading or drive an actuator |

The bar is **casual snooping, not a nation-state**. Section 7.5's forgery bound
is stated in those terms and accepted in those terms.

**Out of scope:**

- **An authorized receiver.** It holds the group key. This is inherent to
  symmetric one-to-many broadcast and is exactly what the v2 tier exists to
  escape. Any document that claims otherwise is wrong about the mathematics.
- **Jamming.** A 2.4 GHz jammer denies service to every protocol in the band
  and no payload-layer design addresses it.
- **A persistent local observer within one session.** See section 7.1; this is
  what `X_PRIV` claimed and could not deliver.

---

## 7. The honest limits

Stated here rather than discovered during integration.

### 7.1 There is no unlinkability against a persistent local observer

**RadiANT does not offer unlinkability, and says so. It offers an identity that
is not a factory serial and can be re-rolled at will.** That is a real,
defensible property. Claiming unlinkability from a 16-bit field sitting beside a
fixed device type would not have been.

Concretely: an observer who sees you twice inside one session links those two
sightings — a fleet catching you at km 0 and km 100 of one ride, or on entry and
exit from one gym visit. `X_PRIV` would have covered that case and nothing here
does.

Worth noting where *neither* scheme helps: an always-on mains-powered node — a
thermostat, a door sensor — never reboots, so Tier 2 gives it one identity for
months. But a stationary node is trivially linkable by RSSI and by simply always
being there, so continuous rotation buys it nothing either. **The case where the
cheap scheme is weak is the case where the expensive scheme was also useless.**

Category fingerprinting survives everything here: device type and transmission
type must stay fixed because a searching receiver matches on them, so an
observer still learns "a heart-rate sensor is here, and it left at 09:40".
Transmission timing — period and phase — is a second, weaker fingerprint.

### 7.2 Any keyholder can forge

Symmetric 1:N means `X_AUTH` authenticates **someone holding the group key**,
not the sensor itself. An authorized receiver holds the same key the sensor
does, so it can produce a frame the sensor would have produced. Inherent to
one-to-many symmetric broadcast, and precisely what the v2 TESLA tier exists to
escape.

### 7.3 There is no per-receiver revocation

Cutting one receiver off means **rekeying the sensor and redistributing the new
key to everyone else**. There is no way to exclude one holder of a group key
without changing the group key.

True revocation needs a broadcast-encryption scheme — LKH, subset-difference —
whose per-message overhead is logarithmic in the group size and does not fit an
8-byte payload at any price. That is not a matter of effort; it does not fit.

Fine for a household with three receivers. An operational problem for a team, a
coaching service, or a gym with staff turnover. **Documented, not solved.**

**Section 11's enrolment path makes this gap more visible rather than less, and
that is the right way round.** Adding a receiver is cheap precisely because
every keyholder holds the same root: no epoch change, no re-keying, no
interruption, and existing receivers observe nothing. The mirror operation is
not the mirror cost. **RadiANT v1 can add a receiver in sixty seconds and can
only remove one by re-provisioning the network** — every remaining receiver,
with a new root. Anyone shipping a product should read that sentence before
choosing an enrolment posture, and it belongs here rather than in a future
issue.

### 7.4 Pairing happens in the clear, and a host-less node cannot pair over the air

Key establishment is section 8. Two limits belong here:

- The X25519 exchange is **unauthenticated Diffie-Hellman** during its one
  window, and the mitigation is the usual short-fingerprint comparison. A
  deployment that cannot display or confirm a fingerprint should use the
  out-of-band path.
- **`0xF5` carries a host-supplied 32-byte scalar**, because the only entropy
  source on nRF54L is `psa_rng`/CRACEN and reaching it drags in `nrf_security`
  on every build. The host has a real CSPRNG; the node does not need one. The
  honest consequence: **a host-less node cannot pair this way.** Documented,
  deferred, and not worked around with a weak on-node PRNG.

**Amended 2026-08-11 — the deferral above is closed by
`docs/decisions/0009-hostless-node-identity.md`.** A battery strap has no host,
so "the host has a real CSPRNG" stops being an answer the moment the node this
whole compat layer exists to build is the node in question. The replacement is
not a weak on-node PRNG: it is a **deterministic** scalar
`KDF(K_dev, "pair" || pair_counter)` over a persisted monotonic counter, with
the counter advanced **before** the public key is transmitted, and an optional
PSA/CRACEN entropy backend behind a `caps` bit for parts that already link
`nrf_security`. The same counter is the hostless node's epoch source, which is
the other half of the same hole. Read ADR 0009 before re-arguing either.

**Amended 2026-08-11 — and the amendment above creates a limit rather than
removing one, which section 11.7's over-air enrolment now makes concrete.** A
hostless node can pair. It still has **no screen**, and the first bullet above
assumed one on both ends: "the usual short-fingerprint comparison" is a human
reading six digits off *two* displays and checking they match, and an attacker
in the middle holds two different shared secrets and cannot make both match.

**A strap gives one side of that comparison and nothing on the other.** The
receiver can show its six digits; there is nothing at the sensor for the user to
compare them against. That is not a gap to be closed later by a cleverer
protocol — the number has to be displayed somewhere a person can read it, and
the device has no somewhere. What remains is a set of mitigations, and they are
named as mitigations:

- the window is **bounded** — `RADIANT_SEC_PAIR_TIMEOUT_DEFAULT_S`, 60 s;
- **one pairing per window**, so the window closes on the first answer rather
  than staying open for a better one;
- a **physical trigger** at the node, so the window exists because a person
  decided it should;
- optionally **reduced transmit power** for the duration, so an attacker has to
  be in the room rather than in the car park. This is the node's to set and
  `src/profiles/profile_enrol.c` does not touch the radio; it is named so its
  absence from the module is a decision.

**And the recommendation is unchanged.** The pairing section of
`radiant/include/radiant/radiant_sec.h` — the block headed "PAIRING
HAPPENS IN THE CLEAR", immediately above `radiant_sec_pair_enter()` — already
says out-of-band pairing "remains the recommended path for anything that
matters: no protocol, no attack surface during pairing at all, and no
dependence on the user actually looking". `enrol = closed` is that posture as a
setting, a printed key or a QR code genuinely defeats the attack rather than
mitigating it, and **none of this is weakened by the fact that over-air
enrolment now exists.** It exists because a sealed strap otherwise has no path
at all, which is a different claim.

*(That block used to be cited by line number, `radiant_sec.h:230-236`. It has
moved — line 230 is now `RADIANT_SEC_X25519_BYTES` — so it is cited by its
heading here. A line-number citation into a file that grows is a citation with a
shelf life.)*

### 7.5 What a spread MAC costs, in numbers

Both of these are properties of the construction, not of the implementation, and
both are stated as numbers because the first draft stated neither.

**Detection latency and loss amplification.** W packets of forged data are
accepted before the window's tag fails, and one lost packet makes its whole
window unverifiable:

| W | Tag length | Detection latency at 4 Hz | Unverifiable fraction against a 0.4% loss floor |
|---|---|---|---|
| 2 (default) | 16 bits | 0.75–1.0 s | ~0.8% |
| 4 | 32 bits | 1.25–2.0 s | ~1.6% |
| 8 | 64 bits | 2.25–4.0 s | ~3.2% |

The latency is a range and not a single number because the tag lags one window
(section 3.2): a forgery in window *k* is detected when window *k+1* completes,
which is W+1 packets later for the last packet of *k* and 2W for the first.

The amplification factor is **W**, and it is why the default is W=2 and why an
unverifiable window is delivered rather than discarded (section 3.2). Discarding
would turn a 0.4% loss floor into a 3.2% data loss at W=8 *and* give an attacker
a W-for-1 denial of service.

**Forgery bound.** At the default W=2 the tag is **16 bits**, which is the same
size the deferred reliable-command page's inline tag is called brute-forceable
at. Per window, an attacker's chance of a *verified* forgery is about 2^-16;
continuous injection at 4 Hz gives an expected time of roughly **2.3 hours** for
one verified forged data window. At W=4 it is 2^-32, and at W=8 it is 2^-64.

This is accepted under the casual-snooping bar, **for sensor data and not for
commands** — and it is said out loud rather than left to be inferred from "a
32-bit tag", which the default does not produce. It is also **wrong for anything
that actuates**: a window of forged commands is a window in which the AC turned
on. That is why the envelope's reliable-command page carries an **inline** tag
rather than a spread one, and why its idempotency rule refuses to change state
on a bad tag. See `docs/radiant-telemetry.md` section 9.

### 7.6 A joining receiver's freshness comes from its host, not from the air

Replay rejection is the time-consistency check of section 3.1: anything more
than a small slack behind the expected index is rejected. That survives receiver
reboot, which a volatile high-water mark cannot — and the *normal* case is a
receiver joining mid-stream, which a high-water mark handles by accepting a full
epoch of replay.

The limit: a joining receiver's freshness is anchored on the **host-supplied**
epoch via `0xF3`, not on anything it hears. The descriptor that carries the
epoch is unauthenticated until section 5.3's frame ships, so **a captured old
epoch replays wholesale to a receiver whose host does not know the current
one.** Stated here rather than implied.

### 7.7 What compat attestation costs, in numbers

The section 11 layer is measured the same way, and the two tiers land in
different places on purpose. Slot figures are at a 4 Hz profile with Tier I at
its default `T` = 20 s; the unverified fraction is against the characterised
~0.4% loss floor of `docs/spike-b-part2-results.md`.

| Tier | slots left to legacy | unverified | latency at 4 Hz | forgery bound |
|---|---|---|---|---|
| **I only (the default), `T` = 20 s** | **98.8%** | **~0.4%** | **~20 s** | **2^-40 per attempt, 1 attempt per `T`** |
| I + II, N = 4 | 74% | ~1.6% | ~1 s | 2^-48 |
| I + II, N = 8 | 86.5% | ~3.2% | ~2 s | 2^-48 |
| I + II, N = 16 | 92.7% | ~6.2% | ~4 s | 2^-48 |
| I + II, N = 32 | 95.8% | ~12% | ~8 s | 2^-48 |

**Tier I has no amplification factor at all, because it amplifies nothing.** It
covers no payload, so a delivered Tier I page verifies whatever else was lost:
**verification rate equals page delivery rate**, independent of the interval.
That is the property a window CMAC cannot have at any length, and it is the
whole reason this tier exists.

**Tier II keeps the amplification the spread MAC's W has and multiplies it by
N.** One injected or suppressed page unverifies `N-1` legitimate ones, N is
simultaneously the airtime cost, the verification latency and the DoS
amplification factor, and deliver-as-unverified (section 3.2) is what stops that
becoming data loss. This is the honest regression against the spread tag, which
resynchronises after a loss and does not, and it is why Tier II is off by
default.

**In the default configuration a compat node spends 2.0% of its slots on
RadiANT** — 0.8% beacon plus 1.2% Tier I — against the **1.65%** ANT+ itself
already spends on common pages 80 and 81 in the same 121-message cycle. Every
deployed receiver demonstrably tolerates the larger number. That comparison is
what the compatibility claim rests on, and section 11 states the measurement
that has to confirm it.

---

## 8. Key establishment

Two paths, and the simple one is the one most deployments should use.

**Out-of-band.** A QR code on the node, an NFC tap, or a key typed in by hand.
No protocol, no code on the node beyond storing 16 bytes, and no attack surface
during pairing at all.

**X25519 over the existing acknowledged/burst path.** At pairing time the two
ends exchange public keys over an ANT burst — a burst carries 32-byte keys
comfortably — and derive the shared secret, from which `K_root` is derived. The
32-byte scalar comes from the host over `0xF5` (section 7.4).

**Measured cost: 374 ms per scalar multiplication**, on an nRF5340 application
core at 128 MHz, printed by `radiant/tests/src/test_sec_x25519.c`. A
pairing needs two — one to produce the local public key, one for the shared
secret — so roughly **0.75 s of computation per pairing**.

This paragraph previously said "roughly 4 ms on a Cortex-M4", which was wrong by
about two orders of magnitude and is worth leaving a note about rather than
quietly fixing. 4 ms is what an *optimised* X25519 costs. What is implemented
here is deliberately not that: 16-bit limbs and schoolbook multiplication,
chosen because it can be read and checked in an afternoon, in a file whose whole
purpose is to be displaceable by a hardware PKA the day the cost matters. The
number was a plausible figure for the algorithm rather than a measurement of the
code, which is exactly the kind of claim that survives review because it sounds
right.

It does not matter here, and that is why the simple implementation is the right
one: a pairing happens once, initiated by a human who is already holding two
devices. It would matter immediately if anything ever tried to re-key on a
schedule, and nothing should.

**The sensor holds one key. Every authorized receiver gets a copy.**

That single sentence is the structural difference from BLE, and it is the
reason RadiANT can do what BLE cannot:

| | BLE | RadiANT |
|---|---|---|
| Key model | pairwise LTK per bonded peer | one group key |
| State on the sensor | grows with the number of bonded peers | **zero per-receiver state** |
| Receivers per sensor | bounded by the bond table | unbounded |
| One-to-many | emulated, or given up | native |

Zero per-receiver state on the sensor is what preserves true one-to-many
broadcast, which is the property ANT has and BLE does not, and which this whole
project exists to keep. It is also, unavoidably, why limits 7.2 and 7.3 are
true: the same absence of per-receiver state is what makes revocation
impossible.

**No design step may quietly regress this to pairwise.** If a proposal needs
per-receiver state on the sensor, it is a different protocol.

### 8.1 One invariant to carry into the pairing implementation

Not a limit of this design - a note for whoever implements the X25519 exchange
above, recorded now for the same reason section 7.4's limits are recorded now
rather than found during integration.

sdk-ant's own AES-CTR negotiation (declined for `radiant`; see section 10)
serialises key exchange across the whole device with a single global "only one
channel negotiates at a time" flag - a reasonable design if a negotiation needs
a scarce shared resource (a work queue, an ECB/crypto peripheral slot) that
cannot simply be duplicated per channel. **If the X25519 exchange in this
section ever grows the same kind of exclusivity flag, the part worth
remembering is not the flag - it is that the flag must be released on loss of
tracking, not only on completion or failure.** A channel that starts pairing,
then loses its peer mid-exchange - the sensor walked out of range, or a rider's
body blocked the link - has no completion event and no failure event to free
the flag with. Forget the release-on-loss-of-tracking path and that channel
deadlocks negotiation for every other channel on the device, permanently, with
no event anywhere to say so: `radiant_channel_on_slot_missed()` already raises
`RADIANT_CH_EVENT_RX_FAIL_GO_TO_SEARCH` after
`RADIANT_CHANNEL_RX_FAIL_TO_SEARCH` consecutive misses (see
`radiant_channel.h`), and that transition - not just an explicit "negotiation
cancelled" message - is what a negotiation-exclusivity flag would have to hook
to stay safe.

This does not apply to the design as specified above: the X25519 exchange runs
over one channel's own acknowledged/burst path with no cross-channel resource
today, so there is nothing to serialise and nothing to leak. It applies only if
a future revision adds one. See `docs/sdk-ant-comparison.md` item 7.

---

## 9. Host side: new `MESG_*` IDs, which ANT+ never had

Because RadiANT defines the serial protocol as well as the radio, the switches
are reachable from a host. This is the direct fix for the fourth problem in
section 2: ANT+ encryption failed in practice not because it was weak but
because no application could call it.

Every new message ID must be allocated from unclaimed ID space and recorded in
`protocol/ant_wire.yaml`, which is the authority on the numbering; this section
records what the IDs are for. The allocation below is the one in the YAML.

### Allocated IDs

| ID | Name | Purpose |
|---|---|---|
| `0xF1` | `ANTW_MESG_RADIANT_SEC_CONFIG_ID` | Per-channel switch bitmask (`X_CONF`/`X_AUTH`), the MAC window W, the secured page range and the drop-vs-deliver policy bit |
| `0xF2` | `ANTW_MESG_RADIANT_SET_KEY_ID` | Install the 16-byte `K_root` for a channel. Write-only |
| `0xF3` | `ANTW_MESG_RADIANT_EPOCH_ID` | Set or read the epoch and its microsecond time anchor |
| `0xF4` | `ANTW_MESG_RADIANT_SEC_STATUS_ID` | Read back: epoch, expected index, window verdict counters, drop counters |
| `0xF5` | `ANTW_MESG_RADIANT_PAIRING_ID` | Enter/leave pairing mode; carry the host-supplied scalar and the X25519 exchange |
| `0xF6-0xFA` | reserved | The rest of the RadiANT family |

`0xF1` bit 3 (descriptor encryption) returns `ANTW_INVALID_PARAMETER_PROVIDED`
in v1; see section 5.2. A page range outside `0x01..0x1F` is refused the same
way; see section 5.5. `0xF3` refuses an epoch less than or equal to the current
one, and refuses epochs near `0xFFFFFFFF`; see section 3.5.

### The two ordering rules, and why they are refusals rather than defaults

`0xF1`–`0xF4` are implemented by `radiant/src/radiant_sec_host.c` behind
`CONFIG_RADIANT_SEC_HOST_MESSAGES`, and reached from the host through
`antr_sec_config()`, `antr_sec_key_set()`, `antr_sec_epoch_set()` and
`antr_sec_status_get()`. Two orderings are mandatory, and both answer
`ANTW_CHANNEL_IN_WRONG_STATE` rather than guessing:

- **The channel ID must be set before the key.** `base_devnum` is bound into the
  KDF and `0xF2` does not carry it — a device number the host had to repeat
  correctly would be a second place for it to be wrong. Keying first would
  derive every sub-key under device number 0, which fails as a link that
  verifies nothing rather than as an error.
- **The channel period must be set before the epoch.** The period is what turns
  `us_into_epoch` into a packet index. A host-supplied period that disagreed
  with the channel's would desynchronise the counter in a way that looks exactly
  like clock drift, so `0xF3` does not carry one either.

`ANTW_CHANNEL_IN_WRONG_STATE` rather than `ANTW_INVALID_PARAMETER_PROVIDED` is
the useful distinction: the parameter is correct and arrived early, and a host
that cannot tell those apart retries the wrong thing.

### What the key path promises, and what it cannot

`0xF2` is write-only. There is no read arm anywhere, and `MESG_REQUEST` for it
is answered `ANTW_INVALID_MESSAGE` — the same answer a device that had never
heard of `0xF2` would give.

`src/ant_serial_bridge.c` wipes its frame buffer after dispatching one, because
that buffer is static and long-lived and would otherwise hold the key until some
later message happened to overwrite it. **That wipe narrows the window and does
not close it**, and saying so is more useful than implying otherwise: the same
bytes passed through the USB stack's ring buffer and the transport buffer
beneath it, neither of which is reachable from that code, and they were in host
memory before that. A root key that has crossed a USB cable should be treated as
having been in host memory, because it has.

### `0xF4` is what makes deliver-as-unverified auditable

Section 3.2's deliver-but-mark policy only means anything if unverified cannot
be silently treated as verified. A host that ignores the per-message verdict
flag can still read the window counters here, so the failure mode "everything
looked fine because nothing was checking" is visible from outside the device.

The counters saturate rather than wrapping. A wrapped counter would let a long
noisy run look quiet, and quiet is precisely the reading these exist to
disprove — a field at `0xFFFF` means *at least* that many.

An unkeyed channel answers with zeros and a `clear` verdict rather than an
error. "No security on this channel" is a state a host must be able to read
without treating an ordinary channel as a fault, and a host that learns to
ignore one error learns to ignore them all.

`tools/ant_sec.py` is the host-side encoder and decoder for all four, and
`tools/ant_features.py --radiant-security` probes them against a real device —
off by default, because a stock ANT stick answers `INVALID_MESSAGE` to all four
and that noise is not informative.

### These were `0xE0`-`0xE4`, and the reason they moved is the useful part

The first allocation was `0xE0`-`0xE4`. It was wrong: **sdk-ant defines
`MESG_EXT_ID_0` … `MESG_EXT_ID_4` as exactly `0xE0`-`0xE4`**, an
extended-message-id block selected by `MSG_EXT_ID_MASK` (`0xE0`) and reserved
for ANT response and request messages that carry extended message IDs. That is
a head-on collision with the protocol this project claims compatibility with.
Nobody should re-propose that range.

The argument that produced it was:

> No message ID at or above `0xC0` appears in the dispatch in
> `src/ant_serial_bridge.c`, in any ID defined in `tools/`, or in the
> capabilities reply the dongle emits.

**Every one of those checks was correct, and the conclusion was still wrong,
because this repository is not the whole protocol.** The bridge implements the
messages a host actually sends; `tools/` defines the ones our own tools use;
the capabilities reply carries no ID space at all. Three accurate readings of
three partial witnesses do not add up to a statement about ANT. The lesson
generalises past this one allocation: an ID reservation is a claim about the
protocol, so the only witnesses that count are ones that speak for the
protocol — and the repo is not one of them. The list of in-repo checks is
necessary and never sufficient.

What settled it was a witness the first allocation could not consult: the one
agent permitted to read sdk-ant. In the region above `0xC8` sdk-ant uses the
extended-ID block `0xE0`-`0xE4` and `MESG_DEBUG_ID` at `0xF0`, and nothing in
`0xF1`-`0xFA`. Hence this allocation, one clear of `0xF0`.

Two properties of the old argument survive the move and are still worth
stating:

- `0xF1`-`0xF5` is not confusable with the SYNC byte `0xA4` or its
  bidirectional variant `0xA5` by a resynchronising parser.
- These are requestable: `MESG_REQUEST_ID` (`0x4D`) takes a message ID as its
  argument, so `0xF3` and `0xF4` are readable with no new mechanism. `0xF2` is
  not: a request for it answers `ANTW_INVALID_MESSAGE`, because keys are
  write-only.

**Lib config `0xE0` is a different namespace and has not moved.** The
`ANTW_LIB_CONFIG_ALL_EXT_FIELDS` setting — channel ID + RSSI + RX timestamp — is
a payload byte of message `0x6E`, not a message ID, and it is unaffected by any
of the above. The RadiANT message IDs no longer share a byte with it, but the
reason they never conflicted was that the two namespaces are unrelated, not
that the numbers differ.

---

## 10. What is deliberately not being built

- **`X_PRIV` / continuous epoch rotation — rejected, not deferred.** Section 3.7
  records the reasons so it is not re-proposed: the anonymity set is empty by
  construction, five separate leaks survive it, it cannot work below about 1 Hz,
  and its entire cost sits in the mid-session rotation that delivers least.
- **Resolvable identity for *standard* device types.** A keyed RadiANT receiver
  can re-acquire across a Tier 2 re-roll; a Garmin head unit cannot, and nothing
  in an 8-byte ANT+ frame will change that. Tier 2 stays RadiANT-only and off.
- **Descriptor encryption** (section 5.2), and the *implementation* of the
  descriptor authentication frame (section 5.3) — both wait for `src/profiles/`.
- **The reliable-command page**, including its rate-limit fix: rate-limit
  *failed* tag verifications with exponential backoff. Rate-limiting *accepted*
  commands does nothing, because the attacker's traffic is 99.998% rejections.
- **Every hardware crypto backend.** The seam is built for them; none ships in
  v1. Expected order when they come: the nRF52840 ECB peripheral (block level,
  trivial), then PSA/`nrf_security` — the one that unlocks CRACEN on nRF54L
  *and* CC310 on nRF52840 through a single backend, and therefore the
  highest-value single addition — then a third-party part if one is adopted.
  Adding a backend must not touch `radiant_sec.c`; if it does, the seam is wrong.
- **Hand-optimised or assembly AES.** About 30 us saved four times a second, at
  the cost of a second implementation to test.
- **NVM epoch ratcheting**, **TESLA delayed key disclosure** (v2), and
  **per-receiver revocation** — the last stays documented as a known limit
  (7.3), not solved.
- **ANT+'s own AES-CTR scheme in `radiant`.** No Windows host can call it,
  it is malleable and unauthenticated, and it reserves stack RAM shared with the
  plain channels. `encryption.conf` and the existing sdk-ant-backed writes stay
  exactly as they are; they cost nothing while that backend exists.
- **Encrypting anything on an ANT+ compatibility channel.** There is no such
  thing, and section 11.1 says why in one sentence. Confidentiality on a compat
  profile is section 11.5's switch or it is theatre.
- **An over-air command that changes a node's compat policy.** Not deferred —
  *refused*, section 11.6.
- **Per-receiver switching.** The sensor has one stream and N listeners; "go
  private for you only" cannot exist.
- **Opportunistic attestation substitution.** The beacon's format bit and `N`
  field are specified so it is a later configuration change rather than a break;
  the implementation and its data-dependent test matrix are not in v1.
- **Speed and cadence.** `0x79` is excluded permanently — section 11.8.
  `0x7A`/`0x7B` are out of v1 pending a per-profile check, not by construction.

---

## 11. Compat mode: RadiANT on an ANT+ device type

**Decided in `docs/decisions/0008-antplus-additive-pages-and-compat-security.md`;
this section is the spec that ADR records.** Everything above secures RadiANT
device type `0x60`. This section covers the other half: a node that is a
byte-exact ANT+ heart-rate strap or power meter to every legacy receiver in the
room, and a verifiable — and on demand, private — RadiANT sensor to a receiver
holding its key. It is enabled by `CONFIG_RADIANT_SEC_COMPAT` (default n,
depends on `CONFIG_RADIANT_SEC`) and it is permitted on ANT+ device types by
`docs/radiant-telemetry.md` section 2 clause 2 as amended.

**A naming rule before anything else, because these two axes have already nearly
collided: attestation tiers are roman, identity tiers are arabic.** Tier I and
Tier II are the two attestation mechanisms in section 11.4. Tier 0, Tier 1 and
Tier 2 are the identity and provisioning tiers of section 4. They are unrelated
axes — a node can run attestation Tier I with identity Tier 0, or Tier II with
Tier 2 — and no document may use one numeral system for both.

### 11.1 Advertise, authenticate and encrypt have three different compatibility costs

This is the sentence the whole layer is downstream of:

> **Advertising is free. Authenticating a clear stream is free to legacy
> receivers and is most of the value. Encrypting is not compatible with
> anything, ever, because a ciphertext page beside the plaintext page carrying
> the same value is theatre. So confidentiality is a switch, not an addition.**

There is no "encrypted ANT+ heart-rate page". The value is either in the clear
where a Garmin head unit can read it — in which case encrypting a second copy of
it protects nothing — or it is not there at all, which means the node has
stopped being an ANT+ sensor. Section 11.5 is that stop, done deliberately and
announced.

One level down, the same cut applies again: **authenticating an identity and
authenticating a data stream are two different things with two different
costs.** Fusing them is what made the first draft's attestation expensive enough
to argue about. Section 11.4 separates them.

### 11.2 Layer A — the capability beacon

One page interleaved into the profile's **existing** 121-message common-page
rotation: one message in 121, **0.8% of slots**, ~30 s discovery latency at
4 Hz. It does not invent a cadence; it rides the one pages 80 and 81 already
established.

Two frames under one page number, on the descriptor's
`(index << 4) | (count - 1)` framing convention (`docs/radiant-telemetry.md`
section 6), so byte `[1]` is the frame index and bytes `[2..7]` are payload.

```
frame 0   [1] = 0x01
  [2]     bits 7:4 compat envelope version (v1 = 0x1)
          bit  3   pairing-available     bit 2 pairing-open
          bit  1   private-available     bit 0 attest-available
  [3]     bits 1:0 private policy: 0 never, 1 physical, 2 command, 3 always
          bits 3:2 attestation window N: 0 -> 4, 1 -> 8, 2 -> 16, 3 -> 32
          bit  4   attestation mode: 0 fixed, 1 opportunistic (not v1)
          bit  5   pending switch
          bits 7:6 reserved, must be 0
  [4..6]  key-group hint, trunc24( CMAC(K_id, epoch) ), CMAC output order
  [7]     reserved, must be 0

frame 1   [1] = 0x11
  [2]     private-mode target device type (bits 6:0); bit 7 must be 0
  [3..4]  private-mode target device number, LE
  [5..6]  private-mode target channel period, LE, counts of 1/32768 s
  [7]     reserved, must be 0
```

- **The beacon carries no epoch.** It is the field a joining receiver would most
  obviously want and it is refused: for a hostless node the epoch *is* the boot
  counter (`docs/decisions/0009-hostless-node-identity.md`), and a slowly
  incrementing 32-bit number broadcast every 30 s is a device fingerprint that
  survives every other privacy measure here — sitting directly beside a
  key-group hint that was made epoch-derived precisely to avoid that class of
  leak. Section 11.3 is how a receiver recovers it instead. The same rule
  already governs the sync-handoff page (`docs/radiant-telemetry.md` section 4).
- **The key-group hint is epoch-derived, never static.** A fixed "RadiANT, group
  ABC" byte string every 30 s is a better tracking identifier than the device
  number, which is section 3.7's whole lesson and the reason page 81's serial is
  `0xFFFFFFFF` under a privacy posture. It answers "is this one of mine" without
  trial-verifying every root, and it is the epoch anchor.
- **A node whose policy is `never` advertises `private-available = 0` and policy
  `never`, and the two must agree.** A receiver that sees them disagree treats
  the beacon as malformed. Frame 1's locator fields are zero on such a node —
  there is nowhere for it to go.
- **Frame 1's locator fields are also zero on any node with `announce =
  silent`**, whatever its policy, and the pending-switch bit is never set there.
  A keyholder derives the locator (section 11.5); an observer gets nothing.
- **Accepted leak:** the beacon's existence marks a node as RadiANT. Same class
  as device type. Noted, not fixed.

### 11.3 Epoch recovery, and its two paths

The requirement the removed field has to keep satisfying is occasional contact,
1:N: receiver A this week, receiver B next month, each possibly many boots out
of date and neither able to ask the sensor anything — a garage-door opener.

```
first contact    the epoch arrives in the pairing exchange, where a two-way channel exists
later contact    the receiver stores last_seen_epoch per sensor and searches
                 forward, computing trunc24( CMAC(K_id, e) ) for
                 e = last_seen, last_seen + 1, ... until it matches the beacon hint
confirm          the first attestation window that verifies is a 40- or 48-bit
                 confirmation of the candidate; a 24-bit hint collision cannot survive it
re-provision     bounded absolute scan from 0, plus a small backward probe. Bounded,
                 never an unbounded loop
```

| Receiver is behind by | CMAC ops | Time on a Cortex-M4 with AES hardware |
|---|---|---|
| 50 boots (a season) | 50 | well under 1 ms |
| 1 000 boots | 1 000 | ~3 ms |
| 65 536 boots (absurd) | 65 536 | ~200 ms, one-off |

Ten sensors' keys multiply that by ten and it is still not a number anyone
notices. It is cheaper than the beacon field it replaces, and it is a one-off
cost at re-acquisition rather than a per-message one.

**Two paths, and the second is the one that otherwise gets written and never
exercised.** With `advertise = on` the receiver does a **forward search against
the key-group hint**, one CMAC per candidate. With `advertise = off` there is no
hint, so the receiver must **trial-verify candidate epochs against the
attestation tag** directly, which costs one block per candidate for Tier I and
`N` blocks for Tier II. An advertise-off node is therefore slower to re-acquire,
not undiscoverable to a keyholder. Both paths are asserted, not just the first.

### 11.4 Layer B — attested clear broadcast, in two tiers

The threat model decides which tier is which. A stranger's sensor on an
overlapping device number is an **identity** question, it is the mainstream
case, and Tier I answers it. Eavesdropping is a confidentiality question and it
is section 11.5. **Injection of forged values is in neither** — shipping the
expensive mechanism that defends against it, on by default, on every strap, was
answering a question nobody in the threat model asked.

Both tiers share one domain byte and are separated by a subtype:

```
nonce_block = epoch[4 LE] || devnum[2 LE] || att_counter[2 LE] || dom || sub || 0x00 x6
                dom = 0x01 CTR keystream | 0x02 spread MAC | 0x03 descriptor MAC
                    | 0x04 compat MAC
                sub = 0x01 Tier I | 0x02 Tier II | 0x03 SWITCH/RETURN announcement
```

**The subtype is inside the nonce and not only in the page.** Without the domain
byte a compat tag and a spread tag can coincide; without the subtype *in the
MAC'd block*, a Tier I and a Tier II tag over the same counter value are
distinguished only by a page byte an attacker chooses. Section 3.3's block is
extended at position 9 rather than duplicated, and positions 10..15 stay zero.

#### Tier I — identity attestation. Default on.

One self-contained page every `T` seconds, **decoupled from the data rate**:

```
[0]     attestation page number, subtype nibble = I
[1..2]  attestation counter, low 16 bits, LE (monotone, derivable from time,
        carried explicitly)
[3..7]  trunc40( CMAC(K_auth, nonce_block) )
```

- **It covers no payload, and that is the point.** The page proves "this stream
  comes from the holder of `K_auth`, now, and is not a replay" and nothing else.
  Because nothing outside the page is covered it is **verifiable on receipt**,
  and a lost packet anywhere else costs nothing. See section 7.7.
- **`T` default 20 s, manufacturer-configurable.** At 4 Hz that is one page in
  ~81, **1.2% of slots**. `T` is in *seconds*, so a slower profile spends
  proportionally less, not more.
- **Replay is closed by the counter, not by payload coverage.** `att_counter` is
  monotone and derivable from elapsed time; a receiver rejects a counter it has
  already seen and re-anchors after a gap. A wrap advances the epoch on both
  sides, exactly as section 3.5 requires.
- **40 bits of tag, not 48**, because the counter needs two bytes in-page now
  that no window index is implied. 2^-40 per attempt against a mechanism
  rate-limited to one attempt per `T` by construction is not the weak link.

#### Tier II — data attestation. Default off.

The original window CMAC, unchanged and now correctly scoped, for a deployment
that genuinely fears injected values rather than colliding sensors:

```
[0]     attestation page number, subtype nibble = II
[1]     window index, low 8 bits
[2..7]  trunc48( CMAC(K_auth, nonce_block || p_1 || p_2 || ... || p_{N-1}) )
```

- **`p_i` is the full 8 transmitted payload bytes**, in transmission order, page
  number included. That is what keeps the mechanism profile-agnostic:
  `radiant_sec_compat.c` never learns what a heart-rate page is.
- **The window is `N` consecutive transmitted messages, not `N` data
  messages**, so the tag covers the common pages, the beacon and the Tier I page
  too.
- **`N in {4, 8, 16, 32}`, default 8 when enabled**, announced in the beacon.
- **The honest regression:** a window CMAC is not self-synchronising under loss
  the way section 3.2's spread tag is, so any lost packet in the window makes it
  unverifiable. Section 7.7 has the numbers.

#### Both tiers ride one page allocation, and downgrade protection is receiver-side

The subtype nibble in byte `[0]` separates them, so the allocation stays at
**two page numbers, not three** (beacon, attestation) — see section 11.8.

Strip the beacon and a naive receiver falls back to clear. The fix is pinning,
and the machinery exists: a channel with a key installed and attestation
expected **reports UNVERIFIED rather than CLEAR** when no attestation arrives.
`CLEAR` keeps meaning "no key here". **Sensor-side advertising is a discovery aid
and never a security decision.** A receiver that completes enrolment pins that
sensor, and unpinning is a deliberate user action.

### 11.5 Layer C — the private-mode switch

Not an extra channel. **The node stops being an ANT+ sensor and becomes a
RadiANT one.**

```
COMPAT      type 0x78 / 0x0B, clear + attested                <- steady state
   |  trigger: physical action, or an authenticated command from a paired keyholder
   |           (never an unauthenticated over-air request)
   v
ANNOUNCING  SWITCH frames in the clear over a K-message countdown, beacon
   |        promoted to 1 in 8. Skipped entirely when announce = silent
   v
PRIVATE     type 0x60, telemetry envelope, X_CONF + X_AUTH    <- existing, unmodified
   |  revert: authenticated command, physical action, bounded maximum duration,
   |          power cycle, channel close
   v
RETURNING   RETURN frames from the private channel, same countdown shape
   |        Skipped when announce = silent, and unavailable on a power cycle or crash
   v
COMPAT
```

`private_policy = always` boots straight into PRIVATE and never enters COMPAT —
that is today's `radiant_sec` node, named as a policy state so the axis is
complete.

**The switch is announced to everyone, in the clear, or 1:N is broken.** The
command comes from *one* keyholder; the stream has *N* listeners. A node that
acts on it and vanishes gives the receiver that asked a clean handover and every
other keyed receiver a dropout — a feature that works on the bench with one head
unit and fails in the room.

SWITCH and RETURN are frame indices in the **beacon page's** existing frame set,
which grows to four frames for the duration of a countdown:

```
frame 2   [1] = 0x23   SWITCH/RETURN frame A
  [2]     target device type (bits 6:0); bit 7 must be 0
  [3..4]  target device number, LE
  [5..6]  target channel period, LE
  [7]     bits 7:6 reason: 0 command, 1 physical, 2 timeout-revert, 3 reserved
          bits 5:0 countdown, in units of one promoted beacon interval
                   (8 transmitted messages)

frame 3   [1] = 0x33   SWITCH/RETURN frame B
  [2..7]  trunc48( CMAC(K_auth, nonce_block || frame_A) ), sub = 0x03,
          att_counter from Tier I
```

**Frames 0 and 1 restate the new count for the duration**, becoming `0x03` and
`0x13` rather than the steady state's `0x01` and `0x11`. That is not a second
convention: byte `[1]` is `(index << 4) | (count - 1)` and the count is the size
of the set the frame belongs to, so a set that grows to four says four in every
frame of it. It is written down because it is the obvious thing to get wrong —
the frame indices that change are the ones a reader looks at, and leaving frames
0 and 1 saying "two in this set" would tell a receiver that heard only those two
that no announcement was running.

**The countdown in frame A's byte `[7]` counts promoted beacon intervals, not
messages.** Six bits of messages would reach 63 and the longest legal countdown
is `K = 128`; six bits of eight-message intervals reaches 504. Multiply by
`8` — the promoted beacon rate, which is exactly why the countdown is expressed
in it — to get messages.

Five rules, each load-bearing:

- **Clear, but self-authenticating — it carries its own tag.** Tier I covers no
  payload and Tier II is off by default, so the announcement cannot lean on
  either: frame B tags frame A's full 8 transmitted bytes. It is verifiable on
  receipt, needs no window to close, survives loss of every other packet, and
  replays fail on the counter. **A receiver acts on a SWITCH frame only after
  its tag verifies**; an unverified one is logged, counted and ignored, and the
  cost of ignoring it is the re-acquisition path, which is already built.
- **Therefore private mode requires a key.** Without a tag the announcement is an
  unauthenticated "everybody follow me to channel X" — a one-packet herding
  attack strictly worse than the mute attack this design already refuses. See
  the dependency in section 11.6 and its one exemption.
- **The countdown is long enough to be received, and the beacon rate rises to
  meet it.** During a countdown **beacon slots are promoted to 1 in 8**, aligned
  to the attestation window. Default `K = 64` messages (~16 s at 4 Hz) gives ~8
  copies; `K in {16, 32, 64, 128}`. The airtime cost is real — ~12% of slots —
  and it is bounded to the countdown and paid only at a switch.
- **Receivers act on countdown expiry, not on receipt.** A late-joining receiver
  reads the remaining count out of the frame, so every keyed receiver retunes on
  the same message. That is what makes a handover look like a handover rather
  than N independent dropouts.
- **The return trip is announced the same way.** The revert paths that cannot
  announce — power cycle, crash — are exactly the ones where the receiver
  searches anyway, and the compat channel is what it finds.

**The switch lands on 1 M GFSK / RF 57 and moves afterwards through the ordinary
descriptor mechanism.** One change at a time. A SWITCH frame carrying target PHY
and RF index would arrive private, coded and off-57 in one step, and a keyholder
that missed it would have to search PHYs and frequencies as well as device
numbers — turning a solved re-acquisition problem into an unsolved one.

#### The locator is derived, which is what makes the announcement optional

```
private devnum = trunc16( CMAC(K_id, "priv" || epoch) ), 0x0000 excluded (ANT wildcard)
collision:       rederive with a 1-byte suffix, 0x00, 0x01, ... ; a searching
                 keyholder tries the first four before falling back to a wildcard
                 search on the private device type
```

Any holder of the root key computes where the node went from the epoch it
already has. That does three things: the SWITCH frame becomes an **optimisation
that saves a search rather than the only way to find the node**; a newly
enrolled receiver finds a node that is *already* private with no announcement to
have missed; and an observer without the key cannot predict the locator.

`devnum = per-boot` (section 11.6) derives the compat device number the same
way, under the distinct label `"boot"` rather than `"priv"`, so that a node's
compat number and its private number are never equal and never derivable from
one another.

#### `announce` is a setting, and `silent` is a supported mode

| `announce` | On air | Existing keyholders | Observer without the key |
|---|---|---|---|
| **`broadcast`** (default) | SWITCH/RETURN frames, countdown, beacon promoted to 1 in 8 | follow on the same message; gap ≈ retune time | learns X and Y are one node, cheaply |
| **`silent`** | nothing; the channel simply closes | rederive the locator and re-acquire; gap ≈ search time | timing correlation only |

**`silent` is not "the announcement failed".** No SWITCH frames, no locator
fields, no pending-switch bit, no beacon promotion. **`silent` buys
unlinkability and pays in availability; `broadcast` is the reverse. Neither is
"more secure"**, and the docs quote the measured gap rather than "a short
search".

**A close, not a mode flip on the same channel.** Keeping device type `0x0B`
alive while emitting only unknown page numbers leaves a head unit showing a
connected sensor with no data — a failure mode users report as a bug. Dropping
the channel looks like walking out of range, which users understand. **While
private, a Garmin head unit and Zwift see nothing at all.** That is the price of
confidentiality and there is no version of it that is cheaper — and it is what
the Kconfig help text says, because the person choosing this is reading
`menuconfig`.

### 11.6 Policy is configuration, and the default is that none of this happens

A RadiANT strap must be configurable as a plain, permanently-open ANT+ sensor.
That is not a degraded mode; it is the setting most straps should ship in.

| Setting | Values | Default | What a legacy receiver sees |
|---|---|---|---|
| `advertise` | off / on | **on** | one extra unknown page in 121 (0.8%) |
| `attest_id` (Tier I) | off / on, interval `T` | **on, `T` = 20 s** | one extra unknown page in ~81 at 4 Hz (**1.2%**) |
| `attest_data` (Tier II) | off / on, `N in {4, 8, 16, 32}` | **off** | when on, one page in `N` — the expensive tier |
| `private_policy` | `never` / `physical` / `command` / `always` | **`never`** | `never`: a normal sensor, forever |
| `announce` | `broadcast` / `silent` | **`broadcast`** | nothing either way; SWITCH frames are an unknown page number |
| `enrol` | `closed` / `physical` / `open-window` | **`physical`** | nothing; pairing frames are an unknown page number |
| `devnum` | `fixed` / `per-boot` | **`fixed`** | `fixed`: a normal sensor. `per-boot`: re-pairs each session |
| `period` | the target profile's permitted rates | **the profile's standard rate** | must match, or the receiver cannot track at all |

**`private_policy` defaults to never**, and the four states are:

- **`never`** — a byte-exact ANT+ sensor for its whole life. It refuses an
  authenticated switch command from a keyholder it trusts, and **counts the
  refusal**.
- **`physical`** — switches only on a physical action at the node (button,
  magnet, strap re-seat). No over-air path exists at all.
- **`command`** — physical action *or* an authenticated command from a paired
  keyholder, using the existing `RADIANT_SEC_LABEL_CMD` key. A strict superset
  of `physical`. Its bytes are below.
- **`always`** — never appears as an ANT+ sensor; boots straight onto `0x60`.

Three rules on top:

- **Policy is set out of band, never over the air.** A `never` node that a
  stranger — or a paired keyholder — can talk into becoming a `command` node has
  no policy; it has a suggestion. Downgrading `command` -> `never` is likewise
  not an over-air operation: it would be a mute attack wearing a safety hat.
- **Three surfaces, one value, one precedence rule: NVM if provisioned, else
  Kconfig; the host message writes NVM** rather than shadowing it. Kconfig gives
  the compile-time default (`RADIANT_SEC_COMPAT_POLICY_*`, default `NEVER`); a
  new host message in the shape of `0xF1`'s switch-bit byte serves a dongle or a
  bench node; the persisted value of ADR 0009 is the authority for a hostless
  strap. Three sources with an unstated precedence is a bug report waiting six
  months.
- **Attestation is independent of `private_policy` in one direction only.**
  Attestation with policy `never` is a perfectly good configuration and probably
  the most useful one here: a strap that is byte-exact ANT+ to every receiver in
  the room and provably itself to the one holding its key. The reverse is
  refused — `private_policy != never` requires `K_auth` and Tier I, because a
  `command` trigger needs an authenticated command and a `broadcast`
  announcement needs its frame-B tag. **`physical` + `silent` is the one
  exemption**: with no announcement there is nothing to forge and no over-air
  trigger to authenticate, so that combination is permitted with both tiers off
  — a button-only, manually-provisioned node whose only on-air security surface
  is the ciphertext itself.

#### The command's bytes, and the key they are under

The trigger is one inbound eight-byte message, arriving as **acknowledged
data** exactly as an enrolment frame does:

```
[0]     page number. Not examined by the receiving layer - the caller has
        already filtered on it - but COVERED BY THE TAG, so nothing an
        attacker can rewrite sits outside it
[1]     0x00 = (index 0 << 4) | (count 1 - 1): a ONE-FRAME SET on the framing
        convention of section 11.5. That is what keeps the command off a page
        number of its own, and what makes it unmistakable for an enrolment
        frame, whose count is six
[2]     the operation: 0x01 go private, 0x02 return to compat. TWO EXIST
[3..7]  trunc40( CMAC(K_cmd, nonce_block || [0..2]) ), sub = 0x04
```

- **Under `K_cmd`, not `K_auth`, and that is the whole reason the key hierarchy
  has a fourth label.** Every receiver holds `K_auth` in order to *verify* the
  node; if commands were signed with it, every receiver could mute the sensor
  for every other one. `sub = 0x04` separates it inside the block as well,
  because "it is under a different key" is a property of the deployment rather
  than of the block.
- **The counter is Tier I's, derived from time on both sides and carried
  nowhere.** A recorded command replayed into a later interval fails the tag.
  There is no counter field a sender could choose.
- **There is no policy operation and its absence is deliberate.** An
  unrecognised operation is counted and dropped rather than reserved for a
  later meaning, so a well-tagged message shaped like a policy change changes
  nothing. See the rejected alternatives in ADR 0008.
- **Accepted commands are rate-limited**, one per minute by default, measured
  from the last *accepted* one. That is not a defence against an attacker - the
  tag is that, and a party holding `K_cmd` is a keyholder - it is a bound on
  what one buggy or captured receiver can do to the other N-1 listeners, each
  of whom pays a countdown and a retune for every command the node takes.

**The Kconfig symbols**, in `src/profiles/Kconfig` beside the `enrol` choice:
`RADIANT_PRIVATE_POLICY_NEVER` / `_PHYSICAL` / `_COMMAND` / `_ALWAYS`,
`RADIANT_PRIVATE_ANNOUNCE_BROADCAST` / `_SILENT`, `RADIANT_ATTEST_ID`,
`RADIANT_PRIVATE_COUNTDOWN` (K) and `RADIANT_PRIVATE_MAX_S`. The dependency
above is expressed there as `depends on`, so the refused combinations cannot be
selected, and again as a `BUILD_ASSERT` in `src/profiles/profile_private.c`, so
they cannot be reached by a defconfig fragment that bypasses the choice.

**On `devnum`:** `fixed` is the default because a stable device number is a
feature users rely on — people learn their sensor's number, and seeing it is how
they know at a glance that the right device is active, which matters most in
exactly the crowded room where an unfamiliar number would be alarming.
`per-boot` is **identity Tier 2 of section 4**, already approved and opt-in, and
is not a reopening of `X_PRIV`: a re-roll at power-up costs nothing at the link
layer because a rotation never happens while a channel is open. Use section 4's
vocabulary; do not invent a third name for it.

**On `period`:** within a profile the receiver's channel period must match the
sensor's or the channel cannot open at all — a harder failure than skipping an
unknown page. The choice is from the set the target profile's document actually
permits, including its defined half- and quarter-rate variants, enumerated per
profile and registered in `docs/profile-registry.md`.

### 11.7 Layer D — adding a receiver to an existing network

A user buys a second head unit in March and must be able to add it to a strap
set up in January, without re-provisioning the first head unit and without
taking the strap apart. **Enrolment is additive and disturbs nothing**: the group
is rooted in one symmetric key, so adding a receiver means giving that receiver
the root — no epoch change, no re-keying, no interruption, and existing
receivers observe nothing. A verify-only receiver is the same operation with the
same key.

Key delivery is **over-air pairing on a physical trigger for anything without a
port, and host/USB provisioning for anything with one.** A printed key or QR
code genuinely defeats MITM rather than merely mitigating it and remains the
recommendation for a product with a manufacturing step; it is not a v1 bench
answer. The `enrol` setting picks the path:

- **`closed`** — no over-air enrolment ever. This is what
  `radiant/include/radiant/radiant_sec.h` already recommends for
  anything that matters, and that recommendation is not weakened here.
- **`physical`** (default) — a bounded window opened by a physical action,
  reusing `RADIANT_SEC_PAIR_TIMEOUT_DEFAULT_S = 60`. One pairing per window.
- **`open-window`** — a window a *current* keyholder opens with an authenticated
  command, for a node with no button and no host. Rate-limited, bounded, and the
  weakest of the three; its Kconfig help says so.

Four rules:

- **Enrolment works while the node is private.** The window opens on whichever
  channel the node is currently on — the frames are additional pages, not a
  channel change — so existing listeners keep their stream throughout. A node
  does not have to leave private mode to gain a receiver.
- **A newly enrolled receiver finds an already-private node without an
  announcement**, because the locator is derived. Enrolment therefore does not
  depend on the `announce` setting at all.
- **Enrolment is visible to the receivers that already exist.** The pairing-open
  state is a beacon bit and a descriptor bit, and a completed enrolment
  increments a counter in `struct radiant_sec_stats` that the `0xF4` report
  already carries. An enrolment the owner did not perform is the whole attack;
  making it silent would be the mistake.
- **The screen-free fingerprint limit of section 7.4 applies unchanged and is
  not solved here.** A strap with no display gives a one-sided fingerprint; the
  mitigations are the bounded window, one pairing per window, a physical
  trigger, and optionally reduced TX power. `closed` remains the recommendation
  for anything that matters. Section 7.4's amendment of 2026-08-11 is the long
  form and is not restated here so that there is one copy of it.

Removal is not the mirror of addition — section 7.3.

#### 11.7.1 The bytes: a six-frame set, in the beacon page's own set

Enrolment gets **no page number of its own**, for the reason 11.8 gives about
the 7-bit namespace and for the reason SWITCH and RETURN get none: the public
key rides the beacon page's existing frame set as six additional frames, and
the set grows from two to eight for the duration of the window.

```
frames 0, 1   the ordinary beacon (11.2), with the pairing-open bit set and
              byte [1] restating the new count: 0x07 and 0x17, not 0x01/0x11
frames 2..7   [1] = ((2 + k) << 4) | 0x07,  k = 0..5
              [2..7]  bytes 6k .. 6k+5 of the 36-byte enrolment set
```

The 36-byte set is **32 bytes of X25519 public key followed by a 4-byte set
check**, `trunc32( CMAC(all-zero key, pubkey) )`. Six frames of six payload
bytes is exactly 36, so there is no spare byte and nothing whose meaning is
undecided.

**The set check is integrity, not authentication**, and the distinction is what
it is for. There is no shared secret during pairing, so an attacker who rewrites
the key rewrites the check with it — that attack is the man in the middle and
the fingerprint is what answers it. What the check catches is the failure the
framing convention cannot: byte `[1]` says *which frame* this is and never
*which key it belongs to*, so a receiver that joined as one window closed and
another opened would otherwise splice two halves into a key neither end holds,
derive a shared secret that is simply wrong, and see no error anywhere. Four
bytes make that a rejected set instead of a silent one.

**The beacon rate is promoted to 1 in 8 for the duration**, the same rate
11.5's countdown uses, and here it is arithmetic rather than symmetry: at the
steady one-frame-per-121-message cadence an eight-frame set needs four minutes
at 4 Hz, and a 60-second window would close having transmitted a quarter of a
public key. At 1 in 8 the set goes out in 16 s and repeats three times inside
the window. The cost is 12.4% of slots, bounded by the window, and paid only
while a user is holding a button. Messages 119 and 120 are never displaced, so
the common-page interleave is untouched.

**A node with `advertise = off` has no beacon frames, so its set is the six
enrolment frames alone** — count 6, indices 0..5 — and it carries no
pairing-open bit because it has no capability field to carry one in. The window
is still visible: the frames themselves are.

**The peer answers over ANT acknowledged data**, six eight-byte payloads
carrying **its own** six-frame set — count is always 6 and the indices are 0..5,
because the receiver has no beacon set to extend. Acknowledged rather than
broadcast so the peer learns each frame arrived; six independent exchanges
rather than a burst because a burst fails whole and these frames are
individually indexed and idempotent, so a lost frame costs a retransmission
rather than a key. Nothing new is built for this: `radiant_transfer_on_data()`
already acknowledges an inbound packet inside the measured 1.55 ms turnaround
and hands the payload up through `radiant_transfer_ops::rx_data`.

**The interlock, recorded before it bites.** Layer C's SWITCH and RETURN frames
are pinned at indices 2 and 3 (11.5), which is where the enrolment block also
starts. A countdown and an enrolment window must not run at once, and whichever
arrives second is refused. There is one client of that frame set today; the
arbitration belongs to the phase that adds the second.

#### 11.7.2 The counter, and where a host can read it

A completed enrolment increments `enrolments` in `struct radiant_sec_stats`,
bumped inside `radiant_sec_pair_peer()` after the root key is installed — so it
counts pairings that took rather than pairings that were attempted, and a
refused small-order peer key leaves it alone. It counts the `0xF5` host exchange
and the over-air window alike, because from the group's point of view they are
one event.

**It is not in the `0xF4` report yet, and that is a deferral rather than an
oversight.** `0xF4`'s payload is a *fixed* 23 bytes and every one of them is
spoken for through `[22]`, so surfacing the counter there is a protocol
extension: `protocol/ant_wire.yaml` and everything `scripts/gen_ant_wire.py`
generates from it, `tools/ant_sec.py`'s `STATUS_LEN`, and every host that reads
the message. That is a host-surface change and it belongs with one, not with the
phase that created the counter. A keyholder on the same device reads it through
`radiant_sec_get_stats()` today.

### 11.8 The page allocation, and the conventions the checker enforces

Page numbers are a **per-profile namespace**, and heart rate is the tight one:
byte 0's high bit is a page-change toggle, so its page numbers are 7-bit and
nothing >= `0x80` is expressible. Two numbers are needed and they must be **the
same numbers in every compat profile**, so a receiver has one rule.

- **The allocation is exactly two page numbers, not three.** SWITCH and RETURN
  do not get their own number: they are frame indices in the beacon page's
  existing frame set (section 11.5), displacing ordinary beacon frames for the
  duration of a countdown. The two attestation subtypes ride one contiguous,
  nibble-aligned attestation claim rather than two independently verified
  numbers. A third page number is recorded and rejected on the 7-bit namespace
  alone; nothing else about the design would change.
- **`device type 0x79 can never carry an additional page`**, permanently — it
  has no page-number byte, so an inserted page decodes as speed and cadence and
  steps four accumulators. `0x7A`/`0x7B` are checked individually before either
  is included.
- The numbers themselves are confirmed against each profile document and
  registered in `docs/profile-registry.md`, where every compat row carries the
  token **`RadiANT compat`** in its Name column so the allocation cannot
  silently drift. `scripts/check_profile_registry.py` asserts the arity, the
  7-bit bound, the cross-profile agreement and the `0x79` exclusion.

**The confirmed allocation is `0x70` beacon, `0x71`-`0x72` attestation**, the
same numbers on `0x0B` and `0x78`. The attestation claim is a **contiguous,
nibble-aligned pair** rather than a single number, and that follows from the
pins above rather than relaxing them: byte `[0]` is the page number, the subtype
nibble lives in byte `[0]`, and neither tier has a spare bit anywhere else —
Tier I spends `[1..2]` on the counter and `[3..7]` on a 40-bit tag, Tier II
spends `[1]` on the window index and `[2..7]` on a 48-bit tag. So the page byte
is `0x70 | sub`, derived from the same nibble the nonce carries at position 9,
and the two tiers cannot share one byte-`[0]` value. What "two page numbers, not
three" refuses is a **third independent claim** — a Tier II page allocated
elsewhere in the namespace and verified on its own terms — and that stays
refused. `0x73`, which subtype `0x03` would imply, never appears on air at all:
the announcement rides the beacon page's frames 2 and 3, which is precisely how
the third number is avoided.

Two risks recorded rather than solved: a manufacturer-specific page number is
only unique per manufacturer id, so collision with a vendor's private page on
`0x0B` is possible — mitigated by the beacon's version field and by the
attestation MAC failing closed; and the heart-rate toggle-bit sequence must not
be disturbed by inserted pages, which is **a bench question against a real head
unit, not a documentation question**.

### 11.9 What compat mode does not defend against

- **Injected values, unless Tier II is on.** Tier I proves who is speaking, not
  what they said. That is the threat model's answer, not an oversight.
- **A watched switch. `Tier 2 unlinkability does not survive a switch that an
  observer watches`** — a passive observer present at the moment learns that
  compat device number X and private device number Y are the same node. It would
  learn the same from the timing of one disappearing as the other appears, so
  the announcement does not create the linkage; it makes it cheap. `silent`
  narrows it to timing correlation and does not remove the fact that the node
  was visibly an ANT+ sensor before the switch.
- **Confidentiality on someone else's receiver.** The honest sentence, and it
  belongs in the README rather than buried here: **RadiANT gives integrity
  everywhere and confidentiality wherever you own the other end.** A rider with
  their own head unit or their own PC running Zwift through a RadiANT dongle
  gets full privacy; a rider on a gym's equipment does not, and cannot, because
  those receivers only understand ANT+.
- **Jamming and channel flooding.** Attestation lets a receiver ignore a forged
  sensor; it cannot stop a hostile transmitter occupying the RF channel.
  Availability against an active attacker is out of scope for any design that
  lives inside ANT.
