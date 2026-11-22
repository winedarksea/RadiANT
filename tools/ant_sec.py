# SPDX-License-Identifier: Apache-2.0

"""Encode and decode the RadiANT security messages, 0xF1 to 0xF5 and 0xF8.

NOT ANT protocol. These are ours, answered by no ANT device, and their
semantics live in docs/radiant-security.md; only the numbering was decided in
protocol/ant_wire.yaml. The message IDs are imported from tools/ant_wire.py
rather than written here, because that file is generated from the YAML and a
second copy of a number is a second place for it to be wrong.

WHAT THIS MODULE IS FOR

Two things, and it is worth being clear which:

  - Building message bodies to send to a dongle, which tools/ant_features.py
    does behind --radiant-security.
  - Being the readable statement of the wire format, so a host implementer has
    something to check against that is not C. Every field is decoded back, and
    tools/test_ant_sec.py round-trips all of it - a decoder that only ever ran
    against its own encoder would agree with itself about a byte order it had
    got backwards.

WHAT IT DELIBERATELY DOES NOT DO

No key generation, no epoch policy, no persistence. Choosing an epoch that
never repeats is the host's obligation and it is the one thing in this design
that firmware cannot enforce for itself - radiant_sec refuses a non-increasing
epoch, but only against the value it currently holds, which a reset clears.
A tool that invented epochs would make that look solved.

A host that does issue epochs must persist the last epoch USED, not the last
one it issued: a counter wrap moves the channel to epoch + 1 with no 0xF3, so
the epoch to stay above is the one 0xF4 reports at [6..9], read before the
session ends. A wall-clock host must also never issue the same minute twice
(two boots inside one minute). The hostless node discharges this with a
reservation persisted ahead of use (docs/radiant-security.md 3.5); a host that
cannot read 0xF4 before power is lost needs the same kind of reservation.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

from ant_wire import (
    MESG_RADIANT_EPOCH_ID,
    MESG_RADIANT_PAIRING_ID,
    MESG_RADIANT_SEC_CONFIG_ID,
    MESG_RADIANT_SEC_STATUS_ID,
    MESG_RADIANT_SEC_VERDICT_ID,
    MESG_RADIANT_SET_KEY_ID,
    RADIANT_PAIR_ENTER,
    RADIANT_PAIR_EXCHANGE,
    RADIANT_PAIR_LEAVE,
    RADIANT_PAIR_NONCE,
    RADIANT_PAIR_PEER_COMMIT,
    RADIANT_PAIR_COMMIT_MISMATCH,
    RADIANT_PAIR_PEER_NONCE,
    RADIANT_PAIR_SCALAR,
)

__all__ = [
    "SW_CONF", "SW_AUTH", "SW_DROP_UNVER", "SW_DESC_CONF",
    "SW_VERDICT_EVENT",
    "VERDICT_CLEAR", "VERDICT_VERIFIED", "VERDICT_UNVERIFIED",
    "PAGE_LO_MIN", "PAGE_HI_MAX", "LEGAL_W", "KEY_BITS", "STATUS_LEN",
    "STATUS_LEN_V1_26", "STATE_KEYED", "STATE_ARMED",
    "STATUS_VERSION", "SecStatus",
    "encode_config", "decode_config",
    "encode_set_key", "decode_set_key",
    "encode_epoch", "decode_epoch",
    "decode_status", "encode_status",
    "switches_str", "verdict_str",
    "VERDICT_LEN", "decode_verdict", "encode_verdict", "verdict_covers",
    "VerdictResolver", "overflow_all",
    "X25519_BYTES", "PAIR_TIMEOUT_DEFAULT_S", "PAIR_NONCE_BYTES",
    "PAIR_COMMIT_BYTES", "PAIR_ROLE_INITIATOR", "PAIR_ROLE_RESPONDER",
    "encode_pair_enter", "encode_pair_leave", "encode_pair_scalar",
    "encode_pair_exchange", "encode_pair_nonce", "encode_pair_peer_commit",
    "encode_pair_peer_nonce", "decode_pair_reply", "fingerprint_str",
    "PAIR_COMMIT_MISMATCH",
]

# 0xF1's switch bitmask. The same byte the firmware uses - radiant_sec.h's
# RADIANT_SEC_SW_* are these values, asserted by BUILD_ASSERT in
# radiant/src/ant_api/radiant_ant_sec.c so the two cannot drift apart quietly.
SW_CONF = 0x01        # X_CONF, confidentiality
SW_AUTH = 0x02        # X_AUTH, authenticity
SW_DROP_UNVER = 0x04  # drop an unverified window instead of delivering it
SW_DESC_CONF = 0x08   # descriptor confidentiality - REFUSED in v1
SW_VERDICT_EVENT = 0x10  # send 0xF8 per judged window; needs SW_AUTH
SW_KNOWN = 0x1F       # bits 7..5 are reserved and must be zero

VERDICT_CLEAR = 0
VERDICT_VERIFIED = 1
VERDICT_UNVERIFIED = 2

# The secured page range is bounded at both ends so the descriptor (0x00) and
# the ANT+ common pages (0x50/0x51/0x52) stay in the clear mechanically rather
# than by memory.
PAGE_LO_MIN = 0x01
PAGE_HI_MAX = 0x1F

# W must divide both 256 and 65536, which is what makes the counter-derived
# window boundary resynchronise after a lost packet. W=1 is reserved for the
# reliable-command page.
LEGAL_W = (2, 4, 8)

KEY_BITS = 128
KEY_BYTES = KEY_BITS // 8

# 0xF4's length and the structure version this module writes and fully
# understands. Fields are only ever appended and an offset is never reused, so a
# longer body or a higher version still decodes everything named here. The
# version stays 1 through the beta; a reader tells the fields apart by length.
# STATUS_LEN_V1_26 is the shortest body still accepted: the layout before
# dropped_direction was appended at [26..27], which then decodes as None, as
# verdict_events_dropped at [28..29] does from a body shorter than 30, and the
# state flags at [30] from one shorter than 31.
STATUS_LEN = 31
STATUS_LEN_V1_26 = 26

# 0xF4 [30], the state flags. Keyed without armed is PENDING: a new root with
# no epoch yet, when the firmware withholds secured-range pages on TX and drops
# them on RX rather than passing them in the clear.
STATE_KEYED = 0x01
STATE_ARMED = 0x02
STATUS_VERSION = 1

# The epoch headroom radiant_sec reserves so a counter wrap always has
# somewhere to advance into.
EPOCH_HEADROOM = 0x1000
EPOCH_MAX = 0xFFFFFFFF - EPOCH_HEADROOM

EPOCH_FLAG_REAL_TIME = 0x01


def _check_channel(channel: int) -> None:
    if not 0 <= channel <= 0xFF:
        raise ValueError(f"channel {channel} does not fit in a byte")


# ── 0xF1: configure ─────────────────────────────────────────────────────────

def encode_config(channel: int, switches: int, w: int,
                  page_lo: int = PAGE_LO_MIN, page_hi: int = 0x0F) -> bytes:
    """Body for MESG_RADIANT_SEC_CONFIG_ID.

    The two transforms are independent, not a ladder: X_AUTH alone is the most
    useful setting in the table, and X_CONF alone is the weakest useful one -
    it hides the payload and authenticates nothing, which is what ANT+ shipped.
    """
    _check_channel(channel)
    if switches & ~SW_KNOWN:
        raise ValueError(
            f"switches 0x{switches:02X} sets a reserved bit; bits 7..5 must "
            "be zero so a later switch is not silently ignored by a node too "
            "old to know it")
    if switches & SW_VERDICT_EVENT and not switches & SW_AUTH:
        raise ValueError(
            "SW_VERDICT_EVENT without SW_AUTH: only X_AUTH has a verdict, and "
            "the firmware refuses a promise of events that would never come")
    if w not in LEGAL_W:
        raise ValueError(
            f"W={w} is not one of {LEGAL_W}. W must divide 256 and 65536 or "
            "the spread MAC stops resynchronising after a lost packet")
    if not PAGE_LO_MIN <= page_lo <= PAGE_HI_MAX:
        raise ValueError(
            f"page_lo 0x{page_lo:02X} outside 0x{PAGE_LO_MIN:02X}.."
            f"0x{PAGE_HI_MAX:02X}; 0x00 would make the node's own descriptor "
            "unreadable and the node undiscoverable")
    if not PAGE_LO_MIN <= page_hi <= PAGE_HI_MAX:
        raise ValueError(
            f"page_hi 0x{page_hi:02X} outside 0x{PAGE_LO_MIN:02X}.."
            f"0x{PAGE_HI_MAX:02X}; 0x50 is common page 80, which every ANT+ "
            "receiver already understands")
    if page_lo > page_hi:
        raise ValueError(f"page range 0x{page_lo:02X}..0x{page_hi:02X} is empty")

    return bytes([channel, switches, w, page_lo, page_hi])


def decode_config(body: bytes) -> dict:
    if len(body) < 5:
        raise ValueError(f"0xF1 body is {len(body)} bytes, want 5")
    return {
        "channel": body[0],
        "switches": body[1],
        "w": body[2],
        "page_lo": body[3],
        "page_hi": body[4],
    }


# ── 0xF2: set key ───────────────────────────────────────────────────────────

def encode_set_key(channel: int, key: bytes, bits: int = KEY_BITS) -> bytes:
    """Body for MESG_RADIANT_SET_KEY_ID.

    WRITE ONLY. There is no read arm anywhere and a MESG_REQUEST for 0xF2 is
    answered INVALID_MESSAGE rather than with a key.

    Everything else - K_enc, K_auth, K_id, K_cmd - is derived from these
    sixteen bytes, so a pairing moves exactly sixteen bytes and no more.
    """
    _check_channel(channel)
    if bits != KEY_BITS:
        raise ValueError(f"{bits}-bit keys are not supported in v1; only {KEY_BITS}")
    if len(key) != KEY_BYTES:
        raise ValueError(f"key is {len(key)} bytes, want {KEY_BYTES}")
    return bytes([channel, bits]) + bytes(key)


def decode_set_key(body: bytes) -> dict:
    if len(body) < 2 + KEY_BYTES:
        raise ValueError(f"0xF2 body is {len(body)} bytes, want {2 + KEY_BYTES}")
    return {
        "channel": body[0],
        "bits": body[1],
        "key": bytes(body[2:2 + KEY_BYTES]),
    }


# ── 0xF3: epoch ─────────────────────────────────────────────────────────────

def encode_epoch(channel: int, epoch: int, us_into_epoch: int = 0,
                 real_time: bool = False) -> bytes:
    """Body for MESG_RADIANT_EPOCH_ID.

    `us_into_epoch` is the phase a receiver derives the slot counter from,
    rather than from arrival history - the only thing that works for a
    mid-epoch join, a gap longer than 255 slots, or slot-aligned sparse mode.

    Monotonicity is the HOST's job: firmware refuses an epoch <= the one it
    holds, but a reset clears that state. A reboot that restarts the counter
    under an unchanged epoch is a two-time pad for X_CONF and a full session
    replay against X_AUTH.
    """
    _check_channel(channel)
    if not 0 <= epoch <= EPOCH_MAX:
        raise ValueError(
            f"epoch {epoch} outside 0..{EPOCH_MAX}; the top 0x{EPOCH_HEADROOM:X} "
            "is reserved so a counter wrap always has room to advance into")
    # Firmware refuses a phase of more than INT32_MAX channel periods (review
    # 2026-11-17 item 3). 2^52 us is past that at every legal period, so a
    # larger value is a host mistake (ns or a raw tick count), caught here.
    if not 0 <= us_into_epoch < (1 << 52):
        raise ValueError(
            "us_into_epoch must be below 2^52 us; firmware refuses a phase "
            "of more than INT32_MAX channel periods")

    flags = EPOCH_FLAG_REAL_TIME if real_time else 0x00
    return (bytes([channel, flags]) +
            struct.pack("<IQ", epoch, us_into_epoch))


def decode_epoch(body: bytes) -> dict:
    if len(body) < 14:
        raise ValueError(f"0xF3 body is {len(body)} bytes, want 14")
    epoch, us = struct.unpack("<IQ", bytes(body[2:14]))
    return {
        "channel": body[0],
        "flags": body[1],
        "real_time": bool(body[1] & EPOCH_FLAG_REAL_TIME),
        "epoch": epoch,
        "us_into_epoch": us,
    }


# ── 0xF4: status ────────────────────────────────────────────────────────────

@dataclass
class SecStatus:
    """What 0xF4 reports for one channel.

    The counters saturate rather than wrapping. That is deliberate and it
    matters for how they are read: a wrapped counter would let a long noisy run
    look quiet, and "quiet" is exactly the reading these exist to disprove. A
    field sitting at 0xFFFF means "at least that many", not "that many".

    `enrolments` counts pairings on this channel that took - the 0xF5 host
    exchange and the over-air enrolment window alike. It is a security signal,
    not a diagnostic: an enrolment the owner did not perform is the attack. Only
    a host on the node that did the enrolment can read it; other receivers
    cannot see it over the air.

    `dropped_direction` counts secured-range broadcasts a MASTER heard and
    refused: a secured master never receives secured pages from a legitimate
    slave, so each one is a forgery or its own window reflected back
    (docs/radiant-security.md 7.6). None when the body predates the field
    (26 bytes).

    `verdict_events_dropped` counts 0xF8 window-verdict events the event
    queue refused - ring refusals only; a USB drop or a discarded window
    shows only as a 0xF8 seq gap. None when the body predates it (< 30).

    `state_flags` is [30]: STATE_KEYED and STATE_ARMED. None when the body
    predates it (< 31), and then `keyed`, `armed` and `pending` are None too:
    an older body cannot tell an armed channel from a pending one.
    """

    channel: int
    version: int
    switches: int
    w: int
    page_lo: int
    page_hi: int
    epoch: int
    expected_index: int
    windows_verified: int
    windows_unverified: int
    dropped_non_broadcast: int
    dropped_replay: int
    dropped_policy: int
    epoch_advances: int
    verdict: int
    enrolments: int
    dropped_direction: int | None = None
    verdict_events_dropped: int | None = None
    state_flags: int | None = None

    @property
    def keyed(self) -> bool | None:
        """A root installed and X_CONF or X_AUTH on; None if not reported."""
        if self.state_flags is None:
            return None
        return bool(self.state_flags & STATE_KEYED)

    @property
    def armed(self) -> bool | None:
        """Keyed and an epoch set, so the transforms run; None if not
        reported."""
        if self.state_flags is None:
            return None
        return bool(self.state_flags & STATE_ARMED)

    @property
    def pending(self) -> bool | None:
        """Keyed but not armed: secured pages withheld and dropped until
        0xF3. None if not reported."""
        if self.state_flags is None:
            return None
        return bool(self.keyed) and not self.armed

    @property
    def secured(self) -> bool:
        """Is either transform actually on?

        DROP_UNVER alone is not security - it is a delivery policy for a
        verdict nothing is producing - so it does not count here.
        """
        return bool(self.switches & (SW_CONF | SW_AUTH))


def decode_status(body: bytes) -> SecStatus:
    """Decode a 0xF4 body.

    Refuses a body shorter than STATUS_LEN_V1_26, the oldest versioned layout;
    a 26-byte body decodes with dropped_direction None. Decodes the known
    fields from a longer body or a higher version, since fields are only ever
    appended.
    Refuses version 0: firmware always writes a version, even for an unkeyed
    channel, so a zero there means this layout is being applied to bytes that
    are not a versioned 0xF4 (such as the old 23-byte reply, whose [1] is the
    switch byte).
    """
    if len(body) < STATUS_LEN_V1_26:
        raise ValueError(f"0xF4 body is {len(body)} bytes, want at least "
                         f"{STATUS_LEN_V1_26}")
    if body[1] == 0:
        raise ValueError(
            "0xF4 structure version is 0; every versioned reply carries 1 or "
            "more, so this body is not laid out the way this decoder reads it")

    epoch, expected = struct.unpack("<IH", bytes(body[6:12]))
    (verified, unverified, non_bcast, replay,
     policy) = struct.unpack("<HHHHH", bytes(body[12:22]))
    (enrolments,) = struct.unpack("<H", bytes(body[24:26]))
    direction = None
    if len(body) >= 28:
        (direction,) = struct.unpack("<H", bytes(body[26:28]))
    verdict_drops = None
    if len(body) >= 30:
        (verdict_drops,) = struct.unpack("<H", bytes(body[28:30]))
    flags = body[30] if len(body) >= 31 else None

    return SecStatus(
        channel=body[0],
        version=body[1],
        switches=body[2],
        w=body[3],
        page_lo=body[4],
        page_hi=body[5],
        epoch=epoch,
        expected_index=expected,
        windows_verified=verified,
        windows_unverified=unverified,
        dropped_non_broadcast=non_bcast,
        dropped_replay=replay,
        dropped_policy=policy,
        epoch_advances=body[22],
        verdict=body[23],
        enrolments=enrolments,
        dropped_direction=direction,
        verdict_events_dropped=verdict_drops,
        state_flags=flags,
    )


def encode_status(st: SecStatus) -> bytes:
    """The device side of 0xF4, so the decoder can be round-tripped.

    Nothing in the tools sends this - it travels device-to-host only. It exists
    so tools/test_ant_sec.py can assert the decoder against bytes it did not
    itself lay out field by field.
    """
    def sat16(v: int) -> int:
        return 0xFFFF if v > 0xFFFF else v

    return (bytes([st.channel, st.version, st.switches, st.w, st.page_lo,
                   st.page_hi]) +
            struct.pack("<IH", st.epoch, st.expected_index) +
            struct.pack("<HHHHH",
                        sat16(st.windows_verified),
                        sat16(st.windows_unverified),
                        sat16(st.dropped_non_broadcast),
                        sat16(st.dropped_replay),
                        sat16(st.dropped_policy)) +
            bytes([min(st.epoch_advances, 0xFF), st.verdict]) +
            struct.pack("<H", sat16(st.enrolments)) +
            struct.pack("<H", sat16(st.dropped_direction or 0)) +
            struct.pack("<H", sat16(st.verdict_events_dropped or 0)) +
            bytes([(st.state_flags or 0) & 0xFF]))


# ── Human-readable ──────────────────────────────────────────────────────────

def switches_str(switches: int) -> str:
    if switches == 0:
        return "none"
    names = []
    if switches & SW_CONF:
        names.append("X_CONF")
    if switches & SW_AUTH:
        names.append("X_AUTH")
    if switches & SW_DROP_UNVER:
        names.append("drop-unverified")
    if switches & SW_DESC_CONF:
        names.append("descriptor-conf")
    if switches & SW_VERDICT_EVENT:
        names.append("verdict-event")
    if switches & ~SW_KNOWN:
        names.append(f"reserved:0x{switches & ~SW_KNOWN:02X}")
    return "|".join(names)


def verdict_str(verdict: int) -> str:
    return {
        VERDICT_CLEAR: "clear",
        VERDICT_VERIFIED: "verified",
        VERDICT_UNVERIFIED: "unverified",
    }.get(verdict, f"unknown:{verdict}")


# ── 0xF8: one window's verdict ──────────────────────────────────────────────
#
# Sent unsolicited, but only on a channel whose 0xF1 set SW_VERDICT_EVENT. It
# follows every packet it covers in the same queue. A verdict is only safe to
# act on under the host rule in protocol/ant_wire.yaml (0xF8) and
# docs/radiant-security.md 3.2, which VerdictResolver below implements: the
# first covering verdict after a packet binds it; a sequence gap, a queue
# overflow or the deadline settles what is pending as unverified; and the
# first verdict after a start or a flush binds nothing.

VERDICT_LEN = 7


def decode_verdict(body: bytes) -> dict:
    """Body of MESG_RADIANT_SEC_VERDICT_ID:
    [ch, verdict, window_start LE, W, seq LE].

    A longer body decodes the fields named here (fields are only appended).
    A verdict other than verified/unverified, or a W the firmware cannot
    have, is refused rather than guessed at.
    """
    if len(body) < VERDICT_LEN:
        raise ValueError(
            f"0xF8 body is {len(body)} bytes, want at least {VERDICT_LEN}")
    if body[1] not in (VERDICT_VERIFIED, VERDICT_UNVERIFIED):
        raise ValueError(f"0xF8 verdict {body[1]} is neither verified nor "
                         "unverified")
    if body[4] not in LEGAL_W:
        raise ValueError(f"0xF8 W={body[4]} is not one of {LEGAL_W}")
    window_start, = struct.unpack_from("<H", body, 2)
    seq, = struct.unpack_from("<H", body, 5)
    return {
        "channel": body[0],
        "verdict": body[1],
        "window_start": window_start,
        "w": body[4],
        "seq": seq,
    }


def encode_verdict(channel: int, verdict: int, window_start: int,
                   w: int, seq: int = 0) -> bytes:
    """The firmware's side, for tests and simulators."""
    _check_channel(channel)
    if verdict not in (VERDICT_VERIFIED, VERDICT_UNVERIFIED):
        raise ValueError(f"0xF8 carries verified or unverified, not {verdict}")
    if w not in LEGAL_W:
        raise ValueError(f"W={w} is not one of {LEGAL_W}")
    if not 0 <= window_start <= 0xFFFF or window_start % w:
        raise ValueError(
            f"window_start {window_start} is not a 16-bit multiple of W={w}")
    if not 0 <= seq <= 0xFFFF:
        raise ValueError(f"seq {seq} does not fit in 16 bits")
    return (bytes([channel, verdict]) + struct.pack("<H", window_start) +
            bytes([w]) + struct.pack("<H", seq))


