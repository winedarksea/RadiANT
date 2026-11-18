# Gate C2: TI (CC2652) XOSC_HF release, on-air A/B/A, 2026-11-18

**Every current figure here is an ESTIMATE**: the firmware's `radiant_power_diag` log line multiplied by typical
datasheet currents (`tools/ant_power_est.py`). No current meter was used.

Code: local master b7d4bd6 (Pass C2 merged). The option under test is `CONFIG_RADIANT_CC26XX_XOSC_HF_RELEASE`
(`xoscHfAlwaysNeeded = false` in radiant's `RFCC26XX_hwAttrs`, see `radiant/src/radiant_ti_rf_hwattrs.c`).

The `ant_verify` record files (`*.rec`), the raw ANSI console captures and the flash logs stay on the bench machine in
`build/c2gate_runs/` (gitignored). This folder holds the small text files: the TI console of each arm with the ANSI
colour codes stripped (`*_ti_console.txt`, complete), the receiver outputs, the estimator outputs and
`c2_summary.json` (from `scripts/c2_summary.py runs A1 B A2`).

## Images

Both built from master b7d4bd6 by hand west, run from `C:\ncs\v3.4.0`:
`west -z C:\ncs\v3.4.0\zephyr build -s radiant\samples\hr_transmitter -b cc26x2r1_launchxl -p always --
-DEXTRA_ZEPHYR_MODULES=C:/ncs/v3.4.0/modules/hal/ti "-DEXTRA_CONF_FILE=<conf>"`.

| Arm | Build dir | `EXTRA_CONF_FILE` | `zephyr.hex` md5 | `zephyr.elf` md5 |
|---|---|---|---|---|
| A (option off) | `build/c2gate_A` | `power_diag.conf;console_on.conf` | `f205db7e307ce369af97af6c034861e5` | `e03e08013d1cc86c38931e311fc9b8ed` |
| B (option on) | `build/c2gate_B` | `ti_lowpower.conf;power_diag.conf;console_on.conf` (row `ti_hr_tx_lp_pd`) | `0525ff6d4606777e822541c15d0ccb15` | `d9b2b2929823c09b31d71b755e39bdb2` |

`.config` read back: the two files differ in exactly one line, `CONFIG_RADIANT_CC26XX_XOSC_HF_RELEASE=y` in B
(`# ... is not set` in A). Both have `CONFIG_RADIANT_POWER_DIAG=y`, `CONFIG_RADIANT_CC26XX_RF_HWATTRS=y`,
`CONFIG_LOG=y`, `CONFIG_LOG_MODE_DEFERRED=y`, `CONFIG_SERIAL=y`, `CONFIG_UART_CONSOLE=y`, and HR_TX_ONE_WAY not set.
In the ELFs, `RFCC26XX_hwAttrs` (rodata 0x117bc) reads `e0 00 01 00 ...` in A and `e0 00 00 00 ...` in B: byte 2,
`xoscHfAlwaysNeeded`, is 1 and 0.

Each was flashed with `scripts\flash_ti.ps1 -Serial L1100TY2 -HexPath ...` (dslite, then the script's backchannel
reset). The console came back after every flash; no manual xds110reset recovery was needed.

## Rig

- **LAUNCHXL-CC26X2R1** (XDS110 L1100TY2): the device under test, HR master #12345, bidirectional. Console COM11 at
  115200, captured from boot with `scripts/gc1_capture.py` (opens the port with DTR and RTS, then pulses
  `xds110reset.exe`). Never JTAG-connected to read memory.
- **Pro Micro** EE6986854FD72803 (pm_diag_acdd9d2, not reflashed): passive receiver.
  `tools/ant_verify.py --profile heart-rate --device-number 12345 --seconds 600 --expect-bpm 120`.
- **nRF54L15 DK** (J-Link 001057737173): reflashed for this gate, through `scripts\flash_sim_jlink.ps1 -Serial
  001057737173`, with apps/dongle direct (core + nrf, no MPSL, UART 115200 on COM4) built from b7d4bd6 into
  `build/c2gate_l15` (`west ... build -s apps\dongle -b nrf54l15dk/nrf54l15/cpuapp -- -DANT_RADIO=core
  -DRADIANT_BACKEND=nrf`), `zephyr.hex` md5 `71fdffe497e214864334a776cec1e587`. `.config`: ANT_DONGLE_RADIO_CORE,
  RADIANT_BACKEND_NRF, GATE_MPSL not set, TRANSPORT_UART. `ant_probe.py --port COM4` PASS (RADIANT0.01B00). This
  silenced the L15's own HR #12345 transmitter (GA image `2a130085`), so the TI was the only #12345 on the air. It then
  served as the second receiver and as the probe of the TI's own receive path, `scripts/c2_acked_probe.py`: a
  bidirectional slave on #12345 that sends one acknowledged message (page 0xF0 + sequence number) every 1–2 s
  (random) and counts EVENT_TRANSFER_TX_COMPLETED / _FAILED, broadcasts and RX_FAIL. The TI must receive each message
  in the listen window after its broadcast (the in-grant RX arm) and answer with an ACK, so the success rate is a
  round trip through exactly the path a late RX arm would hurt. The TI's own count of frames received comes from its
  `radiant_api: rxpath ... tot=` log line.
- The Garmin and the nRF5340 DK were not used.

Per arm, `scripts/c2_arm.py LABEL OUTDIR`: start the capture (boot), wait a random 25–55 s (A1 38.3 s, B 54.7 s,
A2 47.2 s), then run both receivers concurrently for 600 s. The capture runs 25 s past them. The TI summary sums every
power line from 21 s uptime to the end of the capture (65–66 lines, 650–660 s per arm). Arms ran back to back:
A1 15:40, B 15:52, A2 16:04 (local).

A first A1 attempt (15:28, `aborted_A1try1/`) lost the `ant_verify` JSON: the output path was relative and the
child processes ran from the repo. Its TI console and L15 figures agree with A1 (cold/txl 0, pudur 448–480, 312 of
321 acked completed); it is not used in the tables.

## Results

### TI power line (sums over each arm; `rfon_pm`, `cpu_pm`, `rfpu_x10` floored per line)

| | A1 (off) | **B (on)** | A2 (off) |
|---|---|---|---|
| `pudur_us` median (range) | 448 (448–480) | **480 (480–482)** | 448 (448–480) |
| `cold` / `txl` / `txl_max_us` | 0 / 0 / 0 | **0 / 0 / 0** | 0 / 0 / 0 |
| `fsx` / `bks` / `bks_max` / `wraps` | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| `rfon_pm` mean (lines) | 17.62 (17–18) | 17.68 (17–18) | 17.60 (17–18) |
| `rfpu` /s, per broadcast slot | 4.0611, 1.000 | 4.0602, 1.000 | 4.0611, 1.000 |
| RF core on per power-up | 4.34–4.58 ms | 4.36–4.60 ms | 4.33–4.58 ms |
| TX ops/s (broadcasts + ACKs) | 4.541 | 4.536 | 4.546 |
| RX µs per listen window | 1019 | 1020 | 1019 |
| `cpu_pm` mean (lines) | 15.91 (15–18) | **16.74 (16–19)** | 15.86 (15–18) |
| Idle entries /s (`idle_x10`/10) | 37.9 | **49.9** | 38.0 |

`rfpu` per broadcast: power-ups 4.06/s equal the 4.06/s broadcast slots; the TX op count is higher because the ACKs
the TI sends back to the L15 (~0.48/s) are TX ops inside the same power-up.

### Receivers (same 600 s window per arm)

| | A1 (off) | **B (on)** | A2 (off) |
|---|---|---|---|
| Pro Micro: packets / expected | 2401 / 2437 | 2357 / 2437 | 2342 / 2437 |
| Pro Micro: loss | 1.47 % | **3.28 %** | 3.89 % |
| Pro Micro: RX_FAIL / unaccounted | 33 / 3 | 76 / 4 | 93 / 2 |
| Pro Micro: miss runs (1 / 2 slots) | 34 / 1 | 71 / 5 | 89 / 3 |
| Pro Micro: misses per minute | 4 3 1 0 1 1 1 2 10 13 | 4 2 8 21 15 1 16 2 8 4 | 8 11 9 7 6 14 11 11 8 10 |
| Pro Micro: signal mean (min, max) | −46.8 dBm (−70, −33) | −57.4 (−82, −39) | −58.2 (−61, −55) |
| Pro Micro: interval stddev / max gap | 31.7 / 735 ms | 48.6 / 734 ms | 50.8 / 766 ms |
| Pro Micro: off slot grid, host / radio clock | 7.36 / 0.0074 ms | 10.23 / 0.0074 ms | 7.61 / 0.0076 ms |
| L15: broadcasts / expected (loss) | 2421 / 2437 (0.66 %) | 2419 / 2437 (**0.72 %**) | 2421 / 2437 (0.67 %) |
| L15: RX_FAIL / to-search | 16 / 0 | 17 / 0 | 16 / 0 |
| L15: signal mean (min, max) | −41.3 dBm (−57, −33) | −33.3 (−65, −26) | −27.0 (−27, −27) |
| **TI RX: acked sent → TI received** (`rxpath tot`) | 315 → 312 (99.05 %) | 316 → 314 (**99.37 %**) | 319 → 315 (98.75 %) |
| **Acked round trip: completed / failed** | 310 / 5 (98.41 %) | 313 / 3 (**99.05 %**) | 315 / 4 (98.75 %) |
| TI `xfer failures` / `rxpath dropped` | all 0 / 0 | all 0 / 0 | all 0 / 0 |

`ant_verify` exits FAIL on every arm for the expected reason (common pages 80/81: the sample sends page 0 only), and
on B and A2 also on loss above its 1.5 % simulator-grade limit. Heart rate decoded 120 bpm exact in every arm.

### Estimate (`ant_power_est.py --board cc2652 --log <arm>_ti_console.txt --uart-on`; ESTIMATE, µA)

| Component | Basis | A1 | B | A2 |
|---|---|---|---|---|
| Idle floor (SWRS207J Idle, RCOSC_HF) | always | 675.00 | 675.00 | 675.00 |
| UART declared on (Iperi UART) | always | 167.50 | 167.50 | 167.50 |
| CPU active (48 MHz CoreMark row) | 1.585 / 1.669 / 1.580 % | 43.03 | **45.30** | 42.91 |
| Radio RX | 0.414 % | 25.78 | 25.79 | 25.77 |
| Radio TX 0 dBm | 0.123 % | 7.79 | 7.79 | 7.80 |
| RF core idle (LB): rfon − TX − RX at the Iperi RF-core delta | 1.22 % | 2.58 | 2.59 | 2.58 |
| RF synth/boot | wake budget 1781 / 1901 / 1786 µs/s | UNKNOWN, excluded | UNKNOWN, excluded | UNKNOWN, excluded |
| **XOSC_HF outside radio** | no datasheet row | UNKNOWN, excluded | UNKNOWN, excluded | UNKNOWN, excluded |
| **Total (known parts)** | | **921.7** | **924.0** | **921.6** |
| Total without the UART row | | 754.2 | 756.5 | 754.1 |

The estimator cannot show the saving: the CC2652R/P datasheets give no XOSC_HF current, so the HFXO term is UNKNOWN
and excluded in both arms. The only component that moves is CPU, up 0.83 ‰ in B (≈ +2.3 µA), so B's known-parts total
is 2.3 µA *higher*. What B changes is the crystal's duty, and that has no current attached:

- A holds XOSC_HF 100 % of the time (`xoscHfAlwaysNeeded = 1`: the RF driver's dependency is set at RF_open and never
  released).
- B needs it only while the RF core is powered (`rfon` 17.7 ‰) plus the wake before the setup command (bounded by
  power-ups/s × `pudur_us` = 4.06 × 480 µs = 1.9 ‰), so it runs ≤ ~2.0 % of the time and is off ≥ ~98 %. That is
  inferred from the driver's dependency handling and these counters, not sampled: nothing on this bench reads the TI's
  clock state without a JTAG connect, which wipes RAM.
- The saving is therefore ~0.98 × I(XOSC_HF), an unknown current. Note which way the table errs: the Idle floor row is
  specified on RCOSC_HF, which is B's idle state. A's idle also runs the crystal, so A's total is the one that is
  understated, by about that same unknown term.

## Verdicts against the predictions

1. **`pudur_us` 450–750 µs with the option on: PASS, at the bottom of the band.** B read 480 on 63 of 66 lines
   (481–482 on the others), against A's median 448. The driver times a power-up on the AON RTC (`>> 12` of the 64-bit
   count, ~0.95 µs units), so its samples come in whole 32 kHz ticks: 448 = 14 ticks, 480 = 15 ticks. A's in-between
   values (450–464) are its low-pass average of 14- and 15-tick power-ups, and B measured 15 ticks almost every time.
   Releasing the crystal adds at most one RTC tick (~31 µs) to a power-up, not the ~200 µs crystal start the plan
   allowed for: the crystal starts in parallel with the RF core's boot and is ready about when the boot finishes.
   `rfon` per power-up also moved by only +17 µs.
