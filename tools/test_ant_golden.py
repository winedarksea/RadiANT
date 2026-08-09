#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Replay `.antser` transcripts and check them against the framing rules.

    python -m unittest discover -s tools -p "test_*.py"

Runs in the CI `host-tests` job: standard library only, no board, no driver,
no `pyusb`.

## What this does

An `.antser` file is a byte-level host<->dongle transcript, one framed message
per line, direction in a column - the format specified in
`archive/captures/serial/README.md`:

    # ant serial capture v1: <seconds|-> <dir> <framed message, hex>
    0.000000 > a4014a00ef
    0.014200 < a4016f20ea

For every **host to dongle** record, this re-derives the frame from
`tools/ant_wire.py` - `frame(msg_id, payload)` against the payload read out of
the record - and asserts the bytes match exactly. For every **dongle to host**
record it parses the frame and asserts the decode is self-consistent: the id
and payload `unframe()` returns are the bytes that are actually there, the id
is one the tables know, the length declared **for the direction the record is
in** admits the payload it carries, its declared direction admits the column it
arrived in, and a response event names either a real message id or the event
marker.

That is worth doing only because the transcript is foreign. A vector obtained
by running `frame()` and freezing the result agrees with `frame()` by
construction; a transcript recorded by `ANT_DLL.dll`, or typed by hand from a
table, does not. This is the same argument that makes
`scripts/gen_ant_wire.py` generate constants instead of cross-checking them:
generation makes one number appear in three places, and only a foreign
transcript checks the *rules*.

## Two sources, and why one of them can be missing

`archive/captures/serial/` is where real captures go. It now holds
`conformance-sdk-ant.antser`, the Tier 1 reference: a real ANTUSB-m-compatible
dongle answering 284 conformance cases, reproduced byte-identically across two
runs, and the file every future `ant_core` build is diffed against. When that
directory is empty - on a clone that has not fetched it, or before one was ever
recorded - `TestRealCaptures` skips, loudly, naming what is missing. It does
not pass.

`tools/vectors/` holds hand-assembled fixtures with their XOR worked out in
comments, and they are replayed on every run. They exist so the harness itself
is exercised from day one: a first real capture should add coverage, not be the
thing that discovers the harness was broken. A test that skips on every run and
a test that vacuously passes are the same test.

That ordering paid, and not in the direction anyone expected. Replaying the
first real transcript turned this suite red in five places, **all five of them
this file's fault**. The bytes were right; the rules were wrong. Both defects
are worth naming here, because both are the same mistake - a rule stated more
broadly than the thing it is true of.

## A message id is not a message

`payload_len` in `protocol/ant_wire.yaml` used to be one number per id, and a
request's reply is not the same shape as the command sharing its id. `0x78`
takes a 9..12-byte configuration command and answers a request with **4**
bytes; `0x7D` takes a 4-byte enable command and answers with **2**, **5** or
**20**, depending on which encryption info type was asked for. Four of the five
failures were that.

The YAML now carries a `reply_len` block wherever the two differ, and this file
picks between the two by `record.direction`, which it already had. The route
not taken was widening `payload_len` to the union - it would have gone green
immediately and would have admitted a 4-byte `0x78` *command* and a 20-byte
`0x7D` one for the rest of the project's life. Length is one of the few things
a Tier 1 byte diff genuinely rests on, and a check that cannot fail is worth
less than no check, because it also stops anyone writing the real one.

Each reply shape names a constant in the YAML's `message_sizes` rather than
restating a number, so the length this file checks against is the same one the
bridge writes into the LEN byte. Where the reply is self-describing - `0x7D`
echoes its info type at byte 0 - the shape is pinned from the reply alone.
Where it is not - `0x78`'s shape was chosen by byte 0 of the `MESG_REQUEST`,
which the reply does not repeat - it is pinned by pairing the reply with the
request earlier in the file. See `ReplayContext`.

## The five-bit ceiling belongs to the burst header

The fifth failure was `frame/sync-in-payload`, which sends `a4026ea4e08c`: an
antlib-config message using channel `0xA4` **on purpose**, so that a SYNC byte
lands inside a payload and the parser's resynchronisation is put under test.
The dongle echoes `a40340a46e002d` back, correctly, and this file rejected it
for carrying a channel above 32.

That ceiling is real, and it belongs to exactly one byte: `[last | seq(2) |
channel(5)]`, byte 0 of `MESG_BURST_DATA` and its two relatives, which
`src/ant_serial_bridge.c` reads with `ANTW_BURST_HEADER_CHANNEL_MASK`. Byte 0
of a `0x40` is a plain uint8 that echoes whatever the command carried, and on
most messages that byte is not a channel at all. See
`burst_header_problems()` for what is checkable there and what is not.

The other offered repair was renaming the case to carry the `malformed` token,
which would also have gone green. It was rejected: the waiver is for records
that are not conformant, and spending it on one that is would record a true
fact - this frame is unusual - as a false one, and delete the only evidence in
the tree that a `0xA4` channel byte is legal.

## What none of the above can see, and the hashes that can

Every rule here is about a record's *shape*. None of them has an opinion about
what a dongle should have answered, and none can: the answer is the
measurement. Flip one data byte in a reply and the frame is still impeccable.

The gate for that is the sha256s `tools/ant_conformance.py` recorded when it
captured the transcript, committed in `archive/benchmarks/`. This file reads
them back - whole file and per case - so that a byte changed inside an
otherwise conformant reply is caught and localised to its case. See
`integrity_problems()`. It is not there for integrity in the abstract: when a
real transcript turns this suite red, the cheapest way to green is to edit the
transcript, and that falsifies the reference every future A/B is measured
against.

## The `malformed` case convention

A `# case` name containing the token `malformed` declares that the records
under it **need not conform**. `tools/ant_conformance.py` exports the same
token as its `MALFORMED` constant and builds case names like
`42-assign-channel/malformed-short` from it.

The rule is one sentence: **a problem with a record is a failure, unless its
case name declares nonconformance, in which case it is recorded and waived.**

The first draft of this file said something stricter and wrong - that every
record under such a case must be rejected by `unframe()`. Replaying a generated
conformance transcript through it produced 718 failures, none of them real, in
two classes worth writing down because they are the shape of the problem:

* **The reset/startup pair `ant_conformance.py` sends before every case is
  perfectly well formed**, and it carries the case's name, malformed or not.
  409 of the 718. Under the rule above these records simply have no problems,
  so there is nothing to waive and nothing to fail.
* **`malformed-short`, `-long`, `-oob-index` and `-param-ff` are well-formed
  frames carrying a payload the message id does not admit.** That is what makes
  them worth sending: they test the bridge's error mapping, not its framer.
  Only `malformed-checksum`, `malformed-sync`, `malformed-oversize` and
  `malformed-partial-then-valid` are defects this harness can see at the
  framing level.

Two consequences follow, and both are deliberate:

**An unmodelled message id is still a hard failure in a real capture.** A
record can only claim the waiver through a case marker, and a `Device0.txt`
capture has no case markers at all - Garmin's log does not name our test cases.
So `frame/malformed-unknown-id-high` in a conformance transcript is waived,
while the same id turning up in a capture from a real host fails and has to be
absorbed into `protocol/ant_wire.yaml`. That is the right split, and the
absence of case names is what makes it free.

**This harness cannot see every level at which a record is malformed.**
`malformed-oob-index` sends channel `0xFF` in a frame that is impeccable by
every rule here; only the firmware's handler knows it is wrong. So there is no
requirement that a case declaring nonconformance actually produce a problem -
that assertion would fail on exactly the cases whose defect lives above the
framing layer. It *is* asserted for the fixtures in `tools/vectors/`, whose
cases were written here and are known to be visible at this level.

The known hole, stated rather than discovered later: a waiver is granted per
record, so a *preamble* frame that became corrupt inside a malformed case would
be waived along with the record the case is about. It would still be caught in
every `valid` case, since the preamble is the same bytes everywhere. Closing it
properly means `ant_conformance.py` emitting its preamble under a separate
non-malformed case name, which is filed as a change request rather than assumed
here.

