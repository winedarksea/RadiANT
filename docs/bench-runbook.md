# Bench runbook: the three sittings that need a person

**Status:** written 2026-08-15, after a session that finished every part of the
production-hardening plan that did *not* need hands on the hardware. These three
did. Everything here has been set up already — images built, rigs assembled,
traps found — so the human time is the sitting itself and not the preparation.

Read `tools/ab_gates.toml`'s header first. It carries the measurement discipline
and, more importantly, four conclusions that were reached and then overturned in
one afternoon. Starting from those saves repeating them.

---

## Before anything: is the bench fit?

**Do not skip this and do not read past a failure.** `ab_gates.toml` requires
A1 and A2 of a sitting to agree inside `repeat_a_max_delta_pp = 0.35`, and its
own rule is that a wider spread *voids the sitting* — so a bench that cannot
repeat produces numbers that look like results and are not.

As last measured, it cannot. Five 300 s repeats with nothing touched between
them read 3.07, 2.22, 1.79, 3.75, 3.92 % — a range of 2.13 pp, six times the
gate. **Sitting 1 below is the attempt to fix that**, and until it passes,
sittings 2 and 3 are not worth starting.

Two warnings that nearly caught the last person:

- **Three points can show a trend that does not exist.** The first three of
  those readings fall monotonically and read exactly like a warm-up settling on
  a usable floor. The next two go back up.
- **Do not pick the passing pair.** Consecutive deltas were 0.85, 0.43, 1.96 and
  0.17 pp. The last passes the gate by luck. Choosing it is the same act as
  moving the threshold, which is the failure `ab_gates.toml` exists to prevent.
  A1 and A2 are chosen *before* they are read.

---

## Sitting 1 — move the master to nRF52 silicon (Phase 1.7 Tier 2 precondition, and Phase 2.7(b))

**Why:** transmit power is the one factor shown to move both the loss and the
repeat spread on this bench — rebuilding `apps/sim` from 0 dBm to +4 dBm took
loss from 4.87–7.94 % to 3.07/2.22 % and the spread from 3.07 pp to 0.85 pp.
**Level 5 (+8 dBm) is nRF52-only.** An nRF54L master cannot reach it, and every
historical figure — the 0.85–1.37 % in `ANT_SIM_TX_POWER`'s help and the
0.26–0.60 % floor in `archive/benchmarks/` — was taken with a Feather
transmitting. This is what §7.4.3 of `docs/radiant-bridge.md` asks for.

**The one manual step, and there is no way around it.** The Feather has no
debugger and a running release image offers no route into its own bootloader —
verified, see `SECURITY.md`. **Double-tap RESET** to mount `FTHR840BOOT`.

```powershell
# 1. Master image is already built. Rebuild at +8 dBm for the Feather:
. .\scripts\env.ps1 -Bundle fd21892d0f -NcsVersion v3.2.4
Push-Location C:\ncs\v3.2.4
west -z C:\ncs\v3.2.4\zephyr build -s <repo>\apps\sim -d <repo>\build\sim_feather `
     -b adafruit_feather_nrf52840/nrf52840/uf2 --sysbuild -p always -- `
     -DANT_MODULE_DIR=C:/Users/Colin/sdk-ant `
     -Dsim_CONFIG_ANT_SIM_DEVICE_NUMBER=33333 -Dsim_CONFIG_ANT_SIM_TX_POWER=5
Pop-Location

# 2. Double-tap RESET on the Feather, then:
Copy-Item build\sim_feather\sim\zephyr\zephyr.uf2 E:\

# 3. DUT is the L15 over UART. Flash the dongle image and take the repeats:
.\scripts\p4_flash_only.ps1 -Dir h_l15
python tools\ant_verify.py --port COM8 --profile power --expect-watts 100 `
       --expect-rpm 80 --device-number 33333 --seconds 300 --json bench-logs\a1.json
# ...repeat for a2.json, unchanged, immediately after.
```

