# 0025 — A power proxy instead of a meter, and an LF-anchored timebase that lets the HFXO go

- **Status:** accepted (owner-approved power plan, 2026-11-17; passes merged to
  local master 2026-11-18/19). Every option below is **off by default**. The
  owner closed the two open gate calls on 2026-11-19 (G1 PASS with an accepted
  TI-only residual, G4 PASS on a steady-state reading of the anchor band) and
  deferred TI standby. The same day the owner passed B5 (equal-power rerun)
  and G2 (on master-side timing). Every gate is closed. See *Gates*.
- **Date:** 2026-11-19
- **Decides:** how a board with no current meter reports what sets its current
  (A1/A2), and how the nRF backend keeps a 1 µs timebase with the HFXO stopped
  between operations (B2–B6). Also records the TI half (C1/C2), the arm guard
  that B6 forced onto every MPSL build, and what was deliberately not done.
- **Code:** `radiant/include/radiant/radiant_power_diag.h`,
  `radiant/src/radiant_power_diag.c`, `radiant/src/radiant_radio_nrf_lp.h`
  (the arithmetic), `radiant/src/radiant_radio_nrf_lp.c` (the backend half,
  `#include`d by `radiant_radio_nrf.c`), `radiant/src/radiant_radio_nrf.c`
  (`ARM_SLOT_GUARD`), `radiant/src/radiant_radio_nrf_gate_mpsl.c`
  (`gate_grant_gen()`), `radiant/src/radiant_ti_rf_hwattrs.c`,
  `scripts/jlink_peek.ps1`, `tools/ant_power_est.py`. Kconfig in
  `radiant/Kconfig`: `RADIANT_POWER_DIAG*`, `RADIANT_NRF_LOW_POWER`,
  `RADIANT_NRF_LP_*`, `RADIANT_NRF54L_MPSL_CONSTLAT_HOLD`,
  `RADIANT_CC26XX_XOSC_HF_RELEASE`.
- **Related:** [0013](0013-sweep-is-the-elastic-consumer.md) (denials under
  MPSL), [0021](0021-tracked-slots-are-booked-a-period-ahead.md) (the MPSL
  booking path the guard sits under), [0023](0023-radiant-diag-message.md)
  (why the power proxy is *not* a 0xF7 page).

> **Provenance.** Clean-room. Written from this project's own source, its
> bench archives and its tests, and from the vendors' public SoC headers,
> product specifications and Zephyr/nrfx/TI driver APIs. Nothing here derives
> from sdk-ant, from `libant.a`, from disassembly of any binary, or from any
> ANT+ device profile document. See [0002](0002-clean-room-policy.md).

> **Every current in this record is an ESTIMATE.** There is no current meter on
> this bench. Figures are software duty proxies multiplied by datasheet
> currents (`tools/ant_power_est.py`, each current cited there, UNKNOWN rows
> left out of totals). Treat them as ratios and orders of magnitude, not as
> measurements.

---

## Context

Gate GA (2026-11-18, `archive/benchmarks/2026-11-18-power-ga/`) took the first
baseline of the battery node, `radiant/samples/hr_transmitter` on the
nRF54L15 DK, direct backend. The HFXO ran in 200 of 200 register samples and
the stack's own HFXO request covered 100 % of uptime. CONSTLAT (nRF54L errata
20) was on in every sample. The estimate was **~811 µA**, about 97 % of it two
terms: CONSTLAT ~549 µA and the HFXO ~236 µA. The radio itself was ~12 µA. On
the MPSL `ackps` image CONSTLATSTAT read 0/200: the errata-20 raw trigger does
not survive MPSL (finding 2, pass A4).

The cause was structural. The nRF backend requested the HFXO once at
`radiant_radio_init()` and never released it, because its only timebase was a
free-running 1 MHz radio TIMER clocked from that crystal. A 4 Hz node spends
>98 % of every period waiting, with the crystal and CONSTLAT running.

Two things had to exist before that could change: a way to see the saving
without a meter, and a timebase that survives the crystal stopping.

## Decision

### 1. `radiant_power_diag`: a struct read over SWD, not a host message

