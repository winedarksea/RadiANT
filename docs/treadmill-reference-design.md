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

**Including the problem it creates.** Both control paths landing on one
function is the design working as intended, and it is also exactly what makes
two simultaneous controllers *last-writer-wins with no signal to either*: a head
unit running a simulated climb and a phone running an interval workout are both
told their command succeeded while the deck oscillates between two setpoints,
and neither end can see it. So there is a **control-owner token**
(`src/treadmill_control.h`), and the policy is
[§4a](#4a-who-is-allowed-to-command).

**It is not a suppression rule, and the distinction is load-bearing.** Nothing
in this application ever stops a transmitter — FE-C pages 16/17/19/80/81, SDM
`0x7C` and the FTMS Treadmill Data notification all keep running for every
listener regardless of who holds control. *Sharing the air* is a radio question,
answered by the MPSL gate and measured in §7.6. *Whose grade the deck moves to*
is a semantic question with no radio content, which would need answering even if
the two transports were on separate chips. Conflating them is the mistake this
paragraph exists to prevent.

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

## 4a. Who is allowed to command

`src/treadmill_control.{h,c}`, `CONFIG_TREADMILL_CTRL_POLICY_EXCLUSIVE`
(default), `CONFIG_TREADMILL_CTRL_ANT_TIMEOUT_MS` (default 30000).

### The policy: explicit beats implicit

The asymmetry is the whole design and it is **spec-native rather than
invented**. FTMS §4.16.2 already requires an explicit `Request Control` (`0x00`)
handshake before any procedure — it *has* an acquire. FE-C has no equivalent: a
controller simply sends page 51 and reads the page 71 answer. So an explicit
claim outranks an implicit one, and each side already carries the wire signal
needed to say no.

| Event | Result |
|---|---|
| FTMS `Request Control` (`0x00`) | Claims. **Preempts an ANT owner** — explicit beats implicit. |
| FTMS `Reset` (`0x01`), or a disconnect | Releases. |
| FE-C page 50/51, owner `NONE` or `ANT` | Claims or refreshes implicitly, and applies. |
| FE-C page 50/51, owner `BLE` | **Refused: page 71 status `PROFILE_FEC_CMD_STATUS_REJECTED` (3).** |
| FTMS command with no `Request Control` | `CONTROL_NOT_PERMITTED` (`0x05`), which this file already did. |
| ANT owner idle > `TREADMILL_CTRL_ANT_TIMEOUT_MS` | Released. |

Neither refusal code is new: `PROFILE_FEC_CMD_STATUS_REJECTED` is
`profile_fec_tx.h:103` and `0x05` is FTMS Table 4.24. Both already mean exactly
this. **Silence would be worse than either** — §8.8 is explicit that a
controller reports a machine that does not answer as broken, which is why every
branch of the FE-C handler still ends in a page 71.

**Why ANT+ needs a timeout and BLE does not.** ANT+ is connectionless. A BLE
client that walks away generates a disconnect; an FE-C controller that walks
away generates *nothing at all* — it simply stops sending page 51. An idle
timeout is therefore the only release an implicit ANT claim can ever have, and
without one a single page 51 early in a session would lock a phone out of the
machine until power cycle. The reverse does not apply: a phone that has taken
control is still there whether or not it has commanded anything this minute, so
a BLE claim is released by an event and never by a timer.

**Reads are never gated.** Page 70 (Request Data Page) and page 55 (User
Configuration) are answered whoever holds the token. Refusing a *read* would
make the machine look broken to a head unit that is merely observing it, which
is the opposite of what the token is for. Pages 48 and 49 keep answering
`NOT_SUPPORTED` to everybody, because this machine genuinely does not implement
them — answering `REJECTED` there would report a missing capability as a missing
permission and tell a controller to retry something that can never work.

**Pages 50 and 51 are gated together.** They are the simulation-mode command
set, and honouring half of a parameter set while refusing the other half is how
a controller decides simulation mode is broken and stops sending page 51 too.

### Why it lives in its own kernel-free module

`treadmill_control.c` includes no Zephyr header, takes no lock and reads no
clock — it is *told* what time it is, exactly like `treadmill_state.c`. That is
not a stylistic echo. **Nothing anywhere tests `src/treadmill_ble.c`:** there is
no ztest for the GATT service, for `write_cp()` or for the advertising payload,
which are covered only by `BUILD_ASSERT`s and the bench procedure in §7. Putting
the policy where it first seemed to belong — inside `treadmill_ble.c` — would
have put it somewhere nothing could reach. In its own module,
`apps/treadmill/tests/pages` compiles it directly and drives the whole policy
with no radio and no stack.

### The lock that is not there

The module is lock-free, and that is only sound because of a change made
alongside it: **both control paths now marshal onto the system workqueue before
they reach it.**

`treadmill_state.h` states the invariant — *"everything that touches this struct
runs on the system workqueue"* — and both paths violated it as wired. An FE-C
page arrives on the `radiant_event` thread; an FTMS write arrives on the BT RX
thread; the physics tick and source reports are on the workqueue. Three writers,
no lock, and `treadmill_ble.h` even admitted it in a comment. A `grep` for
`k_mutex|k_sem|spinlock|irq_lock` across `apps/treadmill/` returned nothing.

The fix restores the invariant rather than adding a second, weaker one beside
it:

- **FE-C** — `antr_on_message()` copies the eight-byte body into a four-deep
  ring and submits a `k_work`; `apply_control_page()` runs on the workqueue and
  stages page 71 there. Invisible on air: page 71 goes out on the next data slot
  up to 250 ms later either way, against a workqueue turnaround of microseconds.
- **FTMS** — `write_cp()` keeps only the ATT-level validation (§4.16.3 wants
  those two failures as ATT errors) and defers the whole op-code switch to
  `cp_work`. Answering late is free because `cp_respond()` was always a GATT
  *indication*. The connection is `bt_conn_ref()`'d across the hop, and so is
  the one in the disconnect handler — that one does a pointer comparison, and a
  freed `bt_conn` can be reallocated at the same address, which would revoke the
  permission of a client that had just been granted it.

Once both are on one thread, the token needs no lock either — which is the main
reason to prefer this over a mutex.

### Turning it off

`CONFIG_TREADMILL_CTRL_POLICY_NONE` restores last-writer-wins and compiles
`treadmill_control.c` out entirely, so the "none" build really is the old
behaviour rather than a token that always says yes. It is a legitimate choice
when something outside this node already guarantees a single commander, and it
is not a safe default. The `choice` arm is asserted from the generated
`.config` by both `build_all.ps1` and the `build-treadmill` CI job, because a
`choice` always has *some* arm set and silently shipping the wrong one is the
failure this project keeps paying for.

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

### 7.2 C unit tests — RUN, 2026-08-15; re-run 2026-08-17

`scripts/run_ztest_hw.ps1` on a DK, or `twister -p native_sim` on Linux, are
the supported routes and the ones CI takes.

**2026-08-17, on an nRF5340 DK:**
`run_ztest_hw.ps1 -App apps\treadmill\tests\pages -NcsVersion v3.4.0` →
`SUITE PASS - 100.00% [treadmill_pages]: pass = 18, fail = 0`. Twelve of those
are the conversions and accumulators; the six new ones are §4a's control-owner
token — BLE preempting ANT, ANT never preempting BLE, the idle ANT timeout
expiring and being refreshed, release handing the machine back, the timeout
surviving a `k_uptime_get_32()` wrap, and the read pages staying ungated.
`tests/expected_counts.yaml`'s `min_testcases` moved 843 → 849 with them.

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

### 7.4 On air, ANT+ — PARTLY RUN, 2026-08-16

Rig: `tread_l15` on an nRF54L15 DK (J-Link `1057737173`), received by the
nRF52840 Dongle running RadiANT `0.01B00` as an ANT+ stick. RSSI −34 dBm.

**The broadcast half passes.** A receiver pinned to `#4243` heard, over 90 s
on two simultaneous channels:

| | device type | packets | loss |
|---|---|---|---|
| ch 0, FE-C | **17** (`0x11`) at 8192 | 356 | 0.84 % |
| ch 1, SDM | **124** (`0x7C`) at 8134 | 351 | 3.57 % |

Both masters run from one node at once. Page 16 carries `equipment_type 19`,
a live speed (1230–1400 mm/s) and an advancing distance; page 19 carries a
cadence of 70–71 strides/min; page 17 carries the incline; common pages 80/81
carry serial 4243. The rotation matches §10.1.

**USE A PINNED CHANNEL, NOT `ant_scan.py`, TO LOOK FOR THIS NODE.** A wildcard
slave latches onto the first sensor it hears and then reports only that one. On
this bench it locked to a Wahoo trainer (`#52233`) and reported the treadmill as
absent, twice, on a board that was transmitting perfectly the whole time. That
is a false negative that reads exactly like a dead radio.

**Page 54 is not in the broadcast rotation** — it is answered on request
(page 70), so its absence from a passive capture is correct rather than a gap.

**The control loop is UNTESTED, and not for want of trying.** Page 51 was sent
as acknowledged data 13 times across the ~32 s beat, on both the two-master and
the `fec_only.conf` builds: `TRANSFER_TX_FAILED` every time, no page 71, incline
stayed 0. **This does not implicate the treadmill.** The control run: the same
dongle, sending a read-only acknowledged page 70 to the *commercial* trainer
`#52233`, also failed 0/4. The dongle cannot originate an acknowledged transfer,
so the rig cannot ask the question.

> **Narrowed by measurement, 2026-08-17 — and the obvious explanation is
> wrong.** "The dongle cannot originate an acknowledged transfer" is true, and
> the failure is now localised to the **return acknowledgement**, not to the
> transmit. See §7.7 for the A/B that establishes it. In short: the originated
> packet *arrives at the master at the full channel rate*, in every build
> tried, and the sender is still told `TRANSFER_TX_FAILED` because it never
> hears the ack come back. Anyone about to look at the transmit instant should
> read §7.7 first — that hypothesis has already been built, flashed and refuted.

A known-good originator would still be useful for decoupling treadmill
verification from this defect. The Feather is the candidate, but note it is
only an unattended flash while the nRF5340 DK's debug-out SWD cable is
connected to it — checked on 2026-08-17 and it was **not**: a `connect` with
`-Device nRF52840_xxAA` found a Cortex-M**33**, which is the DK's own SoC.
Confirm a Cortex-M4 in the J-Link output before assuming that route is live.

### 7.4a Two defects found on the bench, 2026-08-16 — BOTH RETIRED, 2026-08-17

> **Read §7.4b first. Neither defect below reproduced, and one of them never
> existed.** The original observations are kept in full rather than deleted,
> because what they were actually measuring is the useful part.

Both are treadmill-specific and both were isolated against `apps/hrm_ble` on the
**same board, same probe, same port, same NCS** — which is what makes them
findings rather than bench noise.

**1. The BLE arm never advertises.** `tread_ble` boots, keeps both ANT+ masters
running (78 packets, device type 17, over 25 s), and puts nothing on the air over
BLE: an active scan of 100 devices found no `RadiANT Treadmill` and no `0x1826`
from this board. `node_ble` — `apps/hrm_ble`'s BLE arm — flashed to the same
board minutes later, advertised as `RadiANT HR` with `0x180D` immediately. So the
radiant + SoftDevice-Controller + MPSL-gate integration is *not* the problem.
`treadmill_ble_start()` is called (`main.c:765`) and its result is discarded by
the `(void)` cast, which is exactly the failure mode its own comment at
`treadmill_ble.c:1016` predicts — so the return value of `bt_enable()`,
`bt_rscs_init()` or `bt_le_adv_start()` is the thing to capture first.

**2. No treadmill build produces any console output at all.** Not one byte on
either VCOM, with DTR asserted, on any treadmill image — **not even the Zephyr
boot banner**, which is printed before any application code. `apps/hrm_ble` on
the same board and port prints its banner every time. The console Kconfig of the
two applications is *byte-identical* (diffed across every `CONFIG_LOG*`,
`CONSOLE`, `UART*`, `SERIAL`, `PRINTK`, `BOOT_BANNER` symbol; the only
difference in the whole set is `MULTITHREADING_LOCK`, which BLE brings in), and
the two board overlays are identical including `radiant,radio-timer = &timer20`.
So this is runtime, not configuration.

**Do not "fix" it by switching to `CONFIG_LOG_MODE_IMMEDIATE`.** That was tried:
the image then stops transmitting entirely — no ANT+ at all — while the console
*still* prints nothing. Synchronous logging from this application's contexts is
not survivable, which is consistent with `docs/decisions`' ZLI findings, and it
turns a diagnostic problem into a dead board.

These two are plausibly one root cause: in every treadmill build the things that
happen *after* the ANT+ masters open — the deferred log flush, and BLE — are the
things that do not happen, while the masters themselves run indefinitely. That
is a hypothesis, not a measurement; it was not confirmed this pass.

### 7.4b What §7.4a was actually measuring, 2026-08-17

Rig: the **unmodified** `build/tread_ble` image from 2026-08-16 — the same
binary §7.4a was written against — reflashed to the same nRF54L15 DK (J-Link
`1057737173`), console read with `scripts/capture_log.ps1 -Port COM7` across a
J-Link reset, and a 120 s active scan from `scripts/ble_central.ps1`.

**Defect 1 was a measurement artefact. The node advertises and always did.**

```
FD:D3:C2:A3:0C:4B  rssi -47  adv 42  name 'RadiANT Treadmill'  services [0x1826, 0x1814]
```

42 advertisements in 120 s at the 1000 ms interval, both service UUIDs present,
and the address matches the identity in the boot log. The original scan used
`ble_central.ps1`'s **defaults**, which are `-Name "RadiANT HR"` and
`-Seconds 10` — the strap's name, and a tenth of the ≥ 90 s this application's
own Kconfig tells you to budget at a 1 s advertising interval.
`CONFIG_TREADMILL_BLE_ADV_INTERVAL_MS`'s help says so in as many words, and
`docs/hrm-reference-design.md` §7 records the same trap being paid for once
already. **State the scan duration and the `-Name` in any future report;** a
scan that finds nothing is only evidence if both are known.

**Defect 2 was real but was not what it looked like. The console was never
dead.** The boot capture is complete from radiant's init through to
`BLE: FTMS + RSC advertising every 1000 ms as 'RadiANT Treadmill'`. What is
missing is only the first few lines, and the log says exactly why:

```
[00:37:07.391,971] <inf> radiant_swi: swi: trampoline on IRQ 29 at priority 1
--- 3 messages dropped ---
```

The mechanism is `LOG_MODE_DEFERRED` + `LOG_MODE_OVERFLOW` + a **1024-byte**
`CONFIG_LOG_BUFFER_SIZE`. Every console byte here, `printk` included
(`CONFIG_LOG_PRINTK=y`), goes into that ring and comes out only when the log
processing thread runs — and that thread sleeps 1000 ms and wakes at 10 queued
messages. radiant's init emits far more than 10 lines faster than the first
drain, so the ring discards its **oldest** entries: the banner and the first
application lines. Fixed by raising `CONFIG_LOG_BUFFER_SIZE` to 4096 in
`prj.conf`.

"Not even the boot banner" was the right observation and the wrong inference.
With `LOG_PRINTK=y` the banner is not a direct-to-UART write; it is a ring-buffer
entry like any other, so its absence says nothing about the UART. The
byte-identical `CONFIG_LOG*` diff in §7.4a is consistent with this and always
was — `apps/hrm_ble` has the same log configuration and simply does not emit
enough at boot to overflow 1024 bytes.

**Three corrections of record follow from this pass:**

- §7.4a's unifying hypothesis — "the things that happen *after* the ANT+ masters
  open are the ones that do not happen" — **is refuted, and by §7.4's own
  evidence.** `node_tick_fn` runs on the system workqueue and is what drives
  `treadmill_state_tick()` and `publish()`; §7.4 records live, *advancing* speed
  and distance over 90 s. The workqueue was running the whole time. Nothing was
  ever wrong after the masters opened.
- The `(void)` cast at `main.c:765` was **not** the cause of defect 1, and
  capturing it would not have found anything: `treadmill_ble_start()` already
  `LOG_ERR`s every step it can fail at, and it was returning 0. It is captured
  now anyway, because "this node is ANT+ only" deserves to be one greppable
  line.
- `CONFIG_LOG_MODE_IMMEDIATE` remains the wrong tool and §7.4a's warning stands
  — it stops ANT+ transmission outright. The right probe for a genuinely silent
  console is `CONFIG_LOG_PRINTK=n` plus a bare `printk()`, which bypasses the
  processing thread without making every `LOG_*` in a radio callback
  synchronous. It was not needed this time.

### 7.5 On air, BLE — PARTLY RUN, 2026-08-17

**USE `-Mode hold`, NOT `-Mode connect`.** `connect` is the P9 heart-rate
instrument: it demands service `0x180D`, throws `no Heart Rate service` against
a treadmill and drops the link within seconds. That is what left §7.6 without a
number — the connection it was meant to hold did not survive the service check.
`hold` was added for this and subscribes to FTMS Treadmill Data instead:

    scripts\ble_central.ps1 -Mode hold -Name "RadiANT Treadmill" -Seconds 660

**What that verified on real hardware** (nRF54L15 DK, `tread_ble`):

| check | result |
|---|---|
| discoverable by name | `FD:D3:C2:A3:0C:4B`, 42 adverts in 120 s at 1000 ms |
| advertised services | `0x1826` (FTMS) **and** `0x1814` (RSC) |
| GATT on connect | 4 services: `1800`, `1801`, `1826`, `1814` |
| Treadmill Data (`0x2ACD`) notify | subscribed; **60 notifications in 60 s** |
| connection held | `Connected` throughout, no drop |

The notify rate is the one worth calling out: exactly 1 Hz, which is
`CONFIG_TREADMILL_BLE_NOTIFY_INTERVAL_MS` taking effect rather than the
100 ms node tick leaking through — the pacing in
`treadmill_ble_state_changed()` is doing its job.

Still unverified, and all of them need the acknowledged originator §7.7 shows
this tree does not have, or a phone:

- The FTMS Service Data AD element is present (flags bit 0 set, machine-type
  bit 0 set). Omitting it is the most common reason a machine is invisible to an
  app that otherwise works. (The UUID list was confirmed; the Service Data
  element itself was not read out.)
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

**RUN, 2026-08-17 — and the connected case costs nothing measurable.**

`tread_ble` on the nRF54L15 DK, FE-C received on a pinned channel
(`ant_verify.py --profile fec-treadmill --device-number 4243`) through a
RadiANT USB stick, while `ble_central.ps1 -Mode hold` held a real connection
and consumed Treadmill Data notifications at 1 Hz:

| condition | duration | packets | FE-C loss |
|---|---|---|---|
| BLE **connected**, notifying at 1 Hz | **600 s** | 2172 / 2401 | **9.54 %** |
| BLE advertising only, no central | 300 s | 1086 / 1201 | **9.58 %** |

**The question §7.6 was written to ask is answered: connecting a phone over
FTMS is not what costs the ANT+ side anything.** Both arms sit within 0.04 pp
of each other — far below the room's own instability — so a live FTMS client
notifying once a second beside two ANT+ masters is not detectably worse than
the same node merely advertising.

**It FAILS `[gates.coexistence]` all the same, and the failure is the room.**
`absolute_ceiling_pct` is 1.5 % and both arms are near 9.5 %, every missing
packet reported by the radio as `RX_FAIL` — i.e. it happened on the air, not in
the host or the scheduler. The same rig read **0.84 %** on 2026-08-16 (§7.4) at
−34 dBm, so this is the 5–8 %-and-unstable floor this bench has been carrying,
drifted higher. **Do not read the pass/fail as a verdict on the design, and do
not read the 0.04 pp as a tight bound either** — a spread taken in a room with
a 9.5 % floor bounds the coexistence cost only to "not large". The gate wants
re-running in a quiet room before anything is concluded in either direction.

**One anomaly, unresolved and NOT chased:** every run reports
`sensor liveness: the event counter advanced on 0 of N packet pairs`. FE-C
data pages 16/17/19 carry no event counter, so this is most likely
`ant_verify.py`'s generic counter rule being applied to a profile that has no
such field — but it has not been confirmed, and it should not be read as a
pass. The accompanying "accumulator continuity" violations scale with the lost
packets (142 against 229 missing, 66 against 115) and are consistent with loss
rather than with a defect.

Two bench items remain owed, blocked on a *different* defect from the one that
blocked them before: the FE-C **control loop** (an acknowledged page 51
answered by a page 71) and the **arbitration** check (§4a's token refusing an
ANT+ command while a phone holds control) both need a working acknowledged
originator, and §7.7 shows the tree does not have one. The arbitration policy
itself is covered by six ztest cases; what is unverified is the on-air half.

The older, weaker number, kept because it is what this section used to rest on: with **both ANT+ masters
running and the BLE stack linked in and initialised** (`tread_ble`), the FE-C
channel still delivered 78 packets over 25 s at 3.7 % loss. That is BLE
*present*, not BLE *connected*, so it bounds nothing about the connected case —
but the two masters plus an initialised second stack do not by themselves break
the ANT+ side. The 600 s run above supersedes it.

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

### 7.7 The acknowledged-originator A/B, 2026-08-17 — hypothesis REFUTED

> **THIS SECTION'S CONCLUSION IS WRONG, and §7.8 records why and what
> replaced it.** The "refutation" below rests on a counter that was reading the
> receiving master's OWN slots, not packets delivered to it. ~219 in 55 s is 4 Hz
> — the channel period. Corrected instrumentation shows the packet was never
> arriving at all, the hypothesis this section rejected was right, and the
> revert it justified had to be undone. Left in place because the mis-reading is
> the instructive part: an originator cannot tell "never arrived" from "arrived,
> never acknowledged" — both are `FAIL_NO_ACK` — so the far end has to be
> counted, and counted with the two things kept apart.

The single most useful result of this pass, and it is a negative one.

**Rig.** One nRF54L15 DK (J-Link `1057737173`) flashed with `apps/dongle`
(`-DANT_RADIO=core -DRADIANT_BACKEND=nrf`, `CONFIG_RADIANT_BACKEND_NRF=y` read
back from `.config`) and driven over **COM8** as the slave **originator**. A
RadiANT USB stick held open as a **bidirectional master** (`0x10`, #4321, type
17, period 8192) — not `0x50` `MASTER_TX_ONLY`, which is the one master
documented not to listen and would have armed no reply turnaround at all.
`tools/ant_fec_control.py` sends the acknowledged page 51; a host script counts
what the master actually receives.

Two builds differing in exactly one expression — the `t_sync_at` passed to
`radiant_transfer_ack_data()` — flashed minutes apart in the same room:

| arm | `t_sync_at` | sender result | packets the MASTER received |
|---|---|---|---|
| A | `RADIANT_TIME_NEVER` (as shipped) | `TRANSFER_TX_FAILED` ×4 | **219** in 55 s |
| B | `next_slot + RADIANT_TRANSFER_SLOT_REPLY_US` | `TRANSFER_TX_FAILED` ×4 | **218** in 55 s |

**The packet was never the problem.** It arrives, at ~4 Hz, essentially 1:1 with
the master's own `EVENT_TX` — so it is landing squarely inside the master's
slot, in *both* arms. The proposed root cause — that `RADIANT_TIME_NEVER`
resolves to an arbitrary phase and the packet "lands where nobody is listening"
— is false on this backend, and the slot-aligned build bought nothing while
adding up to one channel period of latency to every acknowledged transfer in
the tree. **Arm B was therefore reverted.** `apps/common/ant_radio_radiant.c`
carries the tombstone so the idea is not re-had.

**What is actually broken is the return acknowledgement.** The receiver has the
data and the sender is told the transfer failed. That points at
`radiant_burst.c`'s `arm_ack_window()` and the `t_data_sync` it is handed —
the *other* side of the exchange from where the search started. It also fits
the one piece of evidence §7.4 could not place: a read-only page 70 to a
commercial Wahoo trainer failing 0/4 is exactly what a broken ack-reception
path looks like, and is *not* something a transmit-phase bug could cause
selectively.

**A second defect is visible in the same data and is not yet explained.** The
sender gave up after 4 attempts, and the packet kept going out **once per
channel period for the remaining ~50 s** (219 and 218 packets against 221 and
220 `EVENT_TX`). The host was told `TRANSFER_TX_FAILED` promptly, so a
transfer that is finished from the caller's point of view is still occupying a
slot on every period. That is air time spent indefinitely on a dead transfer
and it contradicts `radiant_burst.c`'s "fails once and does not retry" rule as
observed from the air.

### 7.7a Narrowing it further, 2026-08-17 — there are TWO defects, not one

> **Superseded by §7.8.** "Defect 1 — the USB sticks never put the packet on the
> air" was right about the symptom and wrong about the cause: it is not the
> sticks, it is every originator, and the reason is the transmit instant this
> section's predecessor had just finished dismissing. The delivery counts in the
> table below carry the same counter error as §7.7. The board-identity caveat at
> the end is now resolved: stick `755972D7…6183` is the Feather.

With the Feather's SWD cable attached (so a second board became
unattended-flashable), the exchange was run in every available direction. The
originator is `tools/ant_fec_control.py`; "delivered" is counted at the
receiving end by a host script holding a bidirectional master open, so it is
independent of what the sender believes.

