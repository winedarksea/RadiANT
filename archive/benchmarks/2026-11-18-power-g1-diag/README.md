# Gate G1 diagnosis: why the TI receiver loses more against `RADIANT_NRF_LOW_POWER`, 2026-11-18

Follow-up to `archive/benchmarks/2026-11-18-power-g1/` (G1 FAIL on TI receiver loss only). Local master eddc858; no
firmware source changed. The diagnostic images are Kconfig fragments on top of G1's B image (`g1d_*.conf`, built by
`scripts/g1d_build.ps1`).

## Verdict

> **2026-11-19, owner:** G1 is PASS with accepted TI-only excess (the residual ~0.3-0.4 pp TI RX_FAIL; see
> `2026-11-19-power-g1-rejudge/`). This diagnosis is left as written.

1. **The G1 "unaccounted" TI misses are not missed syncs. They are CRC-failed frames that the TI repaired and then
   dropped.** This is a firmware defect in the core, present on every `apps/dongle` and `apps/dongle_ti` image (both set
   `CONFIG_RADIANT_CRC_REPAIR=y`). In each of the 25 arms here, the host's "unaccounted" count equals the TI's 0xF7
   `crc_repaired`, and the host's RX_FAIL count equals `slots_missed`. The G1 B2+A3 counters agree too: 41+7 = 48
   repaired, 48 unaccounted. See "Defect" below.
2. **What B changes is the single-bit CRC error rate at the TI, about 1.7x.** Empty windows (no sync) do not change.
   In round 1 (steady −44 dBm), CRC failures per 300 s were A 31 / 32 / 22, B 53 / 59, B3000 58 / 50, while
   `slots_missed` was 4–7 in every arm. Every TI CRC failure was repairable, so each one is a frame that synced on time
   and carried one wrong bit.
3. **H2 (window centring or drift tracking) is rejected.**
   - A timing error loses the sync. It cannot flip one bit of a frame that was found.
   - The TI's window opens RX_RAMP 150 µs plus a guard of ≥ 100 µs early and closes 500 µs late. Against that, B's
     period moves 7.4 µs.
   - In G1 the misses show no dependence on the master's phase against the TI's 32 kHz grid (`g1d_miss_phase.py`), and
     B's misses are spread evenly over time.
4. **H1 as a start-up transient is rejected.** With the start-up allowance at 3000 µs (TX ~3.3 ms after the HFXO
   request, ~2.6 ms after XOTUNED, more than MPSL's ~1.2 ms), B3000 lost as much as B (D3, D5). At 10000 µs (TX
   ~10.3 ms after the request), B10000 still lost as much as B at matched signal: F2 40 and F4 50 CRC failures, against
   B 44 / 46 and A 26 / 34. The code does wait for the HFXO callback (on XOTUNED) before EDGE 2 and TX:
   `lp_settle()` grants only in `LP_AWAKE && !hfxo_wait`, and `hfxo_late_edge2` = 0 in every arm.
5. **The excess needs the HFXO to stop between slots. CONSTLAT and the B build itself are not the cause.**
   - **Bkeep** is the B build with `LP_KEEP_ON_US=1000000`: it never sleeps, so the HFXO stays on (10/10). It is at the
     A level: E1 31 CRC failures, against A 22–32 around it.
   - **Bcl** is B with CONSTLAT held for the whole uptime (`SOC_NRF_FORCE_CONSTLAT`; CONSTLATSTAT 10/10 on, XO
     0/10). It is at the B level: G1 49 and G4 50, against B 38 / 46 and A 22 / 39.
6. **Not yet split:** a property of each HFXO restart that does not settle within 10 ms, versus the LF-timed slot
   timing. Timing cannot produce single-bit CRC errors, so the restart is the candidate. Two possible forms:
   - a per-start carrier offset, for example the XOTUNE result, or a crystal never self-heated at 1.7 % duty;
   - radio analog state lost while the HF clock is off.

   The Pro Micro (nRF52840) shows none of this: 0.00–0.16 % in every arm, except G5 (1.55 %), when the TI's empty
   windows also jumped.

## Defect: repaired frames are dropped and suppress RX_FAIL

`radiant/src/antr/ant_radio_radiant.c`:

1. `api_sched_rx()` takes a CRC-failed event, calls `api_tracked_frame(ch, evt, true)` and, when that returns true,
   sets `slot_heard = true`. So no RX_FAIL is raised for the slot.
2. `api_tracked_frame()` repairs the frame. It counts `crc_repaired` at line 2417 ("repaired AND delivered"), then
   posts it at line 2466 with `api_post_rx(..., evt, ...)`. That is the original HAL event, whose `status` is still
   `RADIANT_RADIO_STATUS_CRC_FAIL`.
3. `radiant_event_post_rx()` (`radiant/src/radiant_event.c:431`) rejects every CRC_FAIL event (`rejected_crc`,
   `api_stats.rx_dropped++`).

So every repaired frame is lost, and the slot is reported to nobody: no data and no RX_FAIL. That is "unaccounted
loss" in `ant_verify`. The api ztest (`test_a_repaired_acknowledged_frame_is_neither_acked_nor_delivered`) checks
only that `crc_repaired` rises on its broadcast control, never that the broadcast reached the host. That is why it
passes.

**Fix (not applied):** in `api_tracked_frame()`, once a repair has been accepted (after the decode and refutation
checks, beside `api_stats.crc_repaired++`), post a copy of the event with `status = RADIANT_RADIO_STATUS_OK`:

```c
struct radiant_rx_event ok;          /* at the top of api_tracked_frame() */
...
if (crc_failed) {
	api_stats.crc_repaired++;
	ok = *evt;
	ok.status = RADIANT_RADIO_STATUS_OK;
	evt = &ok;                   /* evt becomes a local const pointer */
}
```

`radiant_sec_deliver()` (line ~736) already builds its own OK event the same way. Add an api ztest that asserts the
repaired broadcast reaches the host as MESG_BROADCAST_DATA.

**Effect on G1.** With the fix, the TI would deliver every repaired frame, and its loss would be RX_FAIL alone:
4–7 per 300 s in A and in B alike in round 1, and 5 / 7 / 6 in G1's A arms against 27 / 21 in B. G1's B arms would
still read higher than A, because B's G1 RX_FAIL (27, 21) was itself above A (5, 7, 6). So the fix improves every
receiver but does not by itself clear B. The 1.7x CRC error rate is a real change in what B puts on the air, and it
needs the HFXO-restart investigation below.

## Arms

300 s per arm, Pro Micro and TI listening in the same window (`scripts/g1d_arm.py`). The TI's 0xF7 pages were read over
COM11 before and after each arm, and lp_stats were dumped after each LP arm. Full per-arm table:
`g1d_summary.md`. Columns: TI RX_FAIL / unaccounted (host) | TI diag deltas `slots_missed / term_empty / crc_fail /
sw_addr_mismatch / crc_repaired / crc_unrepairable / sync_entries / rx_posted`.

| Image | What | md5 |
|---|---|---|
| A (`g1_A`) | G1's control, HFXO always on | 1fbebd37 |
| B (`g1_B`) | G1's subject, HFXO released, start-up 800 µs | 26e088bc |
| B3000 | B + `LP_HFXO_STARTUP_US=3000` | 91504224 |
| B10000 | B + `LP_HFXO_STARTUP_US=10000` (Kconfig maximum) | 9a86b721 |
| Bkeep | B + `LP_KEEP_ON_US=1000000` (never sleeps) | 94249dd0 |
| Bcl | B + `SOC_NRF_FORCE_CONSTLAT` + `NRF_SYS_EVENT` (IRQ latency off) | 9dc5c377 |

CRC failures at the TI per 300 s (TI signal), by round:

| Round (signal) | A | B | Variant |
|---|---|---|---|
| D (−44 dBm) | 31, 32, 22 | 53, 59 | B3000: 58, 50 |
| E (drifting −37..−49) | 10 (−40), 34 (−49) | — | Bkeep: 31 (−43), 16 (−45, RX_FAIL 23); B10000: 36 (−43), 9 (−37) |
| F (−48 dBm) | 26, 34 | 44, 46 | B10000: 40, 50 |
| G (−48 dBm) | 22, 39 | 38, 46 | Bcl: 49, 50 |

Round E's signal moved by 11 dB and is not used for verdicts, except that Bkeep's E1 sits next to the A arms of round D
at the same signal.

