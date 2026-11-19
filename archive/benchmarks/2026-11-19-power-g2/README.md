# Gate G2 (pass B4): FE-C follow-on regression with `RADIANT_NRF_LOW_POWER` on the nRF54L15, 2026-11-19

**Every current-related number here is a software proxy, not a measured current.**

**Verdict: PASS on ACK rate, bursts and LP stats. The literal 2186–2191 µs band can't be judged on this rig.**
The only available slave is the TI dongle, and its own reply sits at 2200–2208 µs in every arm, A included. Judged
against the same-window A arms instead:

- The acknowledged-data turnaround is unchanged: 2207–2208 µs in all five arms.
- The burst-opener turnaround moves by **+7.6 µs** in both B arms, and is identical within each image. That is the
  size of the option's known LF-timed period shortening (G1: −7.34 µs per period). It is consistent with a slave that
  places its burst opener from a nominal-period prediction. That reading is not proven.
- All 400/400 B bursts completed. B lost no more ACKs than A did.

Whether the band criterion is met this way is the owner's call.

**Garmin rerun (same day): [2026-11-19-power-g2-garmin](../2026-11-19-power-g2-garmin/README.md).** The band is not a master property (the Garmin wanders over about 100 µs in A and B alike; the master-owned share is unchanged to within 0.3 µs). The +7.6 µs opener shift is a radiant-slave artefact (nominal-period burst-START prediction), not a master-side shift. ACK rate and bursts are not worse with the option on.

Code: the worktree was at local master `4d23321`, plus the bench-only instrumentation in `g2_instrumentation.patch`
(see below). No firmware fix was needed. The only new file committed outside this directory is
`apps/trainer/lowpower.conf`, the conf fragment that turns the option on for the trainer.

## Images (hand west from `C:\ncs\v3.4.0`, `scripts/g2_build.ps1`, worktree `C:\wt_power_g2`)

| Arm | `-Dtrainer_EXTRA_CONF_FILE` | `trainer/zephyr/zephyr.hex` md5 |
|---|---|---|
| A (option off) | `g2_power_diag.conf` | `99c15a0ea9e241fc3e4de706c18463d8` |
| B (option on) | `lowpower.conf;g2_power_diag.conf` | `804e9c276905c97bd84cbde81c4accb2` |