| originator | master | delivered to the master | sender result |
|---|---|---|---|
| **L15 DK**, freshly built HEAD | USB stick A | **219**, then 122 | `TX_FAILED` |
| **L15 DK**, freshly built HEAD | USB stick B | **122** | `TX_FAILED` |
| USB stick A | USB stick B | **0** | `TX_FAILED` |
| USB stick A | L15 DK as master | **0** | `TX_FAILED` |
| USB stick A and B | L15 running the **treadmill** | **0**, no page 71 | `TX_FAILED` |

**Defect 1 — the USB sticks never put the packet on the air at all.** Zero
delivered in every combination they originate, including stick-to-stick with no
nRF54L15 anywhere in the path. This is why every previous attempt failed and why
the treadmill's control loop has never once been exercised: **the shipping
dongle is the broken half**, and the treadmill was never implicated.

**Defect 2 — no master ever returns an acknowledgement.** When the L15
originates, the packet reaches the master 122–219 times and the sender is
*still* told `TX_FAILED`. So even with a working transmitter the exchange does
not complete, and this is the defect §7.7 identified.

**A caveat that bounds the first conclusion, and it matters.** Both USB sticks
report `RADIANT0.01B00`; the L15 image was built from HEAD today. The Feather
was reflashed with a current-HEAD image over SWD through the nRF5340 DK's
debug-out header — J-Link found a **Cortex-M4** and programmed it O.K. — but
**neither USB device's behaviour changed afterwards, and both kept their
serials**, so it could not be confirmed that the board just programmed is one of
the two enumerating sticks. Defect 1 is therefore "the two USB sticks, whatever
they are running, do not originate" — **firmware age and SoC are not separated
by this data.** Separating them is the first thing the next session should do,
and it needs a positively identified board (unplug one stick and re-enumerate).

