# 0007 — The long-range coded PHY, and the length extension it unlocks

- **Status:** accepted
- **Date:** 2026-08-11
- **Amends:** [0005](0005-extension-inside-ant-plus.md) axis 4 (renames it, and makes it a decision rather than a reservation); partially unblocks [0005](0005-extension-inside-ant-plus.md) axis 3 **for this PHY only**
- **Depends on:** [0002](0002-clean-room-policy.md) (read scope), [0005](0005-extension-inside-ant-plus.md) (the extension axes and the merged RX window)
- **Related:** [0008](0008-antplus-additive-pages-and-compat-security.md) and [0009](0009-hostless-node-identity.md) — the compatibility plan whose private-mode switch produces the population eligible for everything here

> **Provenance.** Clean-room. Written from this project's own plan and prior
> decision records, from the published Bluetooth core specification's LE Coded
> PHY framing (FEC block structure and coded-symbol timing — public), from
> Nordic's public nRF52840 / nRF5340 / nRF54L15 product specifications and from
> the MDK and nrfx headers shipped in NCS v3.4.0, and from Nordic's
> open-source Zephyr Bluetooth controller (per-SoC chain-delay tables,
> Apache-2.0, already a dependency). Nothing here derives from sdk-ant, from
> `libant.a`, from disassembly of any binary, or from any adopter-gated ANT+
> device profile document. See [0002](0002-clean-room-policy.md).

---

## Context

`radiant_core`'s link-layer correctness work is finished. What is left that can
still move the numbers is sensitivity, and the largest lever available in
software — by a wide margin, and without touching the hardware items that are
out of scope by decision — is the Bluetooth LE Coded PHY.

The HAL has carried a reserved enum value for this since it was written:

```c
RADIANT_PHY_LR_GFSK = 1,   /* Reserved: lower-rate GFSK ... (Phase 7) */
```

Both halves of that comment are wrong, and the second one is only a numbering
slip. This record fixes the first, which is a design decision that was never
made.

## Decision

1. **`RADIANT_PHY_LR_GFSK` becomes `RADIANT_PHY_LR_CODED`, keeping its numeric
   value, and is defined as the Bluetooth LE Coded PHY at S=8** (125 kbit/s),
   with convolutional FEC and pattern mapping performed in hardware.
2. **One rate. S=8 only.** S=2 stays defined in the wire vocabulary and
   deliberately unimplemented.
3. **A new frame configuration, `RADIANT_FRAME_CFG_LR`,** with a 4-byte address
   and a real length field — the first use of `RADIANT_LEN_FROM_BODY` anywhere
   in this project.
4. **Length extension is permitted exactly where the PHY already makes us
   invisible.** Not "on RadiANT device types"; not "off RF 57". On a PHY a
   stock receiver physically cannot demodulate.
5. **ANT+ compatibility channels are permanently excluded** from this PHY and
   from the length extension. Not deferred — excluded.
6. **Discovery never moves.** The search sweep stays 1 M on RF 57 forever.
7. **A node's frame must stay under 25 % of its channel period at its announced
   rate**, enforced by encoder refusal.

---

## Why coded, and not plain low-rate GFSK

The reserved enum said "lower-rate GFSK", and the difference between that and
what is being built is most of the value of this phase.

Narrowing the modulation and halving the symbol rate buys roughly 3 dB per
factor of two of bandwidth, and it buys it only if the receiver's filter is
narrowed to match — which on these parts means a different, non-ANT
configuration, and a link that is still uncoded and therefore still fails at the
first burst of interference. To get 8 dB that way needs roughly a factor of six
in rate, a soft-decision receiver nobody in this project is going to write, and
a modulation format with no second implementation anywhere to check against.

LE Coded gets ~8 dB from a convolutional code and pattern mapping that are
**already in the silicon** on every part in view, that a decade of shipped BLE
has exercised, and that cost one register write to select. There is no
competition between these two options; the reservation was written before the
question was looked at properly.

**Consequence recorded honestly:** we are now using a Bluetooth PHY to carry a
frame that is not Bluetooth. That is fine and is exactly what the nRF RADIO
peripheral permits — `MODE` selects the PHY, `PCNF0`/`PCNF1` select the packet
layout, and they are independent — but it means the on-air result is not a
legal BLE packet and no BLE receiver will accept it. That is a feature here,
not a defect, and it is the same isolation argument the length extension rests
on.