Both images are apps/trainer for `nrf54l15dk/nrf54l15/cpuapp` with `-DRADIANT_BACKEND=nrf`. Both have the shell
console on (COM3 at 115200, DTR), `CONFIG_RADIANT_ACK_FAULTS=y`, no MPSL, `CLOCK_CONTROL_NRF_K32SRC_XTAL=y` (the
DK's LF crystal) and `RADIANT_POWER_DIAG=y` with no log line.

The non-comment `.config` lines differ only in `NRFX_POWER=y` plus `RADIANT_NRF_LOW_POWER=y` and its sub-options
(`LP_HFXO_STARTUP_US=800`, `LP_KEEP_ON_US=3000`, `LP_LINGER_MS=1000`, `LP_STATS=y`).

The enabled shell UARTE does **not** hold the HFXO: B sampled XO.STAT off in 236 of 240 samples.

**Instrumentation (both arms, not for merge, `g2_instrumentation.patch`):**

- In `api_xfer_reply_plan()`, `ant_radio_radiant.c` stores `t_sync(slave packet) − api_ch[].turn_t_sync`. That is
  the master's own hardware-captured TX t_sync for the slot, so both ends of the measurement are on the master's
  TIMER.
- The trainer's hook bins the value (2150–2229 µs at 1 µs; ≥ 3000 µs counts as `later`, i.e. burst continuations).
- `ackfault turn [reset]` prints the bins. Only an acknowledgeable packet reaches the hook, so the bins hold
  acked-data packets and burst packets.

## Rig and protocol

- **L15 DK** (J-Link 001057737173): the trainer, FE-C master #4244, 4 Hz. It was flashed only with
  `scripts/flash_sim_jlink.ps1` and read only with `scripts/jlink_peek.ps1`.
- **TI LaunchXL**: crcfix_ti_rx (apps/dongle_ti, md5 a3862fbf). It was not reflashed and never JTAG-connected. It is
  the dongle under test (the FE-C slave) on COM11 at 57600.
- **Pro Micro**: left on hrtx_promicro_lp_pd, transmitting HR #12345 (device type 120). This doesn't clash with FE-C
  #4244. Garmin not used.

Per arm (`scripts/g2_arm.py`, run back to back by `scripts/chain.sh`, 03:37–04:12 local, in the order
A1 B1 A2 B2 A3):

1. Flash, then take `Sboot_lp` (B only).
2. Trainer: `ackfault preset ideal`, `turn reset`.
3. `S0`: power-diag struct plus 10 register samples. `S1_lp` (B only).
4. `tools/fec_ack_sweep.py --sim-port COM3 --port COM11 --baud 57600 --preset ideal --count 300`: page 51 acked at
   1 Hz, each command joined to the trainer's own record of it. During the sweep, 4 bursts of 25 `jlink_peek` samples
   at random 40–70 s gaps (`S2_*`).
5. `S3_lp`. Trainer `ackfault turn reset`: this prints, then clears, the sweep histogram.
6. `scripts/g2_burst.py`: 100 inbound bursts of 8 packets from the TI to the trainer, each written right after a
   received broadcast. Packet 0 lands in the master's reply window. Every later packet is chained after the trainer's
   ACK, so the trainer must stay awake through the whole chain.
7. Trainer `ackfault turn` plus `stats`. `S5_lp` (B only), then `S4` (struct plus 10 samples).

`scripts/g2_summary.py runs --json g2_summary.json` builds every table below.

## Results

### ACK rate (fec_ack_sweep, preset ideal, 300 commands per arm)

| | A1 | B1 | A2 | B2 | A3 |
|---|---|---|---|---|---|
| ok | 296 | **299** | 296 | **297** | 295 |
| window_missed (trainer ACK on air, TI says FAILED) | 4 | **1** | 3 | **2** | 4 |
| not_heard | 0 | 0 | 1 | 1 | 1 |
| window-miss rate | 1.33 % | **0.33 %** | 1.00 % | **0.67 %** | 1.34 % |
| IMPOSSIBLE / sim_no_tx / too_early | 0 / 0 / 0 | 0 | 0 | 0 | 0 |
| page 71 advanced | 297 | 299 | 298 | 298 | 298 |

- A pooled: 11 misses in 898 ACKed commands (1.22 %). B pooled: 3 in 599 (0.50 %).
- B is not worse than A. It lies at or below the A spread (1.00–1.34 %).
- A miss here is the TI not hearing the trainer's ACK, which is the room's floor. Every ACK the trainer planned went
  out (`acks_sent` equals `ack` in every sweep).

### Bursts (100 × 8 packets per arm, TI → trainer)

All five arms completed **100/100**: A1 100, B1 100, A2 100, B2 100, A3 100.

- The trainer ACKed every one of 800 packets plus 300 acked commands per arm (`acked_frames` 1099–1100, every one
  `ack`).
- Each burst also raised one TX_START (0x0A).
- B2 saw one EVENT_RX_FAIL (0x02), a missed broadcast. It didn't affect any burst.

### Reply turnaround at the master (slave packet t_sync − master TX t_sync)

| | A1 | B1 | A2 | B2 | A3 |
|---|---|---|---|---|---|
| acked page 51 (n) | 300 | 300 | 299 | 299 | 299 |
| acked range, µs | 2207–2208 | 2207–2208 | 2207–2208 | 2207–2208 | 2207–2208 |
| acked mean, µs | 2207.18 | 2207.33 | 2207.16 | 2207.35 | 2207.16 |
| burst opener (n = 100) range, µs | 2200–2202 | **2207–2210** | 2200–2201 | **2207–2210** | 2200–2201 |
| burst opener mean, µs | 2200.74 | **2208.37** | 2200.66 | **2208.31** | 2200.65 |
| outside 2150–2229, or `later` > 7 per burst | 0 | 0 | 0 | 0 | 0 |

Histograms (µs: count):

- Acked:
  - A: 2207:245/251/251, 2208:55/48/48.
  - B: 2207:201/193, 2208:99/106.
- Burst opener:
  - A: 2200:27/34/35, 2201:72/66/65, 2202:1/0/0.
  - B: 2207:6/6, 2208:53/59, 2209:39/33, 2210:2/2.

**Reading:**

1. **The 2186–2191 µs band is not reachable with this slave.** The plan's band is the Garmin/spike-B
   "master → slave reply" figure (docs/spike-b-results.md). The TI's own replies sit at 2200–2208 µs in A as well, so
   B can't be judged against the absolute band. It is judged against A.
2. **Acked data is unchanged:** +0.17 µs in the mean, the same two 1 µs bins.
3. **The burst opener shifts +7.6 µs in both B arms** and is otherwise just as tight (a 4 µs range). In A it is the
   same in all three arms.
   - The size matches the option's known effect: G1 measured the master's period −7.34 µs shorter in B, because the
     gaps run on the LF crystal (−30 ppm on this L15).
   - A slave that places its first burst packet from its own nominal-period prediction (`api_xfer_t_sync_for()`:
     next predicted slot + 2190) would land exactly that much later relative to the master's actual frame.
   - Why acked data doesn't show the same shift was not investigated.
   - Either way the opener stays far inside the master's reply window (2190 ± ACK guard), and every burst completed.

### LP stats and power proxies

| | A1 | B1 | A2 | B2 | A3 |
|---|---|---|---|---|---|
| `hfxo_req_us` / uptime, S0→S4 (~400 s) | 100.000 % | **2.363 %** | 100.000 % | **2.362 %** | 100.000 % |
| HFXO requests (= releases) in window | 0 (held) | 1649 | 0 | 1649 | 0 |
| µs per request | n/a | 5906 | n/a | 5901 | n/a |
| XO.STAT running, sampled | 120/120 | **2/120** | 120/120 | **2/120** | 120/120 |
| CONSTLATSTAT on, sampled | 120/120 | **2/120** | 120/120 | **2/120** | 120/120 |
| flags / constlat_stat at end | 0x3F / 1 | 0x3E / 0 | 0x3F / 1 | 0x3E / 0 | 0x3F / 1 |
| TX / RX ops in window | 2700 / 2400 | 2753 / 2453 | 2702 / 2403 | 2750 / 2451 | 2688 / 2389 |

**`radiant_nrf_lp_stats`, B window S1_lp → S5_lp (~400 s, sweep plus bursts):**

| | B1 | B2 |
|---|---|---|
| sleeps / wakes | 1608 / 1608 | 1609 / 1609 |
| late_wakes, late_anchors, hfxo_late_edge2 | 0, 0, 0 | 0, 0, 0 |
| range_errors, hfxo_errors, spurious, edge_order, refused | 0, 0, 0, 0, 0 | 0, 0, 0, 0, 0 |
| asap_wakes, reanchors, disagreements (window) | 0, 0, 0 | 0, 0, 0 |
| lf_taken / timer_taken | 1608 / 0 | 1609 / 0 |
| release_cancels | 400 | 399 |
| since-boot gauges: max_disagree_us, max_wake_slack_us, max_wake_late_us | 31, 215, 0 | 30, 217, 0 |
| state at each dump | 3 (asleep) | 3 (asleep) |

- **The crystal is released between slots in B.** 98.3 % of samples show XO.STAT off. The request duty is 2.36 %,
  against G1's 1.68 % for hr_transmitter.
- Each request lasts 5.9 ms here, against 4.15 ms on the hr_transmitter master. The extra holding is the follow-on
  path: the master's reply window, and in commanded slots the ACK and burst chain.
- **release_cancels ≈ 400 per window = 300 acked commands + 100 bursts.** Each slave packet in the reply window
  cancels the pending release and keeps the crystal on for the follow-on ACK. That is the path this gate exists to
  exercise, and it worked every time.
- **No fault counters.**
- The since-boot `max_disagree_us` of 30–31 µs comes from boot, as in G1. `Sboot_lp` already shows 31 µs after
  20 sleeps, with `reanchors=1 timer_taken=1`, and the windows have 0 disagreements.

## Verdicts

| Criterion | Result |
|---|---|
| B turnaround within 2186–2191 µs | **Not judgeable on this rig.** The TI slave answers at 2200–2208 µs in A too. Against A: acked unchanged (2207–2208, +0.17 µs mean); burst opener +7.6 µs (2207–2210 vs 2200–2202), consistent with the option's −7.3 µs LF period offset seen by a slave predicting with the nominal period. Owner call |
| B ACK rate not worse than A beyond the same-window spread | **PASS**: B window-miss 0.33 % / 0.67 % against A 1.00–1.34 % |
| Bursts work | **PASS**: 100/100 × 8 packets in both B arms (and in every A arm); trainer ACKed every packet |
| LP stats: HFXO released between slots, no fault counters | **PASS**: 1649 request/release pairs per ~400 s, 2.36 % request duty, XO.STAT off in 236/240 samples, every fault counter 0 |

Follow-ups:

- Measure the absolute turnaround with a non-TI slave (Garmin, which needs the owner) or with the L15 promisc sniffer.
- Check that a third-party slave doesn't place its burst opener with a nominal-period prediction. If one does, the
  +7.4 µs LF offset is still inside the ±guard.

## Board state at the end

- **L15:** image A (`g2_A`, hex md5 99c15a0e…, the trainer, FE-C master #4244, option off). It was hrm_ble 637ed223
  before this gate.
- **TI:** crcfix_ti_rx, unchanged (a3862fbf). Its FE-C channel is closed at the end of each burst run.
- **Pro Micro:** unchanged.

## Files

- `runs/`, per arm:
  - `*_plan.txt` / `*_arm.log`
  - `*_sweep.{txt,json}`
  - `*_burst.{txt,json}`
  - `*_sim_{pre,sweep,burst}.txt`: trainer shell, `TURN` histograms, `ackfault stats`.
  - `*_S*.json` / `_report.txt`: jlink_peek.
- `g2_summary.json`
- `images_md5.txt`
- `g2_power_diag.conf`: bench-only fragment, both arms.
- `g2_instrumentation.patch`
- `scripts/`