## Deliberately not importing tools/ant_trace.py

`ant_trace.py` owns the `.antser` reader for the rest of the tools directory,
and this file reimplements a small strict parser instead. That is not
duplication for its own sake: a fixture read back by the same code that wrote
it proves nothing about the format, and this test's whole value is being an
implementation that the thing under test did not produce.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
import unittest
from collections import namedtuple

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)

import ant_wire as w  # noqa: E402

_REPO = os.path.dirname(_HERE)

VECTORS_DIR = os.path.join(_HERE, "vectors")
CAPTURES_DIR = os.path.join(_REPO, "archive", "captures", "serial")
BENCHMARKS_DIR = os.path.join(_REPO, "archive", "benchmarks")

HOST_TO_DONGLE = ">"
DONGLE_TO_HOST = "<"

HEADER_PREFIX = "# ant serial capture v1:"
CASE_MARKER = "# case "

# The token in a case name that waives conformance for the records under it.
# `tools/ant_conformance.py` exports the same string as its `MALFORMED`
# constant and builds names like `42-assign-channel/malformed-short` from it.
# Spelled out here rather than imported: that tool is the thing whose output
# this file exists to check, and a checker that imports its subject's idea of
# the convention is checking that the tool agrees with itself.
MALFORMED_TOKEN = "malformed"

# `<seconds|-> <dir> <hex>`, with an optional trailing comment so that the
# annotated form `ant_trace.py --to-text` writes still reads back.
_LINE = re.compile(r"^(?P<ts>-|\d+(?:\.\d+)?)\s+(?P<dir>[<>])\s+"
                   r"(?P<hex>[0-9a-fA-F]+)\s*(?:#.*)?$")

Record = namedtuple("Record", "lineno seconds direction data case")


class AntserError(Exception):
    """A line this parser refuses to interpret. Always names the line."""


def read_antser(text, source="<string>"):
    """Parse an `.antser` transcript strictly. No lenient mode.

    A fixture that half-parses is worse than one that does not parse at all:
    the half that was dropped is exactly the half nothing then checks.
    """
    records = []
    case = None
    seen_header = False

    for lineno, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#"):
            if line.startswith(HEADER_PREFIX):
                seen_header = True
            elif line.startswith(CASE_MARKER):
                case = line[len(CASE_MARKER):].strip()
            continue

        match = _LINE.match(line)
        if match is None:
            raise AntserError(
                f"{source}:{lineno}: not an .antser record: {raw!r}\n"
                f"  expected '<seconds|-> <dir> <hex>', e.g. "
                f"'- > a4014a00ef' or '0.014200 < a4016f20ea'")

        hex_text = match.group("hex")
        if len(hex_text) % 2:
            raise AntserError(f"{source}:{lineno}: odd number of hex digits "
                              f"in {hex_text!r}")
        seconds = None if match.group("ts") == "-" else float(
            match.group("ts"))
        records.append(Record(lineno, seconds, match.group("dir"),
                              bytes.fromhex(hex_text), case))

    if not seen_header:
        raise AntserError(
            f"{source}: no '{HEADER_PREFIX}' header line. A reader that does "
            f"not recognise the header should refuse rather than guess at the "
            f"columns.")
    return records


def read_antser_file(path):
    with open(path, "r", encoding="utf-8") as handle:
        return read_antser(handle.read(), source=os.path.basename(path))


def discover(directory):
    if not os.path.isdir(directory):
        return []
    return sorted(os.path.join(directory, name)
                  for name in os.listdir(directory)
                  if name.endswith(".antser"))


# Byte 0 of these three is a burst header - [last | seq(2) | channel(5)] - and
# not a plain channel byte. It is the one place in the serial protocol where a
# channel number is packed into fewer than eight bits, which is what makes the
# five-bit ceiling a property of *these* messages rather than of channel bytes
# in general. See burst_header_problems().
BURST_HEADER_IDS = frozenset({
    w.MESG_BURST_DATA_ID,
    w.MESG_ADV_BURST_DATA_ID,
    w.MESG_EXT_BURST_DATA_ID,
})

BURST_CHANNEL_MASK = w.BURST_HEADER["BURST_HEADER_CHANNEL_MASK"]


def _declared_range(declared):
    """(low, high) payload lengths a message id admits, or None for 'var'."""
    if declared == "var":
        return None
    text = str(declared)
    if ".." in text:
        low, high = text.split("..")
        return int(low), int(high)
    return int(text), int(text)


# ---------------------------------------------------------------------------
# Integrity: the transcript is evidence
# ---------------------------------------------------------------------------
#
# `tools/ant_conformance.py` writes a summary JSON alongside every transcript it
# records - the sha256 of the whole file, and a sha256 per case over that case's
# records. Those summaries are committed in archive/benchmarks/ and until now
# nothing read them back.
#
# This covers what the replay rules structurally cannot. They check that a
# record is well *formed*; they have no opinion about what a dongle should have
# answered and cannot have one, because the answer is the measurement. Flip one
# data byte in a reply and every framing rule still passes - correctly, and
# uselessly, since the Tier 1 acceptance criterion is a byte diff and a silently
# edited reference would make every future A/B agree with a fiction.


def _walk_summaries(node):
    """Every ant_conformance summary inside a decoded JSON document."""
    if isinstance(node, dict):
        if "antser_path" in node and "case_index" in node and "sha256" in node:
            yield node
        for value in node.values():
            yield from _walk_summaries(value)
    elif isinstance(node, list):
        for value in node:
            yield from _walk_summaries(value)


def recorded_summaries(directory=BENCHMARKS_DIR):
    """{transcript basename: (json file, summary)} from a baselines directory.

    Nested rather than top-level on purpose: a sitting's baseline JSON embeds
    the conformance summary under its own key, and a summary written with
    `--json` on its own is the same shape at the root.
    """
    found = {}
    if not os.path.isdir(directory):
        return found
    for name in sorted(os.listdir(directory)):
        if not name.endswith(".json"):
            continue
        path = os.path.join(directory, name)
        try:
            with open(path, "r", encoding="utf-8") as handle:
                document = json.load(handle)
        except (ValueError, OSError):
            continue
        for summary in _walk_summaries(document):
            key = os.path.basename(
                str(summary["antser_path"]).replace("\\", "/"))
            found.setdefault(key, (name, summary))
    return found


def integrity_problems(path, summary):
    """Every way a transcript disagrees with the summary recorded with it.

    Reports the whole-file hash first and then the per-case hashes, because the
    second answers the question the first raises: a file-level mismatch says
    something moved, and the case index says which case.
    """
    problems = []
    with open(path, "rb") as handle:
        raw = handle.read()
    name = os.path.basename(path)

    digest = hashlib.sha256(raw).hexdigest()
    if digest != summary["sha256"]:
        problems.append(
            f"{name}: sha256 is {digest}, but the summary recorded when it was "
            f"captured says {summary['sha256']}. This file is the Tier 1 "
            f"reference every future ant_core build is diffed against; if a "
            f"check disagrees with it, the check is what changes.")

    records = read_antser(raw.decode("utf-8"), source=name)
    if len(records) != summary["records"]:
        problems.append(f"{name}: {len(records)} records, the summary says "
                        f"{summary['records']}")

    per_case = {}
    for record in records:
        per_case.setdefault(record.case or "?", []).append(
            f"{record.direction}{record.data.hex()}")

    indexed = set()
    for entry in summary["case_index"]:
        indexed.add(entry["name"])
        lines = per_case.get(entry["name"])
        if lines is None:
            problems.append(f"{name}: case {entry['name']!r} is in the summary "
                            f"and not in the file")
            continue
        digest = hashlib.sha256("\n".join(lines).encode("utf-8")).hexdigest()
        if len(lines) != entry["records"]:
            problems.append(f"{name}: case {entry['name']!r} has {len(lines)} "
                            f"records, the summary says {entry['records']}")
        elif digest != entry["sha256"]:
            problems.append(f"{name}: case {entry['name']!r} hashes to "
                            f"{digest[:16]}..., the summary says "
                            f"{entry['sha256'][:16]}... - a byte inside this "
                            f"case has changed since it was recorded")

    for case in per_case:
        if case not in indexed:
            problems.append(f"{name}: case {case!r} is in the file and not in "
                            f"the summary")
    return problems