def verdict_covers(verdict: dict, channel: int, payload: bytes,
                   page_lo: int, page_hi: int) -> bool:
    """Could this 0xF8 judge `payload` (8 bytes, BROADCAST_DATA after the
    channel byte) received on `channel`?

    Only a page inside the channel's secured range [page_lo, page_hi] (0xF4
    [4..5]) on the SAME channel is ever judged: a page outside it reaches the
    host in the clear and is never verified, whatever its byte [1] says.

    For a covered page, byte [1] is the counter's low 8 bits, so it is in the
    window when (byte1 - window_start) mod 256 < W, unambiguous since W <= 8.
    "Could" because matching is necessary, not sufficient: only the FIRST
    covering verdict after the packet is the packet's, and only when nothing
    since says verdicts were lost (VerdictResolver).
    """
    if len(payload) < 2:
        raise ValueError("a secured page has at least two bytes")
    if channel != verdict["channel"]:
        return False
    if not page_lo <= payload[0] <= page_hi:
        return False
    return ((payload[1] - verdict["window_start"]) & 0xFF) < verdict["w"]


class VerdictResolver:
    """The host rule for 0xF8, for one channel. Feed it what arrives, in order.

    data(payload, t)        a BROADCAST_DATA on this channel, at time t (s);
                            returns VERDICT_CLEAR at once for a page outside
                            the secured range, else None (pending)
    verdict(body, now)      a 0xF8 for this channel, arriving at `now`; the
                            deadline is applied first
    overflow()              EVENT_QUE_OVERFLOW / EVENT_SERIAL_QUE_OVERFLOW, or
                            a loss the transport reported (overflow_all()
                            feeds the events to every channel's resolver)
    expire(now)             settle packets pending longer than the deadline
    flush()                 CHANNEL_CLOSED, the host's own 0xF3, reset, and
                            the host's own 0xF1 unless it knows that tuple is
                            already installed (an identical 0xF1 keeps the
                            firmware's windows and seq; flushing on it only
                            marks genuine packets unverified)

    Each settled packet is appended to `.settled` as (payload, verdict). A
    packet is settled exactly once: by the first covering verdict after it,
    or as unverified by a gap, an overflow, the deadline, a flush, or the
    first verdict after a start or a flush - which only sets the seq
    baseline, because a packet still in flight across the flush may be
    judged by no verdict at all (the firmware threw its window away).
    """

    def __init__(self, channel: int, w: int, period_s: float,
                 page_lo: int, page_hi: int, slack_s: float = 0.25):
        self.channel = channel
        self.w = w
        self.page_lo = page_lo
        self.page_hi = page_hi
        # A genuine verdict comes on the last packet of the NEXT window,
        # at most 2W-1 periods after the packet - unless clear-only windows
        # follow, when it comes later and this deadline has already settled
        # the packets as unverified. That fails safe, so it stays simple.
        self.deadline_s = (2 * w - 1) * period_s + slack_s
        self.pending: list[tuple[bytes, float]] = []
        self.settled: list[tuple[bytes, int]] = []
        self.next_seq: int | None = None

    def data(self, payload: bytes, t: float):
        if not self.page_lo <= payload[0] <= self.page_hi:
            self.settled.append((bytes(payload), VERDICT_CLEAR))
            return VERDICT_CLEAR
        self.pending.append((bytes(payload), t))
        return None

    def _settle_all(self, how: int) -> None:
        for p, _ in self.pending:
            self.settled.append((p, how))
        self.pending = []

    def verdict(self, body: bytes, now: float) -> None:
        v = decode_verdict(body)
        if v["channel"] != self.channel:
            raise ValueError(f"0xF8 for channel {v['channel']} fed to the "
                             f"resolver for {self.channel}")
        # A packet older than the deadline can't be this verdict's: a read
        # stall longer than it settles everything held.
        self.expire(now)
        if self.next_seq is None or v["seq"] != self.next_seq:
            # No baseline yet, or verdicts were lost: nothing pending may be
            # resolved by this or any later verdict. Settled BEFORE it.
            self._settle_all(VERDICT_UNVERIFIED)
            self.next_seq = (v["seq"] + 1) & 0xFFFF
            return
        self.next_seq = (v["seq"] + 1) & 0xFFFF
        keep = []
        for p, t in self.pending:
            if verdict_covers(v, self.channel, p, self.page_lo, self.page_hi):
                self.settled.append((p, v["verdict"]))
            else:
                keep.append((p, t))
        self.pending = keep

    def overflow(self) -> None:
        self._settle_all(VERDICT_UNVERIFIED)

    def expire(self, now: float) -> None:
        keep = []
        for p, t in self.pending:
            if now - t > self.deadline_s:
                self.settled.append((p, VERDICT_UNVERIFIED))
            else:
                keep.append((p, t))
        self.pending = keep

    def flush(self) -> None:
        self._settle_all(VERDICT_UNVERIFIED)
        # The next verdict only re-establishes the baseline.
        self.next_seq = None


