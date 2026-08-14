#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Tier 1: drive every message the bridge implements and record what came back.

One board, no radio, no sensors, no statistics. Sends every message
`dispatch()` in `src/ant_serial_bridge.c` implements - valid, then deliberately
malformed - and writes the whole conversation to a `.antser` transcript. **The
A/B is a byte diff of two transcripts; byte-identical is the pass.**

    python tools/ant_conformance.py --serial <suffix> --out sdk-ant.antser
    # reflash the other backend, then
    python tools/ant_conformance.py --serial <suffix> --out core.antser
    python tools/ant_conformance.py --compare sdk-ant.antser core.antser

Catches response codes, reply framing/sizes, error mapping, and the
leading-index-byte shapes in `handle_request()` (mask number vs. encryption
info type vs. channel in the same slot) - without a sensor, a second board, or
a single statistic.

Determinism rules, all load-bearing:

* Timestamp column is always `-` (real timestamps never repeat;
  `archive/captures/serial/README.md`).
* Every case starts from a stack reset, so a case's meaning can't depend on
  what the previous one left behind.
* Nothing here transmits: `MESG_RADIO_CW_MODE`, `MESG_RADIO_CW_INIT`,
  `MESG_OPEN_RX_SCAN_MODE` are excluded outright (see `EXCLUDED`), and
  `MESG_OPEN_CHANNEL` only runs on a channel with no id set, refused before
  the radio is touched - an open channel hearing the room would break
  byte-identical repeatability.
* Cases run in a fixed order; replies recorded in arrival order.

Cases are generated from `tools/ant_wire.py`'s `MESSAGES`/`BRIDGED_MESSAGE_IDS`,
so a message added to `protocol/ant_wire.yaml` enters the run by itself.
`CANONICAL` gives a *sensible* payload where it has an opinion; anything else
gets a case built from its declared `payload_len`.

A bench sitting: one board, no sensors, nothing on the air, ~260 cases at
roughly a second each (~5 min per backend plus reflash):

    python tools\ant_conformance.py --serial <suffix> --out conformance-sdk-ant.antser --json conformance-sdk-ant.json
    # reflash the other backend on the same board, same session
    python tools\ant_conformance.py --serial <suffix> --out conformance-core.antser --json conformance-core.json
    python tools\ant_conformance.py --compare conformance-sdk-ant.antser conformance-core.antser

`--serial` is not optional in practice: two boards enumerate as the same
`0FCF:1009`. The sdk-ant transcript is the Tier 1 reference, belongs in
`archive/captures/serial/` with its hash in the sitting's baseline JSON
(`conformance.antser_path`, `conformance.sha256`), and can only be recorded
while a working sdk-ant build exists.

The settle floor is `ant_probe.READ_TIMEOUT_MS`, deliberately a full second: a
read timeout racing a reply loses the answer - the bug that once manufactured
0.4pp of imaginary packet loss.

`--compare` reports differences by case name, not byte offset. One expected
difference: `MESG_VERSION` returns the backend's own name, so sdk-ant and
`radiant` can't and shouldn't match. Use `--allow-differing-case` for it,
recorded in `tools/ab_gates.toml` for review.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import time
from dataclasses import dataclass, field

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

import ant_trace  # noqa: E402
import ant_wire as wire  # noqa: E402

# The published ANT+ network key, imported from ant_scan.py (its canonical
# home per docs/ant-serial-protocol.md). radiant refuses unknown keys, so
# a made-up key here would manufacture a divergence unrelated to the bridge.
from ant_scan import ANT_PLUS_KEY  # noqa: E402
from ant_trace import DONGLE_TO_HOST, HOST_TO_DONGLE, Record  # noqa: E402

# The channel every case uses. Channel 0 exists on every build; anything higher
# would make the transcript depend on CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED.
CHANNEL = 0

# Out-of-range for a channel, a network number, and an SDU mask index, and not
# a defined encryption info type - so it exercises the range check whatever
# byte 0 happens to mean.
BAD_INDEX = 0xFF

# Messages never sent, whatever the YAML says. All three would break
# reproducibility: scan mode fills the transcript with whatever's in the room,
# and CW mode keys the transmitter and leaves it keyed.
EXCLUDED = {
    wire.MESG_RADIO_CW_MODE_ID:
        "keys the transmitter; a backend that bridges it would leave a carrier "
        "on the band for the rest of the run",
    wire.MESG_RADIO_CW_INIT_ID:
        "enters continuous-wave test mode - same reason",
    wire.MESG_OPEN_RX_SCAN_MODE_ID:
        "radiant implements background scan mode, so this would fill the "
        "transcript with whatever sensors happen to be in the room",
}

# Token marking a case as deliberately not conformant. `tools/test_ant_golden.py`
# keys on this word in the case name to know a record isn't meant to be
# well-formed - some `malformed-*` cases (short/long/oob-index) are actually
# well-framed but carry a disallowed payload, which is per record, not "must
# fail unframe()". Contract lives in `tools/vectors/README.md`; change it there
# first. Never apply this to a record that IS conformant just to quiet a
# checker (e.g. `frame/sync-in-payload` is unusual but well-formed).
MALFORMED = "malformed"

