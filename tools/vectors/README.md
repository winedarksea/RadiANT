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

`archive/captures/serial/` is where real captures go, and for a long time it
was empty: a real one needs a Windows host, `ANT_DLL.dll` or a bench sitting,
an application driving a session, and a human. A replay harness pointed only at
that directory would have skipped on every run — and a test that never executes
is indistinguishable from a test that passes.

So the harness was exercised from day one against these, and the first real
capture *added* coverage rather than switching the test on. That ordering
mattered, and it is worth recording how: `conformance-sdk-ant.antser` landed and
turned the suite red in five places, **every one of them a rule this harness had
stated too broadly** — never a defect in the firmware, whose bytes reproduced
identically across two runs. Had the harness been switched on by that same file,
there would have been no way to tell which side was wrong.

## Two conventions the transcript corrected

Both are the same mistake in different clothes: a rule true of one thing,
applied to everything shaped vaguely like it.

**A message id is not a message.** `MESG_REQUEST` (`0x4D`) names an id and the
frame that comes back carries that id with a payload no host ever sends. So
`protocol/ant_wire.yaml` now records `payload_len` (the command, `h2d`) and
`reply_len` (the request reply, `d2h`) separately, and
[`../test_ant_golden.py`](../test_ant_golden.py) picks between them by the
record's direction column. `0x78` takes a 9..12-byte command and answers a
request with 4 bytes; `0x7D` takes a 4-byte command and answers with 2, 5 or 20
by info type. Each reply shape *names* a constant in `message_sizes` instead of
restating a length, so the number checked here is the number the bridge writes.

The route deliberately not taken: widening `payload_len` to the union of both.
It goes green in one edit and permanently admits a 4-byte `0x78` command. **The
harness's failure mode is permissiveness**, so anything loosened has to be
pinned by a test that goes red if it is loosened further —
`TestTheDirectionDependentLengthModel` is that pin, in the shape
`TestTheMalformedConvention` established.

**The five-bit channel ceiling belongs to the burst header and to nothing
else.** Byte 0 of `MESG_BURST_DATA` (and `_ADV_BURST_DATA`, and the legacy
`_EXT_BURST_DATA`) is `[last | seq(2) | channel(5)]`. Byte 0 of a `0x40`
response is a plain uint8 echoing whatever the command carried — often not a
channel at all. Case `frame/sync-in-payload` sends channel `0xA4` on purpose, so
that a SYNC byte lands inside a payload and resynchronisation is put under test;
the echo is conformant and the harness now says so. `TestTheBurstChannelCeiling`
pins both halves: the echo passes, and an out-of-range channel in a burst header
still fails.

**Renaming that case to carry the `malformed` token was the other offered fix,
and it is the wrong one.** The waiver is for records that are not conformant.
Spending it on one that is records a true fact — this frame is unusual — as a
false one, and deletes the only evidence in the tree that a `0xA4` channel byte
is legal. Scope the check; never launder a correct record through the waiver.

## The third leg: a transcript is evidence, and it is hashed

`ant_conformance.py` writes a summary JSON beside every transcript it records —
the sha256 of the whole file and a sha256 per case — and those summaries are
committed in `archive/benchmarks/`. **`test_ant_golden.py` now reads them back**
and fails if a committed transcript no longer hashes to what was recorded with
it. That is the third clause of the contract between the two tools, and it needs
saying here because neither owns the other: a transcript recorded by
`ant_conformance.py` must keep arriving with a summary, or the check silently
covers nothing (`TestRealCaptures` asserts that at least one transcript is
covered, so "silently" is the one thing it cannot do).

It exists because the replay rules structurally cannot cover it. They check that
a record is well *formed*; they have no opinion about what a dongle should have
answered, and cannot have one, because the answer is the measurement. Flip one
data byte in a reply and every framing rule still passes — correctly, and
uselessly, since the Tier 1 acceptance criterion is a byte diff.

