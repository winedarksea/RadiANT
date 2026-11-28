# M1: HRM dual-radio acceptance, 2026-11-27

> Raw ant_verify logs, per-leg JSONs and the full DUT console stay on the bench machine (`bench-logs/2026-11-27-hrm-m1/`, gitignored; the JSONs copied here are gitignored too). The committed numbers are in this README, `B-ble-central.txt`, `dut-console-events.txt` and `image-config.txt`.

First on-air run of the mixed ANT+/BLE HRM reference design: the shared radiant BLE layer (`wip/mixed-p1`, merged d9409b7) plus the HRM rework (`wip/mixed-p2`, merged a899c1c): own HRS with contact support/status and RR, beat ring, one snapshot feeding both radios, BPM expiry, DIS, strict identity.

## Result

| Step | What | Result |
|---|---|---|
| 1 | Garmin Forerunner 965 records External HR over ANT+ while a Pixel 10 XL (nRF Connect) receives BLE HRS at the same time | **PASS** (owner observed) |
| 2 | The saved Garmin activity contains the HR trace | **PASS** (owner observed) |
| 3 | nRF Connect: DIS strings, appearance 833, HRS with Measurement + Body Sensor Location only (no Control Point), 1 Hz notifications with RR, contact bit toggling with Button 0 | **PASS** (owner observed; console shows `skin contact lost` / `detected`) |
| 4 | Steps 1-2 again after a forced BLE link drop (phone Bluetooth off/on) and after a DK power cycle | **PASS** (owner observed; no console log, the J-Link OB VCOM died mid-run) |
| 5 | Reconnect soak, 50 cycles | **OPEN**: needs `ble_central -Mode reconnect-soak` (Phase 5 tooling) |
| 6 | ANT+ loss vs a same-window control | **PASS**, below |

### Step 6: ANT+ loss, A/B/A, 300 s legs

| leg | BLE | DUT #47345 loss (Garmin `006`) | control #12345 loss (Pro Micro) | DUT excess |
|---|---|---|---|---|
| A1 | advertising, not connected | 1.47 % (18/1219, all RX_FAIL) | 0.16 % (2/1219) | +1.31 |
| B | connected (PC central), 1 Hz HRS notify | 1.14 % (14/1219, all RX_FAIL) | 0.07 % (1/1219) | +1.07 |
| A2 | advertising, not connected | 1.07 % (13/1219, all RX_FAIL) | 0.16 % (2/1219) | +0.91 |

- **Delta:** B's excess is inside the A1..A2 band. Against the A mean (+1.11) it is -0.04 pp, within `[gates.coexistence] max_delta_pp` = 0.5.
- **Absolute:** every leg is at or under `absolute_ceiling_pct` = 1.5. A1 (1.47 %) had no connection. In the 2026-11-22 run on the same DK the DUT excess was +5.3..+6.2 pp.
- **BLE:** 340 notifications in 340 s (1 Hz). RR present, e.g. `16 48 9A 03 35 03` = flags 0x16 (contact supported + detected, RR present), 72 bpm, RR 922 and 821 /1024 s. The 0.8/0.9 s alternation is the simulator's 100 ms beat grid, not the encoder.
- **Re-advertise:** `BLE disconnected (0x13)` → `BLE advertising (restart)` 0.5 ms later.
- **Link:** Windows set 13 ms then 43 ms; the node's preferred parameters moved it to 1000 ms (timeout 4 s).
- **ANT+ HR:** mean 72.2 bpm in every leg (ant_verify's beat-time figure ranges from 66.5 to 75.0 because of the same 100 ms sim grid). Jitter stays on the slot grid (max 500 ms = one missed message).

## Rig

- **DUT:** nRF54L15 DK (J-Link 1057737173), `apps/hrm_ble` build_all `node_dual` row (dual.conf + tests/product_test.conf), built in `C:\wt_mp2` at the code merged as a899c1c, erase-flashed. zephyr.hex sha256 `7d99c4abd1b971f87c830e0ad6e6e2d5ba922d26ae07fdbdaa7bc158297f80d1`. Device #47345 and serial 3053172977 from the identity record; BLE name `RadiANT Test`; console COM3.
- **Control:** TI LaunchXL (COM11 @57600, shipping d94d07e) as `ant_sim --profile heart-rate --bpm 120 --device-number 12345`, received by the Pro Micro (`EE6986854FD72803`, 65261e6).
- **DUT receiver:** Garmin USB-m `006`, `ant_verify --serial 006 --device-number 47345 --expect-bpm 72`.
- **BLE central (leg B):** the Windows host radio, `scripts/ble_central.ps1 -Mode connect -Name "RadiANT Test" -Seconds 340`.
- **Owner devices (steps 1-4):** Garmin Forerunner 965, Pixel 10 XL with nRF Connect. Exact firmware/app versions not recorded.