**Pass:** `|A1 − A2| ≤ 0.35 pp` and `unexplained_loss` 0 on both. Then the
thresholds stand and sittings 2 and 3 can proceed.
**Fail:** the remaining factor is not transmit power. Do *not* re-derive the
thresholds from this bench — that bakes an unexplained fault into the product's
acceptance criteria, which is the one outcome to avoid.

**Pin `--device-number` on both ends, always.** `#14871` is
`CONFIG_ANT_SIM_DEVICE_NUMBER`'s default, so a wildcard acquire can silently
lock onto some other board running this project's own sim firmware. That
happened, and reported 87 W against 100 before anyone noticed.

**The sim console's `B-PWR tx page 16` counter is not evidence of radiation.**
It is the profile's page handler on a timer and advances whether or not the
channel is on the air. It looked like evidence for most of an afternoon.

**On nRF54L the log timestamp free-runs across a soft reset**, so a climbing
uptime after a reflash does *not* mean the board failed to reset. Read the boot
banner.

---

## Sitting 2 — the ADR 0001 A/B/A (Phase 1.7 Tier 2)

Only after sitting 1 passes. The full sequence is in the plan's §1.7 and is not
repeated here; `docs/testing.md` is the authority. The short form: Feather as
DUT, three flashes (sdk_ant → core → sdk_ant), nothing moved between them,
then `python tools/ant_ab.py --gates tools/ab_gates.toml a1.json b.json a2.json`.

Expect `gates.scale` to read **SKIP** — legitimately absent for sdk-ant, and a
SKIP must not be recorded as a pass.

---

## Sitting 3 — Tier 3, the Zwift ride (Phase 1.7)

Not automatable, and ADR 0001 says so in as many words. The clause is precise:
*"Zwift pairs a power meter, a heart-rate monitor and a controllable trainer
against a radiant build, and holds a 30-minute ride with resistance changes
taking effect throughout."* Tier 1+2 are necessary and not sufficient.

Windows, `core` on the **Feather** — not the nRF52840 Dongle, whose "82
EVENT_TX and transmits nothing" anomaly is undiagnosed. That board is
nevertheless a release artifact, so before shipping a `core` release somebody
must at minimum confirm its DFU image enumerates and pairs.

**On pass:** edit ADR 0001 (status, date, the run that did it) *then* the
release attachment in `release.yml` and `build-core`'s comment. The ADR says the
edit is the mechanism, not the paperwork.

---

## Not a sitting: the liveness STALE test

Already done — see the commit "Liveness verified on hardware". Recorded here
only so nobody books bench time for it. It needs no hand:
`CONFIG_ANT_DONGLE_PAIRING_WINDOW_AT_BOOT=y` opens the pairing window without
`sw0`, and "powering the master off" is killing `ant_sim.py`.

```
self_channels: bound device 33333 (type 0x78, trans 5) on channel 7
               as source 0, period 8070
bridge_pump: liveness: 3 binding(s) went STALE (total 3, no_period 0)
```

---

## Comparing against a reference dongle

`tools/ant_compare.py` puts RadiANT receivers head to head with a reference
stick (the Garmin ANT USB-m) and grades each one Equal, Better, Worse or
Inconclusive against rules fixed in the tool before any data exists. It does
not need the bench to be fit in the sense above, because it never compares
two sittings: every receiver hears the same DK master in the same window, so
only the gap between a receiver and the reference counts. The room's
5-8 % moves everyone together. It still costs statistical power, though: at
high loss the Wilson intervals widen and more verdicts come out Inconclusive.
That is the honest result, not a reason to loosen the margin.

```powershell
# 0. Re-read COM ports and the dongle's serial, edit them into the rig, then
#    shake out each phase before the real run:
python tools\ant_compare.py --rig tools\compare_rigs\2026-11-07-fixed.toml --dry-run
python tools\ant_compare.py --rig tools\compare_rigs\2026-11-07-fixed.toml `
       --phases identity,loss --rounds A --set phases.loss.seconds=30 `
       --set phases.loss.repeats=1 --out $env:TEMP\compare-shakeout
# 1. The full run (about 2.5 h, unattended once it starts):
python tools\ant_compare.py --rig tools\compare_rigs\2026-11-07-fixed.toml
```

