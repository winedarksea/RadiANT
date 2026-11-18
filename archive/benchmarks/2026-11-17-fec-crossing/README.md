# FE-C / power slot crossing: simulated masters (plan step 4), 2026-11-17

TODO.md "2026-11-15 Zwift captures" item 1. Question: are the FE-C runs of misses
seen at each power/FE-C crossing in Zwift (215313: 13/21 lost, a run of 8, then
GO_TO_SEARCH) made by the dongle, or by the real masters on the air?

## Rig

- FE-C master: `apps/trainer` on the nRF54L15 DK (direct nRF, no MPSL), period
  8192, **device 52235** (the real Kicker #52233 was on the air, so the sim could
  not use its number), `ackfault preset ideal`.
- Power master: `tools/ant_sim.py --profile power --device-number 20329` on the
  nRF5340 DK net core (COM5 @115200), period 8182. The real #20329 was asleep.
- Receivers, all three at once, `tools/ant_crossing.py` (Zwift channel setup:
  pinned IDs, trans 0, search timeout 0xFF, LIB_CONFIG 0xE0):
  - Pro Micro `EE6986854FD72803`, `pm_diag_27a2a15.uf2` (apps/dongle, core/nrf,
    `diag_counters.conf`, master 27a2a15).
  - TI LaunchXL COM11 @57600, diag a63489c5 (94ec6a8; no radiant/apps change
    since, so equal to master).
  - Garmin USB-m `006`, FE-C only, a different stack.
- Leg 1: Pro Micro sends one 0x33 (grade 0 %) at a time at 1 Hz; TI power+FE-C
  without commands; Garmin FE-C only. Leg 2: TI sends; Pro Micro power+FE-C
  without commands; Garmin FE-C only. 1200 s each, 6 crossings each.

Image hashes: `md5sums.txt`. Per-leg logs (`*.log`, decode_pcap text format
plus `# diag` page snapshots), console output (`*.out`) and the replay with
controls (`*_replay.txt`).

## Result

| | FE-C in band (\|offset\| <= 3 ms), 6 crossings | longest FE-C run in band | FE-C whole run | power in band | commands completed |
|---|---|---|---|---|---|
| Leg 1 Pro Micro (cmds) | 6 / 117 | 1 | 0.4 % | 15 / 117 | 1184 / 1198 |
| Leg 1 TI (no cmds) | 8 / 117 | 2 (at +0.11/-0.17 ms) | 0.8 % | 8 / 117 | - |
| Leg 2 TI (cmds) | 7 / 117 | 2 (at +0.12/-0.15 ms) | 0.9 % | 21 / 117 | 1158 / 1198 |
| Leg 2 Pro Micro (no cmds) | 7 / 117 | 1 | 0.4 % | 9 / 117 | - |
| Garmin (FE-C only, both legs) | 1-5 of 22-23 per crossing window | 2 | 5.2-5.7 % | - | - |

- Every FE-C miss in band sits within about +-0.2 ms of power's frame, where the
  two masters transmit on top of each other. The 0.57-2.67 ms band that lost
  every slot in Zwift loses nothing here.
- The FE-C-only controls (Garmin, and the no-command receiver) lose the same
  one slot per crossing, so that slot is on the air.
- Pages: `slots_collided` rises 1-3 per crossing and is 0.0/s outside crossings
  (ADR 0024 on hardware); `turn_yields`, `arm_rejected`, `preempted`, `replans`
  stay 0; page 3 `term_empty` does not rise above baseline.

## Verdict

