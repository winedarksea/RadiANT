# Gate G2 rerun with a Garmin slave: absolute turnaround and the burst-opener shift, 2026-11-19

**Follow-up to [Gate G2](../2026-11-19-power-g2/README.md).** Same two trainer images, same instrumentation. A Garmin
ANT USB-m (USB serial `006`) is now the third-party FE-C slave, and a TI arm runs in every session as the tie-in to G2.
No firmware was built or changed.

**Owner decision, 2026-11-19: G2 PASS on master-side timing.** The band is judged by the part of the
turnaround the master owns, which is unchanged within 0.3 µs with the option on. The slave's nominal-period
opener prediction is recorded in TODO.md and not planned.

**Verdict:**

1. **The 2186–2191 µs band can't be judged against the master.** No slave on this bench puts its reply inside a 6 µs
   band. The Garmin's reply wanders over about 100 µs (2159–2268 µs) inside every arm, A included, and its arm means
   move 18–46 µs between A arms. The TI sits at 2207 µs. With the option on, 15 % (B1) and 11 % (B2) of the Garmin's
   acked replies fell in the band, against 1–8 % in A. That is the Garmin's wander, not the master.
   - The part of the turnaround the master owns is its own TX t_sync capture and its RX timestamp. Those are
     unchanged by the option to within 0.3 µs: the TI's acked reply is anchored on the frame it just received, and it
     reads 2207.13–2207.17 µs in A against 2207.30–2207.39 µs in B.
2. **The +7.6 µs burst-opener shift is a slave-side artefact (radiant slave), not a master-side shift.**
   - With the TI it reproduces exactly: opener means 2199.6–2200.5 µs in A and 2208.3–2208.4 µs in B (+7.9 µs).
     Acked data from the same TI in the same arms doesn't move.
   - The code explains it. A radiant slave places a burst START at `radiant_channel_next_slot() + 2190`
     (`api_xfer_t_sync_for()`, ant_radio_radiant.c). The next slot is `t_sync + period_us(c)` from the last heard frame,
     using the *nominal* period (radiant_channel.c, re-anchor in the tracking path). The option makes the master's
     real period 7.34 µs shorter (G1, LF-timed gaps, −29 ppm), so a nominal prediction lands that much later. A
     radiant slave's acked reply goes out from the frame it just received (`ack_pending` path), so it carries no
     prediction and no shift.
   - The Garmin can't resolve 7.6 µs, because its own wander is 5× larger. Its B opener means (2224.3 / 2224.2 µs) sit
     inside the A range (2180.9–2227.4 µs). Nothing in the Garmin data points to a master-side opener shift.
3. **ACK rate and bursts: not worse with the option on.** For the Garmin, the B window-miss rate (4.0 / 5.0 %) sits
   inside the A spread (3.7–5.4 %), and B bursts were 96 / 96 against A 92 / 96 / 93. All 500 TI bursts completed.
   - One caveat: the TI's B acked misses (4 and 5 per 100) are above its A misses (2, 1, 2), p = 0.09 pooled. G2,
     with 300 per arm, showed the opposite direction (B 0.33 / 0.67 % vs A 1.00–1.34 %), and the Garmin shows no B
     excess. A miss is the slave not hearing the trainer's ACK, which is the room floor.

**Every current-related number here is a software proxy, not a measured current.**

## Images (saved in `build/power_images_2026-11-19/`, not rebuilt)

| Arm | image | `trainer_zephyr_zephyr.hex` md5 |
|---|---|---|
| A (option off) | `g2_A` | `99c15a0ea9e241fc3e4de706c18463d8` |
| B (option on, `lowpower.conf`) | `g2_B` | `804e9c276905c97bd84cbde81c4accb2` |

These are G2's images, with G2's bench instrumentation (`../2026-11-19-power-g2/g2_instrumentation.patch`): the
trainer bins `t_sync(slave packet) − master's own TX t_sync` from 2150 to 2229 µs at 1 µs. `sum` takes every value
below 3000 µs, so the **means below include the values above 2229 µs** (`over`). Medians come from the bins.
`images_md5.txt` also lists the ELFs and the TI image.

## Rig

- **L15 DK** (J-Link 001057737173): apps/trainer, FE-C master #4244, 4 Hz, shell on COM3. It was flashed only with
  `scripts/flash_sim_jlink.ps1` and read only with `scripts/jlink_peek.ps1`.
- **Garmin ANT USB-m**, USB serial `006`, chip serial 0x00529F06: the FE-C slave over libusb0, at its default power
  (level 3 = 0 dBm; never set to 4). It replaced the nRF5340 DK's second USB cable.