def overflow_all(resolvers) -> None:
    """EVENT_QUE_OVERFLOW (0x35) or EVENT_SERIAL_QUE_OVERFLOW (0x34): sent as
    channel 0, but the loss is the queue's, so it reaches every channel."""
    for r in resolvers:
        r.overflow()


# ── 0xF5: pairing ───────────────────────────────────────────────────────────

X25519_BYTES = 32
PAIR_TIMEOUT_DEFAULT_S = 60


def encode_pair_enter(channel: int, timeout_s: int = 0) -> bytes:
    """Enter pairing mode.

    `timeout_s` of 0 means the 60-second default and NEVER "forever". A node in
    pairing mode accepts a key from whoever asks, so an interface where 0 meant
    no bound would turn one forgotten command into a permanently open node.
    """
    _check_channel(channel)
    if not 0 <= timeout_s <= 0xFF:
        raise ValueError(f"timeout {timeout_s}s does not fit in a byte")
    return bytes([channel, RADIANT_PAIR_ENTER, timeout_s])


def encode_pair_leave(channel: int) -> bytes:
    _check_channel(channel)
    return bytes([channel, RADIANT_PAIR_LEAVE])


def encode_pair_scalar(channel: int, scalar: bytes) -> bytes:
    """Supply the host's 32-byte X25519 private scalar.

    THE SCALAR COMES FROM THE HOST because the only entropy source on nRF54L is
    psa_rng/CRACEN and reaching it drags nrf_security into every build. The
    honest consequence is that a host-less node cannot pair this way.

    Nothing on the device can check that this is random. A host that sends a
    constant has a node whose private key is public, and neither end can tell -
    so generate it with `secrets.token_bytes(32)` and never with `random`.
    """
    _check_channel(channel)
    if len(scalar) != X25519_BYTES:
        raise ValueError(f"scalar is {len(scalar)} bytes, want {X25519_BYTES}")
    return bytes([channel, RADIANT_PAIR_SCALAR]) + bytes(scalar)


