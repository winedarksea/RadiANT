# RadiANT receivers vs the Garmin ANT USB-m, 2026-11-07

Rig: `tools/compare_rigs/2026-11-07-fixed.toml` (copied into each run as `rig.toml`).
Every comparison is same-window: one transmitter, every receiver listening at once.

| directory | what | when |
|---|---|---|
| `.` | the pre-registered full run: identity, rounds A/B/C, usb | 00:45-02:50 |
| `rerun/` | loss (A, B) and multi (A, B, C) again | 03:00-03:50 |
| `devnum-ab/` | the diagnosis of round B's loss gap | 02:55-04:20 |

Only the summaries are committed. Each `summary.json` holds every window's
counts and verdict inputs, and `devnum-ab/` keeps its per-receiver results.
The ~760 per-window JSONs, logs and `.antcap` captures were never committed
(see `.gitignore`), so `ant_compare --report-only` cannot re-judge these runs
from a fresh clone.

## What the first run got wrong, and what the rerun replaces

- **multi, first run: void.** Every receiver refused to start: `ant_verify`
  expanded an absent `--channel` to `0` on every channel and then refused the
  repeat. Fixed in `tools/ant_verify.py` (absent now ramps 0, 1, ...). The
  multi figures to use are `rerun/`.
- **The Garmin's multi loss was filed as "after the radio".** It is
  `EVENT_CHANNEL_COLLISION` (156 in 300 s): its own four channels contending
  for its radio. `ant_verify` now counts collisions as the stack owning up to
  the slot; `ant_compare --report-only` re-judged the stored windows.
- **acked ran at level 3**, which the RadiANT images boost to the part's
  ceiling (+8 dBm on a 52840, +4 on the 54L15, 0 on the 5340); the rig now
  uses level 2 (-4 dBm, honoured everywhere). All 0 failures either way.

## Round B's loss gap is a RadiANT receive defect keyed on the device number

Round B (54L15 transmitting `#54100`) lost 2-3 % on every RadiANT receiver and
0.3 % on the Garmin, in both runs. `devnum-ab/` isolates it, 120 s each:

| run | transmitter | device # | Garmin | Feather | Dongle | DK receiver |
|---|---|---|---|---|---|---|
| 1 | 54L15 | 54100 | 0.00 | 1.65 | 1.85 | 2.50 (5340) |
| 2 | 54L15 | 54900 | 0.20 | 0.00 | 0.00 | 0.00 (5340) |
| 3 | 54L15 | 54100 | 0.21 | 2.92 | 2.71 | 3.12 (5340) |
| 4 | 5340  | 54100 | 0.00 | 2.29 | 1.46 | 2.49 (54L15) |

Loss in %, wall-clock basis, all of it reported by the radio as RX_FAIL. It
follows the number, not the transmitter, the room or the hour, and three
RadiANT receivers on two chip families share it. The nRF backend matches a
5-byte hardware address `[A6 C5 dnl dnh dtype]` while tracking, so the device
number is inside the sync correlator - the first place to look. Not yet
root-caused. `devnum-ab/diag_b.ps1` is the script (its output path points at
the session scratchpad; change `$out` to rerun it).
