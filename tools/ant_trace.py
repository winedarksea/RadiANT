#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Normalise Garmin's `Device0.txt` into `.antser`, and read `.antser` back.

Two directions, and the second one is the one that works today.

**Forward.** `ANT_DLL.dll` exports `ANT_SetDebugLogDirectory` (ordinal 132).
Call it before `ANT_Init` and the DLL writes `Device0.txt`, Garmin's own account
of Garmin's own `Tx`/`Rx` bytes. That file is the third implementation this
project needed and never had: the checksum bug survived a full green test suite
because `src/ant_serial_bridge.c` and `tools/ant_probe.py` made the same mistake
in the same direction and agreed perfectly with each other and with nothing else
in the world. A transcript from a stack neither of them wrote is what catches a
wrong *rule*, and a constant-for-constant cross-check never can.

    python tools/ant_trace.py <logdir>/Device0.txt --out zwift-pairing.antser

**THE FORWARD PARSER IS UNVALIDATED AGAINST A REAL SAMPLE.** `Device0.txt`'s
line format is documented nowhere - not in the ANT PC SDK, not in
`archive/host-api/`, not on thisisant.com - and no sample was available when
this was written. What is below is a set of *strict candidate dialects* written
against the plausible shapes, not a parser written against a specimen. It
therefore refuses to guess: an unrecognised line is a hard error naming the line
number and its text, never a silently skipped one, because a parser that skips
what it does not understand turns a golden transcript into an incomplete one
that still looks fine.

To validate it, a human must:

1. Produce a `Device0.txt` - any Windows host with the ANT driver, `ANT_DLL.dll`
   and an app that drives a session; Zwift pairing a sensor is the case worth
   having (see `archive/captures/serial/README.md` for the shopping list).
2. Run `python tools/ant_trace.py Device0.txt --dump-unparsed`. If every line
   parses, the dialect guessed right and the resulting `.antser` is good.
3. If it does not, the dump names the lines. Add one `Dialect` below - a name, a
   prose note, a compiled regex and a timestamp kind. Adding an input dialect is
   deliberately a function, not a rewrite.
4. Write the real line format into the "On `Device0.txt`'s own format" section
   of `archive/captures/serial/README.md`, which currently says it is unknown.

