"""Encode and decode the RadiANT security messages, 0xF1 to 0xF4.

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
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

from ant_wire import (
    MESG_RADIANT_EPOCH_ID,
    MESG_RADIANT_SEC_CONFIG_ID,
    MESG_RADIANT_SEC_STATUS_ID,
    MESG_RADIANT_SET_KEY_ID,
)

__all__ = [
    "SW_CONF", "SW_AUTH", "SW_DROP_UNVER", "SW_DESC_CONF",
    "VERDICT_CLEAR", "VERDICT_VERIFIED", "VERDICT_UNVERIFIED",
    "PAGE_LO_MIN", "PAGE_HI_MAX", "LEGAL_W", "KEY_BITS", "STATUS_LEN",
    "SecStatus",
    "encode_config", "decode_config",
    "encode_set_key", "decode_set_key",
    "encode_epoch", "decode_epoch",
    "decode_status", "encode_status",
    "switches_str", "verdict_str",
]

# 0xF1's switch bitmask. The same byte the firmware uses - radiant_sec.h's
# RADIANT_SEC_SW_* are these values, asserted by BUILD_ASSERT in
# radiant_core/src/radiant_sec_host.c so the two cannot drift apart quietly.
SW_CONF = 0x01        # X_CONF, confidentiality
SW_AUTH = 0x02        # X_AUTH, authenticity
SW_DROP_UNVER = 0x04  # drop an unverified window instead of delivering it
SW_DESC_CONF = 0x08   # descriptor confidentiality - REFUSED in v1
SW_KNOWN = 0x0F       # bits 7..4 are reserved and must be zero

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
STATUS_LEN = 23

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
            f"switches 0x{switches:02X} sets a reserved bit; bits 7..4 must "
            "be zero so a later switch is not silently ignored by a node too "
            "old to know it")
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

    `us_into_epoch` is the phase a receiver derives the packet counter from,
    rather than from arrival history - the only thing that works for a
    mid-epoch join, for a gap longer than 255 packets, and for sparse mode.

    THE MONOTONICITY IS THE HOST'S JOB. The firmware refuses an epoch less than
    or equal to the one it currently holds, but a reset clears that, so the
    obligation to persist the last epoch issued and never reissue it sits here
    - outside anything this repository can enforce. A reboot that restarted the
    counter under an unchanged epoch is a two-time pad for X_CONF and a full
    session replay against X_AUTH.
    """
    _check_channel(channel)
    if not 0 <= epoch <= EPOCH_MAX:
        raise ValueError(
            f"epoch {epoch} outside 0..{EPOCH_MAX}; the top 0x{EPOCH_HEADROOM:X} "
            "is reserved so a counter wrap always has room to advance into")
    if not 0 <= us_into_epoch < (1 << 64):
        raise ValueError("us_into_epoch does not fit in 64 bits")

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
    """

    channel: int
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

    @property
    def secured(self) -> bool:
        """Is either transform actually on?

        DROP_UNVER alone is not security - it is a delivery policy for a
        verdict nothing is producing - so it does not count here.
        """
        return bool(self.switches & (SW_CONF | SW_AUTH))


def decode_status(body: bytes) -> SecStatus:
    if len(body) < STATUS_LEN:
        raise ValueError(f"0xF4 body is {len(body)} bytes, want {STATUS_LEN}")

    epoch, expected = struct.unpack("<IH", bytes(body[5:11]))
    (verified, unverified, non_bcast, replay,
     policy) = struct.unpack("<HHHHH", bytes(body[11:21]))

    return SecStatus(
        channel=body[0],
        switches=body[1],
        w=body[2],
        page_lo=body[3],
        page_hi=body[4],
        epoch=epoch,
        expected_index=expected,
        windows_verified=verified,
        windows_unverified=unverified,
        dropped_non_broadcast=non_bcast,
        dropped_replay=replay,
        dropped_policy=policy,
        epoch_advances=body[21],
        verdict=body[22],
    )


def encode_status(st: SecStatus) -> bytes:
    """The device side of 0xF4, so the decoder can be round-tripped.

    Nothing in the tools sends this - it travels device-to-host only. It exists
    so tools/test_ant_sec.py can assert the decoder against bytes it did not
    itself lay out field by field.
    """
    def sat16(v: int) -> int:
        return 0xFFFF if v > 0xFFFF else v

    return (bytes([st.channel, st.switches, st.w, st.page_lo, st.page_hi]) +
            struct.pack("<IH", st.epoch, st.expected_index) +
            struct.pack("<HHHHH",
                        sat16(st.windows_verified),
                        sat16(st.windows_unverified),
                        sat16(st.dropped_non_broadcast),
                        sat16(st.dropped_replay),
                        sat16(st.dropped_policy)) +
            bytes([min(st.epoch_advances, 0xFF), st.verdict]))


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
    if switches & ~SW_KNOWN:
        names.append(f"reserved:0x{switches & ~SW_KNOWN:02X}")
    return "|".join(names)


def verdict_str(verdict: int) -> str:
    return {
        VERDICT_CLEAR: "clear",
        VERDICT_VERIFIED: "verified",
        VERDICT_UNVERIFIED: "unverified",
    }.get(verdict, f"unknown:{verdict}")


# The message IDs, re-exported so a caller needs one import rather than two.
MESG_CONFIG = MESG_RADIANT_SEC_CONFIG_ID
MESG_SET_KEY = MESG_RADIANT_SET_KEY_ID
MESG_EPOCH = MESG_RADIANT_EPOCH_ID
MESG_STATUS = MESG_RADIANT_SEC_STATUS_ID
