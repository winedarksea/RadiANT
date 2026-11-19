# Pass B5 bench gate: Pro Micro (nRF52840, RTC1) HFXO/timebase release, 2026-11-19

Plan gate (iridescent-fog, Pass B5): "G1 on-air criteria with Pro Micro as master (L15/TI/Garmin receivers);
stack-request duty from LP stats over a CDC debug build (USB itself holds HFXO there, so only the stack's own request
duty is meaningful)." The Garmin was not used.

## Verdict

> **Rerun at equal TX power:** [`../2026-11-19-power-b5-rerun/`](../2026-11-19-power-b5-rerun/README.md). The excess below was transmit power. At 0 dBm with the HFXO held, the TI loses 3.8-4.6 %, and B2 matches A2; the transmitter-side explanation is refuted.

**B5 NOT PASSED: the on-air loss criterion fails against the control that was available, and that control is
confounded. Every power and timing criterion passes.**

| Criterion | Result |
|---|---|
| Stack HFXO request duty (power-diag `hfxo_pm`) | **PASS: 16 ‰ (1.6 %)** in 144 of 145 ten-second lines. The one exception was the first line, 39 ‰, which included the 1 s boot linger |
| No lost or failed operations (late wakes FAIL an op) | **PASS**: tx_ops = rx_ops = 40 or 41 per 10 s (57 : 88 lines), exactly 32768/8070 = 4.0607/s over 1450 s. No `<wrn>` from the LP code. The boot line reads `low power on: sleep beyond 4327 us (wake lead 1052, LF 275, keep-on 3000), RTC1 ch 2/1`, so it was not refused |
| Mean period shift ≤ LFXO offset | **PASS**: −13.90 µs per period at the TI and −13.89 µs at the L15 (A 246278.03 → B 246264.13/.17 µs; L15 246269.55 → 246255.66). That is −56.4 ppm, against spike S5's Pro Micro HF-vs-LF −58.5 ppm. Sign as on the L15: the LF crystal runs fast |
| Interval jitter | **PASS on G1's reading**: every received packet in every arm lies within one receiver tick of the fitted grid. Largest interval residual: 21-25 µs in B, 23-29 µs in A. Phase stddev: 9.7-10.9 µs in B, 8.8-10.7 µs in A. The raw interval-stddev figure (TI 5.9 in A against 14.9 in B) is the tick-quantisation artefact described in G1's README, not transmitter jitter |
| Loss within the same-window control | **FAIL / unresolved**: B 4.43 / 4.42 % (L15) and 4.71 / 4.42 % (TI), against the control's 1.19 % (L15) and 1.10 % (TI) |
| LP fault counters (`radiant_nrf_lp_stats`) | **NOT READ**: the Pro Micro has no SWD on this bench, and the image does not print the struct. The op counts above are the available proxy for late wakes |

### The loss

- **The control is confounded by transmit power.** No option-off Pro Micro hr_transmitter image existed. Building one
  was out of scope, and once hr_transmitter is flashed there is no software path back to the UF2 bootloader. So the
  A arm was run first: the Pro Micro, still on its pm_diag apps/dongle image, was driven as HR master #12345 by
  `tools/ant_sim.py` (HFXO always on, option off). That image has `ANT_DONGLE_TX_POWER_BOOST=y` (+8 dBm on nRF52840),
  while hr_transmitter transmits at the stack default of 0 dBm. Both receivers duly read 8-9 dB less in B: L15 −45 →
  −53 dBm, TI −40 → −49.5 dBm. So B against A compares a different firmware at 8 dB less power, not the option alone.
- **B's misses coincide at the two receivers.** About 73 % of B's missed slots were missed by both: 80 of 108/115 in
  B1, 78 of 108/108 in B2, against 5 expected if they were independent. In A, 7 of 29/27 coincided (0.3 expected), so
  the room has a shared-cause component too. Independent per-link noise cannot produce this. Two explanations fit,
  and this data cannot tell them apart:
  1. a shared interferer near both receivers, which costs more at 8 dB less margin;
  2. the transmitter putting no decodable frame on air in ~3 % of slots while the stack counts each TX op as done.
- The misses are spread over every minute, in runs of at most 3, and every received packet is on the grid.
  The TI's unrepairable CRC failures rose from 0 to 18 / 13 per window, which fits a weaker or degraded frame.
- In B, the TI's `crc_fail` was 25 / 18, of which 7 / 5 were repaired and delivered. The L15 receiver image predates
  the CRC-repair fix, so its 6-7 "unaccounted" are its dropped repairs.

### To close B5

Run an A/B at **equal transmit power** with the same receivers. Either:

- an hr_transmitter Pro Micro build with `CONFIG_HR_TX_RELEASE_HFXO=n` plus `power_diag.conf;console_cdc_promicro.conf`;
- or the ant_sim control at 0 dBm (ant_sim.py has no TX-power option today; LVL_3 is honoured as a true 0 dBm by the
  dongle).

