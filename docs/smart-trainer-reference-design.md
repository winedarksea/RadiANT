# Smart trainer reference design (`apps/smart_trainer`)

A controllable bike trainer that speaks **ANT+ FE-C** (device type 0x11,
equipment type 25) and **BLE FTMS Indoor Bike** at the same time, from one
image. A manufacturer replaces the simulator with a brake driver and supplies
identity strings. Everything else stays as it is.

This app is not `apps/trainer`. That one is the FE-C simulator and ACK-fault
instrument used for bench work, and it refuses BLE by design. The two share one
file: `apps/trainer/src/trainer_model.c`, the FE-C §8.8.4 physics behind this
app's simulated source. It is compiled in place, not copied.

## 1. Build

| Build | Command (`-Dsmart_trainer_EXTRA_CONF_FILE=`) | Rows |
|---|---|---|
| ANT+ FE-C only | (none) | `trainer_l15`, `trainer_52840dk` |
| FE-C + FTMS, development identity | `ble.conf` | `trainer_ble` |
| **Shipping preset** | `"dual.conf;product.conf"` | `trainer_dual`, `trainer_dual_52840` (with `tests/product_test.conf`) |
| Bare preset (must fail) | `dual.conf` | `trainer_dual_fail` |
| Custom source, nothing registered | `source_custom.conf` | `trainer_custom` |

The prefix is the application directory's basename (`smart_trainer`). If the
prefix is wrong, the build accepts it silently and the image has no BLE. See
`apps/treadmill/ble.conf`.

`dual.conf` turns on `CONFIG_RADIANT_PRODUCT_STRICT`, so it refuses to
configure until `product.conf` (copied from `product.conf.example`) supplies a
real manufacturer ID, names and BLE name.

## 2. The seam: `src/trainer_source.h`

A driver fills in `struct trainer_source_ops` and registers it from a
`SYS_INIT` at APPLICATION level:

| Hook | Called for | NULL means |
|---|---|---|
| `set_target_power(W)` | FE-C 49, FTMS 0x05 | no ERG: the FE-C page 54 bit and FTMS TSF bit 3 are cleared |
| `set_resistance(‰)` | FE-C 48, FTMS 0x04 | no basic resistance |
| `set_sim(&params)` | FE-C 50/51, FTMS 0x11, FTMS Reset (flat road) | no simulation |
| `set_user(&user)` | FE-C 55, FTMS 0x12 | no wheel-circumference target; page 55 is still stored and echoed |
| `workout(w)` | FTMS 0x07/0x08 | Start/Stop answer 0x04 (they are mandatory op codes) |
| `start()` | once at boot | optional |

The driver pushes `trainer_source_report()` (power, cadence, speed, brake
force, ERG-speed-low, pedalling) and `trainer_source_report_event()` (STARTED,
PAUSED, STOPPED, CONSOLE_STOP, CONSOLE_RESET, PAIRING_WINDOW, FORGET_BONDS).
Both may be called from any thread. Every hook runs on the system workqueue.

The units are transport-neutral, and the header lists them. Resistance is in
per mille of the brake's maximum. FTMS resistance levels 0.0 to 20.0 (UINT8,
0.1 resolution) map linearly onto 0–100 %. One 0.1 step is 0.5 %, which is
exactly FE-C page 48's step, so the two radios share one scale.

## 3. Success semantics, epoch and arbitration

**What a result reports.** A command's result reports that the **hook
accepted** it, and nothing more:

| Hook returns | FTMS result | FE-C page 71 |
|---|---|---|
| 0 | 0x01 | PASS |
| `-EINVAL`/`-ERANGE` | 0x03 | FAIL |
| any other error | 0x04 | FAIL |

Training Status and the FE state nibble follow the driver's **reports**. If a
trainer accepts Start and nobody pedals, the session does not change.

**The session.** States are IDLE, RUNNING, PAUSED and STOPPED (`trainer_equip.h`):

