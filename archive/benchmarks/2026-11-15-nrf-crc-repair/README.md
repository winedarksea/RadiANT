# nRF52840 (Pro Micro): CRC repair on air, gate C1, 2026-11-15 — NOT ENGAGED

Step C1 of the plan acting on the 2026-11-15 performance review. It repeats
`archive/benchmarks/2026-11-13-ti-crc-repair/` on the nRF backend. The receiver
uses the default short match, and repair runs with the Phase A pages 9 and 10.

**Verdict: the gate did not pass. Repair was "not engaged" at all three rungs
the plan allows** (the first rung plus two changes). No rung reached 20
repairs in 900 s: -28 dBm gave 11, -20 dBm gave 11, 0 dBm gave 0. No
criterion failed outright. Every delivered payload matched a transmitted one,
and the page 10 counters reconcile with the script. But the plan says fewer
than 20 is not a pass, so the "On a pass" config changes must not be made from
this run.

**Owner ruling, 2026-11-15:** the 22 repairs across both rungs with 0
mis-repairs are accepted as the evidence, and the "On a pass" changes go
ahead on that basis. An independent result check (2026-11-16) recomputed
every number in this README from the raw files and found them all correct.
Its corrections to the wording are applied below.

## Rig

- **Receiver (unit under test):** Pro Micro nRF52840 clone, USB serial
  `EE6986854FD72803`.
  - Image `C:\wt_rxsnap\build\b3_pm_crcdiag\dongle\zephyr\zephyr.uf2`, md5
    `CAC3E56A82714A1F8C481B3C10B2488B`, UF2 address 0x26000, family 0xADA52840.
  - Built at 18:17 from `C:\wt_rxsnap` at `3957126`. `ff32752` (18:29, tip
    of `fix/nrf-rx-overwrite`, merged to master as `d3bca53`) changes only a
    comment, so the code equals `ff32752` / `d3bca53^2`. Board `promicro_nrf52840/nrf52840/uf2`.
    `EXTRA_CONF_FILE=crc_repair.conf;diag_counters.conf`.
  - `.config` read back: `RADIANT_BACKEND_NRF=y`, `RADIANT_CRC_REPAIR=y`,
    `RADIANT_PROFILE_SANITY=y`, `RADIANT_DIAG_COUNTERS=y`,
    `RADIANT_SEARCH_SHORT_MATCH=y`, direct build (no MPSL gate).
  - Flashed with `ant_probe --reboot-to-bootloader` and a copy to NICENANO
    (bootloader 0.6.0, S140 6.1.1). It re-enumerated 3 s later and
    `ant_probe` passed.
  - Checked over 0xF7 after the flash (`diag_verify_after_flash.log`): pages 9
    and 10 answer, and page 10 [0..3] read 0, not 0xFFFFFFFF, so repair is
    compiled in.
  - **The board is left on this C1 image.**
- **Transmitter:** nRF54L15 DK RadiANT UART dongle, COM4 at 115200 (699e884
  image, `ant_probe` PASS). It runs as an `ant_sim` power meter #14871
  (0x3A17), trans type 5, RF 57, `--tx-part nrf54l15`, with raw TXPOWER bytes.
  - The receiver's slave channel is pinned to that device number, as in the TI
    run.
- **Script:** `crc_truth_nrf.py` is a copy of the TI run's `crc_truth.py` with
  two plumbing changes and none to the counting logic:
  - the tools path now points at the main checkout (master `3b486c3` tools);
  - a new `--rx-serial` argument opens a USB receiver.
- Garmin, TI, and the 5340 DK were not used.

## Result per 300 s block

Sources: `truth_bN_*.json` for sent, delivered and not-transmitted;
`diag_diff_bN.txt` (page diffs) for the rest; `reconcile.json` for the table.

