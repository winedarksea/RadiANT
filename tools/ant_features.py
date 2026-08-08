#!/usr/bin/env python3
"""Check the optional ANT features against what the dongle claims to support.

Two things nothing else here checks:

1. **Capabilities versus coverage.** `ant_capabilities_get()` reports what the
   ANT *stack* can do; the serial messages `dispatch()` implements are what the
   *bridge* can do, and they are separate lists that nothing keeps in step. A
   bit that is advertised and unimplemented is a trap: a host reads the bit,
   sends the configuration message, gets INVALID_MESSAGE back, and gives up -
   the same failure mode that once cost this project a week over
   `ANT_SetTransmitPower`. This walks every optional bit and probes the message
   behind it with a harmless (usually "off") configuration.

2. **Round trips.** Advanced burst, selective data updates and event filtering
   are all set/get pairs, so what went out can be read back and compared. That
   is the only check that catches a payload offset which is wrong by one - the
   reply for some of these messages leads with an index byte and for others
   does not, and both shapes look perfectly plausible on a scope.

    python tools/ant_features.py
    python tools/ant_features.py --port COM8      # a UART build

Nothing here opens a channel or puts anything on the air. Exit status is 0 when
every implemented feature round-trips and every mismatch is one of the known
ones listed in KNOWN_GAPS.
"""

from __future__ import annotations

import argparse
import sys
import time

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

from ant_probe import (  # noqa: E402
    EP_OUT,
    MESG_CAPABILITIES_ID,
    MESG_REQUEST_ID,
    MESG_RESPONSE_EVENT_ID,
    FrameReader,
    close_device,
    frame,
    open_device,
    reset_stack,
)

MESG_EVENT_BUFFERING_CONFIG_ID = 0x74
MESG_HIGH_DUTY_SEARCH_MODE_ID = 0x77
MESG_CONFIG_ADV_BURST_ID = 0x78
MESG_EVENT_FILTER_CONFIG_ID = 0x79
MESG_SDU_CONFIG_ID = 0x7A
MESG_SDU_SET_MASK_ID = 0x7B
MESG_ENCRYPT_ENABLE_ID = 0x7D
MESG_SET_ENCRYPT_KEY_ID = 0x7E
MESG_SET_ENCRYPT_INFO_ID = 0x7F

RESPONSE_NO_ERROR = 0x00
INVALID_MESSAGE = 0x28
INVALID_PARAMETER_PROVIDED = 0x33

# Advanced burst configuration values (ant_parameters.h).
ADV_BURST_MODE_DISABLE = 0x00
ADV_BURST_MODE_ENABLE = 0x01
ADV_BURST_MODES_SIZE_24_BYTES = 0x03
ADV_BURST_MODES_FREQ_HOP = 0x01

# Selective data update: the value that detaches a channel from every mask.
INVALID_SDU_MASK = 0xFF

# Encryption. The set and get info types do not line up: set 0 is the crypto ID
# and get 0 is the supported mode, so a round trip writes with one number and
# reads with the next.
ENCRYPTION_DISABLED_MODE = 0x00
ENCRYPTION_INFO_SET_CRYPTO_ID = 0x00
ENCRYPTION_INFO_SET_CUSTOM_USER_DATA = 0x01
ENCRYPTION_INFO_SET_RNG_SEED = 0x02
ENCRYPTION_INFO_GET_SUPPORTED_MODE = 0x00
ENCRYPTION_INFO_GET_CRYPTO_ID = 0x01
ENCRYPTION_INFO_GET_CUSTOM_USER_DATA = 0x02
ENCRYPTION_USER_DATA_SIZE = 19

# Capabilities byte 6, "advanced options 3": every feature in the ANT tech
# bulletin this file exists to check, plus search sharing, which predates it.
ADV3_BITS = [
    (0x01, "advanced burst"),
    (0x02, "event buffering"),
    (0x04, "event filtering"),
    (0x08, "high duty search"),
    (0x10, "active search sharing"),
    (0x20, "radio coexistence config"),
    (0x40, "selective data updates"),
    (0x80, "single channel encryption"),
]