## Why S=8, and only S=8

On LE Coded, **FEC block 1 is always coded at S=8 regardless of the selected
data rate.** That is hardware, not a choice, and it is the whole of the
argument. FEC block 1 carries the preamble, the 32-bit access address, the
coding indicator and TERM1:

| FEC block 1 | bits | µs at 8 µs/bit |
|---|---|---|
| preamble | 10 symbols | 80 |
| access address | 32 | 256 |
| coding indicator | 2 | 16 |
| TERM1 | 3 | 24 |
| **total** | | **376** |

For an eight-byte payload, the plan's reference table:

| | FEC1 | FEC2 | Frame | Duty at 4 Hz |
|---|---|---|---|---|
| 1 M | — | — | ~150 µs | 0.06 % |
| S=2 | 376 µs | ~214 µs | ~0.59 ms | 0.24 % |
| S=8 | 376 µs | ~856 µs | **~1.23 ms** | **0.49 %** |

The fixed 376 µs dominates at these payload sizes, so **S=2 is only 2.1×
cheaper than S=8, not 4×.** The last ~3 dB costs 0.64 ms of extra radio-on time
per frame — a quarter of a percent of duty at 4 Hz. Nothing in this design is
short of that, and trading ~3 dB of sensitivity for it is the wrong side of the
trade.

One rate is also one format, one `phy_switch_us`, one `t_sync` calibration and
one set of bench numbers. Simplicity is a stated project value and this is
where it is cheapest to buy.

**S=2 stays in the wire vocabulary with no code behind it.** RF-5a already
defined the 3-bit coding-rate field (`0` uncoded, `1` S=8, `2` S=2 reserved)
precisely so that this phase would inherit a vocabulary rather than invent one,
and so that a second rate arriving later is a value rather than a format break.
This phase makes rate 1 work on the radio; it does not redefine the vocabulary.
`profile_sched_coding_implemented()` returns false for rate 2, the encoder
refuses to *announce* it, and the decoder accepts it and declines the channel —
a receiver's job on meeting a rate it cannot use is to decline, not to reject
the node.

**Reconsider only** if a RadiANT-native control type needs a genuinely tighter
slot budget — and then re-check both sensitivity figures against the bench
part's own datasheet. The −95/−103 dBm pair quoted in early planning is
nRF52840; the bench part is nRF54L15.

### What the implementation actually costs, against the table above

The table assumed a three-byte header. The format built here carries four,
because the length byte the extension needs is one of them. Reproducing the
arithmetic for what was actually built:

- **address** 4 bytes — and it costs nothing extra, because it *is* the coded
  PHY's fixed 32-bit access address, already inside FEC block 1's 376 µs.
- **body** `[length][device type][transmission type][control]` + payload.
- **FEC block 2** = (body + 2 CRC bytes) × 64 µs, + TERM2 24 µs.

| payload | body | frame at S=8 |
|---|---|---|
| 0 | 4 | 784 µs |
| 8 | 12 | **1296 µs** |
| 36 (longest) | 40 | **3088 µs** |

So an eight-byte frame is 1.30 ms rather than the table's ~1.23 ms. The 64 µs
difference is the length byte. The FEC-block arithmetic is reproduced exactly;
only the byte count it is applied to moved, and it moved because the format
stopped being hypothetical. `radiant_frame_airtime_us()` is the single
implementation of this, and `test_lr.c` pins all three rows.

---

## The length extension

ADR 0005 axis 3 — longer frames — is **correctly withdrawn**, and stays
withdrawn. It was withdrawn for two reasons, both specific to *inferring* a
length from an ANT frame:

1. byte 3 is not a length. It reads 10 on a broadcast and 2 on an in-slot frame
   for the same eight payload bytes, so no mechanism for a non-eight-byte ANT
   frame exists or can exist;
2. it would cost every merged RX window.

**Neither applies here, and the reasons are different in kind rather than in
degree:**

- **Authorship.** This format is *written* by this project, not inferred from
  somebody else's. The length byte is at an offset we chose, in a body we
  defined, with a meaning we stated. `RADIANT_LEN_FROM_BODY` has existed unused
  in the HAL since it was written, labelled "NO ANT FORMAT USES THIS MODE, and
  none ever can" — which remains true, and which is a statement about ANT.
- **The merged window.** An LR channel can never join the merged 1 M window
  anyway. It is a different PHY, and `radiant_sched.c`'s membership rule is
  pointer equality on the format — so the cost axis 3 was withdrawn over is
  structurally unavailable here rather than merely unpaid.