| block | rung | RSSI at PM | sent | delivered | loss | never transmitted | backend CRC fail (p4) | offered to repair (p10 0-3) | repaired | unrepairable | refuted / implausible | fw delivered − script |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | -28 dBm | -68.7 | 1205 | 1038 | 13.9 % | 0 | 18 | 13 | 4 | 9 | 0 / 0 | +4 |
| 2 | -28 | -70.0 | 1206 | 1023 | 15.2 % | 0 | 23 | 17 | 5 | 12 | 0 / 0 | +8 |
| 3 | -28 | -70.7 | 1206 | 1016 | 15.8 % | 0 | 20 | 15 | 2 | 13 | 0 / 0 | +5 |
| 4 | -20 | -61.0 | 1206 | 1117 | 7.4 % | 0 | 15 | 9 | 3 | 6 | 0 / 0 | +6 |
| 5 | -20 | -57.5 | 1205 | 1133 | 6.0 % | 0 | 15 | 11 | 4 | 7 | 0 / 0 | +8 |
| 6 | -20 | -57.3 | 1206 | 1133 | 6.0 % | 0 | 16 | 11 | 4 | 7 | 0 / 0 | +6 |
| 7 | 0 | -37.1 | 1206 | 1203 | 0.25 % | 0 | 3 | 0 | 0 | 0 | 0 / 0 | +3 |
| 8 | 0 | -37.0 | 1205 | 1202 | 0.25 % | 0 | 6 | 0 | 0 | 0 | 0 / 0 | +3 |
| 9 | 0 | -37.0 | 1205 | 1202 | 0.25 % | 0 | 3 | 0 | 0 | 0 | 0 / 0 | +2 |

Per rung (900 s each):

| rung | repaired | unrepairable | repair share of offered | gate (≥ 20) |
|---|---|---|---|---|
| -28 dBm | 11 | 34 | 24 % | not engaged |
| -20 dBm | 11 | 20 | 35 % | not engaged |
| 0 dBm | 0 | 0 | — | not engaged |

**Elapsed time.** Each block ran 301.3–301.6 s (`times_bN.txt`). Rung -28 ran
19:49:44–20:05:22, rung -20 ran 20:06:10–20:21:19, and rung 0 ran
20:22:06–20:37:15 (local time, 2026-11-15).

## Against each criterion

1. **`crc_repaired` ≥ 20 within 900 s at one rung: NOT MET.** The best rungs
   reached 11. Across all 2700 s, page 10 [0] reads 22 since boot
   (`diag_after_b9.log`). That total spans two rungs, so it does not satisfy
   the criterion as written.
2. **0 delivered payloads never transmitted: MET.**
   - `rx_not_transmitted` is 0 in all nine `truth_bN_*.json` files.
   - That covers 10,067 delivered of 10,850 sent. The script cannot identify
     repaired frames; any repaired frame delivered before t_end was checked.
   - A mis-repair (≥ 3 bits from the truth) passing by landing on another
     payload sent in the same block is estimated at ~1e-5 per mis-repair.
     0 of 22 bounds the mis-repair rate only to < ~14 % (95 %, rule of
     three); `test_crc_repair.c` covers the 1-in-341 figure.
   - Page 10 [2] refuted and [3] implausible stayed 0 throughout.
3. **Page 10 agrees with the script's delivered count: MET.**
   - The firmware-side estimate is page 0 `rx_sched_events` minus the tracked
     CRC-fail events offered to repair (page 10 [0..3]), plus the repaired ones
     (`reconcile.py`).
   - It exceeds the script's delivered count by +2 to +8 in every block. That
     is the expected tail: the script stops reading at t_end, but the receiver
     channel stays open about 1–2 s until close (4 Hz), and block 1 includes
     the one search-window acquisition frame (page 5 windows/acquired 0→1 in
     block 1, unchanged after). This is agreement within an unmeasured tail,
     not an exact match.
   - Repaired never exceeds delivered, and no delivered payload is foreign.

## Why it did not engage (observations, not retuned)

- The L15's floor is -28 dBm. It is the lowest TXPOWER value that fits the
  one-byte custom escape; Neg40 is 0x130. So the rung could only go up.
- At -28 dBm the Pro Micro hears the link at about -70 dBm, roughly 25 dB above
  sensitivity. So the CRC failures come from room interference, not thermal
  noise. Most of them are multi-bit: 34 of 45 offered were unrepairable. TI
  saw 7 of 35 unrepairable at -63 dBm on the same transmitter rung.
- More power gave fewer failures with a slightly better repairable share
  (-20 dBm: 11 of 31). At 0 dBm none were offered at all.
- Repairs held at about 3.7 per 300 s at both lower rungs. The 20 needed in
  900 s would take about 6.7 per 300 s.