Its phases are identity, loss (×2), multi, acquire, ladder, acked and usb. The
results directory holds every child's JSON and log, `summary.json` and
`summary.md`. Re-running a subset into the same directory replaces only those
records; `--report-only` re-derives the verdicts without touching hardware.

**A re-placement round is a copy of the rig file.** Edit `[placement]`, any
addresses that moved, and `meta.results_dir`, then rerun with
`--phases loss,ladder`. Antenna and position are part of every figure until a
swap round separates them, so compare against the first run's placement
block, never its numbers alone.

Traps this tool already handles, so don't work around them by hand:

- **It never kills `ant_sim`.** A killed sim leaves its master channels on air.
  Each transmitter runs to its own end, and every phase starts by resetting
  each DK with `ant_probe` until rc 0 (the 5340's first probe after a flash
  often fails).
- **Device numbers are pinned per round and phase** (5340 → 53xxx, 54L15 →
  54xxx), never 14871, so a leftover master is never acquired by mistake. A
  window whose receiver heard a different number is flagged.
- **A short window is flagged**, because a dropped COM port can produce a
  truncated run that still looks like a pass. `wall_s` is each receiver's own
  run time, so this fires for every receiver. (It used to be read in launch
  order, which gave every receiver after the first the slowest one's time, and
  that hid the 54L15's VCOM death at 245 s.)
- **A receiver that reboots or loses its port is named.** UART builds send an
  unsolicited STARTUP at boot carrying the hardware reset cause (0x00
  power-on, 0x01 pin or debugger, 0x02 watchdog, lockup, software reset or
  crash), and ant_verify records one that arrives mid-window as
  `receiver_reboots`. A port that dies still writes partial JSON, with
  `port_death` giving the time and the error.
- **`log_port` on a `[[device]]` captures that board's console for every
  window** to `<window>_<device>_console.log`, with every line stamped in host
  time. To classify a drop, compare that log with the ANT side's
  `port_death` time and with `Get-WinEvent` from Kernel-PnP. A console that
  dies at the same moment points at the probe or USB; a fault dump or boot
  banner points at the firmware. An unknown device key is warned about, not
  dropped silently.
- **ant_compare and every tool here use a 1 s USB read, never a short one.**
  Under libusb-win32 a read timeout that lands as a frame arrives loses that
  frame whole. STARTUP used to arrive 52 ms after SYSTEM_RESET, so a 50 ms read
  lost it about 1 % of the time (TODO.md item 2). TODO item 5 cut that latency
  to a few ms, but any reply can land on a short read's expiry. `tools/ant_reset_loop.py`
  measures this.
- **Transmit power changed after 2026-11-07.** A RadiANT ant_sim master
  (which never sets power) runs at the part's maximum, which is now +8 dBm on
  the 54L15 (it was +4) and +5 on CC26xx (it was +4). Signal figures from a
  54L15 master are therefore not comparable with earlier runs. An explicit
  level 3 is now a true 0 dBm.
- **The fine ladder sends every rung as a raw register value**, named levels
  included, so no bridge level mapping can move a rung. (Sent as LVL_4, its
  +4 rung would now measure +8.) The levels ladder still sends named levels,
  so against a RadiANT transmitter its LVL_4 rung reads at the ceiling.
- **The 54L15's fine ladder bottoms out at −28 dBm.** Its −40/−46 dBm settings
  are 11-bit register values, and the wire's custom TX-power byte is one byte.
  If no knee is reached, move the transmitter away; don't extrapolate.

---

## Power without a meter

There is no current meter on this bench, and the DKs have none built in. Every
power figure here is an **ESTIMATE**: a software proxy multiplied by a typical
current from the vendor's datasheet. Use these numbers to compare arms of one
build measured the same way. They are not absolute currents, and nothing here
can be quoted as one.

### The method

1. **Build the image with the proxy.** Add `power_diag_quiet.conf` (no console;
   the `radiant_power_diag` struct is read over SWD) or `power_diag.conf` (a
   `power dt_ms=...` log line every 10 s) to the sample's
   `-DEXTRA_CONF_FILE`. The struct holds free-running counters: HFXO request
   time, radio TX/RX on-time and op counts, CPU active time, idle entries and
   the CONSTLAT state. Its layout is in
   `radiant/include/radiant/radiant_power_diag.h`.
2. **Sample the hardware and dump the struct, with the target running:**

   ```powershell
   .\scripts\jlink_peek.ps1 -Preset nrf54l15 -Serial 001057737173 -Samples 200 `
       -ElfPath build\<dir>\hr_transmitter\zephyr\zephyr.elf -Decode -Json pd.json
   ```

   It reads `CLOCK.XO.STAT` and `POWER.CONSTLATSTAT` 200 times at random
   intervals of 150–450 ms, so the samples cannot phase-lock to the 246 ms slot.
   It reports each as a fraction with a 95 % Wilson interval. It dumps the
   struct before and after the sampling run, about 60 s apart. It never resets
   or halts the core: `connect` plus `mem32` only, behind the
   `exec DisableAutoUpdateFW` guard and a read-only whitelist. Check that the
   run reports `liveness: running` and that `power_diag uptime advanced` matches
   the planned sleeps. Exit 3 means J-Link mentioned security, erase or unlock.
   Stop there and answer nothing. Never point it at TI.
3. **Turn the counters into an estimate:**
   `python tools\ant_power_est.py --board nrf54l15dk pd.json`. It differences
   the two snapshots and takes the sampled HFXO duty and CONSTLAT fraction from
   the same file. It prints a cited row for each component and leaves out any
   current it could not verify (marked UNKNOWN). `--hfxo-row X2` swaps the HFXO
   row for the crystal-core standby current, which gives a lower bound. With
   `--log console.txt --uart-on` it reads the firmware's log lines instead.
4. **Confirm the arm is on the air.** Leave the struct alone for this check
   and run `tools\ant_verify.py --profile heart-rate --device-number 12345` on
   another board. A counter showing radio ops proves the scheduler ran them. It
   does not prove a receiver heard them.

Caveats:

- **Datasheet rows are typical values at 25 °C and 3 V.** The model adds radio
  and CPU time as if they never overlap. The HFXO row is "TIMER10 at 32 MHz on
  HFXO" (ITIMER3), which runs higher than the stack's 1 MHz TIMER20. Both push
  the total high.
- **The probe is attached while it samples** (DHCSR `C_DEBUGEN` reads 1). A
  debug session can hold clocks or power domains that the target would release
  on its own. Today that changes nothing, because the stack holds the HFXO for
  the whole uptime. Once the HFXO is released (Track B), compare the sampled
  duty with the struct's `hfxo_req_us` before trusting either. One thing the
  probe does *not* do is force CONSTLAT on: the MPSL image below reads 0/200
  with the same probe attached.
- **`power_diag_quiet.conf` already turns off LOG, SERIAL, CONSOLE and
  UART_CONSOLE**, the same four symbols `lowpower.conf` sets. "Default + quiet"
  and "lowpower + quiet" therefore build byte-identical images, and that is
  what happened at Gate GA (both `2a130085`). The arm with the console on is
  `power_diag.conf`.
- **The nRF54L15 datasheet has no UARTE current**, so a console-on estimate
  leaves out the console's real cost. All the struct can show is the CPU side.
- **`hfxo_pm` and `cpu_pm` in the log line are floored to whole per-mille.** A
  log-derived HFXO duty therefore reads 99.92 %, not 100 %. Use the struct for
  sums.
- **Every arm of the sample transmits as device 12345, type 0x78.** Two
  transmitters on the air at once (the L15 and the TI, say) cannot be told
  apart by a receiver. Keep only one transmitting while you verify.

### Baseline, Gate GA (2026-11-18): hr_transmitter on the nRF54L15 DK

All arms are local master 78864b2, direct radio (no MPSL), at 0 dBm, with a
bidirectional MASTER at 8070/32768 s unless noted. Each line is one SWD
session: 200 register samples, plus struct snapshots about 60 s apart starting
at 33 s uptime (arm A started at 16 s). The receiver was the Pro Micro
(apps/dongle diag image), about 60 s per arm. Raw data and images are in
`archive/benchmarks/2026-11-18-power-ga/`.

| Arm (image md5) | HFXO on, sampled (95 % CI) | CONSTLAT | TX µs/s | RX µs/s | TX / RX ops/s | CPU active ‰ | Idle entries/s | Est. total µA (ESTIMATE) | Received by Pro Micro |
|---|---|---|---|---|---|---|---|---|---|
| A default + quiet (`2a130085`) | 200/200 (98.1–100 %) | 200/200 on | 603.9 | 2597.5 | 4.068 / 4.068 | 4.14 | 25.35 | **811** (608 with crystal-core HFXO) | 245/245 |
| B lowpower + quiet (`2a130085`, same image) | 200/200 (98.1–100 %) | 200/200 on | 603.9 | 2596.8 | 4.066 / 4.066 | 4.14 | 25.36 | **811** (608) | 245/245 |
| C lowpower + one-way + quiet (`921c146a`) | 200/200 (98.1–100 %) | 200/200 on | 600.3 | 0 | 4.064 / 0 | 3.22 | 21.29 | **800** (597) | 244/245, 1 RX_FAIL |
| D default + `power_diag.conf`, console on (`b869d171`) | 200/200 (98.1–100 %) | 200/200 on | 603.1 | 2595.6 | 4.064 / 4.064 | 5.56 | 25.34 | **815** + UART UNKNOWN (612) | 243/245 |

Breakdown of arm B's total (ESTIMATE, µA). The rows are cited in the tool
output (nRF54L15/L10/L05 Datasheet v1.0):

| Component | Basis | µA |
|---|---|---|
| CONSTLAT (ION_IDLE11 − ION_IDLE0) | on 100 % | **549** |
| HFXO outside radio (ITIMER3; crystal-core X2 = 34) | 99.68 % | **236** |
| CPU active (IAPPCPU0, 128 MHz) | 0.41 % | 10.8 |
| Radio RX (IRADIO_RX0), one 639 µs window per slot | 0.26 % | 8.8 |
| Radio TX 0 dBm (IRADIO_TX0), 148 µs per broadcast | 0.06 % | 2.9 |
| Idle floor (ION_IDLE8) | always | 2.9 |

What the gate showed:

- **The HFXO is held ≈100 %.** The stack's own request counter reads 100.000 %
  of uptime and the register reads 200/200, on every arm. This is the
  regression baseline for Track B.
- **CONSTLAT is on in every direct build**: `constlat_stat` = 1, flags 0x3F.
  Together with the HFXO, it is ~97 % of the estimate. Radio, CPU and the floor
  come to ~25 µA.
- **One-way removes the listen and nothing else.** RX ops go from equal to TX
  ops to 0. Arm C also has exactly 4.06 fewer idle entries per second (one per
  slot) and 0.9 ‰ less CPU. Only ~11 µA of the estimate moves. The RX window
  (639 µs) agrees with finding 6's ~670 µs.
- **Wakes are not "slots + API thread + snapshot".** That sum predicts
  4 + 20 + 1 ≈ 25/s for both arms. One-way measured 21.3/s, so the TX slot
  does not add a separate wake. It probably shares one with the API thread's
  50 ms poll, which an event re-phases (a hypothesis, not checked). The listen
  window adds one wake per slot.
- **The log line agrees with the struct.** In arm D, the ten 10 s lines read
  5929–6096 TX µs (struct 603 µs/s), 25536–26199 RX µs (2596 µs/s),
  idle_x10 251–254 (25.3/s), `cpu_pm` 4–6 (5.6 ‰), `hfxo_pm` 999/1000 and
  `constlat=1/1`. The console costs +1.4 ‰ CPU (~4 µA ESTIMATE) plus a UARTE
  current that cannot be estimated.
- **Arms A and B form a same-image repeat.** Every rate agrees within 0.1 %.

### MPSL builds: CONSTLAT does not stay on

On the same day, before it was reflashed, the L15 carried the MPSL ackps image
(apps/dongle, `CONFIG_MPSL=y`, `build/ackps_l15_mpsl`, `zephyr.hex` md5
`666f64f4`). Sampled with the same tool, it read:

| | Sampled | 95 % CI |
|---|---|---|
| HFXO running | 200/200 | 98.1–100 % |
| CONSTLATSTAT on | **0/200** | 0–1.9 % |

The backend still raw-triggers CONSTLAT at init (`NRF54L_ERRATA_20`), yet the
hardware reads it off. So the errata-20 workaround is **not in force on MPSL
nRF54L15 builds**. That is finding 2 of the power plan, now confirmed. The
image had no power_diag, so the struct could not show whether the trigger ran.

The mechanism, from the image's own disassembly (Pass A4): that build has
neither `CONFIG_NRFX_POWER` nor `CONFIG_NRF_SYS_EVENT`, so MPSL's
`mpsl_low_latency_acquire_callback()` / `_release_callback()`
(`nrf/subsys/mpsl/init/mpsl_init.c`) are raw writes to `TASKS_CONSTLAT`
(0x5010E030) and `TASKS_LOWPWR` (0x5010E034), not a reference count. MPSL's
scheduler calls acquire before each event it dispatches and release after it
(it may skip the release between back-to-back events), and the first release
turned the backend's init-time CONSTLAT off. Pass A4 makes the MPSL gate
select `NRFX_POWER` on nRF54L, which moves MPSL's callbacks onto nrfx's
reference count. With `CONFIG_RADIANT_NRF54L_MPSL_CONSTLAT_HOLD=y` (the
default) the backend also takes one reference at init that it never frees, so
CONSTLAT stays on as in a direct build. With `=n` it takes none and leaves
CONSTLAT to MPSL's per-event acquire.

Whether MPSL's acquire covers its *timeslots*, where all of this backend's
radio work runs, is **not verified**. The disassembly shows a single acquire
site in the scheduler's event-start path, but the library is closed. The GA
0/200 does not settle it either, because the dongle may have had no channel
open. The bench check: flash the `=n` diag image, keep the radio busy with a
never-locking scan (`tools\ant_scan_rate.py --port COM4 --baud 57600
--seconds 300 --scan-type 0x40`), and sample during the scan with
`jlink_peek.ps1 -Preset nrf54l15 -Samples 200 -ElfPath <elf> -Decode
-ExtraSymbol m_power_mode_refs`. A CONSTLAT-on fraction near the scan's radio
duty, with `m_power_mode_refs` 1 in exactly those samples, means the timeslots
are covered. 0/200 means they are not, and `=y` has to stay. On the `=y` image
expect 200/200 and `m_power_mode_refs` 1 idle, 2 inside an MPSL event.

### TI hr_transmitter (no proxy yet)

The `ti_hr_tx` row (`f2c7d5c6`) was flashed with `scripts\flash_ti.ps1`
(dslite, then the backchannel reset) and heard by the Pro Micro for 300 s. It
received 1216 of 1219 packets: 4.053/s against a nominal 4.0605/s. All three
missing packets were RX_FAIL, i.e. lost on the air. Radio-clock timing was
0.007 ms off the slot grid, signal −43 dBm, 120 bpm exact. The TI has no
power_diag figures until the C1 pass fills its fields, so no estimate is given
here.
