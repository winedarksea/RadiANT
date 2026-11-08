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
  frame whole. STARTUP arrives 52 ms after SYSTEM_RESET, so a 50 ms read lost
  it about 1 % of the time (TODO.md item 2). `tools/ant_reset_loop.py`
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
