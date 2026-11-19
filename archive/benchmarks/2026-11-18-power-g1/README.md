# Gate G1: nRF54L15 HFXO/timebase release (`RADIANT_NRF_LOW_POWER`), on-air A/B/A, 2026-11-18

**Every current figure here is an ESTIMATE**: software proxies (the `radiant_power_diag` struct, `radiant_nrf_lp_stats`
and `jlink_peek` register samples) multiplied by typical datasheet currents (`tools/ant_power_est.py`). No current meter
was used.

**Verdict: G1 FAILS on one criterion, TI receiver loss.** Every power and timing criterion passes: HFXO duty 1.68 %,
CONSTLAT off between slots, zero LP faults in the measured windows, and the period shift is the LF offset. But both B
arms lost 2-5x more packets at the TI receiver than any of the three A arms around them. The Pro Micro, in the same
windows, moved by about one packet. The L15 was left on image A.

Code: local master 2ec8901 (B3 merged). Nothing in the firmware was changed for this gate.

## Images

Built by hand west from `C:\ncs\v3.4.0` with `scripts/g1_build.ps1` into `build/g1_*` (md5s and symbol addresses in
`images_md5.txt`):

| Image | What | `-DEXTRA_CONF_FILE` | `zephyr.hex` md5 |
|---|---|---|---|
| `g1_B` (option on) | radiant/samples/hr_transmitter, `nrf54l15dk/nrf54l15/cpuapp` | `lowpower.conf;power_diag_quiet.conf` | `26e088bc19ff288ee11b9c340e60a665` |
| `g1_A` (option off) | same | `lowpower.conf;power_diag_quiet.conf;g1_lp_off.conf` (`CONFIG_HR_TX_RELEASE_HFXO=n`) | `1fbebd37b8ca70ff7c546cbd797b455a` |
| `g1_ti_rx` | apps/dongle_ti, `cc26x2r1_launchxl`, core + cc26xx, ANT UART 57600 | `ti_health.conf;ti_diag.conf` (the `ti_launchxl_diag` row) | `fa76ef0f5ace41db1c164251d13ec001` |

The two `.config` files differ only in `HR_TX_RELEASE_HFXO`, `NRFX_POWER` (selected by the option) and
`RADIANT_NRF_LOW_POWER` with its four sub-options (`LP_HFXO_STARTUP_US=800`, `LP_KEEP_ON_US=3000`, `LP_LINGER_MS=1000`,
`LP_STATS=y`). Both have `RADIANT_BACKEND_NRF=y`, no MPSL, `RADIANT_POWER_DIAG=y`, and no log, serial or console.
Symbols: B `radiant_power_diag` 0x20000324, `radiant_nrf_lp_stats` 0x20000390 (0x5C), `lp` 0x20000120 (0xC0);
A `radiant_power_diag` 0x20000264.

## Rig

- **nRF54L15 DK** (J-Link 001057737173): the transmitter, HR master #12345, bidirectional (one RX window per slot).
  Flashed with `scripts/flash_sim_jlink.ps1` and read only with `scripts/jlink_peek.ps1`.
- **TI LaunchXL** (XDS110 L1100TY2): its C2 transmitter image was replaced by `g1_ti_rx` (flashed with
  `scripts/flash_ti.ps1`; the script's backchannel reset worked; `ant_probe --port COM11 --baud 57600` PASS,
  RADIANT0.01B00). This took the TI's own #12345 off the air, and the TI became the second receiver. It was never
  JTAG-connected after the flash.
- **Pro Micro** EE6986854FD72803 (pm_diag_acdd9d2, not reflashed): the first receiver.
- Garmin not used.

Both receivers ran `scripts/g1_verify.py`: `tools/ant_verify.py --profile heart-rate --device-number 12345 --seconds
600 --expect-bpm 120`, plus one addition. Each channel's summary also carries its raw radio-clock receive intervals. The
16-bit 32768 Hz receive timestamp telescopes over the window, so the transmitter's mean period is known to about
0.01 µs on each receiver's own clock.

## Protocol (`scripts/g1_arm.py`)

Per arm:

