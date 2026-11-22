# HR node: ANT+ and BLE together on air, 2026-11-22

> Raw ant_verify logs and the full DUT console stay on the bench machine (`bench-logs/2026-11-22-hrm-dual/`, gitignored). The committed numbers are in this README, the per-leg JSONs, `B-ble-central.txt` and `dut-console-events.txt`.

Since d530eac the default `ble.conf` build of `apps/hrm_ble` keeps ANT+
transmitting through a live BLE connection (`HRM_SUPPRESS_ANT_WHEN_CONNECTED`
default `n`), with the MPSL gate arbitrating ANT+ against the SoftDevice. This
is its first on-air run.

## Result

**PASS.** The connected leg's ANT+ loss sits inside the band set by the two
disconnected legs, measured against a same-window control; the BLE side
delivered exactly 72 notifications per minute; the DUT console shows no
assert, reboot or unexpected disconnect.

| leg | BLE | DUT #48838 loss (Garmin `006`) | control #12345 loss (TI) | DUT excess |
|---|---|---|---|---|
| A1b | advertising, not connected | 5.78 % (141/2437, all RX_FAIL) | 0.48 % (12/2437) | +5.30 |
| B | connected, 1 s interval, HRS notifying | 6.77 % (165/2437, all RX_FAIL) | 1.14 % (28/2436) | +5.63 |
| A2 | advertising, not connected | 7.55 % (184/2438, all RX_FAIL) | 1.35 % (33/2437) | +6.20 |

Each leg is 600 s. HR has no exact-loss source, so these are ant_verify's
packet-count loss figures; timing is on the slot grid on every leg (DUT
jitter max 0.98-1.22 s, i.e. a few consecutive misses, no dropout).

- **Beat count.** `ble_central.ps1 -Mode connect` held the link 720 s: 864
  notifications, **72 in each of the ten full 60 s windows** inside leg B.
- **Connection interval.** Windows, as central, set 13 ms and then 43 ms
  itself (central-initiated updates; the peripheral's `le_param_req` is not
  consulted and nothing was refused). About 3 s after connecting, the node's
  own preferred-parameters request moved the link to **1000 ms**, timeout
  4 s, where it stayed for the whole leg. A phone that *requests* a shorter
  interval goes through `le_param_req` and would be rewritten and logged;
  that path was not exercised by this host.
- **Console.** `BLE connected` at 10:51:42, `BLE disconnected (0x13)` (remote
  user terminated, the central's own close) at 11:03:49, advertising again.
  No other events. The console timestamp wraps at 2^32 us once (10:30); that
  is not a reboot.

## What the DUT excess is and is not

The DUT loses ~5-6 points more than the control **in every leg, connected or
not**, all of it RX_FAIL. So it is not caused by the connection. The two
transmitters are heard by different receivers (Garmin vs TI), so the excess
mixes the DUT's own refused transmits (the SoftDevice's advertising events
outrank an application timeslot) with the Garmin-to-DUT link in this room
(5-8 % floor, see the bench notes). A second receiver on the DUT was planned
(the nRF5340 DK net core) but did not answer (`no startup message after
reset`), so the two were not separated. Not investigated further.

## Rig

- DUT: nRF54L15 DK (J-Link 1057737173), `apps/hrm_ble` + `ble.conf`, the
  `build_all.ps1` `node_ble` row from e7fa93c (hrm_ble unchanged from master
  c58ca20), sha256 `b3c2bf279a547bf42317240dfaa50b111a5dcd341fdde75497d73d261ca1b2c7`.
  Device #48838 (identity record), simulated 72 bpm, console on COM3.
  `.config` lines in `image-config.txt`: suppression not set, MPSL gate,
  `MPSL_TIMESLOT_SESSION_COUNT=2`, adv 1000 ms, min interval 800 units.
- Control: Pro Micro hrtx, HR #12345 (120 bpm), received by the TI LaunchXL
  (COM11 @57600) with `ant_verify --port COM11`.
- DUT receiver: Garmin USB-m `006`, `ant_verify --serial 006 --expect-bpm 72`.
- BLE central: the Windows host radio, `scripts/ble_central.ps1 -Mode connect`.

## A1, discarded

The first A1 had the Garmin track both HR channels. Both run at 8070 counts,
so as the two transmitters drift through each other the Garmin collides with
itself (259 `CHANNEL_COLLISION`, 2 `GO_TO_SEARCH`, 11-15 % loss on both
channels). That measures the receiver, not the DUT; its JSON is kept as
`A1-shared-receiver.json`. All three legs above use one receiver per
transmitter.