Two assumptions are worth naming because they are the ones most likely to be
wrong, and both are cheap to check against a sample: that `Tx` means
host-to-dongle (the DLL's point of view, so `Tx` is what the host wrote), and
that the bytes logged are the complete frame including SYNC and checksum. If the
frames turn out to be logged without SYNC, `frames_from_bytes()` is where that is
absorbed.

**Reverse.** `.antser` back to annotated text, decoding message ids and payloads
through `tools/ant_wire.py`. This needs no sample, is useful the moment the
first conformance transcript exists, and is what makes a byte diff readable:

    python tools/ant_trace.py conformance-sdk-ant.antser --to-text
    python tools/ant_trace.py annotated.txt --from-text --out round-trip.antser

The annotated form is `.antser` with a trailing `# ...` comment on each line, so
it reads back losslessly. Readers here tolerate trailing comments; the writer
never emits them into a `.antser`, because the format archived in
`archive/captures/serial/` is meant to stay one `sscanf` per line.

This module owns the `.antser` reader and writer for the whole tools directory -
`tools/ant_conformance.py` imports them rather than growing a second copy. It is
standard library plus `ant_wire`, and touches no hardware.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from typing import Callable, Iterable, Sequence

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

import ant_wire as wire  # noqa: E402

# The two direction markers, as `archive/captures/serial/README.md` fixes them.
# Direction is a column rather than two files because ordering *across*
# directions is the thing under test: a reply that arrives before the command
# that caused it, or an unsolicited event interleaved into a request/response
# pair, is invisible if the two directions are recorded separately.
HOST_TO_DONGLE = ">"
DONGLE_TO_HOST = "<"

# First line of every file we write. A reader that does not recognise it should
# refuse rather than guess at the columns.
ANTSER_HEADER = ("# ant serial capture v1: <seconds|-> <dir> "
                 "<framed message, hex>")

# Marker comment introducing a named group of records. Comments are already
# legal in the format, so this costs nothing to a `sscanf` reader that ignores
# them - and it is what lets tools/ant_conformance.py --compare say "the first
# difference is in case 42-assign-channel/short" instead of "byte 8417".
CASE_MARKER = "# case "


class TraceError(Exception):
    """A line, a frame or a file that this tool refuses to interpret.

    Carries the source line number wherever there is one. The whole point of
    this tool failing loudly is that the message names the thing it could not
    read, so raising this without context defeats it.
    """


@dataclass(frozen=True)
class Record:
    """One framed message, in one direction, at one time.

    `seconds` is `None` for a transcript recorded with the timestamp column
    suppressed - which is what `tools/ant_conformance.py` writes, because Tier
    1's acceptance criterion is that two files are byte-identical and real
    timestamps never repeat.

    `case` is the nearest preceding `# case` marker, or None. It is metadata
    about the file, not part of the record on the wire.
    """

    seconds: float | None
    direction: str
    data: bytes
    case: str | None = None

    def __post_init__(self) -> None:
        if self.direction not in (HOST_TO_DONGLE, DONGLE_TO_HOST):
            raise TraceError(f"direction must be '>' or '<', not "
                             f"{self.direction!r}")

    @property
    def hex(self) -> str:
        return self.data.hex()

    def unframed(self):
        """(msg_id, payload), or None if the bytes are not a valid frame.

        A malformed frame is worth *recording* - a deliberately bad checksum is
        one of the conformance cases - so this returns None rather than raising.
        """
        return wire.unframe(self.data)


# ---------------------------------------------------------------------------
# Frame splitting
# A log line, or a USB packet, may carry more than one frame, and a Device0.txt
# line plausibly carries a whole write buffer. Splitting on the length byte and
# checking the checksum is the only way to tell where one ends - and it is also
# the cheapest evidence that the byte extraction guessed right, because a wrong
# guess almost never produces a run of frames that all check out.
# ---------------------------------------------------------------------------


def frames_from_bytes(buf: bytes, *, strict: bool = True) -> list[bytes]:
    """Split a byte run into complete frames.

    With `strict`, anything that is not a well-formed frame - a byte that is not
    SYNC where a frame should start, a truncated tail, a checksum that does not
    match - raises. That is deliberate: on the forward path a checksum failure
    almost certainly means this tool mis-read the line rather than that Garmin's
    DLL emitted a bad frame, and quietly keeping the bytes would launder a
    parser bug into a golden fixture.
    """
    frames: list[bytes] = []
    i = 0
    while i < len(buf):
        if buf[i] != wire.SYNC_TX:
            if not strict:
                break
            raise TraceError(
                f"expected SYNC 0x{wire.SYNC_TX:02X} at offset {i}, found "
                f"0x{buf[i]:02X} in {buf.hex()}"
                + (" (0xA5 is the bidirectional-variant SYNC, which this "
                   "dongle never speaks)" if buf[i] == wire.SYNC_RX else ""))
        if i + wire.MSG_OVERHEAD > len(buf):
            if not strict:
                break
            raise TraceError(f"truncated frame at offset {i} in {buf.hex()}")
        total = buf[i + 1] + wire.MSG_OVERHEAD
        chunk = buf[i:i + total]
        if len(chunk) < total:
            if not strict:
                break
            raise TraceError(
                f"frame at offset {i} declares {buf[i + 1]} payload bytes but "
                f"only {len(chunk) - wire.MSG_OVERHEAD} are present in "
                f"{buf.hex()}")
        if wire.unframe(chunk) is None:
            if not strict:
                break
            raise TraceError(
                f"checksum mismatch on {chunk.hex()} - the XOR covers the SYNC "
                f"byte too (tools/ant_wire.checksum); a run of these usually "
                f"means the bytes were extracted from the wrong columns")
        frames.append(bytes(chunk))
        i += total
    return frames


# ---------------------------------------------------------------------------
# Reading and writing .antser
# ---------------------------------------------------------------------------

# `<seconds|-> <dir> <hex>`, with an optional trailing comment. The writer never
# emits the comment; the reader accepts it so that the annotated form produced
# by --to-text reads back losslessly.
_ANTSER_LINE = re.compile(
    r"^(?P<ts>-|\d+(?:\.\d+)?)\s+(?P<dir>[<>])\s+(?P<hex>[0-9a-fA-F]+)"
    r"\s*(?:#.*)?$")


def read_antser(text: str, *, source: str = "<string>") -> list[Record]:
    """Parse a `.antser` transcript. Raises TraceError on any line it cannot read.

    Comments and blank lines are skipped, `# case` markers are remembered, and
    everything else must match the format exactly. There is no lenient mode: a
    capture is a fixture, and a fixture that half-parses is worse than one that
    does not parse at all.
    """
    records: list[Record] = []
    case: str | None = None

    for lineno, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#"):
            if line.startswith(CASE_MARKER):
                case = line[len(CASE_MARKER):].strip()
            continue

        match = _ANTSER_LINE.match(line)
        if match is None:
            raise TraceError(
                f"{source}:{lineno}: not a .antser record: {raw!r}\n"
                f"  expected '<seconds|-> <dir> <hex>', e.g. "
                f"'0.014200 < a4016f20fa' or '- > a4014a00ef'")

        hex_text = match.group("hex")
        if len(hex_text) % 2:
            raise TraceError(f"{source}:{lineno}: odd number of hex digits "
                             f"in {hex_text!r}")
        data = bytes.fromhex(hex_text)
        # One record is one frame. A line carrying two would silently change the
        # record count between a writer and a reader, so say so instead.
        extra = frames_from_bytes(data, strict=False)
        if len(extra) > 1:
            raise TraceError(
                f"{source}:{lineno}: {len(extra)} frames on one line; a "
                f".antser record is exactly one frame")

        seconds = None if match.group("ts") == "-" else float(match.group("ts"))
        records.append(Record(seconds, match.group("dir"), data, case))

    return records


def read_antser_file(path: str) -> list[Record]:
    with open(path, "r", encoding="utf-8") as handle:
        return read_antser(handle.read(), source=path)


def format_antser(records: Sequence[Record],
                  notes: Iterable[str] = ()) -> str:
    """Render records as a `.antser` file body.

    Returns a string rather than writing, because the byte-identity that the
    whole Tier 1 gate rests on is much easier to unit-test on a value than on a
    file. Nothing here reads a clock, a path or an environment variable: two
    calls with equal records produce equal text, and that property is what
    `tools/test_ant_conformance.py` asserts.
    """
    lines = [ANTSER_HEADER]
    lines.extend(f"# {note}" for note in notes)

    case: object = object()  # a sentinel no Record can carry
    for record in records:
        if record.case != case:
            case = record.case
            if case is not None:
                lines.append(f"{CASE_MARKER}{case}")
        stamp = "-" if record.seconds is None else f"{record.seconds:.6f}"
        lines.append(f"{stamp} {record.direction} {record.hex}")

    return "\n".join(lines) + "\n"


def write_antser(path: str, records: Sequence[Record],
                 notes: Iterable[str] = ()) -> str:
    """Write a `.antser` file and return the text written.

    `newline=""` keeps the line endings the ones in the text - LF - on Windows
    too. A capture whose bytes depend on which operating system wrote it cannot
    be diffed against one written on the other, and the diff is the product.
    """
    text = format_antser(records, notes)
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)
    return text


