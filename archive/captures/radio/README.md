# `archive/captures/radio/` — `.antcap` ANT+ packet captures

Checked by: nothing yet for the `.antcap` replay path. Once the shopping list
below is filled, the replay tests in `tools/test_*.py` and in
`zephyr_aerosense`'s host tests are what fail when a decoder drifts. The two
`.log` files are checked by nothing and are not meant to be — they are raw
bench recordings, and `docs/spike-a-results.md` is what reads them.

**Status: partially filled.**

| File | What it is |
|---|---|
| `2026-08-09-sdk-ant-power-300s.antcap`, `...-run2.antcap` | The two 300 s power runs behind the Phase 4 sdk-ant baselines in `archive/benchmarks/2026-08-09-sdk-ant.json`. Not part of the shopping list below — that set is 900 s per profile and is still outstanding |
| `2026-08-09-nrf54l15-run1.log`, `...-run2.log` | Spike A's raw serial logs — see *Spike A's bench logs* below |

**The four-profile `.antcap` set still needs a bench session — see *What to
record* below.**

## Spike A's bench logs

`2026-08-09-nrf54l15-run1.log` and `2026-08-09-nrf54l15-run2.log` are the COM7
serial output of `ant_core/spike/rx_raw` running on an nRF54L15 DK on
2026-08-09, with a Feather driven as an ANT+ master by `tools/ant_sim.py`. They
are the evidence behind `docs/spike-a-results.md` and behind every `[measured]`
tag in `docs/ant-radio-link.md`. **If those documents and these logs disagree,
the logs are right.**

They are not `.antcap` and nothing replays them: the format is Zephyr log lines,
one `[cfg]` line per configuration tried and one `[res]` line per result, with
per-packet `PKT` / `raw18` detail. Each file opens with a provenance header
saying which firmware revision produced it — run 1's phase C and E sweeps varied
the wrong axis, which is why run 2 exists, and both are kept because that
failure is part of the record.

They live here rather than beside the spike for the reason this directory
exists: they are our own recordings of our own hardware, the safest and
highest-value artifact class in `archive/` (`docs/preservation.md`), and the
spike itself is a throwaway application that a later cleanup may well delete.
Together they are about 53 KB.

## The format

Line-oriented ASCII, one packet per line, defined by `format_capture_line()` /
`parse_capture_line()` in [`tools/ant_pages.py`](../../../tools/ant_pages.py).
The first line is the header, `#` lines are comments:

```
# ant capture v1: <seconds> <device_type> <device_number> <8 payload bytes>
# ant_verify.py, 900 s, profiles: power, csc
# expected 100 W, 80 rpm
12.345678 0B 3A17 1006ff5039300064
12.595901 79 41C2 0100000012340a5b
```

| Field | Meaning |
|---|---|
| `<seconds>` | Arrival time, `%.6f`, relative to the start of the run |
| `<device_type>` | ANT+ device type, two hex digits. `0B` power, `79` speed-and-cadence |
| `<device_number>` | Four hex digits |
| `<payload>` | The 8 data bytes, hex, no separators |

**It is deliberately not JSON**, and the reason is written into the source: the
same captures are replayed through the C decoders in `zephyr_aerosense`'s host
tests, where a line of this shape is one `sscanf` and JSON is a parser
dependency.

**The device number is not decoration.** Two sensors can share a device type —
a power meter emitting standard pages and one emitting torque pages are both
`0x0B` — and merging their streams produces one analysis of two unrelated
series. The field is what lets `ant_verify.py` split a capture back into
per-sensor streams.

**A capture carries no channel numbers**, on purpose. Which channel a packet
arrived on is a fact about the receiver, not about the sensor. What it does
carry is device type and pages, and `profile_for()` names the profile from
those alone — which is what lets one analysis judge `ant_sim.py` and `sim/`
firmware alike.

## How one is produced

```powershell
python tools\ant_verify.py --seconds 900 --record power.antcap `
    --expect-watts 100 --expect-rpm 80 --serial <suffix>
```

`--serial` is not optional in practice: two boards here enumerate as the same
`0FCF:1009`, and the tools refuse to guess.

## How one is replayed

No hardware, no board, no radio:

```powershell
python tools\ant_verify.py --replay power.antcap --expect-watts 100 --expect-rpm 80
```

Replay runs the identical analysis path as a live measurement — loss, exact
loss from the transmitter's own event counter, accumulator continuity, page
mix, common-page interval, decoded power and cadence accuracy. Two things it
cannot report, because a capture does not contain them: RSSI and per-channel
identity, which are facts about the receiver.

## What to record — the bench session's shopping list

**One `.antcap` per profile, ~150 KB each, `--seconds 900`.** That length is
chosen, not rounded: 900 s at ~4 Hz is roughly 3,600 packets, which is about
150 KB of text, and it is long enough to get **all** the 16-bit accumulators
through a wrap. A 60 s run wraps none of the slow ones, and a receiver that
widens before subtracting is correct right up until they roll over.

Transmitter: **`sim/` on the nRF54L15 DK**, which runs Garmin's certified
profile code, or a real sensor. Not `ant_sim.py --dry-run` — see
[`../README.md`](../README.md).

| File | Profile | Device type | Period | Notes |
|---|---|---|---|---|
| `power-standard.antcap` | `power` | `0x0B` | 8182 | Standard power page `0x10` |
| `power-torque.antcap` | `power-torque` | `0x0B` | 8182 | Wheel `0x11` and crank `0x12` torque pages |
| `power-torque-freq.antcap` | `power-torque-freq` | `0x0B` | 8182 | Torque frequency page `0x20` |
| `csc.antcap` | `csc` | `0x79` | 8086 | Combined speed and cadence — the hardest to get right, and not the `sim/` default |

Those four are the profiles [`tools/ant_pages.py`](../../../tools/ant_pages.py)
models today, and `sim/` builds all four in CI precisely so three of them are
not left uncompiled. Budget: about 600 KB, comfortably inside the 10 MB
`archive/` limit.

Record each with `--seed` set and the seed written into the capture's comment
header. The noise is pseudorandom, and a measurement that cannot be replayed
is not a measurement.

**A second set, once the tools model them:** heart rate (`0x78`) and fitness
equipment (`0x11`) from real hardware. These are the two profiles Zwift
actually depends on beyond power, `0x11` is how ERG mode sets resistance, and
neither is simulated today. Capture them from a strap and a trainer when both
are in the room; do not block the first four on it.

**A third, later:** the Tier 4 extension-interop capture — a RadiANT telemetry
node transmitting alongside real ANT+ sensors, proving a stock receiver
neither errors nor reports a phantom device. That one belongs to Phase 7 and
is listed here so it lands in the same directory rather than somewhere new.

## Where these end up

- `tools/ant_verify.py --replay`, as above.
- The replay tests in `tools/test_*.py`, which run in the CI `host-tests` job
  with no board and no secret.
- **`zephyr_aerosense`'s ANT decoder tests.** Three of that project's five test
  fixtures were recorded from this bench already. `tools/ant_pages.py` is the
  shared contract between the two projects, which is why the format is one
  `sscanf` on the C side.

A byte-order mistake found on the host costs nothing. The same mistake found on
the bench costs two boards and a flash cycle. That is the entire argument for
this directory.
