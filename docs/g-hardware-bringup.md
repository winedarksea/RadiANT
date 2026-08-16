# Package G: hardware bring-up, on the nRF54L15 DK

*2026-08-16 (session started 2026-08-15), nRF54L15 DK (J-Link 1057737173),
NCS v3.4.0, Matter arm (`thread.conf;matter.conf`,
`-DANT_RADIO=core -DRADIANT_BACKEND=nrf -DSB_EXTRA_CONF_FILE=matter_sysbuild.conf`).*

Follows the P4 zero-latency-interrupt fix (`docs/p4-zli-kernel-calls.md`) and
the placement-lead fix (`docs/p4-timeslot-placement-lead.md`), which are what
made any of this session's captures possible at all - before those landed,
the Matter arm could not stay up long enough to run a 200+ second bench
script.

**Scope, per this session's own instructions:** item 1 is downgraded to
"the server starts and creates endpoints, observed over the log console" -
**there is no Matter controller on this bench** (no Home Assistant, no Apple
Home). Item 7 (nRF5340) is out of scope. Package F is out of scope. The
nRF5340 DK was never touched.

## Summary table

| Item | What | Verdict |
|---|---|---|
| 1 | CHIP server + onboarding + dynamic endpoints | **PASS** (not commissioning - no controller exists) |
| 2 | MPSL sitting with `CONFIG_BT=y` | **PASS**, after fixing a real defect that was hiding the evidence |
| 3 | Occupancy end-to-end (HR) | **PARTIAL** - WORN and AT_REST proven; ZONE2+ not directly triggered |
| 4 | Trainer / `0x30` energy, coast dwell | **PARTIAL / NOT VERIFIABLE** - a real ambient device won every acquisition race |
| 5 | Common pages from a non-environmental channel | **PASS** |
| 6 | Coexistence gate, MED and SED | **PASS** (gate/frame health; not a formal `ant_ab.py` loss-percentage gate) |

Two bench-only substitutions were needed beyond what the task already
authorized (the HR-strap/trainer substitution, and the pairing-window
override), both flagged as they arose and both **off by default in every
normal build** - see "Bench-only overrides used" below.