class ReplayContext:
    """What a record cannot say about itself, read off the transcript.

    Two checks need more than the record in front of them, and both would be
    weaker without it:

    * A `request_index`-selected reply (`0x78`) does not carry the index that
      chose its shape. Only the `MESG_REQUEST` earlier in the file does.
    * A burst header's channel is bounded by how many channels the device has,
      and the device states that in its own capabilities reply.

    Deriving both from the transcript rather than hardcoding them is what keeps
    the checks honest across backends: a 32-channel `ant_core` advertises 32 and
    the ceiling follows it, with nothing here to update. Nothing in here reads a
    clock, a filesystem or an environment variable, so a replay is as
    reproducible as the transcript it is given.
    """

    def __init__(self, records=()):
        self.max_channels = None
        self.max_channels_line = None
        self._request_index = {}
        for record in records:
            self._scan_capabilities(record)

    def _scan_capabilities(self, record):
        if self.max_channels is not None or declares_nonconformance(record):
            return
        if record.direction != DONGLE_TO_HOST:
            return
        parsed = w.unframe(record.data)
        if parsed is None:
            return
        msg_id, payload = parsed
        if msg_id != w.MESG_CAPABILITIES_ID or not payload:
            return
        self.max_channels = payload[w.CAPABILITIES_OFFSET_MAX_CHANNELS]
        self.max_channels_line = record.lineno

    def observe(self, record):
        """Fed every record in file order, before that record is checked."""
        if record.direction != HOST_TO_DONGLE:
            return
        parsed = w.unframe(record.data)
        if parsed is None:
            return
        msg_id, payload = parsed
        if msg_id == w.MESG_REQUEST_ID and len(payload) >= 2:
            self._request_index[payload[1]] = payload[0]

    def request_index_for(self, msg_id):
        """Byte 0 of the most recent MESG_REQUEST asking for `msg_id`."""
        return self._request_index.get(msg_id)


def declares_nonconformance(record):
    """True when the record's case name waives conformance for it.

    A record with no case marker can never claim the waiver, which is what
    keeps a real `Device0.txt` capture - Garmin's log names no test cases -
    under the strict rule.
    """
    return MALFORMED_TOKEN in (record.case or "")


