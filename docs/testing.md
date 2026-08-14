# Testing

Checked by: `python -m unittest discover -s tools -p "test_*.py"`, which is the
`host-tests` job in [`.github/workflows/build.yml`](../.github/workflows/build.yml)
and covers the host-side claims below. Everything from Tier 2 down needs
hardware and is run by hand; once `tools/ant_ab.py` and `tools/ab_gates.toml`
exist, the Tier 2 thresholds live in that TOML and this table becomes its
mirror rather than its source.

Nothing in this document should be believed about the radio without a bench
run. The one thing it does assert without hardware is *how to read a result*,
and that part is not opinion — it was arrived at by finding that half of the
old loss figure was the measuring tool.

---

## The tools

Everything is a host-side Python script over the dongle's own serial protocol.
The interpreter is the NCS toolchain's own Python — there is no system Python
in this project's workflow. Note that this is *not* the bundle that builds
the firmware: an sdk-ant build needs **NCS v3.2.4 with toolchain bundle
`fd21892d0f`** (what `build/release`'s `CMakeCache.txt` records for the shipping
images), because sdk-ant v2.1.0 fails to link against v3.4.0's renamed
`CONFIG_SOC_SERIES_NRF52X` — see [`backends.md`](backends.md#stub-build). The
toolchain's own interpreter is for the Python tools only, and may be a
different bundle than the one that builds firmware.

Two USB boards enumerate as the same
`0FCF:1009`, so every invocation that touches hardware needs `--serial`; a UART
build takes `--port COM8` instead, and every tool accepts it.

| Tool | What it proves | Board(s) |
|---|---|---|
| [`tools/ant_probe.py`](../tools/ant_probe.py) | The dongle answers questions about itself: reset → startup → capabilities → version. The smoke test everything else assumes | 1 |
| [`tools/ant_scan.py`](../tools/ant_scan.py) | The radio hears real ANT+ sensors. A wildcard slave channel, opened the way a fitness app opens one. Must print device number *and* type — that proves the extended-message path and `RXMATCH` reconstruction, not just that a packet arrived | 1 + a real sensor |
| [`tools/ant_session.py`](../tools/ant_session.py) | The session a fitness app actually runs: eight channels at their real profile rates, plus the acknowledged-data and burst paths | 1 + sensors |
| [`tools/ant_features.py`](../tools/ant_features.py) | Advertised capability bits vs bridged messages, and every set/get round trip. The only check that catches a payload offset wrong by one | 1 |
| [`tools/ant_bench.py`](../tools/ant_bench.py) | USB round-trip latency and throughput. `request capabilities` — cheapest message with a guaranteed reply and no side effects — so it measures the USB path and the bridge, never the radio | 1 |
| [`tools/ant_sim.py`](../tools/ant_sim.py) | Drives a spare board as an ANT+ sensor, so the radio can be tested without owning one. `--dry-run` needs no board at all | 1 (transmitter) |
| [`tools/ant_verify.py`](../tools/ant_verify.py) | The measuring instrument: loss, timing, decoded accuracy, accumulator continuity, liveness, loss accounting. `--replay` needs no board | 1 (receiver) |
| [`tools/ant_pages.py`](../tools/ant_pages.py) | Pure ANT+ page encode/decode. No hardware, host-testable, shared with `zephyr_aerosense` | none |
| [`tools/test_*.py`](../tools/) | The only tests here that run in CI | none |

Three more arrive with the rebuild and are named here so nothing invents a
second one: `tools/ant_conformance.py` (Tier 1), `tools/ant_ab.py` +
`tools/ab_gates.toml` (the Tier 2 gate runner), and `tools/ant_trace.py`
(normalises `ANT_DLL`'s `Device0.txt` into byte-level `.antser` captures).

## Host tests in CI

```sh
python -m unittest discover -s tools -p "test_*.py"
```

That job runs on `ubuntu-24.04` with nothing but `pip install pyusb`, and it is
the only job in the workflow that runs on a fork. It exists because
`ant_pages.py`, the sensor models in `ant_sim.py` and the analysis in
`ant_verify.py` are plain Python with no radio in them, and a byte-order or
accumulator-wrap mistake in any of them otherwise costs two boards and a flash
cycle to find.

C unit tests used to be a different story: `native_sim` does not build on
Windows — no host C compiler, no QEMU — so `west twister` cannot run locally,
and that was taken to mean the ztests in `radiant_core/tests/` execute only in
CI on Linux.

**That inference was wrong, and it cost the suite its first run.** The suites
touch no hardware at all: they drive the module against `tests/fake_radio.c`, a
mock HAL with a virtual clock. The board is not the thing under test — it is
merely a C runtime with a UART, and an attached DK is one that is available now.

```powershell
. .\scripts\env.ps1 -NcsVersion v3.2.4
.\scripts\run_ztest_hw.ps1
```

builds the test application for the nRF5340 DK, flashes it over J-Link, and
parses ztest's console output. A full run is about 40 seconds end to end, most
of it flash erase.

This does not replace the CI job and is not meant to. `twister` runs
`native_sim` at **32 bits**, which is the width that catches the struct-packing
and size assumptions a 64-bit host would hide; the DK runs at the real target
width instead. Two different checks — and this is the one you can have before
you push.

What it found the first time it ran, which is the argument for it: seven suites
green, and `transfer` at 6 of 16. Ten of those failures were one cause — the
suite registered its own radio callbacks, but the transfer engine had been
redirected to post to `radiant_sched.c`, so nothing was ever armed. The
eleventh was real and in the test rather than the module: an assertion named
`RADIANT_CTRL_ACK_LAST_SEQ0` (`0xE2`) while its own comment said `0xF2`, and
`0xF2` — the complemented sequence bit, the third of the three silent failure
modes that file exists to catch — is what the engine had correctly emitted all
along.

### The one secret, and why it cannot be avoided

`build-sdk-ant` is the only job needing a repository secret:
`SDK_ANT_CHECKOUT_TOKEN`, a **classic** PAT with `repo` scope, from an account
that has been granted access to `ant-nrfconnect/sdk-ant`. Without it the job
reports a skip rather than failing red.

Every cheaper option has been tried and none of them work, which is worth
recording so nobody re-litigates it:

- The automatic `GITHUB_TOKEN` is scoped to this repository alone, so it can
  never reach another organisation's private repo. A maintainer's *personal*
  access has to be delegated explicitly; that is the whole problem.
- A GitHub App token or a deploy key both need admin rights on
  `ant-nrfconnect/sdk-ant`, which individual contributors do not have.
- A fine-grained PAT works only if that organisation has opted into them.
- Under SAML SSO the PAT must additionally be authorised for the org, or the
  checkout fails **as though the repository does not exist** — a misleading
  error that reads like a bad revision rather than a permissions problem.

Set repository variable `SDK_ANT_REPO` to build against a fork; it defaults to
`ant-nrfconnect/sdk-ant`.

No network-key secret is required: a dongle receives its ANT+ network key from
the host over `MESG_NETWORK_KEY_ID`. `host-tests`, `ztest` and `build-core`
need no secret at all, which is what makes the build green on a fork.

---

## The four verification tiers

The tiers exist to answer one question: *is `radiant_core` a faithful replacement
for `libant.a`?* They are ordered by what they cost to run, so a divergence is
found by the cheapest tier that can see it.

### Tier 1 — frame conformance (one board, no radio, CI-able)

`tools/ant_conformance.py` drives every message `dispatch()` implements, valid
and malformed, and records the replies to a `.antser` file. **The A/B is a byte
diff of two files; byte-identical is the pass.** That catches most of the
divergence there is to catch — response codes, reply framing and sizes, error
mapping, the leading-index-byte shapes in `src/ant_serial_bridge.c` — with no
sensors and no statistics anywhere near it.

### Tier 2 — radio behaviour (two boards)

Board A is the transmitter and is **always sdk-ant** (`sim/` on the nRF54L15
DK). Board B is the receiver, alternating backends.

> **Hard rule: a Tier 2 A/B run goes A/B/A in one sitting, never across
> sessions.** This is the same protocol as the `synth.conf` experiment (XTAL /
> SYNTH / XTAL, alternating builds, one sitting), and it is not ceremony. The
> run-to-run spread on this bench is comparable to the effect being measured —
> 0.26 to 0.60 % across four identical 300 s runs — so a number recorded today
> against a number recorded last week measures the room, the neighbours' Wi-Fi
> and the position of the boards on the desk, not the backend. The repeated A
> is what makes the difference readable: if the two A runs do not agree, the
> sitting is void and the B number means nothing.

`tools/ant_ab.py` consumes JSON from `ant_verify.py` / `ant_bench.py` /
`ant_conformance.py`, applies the gates below from a reviewable
`tools/ab_gates.toml`, and prints a table in the shape of the USB-stack
comparison in [`backends.md`](backends.md). Baselines commit to
`archive/benchmarks/`.

| Gate | Threshold |
|---|---|
| Tier-1 conformance diff | **byte-identical** |
| `loss (exact)`, 300 s runs | ≤ sdk-ant + 0.2 pp. The bench floor is **characterised at 0.26–0.60 %**, ceiling 1.5 % — read the exact line, not the wall-clock one |
| **Unexplained loss** (a hole with no `RX_FAIL`) | **0.** `loss_accounting()` already enforces it; this is the check that caught the reader bug |
| Accumulator continuity violations | 0 |
| **`timing` line** (intervals minus whole periods) | ≤ sdk-ant × 1.25. **Not the `jitter` line** |
| Time to first packet / re-acquisition | ≤ sdk-ant × 1.5, and ≤ 5 s absolute |
| **Sensitivity** (inline attenuator, else a fixed open-air path, else the transmit-power ladder below) | attenuation/distance/transmit power at 5 % loss within 1 dB-equivalent of sdk-ant on the same rig; baseline recorded in Phase 4, **alongside die temperature** (see below) |
| 32-channel per-channel loss | ≤ single-channel + 0.5 pp — **absolute, not relative**: `libant.a` cannot reach 32 |
| **Ack-data success** (ERG mode) | ≥ 99 %, and ≥ sdk-ant − 1 pp |
| USB round-trip latency | ≤ sdk-ant × 1.1 (expect equality — the radio is not on that path) |

Per-phase functional gates use the tools above, in this order:
`ant_probe.py` → `ant_scan.py` → `ant_verify.py` against `sim/` → `ant_sim.py`
driving `radiant_core` as a sensor, received by the sdk-ant dongle →
`ant_session.py` for ack/burst and eight channels → `ant_features.py` as the
conformance gate.

**Before fitting any threshold to a bench number, check the number can be
accounted for.** The previous 1.0 → 2.5 % retune was fitted to a broken
measurement and encoded the tool's own bug as the spec.

#### The transmit-power ladder, when there is no attenuator

The sensitivity gate names an inline attenuator or a repeatable distance, and
this bench has neither — both are hardware. `tools/ant_sens.py` is the software
substitute: it opens one board as an ANT+ master, steps that master's transmit
power down until the receiver under test starts missing packets, and reports the
interpolated power at 5 % exact loss. That is the same attenuation, applied at
the other end of the link, and it needs nothing that is not already on the desk.

**It is not the transmit-power sweep that is closed.** The closed one swept the
*dongle's own* transmit power against loss at high SNR, looking for something
that was never there. This one steps the *master's* power to walk the *receiver*
through its knee. The two differ in which end of the link moves and which end is
being measured, which is the whole difference between an instrument and a
distraction. `ant_sens.py`'s header says so at length, for the next person who
reads the memory note and reaches for the stop button.

Three things about it are load-bearing:

- **Loss is not recounted.** Each rung's stream goes to
  `ant_verify.py`'s `ChannelAnalyzer`, and the figure read out is
  `loss (exact)` — the transmitter's own event counter, no clock. Past the knee
  too little of the stream survives for that counter to be readable, and the
  step records `loss_basis: master_sent` rather than pretending otherwise.
- **The dial is checked, not trusted.** Every rung records mean RSSI, and across
  the ladder that must fall about 1 dB per commanded dB with no rung far off the
  line. `CONFIG_ANT_DONGLE_TX_POWER_BOOST` on the transmitter folds levels 3 and
  4 onto the same power; a fine ladder against the wrong part writes raw
  register values that mean something else; firmware without the custom byte of
  `MESG_CHANNEL_RADIO_TX_POWER` transmits at 0 dBm for every fine rung. All
  three produce a normal-looking curve and a fabricated dB figure, and only the
  RSSI axis notices.
- **It refuses to extrapolate.** The six ANT power levels span 28 dB and the
  nRF52840 register table spans 48; a desk pair sits further above the knee than
  either. If the bottom rung still shows no loss, the answer is null and the
  boards need moving apart — a number extrapolated past the end of the ladder is
  worse than no number.

**The Phase 0 acceptance run is `--repeat 2`.** Two ladders on an unchanged rig
must agree to within 1 dB, which is `gates.sensitivity.repeat_max_delta_db`, and
it is the same 1 dB as the gate itself because a gate cannot be tighter than the
instrument that reads it. If the repeat is wider, every downstream claim
measured in dB has no gate at all, and that is a reason to fix the instrument
rather than to widen the threshold.

#### The noise floor, and the check that is not a gate

A `core` build logs a `noise rf=... floor=... busy=...` line once a minute,
built from receive windows that ended having heard nothing — which is exactly
the population whose level is the noise floor. Nothing in the core reads those
numbers; they exist so that a bench result nobody can currently explain has one
more thing to look at.

There is no gate on them, and there should not be: the feature measures rather
than changes, so there is nothing to regress. The sanity check is a comparison
rather than a threshold — **move the dongle from a USB 2.0 port to a USB 3.0
one and the reported `floor` should rise visibly**, because USB 3.0 broadband
noise routinely desenses a 2.4 GHz receiver by 10–20 dB. That delta appearing is
itself the evidence the measurement works; see the entry in
[`gotchas.md`](gotchas.md).

Read `floor` (the 10th percentile) for how deaf the receiver is and `busy` (the
90th) for how bursty the band is. They are different questions and a single mean
answers neither.

**The sensitivity baseline is read against a temperature-dependent
instrument.** nRF52840 errata 153 says RSSI has a temperature-dependent error;
`radiant_radio_nrf.c` now corrects it on nRF52840/nRF52833, sourced from
Nordic's own open-source 802.15.4 driver rather than derived on this bench -
see "RSSI temperature correction" in that file. **This bench's own part is
nRF54L15, which has no equivalent correction anywhere in Nordic's tree**, so a
run on this hardware is still uncorrected and a warm run still reads
differently from a cold one on exactly the terms `gotchas.md` describes for
the loss-floor measurement: the instrument, not the thing measured. Call
`radiant_radio_nrf_die_temp_c()` (`radiant_radio_nrf_diag.h`) alongside every
RSSI capture in a Phase 4 baseline or Tier 2 sensitivity run regardless of
which part it runs on - a diagnostic node can publish it as field type `0x10`
of [`radiant-telemetry.md`](radiant-telemetry.md)'s envelope, or a bench
script can just log it next to the RSSI it already records - and treat any
nRF54L15 sensitivity number as read against an uncorrected instrument until
this part gets the same fix nRF52840 did.

### Tier 3 — acceptance

Zwift pairs a power meter, an HRM and a controllable trainer, and holds a
30-minute ride with resistance changes taking effect. Not automatable, and it
is the gate for making `radiant_core` the release default. Release artifacts stay
on the sdk-ant build until it passes, and the switchover is a recorded decision
in [`decisions/0001-backend-selection-and-release-default.md`](decisions/0001-backend-selection-and-release-default.md),
not a drift.

### CC26xx coexistence build arms

The nRF pattern is direct / [`gate.conf`](../gate.conf) / [`coex.conf`](../coex.conf)
— three builds, isolating the arbiter and then the second stack. TI needs a
fourth, because the multi-protocol CPE patch is a confound the nRF side never
had (see [ADR 0015](decisions/0015-cc26xx-coexistence-design.md)):

| arm | fragment | contents | isolates |
|---|---|---|---|
| 1 | none | today: `RF_postCmd` + `rf_patch_cpe_prop` | the floor |
| 2 | [`ti_patch.conf`](../ti_patch.conf) | multi-protocol patch only, still `RF_postCmd` | **the CPE patch's PHY cost** |
| 3 | [`ti_gate.conf`](../ti_gate.conf) | arm 2 + `..._CC26XX_COEX=y` (hooks linked, `RF_scheduleCmd`), no 802.15.4 | the scheduler's cost |
| 4 | [`ti_coex.conf`](../ti_coex.conf) **+ [`ti_coex.overlay`](../ti_coex.overlay)** | arm 3 + `IEEE802154` + the forked driver in `radiant_core/coex154_ti/` | the neighbour's cost |

```powershell
. .\scripts\env.ps1 -NcsVersion v3.4.0
Push-Location C:\ncs\v3.4.0
west -z C:\ncs\v3.4.0\zephyr build -s <repo> -d <repo>\build\ti_gate `
     -b cc26x2r1_launchxl -p always -- `
     -DANT_RADIO=core -DRADIANT_BACKEND=cc26xx `
     -DEXTRA_ZEPHYR_MODULES=C:/ncs/v3.4.0/modules/hal/ti `
     -DEXTRA_CONF_FILE=ti_gate.conf
Pop-Location
```

**`arbiter_only_max_delta_pp` is measured arm 3 against arm 2, never against
arm 1.** Measuring against arm 1 would charge the arbiter for the patch
swap, and the patch swap could plausibly be the larger number — every PHY
constant in `radiant_radio_cc26xx.c` was measured under `rf_patch_cpe_prop`
(arm 1's patch), not `rf_patch_cpe_multi_protocol` (arms 2-4's).

**Arm 3 is genuinely meaningful here, more so than on nRF.** With a single
client, `pCmdBg` in the submit hook (`radiant_radio_cc26xx_arb.c`) is our own
previous command, so `RF_verifyGap` can refuse us where `RF_postCmd`
structurally never could. A non-zero arm-3-vs-arm-2 delta is not
automatically a regression — it can be the scheduler doing real work even
with nothing to arbitrate against.

**Arm 4 needs a fork and an overlay, not just a conf fragment**, and both are
easy to lose silently:

- Zephyr's 802.15.4 driver for this part has no re-post path for its background
  receive command, so the first preemption ends 802.15.4 reception permanently.
  `radiant_core/coex154_ti/ieee802154_cc13xx_cc26xx.c` is a vendored copy with
  that re-post added. `ti_coex.conf` sets `CONFIG_IEEE802154_CC13XX_CC26XX=n`
  so that exactly one of the two compiles — leaving it `y` is a
  duplicate-symbol link error on `driverlib/rfc.c`, which is the good case.
- Zephyr's own `cc26x2r1_launchxl.dts` **disables** the `ieee802154` node and
  deletes the `zephyr,ieee802154` chosen property. Without `ti_coex.overlay`
  the build fails on `error: '__device_dts_ord_53' undeclared` out of
  `DEVICE_DT_INST_GET(0)` — a devicetree problem wearing a C compiler's
  clothes, and worth recognising on sight because everything else in this arm
  fails like a Kconfig problem.

**Arm 4 builds; it has never run against an 802.15.4 peer**, because this bench
has none. Its ANT-side loss figure would be honest (the neighbour's endless
background receive contends for the core whether or not anyone answers it), but
the neighbour's own survival across a preemption — the whole point of the
fork — cannot be observed here. Do not record a coexistence verdict off it.

Always check the generated `.config`, not the build log:

```powershell
Select-String build\<dir>\<app>\zephyr\.config `
    -Pattern "^CONFIG_RADIANT_CORE_BACKEND_CC26XX|^CONFIG_RADIANT_CORE_COEX154|^CONFIG_IEEE802154_CC13XX_CC26XX="
```

#### Flashing the LaunchXL: the backchannel goes silent, and it is not a hang

Every `dslite` flash leaves COM14 — the XDS110 backchannel carrying the ANT
wire protocol — completely silent. Zero bytes, not garbage, to `ant_probe.py`
or to a raw ANT RESET frame over pyserial, on **every** image including the
null backend and a bare blink app, with the LED blinking and JTAG perfectly
healthy. It looks exactly like a firmware hang and once cost a long
investigation into an `RF_open()` hang that was never real.

`scripts/flash_ti.ps1` now pulses `xds110reset.exe` after each flash and the
problem is gone; there is nothing to do by hand and no USB replug is needed.
Two things about it are worth knowing anyway, because they are what makes it
work:

- **Order is load-bearing.** A bare reset into a *closed* port is not a weaker
  fix, it is no fix — measured, 0 bytes. `flash → reset → open` works;
  `reset → open` does not.
- **For a port that is already dead** and that you do not want to reflash to
  recover: open COM14 with **DTR and RTS asserted first**, *then* pulse
  `xds110reset.exe`, then read. That order recovers a live board without
  touching flash.

`-NoBackchannelReset` skips it, which is only useful for reproducing the
symptom deliberately.

#### The Phase C loss re-sweep, without editing source per rung

`RX_BW_CODE`, `AA_FILTER_VALUE`, `AGC_REF_VALUE` and `RX_END_SLOP_US` in
`radiant_radio_cc26xx.c` are each backed by a Kconfig int/hex
(`CONFIG_RADIANT_CORE_CC26XX_RX_BW_CODE`, `..._AA_FILTER`, `..._AGC_REF`,
`..._RX_END_SLOP_US`), defaulting to the values already measured and
documented in each constant's own comment - a plain build is
byte-for-byte what it was before these existed. Sweep a rung with
`-DCONFIG_RADIANT_CORE_CC26XX_RX_END_SLOP_US=250` (etc.) on the command
line instead of editing the source and rebuilding by hand; measure loss
against a paced transmitter (`tools/ant_sim.py` or `tools/ant_sens.py`'s
master, never whatever happens to be transmitting in the room - see ADR
0014's own measurement-discipline section for why that matters) with
`tools/ant_verify.py`.

**Sweep the baseline twice, at the two ends of the sweep, or the whole thing
is unreadable.** This is not general advice, it is what the 2026-08-14 run
found: two runs of the *unchanged* default image, 80 s and ~320 packets each,
came out 1.57 % and 2.19 % — 0.62 pp apart, which is two packets. Every rung
inside that band is a tie, and the first version of this sweep (RX_END_SLOP_US,
single-pass, no control) is worth re-reading knowing it. Both re-swept
parameters turned out to be at the top of their curve already; the numbers are
in `archive/benchmarks/2026-08-13-radiant-cc26xx.json`.

Since `scripts/flash_ti.ps1` resets the backchannel itself, a rung is
`flash → start the transmitter → measure` in one unattended script, with
nobody touching the bench between rungs. That is visible in the data: mean
RSSI held within 0.9 dB across all six runs, against 5 dB of drift in the
earlier hand-replugged sweep.

### Tier 4 — extension interop (Phase 7)

The RadiANT extensions must be provably invisible to everything that does not
want them. With a telemetry node transmitting alongside real ANT+ sensors: **a
shipping sdk-ant dongle and a Garmin head unit both continue to find and hold
every standard sensor, and neither reports an error or a phantom device.**
Capture it as an `.antcap` and add it to the replay fixtures, so the claim is
regression-tested rather than remembered.

**The longer-than-8-byte sub-case is blocked, not skipped.** It used to read
"repeat with a longer-than-8-byte frame on air", on the grounds that such a
frame is the case most likely to upset a stock receiver's `MAXLEN` handling.
Nothing in this project can emit one. Spike B enabled advanced burst and sent
24-byte blocks; every one fragmented into three 8-byte packets, and no frame
with a payload other than eight bytes has ever been seen on air. The mechanism
that was supposed to carry them — a length byte — does not exist, so
`docs/decisions/0005`'s extension axis 3 is withdrawn. Reinstate this check
when a non-eight-byte frame is actually captured with its control byte
decoded; until then there is nothing to test and no `MAXLEN` exposure to
measure.

**Identity Tier 2 has a bench claim of its own, and it is a UX claim.** (The
identity tiers of [`radiant-security.md`](radiant-security.md) section 4 are
unrelated to the verification tiers on this page; the collision is unfortunate
and the two never appear in the same sentence again.) A Tier 2 node re-rolls its
on-air device number at every power-up, so the check is a power cycle and what
survives it:

- **A keyed RadiANT receiver re-acquires with no host intervention.** It
  wildcard-searches the device type, hears the returning node under its new
  number, and confirms identity by whether `X_AUTH` verifies — not by the
  number. `radiant_core/tests/src/test_sec.c` asserts this against the mock, so
  the bench run is confirming on air what is already regression-tested; and the
  negative half is asserted there too, because a receiver that verified whatever
  it heard would "re-acquire" the first stranger to walk past.
- **An unkeyed receiver must re-pair every session, and that is correct
  behaviour rather than a defect.** A Garmin head unit or Zwift pairs by device
  number and the number it stored is gone. Anyone running this check should
  expect to re-pair and should not log it as a fault. It is the whole reason
  Tier 2 is opt-in, off by default, and RadiANT-device-types-only — see the
  `ANT_SIM_IDENTITY_TIER_2` Kconfig help, which says the same thing at the point
  somebody is about to switch it on.

### The security bench run (`X_AUTH` / `X_CONF`)

Everything about the transforms that can be checked without a radio already is:
RFC-published crypto vectors, the windowing state machine, the counter
reconstruction, the host surface and the on-air zero-cost claim all run in
`twister` against `fake_radio`. What is left for the bench is the small set of
claims a mock cannot make.

**Before believing any result, check the build.** Both flags, and then the
`.config`:

```
west build app -b adafruit_feather_nrf52840/nrf52840/uf2 -p always \
  -- "-DANT_RADIO=core" "-DRADIANT_BACKEND=nrf" "-DEXTRA_CONF_FILE=security.conf"
grep CONFIG_RADIANT_CORE_BACKEND_NRF= build/zephyr/.config
```

`-DANT_RADIO=core` alone has silently produced the inert backend before, and a
security run against a radio that never transmits looks exactly like a run with
no losses. The grep is the whole guard.

What the bench is for, and nothing else:

1. **The transforms cost no packets.** Run the standard 300 s loss-and-timing
   capture twice on one link — once with no switches, once with `X_AUTH` and
   `X_CONF` on — and compare `loss (exact)` against the 0.26–0.60 % floor. The
   transforms rewrite payload bytes and touch neither the schedule nor the
   address, so the prediction is *no measurable difference*; anything else means
   the event-thread crypto is displacing a slot, which is the one failure mode
   the mock's virtual clock cannot show.
2. **Detection latency is W+1 to 2W packets, not W.** Corrupt one payload byte
   in flight and count packets to the `unverified` verdict. The range is not
   slop — encrypt-then-MAC means a window's tag cannot be known until its last
   packet is built, so the tag transmitted during window *k* authenticates *k-1*.
   A measurement of exactly W would mean the lag is missing and the
   implementation is not what the spec describes.
3. **A secured channel is invisible to everything that did not ask for it.**
   With a secured node transmitting alongside real ANT+ sensors, a shipping
   sdk-ant dongle and a Garmin head unit must both still find and hold every
   standard sensor and report no error and no phantom device. This is the Tier 4
   claim above, with one addition: **a standard receiver pointed at the secured
   channel itself reads noise, and that is correct.** The transforms rewrite the
   bytes an ANT+ profile defines. Use a RadiANT device type on a channel of its
   own, and do not log "Zwift shows garbage on the secured channel" as a defect.
4. **The pairing fingerprint matches, and a wrong pairing does not.** Pair two
   nodes over `0xF5` and confirm both ends print the same six digits. Then pair
   against a third node and confirm the digits differ. The exchange succeeds
   either way — that is what an anonymous key agreement is — so the digits are
   the only observable that distinguishes a real pairing from a
   man-in-the-middle, and a bench run that never compares them has not tested
   the mitigation at all.

**Board cost, declared up front:** one Feather flash for the secured node, plus
whatever the receiver side needs. Feather flashes are rationed on this bench, so
this run is a deliberate act rather than something to fold into an unrelated
session.

Two scale checks, neither of which has a sdk-ant baseline to compare against
because `libant.a` cannot do either, so both are absolute: **32 channels
tracked simultaneously** with per-channel loss no worse than single-channel
+ 0.5 pp, and **background scan mode** hearing every sensor `ant_scan.py` finds
via a wildcard channel.

---

## Reading a bench result

Three numbers decide whether a run is good, and for each one there is a
neighbouring number that looks like it and is wrong.

- **Loss: read `loss (exact)`, never the wall-clock line.** The headline figure
  divides by elapsed time over the nominal period, which assumes the
  transmitter's crystal is exact and rounds. `loss (exact)` counts what is
  missing from the transmitter's own update event counter, with no clock
  involved. The floor on this bench is **characterised at 0.26–0.60 %** across
  300 s runs; the acceptance ceiling is **1.5 %**. A figure below 0.26 % is
  suspicious, not excellent.
- **Timing: read the `timing` line, not `jitter`.** `jitter` is a stddev over
  the raw gaps between packets, and one lost packet turns a 250 ms gap into a
  500 ms one — six losses in 1200 packets produce its entire figure on their
  own, so it has always moved with the loss number and added nothing to it.
  `timing` is the same packets with the whole periods subtracted out: the error
  without the count. It reads **~2.6 ms on the host clock and 0.009 ms on the
  radio's** — under one tick of the 32 kHz clock. The link's timing was never
  the thing `jitter` was measuring.
- **Unexplained loss must be zero.** The radio raises an `RX_FAIL` for a packet
  lost on the air and says nothing at all about one lost after the radio
  already had it. A hole with no `RX_FAIL` is a bug on the host or in the
  dongle, and it is exactly the signature that exposed the `FrameReader`
  timeout bug described below.

The rest of this document is the material that established those three rules,
moved out of `README.md` unchanged.

---

## Testing the radio

`ant_probe.py` only proves the dongle answers questions about itself. To
exercise the radio, `tools/ant_scan.py` runs the sequence a fitness app opens
with — ANT+ network key, wildcard slave channel, open, listen — and reports
what it hears:

```sh
python tools/ant_scan.py --seconds 30
```

But one channel is not what a fitness app does. Zwift opens a channel per
sensor it cares about and runs them simultaneously for the length of a ride, so
anything that only ever gets exercised on channel 0 stays untested.
`tools/ant_session.py` runs that session instead — all eight channels at their
real profile message rates — and additionally checks the two host-to-sensor
paths:

```sh
python tools/ant_session.py --seconds 30
```

- **Acknowledged data** is how Zwift sets trainer resistance. Sent at a paired
  channel, `TRANSFER_TX_COMPLETED` means a real sensor acknowledged it.
- **Burst** is probed at a closed channel on purpose. A dispatcher that does
  not implement the message answers `INVALID_MESSAGE`; one that does gets
  `CHANNEL_IN_WRONG_STATE` back from the stack. Both are errors, but only the
  second proves it reached the radio — and nothing goes on the air, which
  matters when the sensor in range is someone's trainer.

Two lessons for anyone writing another tool, both of which first showed up as
apparent firmware bugs: assigning a channel that a previous run left assigned
is refused with `CHANNEL_IN_WRONG_STATE`, which is why every host opens with a
system reset; and a close is asynchronous, so unassigning before
`EVENT_CHANNEL_CLOSED` arrives is refused for the same reason.

## Testing the radio without owning a sensor

`ant_scan.py` and `ant_session.py` can only report what happens to be on the
air. If you have a Nordic DK and no power meter, that is nothing at all, and
the radio half of this firmware stays untested.

It does not have to. The dongle is a transparent bridge, so it forwards
`CHANNEL_TYPE_MASTER` to `ant_channel_assign` and `MESG_BROADCAST_DATA_ID` to
`ant_broadcast_message_tx` like any other message — which means a board running
**this firmware, unmodified**, will happily impersonate an ANT+ sensor if a
host asks it to. `tools/ant_sim.py` is that host:

```sh
# board A: pretend to be a 100 W, 80 rpm power meter
python tools/ant_sim.py --profile power --watts 100 --cadence 80 --seed 1 --serial <A>

# board B: the dongle under test, listening and measuring
python tools/ant_verify.py --expect-watts 100 --expect-rpm 80 --seconds 60 --serial <B>
```

**Two boards are needed.** One transmits and one receives; a board cannot hear
itself. Disambiguate them with `--serial` over USB, or `--port COM8` for a UART
build.

`--profile` takes `power` (page 0x10), `power-torque` (pages 0x11 and 0x12),
`power-torque-freq` (page 0x20) or `csc` (combined speed and cadence, device
type 0x79), and repeats — each profile gets its own channel, so several
sensors can be on the air at once from a single board.

Pacing comes from `EVENT_TX`, not from a host timer. The stack raises that
event when it has put a payload on the air, which is exactly when the next one
should be loaded, so the script makes no assumption about how fast the host or
the USB path is. There is a wall-clock fallback for the case where the event
never arrives, and it prints loudly and fails the run — a silent fallback would
hide the one failure worth knowing about.

`ant_verify.py` is the measuring instrument, and it is deliberately told
nothing about the transmitter: the packet count to expect comes from the
channel period, the power to expect comes from the accumulators, and the page
rotation to expect comes from the page numbers. It reports loss, jitter, mean
absolute error against `--expect-watts`/`--expect-rpm`, accumulator continuity,
common-page spacing, and whether the sensor is actually alive rather than
repeating one good packet. `--json` gives machine-readable output in
`ant_bench.py`'s style.

**Pin the device number.** `ant_verify.py` opens a wildcard channel by default,
exactly as a fitness app does — so it pairs with whichever ANT+ sensor of that
type it hears first. If there is a real power meter anywhere nearby it will
find that instead, and the run reads as 25 % loss and 0 W because it is
measuring somebody's idle trainer through a wall. `ant_sim.py` and `sim/` both
default to device **14871**:

```sh
python tools/ant_verify.py --device-number 14871 --trans-type 5 ...
```

A good run against the `sim/` firmware, measured on the bench:

```
OK   loss                   0.33 % (limit 1.50 %, one packet = 0.08 %)
OK   jitter                 stddev 14.7 ms, max 500.0 ms (limit 124.8 ms, period 249.7 ms)
OK   accumulator continuity 0 violation(s)
OK   loss (exact)           0.34 % - 4 of 1163 page 0x10 message(s) missing
OK   sensor liveness        the event counter advanced on 1159 of 1159 packet pairs
OK   power accuracy         mean abs error 1.69 against 100 (limit 10.00)
OK   common pages           80 x10, 81 x10, worst gap 119 (limit 121)
  timing: 2.58 ms off the 249.7 ms slot grid by the host clock, 0.009 ms by the radio's
  signal: mean -28.5 dBm (min -34, max -22, n=1190) - ANT tracks to about -90

channel events: RX_FAIL x4
4 packet(s) missing, all of it reported by the radio as RX_FAIL, so it
happened on the air.
```

Three of those lines exist because the graded checks above them turned out not
to measure what their names claim. `jitter` is a stddev over the gaps between
packets, and one lost packet turns a 250 ms gap into a 500 ms one: six losses
in 1200 packets produce a 17 ms stddev on their own, so that check has always
moved with the loss figure and added nothing to it. The `timing` line is the
same packets with the slot count subtracted out — the error, without the count
— which is why it reads 2.58 ms where the check above says 14.7. Measured on
the radio's own 32 kHz clock rather than on Windows, it is 0.009 ms, under one
tick. The link's timing was never the thing being measured.

**Half of what this used to call the floor was the tool measuring itself.**
The bench sat at "about 1 %" for a long time, and the number was wrong.
`FrameReader` read from USB with a 250 ms timeout while an ANT+ power meter
transmits every 249.7 ms, so essentially every read was racing the packet it
was waiting for. libusb-win32 cancels the pending transfer when a read times
out, and a cancel landing on top of the dongle's answer loses it — the host
never sees the frame and the dongle, whose USB stack reported success, never
knows. It cost about 0.4 percentage points of invented loss and several ms of
invented jitter.

It hid for so long because a fabricated loss looks exactly like a real one in
every figure the report prints. The one thing that gave it away is that the
radio raises an `RX_FAIL` for a packet lost on the air and says nothing at all
about a packet lost after the radio already had it. Runs were losing about
twice what the radio would admit to. That comparison is now a check of its own
and fails on its own, so this cannot come back quietly.

With the timeout raised clear of the channel period, the same two boards on the
same desk measure **0.26, 0.43, 0.51 and 0.60 %** across 300 s runs, and every
missing packet is one the radio reported. What is left really is the air:

| Change | Loss |
| --- | --- |
| ANT+ frequency, 0 dBm | 0.74 / 0.94 / 1.37 % |
| 2450 MHz | 2.40 % |
| 2478 MHz | 1.81 % |
| −12 dBm | 0.85 % |
| +4 dBm | 1.11 % |

Those were all recorded through the broken reader, so every one of them is
inflated by roughly the same amount. They are kept because a common-mode error
cancels in a comparison even when it ruins the absolute values, and the
comparison is the point: no frequency was cleaner than the ANT+ one — the band
is busy everywhere, and 2478 sits beside the BLE advertising channel at 2480 —
and a 16 dB spread of transmit power stayed inside the run-to-run spread of
0 dBm alone. Loss that survives 16 dB is not a link-budget problem. A 4 Hz ANT
channel is a ~150 µs burst every 250 ms, and an occupied slot is occupied at
any power.

Clock drift is not it either — both ends run 50 ppm crystals, which is
nanoseconds across a 250 ms period. Unplugging the co-channel ANT+ trainer that
`tools/ant_scan.py` found on 2457 MHz moved the figure by about 0.1 points,
which is inside the run-to-run spread: it was a real neighbour and not the
explanation.

**What is left has no pattern in it, and that is the finding.** Putting every
packet from seven 300 s runs back on the transmitter's slot grid and looking at
the holes:

- **Every hole is one slot. Never two in a row**, across 57 of them. For
  independent per-slot loss at the observed rate, the arithmetic predicts 17.8
  isolated holes and 0.09 adjacent pairs in the post-fix runs; there were 18
  and 0. Interference that lasted even two slots would not do that.
- **They do not cluster in time.** Against 4000 random shuffles, pairs of holes
  falling within 5, 15, 30 or 60 s of each other are no more common than
  chance (p ≥ 0.08 everywhere). No episodes, no bad minutes.
- **They have no preferred phase** against the 102.4 ms Wi-Fi beacon interval,
  against one second, or against the sensor's own background-page rotation.
- **They are not the link running out of signal.** With `ENABLE_RSSI` the
  dongle reports about −28 dBm against a tracking threshold near −90: some
  60 dB in hand. Within one run the signal wandered between −27 and −37 dBm as
  the room changed, and the losses went the wrong way — five in the −27 dBm
  stretch and none in the −36 dBm one. This is what the earlier transmit-power
  sweep was groping at: you would have to throw away 60 dB before power became
  the limit, and the knob only spans 16.
- **Both ends can account for every one.** The receiver raised an `RX_FAIL`, so
  it was awake, tuned and heard nothing valid in a slot it was listening to.
  The transmitter's event counter advances by two across each hole, so the
  sensor did produce and send a message for that slot. Neither end skipped it.

That is a memoryless per-slot corruption process, which is the signature of
unrelated traffic landing on top of a ~150 µs burst — and 2457 MHz, which ANT+
mandates, sits inside Wi-Fi channel 11. About one slot in two hundred collects
somebody else's packet. Getting nearer zero than this means a quieter room, not
a better setting, and there is nothing in the dongle left to fix: the radio was
listening with 60 dB to spare and the message never arrived intact.

**Nor is there much ANT could do about it.** Frequency agility, where both ends
hop between three frequencies under interference, is exactly the right feature
and is not available here — ANT+ device profiles mandate 2457 MHz, so using it
would mean the dongle no longer talks to real sensors. Continuous scanning mode
would keep the receiver on permanently, but the `RX_FAIL`s already say the
window was open at the right moment, so there is nothing for it to catch. ANT
broadcast has no retransmission by design.

**Which is fine, because the profile was built for exactly this.** A lost 0x10
costs no data at all: accumulated power and the event counter are cumulative,
so the next packet carries what the missing one would have said. That is what
the `accumulator continuity` check is watching, and five minutes of this still
decodes power to within 1.7 W of the target with zero violations. The right
response to 0.4 % isolated single-slot loss on an ANT+ link is to decode it
correctly, which this does.

The last line of the report is the one to read when a run does lose packets.
Losses the stack reported as `RX_FAIL` happened on the air and are the room's
business; losses it never reported happened after the radio, on the host or in
the dongle, and are somebody's bug. That is the difference between moving the
boards apart and looking for the thing above.

**Give it a few minutes.** A minute at ~4 Hz is only 240 packets, so one
dropped packet is 0.4 % — the report prints that resolution next to the number
for exactly this reason. `--seconds 300` also gets the 16-bit accumulators
through a wrap, which a one-minute run does not.

Three things are worth knowing before reading a result:

- **A short run does not test the wraps.** The 16-bit accumulators are meant to
  roll over, and a receiver that widens before subtracting is correct until
  they do. `ant_verify.py` prints which ones wrapped during the run and says so
  when none did. Ten minutes covers all of them; a minute covers none of the
  slow ones.
- **`--seed` is not optional in spirit.** The noise is pseudorandom, and a
  measurement that cannot be replayed is not a measurement.
- **Loss is measured twice, and the exact one is worth more.** The headline
  figure divides by elapsed time over the nominal period, which assumes the
  transmitter's crystal is exact and rounds. Where the transmitter steps its
  update event counter once per message — sdk-ant's does, so `sim/` does too —
  that counter is a serial number, and `loss (exact)` reports what is missing
  from it with no clock involved. A real crank power meter steps it per
  revolution and stops when the rider coasts, so the check reports nothing
  rather than guessing.
- **`--record FILE` saves what was heard.** Those captures replay through
  `ant_verify.py --replay` with no hardware, and they are what
  `zephyr_aerosense`'s ANT decoder tests are built from. Three of that
  project's five test fixtures were recorded from this bench.
- **Common pages come every 121 messages, not 65.** The generic ANT+
  common-page guidance says 65 and this tool used to enforce it, which failed
  the first certified transmitter it was ever pointed at. sdk-ant's own
  bicycle power profile settles it: `COMMON_PAGE_80_INTERVAL 119`,
  `COMMON_PAGE_81_INTERVAL 120`, commented *"Minimum: Interleave every 121
  messages"*.

Nothing here needs a board at all if you only want to check the host side.
`tools/ant_pages.py` is pure encode/decode, and `ant_sim.py --dry-run` builds
the same payload stream without a radio:

```sh
python -m unittest discover -s tools -p "test_*.py"

python tools/ant_sim.py --dry-run --record run.antcap \
  --profile power --profile csc --seconds 900 --seed 1
python tools/ant_verify.py --replay run.antcap --expect-watts 100 --expect-rpm 80
```

Those tests are the only ones in `tools/` that run in CI, and they are there
because a byte-order mistake found on the host costs nothing while the same
mistake found on the bench costs two boards and a flash cycle.

## Simulator firmware on an nRF54L15 DK

`ant_sim.py` needs a host attached to the transmitting board. [`sim/`](../sim/)
does not: it is a standalone application that makes a DK *be* an ANT+ sensor,
untethered, from power-on.

The nRF54L15 DK is the natural host for it, and for a reason that is otherwise
an annoyance — it has no USB device controller in silicon, so it can never be a
dongle. That keeps the nRF5340 DK free as the debug board while the nRF54L15
sits on the bench transmitting.

```powershell
. .\scripts\env.ps1 -NcsVersion v3.2.4
Push-Location C:\ncs\v3.2.4
west -z C:\ncs\v3.2.4\zephyr build -s <repo>\sim -d <repo>\build\sim_l15 `
     -b nrf54l15dk/nrf54l15/cpuapp --sysbuild -p always
Pop-Location
.\scripts\flash_sim_jlink.ps1
```

Then run the *same* `ant_verify.py` invocation against the dongle under test.
The verifier is deliberately transmitter-agnostic, so an identical pass against
`ant_sim.py` and against this firmware is the evidence that the two agree.

`CONFIG_ANT_SIM_PROFILE_*` picks what it transmits — standard power page 0x10,
crank torque 0x12, wheel torque 0x11, or the combined speed-and-cadence page —
and `CONFIG_ANT_SIM_TARGET_WATTS` / `_TARGET_RPM` / `_NOISE` / `_SEED` set the
signal. Buttons 1 and 2 nudge the target while it runs; the DK LEDs show
channel state, which answers "did the channel even come up" without a serial
cable.

Three things about this build are worth knowing:

- **The page encoding is sdk-ant's, not ours.** `ant_bpwr` and `ant_bsc`
  already build every page correctly. What `sim/src/sim_signal.c` replaces is
  the stock *simulator*, which sweeps a ramp from 0 to 2000 W — a receiver
  cannot be shown to be right about a value that never settles.
- **One profile per build.** sdk-ant's BPWR sensor sends page 0x11 or 0x12, not
  both, so the interleaved-torque-page case stays an `ant_sim.py --profile
  power-torque` scenario. That is the case that catches a receiver keeping one
  accumulator baseline for two pages.
- **`CONFIG_ANT_EVALUATION_KEY` is the ANT stack's licence key.** It is not the
  ANT+ network key, which `ant_plus_key_set()` supplies at runtime. Confusing
  the two produces a build that runs happily and is never heard.

`CONFIG_ANT_SIM_RF_FREQ` and `CONFIG_ANT_SIM_TX_POWER` exist to measure the
bench rather than to configure the sensor. At their defaults — 57, meaning
2457 MHz, and level 3, meaning 0 dBm — this is a valid ANT+ device transmitting
at the power a real power meter uses, which is the only setting that tests what
a dongle will meet in the field. Moving either one answers the question "is
this loss the room or the boards", and on the bench here the answer was the
room in both directions: no frequency was cleaner than the ANT+ one and more
power did not help. Those runs predate the reader fix above, so their absolute
figures are all inflated together — which leaves the comparison between them
intact and is the only thing they were for.
`ant_verify.py --rf-freq` has to be given the same number,
or the two sit on different channels and nothing is heard at all.

`scripts/flash_sim_jlink.ps1` wraps the J-Link sequence, and its first line is
load-bearing: `exec DisableAutoUpdateFW`. J-Link V9.66 carries newer onboard
debugger firmware than these DKs ship with and tries to upgrade it on every
connect. That upgrade fails at `Communication timeout. Emulator did not
re-enumerate` every time and leaves the probe in its bootloader until it is
physically replugged.

## Reading logs without a debugger

The Feather has no debugger, so `CONFIG_USE_SEGGER_RTT=y` logs are unreadable
on it. `diag.conf` swaps RTT for a log backend that buffers to RAM and
periodically commits to a reserved flash region. The Adafruit bootloader
exposes that region inside `CURRENT.UF2`, so the log can be read back over the
same USB drive used for flashing:

```powershell
west ... -d build\diagstub -- "-DEXTRA_CONF_FILE=stub.conf;diag.conf"
.\scripts\flash_uf2.ps1 -Uf2Path build\diagstub\ant_dongle\zephyr\zephyr.uf2
# let it run a few seconds, then double-tap RESET
.\scripts\read_flash_log.ps1
```

It also overrides `k_sys_fatal_error_handler`, so an early fault is captured
with its PC/LR rather than silently halting the CPU — from the outside a halt
is indistinguishable from a hang, since it kills USB, the LED heartbeat and the
periodic flush all at once.

Two things pin down `CONFIG_ANT_DONGLE_FLASH_LOG_OFFSET`: it must sit above the
image, and inside the window the bootloader actually dumps. Bootloader 0.8.0
dumps `0x1000`–`0xEA000`, which stops short of the `0xEC000` end of the code
partition — so a slot at `0xEB000` is written correctly and is simply invisible
in the readback. A second copy is written at `0x40000` in case another
bootloader version exposes a narrower window.

## Debugging on an nRF5340 DK instead

A fault inside `usb_enable()` is invisible on the Feather, and the flash log
cannot capture what never got scheduled. An nRF5340 DK has an onboard J-Link
*and* a separate "nRF USB" connector wired to the SoC's own device peripheral,
so the same class code can be enumerated by a real host while its own account
of events comes out the VCOM port. Both cables at once, no conflict. Two of the
gotchas above were found this way in minutes after days of guessing from the
outside.

```powershell
west -z C:\ncs\v3.2.4\zephyr build -s . -d build\dk5340 `
  -b nrf5340dk/nrf5340/cpuapp -- "-DEXTRA_CONF_FILE=stub.conf"
Copy-Item build\dk5340\merged.hex D:\     # the JLINK drive
```

Logs go to the VCOM COM port at 115200 (the *second* of the two the J-Link
exposes), not RTT — RTT would need SEGGER's `JLinkARM` DLL installed, whereas
the VCOM is just a COM port carried by the cable that already programs the
board. [`boards/nrf5340dk_nrf5340_cpuapp.conf`](../boards/nrf5340dk_nrf5340_cpuapp.conf)
sets that up along with `CONFIG_LOG_MODE_IMMEDIATE=y`, so the last line before
a hang has already been emitted rather than sitting in a queue that is about to
be discarded.

Build the DK with `stub.conf`. sdk-ant on an nRF5340 is the dual-core `ANT_NP`
path, which is not what the Feather runs; the DK is here for the USB half.

An nRF54L15 DK cannot substitute: no chip in the nRF54L05/L10/L15 family has a
USB device peripheral at all.

When a build is worth bisecting, Zephyr's own
`samples/subsys/usb/legacy/cdc_acm` is the reference: it enumerates on this
board under NCS v3.2.4 as two COM ports, which separates "USB is broken on this
board" from "our class is broken".
