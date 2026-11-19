# Gate G1 re-judge with the CRC-repair delivery fix on the TI receiver, 2026-11-19

The original G1 (`2026-11-18-power-g1/`) failed only on TI receiver loss. The diagnoses (`-g1-diag`, `-g1-h3`) found
that B's extra TI losses were mostly CRC failures on the frame's last bit, all of them repairable, and that the TI
receiver then dropped every repaired frame. That drop is fixed on master (22c75e3, merge 28d7754). This run repeats
G1's A/B/A with the fixed TI receiver and counts RX_FAIL and CRC failures separately.

## Verdict

> **Owner decision, 2026-11-19: G1 PASS with accepted TI-only excess.** The owner accepts the narrow TI-only
> RX_FAIL excess left after the CRC-repair delivery fix (~0.3-0.4 pp: B 0.69 % against 0.21-0.45 % with the HFXO
> held, `2026-11-19-power-g1-rejudge/`). Cause per H3 (`2026-11-18-power-g1-h3/`): the TI misreads the frame's last
> bit after each HFXO restart; the Pro Micro receiver shows no excess. No TI FREQ_TRIM sweep. The verdict text
> below is the record as judged on the day and is left as written.

**G1 still FAILS on the TI criterion, by a much smaller margin. Every other criterion passes again.**

- **Repaired frames now reach the host.** Every arm has 0 unaccounted misses at the TI (the original B/B2 had 18/41).
  `crc_repaired` (counted only when the frame is repaired and delivered) is 12 / 25 / 10 / 19 / 7. Unrepairable
  frames: 0 in every arm.
