# 0001 — Backend selection, and what the release artifacts are built from

Checked by: `scripts/build_all.ps1` — its backend axis builds every backend
this ADR names, and its assertion that `CONFIG_ANT_DONGLE_RADIO_<X>=y` appears
in `.config` is what stops a build silently selecting a different one. The
release-default clause is checked by nothing: it is a human decision recorded
here so that changing it requires editing this file. It was changed on
2026-08-20, by editing this file.

- **Status:** Accepted. The release-default clause is **resolved** — see
  [Switchover](#switchover-2026-08-20) below.
- **Date:** 2026-08-08, amended 2026-08-20
- **Supersedes:** nothing
- **Related:** [0002 clean-room policy](0002-clean-room-policy.md),
  [0005 extension inside ANT+](0005-extension-inside-ant-plus.md)

## Switchover (2026-08-20)

**The Tier 3 acceptance passed, and release artifacts now come from
`-DANT_RADIO=core`.** This section is the recorded edit the clause below
demands; nothing about the switchover happened by changing a default in a
workflow file.

What was run: Zwift paired against a `radiant` build and held a 30-minute ride
with resistance changes taking effect throughout — the condition stated
verbatim below. Further bench testing is planned and this section will be
extended rather than rewritten; the acceptance is not being treated as the end
of measurement, only as the end of *this* clause.

What changed in the tree, all of it consequences of that one decision:

- `build-core` in `.github/workflows/release.yml` packages, sums and attaches
  the four shipping images. The `build-sdk-ant` job is **deleted** — not
  demoted, not left publishing nothing. No job in this repository needs a
  secret any more, so the release lane runs to completion on a fork.
- `scripts/build_all.ps1` defaults to `-Backend core`, and only `core` writes
  `dist\`. It defaults to NCS v3.4.0; `-Backend sdk_ant` needs
  `-NcsVersion v3.2.4` passed explicitly.
- **`ant_select_radio()` no longer picks a backend from what is on disk.** It
  used to default to `sdk_ant` whenever a checkout resolved, which made the
  backend a property of the *machine* rather than of the build: on any bench
  able to build sdk-ant at all, a bare `west build` produced a proprietary
  image and the only evidence was one `STATUS` line. The two images enumerate
  identically. `sdk_ant` now requires someone to type `-DANT_RADIO=sdk_ant`.
- The Kconfig choice defaults in `apps/dongle` and `apps/dongle_thread` follow,
  so a menuconfig-only reader sees the shipping configuration.

**What did NOT change, and must not be read as deprecation:** sdk-ant remains a
fully selectable backend. `apps/common/ant_radio_sdk_ant.c`, `apps/sim`,
`west.yml`'s opt-in `ant` group, `tools/ab_gates.toml` and the reference
captures in `archive/` all stay exactly as they are. The reasons below survive
intact — see the amendment under them.

**One accepted cost, stated rather than discovered:** `apps/sim`, the reference
transmitter, cannot build without sdk-ant at all, and the deleted job was the
only thing that ever compiled it. It is now built by hand before a bench
sitting. Nothing warns you if it has stopped compiling.

## Context

Today the radio is Nordic's `libant.a`, a prebuilt binary from the private,
non-redistributable `ant-nrfconnect/sdk-ant` repository pinned at `v2.1.0`. Losing
access to that repository means **nothing builds** — not even the radio-stub
configuration, because the root `CMakeLists.txt` hard-fails when sdk-ant's
headers are absent and `src/ant_serial_bridge.c` takes every protocol constant
from sdk-ant's `ant_parameters.h`. Garmin closed the ANT+ membership and
certification programs on 30 June 2025. The dependency is a single point of
failure on a repository nobody new can be granted access to.

The obvious reading of that situation is "replace sdk-ant and delete it." That
reading is wrong, and the reason it is wrong is the substance of this ADR.

Three things constrain the answer:

1. **`src/ant_transport.h` already demonstrates the pattern that works.** One
   header defining a contract, three implementations, a Kconfig choice
   selecting one, and CI asserting which one got compiled. It works, it is
   asserted, and nobody has had to think about it in months. A second seam
   should look exactly like it rather than inventing a second idiom.
2. **A clean-room reimplementation needs something to be measured against.**
   `radiant` has to be byte-compatible with a protocol whose specification for
   the link layer does not exist in public. The only way to know whether it is
   correct is A/B against an implementation that is known correct — and that
   implementation is `libant.a`. Delete the sdk-ant backend and every A/B gate
   in the plan becomes unrunnable.
3. **The shim is self-checking, for free.** `src/ant_radio_sdk_ant.c` is about
   50 one-line forwarders plus a `BUILD_ASSERT` block comparing every `ANTW_*`
   constant against its `MESG_*` counterpart in sdk-ant's headers. If sdk-ant
   changes a signature the file stops compiling; if one of our constants drifts
   from Garmin's value the assert fires. Both checks exist only where sdk-ant is
   present — which is exactly where they *can* exist. That is a benefit no
   other configuration can provide, and it costs nothing to keep.

## Decision

### One seam, three backends

A single contract, `src/ant_radio.h` (`antr_*`, about 50 functions) plus
`src/ant_wire.h` (`ANTW_*` protocol constants), with three implementations
behind it:

```
src/ant_serial_bridge.c        unchanged logic; speaks only ANTW_*/antr_*
        |
   src/ant_radio.h             our ~50-function contract  (antr_*)
   src/ant_wire.h              our protocol constants     (ANTW_*)
        |
   +----+-----------------+----------------------+
   |                      |                      |
ant_radio_sdk_ant.c   ant_radio_stub.c      radiant/
(thin forwarders +    (the no-op radio)     (clean-room stack)
 BUILD_ASSERTs)                                  |
                                          radiant_radio_hal.h
                                                 |
                                       +---------+---------+
                                     nRF52/54L          EFR32 (later)
```

Everything is prefixed. This is not style: sdk-ant's error macros are computed
expressions (`NRF_ANT_ERROR_OFFSET + INVALID_MESSAGE`), not literals, so
identically *named* macros with non-identical token sequences are a hard
redefinition error — `ant_wire.h` and `ant_parameters.h` could never coexist in
one translation unit without the prefix. The prefix is what lets the shim
include both and assert them against each other. It also makes the bridge
textually free of Garmin's API names, which the clean-room narrative needs.

### CMake decides, Kconfig mirrors

`list(APPEND ZEPHYR_EXTRA_MODULES ...)` runs *before* `find_package(Zephyr)`,
therefore before Kconfig exists. A Kconfig symbol therefore cannot decide
whether sdk-ant's Kconfig gets sourced — the decision has to be made earlier, in
CMake. So:

- An `ANT_RADIO` cache variable (`sdk_ant` | `core` | `stub`), defaulting to
  `sdk_ant` when `ANT_MODULE_DIR` resolves and `core` when it does not.
  *(2026-08-20: the disk-following default is gone. It is `core`
  unconditionally — see the Switchover section for why a backend chosen from
  what happens to be checked out is the wrong kind of default.)*
- CMake writes a generated `.conf` fragment setting
  `CONFIG_ANT_DONGLE_RADIO_*`, so `.config` records the backend and CI can
  assert it rather than trust it.
- With `core` or `stub`, sdk-ant is never added as a module, `CONFIG_ANT` never
  exists, and the `FATAL_ERROR` that currently guards a missing checkout is
  deleted outright. No `CONFIG_ANT=n` dance.

(Implementing `ANT_RADIO` is Wave 2's work, not this ADR's. Wave 0 only
un-hardcodes the path — the `FATAL_ERROR` is still there, and still correct,
until the seam exists to replace it.)

Independence is **proved rather than asserted**: `scripts/build_all.ps1` builds
the `core` and `stub` targets with `-DANT_MODULE_DIR=` pointing at a path that
does not exist. If that succeeds, the tree does not depend on sdk-ant. This
costs one line and converts a claim into a test.

### sdk-ant stays a selectable backend

> **Amended 2026-08-20.** As originally written this section said sdk-ant was
> "built in CI whenever a token is present". It no longer is: the CI job is
> gone. Everything else here stands, and the four reasons below are why the
> backend itself stays. The distinction that matters is between *retiring a
> product* and *retiring a measurement instrument* — this is the first, not the
> second.

It is not a deprecated path, not a compatibility shim awaiting deletion, and
not something to be quietly removed once `radiant` works. It is a supported
backend selected by `-DANT_RADIO=sdk_ant`, kept in `west.yml` as a group-gated
optional project, and pinned at `v2.1.0` so that A/B runs months apart compare
against the same reference. It is built by hand, on a bench, when a comparison
is being run.

The four reasons, in order of durability:

1. **It is the A/B reference.** Every gate in the plan's Tier 2 table is
   phrased relative to sdk-ant: loss within +0.2 pp, timing within ×1.25,
   re-acquisition within ×1.5, ack success within −1 pp, sensitivity within
   1 dB-equivalent. Those numbers have no meaning without a run of the
   reference on the same rig.
2. **The `BUILD_ASSERT` block only exists here.** It is the only mechanical
   check that our protocol constants match Garmin's.
3. **`sim/`, the reference transmitter, is built on it.** Keeping the test
   fixture on Garmin's certified profile code is what stops an `ant_verify`
   pass degenerating into our decoder agreeing with our encoder — the
   two-wrong-implementations handshake that the checksum bug in this project's
   own history is a case study in.
4. **Fallback.** If `radiant` develops a field problem, the ability to ship
   the known-good stack the same afternoon is worth more than the tidiness of
   having deleted it. *(2026-08-20: this is now a local build rather than a CI
   artifact — the tree still builds it, but nothing produces an installable
   image of it without someone running `build_all.ps1 -Backend sdk_ant
   -NcsVersion v3.2.4`. "The same afternoon" is still true; "by downloading it
   from a run" is not.)*

### Release artifacts stay on `build-sdk-ant` until Tier 3 passes

> **RESOLVED 2026-08-20 — see [Switchover](#switchover-2026-08-20).** The
> acceptance below passed and release artifacts moved to `build-core`. The
> clause is kept, unedited, because the record of what the bar *was* is the
> only thing that makes "it was met" a claim rather than an assertion.

This is the clause that exists to prevent drift.

CI splits into `build-core` (no secret required, therefore green on forks) and
`build-sdk-ant` (gated on `SDK_ANT_CHECKOUT_TOKEN`). Both build. Only one
produces what gets attached to a release tag, and until further notice that is
**`build-sdk-ant`**. *(As of 2026-08-20 that split no longer exists:
`build-core` publishes and `build-sdk-ant` has been deleted.)*

The condition for switching is the Tier 3 acceptance, stated exactly:

> Zwift pairs a power meter, a heart-rate monitor and a controllable trainer
> against an `radiant` build, and holds a 30-minute ride with resistance
> changes taking effect throughout.

It is not automatable and it is not a proxy. Passing every Tier 1 and Tier 2
gate is necessary and not sufficient: those measure conformance and radio
behaviour against a reference, and the thing that actually has to work is a
commercial application nobody here controls, doing a session long enough for
slow failures to appear.

**When it passes, the switchover is recorded by editing this file** — changing
the status of this clause, dating it, and naming the run. It does not happen
because someone changed a default in a workflow file and everyone assumed the
gate had been met. A release default that moves silently is exactly the failure
this project's documentation rules exist to prevent.

## Consequences

**Good.**

- Losing sdk-ant access stops being fatal. `build-core` needs no secret, which
  finally makes CI green on forks — a thing that has never been true.
- The A/B methodology is structurally possible, because both sides of the
  comparison are buildable from one tree by flipping one CMake variable.
- The stub survives as the cheapest proof the seam holds: it builds in seconds
  and is the only configuration today that runs with zero sdk-ant.
- Adding an EFR32 backend later is an addition, not a redesign, because the
  HAL boundary is already where it needs to be.

**Costs, accepted.**

- Three backends is three things to keep compiling. CI builds all of them, so
  the cost is compute, not attention.
- A mechanical rename of 47 call sites and about 45 constant references in
  `src/ant_serial_bridge.c`, plus the stub (renamed in the same wave from
  `src/ant_stub.c` to `src/ant_radio_stub.c`). Zero behavioural risk, and
  verifiable before and after with `ant_probe.py`, `ant_features.py`,
  `ant_session.py` and `ant_bench.py` — which is why it is worth doing all at
  once rather than incrementally.
- The shim must absorb two couplings the bridge currently has: the event path
  inverts (`ant_cb_register()`/`ant_evt_t` becomes a fixed
  `void antr_on_message(const struct antr_msg *)` implemented by the bridge,
  because `ant_evt_t` is a four-level nested union whose layout is not ours to
  depend on), and `ant_channel_open()` is a macro rather than a function, so the
  split into `..._with_offset` has to be reproduced. Ten lines each, and the
  first deletes the hardest coupling in the bridge.
- Someone has to remember to edit this file when Tier 3 passes. That is the
  point: making the switchover cost a documented edit is the mechanism, not an
  oversight. *(2026-08-20: they did. That edit is the Switchover section at the
  top of this file.)*
