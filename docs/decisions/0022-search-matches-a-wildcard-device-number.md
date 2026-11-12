# 0022 — On nRF, the search can match every device number in one window: [A6 C5] in hardware, devnum_lo received as data

- **Status:** proposed. Built behind `CONFIG_RADIANT_SEARCH_SHORT_MATCH`
  (default n), ztest-verified, A/B/A on air below. It becomes the nRF default
  only when the gates in [Gates](#gates) pass.
- **Date:** 2026-11-11
- **Supersedes:** the "search format is permanent by design and is not touched"
  note in `radiant/src/radiant_addr_split.h` (ADR 0019's scope line), and the
  `RADIANT_SEARCH_ENOTSUP` refusal of `caps.filter_wildcard_dev` in
  `radiant_search_init()`.
- **Evidence:** `archive/benchmarks/2026-11-11-scan-rate/` (baselines and the
  A/B/A, all `tools/ant_scan_rate.py --json`).
- **Code:** `radiant_rx_filter.wild_last` and `radiant_rx_event.wild_value`
  (`radiant/include/radiant/radiant_radio_hal.h`),
  `radiant/src/radiant_addr_split.[ch]`, `radiant/src/radiant_radio_nrf.c`
  (caps, the END branch of the ISR), `radiant/src/radiant_search.c`
  (`init`, `build_set`, `handle_ok`, `set_for_devnum`).

> **Provenance.** Clean-room. Written from this project's source and its own
> bench measurements. Nothing here derives from sdk-ant, `libant.a` or any
> ANT+ profile document. See [0002](0002-clean-room-policy.md).

---

## Context

Two Zwift USB captures on 2026-11-11, one per dongle, same room and sensors:
RadiANT's background scan reported about 0.3 sightings/s across five sensors;
an unmodified Garmin stick reported 2.6-4.9/s and listed every device within
0.25 s of opening its scan.

The cause is structural. The search matches the 3-byte address
`[A6 C5 devnum_lo]` with the RADIO's 8 logical addresses, so it steps through
32 sets of 260 ms. A given device is in the set being listened to once every
8.3 s, whatever its signal.

ADR 0019 already built the opposite: a TRACKING window that matches only
`[A6 C5]` in hardware, receives the remaining address bytes as the first
payload bytes, compares them in software, and adds their airtime to `t_sync`.
The bit stream the RADIO clocks in, and the hardware CRC over it, are
unchanged.

## Decision

For a search window, match `[A6 C5]` in hardware and take `devnum_lo` from the
first received byte. The sweep is then ONE set and every dwell hears every
device number.

- **HAL.** `radiant_rx_filter.wild_last` asks a backend to accept any value in
  the last address byte; `radiant_rx_event.wild_value` reports the byte it
  received. A backend opts in with `caps.filter_wildcard_dev`, which the nRF
  backend sets under `CONFIG_RADIANT_SEARCH_SHORT_MATCH`. No other backend sets
  it, and the fake radio honours `wild_last` only when its caps claim it.
- **`radiant_addr_split`.** A window carrying a `wild_last` filter matches
  `addr_len - 1` bytes in hardware on 1 M (never fewer than 2), the compare
  skips the wild byte, and a wild filter is never slip-repaired (there is no
  expected byte to compare a slipped one against).
- **Search module.** With the cap, `n_sets = 1` with a single filter;
  `handle_ok()` builds the address from `evt->wild_value`; the set lookup
  returns 0. Steer, revisit and retune need no change: the steer queue only
  ever holds set 0, and a revisit of set 0 is the next dwell anyway.
- **The guarantee.** "Certain within one sweep" gets stronger: the sweep is one
  dwell, and a dwell (260 ms) is at least one period of the slowest profile.

### Why the CRC makes this safe

The hardware CRC still covers the whole on-air address, including the
`devnum_lo` byte the hardware no longer matches. A frame is accepted only
when it verifies, so a corrupted `devnum_lo` cannot be reported as a device.
A CRC-failed frame is counted (`radiant_search_stats.crc_fail`) and dropped,
as before. ztest `test_wildcard_crc_failure_is_not_a_device` pins it.

### The cost

The one ADR 0019 names. A 2-byte match also matches noise, so a search window
locks onto more false starts, each costing the airtime of one frame. In a
search window that only delays the next real frame; in a tracking window it
would cost a frame, which is why tracking windows are untouched.

## Gates

To become the nRF default, against a same-window Garmin reference:

1. Each 4 Hz sensor at least 1 sighting/s (baseline about 0.1-0.35/s).
2. Median first sighting after an open under 0.5 s.
3. Tracked loss within the baseline's Wilson interval.
4. Noise matches and CRC failures counted, with no new host-visible events.

Then a Zwift capture: every sensor listed within about 1 s of the pairing
screen opening.

## Result

The A/B/A on 2026-11-11 is in `archive/benchmarks/2026-11-11-scan-rate/`
(eleven 300 s windows, Pro Micro against a Garmin stick in each).

| Gate | Result |
|---|---|
| 1. Each 4 Hz sensor ≥ 1 sighting/s | **Pass.** Idle 3.0-6.1/s per trainer channel (Garmin 3.1-6.3/s); with two tracked channels the untracked one gets 1.5-2.0/s. Off: 0.2-0.6/s. |
| 2. Median first sighting after open < 0.5 s | **Pass.** Idle 0.04-0.12 s, with every device heard in 66/66 opens; tracked 0.34-0.48 s. Off: heard in 57-64 of 65 opens. |
| 3. Tracked loss within the baseline | **Not settled.** Equal to the off image in the first room state (+1.9 against +2.1 pp behind the Garmin). After the room changed, +23.5 and +15.5 pp against +19.5, +14.6, +12.7 and +7.3 pp for the off images. Possibly a cost of a few pp, inside the room's own swing. |
| 4. Noise counted, no new host events | **Partly.** No new host-visible event type. The search's CRC and noise counters are not exported (see the benchmark README). |

So the default stays **n**. Before it flips, gate 3 needs settling with a
diagnostic build. The question is whether a search reception on a sibling
channel of the same device delays a tracked slot.

## Not done here

- **TI** (`nSwBits=16`, 128 sets today) is a follow-up. The CC26xx backend
  does not set the cap, so it is build-only and unchanged.
- **Slipped `devnum_lo`.** A device whose `devnum_lo` starts `010` extends the
  alternating run of `C5` past the hardware match boundary, so its frames can
  arrive with one bit deleted in the wild byte. The repair cannot fix those (no
  expected byte), so they fail CRC. If the A/B shows such devices
  under-reported, a bounded one-bit reinsertion trial over the first ~8 bits
  after `C5`, accepted only when the CRC verifies, can be built on
  `radiant_addr_split_repair()`.
- **The two small waste fixes** proposed with this (no seen-cache steer on a
  wildcard SCAN `begin`; credit an in-flight chunk on close) were not made.
  The first is not waste: a SCAN steer produces a report of the last-seen
  device. The second can't tell, under an arbiter, a granted window from one
  still waiting, so it would reopen the over-credit trap
  `CONFIG_RADIANT_SEARCH_DENIAL_ESCAPE` documents. With one set, neither
  matters on nRF; both matter on TI's 128 sets.
