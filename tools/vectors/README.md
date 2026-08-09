# `tools/vectors/` — hand-assembled fixtures

Checked by: `tools/test_ant_golden.py` (CI `host-tests`), which replays every
`.antser` here on every run.

These are **not** captures. Nothing in this directory came off a wire, and
nothing in it was produced by running `tools/ant_wire.py`'s `frame()` and
freezing the output. Every frame was assembled by hand from the message tables
in [`../../docs/ant-serial-protocol.md`](../../docs/ant-serial-protocol.md),
with the XOR worked out term by term in a comment on the line above it, and the
arithmetic re-checked with a standalone XOR loop rather than with the function
under test.

## Why hand-assembled fixtures exist at all

`archive/captures/serial/` is where real captures go, and it is empty: a real
one needs a Windows host, `ANT_DLL.dll`, an application driving a session, and
a human. Until one exists, a replay harness pointed only at that directory
would skip on every run — and a test that never executes is indistinguishable
from a test that passes.

So the harness is exercised from day one against these, and a real capture
later *adds* coverage rather than switching the test on. That ordering matters:
if the harness itself is broken, the first real capture must not be the thing
that discovers it.

## What they may and may not be used for

They are worth exactly what a hand-derivation is worth. They check that the
framing rules compose — that `frame()` reproduces bytes typed from a table,
that a payload byte equal to `0xA4` does not split a run, that a checksum which
happens to equal `0xA4` is handled by length and not by eye. They are **not**
evidence about what a real host sends, in what order, or with what timing.
Nothing here should ever be cited as an observation.

## Files

| File | Timestamp column | What it covers |
|---|---|---|
| `handmade-session.antser` | `-` | A plausible session opening: reset, capabilities, network key, assign, lib config, open, one broadcast — plus one deliberately malformed frame |
| `handmade-timed.antser` | real | The same opening with a real timestamp column, so the harness exercises both forms. It is `archive/captures/serial/README.md`'s own worked example with its two checksum typos corrected |

## The `malformed` case convention

A `# case` name containing the word `malformed` declares that the records under
it are **expected to be rejected**. `tools/test_ant_golden.py` asserts the
opposite of its usual checks for them, which is what lets a transcript carry a
deliberately bad frame without either failing the suite or being waved through
unexamined.

This convention is introduced here because `.antser` needed one and had none.
`tools/ant_conformance.py` writes the case names for conformance transcripts,
so if it settles on a different word, this file and
`tools/test_ant_golden.py` are what should change to match.