# The four-byte prefix of a message name, stripped for case names.
_NAME_TRIM = ("MESG_", "_ID")


def short_name(msg_id: int) -> str:
    """`0x42` -> `assign-channel`. Stable, lowercase, safe in a filename."""
    name = wire.MESSAGE_NAMES.get(msg_id, f"unknown-{msg_id:02x}")
    if name.startswith(_NAME_TRIM[0]):
        name = name[len(_NAME_TRIM[0]):]
    if name.endswith(_NAME_TRIM[1]):
        name = name[: -len(_NAME_TRIM[1])]
    return name.lower().replace("_", "-")


def payload_bounds(msg_id: int) -> tuple[int, int] | None:
    """(min, max) *command* payload length from the YAML, or None for `var`.

    `payload_len` is an int for a fixed size or an `'a..b'` string for an
    optional tail; `var` means the size depends on a payload field, nothing
    generic to derive.

    Always the command length, never `reply_len` - everything built from this
    (canonical case, malformed-short/-long) goes out in the host column, and a
    reply length has no business sizing a transmitted frame. Changing these
    numbers reshapes the short/long cases and breaks reproducibility of the
    committed Tier 1 transcript.
    """
    declared = wire.MESSAGES[msg_id]["payload_len"]
    if isinstance(declared, int):
        return declared, declared
    if isinstance(declared, str) and ".." in declared:
        low, _, high = declared.partition("..")
        return int(low), int(high)
    return None


# ---------------------------------------------------------------------------
# Canonical payloads: a *sensible* invocation of each message. A message
# missing from this table still gets a case (generated). Values come from
# tools/ant_wire.py wherever a constant names them.
# ---------------------------------------------------------------------------

# ANT+ RF channel and the heart-rate message period, the two settings every
# fitness app writes. Named here rather than imported because they are ANT+
# profile facts (tools/ant_pages.py's territory), not serial protocol constants.
ANT_PLUS_RF_CHANNEL = 57
ANT_PLUS_HR_PERIOD = 8070

# ANT's default search waveform. Any value round-trips; this is the one a stack
# starts with, so it is the one that changes nothing.
DEFAULT_SEARCH_WAVEFORM = 316


def _u16(value: int) -> bytes:
    return bytes([value & 0xFF, value >> 8])


