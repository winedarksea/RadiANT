# Pass B5 rerun at equal TX power, and the Pro Micro spike S3 lp rerun, 2026-11-19

Two Pro Micro (nRF52840 nice!nano clone, serial EE6986854FD72803) reruns for the power plan
(iridescent-fog). They follow up two earlier results:

- [`../2026-11-19-power-b5-promicro/`](../2026-11-19-power-b5-promicro/README.md): B5 NOT PASSED, because its
  control was confounded (+8 dBm against 0 dBm).
- `docs/spike-lp-clock-results.md`: the Pro Micro S3 lp rows were INVALID (an HFXO reference leak).

Every power figure is an ESTIMATE. There is no current meter on this bench.

**Owner decision, 2026-11-19: B5 PASS.** B2 matches the control on every receiver, and B1's excess is within
the drift between the two control windows. No A3 window was run. The verdict table below is left as written.

## Verdicts

| Job | Verdict |
|---|---|
| Spike S3 lp (G0, nRF52840) | **PASS, and the HFXO is proven off this time.** 0 of 10 000 lp requests found the HFXO running (HFCLKSTAT on RC), and every request → callback took a real start of 11 RTC ticks (≈336 µs). The anchors stay inside −1.125…+0.875 µs (1 µs quantisation) for both wake and release. G0's nRF52840 S3 cell no longer rests on the held rows alone |
| B5 on air, equal TX power | **Confound removed. The old B5 excess was transmit power, not the option. Conditional PASS: owner call on one window.** At 0 dBm with the HFXO held (A), the TI and Garmin already lose 3.8–7.7 %, which is the old B5's "B" level. B2 matches A2 on every receiver (z = −0.5 / 0.1 / 0.8). B1 sits above A on all three, but by about the same amount as the A1 → A2 drift (Garmin z 3.3 between two A windows). A3 could not be run: no software path leads off hr_transmitter |
| Old B5 "transmitter puts no frame on air" hypothesis | **Refuted for this rig.** In A and in B alike, the TI and the Garmin miss the same slots (88–135 shared, against 5–16 expected by chance). The L15 hears nearly all of those slots (2–5 shared). So the frame was on air, and the shared cause is on the receive side |

### B5 against the plan's G1 on-air criteria (Pro Micro as master)

| Criterion | Result |
|---|---|
| Stack HFXO request duty (power-diag `hfxo_pm`) | **PASS: 16 ‰ (1.6 %)** in 143 of 144 ten-second lines. The exception was the first line (39 ‰, the boot linger). Same as the first B5 run |
| No lost or failed ops | **PASS**: tx_ops = rx_ops = 40/41 per 10 s (57 : 87 lines), i.e. 4.06/s. No `<wrn>` from the LP code. Boot line: `low power on: sleep beyond 4327 us (wake lead 1052, LF 275, keep-on 3000), RTC1 ch 2/1` |
| Mean period shift ≤ LFXO offset | **PASS**: TI 246278.12 → 246264.47/.54 µs (−13.62 µs), L15 246269.55 → 246255.89/.91 µs (−13.65 µs). That is −55.4 ppm, against spike S5 −56.9 (sleep) / −55.9 (busy) ppm in today's rerun |
| Interval jitter | **Same pattern as the first B5 run, not re-analysed.** TI residual stddev 6.2 µs in A against 14.8–14.9 µs in B; L15 13.0 against 14.1–14.2 µs. The first B5 README attributes this to receiver tick quantisation with every packet on the grid |
| Equal RSSI between A and B | **PASS**: L15 −43.9/−44.0 (A) against −43.3/−43.0 dBm (B); TI −48.3/−48.3 against −48.5/−48.4 dBm. The first B5 run's B read −49.4/−49.5 at the TI, so 0 dBm is the same power in both runs |
| Loss within the same-window control | **Conditional PASS (owner call)**: B2 lies inside A's spread at the L15 and the TI, and 0.6 pp above A2 at the Garmin. B1 lies above A at all three receivers. Pooled B against A: L15 +0.18 pp (z 1.6), TI +1.17 pp (z 2.7), Garmin +2.98 pp (z 5.4). The pooled excess comes entirely from B1. With only A,A,B,B in time order, drift cannot be separated from the option |
| LP fault counters (`radiant_nrf_lp_stats`) | NOT READ (no SWD on the Pro Micro). Op counts are the proxy |