# Advertised, not bridged, and staying that way. Each entry says why, because
# the alternative - clearing the capability bit - would make this dongle report
# something a real ANT USB-m does not, and hosts do read these bytes.
KNOWN_GAPS = {
    "high duty search":
        "no ant_interface.h API on the single-chip path; sdk-ant handles 0x77 "
        "only in the nRF5340 network-processor passthrough",
    "single channel encryption":
        "the write side is behind CONFIG_ANT_DONGLE_ENCRYPTION, off by "
        "default because ANT_DLL.dll exports no encryption call for any host "
        "to reach it with (the read side is always bridged)",
    "radio coexistence config":
        "ant_coex_config_set() exists but no host API reaches it",
}


def command(dev, reader, msg_id: int, payload: bytes, timeout: float = 2.0):
    """Send a configuration message; return its response code, or None."""
    dev.write(EP_OUT, frame(msg_id, payload))

    deadline = time.monotonic() + timeout
    while True:
        result = reader.next_frame(deadline)
        if result is None:
            return None
        got_id, body = result
        if got_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3 and body[1] == msg_id:
            return body[2]


def request(dev, reader, index: int, req_id: int, timeout: float = 2.0):
    """Request a message; return its payload, or an int response code on refusal.

    `index` occupies the channel slot, which for these messages is not a channel
    at all: it is the mask number, the encryption info type, or - for advanced
    burst - 0 for capabilities and 1 for the current configuration.
    """
    dev.write(EP_OUT, frame(MESG_REQUEST_ID, bytes([index, req_id])))

    deadline = time.monotonic() + timeout
    while True:
        result = reader.next_frame(deadline)
        if result is None:
            return None
        got_id, body = result
        if got_id == req_id:
            return body
        if (got_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3
                and body[1] == MESG_REQUEST_ID):
            return body[2]


def bridged(code) -> bool:
    """Did the message reach the stack, whatever the stack then said?

    A message the dispatcher does not implement is refused by the dispatcher
    itself with INVALID_MESSAGE. Anything else - success, or a complaint about
    the parameters - means the message was decoded and handed on.
    """
    return code is not None and code != INVALID_MESSAGE


def check_coverage(dev, reader, adv3: int) -> list[str]:
    """Probe every advertised optional feature. Returns unexpected mismatches."""
    # Each probe is the "off" configuration for its feature, so a firmware that
    # does implement the message is left exactly as it was found.
    probes = {
        "advanced burst": (MESG_CONFIG_ADV_BURST_ID,
                           bytes([0x00, ADV_BURST_MODE_DISABLE,
                                  ADV_BURST_MODES_SIZE_24_BYTES,
                                  0, 0, 0, 0, 0, 0])),
        "event buffering": (MESG_EVENT_BUFFERING_CONFIG_ID, bytes(6)),
        "event filtering": (MESG_EVENT_FILTER_CONFIG_ID, bytes([0x00, 0x00])),
        "high duty search": (MESG_HIGH_DUTY_SEARCH_MODE_ID, bytes([0x00, 0x00])),
        "selective data updates": (MESG_SDU_CONFIG_ID,
                                   bytes([0x00, INVALID_SDU_MASK])),
        "single channel encryption": (MESG_ENCRYPT_ENABLE_ID,
                                      bytes([0x00, 0x00, 0x00, 0x00])),
    }

    problems = []
    print(f"\n[2/6] advertised capabilities vs bridged messages "
          f"(advanced options 3 = 0x{adv3:02X})")

    for bit, name in ADV3_BITS:
        advertised = bool(adv3 & bit)
        if name not in probes:
            # Search sharing is configured per channel by a message the bridge
            # already implements, and radio coexistence has no host API to
            # reach it. Neither has a probe that is worth a false alarm.
            note = KNOWN_GAPS.get(name, "not probed")
            print(f"  .  {name}: "
                  f"{'advertised' if advertised else 'not advertised'} ({note})")
            continue

        msg_id, payload = probes[name]
        code = command(dev, reader, msg_id, payload)
        implemented = bridged(code)

        if advertised and implemented:
            verdict, mark = "advertised, bridged", "OK"
        elif not advertised and not implemented:
            verdict, mark = "not advertised, not bridged", "OK"
        elif advertised and not implemented:
            if name in KNOWN_GAPS:
                verdict, mark = f"advertised, NOT bridged - known: {KNOWN_GAPS[name]}", "--"
            else:
                verdict, mark = "advertised but NOT bridged - a host that " \
                                "believes the bit will stall here", "!!"
                problems.append(f"{name}: advertised, unimplemented")
        else:
            verdict, mark = ("bridged but not advertised - harmless, though no "
                             "host will ever send it", "--")

        print(f"  {mark} {name} (0x{msg_id:02X}): {verdict}")

    return problems