CANONICAL: dict[int, bytes] = {
    wire.MESG_UNASSIGN_CHANNEL_ID: bytes([CHANNEL]),
    wire.MESG_ASSIGN_CHANNEL_ID: bytes([
        CHANNEL, wire.CHANNEL_TYPES["CHANNEL_TYPE_SLAVE"], 0]),
    wire.MESG_CHANNEL_MESG_PERIOD_ID:
        bytes([CHANNEL]) + _u16(ANT_PLUS_HR_PERIOD),
    # 0xFF is "never time out", which is what every tool here sets.
    wire.MESG_CHANNEL_SEARCH_TIMEOUT_ID: bytes([CHANNEL, 0xFF]),
    wire.MESG_CHANNEL_RADIO_FREQ_ID: bytes([CHANNEL, ANT_PLUS_RF_CHANNEL]),
    wire.MESG_NETWORK_KEY_ID: bytes([0]) + ANT_PLUS_KEY,
    # Device-wide form: what Zwift sends while setting up a search.
    wire.MESG_RADIO_TX_POWER_ID: bytes([
        0, wire.RADIO_TX_POWER["RADIO_TX_POWER_LVL_3"]]),
    wire.MESG_SEARCH_WAVEFORM_ID:
        bytes([CHANNEL]) + _u16(DEFAULT_SEARCH_WAVEFORM),
    wire.MESG_SYSTEM_RESET_ID: bytes([0]),
    wire.MESG_OPEN_CHANNEL_ID: bytes([CHANNEL]),
    wire.MESG_CLOSE_CHANNEL_ID: bytes([CHANNEL]),
    # Wildcard: matches anything, and sets nothing searching by itself.
    wire.MESG_CHANNEL_ID_ID: bytes([CHANNEL, 0, 0, 0, 0]),
    wire.MESG_BROADCAST_DATA_ID: bytes([CHANNEL]) + bytes(range(8)),
    wire.MESG_ACKNOWLEDGED_DATA_ID: bytes([CHANNEL]) + bytes(range(8)),
    # Sequence 0 with the last-packet bit set: a complete one-packet burst, the
    # same shape ant_session.py's burst_probe uses.
    wire.MESG_BURST_DATA_ID:
        bytes([CHANNEL | wire.BURST_HEADER["BURST_HEADER_LAST"]])
        + bytes(range(8)),
    wire.MESG_ADV_BURST_DATA_ID:
        bytes([CHANNEL | wire.BURST_HEADER["BURST_HEADER_LAST"]])
        + bytes(range(8)),
    wire.MESG_ID_LIST_ADD_ID: bytes([CHANNEL, 0x34, 0x12, 0x78, 0x01, 0]),
    wire.MESG_ID_LIST_CONFIG_ID: bytes([CHANNEL, 1, 0]),
    wire.MESG_CHANNEL_RADIO_TX_POWER_ID: bytes([
        CHANNEL, wire.RADIO_TX_POWER["RADIO_TX_POWER_LVL_3"]]),
    wire.MESG_SET_LP_SEARCH_TIMEOUT_ID: bytes([CHANNEL, 2]),
    wire.MESG_RX_EXT_MESGS_ENABLE_ID: bytes([0, 1]),
    # 0xE0 = channel id + RSSI + RX timestamp; RX timestamp feeds the
    # radio-clock figure the Tier 2 timing gate reads.
    wire.MESG_ANTLIB_CONFIG_ID: bytes([
        0, wire.LIB_CONFIG["LIB_CONFIG_ALL_EXT_FIELDS"]]),
    wire.MESG_AUTO_FREQ_CONFIG_ID: bytes([CHANNEL, 3, 39, 75]),
    wire.MESG_PROX_SEARCH_CONFIG_ID: bytes([CHANNEL, 0]),
    wire.MESG_SET_SEARCH_CH_PRIORITY_ID: bytes([CHANNEL, 0]),
    # Enable is 0: turning advanced burst *on* changes what later messages mean.
    wire.MESG_CONFIG_ADV_BURST_ID: bytes([
        0, wire.ADV_BURST["ADV_BURST_MODE_DISABLE"],
        wire.ADV_BURST["ADV_BURST_MODES_SIZE_8_BYTES"], 0, 0, 0, 0, 0, 0]),
    # No channel byte on the command, and a filler byte on the reply. That
    # asymmetry is real and is one of the things this tier exists to pin down.
    wire.MESG_EVENT_FILTER_CONFIG_ID: _u16(0),
    # INVALID_SDU_MASK turns selective updates off, so the canonical case
    # leaves the device where it found it.
    wire.MESG_SDU_CONFIG_ID: bytes([CHANNEL, wire.SDU["INVALID_SDU_MASK"]]),
    wire.MESG_SDU_SET_MASK_ID: bytes([0]) + bytes([0xFF] * 8),
    wire.MESG_ENCRYPT_ENABLE_ID: bytes([
        CHANNEL, wire.ENCRYPTION["ENCRYPTION_DISABLED_MODE"], 0, 0]),
    wire.MESG_SET_ENCRYPT_KEY_ID:
        bytes([0]) + bytes(range(wire.ENCRYPTION["ENCRYPTION_KEY_SIZE"])),
    wire.MESG_SET_ENCRYPT_INFO_ID: bytes([
        wire.ENCRYPTION["ENCRYPTION_INFO_SET_CRYPTO_ID"], 1, 2, 3, 4]),
    wire.MESG_ACTIVE_SEARCH_SHARING_ID: bytes([CHANNEL, 0]),
}

# Messages that need the channel to exist first. Without the assign, every one
# answers CHANNEL_IN_WRONG_STATE and only the error path is exercised; with it,
# the success path is too, and the assign itself is recorded.
#
# Deliberately absent: anything that would complete a channel and open it (see
# module docstring).
NEEDS_ASSIGNED_CHANNEL = frozenset({
    wire.MESG_CHANNEL_MESG_PERIOD_ID,
    wire.MESG_CHANNEL_SEARCH_TIMEOUT_ID,
    wire.MESG_CHANNEL_RADIO_FREQ_ID,
    wire.MESG_CHANNEL_ID_ID,
    wire.MESG_SEARCH_WAVEFORM_ID,
    wire.MESG_OPEN_CHANNEL_ID,
    wire.MESG_CLOSE_CHANNEL_ID,
    wire.MESG_CHANNEL_RADIO_TX_POWER_ID,
    wire.MESG_SET_LP_SEARCH_TIMEOUT_ID,
    wire.MESG_ID_LIST_ADD_ID,
    wire.MESG_ID_LIST_CONFIG_ID,
    wire.MESG_AUTO_FREQ_CONFIG_ID,
    wire.MESG_PROX_SEARCH_CONFIG_ID,
    wire.MESG_SET_SEARCH_CH_PRIORITY_ID,
    wire.MESG_ACTIVE_SEARCH_SHARING_ID,
    wire.MESG_SDU_CONFIG_ID,
    wire.MESG_ENCRYPT_ENABLE_ID,
    wire.MESG_BROADCAST_DATA_ID,
    wire.MESG_ACKNOWLEDGED_DATA_ID,
})

ASSIGN_PREAMBLE = (wire.frame(wire.MESG_ASSIGN_CHANNEL_ID,
                              CANONICAL[wire.MESG_ASSIGN_CHANNEL_ID]),)