class ReplayMixin:
    """The checks. Shared by the fixture and real-capture test cases.

    Every check returns *problems* rather than asserting, so that one policy
    decision - fail, or waive and record - is made in one place for all of
    them. The first draft asserted inline and could therefore only express
    "this record must be rejected", which is true of a handful of conformance
    cases and false of most.
    """

    def replay_file(self, path):
        return self.replay_records(read_antser_file(path),
                                   os.path.basename(path))

    def classify(self, records, name):
        """(fatal, waived) - the whole policy, with no assertions in it.

        Kept separate from the assertions because a rule that can only be
        exercised through `assertRaises` cannot be tested for being too
        *permissive*, and permissiveness is this rule's failure mode. See
        TestTheMalformedConvention, which drives this directly.
        """
        fatal = []      # [(record, [problems])]
        waived = {}     # case name -> [problems]
        context = ReplayContext(records)
        for record in records:
            context.observe(record)
            problems = self.record_problems(record, name, context)
            if not problems:
                continue
            if declares_nonconformance(record):
                waived.setdefault(record.case, []).extend(problems)
            else:
                fatal.append((record, problems))
        return fatal, waived

    def replay_records(self, records, name):
        """Check every record. Returns {case name: [waived problems]}."""
        self.assertTrue(records, f"{name}: no records")
        fatal, waived = self.classify(records, name)

        for record, problems in fatal:
            with self.subTest(file=name, line=record.lineno,
                              case=record.case):
                self.fail("\n".join(problems))

        self.check_timestamps(records, name)
        self.check_request_replies(records, name)
        return waived

    # -- per record ---------------------------------------------------------

    def record_problems(self, record, name, context=None):
        """Everything wrong with one record. Empty means fully conformant."""
        if context is None:
            context = ReplayContext()
        data = record.data
        where = f"{name}:{record.lineno} {data.hex()}"
        problems = []

        if len(data) < w.MSG_OVERHEAD:
            return [f"{where}: {len(data)} bytes, shorter than MSG_OVERHEAD "
                    f"({w.MSG_OVERHEAD})"]

        if data[0] != w.SYNC_TX:
            problems.append(
                f"{where}: starts 0x{data[0]:02X}, not SYNC_TX "
                f"0x{w.SYNC_TX:02X}"
                + (" (0xA5 is the bidirectional variant's SYNC, which this "
                   "dongle never speaks)" if data[0] == w.SYNC_RX else ""))

        length_agrees = data[1] + w.MSG_OVERHEAD == len(data)
        if not length_agrees:
            problems.append(
                f"{where}: LEN says {data[1]} payload bytes, "
                f"{len(data) - w.MSG_OVERHEAD} are present")

        if len(data) > w.MAX_FRAME_SIZE:
            problems.append(
                f"{where}: {len(data)} bytes, longer than MAX_FRAME_SIZE "
                f"({w.MAX_FRAME_SIZE}); no receive buffer in the firmware can "
                f"hold it")

        parsed = w.unframe(data)
        if parsed is None:
            note = ""
            if length_agrees:
                computed = w.checksum(data[:-1])
                note = (f" Computed checksum 0x{computed:02X}, frame says "
                        f"0x{data[-1]:02X}.")
                if computed ^ data[-1] == w.SYNC_TX:
                    note += (" Out by exactly the SYNC byte, so whoever wrote "
                             "it left SYNC out of the XOR.")
            problems.append(f"{where}: unframe() rejected it.{note}")
            return problems

        msg_id, payload = parsed

        # The decode must be the bytes that are there, not a reinterpretation.
        if msg_id != data[2]:
            problems.append(f"{where}: decoded id 0x{msg_id:02X} disagrees "
                            f"with byte 2 (0x{data[2]:02X})")
        if payload != data[3:-1]:
            problems.append(f"{where}: decoded payload disagrees with the "
                            f"frame body")
        if len(payload) != data[1]:
            problems.append(f"{where}: decoded payload is {len(payload)} "
                            f"bytes, LEN says {data[1]}")

        # Re-derive the frame from ant_wire and require byte equality. This is
        # the assertion the whole file exists for; everything else is context.
        rederived = w.frame(msg_id, payload)
        if rederived != data:
            problems.append(f"{where}: ant_wire.frame() does not reproduce "
                            f"this record; it produces {rederived.hex()}")

        problems.extend(
            self.message_problems(record, msg_id, payload, where, context))
        return problems

    def message_problems(self, record, msg_id, payload, where, context=None):
        info = w.MESSAGES.get(msg_id) or w.RADIANT_MESSAGES.get(msg_id)
        if info is None:
            return [f"{where}: message id 0x{msg_id:02X} is in no table. In a "
                    f"capture from a real host that is information to absorb "
                    f"into protocol/ant_wire.yaml, not to ignore; in a "
                    f"generated transcript, name the case 'malformed'."]

        problems = []
        if info["direction"] == "marker":
            problems.append(
                f"{where}: 0x{msg_id:02X} ({info['name']}) is a marker inside "
                f"another message, never a frame of its own")
        else:
            allowed = {HOST_TO_DONGLE: ("h2d", "both"),
                       DONGLE_TO_HOST: ("d2h", "both")}[record.direction]
            if info["direction"] not in allowed:
                problems.append(
                    f"{where}: {info['name']} is {info['direction']}, but the "
                    f"record is in the {record.direction!r} column")

        if context is None:
            context = ReplayContext()

        problems.extend(
            self.length_problems(record, msg_id, info, payload, where,
                                 context))

        if msg_id == w.MESG_RESPONSE_EVENT_ID and len(payload) == 3:
            problems.extend(self.response_event_problems(payload, where))
        if msg_id in BURST_HEADER_IDS and payload:
            problems.extend(
                self.burst_header_problems(payload[0], where, context))
        return problems

    # -- length, which depends on which direction the record is in -----------

    def length_problems(self, record, msg_id, info, payload, where, context):
        """The declared length for *this* record, not for this message id.

        A message id is not a message: `MESG_REQUEST` names an id and the reply
        carries that same id with a payload no host ever sends. `payload_len`
        in the YAML describes the command; `reply_len`, where the two differ,
        describes the reply. Picking between them by direction is the whole fix
        - the alternative on offer was widening `payload_len` to the union of
        both, which would have admitted a 4-byte 0x78 *command* and a 20-byte
        0x7D one, and a length check that admits everything is worth less than
        no length check at all.
        """
        reply = info.get("reply_len")
        if record.direction == DONGLE_TO_HOST and reply is not None:
            return self.reply_length_problems(msg_id, info, reply, payload,
                                              where, context)

        bounds = _declared_range(info["payload_len"])
        if bounds is None or bounds[0] <= len(payload) <= bounds[1]:
            return []
        kind = "command payload" if reply is not None else "payload"
        return [f"{where}: {info['name']} declares a {kind} of "
                f"{info['payload_len']}, this carries {len(payload)}"]

    def reply_length_problems(self, msg_id, info, reply, payload, where,
                              context):
        """One of the declared reply shapes, chosen by the declared selector."""
        name = info["name"]
        shapes = reply["shapes"]
        selector = reply["selector"]
        declared = ", ".join(str(shape["len"]) for shape in shapes)

        if selector == "none":
            shape = shapes[0]
            chosen_by = "its one declared reply shape"
        elif selector == "reply_byte_0":
            # Self-describing: the reply echoes the info type it answers, so
            # the exact expected length is readable from the reply alone.
            if not payload:
                return [f"{where}: {name} reply is empty, so there is no info "
                        f"type at byte 0 to select one of its {len(shapes)} "
                        f"reply shapes ({declared} bytes)"]
            match = [s for s in shapes if s["when"] == payload[0]]
            if not match:
                known = ", ".join("0x%02X" % s["when"] for s in shapes)
                return [f"{where}: {name} echoes info type 0x{payload[0]:02X} "
                        f"at byte 0, which is not one it declares a reply "
                        f"shape for ({known}). An undeclared info type is "
                        f"refused with a 0x40 naming MESG_REQUEST, never "
                        f"answered with a 0x{msg_id:02X}"]
            shape = match[0]
            chosen_by = f"info type 0x{payload[0]:02X} at byte 0"
        else:  # request_index
            index = context.request_index_for(msg_id)
            if index is None:
                # Nothing earlier in the file asked, so the index that chose
                # the shape is simply not in evidence. Fall back to the set
                # rather than pretend: a real host log may start mid-session.
                if len(payload) in {s["len"] for s in shapes}:
                    return []
                return [f"{where}: {name} reply carries {len(payload)} bytes. "
                        f"No MESG_REQUEST for 0x{msg_id:02X} appears earlier "
                        f"in this transcript, so only the declared set "
                        f"({declared}) can be checked here - and this is not "
                        f"in it"]
            match = [s for s in shapes if s["when"] == index]
            if not match:
                known = ", ".join("0x%02X" % s["when"] for s in shapes)
                return [f"{where}: the MESG_REQUEST that drew this asked with "
                        f"index 0x{index:02X}, and {name} declares no reply "
                        f"shape for it ({known})"]
            shape = match[0]
            chosen_by = f"request index 0x{index:02X}"

        if len(payload) == shape["len"]:
            return []
        return [f"{where}: {name} reply selected by {chosen_by} declares "
                f"{shape['len']} bytes (ANTW_{shape['size']}), this carries "
                f"{len(payload)}"]

    # -- the five-bit ceiling, where it is actually imposed -------------------

    def burst_header_problems(self, header, where, context):
        """Byte 0 of a burst message: [last | seq(2) | channel(5)].

        This is the only place the serial protocol squeezes a channel number
        below eight bits - `src/ant_serial_bridge.c` reads it with
        `ANTW_BURST_HEADER_CHANNEL_MASK` - and therefore the only place the
        five-bit ceiling is a rule about the bytes rather than about nothing.

        Two consequences, and both bound what this check may honestly claim:

        * Because the field *is* five bits, no burst header on a wire can
          express a channel above 31. A literal "channel > 31 here" test could
          never fire. That is exactly how the ceiling ended up on the
          RESPONSE_EVENT channel byte instead, where it could fire - and did,
          on `frame/sync-in-payload`, which is a perfectly conformant record.
        * What a burst header *can* address is a channel the device does not
          have. The bound for that is the channel count in byte 0 of the
          capabilities reply, and the transcript states it about itself. Read
          it from there rather than hardcoding this firmware's 8: a 32-channel
          `ant_core` advertises 32 and this check follows it with no edit.

        A transcript containing no capabilities reply cannot say how many
        channels the device has, so nothing is claimed. That is a real hole and
        it is narrow: `ant_conformance.py` requests capabilities in every run,
        and a host session that never asks has no basis for the check anyway.
        """
        channel = header & BURST_CHANNEL_MASK
        if context.max_channels is None or channel < context.max_channels:
            return []
        return [f"{where}: burst header 0x{header:02X} addresses channel "
                f"{channel} (low {BURST_CHANNEL_MASK.bit_length()} bits of "
                f"byte 0), but the capabilities reply in this transcript "
                f"advertises {context.max_channels} channels"]

    def response_event_problems(self, payload, where):
        """[channel, message id or MESG_EVENT_ID, code].

        The middle byte decides which number space byte 2 lives in, and the
        two spaces overlap. A reader that skips the check reports EVENT_TX
        (0x03) as a reply to message 0x03.

        Byte 0 carries **no** five-bit ceiling. It is a plain uint8 that echoes
        whatever byte 0 of the command carried, and for most messages that byte
        is not a channel number at all - it is a filler, a mask index or an
        encryption info type. Case `frame/sync-in-payload` sends channel 0xA4
        on purpose, so that a SYNC byte lands inside a payload and the parser's
        resynchronisation is put under test; the dongle echoes 0xA4 back in a
        0x40 and is right to. Rejecting that as "above the 5-bit ceiling" was
        the check reaching outside where its constraint holds - see
        burst_header_problems() for where it does hold.

        It holds in one place here too, and that one is kept: when the response
        answers a burst message, the bridge derived byte 0 by masking a burst
        header, so a value above the mask could not have come from one.
        """
        channel, middle, code = payload
        problems = []
        if middle in BURST_HEADER_IDS and channel > BURST_CHANNEL_MASK:
            problems.append(
                f"{where}: this 0x40 answers 0x{middle:02X}, whose channel the "
                f"bridge takes as `header & 0x{BURST_CHANNEL_MASK:02X}`, so it "
                f"cannot be {channel}")
        if middle == w.MESG_EVENT_ID:
            if code not in w.EVENT_CODES_BY_VALUE:
                problems.append(f"{where}: 0x{code:02X} is not a known event "
                                f"code")
        else:
            if middle not in w.MESSAGES:
                problems.append(f"{where}: response names unknown message "
                                f"0x{middle:02X}")
            if code not in w.RESPONSE_CODES_BY_VALUE:
                problems.append(f"{where}: 0x{code:02X} is not a known "
                                f"response code")
        return problems

    # -- per file -----------------------------------------------------------

    def check_timestamps(self, records, name):
        stamps = [r.seconds for r in records]
        timed = [s for s in stamps if s is not None]
        self.assertIn(len(timed), (0, len(stamps)),
                      f"{name}: mixes real timestamps with '-'. A transcript "
                      f"is either a statement about timing or it is not.")
        for earlier, later in zip(timed, timed[1:]):
            self.assertLessEqual(earlier, later,
                                 f"{name}: timestamps go backwards")

    def check_request_replies(self, records, name):
        """Every MESG_REQUEST is answered somewhere later in the file.

        Ordering across directions is the property the format's
        direction-as-a-column decision exists to preserve, so it is worth one
        check. A refusal comes back as a 0x40 naming 0x4D, not the wanted id.

        Skipped entirely on a one-directional transcript: `ant_conformance.py`
        can generate the host side of a run without a device attached, and a
        file with no replies in it is not making a claim about ordering.
        """
        if not any(r.direction == DONGLE_TO_HOST for r in records):
            return
        for index, record in enumerate(records):
            if (declares_nonconformance(record)
                    or record.direction != HOST_TO_DONGLE
                    or len(record.data) < 5
                    or record.data[2] != w.MESG_REQUEST_ID):
                continue
            wanted = record.data[4]
            answered = False
            for later in records[index + 1:]:
                if later.direction != DONGLE_TO_HOST or len(later.data) < 4:
                    continue
                if later.data[2] == wanted:
                    answered = True
                    break
                if (later.data[2] == w.MESG_RESPONSE_EVENT_ID
                        and len(later.data) >= 6
                        and later.data[4] == w.MESG_REQUEST_ID):
                    answered = True
                    break
            self.assertTrue(
                answered,
                f"{name}:{record.lineno}: nothing later in the transcript "
                f"answers the request for 0x{wanted:02X} - neither the "
                f"message itself nor a 0x40 refusing 0x4D")


