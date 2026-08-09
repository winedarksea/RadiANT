# Documentation index

Checked by: `.github/workflows/linkcheck.yml` for the external URLs, and the
README line-cap step in `.github/workflows/build.yml` for rule 4 below. Both
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
| [`backends.md`](backends.md) | The `sdk_ant` / `core` / `stub` radio seam, the two `ant_core` radio backends, the USB and UART transports, the per-board build targets, and the optional ANT features each backend can do |
| [`sdk-ant-contract.md`](sdk-ant-contract.md) | The ~50 functions the bridge calls. This is the specification `ant_core` must satisfy — not a description of sdk-ant, a statement of what has to exist to replace it |
| [`ant-serial-protocol.md`](ant-serial-protocol.md) | The host↔dongle serial protocol: framing, the SYNC-in-checksum rule with a worked XOR, the generated message tables, and the capabilities reply `080800b23200fd8d0f` decoded bit by bit |
| [`ant-radio-link.md`](ant-radio-link.md) | The clean-room on-air reference, and **the only permitted link-layer source for `ant_core`** — a core agent reads this instead of anything of Garmin's. Every fact carries an inline provenance tag — `[rev5.1 §x]`, `[rtl_433]`, `[measured]`, `[inferred]` — so what is known can be told apart from what is assumed |
| [`ant-plus-profiles.md`](ant-plus-profiles.md) | ANT+ device profiles as an interoperability target: page layouts, accumulators, common pages, and the rates each profile transmits at |
| [`profile-registry.md`](profile-registry.md) | The public registry of device types and pages claimed — ours or a third party's by PR — with the allocation process and the collision risk stated honestly |
| [`radiant-telemetry.md`](radiant-telemetry.md) | The generic telemetry envelope: field kinds, the descriptor page, the MQTT mapping, and the accumulate-by-default rule |
| [`radiant-security.md`](radiant-security.md) | Threat model, the three independent switches, key establishment, and the honest limits — category fingerprinting, group-key forgery, the spread-MAC injection window |
| [`preservation.md`](preservation.md) | What `archive/` holds, why each item is or is not redistributable, and the 10 MB budget |
| [`third-party.md`](third-party.md) | Everything this project depends on that it does not own, and the licence position of each |
| [`decisions/`](decisions/) | Architecture decision records — see below |

### Decisions

| ADR | Records |
|---|---|
| [`0001`](decisions/0001-backend-selection-and-release-default.md) | When `ant_core` becomes the release default, and the Tier 3 evidence required first. Release artifacts stay on the sdk-ant build until this ADR says otherwise — the switchover is a decision, not a drift |
| [`0002`](decisions/0002-clean-room-policy.md) | The clean-room policy: permitted sources, forbidden sources, and why the posture is graded rather than absolute. **This must exist before the first line of `ant_core`** — one person disassembling `libant.a` once contaminates the rebuild retroactively, and there is no way to prove afterwards that it did not happen |
| [`0003`](decisions/0003-naming-trademark-and-usb-identity.md) | The RadiANT name and its mandatory qualifier, the trademark posture, why `0FCF:1009` and the descriptor strings stay, and the driver-free host-access experiment |
| [`0004`](decisions/0004-license-apache-2-0.md) | Apache-2.0 over MIT/BSD, for §3's express patent grant with a retaliation clause, plus `NOTICE` |
| [`0005`](decisions/0005-extension-inside-ant-plus.md) | Why the extensions live inside network `A6 C5` rather than on a separate network |

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

**The prose header comments in [`synth.conf`](../synth.conf),
[`stub.conf`](../stub.conf), [`prj.conf`](../prj.conf),
[`Kconfig`](../Kconfig) and the tops of `src/*.c` stay exactly where they
are.** They are read at the moment somebody edits the file, which is the moment
the reasoning is needed; moved into `docs/` they rot within two changes and are
read by nobody. Documents here link *to* them. This is the one place where
duplication-by-omission beats tidiness, and rule 1 does not override it.