# What MESG_REQUEST (0x4D) can ask for, and the index byte to ask with.
#
# Hand-written, unlike the other tables: the YAML records a message's
# direction but not whether `handle_request()` answers it, nor what byte 0
# means when it does.
REQUESTABLE: tuple[tuple[int, int, str], ...] = (
    (wire.MESG_CAPABILITIES_ID, 0, "what the stack can do"),
    (wire.MESG_VERSION_ID, 0,
     "EXPECTED TO DIFFER between backends - it is the backend's own name"),
    (wire.MESG_CHANNEL_STATUS_ID, CHANNEL, "[channel, status]"),
    (wire.MESG_CHANNEL_ID_ID, CHANNEL, "reply leads with the channel"),
    (wire.MESG_CHANNEL_MESG_PERIOD_ID, CHANNEL, ""),
    (wire.MESG_CHANNEL_RADIO_FREQ_ID, CHANNEL, ""),
    # 0x58 and 0x8C are Nordic extensions, both dispatched and answered by
    # handle_request(), so both belong in the run.
    (wire.MESG_CHANNEL_CRC_MODE_ID, CHANNEL,
     "the command's byte 0 is a filler and the request's is a channel - the "
     "asymmetry is the thing worth pinning"),
    (wire.MESG_ANTLIB_CONFIG_ID, 0, "byte 0 is a filler, not a channel"),
    (wire.MESG_ACTIVE_SEARCH_SHARING_ID, CHANNEL, ""),
    (wire.MESG_EVENT_FILTER_CONFIG_ID, 0,
     "reply carries a filler byte the command did not"),
    (wire.MESG_SET_SEARCH_CH_PRIORITY_ID, CHANNEL, ""),
    (wire.MESG_PENDING_TRANSMIT_CLEAR_ID, CHANNEL,
     "the request form replies MESG_PENDING_TRANSMIT_GET_SIZE bytes, a "
     "different constant from the command's own payload length"),
    (wire.MESG_CONFIG_ADV_BURST_ID, 0, "index 0: capabilities"),
    (wire.MESG_CONFIG_ADV_BURST_ID, 1,
     "index 1: configuration, and the reply drops the enable byte"),
    (wire.MESG_SDU_SET_MASK_ID, 0,
     "byte 0 is a mask number; the reply is the same shape as the command"),
    (wire.MESG_ENCRYPT_ENABLE_ID,
     wire.ENCRYPTION["ENCRYPTION_INFO_GET_SUPPORTED_MODE"],
     "info type echoed at byte 0, 2-byte reply"),
    (wire.MESG_ENCRYPT_ENABLE_ID,
     wire.ENCRYPTION["ENCRYPTION_INFO_GET_CRYPTO_ID"], "5-byte reply"),
    (wire.MESG_ENCRYPT_ENABLE_ID,
     wire.ENCRYPTION["ENCRYPTION_INFO_GET_CUSTOM_USER_DATA"],
     "20-byte reply - the longest the parser ever sees"),
    (wire.MESG_ENCRYPT_ENABLE_ID, 3,
     "not a defined info type: the reply-size switch must map it to "
     "INVALID_MESSAGE rather than send a zero-length reply"),
    (wire.MESG_INVALID_ID, 0,
     "no such message: the default arm of handle_request()"),
)

# Messages whose numeric id this repository still doesn't know, so no case can
# be built. Recorded rather than dropped, so the JSON summary shows the hole.
#
# Derived from `tools/ant_wire.py`'s UNRESOLVED dict (generated from the YAML)
# rather than hand-listed here, so it can't silently fall behind as ids get
# resolved.
UNREQUESTABLE_UNRESOLVED = tuple(sorted(
    name for name in wire.UNRESOLVED
    if name.startswith("MESG_") and name.endswith("_ID")
))


# ---------------------------------------------------------------------------
# Cases
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Case:
    """One thing to send, and why.

    `frames` is everything written for this case, in order: preamble first,
    then the frame under test. Every byte is recorded, so a preamble
    divergence shows up as a difference rather than a changed meaning.
    """

    name: str
    frames: tuple[bytes, ...]
    note: str = ""
    expect_reply: bool = True