**A third argument, which is stronger than the one ADR 0005's revisit trigger
asked for.** That trigger wanted a fresh network address, because a stock
receiver listens on `A6 C5`. A stock receiver **cannot demodulate coded PHY at
all.** It does not fail to parse our frame; it does not see one. Isolation here
is *physical*, and physical isolation is strictly stronger than a second
network address — which is a convention that both sides have to honour.

> **The rule, stated so it cannot be generalised by accident:**
> **length extension is permitted exactly where the PHY already makes us
> invisible.**

That is deliberately not "on RadiANT device types" and not "off RF 57". A
device type is a byte in a frame everyone can hear; a frequency is a place
everyone can tune to. Neither makes a malformed frame unheard. Only the PHY
does.

### What it buys

- **The descriptor set collapses.** The largest battery item here — see below.
- **An inline command tag.** `docs/radiant-telemetry.md` §9 calls a 16-bit tag
  "not a trade-off a node can decline; it is the limit". A 64-bit tag makes the
  actuator path defensible rather than rate-limit-mitigated, and this is the one
  place 2⁻¹⁶ per attempt is genuinely uncomfortable.
- **Multi-field data pages stop being a packing puzzle.**

### Bounded by airtime, not by taste

`max_body_len` on the LR format is **40**, and `RADIANT_RADIO_BODY_MAX` is
raised from 32 to 40 to match. The reason is recorded at the constant itself:
40 is a 4-byte header plus 36 payload bytes, and it is the first format in this
project that may carry a payload other than eight bytes. Nothing ANT-shaped got
longer; both 1 M formats are still static at 10 and 12 body bytes.

*(The plan glossed 40 as "32 payload + header", which implies an 8-byte header.
The header is 4. Taking the plan's explicit 40 as authoritative yields 36
payload bytes rather than 32. This is recorded rather than quietly reconciled,
because the buffer ceiling is not what bounds a node anyway — see below.)*

The buffer is not the limit. **At S=8 every extra byte is 64 µs and they
compound.** The limit is a duty rule:

> **A node's frame must stay under 25 % of its channel period at its announced
> rate.**

Self-enforcing, and no second constant: both halves are already on the wire —
the period is in descriptor frame 0, the rate is in the schedule block — so a
receiver can check the claim as cheaply as the node can make it. It is enforced
by **encoder refusal**, following the dwell-versus-clock-drift rule RF-5a
landed in `profile_sched_check()`: a rule enforced only where somebody
remembered to look is a battery complaint months later with no reproducer.

It binds only where **both** factors are present, which is the correct shape:

- a 0.5 Hz asset tag with a 40-byte frame is at 0.16 % — never sees it;
- an 8-byte frame at S=8 is 1.3 ms against any real period — never sees it;
- what it refuses is a fast node with a long frame, which is reached by
  accident rather than by decision.

Concretely, the longest frame (3088 µs) needs a period of at least 12.35 ms, so
the bound engages only above ~81 Hz. `test_lr.c` pins the boundary one count
either side.

### The descriptor-set collapse, with the arithmetic corrected

A descriptor set is one 8-byte frame per header, per schedule block and per
field, and **every one of those frames is a separate transmission** — a
separate HFXO start and a separate ramp-up, which on a coin cell dominate the
airtime. N frames cost N wakes; one frame costs one. It also cuts mid-stream
join from N periods to one.

**Nothing about the 8-byte frames changes, and that is the design.** Each
descriptor frame already carries its own page byte and its own
`(index << 4) | (count - 1)` byte, so a concatenation of them is
self-describing: a receiver splits a long payload into 8-byte chunks and feeds
each to `profile_desc_rx_feed()` — the same accumulator, the same ordering
rules, the same schema-id invalidation that a 1 M receiver has always used. The
codec is untouched and there is no second encoder to keep in step. A node's
choice to collapse is invisible above the split.

**How complete the collapse actually is.** Four whole frames fit in a 36-byte
payload:

| node | frames | wakes |
|---|---|---|
| asset tag, no fields, no schedule block | 2 | **1** |
| 2 fields | 4 | **1** |
| 2 fields + schedule block | 5 | 2 |
| 8 fields + schedule block | 11 | 3 |
| 14 fields + schedule block | 17 | refused, as always |

