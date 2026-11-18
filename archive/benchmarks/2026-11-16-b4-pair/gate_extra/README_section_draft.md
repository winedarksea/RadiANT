## Gate extras: conformance and the transmit path (2026-11-16 pm)

**Draft by the bench agent, not yet result-checked.** These are the B4 gate items the load legs did not cover:
"conformance transcripts pass", and the transmit path. In the load legs page 11 `tx_armed` was 0, so the
register-shadow invalidation in `apply_tx_address()` had never run on hardware. Same images as the legs above:
A = `C:\wt_c2\build\pm_o2\...\zephyr.uf2` c4b4ca99 and B = `C:\wt_b4\build\r2_pmdiago2\...\zephyr.uf2`
ba0a7084. Every step started from a fresh flash with the md5 checked (`*/flash.log`). Raw data, scripts and
commands are in `gate_extra/` (`commands.txt`, `md5sums.txt`).

### 1. Conformance: PASS

`tools/ant_conformance.py --serial EE6986854FD72803`, run from the repo root.

| Transcript | Records | sha256 |
|---|---|---|
| reference `archive/captures/serial/conformance-nrf-radiant.antser` | 1319 | 3104ad79…5fe0c |
| B (`conformance-B.antser`) | 1319 | 3104ad79…5fe0c |
| A (`conformance-A.antser`) | 1319 | 3104ad79…5fe0c |

`--compare` reports "PASS: byte-identical" for reference vs B, A vs B, and reference vs A
(`compare_ref_vs_B.txt`, `compare_A.txt`). No allowed-differing case was needed. The reference is still
current for master + O2 as well.

### 2. Transmit path: B matches A, with one caveat on the TI link

**Workload** (`tx_leg.py` and `tx_role.py`, 300 s per leg, legs in the order B1, A, B2):

- **Pro Micro, one process, one radio:**
  - ch0 is an ANT+ master, CSC #25001 (0x79, 8086 counts, trans 5). Its payloads come from ant_sim's CSC
    sensor, paced by EVENT_TX.
  - On every 8th master slot the Pro Micro loads an acknowledged message (0x4F, byte 0 = sequence) instead
    of a broadcast, then waits for TRANSFER_TX_COMPLETED / FAILED.
  - ch1 tracks the 5340 net-core HR master #24001 (8070). ch2 tracks the nRF52840 Dongle power master #23002
    (8182). Both are pinned slaves with infinite search timeout.
  - The three periods differ, so every phase gets swept: TX arms, ack RX windows and tracked RX arms all
    interleave.
- **TI LaunchXL**, COM11 @ 57600, `ti_fix.hex`, not flashed. Receive only. It tracks #25001 (the Pro Micro's
  master), #24001 and #23002 in the same wall-clock window. It counts broadcasts and acked messages
  (distinct or duplicate by sequence byte), RX_FAIL and GO_TO_SEARCH.
- **Masters:** ant_sim on the 5340 (COM5) and the Dongle, reset before and after each leg. The L15 was not
  used.