# ---------------------------------------------------------------------------
# Decoding, for the reverse direction
# ---------------------------------------------------------------------------


def _response_event(payload: bytes) -> str:
    """[channel, message id or 0x01, code] - the message every command gets."""
    if len(payload) < 3:
        return "malformed RESPONSE_EVENT"
    channel, middle, code = payload[0], payload[1], payload[2]
    if middle == wire.MESG_EVENT_ID:
        name = wire.EVENT_CODES_BY_VALUE.get(code, f"event 0x{code:02X}")
        return f"channel {channel} event {name}"
    reply_to = wire.MESSAGE_NAMES.get(middle, f"0x{middle:02X}")
    name = wire.RESPONSE_CODES_BY_VALUE.get(code, f"code 0x{code:02X}")
    return f"channel {channel} reply to {reply_to}: {name}"


def _capabilities(payload: bytes) -> str:
    parts = []
    for index, (field, bits) in sorted(wire.CAPABILITY_BYTES.items()):
        if index >= len(payload):
            continue
        value = payload[index]
        if not bits:
            parts.append(f"{field}={value}")
            continue
        set_bits = [name for mask, name, _ in bits if value & mask]
        parts.append(f"{field}=0x{value:02X}"
                     + (" [" + " ".join(set_bits) + "]" if set_bits else ""))
    return ", ".join(parts)