- **B's TI delivered-frame loss falls from 1.84 / 2.54 % to 0.69 / 0.69 %.** The A arms read 0.21 / 0.33 / 0.45 %,
  so B is still outside the A spread (the runbook's G1 limit). The excess is about 6-9 packets per 600 s.
- **The residual excess is RX_FAIL, not CRC.** TI RX_FAIL: A 5 / 8 / 11, B 17 / 17. Pooled, B has 34 of the 58 against
  an expected 23.2 (one-sided binomial p = 0.003). On this receiver RX_FAIL equals `api.slots_missed`, and
  `crc_fail - crc_repaired` is 0 in every B arm. So these are slots where no frame was received at all, not failed
  CRCs. The H3 rounds (at −47 dBm) found B's RX_FAIL at A's level. Here the TI ran at −33 dBm, and the RX_FAIL
  excess matches the original G1 (B 27/21 against A 5/7/6). The H3 conclusion that the CRC fix closes the criterion
  does not hold at this signal level.
- **The excess is specific to the TI.** The Pro Micro, in the same windows, lost 0.12 / 0.36 / 0.25 / 0.16 / 0.25 %
  (A1 B A2 B2 A3; p = 0.30 for a B excess). Not one TI miss in either B arm coincided with a Pro Micro miss.
- **Power side holds** (B and B2): `hfxo_req_us`/uptime **1.684 % / 1.683 %** (4.06 requests/s × 4146.5 µs).
  XO.STAT ran in 1/40 samples, CONSTLATSTAT was on in 2/40. In the window, lp_stats show sleeps 2454 and 2456, with
  wakes = sleeps (±1 in flight). **Every fault counter is 0**: late_anchors, late_wakes, hfxo_late_edge2,
  range_errors, spurious, hfxo_errors, edge_order and disagreements, and refused = 0. The since-boot gauges are the
  known boot values (max_disagree_us 37, max_wake_slack_us 209, max_wake_late_us 0).
- **The period and timing match G1.** The TI period is 246283.41-.50 µs in A and 246276.05/.07 µs in B (−7.4 µs,
  the L15 LF offset). The off-grid stddev is 12.5-12.6 µs (A) and 4.9-5.1 µs (B), the same quantisation pattern as G1.

## Per arm (600 s each; `g1r_summary.json`)

| Arm | PM loss % (pkts) | TI loss % (pkts) | TI RX_FAIL | TI unacc. | TI crc_fail | TI crc_repaired | TI unrepairable | TI dBm | TI period µs | hfxo_req % | XO sampled | LP faults |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| A1 | 0.124 (2434) | 0.206 (2431) | 5 | 0 | 13 | 12 | 0 | −33.3 | 246283.498 | 100.0 | 20/20 | n/a |
| **B** | 0.363 (2429) | **0.694** (2420) | **17** | 0 | 25 | 25 | 0 | −33.2 | 246276.054 | **1.684** | 1/20 | **0** |
| A2 | 0.245 (2431) | 0.327 (2429) | 8 | 0 | 10 | 10 | 0 | −33.2 | 246283.408 | 100.0 | 20/20 | n/a |
| **B2** | 0.160 (2433) | **0.694** (2420) | **17** | 0 | 19 | 19 | 0 | −33.2 | 246276.066 | **1.683** | 0/20 | **0** |
| A3 | 0.245 (2431) | 0.450 (2426) | 11 | 0 | 7 | 7 | 0 | −33.2 | 246283.470 | 100.0 | 20/20 | n/a |

The TI columns come from the host (`ant_verify` accounting) and from 0xF7 deltas over the receive window (`ant_diag`
before and after). In A1, `crc_fail` 13 against 12 repaired means one CRC failure was neither repaired nor counted
unrepairable (it was refuted or implausible). Every arm's `ant_verify` exits 1 for the known reason: the sample sends
no common pages 80/81. The Pro Micro read −47 dBm throughout.

## Images

| Board | Image | md5 |
|---|---|---|
| L15 A | `build/g1_A` (hr_transmitter lowpower+quiet, option off) | hex `1fbebd37b8ca70ff7c546cbd797b455a` |
| L15 B | `build/g1_B` (option on) | hex `26e088bc19ff288ee11b9c340e60a665` |
| TI | `C:\wt_crc_repair\build\crcfix_ti_rx` (apps/dongle_ti, `ti_health.conf;ti_diag.conf`, ANT COM11 @57600; `.config` identical to G1's `g1_ti_rx`) | hex `a3862fbfeb32c53a84b9326350ff2d30` |
| Pro Micro | unchanged, pm_diag_acdd9d2 (apps/dongle diag) | uf2 `6f75c634` |

## Commands

```
scripts\flash_ti.ps1 -HexPath C:\wt_crc_repair\build\crcfix_ti_rx\dongle_ti\zephyr\zephyr.hex   # dslite + backchannel reset
python tools\ant_probe.py --port COM11 --baud 57600          # PASS, RADIANT0.01B00
sh scripts/g1r_seq.sh        # A1 g1_A, B g1_B --lp, A2, B2 --lp, A3; 600 s each, output build\g1r_runs
python scripts/g1r_summary.py build\g1r_runs g1r_summary.json A1 B A2 B2 A3
python ..\2026-11-19-power-b5-promicro\scripts\b5_miss_overlap.py <arm> <period_us> promicro ti   # coincidence check
```

`g1r_arm.py` is H3's `h3_arm.py` with 600 s receive windows and SWD reads only outside the window: S0 (struct + 10
samples), S1_lp before, S3_lp and S4 after. It uses only `flash_sim_jlink.ps1` and `jlink_peek.ps1`. `runs/` holds
the text reports locally (gitignored). The JSON and `.rec` files stay in `build\g1r_runs\`.

## What next

The TI-only RX_FAIL excess of ~0.3-0.4 pp after an HFXO restart is now the whole G1 TI gap. The Pro Micro shows none
of it. Options: accept it as a TI-receiver sensitivity (the TI is not the product's receiver), or chase it with a
TI FREQ_TRIM / RX bandwidth sweep, as the H3 README suggests. That is an owner call.

**2026-11-19, owner:** accepted as a TI-receiver sensitivity. G1 is PASS with accepted TI-only excess; the sweep is
not run.

## Board state at the end of this gate

L15 `g1_A` (1fbebd37). TI `crcfix_ti_rx` (a3862fbf). Pro Micro pm_diag_acdd9d2. Gate B5 then changed the L15 and the
Pro Micro: see `../2026-11-19-power-b5-promicro/`.