`CONFIG_RADIANT_POWER_DIAG` (default n) adds one global, `radiant_power_diag`:
0x58 bytes, every field a little-endian u32, magic `RPD1` (0x52504431),
version 1, pinned. Fields: uptime; the stack's HFXO request (cumulative µs,
requests, releases); radio TX and RX on-time and op counts; CPU active µs and
load (an own idle accumulator on two `TRACING_USER` hooks, because Zephyr's
`cpu_load_get()` floors each window); idle entries; flags; nRF54L
`CONSTLATSTAT`; and five cc26xx fields (RF core on-time, power-ups,
`nPowerUpDuration`, synth errors, radio-timer fold back-steps). A field the
build cannot measure reads 0xFFFFFFFF for the whole run.

- **One writer, a seqlock.** A system-work-queue item publishes a snapshot
  every 1 s (`seq` odd while writing, +2 per snapshot). An SWD reader retries on
  an odd or changed `seq`. The cumulative µs counters wrap at 71.6 min, so
  `CONFIG_RADIANT_POWER_DIAG_LOG_S` is capped at 3600.
- **Why not 0xF7.** A battery node has no host link, and the proxy has to be
  readable on exactly the images that have none. The offsets are
  `_Static_assert`ed. They are the contract with the readers.
- **Zero cost when off.** The hooks are empty static inlines. The stack's
  callers include the header only when the option is on, because even unused
  inline declarations renumbered GCC's `CSWTCH.n` tables and tripped the
  zero-cost symbol comparison.
- **The readers.** `scripts/jlink_peek.ps1` is a read-only J-Link sampler:
  whitelisted commands only, the `DisableAutoUpdateFW` guard first, a DHCSR
  liveness check. It takes N randomised register samples (HFXO running,
  CONSTLAT) with Wilson intervals and dumps a symbol before and after them.
  `-Symbol` and `-ExtraSymbol` read other structs, including
  `radiant_nrf_lp_stats`. `tools/ant_power_est.py` turns two snapshots,
  a `jlink_peek -Json` file or the firmware's own log lines into a
  per-component estimate with a citation per current. The method and caveats
  are in `docs/bench-runbook.md`, "Power without a meter".

### 2. `radiant_nrf_lp_stats`: a second struct, because the first is pinned

`CONFIG_RADIANT_NRF_LP_STATS` (default y with `RADIANT_POWER_DIAG`) adds the
global `radiant_nrf_lp_stats`, magic `RPL1` (0x52504C31). There are two
layouts under one magic, told apart by `size`:

- **0x5C** on the direct build: state, sleeps, wakes, late anchors, late
  wakes, HFXO-late-at-edge-2, re-anchors, range errors, which reading each
  release took, disagreement maxima, wake slack and lateness, ASAP wakes,
  cancelled releases, spurious compares, HFXO errors, `refused`, edge order.
- **0x74** on the MPSL build, which appends `slot_awake_us` (the duty proxy
  there), the START-versus-requested-start cross-check (`start_err_n`,
  min/max/last) and `missed_releases`.

It is a separate symbol because `radiant_power_diag` had no spare field and its
layout is pinned. `g1_lpdecode.py` (under `archive/benchmarks/2026-11-18-power-g1/scripts/`)
decodes both layouts.

### 3. `RADIANT_NRF_LOW_POWER`: release the HFXO, keep time on the LF counter

Off by default, EXPERIMENTAL. With it on, when the next operation is far
enough away, the backend stops the radio TIMER, releases the HFXO and frees
its CONSTLAT reference. It keeps time on the low-frequency counter the system
timer already runs.

- **Crystal only.** `depends on CLOCK_CONTROL_NRF_K32SRC_XTAL`. An RC LF clock
  (±250–500 ppm) cannot time an ANT+ period, and a synthesised LFCLK keeps the
  HFXO running anyway. The backend also refuses at run time if LFCLK is not on
  the crystal.