**Also confirmed again here:** the delivered packet repeats at ~4 Hz for the
whole run — 122 packets against 160 of the master's own `EVENT_TX` — long after
the sender reported three failed attempts. Whatever ends the transfer from the
host's point of view does not stop the radio re-sending it.

**Two instrument bugs found here, both of which faked firmware faults:**

- **`body[1]` is `MESG_EVENT_ID` (`0x01`) for an event, not `0`.** Written as
  `== 0`, every terminal event is silently discarded and the tool reports "no
  terminal event" — which reads as a radio or scheduling fault. It cost a whole
  A/B arm before a raw frame dump showed `resp msg=0x01 code=6` going past
  unread. `tools/ant_verify.py` has always had this right; copy from there.
- The same bug in the master-side script reported **0 `EVENT_TX`** while the
  master was transmitting perfectly and a second stick could see it. "The
  master never transmitted" was wrong twice before the dump settled it.
- **Page 17's incline and page 51's grade are encoded differently**, and
  `ant_fec_control.py` initially decoded the first with the second's equation.
  Page 51's *commanded* grade is an unsigned uint16 with a −200.00 % offset
  (flat is `0x4E20`); page 17's *reported* incline is a plain signed int16 with
  no offset. The wrong equation reported a treadmill sitting at **−198.8 %**,
  which is **+1.20 %** read through the offset — absurd enough to catch here,
  and it would not have been had the machine been near +200 %. Fixed; the same
  board now reads `+1.79 %` rising, matching the simulator's ramp.

