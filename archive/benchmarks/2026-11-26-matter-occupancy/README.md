# Matter occupancy follow-ups: on-air bench (2026-11-26)

On-target check of the occupancy reviewer follow-ups (merge 661e9c2) on the
nRF54L15 DK. It found one defect, fixed in ef6caf8 (liveness STALE after 8
periods, not 3), and every step passed on the fixed image.

## Images

`apps/dongle_thread`, built as `thread.conf;matter.conf`, with
`-DSB_EXTRA_CONF_FILE=matter_sysbuild.conf -DANT_RADIO=core -DRADIANT_BACKEND=nrf`
from master aa1001a, plus a bench fragment:

| Image | Bench fragment | Used for |
|---|---|---|
| `l15_window600_aa1001a.hex` | `PAIRING_WINDOW_AT_BOOT=y`, `PAIRING_WINDOW_S=600`, `RADIANT_LOG_LEVEL_INF=y` | steps 1–3 (flashed with `-Erase`) |
| `l15_nowindow_aa1001a.hex` | `PAIRING_WINDOW_S=600`, `RADIANT_LOG_LEVEL_INF=y` | steps 4–5 (flashed without erase) |

Copies and `.config`s are in `build/occ_images/` (gitignored). RAM is 259 812 B
(99.1 %) with INF logging. The shipping-config numbers are in the merge notes.

**Masters.** These are scripted HR and FE-C masters on top of `tools/ant_sim.py`.
The HR sim holds 100 bpm, rises to 155 bpm, then drops back. The FE-C
(treadmill rotation 16,16,19,19,17) runs READY, then IN_USE, then READY.

| Sensor | Device # | Master |
|---|---|---|
| FE-C trainer | 21002 | TI LaunchXL (COM11) |
| strap A | 21001 | Pro Micro |
| strap B | 21004 | Pro Micro, after strap A had ended |

The masters were paced 2401/2401 by EVENT_TX. The window was 600 s, not 120 s.
The self channel rotates HR, power, FE-C, env 4 Hz, env 0.5 Hz at 25 s a row, so
once the trainer has bound, the free channel does not come back to HR inside
120 s. The trainer therefore pairs first and the strap second, which makes the
strap the newest binding: the one a long press evicts.

## Defect found: liveness STALE every ~5 s from a healthy treadmill

This happened on the 661e9c2 image. From the moment the trainer bound,
`liveness: 3 field(s) went STALE` appeared about every 5 s. Meanwhile `rxpath`
showed every frame of a 4 Hz master being heard (+120 per 30 s). Each STALE
forced In Use false, so In Use flapped 0/1 while the machine ran. When the
machine stopped (17:47:23) it cleared at 17:47:25, so a STALE defeated the 30 s
clear delay.

**Cause.** Liveness hears samples, not frames.
`radiant_power_adapter_decode_fec()` decodes FE-C pages 16 and 25 only. On a
treadmill rotation, pages 19 and 17 post nothing, so the source is quiet for 4
periods (1.0 s) on every rotation. That is past the old 3-period (0.75 s)
expiry. A bike trainer (pages 16 and 25) escapes this, which is why the earlier
Wahoo bench was clean.

**Fix (ef6caf8).** STALE now comes after 8 periods, the count after which the
ANT channel itself raises `RX_FAIL_GO_TO_SEARCH`. MQTT `expire_after` follows.
A new ztest covers 60 s of the treadmill rotation. Twister: 30/30 configurations,
10 067 cases, 0 failed. On air afterwards there were **zero** STALEs from
healthy sensors. Strap A going off air was reported about 5 s after its last
frame (18:39:40 → 18:39:45).

## Results (fixed image)

| Step | Expected | Observed |
|---|---|---|
| 1. Pair HR + FE-C | every occupancy endpoint appears at once, showing `false` | trainer In Use `0` at bind (18:12:11). Strap: At Rest, Zone 2, Zone 3, Worn and Active created and posted `0` at bind (18:14:45). Worn/Active went to `1` 4 s later. **PASS** |
| 2. HR ≥ 146 | Zone 2 and Zone 3 assert | 155 bpm from 18:15:40: Zone 2 + Zone 3 `1` at 18:15:46, both `0` at 18:17:46 after the drop. At Rest stayed `0`. **PASS** (both asserted together because the sim steps straight from 100 to 155 bpm) |
| 3. Trainer stops | In Use holds 30 s | IN_USE ended 18:15:15. In Use `0` at **18:15:48** (33 s). No flapping while in use. **PASS** |
| 4. Reboot, no window, no press | both re-pin; endpoints become reachable | at 18:34:01, 2 bindings and 6 endpoints were restored ("unreachable until heard"). Both channels re-pinned at 18:34:06, and all 6 were adopted with values posted at 18:34:07–08. Same endpoint ids and uuids as before. **PASS** |
| 5. Long-press evict strap A, pair strap B | B's first occupancy is reported | one long press (Button 0). B (#21004) bound into strap A's source 1 at 19:22:55, giving new endpoints 9–13 posted `0` at 19:22:58 and Worn/Active `1` at 19:23:02. A further reboot restored only 21002 and 21004 and endpoints 3 and 9–13: strap A's binding record and Matter endpoints were deleted by the eviction. **PASS** |

## Bench notes

- The press came after the console logger and the sims had stopped, so the
  eviction line itself is not in a log. That the eviction happened is shown by
  three things: the freed channel's wildcard discovery search, B pairing into
  source 1, and the post-reboot restore.
- Strap B was first run as #21003 from the Garmin USB-m. It did **not** pair
  during a 26 s HR search slot with proximity bin 3 (`PAIRING_PROX_BIN=3`). The
  same strap from the Pro Micro paired at once. Proximity search is doing its
  job: the Garmin is further away and/or quieter. Put sims meant to pair on the
  Pro Micro or the TI.
- A message at boot, with nothing paired yet, says "pairing window open, but
  every self channel is tracking a paired sensor - nothing to evict". Pairing
  still worked. It looks like the eviction check runs before the self channels
  have opened. It is cosmetic and was not investigated.
- Not done: Home Assistant commissioning (this bench has no controller).

Raw consoles and sim output are in `bench-logs/2026-11-26-matter-occupancy/`
(gitignored).
