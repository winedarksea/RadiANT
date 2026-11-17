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
