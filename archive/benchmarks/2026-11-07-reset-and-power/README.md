# 2026-11-07: the lost SYSTEM_RESET reply, boot STARTUP, and TX power

This covers TODO.md items 2 and 4 and the boot half of item 3, from the Garmin
comparison (`../2026-11-07-garmin-compare/`). The firmware is this change set
on both boards: the nRF52840 Dongle (3D55F77818BE772A, DFU) and the nRF54L15
DK (1057737173, J-Link).

## Item 2: STARTUP lost after SYSTEM_RESET is a host read race

`tools/ant_reset_loop.py` on the Dongle, idle unless stated otherwise. Full
per-run summaries and every failed row are in `reset_loop_runs.json`.

| run | read timeout | resets | lost STARTUP | Wilson 95 % |
|---|---|---|---|---|
| (a) baseline | 1000 ms | 1000 | 0 | 0-0.38 % |
| (b) timeout at median latency | 52 ms | 1000 | 10 | 0.54-1.83 % |
| (c) the gate's setting | 50 ms | 1000 | 11 | 0.62-1.96 % |
| (c) with a tracked channel | 50 ms | 500 | 0 | 0-0.76 % |
| fix: reopen + reset, the gate's new USB timeout | 1000 ms | 1000 | 0 | 0-0.38 % |

- STARTUP arrives a steady 52.2 ms after the reset write (p10 52.1, p99
  53.1). That is `handle_system_reset()`'s 50 ms sleep plus transport time.
  The first run's 47/63 ms figures were `time.monotonic()`'s 15.6 ms tick on
  Windows; the tool now uses `perf_counter`.
- Every failure looks like the original one: no frame at all for 3 s, and
  zero checksum errors or discarded bytes. The frame is lost whole, which is
  what a libusb-win32 read cancel landing on an arriving transfer does
  (`ant_probe.READ_TIMEOUT_MS`).
- 1.1 % at 50 ms matches the comparison's single miss in about 94 opens.
- With a channel tracking, the race did not reproduce (0/500). At the idle
  rate, zero in 500 has about a 0.4 % chance, so traffic genuinely suppresses
  it. The mechanism was not established. It does not change the verdict:
  `ant_ack_gate.py` resets an endpoint right after opening it, while it is idle.
- Fix: `ant_ack_gate.reader_timeout_ms()` keeps 50 ms only on serial endpoints,
  where pyserial loses nothing on a timeout. USB gets `READ_TIMEOUT_MS`. No
  firmware change: the firmware was never at fault.

## Item 3 (boot half): UART builds announce every boot

The 54L15 was flashed while COM4 (ANT) and COM3 (console, DTR) were held open
(`l15_boot_ant.hex`, `l15_boot_console.log`):

- The ANT port received exactly one frame, `a4 01 6f 01 cb`: an unsolicited
  STARTUP with reason 0x01, hardware reset line. J-Link's `r` is a pin or
  debug reset.
- The console printed `reset cause 0x00000001` (RESET_PIN) on the same boot.
- The STARTUP frame (host 16:43:08.81) followed the console's `transport up`
  (16:43:08.884), so nothing is sent before the UART is ready.

## Item 4: TX power on the 54L15, new rule

Dongle receiving, A/B/A, same sitting (`l15_tx_power.json`):

| what the host did | RSSI at the Dongle |
|---|---|
| never set power (ant_sim), A1 | -21.0 dBm |
| explicit LVL_5 (+8) | -20.96 |
| explicit LVL_4 (+4, boosted) | -20.98 |
| explicit LVL_3 (0) | -27.91 |
| explicit LVL_2 (-4) | -31.95 |
| explicit LVL_1 (-12) | -39.90 |
| explicit LVL_0 (-20) | -48.89 |
| never set power, A2 | -21.0 |

- The unset default equals LVL_5, the part's +8 dBm maximum. Before this
  change it was +4 on the 54L15.
- An explicit LVL_3 is a true 0 dBm. From level 3 down the slope is 1.05 dB
  per dB. LVL_4 is boosted to LVL_5. The top pair reads about 1 dB short of
  the line because RSSI compresses near its -20 dBm ceiling: the boards are
  close.
- The fine ladder (25 raw rungs, +8 to -28 dBm) passes its dial check at 0.99
  dB per dB, worst rung 1.1 dB off the line. The 2026-11-07 run failed this
  check, because its "0 dBm" was +4.

### Not to be read from this data

- **The fine ladder's "knee at +7.7 dBm" is spurious.** Every rung whose loss
  basis was `event_counter` reads exactly 17.5 % with 0 RX_FAIL and the same
  34 packets as the 0 % rungs. The levels ladder shows the same artifact once
  (7.95 % at 0 dBm). It is an ant_sens accounting artifact of short (8-20 s)
  rungs, not loss, and it is still open.
- The RSSI figures are not comparable with the Garmin comparison's: the boards
  are closer and the 54L15 transmits 4 dB hotter by default.
