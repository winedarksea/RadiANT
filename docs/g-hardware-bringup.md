# Package G: hardware bring-up, on the nRF54L15 DK

nRF54L15 DK, NCS v3.4.0, Matter arm (`thread.conf;matter.conf`,
`-DANT_RADIO=core -DRADIANT_BACKEND=nrf -DSB_EXTRA_CONF_FILE=matter_sysbuild.conf`).
The coexistence fixes in `docs/p4-coexistence-bugs.md` are what made these
captures possible - before them the Matter arm could not stay up long enough to
run a 200+ second bench script.

**Scope.** Item 1 is "the server starts and creates endpoints, observed over the
log console" - **there is no Matter controller on this bench** (no Home
Assistant, no Apple Home, no `chip-tool`), so nothing here is verified
commissioning. Item 7 (nRF5340) and Package F are out of scope.

## Summary

| Item | What | Verdict |
|---|---|---|
| 1 | CHIP server + onboarding + dynamic endpoints | **PASS** (not commissioning - no controller exists) |
| 2 | MPSL sitting with `CONFIG_BT=y` | **PASS**, after fixing a real defect that was hiding the evidence |
| 3 | Occupancy end-to-end (HR) | **PARTIAL** - WORN and AT_REST proven; ZONE2+ not directly triggered |
| 4 | Trainer / `0x30` energy, coast dwell | **PARTIAL / NOT VERIFIABLE** - a real ambient device won every acquisition race |
| 5 | Common pages from a non-environmental channel | **PASS** |
| 6 | Coexistence gate, MED and SED | **PASS** (gate/frame health; not a formal `ant_ab.py` loss-percentage gate) |

## The room condition that shaped items 3, 4 and 6

