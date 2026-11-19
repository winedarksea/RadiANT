# Gate G4 (pass B6: HFXO/timebase release on the MPSL gate), hrm_ble on the nRF54L15 DK, 2026-11-19

> Raw per-leg output in `runs/` stays on the bench machine (gitignored; see
> `docs/preservation.md`). The committed numbers are in this README,
> `g4_summary.json` and `g4_extremes_summary.tsv`.

**Verdict: every G4 criterion holds except the anchor band as literally written,
which is an owner call.**
- Held for the hour:
  - a real BLE central stayed connected for 1 h 0 min 36 s;
  - ANT+ ran the whole time;
  - 0 MPSL or SDC asserts, 0 resets;
  - the duty proxy is 1.21 %, and no LP fault counter moved.
- Anchor criterion ("anchor error within today's 0-17 us"):
  - The START-vs-anchor cross-check (`start_err`) sits at a steady **-4 us**
    (-3..-5 in 100+ reads). That is a constant offset 4 us outside the band on
    the negative side, with a spread of about 2 us, much tighter than the
    band's 17 us.
  - The since-boot extremes are **-33 / +31 us**. They are one-off
    transients: the +31/+33 comes from the first 6 STARTs after boot. The
    negative extreme grows at BLE connection setup (-5 to -8 on a reboot,
    -33 in the hour's boot+connect). Neither moved in the 14,566 STARTs of
    the hour itself.

## Image

- `C:\wt_mpsl_arm\build\g4_lp_pd_ns\hrm_ble\zephyr\zephyr.hex`, **md5
  9e6d6eb7bcc3b4e08aeba9dce04258ee**. Built in `C:\wt_mpsl_arm` at master
  **47bdf80**, which contains 88134ca, the MPSL arm-guard fix. It is not the
  old c74d7c6 image.
- `scripts/g4_build.ps1` is a hand west build (one image only):
  `apps/hrm_ble`, `-DRADIANT_BACKEND=nrf`,
  `-Dhrm_ble_EXTRA_CONF_FILE=ble.conf;ble_nosuppress.conf;lowpower.conf;power_diag.conf`.
- It is build_all row `node_ble_lp_pd` **plus `ble_nosuppress.conf`**. The
  row's default `HRM_SUPPRESS_ANT_WHEN_CONNECTED=y` closes ANT+ while a central
  is connected, and G4 needs BLE connected *and* ANT. The `.config` was checked:
  - `RADIANT_NRF_LOW_POWER=y`, `RADIANT_NRF_LP_STATS=y`, `RADIANT_POWER_DIAG=y`;
  - `BACKEND_NRF_GATE_MPSL=y` (not NULL), `HRM_BLE_HRS=y`, suppression not set.

## Rig

- DUT: nRF54L15 DK (J-Link 001057737173). Console on COM3 (DTR), captured for
  the whole hour.
- BLE central: the Windows host radio, via
  `scripts/ble_central.ps1 -Mode connect -Name "RadiANT HR"`. It holds the link
  subscribed to HRS. No person is needed.
  - The link settled at a 1000 ms interval, latency 0, timeout 4 s. The node
    rewrote the 13 ms and then 43 ms intervals the central asked for.
- ANT receiver: the TI LaunchXL `crcfix_ti_rx` (md5 a3862fbf), COM11 @57600,
  `ant_verify --profile heart-rate --device-number 4242`, in 6 x 600 s
  segments.
- Pro Micro: still `hrtx_promicro_lp_pd` transmitting HR #12345, an
  independent device number. The Garmin was not used.
- J-Link use was only `flash_sim_jlink.ps1` and `jlink_peek.ps1`
  (`radiant_nrf_lp_stats` between segments; `radiant_power_diag` plus 10
  register samples at the start and the end).

## Results (1 h; `g4_summary.json`)

| criterion | result |
|---|---|
| BLE connected the whole time | 3636 s, 0 disconnects (console and central), **4361 HRS notifications** = 72.0/min, the simulated 72 bpm |
| ANT+ alongside | TI received 14381 / 14620 = **1.63 % loss** (segments 1.40 to 1.93 %, all RX_FAIL, on the air) |
| ANT+ control, same image, BLE advertising but unconnected (10 min just after) | 2.63 % (64 / 2436). The connection did not raise loss; the room drifts (Task 1 in the same sitting showed 4.6 to 5.6 %) |
| zero MPSL asserts | **0**: no ASSERT, fault, reboot or `<err>` line in 3636 s of console; console port never died |
| anchor error, START vs requested start (`start_err`) | steady **-4 us** (7/7 hour reads; -3..-5 over 112 further reads); since-boot min/max **-33 / +31**, unchanged through the hour |
| other timebase checks | release disagreements 0, max_disagree 1 us, missed_releases 0, range_errors 0, refused 0, hfxo_errors 0 |
| duty proxy: radiant's timeslot awake time (`slot_awake_us`) | 43.73 s over 3623.8 s = **1.207 %**, 3.00 ms per timeslot, 14566 STARTs (4.02/s against the HR 4.06/s) |
| app/stack HFXO request after boot | 0 (power-diag `hfxo_pm=0` on every 10 s line; B6 drops the own request) |
| HFXO sampled running (XO.STAT) | 0/20 register samples (Wilson 95 % upper about 16 %) |
| CONSTLAT | 0/20 samples; power-diag `constlat=0/0` |

ESTIMATE (`tools/ant_power_est.py --board nrf54l15dk --hfxo-duty 0.0121`, no
current meter):
- About **38 uA** for the parts radiant's counters see: CPU 21.4, RX 8.8,
  TX 2.7, HFXO 2.1, floor 2.9.
- It does **not** include the SoftDevice Controller's own radio events or
  MPSL's HFXO ramp ahead of each START, which no counter here sees.
- The G1 direct-build baseline was about 811 uA with HFXO always on.

## Where the start_err extremes come from (`g4_extremes_summary.tsv`)

`scripts/g4_extremes.py`:
1. Twelve steady reads at about 1 h 05 min uptime: last = -4 every time.
2. Reflash (reset), then read every ~2 s for 100 s with no central
   (advertising only):
   - The max is **+33 already at the first read (6 STARTs)**. The min is -5.
   - `last` reads -4 (once -5, once -3).
3. The host central was started again. It found the node, but GATT returned
   "Unreachable", so no full connection was made.
   - During the attempt the min stepped -5 to -8 (around 120 s).
   - Nothing else moved in 90 s.

So the bootstrap STARTs produce the positive extreme, and connection setup
produces the negative one. In the hour run the connection setup also included
two parameter updates (13 ms, 43 ms, then 1000 ms), which fits the -33. The
steady offset of -4 us is negative: the START's GRTC reading is earlier than
the requested start. The gate spike's "0..17 us" was a positive dispatch
latency, so a reader should decide whether G4 means the spread (met: about
2 us) or the absolute band (missed by 4 us, plus the transients).

## Findings outside the gate

1. **hrm_ble BLE builds lose the flash-sync timeslot session.**
   - The boot log says `flash_sync_mpsl: mpsl_timeslot_session_open failed:
     -12`, then `hrm: identity boot failed: identity storage failed (-4);
     falling back to Kconfig device number 4242, no transforms`.
   - `apps/hrm_ble/Kconfig` sets `MPSL_TIMESLOT_SESSION_COUNT` `default 1 if
     HRM_BLE_HRS`.
   - `RADIANT_GATE_MPSL_D2` (default y on the gate) opens a second session.
     Its help says it "needs CONFIG_MPSL_TIMESLOT_SESSION_COUNT one higher".
     So the gate takes both sessions and flash sync gets none.
   - This is not LP-specific: it applies to every hrm_ble row with BLE. It
     was not fixed here.
2. **ble_central trap.** The first attempt found the node advertising from
   the boot before a reflash, then failed with "no Heart Rate service:
   Unreachable" as the node reset (`runs/falsestart/`). Start the central
   only after the node has booted. The runner now retries up to 3 times; one
   attempt was enough.

## Commands

```
# build (one image, nothing else building)
powershell -File scripts/g4_build.ps1
# flash
scripts\flash_sim_jlink.ps1 -HexPath C:\wt_mpsl_arm\build\g4_lp_pd_ns\hrm_ble\zephyr\zephyr.hex -Serial 001057737173
# the hour
python scripts/g4_run.py runs/hour C:\wt_mpsl_arm\build\g4_lp_pd_ns\hrm_ble\zephyr --segs 6 --seg-s 600
# same-image control, BLE unconnected
python scripts/g4_ctrl.py runs/ctrl C:\wt_mpsl_arm\build\g4_lp_pd_ns\hrm_ble\zephyr 600
# start_err extremes
python scripts/g4_extremes.py runs/extremes C:\wt_mpsl_arm\build\g4_lp_pd_ns\hrm_ble\zephyr --steady 12 --reboot
# decode
python ..\2026-11-18-power-g1\scripts\g1_lpdecode.py runs\hour\S0_lp.json ... runs\hour\S6_lp.json
python tools\ant_power_est.py --board nrf54l15dk --hfxo-duty 0.0121 runs\hour\S0_pd.json runs\hour\Send_pd.json
```

Python is `C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe`.

The Task 1 on-air check of the same fix on the default (option-off) MPSL row is
in `../2026-11-19-mpsl-arm-onair/`.