The plan said "a sparse node with eight fields currently wakes ten times per
heartbeat; one frame is one wake". **That is right about the mechanism and
optimistic about the count: ten becomes three, not one.** Ten-into-one would
need a ~76-byte body, which at S=8 is ~5.4 ms of airtime and fails the 25 %
duty bound at any period under 22 ms. The honest claim is a ~70 % cut in wakes
for the eight-field case and a **complete** collapse for the sparse asset tag —
which is the node the envelope document was written for and the node that
needed it most.

---

## Does 1 M also qualify? — **MEASURED 2026-08-11: yes, on the interop question**

The authorship argument above applies equally to a RadiANT-authored **1 M**
format with a real `PCNF0.LFLEN=8` on a device type no stock receiver opens. If
a stock dongle in wildcard search simply drops such a frame on CRC and keeps
every other sensor, the descriptor collapse — the largest battery item here —
arrives without coded PHY at all, and most of this phase becomes optional.

The plan required this be settled **first**, with one Tier 4 capture.

**The capture has now been performed.** The Feather was returned to the ANT
dongle firmware (`dist/ant_dongle.uf2`, sdk-ant, enumerating `0FCF:1009` and
answering `MESG_VERSION` with `BOK02.01.00`) and used as the stock receiver.

### The rig, and the control that makes it mean anything

`radiant_core/spike/x1m_len/` on the nRF54L15 DK transmits **two** frames,
alternating, on the same radio at the same power, each at 4 Hz — both device
type `0x60`, both 1 M GFSK, both ordinary five-byte tracking geometry, both
`RADIANT_LEN_FIXED`, differing **only in body length**:

| | device | body | on-air frame |
|---|---|---|---|
| **A — control** | `0x60A0` | 10 bytes | the ordinary 17-byte ANT frame |
| **B — experiment** | `0x60B0` | **30 bytes** | 37 bytes |

The control is the whole reason this run can be believed. A capture that only
transmitted B and saw nothing would be indistinguishable from a dead
transmitter, a wrong network address, a wrong frequency or an unplugged dongle
— every one of which manufactures the desired answer. A and B differ in one
byte count and nothing else, so A being heard while B is not isolates *length*
as the only variable.

Note that **no shipping code was changed to do this**, and that is a fact about
the design rather than a convenience: `apply_format()` requires
`RADIANT_LEN_FIXED` on 1 M and then accepts any `body_len` up to
`RADIANT_RADIO_BODY_MAX`, which this ADR raised to 40 for the coded format's
sake. A 30-byte static-length 1 M body is therefore already expressible through
the public HAL. The receiver was `tools/ant_rf6_capture.py`, which assigns a
wildcard channel with the extended-assign background-scan bit (`0x01`) — the
mechanism Zwift actually uses — so it never locks onto one device and keeps
reporting everything it hears.

### The result

120 s, one sitting, with a real trainer on the air throughout:

```
  device    type  broadcasts   first    last   rssi(min/mean/max)
  #24736    0x60          47    1.1s  117.3s   -33/-33/-32     <- A, control
  #52233    0x0B          72    2.1s  118.6s   -73/-71/-70     <- real trainer
  #52233    0x11          39    1.5s  120.5s   -71/-71/-70
  #52233    0x7B          36    1.7s  114.5s   -71/-70/-70
```

- **A was heard 47 times at −33 dBm**, spread evenly across the whole window.
  The link was not marginal — it was some 40 dB above where ANT stops tracking,
  and stronger than the real trainer the same dongle was holding.
- **B was transmitted about 480 times and heard exactly zero times.** The
  transmitter's own console reported `B/long sent=…  fail=0 refuse=0` climbing
  at 4 Hz throughout, so the frames were on the air; they did not reach the
  host. Against an expectation of ~47 sightings had it been receivable, zero is
  not a marginal result.
- **The trainer kept being reported on all three of its device types for the
  whole window**, first to last. Nothing else on the air was disturbed.

**So a stock ANT receiver drops a length-extended 1 M frame before the host and
is otherwise unaffected by it.** That is exactly the prediction, and it is now
a measurement.

### What this permits, and what it does not

It permits a second, length-extended 1 M format as an **addition** to this
record and not a revision of it. Everything built in this phase stands; the
coded PHY is still worth its ~8 dB on its own merits.