---

### 7.8 RESOLVED, 2026-08-17 — acknowledged transfers work, and it was four defects in the dongle

An acknowledged transfer now completes on air, end to end, for the first time:
**16 of 18** `TRANSFER_TX_COMPLETED` against 0 of 13 before. Everything in
§7.4a–§7.7a was chasing a receiver that was never the problem — all four faults
are in the dongle stack (`apps/common/ant_radio_radiant.c` and `radiant/`), and
both ends of every failing test were ours.

**The rig that finally separated the failure modes.** Two boards, both running
the same freshly built `apps/dongle`, both host-controlled, and **both ends
counted**:

- **nRF54L15 DK** (`1057737173`), driven over **COM8**, log on COM7;
- **Adafruit Feather nRF52840**, flashed over SWD through the nRF5340 DK's
  debug-out (`1050006310`, confirm **Cortex-M4** in the J-Link output), driven
  over USB as `0FCF:1009` serial **`755972D7183A6183`**.

The second stick, `3D55F77818BE772A`, is a different board still on old
firmware, and it served as a free control arm all afternoon. **That also settles
the board-identity question §7.7a left open.**

`scratchpad/ant_ack_pair.py` runs either end in one of four roles and — this is
the part that mattered — reports *slots transmitted*, *broadcasts received* and
*acknowledged packets received* as three separate numbers. Conflating the first
and third is what produced §7.7's wrong conclusion.

