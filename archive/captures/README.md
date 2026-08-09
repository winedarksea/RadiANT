# `archive/captures/` — our own recordings of our own device

Checked by: nothing yet — treat as narrative. Once captures exist,
`tools/test_ant_golden.py` replays the `.antser` files and the replay tests in
`tools/` and in `zephyr_aerosense` consume the `.antcap` files, and this
directory becomes the most heavily checked thing in `archive/`.

Two capture formats, two layers, one purpose.

| Directory | Layer | Format | Made by | Replayed by |
|---|---|---|---|---|
| [`radio/`](radio/) | ANT+ packets off the air | `.antcap` | `tools/ant_verify.py --record` | `tools/ant_verify.py --replay` |
| [`serial/`](serial/) | Host&harr;dongle bytes over USB | `.antser` | `tools/ant_trace.py`, `tools/ant_conformance.py` | `tools/test_ant_golden.py` |

**These are the highest-value and least complicated artifacts in the whole
archive.** They are recordings of our own hardware, made with our own tools, on
our own bench. No third party has an interest in them, no licence question
arises, and nothing needs to be argued before committing them.

They are also the only thing that makes the rebuild *verifiable*. Everything
else in `archive/` preserves the ability to understand ANT. These preserve the
ability to prove `ant_core` still behaves like the thing it replaces, on a
laptop, in CI, years after the sensor that produced them was returned to a
drawer.

## Bench need — declared

**Neither directory can be filled without a human at the bench, and this agent
did not attempt it.** There is one Feather, every flash needs somebody to
double-tap RESET, and a real capture needs a real transmitter. No flash was
performed and no board was touched.

What each directory needs is specified exactly, in its own `README.md`, so the
bench session that fills them is spent measuring rather than deciding on a
format under time pressure. Read those before booking the bench.

## Why a synthetic capture will not do

`tools/ant_sim.py --dry-run --record run.antcap` produces a well-formed
`.antcap` with no hardware at all, and it is genuinely useful — for testing
`ant_verify.py`'s analysis. It is **not** a golden fixture and must not be
committed as one.

A dry-run capture is our encoder's output fed to our decoder. If both share a
misconception they agree perfectly and the test passes. That is precisely the
failure the checksum bug was: firmware and tools both seeded the running XOR
at `0` instead of `0xA4`, every green test was two wrong implementations
shaking hands, and it hid for a week.

A committed capture must come from a transmitter neither our decoder nor our
encoder wrote:

- for `.antcap`, a **real ANT+ sensor**, or `sim/` running Garmin's certified
  profile code on the nRF54L15 DK;
- for `.antser`, **`ANT_DLL.dll`'s own `Device0.txt`**, which is Garmin's
  account of Garmin's bytes.

That is the whole point of the directory. Anything else is a fixture, belongs
in `tools/`, and should say so.
