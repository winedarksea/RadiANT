<!-- SPDX-License-Identifier: Apache-2.0 -->
# Stale-acknowledgement retries on air, and master acked data: 2026-11-12

Checked by: nothing automated. This is the bench evidence for branch
`fix/burst-retry-followups`: the stale-acknowledgement retry (9d5a25a), the
burst-turnaround window for a repeated slot opener (bbc2c90), and the first
on-air run of acknowledged data sent by a RadiANT master
(`ant_ack_pair.py --master-acked`, 4dadce4).

## 1. Bursts: 300 of 300, five stale acknowledgements retried

| Leg | Image | Bursts failed | Slave got whole |
|---|---|---|---|
| `bursts300` | 4dadce4 (UF2 md5 `7982226e98c0…`) | **0 / 300** | 300, 2400 packets, no gaps |

Same rig and command as `../2026-11-11-burst-retry-ab/` (Pro Micro
`EE6986854FD72803` master, Garmin USB-m `006` slave, 8-packet bursts, both at
power level 0 and then the master alone at -40 dBm, promisc on the nRF54L15
DK), with `--master-bursts 300`.

The sniffer log (`bursts300_sniff.log.gz`) shows five stale acknowledgements,
where the Garmin answered with the sequence bit unchanged because it missed
the packet. Each is followed one packet time later by the same packet with the
same payload, and each of those bursts completed:

    82 -> C2 (+1555) -> 82 (+1554)      92 -> D2 (+1584) -> 92 (+1525)
    82 -> C2 (+1547) -> 82 (+1562)      82 -> C2 (+1539) -> 82 (+1570)
    B2 -> D2 (+1546) -> B2 (+1563)

The 2026-11-11 image ended each of these as `FAIL_BAD_ACK`. Five in 300 is
1.7 %, against A2's 2 in 100 there. The repeated `8A` openers that were heard
were acknowledged at +1552 and +1564 µs, inside the narrower window the repeat
now has. No old-image control leg was run: the mechanism is visible frame by
frame, and at about 2 % a rate comparison of 300 bursts would not resolve it.

## 2. Acknowledged data from the master: a separate defect, not fixed

**Resolved the same day, `../2026-11-12-master-acked-power/`.** The misses
follow the Garmin's transmit power (10 and 12 missed at -20 dBm, 0 and 0 at
0 dBm, alternating legs), so it is the return link's budget, not the
receiver. The slave's ~99 % below was not a like-for-like comparison: the
master's `AA` went out at the open power, not at level 0, because the transfer
engine ignored power set after open (fixed). The text below is kept as
recorded.

`--master-acked` sends one `0x4F` at a time from the master. On air that is
`0xAA`, a one-packet transfer that opens the slot, answered on the slave's
slot-reply turnaround and never retried.

| File | Master | Power | Master told FAILED | Slave received |
|---|---|---|---|---|
| `acked_promicro_run1.json` | Pro Micro | level 0 | 10 / 100 | 99 |
| `acked_promicro_run2.json` | Pro Micro | level 0 | 35 / 100 | 98 |
| `acked_l15.json` | nRF54L15 DK (HEAD) | level 0 | 38 / 100 | 99 |

At level 0 and 30 cm the link loses about nothing, and the slave received
almost every message. The master reports failure because it does not hear the
acknowledgement. What is established:

- **The acknowledgement is on air.** In run 2, 14 of the 35 failures show the
  Garmin's `F2` in the sniffer at +2170..+2220 µs, the same offsets as the
  messages that completed (`acked_promicro_run2_sniff.log.gz`; payload byte 0
  is the index, `acked_promicro_run2_out.txt` has the failed ones). The sniffer
  misses about a third of the Garmin's frames either way.
- **Two boards, two radios**: the Pro Micro (nRF52840) and the L15 (nRF54L15).
  The L15's console counts every failure as `no_ack`: the window closed with
  nothing in it.
- **Temporary instrumentation on the L15** (never committed;
  `acked_l15_diag*_console.log.gz`): each failure is a window that ran to its
  close, at +2563..2576 µs, with no receive event. The acknowledgements that
  were heard cluster at about +2195..2225 and +2245..2265 µs (in the master's
  own timebase), with nothing in the roughly 30 µs between those clusters.
  - diag 2 opened the first-attempt window at 2190 - 250 instead of 1310:
    15 % failed, within the run-to-run spread.
  - diag 3 asked the window for CRC-failed frames: 3 of 7 failed windows had
    received the acknowledgement with a bad CRC (one decoded as `F4`), and
    the other 4 had nothing.
- **Bursts hide it.** A missed first acknowledgement of an `8A` is followed by
  a retry, which the Garmin, already in the burst, answers at +1550. That is
  the A1/A2 pattern of repeated `8A` answered at +1534..+1584.

Not established: why acknowledgements arriving on the slot turnaround are
missed or corrupted when burst acknowledgements at +1560 are not. Candidates
still open: the receiver around +2230 µs, the master's transmit timing jitter
against its reported t_sync, and the Garmin's slot-reply transmit. TODO.md has
the follow-up.
