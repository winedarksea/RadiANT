# `archive/captures/serial/` — `.antser` host&harr;dongle byte traces

Checked by: nothing yet — the directory holds no captures, and
`tools/ant_trace.py` does not exist yet either. Once both do,
`tools/test_ant_golden.py` replays these in the CI `host-tests` job.

**Status: empty. Needs `tools/ant_trace.py` (Wave 1, `tools-new`) and a
session with a real host.**

Where [`../radio/`](../radio/) records what went over the air, this records
what went over USB: the `0xA4`-framed serial protocol, byte for byte, in both
directions. It is the layer `src/ant_serial_bridge.c` implements and
`protocol/ant_wire.yaml` describes, and it is the layer the checksum bug lived
in.

## Why this exists at all

Because a test written by the people who wrote the code cannot catch a mistake
in the *rule* both sides implement.

The parser seeded `running_xor = 0` instead of the `0xA4` SYNC byte it had just
consumed, so every checksum was off by exactly `0xA4` and every frame a real
host sent was dropped without a word. `tools/ant_probe.py` made the identical
mistake in `frame()`. Firmware and tools agreed perfectly with each other and
with nothing else in the world; the whole suite passed — probe, scan, eight
channels, real sensors, ack and burst — and Zwift listed the dongle and never
executed a single command.

A constant-for-constant cross-check between `src/ant_wire.h` and
`tools/ant_wire.py` **would have passed too**. Both sides had `SYNC = 0xA4` and
agreed. The bug was in a rule, not a constant.

What catches a rule is a third implementation neither side wrote. `ANT_DLL.dll`
exports `ANT_SetDebugLogDirectory` (ordinal 132 — see
[`../../host-api/`](../../host-api/)), and the `Device0.txt` it writes is
Garmin's own account of Garmin's own `Tx`/`Rx` bytes. Normalised, that becomes
the fixture set here.

## The format — proposed contract

`tools/ant_trace.py` is owned by the Wave 1 `tools-new` agent, not by this
archive. **The shape below is the contract this directory is specified
against**; if that agent chooses differently, this file is what should change
to match, and the capture set below still applies.

Line-oriented ASCII, mirroring `.antcap`'s design rules — one `sscanf` per
line, deliberately not JSON, `#` for comments, first line the header:

```
# ant serial capture v1: <seconds|-> <dir> <framed message, hex>
# tools/ant_trace.py from Device0.txt, Zwift 1.x, 2026-08-08
0.000000 > a4014a00ef
0.014200 < a4016f20fa
0.014850 > a4024d005400bf
```

| Field | Meaning |
|---|---|
| `<seconds>` | `%.6f` relative to the first record, **or the single character `-`** |
| `<dir>` | `>` host to dongle (`Tx`), `<` dongle to host (`Rx`) |
| `<hex>` | The **complete** frame: SYNC, length, message id, payload, checksum. Lowercase, no separators |

Three decisions in that, each with a reason:

**The whole frame, including SYNC and checksum.** A capture that stores the
decoded payload cannot catch a framing bug, which is the only class of bug this
file exists for. Store the bytes that were on the wire.

**Direction is a column, not two files.** Ordering across directions is the
thing under test — a reply that arrives before the command that caused it, or
an unsolicited event interleaved into a request/response pair, is invisible if
the two directions are recorded separately.

**The timestamp may be literally `-`, and for conformance runs it must be.**
Tier 1's acceptance criterion is that two `.antser` files are **byte-identical**
(`docs/testing.md`). Real timestamps never repeat, so a transcript carrying
them can never be diffed. `tools/ant_conformance.py` therefore writes `-` in
that column: a conformance transcript is a statement about bytes and ordering,
not about timing. `tools/ant_trace.py` writes real timestamps, because a trace
lifted from `Device0.txt` has them and the inter-frame timing is part of what
makes it worth keeping. Parsers accept both; a diff-based test simply requires
both sides to have been produced in the same mode.

### On `Device0.txt`'s own format

**Not documented here, because no sample was to hand when this was written and
guessing at it would be worse than saying so.** Absorbing whatever shape it
actually has is `tools/ant_trace.py`'s entire job, and the first thing that
agent should do is capture one and write the real line format into this
section. What is known from `docs/gotchas.md` is only that it contains Garmin's
`Tx`/`Rx` bytes in a form a human can XOR by hand.

## How one is produced

Two sources, and both belong here.

**1. From a real host, via `ANT_DLL.dll`.** Call `ANT_SetDebugLogDirectory`
before `ANT_Init`, run a session, then normalise:

```powershell
python tools\ant_trace.py <dir>\Device0.txt --out zwift-pairing.antser
```

This is the one that is worth the most and is the most perishable: it needs a
Windows machine, an installed ANT driver, `ANT_DLL.dll`, and an application
that drives a real session. Every one of those is a dependency on somebody
else's software continuing to exist.

**2. From our own conformance run**, which needs one board and no radio:

```powershell
python tools\ant_conformance.py --serial <suffix> --out sdk-ant.antser
python tools\ant_conformance.py --serial <suffix> --out core.antser
fc /b sdk-ant.antser core.antser     # byte-identical is the pass
```

## What to record — the bench session's shopping list

| File | Source | What it pins down |
|---|---|---|
| `zwift-pairing.antser` | `Device0.txt`, Zwift pairing a power meter, an HRM and a trainer | The message sequence a real fitness app opens with, in order, including `ANT_SetTransmitPower` (`0x47`) — the message whose absence stalled the search |
| `zwift-erg.antser` | `Device0.txt`, a ride with resistance changes | Acknowledged data in anger. This is how Zwift sets trainer resistance, and it is on the critical path for Spike B |
| `conformance-sdk-ant.antser` | `tools/ant_conformance.py` against the sdk-ant build | **The Tier 1 reference.** Every message `dispatch()` implements, valid and malformed, with the reply framing, the response codes, the error mapping and the leading-index-byte shapes |

The third one is the A/B baseline for the entire Tier 1 gate and is as
perishable as the performance numbers in [`../../benchmarks/`](../../benchmarks/):
it can only be recorded while a working sdk-ant build exists. Record it early.

Also worth capturing while a host is in the room, cheap and not otherwise
recoverable: the **capabilities reply** a genuine ANT USB-m returns, next to
ours. `docs/ant-serial-protocol.md` decodes our `080800b23200fd8d0f` bit by
bit; having the real stick's alongside is what would settle any future argument
about a capability bit.

## Where these end up

`tools/test_ant_golden.py`, in the CI `host-tests` job — the one job that needs
no secret and therefore runs on forks. Together with the golden frame vectors
in `tools/test_ant_wire.py`, these are the third implementation. They are the
reason `scripts/gen_ant_wire.py` generates constants rather than cross-checking
them: generation makes one number appear in three places, and only a foreign
transcript checks the rules.
