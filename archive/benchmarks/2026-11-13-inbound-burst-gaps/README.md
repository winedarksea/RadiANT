# Inbound-burst gaps (2026-11-12/13)

Branch `fix/inbound-burst-gaps`: d507ae0 (ACK repeats, post-burst guard, burst
fault seam, contention test, static checks) and 442d4f7 (0xAA on the slot-reply
turnaround). Follow-up to `../2026-11-12-inbound-burst/`.

Boards: Pro Micro clone `EE6986854FD72803` (apps/dongle, core/nRF,
`health.conf;diag_counters.conf`), Garmin USB-m `006`, nRF52840 Dongle
`3D55F77818BE772A` (same-SoC control, gate-3 image, unchanged), nRF54L15 DK
(MPSL leg, then the promisc sniffer). Same desk, same sitting.

## 1. MPSL inbound bursts: diagnosis confirmed, fix deferred

L15 DK, `apps/dongle_thread` + `gate.conf` (`RADIANT_BACKEND_NRF_GATE_MPSL=y`),
master at 0938ec6. `ant_ack_pair.py --master-serial 006 --slave-port COM4
--master-bursts 100 --burst-packets 8` (`mpsl_pre.*`):

| told FAILED | slave packets | whole bursts | RX_FAILED |
|---|---|---|---|
| 100/100 | 89 | 0 | 89 |

Packet 0 is delivered, then the continuation window cannot be armed: no grant
holds it. The planned fix (an ACK inside a burst reserves its window) was NOT
built. The MPSL gate serves an in-grant arm only when that arm's own follow-on
reserve also ends inside the current grant (`gate_acquire()`,
`radiant_radio_nrf_gate_mpsl.c`), and fixed-class grants never grow. Every
reserve in the chain must therefore nest inside the first grant (the tracked
window + 4150 us). Packet 1's window, plus the 1906 us reserve for its own ACK,
ends about 1.7 ms past that grant whatever the ACK reserves. The fix is
on-demand extension of fixed-class grants, the same work as outbound MPSL
bursts (TODO item 4). Decision 2026-11-13: defer there. Two static checks
landed anyway: the continuation window's reserve covers the ACK repeat, and
the tracked reserve covers an opener's ACK at +2190.

## 2. ACK repeats default 5: completion unchanged

Image B (default 5). `--master-bursts 100 --burst-packets 8`:

| leg | originator -> receiver | completed | receiver whole | RX_FAILED |
|---|---|---|---|---|
| A1 | Garmin -> RadiANT | 100/100 | 100 | 0 |
| C  | RadiANT -> Garmin (control) | 98/100 | 98 | n/a |
| A2 | Garmin -> RadiANT | 100/100 | 100 | 0 |

2026-11-12 with 20 repeats: 96 / 97 / 98. The lossy capture (section 5) shows
the repeats on air: 6 x D2 (original + 5) at ~3142 us, then the burst ends.

## 3. Long burst against the event queue: pass

`--master-bursts 20 --burst-packets 64` Garmin -> Pro Micro (`long64.*`):
20/20 completed, 20 whole bursts of 64, 1280/1280 packets, 0 RX_FAILED.
0xF6 before/after (`long_health_*.txt`): `event_high_water` 1 (of 64),
`event_overflows` 0. `tx_drops` and `tx_timeout` rose by 1 each. Not the
burst: the control in `teardown_ctl.log` shows +1/+1 for every
`ant_ack_pair.py` session, 2 x 8 packets included. It is the tool closing the
device with an event in flight. No queue change is warranted.

## 4. No-burst regression leg: pass

Pro Micro A = fee84f3 (before inbound bursts), B = d507ae0, same conf, USB
reflash between windows. Gate-3 recipe at 120 s, all three sticks in every
window (`regression/`):

    ant_scan_rate.py --serial EE6986854FD72803 --serial 3D55F77818BE772A \
        --serial 006 --seconds 120 --reopen 4.5 --track 52233:0x0B --track 52233:0x11

| window | Pro Micro % | Dongle % | Garmin % | PM - Dongle pp | PM - Garmin pp |
|---|---|---|---|---|---|
| w1 A | 20.40 | 19.77 | 24.68 | +0.62 | -4.29 |
| w2 B | 20.73 | 20.31 | 25.08 | +0.42 | -4.35 |
| w3 A | 19.06 | 21.04 | 26.35 | -1.98 | -7.29 |

B lies inside the A windows' range against both controls (bench-gates rule:
same-window controls, this room cannot hold a sequential A/B). The absolute
~20 % is the scan channel colliding with the tracked ones, as in gate 3.

## 5. Sniffer captures (L15 `radiant/spike/promisc`)

Logs in `archive/captures/radio/2026-11-13-*.log`; see the README there.

**0xAA (acknowledged data, Garmin master -> RadiANT slave):**
`--master-acked 50` with our 0xF2 at +1535..+1573 us: the Garmin master was
told FAILED **50/50**, though the slave received all 50 (`capA_acked.log`).
With 442d4f7 (+2160..+2169 us): **96/100** completed (`aa_fixed.log`),
against **99/100** for RadiANT -> Garmin (`aa_control.log`). No second
non-RadiANT stick was available to record a reference slave's 0xAA reply
offset; the change rests on the Garmin master's acceptance. In the fixed leg
the slave also counted rx_fail 8 and one go_to_search on broadcasts
(control: rx_fail 2, 0). Not investigated.

**Bursts, normal power** (`capB.*`, 19/20 completed; the failed burst's
0x8A got no answer at all, slave 0 packets):
- our 0x8A ACK at +2165..+2183 us (target 2190);
- master's next packet at +1486..+1608 us after our ACK (median 1561), well
  inside the +1550 +/- 250 window;
- the master's first slot frame after every burst is one period after its
  0x8A and carries the last burst packet: **it keeps its grid**. The
  -30 us/period slope in the log is the sniffer's clock against the Garmin's.

**Bursts, our ACKs weak** (`--burst-slave-tx-power 0`, `capB_lossy.*`):
11/20 completed, 5 broken = 5 RX_FAILED, 0 sequence gaps.
- When the 0x8A's ACK is lost the Garmin master does **not** retry the 0x8A
  in the slot: no further packet follows, our D2 repeats 5 times, and the
  burst fails (bursts 4, 6, 8, 13 of the log).
- A mid-burst packet whose ACK was lost is retried ~3110-3140 us after the
  previous attempt, inside our continuation window.
- Three bursts in a row (10-12) got no answer to their 0x8A at all. Not
  explained; it also happened once at normal power.

## Unit and API suites

Bluefin twister (native_sim): 16/16 scenarios, 5790 cases, check_test_counts
OK, on both commits. Mutation check: dropping the skip term from the guard and
the MUTE-in-burst call fails exactly `guard_widens_for_periods_a_burst_stepped_over`,
`a_burst_longer_than_eight_periods_does_not_drop_to_search` and
`a_muted_burst_packet_is_reacknowledged_on_its_retry`.
