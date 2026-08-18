# Documentation index

Checked by: `.github/workflows/linkcheck.yml` for the external URLs, and the
README line-cap step in `.github/workflows/ci.yml` for rule 4 below. Both
land in Wave 0; until they do, this index is narrative and a moved file will
break a link silently.

`README.md` is the product: how to get a dongle working, and what to do when it
does not. This directory is everything else — the reasoning, the measurements,
the protocol references and the decisions. The split is deliberate and rule 1
below is what keeps it.

---

## The documents

| Document | What it is for |
|---|---|
| [`gotchas.md`](gotchas.md) | The non-obvious constraints the code is shaped around. Each entry cost real time to find, and several look like a generic "USB doesn't work" failure from the outside. Read this before debugging anything |
| [`testing.md`](testing.md) | Every tool, the CI host-tests job, the four verification tiers and their gates, and — the part that is not opinion — how to read a bench result without measuring the instrument |
| [`backends.md`](backends.md) | The `sdk_ant` / `core` / `stub` radio seam, the two `radiant` radio backends, the USB and UART transports, the per-board build targets, and the optional ANT features each backend can do |
| [`sdk-ant-contract.md`](sdk-ant-contract.md) | The ~50 functions the bridge calls. This is the specification `radiant` must satisfy — not a description of sdk-ant, a statement of what has to exist to replace it |
| [`ant-serial-protocol.md`](ant-serial-protocol.md) | The host↔dongle serial protocol: framing, the SYNC-in-checksum rule with a worked XOR, the generated message tables, and the capabilities reply `080800b23200fd8d0f` decoded bit by bit |
| [`ant-radio-link.md`](ant-radio-link.md) | The clean-room on-air reference, and **the only permitted link-layer source for `radiant`** — a core agent reads this instead of anything of Garmin's. Every fact carries an inline provenance tag — `[rev5.1 §x]`, `[rtl_433]`, `[measured]`, `[inferred]` — so what is known can be told apart from what is assumed |
| [`spike-a-results.md`](spike-a-results.md) | What Spike A measured on the bench: a bare nRF RADIO, configured from `ant-radio-link.md` alone, receiving real ANT+ broadcasts with no software in the bit path. The primary record — every `[measured]` tag in `ant-radio-link.md` cites it, and the raw logs it was read from are in [`../archive/captures/radio/`](../archive/captures/radio/). Measured on nRF54L15; the nRF52840 confirmation has not happened |
| [`spike-b-results.md`](spike-b-results.md) | Spike B part 1, the promiscuous capture: what the fourth byte of an ANT frame actually is. The finding that byte 3 is a **control byte and not a length** starts here, and so does the withdrawal of part 1's own three-bit-sequence reading |
| [`spike-b-part2-results.md`](spike-b-part2-results.md) | Spike B part 2, the acknowledged-exchange capture: the four measured data/acknowledgement pairs, the one-bit sequence, the 1.55 ms turnaround, and every microsecond figure `radiant_transfer.h` is built on. The primary record behind the transfer engine |
| [`device-profiles.md`](device-profiles.md) | **Every device profile in one place.** ANT+ profiles as an interoperability target (page layouts, accumulators, common pages, transmission rates), the RadiANT profile device types, and the published `0x60` schema recipes. Absorbed the former `ant-plus-profiles.md` and `new_device_profiles.md` |
| [`profile-registry.md`](profile-registry.md) | The public registry of device types and pages claimed — ours or a third party's by PR — with the allocation process and the collision risk stated honestly |
| [`radiant-telemetry.md`](radiant-telemetry.md) | The generic telemetry envelope: field kinds, the descriptor page, the MQTT mapping, and the accumulate-by-default rule |
| [`radiant-bridge.md`](radiant-bridge.md) | What a receiver does after decoding: the sample bus, the sink registry, the binding table, the Thread/Matter/MQTT/BLE planes, and the 802.15.4 coexistence gate that decides whether a single-chip bridge exists at all |
| [`bench-runbook.md`](bench-runbook.md) | The three sittings that need a person, with the images already built and the traps already found. Read the fitness check first: the bench cannot currently repeat inside the gate, and a bench that cannot repeat produces numbers that look like results |
| [`matter-flash-spike.md`](matter-flash-spike.md) | Whether Matter and RadiANT fit on one nRF54L15, measured before any more Matter code is written. Flash was the feared wall and is not one — 676 KB spare; RAM goes to ~76 % and is the number to watch. What the spike does **not** answer, including the three-client interlock during commissioning |
| [`hrm-reference-design.md`](hrm-reference-design.md) | `apps/hrm_ble`, the heart-rate node, as the design a manufacturer copies: the `hrm_sensor.h` seam and why it is push rather than poll, the port-to-your-board checklist, the nine-item production checklist and the provisioning gap that makes two of them unachievable today, the drift bug that escaped 681 ztests and the three checks that now cover it, and why the BLE intervals are an arbitration policy rather than a power setting |
| [`treadmill-reference-design.md`](treadmill-reference-design.md) | `apps/treadmill`, the fitness-equipment node: two ANT+ masters (FE-C `0x11` and SDM `0x7C`) and two BLE services (FTMS and RSC) on one radio, the `treadmill_source.h` seam and the one thing it has that a chest strap does not — it is **told** things, over two protocols, and they have to agree. The five-unit conversion table and the factor-of-two cadence trap, the seven-item production checklist with the two items nothing in this tree can satisfy, the bench procedure, and why a gym's occupancy roll-up needs nothing new on the treadmill at all |
| [`radiant-security.md`](radiant-security.md) | Threat model, the two payload switches (`X_AUTH`, `X_CONF`), the identity tiers that replace `X_PRIV`, key establishment, and the honest limits — no unlinkability, group-key forgery, the spread-MAC injection window and its forgery bound |
| [`preservation.md`](preservation.md) | What `archive/` holds, why each item is or is not redistributable, and the 10 MB budget |
| [`third-party.md`](third-party.md) | Everything this project depends on that it does not own, and the licence position of each |
| [`decisions/`](decisions/) | Architecture decision records — see below |

