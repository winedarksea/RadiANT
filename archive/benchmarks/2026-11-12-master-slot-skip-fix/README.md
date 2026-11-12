<!-- SPDX-License-Identifier: Apache-2.0 -->
# Master slot skip after acknowledged data: fixed, 2026-11-12

Checked by: `radiant/tests/api` (`test_a_master_keeps_its_grid_after_acknowledged_data`
fails without the fix: next broadcast at +499 390 µs, `slots_missed` +1). The
on-air result below is from one sitting and is not automated.

This closes section 5 of `../2026-11-12-master-acked-power/README.md`: after
every `0xAA` a RadiANT master sent, its next frame came two periods later.

## Result

| Leg (same rig and powers as D') | `AA -> next 0A` gap | Garmin `rx_fail` | Broadcasts the Garmin heard |
|---|---|---|---|
| D' before (`../2026-11-12-master-acked-power/D2.json`) | 2 periods, 100/100 | 100 | 109 |
| D after (`D.json`) | **1 period, 100/100** | **6** | 207 |

Every other slot gap in both captures is one period. The 6 `rx_fail` after
the fix are ordinary loss: in the same run the Garmin missed 6 of 213
broadcast slots and 4 of 100 `AA`s.

Outcomes in leg D: 91 COMPLETED, 9 FAILED (D' had 100/0). Every FAILED message
is an `AA` that the sniffer also saw no `F2` reply to (messages 1, 28, 38, 48,
62, ...). So the master's outcome matches the air, and the change is room loss
today. It is not a receiver regression: the fix touches neither the transfer
engine nor the ack window. RSSI matched the earlier run (`AA` −39 dBm,
`F2` median −59 dBm, against −42/−57 then).

## Bursts never skipped

100 bursts of 8 (`bursts.json`): `8A -> next 0A` is 1 period, 100/100, both in
this run and in `../2026-11-12-master-acked-power/bursts_sniff.log.gz`. 97
completed, 3 failed. All 19 repeated `8A`s and the 3 failures follow an `8A`
with no `D2` on air, so the Garmin did not answer and the master retried
correctly. The fake radio agrees: the burst test passes with and without the
fix.

## Cause

The api layer binds each channel's scheduler request to a kind
(`api_ch[ch].slot_kind`) so a `done()` can be attributed. On hardware the
host's broadcast write (on `EVENT_TX`) or the 50 ms pump has already posted
the next `MASTER_TX` when the `0x4F` arrives. The transfer's submit replaces
that request in the scheduler, which drops an unstarted request without a
`done()`. The binding still read "unheard `MASTER_TX`".

When the slave acknowledged, the transfer engine finished inside the RX
callback. When the operation's `done()` then arrived, the engine was idle,
so `api_sched_done()` skipped the engine branch and took the stale
missed-slot branch: `slots_missed++` and `radiant_channel_on_slot_missed()`,
a second clock advance on top of the `0xAA`'s own `on_slot()`.

When nothing answered, the engine was still busy at that `done()` and
absorbed it, so the fake radio did not skip in that case. On hardware the
skip appeared in both cases. The fix covers both, because the binding no longer
claims the transfer's operation.

The earlier fake-radio probe missed it because no test let the next slot be
posted before the `0xAA` write. The new tests call `host_writes_broadcast()`
first and assert that it posted a slot.

Fix: `api_xfer_superseded()` in `radiant/src/antr/ant_radio_radiant.c`. It
clears the binding's kind when a host transfer starts (acknowledged data, or a
burst's START block on an idle engine) and keeps `op`, so a close during the
transfer completes as before. A secured slave's `0xAA` replaces its `TRACK_RX`
window the same way, and would have counted a false miss and raised
`RX_FAIL`. It is covered by the same change but has not been run on air.

## Rig

- Master: Pro Micro `EE6986854FD72803`, `apps/dongle`, core backend, non-MPSL,
  `scripts/build_all.ps1 -Only radiant_dongle_promicro.uf2` at commit 5cba7bf,
  clean tree (UF2 md5 `15debe5c9188...`), flashed with
  `ant_probe.py --reboot-to-bootloader`.
- Slave: Garmin ANT USB-m `006`, about 30 cm away, level 3 (0 dBm).
- Sniffer: `radiant/spike/promisc` (`build/promisc_l15/merged.hex`) on the
  nRF54L15 DK, console COM3 with DTR. The L15 was restored to
  `radiant_dongle_nrf54l15dk.hex` afterwards.
- Commands:

      python tools/ant_ack_pair.py --master-serial EE6986854FD72803 \
          --slave-serial 006 --master-acked 100 --burst-tx-power 4 \
          --burst-slave-tx-power 3 --log-port COM3 \
          --log-file D_sniff.log --json-bursts D.json
      python tools/ant_ack_pair.py --master-serial EE6986854FD72803 \
          --slave-serial 006 --master-bursts 100 --burst-packets 8 \
          --burst-tx-power 4 --burst-slave-tx-power 3 --log-port COM3 \
          --log-file bursts_sniff.log --json-bursts bursts.json

- Gaps were counted by rounding each slot-frame-to-slot-frame interval (`0A`,
  `AA`, `8A`) to a multiple of 250 000 sniffer ticks, which is the measured
  `0A -> 0A` spacing.
- An earlier attempt at leg D had no sniffer running (the L15 was not on
  promisc) and was discarded: 85/15, Garmin `rx_fail` 35.