- **The GRTC is not on LFLPRC.** `depends on !NRF_GRTC_TIMER_SOURCE_LFLPRC`, and
  a run-time refusal when GRTC `CLKSEL` reads LFLPRC (b19ad5f, review
  follow-up). The GRTC picks its own source whatever LFCLK runs from. A refusal
  logs, sets `refused`, and keeps the HFXO on for the whole run.
- **The LF counter seam.** One state machine behind a small counter interface:
  - nRF54L15: the GRTC SYSCOUNTER (1 MHz, extended 64-bit read). The GRTC and
    the radio TIMER share DPPIC20, so the anchor is one same-domain DPPI
    connection.
  - nRF52840: RTC1 (32768 Hz, 24-bit only, two user channels, write lead
    4 ticks). The anchor is one PPI channel with a FORK. The TIMER is SHUT DOWN
    after STOP (nRF52 anomaly 78). The 15-bit RTC build is refused.
  - Other parts with these timers (rest of both families, nRF5340 net core)
    build but are unmeasured.
- **The two-edge wake.** Every hand-over is an LF compare event, so it happens
  on a known LF tick:
  - *Release:* (D)PPI captures the running TIMER into CC5 on the tick. The
    interrupt then halts the TIMER, takes the release and drops the HFXO and
    CONSTLAT.
  - *Edge 1:* an interrupt requests the HFXO,
    `RADIANT_NRF_LP_HFXO_STARTUP_US` + 100 µs before edge 2.
  - *Edge 2:* (D)PPI starts the cleared TIMER on the tick,
    `LP_ARM_LEAD_US` (230 µs) before the operation.

  The TIMER is not started on the HFXO request, because until the crystal is
  up the HF clock is HFINT (~1 %), and the error would be kept every period.
  Only LF compare events anchor the TIMER: spike B1 showed a GRTC CAPTURE
  triggered while the CPU sleeps returns a stale SYSCOUNTER, so it is never
  used. Start-up defaults come from spike B1's p99.9 (800 µs nRF54L15, 500 µs
  nRF52840), else the devicetree figure, else MPSL's 1400.
- **When it sleeps.** Only when the next operation is strictly more than the
  wake lead + the LF overhead + `RADIANT_NRF_LP_KEEP_ON_US` (3000) away:
  4349 µs on the GRTC, 4167 µs on RTC1. Also not for
  `RADIANT_NRF_LP_LINGER_MS` (1000) after enable or any `PRIO_NORMAL` acquire,
  so searches, acquisition and bursts never sleep. Every failure path is
  counted. A late anchor or a late wake fails the operation through the
  existing FAILED terminal and never transmits late. An HFXO error drops the
  option to always-on for the rest of the run.
- **Accepted behaviour change.** Each operation after a sleep depends on edge
  1's interrupt about 1 ms earlier. Anything that holds the CPU across it costs
  that slot (counted as a late wake).

### 4. The MPSL variant: the timeslot START is the wake

On `RADIANT_BACKEND_NRF_GATE_MPSL` (pass B6, GRTC parts only) none of the
edges exist. The crystal belongs to MPSL. Every timeslot is requested with
`MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED`, which **is kept**, so MPSL starts
the HFXO before each START and holds it to the end.

- The backend drops its own boot-time HFXO request once the option runs.
- At each `SIGNAL_START` it reads the GRTC in the zero-latency signal and
  restarts its TIMER from zero on that tick. This is why the MPSL variant needs
  the GRTC: an RTC1 read there would busy-wait up to a 30.5 µs tick.
- It stops the TIMER at the ACTION_END funnel and keeps time on the GRTC until
  the next START. A missed release is caught at the next START or a backstop
  and counted.
- There is no compare, no (D)PPI connection, no added interrupt, and no late
  path: a START either comes on a stable crystal or does not come.

`RADIANT_NRF54L_MPSL_CONSTLAT_HOLD` defaults to **n** (40d6961). Pass A4 put
MPSL on nrfx's CONSTLAT reference count (`NRFX_POWER`). The A4 bench recheck
then showed MPSL's per-event acquire already covers our timeslots: CONSTLAT
was on in 90 % of samples at 89 % RX duty, and the references stayed balanced.
An always-on hold would cost ~549 µA (ESTIMATE) for nothing measured.