The real reason is blunter. When a transcript turns this suite red, the cheapest
way to green is to edit the transcript, and that falsifies the reference every
future A/B is measured against. It should be the one repair that cannot be made
quietly.

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
| `handmade-session.antser` | `-` | A plausible session opening: reset, capabilities, network key, assign, lib config, open, one broadcast — plus the three shapes of deliberate defect described below |
| `handmade-timed.antser` | real | The same opening with a real timestamp column, so the harness exercises both forms. It is `archive/captures/serial/README.md`'s own worked example with its two checksum typos corrected |
| `handmade-multichannel.antcap` | simulated | Two power meters at once, cut to different packet losses, for `tools/test_ant_verify.py`'s per-channel accounting. See the section below — it is neither hand-assembled nor off a wire |

## The `.antcap` files are a third kind, and the paragraph above does not apply

Everything above is about `.antser` serial transcripts and about frames typed
from a table. The two `.antcap` files here are neither hand-assembled nor off a
wire: they are **the output of C code, frozen**.

`radiant/tests/src/test_profile_compat.c` drives `src/profiles/profile_hr.c`
and `src/profiles/profile_power.c` for three 121-message cycles on a DK and
prints every transmitted message; the lines are lifted out of the ztest console
and committed here. `tools/test_compat_capture.py` then decodes each message with
`tools/ant_pages.py` and **re-encodes it, asserting the bytes come back
identical** — so the file is the channel through which a C implementation and a
Python implementation, written in different phases from the same layout tables,
check each other.

## `handmade-multichannel.antcap` is a fourth kind: simulator output, then cut

Neither hand-assembled nor frozen C. It is `tools/ant_sim.py`'s dry run — no
radio, no room — with some page 0x10 packets **deleted**, and the deletions are
the fixture: the exact-loss figure counts what is missing from the
transmitter's own event counter, so a deleted packet leaves a hole the counter
still numbers.

Two power meters in one file, cut to different losses (1 hole for `#4660`, 6
for `#4661`), because the property `tools/test_ant_verify.py` exists to check
is that the two are **counted apart**. A tool that pooled them would report one
figure roughly halfway between and look entirely reasonable doing it, which is
exactly how a multi-channel scheduling fault stays invisible. Only data pages
were deleted, never a common page — a common page is what the analyser votes on
to decide whether the counter steps per message or per page, and deleting those
would make the vector test the vote instead of the accounting.

The full recipe (seeds, periods, which records were dropped) is in the comment
block at the top of the file, so it can be rebuilt rather than patched. The
losses it produces are asserted by name in the test, so a rebuild that changes
them turns the suite red instead of being absorbed.

It carries no channel numbers, and that is not an omission: **a capture records
the sensor, not the receiver**. `ant_verify.py --replay` labels the streams with
channels only when the operator pairs them on the command line
(`--channel 0 --device-number 4660 --channel 1 --device-number 4661`), and
reports a null channel when nobody did.

| File | Timestamp column | What it covers |
|---|---|---|
| `compat-hr.antcap` | real | Heart rate `0x78` at period 8070, with the RadiANT beacon `0x70` and Tier I attestation `0x71` interleaved. 363 messages |
| `compat-power.antcap` | real | Bicycle power `0x0B` at period 8182, same two compat pages. 363 messages |

Three rules follow from what they are, and they are different from the rules
above:

1. **They are reproducible, not observed.** No physics, no noise, no random
   walk: the heart beats exactly once a second and the power meter reports
   exactly 200 W. That is what lets them be diffed. They are still **not**
   evidence about what a real sensor sends — a bench capture against a real head
   unit is the interop phase's job and belongs in `archive/captures/radio/`.
2. **Regenerating one is a deliberate act.** The whole point is that the bytes
   stop changing; a diff here means the C stream changed, and the question is
   always whether it was supposed to. Regenerate with
   `scripts\run_ztest_hw.ps1` and lift the `@@ compat-hr` / `@@ compat-power`
   lines out of `build\ztest_hw\ztest-console.txt`.