### Decisions

| ADR | Records |
|---|---|
| [`0001`](decisions/0001-backend-selection-and-release-default.md) | When `radiant` becomes the release default, and the Tier 3 evidence required first. Release artifacts stay on the sdk-ant build until this ADR says otherwise — the switchover is a decision, not a drift |
| [`0002`](decisions/0002-clean-room-policy.md) | The clean-room policy: permitted sources, forbidden sources, and why the posture is graded rather than absolute. **This must exist before the first line of `radiant`** — one person disassembling `libant.a` once contaminates the rebuild retroactively, and there is no way to prove afterwards that it did not happen |
| [`0003`](decisions/0003-naming-trademark-and-usb-identity.md) | The RadiANT name and its mandatory qualifier, the trademark posture, why `0FCF:1009` and the descriptor strings stay, and the driver-free host-access experiment |
| [`0004`](decisions/0004-license-apache-2-0.md) | Apache-2.0 over MIT/BSD, for §3's express patent grant with a retaliation clause, plus `NOTICE` |
| [`0005`](decisions/0005-extension-inside-ant-plus.md) | Why the extensions live inside network `A6 C5` rather than on a separate network |
| [`0006`](decisions/0006-security-v1-scope-and-x-priv-withdrawal.md) | Security v1 is `X_AUTH` + `X_CONF`; `X_PRIV` is rejected rather than deferred, with the reasons recorded so it is not re-proposed, and the identity tiers that replace it |
| [`0007`](decisions/0007-long-range-phy.md) | The long-range PHY is Bluetooth LE Coded at S=8, and the length extension it unlocks, permitted only where the PHY already makes us invisible. **Amended 2026-08-15:** the arm-lead cost it accepted was assessed as single-channel slack and is a multi-channel exclusion radius, so the PHY is now behind `CONFIG_RADIANT_PHY_LR_CODED`, `default n` |
| [`0008`](decisions/0008-antplus-additive-pages-and-compat-security.md) | Additive pages are permitted on ANT+ device types, `0x79` is excluded permanently, and the compat security layer is three layers with a **two-tier** attestation — identity Tier I on by default at 1.2% of slots, data Tier II off |
| [`0009`](decisions/0009-hostless-node-identity.md) | The hostless node: one monotonic NVM counter is the epoch and the pairing-scalar index, `K_dev` is provisioned at manufacture, and the counter advances **before** the pairing pubkey goes on the air |
| [`0010`](decisions/0010-bridge-two-planes.md) | The bridge is two planes over one Thread network: MQTT carries the values (Matter has no heart-rate cluster), Matter carries the derived semantics and the actuator control — and Matter is implemented even in an MQTT-only build, because its commissioning is how the device joins Thread |
| [`0011`](decisions/0011-never-a-border-router.md) | The bridge rebroadcasts but never routes. Duty cycle was the wrong metric — ~4 % ANT duty clips ~18 % of full-size 802.15.4 frames — and the rule that decides a role is **blast radius**, not radio time. A 2-in-1 border router needs a second radio, which is a BOM decision |
| [`0012`](decisions/0012-adaptive-frequency.md) | Leaving 2457 MHz, slowly and out loud — and the bench gate that **refuted its own stated motivation**: the loss floor is not the frequency. The mechanism stands; the Context section does not |
| [`0013`](decisions/0013-sweep-is-the-elastic-consumer.md) | Under an arbiter the sweep gives way and tracked slots are inviolate — the product priority is delivered by shaping demand, because MPSL will not let us outrank the other stack |
| [`0014`](decisions/0014-second-vendor-port-what-it-cost.md) | What a second vendor's silicon cost: the HAL seam held, and the three things it did not anticipate — one new capability field, PHY parameters that cannot be inherited, and a measurement discipline learned the hard way |
| [`0015`](decisions/0015-cc26xx-coexistence-design.md) | CC26xx coexistence is two Kconfig symbols and an RF-scheduler policy, not a portable gate — there is no timeslot API on this part to sit behind |
| [`0016`](decisions/0016-merge-reach-is-the-arm-lead.md) | `min_arm_lead_us` is the scheduler's **exclusion radius**, not slack, so the RX-window merge reach is derived from it. Closes the band in which two tracked channels could neither merge nor be armed in sequence — invisible at one channel, ~1 % per channel at eight |
| [`0017`](decisions/0017-fec-treadmill-control.md) | FE-C page 51 "Track Resistance" **is** the treadmill incline command — the spec is written treadmill-first and the trainer reading is the fallback — so no RadiANT-private incline page is invented. Also: a capability bit and its command page move together or a controller reports the machine as broken; the BLE name goes in the scan response and what that costs; and an amendment to ADR 0003 on shipping no trademarked default device name |