**A real ANT+ trainer (device #52233) is on the air continuously**, broadcasting
as a bicycle-power meter (devtype `0x0B`) and fitness equipment (devtype `0x11`)
- `ab_gates.toml`'s own header already names this exact device number. It is not
something this bench controls or can turn off, and **it wins every acquisition
race against a simulated master on the same device type**. That is why items 3
and 4 are PARTIAL. On heart rate (`0x78`) it is not present and acquisition
worked, just slower than the ANT default 25 s search window under the added
self-channel contention: 130-210 s across three independent captures.

## Bench-only overrides used

All of these are `-D` flags on the `west build` command line for one-off
verification images. **None of them are in `matter.conf` or any other
committed `.conf` file** - a normal build of this arm carries none of them,
exactly the same posture `CONFIG_RADIANT_SWEEP_DEBUG` already has in
`build_p4.ps1` ("a property of THIS sitting, not of the images").

| Symbol | Why | Confirmed off by default |
|---|---|---|
| `CONFIG_ANT_DONGLE_PAIRING_WINDOW_AT_BOOT=y` | Binding an ANT+ sensor requires a physical press of `sw0` (`self_channels.c`'s opt-in rule, section 10.1 rule 1) and this session has no hands on the bench. This symbol is the project's own documented bench instrument for exactly this gap - see its Kconfig help ("A bench instrument, not a feature... off by default, selected by no shipping image, and it logs a warning naming this symbol"). | Confirmed: no `default y` in `apps/dongle_thread/Kconfig`, absent from `m_g_med`/`m_g_sed`'s `.config`, and every capture that has it on prints `BENCH BUILD: opening the pairing window at boot without a button press` in `LOG_WRN`, so a capture can never be mistaken for a normal build. |
| `CONFIG_ANT_DONGLE_PAIRING_WINDOW_S=600` | The 60 s default window is shorter than the acquisition delays this bench's ambient interference produced. 600 s is the Kconfig's own stated ceiling (`range 5 600`). | Same file, same posture - a value, not a new capability. |
| `CONFIG_ANT_DONGLE_SELF_CHANNELS=4` (one build only, `m_g_med_pair4`) | With the shipped value of 2, both self-channels got captured by the same real ambient device and no channel was left free to search heart rate at all for the rest of that boot. 4 channels leaves at least one dedicated to any one profile family even after the ambient device claims two of its own device types. | Not touched in `m_g_med`/`m_g_sed`/`m_g_med_loginf`, which kept the shipped value of 2 (confirmed in their `.config`). |
| `CONFIG_RADIANT_LOG_LEVEL_INF=y` | Fixes item 2's blocker - see below. Raises radiant's own log verbosity; it does not change behaviour. | Confirmed absent from `m_g_med`/`m_g_sed` (which read back `CONFIG_RADIANT_LOG_LEVEL=1`, i.e. errors only). |

One line is kept in the tree from this sitting:
`apps/dongle_thread/src/matter/ant/ant_bridge.cpp`'s `FlushHandler()` logs each
dirty CHIP attribute write immediately before `NotifyUpdateState()`, because
nothing on the path from a decoded ANT+ sample to a CHIP attribute write
printed anything at all before it. It fires only on a dirty attribute at the
1 Hz flush pace, on the order of one line every few seconds, not per message.

## Item 1 - CHIP server, onboarding, dynamic endpoints

**PASS**, read off the log console with zero external Matter tooling: the
server starts (`[SVR]Server Listening...`, `matter: ANT bridge ready (16
endpoints max, aggregator on endpoint 1)`); the onboarding payload
(`SetupQRCode`, manual pairing code) is logged unprompted on every boot by
`PrintOnboardingCodes()`, called unconditionally from
`Nrf::Matter::StartServer()` - nothing here has to call it and nothing here
could accidentally suppress it; and dynamic endpoints were observed in both
forms, restored from persistent storage under the same stable identity
(`ant_bridge.cpp`'s `StableKey()` hash of devnum/devtype/trans) and freshly
created when a new bound source posts its first sample. No controller ever
scanned that QR code or read that endpoint - server-side behaviour only.

## Item 2 - the MPSL sitting with `CONFIG_BT=y`

`.config` confirms `CONFIG_BT=y` on every Matter-arm build, read from the
generated file, never the log.

**A real defect blocked the measurement.** `.config` reads
`CONFIG_RADIANT_LOG_LEVEL=1` (errors only) on the Matter arm while
`CONFIG_CHIP_APP_LOG_LEVEL=4` (debug) is set at the same time. The source is
connectedhomeip's own Kconfig, not this repository's -
`modules/lib/matter/config/nrfconnect/chip-module/Kconfig.defaults` sets
`CHIP_APP_LOG_LEVEL default 4` and `LOG_DEFAULT_LEVEL default 1`. Every module
in `radiant/` registers at `CONFIG_RADIANT_LOG_LEVEL`, which defaults to
"follow the global default", so all of them go silent on this arm - not just
`LOG_INF` but `LOG_WRN` too (level 2, also below the level-1 threshold).
`CONFIG_RADIANT_SWEEP_DEBUG=y` faithfully compiles the gate dump in and
schedules it every second, forever, and CHIP's Kconfig silently pulls the rug
out from under its log LEVEL with no warning anywhere. **This has been true of
every Matter-arm capture in this project to date.**

Fixed for this sitting with `-DCONFIG_RADIANT_LOG_LEVEL_INF=y` on the build
command line; `-DCONFIG_RADIANT_LOG_LEVEL=3` fails the Kconfig configure step
outright, because `RADIANT_LOG_LEVEL` is choice-derived and not directly
assignable (the same pattern `radiant/tests/gate/prj.conf` uses as
`CONFIG_RADIANT_LOG_LEVEL_WRN=y`). Not applied to `matter.conf`, since whether
a shipping image carries radiant's diagnostics at WRN or INF is a product
trade-off; the minimum follow-up is `CONFIG_RADIANT_LOG_LEVEL_WRN=y` there so
radiant's warnings are never silently dropped.

**The measurement, once visible** (79 s, self-channels open and searching, no
sensor bound, CHIP/BLE/OpenThread all up): 80 `gate: acq=` lines, strictly
1 Hz, no gap > 2 s anywhere, ending at

```
gate: acq=1201 in_grant=345/345 placed=1202 granted=1201 blocked=0 cancel=0 eagain=344 near=0 long=0
      | den pend=0 anch=0 dist=0 degen=0 nosess=0 owed=0
      | sent grant=2 dead=0 blk=0 supp=1198 | end=1201 (norm=1056 rel=142 idle=0 late=0)
      | bad over=0 inval=0 unk=0 inl=0 | req=4888/+124849 lead=124166/24014 len=4888/4888
```

**Verdict: PASS.** `blocked=0`, `bad` all zero and the `den` block all zero
throughout - the fault-free shape `docs/p4-coexistence-bugs.md` records.
`CONFIG_BT=y` has not reopened the arbitration problem.

## Item 3 - occupancy end-to-end (heart rate)

No real HR strap: `tools/ant_sim.py`'s heart-rate profile via the ANT USB-m
stick, bound through the bench-only pairing window. The RF link was verified
before blaming acquisition - a PINNED (non-wildcard) receive channel via
`tools/ant_verify.py --port COM8` against the same simulated strap measured
**0.02% loss at -34 dBm over 30 s**, so whatever slows `self_channels`'
wildcard acquisition is not signal strength.

**WORN and AT_REST: PASS.** Device #501 bound as source 1 after ~130 s (three
of `m_g_med_pair4`'s four channels ended up tracking the real trainer's two
device types; the fourth rotated to heart rate). Both booleans reached
dedicated Matter endpoints (`devtype 0x0107 cluster 0x0406`) and their CHIP
attributes toggled in near-lockstep for the rest of the capture, driven by
#501's real accumulator data (constant 70 bpm, below the 82 bpm rest threshold,
so WORN and AT_REST correlate as expected). The toggling period of 2-15 s is
consistent with real intermittent reception on this contended bench:
`ACTIVITY_CLEAR_US` is 20 s, so every observed gap is inside the latch budget.

**ZONE2+: not directly triggered.** A 215 s script (170 s @ 70 bpm, 30 s @ 150
bpm, 15 s @ 70 bpm) bound at ~200-210 s in, leaving too little 150 bpm data
post-binding for `ZONE_DWELL_US` (3 s) to close. `eval_zone()`
(`radiant_rules.c`) is the SAME function that produced the AT_REST result -
ZONE2+ differs only in the threshold against the identical `z->hr_bpm`
(`>= 134` instead of `< 82`), with the identical 3 s dwell - so it is
code-path-identical to what was proven, but was not observed firing.
**PARTIAL** for a bench-timing reason, not a code reason.

## Item 4 - trainer / `0x30` energy, coast dwell

No real trainer: `tools/ant_sim.py`'s power profile. Two attempts (80 s, then
396 s with a long lead and repeated coast cycles) both failed to bind. Across
~400 s the self-channel log shows **six separate `bound device 52233` events**
on devtype `0x0B` or `0x11` (channels 4, 6 and 7 independently acquired the
real trainer as they rotated) and **zero** `bound device 502` events, despite
the simulator confirming 0 fallback and all 1598 messages paced by `EVENT_TX`.