| | txB1 (B) | txA (A) | txB2 (B) |
|---|---|---|---|
| p11 `tx_armed` (diff) | **1312** | 1270 | **1311** |
| p4 `sw_addr_mismatch` | **0** | 0 | **0** |
| p4 `crc_fail` (diff) | 4 | 1 | 2 |
| p9 [7] `early_etime` | **0** | 0 | **0** |
| p7 `tx_start_missed` / `rx_start_missed` | **0 / 0** | 0 / 0 | **0 / 0** |
| p2 `tx_expired` / p11 `arm_rejected` | 0 / 0 | 0 / 0 | 0 / 0 |
| PM tracks #24001 (5340) loss | 0.66 % (8/1214) | 0.25 % (3/1214) | 0.41 % (5/1213) |
| PM tracks #23002 (Dongle) loss | 0.08 % (1/1198) | 0.08 % (1/1196) | 0.42 % (5/1195) |
| **TI hears PM master #25001** | **8.17 %** (99/1212) | **12.27 %** (149/1214) | **12.70 %** (154/1213) |
| TI hears #24001 (5340) | 0.58 % | 0.33 % | 0.25 % |
| TI hears #23002 (Dongle) | 2.25 % | 0.67 % | 4.26 % (1 go-to-search) |
| PM master EVENT_TX / fallbacks | 1064 / 0 | 1064 / 0 | 1064 / 0 |
| Acked: loaded / COMPLETED / FAILED / no terminal | 151 / 139 / 12 / 0 | 152 / 135 / 17 / 0 | 152 / 134 / 18 / 0 |
| Acked messages the TI received (distinct / dups) | 140 / 0 | 136 / 0 | 135 / 0 |
| p0 `xfer_no_ack` | 12 | 17 | 18 |
| p0 `sched_missed` / `slots_missed`, p11 `slots_collided` | 9 / 11 / 9 | 10 / 5 / 10 | 10 / 10 / 10 |
| p1 `mrx_posted` / `mrx_done_ok` / `mrx_done_missed` | 1160 / 1020 / 0 | 1117 / 969 / 0 | 1159 / 1014 / 0 |
| p10 `late_isr_drops` | 0 | 0 | 0 |
| p9 [2] `arm_call_max_us` / [5] `pass_to_arm_max_us` | 73 / 178 | 144 / 235 | 73 / 178 |
| p2 `tx_margin_min_us` (level) | 79 | 115 | 80 |

Full table: `table_tx.txt` (`analyse_tx.py`).

**Readings.**

- **The shadow is exercised and holds.** About 1310 TX arms per B leg are interleaved with about 3900 RX arms
  (`rx_posted`). That gives:
  - `sw_addr_mismatch` 0;
  - `crc_fail` 2–4 per leg against A's 1, all tiny (the load legs saw ~3600);
  - both tracked channels at 0.1–0.7 % loss, A and B alike;
  - an acked-message outcome for every one.

  A stale BASE0/PREFIX0 after a TX would make the next RX arm deaf to its master. Nothing like that shows.
- **Acknowledged data works on both images.** The TI received every acked message the Pro Micro reported
  COMPLETED, plus exactly one more per leg (140/139, 136/135, 135/134). In each leg one TI ACK was lost on
  the return path, and none was passed up twice. The FAILED counts (7.9 %, 11.2 %, 11.8 %) track the TI's
  loss on the same link (8.2 %, 12.3 %, 12.7 %), so the failures are forward-link misses, not ack-window
  misses.
- **Caveat: the literal "in line with the other links" criterion fails on both images.** The TI hears the
  Pro Micro's master at 8–13 % loss. On the same window it hears the 5340 at 0.25–0.6 % and the Dongle at
  0.7–4.3 %. A is no better than B (A 12.3 %, B 8.2 / 12.7 %), so this is not B4. It is the
  Pro Micro → TI link: the Pro Micro's default TX level, antenna and placement, against the TI receiver
  (see ti-findings-2026-11-12-pm). Not investigated further. If it matters, rerun with another receiver (the
  L15) on the Pro Micro master.
- **Timing on B under TX load**: [2] 73 µs vs A 144, and [5] 178 vs 235. Both repeat the load-leg
  saving exactly.
- **`tx_margin_min_us`**: 79/80 on B against 115 on A. This is the smallest `t_start − (earliest +
  phy_lead)` seen when a TX slot is decided (`radiant_sched.c:2241`), so it depends on phase. It stayed
  positive, with `tx_expired` 0. The 40 s `smoke_B` shakedown on the B image read 116, so B is not tied to a
  lower value. It is noted rather than explained.

### Bench notes

- `smoke_B/` is a 40 s shakedown of the driver on B, run after conformance (so its 0xF6 host_csum/desync
  come from conformance's malformed frames). It is not a gate leg.
- txA's 5340 ant_sim exited rc 1 on one wall-clock-fallback message: 1706 sent, 1705 expected. This is the
  known strict pacing check. Every sim in every leg was on air throughout, since the PM and TI tracks
  received ~1200 frames each.
- 0xF6 `tx_drops` 0→2 and `tx_timeout` 0→1 in every leg, on both images. These come from the closing
  channels after the roles exit.
- Final state: Pro Micro on the **B image** (txB2 flash, ba0a7084). TI, 5340, Dongle and L15 were not
  flashed, and the masters were reset after the last leg.