### 5. The arm guard and the grant generation, on every MPSL build

B6's review found that an arm served inside a held grant (a follow-on from the
software interrupt, or an arm from a preemptible thread) could be preempted
between `gate_acquire()` and `program_*()` by the zero-latency ACTION_END.
Resumed, it would write MODE, FREQUENCY, SHORTS and the rest into the other
stack's live RADIO and enable TIMER→RADIO connections inside its event. That
is the class of fault that asserts the SDC. Because the class did not depend on
the option, the fix was applied to every MPSL build (a8ad20d, dd28bde, 446b9a8;
merge 88134ca). It changes default MPSL images.

- `gate_grant_gen()` (`radiant_radio_nrf_gate_mpsl.c`) is odd only while a
  timeslot's loan of the radio runs. It moves at every START and every
  hand-back.
- The claim records the generation (`arm_gen`) and marks an arm in progress,
  both under the zero-latency lock.
- `program_tx/rx/ed` run their whole register span under PRIMASK. The span
  first checks that the generation is still the claim's and still live;
  otherwise it writes nothing and fails ETIME.
- A zero-latency abort, or a deferred denial, that lands mid-arm is left to
  that arm, so the operation gets one outcome. A denial that lands while the
  arm stages is held, not dropped.
- With the option on this replaces B6's own TIMER generation.

The 2026-11-19 correction: in production the wedge was **latent**. Every
scheduler arm runs inside `pass()` under `irq_lock`, so only the MPSL ZLI can
preempt it. It was reachable from out-of-pass callers (spikes) and the ztest
fakes. On air (archive `2026-11-19-mpsl-arm-onair`, image 41f6e951): 0 asserts,
loss 4.59 % against the TI's 5.57 % in the same window, and 58/59/60 of 60
acked transfers.

### 6. The TI side: hooks, then an XOSC_HF release, off by default

- **C1** (6efbbe4): radiant defines its own `RFCC26XX_hwAttrs`
  (`radiant_ti_rf_hwattrs.c`, `RADIANT_CC26XX_RF_HWATTRS`, never set by hand).
  Its global callback feeds the `ti_*` fields.
- **C2** (b7d4bd6): `apply_power()` now acts, and logs, only when the requested
  TX power changes (it called `RF_setTxPower` on every transmit), and the
  review follow-up caches the power only when the RF core was active (dcd2e23).
  The option itself is `RADIANT_CC26XX_XOSC_HF_RELEASE`, **default n**, which sets
  `xoscHfAlwaysNeeded = false`. TI's RF driver then requests the 48 MHz crystal
  at each RF core power-up and releases it at power-down. Between slots the
  system runs on RCOSC_HF. It is enabled only in
  `radiant/samples/hr_transmitter/ti_lowpower.conf`. It is refused at compile
  time, as policy, in every image with an ANT host link (a dongle's RF core is
  rarely off, and a host link on RCOSC_HF is untested), on the cc2652p_dongle,
  and on 802.15.4/BLE coexistence builds.

### 7. Not done, deliberately

- **TI standby.** There is no `CC26XX_LOW_POWER` option. GC1 confirmed H1: one
  RF core power-up per slot, core on ~1.8 %. The CC2652's estimated idle floor
  (~675 µA, no standby) now dominates the TI. The estimator's `--ti-standby`
  what-if gives ~227 µA, but this firmware has no standby. That is a separate
  decision.
  **Deferred by the owner, 2026-11-19 (`CONFIG_PM` on the TI).** Standby needs
  a power policy, a timer wake for the next slot and a UART rework. The
  easy step is already taken by `RADIANT_CC26XX_XOSC_HF_RELEASE` (C2).
- **nRF5340 network-core low power.** RTC1 on the net core builds through the
  same seam but is unmeasured. Spike S1 retention there was not run.
- **Adaptive API wake.** The API thread still polls (20 Hz). Blocking on events
  and arming a deadline only when a retry, search or watchdog is pending
  (TODO.md) was left for when it becomes visible next to the radio's duty.