1. Flash. In B, take an `radiant_nrf_lp_stats` dump straight after boot (`Sboot_lp`).
2. Wait a random 20-50 s.
3. Start both receivers for 600 s.
4. Run the J-Link sessions:
   - `S0`: struct dump plus 10 register samples.
   - No debugger for a random 90-130 s (the "unattached" span).
   - `S1_lp`: lp_stats dump (B only).
   - The main sampling run (`S2`).
   - `S3_lp`: lp_stats dump (B only).
   - `S4`: struct dump plus 10 samples.

`scripts/g1_summary.py` pools every session's register samples. It differences the first `S0` snapshot against the last
`S4` snapshot over ~410-570 s, and writes the estimator input (`<arm>_pooled_peek.json`). `g1_lpdecode.py` decodes the
lp_stats words, using the layout in the `radiant_radio_nrf_lp.c` header.

Arms, in order (local time):

| Arm | Start | Main sampling |
|---|---|---|
| A1 | 17:46 | `S2`: 130 samples, 1.7-4.7 s apart |
| B | 17:57 | `S2`, same schedule. **The SWD link died after 6 reads** (see Anomalies) |
| A2 | 18:08 | `S2`; the link died after 116 reads |
| *(crash)* | | The machine crashed during a first B2 attempt; that attempt's partial files were discarded |
| B2 | 18:34 | Repeat of B with `--burst`: six bursts of 25 samples at 150-450 ms, 40-70 s apart |
| A3 | 18:46 | Control A after the crash, `--burst` |

After the crash, the L15 was found running B (lp_stats clean over ~830 s since its power-on:
`resume_after_crash_lp_stats` in `g1_est_lp_summary.txt`). The TI was found answering `ant_probe` as the RadiANT receiver. Nothing was
reflashed for the resume. B2 and A3 were added because B's sample count was too small for the 3 % bound (56 samples) and
to bracket the TI loss question.

## Results

### Power proxies (struct deltas, `S0` to `S4`; sampled = pooled register reads)

| | A1 | B | A2 | B2 | A3 |
|---|---|---|---|---|---|
| `hfxo_req_us` / uptime | 100.000 % | **1.684 %** | 100.000 % | **1.684 %** | 100.000 % |
| same, unattached span only | 100.000 % | 1.684 % | 100.000 % | 1.683 % | 100.000 % |
| HFXO requests /s, µs per request | 0 (held) | 4.060, 4147 | 0 | 4.061, 4147 | 0 |
| XO.STAT running, sampled | 150/150 | 1/56 (1.8 %) | 136/136 | **3/200 (1.5 %, CI 0.5-4.3)** | 170/170 |
| CONSTLATSTAT on, sampled | 150/150 | 1/56 | 136/136 | **2/200 (1.0 %, CI 0.3-3.6)** | 170/170 |
| TX / RX µs per s | 602.7 / 2592 | 603.4 / 2597 | 602.9 / 2593 | 603.2 / 2597 | 602.9 / 2593 |
| TX = RX ops /s | 4.059 | 4.060 | 4.060 | 4.061 | 4.061 |
| CPU active ‰ | 4.138 | **4.853** | 4.138 | **4.842** | 4.138 |
| Idle entries /s | 25.35 | **45.66** | 25.36 | **45.66** | 25.36 |
| flags / `constlat_stat` at end | 0x3F / 1 | 0x3E / 0 | 0x3F / 1 | 0x3E / 0 | 0x3F / 1 |

B and B2 pooled: XO.STAT 4/256 = 1.56 % (95 % Wilson 0.6-4.0 %), CONSTLAT 3/256 = 1.2 % (0.4-3.4 %). The sampled and
counted HFXO duties agree, and the unattached span matches the attached one, so the attached probe does not hold the
crystal on this part. Each HFXO request lasts 4.15 ms per slot. That covers the 800 µs start-up allowance with its
margin and the arm lead, the 148 µs TX and the 640 µs RX window, and the run to the release edge.

### `radiant_nrf_lp_stats` (B: `S1_lp` to `S3_lp`, 445 s; B2: 307 s)

| | B | B2 |
|---|---|---|
| sleeps / wakes in the window | 1805 / 1805 (4.056/s each) | 1246 / 1246 (4.057/s) |
| late_wakes, late_anchors, hfxo_late_edge2 | 0, 0, 0 | 0, 0, 0 |
| range_errors, hfxo_errors, spurious, edge_order, refused | 0, 0, 0, 0, 0 | 0, 0, 0, 0, 0 |
| asap_wakes, release_cancels, reanchors (window) | 0, 0, 0 | 0, 0, 0 |
| lf_taken / timer_taken (window) | all lf_taken | all lf_taken |
| disagreements (window) | 0 | 0 |
| `last_disagree_q` (every dump) | 0 or 1 µs | 0 or 1 µs |
| gauges since boot: max_disagree_us, max_wake_slack_us, max_wake_late_us | 37, 209, 0 | 37, 209, 0 |
| state at each dump | 3 (asleep) | 3 (asleep) |

