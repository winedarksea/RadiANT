#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Tier 1: drive every message the bridge implements and record what came back.

One board, no radio, no sensors, no statistics. The tool sends every message
`dispatch()` in `src/ant_serial_bridge.c` implements - valid, then deliberately
malformed - and writes the whole conversation to a `.antser` transcript. **The
A/B is a byte diff of two transcripts; byte-identical is the pass.**

    python tools/ant_conformance.py --serial <suffix> --out sdk-ant.antser
    # reflash the other backend, then
    python tools/ant_conformance.py --serial <suffix> --out core.antser
    python tools/ant_conformance.py --compare sdk-ant.antser core.antser

That catches most of the divergence there is to catch between two radio
backends - response codes, reply framing and sizes, error mapping, and the
leading-index-byte shapes in `handle_request()` where a reply carries a mask
number or an encryption info type in the slot everything else puts a channel in
- and it catches it without owning a sensor, without a second board and without
a single statistic. That is why it is worth more than it looks.

Determinism is the whole product
--------------------------------

A transcript that is not reproducible cannot be diffed, and a tier whose
acceptance criterion is a byte diff is then worth nothing. Four rules follow,
and all four are load-bearing:

* **The timestamp column is always `-`.** Real timestamps never repeat.
  `archive/captures/serial/README.md` fixes this: a conformance transcript is a
  statement about bytes and ordering, not about timing.
* **Every case starts from a stack reset.** A case that depended on what the
  previous case left behind would reorder its own meaning the first time a case
  was inserted above it. The cost is about a second per case; the run takes a
  few minutes and that is the right trade.
* **Nothing here ever puts anything on the air.** Not for politeness - for
  determinism. An open channel hears whatever is in the room, and a transcript
  containing the neighbour's heart-rate strap can never be byte-identical
  twice. `MESG_RADIO_CW_MODE`, `MESG_RADIO_CW_INIT` and
  `MESG_OPEN_RX_SCAN_MODE` are excluded outright (see `EXCLUDED`), and
  `MESG_OPEN_CHANNEL` is only ever sent at a channel that has no channel id
  set, so it is refused before the radio is touched.
* **Order everything this tool controls; sort nothing the device ordered.**
  Cases run in a fixed order and replies are recorded in arrival order.

The message set comes from `tools/ant_wire.py`, not from a list here
--------------------------------------------------------------------

Cases are generated from `MESSAGES` and `BRIDGED_MESSAGE_IDS`, so a message
added to `protocol/ant_wire.yaml` enters the conformance run by itself. The
per-message payloads in `CANONICAL` only say what a *sensible* invocation looks
like; a message with no entry there still gets a case built from its declared
`payload_len`. Constants are imported by name and never restated - the values
in that module are generated, and a copy here would be a fifth place for one
number to live.

What a bench sitting has to run
-------------------------------

One board, no sensors, nothing on the air. About 260 cases at roughly a second
each, so budget five minutes per backend plus the reflash between them:

    python tools\ant_conformance.py --serial <suffix> --out conformance-sdk-ant.antser --json conformance-sdk-ant.json
    # reflash the other backend on the same board, same session
    python tools\ant_conformance.py --serial <suffix> --out conformance-core.antser --json conformance-core.json
    python tools\ant_conformance.py --compare conformance-sdk-ant.antser conformance-core.antser

`--serial` is not optional in practice: two boards here enumerate as the same
`0FCF:1009`, so a transcript recorded without it cannot be attributed to a
board. The sdk-ant transcript is the Tier 1 reference and belongs in
`archive/captures/serial/` with its hash recorded in the sitting's baseline
JSON (`conformance.antser_path`, `conformance.sha256`). It is as perishable as
the performance numbers: it can only be recorded while a working sdk-ant build
exists.

Where the time goes, and why it is not shortened: after every write the tool
reads until the device has been quiet for `--settle`, and the floor on that is
`ant_probe.READ_TIMEOUT_MS`. That value is deliberately a full second, because a
read timeout that lands while the dongle is answering loses the answer - the bug
that manufactured 0.4 percentage points of imaginary packet loss and hid for
weeks behind losses that looked exactly like the on-air kind.

What a difference means
-----------------------

`--compare` reports differences by case name rather than by byte offset, which
is the difference between "case 4d-request-version/valid differs" and "the files
differ at byte 8417". One difference is expected and is a decision rather than a
bug: the version string. `MESG_VERSION` returns whatever the backend calls
itself, so sdk-ant and `ant_core` cannot both answer it identically and should
not. Use `--allow-differing-case` for it, and record the allowance in
`tools/ab_gates.toml` where somebody has to review it, rather than dropping the
message from the run.
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
from ant_trace import DONGLE_TO_HOST, HOST_TO_DONGLE, Record  # noqa: E402