**It does not license writing that format yet, and the reason is recorded here
so it is not lost.** The justification for length extension on the coded PHY is
*physical invisibility* — a stock receiver cannot demodulate LE Coded at all.
The justification on 1 M would be *device-type isolation*, which is strictly
weaker: it depends on no stock receiver ever opening a channel on the device
type in question, which is a claim about other people's software rather than
about physics. **That is a different argument and it owes its own decision
record**, with at minimum: the choice of device type and why it is safe, what
happens to a receiver that does open it, and whether the backend's 1 M
`RADIANT_LEN_FIXED` requirement should be relaxed at all or whether a second
fixed length is enough. The guard in `apply_format()` deliberately pairs
`RADIANT_LEN_FROM_BODY` with the coded PHY, and this result is **not** grounds
to loosen that pairing casually — it is grounds to write the record that would
justify doing so deliberately.

### Two limitations, stated rather than buried

**1. This run proves HARMLESSNESS, not USABILITY, and the difference matters.**
Nothing in it demonstrates that frame B is a *well-formed* 30-byte frame — only
that the transmitter's HAL accepted it (`refuse=0`) and that a stock receiver
did not report it. A frame that was malformed for some reason unrelated to its
length would produce a byte-for-byte identical result. The experiment was
designed to answer "does a long frame damage a stock receiver", and it answers
that; it does not answer "can a RadiANT receiver read a long 1 M frame", which
has never been tested at all.

Closing that gap is one run: put a `radiant_core` receiver on the same air and
show it decoding B while the stock dongle continues not to. **The ADR owed for a
length-extended 1 M format must not be written until that has been done** —
otherwise the format would rest on a capture that is equally consistent with the
frame being broken.

**2. The only independent sensor on the air was the trainer**, since heart-rate
straps sleep when not worn. "Keeps every other sensor" therefore rests on one
real device across three device types, plus the A control. A repeat with a
second live sensor would strengthen that half. It would not change the B result,
which is what the question turned on.

**Status: settled for the interop question, 2026-08-11. A length-extended 1 M
format remains unwritten and owes its own ADR.**

---

## The compatibility boundary

**ANT+ compatibility channels are permanently excluded from this PHY and from
the length extension.** Not deferred — excluded, by the same byte-exactness
constraint that forbids touching a page layout. A compat channel is 1 M on
RF 57 for its whole life. No capture, no bench result and no future ADR changes
that without also abandoning the compatibility claim.

**This is the same boundary the compatibility plan's Layer C crosses, and the
two records should be read together.** A sensor that switches to private mode
*becomes* a RadiANT-native `0x60` node and is therefore eligible for this PHY,
for the length extension, for frequency agility and for response slots. That
reframes private mode from a sacrifice ("Zwift sees nothing") into the reason
this RF work has an audience at all: **private mode is the doorway to
everything in this record.** Nothing in this phase touches
`profile_private.c`, `profile_hr.c`, `profile_power.c` or `profile_compat.c`,
and the exclusion is structural rather than remembered — those files reach the
coded PHY through no path.

**A private-mode switch lands on 1 M / RF 57**, and any PHY move happens
afterwards through the ordinary descriptor mechanism. One change at a time: a
switch that arrived private, coded *and* off-57 would force a keyholder that
missed the announcement to search PHYs and frequencies as well as device
numbers, converting a solved re-acquisition problem into an unsolved one.

### Discovery never moves

A searching receiver stays on **1 M, RF 57, the 3-byte search format**, and
finds every node there. An LR node is found by its 1 M descriptor or by the
sync handoff (RF-5b) — never by sweeping the coded PHY.

The reason is the sweep's guarantee, not tidiness. The sweep covers all 256
values of `devnum_lo` and is therefore *certain* to find any transmitting device
within one sweep — a property seven tests in `test_search.c` defend by name. A
second PHY would not add windows to that sweep, it would **multiply** it: every
set listened to twice, the sweep twice as long, and "certain within one sweep"
quietly becoming "certain within two". Adding a frequency axis later would
multiply it again. This is why the extension axes are announced in a descriptor
a searching receiver already hears, and why the receiver moves deliberately.

---

## `t_sync`, per PHY — **bounded, not measured**

The HAL's `t_sync` contract requires the calibration constant to be re-measured
"for every new part and every PHY". This phase added a PHY.

It is a genuinely different number, not a rounding of the existing one:

| | 1 M | S=8 |
|---|---|---|
| RX chain delay | 9.4 µs | **29.6 µs** |
| TX chain delay | 0.6 µs | 0.6 µs |