The since-boot gauges come from boot. `Sboot_lp`, read about 5 s after reset, already shows `disagreements=1
max_disagree_us=37 timer_taken=1 reanchors=1 max_wake_slack_us=209` with only 5 sleeps done. It shows the identical
values on the B, B2 and pre-check boots. So this is one deterministic event at start-up: the first release after the
boot-time re-anchor finds the LF image 37 µs from the TIMER and correctly takes the TIMER reading. It never recurs. The
counters cannot be reset, so no in-window maximum exists. Every dump's `last_disagree_q` is 0 or 1 µs.

### On air (600 s per arm, per receiver)

| | A1 | B | A2 | B2 | A3 |
|---|---|---|---|---|---|
| **Pro Micro** loss (packets) | 0.00 % (2437/2437) | 0.08 % (2435) | 0.08 % (2435) | 0.12 % (2434) | 0.00 % (2437) |
| Pro Micro mean period, its radio clock | 246281.478 µs | **246274.137** | 246281.478 | **246274.024** | 246281.466 |
| Pro Micro off-grid stddev (radio clock) | 10.94 µs | 8.74 | 10.95 | 8.86 | 10.93 |
| Pro Micro host interval mean / stddev | 246.27 / 9.8 ms | 246.47 / 24.8 | 246.48 / 23.2 | 246.57 / 11.1 | 246.27 / 7.5 |
| Pro Micro signal | −44 dBm | −44 | −44 | −41 (−66..−31) | −41 |
| **TI** loss (packets; RX_FAIL / unaccounted) | 0.41 % (2426; 5 / 5) | **1.84 %** (2391; 27 / 18) | 1.11 % (2417; 7 / 20) | **2.54 %** (2374; 21 / 41) | 0.53 % (2423; 6 / 7) |
| TI mean period, its radio clock | 246283.448 µs | **246276.116** | 246283.460 | **246276.078** | 246283.586 |
| TI off-grid stddev (radio clock) | 12.58 µs | 4.98 | 12.61 | 5.10 | 12.68 |
| TI host interval mean / stddev | 247.29 / 21.3 ms | 250.90 / 37.1 | 249.04 / 45.2 | 252.70 / 42.3 | 247.59 / 19.0 |
| TI signal | −26.3 dBm | −25.4 | −26.2 | −39.7 (−78..−35) | −37.6 |
| TI misses per minute | 1 0 1 3 1 1 2 1 2 0 | 3 2 3 12 7 9 1 3 3 3 | 1 5 1 2 4 2 4 0 1 9 | 17 9 3 3 4 7 7 4 1 7 | 1 1 0 3 0 0 2 2 2 2 |

Heart rate decoded 120 bpm exact in every arm. Each `ant_verify` exits 1 for the known reason: the sample sends no
common pages 80/81.

**Period.** B shortens the period by 7.341 µs on the Pro Micro (A mean 246281.474, B 246274.137) and by 7.338 µs on the
TI. B2 gives 7.45 and 7.42 µs. That is −29.8 ppm, the L15's LF-vs-HF offset from spike S5 (7.4 µs per period). The sign
says the asleep (LFXO-timed) period is the shorter one. The brief expected +7.4 µs; the magnitude matches and the sign
is opposite. The two receivers disagree with each other by ~2 µs per period (their own clocks), identically in every
arm.

**Jitter.** Every received packet in every arm lies within one receiver tick (30.5 µs) of the fitted grid. The
largest residual is 33 µs in B and 26 µs in A. The off-grid stddev is lower in B (8.7-8.9 and 5.0-5.1 µs, against 10.9
and 12.6 µs). That is a quantisation effect, not a cleaner transmitter: B's period sits nearer a whole number of
receiver ticks, so the residuals collapse onto two tick values instead of spreading across them.

### Estimate (`ant_power_est.py --board nrf54l15dk <arm>_pooled_peek.json`; ESTIMATE, µA)

