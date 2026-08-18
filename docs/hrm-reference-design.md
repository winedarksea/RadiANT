# The heart-rate node: a manufacturer reference design

Checked by: `.github/workflows/release.yml`'s `build-node` job (every configuration
named in §2 and §4 is built there and its `.config` asserted symbol by symbol),
`apps/hrm_ble/tests/sim_sched` (§6's first drift check), `apps/hrm_ble/sample.yaml`
and `scripts/build_all.ps1`'s node rows. **§7's bench procedures are narrative** —
they name numbers that were measured once, on one bench, and nothing re-measures
them.

`apps/hrm_ble` is an ANT+ heart-rate monitor built entirely out of `radiant` and
`radiant/src/profiles/`. It is the design a manufacturer copies.

---

## 1. What this is, and what it is not

**What it is.** A complete ANT+ device type 0x78 sensor: the clean-room link
layer, a real profile with page 0x00's event count and event time, the common
pages 0x50/0x51 a head unit reads, the hostless identity record of ADR 0009, and
— optionally — the SIG Heart Rate Service over BLE on the same radio, arbitrated
through MPSL timeslots. It is the first and still the only image in this
repository where RadiANT transmits as a **master** with a profile on top.

**What it is not.**

- **It is not a product.** §5's checklist has nine items and at least two of them
  cannot be completed on any board in this tree today (§5.1).
- **It is not a bench instrument any more, but it was one.** It began as
  `strap/`, written to give the BLE coexistence work (P8/P9) a node whose ANT+
  side is a master — a dongle, being a slave, cannot stand in, because a master
  owns its slot phase. Everything about the intervals in §3 comes from that
  origin and is load-bearing, not vestigial.
- **It is not Garmin-compatible beyond ANT+.** The BLE half is the standard SIG
  service, not Garmin's proprietary Multi-Link/GFDI, and there is no running
  dynamics. See `ble-running-dynamics-notes.md`.
- **It carries no RadiANT extension pages.** The compat layer is linked and
  inert: `profile_hr_set_compat()` is never called, so on the air this is a plain
  ANT+ 0x78 sensor. That was a P8b requirement — a strap used to measure BLE
  coexistence must broadcast exactly what a commercial strap broadcasts — and it
  is also the right default for a reference design.

---

## 2. Build and flash

```powershell
. .\scripts\env.ps1 -Bundle dcbdc366a1 -NcsVersion v3.4.0
Push-Location C:\ncs\v3.4.0
west -z C:\ncs\v3.4.0\zephyr build -s <repo>\apps\hrm_ble `
     -d <repo>\build\node -b nrf54l15dk/nrf54l15/cpuapp -p always `
     -- -DRADIANT_BACKEND=nrf
Pop-Location
```

`west` must run with its working directory inside the workspace even though
`-s`/`-d`/`-z` all point elsewhere: `build` is an extension command discovered
through the manifest.

### The configurations

| Fragment | What it adds |
|---|---|
| *(none)* | ANT+ only. The baseline, and where a port starts |
| `ble.conf` | SIG HRS over BLE, plus the MPSL gate |
| `ble.conf;ble_nosuppress.conf` | …and ANT+ keeps transmitting while connected — the arm that actually stresses the arbiter |
| `sensor_example.conf` | the seam's second implementation instead of the simulator |
| `sensor_custom.conf` | no implementation of the seam at all — the integrator's starting point |

### The one flag people get wrong

```
-Dhrm_ble_EXTRA_CONF_FILE=ble.conf
```

**The prefix is the basename of the application directory**, because that is how
sysbuild derives an image's name. While this directory was `strap/`,
`-Dstrap_EXTRA_CONF_FILE=` was right; after the rename the same flag named an
image that does not exist. CMake said nothing beyond its end-of-run
"manually-specified variables were not used" warning, `CONFIG_BT` stayed unset,
`radiant/Kconfig`'s `depends on !BT || RADIANT_BACKEND_NRF_GATE_MPSL` was
satisfied by its `!BT` half, and **the BLE image built green with no BLE in it.**

That is a measured claim, not a plausible one: two builds, one with the flag and
one without, produced **byte-identical `.config` files**, the with-flag one
recording `# CONFIG_BT is not set`.

Three things now stand between that and a repeat, and none of them is a comment:

1. `build-node` in CI asserts the resulting `.config` symbol by symbol. A
   build-only check passes the no-op without noticing; only a read-back does not.
2. `CONFIG_HRM_BLE_HRS` `select`s the MPSL gate and `default`s
   `MPSL_TIMESLOT_SESSION_COUNT` to 1, so turning BLE on carries the interlock
   with it however it is turned on.
3. `apps/hrm_ble/CMakeLists.txt` fails the configure with a named message if
   `CONFIG_BT` is set without `CONFIG_HRM_BLE_HRS`.

A bare `-DEXTRA_CONF_FILE=` is no better than the wrong prefix — it lands in the
sysbuild cache and never reaches the image — and neither is a bare `-DCONFIG_x`.
That is why even the sensor choice travels as a conf fragment.

---

## 3. The sensor seam

`apps/hrm_ble/src/hrm_sensor.h` is the one boundary a manufacturer implements.
Read its header before anything else in the application; the reasoning below is a
summary of it.

### Shape

```c
struct hrm_sensor_api {
	int  (*start)(const struct hrm_sensor *sensor);   /* required */
	int  (*stop)(const struct hrm_sensor *sensor);    /* optional */
	void (*poll)(const struct hrm_sensor *sensor);    /* optional, 100 ms */
};

int  hrm_sensor_register(const struct hrm_sensor *sensor);
void hrm_sensor_report_beat(uint16_t event_time_1024);
void hrm_sensor_report_rate(uint8_t bpm);
uint16_t hrm_sensor_now_1024(void);
```

### Why it is push, and why not the two obvious alternatives

**Push (source-driven), not poll.** A real detector is an ISR on an analogue
front end's comparator line; it knows *when the beat happened*. An application
tick only knows when it last looked, and cannot recover the difference. The
direction is the design.

**Not Zephyr's `sensor` API.** `zephyr/drivers/sensor.h` has no heart-rate
channel and no beat trigger, so a heart-rate driver must invent an unnamed
`SENSOR_CHAN_PRIV_START + n` — which is a smell, but not the reason. The reason
is that it is a *poll-a-scalar* model and a beat is an *edge*. A sensor-API strap
re-derives the beat instant from whatever tick did the fetch, which is
structurally the same mistake that produced the 72-versus-66.7 bpm drift bug
this application already shipped once (§6). Adopting it would have made that bug
the architecture.

**Not a weak symbol.** A weak default a manufacturer overrides is one signature
typo away from silently keeping the default — which here means a shipped chest
strap reporting a constant simulated 72 bpm.

### Why the seam is here and not lower

`profile_hr_beat(&hr, event_time)` plus `profile_hr_set_computed(&hr, bpm)` is
precisely what ANT+ page 0x00 carries. A seam that did not match the wire format
would make every integrator write the same adapter to get back to it.

### The concurrency rule

`profile_hr` has no lock and is **already** reached from two contexts: the system
workqueue, which advances the accumulators, and the radiant callback path
(`antr_on_message()` → `load_next_page()` → `profile_hr_next()`), which reads
them. An ISR-driven sensor would make that three-way.

So `hrm_sensor_report_beat()` does not touch `profile_hr` at all. It writes one
32-bit `{seq, event_time}` word with a single atomic store — one word so the
sequence number and the instant it belongs to can never tear apart — and submits
one work item. The handler runs on the workqueue (context 1, which already
existed) and does `profile_hr_beat()`, `profile_hr_set_computed()` and
`hrm_ble_notify_hr()` together, so a receiver on either radio cannot see a rate
the other does not.

The consumer compares sequence numbers with modular arithmetic, never with `<`,
which is why 16 bits suffice. The word is **single-producer by construction**:
exactly one sensor registers, so exactly one context reports. That is what
`hrm_sensor_register()`'s `-EEXIST` exists to keep.

### The Kconfig is a `choice`, and that is the safety property

`HRM_SENSOR_SIMULATED` (default) / `HRM_SENSOR_EXAMPLE` / `HRM_SENSOR_CUSTOM`.

A `bool` ("enable the simulator") lets a manufacturer forget to turn it off. A
`choice` makes that unrepresentable: exactly one arm compiles, exactly one
`hrm_sensor_register()` call links, and the boot log names which.

Under `HRM_SENSOR_CUSTOM` **nothing in this tree implements the seam**. If your
code does not call `hrm_sensor_register()`, the node logs an error and transmits
`PROFILE_HR_INVALID_BPM` forever. That is the correct failure and is deliberately
**not** a fallback to the simulator: a strap quietly reporting a simulated rate
on a real chest is invisible from the receiving end; a strap reporting nothing is
not.

Note that `PROFILE_HR_INVALID_BPM` is **0**. On this profile `0xFF` is a real
255 bpm and is not a sentinel.

### A worked example

```c
/* my_afe.c - a driver for a real front end. */
#include "hrm_sensor.h"

static void afe_comparator_isr(const struct device *port, ...)
{
	/* If your AFE latched the edge into a timer capture, convert THAT to
	 * 1/1024 s and pass it. hrm_sensor_now_1024() is the fallback, not the
	 * goal: the whole reason the seam takes an instant is so a driver that
	 * has a better one can use it. */
	hrm_sensor_report_beat(hrm_sensor_now_1024());
}

static int afe_start(const struct hrm_sensor *s)
{
	/* Power the AFE, configure the comparator, enable the interrupt. */
	return 0;
}

static void afe_poll(const struct hrm_sensor *s)
{
	/* 100 ms, thread context. Contact detection, AGC, battery. Compute your
	 * averaged rate here if you average one - and see §6, because your
	 * averaging code is at least as likely to disagree with the accumulator
	 * pair as the scheduler was. */
	hrm_sensor_report_rate(my_averaged_bpm());
}

static const struct hrm_sensor_api afe_api = {
	.start = afe_start,
	.poll  = afe_poll,
};
static const struct hrm_sensor afe_sensor = {
	.name = "ACME AFE-1",   /* logged at boot, verbatim */
	.api  = &afe_api,
};

static int afe_register(void) { return hrm_sensor_register(&afe_sensor), 0; }
SYS_INIT(afe_register, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
```

Build it with `-Dhrm_ble_EXTRA_CONF_FILE=sensor_custom.conf` and add your `.c` to
`apps/hrm_ble/CMakeLists.txt`. `src/hrm_sensor_example.c` is the same shape,
compiled in CI, with the ISR-context rules spelled out inline.

---

## 4. Porting to your board

There is no board file that will be right for your hardware, so the checklist is
worth more than any of the three in `apps/hrm_ble/boards/`.

1. **`chosen { radiant,radio-timer = &timerN; }`, and the TIMER must have at
   least five CC channels.** The backend needs `t_sync` capture, RX start, window
   close, a software now-capture and TX start; receive-start and transmit-start
   cannot share a compare, because a DPPI event may publish to only one channel.
   On nRF52 that means **TIMER3 or TIMER4** and it is a compile-time `#error`
   otherwise. On nRF54L, TIMER20. Name it in devicetree rather than in source, so
   that a second user is a devicetree conflict instead of two drivers
   reprogramming each other's prescaler. Set `status = "okay"` too — every TIMER
   on nRF54L is disabled by default, and an unclocked peripheral is not
   addressable, which the backend once found by taking a bus fault on its first
   register write.
   - ⚠ On nRF54L, `mpsl.rst` lists MPSL as owning TIMER10 **and** TIMER20 while
     `mpsl_hwres.h` reserves only TIMER10. TIMER20 was **measured** not to
     collide on the nRF54L15, and TIMER22 behaved identically. If a future MPSL
     revision claims it, this is the line that moves.
2. **A `storage_partition`,** because `CONFIG_SETTINGS_NVS` backs the identity
   record and `radiant/src/node/Kconfig` makes `RADIANT_SEC` non-optional — every
   part of `node_ident` derives keys, down to the boot counter's epoch. A node
   without it is a node with no identity, which is the one thing ADR 0009 says a
   node must have.
3. **The LFCLK source.** The ANT+ channel period is 8070 counts of 32768 Hz; an
   RC oscillator's tolerance shows up directly as a rate a receiver has to
   re-track.
4. **The 22-character device-name budget,** if you build the BLE half. See §5.5.

`apps/hrm_ble/boards/` has `nrf54l15dk` (the one board any of this has run on),
`nrf54lm20dk` and `nrf52840dk`. **The last two are build-verified only.** The
nRF52840 entry exists because `RADIANT_BACKEND_NRF` depends on
`SOC_COMPATIBLE_NRF52X || SOC_COMPATIBLE_NRF54LX` and until it was added only the
nRF54L half was ever compiled — on the family most existing ANT+ silicon actually
is. `radiant/Kconfig` records that the nRF52840's on-air behaviour is *predicted*
from the nRF54L15 measurements and has not been confirmed. Do not read a green
build as a working node.

---

## 5. The production checklist

Nine items. Items 3 and 4 are **not achievable on any board in this tree today** —
see §5.1.

1. **Manufacturer ID ≠ 255.** `CONFIG_HRM_MANUFACTURER_ID` defaults to 255, the
   ANT+ development ID. A shipped product needs a real one from the alliance;
   using someone else's is claiming someone else's.
2. **`model_number`, `hw_revision`, `sw_revision`.** All four are hardcoded to
   `1` in `main.c`, and common pages 0x50/0x51 are what a head unit's
   device-information screen displays. They are placeholders, and they look
   exactly like real values from the outside.
3. **Device number from the identity record, not from Kconfig.**
   `CONFIG_HRM_DEVICE_NUMBER` is a bench fallback so a freshly-flashed DK works
   without a provisioning step. ADR 0009 makes the number a property of the
   node's identity, not of the image.
4. **`K_dev` provisioned at manufacture,** per ADR 0009.
5. **The 22-character BLE name budget.** Against the 31-byte legacy advertising
   payload: flags `1+1+1 = 3`, UUID16 list `1+1+2 = 4`, name `1+1+N = 2+N`, so
   `9 + N ≤ 31` and **N ≤ 22**. `src/hrm_ble.c` used to claim "~24" from memory
   and was wrong. Over the budget, `bt_le_adv_start()` returns `-EINVAL`, and
   `main.c` calls `(void)hrm_ble_start()` — so an over-long product name ships a
   node that transmits ANT+ perfectly and **never advertises**, with the only
   evidence a log line on a board with no console attached. There is now a
   `BUILD_ASSERT`.
6. **Beat-schedule drift.** See §6 — this is the item with the history.
7. **The suppression policy.** `CONFIG_HRM_SUPPRESS_ANT_WHEN_CONNECTED` defaults
   on: a connected phone is already getting the rate over the SIG service, so the
   ANT+ broadcast is a duplicate. Turning it off is the minority-manufacturer
   behaviour (a watch and a phone served at once) and is also the only
   configuration in which the arbiter is genuinely exercised. Decide it
   deliberately; it changes what your device costs in air.
8. **Never ship `-DRADIANT_BACKEND=null`.** A null-radio node boots, logs
   `transmitting`, and puts nothing on the air — indistinguishable from a dead
   antenna. `radiant_assert_backend()` in the application's `CMakeLists.txt` and
   the `.config` read-backs in CI and `build_all.ps1` are what catch it; the
   failure has cost this project a Feather flash and a whole session, twice.
9. **The honest gaps.** This application has **no signing, no watchdog and no
   version string.**
   - Signing: whatever the target's bootloader supports. On UF2 targets the
     answer is nothing at all and physical access is the trust boundary.
   - Watchdog: **deliberately absent, and not an oversight.** On a node with a
     4.06 Hz transmit obligation *and* an MPSL timeslot user, the feeding policy
     has to tie to the slot rather than to a 1 Hz thread. Get it wrong and you
     ship a strap that resets during a BLE connection, which reads from the far
     end as an intermittent radio — the most expensive class of symptom to chase.
     A watchdog added as a hygiene checkbox here is worse than none.
   - Version: there is no `VERSION` file in this repository yet, so
     `APP_VERSION_STRING` is unavailable.

### 5.1 The gap items 3 and 4 depend on

**Nothing in `apps/` calls `node_ident_provision()`.**

`radiant/src/node/node_ident.c` implements provisioning; no application invokes
it. So on every board in this tree the Kconfig fallback is the only branch that
ever runs, and checklist items 3 and 4 are unachievable — not difficult,
unachievable.

Closing it is a **manufacturing-process decision, not a firmware one**, and it
has at least three shapes with different consequences:

- a bench-only `PROVISION_ON_FIRST_BOOT` that refuses to compile in a release
  configuration;
- a separate one-shot provisioning image;
- a factory step that writes `K_dev` through the programmer.

**It should be scoped as its own phase with its own ADR.** This document names
the gap so that it is a recorded decision rather than something discovered at
manufacture; it does not close it.

---

## 6. Drift: three checks at three costs

This is the bug that escaped 681 ztest cases.

The beat schedule advanced on a 100 ms tick with an 833 ms interval (72 bpm).
Written as `next_beat_ms = elapsed_ms + interval_ms` it looks equivalent to
`next_beat_ms += interval_ms` and is not: `elapsed_ms` is quantised to the tick,
so re-anchoring turns 833 ms into 900 ms and compounds the rounding instead of
cancelling it. **A 60 s BLE hold measured 67 HR notifications against a claimed
72.** It was found on a bench, with a stopwatch, by accident.

Three checks, deliberately at three different costs:

1. **A unit test on the now-pure scheduler — free, every CI run.** This is the
   check that did not exist. `src/hrm_sim_sched.c` is a pure function with no
   globals, no kernel calls and no clock, precisely so that
   `apps/hrm_ble/tests/sim_sched` can drive 36 000 virtual ticks through it in
   microseconds on any board. It asserts 4322 beats per hour at 72 bpm and
   exactly 72 in every minute after the first (the first carries the anchoring
   beat at *t* = 0). It also reimplements **the bug** locally and asserts that
   variant produces exactly **67** in the first minute and 4000 in the hour — so
   the test demonstrably discriminates between the bug and the fix, rather than
   merely agreeing with whatever the code currently does. The suite runs because
   the `ztest` job passes `-T app/apps/hrm_ble/tests`; `tests/expected_counts.yaml`'s
   scenario floor is what says so if that argument is ever dropped.
2. **A 60 s bench hold counting HRS notifications** — the measurement that found
   it. §7.
3. **A debug-build runtime self-check** that byte 7 agrees with `61440·n/Σdt`
   within 5 %. *Not implemented.*

**The general lesson is the third one**, and it is the reason this section is in
a manufacturer's document rather than only in a commit message: **the computed
field and the event-count/event-time pair must agree.** A manufacturer's own
averaging code is at least as likely to break that as the scheduler was, and
nothing in ANT+ will tell them — the two fields simply disagree, and a head unit
believes whichever one it happens to read.

---

## 7. Verifying on a bench

**Narrative.** These procedures were run once, on one bench, and nothing
re-measures them.

- **ANT+ side.** `tools/ant_verify.py` against a dongle, reading `loss (exact)`
  and never wall-clock, `timing` and never `jitter`. The measurement discipline
  in `testing.md` applies unchanged.
- **BLE connections.** `scripts/ble_central.ps1` from the Windows host radio. It
  is single-user and is needed at the same time as the nRF54L15 DK, so the two
  cannot be split across sessions.
- **Advertising rate: use nRF Connect Mobile, not the host radio.** The Windows
  BLE stack does not surface per-advertisement timing.
- **Budget ≥ 90 s before calling a node missing, and this is the number people
  get wrong.** At a 1009 ms advertising interval a scanner saw **18
  advertisements in 120 s**, and a 12 s scan and a 40 s scan **both found
  nothing** while the node was demonstrably on the air. A short scan finding
  nothing is not evidence of a problem.
- **The beat-count hold.** 60 s connected, count HRS notifications, expect 72 at
  the default rate. This is check 2 of §6 and is what found the drift bug.

### Why the intervals are the policy, not a power setting

`CONFIG_HRM_BLE_ADV_INTERVAL_MS` defaults to 1000 and
`CONFIG_HRM_BLE_MIN_CONN_INTERVAL_UNITS` to 800 (1 s), and shortening either is
**not a power/latency trade — it is a decision to take air away from ANT+.**

MPSL gives applications only `PRIORITY_NORMAL`, and other MPSL users — the
SoftDevice Controller among them — rank **above anything an application can
request**. There is no setting that makes ANT+ outrank BLE. So RadiANT's priority
is delivered by *shaping the other stack's demand*, which is exactly what
`nrf/samples/esb/esb_ptx_ble` does with its 1 s connection interval.

A phone will ask for far less — iOS asks for tens of milliseconds — and accepting
hands the controller a recurring high-priority event for the life of the
connection. `src/hrm_ble.c` refuses the update in `le_param_req` and logs it, so
a degraded node is traceable to a phone connection rather than looking like a
radio fault.

The cost of the long interval is discovery latency, and that cost is the ≥ 90 s
floor above. It is a real trade, made deliberately, in one direction.

---

## 8. Known limits

- **The nRF52840 and nRF54LM20 board files are build-verified only.** No bench
  time on either.
- **`radiant`'s nRF52840 on-air behaviour is predicted, not confirmed.** The
  address arithmetic and packet configuration were measured on nRF54L15; the
  ramp-up and turnaround figures are from the datasheet, and the two `t_sync`
  calibration constants are seeded from Zephyr's per-SoC BLE chain delays rather
  than measured.
- **The BLE half has never been compiled, let alone run, on an nRF52.** SDC,
  MPSL and the gate together on that family is untested.
- **No provisioning path** — §5.1.
- **No signing, watchdog or version string** — §5, item 9.
- **The MPSL gate's BLE arm is the less-measured one.** `radiant-bridge.md`
  records that bug 22's `elastic_skew_us` removal "has only been measured against
  OpenThread. Re-measure the BLE arm before treating the advertiser case as
  settled." `ble_nosuppress.conf` is that arm.
- **Wider RadiANT limits** — discovery, loss floor, the sweep gap — are the
  dongle's, and live with it rather than here.