# The published ANT+ network key. Imported rather than restated: it lives in
# ant_scan.py because it is a network key, not a protocol constant, and
# docs/ant-serial-protocol.md says it stays there. It matters here because
# `ant_network_address_set()` is one of the few calls whose result depends on
# the *value* handed in - ant_core holds a table seeded with the ANT+ pair and
# refuses unknown keys - so a made-up key would manufacture a divergence that
# says nothing about the bridge.
from ant_scan import ANT_PLUS_KEY  # noqa: E402

# The channel every case uses. Channel 0 exists on every build; anything higher
# would make the transcript depend on CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED.
CHANNEL = 0

# The channel/index byte used by the out-of-range variant. 0xFF is out of range
# for a channel on any build, out of range for a network number, out of range
# for an SDU mask index and not a defined encryption info type, so the same
# value exercises the range check whatever byte 0 happens to mean.
BAD_INDEX = 0xFF

# Messages that are never sent, whatever the YAML says about them.
#
# All three are radio-quiet on today's firmware only because the bridge does not
# implement them. `ant_core` implements scan mode by design (the capability bit
# is turned on in the plan), and a continuous-wave test mode keys the
# transmitter and leaves it keyed. A conformance run that opened either would
# stop being reproducible the moment the room changed, and one of them would put
# a carrier on 2457 MHz for the rest of the session.
EXCLUDED = {
    wire.MESG_RADIO_CW_MODE_ID:
        "keys the transmitter; a backend that bridges it would leave a carrier "
        "on the band for the rest of the run",
    wire.MESG_RADIO_CW_INIT_ID:
        "enters continuous-wave test mode - same reason",
    wire.MESG_OPEN_RX_SCAN_MODE_ID:
        "ant_core implements background scan mode, so this would fill the "
        "transcript with whatever sensors happen to be in the room",
}