def check_adv_burst(dev, reader) -> list[str]:
    """Set an advanced burst configuration and read it back."""
    print("\n[3/6] advanced burst round trip (0x78)")
    problems = []

    caps = request(dev, reader, 0, MESG_CONFIG_ADV_BURST_ID)
    if isinstance(caps, (int, type(None))):
        print(f"  FAIL: capabilities request refused ({caps})")
        return ["advanced burst: capabilities unreadable"]
    print(f"  OK: supported = {caps.hex()} "
          f"(max packet size code {caps[0]}, modes 0x{caps[1]:02X})")

    # [filler, enable, rf payload size, required modes, 0, 0, optional modes,
    #  0, 0] - the 8-byte required part of the structure, one byte in.
    wanted = bytes([ADV_BURST_MODE_ENABLE, ADV_BURST_MODES_SIZE_24_BYTES,
                    0x00, 0, 0, ADV_BURST_MODES_FREQ_HOP, 0, 0])
    code = command(dev, reader, MESG_CONFIG_ADV_BURST_ID, bytes([0x00]) + wanted)
    if code != RESPONSE_NO_ERROR:
        print(f"  FAIL: configuration refused, code {code}")
        return ["advanced burst: set refused"]
    print("  OK: 24-byte packets enabled, frequency hopping optional")

    got = request(dev, reader, 1, MESG_CONFIG_ADV_BURST_ID)
    if isinstance(got, (int, type(None))):
        print(f"  FAIL: configuration request refused ({got})")
        problems.append("advanced burst: config unreadable")
    else:
        # The reply drops the enable byte the command carried: it is
        # [size, required, 0, 0, optional, 0, 0, stall lsb, stall msb, retry].
        print(f"  .  read back {got.hex()}")
        if got[0] != wanted[1] or got[1] != wanted[2] or got[4] != wanted[5]:
            print(f"  FAIL: expected size={wanted[1]} required={wanted[2]} "
                  f"optional={wanted[5]}, got size={got[0]} required={got[1]} "
                  f"optional={got[4]}")
            problems.append("advanced burst: readback does not match")
        else:
            print("  OK: readback matches what was set")

    # Leave the stack as it was found - advanced burst is off by default.
    command(dev, reader, MESG_CONFIG_ADV_BURST_ID,
            bytes([0x00, ADV_BURST_MODE_DISABLE, ADV_BURST_MODES_SIZE_24_BYTES,
                   0, 0, 0, 0, 0, 0]))
    return problems


def check_sdu(dev, reader) -> list[str]:
    """Set two selective-data-update masks and read both back."""
    print("\n[4/6] selective data update round trip (0x7B, 0x7A)")
    problems = []

    masks = {
        0: bytes([0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08]),
        1: bytes([0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7]),
    }

    for index, mask in masks.items():
        code = command(dev, reader, MESG_SDU_SET_MASK_ID, bytes([index]) + mask)
        if code != RESPONSE_NO_ERROR:
            # A stack with fewer mask slots than this is not a bridge fault.
            print(f"  -- mask {index}: stack refused the set, code {code}")
            continue

        got = request(dev, reader, index, MESG_SDU_SET_MASK_ID)
        if isinstance(got, (int, type(None))):
            print(f"  FAIL: mask {index} unreadable ({got})")
            problems.append(f"sdu: mask {index} unreadable")
            continue

        # [mask index, mask[8]]. An off-by-one in either direction shows up
        # here as the index byte carrying mask[0] and the mask sliding down.
        if len(got) < 9:
            print(f"  FAIL: mask {index} reply is {len(got)} bytes, want 9: {got.hex()}")
            problems.append(f"sdu: mask {index} reply truncated")
        elif got[0] != index:
            print(f"  FAIL: mask {index} reply leads with 0x{got[0]:02X}, "
                  f"not the index: {got.hex()}")
            problems.append(f"sdu: mask {index} reply misframed")
        elif got[1:9] != mask:
            print(f"  FAIL: mask {index} read back {got[1:9].hex()}, "
                  f"set {mask.hex()}")
            problems.append(f"sdu: mask {index} readback does not match")
        else:
            print(f"  OK: mask {index} = {mask.hex()}, read back identically")

    # Attach a mask to a channel and detach it again. There is no request for
    # this one, so all it proves is that the message is accepted.
    if command(dev, reader, MESG_SDU_CONFIG_ID, bytes([0x00, 0x01])) == RESPONSE_NO_ERROR:
        print("  OK: channel 0 attached to mask 1")
    else:
        print("  -- channel 0 could not be attached to mask 1")
    if command(dev, reader, MESG_SDU_CONFIG_ID,
               bytes([0x00, INVALID_SDU_MASK])) == RESPONSE_NO_ERROR:
        print("  OK: channel 0 detached again")
    else:
        problems.append("sdu: channel could not be detached")
        print("  FAIL: channel 0 could not be detached")

    return problems


