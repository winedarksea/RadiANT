#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Encode and decode the ANT+ data pages this project cares about.

Pure functions over 8-byte payloads: no USB, no serial port, no hardware. That
is deliberate. Everything else in tools/ needs a board attached before it can
say anything at all, so a byte-order mistake costs a flash cycle to find. Here
the same mistake is a unittest away (tools/test_ant_pages.py), and the encoders
and decoders check each other.

The constants mirror zephyr_aerosense/src/ant/ant_pages.h, which is why they
carry the same values with different names: these pages are the contract
between the two projects, and a page built here is fed to that project's C
decoders in its replay tests.

Two traps worth stating up front, because both produce plausible-looking
garbage rather than an error:

- The combined speed-and-cadence page (device type 0x79) has **no page-number
  byte**. Byte 0 is the low half of the cadence event time. Dispatching on
  payload[0] the way every other profile does will decode it as whatever page
  that byte happens to name.
- Page 0x20 is big-endian. Every other multi-byte field in every other page
  here is little-endian.
"""

from __future__ import annotations

import dataclasses
import math

# Network. 0x39 is 2457 MHz, the ANT+ public frequency; ant_scan.py spells the
# same number 57 because the message takes it as an offset from 2400 MHz.
ANT_PLUS_RF_FREQ = 0x39
ANT_PLUS_NETWORK_NUM = 0

# Bicycle Power, device type 0x0B, 8182 counts of 32768 Hz (~4.0049 Hz).
BPWR_DEVICE_TYPE = 0x0B
BPWR_PERIOD = 8182
PAGE_POWER_STANDARD = 0x10
PAGE_POWER_WHEEL_TORQUE = 0x11
PAGE_POWER_CRANK_TORQUE = 0x12
PAGE_POWER_TORQUE_FREQ = 0x20

# Bike speed and cadence. The combined sensor carries both halves in one page
# and is the only one of the three that reports cadence and speed together.
BSC_SPEED_DEVICE_TYPE = 0x7B
BSC_CADENCE_DEVICE_TYPE = 0x7A
BSC_COMBINED_DEVICE_TYPE = 0x79
BSC_SPEED_PERIOD = 0x1FB6      # 8118, ~4.04 Hz
BSC_CADENCE_PERIOD = 0x1FA6    # 8102, ~4.05 Hz
BSC_COMBINED_PERIOD = 0x1F96   # 8086, ~4.05 Hz

# Common pages. The profiles require both at least once per 65 messages, so a
# receiver that pairs on manufacturer id has something to pair on.
PAGE_COMMON_MANUFACTURER = 0x50   # page 80
PAGE_COMMON_PRODUCT = 0x51        # page 81
PAGE_COMMON_BATTERY = 0x52        # page 82
# Data pages between common-page pairs. 120 rather than the 64 this used to
# use, because 64 is not what a real sensor does: sdk-ant's certified bicycle
# power profile interleaves page 80 at 119 and page 81 at 120, commented
# "Minimum: Interleave every 121 messages". A simulator that sends them twice
# as often as the profile requires is not simulating anything.
COMMON_PAGE_INTERVAL = 120

# Marker for "this field is not being reported". The profiles spell it 0xFF in
# a byte and 0xFFFF in a word, and a receiver that treats it as a number reads
# 255 rpm or 65535 W.
INVALID_U8 = 0xFF
INVALID_U16 = 0xFFFF
INVALID_U32 = 0xFFFFFFFF

# Accumulators are 8 or 16 bits wide and are *meant* to wrap. A receiver
# subtracts in the same width and gets the right delta; one that promotes to
# int first gets a large negative number exactly once per wrap.
U8_WRAP = 1 << 8
U16_WRAP = 1 << 16
U32_WRAP = 1 << 32


def _u8(value: int) -> int:
    return value & 0xFF


def _u16(value: int) -> int:
    return value & 0xFFFF


def _le16(value: int) -> bytes:
    return bytes([value & 0xFF, (value >> 8) & 0xFF])


def _be16(value: int) -> bytes:
    return bytes([(value >> 8) & 0xFF, value & 0xFF])


def _rd_le16(payload: bytes, offset: int) -> int:
    return payload[offset] | (payload[offset + 1] << 8)


def _rd_be16(payload: bytes, offset: int) -> int:
    return (payload[offset] << 8) | payload[offset + 1]


def delta_u8(now: int, before: int) -> int:
    """Difference between two 8-bit accumulator readings, across the wrap."""
    return (now - before) % U8_WRAP


def delta_u16(now: int, before: int) -> int:
    """Difference between two 16-bit accumulator readings, across the wrap."""
    return (now - before) % U16_WRAP


# ---------------------------------------------------------------------------
# Bicycle power
# ---------------------------------------------------------------------------


def encode_power_std(event_count: int, cadence: int | None, acc_power: int,
                     inst_power: int, pedal_power: int | None = None) -> bytes:
    """Page 0x10, Standard Power Only.

        [0]    page 0x10
        [1]    update event count, +1 per power event, wraps at 256
        [2]    pedal power balance, 0xFF when not reported
        [3]    instantaneous cadence in rpm, 0xFF when not reported
        [4..5] accumulated power in watts (LE), wraps at 65536
        [6..7] instantaneous power in watts (LE)

    `cadence` and `pedal_power` accept None for "not reported" so callers do
    not have to remember which sentinel this page uses.
    """
    return bytes([
        PAGE_POWER_STANDARD,
        _u8(event_count),
        INVALID_U8 if pedal_power is None else _u8(pedal_power),
        INVALID_U8 if cadence is None else _u8(cadence),
    ]) + _le16(acc_power) + _le16(inst_power)


def decode_power_std(payload: bytes) -> dict:
    return {
        "page": payload[0],
        "event_count": payload[1],
        "pedal_power": None if payload[2] == INVALID_U8 else payload[2],
        "cadence": None if payload[3] == INVALID_U8 else payload[3],
        "acc_power": _rd_le16(payload, 4),
        "inst_power": _rd_le16(payload, 6),
    }


def encode_power_torque(page: int, event_count: int, ticks: int,
                        cadence: int | None, acc_period: int,
                        acc_torque: int) -> bytes:
    """Pages 0x11 (wheel torque) and 0x12 (crank torque).

        [0]    page 0x11 or 0x12
        [1]    update event count, +1 per wheel/crank revolution
        [2]    wheel or crank tick count, wraps at 256
        [3]    instantaneous cadence in rpm, 0xFF when not reported
        [4..5] accumulated period in 1/2048 s (LE), wraps at 65536
        [6..7] accumulated torque in 1/32 Nm (LE), wraps at 65536

    All four accumulators wrap independently. Emitting a run long enough to
    take each of them through its wrap is the point of these pages: a receiver
    that widens before subtracting is correct for hours and then reports a
    single absurd sample, which is easy to dismiss as radio noise.
    """
    if page not in (PAGE_POWER_WHEEL_TORQUE, PAGE_POWER_CRANK_TORQUE):
        raise ValueError(f"page 0x{page:02X} is not a torque page")

    return bytes([
        page,
        _u8(event_count),
        _u8(ticks),
        INVALID_U8 if cadence is None else _u8(cadence),
    ]) + _le16(acc_period) + _le16(acc_torque)


def decode_power_torque(payload: bytes) -> dict:
    return {
        "page": payload[0],
        "event_count": payload[1],
        "ticks": payload[2],
        "cadence": None if payload[3] == INVALID_U8 else payload[3],
        "acc_period": _rd_le16(payload, 4),
        "acc_torque": _rd_le16(payload, 6),
    }


def encode_power_torque_freq(event_count: int, slope_tenth_nm_hz: int,
                             time_stamp: int, torque_ticks: int) -> bytes:
    """Page 0x20, Crank Torque Frequency. Big-endian, unlike every other page.

        [0]    page 0x20
        [1]    update event count
        [2..3] slope in 1/10 Nm/Hz (BE)
        [4..5] time stamp in 1/2000 s (BE), wraps at 65536
        [6..7] torque ticks stamp (BE), wraps at 65536

    Some power meters emit only this page. aerosense ignores it today, which is
    write-back item 7; encoding it here is what makes that testable.
    """
    return (bytes([PAGE_POWER_TORQUE_FREQ, _u8(event_count)])
            + _be16(slope_tenth_nm_hz) + _be16(time_stamp)
            + _be16(torque_ticks))


def decode_power_torque_freq(payload: bytes) -> dict:
    return {
        "page": payload[0],
        "event_count": payload[1],
        "slope_tenth_nm_hz": _rd_be16(payload, 2),
        "time_stamp": _rd_be16(payload, 4),
        "torque_ticks": _rd_be16(payload, 6),
    }


def power_from_torque(delta_torque: int, delta_period: int) -> float:
    """Average watts between two torque-page samples.

    ANT+ Bicycle Power device profile:
        avg_torque_Nm  = delta_torque / 32 / delta_event
        avg_omega_rads = 2*pi * delta_event * 2048 / delta_period
        power_w        = avg_torque_Nm * avg_omega_rads

    delta_event cancels, which is worth writing down: the event count is not
    needed for the power figure, only to tell "no new event" from "a new event
    with zero torque". Both show up as delta_period == 0 otherwise.
    """
    if delta_period == 0:
        raise ZeroDivisionError("no elapsed accumulated period")
    return delta_torque * (2.0 * math.pi * 64.0) / delta_period


def power_from_torque_freq(delta_event: int, delta_time: int,
                           delta_ticks: int, slope_tenth_nm_hz: int,
                           offset_hz: float = 0.0) -> float:
    """Average watts between two page-0x20 samples.

    Unlike the torque pages, delta_event does *not* cancel here: the torque
    comes from a tick frequency and the angular velocity from the event count,
    so both are needed.
    """
    if delta_time == 0:
        raise ZeroDivisionError("no elapsed time stamp")
    elapsed_s = delta_time / 2000.0
    torque_hz = delta_ticks / elapsed_s - offset_hz
    torque_nm = torque_hz / (slope_tenth_nm_hz / 10.0)
    omega = 2.0 * math.pi * delta_event / elapsed_s
    return torque_nm * omega


def torque_nm_for(watts: float, rpm: float) -> float:
    """Crank torque that produces `watts` at `rpm`. 100 W at 80 rpm is ~11.9 Nm."""
    if rpm <= 0.0:
        return 0.0
    return watts / (2.0 * math.pi * rpm / 60.0)


# ---------------------------------------------------------------------------
# Bike speed and cadence
# ---------------------------------------------------------------------------


def encode_bsc_combined(cad_event_time: int, cad_revs: int,
                       spd_event_time: int, spd_revs: int) -> bytes:
    """The combined speed-and-cadence page, device type 0x79.

        [0..1] cadence event time in 1/1024 s (LE), wraps at 65536
        [2..3] cumulative crank revolutions (LE), wraps at 65536
        [4..5] speed event time in 1/1024 s (LE), wraps at 65536
        [6..7] cumulative wheel revolutions (LE), wraps at 65536

    There is no page number. This page is identified by the channel's device
    type and nothing else, so a receiver has to know what it asked for. That is
    also why the cadence half is so easy to lose: bytes [4..7] alone are a
    valid-looking speed-only page, and reading only those is exactly what
    aerosense does today (write-back item 2).
    """
    return (_le16(cad_event_time) + _le16(cad_revs)
            + _le16(spd_event_time) + _le16(spd_revs))


def decode_bsc_combined(payload: bytes) -> dict:
    return {
        "cad_event_time": _rd_le16(payload, 0),
        "cad_revs": _rd_le16(payload, 2),
        "spd_event_time": _rd_le16(payload, 4),
        "spd_revs": _rd_le16(payload, 6),
    }


def cadence_rpm_from(delta_revs: int, delta_event_time: int) -> float:
    """Crank rpm between two combined-page samples. Event time is 1/1024 s."""
    if delta_event_time == 0:
        raise ZeroDivisionError("no elapsed event time")
    return delta_revs * 1024.0 * 60.0 / delta_event_time


def speed_mps_from(delta_revs: int, delta_event_time: int,
                   wheel_circ_m: float = 2.105) -> float:
    """Metres per second between two combined-page samples."""
    if delta_event_time == 0:
        raise ZeroDivisionError("no elapsed event time")
    return delta_revs * wheel_circ_m * 1024.0 / delta_event_time


# ---------------------------------------------------------------------------
# Common pages
# ---------------------------------------------------------------------------


def encode_common_80(hw_revision: int, manufacturer_id: int,
                     model_number: int) -> bytes:
    """Page 80, Manufacturer's Information.

        [0]    page 0x50
        [1..2] reserved, 0xFF
        [3]    hardware revision
        [4..5] manufacturer id (LE)
        [6..7] model number (LE)
    """
    return (bytes([PAGE_COMMON_MANUFACTURER, INVALID_U8, INVALID_U8,
                   _u8(hw_revision)])
            + _le16(manufacturer_id) + _le16(model_number))


def decode_common_80(payload: bytes) -> dict:
    return {
        "page": payload[0],
        "hw_revision": payload[3],
        "manufacturer_id": _rd_le16(payload, 4),
        "model_number": _rd_le16(payload, 6),
    }


def encode_common_81(sw_revision_main: int, serial_number: int | None,
                     sw_revision_supplemental: int | None = None) -> bytes:
    """Page 81, Product Information.

        [0]    page 0x51
        [1]    reserved, 0xFF
        [2]    supplemental software revision, 0xFF when not used
        [3]    main software revision
        [4..7] serial number (LE, 32-bit), 0xFFFFFFFF when not supplied

    `serial_number=None` emits the not-supplied sentinel, and that is the whole
    of the page 81 privacy rule.

    A 32-bit globally unique serial broadcast in the clear every 30 seconds is
    STRICTLY MORE IDENTIFYING than the 16-bit device number, it is unaffected
    by any device-number re-roll, and it therefore defeats identity Tiers 1 and
    2 outright. A node that re-rolls its device number and keeps broadcasting
    its serial has not changed identity; it has added a field. See
    docs/radiant-security.md section 5.4, which makes this normative and
    independent of every security switch.
    """
    supplemental = (INVALID_U8 if sw_revision_supplemental is None
                    else _u8(sw_revision_supplemental))
    serial = (INVALID_U32 if serial_number is None
              else serial_number & 0xFFFFFFFF)
    return bytes([
        PAGE_COMMON_PRODUCT,
        INVALID_U8,
        supplemental,
        _u8(sw_revision_main),
        serial & 0xFF,
        (serial >> 8) & 0xFF,
        (serial >> 16) & 0xFF,
        (serial >> 24) & 0xFF,
    ])


def decode_common_81(payload: bytes) -> dict:
    """The mirror of encode_common_81.

    `serial_number` is None when the node sent the not-supplied sentinel. The
    encoder documented that sentinel from the day it was written and the
    decoder returned 4294967295 for it anyway - so a tool asking "which sensor
    is this" got an answer that looked like a serial number, sorted like one,
    and was the same for every privacy-preserving node on the air.
    """
    serial = int.from_bytes(payload[4:8], "little")
    return {
        "page": payload[0],
        "sw_revision_supplemental": (None if payload[2] == INVALID_U8
                                     else payload[2]),
        "sw_revision_main": payload[3],
        "serial_number": None if serial == INVALID_U32 else serial,
    }


def encode_common_82(fractional_voltage: int, coarse_voltage: int,
                     status: int, operating_time: int = 0,
                     time_resolution_16s: bool = False) -> bytes:
    """Page 82, Battery Status. Emitted by aerosense's declared TX API.

        [0]    page 0x52
        [1]    reserved, 0xFF
        [2]    battery identifier, 0x00 when there is only one battery
        [3..5] cumulative operating time (LE, 24-bit)
        [6]    fractional battery voltage, 1/256 V
        [7]    coarse voltage (low nibble) and status (bits 4..6), bit 7 is
               the time resolution: 0 = 16 s units, 1 = 2 s units
    """
    operating_time &= 0xFFFFFF
    descriptor = (_u8(coarse_voltage) & 0x0F) | ((status & 0x07) << 4)
    if not time_resolution_16s:
        descriptor |= 0x80
    return bytes([
        PAGE_COMMON_BATTERY,
        INVALID_U8,
        0x00,
        operating_time & 0xFF,
        (operating_time >> 8) & 0xFF,
        (operating_time >> 16) & 0xFF,
        _u8(fractional_voltage),
        descriptor,
    ])


def decode_common_82(payload: bytes) -> dict:
    return {
        "page": payload[0],
        "battery_id": payload[2],
        "operating_time": int.from_bytes(payload[3:6], "little"),
        "fractional_voltage": payload[6],
        "coarse_voltage": payload[7] & 0x0F,
        "status": (payload[7] >> 4) & 0x07,
        "time_resolution_16s": not (payload[7] & 0x80),
        "voltage": (payload[7] & 0x0F) + payload[6] / 256.0,
    }


# ---------------------------------------------------------------------------
# The RadiANT telemetry envelope, device type 0x60
#
# The mirror of src/profiles/. docs/radiant-telemetry.md section 10 says why
# the split exists at all: the Python encoders and decoders check each other
# here, the same vectors feed the C decoders, and native_sim does not build on
# Windows - so this is the only layer a developer can iterate against locally.
#
# Everything below is the envelope, not a sensor. A node publishes a schema and
# a receiver decodes typed values against it having never heard of the node
# before. Read section 6 of that document alongside this; the layout tables
# there are normative and the code here is meant to be checkable against them
# line by line.
# ---------------------------------------------------------------------------

RADIANT_TLM_DEVICE_TYPE = 0x60
# Fixed at 5, because a searching receiver matches on it. Not a node choice.
RADIANT_TLM_TRANS_TYPE = 0x05
RADIANT_TLM_VERSION = 1
RADIANT_TLM_RF_INDEX_DEFAULT = 57
RADIANT_TLM_RF_INDEX_MAX = 124
# The period this project's simulated 0x60 nodes choose. NOT a property of the
# device type: 0x60's period is per-node and is announced in descriptor frame
# 0, which is why docs/profile-registry.md records "per-node" for it and the
# registry checker cross-checks no number here.
RADIANT_TLM_PERIOD_DEFAULT = 8182

RADIANT_TLM_MAX_FIELDS = 14
RADIANT_TLM_MAX_FRAMES = 16      # two header frames plus up to 14 field frames
RADIANT_TLM_FRAME_LEN = 8

PAGE_TLM_DESCRIPTOR = 0x00
PAGE_TLM_DATA_FIRST = 0x01
PAGE_TLM_DATA_LAST = 0x0F
PAGE_TLM_COMMAND = 0x10
PAGE_TLM_COMMAND_ACK = 0x11

# 119 / 120, cycle length 121 - the same numbers as COMMON_PAGE_INTERVAL above
# and for the same reason. Named separately because the envelope states the
# cycle length explicitly and a reader should not have to add one.
RADIANT_TLM_CYCLE = 121
RADIANT_TLM_SLOT_PAGE_80 = 119
RADIANT_TLM_SLOT_PAGE_81 = 120

# Frame 0 flags, byte [7].
#
# THE FORWARD-COMPATIBILITY RULE: bits 7..4 are TRANSFORM flags and change how
# the bytes must be interpreted, so a receiver that sees one it does not
# implement must not decode the node's data pages - fail closed. Bits 3..0 are
# INFORMATIONAL and describe the node rather than the layout, so an unknown one
# is ignored - fail open.
TLM_FLAG_X_CONF = 0x80
TLM_FLAG_X_AUTH = 0x40
TLM_FLAG_RSVD_5 = 0x20
TLM_FLAG_RSVD_4 = 0x10
# Bit 3 was X_PRIV. ADR 0006 rejected CONTINUOUS per-128-second rotation, whose
# cost cliff is mid-session rotation; per-boot re-roll (identity Tier 2) is an
# approved opt-in and needs no bit here, because a node that re-rolls at
# power-up is indistinguishable on air from one provisioned with that number.
# The bit stays reserved rather than reused: announcing a privacy posture in
# the clear is itself a leak.
TLM_FLAG_RSVD_3 = 0x08
TLM_FLAG_SPARSE = 0x04
TLM_FLAG_OFF_RF57 = 0x02
TLM_FLAG_LR_PHY = 0x01

TLM_FLAG_TRANSFORM_MASK = 0xF0
TLM_FLAG_INFO_MASK = 0x0F

# MAC window W. Encoding 0 means W=1, an inline 16-bit tag, which is reserved
# for the reliable-command page and refused on a data page. The set is not
# arbitrary: the spread MAC self-synchronises across loss only because W
# divides both 256 and 65536.
TLM_W_CODE_RESERVED, TLM_W_CODE_2, TLM_W_CODE_4, TLM_W_CODE_8 = 0, 1, 2, 3
# Sparse repeat count k. 3x is the documented default.
TLM_K_CODE_1, TLM_K_CODE_2, TLM_K_CODE_3, TLM_K_CODE_5 = 0, 1, 2, 3
TLM_REPEATS = {TLM_K_CODE_1: 1, TLM_K_CODE_2: 2, TLM_K_CODE_3: 3,
               TLM_K_CODE_5: 5}

# Width codes, section 6. The gaps are not accidents - 1, 2, 4 and 6 bits exist
# for the boolean and small-enum types of class 0x00-0x0F - and 13..15 are
# reserved, so a code outside this table is rejected rather than guessed at.
TLM_WIDTHS = (1, 2, 4, 6, 8, 10, 12, 16, 20, 24, 32, 40, 48)

# The field-type vocabulary, section 7: code -> (name, canonical unit).
#
# Kept here rather than in a bridge because the whole point of fixing the
# quantity namespace is that a host has a TABLE LOOKUP rather than a per-node
# adapter. Adding a field to a node adds zero lines anywhere else. Where a
# quantity has no Matter cluster the entry is still first class - Matter has no
# heart-rate quantity, and heart rate is this envelope's headline example.
TLM_TYPES = {
    0x01: ("boolean state", "0/1"),
    0x02: ("occupancy", "0/1"),
    0x03: ("contact", "0/1"),
    0x04: ("on/off actuator state", "0/1"),
    0x05: ("lock state", "enum"),
    0x10: ("temperature", "K"),
    0x11: ("relative humidity", "%"),
    0x12: ("pressure", "Pa"),
    0x13: ("illuminance", "lx"),
    0x14: ("length", "m"),
    0x15: ("mass", "kg"),
    0x16: ("speed", "m/s"),
    0x17: ("acceleration", "m/s^2"),
    0x18: ("angle", "rad"),
    0x19: ("angular rate", "rad/s"),
    0x1A: ("force", "N"),
    0x1B: ("torque", "N.m"),
    0x1C: ("active power", "W"),
    0x1D: ("voltage", "V"),
    0x1E: ("current", "A"),
    0x1F: ("frequency", "Hz"),
    0x20: ("volumetric flow", "m^3/s"),
    0x21: ("gas concentration", "ppm"),
    0x22: ("mass concentration", "ug/m^3"),
    0x23: ("sound pressure level", "dB SPL"),
    0x24: ("percentage", "%"),
    0x25: ("battery state of charge", "%"),
    0x26: ("heart rate", "bpm"),          # non-SI, one of the two exceptions
    0x30: ("energy", "J"),
    0x31: ("charge", "C"),
    0x32: ("volume", "m^3"),
    0x33: ("mass, cumulative", "kg"),
    0x34: ("distance", "m"),
    0x35: ("revolutions", "count"),
    0x36: ("event count", "count"),
    0x37: ("duration", "s"),
    0x40: ("generic enum", "enum"),
    0x41: ("HVAC system mode", "enum"),
    0x42: ("fan mode", "enum"),
    0x50: ("opaque", "bytes"),
}

TLM_CLASS_ACC_FIRST, TLM_CLASS_ACC_LAST = 0x30, 0x3F

# Section 7's "Integral partner" column, transcribed. Section 5's rule is the
# reason it matters: "anything integrable is published as an accumulating
# field", and the accumulator is authoritative.
TLM_INTEGRAL_PARTNER = {
    0x15: 0x33,   # mass          -> mass, cumulative
    0x16: 0x34,   # speed         -> distance
    0x19: 0x35,   # angular rate  -> revolutions
    0x1B: 0x30,   # torque        -> energy
    0x1C: 0x30,   # active power  -> energy
    0x1E: 0x31,   # current       -> charge
    0x1F: 0x36,   # frequency     -> event count
    0x20: 0x32,   # volumetric flow -> volume
}

# The subset of the above where the accumulator is LITERALLY the time integral
# of the instantaneous field in canonical SI units, so a receiver can check one
# against the other with no constant of its own.
#
# The exclusions are the point of having two tables. Angular rate is rad/s and
# revolutions is a count, so that pair needs a 2*pi; torque needs an angular
# rate it does not have; mass to cumulative mass is not a time integral at all.
# A verifier that used the full table would be inventing physics, which is
# exactly the kind of confident wrongness this envelope is built to avoid.
TLM_TIME_INTEGRAL_PARTNER = {
    0x16: 0x34,   # m/s     -> m
    0x1C: 0x30,   # W       -> J
    0x1E: 0x31,   # A       -> C
    0x1F: 0x36,   # Hz      -> count
    0x20: 0x32,   # m^3/s   -> m^3
}


def tlm_type_is_accumulating(type_code: int) -> bool:
    """Class 0x30-0x3F.

    Everything in that class MUST carry the accumulate bit - that is what the
    class is for, and a descriptor that clears it on one of these types is
    malformed. Cheaper to reject at the encoder than to have every receiver
    discover the same malformed node separately.
    """
    return TLM_CLASS_ACC_FIRST <= type_code <= TLM_CLASS_ACC_LAST


def tlm_width_for_code(code: int) -> int:
    if not 0 <= code < len(TLM_WIDTHS):
        raise ValueError(f"width code {code} is reserved")
    return TLM_WIDTHS[code]


def tlm_code_for_width(bits: int) -> int:
    try:
        return TLM_WIDTHS.index(bits)
    except ValueError:
        raise ValueError(f"no width code names {bits} bits") from None


# ---------------------------------------------------------------------------
# The MSB-first bit packer
#
# THE TWIN OF src/profiles/profile_bits.c, AND THE VECTORS IN
# tools/test_ant_pages.py ARE SHARED WITH radiant_core/tests/src/test_profiles.c.
#
# Section 6: bit packing here is MSB-first and the value is stored MSB-first
# within its width. That is the opposite of the little-endian byte order every
# other page in this file uses, and the difference is deliberate - those fields
# are byte-aligned, where little-endian is unambiguous; these are not, where
# little-endian bit order is "a reliable source of two implementations that
# each work alone."
#
#   bit offset 0 is the MOST SIGNIFICANT BIT OF THE FIRST BYTE of the area,
#   which is payload byte [2] of a data page.
#
# There is no alignment requirement: a 6-bit field may start at offset 7. The
# descriptor carries an explicit bit offset per field precisely so a node may
# pack tightly and a receiver never has to guess.
# ---------------------------------------------------------------------------


def _bits_check(area_bits: int, bit_off: int, width: int) -> None:
    if width <= 0 or width > 48:
        raise ValueError(f"width {width} is not 1..48 bits")
    if bit_off + width > area_bits:
        raise ValueError(
            f"a {width}-bit field at offset {bit_off} runs past the end of a "
            f"{area_bits}-bit field area")


def pack_bits(area: bytearray, area_bits: int, bit_off: int, width: int,
              value: int) -> bytearray:
    """Write `width` bits of `value` at `bit_off`, MSB first.

    Bits above `width` are dropped rather than rejected. Section 5: the
    transmitted value of an accumulating field is a running total in the
    field's declared width and it is MEANT to wrap, so refusing here would push
    the masking - and therefore the wrap semantics - out to every caller.
    """
    _bits_check(area_bits, bit_off, width)
    value &= (1 << width) - 1
    for i in range(width):
        bit = bit_off + i
        mask = 0x80 >> (bit & 7)
        if (value >> (width - 1 - i)) & 1:
            area[bit >> 3] |= mask
        else:
            area[bit >> 3] &= 0xFF ^ mask
    return area


def unpack_bits(area: bytes, area_bits: int, bit_off: int, width: int) -> int:
    """The exact inverse. Zero-extended; see tlm_sign_extend()."""
    _bits_check(area_bits, bit_off, width)
    value = 0
    for i in range(width):
        bit = bit_off + i
        value <<= 1
        if area[bit >> 3] & (0x80 >> (bit & 7)):
            value |= 1
    return value


def tlm_sign_extend(raw: int, width: int) -> int:
    """Sign-extend a raw value of `width` bits.

    Separate from the unpack because `signed` is a property of the field, not
    of the packing - and because a receiver that differences two ACCUMULATING
    readings must do so in the field's own width. Sign-extending before
    subtracting is exactly the "one absurd sample per wrap" bug section 5
    warns about.
    """
    if width <= 0 or width >= 64:
        return raw
    sign = 1 << (width - 1)
    return raw - (sign << 1) if raw & sign else raw


# ---------------------------------------------------------------------------
# The descriptor, page 0x00
# ---------------------------------------------------------------------------


@dataclasses.dataclass
class TlmField:
    """One field descriptor - one of frames 2..N.

    `page` and `bit_offset` together are what make this a SCHEMA rather than a
    format: turning X_AUTH on shrinks the field area from 48 bits to 40 and
    moves nothing, because the node republishes a descriptor with a new schema
    id and every receiver picks it up within one interleave cycle.
    """

    id: int
    type: int
    page: int
    bit_offset: int
    width_code: int
    accumulate: bool = False
    signed: bool = False
    exponent: int = 0

    @property
    def width(self) -> int:
        return tlm_width_for_code(self.width_code)


@dataclasses.dataclass
class TlmDescriptor:
    schema_id: int
    period: int                  # counts of 1/32768 s; 0 = asynchronous
    fields: list = dataclasses.field(default_factory=list)
    version: int = RADIANT_TLM_VERSION
    rf_index: int = RADIANT_TLM_RF_INDEX_DEFAULT
    flags: int = 0
    heartbeat_s: int = 0
    w_code: int = 0
    k_code: int = 0
    epoch: int = 0

    @property
    def field_area_bits(self) -> int:
        # With X_AUTH on, byte [7] is the spread-MAC tag byte and the field
        # area is bits 0..39.
        return 40 if self.flags & TLM_FLAG_X_AUTH else 48

    @property
    def may_decode_data(self) -> bool:
        """The forward-compatibility rule, in one place.

        False when ANY transform bit is set, because v1 implements none of
        them. A receiver that gets False has still learned the node's identity,
        period, RF index and schema - fail closed means "do not decode", not
        "do not listen".
        """
        return not self.flags & TLM_FLAG_TRANSFORM_MASK

    @property
    def frame_count(self) -> int:
        # There is no descriptor authentication frame in v1 because there is no
        # transform in v1; when there is one it takes the last slot.
        return 2 + len(self.fields)

    @property
    def data_pages(self) -> list:
        """The node's rotation, ascending - derived from the schema rather than
        configured alongside it, because two places to say which pages exist is
        one place and one drift."""
        return sorted({f.page for f in self.fields})

    def field_by_id(self, field_id: int):
        for f in self.fields:
            if f.id == field_id:
                return f
        return None


def _tlm_validate(d: TlmDescriptor) -> None:
    """Every check here exists because the failure it prevents is silent on air.

    See src/profiles/profile_telemetry.c, which carries the same list; the two
    must refuse the same descriptors or the C node and the Python simulator are
    not the same node.
    """
    if d.version != RADIANT_TLM_VERSION:
        raise ValueError(f"envelope version {d.version} is not implemented")
    if len(d.fields) > RADIANT_TLM_MAX_FIELDS:
        raise ValueError("15 field descriptors is reserved for an extended "
                         "descriptor, not a longer set")
    if not 0 <= d.rf_index <= RADIANT_TLM_RF_INDEX_MAX:
        raise ValueError(f"RF index {d.rf_index} is outside 0..124")
    if d.flags & TLM_FLAG_RSVD_3:
        raise ValueError("descriptor INFO bit 3 is reserved-must-be-zero "
                         "(it was X_PRIV; see ADR 0006)")

    off_57 = bool(d.flags & TLM_FLAG_OFF_RF57)
    if off_57 != (d.rf_index != RADIANT_TLM_RF_INDEX_DEFAULT):
        raise ValueError("the off-57 flag and byte [6] disagree about the "
                         "node's own frequency")

    sparse = bool(d.flags & TLM_FLAG_SPARSE)
    if sparse:
        # Zero is invalid for a sparse node: without a heartbeat a receiver
        # cannot tell a quiet node from a dead one, and "no data" is the one
        # reading a telemetry system must never produce silently.
        if not 1 <= d.heartbeat_s <= 255:
            raise ValueError("a sparse node needs a heartbeat interval")
        if d.k_code not in TLM_REPEATS:
            raise ValueError(f"sparse repeat code {d.k_code} is not 0..3")
    else:
        if d.heartbeat_s:
            raise ValueError("heartbeat_s is meaningless off sparse mode")
        if d.k_code:
            raise ValueError("the repeat count is meaningless off sparse mode")
        if d.period == 0:
            raise ValueError("asynchronous (period 0x0000) has no slot "
                             "discipline and needs the sparse flag")
    if not 0 <= d.period <= 0xFFFF:
        raise ValueError("period is 16 bits")

    # Section 11: every reservation is populated with zeros in v1.
    if d.flags & TLM_FLAG_X_AUTH:
        if d.w_code not in (TLM_W_CODE_2, TLM_W_CODE_4, TLM_W_CODE_8):
            raise ValueError("W=1 is the command page's inline tag and is "
                             "refused on a data page")
    elif d.w_code:
        raise ValueError("the MAC window is reserved while no transform is on")

    seen_ids = set()
    spans = {}
    for f in d.fields:
        if not PAGE_TLM_DATA_FIRST <= f.page <= PAGE_TLM_DATA_LAST:
            raise ValueError(f"field {f.id} names page 0x{f.page:02X}, which "
                             "is not a data page")
        width = tlm_width_for_code(f.width_code)
        if f.bit_offset + width > d.field_area_bits:
            raise ValueError(f"field {f.id} runs past the end of the "
                             f"{d.field_area_bits}-bit field area")
        if tlm_type_is_accumulating(f.type) and not f.accumulate:
            raise ValueError(f"field {f.id} is an accumulating-class type "
                             "with the accumulate bit clear")
        if f.id in seen_ids:
            raise ValueError(f"two fields share id {f.id}")
        seen_ids.add(f.id)
        for other_off, other_width, other_id in spans.get(f.page, ()):
            if (f.bit_offset < other_off + other_width
                    and other_off < f.bit_offset + width):
                raise ValueError(f"fields {other_id} and {f.id} overlap in "
                                 f"page 0x{f.page:02X}")
        spans.setdefault(f.page, []).append((f.bit_offset, width, f.id))
        if not -128 <= f.exponent <= 127:
            raise ValueError("the scale exponent is an int8")


def encode_tlm_descriptor(d: TlmDescriptor) -> list:
    """The whole descriptor set, as a list of 8-byte frames.

        [0] 0x00                       page number
        [1] (index << 4) | (count - 1) frame index, and how many in the set
        [2..7]                         6 bytes of frame body

    Byte [1] is a FRAME INDEX and not a counter - one of the two documented
    exceptions to the counter invariant of section 4.

    A transform bit is REFUSED rather than emitted: section 6 makes the
    descriptor authentication frame mandatory alongside one, this build cannot
    produce that frame, and a transform announced without it gets an attacker
    wrong readings out of correctly authenticated packets. Checked before the
    field validation, because X_AUTH shrinks the field area and would otherwise
    produce a true statement about the fields that is a misleading answer to
    what was asked.
    """
    if d.flags & TLM_FLAG_TRANSFORM_MASK:
        raise ValueError("v1 announces no transform: there is no descriptor "
                         "authentication frame to go with it")
    _tlm_validate(d)

    count = d.frame_count
    if count > RADIANT_TLM_MAX_FRAMES:
        raise ValueError("a descriptor set is at most 16 frames")

    def head(index: int) -> bytes:
        return bytes([PAGE_TLM_DESCRIPTOR, (index << 4) | (count - 1)])

    frames = [
        # Frame 0 - identity and timing.
        head(0) + bytes([
            (d.version << 4) | len(d.fields),
            d.schema_id & 0xFF,
            d.period & 0xFF,
            (d.period >> 8) & 0xFF,
            d.rf_index,
            d.flags,
        ]),
        # Frame 1 - mode and the reserved security space. Four bytes of epoch
        # from the first shipped node, because a receiver with no real-time
        # clock has to get it from a page the node already sends, and adding it
        # later is a format break for every deployed node. Bits 3..0 of byte
        # [3] stay zero: the informational-flag extension space a later phase's
        # schedule block claims.
        head(1) + bytes([
            d.heartbeat_s,
            (d.w_code << 6) | (d.k_code << 4),
            d.epoch & 0xFF,
            (d.epoch >> 8) & 0xFF,
            (d.epoch >> 16) & 0xFF,
            (d.epoch >> 24) & 0xFF,
        ]),
    ]

    for index, f in enumerate(d.fields):
        frames.append(head(2 + index) + bytes([
            f.id,
            f.type,
            # bits 1..0 reserved, must be 0 - and this expression never sets
            # them.
            (0x80 if f.accumulate else 0) | (0x40 if f.signed else 0)
            | (f.width_code << 2),
            f.exponent & 0xFF,
            f.page,
            f.bit_offset,
        ]))

    return frames


def decode_tlm_descriptor(frames) -> TlmDescriptor:
    """Turn a complete, in-order frame set back into a schema.

    This is the function the Phase E gate turns on: given nothing but eight
    bytes at a time off the air, produce the schema, with no registry lookup
    and no out-of-band knowledge.
    """
    frames = [bytes(f[:RADIANT_TLM_FRAME_LEN]) for f in frames]
    n = len(frames)
    if not 2 <= n <= RADIANT_TLM_MAX_FRAMES:
        raise ValueError(f"a descriptor set of {n} frames cannot exist")

    for index, f in enumerate(frames):
        if len(f) != RADIANT_TLM_FRAME_LEN:
            raise ValueError("a descriptor frame is 8 bytes")
        if f[0] != PAGE_TLM_DESCRIPTOR:
            raise ValueError(f"frame {index} is page 0x{f[0]:02X}, not 0x00")
        if f[1] >> 4 != index:
            raise ValueError(f"frame {index} says it is frame {f[1] >> 4}")
        if (f[1] & 0x0F) + 1 != n:
            raise ValueError("the frames disagree about the size of the set")

    version = frames[0][2] >> 4
    n_fields = frames[0][2] & 0x0F
    # A receiver that does not implement the version MUST REJECT THE NODE
    # rather than guess: the version governs the frame layout itself, so every
    # byte read below would be read under the wrong rules.
    if version != RADIANT_TLM_VERSION:
        raise ValueError(f"envelope version {version} is not implemented")
    if n_fields == 0x0F:
        raise ValueError("field count 15 is reserved for an extended "
                         "descriptor - a different frame set, not 15 fields")
    if 2 + n_fields != n:
        raise ValueError("the field count and the frame count disagree")

    if frames[1][3] & 0x0F:
        # Reserved, must be 0 in v1. Unlike an unknown informational FLAG,
        # these bits have no defined meaning at all, so a receiver that ignored
        # them would be inventing one.
        raise ValueError("frame 1 byte [3] bits 3..0 are reserved-must-be-zero")

    fields = []
    for index in range(n_fields):
        f = frames[2 + index]
        if f[4] & 0x03:
            raise ValueError("the encoding byte's bits 1..0 are reserved")
        fields.append(TlmField(
            id=f[2],
            type=f[3],
            accumulate=bool(f[4] & 0x80),
            signed=bool(f[4] & 0x40),
            width_code=(f[4] >> 2) & 0x0F,
            exponent=f[5] - 256 if f[5] > 127 else f[5],
            page=f[6],
            bit_offset=f[7],
        ))

    d = TlmDescriptor(
        schema_id=frames[0][3],
        period=_rd_le16(frames[0], 4),
        fields=fields,
        version=version,
        rf_index=frames[0][6],
        flags=frames[0][7],
        heartbeat_s=frames[1][2],
        w_code=(frames[1][3] >> 6) & 0x03,
        k_code=(frames[1][3] >> 4) & 0x03,
        epoch=int.from_bytes(frames[1][4:8], "little"),
    )
    _tlm_validate(d)
    return d


class TlmDescriptorRx:
    """Assemble a descriptor set out of frames arriving one per slot.

    A receiver does not get a frame set; it gets frames, starting wherever it
    happened to join, in whatever order the air delivered them, with holes.
    Byte [1] carries (index << 4) | (count - 1), which is enough to assemble
    the set with no ordering assumption and no timer.
    """

    def __init__(self):
        self.frames = {}
        self.count = 0
        self.schema_id = None
        self.frames_fed = 0
        self.resets = 0

    def feed(self, body: bytes) -> bool:
        """Feed one 8-byte payload; anything that is not page 0x00 is ignored.

        Returns True once the set is complete. Restarts when the frame count
        changes or when frame 0 arrives carrying a different schema id - which
        is exactly what the schema id is for: a receiver notices a change after
        reading a SINGLE frame instead of the whole set, and anything already
        assembled describes the previous schema.
        """
        body = bytes(body[:RADIANT_TLM_FRAME_LEN])
        if body[0] != PAGE_TLM_DESCRIPTOR:
            return self.complete

        index = body[1] >> 4
        count = (body[1] & 0x0F) + 1
        if count < 2 or index >= count:
            raise ValueError("a descriptor frame that belongs to no set")

        self.frames_fed += 1
        if self.count and self.count != count:
            self._restart()
        self.count = count

        if index == 0:
            schema_id = body[3]
            if self.schema_id is not None and schema_id != self.schema_id:
                self._restart()
            self.schema_id = schema_id

        self.frames[index] = body
        return self.complete

    def _restart(self) -> None:
        self.frames = {}
        self.schema_id = None
        self.resets += 1

    @property
    def complete(self) -> bool:
        return bool(self.count) and len(self.frames) == self.count

    def take(self) -> TlmDescriptor:
        if not self.complete:
            raise ValueError("the descriptor set is incomplete")
        return decode_tlm_descriptor([self.frames[i]
                                      for i in range(self.count)])


# ---------------------------------------------------------------------------
# Data pages 0x01..0x0F
# ---------------------------------------------------------------------------


def encode_tlm_data(d: TlmDescriptor, page: int, counter: int,
                    values: dict) -> bytes:
    """Pack every field the schema places on `page`.

        [0]    page number, 0x01..0x0F
        [1]    event counter, +1 per TRANSMITTED data page, wraps at 256
        [2..7] field area, 48 bits, bit offset 0 = MSB of byte [2]

    `values` is keyed by field id; entries for other pages are ignored and a
    missing one packs as zero. The event counter counts TRANSMISSIONS, not
    application writes: a master retransmits its current body whether or not
    the application supplied a new one, and a counter that advanced per write
    would repeat across retransmissions - which under a secured channel is
    keystream reuse.

    Bytes the schema does not claim come out zero, because a later schema may
    claim them and a receiver mid-change would read a stale value as a number.
    """
    if not PAGE_TLM_DATA_FIRST <= page <= PAGE_TLM_DATA_LAST:
        raise ValueError(f"0x{page:02X} is not a data page")
    area = bytearray(6)
    for f in d.fields:
        if f.page != page:
            continue
        pack_bits(area, d.field_area_bits, f.bit_offset, f.width,
                  values.get(f.id, 0))
    return bytes([page, counter & 0xFF]) + bytes(area)


def decode_tlm_data(d: TlmDescriptor, body: bytes) -> dict:
    """The inverse. Returns {"page", "counter", "values": {id: value}}.

    Signed fields are sign-extended in their own width; unsigned ones are not.
    Raises if a transform bit forbids decoding - loudly rather than silently,
    because a receiver that returned zeros would be reporting
    plaintext-looking numbers off a ciphertext page.
    """
    body = bytes(body[:RADIANT_TLM_FRAME_LEN])
    if not d.may_decode_data:
        raise ValueError("a transform bit this receiver does not implement is "
                         "set; fail closed")
    page = body[0]
    if not PAGE_TLM_DATA_FIRST <= page <= PAGE_TLM_DATA_LAST:
        raise ValueError(f"0x{page:02X} is not a data page")

    values = {}
    for f in d.fields:
        if f.page != page:
            continue
        raw = unpack_bits(body[2:], d.field_area_bits, f.bit_offset, f.width)
        values[f.id] = tlm_sign_extend(raw, f.width) if f.signed else raw
    return {"page": page, "counter": body[1], "values": values}


def tlm_value_si(field: TlmField, raw: int) -> float:
    """value_SI = raw * 10^exp, in the type's canonical unit (section 7).

    The whole of the Matter mapping rule's first line. What follows it -
    matter_value = f_type(value_SI) - is one fixed function per type and lives
    host-side in a bridge that does not exist yet.
    """
    return raw * (10.0 ** field.exponent)


# ---------------------------------------------------------------------------
# The reliable-command pages, section 9 - LAYOUT ONLY
#
# The idempotency rule, the accept window, the tag derivation and the
# exponential backoff on FAILED verifications all need a key and a state
# machine, and both belong to the phase that turns the command path on. What is
# here is the byte order, so that phase does not also get to choose it.
# ---------------------------------------------------------------------------

TLM_TARGET_NODE = 0xFF

TLM_RESULT_OK = 0x00
TLM_RESULT_ALREADY = 0x01
TLM_RESULT_BAD_SEQ = 0x02
TLM_RESULT_BAD_TAG = 0x03
TLM_RESULT_UNKNOWN_CMD = 0x04
TLM_RESULT_BAD_ARG = 0x05
TLM_RESULT_BUSY = 0x06

TLM_CMD_NOP = 0x00
TLM_CMD_SET_BOOL = 0x01
TLM_CMD_SET_LEVEL = 0x02
TLM_CMD_SET_SETPOINT = 0x03
TLM_CMD_SET_MODE = 0x04
TLM_CMD_IDENTIFY = 0x05
TLM_CMD_SEND_DESCRIPTOR = 0x06
TLM_CMD_REPORT_SCHEMA = 0x07


def encode_tlm_command(seq: int, cmd: int, target: int, arg: int,
                       tag: int) -> bytes:
    """Page 0x10. Byte [1] is the sequence number - the counter invariant - and
    the trailing bytes are tag space, which is where a RadiANT page's
    authentication tag must always live."""
    return bytes([PAGE_TLM_COMMAND, _u8(seq), _u8(cmd), _u8(target)]) \
        + _le16(arg) + _le16(tag)


def decode_tlm_command(body: bytes) -> dict:
    return {
        "page": body[0],
        "seq": body[1],
        "cmd": body[2],
        "target": body[3],
        "arg": _rd_le16(body, 4),
        "tag": _rd_le16(body, 6),
    }


def encode_tlm_command_ack(seq: int, result: int, cmd: int, value: int,
                           tag: int) -> bytes:
    """Page 0x11, broadcast back k times."""
    return bytes([PAGE_TLM_COMMAND_ACK, _u8(seq), _u8(result), _u8(cmd)]) \
        + _le16(value) + _le16(tag)


def decode_tlm_command_ack(body: bytes) -> dict:
    return {
        "page": body[0],
        "seq": body[1],
        "result": body[2],
        "cmd": body[3],
        "value": _rd_le16(body, 4),
        "tag": _rd_le16(body, 6),
    }


# ---------------------------------------------------------------------------
# The sync-handoff page, section 12 - the mirror of src/profiles/profile_handoff.c
#
# The one page in the envelope document that a NODE does not send. A receiver
# already tracking a node broadcasts it so that a second receiver can configure
# a channel and go straight to tracking, skipping the wildcard sweep entirely -
# and so that the same receiver, after a reboot, re-acquires in one channel
# period instead of one sweep.
#
# Two frames under one page number, and the arithmetic that forced it is in
# section 12: page byte and counter byte are fixed by the section 4 positional
# invariants, byte [7] is tag space by the second of them, and identity plus
# period alone is 47 of the 48 bits a single frame would have left - one bit
# spare and no room for a phase field, which is the entire point of the page.
#
# THE PAGE CARRIES NO EPOCH. It is the obvious field to add and it must not be
# added: for a hostless node the epoch is the boot counter, and a slowly
# incrementing number broadcast in the clear fingerprints the device across
# sessions and defeats per-boot device-number rotation on its own. Every one of
# the 64 bits below is assigned, so there is no space it could quietly occupy.
# ---------------------------------------------------------------------------

PAGE_TLM_SYNC_HANDOFF = 0x12

TLM_HANDOFF_FRAMES = 2
TLM_HANDOFF_AREA_BITS = 32       # bytes [3..6]; [7] is tag space

# Slot phase is a FRACTION of the node's own period, in period/8192 units,
# rather than a count of 1/32768 s. That is self-scaling: round-trip error is at
# most period/16384 counts - 15 us at the 4 Hz ANT+ period, 122 us at the
# longest period the field can express - so it is inside the guard window at
# both ends of the range. A fixed-resolution count would have had to choose
# between covering a 2 s period and being useful at 4 Hz.
TLM_HANDOFF_PHASE_DEN = 8192
TLM_HANDOFF_PHASE_MAX = TLM_HANDOFF_PHASE_DEN - 1

# Clock accuracy: the ceiling on the node's clock error in ppm. BLE's ladder,
# adopted rather than invented - it already spans exactly the range that
# matters, from an RC-oscillator coin-cell node at the top to a crystal at the
# bottom, and a second ladder meaning the same thing differently would be a pure
# interoperability cost.
#
# Code 0 is the worst case AND the value to send when the accuracy is not known,
# because on no evidence the worst case is the only safe answer.
TLM_CLK_UNKNOWN = 0
TLM_CLK_250PPM = 1
TLM_CLK_150PPM = 2
TLM_CLK_100PPM = 3
TLM_CLK_75PPM = 4
TLM_CLK_50PPM = 5
TLM_CLK_30PPM = 6
TLM_CLK_20PPM = 7

TLM_CLK_PPM_CEILING = {
    TLM_CLK_UNKNOWN: 500,
    TLM_CLK_250PPM: 250,
    TLM_CLK_150PPM: 150,
    TLM_CLK_100PPM: 100,
    TLM_CLK_75PPM: 75,
    TLM_CLK_50PPM: 50,
    TLM_CLK_30PPM: 30,
    TLM_CLK_20PPM: 20,
}


def tlm_clock_ppm(code: int) -> int:
    """The ppm ceiling a clock-accuracy code promises. Never narrower than the
    code says: a receiver sizing a window on this must round outward."""
    if code not in TLM_CLK_PPM_CEILING:
        raise ValueError(f"clock accuracy code {code} is not 0..7")
    return TLM_CLK_PPM_CEILING[code]


def tlm_handoff_phase_encode(phase_counts: int, period: int) -> int:
    """Counts from the carrying frame's t_sync to the node's next slot, as a
    period/8192 fraction.

    `phase_counts` is reduced modulo the period here rather than refused: it is
    a phase, and a caller that measured across a slot boundary is not wrong, it
    is one period out.
    """
    if not 1 <= period <= 65535:
        raise ValueError("a handoff needs a real period; 0 is asynchronous and "
                         "an asynchronous node has no slot to hand over")
    if phase_counts < 0:
        raise ValueError("a slot phase is measured forward from the carrying "
                         "frame and is never negative")
    phase_counts %= period
    code = (phase_counts * TLM_HANDOFF_PHASE_DEN + period // 2) // period
    return min(code, TLM_HANDOFF_PHASE_MAX)


def tlm_handoff_phase_counts(phase_code: int, period: int) -> int:
    """The inverse: 32768 Hz counts from the carrying frame's t_sync."""
    if not 0 <= phase_code <= TLM_HANDOFF_PHASE_MAX:
        raise ValueError(f"phase code {phase_code} is not 0..8191")
    return ((phase_code * period) + TLM_HANDOFF_PHASE_DEN // 2) \
        // TLM_HANDOFF_PHASE_DEN


@dataclasses.dataclass
class TlmHandoff:
    """Everything a second receiver needs to open a tracked channel, and
    nothing else. The absent field is the epoch; see section 12."""

    device_number: int
    device_type: int          # 1..127; the pairing bit is not handed over
    trans_type: int
    period: int               # counts of 1/32768 s
    phase: int = 0            # 0..8191, units of period/8192, from frame 1
    clock_accuracy: int = TLM_CLK_UNKNOWN
    counter: int = 0          # byte [1], the counter invariant


def _tlm_handoff_validate(h: TlmHandoff) -> None:
    if not 0 <= h.device_number <= 0xFFFF:
        raise ValueError("device number is a u16")
    if not 1 <= h.device_type <= 127:
        raise ValueError("device type is 1..127; the MSB of the device type "
                         "field is the pairing bit and is not handed over")
    if not 0 <= h.trans_type <= 0xFF:
        raise ValueError("transmission type is a u8")
    if not 1 <= h.period <= 0xFFFF:
        raise ValueError("a handoff needs a real period; 0 is asynchronous and "
                         "an asynchronous node has no slot to hand over")
    if not 0 <= h.phase <= TLM_HANDOFF_PHASE_MAX:
        raise ValueError("slot phase is 0..8191 units of period/8192")
    if not 0 <= h.clock_accuracy <= 7:
        raise ValueError("clock accuracy is a 3-bit code")
    if not 0 <= h.counter <= 0xFF:
        raise ValueError("the handoff counter is a u8")


def _tlm_handoff_head(index: int, counter: int) -> bytes:
    return bytes([PAGE_TLM_SYNC_HANDOFF, _u8(counter),
                  (index << 4) | (TLM_HANDOFF_FRAMES - 1)])


def encode_tlm_handoff(h: TlmHandoff) -> list:
    """The whole handoff set, as a list of 2 eight-byte frames.

        [0] 0x12                        page number
        [1] handoff counter             the section 4 counter invariant
        [2] (index << 4) | (count - 1)  frame index; moved to [2] so that [1]
                                        can stay a counter and this page needs
                                        no third exception to that invariant
        [3..6] 32-bit field area, MSB-first, exactly as section 6 packs a
               descriptor field area
        [7] tag space; zero when no transform is in use

    Frame 0 is WHO and is timeless. Frame 1 is WHEN and is self-dating: its
    phase is measured from its own t_sync, so the two frames need not be
    adjacent and need not arrive in order.
    """
    _tlm_handoff_validate(h)

    who = bytearray(TLM_HANDOFF_AREA_BITS // 8)
    pack_bits(who, TLM_HANDOFF_AREA_BITS, 0, 16, h.device_number)
    pack_bits(who, TLM_HANDOFF_AREA_BITS, 16, 7, h.device_type)
    pack_bits(who, TLM_HANDOFF_AREA_BITS, 23, 8, h.trans_type)
    # bit 31 is reserved, must be 0, and this expression never sets it.

    when = bytearray(TLM_HANDOFF_AREA_BITS // 8)
    pack_bits(when, TLM_HANDOFF_AREA_BITS, 0, 16, h.period)
    pack_bits(when, TLM_HANDOFF_AREA_BITS, 16, 13, h.phase)
    pack_bits(when, TLM_HANDOFF_AREA_BITS, 29, 3, h.clock_accuracy)

    # The trailing zero is byte [7], tag space. Not padding: the second
    # positional invariant of section 4 says an authentication tag lives at the
    # end of a RadiANT page and nowhere else, so no field may be placed there.
    return [_tlm_handoff_head(0, h.counter) + bytes(who) + b"\x00",
            _tlm_handoff_head(1, h.counter) + bytes(when) + b"\x00"]


def tlm_handoff_frame_index(frame: bytes):
    """(index, count) from byte [2], or None if this is not a handoff frame."""
    if len(frame) < RADIANT_TLM_FRAME_LEN or frame[0] != PAGE_TLM_SYNC_HANDOFF:
        return None
    return (frame[2] >> 4) & 0x0F, (frame[2] & 0x0F) + 1


def decode_tlm_handoff(frames) -> TlmHandoff:
    """Turn a complete handoff set back into the parameters of a channel.

    Order-insensitive on purpose: frame 0 carries no time and frame 1 carries
    its own, so a receiver that hears them out of order or a rotation apart has
    lost nothing but the wait.
    """
    frames = [bytes(f[:RADIANT_TLM_FRAME_LEN]) for f in frames]
    by_index = {}
    for frame in frames:
        if len(frame) != RADIANT_TLM_FRAME_LEN:
            raise ValueError("a handoff frame is 8 bytes")
        head = tlm_handoff_frame_index(frame)
        if head is None:
            raise ValueError(f"page 0x{frame[0]:02X} is not the handoff page")
        index, count = head
        if count != TLM_HANDOFF_FRAMES:
            raise ValueError(f"a v1 handoff set is {TLM_HANDOFF_FRAMES} frames, "
                             f"not {count}")
        if index >= TLM_HANDOFF_FRAMES:
            raise ValueError(f"frame index {index} is outside a {count}-frame set")
        by_index[index] = frame

    missing = [i for i in range(TLM_HANDOFF_FRAMES) if i not in by_index]
    if missing:
        raise ValueError(f"handoff set is missing frame(s) {missing}")

    counters = {by_index[i][1] for i in range(TLM_HANDOFF_FRAMES)}
    if len(counters) != 1:
        raise ValueError("the two frames carry different counters and are "
                         "therefore two different handoffs")

    who = by_index[0][3:7]
    when = by_index[1][3:7]

    reserved = unpack_bits(who, TLM_HANDOFF_AREA_BITS, 31, 1)
    if reserved:
        raise ValueError("frame 0 bit 31 is reserved and must be 0")
    for index in range(TLM_HANDOFF_FRAMES):
        if by_index[index][7] != 0:
            raise ValueError(f"frame {index} byte [7] is tag space and no "
                             f"transform is in use, so it must be 0")

    h = TlmHandoff(
        device_number=unpack_bits(who, TLM_HANDOFF_AREA_BITS, 0, 16),
        device_type=unpack_bits(who, TLM_HANDOFF_AREA_BITS, 16, 7),
        trans_type=unpack_bits(who, TLM_HANDOFF_AREA_BITS, 23, 8),
        period=unpack_bits(when, TLM_HANDOFF_AREA_BITS, 0, 16),
        phase=unpack_bits(when, TLM_HANDOFF_AREA_BITS, 16, 13),
        clock_accuracy=unpack_bits(when, TLM_HANDOFF_AREA_BITS, 29, 3),
        counter=by_index[0][1],
    )
    _tlm_handoff_validate(h)
    return h


def tlm_handoff_next_slot(h: TlmHandoff, t_carrier_counts: int) -> int:
    """The node's next slot, on the RECIPIENT's clock.

    `t_carrier_counts` is the t_sync at which the recipient heard frame 1, in
    its own 32768 Hz counts. Nothing absolute crosses the air: the phase is
    relative to that frame and to nothing else, because two receivers share no
    time base and an absolute slot instant would be a number from somebody
    else's clock.
    """
    return t_carrier_counts + tlm_handoff_phase_counts(h.phase, h.period)


# ---------------------------------------------------------------------------
# The page scheduler - the mirror of src/profiles/profile_sched.c
#
# Which page goes in the next message slot, and nothing else. It knows no
# microsecond and no radio; a master calls next() to fill the body it is about
# to transmit.
#
# THE SLOT-INSERTION SEAM. Two plans need this engine: this one runs it for
# device type 0x60, and the ANT+ compatibility work needs the same interleave
# for 0x78 and 0x0B with its own beacon and attestation pages inserted. Two
# implementations of one cadence rule is one implementation and one drift, so
# there is one engine and the other plan is a CLIENT of it. A client gets first
# refusal on any slot the rotation would have filled with a data page - and, on
# a sparse node, on any slot that would otherwise be silent. It is never
# offered message 119, message 120 or a descriptor frame, because those three
# ARE the cadence rule.
# ---------------------------------------------------------------------------

SLOT_IDLE = "idle"
SLOT_DESCRIPTOR = "descriptor"
SLOT_DATA = "data"
SLOT_COMMON_80 = "common80"
SLOT_COMMON_81 = "common81"
SLOT_COMMON_82 = "common82"
SLOT_CLIENT = "client"


class TlmPageScheduler:
    """The 119/120/121 interleave, the descriptor burst and the sparse rule.

    `builders` is a dict of callables:
        data_page(page, counter) -> 8 bytes or None
        common_80() / common_81() / common_82() -> 8 bytes or None
    A missing or None builder means the node does not emit that page.
    Suppressing page 82 outright is not an omission: it is the privacy rule of
    section 6, and it is the mitigation that matters most for a node whose
    whole payload is a stable identity.
    """

    def __init__(self, descriptor: TlmDescriptor | None, builders: dict,
                 common_82_every: int = 0, client=None):
        self.desc = descriptor
        self.builders = builders
        self.common_82_every = common_82_every
        self.client = client

        self.frames = (encode_tlm_descriptor(descriptor)
                       if descriptor is not None else [])
        self.pages = descriptor.data_pages if descriptor is not None else []
        self.page_cursor = 0

        self.m = 0
        self.counter = 0
        self.burst = False
        self.burst_index = 0
        self.data_since_82 = 0

        self.sparse = bool(descriptor is not None
                           and descriptor.flags & TLM_FLAG_SPARSE)
        self.hb_slots = 0
        if self.sparse and descriptor.period:
            # Slot-aligned sparse: the node keeps its period configured, so the
            # transmissions it does make land on a predictable phase.
            self.hb_slots = max(
                1, (descriptor.heartbeat_s * 32768) // descriptor.period)
        self.since_hb = 0
        # A sparse node expresses "send the schema first" as a pending
        # heartbeat rather than as a burst: the heartbeat is what resets the
        # interval, so starting the burst directly would fire a second
        # heartbeat two slots later.
        self.hb_pending = self.sparse
        self.want_82 = False
        self.event_page = None
        self.event_left = 0

    # -- the seam ---------------------------------------------------------
    def set_client(self, client) -> None:
        self.client = client

    def _offer(self, m: int):
        if self.client is None:
            return None
        return self.client(m)

    # -- the node's own controls -----------------------------------------
    def request_descriptor(self) -> None:
        """Send the whole set starting at the next slot: a schema change, or
        command 0x06, or a heartbeat."""
        if self.frames:
            self.burst = True
            self.burst_index = 0

    def post_event(self, page: int) -> None:
        """A sparse node has something to say; k repeats, one per slot."""
        if not self.sparse:
            raise ValueError("only a sparse node posts events")
        self.event_page = page
        self.event_left = TLM_REPEATS[self.desc.k_code]

    def post_heartbeat(self) -> None:
        self.hb_pending = True

    # -- the engine -------------------------------------------------------
    def next(self):
        """Returns (kind, body). body is None for SLOT_IDLE."""
        m = self.m
        if self.sparse:
            self.m += 1
            return self._next_sparse(m)
        self.m = (m + 1) % RADIANT_TLM_CYCLE
        return self._next_periodic(m)

    def _take_descriptor(self):
        body = self.frames[self.burst_index]
        self.burst_index += 1
        if self.burst_index >= len(self.frames):
            self.burst = False
            self.burst_index = 0
        return SLOT_DESCRIPTOR, body

    def _take_data(self, page: int):
        builder = self.builders.get("data_page")
        if builder is None:
            return SLOT_IDLE, None
        body = builder(page, self.counter)
        if body is None:
            return SLOT_IDLE, None
        # Advanced only when the builder actually produced a body: the counter
        # counts transmissions, and a slot the application declined is not one.
        self.counter = (self.counter + 1) % U8_WRAP
        return SLOT_DATA, body

    def _take_rotation(self):
        if not self.pages:
            # A node with no fields at all - an asset tag is the envelope with
            # everything turned off - has nothing to rotate.
            return SLOT_IDLE, None
        page = self.pages[self.page_cursor]
        self.page_cursor = (self.page_cursor + 1) % len(self.pages)
        return self._take_data(page)

    def _page_82_due(self) -> bool:
        if not self.common_82_every or not self.builders.get("common_82"):
            return False
        self.data_since_82 += 1
        if self.data_since_82 < self.common_82_every:
            return False
        self.data_since_82 = 0
        return True

    def _common(self, key, kind):
        builder = self.builders.get(key)
        if builder is None:
            return None
        body = builder()
        return None if body is None else (kind, body)

    def _next_periodic(self, m: int):
        if m == RADIANT_TLM_SLOT_PAGE_80:
            got = self._common("common_80", SLOT_COMMON_80)
            if got:
                return got
        if m == RADIANT_TLM_SLOT_PAGE_81:
            got = self._common("common_81", SLOT_COMMON_81)
            if got:
                return got

        if m == 0 and self.frames:
            self.burst = True
            self.burst_index = 0
        if self.burst:
            return self._take_descriptor()

        claimed = self._offer(m)
        if claimed is not None:
            return SLOT_CLIENT, claimed

        if self._page_82_due():
            got = self._common("common_82", SLOT_COMMON_82)
            if got:
                return got

        return self._take_rotation()

    def _next_sparse(self, m: int):
        # The 121-message interleave is useless to a node that sends forty
        # messages a day, so section 8 replaces it: the whole descriptor set
        # rides every heartbeat, and nothing else is periodic.
        if not self.burst:
            self.since_hb += 1
            if self.hb_pending or (self.hb_slots
                                   and self.since_hb >= self.hb_slots):
                self.hb_pending = False
                self.since_hb = 0
                if self.frames:
                    self.burst = True
                    self.burst_index = 0
                self.want_82 = self.builders.get("common_82") is not None

        if self.burst:
            return self._take_descriptor()

        if self.want_82:
            self.want_82 = False
            got = self._common("common_82", SLOT_COMMON_82)
            if got:
                return got

        if self.event_left:
            self.event_left -= 1
            return self._take_data(self.event_page)

        # On a sparse node the silence is where the free slots are, so that is
        # what the client is offered.
        claimed = self._offer(m)
        if claimed is not None:
            return SLOT_CLIENT, claimed

        return SLOT_IDLE, None


# ---------------------------------------------------------------------------
# Dispatch
# ---------------------------------------------------------------------------

_PAGE_DECODERS = {
    PAGE_POWER_STANDARD: decode_power_std,
    PAGE_POWER_WHEEL_TORQUE: decode_power_torque,
    PAGE_POWER_CRANK_TORQUE: decode_power_torque,
    PAGE_POWER_TORQUE_FREQ: decode_power_torque_freq,
    PAGE_COMMON_MANUFACTURER: decode_common_80,
    PAGE_COMMON_PRODUCT: decode_common_81,
    PAGE_COMMON_BATTERY: decode_common_82,
}


def decode(payload: bytes, device_type: int) -> dict:
    """Decode one 8-byte payload heard on a channel of `device_type`.

    `device_type` is not decoration. The combined sensor's page has no page
    number, so byte 0 cannot be trusted to name a page unless the channel says
    it is not 0x79 - which is the entire reason this function takes an argument
    that looks redundant.
    """
    if len(payload) < 8:
        raise ValueError(f"payload is {len(payload)} bytes, need 8")
    payload = bytes(payload[:8])

    if device_type == BSC_COMBINED_DEVICE_TYPE:
        result = decode_bsc_combined(payload)
        result["page"] = None
        return result

    decoder = _PAGE_DECODERS.get(payload[0])
    if decoder is None:
        return {"page": payload[0], "raw": payload}
    return decoder(payload)


# ---------------------------------------------------------------------------
# Capture files
# ---------------------------------------------------------------------------

CAPTURE_HEADER = ("# ant capture v1: <seconds> <device_type> <device_number> "
                  "<8 payload bytes>")


def format_capture_line(t: float, device_type: int, device_number: int,
                        payload: bytes) -> str:
    """One captured packet, as a line of a .antcap file.

        12.345678 0B 3A17 1006ff5039300064

    Deliberately not JSON. These captures are replayed through the C decoders
    in zephyr_aerosense's host tests, and a line of this shape is one sscanf
    there against a parser dependency for JSON.

    The device number is not decoration either: two sensors can share a device
    type - a power meter emitting standard pages and one emitting torque pages
    are both 0x0B - and merging their streams produces one analysis of two
    unrelated series. That is the same mistake as write-back item 1, made in
    the analysis tool instead of the firmware.
    """
    return (f"{t:.6f} {device_type:02X} {device_number & 0xFFFF:04X} "
            f"{bytes(payload[:8]).hex()}")


def parse_capture_line(line: str):
    """Inverse of format_capture_line. Returns None for blanks and comments."""
    line = line.strip()
    if not line or line.startswith("#"):
        return None
    parts = line.split()
    if len(parts) != 4:
        raise ValueError(f"malformed capture line: {line!r}")
    return (float(parts[0]), int(parts[1], 16), int(parts[2], 16),
            bytes.fromhex(parts[3]))


def write_capture(path: str, records, comments=()) -> None:
    """Write a capture, with `comments` recorded as # lines above the data."""
    with open(path, "w") as handle:
        handle.write(CAPTURE_HEADER + "\n")
        for comment in comments:
            handle.write(f"# {comment}\n")
        for t, device_type, device_number, payload in records:
            handle.write(
                format_capture_line(t, device_type, device_number, payload)
                + "\n")


def read_capture(path: str) -> list:
    """Read a capture written by write_capture. Returns a list of records."""
    records = []
    with open(path) as handle:
        for line in handle:
            record = parse_capture_line(line)
            if record is not None:
                records.append(record)
    return records


# Profiles this project simulates and verifies, keyed by the --profile name the
# tools take. Kept here rather than in either tool so ant_sim.py and
# ant_verify.py cannot disagree about a period, which would show up as a loss
# figure rather than as an error.
PROFILES = {
    "power": {
        "device_type": BPWR_DEVICE_TYPE,
        "period": BPWR_PERIOD,
        "pages": (PAGE_POWER_STANDARD,),
        "label": "bicycle power, standard page 0x10",
    },
    "power-torque": {
        "device_type": BPWR_DEVICE_TYPE,
        "period": BPWR_PERIOD,
        "pages": (PAGE_POWER_WHEEL_TORQUE, PAGE_POWER_CRANK_TORQUE),
        "label": "bicycle power, torque pages 0x11 and 0x12",
    },
    "power-torque-freq": {
        "device_type": BPWR_DEVICE_TYPE,
        "period": BPWR_PERIOD,
        "pages": (PAGE_POWER_TORQUE_FREQ,),
        "label": "bicycle power, torque frequency page 0x20",
    },
    "csc": {
        "device_type": BSC_COMBINED_DEVICE_TYPE,
        "period": BSC_COMBINED_PERIOD,
        "pages": (),
        "label": "combined speed and cadence, device type 0x79",
    },
    # Device type 0x60 is per-node by design - the period is announced in
    # descriptor frame 0, not fixed by the type - so docs/profile-registry.md
    # records "per-node" rather than a number and the registry checker does not
    # cross-check these against it. The value below is what the simulator
    # chooses, and it is the ANT+ 4 Hz period only because a familiar number
    # makes a capture easier to read.
    "telemetry": {
        "device_type": RADIANT_TLM_DEVICE_TYPE,
        "period": RADIANT_TLM_PERIOD_DEFAULT,
        "pages": (PAGE_TLM_DESCRIPTOR, 0x01, 0x02),
        "label": "RadiANT generic telemetry, device type 0x60",
    },
    # The envelope with everything turned off: no fields, a heartbeat, and
    # page 82 - which the privacy rule then suppresses. Free to run, and the
    # cheapest exercise of the sparse path there is.
    "asset-tag": {
        "device_type": RADIANT_TLM_DEVICE_TYPE,
        "period": RADIANT_TLM_PERIOD_DEFAULT,
        "pages": (PAGE_TLM_DESCRIPTOR,),
        "label": "RadiANT sparse asset tag, device type 0x60",
    },
}
