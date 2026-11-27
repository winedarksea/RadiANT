# Matter outputs off: on-air bench (2026-11-27)

On-target check of branch `wip/matter-outputs-off` (c7aa610) on the nRF54L15 DK.
This branch adds a Kconfig switch per rule output (`CONFIG_RADIANT_BRIDGE_RULES_OUT_*`)
and retires restored Matter endpoints that no longer have a source. All three
steps passed.

## Images

`apps/dongle_thread`, built as `thread.conf;matter.conf`, with
`-DSB_EXTRA_CONF_FILE=matter_sysbuild.conf -DANT_RADIO=core -DRADIANT_BACKEND=nrf`,
plus the bench fragment `PAIRING_WINDOW_S=600`, `RADIANT_LOG_LEVEL_INF=y`:

| Image | Extra fragment | Used for |
|---|---|---|
| `build/bm_def` | none (every output on, the default) | step 1 |
| `build/bm_z3n` | `RADIANT_BRIDGE_RULES_OUT_ZONE3=n` | steps 2–3 |

Both are single-image builds: flash `dongle_thread/zephyr/zephyr.hex`, which has
no `merged.hex`. Both were flashed **without erase** over the 11-26 occupancy
bench state. That state held bindings for #21002 (FE-C) and #21004 (HR), plus
6 endpoints: 3 = trainer In Use; 9 At Rest, 10 Zone 2, 11 Zone 3, 12 Worn and
13 Active for the strap. The two images are the same size (FLASH 849 040 B,
RAM 259 884 B). Turning an output off compiles out no code; it only clears a
bit in the mask.

**Masters.** These are `tools/ant_sim.py` scripted masters, held steady:
FE-C #21002 on the TI LaunchXL (COM11) and HR #21004 on the Pro Micro.

## Results

| Step | Expected | Observed |
|---|---|---|
| 1. Default image over the old state | all 6 endpoints restored and adopted; nothing retired | 6 restored at 00:21:55. Both channels re-pinned at 00:22:00. All 6 adopted by 00:22:02, and Worn/Active went to `1` at 00:22:04. Nothing removed. **PASS** |
| 2. Zone 3 off image | endpoint 11 (Zone 3) retired at boot, never re-created; the rest adopted | 00:44:18: `removed stored endpoint 11 (71a84fa27624802805, type 0x0107): rule output disabled in this build`. 3, 9, 10, 12 and 13 adopted by 00:44:26. Zone 3 (`…05`) was never re-created while the strap was heard. The next boot (00:48:16) restored only 3, 9, 10, 12 and 13. **PASS** |
| 3. Evict the strap before it is heard | its restored endpoints are retired; the trainer's are kept | Strap off air (Pro Micro reset). Reboot at 08:32:15 restored 3, 9, 10, 12 and 13, and only 3 was adopted. A long press (Button 0) at 08:39:13 logged `long press: evicting device 21004`, then `removed stored endpoint 9/10/12/13 … sensor evicted before it was heard`. The next boot (08:39:28) restored only binding 21002 and endpoint 3, which was adopted again. **PASS** |

## Bench notes

- The CHIP boot `<err>` lines (IPV6_PKTINFO, DIS remove/finalize) also appear on
  the 11-26 bench, so they are baseline.
- `Long dispatch time: 124 ms` (step 2) and `234 ms` (step 3) come from the
  endpoint deletes writing to flash on the CHIP thread. The same warning
  (101–280 ms) appears on the 11-22 and 11-26 benches whenever endpoints
  persist.
- Killing a sim host process leaves its master channel open on the stick. Reset
  the stick (`ant_probe.py --serial … -q`) to take the sensor off air.
- Not covered here: Home Assistant's view of the retired entities (owner check).
  MQTT retained discovery for a disabled output is cleared by hand; see the docs.

Logs: `bench-logs/2026-11-27-matter-outputs-off/` (gitignored): `l15.log`,
`l15_step3.log`, `fec_ti*.log`, `hr_pm.log`.
