<!-- SPDX-License-Identifier: Apache-2.0 -->
# Outbound burst retries: A/B/A, 2026-11-11

Checked by: nothing automated. This is the bench evidence for the outbound
burst retries on branch `fix/spec-rev51-gaps` (Rev 5.1 sec 5.4.3 / 9.5.5.3:
retry each burst packet up to five times, then `EVENT_TRANSFER_TX_FAILED`).

## Result

| Leg | Image | Bursts failed | Slave got whole |
|---|---|---|---|
| A1 | retries (5) | **0 / 100** | 100 |
| B | retries compiled to 0 | **47 / 100** | 56 |
| A2 | retries (5) | **2 / 100** | 98 |

Each burst is 8 packets. Without retries 47 % failed, which is what
1-(1-p)^8 gives at p = 7.6 % per packet (a lost packet or a lost
acknowledgement). With retries 0 and 2 failed, in the legs either side of it,
so the room did not drift enough to explain the difference.

**Correction (2026-11-12, `../2026-11-12-master-acked-power/`).** The
-40 dBm never reached the burst packets. The transfer engine kept the power
the channel opened with, and `0x60` after open changed only the broadcasts
(fixed since). So the master's `82/8A/92/B2` went out at full power (-33 dBm
at the sniffer against -81 for its `0A`), and the Garmin's acknowledgements
were at its level 0. The per-packet losses here were mostly missed
acknowledgements on the return path, not lost packets. The finding that
retries recover them still stands; the "-40 dBm" placement description below
does not.

The sniffer's control-byte counts show the retries doing the work. 100 bursts
need exactly 100 `8A`, 300 `92`, 300 `82` and 100 `B2`:

| Leg | `8A` | `92` | `82` | `B2` |
|---|---|---|---|---|
| A1 | 111 | 329 | 319 | 111 |
| B | 100 | 232 | 216 | 56 |
| A2 | 104 | 326 | 321 | 105 |

In A1 that is about 70 repeated packets in 800 (`B3` histograms from the
`.log.gz` files here, which also miss frames, so they are lower bounds). In B
nothing repeats, and the counts fall off after the first lost packet of each
burst.

## Rig

- **Master (dongle under test):** the Pro Micro (`EE6986854FD72803`),
  `apps/dongle`, core backend, nRF radio, non-MPSL, built with
  `scripts/build_all.ps1 -Only radiant_dongle_promicro.uf2` at NCS v3.4.0.
  A legs: the tree of commit `ea9b3f1` (UF2 md5 `55201c2f1283...`). B leg: the
  same tree with `RADIANT_TRANSFER_TX_RETRIES_MAX` set to `0u`, built in a
  separate worktree (md5 `9bdb9529ec11...`). Flashed over USB with
  `ant_probe.py --reboot-to-bootloader`.
- **Slave (reference):** Garmin ANT USB-m (`006`), bidirectional slave.
- **Sniffer:** `radiant/spike/promisc` on the nRF54L15 DK, console on its log
  VCOM with DTR set.
- **Command:**

      python tools/ant_ack_pair.py --master-serial EE6986854FD72803 \
          --slave-serial 006 --master-bursts 100 --burst-packets 8 \
          --burst-gap 0.3 --burst-tx-power 0 --burst-master-dbm -40

  Both ends at ANT power level 0, then the master alone at -40 dBm through
  the custom-register escape. At level 0 alone the two sticks, about 30 cm
  apart, lost nothing, and neither leg could have shown a difference.

## Before this could run

The first attempt failed every burst on the old image too. That was not
retries: a RadiANT master's multi-packet burst had never completed on air,
because the slave answers packet 0 on its slot-reply turnaround (+2209 us)
and the master stopped listening at +1810 us, and because the master
re-anchored its period on every burst packet. Both are fixed in `ea9b3f1`;
the captures are in `archive/captures/radio/` ("The master-burst captures").

## A2's two failures (traced 2026-11-12)

Both are the same thing: the Garmin missed a packet and repeated its
acknowledgement of the previous one, and the master took that for a bad
acknowledgement and gave up without retrying (`FAIL_BAD_ACK` was terminal).

| Burst | Sniffer frames | Master sent | Garmin answered |
|---|---|---|---|
| `0x15` | n=7811..7816 | `82` (seq 0) | `C2` at +1544, then `C2` four more times ~3140 us apart |
| `0x24` | n=8061..8066 | `92` (seq 1) | `D2` at +1588, then `D2` four more times |

A real acknowledgement complements the sequence bit (`82` -> `D2`,
`92` -> `C2`). These kept it, which is "I still expect the packet you just
sent". The master sent nothing after either one, and the next frame on air is
the master's next slot. Fixed on `fix/burst-retry-followups`: a stale
acknowledgement (`radiant_transfer_ack_is_stale()`) is retried like an empty
window.

## Not covered

- MPSL builds. A retry falls outside the data packet's grant there, as
  multi-packet bursts do (TODO.md).
- Inbound bursts (RadiANT as the receiving slave). That is the deferred
  inbound-burst work in TODO.md.