def check_event_filter(dev, reader) -> list[str]:
    """Set an event filter and read it back."""
    print("\n[5/6] event filter round trip (0x79)")

    wanted = 0x0006
    code = command(dev, reader, MESG_EVENT_FILTER_CONFIG_ID,
                   bytes([wanted & 0xFF, wanted >> 8]))
    if code != RESPONSE_NO_ERROR:
        print(f"  FAIL: filter refused, code {code}")
        return ["event filter: set refused"]

    got = request(dev, reader, 0, MESG_EVENT_FILTER_CONFIG_ID)
    problems = []
    if isinstance(got, (int, type(None))):
        print(f"  FAIL: filter unreadable ({got})")
        problems.append("event filter: unreadable")
    elif len(got) < 3 or (got[1] | (got[2] << 8)) != wanted:
        print(f"  FAIL: read back {got.hex()}, expected filter 0x{wanted:04X}")
        problems.append("event filter: readback does not match")
    else:
        print(f"  OK: filter 0x{wanted:04X} set and read back")

    # Filtering nothing is the default; leave it that way.
    command(dev, reader, MESG_EVENT_FILTER_CONFIG_ID, bytes([0x00, 0x00]))
    return problems


def check_encryption(dev, reader) -> list[str]:
    """Read the encryption capability, and round-trip the writes if they are in.

    The read side is always bridged. The writes are behind
    CONFIG_ANT_DONGLE_ENCRYPTION and are off in a shipping image, so finding
    them absent is a result, not a failure.

    The read is worth checking on its own: it is the other message whose reply
    leads with the byte that was requested, and a misframed one looks exactly
    like a stack that supports nothing.
    """
    print("\n[6/6] encryption (0x7D–0x7F)")

    got = request(dev, reader, ENCRYPTION_INFO_GET_SUPPORTED_MODE,
                  MESG_ENCRYPT_ENABLE_ID)
    if isinstance(got, (int, type(None))):
        print(f"  -- supported-mode request refused ({got})")
        return []
    if len(got) < 2 or got[0] != ENCRYPTION_INFO_GET_SUPPORTED_MODE:
        print(f"  FAIL: reply {got.hex()} does not lead with the info type")
        return ["encryption: reply misframed"]
    print(f"  OK: reply {got.hex()} (info type echoed, supported mode 0x{got[1]:02X})")

    # Does this build carry the write side? Setting the crypto ID is the
    # cheapest way to ask, and it is also the one write that can be read back.
    wanted_id = bytes([0xDE, 0xAD, 0xBE, 0xEF])
    code = command(dev, reader, MESG_SET_ENCRYPT_INFO_ID,
                   bytes([ENCRYPTION_INFO_SET_CRYPTO_ID]) + wanted_id)
    if not bridged(code):
        print("  -- writes are not compiled in "
              "(CONFIG_ANT_DONGLE_ENCRYPTION=n, the shipping default)")
        return []
    if code != RESPONSE_NO_ERROR:
        print(f"  FAIL: crypto ID refused, code {code}")
        return ["encryption: crypto ID set refused"]

    problems = []
    got = request(dev, reader, ENCRYPTION_INFO_GET_CRYPTO_ID,
                  MESG_ENCRYPT_ENABLE_ID)
    if isinstance(got, (int, type(None))):
        print(f"  FAIL: crypto ID unreadable ({got})")
        problems.append("encryption: crypto ID unreadable")
    elif len(got) < 5 or got[0] != ENCRYPTION_INFO_GET_CRYPTO_ID:
        print(f"  FAIL: crypto ID reply {got.hex()} is misframed")
        problems.append("encryption: crypto ID reply misframed")
    elif got[1:5] != wanted_id:
        print(f"  FAIL: crypto ID read back {got[1:5].hex()}, set {wanted_id.hex()}")
        problems.append("encryption: crypto ID readback does not match")
    else:
        print(f"  OK: crypto ID {wanted_id.hex()} set and read back")

    # Custom user data is the longest message the parser ever sees: one byte of
    # info type plus 19 of payload fills MESG_MAX_SIZE_VALUE exactly.
    wanted_data = bytes(range(1, ENCRYPTION_USER_DATA_SIZE + 1))
    code = command(dev, reader, MESG_SET_ENCRYPT_INFO_ID,
                   bytes([ENCRYPTION_INFO_SET_CUSTOM_USER_DATA]) + wanted_data)
    if code != RESPONSE_NO_ERROR:
        print(f"  -- custom user data refused, code {code}")
    else:
        got = request(dev, reader, ENCRYPTION_INFO_GET_CUSTOM_USER_DATA,
                      MESG_ENCRYPT_ENABLE_ID)
        if isinstance(got, (int, type(None))) or len(got) < 20:
            print(f"  FAIL: custom user data unreadable ({got})")
            problems.append("encryption: user data unreadable")
        elif got[1:20] != wanted_data:
            print(f"  FAIL: user data read back {got[1:20].hex()}")
            problems.append("encryption: user data readback does not match")
        else:
            print(f"  OK: {ENCRYPTION_USER_DATA_SIZE} bytes of custom user data "
                  "set and read back")

    # A key has no getter, so all this proves is that the message is decoded
    # and the stack takes it.
    code = command(dev, reader, MESG_SET_ENCRYPT_KEY_ID, bytes([0x00]) + bytes(16))
    print(f"  {'OK' if code == RESPONSE_NO_ERROR else '--'}: 128-bit key at "
          f"index 0, code {code}")

    # An RNG seed is refused on purpose - no size for it is documented.
    code = command(dev, reader, MESG_SET_ENCRYPT_INFO_ID,
                   bytes([ENCRYPTION_INFO_SET_RNG_SEED]) + bytes(4))
    if code == INVALID_MESSAGE:
        print("  OK: RNG seed refused, as intended")
    else:
        print(f"  -- RNG seed answered {code}, expected INVALID_MESSAGE")

    # Leave every channel unencrypted, whatever this build started with.
    for ch in range(8):
        command(dev, reader, MESG_ENCRYPT_ENABLE_ID,
                bytes([ch, ENCRYPTION_DISABLED_MODE, 0x00, 0x00]))
    print("  OK: encryption disabled on all eight channels")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", help="match a device whose serial ends with this")
    parser.add_argument("--port", help="talk to a UART build over this serial port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("-q", "--quiet", action="store_true")
    args = parser.parse_args()

    dev = open_device(not args.quiet, serial=args.serial, port=args.port,
                      baud=args.baud)
    reader = FrameReader(dev, timeout_ms=250)

    print("\n[1/6] reset and capabilities")
    if not reset_stack(dev, reader):
        print("  FAIL: no startup message after reset")
        close_device(dev)
        return 1
    caps = request(dev, reader, 0, MESG_CAPABILITIES_ID)
    if isinstance(caps, (int, type(None))) or len(caps) < 9:
        print(f"  FAIL: capabilities unreadable ({caps})")
        close_device(dev)
        return 1
    print(f"  OK: {caps.hex()}")

    problems = []
    problems += check_coverage(dev, reader, caps[6])
    problems += check_adv_burst(dev, reader)
    problems += check_sdu(dev, reader)
    problems += check_event_filter(dev, reader)
    problems += check_encryption(dev, reader)

    close_device(dev)

    print()
    if problems:
        print("FAILED:")
        for problem in problems:
            print(f"  - {problem}")
        return 1
    print("PASS: advertised features are bridged, and every set/get round-trips")
    return 0


if __name__ == "__main__":
    sys.exit(main())