class TestHandmadeFixtures(ReplayMixin, unittest.TestCase):
    """The fixtures in tools/vectors/, which are committed and always run."""

    def test_the_fixtures_are_still_there(self):
        # If these vanish, every other test in this file silently stops
        # checking anything. Say so instead.
        self.assertTrue(discover(VECTORS_DIR),
                        f"no .antser fixtures in {VECTORS_DIR} - the harness "
                        f"would run against nothing")

    def test_replay(self):
        for path in discover(VECTORS_DIR):
            with self.subTest(os.path.basename(path)):
                self.replay_file(path)

    def test_every_declared_nonconformance_is_real(self):
        # Asserted for these fixtures and deliberately NOT for real captures.
        # The cases here were written in this directory and their defects were
        # chosen to be visible at the framing-and-tables level. A conformance
        # transcript's `malformed-oob-index` is malformed in a way only the
        # firmware's handler can see, so demanding a visible problem there
        # would fail on a correct file.
        for path in discover(VECTORS_DIR):
            records = read_antser_file(path)
            declared = {r.case for r in records if declares_nonconformance(r)}
            waived = self.replay_records(records, os.path.basename(path))
            for case in declared:
                with self.subTest(case=case):
                    self.assertIn(
                        case, waived,
                        f"case {case!r} declares nonconformance but every "
                        f"record in it is fully conformant - the declaration "
                        f"is stale and the case checks nothing")

    def test_the_fixtures_cover_both_kinds_of_defect(self):
        # The waiver has two quite different customers and both need to be
        # exercised in CI, or the first real conformance transcript is what
        # discovers that one of the branches never ran:
        #   * a frame the framer itself rejects (the checksum bug), and
        #   * an impeccable frame whose payload the message id does not admit,
        #     which is the whole `malformed-short` / `-long` family.
        framing = semantic = False
        for path in discover(VECTORS_DIR):
            for record in read_antser_file(path):
                if not declares_nonconformance(record):
                    continue
                if w.unframe(record.data) is None:
                    framing = True
                elif self.record_problems(record, os.path.basename(path)):
                    semantic = True
        self.assertTrue(framing, "no fixture carries a framing-level defect")
        self.assertTrue(semantic, "no fixture carries a well-formed frame "
                                  "whose payload its id does not admit")

    def test_both_timestamp_forms_are_exercised(self):
        forms = set()
        for path in discover(VECTORS_DIR):
            for record in read_antser_file(path):
                forms.add(record.seconds is None)
        self.assertEqual(forms, {True, False},
                         "the fixtures should cover both the '-' column and "
                         "real timestamps; ant_conformance.py writes one and "
                         "ant_trace.py the other")


class TestRealCaptures(ReplayMixin, unittest.TestCase):
    """archive/captures/serial/*.antser, when any exist."""

    @classmethod
    def setUpClass(cls):
        cls.paths = discover(CAPTURES_DIR)
        if not cls.paths:
            raise unittest.SkipTest(
                f"no .antser captures in {CAPTURES_DIR}. This is not a pass: "
                f"nothing recorded from a real host has been replayed. One is "
                f"produced by calling ANT_DLL.dll's ANT_SetDebugLogDirectory "
                f"before ANT_Init, running a session, and normalising the "
                f"resulting Device0.txt with tools/ant_trace.py - see that "
                f"directory's README for the shopping list. Until then the "
                f"hand-assembled fixtures in tools/vectors/ are the only "
                f"thing exercising this harness.")

    def test_replay(self):
        for path in self.paths:
            with self.subTest(os.path.basename(path)):
                self.replay_file(path)

    def test_the_transcript_is_still_the_one_that_was_recorded(self):
        """Hashes from `archive/benchmarks/`, checked back against the bytes.

        The reason to run this next to the replay checks is blunter than
        integrity in the abstract. When a real transcript turns this suite red
        the cheapest way to green is to edit the transcript, and that is
        falsifying the reference every future A/B is measured against. It
        should be the one repair that cannot be made quietly.
        """
        summaries = recorded_summaries()
        checked = 0
        for path in self.paths:
            entry = summaries.get(os.path.basename(path))
            if entry is None:
                continue
            source, summary = entry
            checked += 1
            with self.subTest(os.path.basename(path), summary=source):
                problems = integrity_problems(path, summary)
                if problems:
                    self.fail("\n".join(problems))
        self.assertTrue(
            checked,
            f"none of the {len(self.paths)} transcript(s) in {CAPTURES_DIR} is "
            f"named by a summary in {BENCHMARKS_DIR}, so this test checked "
            f"nothing. A transcript recorded by tools/ant_conformance.py has "
            f"one; a Device0.txt capture normalised by ant_trace.py does not "
            f"and never will, and would need its hash recorded some other way "
            f"before it is worth citing as evidence.")