3. **The key is in the header comment, and it is a test key.** Root bytes
   `0x00..0x0F`, epoch 7, device number `0x2C41` — the same values pinned in the
   C suite and in `tools/test_compat_capture.py`, because the Python side has to
   derive the same `K_auth` to check the tags. It is `tools/ant_sim.py`'s
   `DEFAULT_COMPAT_ROOT`, so a capture from the firmware and one from the
   simulator are two streams under one key.

## The `malformed` case convention

This is the contract between `tools/test_ant_golden.py` (which enforces it) and
`tools/ant_conformance.py` (which writes transcripts against it). It is stated
here because the two are owned by different agents and neither owns the other.

**A `# case` name containing the token `malformed` declares that the records
under it need not conform. A problem with a record is a failure, unless its
case name declares nonconformance, in which case it is recorded and waived.**

`ant_conformance.py` exports the token as its `MALFORMED` constant and builds
names like `42-assign-channel/malformed-short`. `test_ant_golden.py` spells the
string out rather than importing it: a checker that imports its subject's idea
of the convention is checking that the tool agrees with itself.

Four properties of that rule, each chosen against a concrete failure:

1. **A conformant record inside a `malformed` case is not waived — it has
   nothing to waive.** The waiver is spent per record and only on a record that
   actually has a problem. This is what makes the reset/startup preamble
   `ant_conformance.py` emits before *every* case, malformed or not, a
   non-event. The first draft of the rule read `malformed` as "`unframe()` must
   reject this" and produced 718 false failures on a generated transcript, 409
   of them from exactly that preamble.
2. **A well-formed frame can be malformed.** `malformed-short`, `-long`,
   `-oob-index` and `-param-ff` are impeccable frames carrying a payload the
   message id does not admit — which is what makes them worth sending, since
   they test the bridge's error mapping rather than its framer. Only
   `malformed-checksum`, `-sync`, `-oversize` and `-partial-then-valid` are
   defects visible at the framing level.
3. **An unmodelled message id is still a hard failure in a real capture.** The
   waiver can only be claimed through a case marker, and a `Device0.txt`
   capture has none — Garmin's log does not name our test cases. So
   `frame/malformed-unknown-id-high` is waived in a conformance transcript
   while the same id in a capture from a real host fails and has to be absorbed
   into `protocol/ant_wire.yaml`. The absence of case names is the
   discriminator, and it costs no new vocabulary.
4. **There is no requirement that a declared case produce a visible problem —
   except for the fixtures here.** `malformed-oob-index` sends channel `0xFF`
   in a frame that is correct by every rule the harness knows; only the
   firmware's handler can see it is wrong. Demanding a visible problem would
   fail on a correct transcript. The fixtures in this directory *are* held to
   it, because their cases were written here and their defects were chosen to
   be visible at this level.

**Known hole**, stated rather than discovered later: because the waiver is
per record, a *preamble* frame that went bad inside a `malformed` case would be
waived along with the record the case is about. It is narrow — the preamble is
the same bytes in every case, so the same corruption is caught in every `valid`
case — and closing it means `ant_conformance.py` emitting its preamble under
its own non-malformed case name. That is filed as a change request, not assumed
here. `TestTheMalformedConvention.test_the_known_hole_in_the_convention` pins
the current behaviour so that closing it is a deliberate edit.

If `ant_conformance.py` needs different wording, this file and
`tools/test_ant_golden.py` are what change.

## The three defects the fixtures carry

`handmade-session.antser` ends with one case of each shape, so that every
branch of the rule above is exercised in CI rather than waiting for the first
real transcript:

| Case | Shape | Why it is here |
|---|---|---|
| `malformed/sync-omitted-checksum` | framing | The historical bug on the wire: `unframe()` must reject it |
| `malformed/short-payload` | semantic | An impeccable frame whose payload its id does not admit, plus the `INVALID_MESSAGE` refusal it should draw — and that refusal is fully conformant, sitting inside a malformed case, which is what proves property 1 above |
| `malformed/unknown-id` | unknown id | Id `0xFE`, in no table. Waived here because the case says so; fatal in an uncased capture |