def _channel_id(payload: bytes) -> str:
    if len(payload) < 5:
        return "malformed CHANNEL_ID"
    number = payload[1] | (payload[2] << 8)
    return (f"channel {payload[0]} device #{number} "
            f"type 0x{payload[3]:02X} trans 0x{payload[4]:02X}")


def _data(payload: bytes) -> str:
    """Broadcast / acknowledged data, with the extended fields if they are on."""
    if not payload:
        return "empty"
    text = f"channel {payload[0]} data {payload[1:9].hex()}"
    if len(payload) > 9:
        flag = payload[9]
        names = [name for mask, name in wire.EXT_FLAGS_BY_VALUE.items()
                 if flag & mask]
        text += (f" ext 0x{flag:02X}"
                 + (" [" + " ".join(names) + "]" if names else "")
                 + f" {payload[10:].hex()}")
    return text


def _burst(payload: bytes) -> str:
    if not payload:
        return "empty"
    header = payload[0]
    channel = header & wire.BURST_HEADER["BURST_HEADER_CHANNEL_MASK"]
    seq = ((header >> wire.BURST_HEADER["BURST_HEADER_SEQ_SHIFT"])
           & wire.BURST_HEADER["BURST_HEADER_SEQ_MASK"])
    last = " last" if header & wire.BURST_HEADER["BURST_HEADER_LAST"] else ""
    return f"channel {channel} seq {seq}{last} {payload[1:].hex()}"


def _request(payload: bytes) -> str:
    if len(payload) < 2:
        return "malformed REQUEST"
    wanted = wire.MESSAGE_NAMES.get(payload[1], f"0x{payload[1]:02X}")
    # For several messages byte 0 is not a channel at all - it is a mask
    # number, an encryption info type, or 0/1 for advanced burst
    # capabilities/configuration - so call it an index and let the reader
    # decide.
    return f"{wanted} at index {payload[0]}"


def _startup(payload: bytes) -> str:
    if not payload:
        return "malformed STARTUP"
    name = wire.STARTUP_REASONS_BY_VALUE.get(payload[0],
                                             f"reason 0x{payload[0]:02X}")
    return name


def _version(payload: bytes) -> str:
    return repr(payload.split(b"\x00")[0].decode("ascii", errors="replace"))


_DECODERS: dict[int, Callable[[bytes], str]] = {
    wire.MESG_RESPONSE_EVENT_ID: _response_event,
    wire.MESG_CAPABILITIES_ID: _capabilities,
    wire.MESG_CHANNEL_ID_ID: _channel_id,
    wire.MESG_BROADCAST_DATA_ID: _data,
    wire.MESG_ACKNOWLEDGED_DATA_ID: _data,
    wire.MESG_BURST_DATA_ID: _burst,
    wire.MESG_ADV_BURST_DATA_ID: _burst,
    wire.MESG_REQUEST_ID: _request,
    wire.MESG_STARTUP_MESG_ID: _startup,
    wire.MESG_VERSION_ID: _version,
}


