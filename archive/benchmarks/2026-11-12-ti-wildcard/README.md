<!-- SPDX-License-Identifier: Apache-2.0 -->
# TI wildcard short match, A/B/A against a same-window Garmin: 2026-11-12

Checked by: nothing automated. Raw results behind the "On TI" section of
[ADR 0022](../../../docs/decisions/0022-search-matches-a-wildcard-device-number.md).

## Rig

- **DUT:** LAUNCHXL-CC26X2R1 (CC2652R), `apps/dongle_ti`, core/cc26xx, ANT on
  COM11 at 57600.
- **Reference:** Garmin USB-m `006` in the same window, every run.
- **On the air:** Wahoo trainer #52233 (power 0x0B, FE-C 0x11, speed 0x7B).
- **Tool:** one window each, 300 s:

      python tools/ant_scan_rate.py --port COM11 --baud 57600 --serial 006 \
          --seconds 300 --reopen 4.5 --track 52233:0x11 --json

  `--port`/`--baud` joining `--serial` in one same-start window is 0e26977.
- **Order:** A1, B1, A2.
- **Images** (all with `ti_health.conf` and the bench-only `diag_health.patch`,
  and with the LaunchXL's then-default immediate logging, **both arms alike**):
  - **A (A1, A2):** c911f31, `CONFIG_RADIANT_SEARCH_SHORT_MATCH` off (its
    default there): 24-bit sync word, 128-set sweep. Today this is
    `apps/dongle_ti/ti_long_match.conf`.
  - **B (B1):** the same code with the symbol on (`ti_short_match.conf` at
    c911f31, the file ti_long_match.conf replaced): 16-bit sync word, one set.

  The untracked bench-worktree build directories were `build/s3_a` and
  `build/s3_b`.

## Files

| File | What |
|---|---|
| `A1.scan.json`, `B1.scan.json`, `A2.scan.json` | `ant_scan_rate.py --json` merged output for the window; the TI is the child labelled `COM11`, the Garmin is `006` |
| `<w>.health.h0.json`, `<w>.health.h1.json` | 0xF6 health reply before and after the window (`tools/ant_health.py --port COM11 --baud 57600 --json`) |
| `diag_health.patch` | bench-only patch to `apps/common/ant_health.c`, never committed |

**The wildcard counters are in the wrong-named fields.** The patch writes
`radiant_cc26xx_sync_frames` into the 0xF6 reply's `tx_drops` and
`radiant_cc26xx_sw_addr_mismatch` into `rx_backpressure` (both UART-zero
fields on this build), **mod 65536**. Read `h1` minus `h0`:

| Window | `tx_drops` = sync frames | `rx_backpressure` = software address mismatches |
|---|---|---|
| A1 | 1110 | 42 |
| B1 | 3149 | 14 |
| A2 | 1196 | 31 |

`radiant_cc26xx_sync_frames` counts every received entry `rx_drain()` looked
at (the hardware matched a sync word), and `radiant_cc26xx_sw_addr_mismatch`
those whose software-compared address bytes matched no filter: noise or
another sensor's frame. Both run in
both images (c911f31), so A and B compare. The other health fields in these
files are real: `host_csum` and `host_desync` read 0 throughout.

## Results

Idle sightings are per second over 300 s, medians are the time from scan open
to the first report of that device (64 opens on the TI, 66-67 on the Garmin), tracked loss is on
`52233:0x11`.

| Window | TI /s | TI median 1st 0x0B / 0x7B | TI tracked loss | Garmin /s | Garmin tracked loss | TI - Garmin | Host events |
|---|---|---|---|---|---|---|---|
| A1 24-bit | 0.52 | 0.84 / 0.91 s | 34.94 % | 4.82 | 53.80 % | -18.9 pp | `ch1:0x08` x2 |
| B 16-bit | 4.45 | 0.30 / 0.39 s | 23.07 % | 4.58 | 58.44 % | -35.4 pp | none |
| A2 24-bit | 0.52 | 0.59 / 0.74 s | 26.30 % | 6.78 | 38.65 % | -12.4 pp | `ch1:0x08` x1 |

In B the TI heard 0x0B at 2.98/s and 0x7B at 1.47/s (Garmin 3.04 and 1.54).

## What it says

- **Discovery at parity.** 4.45 sightings/s against the Garmin's 4.58 in the
  same window; 0.52 with the 24-bit match and its 128-set sweep.
- **Gate 1 (each 4 Hz sensor >= 1 sighting/s):** pass. **Gate 2 (median first
  sighting < 0.5 s):** pass, 0.30 and 0.39 s against 0.59-0.91 s.
- **Gate 3 (tracked loss within the baseline):** pass, as "no worse". B's
  23.07 % is below both A windows (34.94 %, 26.30 %); the A windows are 8.6 pp
  apart themselves, so the room's swing is larger than the B-A gap. Normalised to
  the same-window Garmin, the TI is 35.4 pp below it in B against 18.9 and 12.4
  in the A windows.
- **Gate 4 (noise counted, no new host events):** pass. Fewer software
  mismatches in B (14) than in either A (42, 31), and no host event.
- `tools/ab_gates.toml`'s acquisition gate was **not** run; it needs
  `ant_verify` T1 sittings.

## Caveats

- **The 16-bit match applies to tracking too** on this radio (`nSwBits` is
  fixed per `RF_open()`), so gate 3 is a measurement of the whole backend, not
  of search alone.
- **The Garmin's own tracked loss is high (39-58 %).** Its scan channel
  collides with its tracked channel (a known Garmin trait), and it moved
  between windows (53.80, 58.44, 38.65 %). Compare the TI with itself first.
- **One window per arm** (two for A). The A pair shows the room's drift.
- **Immediate logging in all three images.** It stalls the ANT UART under
  acked traffic (see `../2026-11-12-ti-coex-rearm/`). Here `host_csum` and
  `host_desync` read 0 in every window, and the setting is the same in A and B,
  which is what this A/B needs. The TI's sighting rate may still have been
  lower than with deferred logging; that was not measured.
- The counters are mod 65536 and were read once per window. The sync-frame
  count is far below that.
- The nRF default is unchanged (n); the nRF gate 3 question is separate
  (`../2026-11-11-scan-rate/`).
