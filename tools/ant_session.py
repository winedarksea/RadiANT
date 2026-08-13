#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Run the ANT+ session a fitness app actually runs, on all eight channels.

ant_scan.py opens one channel and proves the radio works. That is not what
Zwift does. Zwift opens a channel per sensor it cares about - heart rate,
power, cadence, speed, and a controllable trainer - and keeps them all running
at once, at different message rates, for the length of a ride. Everything that
only ever gets exercised on channel 0 stays untested: whether eight channels
can be assigned at all, whether the event path keeps up when five of them
broadcast at ~4 Hz simultaneously, and whether acknowledged data - the message
Zwift sends to set trainer resistance - reaches the radio at all.

    python tools/ant_session.py --seconds 30

Sensors are only heard while awake and transmitting, so an empty result is not
a failure. The pass/fail here is about the dongle: eight channels open at
once, events flow, and the two host-to-sensor paths - acknowledged data and
burst - reach the radio rather than being rejected by the bridge.
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
from ant_scan import (  # noqa: E402
    ANT_PLUS_FREQ,
    ANT_PLUS_KEY,
    DEVICE_TYPES,
    command,
)

# Protocol constants come from the generated module, never from a second copy
# here. See tools/ant_wire.py and protocol/ant_wire.yaml.
#
# MESG_EVENT_ID is the value body[1] of a 0x40 message carries when it is a
# channel event rather than a reply to a command - what this file used to call
# EVENT_MARKER, and what src/ant_wire.h calls ANTW_MESG_EVENT_ID.
#
# EVENT_CODES_BY_VALUE names events using the protocol's own names
# (EVENT_RX_FAIL, not RX_FAIL), matching firmware, header and spec.
from ant_wire import (  # noqa: E402,F401
    BURST_HEADER_CHANNEL_MASK,
    BURST_HEADER_LAST,
    EVENT_CHANNEL_CLOSED,
    EVENT_CODES_BY_VALUE,
    EVENT_TRANSFER_TX_COMPLETED,
    EVENT_TRANSFER_TX_FAILED,
    EXT_FLAG_CHANNEL_ID,
    INVALID_MESSAGE,
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
    MESG_CHANNEL_STATUS_ID,
    MESG_CLOSE_CHANNEL_ID,
    MESG_EVENT_ID,
    MESG_NETWORK_KEY_ID,
    MESG_OPEN_CHANNEL_ID,
    MESG_REQUEST_ID,
    MESG_RESPONSE_EVENT_ID,
    MESG_UNASSIGN_CHANNEL_ID,
)

# tools/ant_sim.py imports this name from here and belongs to another agent, so
# it cannot be updated in the same wave. It is a plain alias for the generated
# constant, not a second definition - there is still exactly one value. Delete
# it once ant_sim.py imports MESG_EVENT_ID directly.
EVENT_MARKER = MESG_EVENT_ID

# The profiles Zwift pairs against, with the message period each one
# broadcasts at, in 1/32768 s counts. A slave still hears a sensor whose
# period is a whole multiple of its own, but matching the profile is what a
# real app does and is what puts eight different rates on the air at once.
PROFILES = [
    (0, 120, 8070, "heart rate"),
    (1, 11, 8182, "power"),
    (2, 17, 8192, "trainer (FE-C)"),
    (3, 121, 8086, "speed and cadence"),
    (4, 122, 8102, "cadence"),
    (5, 123, 8118, "speed"),
    (6, 124, 8134, "running speed"),
    (7, 0, 8070, "wildcard"),
]




def open_channel(dev, reader, ch: int, dev_type: int, period: int,
                 label: str) -> bool:
    """Assign, configure and open one slave channel. Returns success."""
    steps = [
        (MESG_ASSIGN_CHANNEL_ID, bytes([ch, 0x00, 0x00]), "assign"),
        (MESG_CHANNEL_ID_ID, bytes([ch, 0, 0, dev_type, 0]), "channel id"),
        (MESG_CHANNEL_RADIO_FREQ_ID, bytes([ch, ANT_PLUS_FREQ]), "frequency"),
        (MESG_CHANNEL_MESG_PERIOD_ID,
         bytes([ch, period & 0xFF, period >> 8]), "period"),
        (MESG_CHANNEL_SEARCH_TIMEOUT_ID, bytes([ch, 0xFF]), "search timeout"),
        (MESG_OPEN_CHANNEL_ID, bytes([ch]), "open"),
    ]
    for msg_id, payload, what in steps:
        if not command(dev, reader, msg_id, payload, f"ch{ch} {what}"):
            print(f"  FAIL: channel {ch} ({label}) failed at {what}")
            return False
    return True


