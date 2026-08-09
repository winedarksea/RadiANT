# RadiANT security: three independent switches, all off by default

Checked by: nothing — treat as narrative. Nothing in this document is
implemented; Phase 7 turns it on. What *is* checked is the space it needs:
`scripts/check_profile_registry.py` asserts that the reserved page range
`0x20-0x2F` and the descriptor's epoch and counter fields stay claimed in
`docs/profile-registry.md` and `docs/radiant-telemetry.md`, which is the part
that cannot be retrofitted.

This document is written in Phase 4 rather than Phase 7 on purpose. The
switches below constrain the page envelope — room for an epoch, a counter and a
MAC byte — and discovering that after the first profile ships means a format
break for every deployed node.

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
   broadcasts a **fixed 16-bit device number forever**. Turning encryption on
   does not change the device number; it is outside the encrypted payload
   because the receiver's hardware filter has to match on it. So anyone can
   track a rider across sessions, across gyms, and across locations, by device
   number alone, whether or not the payload is encrypted. BLE fixed this a
   decade ago with resolvable private addresses.
4. **No Windows host can use it.** `ANT_DLL.dll` exports no encryption call at
   all — not `ANT_EncryptedChannelEnable`, not `ANT_SetCryptoKey`, none of
   them; Zwift resolves none of them either. The feature is dead code that
   costs stack RAM shared with the plain channels. `CONFIG_ANT_DONGLE_ENCRYPTION`
   in this repo is off by default for exactly that reason, and its Kconfig help
   explains it.

RadiANT does not implement ANT+'s scheme. It replaces it.

---

## 3. Three independently selectable switches, not a ladder

The correction that matters most is structural: these are **three orthogonal
per-channel switches**, and any of the eight combinations is legal. A ladder
(none -> encrypted -> encrypted+authenticated -> ...) never reaches the
combinations that turn out to be the useful ones.

| Switch | Property | Payload cost |
|---|---|---|
| `X_PRIV` | unlinkability | **0 bytes** |
| `X_CONF` | confidentiality | **0 bytes** |
| `X_AUTH` | authenticity | **1 byte/packet** |
| *(v2)* | non-forgeability among receivers | v2 |

### `X_PRIV` — unlinkability

```
epoch     = floor(t / 128 s)
devnum_e  = trunc16(HMAC(K_id, epoch))
```

Both ends compute it. There is nothing on the air: the device number *is* the
rotating value, so this costs **zero payload bytes**.

The receiver programs its hardware address filter for the current epoch's
`devnum_e`, and across an epoch boundary listens on **both** the outgoing and
incoming values for a guard window — two of the nRF's eight filter slots, which
the wildcard-search design already budgets for. Clock drift is not a problem at
this granularity: 50 ppm over 128 s is 6.4 ms, three orders of magnitude below
the guard window a boundary needs.

A `devnum_e` of 0 is invalid on an ANT channel (0 is the wildcard); the
derivation retries with a counter appended until it lands in 1..65535. Two
sensors in one room can collide on a 16-bit device number, which is already
true today and already handled by the fact that device type and transmission
type also have to match.

### `X_CONF` — confidentiality

```
K_enc = HKDF(K_master, "enc" || epoch)
nonce = epoch(4) || devnum_e(2) || counter(2)
```

AES-128-CTR keystream from the nRF ECB peripheral or the EFR32 crypto
peripheral: about 7 us per block, which at 4 Hz is energy nobody can measure.

The counter is the packet index within the current epoch, reconstructed on the
receiver from the data page's own event counter (`docs/radiant-telemetry.md`
byte `[1]`) plus rollover tracking. Because `K_enc` is re-derived every epoch
and an epoch is 128 s, the counter only has to be unique within 128 s — 512
packets at 4 Hz — so a 16-bit counter cannot roll over inside its key's
lifetime. Data pages use counters with the top bit clear; the (rarely)
encrypted descriptor set uses the top bit set, so the two streams cannot
collide on a nonce.

Bytes `[0]` (page number) and `[1]` (event counter) stay in the clear so a
receiver can tell a data page from a descriptor and can build the nonce. That
is what makes `X_CONF` cost **zero payload bytes**.

Replay is detectable — not prevented, detectable — via highest-counter-seen per
epoch. A replayed packet decrypts to a counter the receiver has already
consumed and is dropped. Detection, not prevention, is the honest word: the
attacker can still transmit.

### `X_AUTH` — authenticity, via a spread MAC

A 32-bit tag computed over a window of **W** consecutive packets, transmitted
**one tag byte per packet**. The receiver buffers the window, reassembles the
32-bit tag, and verifies the whole window at once.

- Cost: **1 byte per packet**, and full 32-bit authentication strength.
- Detection latency: **W periods**. At 4 Hz with W=4 that is about 1 second.