| Component | Basis | A (A1 = A2 = A3) | B | B2 |
|---|---|---|---|---|
| CONSTLAT (ION_IDLE11 − ION_IDLE0) | sampled 100 % / 1.79 % / 1.00 % | **549.2** | 9.8 | 5.5 |
| HFXO outside radio (ITIMER3 row) | 99.68 % / 1.47 % / 1.18 % | **236.3** | 3.5 | 2.8 |
| CPU active (IAPPCPU0) | 0.414 % / 0.485 % / 0.484 % | 10.8 | 12.6 | 12.6 |
| Radio RX (IRADIO_RX0) | 0.26 % | 8.8 | 8.8 | 8.8 |
| Radio TX 0 dBm (IRADIO_TX0) | 0.06 % | 2.9 | 2.9 | 2.9 |
| Idle floor (ION_IDLE8) | always | 2.9 | 2.9 | 2.9 |
| **Total (known parts)** | | **810.9** | **40.5** | **35.5** |
| Total with the crystal-core HFXO row (X2) | | 608.4 | 37.5 | 33.1 |

The CONSTLAT and HFXO terms in B rest on 56 and 200 samples. The backend frees CONSTLAT together with the HFXO, so the
counter's 1.684 % is the better duty for both terms: ~9.3 µA CONSTLAT and ~3.2 µA HFXO, about 39.7 µA in total
(`B_est_counter` in `g1_est_lp_summary.txt` substitutes the HFXO duty only, giving 40.3 µA). **A ≈ 811 µA → B ≈ 36-41 µA (ESTIMATE), a
~20x reduction**, almost all of it CONSTLAT (−540 µA) and the HFXO (−233 µA). CPU rises by 1.8 µA: the extra +20.3
wakes/s are the GRTC release and wake-edge interrupts plus the HFXO callback, about five per slot.

Which numbers are what:

- **Measured proxies:** duties from the struct counters (`hfxo_req_us`, radio µs, CPU µs, idle entries) and the
  sampled XO.STAT and CONSTLATSTAT fractions.
- **Datasheet values:** every mA figure (nRF54L15 Datasheet v1.0 rows, typical at 25 °C and 3 V, cited in each
  estimator output in `g1_est_lp_summary.txt`).
- The ITIMER3 HFXO row leans high, and X2 is a lower bound.
- HFXO start-up current (the crystal's start transient each slot) has no datasheet row and is not in the model.

## Verdicts

| Criterion | Result |
|---|---|
| HFXO duty < 3 %, by `hfxo_req_us`/uptime | **PASS**: 1.684 % (B), 1.684 % (B2). The simulator predicted ~0.7 %; each request is 4.15 ms, not ~1.7 ms |
| HFXO duty < 3 %, sampled XO.STAT | **PASS**: 1.56 % pooled (4/256, CI 0.6-4.0 %); B2 alone 1.5 % (CI 0.5-4.3). The upper CI bound is above 3 %, but the counter agrees exactly |
| CONSTLATSTAT 0 in most samples | **PASS**: 253/256 samples off; `constlat_stat` 0 and flags 0x3E at every B snapshot |
| late_wakes, late_anchors, hfxo_late_edge2, range_errors, hfxo_errors, spurious, edge_order, refused = 0 | **PASS**: all 0, since boot as well as in the window |
| disagreements 0 or tiny, max_disagree ≤ 1-3 µs | **PASS in the windows** (0 disagreements; `last_disagree_q` 0-1 µs). **Caveat:** the since-boot gauge reads 37 µs, from one deterministic release ~2 s after every boot (`Sboot_lp`) |
| sleeps ≈ wakes ≈ 4/s | **PASS**: 4.056 and 4.057/s, sleeps = wakes |
| Mean period shift within ~7.4 µs/period of A | **PASS**: −7.34 µs (both receivers, B), −7.45 / −7.42 µs (B2). The sign is opposite to the brief's "+" |
| Interval stddev ≤ A + 1 µs | **PASS**: radio-clock off-grid stddev 8.7 vs 10.9 µs (Pro Micro), 5.0 vs 12.6 µs (TI). No outliers beyond one tick |
| Loss within the A1-A2 spread, Pro Micro | **PASS (marginal)**: B 0.08 % in [0.00, 0.08]; B2 0.12 % is one packet above it; A3 0.00 % |
| Loss within the A1-A2 spread, TI | **FAIL**: B 1.84 % and B2 2.54 %, against A1 0.41 / A2 1.11 / A3 0.53 % |

### The TI loss

Both B arms lose more at the TI than all three A arms. Interleaved in time, A1 < B > A2 < B2 > A3:

- In B the TI's signal was the same as in A1/A2 (−25 dBm).
- In B2 and A3 it was the same as each other (−40 / −38 dBm).

So the excess does not follow the signal. Most of the extra misses are "unaccounted": the TI saw no RX_FAIL at all
(18 and 41, against 5 / 20 / 7). That means it found no sync word in the window, rather than a failed CRC. In B they
cluster in minutes 3-5. In B2 they cluster in minutes 0-1, but B2 had TI misses in every minute.

The Pro Micro shows at most a one-packet effect in the same windows. Every packet the TI did receive was on the grid
within one tick, so the received B transmissions are not mistimed. That leaves something about B's transmissions that
the TI's receiver is more sensitive to. **Untested hypothesis:** TX frequency. In B, every transmit follows a fresh HFXO
start ~1 ms earlier (800 µs allowance plus margin), so the carrier may not have settled to the crystal's final
frequency. This bench has already found the TI's receiver sensitive to frequency/IF placement (the intFreq finding in
the TI RF-core notes). Two Kconfig-only checks would test it, as no firmware fix was attempted here:

- an arm with a longer `CONFIG_RADIANT_NRF_LP_HFXO_STARTUP_US` (e.g. 1500);
- or a TI receive run against a B image with `CONFIG_RADIANT_NRF_LP_KEEP_ON_US` large enough to keep the crystal on.

## Anomalies and notes

- **The SWD link dies under long gaps between reads.** In B's `S2`, reads 1.7-4.7 s apart failed with `Could not read
  memory` from the 7th read onwards, after a 4.2 s gap. J-Link never recovered within that session. The next session
  connected normally, and the target kept running and transmitting throughout. A2's `S2` died the same way after 116
  reads, so this is not B-specific. A1 survived all 130. B2 and A3 used short bursts at jlink_peek's default 150-450 ms
  spacing and lost nothing. The jlink_peek parse guard caught every failure (rc 2, `PARSE PROBLEM`, `liveness:
  unknown`), and no samples were shifted. For long runs, use several short sessions.
- **No lp_stats faults over ~830 s after the crash.** The board ran unattended on B through a host crash
  (`resume_after_crash_lp_stats` in `g1_est_lp_summary.txt`: 3375 sleeps, all LP fault counters 0).
- **The `lp` struct was not dumped.** The stats show no fault to diagnose, and the failure is on the air.
- B adds +0.71 ‰ CPU and +20.3 idle entries/s (about five extra wakes per slot).
- An HFXO request lasts 4.15 ms per slot, 2.4x the simulator's ~1.7 ms. The extra is probably the TIMER keep-on and
  release-edge placement after the RX window. It is not broken down here; it costs ~1 % of HFXO and CONSTLAT duty
  (~7 µA ESTIMATE).
- **Tracked here:** this README, `images_md5.txt`, `g1_lp_off.conf`, the scripts, `g1_summary.json` (every per-arm
  figure above, from `scripts/g1_summary.py build/g1_runs . A1 B A2 B2 A3`) and `g1_est_lp_summary.txt` (every
  estimator output and lp_stats decode). The per-session jlink_peek reports and JSON, the `ant_verify` outputs
  (with the radio intervals), the arm logs and the estimator JSON sit in `runs/`, which the repo's archive rules keep
  local. The record files (`*.rec`), raw J-Link logs and flash logs stay in `build/g1_runs/`.

## Board state after the gate

| Board | Image | md5 |
|---|---|---|
| nRF54L15 DK (001057737173) | **A**, `build/g1_A` (hr_transmitter, option off, power_diag quiet), transmitting HR #12345 | `1fbebd37b8ca70ff7c546cbd797b455a` |
| TI LaunchXL (L1100TY2) | `build/g1_ti_rx` (apps/dongle_ti receiver, ANT on COM11 @57600); not transmitting | `fa76ef0f5ace41db1c164251d13ec001` |
| Pro Micro (EE6986854FD72803) | unchanged, pm_diag_acdd9d2 | — |
