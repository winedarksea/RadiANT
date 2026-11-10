# Phase 1 (d2) raw logs

All files are in `C:\adr18_logs\phase-1-d2\`, outside the repository. Measured
2026-11-10 on branch `wip/adr18-d2`. Report: [d2.md](d2.md).

## Images

| File | What it is |
|---|---|
| `img_d2_gate_s1.hex` ... `img_d2_gate_s13.hex`, `build_d2_gate1..13.log` (+`.cmd`) | the intermediate d2 `gate.conf;phase_diag.conf;d2_stats.conf;health.conf` images, one per on-air finding (d2.md, "What changed once it was on air"). s1 = 9bc91e9 (first on air); s3 adds the notice bins; s5 the idle-session preference; s6 the held placement; s7 `d2_settle_now()`; s8 the late drop; s9-s11 the release diagnostics; s12-s13 the post-slot timing attempts |
| `img_d2_gate_s14.hex`, `img_d2_gate_s14_config.txt`, `build_d2_gate14.log` | d2 at 78ce810, `gate.conf` (SHA-256 `3f214b96...`) |
| `img_d2_gate_79ce02f.hex`, `..._config.txt`, `build_d2_gate16.log` | **final d2 `gate.conf` image**, 79ce02f (SHA-256 `f1812af0...`): R1 walk and discovery leg |
| `img_d2_thread_s14.hex`, `..._config.txt`, `build_d2_thread5.log` | d2 at 78ce810, `thread.conf;phase_diag.conf;d2_stats.conf;health.conf` (SHA-256 `85d04f36...`), `MPSL_TIMESLOT_SESSION_COUNT=3` |
| `img_d2_thread_s15.hex`, `..._config.txt`, `build_d2_thread6.log` | d2' at 946a6a5 (the extension skip), same confs (SHA-256 `ce81b4f1...`) |
| `img_d2_ble_s14.hex`, `img_d2_ble_79ce02f.hex` (+ configs), `build_d2_ble1/2.log` | d2 `coex.conf;ble_load.conf;phase_diag.conf;d2_stats.conf;health.conf`; the BLE leg used 79ce02f (SHA-256 `03ee0b3c...`), `SESSION_COUNT=2`, `BLE_LOAD=y` |
| master controls | (a)'s 237b7c4 images, reused: `C:\adr18_logs\phase-1-a\img_a_gate.hex`, `img_a_thread_237b7c4.hex`, `img_a_ble_237b7c4.hex` (see `a-raw-logs.md` on `wip/adr18-a`) |
| `flash_dut_s1.txt`, `*/flash.txt` | `flash_sim_jlink.ps1` output for every flash of the L15 (`1057737173`) |
| `identity.txt` | `ant_probe` on the Dongle, the Pro Micro, the Garmin and the DUT (COM4) before the legs |
| `ot_leader_flash.txt` | the 5340 DK flashed as the OpenThread leader (`C:\wt_timing` `build\p4peer`), `peer state: leader` |

## Isolated rule-1 readings on `gate.conf` (`s*_rule1/`, `m_rule1*/`)

ch0 pinned to #2002 (Dongle, `ant_sim` HR), ch1 searching for absent #9999, the
Garmin (`006`) on #2002 in the same window, 120 s unless noted. Each has
`dut.json`/`dut_verify.txt`, `ref.json`/`ref_verify.txt`, `dut_log.txt` (COM3,
DTR: `D2STAT`, `D2BOOK`, `D2EL`, `D2REL`, `D2SETTLE`, `TEMPDIAG`, `TDIAG` every
5 s), `sim.txt`, `a_reset.txt`, `health.txt` (0xF6).

| Dir | Image | DUT ch0 | Garmin | What it showed |
|---|---|---|---|---|
| `s1_rule1/` (60 s), `s1_rule1b/` | s1 | 231/243, 453/487 | 0.0 %, 0.8 % | all tracked slots booked; 23/64 elastic refusals; el lead to 12 ms |
| `m_rule1/` | master | 435/483 (9.9 %) | 0.8 % | room control |
| `s2_rule1/` | s2 | 451/483 | 0.8 % | refusals not adjacent to a booking (`adj=0`) |
| `s3_rule1/` | s3 | 455/486 | 0.0 % | `D2EL`: 43/43 refusals deferred with < 2 ms notice, 0/970 refused at >= 2 ms |
| `s4_rule1/` | s4 (3 ms lead) | 448/487 | 0.0 % | 448 refusals, all < 1 ms, all deferred |
| `s5_rule1/` | s5 | 455/487 | 0.6 % | 51 refusals at 2-8 ms, none deferred, none adjacent |
| `s6_rule1/`, `s7_rule1/` | s6, s7 | 448/487, 424/486 | 0.8 % | held placements (`dfl`) then refused at < 1 ms |
| `s8_rule1/` | s8 | 414/486 | 0.2 % | 0 refusals; every post-slot chunk late-dropped after a 4.3 ms hold |
| `s9_rule1/`, `s10_rule1/`, `s11_rule1/` (40 s) | s9-s11 | 138, 151, 121 | 0.0-0.6 % | the release compare is accurate (`cap = est`); `t0fix bs` = frames received |
| `s12_rule1/`, `s13_rule1/` (90 s) | s12, s13 | 265/365, 322/357 | 0.5 %, 0.0 % | room swing; post-slot timing not yet effective |
| `s14_rule1/` (90 s) | s14 (78ce810) | 344/364 (5.5 %) | 0.3 % | 0 refusals, 0 held, 0 late drops, all slots booked |
| `m_rule1b/` (60 s) | master | 156/244 (36.1 %) | 0.0 % | room control |

## Thread leg (`thread*/`)

Run by `stackleg_d2.ps1` (scratchpad): flash, wait for the MTD to attach (`ot
child table` on the 5340 console, logged to `ot_child_table.txt`), the 120 s
rule-1 reading in `rule1/` (laid out as above), a 0xF6 read, then three in-band
`ant_disc_phase.py` trials at delays 195, 75, 180 ms in `disc/`. Console beside
each.

| Dir | Image | Result |
|---|---|---|
| `thread1/` | d2 s14 | ch0 9.7 %, Garmin 0.19 %, `hi 419/419/0/418`, `ext=0/3470`, mask `0x00fffffe`; **MPSL ASSERT 69:108** at 4173 placements (`rule1/dut_log.txt` tail), so no 0xF6 and the disc trials found no startup |
| `thread_master/` | master | ch0 7.6 %, Garmin 0.19 %, missed/denied 0/0, mask `0x00249124`, in-band 0/3 |
| `thread2/` | d2 s14 | ch0 154 frames in ~40 s (7.8 %), Garmin 0.41 %; **MPSL ASSERT 69:108** at 1608 placements |
| `thread3_skip/` | d2' s15 | ch0 5.6 %, Garmin 0.00 %, missed/denied 0/89 (scan), `hi 1524/1524/0/1521`, `ext=0/333`, `ext_skipped=6828`, in-band 2/3, no assert (8987 placements) |
| `thread_master2/` | master | ch0 6.6 %, Garmin 0.19 %, 0/0, mask `0x00249124`, in-band 0/3 |
| `thread4_skip/` | d2' s15 | ch0 6.8 %, Garmin 0.00 %, 0/89, `hi 1469/1469/0/1467`, `ext=0/336`, in-band 2/3, no assert (9018 placements) |

## BLE leg (`ble*/`)

Same script without the Thread attach; 1 s connectable advertiser
(`ble_load.conf`).

| Dir | Image | Result |
|---|---|---|
| `ble1/` | d2 79ce02f | ch0 428/487 (11.9 %), Garmin 0.21 %, missed/denied 0/44, `hi 487/485/2/484` (2 bookings BLOCKED), in-band 2/3 |
| `ble_master/` | master + BLE load (`img_a_ble_237b7c4.hex`) | ch0 444/487 (8.8 %), Garmin 0.20 %, 0/42, masks `0x003ffffc`/`0x003fcffc`, in-band 1/3 |
| `ble2/` | d2 79ce02f | ch0 399/487 (18.1 %), Garmin 0.19 %, 0/51, `hi 487/481/6/481` (6 bookings BLOCKED), in-band 2/3 |

## Discovery on `gate.conf` (`gate_disc_*/`)

`stackleg_d2.ps1 -DiscOnly -Delays 195,210,75,165,180,15,120`: `disc/` holds
`run.txt`, `results.jsonl`, `summary.json`, `dut_log.txt`, `tNN.json`,
`tNN_verify.txt`; the `legsum.py` summary is at the end of the console.

| Dir | Image | Result |
|---|---|---|
| `gate_disc_d2/` | d2 79ce02f | 6/7, in-band 4/5 (miss at 4.6 ms), first packet 8.7-9.2 s, masks `0x00ffffff`/`0x01ffffff` |
| `gate_disc_master/` | master | 3/7, in-band 1/5 (221.0 ms), masks `0x003fcffc`; first trial's ch0 got 0 packets |

## R1 walk (`r1walk*/`)

`r1walk_d2.ps1`: `ant_disc_phase.py --r1-walk --seconds 60`, master A = Dongle
#2002, master B = Garmin #5005 (accepts a period change on an open channel),
reference on B = Pro Micro. Each dir has `run.txt`, `r1w_summary.json`,
`r1w_dut.antser`, `r1w_dut.json`/`r1w_dut_verify.txt`,
`r1w_ref.json`/`r1w_ref_verify.txt`, `dut_log.txt`, `console.txt`.

| Dir | Image | A | B | Reference on B |
|---|---|---|---|---|
| `r1walk/` | d2 79ce02f | 217 (11.0 %) | **218 (10.6 %)** | 0.00 % |
| `r1walk_master/` | master | 221 (9.3 %) | 0 (100 %) | 0.00 % |

## Tests

| File | What it is |
|---|---|
| `ztest_gate_1.txt`, `ztest_gate_final.txt` | `radiant\tests\gate` on the 5340 DK (COM6): 32/32 + 9/9, then 32/32 + 10/10 |
| `ztest_main_1.txt` (the R1 table bug), `ztest_main_2.txt`, `ztest_main_final.txt` | `radiant\tests`: 50 suites, 983 cases, all pass on the final run; every `deaf-shape`, `d2-*` and `r1-shape` line |
| `ztest_api_1.txt`, `ztest_api_final.txt` | `radiant\tests\api`: 65/65 |
| `twister_bluefin.txt`, `branch_snapshot_3f2848f.tar` | native_sim twister on Bluefin over `git archive` of 3f2848f: 13 scenarios, 4250/4250, counts OK |

## Sizes and zero cost

| File | What it is |
|---|---|
| `size_l15_bridge.log` (+`.cmd`, `_out.txt`) | L15 `thread.conf;bridge.conf` at 78ce810: FLASH 326 636, RAM 198 552 |
| `size_l15_matter.log` (+`.cmd`, `_out.txt`) | L15 `thread.conf;matter.conf`: FLASH 822 180, RAM 253 900 |
| `size_n52840_matter.log` (+`.cmd`, `_out.txt`) | nRF52840 DK Matter: FLASH 814 952, RAM 245 336 |
| `zero_cost.txt` | `scripts/check_zero_cost.ps1 -BaseRef 237b7c4` |