class TestTheMalformedConvention(ReplayMixin, unittest.TestCase):
    """The waiver, exercised on transcripts built in memory.

    These are the shapes `tools/ant_conformance.py` actually emits, reduced to
    one record each. They are here rather than as fixtures because the point is
    the *rule*, not the bytes, and because half of them must fail - which a
    committed fixture cannot express.
    """

    HEADER = HEADER_PREFIX + " <seconds|-> <dir> <framed message, hex>\n"

    # MESG_ASSIGN_CHANNEL declares 3..4 payload bytes; this carries 1. The
    # frame is impeccable. `ant_conformance.py` calls this malformed-short.
    SHORT = "a4014201e6"
    # Message id 0xFE is in no table. `ant_conformance.py` calls the same shape
    # frame/malformed-unknown-id-high.
    UNKNOWN = "a401fe005b"
    # The reset/startup pair that precedes every conformance case, malformed or
    # not, and is perfectly well formed in both.
    PREAMBLE = "- > a4014a00ef\n- < a4016f20ea\n"

    def classify_text(self, body, name="in-memory.antser"):
        return self.classify(read_antser(self.HEADER + body), name)

    def test_a_short_payload_fails_when_no_case_waives_it(self):
        fatal, waived = self.classify_text(f"- > {self.SHORT}\n")
        self.assertEqual(waived, {})
        self.assertEqual(len(fatal), 1)
        self.assertIn("declares a payload of 3..4, this carries 1",
                      fatal[0][1][0])

    def test_a_short_payload_is_waived_inside_a_malformed_case(self):
        fatal, waived = self.classify_text(
            f"# case 42-assign-channel/malformed-short\n- > {self.SHORT}\n")
        self.assertEqual(fatal, [])
        self.assertIn("42-assign-channel/malformed-short", waived)

    def test_an_unknown_id_fails_without_a_case_marker(self):
        # The discriminator that matters. A Device0.txt capture has no case
        # names, so an id no table knows can never be waived there - it has to
        # be absorbed into protocol/ant_wire.yaml.
        fatal, waived = self.classify_text(f"- > {self.UNKNOWN}\n")
        self.assertEqual(waived, {})
        self.assertEqual(len(fatal), 1)
        self.assertIn("is in no table", fatal[0][1][0])

    def test_an_unknown_id_is_waived_inside_a_malformed_case(self):
        fatal, waived = self.classify_text(
            f"# case frame/malformed-unknown-id-high\n- > {self.UNKNOWN}\n")
        self.assertEqual(fatal, [])
        self.assertIn("frame/malformed-unknown-id-high", waived)

    def test_a_well_formed_preamble_inside_a_malformed_case_is_fine(self):
        # The 409 false failures. These records have no problems at all, so
        # there is nothing to waive and nothing to fail.
        fatal, waived = self.classify_text(
            "# case 42-assign-channel/malformed-short\n"
            + self.PREAMBLE + f"- > {self.SHORT}\n")
        self.assertEqual(fatal, [])
        self.assertEqual(list(waived), ["42-assign-channel/malformed-short"])
        self.assertEqual(len(waived["42-assign-channel/malformed-short"]), 1)

    def test_a_valid_case_is_still_strict(self):
        fatal, _ = self.classify_text(
            f"# case 42-assign-channel/valid\n- > {self.SHORT}\n")
        self.assertEqual(len(fatal), 1)

    def test_a_checksum_defect_is_waived_and_diagnosed(self):
        fatal, waived = self.classify_text(
            "# case 4d-request-capabilities/malformed-checksum\n"
            "- > a4024d00541b\n")
        case = "4d-request-capabilities/malformed-checksum"
        self.assertEqual(fatal, [])
        self.assertIn(case, waived,
                      "these bytes carry the SYNC-omitted checksum and must "
                      "be rejected; nothing was waived, so unframe() accepted "
                      "them")
        self.assertIn("Out by exactly the SYNC byte", waived[case][0])

    def test_the_known_hole_in_the_convention(self):
        # A waiver is granted per record, so a preamble frame that went bad
        # inside a malformed case is waived along with the record the case is
        # about. Asserted rather than left implicit: closing it means
        # ant_conformance.py emitting its preamble under its own case name, and
        # when that happens this test is the one that should change.
        broken_preamble = "- > a4014a00ee\n"       # checksum EF -> EE
        fatal, waived = self.classify_text(
            "# case 42-assign-channel/malformed-short\n"
            + broken_preamble + f"- > {self.SHORT}\n")
        self.assertEqual(fatal, [])
        self.assertEqual(len(waived["42-assign-channel/malformed-short"]), 2)
        # The same bytes in a valid case are caught, which is why the hole is
        # narrow: the preamble is identical in every case, malformed or not.
        fatal, _ = self.classify_text(
            "# case 42-assign-channel/valid\n" + broken_preamble)
        self.assertEqual(len(fatal), 1)


class TestTheDirectionDependentLengthModel(ReplayMixin, unittest.TestCase):
    """`payload_len` is the command; `reply_len` is the request reply.

    Written in the shape `TestTheMalformedConvention` established, and for the
    same reason: the failure mode of a length rule is being too permissive, and
    permissiveness cannot be tested through `assertRaises`. Every rule below is
    pinned from both sides - the conformant bytes pass, and the smallest edit
    that should break them does.

    The bytes are the real ones from
    `archive/captures/serial/conformance-sdk-ant.antser`, reduced to the two or
    three records that matter, so a change that makes these pass while the
    transcript fails is not available.
    """

    HEADER = HEADER_PREFIX + " <seconds|-> <dir> <framed message, hex>\n"

    ADV_BURST_CAPS = w.frame(w.MESG_CONFIG_ADV_BURST_ID,
                             bytes([0x03, 0x03, 0x00, 0x00])).hex()
    ADV_BURST_CONFIG = w.frame(
        w.MESG_CONFIG_ADV_BURST_ID,
        bytes([0x03, 0, 0, 0, 0, 0, 0, 0x8A, 0x0C, 0x03])).hex()
    ENCRYPT_MODE = w.frame(w.MESG_ENCRYPT_ENABLE_ID, bytes([0x00, 0x02])).hex()
    ENCRYPT_ID = w.frame(w.MESG_ENCRYPT_ENABLE_ID, bytes([0x01, 0, 0, 0, 0])).hex()
    ENCRYPT_USER = w.frame(w.MESG_ENCRYPT_ENABLE_ID,
                           bytes([0x02]) + bytes(19)).hex()

    @staticmethod
    def request(msg_id, index):
        return w.frame(w.MESG_REQUEST_ID, bytes([index, msg_id])).hex()

    def classify_text(self, body, name="in-memory.antser"):
        return self.classify(read_antser(self.HEADER + body), name)

    def only_problem(self, body):
        fatal, waived = self.classify_text(body)
        self.assertEqual(waived, {})
        self.assertEqual(len(fatal), 1, f"expected exactly one bad record in\n{body}")
        self.assertEqual(len(fatal[0][1]), 1, fatal[0][1])
        return fatal[0][1][0]

    def assert_clean(self, body):
        fatal, waived = self.classify_text(body)
        self.assertEqual(fatal, [], fatal)
        self.assertEqual(waived, {})

    # -- the four records that were failing ----------------------------------

    def test_the_transcripts_own_adv_burst_replies_are_conformant(self):
        for index, reply in ((0, self.ADV_BURST_CAPS),
                             (1, self.ADV_BURST_CONFIG)):
            with self.subTest(index=index):
                self.assert_clean(
                    f"- > {self.request(w.MESG_CONFIG_ADV_BURST_ID, index)}\n"
                    f"- < {reply}\n")

    def test_the_transcripts_own_encrypt_replies_are_conformant(self):
        for reply in (self.ENCRYPT_MODE, self.ENCRYPT_ID, self.ENCRYPT_USER):
            with self.subTest(reply=reply):
                self.assert_clean(f"- < {reply}\n")

    # -- and the edits that must break them ----------------------------------

    def test_a_reply_of_the_other_shape_fails(self):
        # The capabilities reply answering a configuration request. Both
        # lengths are declared for this id, so a check that only asked "is it
        # one of the declared shapes" would let this through - which is the
        # whole reason the request index is tracked.
        problem = self.only_problem(
            f"- > {self.request(w.MESG_CONFIG_ADV_BURST_ID, 1)}\n"
            f"- < {self.ADV_BURST_CAPS}\n")
        self.assertIn("request index 0x01", problem)
        self.assertIn("declares 10 bytes "
                      "(ANTW_MESG_CONFIG_ADV_BURST_REQ_CONFIG_SIZE)", problem)
        self.assertIn("this carries 4", problem)

    def test_the_other_direction_of_the_same_swap_fails(self):
        problem = self.only_problem(
            f"- > {self.request(w.MESG_CONFIG_ADV_BURST_ID, 0)}\n"
            f"- < {self.ADV_BURST_CONFIG}\n")
        self.assertIn("request index 0x00", problem)
        self.assertIn("this carries 10", problem)

    def test_an_encrypt_reply_of_the_wrong_length_for_its_info_type_fails(self):
        # Info type 0x01 echoed at byte 0, but the 2-byte body of type 0x00.
        bad = w.frame(w.MESG_ENCRYPT_ENABLE_ID, bytes([0x01, 0x02])).hex()
        problem = self.only_problem(f"- < {bad}\n")
        self.assertIn("info type 0x01 at byte 0", problem)
        self.assertIn("declares 5 bytes "
                      "(ANTW_MESG_CONFIG_ENCRYPT_REQ_CONFIG_ID_SIZE)", problem)

    def test_an_undeclared_encrypt_info_type_fails(self):
        # 4d-request-encrypt-enable-3 exists precisely because index 3 must be
        # refused with a 0x40 rather than answered with a short 0x7D.
        bad = w.frame(w.MESG_ENCRYPT_ENABLE_ID, bytes([0x03, 0x00])).hex()
        problem = self.only_problem(f"- < {bad}\n")
        self.assertIn("info type 0x03", problem)
        self.assertIn("not one it declares a reply shape for", problem)

    def test_a_reply_shape_never_widens_the_command(self):
        # The failure mode the fix had to avoid: folding the reply lengths into
        # payload_len would make every one of these legal in the host column.
        for msg_id, payload, expect in (
                (w.MESG_CONFIG_ADV_BURST_ID, bytes([3, 3, 0, 0]),
                 "declares a command payload of 9..12, this carries 4"),
                (w.MESG_ENCRYPT_ENABLE_ID, bytes([0x00, 0x02]),
                 "declares a command payload of 4, this carries 2"),
                (w.MESG_ENCRYPT_ENABLE_ID, bytes([0x02]) + bytes(19),
                 "declares a command payload of 4, this carries 20"),
                (w.MESG_EVENT_FILTER_CONFIG_ID, bytes([0, 0, 0, 0]),
                 "declares a command payload of 2..3, this carries 4")):
            with self.subTest(msg_id=msg_id, length=len(payload)):
                problem = self.only_problem(
                    f"- > {w.frame(msg_id, payload).hex()}\n")
                self.assertIn(expect, problem)

    def test_the_single_shape_selector_is_exact(self):
        # 0x79's command is two bytes and its reply three. payload_len still
        # reads '2..3' because ant_conformance.py derives its malformed cases
        # from those bounds, but the reply side is pinned exactly - so the
        # asymmetry the round trip exists to catch is checked in one direction
        # even though the union is still tolerated in the other.
        self.assert_clean(
            f"- < {w.frame(w.MESG_EVENT_FILTER_CONFIG_ID, bytes(3)).hex()}\n")
        problem = self.only_problem(
            f"- < {w.frame(w.MESG_EVENT_FILTER_CONFIG_ID, bytes(2)).hex()}\n")
        self.assertIn("its one declared reply shape declares 3 bytes "
                      "(ANTW_MESG_EVENT_FILTER_CONFIG_REQ_SIZE)", problem)

    def test_an_unrequested_indexed_reply_falls_back_to_the_declared_set(self):
        # A real host log may start mid-session, so a reply with no request in
        # evidence is checked against the set rather than rejected outright.
        # Pinned because it is the one place this model is deliberately weaker,
        # and a future change that makes it stricter should be deliberate.
        self.assert_clean(f"- < {self.ADV_BURST_CAPS}\n")
        self.assert_clean(f"- < {self.ADV_BURST_CONFIG}\n")
        problem = self.only_problem(
            f"- < {w.frame(w.MESG_CONFIG_ADV_BURST_ID, bytes(5)).hex()}\n")
        self.assertIn("No MESG_REQUEST for 0x78 appears earlier", problem)

    def test_every_reply_shape_resolves_to_a_committed_size_constant(self):
        # The YAML names a size constant and the generator resolves the number
        # from `message_sizes`, so the number exists once. Assert the link held:
        # a shape whose length stopped matching its constant would mean the
        # generator resolved something else, and the transcript would then be
        # checked against a number nothing in the firmware emits.
        seen = 0
        for msg_id, info in w.MESSAGES.items():
            reply = info.get("reply_len")
            if not reply:
                continue
            self.assertIn(reply["selector"],
                          ("reply_byte_0", "request_index", "none"))
            for shape in reply["shapes"]:
                seen += 1
                self.assertEqual(
                    shape["len"], getattr(w, shape["size"]),
                    f"0x{msg_id:02X} reply shape claims {shape['len']} bytes "
                    f"but {shape['size']} is {getattr(w, shape['size'])}")
        self.assertGreaterEqual(seen, 6, "the reply_len model is not wired up")