| # | Defect | Where | Evidence it was real |
|---|---|---|---|
| 1 | An originated acknowledged transfer went out at an arbitrary phase, not on the channel's slot | `antr_acknowledge_message_tx()`, `antr_burst_tx()` | master sent 20, slave heard **0** while receiving 150 of its broadcasts; sender failed 0–16 ms after queueing |
| 2 | A master-originated packet (`0xAA`) had no reply mapping, so no RadiANT peer could ever answer it | `radiant_ctrl_reply_for()` | `reply=0x00` in the receiver's own log; `unackable_openers` |
| 3 | A channel with an unwritten broadcast buffer refused to acknowledge — which for a **slave** is permanent | `api_xfer_broadcast()` | staging one broadcast by hand turned 0 received into thousands, with no firmware change |
| 4 | The master's turnaround window reserved no follow-on air for the reply it may have to send | `api_post_master_rx()` | the tracked window sets 1960 µs for the identical reason; matters on MPSL |

**Why all four hid behind each other.** Each one alone is fatal to the exchange,
so fixing any one changed nothing observable, and the surviving symptom
(`FAIL_NO_ACK`) is identical for all of them. Defect 1 in particular made
defects 2 and 3 untestable, because the packet never reached a receiver to be
refused by them.

**Why the test suite did not catch any of it.** 849 tests passed throughout.
`antr_acknowledge_message_tx()` **had no caller in any suite**, so the transmit
instant of an originated transfer had never once been asserted. Defect 2 was
worse than untested — it was *pinned*, by
`test_a_slot_opening_packet_has_no_measured_acknowledgement`, which asserted the
refusal on the principle that refusing keeps an unmeasured gap honest. On
hardware it did the opposite: it made the gap permanent and invisible. That test
is now inverted, and the general lesson is written into it — **a refusal is only
honest if something can still make progress; when both ends of the link are
yours, "we never measured the reply" and "the transfer can never complete" are
the same sentence.** Five tests were added, one inverted.

