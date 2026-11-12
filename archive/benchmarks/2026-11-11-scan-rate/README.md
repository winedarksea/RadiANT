# 2026-11-11 — background-scan discovery rate, short match A/B/A

The evidence for [ADR 0022](../../../docs/decisions/0022-search-matches-a-wildcard-device-number.md).

## Rig

- **DUT:** Pro Micro (nRF52840, `EE6986854FD72803`), `apps/dongle`, core/nrf.
- **Reference:** Garmin USB stick (`006`, `AJK2.05RAF`) in the same window,
  every run.
- **On the air:** a Wahoo trainer #52233 broadcasting three channels (power
  0x0B, FE-C 0x11, speed 0x7B), plus for part of window 0 a power meter #20329
  and an unknown #41999 type 0x22.
- **Tool:** `tools/ant_scan_rate.py --serial EE6986854FD72803 --serial 006
  --seconds 300 --reopen 4.5` (Zwift's reopen cadence), with
  `--track 52233:0x0B --track 52233:0x11` for the `track2` windows.
- **Images:**
  - **A (WS1):** `0fc794f`, the serial-number commit, before any search change.
  - **A2:** the WS2 code with `CONFIG_RADIANT_SEARCH_SHORT_MATCH` off
    (`build/cu_sm_off`).
  - **B:** the same code with it on (`build/cu_sm_on`).

Each file is one window's `--json`; the number is the order they ran in.

## Results

"Worst median 1st" is the slowest per-device median time from scan open to the
first report, over devices with at least 50 sightings. Loss is RX_FAIL over
(broadcasts + RX_FAIL), averaged over the two tracked channels.

| window | RadiANT /s | Garmin /s | RadiANT worst median 1st (s) | Garmin worst median 1st (s) | RadiANT tracked loss % | Garmin tracked loss % | difference (pp) |
|---|---|---|---|---|---|---|---|
| `0-A-ws1-idle.json` | 0.95 | 16.63 | 1.79 | - | - | - | - |
| `1-A-ws1-track2.json` | 0.60 | 5.90 | 0.45 | 0.27 | 21.9 | 19.8 | +2.1 |
| `2-B-short-match-idle.json` | 12.22 | 12.58 | 0.12 | 0.16 | - | - | - |
| `3-B-short-match-track2.json` | 4.06 | 3.03 | 0.42 | 0.27 | 21.5 | 19.7 | +1.9 |
| `4-A2-off-idle.json` | 1.17 | 13.21 | 0.41 | 0.14 | - | - | - |
| `5-A2-off-track2.json` | 0.36 | 3.12 | 0.59 | 0.23 | 37.8 | 18.3 | +19.5 |
| `6-A2-off-track2-repeat.json` | 0.43 | 3.15 | 0.61 | 0.22 | 30.8 | 16.2 | +14.6 |
| `7-A-ws1-track2-repeat.json` | 0.46 | 3.21 | 0.45 | 0.23 | 28.4 | 15.7 | +12.7 |
| `8-B-short-match-track2-repeat.json` | 3.08 | 3.13 | 0.48 | 0.23 | 37.9 | 14.4 | +23.5 |
| `9-B-short-match-track2-repeat2.json` | 3.43 | 3.10 | 0.44 | 0.23 | 33.3 | 17.8 | +15.5 |
| `10-A2-off-track2-repeat2.json` | 0.48 | 3.13 | 0.44 | 0.24 | 22.9 | 15.6 | +7.3 |

## What it says

- **Discovery: at parity with the Garmin.** Idle, the short match reports
  12.2/s against the Garmin's 12.6/s in the same window. The image with it off
  reports 1.0-1.2/s. Each trainer channel is heard in every 4.5 s scan open
  (66/66), with medians of 0.04-0.12 s. With it off, they are heard in 57-64 of
  65 opens, with medians of 0.33-0.41 s.
- **With two tracked channels**, the untracked device 0x7B gets 1.5-2.0/s
  (Garmin 3.0-3.2/s, off 0.2/s). The total rates are not comparable here.
  The Garmin stops reporting a tracked device on its scan channel, and
  RadiANT keeps reporting one of them.
- **Tracked loss: not settled.** In the first room state (windows 1 and 3), A and
  B were both about 2 pp behind the Garmin. Later the room changed: the WS1
  image itself went from +2.1 to +12.7 pp, and the same A2 image read +19.5,
  +14.6 and +7.3 pp in three sittings. In that state, B read +23.5 and +15.5 pp.
  The mean (19.5 against 13.5 pp) suggests a cost of a few pp, but that is
  inside the off arm's own spread. So gate 3 (tracked loss within the baseline)
  is neither passed nor failed, and the Kconfig stays default n.
- **A pre-existing gap, not this work's.** In the later room state every
  RadiANT image lost 7-24 pp more than the Garmin on the trainer's channels.
  The trainer's three channels share device number 52233. A plausible
  mechanism for a short-match cost on top: search receptions of the sibling
  channels now happen constantly and may overlap the start of a tracked slot.
  Worth a diagnostic build before the default flips.

## Not measured

- `radiant_search_stats` (CRC failures, noise matches). They are not in the
  0xF6 reply, and adding them needs a layout decision about the conditional
  coverage field at [30..31].
- Host-visible events: the only channel events in any window were
  `EVENT_RX_FAIL_GO_TO_SEARCH` (0x08) on tracked channels, on both images. No
  new event type appeared.
- A device whose `devnum_lo` starts `010`, and so could slip in the wild byte.
  None was on the air.