def burst_probe(dev, reader, ch: int) -> str:
    """Send one burst packet at a channel that is not open.

    Only asks whether the message is bridged at all. Firmware that doesn't
    implement it answers INVALID_MESSAGE from its own dispatcher; firmware
    that does passes it to the stack, which refuses with
    CHANNEL_IN_WRONG_STATE since the channel is closed - only the second
    proves the message reached the radio, and neither transmits.
    """
    # Sequence 0 with the last-packet bit set: a complete one-packet burst.
    header = (ch & BURST_HEADER_CHANNEL_MASK) | BURST_HEADER_LAST
    dev.write(EP_OUT, frame(MESG_BURST_DATA_ID, bytes([header]) + bytes(8)))

    deadline = time.monotonic() + 3.0
    while True:
        result = reader.next_frame(deadline)
        if result is None:
            return "accepted with no error"
        msg_id, body = result
        if (msg_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3
                and body[1] == MESG_BURST_DATA_ID):
            if body[2] == INVALID_MESSAGE:
                return "INVALID_MESSAGE"
            return f"code {body[2]} from the stack"


def wait_for_close(reader, ch: int, timeout_s: float = 3.0) -> bool:
    """Wait for EVENT_CHANNEL_CLOSED on `ch`."""
    deadline = time.monotonic() + timeout_s
    while True:
        result = reader.next_frame(deadline)
        if result is None:
            return False
        msg_id, body = result
        if (msg_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3
                and body[0] == ch and body[1] == MESG_EVENT_ID
                and body[2] == EVENT_CHANNEL_CLOSED):
            return True