class TestTheBurstChannelCeiling(ReplayMixin, unittest.TestCase):
    """The five-bit ceiling, checked only where a burst header imposes it.

    The bug this pins: the ceiling was applied to byte 0 of every
    RESPONSE_EVENT, which is a plain uint8 echoing whatever the command
    carried. `frame/sync-in-payload` sends channel 0xA4 deliberately - it is
    how a SYNC byte is made to land inside a payload - and the harness called
    the dongle's correct echo a defect.

    Renaming that case to carry the `malformed` token would also have made the
    suite green, and was rejected: the record is conformant, the waiver is for
    records that are not, and spending it here would have recorded a true fact
    (this frame is unusual) as a false one (this frame is wrong). Scoping the
    check keeps the information.
    """

    HEADER = HEADER_PREFIX + " <seconds|-> <dir> <framed message, hex>\n"

    CAPS_8 = w.frame(w.MESG_CAPABILITIES_ID, w.OBSERVED_CAPABILITIES).hex()
    CAPS_32 = w.frame(w.MESG_CAPABILITIES_ID,
                      bytes([32]) + w.OBSERVED_CAPABILITIES[1:]).hex()

    @staticmethod
    def burst(header):
        return w.frame(w.MESG_BURST_DATA_ID,
                       bytes([header]) + bytes(range(8))).hex()

    def classify_text(self, body, name="in-memory.antser"):
        return self.classify(read_antser(self.HEADER + body), name)

    def test_the_record_that_used_to_fail_is_conformant(self):
        # a4026ea4e08c in, a40340a46e002d back: channel 0xA4 both ways.
        fatal, waived = self.classify_text(
            "# case frame/sync-in-payload\n"
            "- > a4026ea4e08c\n"
            "- < a40340a46e002d\n")
        self.assertEqual(fatal, [])
        self.assertEqual(waived, {})

    def test_a_burst_header_above_the_device_channel_count_fails(self):
        last = w.BURST_HEADER["BURST_HEADER_LAST"]
        fatal, waived = self.classify_text(
            f"- < {self.CAPS_8}\n- > {self.burst(last | 31)}\n")
        self.assertEqual(waived, {})
        self.assertEqual(len(fatal), 1)
        self.assertIn("burst header 0x9F addresses channel 31", fatal[0][1][0])
        self.assertIn("advertises 8 channels", fatal[0][1][0])

    def test_a_burst_header_inside_the_count_is_fine(self):
        last = w.BURST_HEADER["BURST_HEADER_LAST"]
        for channel in (0, 7):
            with self.subTest(channel=channel):
                fatal, _ = self.classify_text(
                    f"- < {self.CAPS_8}\n- > {self.burst(last | channel)}\n")
                self.assertEqual(fatal, [])

    def test_the_ceiling_comes_from_the_transcript_not_from_this_file(self):
        # The same burst that fails against an 8-channel device passes against
        # a 32-channel one. This is what lets an ant_core transcript be
        # replayed by an unmodified harness.
        last = w.BURST_HEADER["BURST_HEADER_LAST"]
        fatal, _ = self.classify_text(
            f"- < {self.CAPS_32}\n- > {self.burst(last | 31)}\n")
        self.assertEqual(fatal, [])

    def test_a_bad_burst_header_is_still_waivable_by_case_name(self):
        # malformed-oob-index sends 0xFF as byte 0 on every message, burst
        # included, and the waiver has to reach it like any other problem.
        # The capabilities reply sits outside the case on purpose: a record
        # inside a malformed case is not trusted to state the device's channel
        # count either, which is the same waiver working in the other
        # direction.
        fatal, waived = self.classify_text(
            f"# case 4d-request-capabilities-0/valid\n- < {self.CAPS_8}\n"
            "# case 50-burst-data/malformed-oob-index\n"
            f"- > {self.burst(0xFF)}\n")
        self.assertEqual(fatal, [])
        self.assertIn("50-burst-data/malformed-oob-index", waived)

    def test_the_ceiling_is_not_applied_to_a_plain_response_channel(self):
        # Every non-burst message: byte 0 of the 0x40 is a uint8 and 0xA4 is a
        # legal value for it. If this ever fails, the ceiling has leaked back
        # out of the burst header.
        for answered in (w.MESG_ANTLIB_CONFIG_ID, w.MESG_SDU_SET_MASK_ID,
                         w.MESG_ENCRYPT_ENABLE_ID):
            with self.subTest(answered=answered):
                frame = w.frame(w.MESG_RESPONSE_EVENT_ID,
                                bytes([0xA4, answered, 0x00]))
                fatal, _ = self.classify_text(f"- < {frame.hex()}\n")
                self.assertEqual(fatal, [])

    def test_it_is_applied_to_a_response_that_answers_a_burst(self):
        # Scoped, not deleted. The bridge answers a burst with
        # send_response(header & 0x1F, ...), so a channel above the mask in
        # that reply could not have come from a burst header.
        for answered in sorted(BURST_HEADER_IDS):
            with self.subTest(answered=answered):
                frame = w.frame(w.MESG_RESPONSE_EVENT_ID,
                                bytes([0xA4, answered, 0x00]))
                fatal, _ = self.classify_text(f"- < {frame.hex()}\n")
                self.assertEqual(len(fatal), 1)
                self.assertIn("cannot be 164", fatal[0][1][0])

    def test_a_transcript_with_no_capabilities_reply_claims_nothing(self):
        # The documented hole, pinned so that closing it is a deliberate edit
        # rather than a silent change of meaning. Without a capabilities reply
        # the transcript never says how many channels the device has, and the
        # five-bit field cannot be overflowed, so there is nothing left to
        # check.
        fatal, _ = self.classify_text(f"- > {self.burst(0x9F)}\n")
        self.assertEqual(fatal, [])