- A report with `pedalling` set starts the session, **from IDLE only**.
  PAUSED and STOPPED are left only by FTMS Start/Resume, a STARTED event or
  a console action (the treadmill's rule).
- The STARTED, PAUSED and STOPPED events move it.
- FTMS Reset and a console reset return it to IDLE.
- FTMS Start while RUNNING answers 0x01 and does nothing: the core's
  `caps.passive` flag, set by the trainer.
- An event drops a driver report still pending from before it
  (`trainer_inbox.h`), so a stale pedalling report cannot undo a Pause.

**The epoch.** A trainer is passive, so there is no motion authorisation. The
session epoch still applies: a **local** stop or reset (CONSOLE_STOP,
CONSOLE_RESET) and a successful FTMS Reset bump it. An FE-C page or FTMS procedure received before the
bump is refused (FE-C REJECTED, FTMS 0x04) without reaching a hook. Stop and
Pause are never refused for this.

**Arbitration** goes through `radiant_ctrl_arbiter`, with all implemented
control pages gated:

- An FTMS Request Control is explicit and pre-empts an FE-C controller's
  implicit claim. That controller's next command is REJECTED.
- An idle FE-C claim lapses after `SMART_TRAINER_CTRL_ANT_TIMEOUT_MS`.
- The arbiter is not a suppression rule. FE-C pages and FTMS notifications
  always reach every listener.

## 4. Control Point and FE-C mapping

| FTMS op | Hook | Status on success |
|---|---|---|
| 0x00 Request Control | (arbiter claim, BLE) | — |
| 0x01 Reset | the brake release (below) | 0x01 Reset; control released; FTMS view zeroed; epoch moved; no Stopped status |
| 0x04 Set Target Resistance | `set_resistance` | 0x07 |
| 0x05 Set Target Power | `set_target_power` | 0x08 |
| 0x07 Start/Resume | `workout` | session report → 0x04 Started |
| 0x08 Stop/Pause | `workout` | session report → 0x02 Stopped/Paused |
| 0x11 Indoor Bike Simulation | `set_sim` (drafting kept) | 0x12 |
| 0x12 Wheel Circumference | `set_user` | 0x13 |

| FE-C page | Hook | Answer |
|---|---|---|
| 48, 49, 50, 51 | as above | page 71 PASS / FAIL / REJECTED, with bytes 4–7 echoed (Table 8-49) |
| 55 | `set_user` | none: page 55 is not a control page (§8.12.3.1) |
| 70 | — | the requested page is scheduled; never a page 71 |

A target that FE-C sets also notifies FTMS subscribers (0x07, 0x08, 0x12 or
0x13). Every status carries the value the hook applied (a clamped grade, for
instance), not the one requested.

**The brake release** (`trainer_cmd_release_brake()`), run by FTMS Reset and
by a console reset, is chosen from the *declared* capabilities:

| Declared | Action | State after |
|---|---|---|
| simulation | `set_sim(flat road)` | free ride |
| basic resistance | `set_resistance(0)` | free ride |
| ERG only | `set_target_power(minimum)` | ERG at the minimum |
| none | refused (FTMS 0x04) | unchanged |

An FE-C control page the trainer does not support is **ignored**, as
§8.12.3.1 requires: no page 71 is sent and no sequence number is used. FE-C
status 2 (NOT_SUPPORTED) is one the FE "shall not use".

## 5. FE-C transmitter (`radiant_profile_fec_tx`)

**Pages sent.**

- Rotation: pages 16, 25 and 17 in the §10.1 interleave.
- Common pages 80/81, from `radiant_product`.
- On request: 17, 50, 51, 54, 71, 80 and 81, and **55** once the node has
  called `radiant_profile_fec_tx_set_user()` (this app always does; apps/trainer
  and apps/treadmill do not, so they still answer -ENOTSUP).
- One on-request slot: a second request before the first is served replaces
  it.

**Page 70 handling**, via `radiant_profile_fec_tx_handle_request()`:

- Only command type 1 is served.
- Byte 5 = 0x00 (reserved as invalid) is ignored.
- **The acknowledged-reply bit:** FE-C §10.3 [MD_0006] says the FE "shall
  only send broadcast messages ... and shall never send acknowledged or burst
  messages". So the reply is broadcast, the requested number of times. 0x80
  ("until acknowledged") sends four broadcast copies.

**Page 71** reports the last control page (48–51), never a page 70
(§8.12.3.1). The sequence number counts supported commands from 255, as
§8.12.3.2 says.

## 6. FTMS Indoor Bike Data (0x2AD2)

| Field | Encoding |
|---|---|
| Instantaneous speed | 0.01 km/h |
| Instantaneous cadence | 0.5 /min |
| Total distance | uint24 m |
| Resistance level | sint16, **resolution 1** (FTMS 1.0 Table 4.9), on the 0–20 level scale |
| Instantaneous power | W |
| Elapsed time | s |

These are 15 bytes in one notification. The Fitness Machine Feature bits are
exactly those fields: cadence, total distance, resistance level, power
measurement and elapsed time.

**Supported ranges:**

- Resistance Level Range: 0.0–20.0, step 0.1.
- Power Range: `SMART_TRAINER_MIN/MAX_POWER_W` (0–2000 W), step 1, or the
  driver's own range if it states one.

Service Data advertises machine type Indoor Bike (bit 5).

**FTMS Reset** zeros the FTMS view of elapsed time and distance through a
baseline. The ANT+ accumulators are untouched.

## 7. Identity and bonding

Per-unit device number and serial come from `radiant_node_ident` (ADR 0009 and
0020) through `radiant_product_unit_boot()`. A strict build stays off the air
until identity boots. One `radiant_product` record feeds:

- ANT+ pages 80/81;
- the Device Information Service.

Bonding uses the pairing window (`CONFIG_RADIANT_BLE_PAIRING_WINDOW`):

- New phones can bond only inside a window opened at the trainer, with
  `TRAINER_EVT_PAIRING_WINDOW` (the simulator's sw0).
- Holding sw0 for 5 s forgets every bond.
- `dual.conf` also opens a 120 s window at power-on. Drop that once your
  trainer has a pairing control.

The simulator's DK buttons: sw0 pairing, sw1 rider on/off, sw2 console stop,
sw3 console reset.

## 8. Production checklist

1. Write the brake driver. Implement exactly the hooks the brake honours,
   because the capability bits are derived from them.
2. Copy `product.conf.example` to `product.conf`, fill it in, and build with
   `dual.conf`.
3. State `max_resistance_n` (FE-C page 54) and the power range in the ops.
4. Report `pedalling` and the brake force, so FE-C page 17's resistance is
   valid. §10.1.2.6 makes it mandatory on a trainer.
5. Add a pairing control, then drop `RADIANT_BLE_PAIRING_WINDOW_AT_BOOT_S`.
6. Run `scripts/ble_central.ps1 -Mode ftms-conformance -Machine bike` and the
   FE-C bench tools against the unit.

## 9. Tests

`apps/smart_trainer/tests/glue` (native_sim) covers:

- the equipment state;
- the seam-to-model glue;
- every FE-C and FTMS command path, through the real FTMS core and the real
  arbiter;
- the page 70 → 54/55/71 round trips, through the real FE-C master.

The FE-C transmitter's own cases are in `radiant/tests/src/test_profile_fec_tx.c`.

## 10. Not done

- An optional Cycling Power Service (plan item 5). It is not built.
- Calibration (FE-C §6.5). It is not declared, and page 25's calibration bits
  stay clear.
- Nothing here has been on air yet.