- Options for the owner, none of them taken here:
  - a transmitter that can go lower, such as the 5340 net core at -40 dBm,
    which would reach the noise-error regime where single-bit failures
    dominate;
  - a longer window at -28 dBm, which would reach 20 in about 25 min. The plan
    caps a rung at 900 s.
  - accepting the 22 repairs across both rungs with 0 mis-repairs as the
    evidence.

## Anomaly: backend CRC failures that never reach the repair counters

- Page 4 `crc_fail` counts frames with a matched filter (`fidx >= 0`) and a
  bad CRC. It always exceeds page 10's offered sum.
- The surplus is 5, 6, 5 at -28 dBm, 6, 4, 5 at -20 dBm, and 3, 6, 3 at 0 dBm.
  At 0 dBm, all 12 backend CRC failures went unoffered.
- The surplus barely depends on power, so it is probably not air errors on the
  tracked link.
- Result check (2026-11-16, line refs at `ff32752`): in every block
  p4 `crc_fail` − offered = page 3 `sync_entries` − page 0 `rx_sched_events`,
  and page 3 `term_ok` = offered + 1.
  - `rx_sched_events` is the first statement of `api_sched_rx()`
    (`ant_radio_radiant.c:2296`), ahead of all its exits, so the unoffered
    failures never enter it. They drop between `radiant_radio_nrf.c:4146` and
    there, most likely at the scheduler's stale-op check
    (`radiant_sched.c:2252`, counted only in sched `stale_events`, which no
    0xF7 page shows).
  - They share windows that ended on a good frame, so no payload was lost.
    Benign for criteria 2 and 3. Exposing `stale_events` would settle it.
- Page 5 `frames_ok` fell 5→3 across block 6 and 4→3 across block 9. In code
  it only increments (`radiant_search.c:999`) and is only zeroed by a
  whole-struct memset that would also have zeroed `windows`. **Unexplained**;
  possibly a read or memory fault.

## First hardware read of the new instrumentation

Final values, `diag_after_b9.log`. They are levels since boot under a single
tracked channel at 4 Hz, so this is light load, not the C2 load.

| field | value |
|---|---|
| page 9 [0] sweep_max_us | 11 — **the A0 cycle clock runs** (not 0xFFFFFFFF; 7 right after boot) |
| page 9 [1] decide_max_us | 48 |
| page 9 [2] arm_call_max_us | 91 |
| page 9 [3] done_cb_max_us | 0 — **artefact, not a measurement**: hal_rx-path callbacks are timed into `pb`, which the next `pass()` zeroes |
| page 9 [4] book_max_us | 0xFFFFFFFF (`SCHED_BOOKING` off in this build) |
| page 9 [5] pass_to_arm_max_us | 138 |
| page 9 [6] arm_slack_min_us | 178 |
| page 9 [7] early_etime | 0 |
| page 10 [4] late_isr_drops | 0 |
| page 10 [5] end_late_max_us | 17 |
| page 10 [6], [7] | 0xFFFFFFFF (cc26xx-only fields, as specified) |

- Page 8 `pass_max_us` was 158. `pass_over_100us` equals `sched_dones` (10,823
  of 10,823): every window-arming pass (one per sched_done) exceeds 100 µs;
  housekeeping passes stay under. Wiring checked: 1 MHz radio TIMER, µs, no
  unit mix (sweep 11 + decide 48 + arm 91 = 150 ≈ 158).
- Page 7 `tx/rx_start_missed` and `trk_missed_behind_scan` stayed 0.
- 0xF6 read `tx_drops` 0 and `tx_high_water` 0 (`health_after_b9.txt`).

## Files

Only this README is in git. The files listed below live on the bench machine
only (the directory is gitignored).

- `commands.txt`: exact command lines.
- `run_rung.ps1`: the block loop used for rungs -20 and 0. Rung -28 ran the
  same commands inline.
- `crc_truth_nrf.py`: the adapted script.
- `reconcile.py` → `reconcile.json`.
- `truth_bN_<rung>.json/.log`: script output.
- `diag_before_bN` / `diag_after_bN` `.json/.log`, and `diag_diff_bN.txt`.
- `diag_verify_after_flash.*`: the page check after the flash.
- `health_after_b3.txt`, `health_after_b9.txt`: 0xF6 reads.
- `probe_*.log`, `reboot_to_bootloader.log`.