**W must be configurable, and this is not a detail.** At 0.25 Hz a 4-packet
window is 16 seconds of unverified data, which is far too long for anything but
the slowest logging. Low-rate profiles want W=2, or an inline tag (W=1, a
16-bit tag in the packet itself) and the smaller strength that comes with it.
The envelope carries W in descriptor frame 1 byte `[3]` bits 7..6, so a
receiver reads it rather than being configured with it.

### v2, named and not specified: non-forgeability among receivers

TESLA-style delayed key disclosure. The sensor MACs packet *i* with `K_i`, and
discloses `K_i` *d* packets later; `K_i` comes from a hash chain so a receiver
can verify the disclosure against a previously authenticated element.
Receivers verify retroactively and **cannot forge**, because they learn the key
only after the window in which it would have been useful has closed. It reuses
`X_PRIV`'s loose time synchronisation and needs no new time source.

Named here so the reserved transform-flag bit (descriptor frame 0, bit 4) and
the reserved page range `0x20-0x2F` are visibly for something. Not specified in
v1.

---

## 4. The combinations a ladder never reaches

### `X_AUTH` alone — the most useful setting in the table

*Anyone can read my power, but nobody can spoof it.*

For a race, a segment leaderboard, or any competitive context, this is
arguably the single most valuable configuration RadiANT offers, and a
confidentiality-first ladder cannot express it.

It costs one byte per packet, and — the part that matters operationally — it
has **no key distribution problem for readers**. Only the sensor and the
verifier need the key. Every other receiver in the room still gets plaintext
and works exactly as it does today. Compare that with encrypting the stream,
which requires giving the key to everyone who is allowed to read, which for a
public leaderboard is everyone, which means the key protects nothing.

### `X_PRIV` alone — also coherent

*Read my data freely; you cannot tell it is the same me as last week.*

Zero payload cost. This is the setting that addresses the problem ANT+
encryption never touched, and it does so without giving up interoperability
with any tool that can be told the current device number.

### `X_CONF` alone — what ANT+ did, and the weakest useful choice

Confidentiality with no authentication is malleable, and confidentiality with a
fixed device number does not stop tracking. It is in the table because it is
sometimes what you want, and it is listed last on purpose.

---

## 5. Three rules that follow from open-by-default

### 5.1 Per channel, not per device

A node may broadcast heart rate in the clear on one channel and something
protected on another. The switches are per-channel state.

ANT already has per-channel encryption semantics
(`ant_crypto_channel_enable(channel, ...)`), so this matches the existing API
shape rather than inventing one — which also means the host-side messages in
section 9 take a channel number, like every other configuration message.

### 5.2 The descriptor page stays in the clear by default

Even when data pages do not.

Otherwise a protected node is indistinguishable from noise: it cannot be
enumerated, `tools/ant_scan.py` reports a silent hole rather than a device it
cannot read, and the operator has no way to tell a working encrypted node from
a dead one.

Encrypting the descriptor too is a **further explicit opt-in** (descriptor
frame 0, transform bit 5), and its cost must be stated wherever it is offered:
**the node becomes undiscoverable to anything without the key.** No scan finds
it, no tool lists it, and a lost key means a node that can only be recovered by
physical access. That is occasionally the right answer and it is never the
default.

### 5.3 Zero cost when off

All of it compiles out behind Kconfig, mirroring the existing
`CONFIG_ANT_DONGLE_ENCRYPTION` pattern. A plain ANT+ dongle built without the
switches carries:

- no AES or HMAC code,
- no key material and no key storage,
- no per-channel crypto state competing for RAM with the 32 plain channels.

The Phase 7 gate states this as a testable property rather than an intention:
*a RadiANT build with everything off is byte-identical on air to one built
without the code at all.*

---

## 6. Threat model

Stated as such, and stated to be **not the household case**. In a house, the
default open broadcast is correct and none of this should be enabled.

**In scope:**

| Adversary | Capability |
|---|---|
| Passive eavesdropper | Receives every frame in range and reads the payload |
| Passive tracker | Correlates device numbers across sessions, days and locations to follow a person |
| Active injector | Transmits well-formed frames impersonating a sensor, to spoof a reading or drive an actuator |

**Out of scope:**

- **An authorized receiver.** It holds the group key. This is inherent to
  symmetric one-to-many broadcast and is exactly what the v2 tier exists to
  escape. Any document that claims otherwise is wrong about the mathematics.
- **Jamming.** A 2.4 GHz jammer denies service to every protocol in the band
  and no payload-layer design addresses it.

---

## 7. Five honest limits

Stated here rather than discovered during Phase 7 integration.

### 7.1 Category fingerprinting survives `X_PRIV`

The device type and transmission type must stay **fixed**, because a searching
receiver matches on them — rotate those and discovery breaks. So rotation
defeats *identity* tracking, not *category* tracking. An observer still learns
"a heart-rate sensor is here, and it left at 09:40". It no longer learns that
it is the same heart-rate sensor that was here on Tuesday.

Transmission timing is a second, weaker fingerprint: a node's channel period
and phase are stable across an epoch boundary. `X_PRIV` does not address it.