- **TI LaunchXL**: crcfix_ti_rx (a3862fbf), COM11 at 57600. It was not reflashed and not JTAG-connected.
- **Pro Micro**: hrtx_promicro_lp_pd, transmitting HR #12345 (device type 120). This is co-channel traffic on 2457 MHz,
  but a different channel ID, and it was the same in every arm.

## Protocol

`scripts/chain.sh` ran the arms back to back, A1 B1 A2 B2 A3, from 10:18 to 11:07 local. Each arm is
`scripts/g2g_arm.py LABEL IMAGE runs --phases gsweep,gburst,tsweep,tburst --count 300 --bursts 100 --ti-count 100
--ti-bursts 50 [--lp]`:

1. Flash the image. Trainer: `ackfault preset ideal`, `turn reset`. `S0` (power-diag struct plus 10 register samples),
   then `S1_lp` (B only).
2. **gsweep:** `tools/fec_ack_sweep.py --sim-port COM3 --serial 006 --preset ideal --count 300`. The Garmin sends
   page 51 acked at 1 Hz.
3. **gburst:** `scripts/g2g_burst.py --serial 006 --bursts 100 --packets 8`. This is G2's g2_burst.py with
   `--serial` added. Each burst is written right after a received broadcast.
4. **tsweep:** the same sweep with `--port COM11 --baud 57600 --count 100`.
5. **tburst:** the same bursts from the TI, 50 of them.
6. After every phase the trainer runs `ackfault turn reset` (print, then clear) and `ackfault stats`, so each phase
   has its own histogram. Then `S5_lp` (B only) and `S4`.

`scripts/g2g_summary.py runs --json g2g_summary.json` builds every table below. A 15-command smoke run on A without a
flash came first (not archived).

## Results

### Reply turnaround at the master (µs, slave packet t_sync − master TX t_sync)

| | A1 | B1 | A2 | B2 | A3 |
|---|---|---|---|---|---|
| **Garmin acked**: n | 296 | 300 | 297 | 298 | 296 |
| mean / median | 2203.3 / 2201 | **2197.9 / 2197** | 2221.4 / 2218 | **2201.1 / 2199** | 2222.4 / 2219 |
| min–max | 2160–2266 | 2162–2239 | 2183–2266 | 2162–2248 | 2185–2268 |
| > 2229 (over) | 39 | 9 | 62 | 26 | 62 |
| in 2186–2191 | 25 (8 %) | **45 (15 %)** | 3 (1 %) | **32 (11 %)** | 4 (1 %) |
| **Garmin burst opener**: n | 99 | 100 | 98 | 100 | 95 |
| mean / median | 2227.4 / 2223 | **2224.3 / 2224** | 2197.4 / 2194 | **2224.2 / 2223** | 2180.9 / 2177 |
| min–max | 2206–2267 | 2207–2240 | 2168–2252 | 2210–2243 | 2159–2232 |
| **TI acked** (n = 100) mean | 2207.13 | **2207.39** | 2207.13 | **2207.30** | 2207.17 |
| TI acked range | 2207–2208 | 2207–2208 | 2207–2208 | 2206–2208 | 2207–2208 |
| **TI burst opener** (n = 50–55) mean | 2200.52 | **2208.28** | 2200.31 | **2208.41** | 2199.55 |
| TI opener range | 2200–2201 | 2207–2209 | 2194–2201 | 2208–2209 | 2187–2201 |

Pooled Garmin acked means: A 2215.7 µs, B 2199.5 µs.

- Both B arms sat below every A arm. That is the opposite sign to the TI's opener shift, and smaller than the 18 µs
  between A1 and A2.
- The Garmin's opener ran the other way: B sat inside the A range.

Both readings come from two B arms against a slave whose placement drifts over minutes. A3's openers clustered at
2159–2197 µs, while its acked replies in the same arm clustered at 2205–2229 µs. Neither is evidence of an effect.

The G2 numbers for the TI were: acked 2207.16–2207.35, opener A 2200.65–2200.74, opener B 2208.31–2208.37.

### ACK rate and bursts

