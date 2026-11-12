# TI burst retry: ack-window reserve + cc26xx in-grant lead, 2026-11-12 (late afternoon)

Commit e042a10 on `ti/in-grant-lead`. Follows
`../2026-11-12-ti-findings/README.md`, where every remaining TI master burst
failure was a transmit `pass_step()` expired by 18-29 us: the retry after a
missed acknowledgement, armed from the empty ACK window's TIMEOUT (late by the
CC26x2's 508 us close hold) against the 600 us fresh-arm lead.

## The change

- `radiant_burst.c` `arm_ack_window()`: the window reserves
  `RADIANT_TRANSFER_NEXT_PACKET_US` (1550 us) past `t_close`. That covers the
  retry (`t_close - ACK_GUARD_US + NEXT_PACKET_US`) and the next packet. Before
  it, the ACK window reserved nothing, so neither could ever be armed at an
  in-grant lead. Lowering the caps alone would have changed nothing.
- `radiant_radio_cc26xx.c`: non-coex `min_arm_lead_in_grant_us` = 400 us
  (`IN_GRANT_LEAD_US`; was 0, i.e. 600). Coex stays 600.
- ztest `test_a_retry_is_armed_inside_the_ack_windows_reserve`: fails without
  the reserve line (checked on Bluefin), passes with it. Full suite 4625/4625.

## Images

| name | commit | zephyr md5 |
|---|---|---|
| `ti_lead` | e042a10 | hex `AB20678BF2F07B92A5D8CFCD39D29F4B` |
| `ti_stage` (master code) | 0207f86 | hex `9C8BBF3C1C0CB170FEED7FEDB3F085B3` |
| `pm_lead` (Pro Micro) | e042a10 | uf2 `FEC1A6EBE23031C7AA7A12A261B53096` |
| Pro Micro original (12:18) | - | uf2 `15DEBE5C918813C377D8B5358B899E68` |

## TI A/B (`ab_lead.ps1`, `run_leg.sh`), alternating, reflashed every leg

Bursts: TI master, Garmin `006` slave at `--burst-tx-power 3`, 150 x 8 packets.

| leg | image | failed | expired TX | worst margin |
|---|---|---|---|---|
| L1 | ti_lead | 4 / 150 | 0 | +188 us |
| S1 | ti_stage | 5 / 150 | 5 | -26 us |
| L2 | ti_lead | 1 / 150 | 0 | +187 us |
| S2 | ti_stage | 3 / 150 | 3 | -29 us |
| L3 | ti_lead | 2 / 150 | 0 | +188 us |
| S3 | ti_stage | 3 / 150 | 2 | -25 us |

**ti_lead 7/450 (1.6 %) vs ti_stage 11/450 (2.4 %); expired transmits 0 vs
10.** All three pairs the same sign. The failures left on ti_lead are
`xfer_no_ack` (five retries sent, none acknowledged): the same class and
about the same rate as the nRF master against the Garmin, so the room's
floor, not TI timing. The count difference alone is small; the decision rests
on the eliminated failure class plus no regression below.

Tracking, same legs, 300 s, TI and Garmin in the same window, trainer #52233
and the L15 DK simulating #4243 (`ant_sim.py`, RadiANT UART dongle on COM4):

| leg | TI #4243 loss | TI #52233 loss | Garmin #52233 loss | TI - Garmin |
|---|---|---|---|---|
| L1 | 6.82 % | 17.89 % | 21.88 % | -4.0 pp |
| S1 | 7.33 % | 15.58 % | 17.43 % | -1.9 pp |
| L2 | 6.41 % | 19.87 % | 23.82 % | -4.0 pp |
| S2 | 7.33 % | 16.67 % | 18.97 % | -2.3 pp |
| L3 | 6.58 % | 17.58 % | 20.53 % | -3.0 pp |
| S3 | 6.74 % | 15.67 % | 19.48 % | -3.8 pp |

No tracking regression (ti_lead means 6.6 % / -3.7 pp, ti_stage 7.1 % /
-2.7 pp). The Garmin saw #4243 at 0.0-0.55 % throughout.

## nRF regression (`ab_nrf.ps1`): `radiant_burst.c` is shared

Pro Micro master, Garmin slave at level 3, 150 x 8 packets, alternating:

| leg | image | failed |
|---|---|---|
| N1 | pm_lead | 9 / 150 |
| O1 | original | 10 / 150 |
| N2 | pm_lead | 9 / 150 |
| O2 | original | 8 / 150 |

**18/300 each.** No regression (the room was worse than at 13:00, for both).
On a direct nRF build `gate_acquire()` ignores `follow_on_us` and the two
leads coincide, so `in_reserve()` is never true; only the scheduler's
planning around the reserve changes.

## Not measured

- MPSL builds: the ACK window now holds 1550 us more of the arbiter's air,
  and the retry/next packet become armable at the MPSL in-grant lead (before,
  at the full MPSL lead, they never were). No MPSL burst rig was run.
- cc26xx coex: in-grant lead stays 600, so the retry still cannot fit there.

## State left behind

TI: `ti_lead` (the merged code). Pro Micro: original 12:18 image (probe
PASS). L15: untouched.