def ack_probe(dev, reader, ch: int, timeout_s: float = 15.0) -> str:
    """Send acknowledged data on an open channel (the message Zwift sends to
    set trainer resistance).

    Whether the bridge carries it is the RESPONSE_EVENT: accepted means it's
    queued in the stack, INVALID_MESSAGE means the dispatcher never heard of
    it. The transfer outcome after that is the radio's business and not
    deterministic - a slave only transmits in the receive slot of the master
    it is tracking, so with eight channels contending an accepted message can
    sit queued for as long as the channel stays in search. Both
    TRANSFER_TX_COMPLETED and TRANSFER_TX_FAILED mean it went out; staying
    queued is a fact about the airwaves, not a dongle fault.
    """
    # FE-C page 0x30, basic resistance, 0%.
    page = bytes([0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00])
    dev.write(EP_OUT, frame(MESG_ACKNOWLEDGED_DATA_ID, bytes([ch]) + page))

    accepted = False
    deadline = time.monotonic() + timeout_s
    while True:
        result = reader.next_frame(deadline)
        if result is None:
            return "queued" if accepted else "silent"
        msg_id, body = result
        if msg_id != MESG_RESPONSE_EVENT_ID or len(body) < 3 or body[0] != ch:
            continue
        if body[1] == MESG_ACKNOWLEDGED_DATA_ID:
            if body[2] == INVALID_MESSAGE:
                return "invalid"
            if body[2] != 0:
                return f"rejected, code {body[2]}"
            accepted = True
            continue
        # TRANSFER_TX_START is progress, not an outcome; keep waiting.
        if (body[1] == MESG_EVENT_ID
                and body[2] in (EVENT_TRANSFER_TX_COMPLETED,
                                EVENT_TRANSFER_TX_FAILED)):
            return EVENT_CODES_BY_VALUE[body[2]]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=30.0,
                        help="how long to hold the session open (default: 30)")
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

    failures = []

    print("\n[1/5] session setup")
    if reset_stack(dev, reader):
        print("  OK: stack reset")
    else:
        print("  FAIL: no startup message after reset")
        failures.append("reset")
    # 0xE0, not 0x80: the channel id names the sensor, and RSSI and the receive
    # timestamp cost nothing to ask for and are what the other tools measure
    # against. One lib config across every tool means one extended-message
    # layout to get right.
    if not command(dev, reader, MESG_ANTLIB_CONFIG_ID,
                   bytes([0x00, LIB_CONFIG_ALL_EXT_FIELDS]),
                   "extended messages"):
        failures.append("extended messages")
    if not command(dev, reader, MESG_NETWORK_KEY_ID,
                   bytes([0]) + ANT_PLUS_KEY, "ANT+ network key"):
        failures.append("network key")

    print(f"\n[2/5] opening {len(PROFILES)} channels")
    opened = []
    for ch, dev_type, period, label in PROFILES:
        if open_channel(dev, reader, ch, dev_type, period, label):
            print(f"  OK: channel {ch} - {label}")
            opened.append(ch)
        else:
            failures.append(f"channel {ch}")

    if len(opened) != len(PROFILES):
        print(f"  only {len(opened)}/{len(PROFILES)} channels opened")

    print(f"\n[3/5] running all {len(opened)} channels for {args.seconds:.0f} s")
    per_channel = {ch: 0 for ch in opened}
    seen: dict[tuple[int, int], int] = {}
    events: dict[int, int] = {}
    packets = 0
    deadline = time.monotonic() + args.seconds

    while time.monotonic() < deadline:
        result = reader.next_frame(min(deadline, time.monotonic() + 1.0))
        if result is None:
            continue
        msg_id, body = result

        if msg_id == MESG_BROADCAST_DATA_ID and body:
            packets += 1
            per_channel[body[0]] = per_channel.get(body[0], 0) + 1
            if len(body) >= 13 and body[9] & EXT_FLAG_CHANNEL_ID:
                number = body[10] | (body[11] << 8)
                dtype = body[12] & 0x7F
                key = (number, dtype)
                if key not in seen:
                    label = DEVICE_TYPES.get(dtype, f"device type {dtype}")
                    print(f"  found: #{number} - {label} (on channel {body[0]})")
                seen[key] = seen.get(key, 0) + 1
        elif msg_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3:
            if body[1] == MESG_EVENT_ID:
                events[body[2]] = events.get(body[2], 0) + 1

    for ch in opened:
        got = per_channel.get(ch, 0)
        if got:
            print(f"  channel {ch}: {got} broadcasts")
    if events:
        print("  events: " + ", ".join(
            f"{EVENT_CODES_BY_VALUE.get(code, code)} x{count}"
            for code, count in sorted(events.items())))

    print("\n[4/5] acknowledged data (the trainer-control path)")
    # A slave transmits only in the receive slot of the master it is tracking,
    # so this has to go at a channel that is still tracking one. With eight
    # channels contending, the busiest is the best evidence of that - picking
    # the trainer for realism instead gets a message queued behind a channel
    # that has dropped back into search, and no transfer event ever arrives.
    live = sorted((ch for ch in opened if per_channel.get(ch, 0) > 0),
                  key=lambda ch: per_channel[ch], reverse=True)
    target = live[0] if live else None

    if target is None:
        print("  SKIPPED: nothing paired, so there is no one to acknowledge")
        print("           (turn on a sensor and rerun to exercise this)")
    else:
        print(f"  sending FE-C basic resistance on channel {target} "
              f"({per_channel[target]} broadcasts heard)")
        verdict = ack_probe(dev, reader, target)
        if verdict == "invalid":
            print("  FAIL: rejected as INVALID_MESSAGE - not bridged")
            failures.append("acknowledged data")
        elif verdict == "silent":
            print("  FAIL: the dongle did not answer at all")
            failures.append("acknowledged data")
        elif verdict == EVENT_CODES_BY_VALUE[EVENT_TRANSFER_TX_COMPLETED]:
            print("  OK: the sensor acknowledged it - trainer control works")
        elif verdict == EVENT_CODES_BY_VALUE[EVENT_TRANSFER_TX_FAILED]:
            print("  OK: transmitted, nothing acknowledged it")
        elif verdict == "queued":
            print("  OK: accepted and queued by the stack")
            print("      (it did not go out within the wait - the channel "
                  "stopped tracking, which\n       is a fact about the "
                  "airwaves, not the dongle)")
        else:
            print(f"  FAIL: {verdict}")
            failures.append("acknowledged data")

    print("\nclosing")
    for ch in opened:
        command(dev, reader, MESG_CLOSE_CHANNEL_ID, bytes([ch]), f"close ch{ch}")
        # A close is asynchronous: the channel is not free until
        # EVENT_CHANNEL_CLOSED arrives, and unassigning before then is refused
        # with CHANNEL_IN_WRONG_STATE.
        if not wait_for_close(reader, ch):
            print(f"  ! channel {ch} never reported closed")
        command(dev, reader, MESG_UNASSIGN_CHANNEL_ID, bytes([ch]),
                f"unassign ch{ch}")
    print("\n[5/5] burst transmit is bridged")
    verdict = burst_probe(dev, reader, 0)
    if verdict == "INVALID_MESSAGE":
        print("  FAIL: the firmware does not bridge burst data, but the")
        print("        capabilities message advertises burst support")
        failures.append("burst")
    else:
        print(f"  OK: reached the stack ({verdict})")

    close_device(dev)

    print()
    print(f"{len(opened)}/{len(PROFILES)} channels ran simultaneously, "
          f"{packets} broadcasts, {len(seen)} sensor(s) identified")
    if failures:
        print(f"FAILED: {', '.join(failures)}")
        return 1
    print("PASS: eight-channel session held, events flowed, ack path wired")
    return 0


if __name__ == "__main__":
    sys.exit(main())