**Verified by code inspection, not measurement:** `radiant_rules.c`'s
`eval_activity()` for the ACTIVE slot keys on `RADIANT_POWER_FIELD_ENERGY` (the
0x30 integral, `inst_w x dt`, `radiant_power_adapter.c`) advancing, not on
instantaneous power; `ACTIVITY_ASSERT_US` (2 s) and `ACTIVITY_CLEAR_US` (20 s)
are the same dwell constants and the same code path item 3 exercised. **Not
verified:** whether a real coast under 20 s leaves "bike in use" asserted and a
coast over 20 s clears it. The ambient trainer's own energy field did reach
Matter attributes repeatedly, so the pipeline demonstrably carries real
energy-derived booleans to CHIP - but with no ground truth for when that
trainer was being pedalled, that is not the controlled 20 s-boundary test.

**PARTIAL / NOT VERIFIABLE ON THIS BENCH:** an environmental limitation, not a
firmware defect, and not one a longer test would fix - the race was lost six to
zero, not narrowly. Do not read item 3's PASS as implying item 4 would pass
given more time; slow-but-possible and never-once are different problems.

## Item 5 - common pages from a non-environmental channel

Page 84 was injected into a heart-rate (`0x78`) master's transmit rotation by a
scratch script built on `tools/ant_sim.py`'s own exported machinery, following
`decode_common_84`'s wire layout from `tools/ant_pages.py`
(`[0] 0x54  [1] 0xFF  [2] subpage1  [3] subpage2  [4:6] field1 LE u16
[6:8] field2 LE u16`). Heart rate rather than the plan's bicycle-power example
because the real ambient trainer occupies `0x0B` and wins every race there; the
devtype is not load-bearing to the claim, since common pages 0x40-0x5D decode
regardless of devtype per Table 4-1.