Same provenance as the existing 1 M seed, and on the same terms: Nordic's
per-SoC tables in the Zephyr Bluetooth controller, whose own header calls them
"based on empirical measurements and sniffer logs". The S=8 figures are
**byte-identical across nRF52840, nRF5340 and nRF54LX**, exactly as the 1 M pair
is, so one constant covers both series here for the same reason the other one
does. The transmit side is the same on both PHYs — modulator and PA delay, which
the coding does not touch — so there is deliberately no second TX constant.

**What reusing the 1 M constant would have done:** put every coded receive
window 20 µs off centre. A *constant* offset cancels out of the period estimate,
so the drift estimator still locks, no CRC fails, and nothing is logged — yield
simply falls by a fraction of a percent, which on this bench is
indistinguishable from the characterised ~0.4 % collision floor. That is the
silent failure the `t_sync` contract has a page of prose about, and it is why
this constant was worth finding before the bench session rather than during it.

**Both numbers remain seeds.** Neither the 1 M value nor the S=8 value has been
through the wired two-board trigger the HAL specifies, and the caveat applies
with *more* force to the coded one: these are BLE figures for BLE's own filter
bandwidth, and ANT-1M is ~170 kHz deviation against BLE's 250 kHz.

- **1 M: −9 µs RX / +1 µs TX — bounded, dated 2026-08-10, seeded from Zephyr.**
- **S=8: −30 µs RX / +1 µs TX — bounded, dated 2026-08-11, seeded from Zephyr.**
- **Neither is measured. Both are owed the wired two-board trigger in the same
  sitting.**

The plan's fallback — bound with the `ant_sens.py` ladder — also needs the
bench (two radios and the sensitivity rig), so it is deferred with the rest. No
number here is invented; each is a cited prior with its provenance and its
status attached.

---

## What `phy_switch_us` now means

`caps.phy_switch_us` was written with the HAL, has been zero on every backend
since, and was **read by nothing** until this phase — which is exactly the shape
of field that turns out to be wrong the first time it is used. So its meaning is
pinned here:

**It is a scheduling cost, not an airtime one, and it is spent only on a
CHANGE.** `radiant_sched.c` adds it as extra arm lead on the first operation
after the PHY changes, and never on an operation that stays on the PHY the radio
is already configured for. Folding it into `min_arm_lead_us` would charge every
window on a two-PHY build for a switch that happens twice a period at most.

**The coded PHY's longer air lead is NOT this field.** FEC block 1 puts `t_sync`
336 µs after the start of transmission, against 48 µs at 1 M, and that is owed on
*every* coded operation including one following another coded operation. It
therefore lives in `min_arm_lead_us`, which now advertises the worse of the two
PHYs — the same worst-case doctrine that file already applies to address length,
and for the same reason: a backend that needs more than it advertised gets every
arm refused `ETIME`, and a refused arm wedges the board.

On the nRF backend a mode change is a handful of register writes that
`apply_format()` already performs on every arm, and the RADIO is always
`DISABLED` between operations. The honest incremental cost is near zero; the
value is set small and non-zero (20 µs, bounded not measured) because zero would
make the scheduler's PHY budgeting dead code on the only backend that ships.

---

## Consequences

**Gained**

- ~8 dB of sensitivity for opted-in device types — the largest single lever
  available in software.
- The descriptor-set collapse: complete for a sparse asset tag, ~70 % fewer
  wakes for an eight-field node.
- A 64-bit inline command tag becomes possible, which is what the reliable
  actuator path needs.
- `RADIANT_LEN_FROM_BODY` and `caps.phy_switch_us` stop being untested
  reservations and acquire real implementations and real assertions.

**Paid**

- A second frame format, a second PHY configuration, and a scheduler that has to
  reason about which PHY the radio is currently on.
- `min_arm_lead_us` grows by ~328 µs on a build with the coded PHY, charged to
  1 M windows that do not need it. This is scheduling slack — it moves *when* an
  arm happens, not when a window opens or how long it stays open — and against a
  249.7 ms period it is 0.13 %.
- `RADIANT_RADIO_BODY_MAX` grows from 32 to 40: 8 bytes in each of a backend's
  two DMA buffers.
- An LR channel can never join the merged 1 M window. This is the accepted price
  of ADR 0005 axis 4, scoped to opted-in types.
