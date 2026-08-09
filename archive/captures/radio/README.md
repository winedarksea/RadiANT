# `archive/captures/radio/` — `.antcap` ANT+ packet captures

Checked by: nothing yet for the `.antcap` replay path. Once the shopping list
below is filled, the replay tests in `tools/test_*.py` and in
`zephyr_aerosense`'s host tests are what fail when a decoder drifts. The
thirteen `.log` files are checked by nothing *except* the four Spike B part 2
ones, which are checked by

```powershell
C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe radiant_core\spike\promisc\spike_b_analyse.py `
    archive\captures\radio\2026-08-09-spike-b2-run0-pacing-bug.log `
    archive\captures\radio\2026-08-09-spike-b2-runA-burst-seq.log `
    archive\captures\radio\2026-08-09-spike-b2-runB-burst-seq-advburst.log `
    archive\captures\radio\2026-08-09-spike-b2-runC-master-control.log --strict
```

which exits non-zero if any of them is incomplete. The rest are raw bench
recordings and are not meant to be checked — `docs/spike-a-results.md`,
`docs/spike-b-results.md` and `docs/spike-b-part2-results.md` are what read
them.

**A note on `ant_core/` in the prose below.** On 2026-08-09 the module was
renamed `ant_core` → `radiant_core` and moved to Zephyr module shape; the
capture programs now live at `radiant_core/spike/rx_raw` and
`radiant_core/spike/promisc`. The provenance sentences further down still name
`ant_core/spike/...` deliberately, because that is the path that existed when
these logs were recorded, and the `.log` files' own headers say the same.
Rewriting a record to match today's tree is how a preservation directory stops
being evidence. Only the runnable command above was updated, because a command
that does not run is not a record of anything.

**Status: partially filled.**

| File | What it is |
|---|---|
| `2026-08-09-sdk-ant-power-300s.antcap`, `...-run2.antcap` | The two 300 s power runs behind the Phase 4 sdk-ant baselines in `archive/benchmarks/2026-08-09-sdk-ant.json`. Not part of the shopping list below — that set is 900 s per profile and is still outstanding |
| `2026-08-09-nrf54l15-run1.log`, `...-run2.log` | Spike A's raw serial logs — see *Spike A's bench logs* below |
| `2026-08-09-spike-b-nrf54l15-run1.log`, `...-run2.log`, `...-run6.log` | Spike B's three plain capture runs — see *Spike B's bench logs* below |
| `2026-08-09-spike-b-nrf54l15-run3-bursts.log` | Spike B, the run that provoked bursts and acknowledged messages |
| `2026-08-09-spike-b-nrf54l15-run4-burst-lengths.log` | Spike B, bursts of 2, 3 and 12 blocks, and advanced burst at 24 bytes |
| `2026-08-09-spike-b-nrf54l15-run5-timed.log` | Spike B, the host-timestamped run behind the open-time cross-check |
| `2026-08-09-spike-b-nrf54l15-config.log` | Spike B's configuration banner — which register words were actually programmed, and the eight-prefix slot map |
| `2026-08-09-spike-b2-run0-pacing-bug.log` | Spike B **part 2**, the run that failed — and the only one that shows what a receiver does when a burst stops. See *Spike B part 2's bench logs* below |
| `2026-08-09-spike-b2-runA-burst-seq.log` | Part 2, the burst sequence walk: bursts of 1, 2, 3, 9 and 17 packets, complete |
| `2026-08-09-spike-b2-runB-burst-seq-advburst.log` | Part 2, the same plus advanced burst — bursts up to 51 on-air packets |
| `2026-08-09-spike-b2-runC-master-control.log` | Part 2, the master/slave control: the same board, script and payload with only the channel role changed |

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

## Spike B's bench logs

The seven `2026-08-09-spike-b-nrf54l15-*.log` files are the COM7 serial output
of `ant_core/spike/promisc` running on an nRF54L15 DK on 2026-08-09, with the
same Feather — already carrying the shipping dongle firmware — driven as an
ANT+ master by `spike_b_drive.py`. Zero Feather flashes were used. They are the
evidence behind `docs/spike-b-results.md`. **If that document and these logs
disagree, the logs are right.**

They are what refuted this project's reading of byte 3 of the frame. Six
capture runs, 750 CRC-valid frames, and byte 3 taking three values — `0x0A`
broadcast, `0xAA` acknowledged, `0x8A` burst — while the payload stayed eight
bytes. `docs/ant-radio-link.md` had that byte down as a length.

The same caveat as Spike A's logs applies and is the reason all seven are kept
rather than the best one: the runs differ in what the driver provoked, not in
firmware quality, and the burst runs are the ones that failed to reach their
pass criterion. `...-config.log` is separate from the runs because it is the
record of which register words were programmed, including the eight-prefix slot
map — the real `devnum_lo` at slot 5, seven decoys — that makes the `RXMATCH`
result mean something.

They are not `.antcap` and **nothing replays them**: the format is the spike's
own line-oriented output, one `P` line per frame with timestamp, `RXMATCH`,
RSSI, channel ID, byte 3, payload and CRC, and one `S` line per summary
interval carrying a histogram of byte 3 over CRC-valid frames. Each file opens
with a provenance header giving the rig, the device under test and the line
format.

Together they are about 98 KB.

## Spike B part 2's bench logs

The four `2026-08-09-spike-b2-*.log` files are the output of the same
`ant_core/spike/promisc` capture program, this time on the **nRF5340 DK's
network core**, with three radios on the air at once: an nRF54L15 DK running
`sim/` as the master, the same never-reflashed Feather as the slave, and the
nRF5340 sniffing. They are the evidence behind `docs/spike-b-part2-results.md`.
**If that document and these logs disagree, the logs are right** — and they do
disagree in one small place, noted below.

They are what turned byte 3 from three values into eleven, and from a
type-and-length reading into six independent fields. `run0` is kept although it
failed: the driver was pacing burst packets on an event the serial bridge
consumes, so every multi-packet burst died after packet 0 — and in doing so it
recorded the only measurement of a receiver retransmitting its acknowledgement,
21 times at 3143 µs, which no successful run can show.

The line format adds two columns to part 1's: `dr=` is the sniffer's ring-drop
counter as it stood when that record was made, and `q=` its occupancy.
`spike_b_analyse.py --strict` fails the run if `dr` ever increments, which is
what makes a *sequence* result usable where part 1 only needed a histogram to
survive a lost frame. `dr = 0` on every line of all four.

Two things worth knowing before quoting these logs:

- **`B3=0x0C`, `0x0E` and `0x1A` appear, and they are bit errors, not control
  bytes.** Every one is on a line the analyser prints as CRC-failed, and each is
  one or two bits away from `0x0A`. In particular the two `0x1A` lines are *not*
  the withdrawn advanced-burst prediction.
- **The data → acknowledgement relationship holds on 168 adjacent CRC-valid
  pairs, not the 165 part 2 states**, and there are three further data packets
  whose acknowledgement failed CRC at the sniffer — visible as a doubled
  ~3.08 ms gap where ~1.55 ms is expected. No counterexample exists in either
  count.

Together they are about 292 KB, which puts `archive/` at **2.72 MB** against
its 10 MB budget.

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
`archive/` limit — `archive/` stands at **2.72 MB** with both parts of Spike B's
logs in it (2.43 MB before part 2 added 292 KB), so the four `.antcap` files
land it near 3.3 MB.

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