## Arms (600 s each, receivers L15 + TI + Garmin in the same window; `b5r_summary.json`)

Misses are counted on the transmitter's slot grid by host time (the `b5_miss_overlap.py` method in
`scripts/b5r_summary.py`). Loss % is `ant_verify`'s exact figure.

| Arm | Transmitter | L15 loss % (RX_FAIL) | TI loss % (RX_FAIL) | Garmin loss % (RX_FAIL) | TI crc_fail / repaired / unrepairable | L15 / TI dBm | TI period µs | Shared misses TI+Garmin (exp.) | L15+TI | L15+Garmin |
|---|---|---|---|---|---|---|---|---|---|---|
| A1 | pm_diag apps/dongle + `ant_sim.py --tx-power 3`, 0 dBm, HFXO held | 0.162 (4) | 3.813 (93) | 5.372 (131) | 10 / 2 / 8 | −43.9 / −48.3 | 246278.121 | 88 (5.0) | 2 (0.15) | 2 (0.22) |
| A2 | same, continuing | 0.324 (8) | 4.555 (111) | 7.711 (188) | 14 / 4 / 9 | −44.0 / −48.3 | 246278.121 | 102 (8.6) | 2 (0.37) | 3 (0.62) |
| B1 | `hrtx_promicro_lp_pd`, 0 dBm, option on | 0.442 (11) | 5.982 (146) | 10.623 (260) | 13 / 2 / 10 | −43.3 / −48.5 | 246264.466 | 135 (16.0) | 5 (0.92) | 7 (1.61) |
| B2 | same, continuing | 0.195 (5) | 4.588 (112) | 8.321 (203) | 9 / 2 / 6 | −43.0 / −48.4 | 246264.536 | 105 (9.3) | 3 (0.28) | 3 (0.50) |

- Every miss at every receiver was an RX_FAIL (`unaccounted` 0 everywhere).
- The L15 receiver image predates the CRC-repair fix. Its 0 unaccounted means no CRC-repairable frame reached
  it in these windows.
- Arm A ran 09:26–09:47 and arm B 09:48–10:09, back to back.
- `ant_verify` exits 1 in B for the known reason (hr_transmitter sends no pages 80/81), and in A at the TI
  and Garmin on the loss limit only.
- The Garmin reports no RSSI and no radio timestamps, so its period column in the JSON is meaningless.

**The TX-power check, before the arms** (90 s each, TI + Garmin, `runs/rssi_*`). With `ant_sim` on the
pm_diag dongle, the TI read **−39.3 dBm with no power message** (boost, +8 dBm) and **−48.3 dBm with
`--tx-power 3`**: a 9 dB drop. Loss went with it: TI 0.80 → 4.93 %, Garmin 3.00 → 8.18 %. On this rig the
0 dBm link to the TI and the Garmin sits in the room's lossy band whatever the transmitter runs. That
explains the first B5 run's 1.1 % against 4.4 %.

**Reading the coincidences.** The first B5 README left two explanations for its shared misses: (1) a shared
interferer near the receivers, and (2) the transmitter putting no frame on air. Today's L15 heard 99.6–99.8 %
of slots and shared almost none of the TI/Garmin misses, in both arms. A frame the transmitter never sent
would be missed by all three. That leaves (1), or link margin common to the TI and the Garmin, as the shared
cause. Today's L15 was far stronger (−44 dBm at 0 dBm) than in the first run (−53 dBm), so the board has
probably moved. Only within-run comparisons hold.