- Two `t_sync` calibrations to maintain instead of one, both currently seeds.

**Refused**

- S=2 (defined, not built — see above).
- Any change to ANT+ compatibility channels.
- Any move of discovery off 1 M / RF 57.
- Length extension anywhere the PHY does not already hide us.

---

## Verification status

**Deterministic, and passing** (`radiant_core/tests/src/test_lr.c`, on the
nRF5340 DK via `scripts/run_ztest_hw.ps1`):

- the format's geometry and the FEC-block airtime arithmetic;
- the length field in both directions, including the refusals that stop a
  corrupted length becoming a read past a DMA buffer;
- that the ANT encoders refuse the LR configuration rather than approximating
  it;
- that the search sweep is still 1 M / RF 57 / 8 filters / 32 sets with both
  PHYs advertised;
- that the scheduler charges `phy_switch_us` on a change and not on a repeat;
- the duty bound, asserted one count either side of the boundary;
- the descriptor collapse round-tripping through the unmodified 1 M
  accumulator.

**Deferred to a hardware session, and NOT claimed here:**

| Owed | Why deferred |
|---|---|
| **The gate: ≥ 6 dB improvement in the 5 %-loss point for S=8 vs 1 M** (`tools/ant_sens.py`) | Needs the sensitivity rig and a transmitter-power ladder |
| **A length-extended frame showing `loss_exact` no worse than an eight-byte frame at the same power** | Same rig |
| ~~**The 1 M length-extension Tier 4 capture**~~ | **DONE 2026-08-11** — see *Does 1 M also qualify?* above |
| **`t_sync` measured (both PHYs) via the wired two-board trigger** | Needs two boards and a GPIO trigger rig |
| **`phy_switch_us` measured on nRF** | Same session |

A green test suite is not a met gate, and this record says so in both places
rather than letting the two be confused.

### An open shipping-code defect that will bias the gate against S=8

**`apply_format()` in `radiant_radio_nrf.c` does not apply the nRF52840's
errata 191 workaround**, on either PHY path. Found 2026-08-11 while building the
ladder instrument (`radiant_core/spike/phy_ladder/`), by comparison with
Nordic's own open-source Zephyr Bluetooth controller, which writes a documented
register at `0x40001740` on every mode change into and out of the coded PHY
(`radio_nrf52840.h`, `hal_radio_phy_mode_get()`). Public erratum, public
workaround, Apache-2.0 source already a dependency of this project — no
clean-room concern.

**Why it matters here specifically.** The nRF5340 DK cannot run this backend at
all (its RADIO is on the network core, and `CONFIG_RADIANT_CORE_BACKEND_NRF`
depends on `SOC_COMPATIBLE_NRF52X || SOC_COMPATIBLE_NRF54LX`, neither of which
an nRF5340 sets — it falls back to the **null** backend silently). So the only
two radiant-capable radios on this bench are the nRF54L15 DK and the **nRF52840**
Feather, and every S=8 link therefore has an nRF52840 at one end. The erratum
degrades coded-PHY reception on exactly that part.

**The consequence for the gate, stated before the measurement rather than
after:** any S=8 improvement measured on this bench is a **lower bound**. If the
result lands near but below the ≥ 6 dB gate, this is the first suspect and the
run must not be recorded as a failure of the coded PHY until the workaround is
applied and the run repeated. The spike deliberately does **not** poke the
register itself — an instrument that quietly fixes the code under test would
report a sensitivity the shipping backend does not deliver.

Owed as a shipping-code decision in its own right, independent of this ADR:
apply the workaround in `apply_format()`, or record why not.

**One thing that will fail loudly rather than silently, which is why it was safe
to land ahead of the bench:** the CRC configuration. `t_sync` being wrong costs
tenths of a percent of yield and hides in the noise floor. A wrong CRC
configuration on the coded PHY produces *zero* valid frames. The LR format
therefore keeps CRC-16/CCITT-FALSE with `cover_addr` — one software CRC across
every format this project defines, and `radiant_crc_repair.c` keeps working on
it. BLE's own coded PHY uses a 24-bit CRC and that is unarguably better over a
40-byte body; it is not adopted because it would buy a second software CRC, a
second repair path, or the loss of single-bit repair here, for a
residual-error improvement that the FEC has already made the smaller term. If
long frames become the common case rather than the descriptor-collapse case,
CRC-24 is the obvious next change, and this paragraph is the record of why it
was not the first one.
