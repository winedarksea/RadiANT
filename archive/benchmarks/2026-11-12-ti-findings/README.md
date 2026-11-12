# TI CC2652R open findings, 2026-11-12 (afternoon)

Branch `ti/findings-2026-11-12` (worktree `C:\Users\Colin\ant_dongle-ti`).
Plan: `make-a-plan-to-synchronous-lighthouse.md`. Closes or measures the six
TI items TODO.md opened after the morning's LaunchXL bench.

Rig: LAUNCHXL-CC26X2R1 on COM11 at 57600 (`scripts/flash_ti.ps1`), Garmin
USB-m `006` (the reference), Pro Micro clone `EE6986854FD72803`, nRF54L15 DK
running a RadiANT UART dongle on COM4. All tools run with the NCS v3.4.0
interpreter, `python -u`, and `ant_probe.py` between workloads.

**The room was bad all day.** The trainer #52233 tracked at 20-45 % loss on
every receiver (a normal day is 5-8 %). Every comparison below is same-window
or alternating for that reason.

## Images

| name | commit | config | zephyr.hex md5 |
|---|---|---|---|
| `ti_diag` | a1cc246 | `ti_health.conf;ti_diag.conf` | `C66D48FF8E44265333E6AC404258FCF3` |
| `ti_stage` | 0207f86 | `ti_health.conf;ti_diag.conf` | `9C8BBF3C1C0CB170FEED7FEDB3F085B3` |
| `pm_stage` (Pro Micro, UF2) | 0207f86 | `diag_counters.conf` | `C330C5C1FCD48729917233C92DFBE438` |

The Pro Micro ran its existing image (version string `RADIANT0.01B00`) for
every leg in this directory unless stated otherwise.

Counters are read with `tools/ant_diag.py` (0xF7, new on this branch, ADR
0023) and `tools/ant_health.py` (0xF6), before and after each leg; the
`*_diag_{before,after}.json` pairs are those reads, `ant_diag.py --diff`
turns a pair into the leg's counts.

## Item 5: Pro Micro "0 sightings" in `ant_scan_rate.py` - NOT REPRODUCED

`scan_{a00,b40,c00,d40}.json`, `scan_e_track.json`. Pro Micro and Garmin in
the same 120 s windows, alternating the scan channel type:

| leg | type | Pro Micro sightings/s | Garmin sightings/s |
|---|---|---|---|
| a | 0x00 | 0.42 | 11.14 |
| b | 0x40 | 0.47 | 10.14 |
| c | 0x00 | 0.46 | 10.53 |
| d | 0x40 | 0.41 | 9.97 |

The channel type makes no difference, and the Pro Micro's rate matches the
2026-11-11 long-match nRF baseline (0.36-1.17/s; the short match is default n
on nRF). The morning's 0 did not recur, so no bisect was run. The scan with an
assigned bidirectional slave (0x00) now has ztests too
(`test_bidir_*` in `radiant/tests/api/src/test_api.c`), and they pass.

## Item 1: TI master hears only a few acked messages - NOT A TI DEFECT

`item1_*`. TI master, 100 acked sends per leg from the slave originator:

| leg | originator | TI turnarounds posted / done OK | frames heard in them | RF sync matches | completed |
|---|---|---|---|---|---|
| 1P | Pro Micro | 159 / 159 | 5 | 8 | 5 |
| 2G | Garmin | 159 / 159 | 100 | 101 | 100 |
| 3P | Pro Micro | 159 / 159 | 3 | 9 | 3 |
| 4G | Garmin | 159 / 159 | 100 | 102 | 100 |

Every turnaround window was armed and ran (H2 ruled out), and TI heard all of
the Garmin's acked messages in those same windows (H3, a deaf receiver after
TX, ruled out). Against the Pro Micro the RF core matched almost nothing.

The rest of the classification:

- `item1_5`: Pro Micro slave originating to a **Garmin** master: 93/100
  heard, 91 completed. The Pro Micro's acked transmit works.