def _variants(msg_id: int, canonical: bytes,
              preamble: tuple[bytes, ...]) -> list[Case]:
    """The valid case and its malformed siblings, in a fixed order."""
    stem = f"{msg_id:02x}-{short_name(msg_id)}"
    bounds = payload_bounds(msg_id)
    cases = [Case(f"{stem}/valid", preamble + (wire.frame(msg_id, canonical),),
                  "a sensible invocation")]

    if bounds is not None:
        low, high = bounds
        # Short: one byte less than the declared minimum. dispatch() handlers
        # guard on `len >= N` and leave `err` at INVALID_MESSAGE when it fails.
        if low >= 1:
            cases.append(Case(
                f"{stem}/{MALFORMED}-short",
                preamble + (wire.frame(msg_id, canonical[: low - 1]),),
                f"{low - 1} payload bytes where the message declares {low}"))
        # Long: one byte more than the declared maximum, capped to the
        # parser's body buffer so the case measures the handler, not the buffer.
        long_len = min(high + 1, wire.MAX_SIZE_VALUE)
        if long_len > high:
            padded = (canonical + bytes(long_len))[:long_len]
            cases.append(Case(
                f"{stem}/{MALFORMED}-long",
                preamble + (wire.frame(msg_id, padded),),
                f"{long_len} payload bytes where the message declares at most "
                f"{high}"))

    if canonical:
        oob = bytes([BAD_INDEX]) + canonical[1:]
        cases.append(Case(
            f"{stem}/{MALFORMED}-oob-index",
            preamble + (wire.frame(msg_id, oob),),
            "byte 0 out of range - a channel number on most messages, a mask "
            "number or an info type on a few, and 0xFF is out of range for all "
            "of them"))

    if len(canonical) > 1:
        ff = canonical[:1] + bytes([0xFF] * (len(canonical) - 1))
        if ff != canonical:
            cases.append(Case(
                f"{stem}/{MALFORMED}-param-ff",
                preamble + (wire.frame(msg_id, ff),),
                "every parameter byte 0xFF: the error mapping is the thing "
                "under test, not the value"))

    # Checksum wrong by one bit: bridge drops it silently instead of answering
    # MESG_SERIAL_ERROR, so the expected recording is just the request.
    #
    # No preamble here - a malformed frame never reaches dispatch(), and a
    # preamble that replies would make "expect silence" untestable.
    good = bytearray(wire.frame(msg_id, canonical))
    good[-1] ^= 0x01
    cases.append(Case(f"{stem}/{MALFORMED}-checksum", (bytes(good),),
                      "checksum off by one bit; expect silence",
                      expect_reply=False))
    return cases


def _request_cases() -> list[Case]:
    cases = []
    for req_id, index, note in REQUESTABLE:
        name = short_name(req_id)
        payload = bytes([index, req_id])
        cases.append(Case(
            f"4d-request-{name}-{index}/valid",
            (wire.frame(wire.MESG_REQUEST_ID, payload),),
            note or f"request {wire.MESSAGE_NAMES.get(req_id, req_id)}"))
    return cases


def unknown_message_id() -> int:
    """The highest id no table claims. Computed so it can't go stale as ids
    get allocated in `protocol/ant_wire.yaml`."""
    claimed = set(wire.MESSAGES) | set(getattr(wire, "RADIANT_MESSAGES", {}))
    return max(value for value in range(0x100) if value not in claimed)


def _frame_cases() -> list[Case]:
    """Cases about the frame parser rather than about any one message.

    Last in the run: two of them deliberately leave junk in the parser's
    input, so the next thing on the wire should be a reset.
    """
    freq = wire.frame(wire.MESG_CHANNEL_RADIO_FREQ_ID,
                      CANONICAL[wire.MESG_CHANNEL_RADIO_FREQ_ID])
    cases = [
        Case(f"frame/{MALFORMED}-unknown-id-low",
             (wire.frame(wire.MESG_INVALID_ID, bytes([CHANNEL])),),
             "message id 0x00 is reserved and must map to INVALID_MESSAGE"),
        Case(f"frame/{MALFORMED}-unknown-id-high",
             (wire.frame(unknown_message_id(), bytes([CHANNEL])),),
             "the highest id no table in ant_wire.py claims"),
        Case(f"frame/{MALFORMED}-zero-length",
             (wire.frame(wire.MESG_OPEN_CHANNEL_ID, b""),),
             "a bridged message with no payload at all: `ch` defaults to 0 and "
             "`err` stays at INVALID_MESSAGE"),
        Case(f"frame/{MALFORMED}-oversize",
             (wire.frame(wire.MESG_CHANNEL_RADIO_FREQ_ID,
                         bytes(wire.MAX_SIZE_VALUE + 4)),),
             "a payload longer than MAX_SIZE_VALUE: the parser stops storing "
             "bytes but keeps counting them"),
        Case(f"frame/{MALFORMED}-sync",
             (bytes([wire.SYNC_RX]) + freq[1:],),
             "0xA5 is the bidirectional variant's SYNC and this dongle never "
             "resynchronises on it; expect silence",
             expect_reply=False),
        Case("frame/sync-in-payload",
             (wire.frame(wire.MESG_ANTLIB_CONFIG_ID,
                         bytes([wire.SYNC_TX,
                                wire.LIB_CONFIG["LIB_CONFIG_ALL_EXT_FIELDS"]])),),
             "0xA4 inside a payload: a parser that resynchronised mid-frame "
             "would lose this one"),
    ]

    # A header declaring three body bytes, then a complete valid frame. The
    # header eats the valid frame's first bytes as its own body, fails its
    # checksum, and is dropped; the parser must resync on the next SYNC.
    truncated = bytes([wire.SYNC_TX, 0x03, wire.MESG_CHANNEL_RADIO_FREQ_ID])
    cases.append(Case(
        f"frame/{MALFORMED}-partial-then-valid", (truncated + freq, freq),
        "a truncated header swallows the frame behind it; the one after that "
        "must still be answered"))
    return cases


