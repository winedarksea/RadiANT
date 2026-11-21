# 0022 — On nRF, the search can match every device number in one window: [A6 C5] in hardware, devnum_lo received as data

- **Status:** accepted 2026-11-20 on the
  [Zwift captures](#zwift-acceptance-2026-11-20). Built behind
  `CONFIG_RADIANT_SEARCH_SHORT_MATCH`, ztest-verified, A/B/A on air below.
  Default **y on cc26xx** since 2026-11-12 (the TI A/B/A passed all four
  gates). Default **y on nRF** since 2026-11-13: its four gates passed on
  2026-11-12 ([Gate 3 on nRF](#gate-3-on-nrf-2026-11-12)), and on every nRF
  image (MPSL, Matter, nRF5340 net core, nRF54L15) by the owner's decision of
  2026-11-14. `apps/dongle/long_match.conf` builds the 24-bit nRF image (no CI
  job, by decision).
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
  (`init`, `build_set`, `handle_ok`, `set_for_devnum`); on TI,
  `radiant/src/radiant_radio_cc26xx.c` (`rx_match()`, `rx_drain()`, caps).

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
  backend sets under `CONFIG_RADIANT_SEARCH_SHORT_MATCH`, and so does the
  cc26xx backend (see "On TI" below). No other backend sets it, and the fake
  radio honours `wild_last` only when its caps claim it.
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

### Gate 3 on nRF, 2026-11-12

Six alternating 300 s windows with 0xF7 page 7 (ADR 0023), the Pro Micro on A
(off) or B (on) against an nRF52840 Dongle on A in every window as the
same-SoC control (`archive/benchmarks/2026-11-12-gate3/`).

- Tracked loss: B - A = **-0.23 pp** against the control, bound 2.16 pp.
  No measurable cost.
- Page 7: no tracked window opened late and none expired behind a scan
  chunk on either image, so the suspected mechanism (search END interrupt
  work overrunning the dispatch slack) was not seen.
- Tracked slots opening just after the search heard the trainer's device
  number were all missed, about 12 per window on B and 2 on A. This was first
  read as a possible mechanism. On review it is not evidence either way. The
  counter only counts when the search hears a frame, which B does nine times
  as often, so it grows with discovery whatever made the slot miss. It also
  read 100 % missed on A, where the short match is off. Splitting it by
  channel ID would not remove that bias, so the split was not built and page
  7 fields 6-7 were reserved (ADR 0023). Fields 3-5 measure the mechanism
  directly, and they stayed at zero.

**Gate 3 on nRF: pass.** The same-window loss comparison is the gate's own
measure, and it shows no cost.

**Gate 4 on nRF: pass**, from the same run's 0xF7 pages 4-5 (Pro Micro, per
300 s window). The backend's CRC failures, which are the noise matches,
were 2925-2995 on B against 142-150 on A, about 10/s more. All were dropped
before the search: the search's own `crc_fail` read 0 on both images. Every
device sighted on B was also sighted by the dongle and the Garmin (#52233
types 0x0B/0x7B, and #1458 in window 6), so no noise frame got past the CRC
as a false device. The host saw the same event types on both images (channel
event 0x08 only, 2-3 per window).

All four gates now pass on nRF. The default stayed **n** at first, waiting
on the Zwift capture listed under [Gates](#gates). On 2026-11-13 it flipped to
**y** ahead of that capture, which became the acceptance check instead of a
precondition (`apps/dongle/long_match.conf` is the fallback image).

**Scope, decided 2026-11-14 by the project owner.** The default is **y on every
nRF image**, not only the USB dongle the gates ran on: the MPSL builds
(`gate.conf`, Thread, Matter), the nRF5340 network-core endpoint and the
nRF54L15 images included, although none of those had a gate run of its own.
No CI job builds `long_match.conf`; the owner declined one, so the fallback is
built by hand when needed.

### On TI (CC2652R), 2026-11-12

Same decision on `radiant_radio_cc26xx.c` (c911f31). `CC26XX_HW_ADDR_LEN`
(2 with the symbol, 3 without) drives `formatConf.nSwBits` (16 or 24),
`caps.addr_len_hw_max` and `caps.filter_wildcard_dev`. `rx_match()` skips a
`wild_last` filter's last byte; `rx_drain()` sets `evt.wild_value` and feeds
the received byte into the **software** CRC. This backend folds the CRC in
software, so without that every wildcard frame would fail it. A `wild_last`
filter whose last byte would fall inside the hardware prefix is refused
`ENOTSUP`, and the decoy sync word becomes `0x59C5` (removed 2026-11-13: a
one-group window now sets `syncWord1 = 0`, see ADR 0023's page 4 note). native_sim twister at
c911f31: 14/14 scenarios, 4463/4463 cases, `check_test_counts` OK.

**The 16 bits apply to tracking as well.** `nSwBits` is consumed by
`RF_open()` and cannot change per window, so the sync word is `[A6 C5]` for
every operation, tracking windows included; their remaining three address bytes
are compared in software (two before). Unlike nRF, where tracking windows are
untouched, a TI tracked channel pays the noise cost too. That is why gate 3
below matters more here.

Bench: `ant_scan_rate.py --port COM11 --baud 57600 --serial 006 --seconds 300
--reopen 4.5 --track 52233:0x11`, the TI and Garmin USB-m `006` in the same
window, against trainer #52233. Images carried the 0xF6 health counters and
were built with immediate logging, both arms alike (see the benchmark README).
The false-sync counters were read through the bench-only `diag_health.patch`,
mod 65536.

| window | TI /s | TI median 1st 0x0B / 0x7B | TI tracked loss | Garmin /s | Garmin tracked loss | TI - Garmin | sync frames / sw mismatches |
|---|---|---|---|---|---|---|---|
| A1 24-bit (128 sets) | 0.52 | 0.84 / 0.91 s | 34.94 % | 4.82 | 53.80 % | -18.9 pp | 1110 / 42 |
| B 16-bit (1 set) | 4.45 | 0.30 / 0.39 s | 23.07 % | 4.58 | 58.44 % | -35.4 pp | 3149 / 14 |
| A2 24-bit | 0.52 | 0.59 / 0.74 s | 26.30 % | 6.78 | 38.65 % | -12.4 pp | 1196 / 31 |

Per device in B: 0x0B 2.98/s and 0x7B 1.47/s (Garmin 3.04 and 1.54). Host
events: B none; A1 `ch1:0x08` x2, A2 x1.

| Gate | Result on TI |
|---|---|
| 1. Each 4 Hz sensor >= 1 sighting/s | **Pass.** 2.98 and 1.47/s. Off: 0.52/s in total. |
| 2. Median first sighting < 0.5 s | **Pass.** 0.30 and 0.39 s. Off: 0.59-0.91 s. |
| 3. Tracked loss within the baseline | **Pass.** 23.07 % against 34.94 % and 26.30 %; against the same-window Garmin, -35.4 pp against -18.9 and -12.4. The two A windows differ by 8.6 pp between themselves, so read this as "no worse", not "better". The Garmin's own 39-58 % is its scan channel colliding with its tracked one. |
| 4. Noise counted, no new host events | **Pass.** 14 software-address mismatches against 42 and 31, and no host event in B. |

Not run: `tools/ab_gates.toml`'s acquisition gate (needs `ant_verify` T1
sittings). Raw results: `archive/benchmarks/2026-11-12-ti-wildcard/`.

**Decision.** The default is **y on cc26xx** (5875bd9). nRF stays **n** until
its own gate 3 is settled. `apps/dongle_ti/ti_long_match.conf` builds the
24-bit A image.

## Zwift acceptance, 2026-11-20

Gate: every sensor listed within about 1 s of the pairing screen opening.
Read from five Zwift USB captures with short match on (`.config` checked:
`build/zwift_3b486c3/*.config`, all `CONFIG_RADIANT_SEARCH_SHORT_MATCH=y`)
and the two 2026-11-11 references, decoded with `tools/decode_pcap.py`. A
"listing" is the first frame per device number and type on Zwift's
background scan channel (channel 0, network 0) after its `OPEN_CHANNEL`.
Same room and sensors throughout: power #20329, HR #1458 (types 0x78, 0x7C),
Kicker #52233 (0x0B, 0x11, 0x7B), #41999 (0x22).

| Capture | Dongle | Cold open (nothing tracked): device types listed, by | Open with 3 channels tracked: untracked types listed at |
|---|---|---|---|
| `zwift-20261111-193225` | Pro Micro, short match **off** | 2 of 7 in 2.6 s | 0-1 per 4.6 s open, first sightings up to 4.0 s |
| `zwift-20261111-193805` | Garmin USB-m (reference) | 5 of 5 seen at 0.24 s | steady devices 0.02-0.67 s; #52233 types often 1-3.8 s |
| `zwift-20261115-215313` | Pro Micro | 7 of 7 at 0.24 s | 0.27-0.48 s |
| `zwift-20261115-215820` | Pro Micro | 7 of 7 at 0.17 s | 0.15-0.54 s (two opens) |
| `zwift-20261117-192749` | Pro Micro (`acdd9d2`) | 7 of 7 at 0.66 s | 0.09-0.50 s |
| `zwift-20261115-214118` | TI over the shim | 7 of 7 at 0.21 s | 0.14-0.39 s (two opens) |
| `zwift-20261115-214722` | nRF52840 Dongle, MPSL gate | 5 of 5 seen at 0.29 s | **fail:** 3 frames in 3.66 s (0.13, 0.45, 3.47 s), #52233 0x0B never |

A tracked sensor's own frames go to its pinned channel, not the scan, on
every dongle including the Garmin, so the tracked column counts only the
untracked device types. Zwift lists a paired sensor anyway.

The other two acceptance points come from the same captures: 0x61 and 0xC6
are answered within 4 ms of the reset on every RadiANT capture. Reset to the
first `NETWORK_KEY` takes 11.0-15.1 s, against 42-94 s before the serial
replies and 9.2 s on the Garmin. One exception, `192749`, takes 51.6 s, but
the dongle had answered everything by 4 ms and the gap is host silence.

**Verdict: accepted** for the nRF direct backend (the USB dongle the gates
ran on) and TI. Cold-open discovery is at Garmin parity or better, and with
three channels tracked it is better than the Garmin.

**The MPSL row is not this ADR's defect.** With three pinned channels the
MPSL gate's scan heard almost nothing until Zwift closed them at 275.46 s, and
then it heard every sensor within 1 s. It is one open (n = 1), on the stick
whose Kicker link was 8 dB weak (TODO "From the 2026-11-15 Zwift captures",
item 4). The cause is scan airtime under tracking (ADR 0018 and 0021, the
arm lead), which a 24-bit match would make worse: a dwell would hear 1 of 32
device-number sets. Tracked separately in TODO.md.

## Not done here

- **TI** is done: see "On TI" above.
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
  `CONFIG_RADIANT_SEARCH_DENIAL_ESCAPE` documents. With one set neither
  matters, and TI has one set by default since 2026-11-12, so the item is
  **closed for TI**. nRF followed on 2026-11-13, so it is closed there too; it
  matters only for a `long_match.conf` image (32 sets). No scheduler "window
  is live" query is built.