- `item1_6`: **TI** slave originating to a Garmin master (Zwift's ERG path):
  100/100 completed, 3 slots missed of 148.
- `item1_7`: TI slave originating to a **Pro Micro** master: 0/100 completed,
  191 of 376 master slots missed. TI misses the Pro Micro's broadcasts too.
- `rx_nrf_vs_trainer.json`: the Pro Micro as a simulated power meter
  (`ant_sim.py`, #4242), tracked by TI and the Garmin in the same window:
  **TI 87.5 % loss, Garmin 0.34 %.** On the trainer #52233 in the same window
  TI did better than the Garmin (22.1 % against 28.7 %), so TI's sensitivity
  is not the problem.
- `rx_l15_vs_trainer.json`: the same with the **nRF54L15 DK** as the
  transmitter (#4243): TI 5.4 %, Garmin 0.34 %.

So the morning's "2-5 of 8" was this Pro Micro clone's transmission, which TI
cannot receive and the Garmin can, not a fault in TI's acked-data path.
Likely cause: the clone's carrier offset (its 32 MHz crystal) against a TI
receive bandwidth narrower than the Garmin's. Not measured; see TODO.md.

## Item 2: TI burst chaining

`item2_*`. TI master, Garmin slave at `--burst-tx-power 3` (0 dBm).

The 2026-11-12 morning test (0/20) used the Pro Micro as the slave, and a
RadiANT slave never listens at +1550 us, so it read 0 against any master. With
the Garmin as the slave, before any fix (`ti_diag`):

| leg | blocks | failed | expired TX slots | host turnaround (ack to submit) |
|---|---|---|---|---|
| `pre_ti8` | 8 x 8 bytes | 3 / 100 | 3 | 530-538 us, never late |
| `pre_ti24` | 3 x 24 bytes | 14 / 100 | 14 | 520-544 us |
| `pre_pm8` (Pro Micro master, control) | 8 x 8 bytes | 2 / 100 | 0 | - |

The planned cause (the host round trip missing ack + 1550 us) is refuted: the
round trip is ~530 us and was never late. Every failure is a chained transmit
`pass_step()` reports MISSED, with the worst margin -21 us against the 600 us
arm lead, on both the host path and the in-block (callback) path.

With pre-staging (`ti_stage`), every continuation is staged:

| leg | failed | expired | worst margin |
|---|---|---|---|
| `stage_ti8` | 4 / 100 | 4 | -27 us |
| `stage_ti24` | 3 / 100 | 3 | -27 us |
| `stage300_ti8` | 25 / 300 (24 expired, 1 no terminal) | 24 | -27 us |

The alternating A/B of the two images is below.

### The alternating A/B (`ab_stage.ps1`, `item2_ab_*`)

TI reflashed before every leg, 150 x 8-packet bursts each, Garmin slave at level 3:

| leg | image | failed | expired | worst margin |
|---|---|---|---|---|
| S1 | `ti_stage` | 4 / 150 | 3 | -25 us |
| P1 | `ti_diag` | 10 / 150 | 9 | -18 us |
| S2 | `ti_stage` | 5 / 150 | 4 | -26 us |
| P2 | `ti_diag` | 14 / 150 | 14 | -20 us |

**Staging 9/300 (3.0 %) against no staging 24/300 (8.0 %)**, both pairs the
same sign. The single 300-burst `stage300_ti8` run (8.3 %) came earlier in
the afternoon, and the room drifted. Trust the alternating figure.
97.0 % completion meets the plan's >= 97 % gate exactly, with no margin.

Every remaining failure is still an expired transmit. The margin is nearly
constant at -18 to -27 us, which fits the retry-deadline explanation in
`docs/backends.md` ("CC26x2 measured timing"): the retry at +3110 us is armed
from a TIMEOUT that arrives 508 us late, after the RX close hold. Inferred,
not counted.

## Phase 3 gates

| gate | result |
|---|---|
| TI + Garmin slave, 8-packet bursts, >= 97 % | 97.0 % (A/B above) |
| nRF regression, Pro Micro + Garmin, 300 bursts, <= 2 % fail | `pm_stage` 9/300 and 9/300 (3.0 %), but **the original image's same-day control was 8/300 (2.7 %)** (`phase3_nrf_pm_orig_300.log`). No regression; the 2 % bar is out of reach in today's room for either image |
| acked sends, no regression | Garmin to TI master on `ti_stage`: 99/100 (`phase3_acked_ti_stage.log`) |

## Item 4: decoy sync word

0xF7 page 4 `alt_sync_hits` (decoy syncWord1 fired with one sync group):
0-5 per leg, against 159 turnaround windows per item-1 leg and thousands of
burst windows. That is not "near zero" in the plan's sense, but it is two
orders below `sw_addr_mismatch` (98 in the 300-burst run). Not decided here.
The `syncWord1 = 0` A/B needs a quiet room for its 6 x 300 s tracked-loss
legs, and today's was not one.

## State left behind

- TI: `ti_stage` (md5 `9C8BBF3C...`), 0xF6 and 0xF7 on.
- Pro Micro: restored to the 12:18 `build/promicro` image from the main tree
  (uf2 md5 `15debe5c918813c377d8b5358b899e68`; probe PASS, no 0xF7 reply, as
  before).
- L15 DK: untouched (the RadiANT UART dongle on COM4 was used as a transmitter
  only).

## Not done

- No sniffer legs. The Garmin and L15 controls classified item 1 without one.
- No `min_arm_lead_in_grant_us` A/B. It is a lead change and needs the
  tracked-loss gate.
- Item 6 (acquisition gate) deferred, as planned.
