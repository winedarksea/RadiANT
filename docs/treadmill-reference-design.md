# The treadmill node: a fitness-equipment reference design

Checked by: `.github/workflows/build.yml`'s `build-treadmill` job (every
configuration below is a matrix row whose resulting `.config` is asserted),
`scripts/build_all.ps1`'s `$treadmillTargets` loop, and three ztest suites —
`radiant/tests/src/test_profile_fec_tx.c`, `radiant/tests/src/test_profile_sdm.c`
and `apps/treadmill/tests/pages/`. §7.1–§7.3 record results from a run on
2026-08-15; §7.4 onwards are narrative, because nothing machine-checks a
stopwatch.

---

## 1. What this is, and what it is not

`apps/treadmill` is the second manufacturer reference design in this tree,
after [`hrm-reference-design.md`](hrm-reference-design.md). **Read that one
first.** The seam, the Kconfig `choice`, the ISR-to-workqueue hop and the BLE
interval policy are all the same and are argued there at length; this document
covers what is different, and what is different is one thing:

> A chest strap is only ever asked to *report*. A treadmill is also *told* —
> over ANT+ FE-C page 51 and over the BLE FTMS control point — and the two
> commands have to agree.

That symmetry is the whole architecture. Everything else falls out of it.

**What it is.** A node with:

- **Two ANT+ masters.** FE-C (`0x11`, period 8192) is what a control-capable
  head unit pairs with; Stride Based Speed and Distance (`0x7C`, period
  **8134**) is what anything that only knows foot pods pairs with, including
  Zwift Run and most watches.
