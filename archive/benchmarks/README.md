# `archive/benchmarks/` — the sdk-ant baselines

Checked by: `python -m json.tool archive/benchmarks/baseline.schema.json` today.
Once baselines exist, `tools/ant_ab.py` validates each one against
[`baseline.schema.json`](baseline.schema.json) and applies the thresholds from
`tools/ab_gates.toml`, and that is what fails when a file here drifts.

**Status: one sdk-ant sitting, plus one paired cross-vendor A/B/A sitting.**

* [`2026-08-09-sdk-ant.json`](2026-08-09-sdk-ant.json) — two 300 s radio runs
  and a USB latency run against `sim/` on an nRF54L15 DK.
* [`2026-08-13-radiant-nrf.json`](2026-08-13-radiant-nrf.json) — the A legs of
  an A/B/A: `radiant_core`'s nRF backend on an nRF54L15 DK, **0.139 %** and
  **0.150 %** loss.
* [`2026-08-13-radiant-cc26xx-ab.json`](2026-08-13-radiant-cc26xx-ab.json) — the
  B leg of the same sitting: the CC26x2 backend on a LAUNCHXL, **3.744 %** loss.
* [`2026-08-13-radiant-cc26xx.json`](2026-08-13-radiant-cc26xx.json) — a
  standalone 300 s run and a serial latency run for the CC26x2 backend,
  recorded before the paired sitting. Not part of an A/B.

The three-file A/B/A is a real sitting: one transmitter, running continuously,
never touched, receivers alternated, and the two A legs agree to 0.011
percentage points.

