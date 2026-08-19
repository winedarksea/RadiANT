#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Measure what a SEARCHING channel costs a channel that is already TRACKING.

Two Zwift captures show the same step: while one channel searches, another
channel that is tracking a sensor at full signal loses close to every other
slot, in a clean OK/FAIL alternation, and the searching channel hears almost
nothing either. `captures/zwift-20260818-225148.pcap` has it twice - once with
Zwift's wildcard discovery channel open (49.5% loss on the tracked power
meter), once after the heart-rate strap was switched off and its channel fell
back to search (49.2%, perfect alternation) - against 10.8% for the same
channel with nothing searching.

Neither `radiant_sched` nor the `api` ztest reproduces it (see
`radiant/tests/src/test_sched.c` and `radiant/tests/api/src/test_api.c`), so
the measurement has to happen on real hardware. This is that measurement,
without Zwift in the loop:

    A   track one sensor, nothing else open        <- control
    B   same, plus a wildcard search channel       <- arm
    A'  search closed again                        <- control repeat

A'/A guards the result against the room changing under it: this bench floor is
0.4% in a quiet room and 5-8% with the interference present in August 2026, so
only an effect far above that spread means anything. The alternation column is
the discriminator that RF noise cannot fake - a radio losing packets to
interference does not lose exactly every second one.

    python tools/ant_search_contention.py --seconds 30