# The token that marks a case as deliberately not conformant.
#
# `tools/test_ant_golden.py` replays every `.antser` in
# `archive/captures/serial/` against the framing rules, and it keys on this word
# in the case name to know that a record is not meant to be well formed. That
# suite was written by a different agent in the same wave and settled on the
# token independently; using the same one is what lets a committed conformance
# transcript be replayed at all.
#
# The gap that was recorded here as a change request is closed. That suite once
# read "malformed" as "unframe() must reject this" - true of
# `malformed-checksum` and `malformed-sync`, false of `malformed-short`,
# `malformed-long` and `malformed-oob-index`, which are perfectly well-formed
# frames carrying a payload the message id does not admit, and that is exactly
# what makes them worth sending. It now reads it as "a problem with this record
# is recorded and waived rather than failed", which is per record, so a case
# that happens to be conformant simply has nothing to waive. The written
# contract between the two files is `tools/vectors/README.md`; neither owns the
# other, so a change to the convention goes there first.
#
# The one thing this tool must never do is claim the waiver for a record that is
# conformant. `frame/sync-in-payload` deliberately carries channel 0xA4 so a
# SYNC byte lands inside a payload, and naming it `malformed` to quiet a checker
# would record a true fact - this frame is unusual - as a false one.
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

    `payload_len` is an int for a fixed-size message and a `'a..b'` string for
    one with an optional tail. `var` means the size depends on a field inside
    the payload, and there is nothing generic to derive from it.

    Deliberately the command length and never `reply_len`. Everything built
    from this goes out in the host column - the canonical case, and the
    malformed-short and malformed-long siblings derived one byte either side of
    these bounds - and a reply length has no business setting the size of a
    frame this tool transmits.

    A second and blunter reason not to touch these numbers: the bounds decide
    how long the short and long variants are, so narrowing one silently changes
    the bytes of two cases, and the committed Tier 1 reference transcript stops
    being reproducible by the tool that produced it. `0x79` is the live example
    - its command is two bytes and its reply three, and `payload_len` still
    reads `2..3` for exactly this reason, with the reply side pinned exactly by
    `reply_len` instead.
    """
    declared = wire.MESSAGES[msg_id]["payload_len"]
    if isinstance(declared, int):
        return declared, declared
    if isinstance(declared, str) and ".." in declared:
        low, _, high = declared.partition("..")
        return int(low), int(high)
    return None


# ---------------------------------------------------------------------------
# Canonical payloads
#
# What a *sensible* invocation of each message looks like. Everything else is
# derived, so this table is about intent, not coverage: a message missing from
# it still gets a case. Values come from tools/ant_wire.py wherever a constant
# names them, because a literal here would be a constant with no source.
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
    # The device-wide form, which is the one Zwift sends while setting up a
    # search and the one whose INVALID_MESSAGE answer stalled it.
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
    # 0xE0 - channel id + RSSI + RX timestamp. Not 0x80: all three fields are
    # on the path ant_verify.py depends on, and the RX timestamp is what gives
    # the radio-clock timing figure the Tier 2 timing gate is read against.
    wire.MESG_ANTLIB_CONFIG_ID: bytes([
        0, wire.LIB_CONFIG["LIB_CONFIG_ALL_EXT_FIELDS"]]),
    wire.MESG_AUTO_FREQ_CONFIG_ID: bytes([CHANNEL, 3, 39, 75]),
    wire.MESG_PROX_SEARCH_CONFIG_ID: bytes([CHANNEL, 0]),
    wire.MESG_SET_SEARCH_CH_PRIORITY_ID: bytes([CHANNEL, 0]),
    # [filler, enable, rf payload size, required modes, 0, 0, optional modes,
    # 0, 0]. Enable is 0: turning advanced burst *on* changes what later
    # messages mean, and the reset before each case would undo it anyway.
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

# Messages that need the channel to exist before they mean anything. Without
# the assign, every one of them answers CHANNEL_IN_WRONG_STATE and the case
# proves only that the error path works. With it, the success path is exercised
# too - and the assign itself is recorded, so a divergence in the preamble is
# visible rather than silently changing what the case measured.
#
# Deliberately absent: anything that would complete a channel and open it. See
# the module docstring.
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
# Hand-written, and it is the one list here that is: `protocol/ant_wire.yaml`
# records a message's direction but not whether `handle_request()` answers it,
# nor what byte 0 means when it does. Adding a `requestable:` field to the YAML
# would let this be generated too - see the change request in this agent's
# report.
REQUESTABLE: tuple[tuple[int, int, str], ...] = (
    (wire.MESG_CAPABILITIES_ID, 0, "what the stack can do"),
    (wire.MESG_VERSION_ID, 0,
     "EXPECTED TO DIFFER between backends - it is the backend's own name"),
    (wire.MESG_CHANNEL_STATUS_ID, CHANNEL, "[channel, status]"),
    (wire.MESG_CHANNEL_ID_ID, CHANNEL, "reply leads with the channel"),
    (wire.MESG_CHANNEL_MESG_PERIOD_ID, CHANNEL, ""),
    (wire.MESG_CHANNEL_RADIO_FREQ_ID, CHANNEL, ""),
    # 0x58 and 0x8C are Nordic extensions whose ids this repository did not
    # know until the Wave 2 shim recovered them. Both are dispatched and both
    # are answered by handle_request(), so both belong in the run.
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

# Messages whose numeric id this repository still does not know, so no case can
# be built for them. Recorded rather than dropped: a reader of the JSON summary
# should be able to see the hole.
#
# **Derived, not listed.** This was a hand-written tuple naming
# MESG_CHANNEL_CRC_MODE_ID and MESG_PENDING_TRANSMIT_CLEAR_ID, and it stayed
# that way after the Wave 2 shim resolved them to 0x58 and 0x8C - so two
# messages the bridge implements were silently absent from every transcript
# while the summary claimed, with a reason attached, that they could not be
# sent. Nothing failed: the test on this list only checks that each entry
# carries a non-empty "why". A transcript that quietly omits a message is
# exactly the coverage hole this tool exists to close, so the list now comes
# from `tools/ant_wire.py`'s UNRESOLVED dict, which is generated from the YAML
# and cannot fall behind it.
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

    `frames` is everything written to the device for this case, in order:
    preamble first, then the frame under test. Every byte written is recorded,
    so a divergence in a preamble shows up as a difference rather than as a
    changed meaning.
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
        # Short: one byte less than the declared minimum. Every handler in
        # dispatch() guards on `len >= N` and leaves `err` at its INVALID_MESSAGE
        # initial value when the guard fails - the reason that initial value is
        # not RESPONSE_NO_ERROR is that a truncated command would otherwise be
        # acknowledged without ever having run.
        if low >= 1:
            cases.append(Case(
                f"{stem}/{MALFORMED}-short",
                preamble + (wire.frame(msg_id, canonical[: low - 1]),),
                f"{low - 1} payload bytes where the message declares {low}"))
        # Long: one byte more than the declared maximum, capped so the frame
        # still fits the parser's body buffer. Beyond that the bridge stops
        # storing bytes and the case would measure the buffer, not the handler.
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

    # A frame whose checksum is wrong by one bit. The bridge drops it silently
    # rather than answering MESG_SERIAL_ERROR, so the expected recording is the
    # request and nothing else - which is a real statement about the firmware
    # and is exactly what a byte diff can check.
    #
    # No preamble on this one. A malformed frame never reaches dispatch(), so
    # channel state cannot affect the answer, and a preamble that replies would
    # make "expect silence" untestable - the case would look answered.
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
    """The highest id no table claims. Computed, so it cannot go stale.

    A hardcoded 0xFE would quietly stop being an unknown id the day somebody
    allocated it - and `protocol/ant_wire.yaml` has an unclaimed-id allocation
    process precisely so that ids do get allocated.
    """
    claimed = set(wire.MESSAGES) | set(getattr(wire, "RADIANT_MESSAGES", {}))
    return max(value for value in range(0x100) if value not in claimed)


def _frame_cases() -> list[Case]:
    """Cases about the frame parser rather than about any one message.

    Last in the run, because two of them deliberately leave junk in the parser's
    input and the next thing down the wire should be a reset.
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

    # A header that declares three body bytes, followed immediately by a
    # complete valid frame. The header eats the valid frame's first bytes as its
    # own body, fails its checksum and is dropped; the parser must then find the
    # next SYNC and carry on. If it does not, everything after this point in the
    # transcript shifts - which a byte diff sees at once.
    truncated = bytes([wire.SYNC_TX, 0x03, wire.MESG_CHANNEL_RADIO_FREQ_ID])
    cases.append(Case(
        f"frame/{MALFORMED}-partial-then-valid", (truncated + freq, freq),
        "a truncated header swallows the frame behind it; the one after that "
        "must still be answered"))
    return cases