def describe_frame(data: bytes) -> str:
    """One line of prose for one frame, or a complaint if it is not one.

    Deliberately short and deliberately not a table: this hangs off the end of a
    hex line, and its job is to let somebody reading a diff at 1 a.m. see which
    message is which without counting bytes.
    """
    parsed = wire.unframe(data)
    if parsed is None:
        if not data:
            return "empty"
        if data[0] != wire.SYNC_TX:
            return f"not a frame (first byte 0x{data[0]:02X}, not SYNC)"
        return "not a frame (bad checksum or truncated)"

    msg_id, payload = parsed
    name = wire.MESSAGE_NAMES.get(msg_id, "unknown message")
    text = f"{name} (0x{msg_id:02X}) len={len(payload)}"
    decoder = _DECODERS.get(msg_id)
    if decoder is not None:
        text += "  " + decoder(payload)
    return text


def annotate(records: Sequence[Record]) -> str:
    """`.antser` plus a trailing comment per line, for human reading.

    The result is still parseable by `read_antser`, which is what makes the
    round trip in `tools/test_ant_trace.py` a real check rather than a
    formality.
    """
    lines = [ANTSER_HEADER,
             "# annotated by tools/ant_trace.py --to-text; the trailing "
             "comments are not part of the format"]
    case: object = object()
    for record in records:
        if record.case != case:
            case = record.case
            if case is not None:
                lines.append(f"{CASE_MARKER}{case}")
        stamp = "-" if record.seconds is None else f"{record.seconds:.6f}"
        arrow = "host->dongle" if record.direction == HOST_TO_DONGLE \
            else "dongle->host"
        lines.append(f"{stamp} {record.direction} {record.hex}"
                     f"  # {arrow}  {describe_frame(record.data)}")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Forward direction: Device0.txt dialects
#
# Every dialect is a strict regex plus a timestamp kind. Strictness is the whole
# design: a permissive pattern that matches "something that looks like hex after
# something that looks like a direction" will happily read a summary line, a
# byte count or a truncated buffer as a frame, and the resulting fixture is
# wrong in a way nothing downstream can detect. Better to refuse and be told.
#
# ADDING A DIALECT: write one `Dialect(...)` entry. Nothing else changes.
# ---------------------------------------------------------------------------

# Timestamp kinds, in the forms a log plausibly uses.
TS_NONE = "none"          # no timestamp column
TS_SECONDS = "seconds"    # a float, absolute or relative
TS_MILLIS = "millis"      # an integer count of milliseconds
TS_CLOCK = "clock"        # HH:MM:SS(.fff), wall clock


@dataclass(frozen=True)
class Dialect:
    """One candidate `Device0.txt` line format.

    `pattern` must capture `dir` and `hex`, and `ts` when `ts_kind` is not
    TS_NONE. `note` says what this dialect is a guess *at*, because none of
    them has been seen in the wild and the next person needs to know which
    guesses were already made.
    """

    name: str
    note: str
    pattern: re.Pattern
    ts_kind: str


# Direction tokens. Assumption, flagged in the module docstring: `Tx` is the
# DLL's own point of view, so it is the host writing to the dongle.
_DIR_MAP = {
    "tx": HOST_TO_DONGLE,
    "rx": DONGLE_TO_HOST,
    ">": HOST_TO_DONGLE,
    "<": DONGLE_TO_HOST,
}