def encode_pair_exchange(channel: int, peer_pub: bytes) -> bytes:
    """The Diffie-Hellman half. Installs nothing and returns no fingerprint:
    those wait for the nonce commit/reveal below."""
    _check_channel(channel)
    if len(peer_pub) != X25519_BYTES:
        raise ValueError(
            f"public key is {len(peer_pub)} bytes, want {X25519_BYTES}")
    return bytes([channel, RADIANT_PAIR_EXCHANGE]) + bytes(peer_pub)


# The commit/reveal (docs/radiant-security.md section 7.4). Without it a man in
# the middle grinds key pairs until both screens show the same six digits; with
# it, one of the two nonces is always fixed before the other is known. The
# host sequence, A = initiator and B = responder, each host driving its own end:
#
#   both  ENTER, SCALAR -> own key; [keys cross]; EXCHANGE(peer key)
#   B     NONCE(RESPONDER, Nb) -> Cb;                  [Cb to A]
#   A     NONCE(INITIATOR, Na); PEER_COMMIT(Cb);       [Na to B]
#   B     PEER_NONCE(Na) -> fp;                        [Nb to A]
#   A     PEER_NONCE(Nb) -> fp, or a refusal if Nb does not open Cb
#
# The two waits are the protocol: A sends Na only after PEER_COMMIT has been
# answered, and B sends Nb only after its own PEER_NONCE (the node cannot
# enforce B's wait - Nb never passes through it). Nonces come from
# `secrets.token_bytes(16)`, like the scalar.
#
# The key is live once PEER_NONCE answers, before the humans compare. On a
# mismatch, an abort or a timeout at either end, remove it: close the channel,
# and after EVENT_CHANNEL_CLOSED send MESG_UNASSIGN_CHANNEL (0x41) and check it
# answered NO_ERROR (sent before the close completes it is refused and the key
# stays live). LEAVE does not uninstall it. And
# an abort at one end while the other shows digits is a FAILED pairing, never
# one to retry until the digits match: the 10^-6 bound is per attempt.