def generate_cases() -> list[Case]:
    """Every case, in the fixed order they are sent in.

    Pure: no clock, no device, no environment. Two calls return equal cases,
    which is the property `tools/test_ant_conformance.py` asserts and the
    foundation the byte diff stands on.
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
                # `var`: the length depends on a field inside the payload and
                # there is nothing safe to derive. Two bytes reaches every
                # handler's guard, which is what the case is for.
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

    With `until`, stop as soon as that message id lands. That is only used for
    the reset before each case: waiting the full settle window there would
    double the length of the run for a message whose reply is the one thing
    guaranteed to arrive, and nothing else is in flight at that point because
    the previous case's window already closed.

    The frame bytes are rebuilt with `wire.frame()` rather than kept raw,
    because `FrameReader` hands back (id, payload) and discards the buffer. That
    is byte-exact and not an approximation: the reader has already checked that
    the length byte matches the payload it split out and that the checksum over
    the whole frame is right, so there is exactly one byte sequence those two
    values can have come from.
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
    # Imported here rather than at the top so that every name that can touch a
    # device is inside the one function that does. --list, --replay, --compare
    # and the unit tests never reach this line.
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
                # A stack that does not answer a reset is not going to produce
                # a transcript worth diffing, and carrying on would bury that
                # fact in 260 more cases.
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

    Aligned by position rather than by a longest-common-subsequence: two
    transcripts from the same tool run the same cases in the same order, so a
    positional difference is a real one and an insertion shows up as a run of
    them. Attributing to a case is what makes that readable anyway.
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

    Also, since the Tier 1 reference landed, the integrity record for the
    transcript itself. `tools/test_ant_golden.py` reads `sha256`, `records` and
    `case_index` back out of the committed copy in `archive/benchmarks/` and
    fails if the bytes on disk no longer hash to them - which is the only check
    in the tree that can see a flipped byte inside an otherwise well-formed
    reply, because a framing rule has no opinion about what a dongle should
    have answered.

    Three consequences for anyone editing this function:

    * `case_index` earns its keep as more than a summary - it is what turns
      "this file changed" into "this case changed", so keep it per case and
      keep the hash over the same `<dir><hex>` lines joined with newlines.
    * A transcript committed to `archive/captures/serial/` needs its summary
      committed too, or nothing checks it.
    * Changing how any of these three fields is computed invalidates every
      committed baseline at once. That is a deliberate act, not a refactor.
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
        # A case that expected a reply and got none is not automatically a
        # failure - a bad checksum is meant to be silent - but a *valid* case
        # falling silent means the dongle never answered, and that is worth
        # having in the file rather than only on the terminal.
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
                             "(default: 0.25). The floor on how long that "
                             "takes is ant_probe.READ_TIMEOUT_MS, which is "
                             "deliberately not short: a read timeout racing a "
                             "reply loses it, and that bug cost this project "
                             "0.4 pp of imaginary packet loss")
    parser.add_argument("--no-reset-each", action="store_true",
                        help="do not reset the stack before every case. "
                             "Faster, and gives up the independence that makes "
                             "the transcript diffable after a case is inserted")
    parser.add_argument("--only", metavar="SUBSTRING",
                        help="run only cases whose name contains this. For "
                             "debugging one case; a transcript produced this "
                             "way is not a Tier 1 baseline")
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