# `(?![A-Za-z])` rather than `\b`: `\b` is not a boundary between `>` and a
# space, so the arrow forms would never match one.
_DIR = r"(?P<dir>[Tt]x|[Rr]x|TX|RX|[<>])(?![A-Za-z])"
_CLOCK = r"(?P<ts>\d{1,2}:\d{2}:\d{2}(?:[.,]\d{1,6})?)"
_FLOAT = r"(?P<ts>\d+\.\d+)"
_INT = r"(?P<ts>\d+)"
# Hex bytes, spaced, comma-separated, or each in its own brackets.
_HEX_SPACED = r"(?P<hex>[0-9A-Fa-f]{2}(?:[ ,\t]+[0-9A-Fa-f]{2})*)"
_HEX_BRACKETED = r"(?P<hex>(?:\[[0-9A-Fa-f]{2}\])+)"
_HEX_RUN = r"(?P<hex>[0-9A-Fa-f]{8,})"

DIALECTS: tuple[Dialect, ...] = (
    Dialect(
        "antser",
        "our own format, so that a .antser can be fed back through this tool "
        "and re-annotated without a second entry point",
        _ANTSER_LINE,
        TS_SECONDS,
    ),
    Dialect(
        "clock-dir-bracketed",
        "guess: wall-clock stamp, a Tx/Rx token, and each byte in its own "
        "brackets - the shape the ANT PC library's DSIDebug output is "
        "described as having",
        re.compile(rf"^{_CLOCK}\s+{_DIR}[^\[]*{_HEX_BRACKETED}\s*$"),
        TS_CLOCK,
    ),
    Dialect(
        "clock-dir-spaced",
        "guess: wall-clock stamp, a Tx/Rx token, and space-separated hex",
        re.compile(rf"^{_CLOCK}\s*[,:]?\s+{_DIR}[^0-9A-Fa-f]*"
                   rf"{_HEX_SPACED}\s*$"),
        TS_CLOCK,
    ),
    Dialect(
        "millis-dir-spaced",
        "guess: a millisecond counter rather than a clock; same otherwise",
        re.compile(rf"^{_INT}\s*[,:]?\s+{_DIR}[^0-9A-Fa-f]*"
                   rf"{_HEX_SPACED}\s*$"),
        TS_MILLIS,
    ),
    Dialect(
        "seconds-dir-spaced",
        "guess: a fractional-second counter; same otherwise",
        re.compile(rf"^{_FLOAT}\s*[,:]?\s+{_DIR}[^0-9A-Fa-f]*"
                   rf"{_HEX_SPACED}\s*$"),
        TS_SECONDS,
    ),
    Dialect(
        "dir-run",
        "guess: no timestamp at all, a direction token and an unseparated run "
        "of hex. Last because it is the loosest and would shadow the others",
        re.compile(rf"^{_DIR}[^0-9A-Fa-f]*{_HEX_RUN}\s*$"),
        TS_NONE,
    ),
)

# Lines that are certainly not records, matched exactly rather than skipped by
# guesswork. Anything not on this list and not matching the dialect is an error.
_IGNORABLE = re.compile(
    r"^\s*$"                                  # blank
    r"|^\s*[-=*_]{3,}\s*$"                    # rule
    r"|^\s*#",                                # comment
    re.IGNORECASE)


def _parse_timestamp(text: str, kind: str) -> float | None:
    if kind == TS_NONE:
        return None
    if kind == TS_SECONDS:
        return None if text == "-" else float(text)
    if kind == TS_MILLIS:
        return int(text) / 1000.0
    if kind == TS_CLOCK:
        body, _, frac = text.replace(",", ".").partition(".")
        hours, minutes, seconds = (int(part) for part in body.split(":"))
        total = hours * 3600 + minutes * 60 + seconds
        if frac:
            total += int(frac) / (10 ** len(frac))
        return float(total)
    raise TraceError(f"unknown timestamp kind {kind!r}")


@dataclass
class ParseResult:
    """What one pass over an input file produced."""

    dialect: str
    records: list[Record]
    unparsed: list[tuple[int, str]]
    frames: int