2. **No growth in `cold` / `txl`: PASS.** 0 / 0 / 0 µs in all three arms (about 2700 power-ups and 3000 transmits per
   arm), and `fsx` 0.
3. **Loss within the same-window spread: PASS.**
   - Pro Micro: B's 3.28 % sits inside A1–A2 (1.47–3.89 %). The Pro Micro's loss followed its signal, which fell
     about 10 dB between A1 and B and stayed down for A2 (−46.8 → −57.4 → −58.2 dBm). The misses come in episodes
     (A1 minutes 8–9; B minutes 3–4 and 6). Over the same period the L15's signal rose (−41 → −33 → −27 dBm). Opposite
     drifts at two receivers that run through A2 as well are a propagation change in the room, not the TI's
     transmitter.
   - The L15 hears the TI in the same windows: 0.66 / 0.72 / 0.67 %. B is +0.05 pp, about one packet.
   - Interval statistics are inside A's in B. Radio-clock off-grid timing is 0.0074 ms in every arm. The interval
     stddev tracks loss: a missed slot doubles a gap.
4. **TI receive path (the review's RCOSC_HF recalibration concern): no degradation.** The worry was a late in-grant RX
   arm, now that the calibration runs at each power-up at the RF driver's interrupt priority; it would not show in
   `cold` / `txl`. In B the TI received 99.37 % of the L15's acknowledged messages (A: 99.05 / 98.75 %), and 99.05 % of
   them completed the round trip with the ACK (A: 98.41 / 98.75 %). The TI's transfer-failure counters and receive drops
   were 0 in every arm. One slave-originated transfer every ~2 s (0.48/s) probes about 1 in 8 listen windows: a fault that hit a
   few percent of windows would show, one at the 0.1 % level would not.

**Gate C2: PASS.** The option costs nothing measurable on the air and on the TI's own counters. Its benefit is the
crystal off ~98 % of the time, which no datasheet row lets this bench price.

## Anomalies and notes

- **Idle entries +12.0/s in B** (37.9 → 49.9), about 2.95 more wakes per RF power-up, with **CPU +0.83 ‰** (≈ 0.2 ms/s,
  ~50 µs per power-up). These fit the Power driver's crystal switch: the OSC interrupt that switches SCLK_HF and the
  RCOSC_HF calibration's own steps, per power-up. Not broken down further. The +2.3 µA (ESTIMATE) they add to the CPU
  term is part of B's cost.
- **CPU is 15.9 ‰ in A, against 55.6 ‰ at GC1** on the same sample: C2's `apply_power()` fix (log at DBG and act only
  on change) removed the per-transmit `tx power req` INF line and `RF_setTxPower()` call. The A arm's estimate fell from
  GC1's 1030 to 922 µA for that reason alone, and the console capture is now ~22 KB per 11 minutes instead of ~1.2 MB.
- **L15 signal −27 dBm with min = max over 2421 packets in A2** (and −26 max in B): a very strong, flat reading at
  the L15. It moved monotonically over the session along with the Pro Micro's opposite drift. It is not a fault in the
  loss counts, which are hard packet counts, but RSSI from this stack near the top of its range has not been
  characterised.
- The room loss today is higher than at GC1 (Pro Micro 0.08 % at −42 dBm then). It is a link-margin and window effect,
  per the bench's loss floor notes; the gate rests on same-window and A/B/A comparisons, not on the absolute figure.

## Board state after the gate

| Board | Image | md5 |
|---|---|---|
| TI LaunchXL (L1100TY2) | **B**, hr_transmitter XOSC_HF release + power_diag + console (`build/c2gate_B`), transmitting HR #12345, console COM11 @115200; boot rechecked (`final_B_boot_check.txt`) | `0525ff6d4606777e822541c15d0ccb15` |
| nRF54L15 DK (001057737173) | apps/dongle direct core+nrf, UART COM4 @115200 (`build/c2gate_l15`); no channel open, so silent | `71fdffe497e214864334a776cec1e587` |
| Pro Micro (EE6986854FD72803) | unchanged, pm_diag_acdd9d2 | `6f75c634` (per bench notes) |