def generate_cases() -> list[Case]:
    """Every case, in the fixed order they are sent in.

    Pure: no clock, no device, no environment. Two calls return equal cases
    (asserted by `tools/test_ant_conformance.py`), the foundation the byte
    diff stands on.
    """
    cases: list[Case] = []

    for msg_id in sorted(wire.MESSAGES):
        info = wire.MESSAGES[msg_id]
        if info["direction"] not in ("h2d", "both"):
            continue  # a marker, or something only the dongle ever sends
        if msg_id in EXCLUDED:
            continue
        if msg_id == wire.MESG_REQUEST_ID:
            continue  # expanded by _request_cases()

        canonical = CANONICAL.get(msg_id)
        if canonical is None:
            bounds = payload_bounds(msg_id)
            if bounds is None:
                # `var`: nothing safe to derive. Two bytes reaches every
                # handler's guard.
                canonical = bytes([CHANNEL, 0])
            else:
                canonical = (bytes([CHANNEL]) + bytes(bounds[0]))[:bounds[0]]

        preamble = (ASSIGN_PREAMBLE if msg_id in NEEDS_ASSIGNED_CHANNEL
                    else ())
        cases.extend(_variants(msg_id, canonical, preamble))

    cases.extend(_request_cases())
    cases.extend(_frame_cases())
    return cases


# ---------------------------------------------------------------------------
# Running against a device
# ---------------------------------------------------------------------------


@dataclass
class RunState:
    records: list[Record] = field(default_factory=list)
    replies: int = 0
    silent_cases: list[str] = field(default_factory=list)
    unexpected_replies: list[str] = field(default_factory=list)


def _collect(reader, case_name: str, seconds: float, state: RunState,
             until: int | None = None) -> int:
    """Record every frame that arrives within `seconds`. Returns the count.

    With `until`, stop as soon as that message id lands - used only for the
    reset before each case, where waiting the full settle window would double
    the run length for a guaranteed reply.

    Frame bytes are rebuilt with `wire.frame()` rather than kept raw, since
    `FrameReader` hands back (id, payload) and discards the buffer - this is
    byte-exact, not an approximation, because the reader already validated
    length and checksum.
    """
    count = 0
    deadline = time.monotonic() + seconds
    while True:
        got = reader.next_frame(deadline)
        if got is None:
            return count
        msg_id, payload = got
        state.records.append(Record(None, DONGLE_TO_HOST,
                                    wire.frame(msg_id, payload), case_name))
        state.replies += 1
        count += 1
        if until is not None and msg_id == until:
            return count


def run_cases(dev, reader, cases, *, settle: float, reset_each: bool,
              verbose: bool, reset_timeout: float = 3.0) -> RunState:
    """Drive every case against an open device and record the conversation."""
    # Imported here, not at top, so --list/--replay/--compare and the unit
    # tests never need a device import.
    from ant_probe import EP_OUT

    state = RunState()
    reset_frame = wire.frame(wire.MESG_SYSTEM_RESET_ID,
                             CANONICAL[wire.MESG_SYSTEM_RESET_ID])

    for index, case in enumerate(cases, start=1):
        if verbose:
            print(f"  [{index}/{len(cases)}] {case.name}")

        if reset_each:
            state.records.append(Record(None, HOST_TO_DONGLE, reset_frame,
                                        case.name))
            dev.write(EP_OUT, reset_frame)
            if _collect(reader, case.name, reset_timeout, state,
                        until=wire.MESG_STARTUP_MESG_ID) == 0:
                # A stack that doesn't answer a reset won't produce a
                # transcript worth diffing; don't bury that in 260 more cases.
                raise SystemExit(
                    f"no startup message after reset before case {case.name} - "
                    f"is this the ANT firmware?")

        for payload in case.frames:
            state.records.append(Record(None, HOST_TO_DONGLE, payload,
                                        case.name))
            dev.write(EP_OUT, payload)

        replies = _collect(reader, case.name, settle, state)
        if replies == 0 and case.expect_reply:
            state.silent_cases.append(case.name)
        elif replies and not case.expect_reply:
            state.unexpected_replies.append(case.name)

    return state


# ---------------------------------------------------------------------------
# Comparing two transcripts
# ---------------------------------------------------------------------------


@dataclass
class Difference:
    case: str
    index: int
    a: str | None
    b: str | None


def compare_records(a: list[Record], b: list[Record]) -> list[Difference]:
    """Line-for-line differences, attributed to the case they fall in.

    Aligned by position, not LCS: two transcripts from the same tool run the
    same cases in the same order, so a positional difference is a real one.
    """
    diffs: list[Difference] = []
    for index in range(max(len(a), len(b))):
        left = a[index] if index < len(a) else None
        right = b[index] if index < len(b) else None
        if left is not None and right is not None:
            same = (left.direction == right.direction
                    and left.data == right.data
                    and left.case == right.case)
            if same:
                continue
        case = (left.case if left is not None else None) or \
               (right.case if right is not None else None) or "?"
        diffs.append(Difference(
            case, index,
            None if left is None else f"{left.direction} {left.hex}",
            None if right is None else f"{right.direction} {right.hex}"))
    return diffs