- **Two BLE services.** FTMS (`0x1826`, hand-written — neither Zephyr nor NCS
  ships it) and RSC (`0x1814`, NCS's `bt_rscs_*`).
- **One state struct** in SI units that all four read, and **one function** that
  both control paths land on.
- Optionally a **third ANT+ channel**, a heart-rate slave, whose bpm reaches
  three consumers at once.

**What it is not.**

- **Not validated against a real treadmill.** The simulator is the only
  "hardware" here. Nothing in this tree has been near a motor controller or an
  incline actuator, and `src/treadmill_source.h` is deliberately the boundary
  where that work would start.
- **Not certified, and could not be.** ANT+ certification closed on
  2025-06-30. See [ADR 0017](decisions/0017-fec-treadmill-control.md) for the
  one place that shows: §10.1.3.1 of the FE-C spec says only trainer equipment
  types "may support the controllable fitness equipment use case".
- **Not a display.** There is no display driver, no LVGL and nothing proposed.
  Phase 5 makes heart rate *available*; rendering it is a separate project.
- **Not a Thread device.** A gym's occupancy roll-up runs on a
  `apps/dongle_thread` receiving FE-C, not on the treadmill. See §8, and
  [ADR 0011](decisions/0011-never-a-border-router.md) for why the alternative is
  refused rather than deferred.

---

## 2. Build and flash

```
west build apps/treadmill -b nrf54l15dk/nrf54l15/cpuapp -p always \
  -- -DRADIANT_BACKEND=nrf
west flash
```

### The configurations

| Fragment | What it adds |
|---|---|
| *(none)* | Both ANT+ masters, the simulator, no BLE. The baseline. |
| `ble.conf` | FTMS + RSC, SMP and a bonding store |
| `hr_rx.conf` | The ANT+ heart-rate slave (layer it on top of `ble.conf`) |
| `fec_only.conf` | **One** ANT+ master. The coexistence fallback — see §7 |
| `source_custom.conf` | No source at all: the integrator's starting point |

### The one flag people get wrong

Fragments are **image-scoped** and the prefix is the basename of the
application directory:

```
west build apps/treadmill ... -- -DRADIANT_BACKEND=nrf \
  -Dtreadmill_EXTRA_CONF_FILE=ble.conf
```

Not `-DEXTRA_CONF_FILE=` (that lands in the sysbuild cache and never reaches
the image) and not a bare `-DCONFIG_x=y` (same). A wrong prefix names an image
that does not exist and is accepted **in silence**: CMake emits nothing beyond
its end-of-run "manually-specified variables were not used" warning, and the
BLE image builds green with no BLE in it. `apps/hrm_ble` shipped exactly that
after a directory rename and *measured* it — two builds, with the flag and
without, produced byte-identical `.config` files. That is why the CI job asserts
the resulting `.config` rather than merely building.

---

## 3. The source seam

`src/treadmill_source.h`. It is `hrm_sensor.h` with one addition, so read that
header's two rules first — they are restated in this one but argued in that one.

### What is the same

- **Push, not poll**, for measurements. A stride is an edge; a tachometer edge
  knows when it happened and an application tick does not.
- **One atomic store and one `k_work_submit()`** in every `report_*()` call.
  Nothing in that path touches a profile module, `treadmill_state` or the BLE
  stack, so an ISR-driven source adds no third context to modules that have no
  lock.
- **A Kconfig `choice`, never a weak symbol.** Exactly one arm compiles,
  exactly one `treadmill_source_register()` links, and the boot log names it.
  `CONFIG_TREADMILL_SOURCE_CUSTOM` compiles none of them, with no fallback.

### What is new: it is bidirectional

```c
int (*set_incline)(const struct treadmill_source *src, int32_t centi_pct);
int (*set_speed)(const struct treadmill_source *src, uint32_t mm_s);
```

**Three return values, three different things to say on the air**, and this is
the part worth getting right:

| Return | FE-C page 71 | FTMS result code |
|---|---|---|
| `0` | `PASS` | `0x01` Success |
| `-ENOTSUP` | `NOT_SUPPORTED` | `0x02` Op Code not supported |
| other negative | `FAIL` | `0x04` Operation Failed |

`-ENOTSUP` is a first-class answer and not a failure. A manual treadmill with a
fixed deck is a legitimate product: it returns `-ENOTSUP`, **and** clears FE-C
page 54's Simulation-mode bit **and** clears FTMS's Inclination Target Setting
bit. Doing one without the others is what makes a controller report the machine
as broken rather than as fixed-incline.

Returning `0` and doing nothing is the worst of the three: a controller that
sees Success and then watches page 17 report an unchanged incline concludes the
machine is faulty.

### The one call that is easy to leave out

`treadmill_source_report_state()`. It sets the FE state — `READY`, `IN_USE`,
`FINISHED` — that FE-C page 16 byte 7 carries. **That field is the entire basis
of §8's gym occupancy signal.** A source that never calls it reports `READY`
forever and every dashboard downstream reads zero.

---

## 4. The state module, and why every conversion has a test

`src/treadmill_state.h`. One struct, one owner, integer millimetres.

Five outputs want the same physical facts in different units:

| Quantity | FE-C | ANT+ SDM | BLE FTMS | BLE RSC |
|---|---|---|---|---|
| speed | 0.001 m/s | m/s nibble + 1/256 m/s | **0.01 km/h** | 1/256 m/s |
| distance | 1 m (u8, wraps) | 1 m + 1/16 m | 1 m (u24) | 1 m (u32) |
| incline | **0.01 %** | — | **0.1 %** | — |
| cadence | **strides**/min | **strides**/min, 1/16 | — | **steps**/min |
| stride length | 0.01 m | — | — | 0.01 m |

**Three denominators for one fraction, and one factor of two.** If exactly one
of the five outputs is ever wrong, it will be the cadence: one stride is two
footfalls, so RSC's number is twice FE-C's and twice SDM's. Nordic's
`rscs.h` comments its field as "1 unit = 1 stride/minute", but its own sample
(`nrf/samples/bluetooth/peripheral_rscs/src/main.c`) simulates 150–180, which is
**steps** — strides would be 75–90 and no runner has a 170 stride/min gait.
Steps is what real clients expect.

So: one conversion function per direction, all in `treadmill_state.h`, each with
a case in `apps/treadmill/tests/pages/`. Not one of them is inlined at a call
site.

**The incline pair is the second trap.** FE-C is 0.01 % and FTMS is 0.1 %, so a
missing factor of ten commands 30 % where 3 % was meant — and the machine would
report it back consistently, forever, with nothing anywhere disagreeing.

**And the grade/incline encodings are not the same encoding**, one page apart:

| | FE-C page 51 grade | FE-C page 17 incline |
|---|---|---|
| type | **unsigned** u16, biased | **two's complement** sint16 |
| scale | `Grade% = raw × 0.01 − 200.00` | 0.01 % directly |
| zero | `0x4E20` | `0x0000` |
| invalid | `0xFFFF` → *assume flat* | `0x7FFF` → *cannot report* |

`0xFFFF` read as an incline is −0.01 %; `0x7FFF` read as a grade is +127.11 %.
Each is a legal-looking reading of the other's sentinel and neither produces an
error.

### No floating point, anywhere

Every output format is an integer with a fixed denominator, and integer
millimetres divide into all of them without a rounding mode to argue about. A
treadmill's distance accumulator also runs for hours, and a float loses its last
millimetre long before it loses its first metre.

Every accumulator keeps a **carry** for the same reason: at 3333 mm/s a 100 ms
tick is 333.3 mm, and dropping the tenth loses 12 metres per hour — about one
percent of a twenty-minute run, and exactly the kind of error a bench reads as a
calibration problem rather than as a bug.

---

## 5. Porting to your board

1. **Copy an overlay.** `boards/nrf54l15dk_nrf54l15_cpuapp.overlay` is three
   lines and the important one is `chosen { radiant,radio-timer }`. On nRF52
   only TIMER3 and TIMER4 have the five compare/capture channels the backend
   needs, which is a compile-time `#error` rather than something to find on the
   air.
2. **Write your source.** Start from `src/treadmill_source_custom.c`, which
   contains the instructions, and read `src/treadmill_sim.c` as the worked
   example — particularly its ramp, which is what exercises the reporting loop
   that a snap-to-target implementation would short-circuit.
3. **Set the identity.** `CONFIG_TREADMILL_DEVICE_NUMBER`,
   `CONFIG_TREADMILL_MANUFACTURER_ID`, and the four placeholder `1`s in
   `main.c`'s `fill_common_id()` — model number, hardware revision and the two
   software revisions all land on a head unit's device-information screen.
4. **Set the ranges to match your machine.**
   `CONFIG_TREADMILL_BLE_MAX_SPEED_CENTI_KPH`,
   `CONFIG_TREADMILL_BLE_MIN_INCLINE_DECI` and
   `CONFIG_TREADMILL_BLE_MAX_INCLINE_DECI` become FTMS's Supported Speed Range
   and Supported Inclination Range, which is what a client reads to learn the
   legal span before commanding one. **They must agree with what your source
   will actually accept**: advertising a range wider than the belt can do means
   being commanded into it and then answering Operation Failed, which a client
   reports as a fault rather than as a limit.
5. **Name it.** `CONFIG_BT_DEVICE_NAME` in `ble.conf`. The default is
   `"RadiANT Treadmill"` and it is deliberately not anybody's product name —
   see [ADR 0003](decisions/0003-naming-trademark-and-usb-identity.md)'s
   amendment. The budget is 29 characters (the name is in the scan response,
   not the advertisement) and a `BUILD_ASSERT` enforces it, because an over-long
   name makes `bt_le_adv_start()` return `-EINVAL` and the node then transmits
   ANT+ perfectly while never advertising.

---

## 6. The production checklist

Seven items. Items 1 and 2 are **not achievable in this tree today** and say so.

1. **A per-unit device number.** Nothing in `apps/` calls
   `node_ident_provision()`, so every machine flashed from one build shares one
   ANT device number. For a single demo that is fine. **For a gym it is fatal**
   — twenty treadmills answering to one number is twenty machines a receiver
   cannot tell apart — and closing it is a manufacturing-process decision with
   its own ADR, exactly as `hrm-reference-design.md`'s items 3 and 4 record.
2. **A real manufacturer id.** 255 is the ANT+ development id. Shipping with it
   is claiming somebody else's identity.
3. **Real model and revision numbers** in `fill_common_id()`.
4. **The capability bits must match the machine.** FE-C page 54 bit 2
   (Simulation mode) and FTMS's Inclination Target Setting bit are *claims*
   that page 51 and op code `0x03` will be honoured. If `set_incline()` returns
   `-ENOTSUP`, clear both.
5. **The advertised ranges must match the machine.** See §5 item 4.
6. **A pairing UX.** FTMS's control point requires encryption, so a client must
   bond before it can command anything. This build accepts pairing from any
   central with no user confirmation, which is right for a bench and wrong for
   a gym floor where anybody's phone is in range. Decide what "pairing mode"
   means on your console.
7. **Decide what happens when a second client asks for control.** FTMS §4.16.2
   gives control permission to one client at a time; this build's
   `CONFIG_BT_MAX_CONN` is 1, so the branch that would revoke a previous
   holder's permission exists and is unreachable. Raising the connection count
   is what makes it reachable.

---

## 7. Verifying on a bench

**§7.1 to §7.3 have been run. §7.4 onwards have not** — everything from "on
air" down is the procedure, not a result.

### 7.1 Host, no hardware — RUN, 2026-08-15

```
python -m pytest tools/            # 694 passed
python scripts/check_profile_registry.py
python scripts/check_test_counts.py twister-out/twister.json
```

(Use the NCS toolchain's Python; there is no system Python in this project.)

### 7.2 C unit tests — RUN, 2026-08-15

`scripts/run_ztest_hw.ps1` on a DK, or `twister -p native_sim` on Linux, are
the supported routes and the ones CI takes.

**They were also run on the host, and that is worth writing down because it is
not obvious that it is possible.** `radiant/tests/src/test_profile_fec_tx.c`,
`test_profile_sdm.c`, `test_rules.c` and `apps/treadmill/tests/pages/` were
compiled with the host clang against a throwaway `ztest.h` shim and executed:
**68 cases, 0 failures.** Nothing about that shim is in this repository and
nothing should be — the point is only that these particular suites reach no
kernel object, no radio and no clock, which is exactly the property their
CMakeLists claim, so the claim is now tested rather than asserted.

The assertions worth naming: grade `0x4E20` → 0.00 % and `0x4F4C` → +3.00 %;
incline `0x7FFF` → invalid against grade's `0xFFFF`; page 16 and page 19 each
at least once every five messages over 264 messages, with the common pair
consecutive every 65; the strides→steps doubling; the distance carry exact over
a simulated hour; and the vertical divisor being 10000 rather than 100.

### 7.3 Build matrix

**Run, 2026-08-15, NCS v3.2.4.** All six rows build and every `.config`
assertion in `build-treadmill` holds. Measured on `nrf54l15dk/nrf54l15/cpuapp`
unless stated:

| Configuration | Flash | RAM |
|---|---|---|
| FE-C + SDM, no BLE | 96.7 kB (6.7 %) | 41.2 kB (21.4 %) |
| FE-C only (`fec_only.conf`) | 95.4 kB (6.6 %) | 40.9 kB (21.2 %) |
| No source (`source_custom.conf`) | 95.5 kB (6.6 %) | 41.0 kB (21.3 %) |
| **+ FTMS + RSC + SMP** (`ble.conf`) | **245.9 kB (16.9 %)** | **68.5 kB (35.6 %)** |
| **+ ANT+ HR slave** (heaviest) | **247.3 kB (17.0 %)** | **68.5 kB (35.6 %)** |
| FE-C + SDM on nRF52840 | 86.3 kB (8.3 %) | 41.1 kB (15.7 %) |

**The RAM worry in §9 was the right worry and the answer is comfortable.** The
BLE second stack costs ~150 kB of flash and ~27 kB of RAM here, and the third
ANT+ channel costs almost nothing on top. 35.6 % of 188 kB leaves real room —
which matters because the Matter flash spike already established for this
project that flash is not the wall, RAM is.

These are *build* numbers. They say nothing about whether the radio can carry
two masters and a BLE connection at once, which is §7.6.

`scripts/build_all.ps1 -Backend core -RadiantBackend nrf`. Confirm the
resulting `.config` really has `CONFIG_BT=y` **and**
`CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL=y`: per `radiant/Kconfig`,
`RADIANT_BACKEND_NRF` has `depends on !BT || RADIANT_BACKEND_NRF_GATE_MPSL`, so
getting this wrong silently builds the **null radio backend** and the board
transmits nothing. **This trap has bitten this project twice. Assert, do not
eyeball** — which is what the script's own loop does.

### 7.4 On air, ANT+

With a second board or a dongle running `tools/ant_sim.py`:

- FE-C `0x11` and SDM `0x7C` both discoverable, at 8192 and **8134** counts.
- Page 16's speed and distance advance; page 19's cadence and vertical distance
  advance on an incline.
- Page 54 reports Simulation mode (byte 7 bit 2).
- **The control loop**: send page 51 with grade = +3.00 % (raw `0x4F4C`) and
  confirm a page 71 `PASS` echoing the grade at bytes 5–6 **and** a page 17
  incline of `+300` once the simulated actuator has ramped.

### 7.5 On air, BLE

`scripts/ble_central.ps1` / `tools/ble_central.cs`, or nRF Connect on a phone:

- The FTMS Service Data AD element is present (flags bit 0 set, machine-type
  bit 0 set). Omitting it is the most common reason a machine is invisible to an
  app that otherwise works.
- Treadmill Data notifies at ~1 Hz.
- A control-point write **without** Request Control is rejected with
  `0x05` Control Not Permitted.
- An **unpaired** write fails outright — Table 4.1 gives the control point the
  Encryption permission.
- `Set Target Inclination` of +3.0 % (wire bytes `03 1E 00`) after Request
  Control is indicated as success and appears on the ANT+ side within one FE-C
  cycle.

**Check every field's data type and resolution against the GATT Specification
Supplement, not against FTMS v1.0.** FTMS defers all of them to Assigned
Numbers and prints no 16-bit UUID values at all; a scale factor remembered
rather than read is a silent wrong-by-ten.

### 7.6 Both at once, and the number that decides the design

A head unit tracking FE-C while a phone is connected over FTMS, for ≥10
minutes, against `tools/ab_gates.toml`'s `[gates.coexistence]`.

**Two ANT+ masters plus BLE on one radio is more contention than anything this
project has measured.** The arbiter has been characterised with one master
beside BLE (P8b) and with eight masters beside 802.15.4 (P3.5), never this mix.
The two periods — 8192 and 8134 — beat against each other with a period of about
**32 seconds**, so if they interact badly the symptom is a slow cycle of loss on
both channels rather than a steady figure, which a short run reads as noise.
`fec_only.conf` is what answers "is it the second master?" in one build.

Two bench traps that have faked results here before:

- `[gates.loss_exact]`'s availability is **inverted** in a way that has produced
  phantom bugs. Read the bench-traps notes before believing a bad number.
- **Do not attempt an A/B/A sitting in the current RF environment.** The room's
  loss floor is 5–8 % and unstable, which voids the spread. Take absolute
  pass/fail on `[gates.coexistence]` only.

---

## 8. Gym occupancy: what the treadmill does and does not do

**Nothing new on the treadmill at all.** A machine built for §1–§7 is already a
gym occupancy sensor to any dongle that cares: it broadcasts FE-C, an
`apps/dongle_thread` receives, and the bridge publishes.

Do not put Thread on the treadmill. `docs/radiant-bridge.md` §13 lists the
bridge as a Thread Router, Leader or Border Router as **"Not deferred — refused,
on one radio, permanently"** ([ADR 0011](decisions/0011-never-a-border-router.md)).

**And occupancy stops being a stretch here.** `docs/radiant-bridge.md` §8.2
admits the derived booleans stretch Matter's Occupancy Sensor ("a rider is
occupying the trainer"). A treadmill is the case where it is not inference at
all: FE-C page 16 byte 7 carries an explicit FE state, which is the machine's own
report. `radiant_rules.c` now turns `IN_USE` into `RADIANT_FIELD_OCCUPANCY` on
`RADIANT_RULE_FIELD_EQUIPMENT_IN_USE` — no vocabulary addition needed, because
occupancy `0x02` is already in all three copies.

**What remains open, and it is a measurement rather than a feature.**
`RADIANT_BINDING_MAX` is **8**, and its own comment says it was "sized with the
Matter endpoint count, ANT tracked-channel count, and coexistence budget
together — raising it is a radio decision, not a memory one". A gym with twenty
treadmills needs twenty tracked 4 Hz slave channels on one receiver, and the
largest concurrent load ever measured in this repository is eight ANT masters.
Nothing says it fails; nothing says it works. **Run N synthetic FE-C masters
against one dongle, climb past 8, and record where loss or sweep throughput
falls over.** If the answer is under twenty, the shape is one dongle per bank of
machines — which is fine, and much better discovered on a bench than in a gym.

Three smaller notes, one nastier than it looks:

- **Binding is explicit opt-in, always** (§10.1 rule 1: "no promiscuous ingest,
  ever"). Reasonable for a gym operator, and it means a provisioning flow for N
  machines.
- **The binding table is not persisted.** Every reboot loses all bindings, and
  re-pairing mints a **new uuid**, hence new MQTT topics and a fresh set of Home
  Assistant entities. A dashboard accumulates orphans on every power cut.
  Persistence is a prerequisite for this use case, not a nicety.
- **Publish pacing is specified and not implemented.** `mqtt_sink.c` publishes
  every sample it is handed. The derived booleans are change-only so occupancy
  is fine; raw speed and FE state at 4 Hz × N machines is not.

The privacy argument that makes `docs/radiant-bridge.md` §10 careful about heart
rate mostly evaporates here: "treadmill 7 is in use" is machine state, not
biometric data about a person.

---

## 9. Known limits

- **The simulator is the only hardware.** Nothing is validated against a motor
  controller or an incline actuator.
- **Memory: measured, and it is fine.** §7.3 has the table. The heaviest
  configuration is 247 kB flash and 68.5 kB RAM on an nRF54L15 — 17 % and 36 %.
  The concern was real (`radiant/spike/mpsl_arb/README.md` measured the BLE
  second stack at +78.7 kB flash / +16.0 kB RAM before two hand-written GATT
  services, SMP and a bonding store were added on top) and the answer came out
  comfortable. `docs/radiant-bridge.md` §11 still rates nRF52840 as "very
  tight" for a *full bridge* build; this application is not that, and its
  nRF52840 row is 8 % flash and 16 % RAM — but it is ANT+ only, and treat
  nRF54L15 as the target and nRF52840 as build-verified, exactly as `hrm_ble`'s
  own matrix does.
- **Page layouts of varying strength.** `profile_fec_tx.h` and `profile_sdm.h`
  both open with an honesty block separating the layouts that carry a table
  citation from the ones recorded by byte offset. Believe a bench run over
  either header.
- **Zwift's native treadmill incline is out of scope.** Zwift sends route grade
  to a KICKR RUN over a *proprietary Wahoo service*, not over FTMS's control
  point and not over FE-C. `treadmill_ble_grade_provider_register()` is a named
  seam with nothing behind it; implementing the protocol would need its own ADR
  in the shape of [ADR 0002](decisions/0002-clean-room-policy.md), which is the
  same ruling `docs/ble-running-dynamics-notes.md` already carries for the
  Garmin BLE path.
- **No procedure timeout on the FTMS control point.** FTMS v1.0 §4.16.4 is
  titled "Procedure Timeout" and gives none. The commonly-assumed 30 s is not in
  the spec, and this implementation does not invent one.
- **`Reset` does not zero the session.** FTMS §4.16.2 specifies Reset as target
  speed and inclination to 0, time fields to 0, Training Status Idle. This build
  does the first and the third and deliberately not the second: a BLE client
  resetting its own view must not silently discard the ANT+ side's accumulators
  mid-run.