- **The H3 root cause** (see *Gates*). No nRF-side fix was found. A TI
  FREQ_TRIM / RX bandwidth sweep was proposed and not run; with G1 accepted
  (2026-11-19) it is not planned. The receiver counter that found the
  mechanism is kept behind `RADIANT_DIAG_CRC_BITS` (default n, 0xF7 page 12
  fields 5-7) for a later look.

## Gates

All **power figures are ESTIMATES**. Loss figures are on-air counts in a room
whose loss floor drifts by several percent, so each gate is judged against
same-window controls.

| Gate | What | Verdict |
|---|---|---|
| GA | baseline, L15 hr_transmitter direct | HFXO 100 %, CONSTLAT on, **~811 µA**. On MPSL, CONSTLATSTAT 0/200 → A4 |
| A4 recheck | MPSL CONSTLAT coverage | MPSL's acquire covers our timeslots → `CONSTLAT_HOLD` default n |
| G0 (spike B1) | hardware facts | PASS: restore list empty, anchor ±1 µs via LF COMPARE → TIMER START, stale GRTC capture trap, HF vs LF −30 ppm (L15) / −58.5 ppm (Pro Micro) |
| GC1 | TI power-diag | H1 holds (1 power-up/slot, core on ~1.8 %, `pudur` ~448 µs). Fold finding 4 not reproduced (0 back-steps over 9705 power cycles) |
| C2 | TI XOSC_HF release A/B/A | **PASS.** `pudur` 448 → 480 µs (one AON RTC tick); cold/late 0; TI received 99.05/99.37/98.75 % of acked messages. The saving cannot be priced: the XOSC_HF row is UNKNOWN. B costs +0.8 ‰ CPU and +12 wakes/s |
| G1 | L15 direct, option on, A/B/A | Power and timing PASS: HFXO request 1.684 % (4.06 req/s × 4.15 ms), CONSTLAT off between slots, all LP fault counters 0, period −7.4 µs (the LF offset). ~811 → **~36–41 µA**. **FAIL on TI receiver loss** (B 1.84/2.54 % vs A 0.41–1.11 %) |
| G1 re-judge | same, TI with the CRC-repair delivery fix | **Still FAIL, narrowly.** B 0.69/0.69 % vs A 0.21–0.45 %. Every CRC failure is now repaired and delivered. The residual is TI-only RX_FAIL (B 34 of 58 pooled, p = 0.003). The Pro Micro shows no B excess (p = 0.30). **Owner, 2026-11-19: PASS with accepted TI-only excess** (TI-receiver sensitivity; no FREQ_TRIM sweep) |
| B5 | Pro Micro (RTC1), option on | **Not passed: confounded control.** Duty 1.6 %, ops 4.06/s, period −13.9 µs (−56.4 ppm, matches the LF crystal). Loss 4.4 % against a +8 dBm control image (1.1 %), while B transmits at 0 dBm. Needs an equal-power A/B |
| B5 rerun | Pro Micro, equal TX power (0 dBm both arms), A1 B1 A2 B2 | Confound removed: at 0 dBm with the HFXO held, the TI and Garmin already lose 3.8-7.7 %. B2 matches A2 on all three receivers; B1 sits above A by about the A1-A2 drift. RSSI equal across arms. Spike S3 lp rerun PASS (HFXO off before 10 000 of 10 000 requests, anchors within ±1.125 µs). **Owner, 2026-11-19: PASS** (archive `2026-11-19-power-b5-rerun`) |
| G2 (B4) | trainer FE-C master on the L15, option on; TI slave, then Garmin + TI A/B/A/B/A | ACK rate and bursts unchanged against same-session controls; HFXO request duty 2.4 %. The literal 2186-2191 µs band cannot be judged: the Garmin's reply wanders ~100 µs and the TI answers at ~2207 µs. The master-side part is unchanged within 0.3 µs. The +7.6 µs burst-opener shift is the RadiANT slave placing its opener from the nominal period (TODO item). **Owner, 2026-11-19: PASS on master-side timing** (archives `2026-11-19-power-g2`, `2026-11-19-power-g2-garmin`) |
| G4 | MPSL hrm_ble + BLE connected, 1 h | **Passed except the anchor band as written.** 0 asserts, 0 disconnects, TI loss 1.63 %, `slot_awake` duty 1.207 %, no own HFXO request after boot, ~38 µA for the parts the counters see (SDC events and MPSL's ramp excluded). `start_err` sits at a steady **−4 µs** (band 0–17). The since-boot extremes −33/+31 µs come from boot STARTs and BLE connect setup, and did not move during the hour. **Owner, 2026-11-19: PASS** - the band is read as steady state only; the −4 µs steady offset is accepted and the boot/connect swings are one-off events |

**H3, the G1 mechanism** (archive `2026-11-18-power-g1-h3`): B does not add a
new kind of error at the TI. It raises the rate of the one error A already has,
**the frame's last bit (or last-but-one) sent as 0 and received as 1**,
single-bit and CRC-repairable, after each HFXO restart. RADIO trims, HFXO
INTCAP and the XO status word are unchanged across a restart. PHYEND is set
before DISABLE in both images, and a 50 µs TX tail did not help. The likely
reading is the transmitter's carrier frequency at the frame's end seen through
the TI demodulator's delay. This is not proven. The finding uncovered a
default-build bug that repaired frames were dropped at the event CRC gate, fixed
in 22c75e3 / 28d7754. Since that fix only the RX_FAIL residual above remains.

## Consequences

- A battery node can now **show** its saving without a meter, and the proxy
  is the same code on nRF and cc26xx. The estimate is only as good as the
  datasheet rows. Where a row is missing (cc26xx XOSC_HF), the tool says
  UNKNOWN rather than guess.
- With `RADIANT_NRF_LOW_POWER`, the nRF54L15 transmitter's two dominant terms
  fall by about 20× (ESTIMATE). What remains is radio, CPU and floor, so the
  next saving is on the API thread's polling, not the clock tree.
- Default images change in one place only: the MPSL arm guard. Every other
  part of this record is behind a default-n option.
- **Regression guard:** `test_idle_without_near_op_requests_no_hfxo` and its
  positive control `test_op_due_inside_idle_bound_requests_hfxo`
  (`radiant/tests/backend/src_lp/`, scenarios `radiant.backend.lp` and
  `radiant.backend.lp_rtc`). No HFXO may be held or requested while nothing is
  due within 5 ms, and the same check must see the crystal when something is.

## Verification

- ztest: `radiant.lp_clock` (the arithmetic, with drift simulations and
  negative controls), `radiant.backend.lp` / `.lp_rtc` / `.lp_mpsl` (the
  backend against simulated GRTC, RTC1, HFXO and TIMER), `radiant.gate.lp`, the
  grant-generation cases in `radiant.gate` and `radiant.backend`,
  `radiant/tests/src/test_power_diag.c`. Full native_sim suite on the Bluefin
  server at the wrap-up: 6908 cases in 21 scenarios, all passed.
- Bench: the archives named in *Gates*, under `archive/benchmarks/2026-11-1[89]-*`.

## Open

- ~~G1's TI-only RX_FAIL residual and G4's −4 µs anchor offset: owner calls.~~
  Closed 2026-11-19: both accepted by the owner (G1 PASS with accepted TI-only
  excess, G4 PASS on steady state). TI standby: deferred (section 7).
- ~~B5: equal-power A/B on the Pro Micro; spike S3 lp rerun.~~ Closed
  2026-11-19: both rerun, owner PASS (archive `2026-11-19-power-b5-rerun`).
- ~~G2 absolute turnaround.~~ Closed 2026-11-19: owner PASS on master-side
  timing. The RadiANT slave's burst opener uses the nominal period (+7.3 µs
  against this master); recorded in TODO.md, not planned.
- G4 found that hrm_ble BLE rows set `MPSL_TIMESLOT_SESSION_COUNT` 1 while D2
  takes 2 (`flash_sync_mpsl` −12, identity falls back to devnum 4242).
  This is unrelated to power but was found here. **Fixed 7f2974f**
  (BLE rows default the count to 2 with D2).