def _sha256(path: str) -> str:
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def compare(path_a: str, path_b: str, allowed: set[str],
            *, limit: int = 20) -> dict:
    """The Tier 1 gate, as a readable report rather than as `fc /b`."""
    sha_a, sha_b = _sha256(path_a), _sha256(path_b)
    identical = sha_a == sha_b

    records_a = ant_trace.read_antser_file(path_a)
    records_b = ant_trace.read_antser_file(path_b)
    diffs = compare_records(records_a, records_b)
    differing_cases = sorted({d.case for d in diffs})
    unexpected = [case for case in differing_cases if case not in allowed]

    print(f"A  {path_a}")
    print(f"   {len(records_a)} records, sha256 {sha_a}")
    print(f"B  {path_b}")
    print(f"   {len(records_b)} records, sha256 {sha_b}")

    if identical:
        print("\nPASS: byte-identical")
    else:
        print(f"\n{len(diffs)} differing record(s) across "
              f"{len(differing_cases)} case(s)")
        for diff in diffs[:limit]:
            allow = " (allowed)" if diff.case in allowed else ""
            print(f"\n  case {diff.case}{allow}, record {diff.index}")
            print(f"    A  {diff.a if diff.a is not None else '(end of file)'}")
            print(f"    B  {diff.b if diff.b is not None else '(end of file)'}")
            for label, text in (("A", diff.a), ("B", diff.b)):
                if text:
                    data = bytes.fromhex(text.split()[1])
                    print(f"    {label}  {ant_trace.describe_frame(data)}")
        if len(diffs) > limit:
            print(f"\n  ... and {len(diffs) - limit} more")

        if unexpected:
            print(f"\nFAILED: {len(unexpected)} case(s) differ that were not "
                  f"allowed to:")
            for case in unexpected:
                print(f"  {case}")
        else:
            print("\nPASS with allowances: every difference is in a case "
                  "listed with --allow-differing-case")

    return {
        "tool": "ant_conformance.py",
        "mode": "compare",
        "a": {"path": path_a, "sha256": sha_a, "records": len(records_a)},
        "b": {"path": path_b, "sha256": sha_b, "records": len(records_b)},
        "byte_identical": identical,
        "differing_records": len(diffs),
        "differing_cases": differing_cases,
        "allowed_differing_cases": sorted(allowed),
        "unexpected_differing_cases": unexpected,
        "pass": identical or not unexpected,
    }


# ---------------------------------------------------------------------------
# Summaries
# ---------------------------------------------------------------------------


def summarise(cases: list[Case], state: RunState, path: str,
              text: str) -> dict:
    """The JSON `tools/ant_ab.py` reads, and the record of what was skipped.

    Also the integrity record for the transcript: `tools/test_ant_golden.py`
    reads `sha256`, `records`, and `case_index` back from the committed copy
    and fails if the bytes no longer hash to them - the only check that can
    see a flipped byte inside an otherwise well-formed reply.

    `case_index` is per-case (not just per-file) so a diff can say which case
    changed. Changing how any of these three fields is computed invalidates
    every committed baseline - a deliberate act, not a refactor.
    """
    per_case: dict[str, dict] = {}
    for record in state.records:
        entry = per_case.setdefault(record.case or "?",
                                    {"records": 0, "hex": []})
        entry["records"] += 1
        entry["hex"].append(f"{record.direction}{record.hex}")

    exercised = {record.data[2] for record in state.records
                 if record.direction == HOST_TO_DONGLE
                 and wire.unframe(record.data) is not None}

    return {
        "tool": "ant_conformance.py",
        "format": "antser-v1",
        "antser_path": path,
        "sha256": hashlib.sha256(text.encode("utf-8")).hexdigest(),
        "cases": len(cases),
        "records": len(state.records),
        "replies": state.replies,
        "messages_exercised": len(exercised),
        # A silent malformed-checksum case is expected; a silent *valid* case
        # means the dongle never answered.
        "silent_cases": state.silent_cases,
        "unexpected_replies": state.unexpected_replies,
        "skipped": [
            {"what": wire.MESSAGE_NAMES.get(msg_id, hex(msg_id)),
             "why": why}
            for msg_id, why in sorted(EXCLUDED.items())
        ] + [
            {"what": name,
             "why": "numeric id unresolved without sdk-ant: "
                    + wire.UNRESOLVED.get(name, "")}
            for name in UNREQUESTABLE_UNRESOLVED
        ] + [
            {"what": name,
             "why": "a RadiANT extension message. Nothing implements these "
                    "yet, so a case for one would record INVALID_MESSAGE today "
                    "and mean something else in Phase 7 - which is the one "
                    "thing a transcript diffed across firmware revisions must "
                    "not do. Add them when the extensions land."}
            for name in sorted(
                info["name"] for info in
                getattr(wire, "RADIANT_MESSAGES", {}).values())
        ],
        "case_index": [
            {"name": name, "records": entry["records"],
             "sha256": hashlib.sha256(
                 "\n".join(entry["hex"]).encode("utf-8")).hexdigest()}
            for name, entry in per_case.items()
        ],
    }