**The fifth defect, found by fixing the others — RESOLVED, see §7.9.** Once
acknowledged data started arriving, the receiving dongle posted it to its host
**~600 times a second** — 21,234 host messages for 16 exchanges — and its
broadcast count collapsed from ~150 to 4 in the same run. It is not on-air
duplication: the originator's own log shows **169** programmed transmits in 34 s
while the receiver's host saw 12,625 messages. The localisation recorded here —
"somewhere between the scheduler's RX notification and `api_tracked_frame()`" —
was one layer too high; it is in the nRF backend, below both. §7.9 has the
measurement and the fix.

**Regression: the plain "relay sensor data to Zwift" path is unaffected.**
Broadcast-only, same two boards, 62 s: **248 slots transmitted, 244 received —
1.6 % loss**, better than the room's recent 5–8 %. Discovery still opens a
wildcard channel cleanly. The flood above is reachable only by *receiving*
acknowledged data, which no sensor sends to a dongle.

---

### 7.9 RESOLVED, 2026-08-17 — the flood was the radio replaying its last frame

**One defect in `radiant_radio_nrf.c`, and it explains every symptom in §7.8's
"still open" paragraph, including the `+1092 µs` transmit §7.7 could not
account for.** `radio_isr()` turned every `EVENTS_END` on a receive window into
a delivered frame without ever checking that a frame had arrived. Nothing that
the delivery reads is cleared between windows:

| what the callback reports | where it comes from | what it holds when no frame arrived |
|---|---|---|
| `evt.body` | `radiant_rx_buf` | the previous frame, byte for byte |
| `crc_ok` | `RADIO->CRCSTATUS` | the previous frame's verdict — OK |
| `evt.t_sync` | the `CC_SYNC` capture | a timestamp only `EVENTS_ADDRESS` writes |

So an `END` with no address behind it handed the core a **byte-perfect replay of
the last frame received, carrying a stale timestamp**, and the core had no way
to know: it decoded, acknowledged and reported it exactly as it would a real one.

**The measurement that settled it, and why the earlier localisation missed.**
Counters were added at every stage of the receive path — `api_sched_rx()`,
`api_tracked_frame()`, `radiant_transfer_on_data()`, `api_xfer_rx_data()`,
`radiant_event_post_rx()` — and they all read **the same number**. Nothing above
the HAL multiplies anything; the copies were already there when the scheduler
first saw them. That is what put the search below `api_sched_rx()` instead of
between it and `api_tracked_frame()`.

Then the control that admits no other reading: **one board, opened as a master,
with no originator existing anywhere.**

| | before | after |
|---|---|---|
| acknowledged-data messages, 20 s, no peer on the air | **79 (4.0/s)** | **0** |
| acknowledged received per packet sent (8 sends) | **4.0x**, rising to **28.7x** over 20 s | **1.0x** |
| the originator was told | 8x `TRANSFER_TX_FAILED` | 8x `TRANSFER_TX_COMPLETED` |