**PASS.** Device #779 bound in 15 s (no ambient competition on `0x78`). Both
subfields decoded and reached dedicated Matter endpoints - field 33 ->
devtype `0x0302` cluster `0x0402` (Temperature), field 35 -> devtype `0x0307`
cluster `0x0405` (Humidity) - with live attribute writes tracking the injected
values (Temperature MeasuredValue 2170 -> 2235 -> 2290 -> 2350 in 0.01 degC;
Humidity 4280 -> 4400 -> 4540 -> 4680 -> 4840 -> 4960 -> 5080 in 0.01 %). The
injector steps its raw wire value by a small fixed amount every ~4 s and the
writes jump by roughly 3x that, which is what the deadbands (0.5 degC, 1 %RH -
`radiant_matter.c`'s own table) should do: suppress the small steps, publish
once the accumulated change crosses the threshold.

## Item 6 - coexistence gate, MED and SED

**Not a formal `tools/ant_ab.py` JSON-schema gate run.** `ab_gates.toml`'s own
header is explicit that no A/B/A sitting on this bench today is valid (the
room's repeat spread, 3+ pp, is roughly nine times the 0.35 pp gate the tool
enforces). This is a gate/frame health capture instead, on the Matter-arm build
(`CONFIG_BT=y`, `CONFIG_CHIP=y`), using the `power` profile per section 7.4.1's
own recipe note (not heart rate, whose event counter does not step per
message). **No live OpenThread peer** - every run is against the DUT's own
detached MED/SED role, the condition the placement-lead work calls the worst
case.

Loaded runs, `power` profile, device #601, ~220 s each, two flashes per role:

| | MED run 1 | MED run 2 | SED run 1 | SED run 2 |
|---|---|---|---|---|
| boots | 1 | 1 | 1 | 1 |
| fault indicators (ASSERT/FATAL/HARD FAULT/reset 0x100) | 0 | 0 | 0 | 0 |
| `blocked` (final) | 7 | 7 | 5 | 4 |
| `bad over/inval/unk` (final) | 0/0/0 | 0/0/0 | 0/0/0 | 0/0/0 |
| `sw: rx=` (frames in tracked windows) | 1045 | 1106 | 1126 | 1254 |
| `sw: n=` (tracked windows opened) | 1384 | 1418 | 1375 | 1434 |

Idle baselines: MED 79 s `blocked=0`, SED 90 s `blocked=6`, `bad` zero and 0
faults in both. `blocked` reached its value early and never grew again for the
rest of either 220 s MED run (checked mid-run) - a small, bounded,
non-pathological count, not the runaway-refusal shape of bugs 25 and 26.

**Verdict: PASS** on gate and frame health for both MED and SED. No loss
percentage is quoted, deliberately. The `blocked` counts across the four loaded
runs (7, 7, 5, 4) and the two idle runs (0, 6) are the closest thing to a
same-image-repeat variance figure this bench can honestly offer - small,
single-digit, and not trending with load or with MED-vs-SED.