def parse_lines(lines: Sequence[str], dialect: Dialect, *,
                source: str = "<input>",
                collect_unparsed: bool = False) -> ParseResult:
    """Apply one dialect to a whole file.

    With `collect_unparsed` false - the default, and what a normal run does -
    the first line that does not parse raises. With it true, every such line is
    collected so `--dump-unparsed` can show a human the whole picture at once.
    """
    records: list[Record] = []
    unparsed: list[tuple[int, str]] = []
    base: float | None = None
    frames = 0

    for lineno, raw in enumerate(lines, start=1):
        line = raw.rstrip("\r\n")
        if _IGNORABLE.match(line):
            continue

        match = dialect.pattern.match(line.strip())
        if match is None:
            if collect_unparsed:
                unparsed.append((lineno, line))
                continue
            raise TraceError(
                f"{source}:{lineno}: dialect {dialect.name!r} cannot read this "
                f"line:\n  {line!r}\n"
                f"  Run again with --dump-unparsed to see every line it could "
                f"not read, then add a Dialect to tools/ant_trace.py. Do not "
                f"loosen an existing pattern: a permissive one reads summary "
                f"lines as frames.")

        token = match.group("dir").lower()
        direction = _DIR_MAP.get(token)
        if direction is None:  # pragma: no cover - the regex constrains this
            raise TraceError(f"{source}:{lineno}: unknown direction "
                             f"{token!r}")

        hex_text = re.sub(r"[\[\], \t]", "", match.group("hex"))
        if len(hex_text) % 2:
            raise TraceError(f"{source}:{lineno}: odd number of hex digits in "
                             f"{match.group('hex')!r}")
        try:
            payload = bytes.fromhex(hex_text)
        except ValueError as exc:  # pragma: no cover - the regex constrains it
            raise TraceError(f"{source}:{lineno}: {exc}") from exc

        stamp = None
        if dialect.ts_kind != TS_NONE:
            stamp = _parse_timestamp(match.group("ts"), dialect.ts_kind)
            if stamp is not None:
                # Relative to the first record, per the format contract. A wall
                # clock is only ever meaningful as an interval here anyway.
                if base is None:
                    base = stamp
                stamp = round(stamp - base, 6)

        try:
            split = frames_from_bytes(payload)
        except TraceError as exc:
            raise TraceError(f"{source}:{lineno}: {exc}") from exc

        for chunk in split:
            frames += 1
            records.append(Record(stamp, direction, chunk))

    return ParseResult(dialect.name, records, unparsed, frames)


def detect_dialect(lines: Sequence[str], *, sample: int = 200) -> Dialect:
    """Pick the dialect that reads the most of the first `sample` lines.

    Requires a clear winner: at least one record and at least 90% of the
    candidate lines. A file that half-matches two dialects is a file this tool
    does not understand, and saying so beats picking the luckier regex.
    """
    candidates = [line for line in lines[:sample]
                  if not _IGNORABLE.match(line.rstrip("\r\n"))]
    if not candidates:
        raise TraceError("nothing to parse: the file has no candidate lines")

    scores = []
    for dialect in DIALECTS:
        hits = sum(1 for line in candidates
                   if dialect.pattern.match(line.strip()))
        scores.append((hits, dialect))

    hits, best = max(scores, key=lambda pair: pair[0])
    if hits == 0 or hits < 0.9 * len(candidates):
        detail = ", ".join(f"{d.name}={h}/{len(candidates)}" for h, d in scores)
        raise TraceError(
            "no dialect reads this file.\n"
            f"  tried: {detail}\n"
            f"  first line: {candidates[0].strip()!r}\n"
            "  Device0.txt's line format is not documented anywhere and this "
            "parser is written against guesses, not a specimen. Add a Dialect "
            "to tools/ant_trace.py describing what you have, and write the "
            "real format into archive/captures/serial/README.md.")
    return best


# ---------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------


