# A4 CONSTLAT recheck: does MPSL hold CONSTLAT during our timeslots? 2026-11-18

> Raw J-Link logs (`*_jlink.log`) and per-run JSON stay on the bench machine (gitignored). The committed numbers are
> in this README, `a4_constlat_summary.json` / `.txt` (made by `scripts/a4_summary.py` from the JSON), the
> `jlink_peek` text reports (`*_peek*.txt`, `*_idle.txt`), the scan/track tool outputs and `images_md5.txt`. The
> conclusion is in `docs/bench-runbook.md`, "MPSL builds: CONSTLAT does not stay on".

No current is quoted here; CONSTLAT's ~549 µA is the Gate GA ESTIMATE.

## Question

Pass A4 moved MPSL's low-latency callbacks onto nrfx's CONSTLAT reference count (`NRFX_POWER`) on nRF54L MPSL-gate
builds. `CONFIG_RADIANT_NRF54L_MPSL_CONSTLAT_HOLD=y` (default) has the backend hold one reference for the whole
uptime. If MPSL's per-event acquire already covers the timeslots this backend's radio work runs in, `=n` is enough
for errata 20 and saves the always-on CONSTLAT current. Also check `=y` gives CONSTLAT 200/200, and that the count
(`m_power_mode_refs`, a `uint8_t`) is balanced.

## Images

apps/dongle for `nrf54l15dk/nrf54l15/cpuapp`, sysbuild, built by the A4 author in `C:\wt_power_a4` at ac6904a (an
ancestor of this branch), `-DEXTRA_CONF_FILE=apps/dongle_thread/gate.conf;apps/dongle_thread/uart_57600.conf;
apps/dongle/diag_counters.conf -Ddongle_CONFIG_RADIANT_POWER_DIAG=y -Ddongle_CONFIG_RADIANT_POWER_DIAG_LOG_S=0`.
`.config` read back `CONFIG_MPSL=y`, `CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL=y`, `CONFIG_NRFX_POWER=y`,
`CONFIG_RADIANT_POWER_DIAG=y`, `CONFIG_ANT_DONGLE_UART_BAUD=57600`.

| Arm | Build dir | Extra | `zephyr.hex` md5 |
|---|---|---|---|
| hold=n | `build/a4_l15_mpsl_pd_nohold` | `-Ddongle_CONFIG_RADIANT_NRF54L_MPSL_CONSTLAT_HOLD=n` | d89c24763bd6dc52015e28b46cf65afd |
| hold=y | `build/a4_l15_mpsl_pd` | (default y) | c4c8fbf1f116a4289b2fdf65875d8982 |

`radiant_power_diag` at 0x20000300 and `m_power_mode_refs` at 0x20008682 (byte 2 of word 0x20008680) in both.
md5s matched the A4 author's before flashing. Flashed with `scripts/flash_sim_jlink.ps1 -Serial 001057737173` (no
`-Erase`); `tools/ant_probe.py --port COM4 --baud 57600` passed on both (reset, capabilities, version
`RADIANT0.01B00`, serial).

## Rig

- nRF54L15 DK, J-Link 001057737173, ANT UART COM4 @57600. Only this board was touched.
- The TI transmitted HR #12345 throughout; the room also had #52233 (power 0x0B, FE-C 0x11, speed 0x7B).
- Busy: `tools/ant_scan_rate.py --port COM4 --baud 57600 --seconds 300 --scan-type 0x40` (never-locking background
  scan). Tracking: `tools/ant_scan.py --port COM4 --baud 57600 --seconds 120` (wildcard slave, 4 Hz).
- Sampling: `scripts/jlink_peek.ps1 -Preset nrf54l15 -Serial 001057737173 -Samples 200 -ElfPath <elf> -Decode
  -ExtraSymbol m_power_mode_refs -Json <run>.json`, started ~8 s after the scan/track tool. Each sample reads
  `CLOCK.XO.*` and `POWER.CONSTLATSTAT` first and the refs word after it. RX duty = Δ`radio_rx_us` / Δ`uptime_ms`
  between the struct dumps that bracket the same session.

## Results