class TestTheIntegrityCheckItself(unittest.TestCase):
    """A hash check nobody has watched fail is a hash check that does nothing.

    Driven against transcripts written to a temporary directory, with a summary
    built the way `ant_conformance.summarise()` builds one, so the assertions
    are about the rule and not about the committed bytes.
    """

    BODY = (HEADER_PREFIX + " <seconds|-> <dir> <framed message, hex>\n"
            "# case 41-unassign-channel/valid\n"
            "- > a4014a00ef\n"
            "- < a4016f00ca\n"
            "# case 42-assign-channel/valid\n"
            "- > a40342000000e5\n"
            "- < a40340004200a5\n")

    def write(self, text):
        directory = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, directory, True)
        path = os.path.join(directory, "t.antser")
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        return path

    def summarise(self, text):
        """The shape ant_conformance.summarise() writes, from the same rule."""
        records = read_antser(text)
        per_case = {}
        for record in records:
            per_case.setdefault(record.case or "?", []).append(
                f"{record.direction}{record.data.hex()}")
        return {
            "antser_path": "t.antser",
            "sha256": hashlib.sha256(text.encode("utf-8")).hexdigest(),
            "records": len(records),
            "case_index": [
                {"name": name, "records": len(lines),
                 "sha256": hashlib.sha256(
                     "\n".join(lines).encode("utf-8")).hexdigest()}
                for name, lines in per_case.items()],
        }

    def test_an_untouched_transcript_has_no_problems(self):
        self.assertEqual(
            integrity_problems(self.write(self.BODY),
                               self.summarise(self.BODY)), [])

    def test_one_flipped_byte_is_caught_and_localised(self):
        # The mutation the replay rules cannot see: a well-formed frame whose
        # content moved. The case index is what turns "this file changed" into
        # "this case changed".
        changed = self.BODY.replace("- < a40340004200a5\n",
                                    "- < a4034000420ba6\n")
        problems = integrity_problems(self.write(changed),
                                      self.summarise(self.BODY))
        self.assertEqual(len(problems), 2, problems)
        self.assertIn("sha256 is", problems[0])
        self.assertIn("42-assign-channel/valid", problems[1])
        self.assertIn("a byte inside this case has changed", problems[1])

    def test_a_deleted_record_is_caught(self):
        shorter = self.BODY.replace("- < a4016f00ca\n", "")
        problems = integrity_problems(self.write(shorter),
                                      self.summarise(self.BODY))
        joined = "\n".join(problems)
        self.assertIn("3 records, the summary says 4", joined)
        self.assertIn("'41-unassign-channel/valid' has 1 records, the summary "
                      "says 2", joined)

    def test_a_case_that_vanished_and_a_case_that_appeared_are_both_named(self):
        renamed = self.BODY.replace("# case 42-assign-channel/valid",
                                    "# case 42-assign-channel/malformed-short")
        problems = integrity_problems(self.write(renamed),
                                      self.summarise(self.BODY))
        joined = "\n".join(problems)
        self.assertIn("'42-assign-channel/valid' is in the summary and not in "
                      "the file", joined)
        self.assertIn("'42-assign-channel/malformed-short' is in the file and "
                      "not in the summary", joined)

    def test_the_committed_summary_is_found_and_names_a_real_transcript(self):
        # The lookup, not the hashes: if archive/benchmarks/ ever stops being
        # walked correctly, TestRealCaptures' integrity test would pass by
        # checking nothing, and its own guard would be the only thing left.
        summaries = recorded_summaries()
        self.assertIn("conformance-sdk-ant.antser", summaries)
        source, summary = summaries["conformance-sdk-ant.antser"]
        self.assertTrue(source.endswith(".json"))
        self.assertEqual(len(summary["case_index"]), summary["cases"])


class TestTheParserItself(unittest.TestCase):
    """A harness with a broken reader reports success on nothing."""

    HEADER = HEADER_PREFIX + " <seconds|-> <dir> <framed message, hex>\n"

    def test_minimal_file(self):
        records = read_antser(self.HEADER + "- > a4014a00ef\n")
        self.assertEqual(len(records), 1)
        self.assertIsNone(records[0].seconds)
        self.assertEqual(records[0].direction, ">")
        self.assertEqual(records[0].data.hex(), "a4014a00ef")

    def test_timestamps_and_trailing_comments(self):
        records = read_antser(
            self.HEADER + "0.014200 < a4016f20ea  # startup\n")
        self.assertEqual(records[0].seconds, 0.0142)
        self.assertEqual(records[0].data.hex(), "a4016f20ea")

    def test_case_markers_attach_to_following_records(self):
        records = read_antser(self.HEADER
                              + "# case one\n- > a4014a00ef\n"
                              + "# case two/malformed\n- > a4024d00541b\n")
        self.assertEqual([r.case for r in records], ["one", "two/malformed"])
        self.assertFalse(declares_nonconformance(records[0]))
        self.assertTrue(declares_nonconformance(records[1]))

    def test_a_record_with_no_case_can_never_waive_anything(self):
        records = read_antser(self.HEADER + "- > a4014a00ef\n")
        self.assertIsNone(records[0].case)
        self.assertFalse(declares_nonconformance(records[0]))

    def test_a_file_with_no_header_is_refused(self):
        with self.assertRaises(AntserError):
            read_antser("- > a4014a00ef\n")

    def test_a_line_it_cannot_read_is_refused(self):
        for bad in ("a4014a00ef",            # no columns
                    "- ! a4014a00ef",        # not a direction
                    "- > zz014a00ef",        # not hex
                    "0.1 > a4014a00e"):      # odd digit count
            with self.subTest(bad):
                with self.assertRaises(AntserError):
                    read_antser(self.HEADER + bad + "\n")

    def test_blank_lines_and_comments_are_skipped(self):
        self.assertEqual(
            read_antser(self.HEADER + "\n# just a note\n\n"), [])


if __name__ == "__main__":
    unittest.main()