Each Pro Micro switch away from hr_transmitter needs a **physical double-tap** (no reboot-to-bootloader on this
image). Interleave A/B/A if a human is at the bench for the taps. To test explanation 2 directly, add a third receiver
next to the Pro Micro.

## Arms (600 s each, receivers L15 + TI in the same window; `b5_summary.json`)

| Arm | Transmitter | L15 loss % (pkts; RX_FAIL / unacc.) | TI loss % (pkts; RX_FAIL / unacc.) | TI crc_fail / repaired / unrepairable | L15 / TI dBm | TI period µs | Misses in common (indep. exp.) |
|---|---|---|---|---|---|---|---|
| A | Pro Micro pm_diag apps/dongle + `ant_sim.py` HR #12345, +8 dBm, option off | 1.186 (2407; 27 / 2) | 1.104 (2410; 27 / 0) | 2 / 2 / 0 | −45.0 / −40.2 | 246278.033 | 7 (0.32) |
| B1 | Pro Micro `hrtx_promicro_lp_pd`, 0 dBm, option on | 4.427 (2328; 102 / 6) | 4.711 (2321; 115 / 0) | 25 / 7 / 18 | −53.0 / −49.4 | 246264.135 | 80 (5.10) |
| B2 | same, continuing | 4.424 (2328; 101 / 7) | 4.424 (2328; 108 / 0) | 18 / 5 / 13 | −53.0 / −49.5 | 246264.172 | 78 (4.79) |

Heart rate decoded 120 bpm in B. `ant_verify` exits 1 in B for the known reason: hr_transmitter sends no pages 80/81.
The control passes because ant_sim sends them.

## Images

| Board | Image | md5 |
|---|---|---|
| Pro Micro, B | `C:\wt_power_b5\build\hrtx_promicro_lp_pd` (hr_transmitter, `lowpower.conf;power_diag.conf;console_cdc_promicro.conf`, `dcdc_promicro.overlay;console_cdc_promicro.overlay`; LP_HFXO_STARTUP_US 500) | uf2 `875a1aee8a0b9d66c9810743e30a6dd0` |
| Pro Micro, A | pm_diag_acdd9d2 (`archive/benchmarks/2026-11-17-fec-crossing/images/`), driven by ant_sim | uf2 `6f75c634ad3c1014cac78fe969d91076` |
| L15 (receiver) | `build/c2gate_l15` (apps/dongle direct core+nrf, ANT COM4 @115200, from b7d4bd6, before the CRC-repair fix) | hex `71fdffe497e214864334a776cec1e587` |
| TI (receiver) | `C:\wt_crc_repair\build\crcfix_ti_rx` | hex `a3862fbfeb32c53a84b9326350ff2d30` |

## Commands

```
scripts\flash_sim_jlink.ps1 -HexPath build\c2gate_l15\dongle\zephyr\zephyr.hex -Device nRF54L15_M33 -Serial 001057737173
python tools\ant_probe.py --port COM4 --baud 115200                                   # PASS
# A (Pro Micro still the dongle):
python tools\ant_sim.py --profile heart-rate --bpm 120 --device-number 12345 --trans-type 5 --serial EE6986854FD72803 --seconds 720
python scripts\b5_arm.py A build\b5_runs                                             # concurrently
# B:
python tools\ant_probe.py --serial EE6986854FD72803 --reboot-to-bootloader           # NICENANO on F:, bootloader 0.6.0
scripts\flash_uf2.ps1 -Uf2Path C:\wt_power_b5\build\hrtx_promicro_lp_pd\hr_transmitter\zephyr\zephyr.uf2 -VolumeLabel NICENANO
python scripts\b5_cdc_capture.py COM12 1450 build\b5_runs\B_cdc.log                  # CDC console, DTR on
python scripts\b5_arm.py B1 build\b5_runs ; python scripts\b5_arm.py B2 build\b5_runs
python scripts\b5_miss_overlap.py <arm> <period_us>                                  # run in build\b5_runs
```

`b5_cdc_summary.txt` holds the boot LP line and the aggregated power-diag lines. One `<err> udc: Failed to allocat...`
line appeared at 1.2 s uptime (a USB stack message during enumeration; it did not recur). The full console and
per-arm texts are in `runs/` (gitignored). JSON and `.rec` stay in `build\b5_runs\`.

## Board state after this gate

| Board | Image | Note |
|---|---|---|
| Pro Micro (EE6986854FD72803) | **`hrtx_promicro_lp_pd`, uf2 875a1aee**, transmitting HR #12345 at 0 dBm, CDC console COM12 | Not restored: hr_transmitter has no reboot-to-bootloader. To restore pm_diag_acdd9d2 (6f75c634): double-tap RST, then `flash_uf2.ps1 ... -VolumeLabel NICENANO` |
| nRF54L15 DK (001057737173) | `build/c2gate_l15` apps/dongle receiver, hex 71fdffe4 | Silent. Left as a receiver so #12345 is not on air twice |
| TI LaunchXL | `crcfix_ti_rx`, hex a3862fbf | Receiver, ANT COM11 @57600 |