PAIR_NONCE_BYTES = 16
PAIR_COMMIT_BYTES = 16
PAIR_ROLE_INITIATOR = 0
PAIR_ROLE_RESPONDER = 1


def _check_nonce(what: str, value: bytes, want: int) -> None:
    if len(value) != want:
        raise ValueError(f"{what} is {len(value)} bytes, want {want}")
    if what == "nonce" and not any(value):
        # The device refuses it too: all zeros is enrolment's constant, and a
        # host that produced one has no random source.
        raise ValueError("an all-zero nonce is refused")


#: The response code (not a reply body) when the initiator's PEER_NONCE does
#: not open the responder's commitment: the man-in-the-middle signature, kept
#: apart from INVALID_PARAMETER_PROVIDED (an out-of-order call). The exchange
#: is wiped; the installed key, if any, is not - see the sequence above.
PAIR_COMMIT_MISMATCH = RADIANT_PAIR_COMMIT_MISMATCH


def encode_pair_nonce(channel: int, role: int, nonce: bytes) -> bytes:
    """This end's role and nonce. A responder's reply carries Cb."""
    _check_channel(channel)
    if role not in (PAIR_ROLE_INITIATOR, PAIR_ROLE_RESPONDER):
        raise ValueError(f"role {role} is neither initiator (0) nor "
                         "responder (1)")
    _check_nonce("nonce", nonce, PAIR_NONCE_BYTES)
    return bytes([channel, RADIANT_PAIR_NONCE, role]) + bytes(nonce)