4.0/s is exactly the channel period, and §7.8's own lesson says to assume any
number near `4 x seconds` is a slot count rather than a delivery count — which
is precisely why the lone-master arm was necessary. With no second radio in the
room, "the originator is still transmitting" is not available as an explanation.

**The fix is one condition.** An `END` on a receive window is a reception only
if `EVENTS_ADDRESS` fired in that window. The flag is already cleared when the
window is armed, it is raised by hardware on a sync-word match, and the frame's
own `t_sync` is read from the capture it drives — so a delivery this test
rejects had no honest timestamp to report either. It is consumed on each
delivery, because `END->START` keeps the receiver armed across frames and one
window may legitimately carry several.

**It also fixed the acknowledged transfers themselves, which was not the goal.**
The same replays were being routed into the *originator's* ack window
(`radiant_transfer_on_rx_event()`), where they corrupted ack matching. Before:
8 of 8 `TRANSFER_TX_FAILED`. After: 8 of 8 `TRANSFER_TX_COMPLETED`. The
`+1092 µs` second transmit on every period was the same defect seen from the
transmit side — a transfer that never completes keeps retrying.

**Verified, both boards carrying the fix** (nRF54L15 DK and the Feather, the
latter flashed over the nRF5340 DK's debug-out — confirm **Cortex-M4**):

- lone master, no peer, 20 s: **0** data messages, on both boards;
- 8 acknowledged sends: **8** received, **8** `TRANSFER_TX_COMPLETED`;
- 20 s settle with the originator's channel *closed*: **0** received;
- broadcast relay, a real 4 Hz power sensor for 60 s: **238 of 238 packets,
  loss (exact) 0.00 %** against a 1.5 % gate — the Zwift path is unharmed;
- `radiant/tests` and `radiant/tests/api` on hardware: **43 suites, 0 failures**.

**The regression check is `tools/ant_ack_pair.py --lone-master`**, and it needs
one board and no originator: open a master, transmit, and require that nothing
is reported as received. A receive path that replays its last frame fails it in
an empty room. `radiant/tests/api` gained
`test_a_tracking_slave_reports_one_message_per_acknowledged_packet` for the
layer above, which passed throughout and is what proved the fault was not there.

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
- ~~**No acknowledged originator exists in this tree.**~~ **Resolved** — this
  bullet carried §7.7's superseded conclusion and was not updated when §7.8
  landed. Acknowledged transfers complete on air in both directions as of
  2026-08-17 (§7.8, §7.9), so the FE-C control loop and §4a's arbitration
  refusal path can both be exercised. What remains untested here is the loop
  against a real machine rather than against our own second board.
- **Nothing tests `src/treadmill_ble.c`.** There is no ztest for the FTMS GATT
  service, for `write_cp()` or for the advertising payload; they are covered
  only by `BUILD_ASSERT`s and §7's bench procedure. That is why the arbitration
  policy was put in a kernel-free module of its own (§4a) instead of inside
  that file, and it is the largest remaining coverage gap in this application.
- **The workqueue conversion is not covered by a test either.** §4a's marshalling
  is what makes the lock-free state and the lock-free token sound, and no ztest
  can see it — `test_both_control_paths_land_on_one_target` drives both paths
  sequentially on one thread, so a pass there proves the *conversions* and says
  nothing about the concurrency. It is verified by inspection and by §7.7's
  bench run, and a pass on that suite must not be read as covering it.
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