def _write_json(target: str, payload: dict) -> None:
    if target == "-":
        json.dump(payload, sys.stdout, indent=2)
        sys.stdout.write("\n")
        return
    with open(target, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", metavar="FILE", default="conformance.antser",
                        help="where to write the transcript "
                             "(default: conformance.antser)")
    parser.add_argument("--serial",
                        help="match a dongle whose serial ends with this. Two "
                             "boards here enumerate as the same 0FCF:1009, so "
                             "a run with no --serial cannot be attributed to a "
                             "board")
    parser.add_argument(
        "--port",
        help="talk to a UART build over this serial port (e.g. COM8, "
             "/dev/ttyACM1) instead of over USB",
    )
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--settle", type=float, default=0.25,
                        help="seconds to keep reading after each write "
                             "(default: 0.25). Floored by "
                             "ant_probe.READ_TIMEOUT_MS")
    parser.add_argument("--no-reset-each", action="store_true",
                        help="do not reset the stack before every case. "
                             "Faster, but loses per-case independence")
    parser.add_argument("--only", metavar="SUBSTRING",
                        help="run only cases whose name contains this. For "
                             "debugging one case; not a Tier 1 baseline")
    parser.add_argument("--list", action="store_true",
                        help="print the case list and exit. No device needed")
    parser.add_argument("--replay", metavar="FILE",
                        help="read a transcript back, check every frame and "
                             "print it decoded. No device needed")
    parser.add_argument("--compare", nargs=2, metavar=("A", "B"),
                        help="diff two transcripts by case. This is the Tier 1 "
                             "gate; exits nonzero unless they match")
    parser.add_argument("--allow-differing-case", action="append", default=[],
                        metavar="NAME",
                        help="a case that is allowed to differ under "
                             "--compare. The version string is the one that "
                             "legitimately does; record the allowance in "
                             "tools/ab_gates.toml so it is reviewed")
    parser.add_argument("--json", metavar="FILE", nargs="?", const="-",
                        help="write a summary as JSON (default: stdout)")
    parser.add_argument("-q", "--quiet", action="store_true")
    args = parser.parse_args()

    cases = generate_cases()
    if args.only:
        cases = [case for case in cases if args.only in case.name]

    if args.list:
        for case in cases:
            print(f"{case.name:44} {len(case.frames)} frame(s)  {case.note}")
        print(f"\n{len(cases)} cases, "
              f"{len({case.name.split('/')[0] for case in cases})} message "
              f"groups")
        return 0

    if args.compare:
        result = compare(args.compare[0], args.compare[1],
                         set(args.allow_differing_case))
        if args.json:
            _write_json(args.json, result)
        return 0 if result["pass"] else 1

    if args.replay:
        try:
            records = ant_trace.read_antser_file(args.replay)
        except ant_trace.TraceError as exc:
            sys.exit(f"ant_conformance: {exc}")
        if not args.quiet:
            sys.stdout.write(ant_trace.annotate(records))
        bad = [r for r in records if r.unframed() is None]
        print(f"\n{len(records)} record(s), {len(bad)} not a valid frame")
        if args.json:
            _write_json(args.json, {
                "tool": "ant_conformance.py",
                "mode": "replay",
                "antser_path": args.replay,
                "sha256": _sha256(args.replay),
                "records": len(records),
                "invalid_frames": len(bad),
            })
        return 0

    # Everything below this line needs a board.
    from ant_probe import FrameReader, close_device, open_device

    dev = open_device(not args.quiet, serial=args.serial, port=args.port,
                      baud=args.baud)
    reader = FrameReader(dev)

    print(f"\nrunning {len(cases)} conformance cases "
          f"(reset before each: {not args.no_reset_each})")
    state = run_cases(dev, reader, cases, settle=args.settle,
                      reset_each=not args.no_reset_each,
                      verbose=not args.quiet)
    close_device(dev)

    text = ant_trace.write_antser(args.out, state.records, notes=[
        "tools/ant_conformance.py - Tier 1 frame conformance",
        "timestamps are suppressed on purpose: the acceptance criterion is a "
        "byte diff of two of these files",
        f"{len(cases)} cases, {len(state.records)} records",
    ])

    summary = summarise(cases, state, args.out, text)
    print(f"\nwrote {args.out}")
    print(f"  {summary['cases']} cases, {summary['records']} records, "
          f"{summary['replies']} replies")
    print(f"  sha256 {summary['sha256']}")
    if state.silent_cases:
        print(f"  {len(state.silent_cases)} case(s) expected a reply and got "
              f"none: {', '.join(state.silent_cases[:6])}"
              + (" ..." if len(state.silent_cases) > 6 else ""))
    if state.unexpected_replies:
        print(f"  {len(state.unexpected_replies)} case(s) answered when "
              f"silence was expected: "
              f"{', '.join(state.unexpected_replies[:6])}")

    if args.json:
        _write_json(args.json, summary)

    print("\nRun this against the other backend and diff the two:")
    print(f"  python tools/ant_conformance.py --compare <other> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