| Image | Case | Uptime (s) | CONSTLAT on (95 % CI) | RX duty | refs histogram (CONSTLAT on within) |
|---|---|---|---|---|---|
| hold=n | idle, no channel | 20–79 | 0/200 (0–1.9 %) | 0 | 0: 200 (0) |
| hold=n | scan, early | 101–161 | 178/200 = 89.0 % (83.9–92.6) | 89.14 % | 0: 23 (1); 1: 177 (177) |
| hold=n | scan, late | 177–238 | 183/200 = 91.5 % (86.8–94.6) | 89.08 % | 0: 17 (0); 1: 183 (183) |
| hold=n | tracking | 411–472 | 3/200 = 1.5 % (0.5–4.3) | 0.34 % (4.20 ops/s) | 0: 199 (2); 1: 1 (1) |
| hold=y | idle, no channel | 13–73 | 200/200 | 0 | 1: 200 (200) |
| hold=y | scan, early | 97–157 | 200/200 | 89.14 % | 1: 17 (17); 2: 183 (183) |
| hold=y | scan, late, 600 samples | 180–360 | 600/600 (99.4–100) | 89.17 % | 1: 65 (65); 2: 535 (535) |
| hold=y | idle after scan closed | 396–426 | 100/100 | 0 | 1: 100 (100) |
| hold=y | tracking | 450–510 | 200/200 | 0.11 % (4.07 ops/s) | 1: 199 (199); 2: 1 (1) |
| hold=y | idle after tracking closed | 563–578 | 50/50 | 0 | 1: 50 (50) |

Scan tool output: hold=n 4512 sightings / 300.1 s (15.04/s), hold=y 4569 (15.23/s), same four devices, so the hold
does not change scan behaviour. HFXO was 200/200 (or n/n) in every run; liveness `running` in every run.

## Verdict

**MPSL's per-event acquire covers this backend's timeslots.** With hold=n and the radio 89.1 % busy, CONSTLAT was on
in 361/400 samples (90.3 %), and every sample with `m_power_mode_refs` = 1 (360) had CONSTLAT on; refs = 0 had it
off in 39 of 40. CONSTLAT's on-fraction sits at or slightly above the RX duty, as it should if the MPSL event
(timeslot plus ramp/lead) contains the radio window. If the timeslots were not covered, the fraction would be ~0
(the GA ackps reading). At 4 Hz tracking the hold=n fraction is 1.5 % against 0.34 % RX on-time, again a superset;
only 3 hits in 200 samples, so it is a consistency check, not a per-slot proof.

**hold=y works:** CONSTLAT 1350/1350 across idle, scan and tracking.

**The count is balanced:** hold=y refs only ever 1 (idle) or 2 (inside an event), never above 2 over ~10 minutes and
~3500 RX ops, and back to exactly 1 after the scan and after tracking closed. hold=n returns to 0 between events
and after the scan. No drift.

## Oddities

- 3 samples (1 in hold=n scan, 2 in hold=n tracking) read CONSTLAT on with refs 0. The refs word is read after the
  CONSTLATSTAT register in each sample, about one SWD round trip later, so an event ending between the two reads
  gives exactly this. The tracking case has short events, hence 2 of 3 there. No sample read refs ≥ 1 with
  CONSTLAT off.
- Per RX op, hold=n tracking averaged 815 µs and hold=y tracking 264 µs. The wildcard channel locked onto
  different sensors in the two runs (it reported 4 and 3 sensors), so this is not a hold effect.
- `jlink_peek` prints with Write-Host: a PowerShell `>` redirect captures nothing. The text reports here were made
  with `6>&1 | Out-File -Encoding utf8` (the first four were regenerated from the raw log with `-ParseLog`), then
  normalised to LF.

## Not done

- No per-slot proof that CONSTLAT rises before the radio ramps: the sampler cannot read RADIO state. The evidence is
  statistical plus the acquire's position at event start (A4 disassembly).
- No estimate run with `tools/ant_power_est.py` (the dongle image has no CPU accounting; the CONSTLAT row is the
  only one that changes).
- The Kconfig default was not changed; that is the owner's call.

## Board state at the end

L15 restored to the Gate GA image (`C:\wt_power_ta\build\ga_lp_q`, `2a130085`); `post_restore_ga.txt` shows liveness
running, power_diag uptime 6.0 → 12.0 s and 24 → 48 TX ops, CONSTLAT 20/20. TI, Pro Micro and the 5340 untouched.