---

## Four rules that keep this from rotting

A docs tree decays into a graveyard by default. These four are what stop it,
and three of the four are machine-enforced precisely because the fourth kind of
discipline is what produced a 1200-line README.

**1. No duplication. `README.md` links; `docs/` owns.** If a fact is in both
places, one of them is already wrong and nobody can tell which. There is
exactly one deliberate exception: **the frame checksum covers the `0xA4` SYNC
byte** is stated in both [`gotchas.md`](gotchas.md) and
[`ant-serial-protocol.md`](ant-serial-protocol.md), because its absence cost a
week and because two independent implementations agreeing with each other and
with nothing else in the world is the failure it produced. That exception is
argued, not assumed — do not add a second one without the same argument.

**2. Every document opens with a `Checked by:` line** naming the test,
generator or script that fails if the document drifts. If nothing checks it,
the line reads literally:

```
Checked by: nothing — treat as narrative.
```

That is not an apology and it is not a placeholder. It is the whole point:
unverifiable prose becomes *visibly* unverifiable, so a reader knows whether
they are looking at something CI would have caught or at somebody's memory of a
bench session. [`gotchas.md`](gotchas.md) carries exactly that line and is
still the most valuable file in the tree — narrative is not a lesser status,
it is a labelled one.

**3. Generated regions are delimited, and CI diffs them.** Anything produced by
`scripts/gen_ant_wire.py` from `protocol/ant_wire.yaml` sits between explicit
markers. The generator's `--check` mode regenerates and diffs; CI fails on
drift. Hand-editing inside the markers is a build failure, not a review
comment.

**4. The README line cap is enforced in CI** (`< 450` lines). Crude, and that
is the merit of it: it sends the next 200-line addition to `docs/` by
construction rather than by discipline.

## One thing that does not move into `docs/`

**The prose header comments in [`synth.conf`](../apps/dongle/synth.conf),
[`stub.conf`](../apps/dongle/stub.conf), [`prj.conf`](../apps/dongle/prj.conf),
[`Kconfig`](../apps/dongle/Kconfig) and the tops of `apps/common/*.c` and
`radiant/src/*.c` stay exactly where they are.** They are read at the moment somebody edits the file, which is the moment
the reasoning is needed; moved into `docs/` they rot within two changes and are
read by nobody. Documents here link *to* them. This is the one place where
duplication-by-omission beats tidiness, and rule 1 does not override it.