### 7.2 Any authorized receiver can forge

It holds the same key the sensor does, so it can produce a frame the sensor
would have produced. Inherent to one-to-many symmetric broadcast, and precisely
what the v2 TESLA tier exists to escape.

### 7.3 There is no per-receiver revocation

Cutting one receiver off means **rekeying the sensor and redistributing the new
key to everyone else**. There is no way to exclude one holder of a group key
without changing the group key.

True revocation needs a broadcast-encryption scheme — LKH, subset-difference —
whose per-message overhead is logarithmic in the group size and does not fit an
8-byte payload at any price. That is not a matter of effort; it does not fit.

Fine for a household with three receivers. An operational problem for a team,
a coaching service, or a gym with staff turnover. **Documented, not solved.**

### 7.4 Pairing happens in the clear, and must

An `X_PRIV` sensor is undiscoverable to anyone without `K_id` — that is the
point of it. So there has to be a state in which it *is* discoverable, and that
state is pairing:

1. The node enters pairing mode with **rotation off**: it broadcasts its base
   device number with the **pairing bit set** (the MSB of the device type
   field).
2. A receiver finds it the ordinary way, with an ordinary wildcard search.
3. Key exchange completes (section 8).
4. The node leaves pairing mode, switches to rotating device numbers, and from
   that moment is invisible to anything without the key.

Same shape as BLE. It is specified here because it is the gap that otherwise
turns up during Phase 7 integration, at which point the descriptor format is
already deployed.

Pairing mode is a window of linkability and must be bounded: a node in pairing
mode times out (60 s is the suggested default) and returns to rotating.

### 7.5 A spread MAC lets an attacker inject up to one window before detection

W packets of forged data are accepted before the window's tag fails to verify.
At 4 Hz with W=4 that is one second of wrong wattage, which for telemetry is
fine — the receiver discards the window retroactively and the accumulator
semantics mean the next valid window resynchronises the total.

It is **wrong for anything that actuates.** A window of forged commands is a
window in which the AC turned on. That is why the envelope's reliable-command
page carries an **inline** tag rather than a spread one, and why its
idempotency rule refuses to change state on a bad tag. See
`docs/radiant-telemetry.md` section 9, including the honest limit on a 16-bit
inline tag.

---

## 8. Key establishment

Two paths, and the simple one is the one most deployments should use.

**Out-of-band.** A QR code on the node, an NFC tap, or a key typed in by hand.
No protocol, no code on the node beyond storing 16 bytes, and no attack surface
during pairing at all.

**X25519 over the existing acknowledged/burst path.** At pairing time, the two
ends exchange public keys over an ANT burst — a burst carries 32-byte keys
comfortably — and derive the shared secret. About 1 KB of code and roughly 4 ms
on a Cortex-M4, paid exactly once per pairing. Unauthenticated Diffie-Hellman
is vulnerable to an active man-in-the-middle during that one window; the
mitigation is the usual one (a short displayed or NFC-confirmed fingerprint),
and a deployment that cannot do that should use the out-of-band path.

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
| `0xF1` | `ANTW_MESG_RADIANT_SEC_CONFIG_ID` | Per-channel switch bitmask (`X_PRIV`/`X_CONF`/`X_AUTH`) plus the MAC window W |
| `0xF2` | `ANTW_MESG_RADIANT_SET_KEY_ID` | Install `K_id` / `K_master` for a channel |
| `0xF3` | `ANTW_MESG_RADIANT_EPOCH_ID` | Set or read the epoch and the host time base |
| `0xF4` | `ANTW_MESG_RADIANT_SEC_STATUS_ID` | Read back: current `devnum_e`, epoch, highest counter seen, MAC pass/fail counts |
| `0xF5` | `ANTW_MESG_RADIANT_PAIRING_ID` | Enter/leave pairing mode; carry the X25519 exchange over burst |
| `0xF6-0xFA` | reserved | The rest of the RadiANT family |

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
  argument, so `0xF3` and `0xF4` are readable with no new mechanism.

**Lib config `0xE0` is a different namespace and has not moved.** The
`ANTW_LIB_CONFIG_ALL_EXT_FIELDS` setting — channel ID + RSSI + RX timestamp — is
a payload byte of message `0x6E`, not a message ID, and it is unaffected by any
of the above. The RadiANT message IDs no longer share a byte with it, but the
reason they never conflicted was that the two namespaces are unrelated, not
that the numbers differ.

---

## 10. What is deliberately not being built

- **ANT+'s own AES-CTR scheme in `ant_core`.** No Windows host can call it, it
  is malleable and unauthenticated, and it reserves stack RAM shared with the
  plain channels. `encryption.conf` and the existing sdk-ant-backed writes stay
  exactly as they are; they cost nothing while that backend exists.
- **Per-receiver key revocation.** Section 7.3. Documented, not solved.
- **Anything enabled by default.** Every switch in this document is off, and
  Phase 7's gate is that a build with them off is byte-identical on air to a
  build without the code.