lp_stats after each LP arm: `late_anchors`, `late_wakes`, `hfxo_late_edge2`, `range_errors`, `hfxo_errors`, `spurious`,
`edge_order` and `refused` are all 0, and sleeps = wakes (Bkeep: 0 sleeps). The since-boot gauges are the known
boot event: `max_disagree_us` 0x26 = 38 µs and `max_wake_slack_us` 0xD1 = 209 µs.

## Why an HFXO request lasts 4.15 ms per slot (not ~1.7 ms)

From the code, for a bidirectional master. Times are relative to the master frame's t_sync:

| Span | µs | Source |
|---|---|---|
| EDGE 1 to EDGE 2: start-up allowance + ISR margin | 900 | `LP_HFXO_LEAD_US` = 800 + `RADIANT_LP_HFXO_ISR_MARGIN_US` 100 |
| EDGE 2 to the TX operation's t_from | 230 | `LP_ARM_LEAD_US` = `ARM_SETUP_US` 80 + `LP_EDGE2_ISR_US` 150 |
| t_from to t_sync: ramp + preamble + address | 88 | `air_lead_us()` = 40 + 6 bytes × 8 |
| t_sync to the master's reply-window close | 2440 | `api_post_master_rx()`: `SLOT_REPLY_US` 2190 + `ACK_GUARD_US` 250 |
| window tail to its terminal | ~50 | address time after t_close (RX 640 µs per slot measured) |
| terminal to the release edge | 300 | `LP_RELEASE_GRACE` (`lp_reconsider()` → `lp_commit_release_locked(LP_RELEASE_GRACE)`) |
| interrupt and core-callback overhead | ~100 | the terminal's scheduler pass, release ISR |
| **Total** | **~4.1 ms** | measured 4147 µs |

The TX-to-reply gap (~1.85 ms) is shorter than the wake lead plus keep-on (~4.35 ms), so the backend stays awake
through it. That is by design (`LP_KEEP_ON_US` help). The simulator's ~1.7 ms is this table without the reply window:
900 + 230 + 88 + 148 (TX) + 300 + ~50. In other words, a one-way master. In `one_way.conf` (no master RX), the request
should fall to ~1.7 ms. The only trim available on the bidirectional path is the 300 µs release grace (~0.12 % HFXO duty).

## Next steps

1. **Fix the repaired-frame drop** (above). It is independent of G1 and hits every receiver image.
2. **Find what an HFXO restart changes on the L15's air.**
   - (a) **Carrier offset** (Kconfig only, TI reflash per rung): a TI `CONFIG_RADIANT_CC26XX_FREQ_TRIM_KHZ` sweep
     (e.g. −100, −50, 0, +50, +100 kHz), each rung with an A and a B arm interleaved. If B's CRC-error minimum sits at
     a different trim than A's, the restarted crystal runs at a different frequency.
   - (b) **Lost radio analog state**: compare the RADIO and other peripheral registers just before a release and just
     after EDGE 2, beyond the public RADIO set that spike B1 compared. This needs a peek of addresses, not symbols.
   - (c) **Crystal warm-up**: a B variant that holds the HFXO for ~100 ms after each wake, every Nth slot. That needs
     source, not Kconfig.
3. Re-run G1's TI criterion after step 1, judged on RX_FAIL and CRC failures separately.

## Board state at the end

- **nRF54L15 DK** (001057737173): image A, `build/g1_A`, 1fbebd37 (the last arm, G6_A, flashed it).
- **TI LaunchXL**: `build/g1_ti_rx` (fa76ef0f), unchanged; read only over COM11; never JTAG-connected.
- **Pro Micro**: unchanged.

## Files

- `g1d_*.conf`: the diagnostic fragments.
- `scripts/g1d_build.ps1`: the builds.
- `scripts/g1d_arm.py` and `g1d_seq*.sh`: the arm protocol and the four rounds.
- `scripts/g1d_summary.py`: the table.
- `scripts/g1d_miss_phase.py`: G1 miss runs, minute histogram and phase against the receiver's 32 kHz grid.
- `g1d_summary.md`: every arm.
- `runs/*.txt`: ant_verify outputs, 0xF7 dumps, lp_stats reports and arm logs. The JSON and `.rec` files stay in
  `build/g1d_runs/`.
