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
}