| | A1 | B1 | A2 | B2 | A3 |
|---|---|---|---|---|---|
| Garmin sweep ok / window_missed / not_heard (of 300) | 283 / 13 / 4 | 288 / 12 / 0 | 286 / 11 / 3 | 283 / 15 / 2 | 280 / 16 / 4 |
| Garmin window-miss rate | 4.39 % | **4.00 %** | 3.70 % | **5.03 %** | 5.41 % |
| Garmin bursts completed (of 100) | 92 | **96** | 96 | **96** | 93 |
| TI sweep ok / window_missed (of 100) | 98 / 2 | 96 / 4 | 99 / 1 | 95 / 5 | 98 / 2 |
| TI bursts completed | 50/50 | 50/50 | 50/50 | 50/50 | 50/50 |

- Pooled (Fisher exact):
  - Garmin acked misses: A 40/893, B 27/598, p = 1.0.
  - Garmin bursts failed: A 19/300, B 8/200, p = 0.32.
  - TI acked misses: A 5/300, B 9/200, p = 0.09. See the caveat in the verdict.
- The trainer ACKed every acked frame it heard (`ack` = `acked_frames`, `mute` = `deaf` = 0, `too_early` = 0 in every
  phase).
- The Garmin's window misses (3.7–5.4 %) are the Garmin not hearing the trainer's ACK, at the same rate in A and B.
  This rig doesn't say whether that comes from link margin or from its wandering placement.

### LP stats and power proxies (B arms, S1_lp → S5_lp, about 560 s)

| | B1 | B2 |
|---|---|---|
| HFXO request duty, S0 → S4 (A: 100 %) | 2.433 % | 2.445 % |
| HFXO requests in window | 2257 | 2261 |
| XO.STAT running, sampled (A: 20/20) | 1/20 | 0/20 |
| sleeps = wakes | 2210 | 2219 |
| release_cancels (300 + 100 + 100 + 50 slave transfers) | 550 | 549 |
| late_wakes, late_anchors, range/hfxo errors, spurious, edge_order, refused, disagreements | all 0 | all 0 |
| since-boot max_disagree_us / max_wake_slack_us | 31 / 223 | 31 / 223 |

The duty is slightly above G2's 2.36 %, because this session carries more commanded slots. The since-boot gauges
come from boot, as in G1 and G2.

## Verdicts

| Question | Result |
|---|---|
| (1) B turnaround within 2186–2191 µs | **Not a master property; owner call.** The Garmin wanders over about 100 µs in A and B alike (11–15 % of B replies in the band, 1–8 % of A). The TI sits at 2207 µs. The master's own share is unchanged to within 0.3 µs (TI acked A 2207.13–2207.17, B 2207.30–2207.39) |
| (2) +7.6 µs opener shift: TI artefact or master-side? | **Radiant-slave artefact.** It reproduces with the TI (+7.9 µs) and is explained by the slave's nominal-period burst-START prediction against the master's 7.34 µs shorter LF-timed period. The Garmin shows no B-specific opener shift (B 2224 µs, inside A's 2181–2227 µs), but its wander can't resolve 7.6 µs |
| (3) ACK rate and bursts vs control | **PASS.** Garmin window-miss B 4.0 / 5.0 % vs A 3.7–5.4 %; bursts B 96 / 96 vs A 92–96; TI bursts 250 / 250 in every arm. The TI acked B excess (9/200 vs 5/300, p = 0.09) is not significant and was reversed in G2 |

**Side note, not a G2 item:** a radiant slave whose master runs `RADIANT_NRF_LOW_POWER` (or any master whose period
is off nominal by tens of ppm) places its burst START at a fixed per-period error. Here it was +7.9 µs, well inside
the master's reply window, and every burst completed. A slave predicting with its drift estimate would remove it.
That would be a separate change, not proposed here.

## Board state at the end

- **L15:** `g2_A`, hex md5 99c15a0e (A3 was the last arm): the trainer, FE-C master #4244, option off.
- **Garmin:** attached, reset with `tools/ant_probe.py --serial 006` (PASS) after the last arm.
- **TI:** crcfix_ti_rx, unchanged (a3862fbf). Its channel was closed at the end of its last burst run.
- **Pro Micro:** unchanged (hrtx_promicro_lp_pd, transmitting #12345).

## Files

- `runs/`: gitignored, local only. Per arm:
  - `*_plan.txt`, `*_arm.log`
  - `*_{gsweep,gburst,tsweep,tburst}.{txt,json}`
  - `*_sim_*.txt`: trainer shell, `TURN` histograms, `ackfault stats`
  - `*_S*.json` / `_report.txt`: jlink_peek
  - `chain.log`
- `g2g_summary.json`
- `images_md5.txt`
- `scripts/`: `chain.sh`, `g2g_arm.py`, `g2g_burst.py`, `g2g_summary.py` (needs G1's `scripts/g1_lpdecode.py`)