Needs a sensor on air, as ant_scan.py does. It picks the one it hears most in
an opening sweep, so spin the cranks or wear the strap while it starts.
"""

from __future__ import annotations

import argparse
import sys
import time
from collections import Counter

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

from ant_wire import (  # noqa: E402
    EVENT_RX_FAIL,
    EVENT_RX_FAIL_GO_TO_SEARCH,
    EVENT_RX_SEARCH_TIMEOUT,
    EXT_FLAG_CHANNEL_ID,
    LIB_CONFIG_ALL_EXT_FIELDS,
    MESG_ANTLIB_CONFIG_ID,
    MESG_ASSIGN_CHANNEL_ID,
    MESG_BROADCAST_DATA_ID,
    MESG_CHANNEL_ID_ID,
    MESG_CHANNEL_MESG_PERIOD_ID,
    MESG_CHANNEL_RADIO_FREQ_ID,
    MESG_CHANNEL_SEARCH_TIMEOUT_ID,
    MESG_CLOSE_CHANNEL_ID,
    MESG_EVENT_ID,
    MESG_NETWORK_KEY_ID,
    MESG_OPEN_CHANNEL_ID,
    MESG_RESPONSE_EVENT_ID,
    MESG_SET_LP_SEARCH_TIMEOUT_ID,
    MESG_UNASSIGN_CHANNEL_ID,
)

ANT_PLUS_KEY = bytes([0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45])
ANT_PLUS_FREQ = 57       # 2457 MHz
TRACK_CH = 1
SEARCH_CH = 0

# Counts of 32768 Hz, from the ANT+ device profiles. A tracked channel must be
# opened on its sensor's own period or every prediction is wrong, which would
# look exactly like the defect being measured.
PERIODS = {
    11: 8182,    # bicycle power
    120: 8070,   # heart rate
    121: 8086,   # speed and cadence
    122: 8102,   # cadence
    123: 8118,   # speed
    124: 8134,   # stride-based speed
    17: 8192,    # fitness equipment
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
                print(f"  FAIL: {name} rejected, code 0x{body[2]:02x}")
                return False
            return True


def collect(reader, seconds: float, ch: int):
    """Return one entry per slot on `ch`: True heard, False EVENT_RX_FAIL.

    Slots on other channels are counted separately. Every other event is
    returned too, because a SEARCH_TIMEOUT or GO_TO_SEARCH mid-arm invalidates
    the arm rather than lowering its score.
    """
    slots: list[bool] = []
    other = Counter()
    notes: list[str] = []
    deadline = time.monotonic() + seconds

    while time.monotonic() < deadline:
        result = reader.next_frame(min(deadline, time.monotonic() + 0.5))
        if result is None:
            continue
        msg_id, body = result
        if msg_id == MESG_BROADCAST_DATA_ID and body:
            if body[0] == ch:
                slots.append(True)
            else:
                other[body[0]] += 1
            continue
        if msg_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3:
            if body[1] != MESG_EVENT_ID:
                continue
            if body[0] == ch and body[2] == EVENT_RX_FAIL:
                slots.append(False)
            elif body[2] in (EVENT_RX_FAIL_GO_TO_SEARCH,
                             EVENT_RX_SEARCH_TIMEOUT):
                notes.append(f"ch{body[0]} event 0x{body[2]:02x}")
    return slots, other, notes


def score(label: str, slots, other, notes) -> float:
    """Print one arm and return its loss percentage."""
    if not slots:
        print(f"  {label:26s} no slots seen")
        return float("nan")
    lost = slots.count(False)
    loss = 100.0 * lost / len(slots)
    # Alternation: how much of the run is a strict OK/FAIL/OK/FAIL sequence.
    # Interference does not produce it; a radio being taken away every other
    # period does.
    flips = sum(1 for i in range(len(slots) - 1) if slots[i] != slots[i + 1])
    alt = 100.0 * flips / (len(slots) - 1) if len(slots) > 1 else 0.0
    extra = f"  search ch heard {sum(other.values())}" if other else ""
    print(f"  {label:26s} {len(slots):4d} slots  {loss:5.1f}% loss  "
          f"alternating {alt:5.1f}%{extra}")
    for note in notes:
        print(f"      note: {note}")
    return loss


def open_search(dev, reader, ext: int | None = None, lp: int | None = None) -> bool:
    """Zwift's discovery channel: wildcard slave, infinite search timeout.

    `ext` is the extended-assign byte ANT appends to ASSIGN_CHANNEL - Zwift
    sends `0x01` (background scanning), which is a different channel MODE and
    not a flavour of the same one, so an arm without it is testing something
    else. `lp` is SET_LP_SEARCH_TIMEOUT, which Zwift sets to 0. Both default
    to absent because the plain form is the useful control.
    """
    assign = bytes([SEARCH_CH, 0x00, 0x00])
    if ext is not None:
        assign += bytes([ext])
    steps = [
        (MESG_ASSIGN_CHANNEL_ID, assign, "search assign"),
    ]
    # After the assign, never before: an unassigned channel answers
    # CHANNEL_IN_WRONG_STATE, which is what Zwift's own ordering avoids.
    if lp is not None:
        steps.append((MESG_SET_LP_SEARCH_TIMEOUT_ID, bytes([SEARCH_CH, lp]),
                      "search lp timeout"))
    steps += [
        (MESG_CHANNEL_ID_ID, bytes([SEARCH_CH, 0, 0, 0, 0]), "search wildcard id"),
        (MESG_CHANNEL_RADIO_FREQ_ID, bytes([SEARCH_CH, ANT_PLUS_FREQ]),
         "search frequency"),
        (MESG_CHANNEL_SEARCH_TIMEOUT_ID, bytes([SEARCH_CH, 0xFF]),
         "search timeout"),
        (MESG_OPEN_CHANNEL_ID, bytes([SEARCH_CH]), "search open"),
    ]
    return all(command(dev, reader, m, p, n) for m, p, n in steps)


def close_search(dev, reader) -> None:
    command(dev, reader, MESG_CLOSE_CHANNEL_ID, bytes([SEARCH_CH]), "search close")
    command(dev, reader, MESG_UNASSIGN_CHANNEL_ID, bytes([SEARCH_CH]),
            "search unassign")


def discover(dev, reader, seconds: float):
    """Sweep for sensors and return the (number, type, trans) heard most."""
    if not open_search(dev, reader):
        return None
    print(f"  sweeping {seconds:.0f} s for a sensor")
    heard: Counter = Counter()
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = reader.next_frame(min(deadline, time.monotonic() + 0.5))
        if result is None:
            continue
        msg_id, body = result
        if msg_id != MESG_BROADCAST_DATA_ID or len(body) < 14:
            continue
        if not body[9] & EXT_FLAG_CHANNEL_ID:
            continue
        heard[(body[10] | (body[11] << 8), body[12] & 0x7F, body[13])] += 1
    close_search(dev, reader)

    for (num, dtype, trans), n in heard.most_common():
        print(f"    #{num} type {dtype} trans 0x{trans:02x}: {n} packets")
    for (num, dtype, trans), _ in heard.most_common():
        if dtype in PERIODS:
            return num, dtype, trans
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=30.0,
                        help="length of each arm (default: 30)")
    parser.add_argument("--sweep", type=float, default=15.0,
                        help="opening sweep for a sensor (default: 15)")
    parser.add_argument("--device", type=int,
                        help="track this device number instead of sweeping")
    parser.add_argument("--type", type=int, help="its ANT+ device type")
    parser.add_argument("--trans", type=int, default=0,
                        help="its transmission type (default: 0, wildcard)")
    parser.add_argument("--ext", type=lambda s: int(s, 0),
                        help="extended-assign byte for the search channel; "
                             "Zwift sends 0x01 (background scanning)")
    parser.add_argument("--lp", type=lambda s: int(s, 0),
                        help="SET_LP_SEARCH_TIMEOUT for the search channel; "
                             "Zwift sends 0")
    parser.add_argument("--serial", help="match a dongle by serial suffix")
    parser.add_argument("--port", help="talk to a UART build over this port")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    dev = open_device(True, serial=args.serial, port=args.port, baud=args.baud)
    reader = FrameReader(dev)

    if not reset_stack(dev, reader):
        print("  FAIL: no startup message after reset")
        return 1
    command(dev, reader, MESG_ANTLIB_CONFIG_ID,
            bytes([0x00, LIB_CONFIG_ALL_EXT_FIELDS]), "extended messages")
    if not command(dev, reader, MESG_NETWORK_KEY_ID,
                   bytes([0]) + ANT_PLUS_KEY, "network key"):
        return 1

    if args.device is not None and args.type is not None:
        target = (args.device, args.type, args.trans)
    else:
        target = discover(dev, reader, args.sweep)
        if target is None:
            print("\nNo ANT+ sensor of a known type heard. Spin the cranks or "
                  "wear the strap\nand run this again.")
            return 1
    number, dtype, trans = target
    period = PERIODS[dtype]
    print(f"\nTracking #{number} (type {dtype}) at period {period} "
          f"({period / 32768.0 * 1000.0:.1f} ms)")

    steps = [
        (MESG_ASSIGN_CHANNEL_ID, bytes([TRACK_CH, 0x00, 0x00]), "track assign"),
        (MESG_CHANNEL_ID_ID,
         bytes([TRACK_CH, number & 0xFF, (number >> 8) & 0xFF, dtype, trans]),
         "track channel id"),
        (MESG_CHANNEL_RADIO_FREQ_ID, bytes([TRACK_CH, ANT_PLUS_FREQ]),
         "track frequency"),
        (MESG_CHANNEL_MESG_PERIOD_ID,
         bytes([TRACK_CH, period & 0xFF, (period >> 8) & 0xFF]), "track period"),
        (MESG_CHANNEL_SEARCH_TIMEOUT_ID, bytes([TRACK_CH, 0xFF]),
         "track search timeout"),
        (MESG_OPEN_CHANNEL_ID, bytes([TRACK_CH]), "track open"),
    ]
    for msg_id, payload, name in steps:
        if not command(dev, reader, msg_id, payload, name):
            return 1

    # Let it acquire before anything is measured: the acquisition itself is a
    # search, so counting it would put the arm's own effect into the control.
    print("  acquiring")
    collect(reader, 5.0, TRACK_CH)

    print(f"\nA/B/A, {args.seconds:.0f} s per arm")
    a1 = score("A  no search", *collect(reader, args.seconds, TRACK_CH))

    if not open_search(dev, reader, ext=args.ext, lp=args.lp):
        return 1
    b = score("B  wildcard search open", *collect(reader, args.seconds, TRACK_CH))
    close_search(dev, reader)

    a2 = score("A' search closed again", *collect(reader, args.seconds, TRACK_CH))

    command(dev, reader, MESG_CLOSE_CHANNEL_ID, bytes([TRACK_CH]), "track close")
    close_device(dev)

    print()
    controls = [x for x in (a1, a2) if x == x]
    if not controls or b != b:
        print("Inconclusive: an arm produced no slots.")
        return 1
    floor = max(controls)
    spread = abs(a1 - a2) if len(controls) == 2 else 0.0
    print(f"controls {a1:.1f}% / {a2:.1f}% (spread {spread:.1f}), "
          f"search open {b:.1f}%")
    if b > floor + 15.0 and b > 2.0 * spread:
        print("REPRODUCED: a searching channel costs a tracking one its slots.")
        return 2
    print("Not reproduced in this sitting.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
