<!-- SPDX-License-Identifier: Apache-2.0 -->
# Master acked data: the misses follow the slave's power, 2026-11-12

Checked by: nothing automated for the on-air part. The transfer-engine power
fix it starts from is covered by two ztests in `radiant/tests/api`
(`test_a_power_change_after_open_reaches_*`, both failing without the fix).

This closes the open item in `../2026-11-12-burst-stale-ack/README.md`,
section 2: a RadiANT master sending acknowledged data (`0xAA`) was told FAILED
10-38 % of the time while the slave received about 99 %.

## Result

There is no receiver fault on the slot turnaround. The master misses the
Garmin's acknowledgement when the Garmin transmits at -20 dBm, and misses none
when it transmits at 0 dBm. The asymmetry that made it look like a master-only
defect was a second bug: the master's acknowledged data was ~25 dB hotter
than the power the host had set.

| Leg | Master power | Garmin power (on air) | Garmin `F2` at sniffer | Slave got | Master heard the ack |
|---|---|---|---|---|---|
| C  | level 4 (+8 dBm) | -20 dBm | -76 dBm | 99 | 89 (**10 missed**) |
| D  | level 4 (+8 dBm) | 0 dBm   | -57 dBm | 98 | 98 (**0 missed**) |
| C' | level 4 (+8 dBm) | -20 dBm | -76 dBm | 95 | 83 (**12 missed**) |
| D' | level 4 (+8 dBm) | 0 dBm   | -57 dBm | 100 | 100 (**0 missed**) |

"Master heard the ack" counts messages the slave passed up and the master was
told COMPLETED. D's two FAILED messages are the two the slave never received.
22 misses in 194 against 0 in 198, in alternating legs, so room drift does not
explain it. Leg C and C' are the original failing condition: master hot (it was,
through the bug), Garmin at the weakest level.

## 1. The transfer engine ignored power set after open (fixed)

`api_xfer_setup()` copied the channel's power into the transfer engine at
open, and `0x60` on an open channel only changed what broadcasts read.
`ant_ack_pair.py` sets the power after the channel opens, so every earlier
`--burst-tx-power` run sent broadcasts at that level and acknowledged data,
burst packets and acknowledgements at the open power (level 4 boosted to the
part's ceiling). Now `antr_channel_radio_tx_power_set()` also calls
`radiant_transfer_set_power()`.

On air, sniffer RSSI of the master's packets in the same run:

| Run | `0A` | `AA` / burst packets |
|---|---|---|
| before (`../2026-11-12-burst-stale-ack/acked_promicro_run2_sniff.log.gz`), level 0 | -64..-70 | -34..-42 |
| before (`bursts300_sniff.log.gz`), -40 dBm | -81 | -33 |
| after, leg A (level 0) | median -68 | median -68 |
| after, leg C (level 4) | -42 | -42 |
| after, bursts (level 4) | -46 | -46 (`82/8A/92/B2`) |

## 2. The Garmin's 0x60 level 4 is its weakest level

The plan's deciding leg was "Garmin at level 4". It ran (leg B) but the
Garmin's `F2` stayed at -76 dBm. A direct test with the Garmin as a master on
#14871 (`garmin_power_sniff.log.gz`), sniffer RSSI of its broadcasts:

| Phase | RSSI |
|---|---|
| after reset (default) | -58 |
| `0x60` level 0 while open | -76 |
| `0x60` level 4 while open | -76 |
| level 0 set before open | -74 |
| `0x60` level 4 again | -76 |
| `0x47` global level 0 | -76 |

It answers `RESPONSE_NO_ERROR` to all of them and advertises per-channel power
(capabilities `08 08 00 ba 36 00 df`). Level 3 (D, D') gave -57, the default.
So use level 3 for a Garmin at 0 dBm, never level 4. The JSONs of A, B and C
record `slave_tx_power` 0, 4 and 4; on air all three were the Garmin's -20 dBm.

## 3. At level 0 the link is weak in both directions

| Leg | Master | Garmin (on air) | Slave got | Master heard of those | Slave broadcasts / rx_fail |
|---|---|---|---|---|---|
| A  | level 0 | -20 dBm | 1  | 1 of 1   | (not counted yet) |
| B  | level 0 | -20 dBm (asked 4) | 43 | 41 of 43 | 72 / 190 |
| A' | level 0 | -20 dBm | 48 | 39 of 48 | 85 / 174 |

With the fix, a level-0 master really is at -20 dBm, and the Garmin about
30 cm away loses about half of its packets. The 2026-11-11 claim that "at
level 0 the two sticks lost nothing" measured packets that were not at level 0
(see the correction in `../2026-11-11-burst-retry-ab/README.md`).

## 4. Bursts, both directions strong: no regression

100 bursts of 8, master level 4, Garmin level 3: 100/100 completed, 800/800
packets, no gaps. One repeat each of `8A`, `82` and `B2` in the sniffer.

## 5. New: the master skips the slot after each acknowledged message

In every leg the next master frame after an `AA` comes two periods later, not
one. D': 100 of 100 `AA` followed by a gap of 2 periods, every `0A -> 0A` and
`0A -> AA` gap is 1 period. The Garmin's ~100 `rx_fail` per leg of 100
messages is that missing slot. The api suite's fake radio does not reproduce
it (a probe armed the next broadcast at +249 695 µs after a completed `AA`).
Fixed the same day; see `../2026-11-12-master-slot-skip-fix/README.md`. The
probe missed it because it never let the next slot be posted before the `AA`.

## Rig

- Master: Pro Micro `EE6986854FD72803`, `apps/dongle`, core backend, non-MPSL,
  `scripts/build_all.ps1 -Only radiant_dongle_promicro.uf2` on the working tree
  with the power fix (UF2 md5 `2e9b9946747e...`), flashed over USB with
  `ant_probe.py --reboot-to-bootloader`.
- Slave: Garmin ANT USB-m `006`, about 30 cm away.
- Sniffer: `radiant/spike/promisc` on the nRF54L15 DK (`build/promisc_l15`),
  console COM3 with DTR, captured by `ant_ack_pair.py --log-port`.
- Command (per leg, powers from the table):

      python tools/ant_ack_pair.py --master-serial EE6986854FD72803 \
          --slave-serial 006 --master-acked 100 --burst-tx-power 4 \
          --burst-slave-tx-power 3 --log-port COM3 \
          --log-file D_sniff.log --json-bursts D.json

- Order: A, B, A', C, (Garmin power test), D, C', D', bursts.
- The console transcripts of each leg were not kept (PowerShell wrote them as
  UTF-16 and a conversion garbled them). Every figure above is in the leg's
  `.json` or the sniffer log.

Step 3 of the plan (RSSI at the master under `CONFIG_RADIANT_NRF_RX_DIAG`) was
not run: C/D/C'/D' already isolate the variable. Whether RadiANT is a less
sensitive receiver than the Garmin at equal input is a separate question,
open in TODO.md.
