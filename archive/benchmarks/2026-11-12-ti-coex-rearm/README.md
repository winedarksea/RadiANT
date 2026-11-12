<!-- SPDX-License-Identifier: Apache-2.0 -->
# TI coexistence re-arm refusal, and what hid it: 2026-11-12

Checked by: nothing automated. Raw results behind the "Result (2026-11-12)"
section of [ADR 0015](../../../docs/decisions/0015-cc26xx-coexistence-design.md).

## Rig

- **DUT:** LAUNCHXL-CC26X2R1 (CC2652R), `apps/dongle_ti`, core/cc26xx. ANT on
  COM11 at 57600. The board's log pins (uart1, DIO22/24) have nothing attached.
- **Peer:** Pro Micro `EE6986854FD72803`, a bidirectional slave originating
  acked data.
- **Why health counters.** A DSS (JTAG) connect resets the part (Zephyr
  `curr_tick` read 0 right after connect, about 30876 ticks 1 s later), so no
  SRAM counter can be read from outside. Every image carries
  `CONFIG_ANT_DONGLE_HEALTH_COUNTERS=y` (`apps/dongle_ti/ti_health.conf`) and
  the 0xF6 reply is read over the ANT UART.

## Commands

    python tools/ant_health.py --port COM11 --baud 57600 --json     # h0: before
    python tools/ant_ack_pair.py --master-port COM11 --master-baud 57600 \
        --slave-serial EE6986854FD72803 --seconds 120                # 8 acked sends
    python tools/ant_health.py --port COM11 --baud 57600 --json     # h1: after the ack phase
    python tools/ant_ack_pair.py --master-port COM11 --master-baud 57600 \
        --slave-serial EE6986854FD72803 --master-bursts 20 --burst-packets 8
    python tools/ant_health.py --port COM11 --baud 57600 --json     # h2: after the bursts

Health was read by hand between the phases; the `ant_health.py` options above
are the ones it takes (`--port`, `--baud`, `--json`).

## Files

Each image `<name>` has five files:

| File | What |
|---|---|
| `<name>.ack.json` | `ant_ack_pair.py` result for the 120 s acked phase: `sent`, `completed`, `failed`, `peer_received`, staged-payload loss, events |
| `<name>.burst.json` | the same tool with `--master-bursts 20 --burst-packets 8`: `outcomes`, and `slave.packets/whole/broken` as the slave saw them |
| `<name>.health.h0.json` | 0xF6 health reply before the ack phase (all zero on a fresh boot) |
| `<name>.health.h1.json` | after the ack phase |
| `<name>.health.h2.json` | after the bursts (counters are cumulative, so h2 includes the ack phase) |

The health JSON is `tools/ant_health.py --json`: `host_csum` and `host_desync`
(host-link frames dropped on the ANT UART), `sched_denied` (arms refused by
`RF_scheduleCmd`, synchronous `EDENIED`), `sched_missed` (arms that missed
their lead), `sched_failed`, and the other 0xF6 fields.

| Name | Image |
|---|---|
| `A_patch` | arm 2, `ti_patch.conf` (`RF_postCmd`, the control) with `ti_health.conf` |
| `B_gate` | arm 3, `ti_gate.conf` with `ti_health.conf`, at c125bb6 (refusals counted, not fixed) |
| `Bfix_gate` | arm 3 at e82a0ea (own cancelled window treated as absent) |
| `nolog` | arm 1 (core/cc26xx, no coexistence), `CONFIG_LOG=n`, health counters |
| `final_deferred` | arm 1 at 5875bd9: deferred logging, short match on, health counters |

The first three, and the `s3_*` images of `../2026-11-12-ti-wildcard/`, carried
the LaunchXL's then-default **immediate** logging. Build directories were
untracked (`build/` in the bench worktree), so the commit and fragment list
above is the record.

## Results

| Image | Acked received at TI | `sched_denied` | ACK completed | `host_csum` / `host_desync` after ack phase | Staged payload loss | Bursts whole | Packets at slave | `sched_missed` after bursts |
|---|---|---|---|---|---|---|---|---|
| `A_patch` | 5/8 | 0 | 0/8 | 251 / 2650 | 100 % | 0/20 | 16 of 160 | 16 |
| `B_gate` | 8/8 | **8** | 0/8 | 253 / 2620 | 100 % | 0/20 | 14 | 14 |
| `Bfix_gate` | 7/8 | **0** | 0/8 | 251 / 2648 | 100 % | 0/20 | 14 | 14 |
| `nolog` | 4/8 | 0 | 4/8 | 0 / 0 | 0.8 % | 0/20 | 20 | 20 |
| `final_deferred` | 2/8 | 0 | 2/8 | 0 / 0 | 0.2 % | 0/20 | 20 | 20 |

## What it says

- **The refusal is real and the fix removes it.** One `sched_denied` per
  received acked message on arm 3 (8 of 8), none on arm 2, none after e82a0ea.
- **The plan's success bar could not be judged.** The control, arm 2, completed
  no ACK either. Two older defects explain that:
  1. Immediate logging at INF onto unattached uart1 stalls the ANT UART and the
     radio: about 250 `host_csum` and 2650 `host_desync` in 120 s, 100 %
     staged-payload loss, 0/8 ACKs, on the three immediate-mode images. With
     logging off or deferred both fall to 0, and every acked message the TI
     receives is ACKed. 5875bd9 makes deferred the board default.
  2. Burst chaining fails on every image, including the two clean ones: the
     slave gets about one packet per burst and `sched_missed` rises by one per
     burst. The arm for the chained packet misses its lead
     (`MIN_ARM_LEAD_US` 600).
- **Still open:** the TI hears only 2-5 of the 8 acked packets even without the
  UART stall; burst chaining (see `TODO.md`, "TI CC2652R findings").

## Caveats

- Eight acked sends per image is a small sample. 5/8, 7/8 and 8/8 are not
  distinguishable from each other; the refusal counter is the measurement.
- The acked phase on the immediate images shared its 120 s with the UART
  stall, so its "received at TI" numbers are not comparable with `nolog` and
  `final_deferred`.
- "ACK completed" is out of the 8 sent. On `nolog` and `final_deferred` it
  equals the number received (4 and 2).
- Arm 4 (`ti_coex`) was only built and linked at both commits; there is no
  802.15.4 peer on this bench.
