#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Listen for ANT+ sensors, the way a fitness app would.

ant_probe.py proves the dongle answers for itself. This proves the radio
actually works: it runs the same opening sequence Zwift and every other ANT+
application use - set the ANT+ network key, assign a wildcard slave channel,
open it, and report whatever broadcasts arrive - and prints the device number
and type of each sensor it hears.

    python tools/ant_scan.py --seconds 30

Sensors are only heard while they are awake and transmitting. A heart rate
strap needs to be worn, a power meter needs the cranks turning.
"""

from __future__ import annotations

import argparse
import sys
import time

try:
    import usb.core  # noqa: F401 - import-guard, verifies pyusb is installed
except ImportError:  # pragma: no cover - user-facing guidance
    sys.exit("pyusb is not installed. Run: pip install pyusb")

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

from ant_probe import (  # noqa: E402
    EP_OUT,
    FrameReader,
    close_device,
    frame,
    open_device,
    reset_stack,
)

# Protocol constants come from the generated module, never from a second copy
# here. See tools/ant_wire.py and protocol/ant_wire.yaml.
from ant_wire import (  # noqa: E402
    EXT_FLAG_CHANNEL_ID,
    LIB_CONFIG_ALL_EXT_FIELDS,
    MESG_ACKNOWLEDGED_DATA_ID,
    MESG_ANTLIB_CONFIG_ID,
    MESG_ASSIGN_CHANNEL_ID,
    MESG_BROADCAST_DATA_ID,
    MESG_BURST_DATA_ID,
    MESG_CHANNEL_ID_ID,
    MESG_CHANNEL_MESG_PERIOD_ID,
    MESG_CHANNEL_RADIO_FREQ_ID,
    MESG_CHANNEL_SEARCH_TIMEOUT_ID,
    MESG_CLOSE_CHANNEL_ID,
    MESG_NETWORK_KEY_ID,
    MESG_OPEN_CHANNEL_ID,
    MESG_RESPONSE_EVENT_ID,
)

# The public ANT+ network key. Every ANT+ sensor uses it; it is published in
# the ANT+ device profiles and hardcoded in every open-source ANT+ library.
ANT_PLUS_KEY = bytes([0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45])
ANT_PLUS_FREQ = 57       # 2457 MHz
CHANNEL = 0

DEVICE_TYPES = {
    11: "power meter",
    120: "heart rate monitor",
    121: "speed and cadence",
    122: "cadence",
    123: "speed",
    124: "stride-based speed",
    17: "fitness equipment (trainer)",
}


def command(dev, reader, msg_id: int, payload: bytes, name: str,
            timeout: float = 2.0) -> bool:
    """Send a configuration message and wait for its 0x40 acknowledgement."""
    dev.write(EP_OUT, frame(msg_id, payload))

    deadline = time.monotonic() + timeout
    while True:
        result = reader.next_frame(deadline)
        if result is None:
            print(f"  FAIL: {name} was not acknowledged")
            return False

        got_id, body = result
        if got_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3:
            if body[1] != msg_id:
                continue
            if body[2] != 0:
                print(f"  FAIL: {name} rejected, code {body[2]}")
                return False
            print(f"  OK: {name}")
            return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=30.0,
                        help="how long to listen (default: 30)")
    parser.add_argument("--serial",
                        help="match a device whose serial ends with this")
    parser.add_argument(
        "--port",
        help="talk to a UART build over this serial port (e.g. COM8, "
             "/dev/ttyACM1) instead of over USB",
    )
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    dev = open_device(True, serial=args.serial, port=args.port,
                      baud=args.baud)
    reader = FrameReader(dev)

    print("\nOpening a wildcard ANT+ receive channel")
    # Start from a known state, or a channel another tool left assigned makes
    # the assign below fail with CHANNEL_IN_WRONG_STATE.
    if not reset_stack(dev, reader):
        print("  FAIL: no startup message after reset")
        return 1

    # 0xE0 is all three extended fields: channel id, RSSI, receive timestamp.
    # Without the channel id, sensors can be heard but not named. The other
    # two are free and keep every tool on one lib config; not worth aborting
    # over if refused.
    command(dev, reader, MESG_ANTLIB_CONFIG_ID,
            bytes([0x00, LIB_CONFIG_ALL_EXT_FIELDS]), "extended messages")

    steps = [
        (MESG_NETWORK_KEY_ID, bytes([0]) + ANT_PLUS_KEY, "network key"),
        (MESG_ASSIGN_CHANNEL_ID, bytes([CHANNEL, 0x00, 0x00]),
         "assign channel (slave)"),
        # Device number 0, type 0, transmission type 0: match anything.
        (MESG_CHANNEL_ID_ID, bytes([CHANNEL, 0, 0, 0, 0]), "wildcard channel id"),
        (MESG_CHANNEL_RADIO_FREQ_ID, bytes([CHANNEL, ANT_PLUS_FREQ]),
         "radio frequency 2457 MHz"),
        # 8070 counts of 32768 Hz, the ANT+ heart rate period. A slave on a
        # wildcard search still hears faster and slower profiles.
        (MESG_CHANNEL_MESG_PERIOD_ID, bytes([CHANNEL, 0x86, 0x1F]),
         "message period"),
        (MESG_CHANNEL_SEARCH_TIMEOUT_ID, bytes([CHANNEL, 0xFF]),
         "infinite search timeout"),
        (MESG_OPEN_CHANNEL_ID, bytes([CHANNEL]), "open channel"),
    ]

    for msg_id, payload, name in steps:
        if not command(dev, reader, msg_id, payload, name):
            return 1

    print(f"\nListening for {args.seconds:.0f} s")
    seen: dict[tuple[int, int], int] = {}
    packets = 0
    deadline = time.monotonic() + args.seconds

    while time.monotonic() < deadline:
        result = reader.next_frame(min(deadline, time.monotonic() + 1.0))
        if result is None:
            continue

        msg_id, body = result
        if msg_id in (MESG_BROADCAST_DATA_ID, MESG_ACKNOWLEDGED_DATA_ID,
                      MESG_BURST_DATA_ID):
            packets += 1
            # Extended messages append the channel id first (device number,
            # device type, trans type), then RSSI and receive timestamp, which
            # this tool ignores (see ant_verify.py's extended_fields()). Fixed
            # offsets work because the channel id always comes first.
            if len(body) >= 13 and body[9] & EXT_FLAG_CHANNEL_ID:
                number = body[10] | (body[11] << 8)
                dtype = body[12] & 0x7F
                key = (number, dtype)
                if key not in seen:
                    label = DEVICE_TYPES.get(dtype, f"device type {dtype}")
                    print(f"  found: #{number} - {label}")
                seen[key] = seen.get(key, 0) + 1

    print("\nClosing")
    command(dev, reader, MESG_CLOSE_CHANNEL_ID, bytes([CHANNEL]),
            "close channel")
    close_device(dev)

    print()
    if packets == 0:
        print("No broadcasts heard. The channel opened and the radio ran, so "
              "the dongle is working;\nthere was simply nothing transmitting "
              "in range. Wear a heart rate strap or spin\na power meter and "
              "run this again.")
        return 0

    if seen:
        print(f"Heard {packets} broadcasts from {len(seen)} sensor(s)")
    else:
        print(f"Heard {packets} broadcasts, but without channel ids - the "
              "radio works, the\nlibrary config for extended messages did not "
              "take.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