## Spike S3 rerun (Pro Micro, `radiant/spike/lp_clock`, current master + this branch)

Bench change, committed (674c067):

- S3 lp records, per cycle, the HFXO state just before the request and the request → callback time (RTC
  ticks).
- On a CDC build the spike reboots into the UF2 bootloader 20 s after `LPSPIKE DONE`, or at once on `B`.
  It writes `NRF_POWER->GPREGRET = 0x57` (DFU_MAGIC_UF2_RESET, the same constant and path as
  `apps/common/usb_ant_class.c`), then a cold reboot. Bootloader 0.6.0 (`INFO_UF2.TXT`).

Run 09:03–09:18 (USB console up at 09:05:55). The spike went back to NICENANO by itself; the host's 'B' was never sent because the capture
script stalled, which was a bug in the host script, not the firmware. Full results are in
`spike_s3_summary.txt`; the full log is `runs/spike_pm_s3r.log` (gitignored).

| | n | distribution (1/8 µs; −9/−1/+7 = −1.125/−0.125/+0.875 µs) |
|---|---|---|
| lp wake | 10 000 | −9: 1124, −1: 8840, +7: 36 |
| lp release | 10 000 | −9: 123, −1: 9614, +7: 263 |
| held wake | 10 000 | −9: 1702, −1: 8295, +7: 3 |
| held release | 10 000 | −9: 372, −1: 9485, +7: 143 |
| lp: HFXO running at request | 10 000 | **0** |
| lp: request → callback | 10 000 | **11 RTC ticks in all 10 000** (≈336 µs, a real start) |

Also in the same run:

- **S1**: 53/54 registers retained (the one diff is the spike's own HFCLKSTARTED → CAPTURE[2]). The HFXO was
  off mid-sleep in all three variants. SHUTDOWN zeroes the counter.
- **S4**: request → callback CONSTLAT on p50 345, p99.9 395, max 413 µs; off p50 346, p99.9 400, max 416 µs.
  The first run's maxima were 385/418 µs, so the 500 µs Kconfig default still holds.
- **S5**: −56.87 (sleep, 300 s) and −55.89 (busy, 120 s) ppm.
- **Late HFXO starts**: 0.

## Images

| Board | Image | md5 |
|---|---|---|
| Pro Micro, spike | `radiant/spike/lp_clock` at 674c067, `promicro_nrf52840/nrf52840/uf2`, `--no-sysbuild`; saved as `build/power_images_2026-11-19/lp_pm_s3r/spike_lp_clock_zephyr.uf2` | uf2 `8f9d97c7d4e2d3b43b12c258d49f44b3` |
| Pro Micro, A | `archive/benchmarks/2026-11-17-fec-crossing/images/pm_diag_acdd9d2.uf2`, driven by `ant_sim.py --tx-power 3` | uf2 `6f75c634ad3c1014cac78fe969d91076` |
| Pro Micro, B | `build/power_images_2026-11-19/hrtx_promicro_lp_pd/hr_transmitter_zephyr_zephyr.uf2` (the first B5 run's B image) | uf2 `875a1aee8a0b9d66c9810743e30a6dd0` |
| L15 (receiver) | `build/c2gate_l15` apps/dongle direct core+nrf, ANT COM4 @115200, from b7d4bd6 (before the CRC-repair fix; same as the first B5 run). A current-master build was attempted, but the build lock was held by another agent's long zero-cost run | hex `71fdffe497e214864334a776cec1e587` |
| TI (receiver) | `crcfix_ti_rx`, unchanged | hex `a3862fbfeb32c53a84b9326350ff2d30` |
| Garmin USB-m (receiver) | stock, serial 006, libusb0 | — |

## Commands

```
# Spike (build under the build lock; run west from C:\ncs\v3.4.0)
west -z C:\ncs\v3.4.0\zephyr build -b promicro_nrf52840/nrf52840/uf2 -p always --no-sysbuild -s <wt>\radiant\spike\lp_clock -d <wt>\build\lp_pm_s3r
scripts\flash_uf2.ps1 -Uf2Path <wt>\build\lp_pm_s3r\zephyr\zephyr.uf2 -VolumeLabel NICENANO   # board already in bootloader
# capture: wait for VID 0x2FE3 (USB comes up after S1/S4/S3-lp, ~176 s), DTR on, read to LPSPIKE DONE;
# the spike then reboots into NICENANO by itself.

# B5
scripts\flash_uf2.ps1 -Uf2Path archive\benchmarks\2026-11-17-fec-crossing\images\pm_diag_acdd9d2.uf2 -VolumeLabel NICENANO
python tools\ant_probe.py --serial EE6986854FD72803                                   # PASS, RADIANT0.01B00
scripts\flash_sim_jlink.ps1 -HexPath build\c2gate_l15\dongle\zephyr\zephyr.hex -Device nRF54L15_M33 -Serial 001057737173
# TX-power check (90 s, TI + Garmin), with and without --tx-power 3
# A:
python tools\ant_sim.py --profile heart-rate --bpm 120 --device-number 12345 --trans-type 5 --serial EE6986854FD72803 --tx-power 3 --seconds 1500
python archive\benchmarks\2026-11-19-power-b5-rerun\scripts\b5r_arm.py A1 build\b5r_runs --garmin   # then A2
# B:
python tools\ant_probe.py --serial EE6986854FD72803 --reboot-to-bootloader
scripts\flash_uf2.ps1 -Uf2Path build\power_images_2026-11-19\hrtx_promicro_lp_pd\hr_transmitter_zephyr_zephyr.uf2 -VolumeLabel NICENANO
python archive\benchmarks\2026-11-19-power-b5-rerun\scripts\b5_cdc_capture.py COM12 1450 build\b5r_runs\B_cdc.log
python archive\benchmarks\2026-11-19-power-b5-rerun\scripts\b5r_arm.py B1 build\b5r_runs --garmin   # then B2
python archive\benchmarks\2026-11-19-power-b5-rerun\scripts\b5r_summary.py build\b5r_runs archive\benchmarks\2026-11-19-power-b5-rerun\b5r_summary.json A1 A2 B1 B2
```

`b5r_arm.py` is the first run's `b5_arm.py` with an optional Garmin receiver (`--serial 006`). The USB stack
logged `<err> udc: Failed to allocate net_buf 4095, ep 0x80` twice at boot and once at 4 min 11 s, as in the
first run; nothing followed from it. `runs/` (gitignored) holds every verify, plan, TI-diag, sim, flash and
console text.

## Board state after this gate

| Board | Image | Note |
|---|---|---|
| Pro Micro (EE6986854FD72803) | **`hrtx_promicro_lp_pd`, uf2 875a1aee**, transmitting HR #12345 at 0 dBm, CDC console COM12 | No software path back to the bootloader. Reflashing needs a physical double-tap |
| nRF54L15 DK (001057737173) | **trainer `g2_A`, hex 99c15a0e** (FE-C #4244), restored | Confirmed on air by a TI scan (#4244, #12345, plus the room's #52233) |
| TI LaunchXL | `crcfix_ti_rx`, hex a3862fbf | Receiver, ANT COM11 @57600 (unchanged) |
| Garmin USB-m | unchanged | Used as a receiver only, through libusb0 |

## Open

- **B5 loss: owner call.** B2 = A2, while B1 is high by about one drift step. A closing A3 window, or an
  interleaved A/B/A, needs a double-tap; the alternative is an hr_transmitter build with
  `CONFIG_HR_TX_RELEASE_HFXO=n`.
- The TI and the Garmin share a receive-side loss cause at this placement (not the transmitter). The L15 does
  not see it.