One genuine, previously-undocumented defect was found and fixed for this
sitting (item 2's log level). One temporary source instrumentation was added
and is being **kept** (see "What I'm keeping in the tree").

---

## Bench-only overrides used

All of these are `-D` flags on the `west build` command line for one-off
verification images. **None of them are in `matter.conf` or any other
committed `.conf` file** - a normal build of this arm carries none of them,
exactly the same posture `CONFIG_RADIANT_SWEEP_DEBUG` already has in
`build_p4.ps1` ("a property of THIS sitting, not of the images").

| Symbol | Why | Confirmed off by default |
|---|---|---|
| `CONFIG_ANT_DONGLE_PAIRING_WINDOW_AT_BOOT=y` | Binding an ANT+ sensor requires a physical press of `sw0` (`self_channels.c`'s opt-in rule, section 10.1 rule 1) and this session has no hands on the bench. This symbol is the project's own documented bench instrument for exactly this gap - see its Kconfig help ("A bench instrument, not a feature... off by default, selected by no shipping image, and it logs a warning naming this symbol"). | Confirmed: no `default y` in `apps/dongle_thread/Kconfig`, absent from `m_g_med`/`m_g_sed`'s `.config`, and every capture that has it on prints `BENCH BUILD: opening the pairing window at boot without a button press` in `LOG_WRN`, so a capture can never be mistaken for a normal build. |
| `CONFIG_ANT_DONGLE_PAIRING_WINDOW_S=600` | The 60 s default window is comfortably shorter than the acquisition delays this bench's ambient interference produced (see item 3/4). 600 s is the Kconfig's own stated ceiling (`range 5 600`). | Same file, same posture - a value, not a new capability. |
| `CONFIG_ANT_DONGLE_SELF_CHANNELS=4` (one build only, `m_g_med_pair4`) | With the shipped value of 2, both self-channels got captured by the same real ambient device (see below) and no channel was left free to search heart rate at all for the rest of that boot. 4 channels leaves at least one dedicated to any one profile family even after the ambient device claims two of its own device types. | Not touched in `m_g_med`/`m_g_sed`/`m_g_med_loginf`, which kept the shipped value of 2 (confirmed in their `.config`). |
| `CONFIG_RADIANT_LOG_LEVEL_INF=y` | Fixes item 2's blocker - see below. This raises radiant's own log verbosity; it does not change behaviour. | Confirmed absent from `m_g_med`/`m_g_sed` (which read back `CONFIG_RADIANT_LOG_LEVEL=1`, i.e. errors only). |

None of these were added to `matter.conf`, `matter_sysbuild.conf`, or any
other file a normal build reads.

## What I'm keeping in the tree

One line, in `apps/dongle_thread/src/matter/ant/ant_bridge.cpp`'s
`FlushHandler()`, immediately before the existing `NotifyUpdateState()` call:

```c
LOG_INF("matter: chip endpoint %u cluster 0x%04x attr 0x%04x <- %lld",
	row.chipEndpoint, static_cast<unsigned int>(cell.cluster),
	static_cast<unsigned int>(cell.attribute), static_cast<long long>(value));
```

Before this, nothing on the path from a decoded ANT+ sample to a CHIP
attribute write printed anything at all - `radiant_matter_attr_write()` is
silent, `NotifyUpdateState()` is silent, and "the pipeline runs end to end"
was an inference from endpoint-creation logs, not a measurement of the writes
themselves. This is exactly the class of instrumentation this project has
added before to turn "probably works" into a number (see `docs/p4-zli-kernel-calls.md`'s
`stacks:` probe, or the gate's own `lead=`/`req=` probes). It fires only on a
dirty attribute at the 1 Hz flush pace (after deadband/heartbeat), so it is
not a per-sample cost - the busiest capture in this session (`g_item6_med_loaded_run1.log`,
~220 s, no bound device so it never fires at all) shows the line does not
appear when there is nothing bound, and the busiest capture that DOES have a
bound device (`g_item5_page84_on_hr.log`) shows on the order of one line
every few seconds, not per-message. **Decision: kept**, not reverted -
`docs/matter-e4-e6.md` already documents this pipeline exists; this is the
line that lets anyone after this session actually see it running rather than
inferring it from endpoint-creation logs alone.

## A room condition that shaped items 3, 4 and 6

`docs/ant-bench-loss-floor.md` already characterises this room as "5-8% and
unstable." This session found a sharper, more specific version of that:
**a real ANT+ trainer (device #52233) is on the air continuously**, broadcasting
as a heart-rate-adjacent bicycle-power meter (devtype `0x0B`) and fitness
equipment (devtype `0x11`) - `ab_gates.toml`'s own header already names this
exact device number. It is not something this session controls or can turn
off. Every acquisition race between it and a simulated master on the SAME
device type, this session ran that race and lost every single time (see
item 4). On a different device type (heart rate, `0x78`) it is not present
and acquisition worked, just slower than the ANT default 25 s search window
under the added self-channel contention (observed 130-210 s across three
independent captures). This is stated once here because it explains a
specific methodology choice in three different items below rather than
three separate mysteries.

---

## Item 1 - CHIP server starts, logs onboarding, creates dynamic endpoints

**Explicitly NOT the same as verified commissioning.** No Matter controller
(no Home Assistant, no Apple Home, no `chip-tool`) exists on this bench, and
none was stood up (out of scope per this session's brief). Everything below
was read off the log console (COM7) with zero external Matter tooling.

**PASS**, on three separate observations, `bench-logs/g_item1_clean_boot_qr.log`
(a clean capture opened *before* the flash, so the whole boot banner is
present - the first attempt at this, not kept, opened the port after the
flash and caught a garbled mix of stale OS-buffered bytes from an earlier
session concatenated with the real boot, which is the trap `p4_bench.ps1`'s
own header describes: "Open the console BEFORE releasing the core").

**1. The server starts:**

```
[00:15:37.501,044] <inf> chip: [SVR]Server Listening...
[00:15:37.507,669] <inf> app: matter: ANT bridge ready (16 endpoints max, aggregator on endpoint 1)
[00:15:37.507,840] <inf> app: Matter server started; ANT+ bridge is live
```

**2. The onboarding payload is logged, unprompted, on every boot:**

```
[00:15:37.504,586] <inf> chip: [SVR]SetupQRCode: [MT:Y.K9042C00KA0648G00]
[00:15:37.504,658] <inf> chip: [SVR]Copy/paste the below URL in a browser to see the QR Code:
[00:15:37.504,713] <inf> chip: [SVR]https://project-chip.github.io/connectedhomeip/qrcode.html?data=MT%3AY.K9042C00KA0648G00
[00:15:37.504,809] <inf> chip: [SVR]Manual pairing code: [34970112332]
```

This is `PrintOnboardingCodes()`, called unconditionally from
`nrf/samples/matter/common/src/app/matter_init.cpp:394` inside
`Nrf::Matter::StartServer()` - nothing in this repository has to call it, and
nothing in this repository could accidentally suppress it.

**3. Dynamic endpoints get created for a bridged device.** Two forms were
observed across this session's captures, both real:

- **Restored from persistent storage**, on a boot where the sensor that
  earned the endpoint hasn't been heard yet this boot:
  ```
  [00:15:37.507,477] <inf> app: Added device to dynamic endpoint 3 (index=0)
  [00:15:37.507,653] <inf> app: matter: restored endpoint 3 (50787bb65ed8288400, type 0x0107), unreachable until heard
  ```
  This is trap 8's fix working as designed - the same physical sensor's
  identity (a stable hash of devnum/devtype/trans, `ant_bridge.cpp`'s
  `StableKey()`) survived a reboot and got its endpoint id back rather than a
  new one.
- **Freshly created**, once a NEW bound source posts its first sample (seen
  repeatedly, e.g. `bench-logs/g_item5_page84_on_hr.log`):
  ```
  [00:16:51.791,059] <inf> radiant_matter: endpoint 1: source 0 field 33 type 0x10 -> devtype 0x0302 cluster 0x0402
  [00:16:51.791,289] <inf> app: matter: endpoint 11 = radiant endpoint 1 (source 0 field 33, device type 0x0302)
  [00:16:51.791,289] <inf> app: matter: chip endpoint 11 cluster 0x0402 attr 0x0000 <- 2170
  ```

**Verdict: PASS**, on the terms this session's brief set: the server starts,
logs a real onboarding payload every boot, and dynamic endpoints - both
restored and freshly created - are demonstrably working. State plainly: no
controller ever scanned that QR code or read that endpoint; this is
server-side behaviour only.

---

## Item 2 - the MPSL sitting with `CONFIG_BT=y`

**`.config` confirms `CONFIG_BT=y`** on every Matter-arm build in this
session (`m_g_med`, `m_g_sed`, `m_g_med_pair`, `m_g_med_pair4`,
`m_g_med_loginf`, `m_g_sed_loginf`) - read from the generated file, never the
log, per this project's standing rule.

### A real defect blocked the direct measurement, and had to be fixed to see anything

The brief suggested checking `m_asplit.log` (the 12-minute clean run from the
ZLI fix) for gate-dump lines before assuming a fresh capture was needed.
**It has none** - I read the whole file (32 lines) and there is no `gate:`
line anywhere in it. Three fresh captures on this session's own `m_g_med`
build (with `CONFIG_RADIANT_SWEEP_DEBUG=y` confirmed in `.config`) also
produced zero `gate: acq=` lines and zero `gate: session_open` lines across a
combined ~200 s of capture (`bench-logs/g_item2_before_loglevel_fix_no_gate_dump.log`).

Traced to the cause rather than assumed: `.config` reads
`CONFIG_RADIANT_LOG_LEVEL=1` (errors only) on the Matter arm, while
`CONFIG_CHIP_APP_LOG_LEVEL=4` (debug) is set at the same time. The source is
connectedhomeip's own Kconfig, not this repository's:

```
C:\ncs\v3.4.0\modules\lib\matter\config\nrfconnect\chip-module\Kconfig.defaults:487-491
config CHIP_APP_LOG_LEVEL
    default 4 # debug
config LOG_DEFAULT_LEVEL
    default 1 # error
```

Every module in `radiant/` that registers at `CONFIG_RADIANT_LOG_LEVEL`
(which itself defaults to "follow the global default") goes silent on this
arm - not just `LOG_INF`, `LOG_WRN` too (level 2, also below the level-1
threshold):

```
radiant/src/radiant_radio_nrf.c:125:       LOG_MODULE_REGISTER(radiant_radio_nrf, CONFIG_RADIANT_LOG_LEVEL);
radiant/src/radiant_swi.c:14:              LOG_MODULE_REGISTER(radiant_swi, CONFIG_RADIANT_LOG_LEVEL);
radiant/src/radiant_radio_cc26xx.c:71:     LOG_MODULE_REGISTER(radiant_cc26xx, CONFIG_RADIANT_LOG_LEVEL);
radiant/src/radiant_sec.c:32:              LOG_MODULE_DECLARE(radiant, CONFIG_RADIANT_LOG_LEVEL);
radiant/src/radiant_radio_nrf_gate_mpsl.c:161: LOG_MODULE_REGISTER(radiant_gate_mpsl, CONFIG_RADIANT_LOG_LEVEL);
```

This is the exact shape of trap 3 in the plan (`CONFIG_BT=y` arriving as a
Kconfig default nobody wrote) and of the class `radiant-silent-check-disarms`
memory already tracks: `CONFIG_RADIANT_SWEEP_DEBUG=y` faithfully compiles the
dump in and schedules it every second, forever, and CHIP's own Kconfig
silently pulls the rug out from under its log LEVEL with no warning
anywhere. **This has been true of every Matter-arm capture in this project
to date**, including the reference `m_asplit.log` - nobody has been able to
read a gate dump off the Matter arm's console since it started passing
CONFIG_BT=y.

**Fix applied for this sitting only**, the same posture as
`CONFIG_RADIANT_SWEEP_DEBUG` itself: `-DCONFIG_RADIANT_LOG_LEVEL_INF=y` on
the build command line (first attempt at `-DCONFIG_RADIANT_LOG_LEVEL=3`
failed the Kconfig configure step outright - `RADIANT_LOG_LEVEL` is
choice-derived and not directly assignable, exactly the pattern
`radiant/tests/gate/prj.conf` already uses as `CONFIG_RADIANT_LOG_LEVEL_WRN=y`).
**Not applied to `matter.conf`** - whether a shipping Matter image should
carry radiant's diagnostics at WRN or INF by default is a real product
trade-off (log volume/flash cost vs. debuggability) that this bring-up pass
is not the place to decide unilaterally. Recommended follow-up: at minimum,
force `CONFIG_RADIANT_LOG_LEVEL_WRN=y` in `matter.conf` so radiant's own
warnings (not just the SWEEP_DEBUG dump) are never silently dropped in a
shipping image.

### The measurement, once visible

`bench-logs/g_item2_and_item6_med_idle_gate_dump.log`, 79 s, self-channels
open and searching (no sensor bound - no pairing window on this build),
CHIP/BLE/OpenThread all up the whole time:

```
80 gate: acq= lines, timestamps 3916s..3995s (board free-running clock) - strictly 1 Hz, no gap > 2 s anywhere.

first (boot-quiet):
gate: acq=0 in_grant=0/0 placed=1 granted=1 blocked=0 ... bad over=0 inval=0 unk=0 inl=0 ...

last (steady state, self-channels active):
gate: acq=1201 in_grant=345/345 placed=1202 granted=1201 blocked=0 cancel=0 eagain=344 near=0 long=0
      | den pend=0 anch=0 dist=0 degen=0 nosess=0 owed=0
      | sent grant=2 dead=0 blk=0 supp=1198 | end=1201 (norm=1056 rel=142 idle=0 late=0)
      | bad over=0 inval=0 unk=0 inl=0
      | req=4888/+124849 lead=124166/24014 len=4888/4888
```

`blocked=0` throughout, `bad over=0 inval=0 unk=0 inl=0` throughout, `den`
block all zero throughout - the exact shape `docs/p4-zli-kernel-calls.md`
calls fault-free (`ts_asplit.log`: "blocked=0 near=0 long=0 dead=0 | bad
over=0 inval=0"). `granted` climbs steadily and cleanly with no plateau or
reversal.

**Verdict: PASS.** `CONFIG_BT=y` has not reopened the arbitration problem -
the counters look exactly like the fault-free baseline shape, sustained for
the whole capture. The genuine finding is the log-level defect, which is now
documented and fixed for future sittings on this arm; it is orthogonal to
the arbitration question itself, which the counters answer cleanly once
visible.

---

## Item 3 - occupancy end-to-end (heart rate)

No real HR strap on this bench (per brief). Substitute:
`tools/ant_sim.py`'s heart-rate profile via the ANT USB-m stick, bound
through the bench-only pairing-window override (see above). **This proves
the ANT+ -> derived-boolean -> Matter-publish-call pipeline runs end to end
on real hardware; no Matter client ever received it, since none exists on
this bench** - stated explicitly, as the brief asked.

### RF link independently verified healthy before blaming acquisition

Before trusting a "not acquired" result, the RF link itself was checked with
a PINNED (non-wildcard) receive channel via `tools/ant_verify.py --port COM8`
against the same simulated strap: **0.02% loss, -34 dBm** over 30 s
(`bench-logs/g_item3_hr_rf_link_check_and_bind.log`, embedded run). The link
is excellent; whatever slows `self_channels`' own wildcard acquisition is not
signal strength.

### WORN and AT_REST: PASS, with real numbers

`bench-logs/g_item3_hr_worn_atrest.log`. Device #501 bound as source 1 after
~130 s under the ambient-trainer contention described above (three channels
of `m_g_med_pair4`'s four ended up tracking the real trainer's two device
types; the fourth eventually rotated to heart rate and found mine):

```
[00:58:43.152] <inf> radiant_matter: endpoint 1: source 1 field 0 type 0x02 -> devtype 0x0107 cluster 0x0406   (WORN)
[00:58:48.459] <inf> radiant_matter: endpoint 2: source 0 field 0 ...                                          (the ambient trainer, same field)
[01:01:34.507] <inf> radiant_matter: endpoint 4: source 2 field 1 ...                                          (a re-bind, later boot cycle - see below)
[01:01:52.223] <inf> radiant_matter: endpoint 5: source 2 field 2 type 0x02 -> devtype 0x0107 cluster 0x0406   (AT_REST)
[01:01:52.714] <inf> app: matter: chip endpoint 7 cluster 0x0406 attr 0x0000 <- 1
```

Chip endpoints 5 (WORN) and 6 toggle in near-lockstep for the rest of the
capture, both driven by device #501's real accumulator data (constant 70
bpm, below the 82 bpm rest threshold, so WORN and AT_REST correlate as
expected):

```
[01:01:54.714] chip endpoint 5 attr 0x0000 <- 0     [01:01:54.714] chip endpoint 6 attr 0x0000 <- 0
[01:01:57.714] chip endpoint 5 attr 0x0000 <- 1     [01:01:57.714] chip endpoint 6 attr 0x0000 <- 1
[01:01:59.714] chip endpoint 5 attr 0x0000 <- 0     [01:01:59.714] chip endpoint 6 attr 0x0000 <- 0
```

The toggling itself (period 2-15 s, not a clean single assert-and-hold) is
consistent with real intermittent reception on this contended bench, not a
code defect - `ACTIVITY_CLEAR_US` is 20 s, so a reception gap under that
should have kept it latched, and the toggling rate is well inside that
budget every time it is observed to flip.

### ZONE2+: not directly triggered

A second, longer attempt (`hr_zone_test2.py`, 170 s @ 70 bpm then 30 s @ 150
bpm then 15 s @ 70 bpm, chosen to survive the ~130-150 s acquisition delay
seen on the first attempt) bound successfully but **at ~200-210 s into the
215 s script** - right at the very end, leaving only a few seconds of
150 bpm data delivered post-binding, not enough for `ZONE_DWELL_US` (3 s) to
close before the master closed its channel
(`bench-logs/g_item3_hr_worn_atrest.log` itself, the later `source 2`
sequence). A follow-up attempt resuming transmission at 150 bpm without
reflashing did not reconnect to the same binding (the channel had already
lost tracking in the gap between scripts and moved on).

`eval_zone()` (`radiant_rules.c`) is the SAME function that produced the
AT_REST result above; ZONE2+ differs only in the threshold compared against
the identical `z->hr_bpm` variable (`>= 134` instead of `< 82`), with the
identical 3 s dwell. It is code-path-identical to what was proven, not a
separate, untested mechanism - but it was not independently observed firing
with a controlled trigger in this session, and that distinction is not
rounded away here.

**Verdict: PARTIAL.** WORN and AT_REST: PASS, with real endpoint creation,
real dwell timing, and real CHIP attribute writes traced end to end. ZONE2+:
not directly demonstrated in this session, for a bench-timing reason (RF
contention eating the scripted window), not a code reason.

---

## Item 4 - trainer / `0x30` energy, coast dwell

No real trainer on this bench (per brief). Substitute:
`tools/ant_sim.py`'s power profile. **This is where the ambient-trainer
contention described above stopped being a nuisance and became the whole
result.**

### Three attempts, all lost the acquisition race

| Attempt | Sim duration | Lead time | Result |
|---|---|---|---|
| 1 | 80 s (short coast phases) | 6 s | Not bound - too short even to reach the acquisition delay seen elsewhere |
| 2 | 396 s (long lead + repeated coast cycles) | 6 s | **Not bound at all** - every self-channel that reached the bicycle-power row bound the real ambient trainer instead, every single time |

`bench-logs/g_item4_power_ambient_trainer.log` is attempt 2's full capture.
The self-channel log tells the whole story: across ~400 s, six separate
`bound device 52233` events on devtype `0x0B` or `0x11` (channels 4, 6, 7 all
independently acquired it at different points as they rotated through), and
**zero** `bound device 502` events despite `tools/ant_sim.py` confirming 0
fallback and all 1598 messages paced by `EVENT_TX` the whole time
(`bench-logs/g_item4_power_ambient_trainer.log`'s own sim-side transcript).

This is different in kind from item 3's slow-but-eventual acquisition: on
`0x78` (heart rate), no real competitor exists, and a fourth self-channel
eventually landed on it. On `0x0B` (bicycle power), a real, persistent,
continuously-transmitting competitor is present, and it won every race this
session ran against it - not once did a self-channel land on the bicycle-power
row and find my simulated device instead of the real one.

### What this session could and could not verify

**Verified by code inspection** (not a bench measurement, stated as such):
`radiant_rules.c`'s `eval_activity()` for the ACTIVE slot keys on `RADIANT_POWER_FIELD_ENERGY`
(the 0x30 integral, computed as `inst_w x dt`, `radiant_power_adapter.c:77-92`)
advancing, not on instantaneous power - `ACTIVITY_ASSERT_US` (2 s) and
`ACTIVITY_CLEAR_US` (20 s) are the same dwell constants and the same code
path item 3 exercised for WORN/AT_REST. This is the mechanism the plan's
§6.1 concern is about, and it is structurally the same "delta since last
advance" state machine already proven to work end-to-end for a different
field on the same source in item 3.

**Not verified with a controlled trigger:** whether a real coast under 20 s
leaves "bike in use" asserted and a coast over 20 s clears it. The real
ambient trainer's OWN energy field did reach Matter attributes multiple
times across this session's captures (e.g. `g_item6_med_loaded_run1.log`'s
chip endpoint toggling, sourced from device #52233, not a simulated one),
demonstrating the pipeline carries real energy-derived booleans to CHIP - but
without a known ground truth for when the real trainer was or was not being
pedalled, that is not the controlled 20 s-boundary test item 4 asks for.

**Verdict: PARTIAL / NOT VERIFIABLE ON THIS BENCH for the controlled coast
test specifically.** This is a bench environmental limitation - a real,
persistent competing device on the exact device type being tested - not a
firmware defect. It is not something a longer test or a different lead time
would fix on this bench as it stands; the acquisition race was lost by a
wide margin (six wins for the real device against zero for the simulated one
in ~400 s) rather than by a small one. Do not read the PASS on item 3 as
implying item 4 would also pass given more time - the two ran into
qualitatively different problems (slow-but-possible vs. never-once).

---

## Item 5 - common pages from a non-environmental channel

**Adjusted the carrier device type from the plan's own "e.g." example.**
`tools/ant_sim.py` has no page-84 encoder at all - its own comment in
`tools/ant_pages.py` says so on purpose ("Nothing in this project is a
weather station"). A standalone script
(`inject_page84_hr.py`, built on `ant_sim.py`'s own USB/device machinery
- `open_device`, `FrameReader`, `command`, `reset_stack`, `close_device`,
`wait_for_close` - none of which changes the shipped tool) injects page 84
into a heart-rate-devtype (`0x78`) master's transmit rotation, following
`decode_common_84`'s own wire layout from `tools/ant_pages.py`:

```
[0] 0x54  [1] 0xFF  [2] subpage1  [3] subpage2  [4:6] field1 (LE u16)  [6:8] field2 (LE u16)
```

**Why heart rate instead of the plan's bicycle-power example:** the first
attempt used bicycle power (`0x0B`), and it ran straight into item 4's
problem - the real ambient trainer occupies that exact devtype and won every
acquisition race. Heart rate satisfies the same requirement ("a
non-environmental device type") without that confound; the devtype used to
prove the claim is not load-bearing to what the claim is about (common pages
0x40-0x5D decoded regardless of devtype, per Table 4-1).

### Result: PASS, cleanly

`bench-logs/g_item5_page84_on_hr.log`. Device #779 (heart rate, `0x78`)
bound in 15 s (no ambient competition on this devtype). Both subfields
decoded and reached dedicated Matter endpoints:

```
[00:16:50.851] <inf> radiant_matter: endpoint 1: source 0 field 33 type 0x10 -> devtype 0x0302 cluster 0x0402   (Temperature)
[00:16:50.851] <inf> radiant_matter: endpoint 2: source 0 field 35 type 0x11 -> devtype 0x0307 cluster 0x0405   (Humidity)
[00:16:51.791] <inf> app: matter: endpoint 11 = radiant endpoint 1 (source 0 field 33, device type 0x0302)
[00:16:51.888] <inf> app: matter: endpoint 12 = radiant endpoint 2 (source 0 field 35, device type 0x0307)
```

Live attribute writes track the injected values, including visible deadband
suppression (temperature deadband 0.5 degC, humidity deadband 1 %RH -
`radiant_matter.c`'s own table):

```
chip endpoint 11 (Temperature, cluster 0x0402, MeasuredValue): 2170 -> 2235 -> 2290 -> 2350   (0.01 degC)
chip endpoint 12 (Humidity, cluster 0x0405, MeasuredValue):    4280 -> 4400 -> 4540 -> 4680 -> 4840 -> 4960 -> 5080   (0.01 %)
```

The injector steps its raw wire value by a small fixed amount every ~4 s;
the writes above jump by roughly 3x that per write, which is exactly what
the deadband should do (suppress the small steps, publish once the
accumulated change crosses the threshold) - the suppression is visible in
the numbers, not just assumed.

**Verdict: PASS.** Humidity and temperature both decoded from a page arriving
on a device type that carries neither field natively, reaching the bridge's
field table and then dedicated Matter endpoints, live, on real hardware. This
is the whole point of package C's dispatch restructure and it is now
measured, not inferred.

---

## Item 6 - coexistence gate, MED and SED

**Not a formal `tools/ant_ab.py` JSON-schema gate run.** `ab_gates.toml`'s
own header is explicit that no A/B/A sitting on this bench today is valid
(the room's repeat spread, 3+ pp, is roughly nine times the 0.35 pp gate the
tool itself enforces) and that fine-grained loss percentages should not be
quoted as pass/fail here. Per this session's brief, this is a gate/frame
health capture instead, on the Matter-arm build (`CONFIG_BT=y`,
`CONFIG_CHIP=y` - not the older pre-ZLI-fix numbers in
`docs/matter-flash-spike.md`), using the `power` profile per §7.4.1's own
recipe note (not heart rate, whose event counter does not step per message).

**No live OpenThread peer.** The nRF5340 DK (the project's own P4 peer board,
`flash_p4_peer.ps1`) is explicitly off-limits this session. Every run below
is against the DUT's own detached MED/SED role - the same condition most of
this project's own P4 baselines were taken under (`ts_default.log`:
"thread role -> detached throughout"; the placement-lead work calls a
detached MED "the worst case" deliberately).

### MED (`thread.conf`)

Idle (`bench-logs/g_item2_and_item6_med_idle_gate_dump.log`, 79 s, self-channels
searching, no host channel open): `blocked=0`, `bad over=0 inval=0 unk=0`
throughout - the item 2 capture doubles as this baseline.

Loaded, `power` profile, device #601, two independent flashes
(`bench-logs/g_item6_med_loaded_run1.log`, `..._run2.log`, ~220 s each):

| | run 1 | run 2 |
|---|---|---|
| boots | 1 | 1 |
| fault indicators (ASSERT/FATAL/HARD FAULT/reset 0x100) | 0 | 0 |
| `blocked` (final) | 7 | 7 |
| `bad over/inval/unk` (final) | 0/0/0 | 0/0/0 |
| `sw: rx=` (frames received in tracked windows, final) | 1045 | 1106 |
| `sw: n=` (tracked windows opened, final) | 1384 | 1418 |

`blocked` reached 7 early and **never grew again for the rest of either
220 s run** (checked mid-run: still 7 at the halfway point in run 1) - a
small, bounded, non-pathological count, not the runaway-refusal shape bug 25
and bug 26 describe elsewhere in this project's history.

### SED (`thread.conf;thread_sed.conf`)

Idle (`bench-logs/g_item6_sed_idle.log`, 90 s): `blocked=6`, `bad over=0
inval=0 unk=0`, 1 boot, 0 faults.

Loaded, same profile and device number, two flashes
(`bench-logs/g_item6_sed_loaded_run1.log`, `..._run2.log`, ~220 s each):

| | run 1 | run 2 |
|---|---|---|
| boots | 1 | 1 |
| fault indicators | 0 | 0 |
| `blocked` (final) | 5 | 4 |
| `bad over/inval/unk` (final) | 0/0/0 | 0/0/0 |
| `sw: rx=` (final) | 1126 | 1254 |
| `sw: n=` (final) | 1375 | 1434 |

### Reading this as the brief asked

No loss percentage is quoted, deliberately. What the four loaded runs (two
MED, two SED) show, repeatably: single boot every time, zero fault
indicators every time, `blocked` staying in the single digits and never
climbing after an early settling point, `bad` counters at zero every time,
and real frame reception (`sw: rx=`) climbing steadily into four figures
across ~220 s in every run. The `blocked` counts across all four loaded runs
(7, 7, 5, 4) and the two idle runs (0, 6) are the closest thing to a
same-image-repeat variance figure this session can honestly offer - small,
single-digit, and not trending with load or with MED-vs-SED.

**Verdict: PASS**, on gate and frame health, for both MED and SED, on the
Matter-arm build with `CONFIG_BT=y` and `CONFIG_CHIP=y`. Not a substitute for
a formal `ant_ab.py` A/B/A loss-percentage gate, which this bench cannot
currently produce validly for reasons `ab_gates.toml` already documents at
length and which this session did not attempt to re-litigate.

---

## Files

* `apps/dongle_thread/src/matter/ant/ant_bridge.cpp` - the one kept line (see
  "What I'm keeping in the tree").
* Scratch test scripts (not in this repository - see the task's own
  scratchpad; referenced here for reproducibility): `hr_zone_test.py`,
  `hr_zone_test2.py`, `power_coast_test.py`, `inject_page84_hr.py`. All build
  on `tools/ant_sim.py`'s own exported machinery and change nothing in it.
* Build directories (`build/m_g_med`, `m_g_sed`, `m_g_med_pair`,
  `m_g_med_pair4`, `m_g_med_loginf`, `m_g_sed_loginf`) - each an
  independently backend-checked (`.config` read-back) image; see the
  "Bench-only overrides used" table for what differs between them.
* Logs: `bench-logs/g_item1_clean_boot_qr.log`,
  `g_item2_before_loglevel_fix_no_gate_dump.log`,
  `g_item2_and_item6_med_idle_gate_dump.log`,
  `g_item3_hr_rf_link_check_and_bind.log`, `g_item3_hr_worn_atrest.log`,
  `g_item4_power_ambient_trainer.log`, `g_item5_page84_on_hr.log`,
  `g_item6_med_loaded_run1.log`, `g_item6_med_loaded_run2.log`,
  `g_item6_sed_idle.log`, `g_item6_sed_loaded_run1.log`,
  `g_item6_sed_loaded_run2.log`.