def encode_pair_peer_commit(channel: int, commit: bytes) -> bytes:
    """Initiator only: the responder's commitment, before Na goes out."""
    _check_channel(channel)
    _check_nonce("commitment", commit, PAIR_COMMIT_BYTES)
    return bytes([channel, RADIANT_PAIR_PEER_COMMIT]) + bytes(commit)


def encode_pair_peer_nonce(channel: int, nonce: bytes) -> bytes:
    """The peer's nonce. The reply carries the fingerprint."""
    _check_channel(channel)
    _check_nonce("nonce", nonce, PAIR_NONCE_BYTES)
    return bytes([channel, RADIANT_PAIR_PEER_NONCE]) + bytes(nonce)


def decode_pair_reply(body: bytes) -> dict:
    """Decode a 0xF5 reply.

    Every reply leads with [channel, sub-command], which is what lets a host
    reading a stream of them tell which one answered: a public key, a
    commitment and a fingerprint are all "some bytes after a channel byte"
    otherwise. A NONCE reply carries a commitment only from a responder, so
    its length is what says which.
    """
    if len(body) < 2:
        raise ValueError(f"0xF5 reply is {len(body)} bytes, want at least 2")

    out = {"channel": body[0], "subcmd": body[1]}

    if body[1] == RADIANT_PAIR_SCALAR:
        if len(body) < 2 + X25519_BYTES:
            raise ValueError("a scalar reply carries the 32-byte public key")
        out["public_key"] = bytes(body[2:2 + X25519_BYTES])
    elif body[1] == RADIANT_PAIR_NONCE and len(body) > 2:
        if len(body) < 2 + PAIR_COMMIT_BYTES:
            raise ValueError("a responder's nonce reply carries a 16-byte "
                             "commitment")
        out["commit"] = bytes(body[2:2 + PAIR_COMMIT_BYTES])
    elif body[1] == RADIANT_PAIR_PEER_NONCE:
        if len(body) < 5:
            raise ValueError("a peer-nonce reply carries a u24 fingerprint")
        out["fingerprint"] = (int(body[2]) | (int(body[3]) << 8) |
                              (int(body[4]) << 16))
    return out


def fingerprint_str(fp: int) -> str:
    """Six digits, zero-padded.

    Padded because "042317" and "42317" are the same number and a user
    comparing two screens is comparing strings. An unpadded fingerprint that
    happened to start with a zero would look like a mismatch and teach the user
    to ignore the check.
    """
    if not 0 <= fp <= 999999:
        raise ValueError(f"fingerprint {fp} is not six digits")
    return f"{fp:06d}"


# The message IDs, re-exported so a caller needs one import rather than two.
MESG_CONFIG = MESG_RADIANT_SEC_CONFIG_ID
MESG_SET_KEY = MESG_RADIANT_SET_KEY_ID
MESG_EPOCH = MESG_RADIANT_EPOCH_ID
MESG_STATUS = MESG_RADIANT_SEC_STATUS_ID
MESG_PAIRING = MESG_RADIANT_PAIRING_ID
MESG_VERDICT = MESG_RADIANT_SEC_VERDICT_ID
