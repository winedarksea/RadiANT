# Pro Micro loses 14-22 % on the nRF5340 masters: B4 cleared (2026-11-16, 16:22-17:00)

Trigger: `../2026-11-16-pass-breakdown/ti_rerun/` (15:44-16:16). In that run the Pro Micro `EE6986854FD72803`
ran the B4 -O2 diag image ba0a7084 and had not been reflashed since gate_extra (~14:46). It lost 14-22 % on the
nRF5340 net-core masters 24001-3. In the same windows the TI LaunchXL lost 0.8-1.6 % on them. The Pro Micro's
loss on the L15 and Dongle masters was normal.
Plan: `C:\Users\Colin\.claude\plans\a-reviewer-had-some-shiny-prism.md` B4. Bench agent draft, not result-checked.

## Verdict

**B4 is not implicated. The degradation is not uptime- or state-dependent, and it is not caused by the TI.**
- It persisted across a fresh flash of the same B image.
- It then disappeared between 16:38 and 16:42 for both images, with no change to the Pro Micro.
- The B image was clean in the two legs after that, including a fresh flash.

What caused it is **not established**. The data point to a temporary few-dB loss on the **Pro Micro <-> 5340
path only**. Possible causes are something in that path, or an interferer near the Pro Micro (inference, see
"Mechanism").

## Runs (120 s window each; same workload as ti_rerun, from one shared --start-at)

Workload, as in ti_rerun:
- 8 ant_sim masters with the same numbers, seeds and periods;
- the scan channel reopened every 4.5 s;
- 9 tracked channels (the 8 masters and trainer 52233:0x11).

The Pro Micro and the TI (COM11 @ 57600, `ti2_o2.hex`, not flashed) each ran the window in their own process.
Masters were reset before each leg, and the pre/post count was 8/8 in every leg. Runner: `leg_anom.py` =
`ti_rerun/leg_ti2.py` with three changes: LEG_S/SIM_S come from the environment (120/330), the outdir is made
absolute, and `NO_TI=1` keeps the TI idle in the window.

| Leg | Window start | PM image | PM boot | TI in window | PM 24001 / 24002 / 24003 | TI 24001 / 24002 / 24003 |
|---|---|---|---|---|---|---|
| s1_aged | 16:24:47 | B ba0a7084 | ~14:46 (aged, as in ti_rerun) | yes | **20.95 / 18.18 / 18.45 %** | 2.48 / 0.62 / 1.26 % |
| s2_fresh_B | 16:30:26 | B, reflashed 16:28 | fresh | yes | **15.82 / 17.29 / 16.95 %** | 1.47 / 1.24 / 0.84 % |
| s3_B_noti | 16:36:02 | B (same boot as s2) | 8 min | **no (idle)** | **20.66 / 23.33 / 20.59 %** | - |
| s4_fresh_A | 16:41:56 | **A c4b4ca99 (no B4)**, flashed 16:41 | fresh | yes | 3.09 / 2.06 / 1.47 % | 2.08 / 1.24 / 0.21 % |
| s5_fresh_B2 | 16:48:18 | **B, reflashed** 16:47 | fresh | yes | **1.23 / 1.03 / 1.25 %** | 1.66 / 1.04 / 1.47 % |
| s6_B_repeat | 16:53:49 | B (same boot as s5) | 6 min | yes | **0.82 / 0.82 / 1.04 %** | 1.03 / 2.07 / 1.48 % |

- **L15 and Dongle masters:** normal in every leg (L15 2.9-5.2 %, Dongle 0-1.9 %), with the TI and the Pro Micro
  close together.
- **s4 transmitter hiccup:** 21001 and 23002 delivered fewer frames to *both* receivers (294/293 and 403/372 of
  ~480). That is on the transmitter side and does not affect the 5340 rows.
- **Trainer FE-C on the Pro Micro:** 26, 35, 85, 33, 19, 16 %. In s3 the trainer was nearly silent (25 frames
  received).
- Raw files per leg:
  - `scan_pm.json` and `scan_ti.json`;
  - `pm_diag_{before,after}.json` and `pm_diag_diff.txt`, with the same files for the TI;
  - health, counts, sim logs and `meta.json`.
- Summaries: `python loss.py <leg>...` and `python diag_table.py <leg>...`.

**Logic of the conclusion:**
- s1 → s2: a fresh boot did not help, so the effect is **not uptime or accumulated state**. That rules out the
  memo LRU stamps, the arm counter, shadow drift and anything else held in RAM.
- s3: removing the TI did not help, so it is **not TI desense or interference**.
- s4 → s5 → s6: after ~16:40 both images are clean, and B is clean twice, once after a fresh flash. The A/B
  difference between s2/s3 and s4 is the transition in time, not the image.

## Pro Micro counters (diff over the window)

| Field | s1 | s2 | s3 | s4 (A) | s5 | s6 |
|---|---|---|---|---|---|---|
| term_empty | 3015 | 3045 | 3290 | 2511 | 2591 | 2711 |
| slots_missed (windows that heard nothing) | 525 | 539 | 579 | 312 | 187 | 170 |
| crc_repaired / crc_unrepairable | 12 / 56 | 25 / 39 | 25 / 30 | 10 / 25 | 5 / 10 | 3 / 11 |
| crc_fail (p4) | 1527 | 1412 | 1710 | 1484 | 1523 | 1499 |
| sw_addr_mismatch | 0 | 0 | 0 | 0 | 0 | 0 |
| late_isr_drops / rx_start_missed / early_etime | 0/0/0 | 0/0/0 | 0/0/0 | 0/0/1 | 0/0/0 | 0/0/0 |
| windows_merged | 74 | 73 | 58 | 162 | 78 | 64 |
| arm_call_max / pass_to_arm_max µs | 74/185 | 74/182 | 74/183 | 224/498 | 74/179 | 74/184 |