def _read_lines(path: str) -> list[str]:
    # errors="replace" rather than strict: a log written by a Windows DLL may
    # carry a stray byte in a banner line, and losing the whole capture to one
    # of those would be absurd. Anything it mangles lands in a line that then
    # fails to parse, loudly.
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.readlines()


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", help="Device0.txt, a .antser file, or "
                                      "annotated text")
    parser.add_argument("--out", metavar="FILE",
                        help="write the result here instead of to stdout")
    parser.add_argument("--dialect", choices=[d.name for d in DIALECTS],
                        help="force an input dialect instead of detecting one")
    parser.add_argument("--to-text", action="store_true",
                        help="reverse direction: read .antser and write "
                             "annotated text, decoding every message id and "
                             "payload through tools/ant_wire.py")
    parser.add_argument("--from-text", action="store_true",
                        help="read annotated text (or .antser) and write "
                             "plain .antser")
    parser.add_argument("--dump-unparsed", action="store_true",
                        help="parse everything it can and list every line it "
                             "could not read, instead of stopping at the "
                             "first. Exits nonzero if there were any")
    parser.add_argument("--note", action="append", default=[], metavar="TEXT",
                        help="provenance comment for the output header; "
                             "repeatable. Say where the log came from")
    parser.add_argument("--json", metavar="FILE", nargs="?", const="-",
                        help="write a summary as JSON (default: stdout)")
    args = parser.parse_args()

    if args.to_text and args.from_text:
        sys.exit("--to-text and --from-text are opposite directions; pick one")

    try:
        if args.to_text or args.from_text:
            # Both directions read the same format - annotated text is .antser
            # with trailing comments - so there is one reader here, not two.
            records = read_antser_file(args.input)
            text = (annotate(records) if args.to_text
                    else format_antser(records, args.note))
            result = ParseResult("antser", records, [], len(records))
        else:
            lines = _read_lines(args.input)
            dialect = (next(d for d in DIALECTS if d.name == args.dialect)
                       if args.dialect else detect_dialect(lines))
            result = parse_lines(lines, dialect, source=args.input,
                                 collect_unparsed=args.dump_unparsed)
            notes = list(args.note) or [
                f"tools/ant_trace.py from {args.input}, dialect "
                f"{result.dialect} - PARSER UNVALIDATED AGAINST A REAL SAMPLE"]
            text = format_antser(result.records, notes)
    except TraceError as exc:
        sys.exit(f"ant_trace: {exc}")

    if args.dump_unparsed and result.unparsed:
        print(f"{len(result.unparsed)} line(s) could not be read as dialect "
              f"{result.dialect!r}:", file=sys.stderr)
        for lineno, line in result.unparsed:
            print(f"  {args.input}:{lineno}: {line!r}", file=sys.stderr)
        print("\nAdd a Dialect to tools/ant_trace.py for these, and write the "
              "real Device0.txt format into archive/captures/serial/README.md.",
              file=sys.stderr)

    if args.out:
        with open(args.out, "w", encoding="utf-8", newline="") as handle:
            handle.write(text)
        print(f"wrote {args.out}: {len(result.records)} record(s), dialect "
              f"{result.dialect}")
    else:
        sys.stdout.write(text)

    if args.json:
        summary = {
            "tool": "ant_trace.py",
            "input": args.input,
            "dialect": result.dialect,
            "records": len(result.records),
            "unparsed": len(result.unparsed),
            "host_to_dongle": sum(1 for r in result.records
                                  if r.direction == HOST_TO_DONGLE),
            "dongle_to_host": sum(1 for r in result.records
                                  if r.direction == DONGLE_TO_HOST),
            "parser_validated_against_real_sample": False,
        }
        if args.json == "-":
            json.dump(summary, sys.stdout, indent=2)
            sys.stdout.write("\n")
        else:
            with open(args.json, "w", encoding="utf-8") as handle:
                json.dump(summary, handle, indent=2)

    return 1 if result.unparsed else 0


if __name__ == "__main__":
    sys.exit(main())