**`tools/ant_ab.py` will not gate it, and that is correct.** It compares `rig`
dicts wholesale and the receiver board genuinely differs — you cannot put the
nRF backend on a CC2652R, so a cross-vendor A/B cannot satisfy a same-rig rule
built for alternating *firmware* on one board. Nothing was edited to make the
tool agree. See
[ADR 0014](../../docs/decisions/0014-second-vendor-port-what-it-cost.md) for
the numbers, the 13 dB RSSI caveat, and why `[gates.sensitivity]` is still
unmeasurable for *every* backend (`tools/ant_sens.py`'s power ladder does not
change the transmitter's power, and it crashes at `ant_sens.py:800`).
Still the most perishable item in the project, because what it does *not* cover
is named in that file's own `notes` and each gap is a separate bench sitting:
`sensitivity` (needs an attenuator or a distance sweep), `scale` (`libant.a`
allocates 8 channels, so there is nothing to measure at 32), and **`ack_data`
(no A leg — `[gates.ack_data]` in `tools/ab_gates.toml` is therefore still
`required = false`, and it is the one Zwift actually needs)**. Measuring
`ack_data` needs a held master/slave pair, which `sim/` does not provide.

## Why the empty directory was the problem

`radiant_core` is measured against `libant.a`. Every gate in
[`docs/testing.md`](../../docs/testing.md) — loss, timing, acquisition,
sensitivity, ack-data success, USB latency — is phrased relative to what
sdk-ant does *on this rig, with these boards, in this room*. Not to a datasheet
figure and not to a remembered number.

sdk-ant is a private, non-redistributable repository pinned at `v2.1.0`, reached with
one person's personal access token. The day that access lapses, every one of
those numbers becomes unobtainable. Not harder to obtain — unobtainable. And
the failure is silent: the rebuild proceeds, the gates get quietly restated as
absolutes, and nobody can tell whether `radiant_core` is as good as what it
replaced.

**You cannot A/B against a baseline you never recorded.** That sentence is why
Phase 4 does not wait for Phase 5, and why this directory has a schema before
it has data.

## Bench need — declared

Filling this needs two boards, a human to double-tap RESET on the Feather, and
ideally an inline attenuator. **No flash was performed by the agent that wrote
this directory.** What it did instead was fix the format, so the bench sitting
is spent measuring.

## What a bench sitting must produce

One JSON file per sitting, per backend, named `<date>-<backend>.json` — for
example `2026-08-15-sdk-ant.json`. Validate it against the schema before
committing:

```powershell
C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe -m json.tool <file> > $null
```

The schema is annotated field by field; read it rather than this file for the
details. What follows is the reasoning that the schema can only gesture at.

### Run A/B/A in one sitting, never across sessions

Board A is the transmitter and is **always sdk-ant** — `sim/` on the nRF54L15
DK, running Garmin's certified profile code. Board B is the receiver and
alternates backends. Measure sdk-ant, then the other backend, then sdk-ant
again, without moving anything.

This is not ceremony. The room changes: Wi-Fi traffic, bodies, a laptop lid.
The second sdk-ant run is what tells you whether the difference you measured
was the backend or the afternoon. It is the same protocol the `synth.conf`
experiment used, and `docs/testing.md` states it as a hard rule.

### Record the whole tool output, not a summary

`radio_runs[].verify` and `usb_runs[].bench` take the **verbatim** JSON from
`tools/ant_verify.py --json` and `tools/ant_bench.py --json`. Embedded whole,
untrimmed, unedited.

The `derived` block beside each one lifts out the handful of numbers the gates
read, so a gate runner does not need to know the tool's object shape. It is a
convenience, not the record. If the two ever disagree, `verify` is right.

The reason for keeping everything is that the field nobody thought worth
keeping is the one a future question needs. The page-mix counts, the
accumulator-wrap dictionary, the common-page gap histogram: none of them is
gated on, and any of them may be the thing that explains a result two years
from now.

### Read the exact line, not the wall-clock one

`derived.loss_exact_pct` is the gate. It counts what is missing from the
transmitter's own event counter, with no clock involved. `loss_pct` divides by
elapsed time over the nominal period and assumes the transmitter's crystal is
exact.

The bench floor is **characterised at 0.26–0.60%**, ceiling 1.5%. That floor is
memoryless per-slot collision on a ~150 µs burst inside Wi-Fi channel 11 over
2457 MHz, with an `RX_FAIL` for every hole. It is understood and it is closed.

**Do not re-run the frequency or transmit-power sweep, and do not
re-investigate the residual loss.** Both are settled: a 16 dB power spread
stays inside one setting's own run-to-run spread, and no frequency beat the
ANT+ default.

**And before fitting any threshold to a number, check the number can be
accounted for.** A previous 1.0 → 2.5% retune was fitted to a broken
measurement and encoded the tool's own bug as the spec. `unexplained_loss` — a
hole with no `RX_FAIL` — must be **0**, and a nonzero value invalidates the
sitting rather than failing one gate.

### `timing`, not `jitter`

`derived.timing_offgrid_host_ms` is the `timing` line: intervals minus whole
periods. The `jitter` line is a stddev over raw gaps, so one lost packet turns
250 ms into 500 ms, and six losses in 1,200 packets produce its entire figure.
Gating on jitter gates on loss twice under a different name.

Record both `timing_offgrid_host_ms` (~2.6 ms) and
`timing_offgrid_radio_ms` (~0.009 ms). The gap between them is the measurement
that says the host clock is the instrument and the link is fine — and it is
what the sub-millisecond multi-sensor fusion capability rests on. It needs lib
config **`0xE0`** (channel ID **+ RSSI + RX timestamp**), not the `0x80`
device-ID-only setting.

### The sensitivity curve is not optional

`sensitivity` is the slot most likely to be skipped because it takes the
longest, and it is the one with the least chance of being recoverable later.

Sweep attenuation (or distance, if no inline attenuator is available) in steps,
`seconds_per_step` at each, recording exact loss at every point. Report
`loss5pct_attenuation_db`: the interpolated attenuation at 5% exact loss. That
single number is what the gate compares, to within 1 dB-equivalent on the same
rig.

It is load-bearing three times over:

1. It is the **Tier 2 sensitivity gate's** reference.
2. Sensitivity is a stated core value of this project — the nRF52840's ~10 dB
   over an nRF24AP2 is in the first paragraph of `README.md`. A claim of that
   size deserves a measurement.
3. **Spike C shares this rig.** Silicon Labs' ~−105 dBm for the EFR32xG24 is
   quoted for BLE 1M — 1 Mbps at 250 kHz deviation — while ANT is 1 Mbps at
   ~170 kHz, a different RX filter bandwidth. The number does not transfer. The
   only way to find out whether the xG24 keeps its advantage on ANT's PHY is
   the same transmitter, the same attenuated link, two receivers. Without this
   curve there is nothing to compare against, and the EFR32 backend gets
   scheduled on a datasheet quote.

### Absolute gates, and why they have no baseline

`scale` (32 channels, background scan) has **no sdk-ant reference and must not
be given a fabricated one**. `libant.a` allocates 8 channels and advertises
scan mode off without implementing it, so there is nothing to compare to. The
gate is per-channel loss no worse than single-channel + 0.5 pp — measured
against *ourselves*, absolutely.

Leave the whole `scale` object out of an sdk-ant baseline rather than filling
it with zeroes. An absent measurement is honest; a zero is a claim.

## What lands where

| Gate in `docs/testing.md` | Field |
|---|---|
| Tier-1 conformance diff | `conformance.sha256`, `conformance.byte_identical_to` |
| `loss (exact)`, 300 s | `radio_runs[].derived.loss_exact_pct` |
| Unexplained loss | `radio_runs[].derived.unexplained_loss` |
| Accumulator continuity | `radio_runs[].derived.accumulator_violations` |
| `timing` line | `radio_runs[].derived.timing_offgrid_host_ms` / `_radio_ms` |
| Time to first packet / re-acquisition | `radio_runs[].derived.time_to_first_packet_s` / `reacquire_s` |
| Sensitivity | `sensitivity.loss5pct_attenuation_db` |
| 32-channel per-channel loss | `scale.per_channel_loss_pct` |
| Ack-data success (ERG) | `ack_data.success_pct` |
| USB round-trip latency | `usb_runs[].derived.latency_p50_ms` |

Thresholds live in `tools/ab_gates.toml`, deliberately not here. A baseline
records what was measured; a gate records what is required. Keeping them in
separate files is what makes it obvious when somebody moves a threshold to meet
a result instead of the other way round.

## Budget

A baseline file with four embedded `ant_verify.py` outputs runs to tens of
kilobytes, so the whole Phase 4 set fits inside the 10 MB `archive/` budget
several times over. The `.antcap` recorded alongside each run does not live
here — it goes to [`../captures/radio/`](../captures/radio/), and
`radio_runs[].capture` points at it. A run with a capture can be re-analysed
after a decoder fix; a run without one is only ever worth the numbers it
already printed.
