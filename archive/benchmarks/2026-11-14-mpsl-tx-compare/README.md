# program_tx() compare order (TODO "Left open by the 2026-11-08 clearance"), 2026-11-14

Change: `program_tx()` now parks CC_TXSTART, clears its event, enables
`conn_start_tx`, writes the compare and decides "missed" by capture-then-event
check (`start_compare_missed(cc, start_cc)`), as `program_rx()` does. The old
order (compare, enable, then `t_start <= now`) could call a live transmit late.
This is the one change of the pass that reaches direct builds.

Rig: nRF54L15 DK (`apps/dongle_thread`, UART COM4) as an `ant_sim.py --profile
power --device-number 3333` master; Garmin USB-m `006` receiving with
`ant_verify.py`, 120 s per leg, run back to back in one sitting. Direct images
carry `apps/dongle/diag_counters.conf` so 0xF7 page 7 fields 6-7 can be read
(`ant_diag.py --port COM4` after the leg).

| leg | image | Garmin loss | page 7 tx/rx_start_missed |
|---|---|---|---|
| h_dir | HEAD dd38b26, direct (md5 7028180F...) | 0.82 % | not measured (HEAD) |
| f_dir | fix, direct (md5 76A261E9...) | -0.01 % | 0 / 0 |
| h_gate | HEAD, `gate.conf` (md5 E0B339F1...) | 0.82 % | (no diag in this image) |
| f_gate | fix, `gate.conf` (md5 89597463...) | 0.20 % | (no diag; the soak's ARMDBG line reads them on MPSL) |

One packet is 0.21 %, so every leg is within a few packets of the others: loss
unchanged, and no transmit compare was found written behind the counter.

Zero cost: plain direct release builds (no fragments) grow by 32 B of flash with
RAM unchanged: L15 `apps/dongle_thread` 87156 -> 87188 B, Pro Micro
`apps/dongle` (promicro_nrf52840/uf2) 94084 -> 94116 B.
