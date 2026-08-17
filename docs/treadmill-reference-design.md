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

> **The cause was found, 2026-08-17, and it is a real capability gap in
> `apps/common` rather than a rig problem.** "The dongle cannot originate an
> acknowledged transfer" was true, and the reason is that the transfer was
> never **slot-aligned**. `radiant_transfer.h` states the contract at
> `radiant_transfer_submit()` — *"a slave answering its master passes
> `master_t_sync + RADIANT_TRANSFER_SLOT_REPLY_US`"* — and the sole production
> caller, `antr_acknowledge_message_tx()`, passed `RADIANT_TIME_NEVER`.
> `radiant_burst.c` resolves that to `radiant_radio_now() + min_lead_us +
> ARM_SLACK_US`, an arbitrary phase. The peer master only listens for ±250 µs
> around `t_sync + 2190 µs` (`api_post_master_rx()`), so the packet landed where
> nothing was listening, the reply window closed empty, and the engine failed it
> once with `FAIL_NO_ACK` — which it deliberately does not retry.
>
> **The 0/4 against the commercial Wahoo is the proof, not the confounder.** No
> bug in any node in this tree could make a read-only page 70 to somebody else's
> trainer fail. The control that this section treats as inconclusive is in fact
> the thing that identifies the transmit instant as the only possible cause.
> Fixed in `apps/common/ant_radio_radiant.c` (`api_slave_reply_t_sync()`); the
> instant now comes from `radiant_channel_next_slot()`, which is the same
> prediction `api_post_track_rx()` already arms tracked windows around.

If the fix ever needs decoupling from treadmill verification, the sdk-ant
Feather image at `build/release/dongle/zephyr/zephyr.uf2` is a known-good
originator and the Feather sits in its UF2 bootloader, needing no double-tap.
That is a rig workaround and not a substitute for the fix.

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

**UNBLOCKED, 2026-08-17** — §7.4a defect 1 was a measurement artefact (§7.4b);
the node advertises and a connection can be made. The measurement below is what
is still owed.

One number is in, and it is the reassuring half: with **both ANT+ masters
running and the BLE stack linked in and initialised** (`tread_ble`), the FE-C
channel still delivered 78 packets over 25 s at 3.7 % loss. That is BLE
*present*, not BLE *connected*, so it bounds nothing about the connected case —
but the two masters plus an initialised second stack do not by themselves break
the ANT+ side.

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