Against simulated masters the dongle serves the crossing on both stacks: no
FE-C run longer than the on-air overlap, band loss equal to the controls. The
Zwift loss therefore needs something the sims do not have. The real Kicker is
the leading candidate (the plan's "on the air" hypothesis), but this run cannot
show that. Plan step 3, with the real Kicker and #20329 and FE-C-only controls,
is the test that can. Two parts of the comparison are not like for like: the
sim uses device 52235, not 52233 (so pair-specific address matching is not
tested), and its link is short and strong.

---

# Real masters (plan step 3), 2026-11-17 — raw logs local in `real/`

TODO.md "2026-11-15 Zwift captures" item 1. Question: are the FE-C misses at a
power #20329 / FE-C #52233 crossing made by the dongle or by the masters on the air?

## Rig

- Masters: the real Wahoo Kicker #52233 (FE-C, 8192) and power meter #20329
  (8182), owner pedalling, Zwift closed.
- DUT: Pro Micro `EE6986854FD72803`, `images/pm_diag_acdd9d2.uf2` (apps/dongle,
  core/nrf, `diag_counters.conf`, master acdd9d2; md5 in `md5sums.txt`),
  `ant_crossing.py --channels power,fec`, one 0x33 (0 %) pending at 1 Hz.
- Control A: TI LaunchXL COM11 @57600, `images/ti_diag_acdd9d2.hex`, FE-C only,
  no commands.
- Control B: Garmin USB-m `006`, FE-C only, no commands.
- All three started in the same second. The controls were stopped by hand when
  the DUT closed crossing 2 (381 s).

Files: `pm_dut.log`/`.out`, `ti_fec.log`/`.out`, `garmin_fec.log`/`.out`,
`replay.txt` (`ant_crossing.py --replay pm_dut.log --control ti_fec.log garmin_fec.log`).

## Result

The FE-C link was weak today on every receiver:

| receiver | FE-C missed, whole run |
|---|---|
| Pro Micro DUT (power + FE-C, commands) | 40.5 % background (\|offset\| > 10 ms), 41.0 % overall |
| TI (FE-C only) | 29 % (454 / 1564) |
| Garmin (FE-C only) | 69 % (1076 / 1566): **dropped from the verdict** (> 40 % rule) |

| | DUT in band (\|offset\| <= 3 ms) | TI in band | Garmin in band | DUT pages over the crossing |
|---|---|---|---|---|
| crossing 1 (180.2-184.7 s) | 11 / 19, run 2 | 11 / 22, run 2 | 16 / 22, run 4 | `slots_collided` 1, `turn_yields`/`arm_rejected`/`preempted` 0, `term_empty` 34 [35.4 baseline] |
| crossing 2 (372.7-377.5 s) | 16 / 20, run 8, GO_TO_SEARCH, 3 cmd failed | 15 / 23, run 5 | 20 / 23, run 14 | `slots_collided` 0, others 0, `term_empty` 22 [17.7], `slots_missed` 27 [17.0] |

Slot by slot (DUT and TI matched by host time, ±80 ms on a 250 ms grid):

| | both heard | TI only | DUT only | neither | P(DUT miss \| TI heard) |
|---|---|---|---|---|---|
| background | 811 | 246 | 74 | 350 | 0.23 |
| crossing windows | 12 | 4 | 1 | 23 | 0.25 |

- The excess loss in the crossing is in the "neither" column: 58 % of in-band
  slots were missed by both receivers, against 24 % elsewhere. The DUT's own
  conditional loss (missed although the TI heard the frame) is the same in the
  band as outside it.
- The DUT's scheduler did nothing different: one collision in crossing 1, none
  in crossing 2, no turn yields, no arm rejections. Its extra misses in crossing
  2 show up as armed-empty windows (`term_empty`), which is what a frame that
  was never on the air looks like.

## Verdict

**On the air.** In the crossing band the FE-C-only TI, which has no second channel
to schedule around, loses the same slots as the DUT. The DUT loses no more of the
frames the TI heard than it does in the rest of the ride, and its scheduler counters
stay flat. So the Kicker's FE-C frames are not being received near #20329's frame by
any receiver. The leading explanation is still that the Kicker's radio is busy with
#20329 then (an old PowerMatch pairing). This run cannot show the mechanism inside
the Kicker.

Limits:
- Only two crossings.
- Background loss was 29-40 %, so the band excess is measured against a noisy floor.
- The 2026-11-15 "every slot in 0.6-2.7 ms is lost" pattern is weaker here: a few
  in-band slots were heard, by both receivers.

Crossing 2 still sent FE-C to GO_TO_SEARCH (8 armed-empty slots in a row).
ADR 0024's alternation cannot help with misses that are on the air.