- **What marks the bad legs:**
  - +300-400 windows that heard nothing (`slots_missed`, `term_empty`). That roughly matches the 255-300 extra
    5340 misses per 120 s.
  - More CRC repairs and unrepairable frames (30-56 against 10-25).
- That is a **weak-signal signature**: fewer syncs, and bit errors in the frames that do sync. It is not a logic
  signature: `sw_addr_mismatch` is 0, and there are no stale or late events and no start-missed.
- **windows_merged differs by image, not by loss.** B gives 58-78 with or without loss; A gave 162. This is a
  B4 timing side effect on how the scheduler groups windows: the arms are shorter, so the pass grouping
  changes (cause not traced). It is benign here, because the loss follows the clock time, not the merge count.

## Step 0 snapshot (before anything was touched, 16:22)

`s0_snapshot/`:
- `pm_diag_now.{json,txt}` and `pm_health_now.txt`;
- `diff_vs_leg4_after.txt`, against ti_rerun leg4's `pm_diag_after.json`.

Since boot (~14:46, gate_extra txB2 flash) the board shows no anomaly:
- `sw_addr_mismatch` 0, `late_isr_drops` 0, `rx_start_missed` / `tx_start_missed` 0, `early_etime` 0.
- `stale_*` is only `stale_rx_end` (408, about the scan closes).
- p11 `tx_armed` 1311 and p1 `mrx_posted` 1159 are gate_extra's transmit leg on this same boot.
- Since leg 4 only the post-count and its reset ran (135 sched_dones). Pages 5-6 are zeroed by the
  stack reset.
- 0xF6: `tx_drops` 286, `tx_high_water` 32, `tx_timeout` 5. That is the known post-window artefact (channels
  left open with no reader).

## Mechanism (inference, not established)

- **Only the 5340 masters are affected.** At 0 dBm maximum they are the weakest transmitters on the bench
  (`radiant-nrf53-netcore-endpoint`), so a few dB lost on one path shows there first and only there.
- **The TI, at another position, heard the 5340 normally.** So the 5340 transmitted normally, and the loss was
  on the Pro Micro <-> 5340 path or local to the Pro Micro.
- **It was present from at least 15:44 (ti_rerun leg 1) to 16:38, and gone by 16:42.** No bench step happened at
  that boundary: s3 → s4 was a Pro Micro reflash, as s1 → s2 had been. Candidates:
  - an object or person in that path;
  - a 2.4 GHz interferer near the Pro Micro that stopped;
  - something on another agent's or the user's side.

  None of these was observed. In gate_extra, at 14:46-14:52 on B, the Pro Micro tracked 24001 at 0.4-0.7 %, so
  the onset was after 14:52.
- **No code mechanism.** Before s5 cleared the image, I reviewed the B4 diff (`radiant/src/radiant_radio_nrf.c`).
  - The format and filter memos are pure. The keys cover every field that `format_prepare()` and
    `filters_prepare()` / `radiant_addr_split_rx_hw_len()` read.
  - The register shadow is invalidated by every other writer of the shadowed registers (`apply_tx_address`,
    `program_ed` / `ed_enter_index`, init/enable/disable, grant hooks). A grep found no other writer of MODE,
    FREQUENCY, PCNF0/1, CRC*, BASE0/1, PREFIX0/1 or RXADDRESSES.
  - Nothing found in that review can produce master-specific partial loss.

## Not done / spare

- `images/C_b4_noshadow.uf2` (b46b9977) was built so that, if B had stayed guilty, the shadow could be split
  from the memos.
  - Source: `C:\wt_b4` at f90fff9, unchanged; build dir `C:\wt_b4\build\an_pmns`; script `build_noshadow.ps1`.
  - Build: B's row plus `EXTRA_CFLAGS=-DRADIANT_NRF_REG_SHADOW=0`.
  - Its `.config` is identical to B's. `nm` shows `fmt_memo`/`filt_memo` but no `reg_shadow`.
  - **Never flashed**, because s5/s6 made it unnecessary.
- No long A-vs-B soak was run, since the uptime hypothesis was already falsified at s2.

## Bench events

- **My error:** s2 was first started while s1's sims still held COM4, COM5 and the Dongle. Every master reset
  failed, and I killed it within seconds of starting. Its only effects were a TI probe and a PM probe. It was
  deleted and rerun cleanly.
- **Aborted s5:** the first s5 attempt (`aborted_s5_unflashed/`) launched its flash through `powershell -File`
  without `-ExecutionPolicy Bypass`. The flash was refused, and the leg was killed after its TI probe. The PM
  still held A, and no window ran. The rerun flashed B (md5 in `s5_flash_B.log`).
- **Flashes:** only the Pro Micro was flashed. `s2_flash_B.log`, `s4_flash_A.log` and `s5_flash_B.log` record
  the md5 before each copy, re-enumeration and ant_probe PASS.
- The L15, 5340 and Dongle were reset by ant_probe and ran sims only. The TI was never flashed. No repo source
  was changed and nothing was committed.

## Final board state

- **Pro Micro: B4 -O2 diag image ba0a7084**, flashed 16:47 (s5), unchanged through s6.
- TI LaunchXL `ti2_o2.hex` c4e5eb81, unchanged, reset at the end of each leg.
- L15, 5340 and Dongle: unchanged images. Their sims ran out.
