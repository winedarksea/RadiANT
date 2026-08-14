#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""RF-6: does a stock ANT+ dongle ignore a length-extended 1 M frame?

The measurement instrument for the open question in
docs/decisions/0007-long-range-phy.md ("Does 1 M also qualify? - NOT
SETTLED"). ADR 0007 permits length extension only where the coded PHY makes
us physically invisible to a stock receiver; whether a RadiANT *1 M* format
would also be safe is unsettled, and if so most of the descriptor-set-collapse
phase (the record's largest battery item) becomes optional. This tool settles
it rather than assuming.

    python tools/ant_rf6_capture.py --seconds 120 \
        --expect-seen 0x60A0 --expect-unseen 0x60B0

## The experiment this grades

A separate board transmits two frames, both device type 0x60:

- device **0x60A0**, a standard 10-byte body - the control;
- device **0x60B0**, a 30-byte body - the length-extended frame under test.

A stock dongle should hear 0x60A0 and should **never** hear 0x60B0 (the long
frame's CRC covers bytes a stock receiver never clocks in, so it should drop
in hardware). Real ANT+ sensors in the room should keep being reported the
whole window too - a receiver gone deaf to its own sensors while the long
frame is on air has failed just as surely as one that decodes it.

Three outcomes, hence self-grading rather than eyeballed:

- **0x60A0 seen + 0x60B0 unseen + other sensors still seen** -> interop-safe.
  ADR 0007 gains a second, weaker-justified (device-type isolation, not
  physical invisibility) 1 M length-extended format.
- **0x60A0 seen + 0x60B0 also seen** -> NOT safe. ADR 0007's exclusion stands.
- **0x60A0 unseen** -> the rig is broken (wrong address/frequency, dead
  transmitter, sick dongle) and the run says **nothing either way** - silence
  looks identical to "correctly ignored", so --expect-seen makes that
  ambiguity impossible to misread as a pass.

## Why this is not ant_scan.py

ant_scan.py's plain wildcard slave channel acquires the first sensor it hears
and tracks only that one - useless here, since the run must keep hearing
every device in the room for the whole window.

MESG_ASSIGN_CHANNEL's optional fourth byte, bit 0
(EXT_PARAM_ALWAYS_SEARCH), is "background scan": the channel never locks on
and reports every device it hears. This is what Zwift does against a stock
dongle (captured in radiant/tests/api/src/test_api.c,
test_a_background_scan_reports_every_device_not_only_the_first); the stock
dongle advertises it as CAPABILITIES_EXT_ASSIGN_ENABLED.

Extended messages use lib config 0xE0 (channel id, RSSI, receive timestamp),
same as every other tool here. The channel id is what's graded; RSSI is
reported for free and is informational only.
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

# ant_scan.py owns the ANT+ network key, the frequency, the device-type names
# and the command/acknowledge wrapper. Importing them is the pattern the other
# tools already follow (ant_verify.py imports the same three from it), and it
# is what keeps this tool speaking the same opening sequence as the one whose
# behaviour it is deliberately departing from in exactly one byte.
from ant_scan import (  # noqa: E402
    ANT_PLUS_FREQ,
    ANT_PLUS_KEY,
    DEVICE_TYPES,
    command,
)

# The extended-field decoder, which reads the channel id, RSSI and receive
# timestamp by walking the flag byte rather than at fixed offsets. Not copied:
# reading RSSI at a fixed offset silently reports a device number as a signal
# strength the first time a run is made without the channel id.
from ant_verify import extended_fields  # noqa: E402

# Protocol constants come from the generated module, never from a second copy
# here. See tools/ant_wire.py and protocol/ant_wire.yaml.
from ant_wire import (  # noqa: E402
    CHANNEL_TYPE_SLAVE,
    CHANNEL_TYPE_SLAVE_RX_ONLY,
    EVENT_CHANNEL_CLOSED,
    EVENT_CODES_BY_VALUE,
    EVENT_RX_SEARCH_TIMEOUT,
    EXT_PARAM_ALWAYS_SEARCH,
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
    MESG_EVENT_ID,
    MESG_NETWORK_KEY_ID,
    MESG_OPEN_CHANNEL_ID,
    MESG_RESPONSE_EVENT_ID,
    RESPONSE_CODES_BY_VALUE,
)

CHANNEL = 0

# The assignments to try, best first. #1 is what this tool needs. #2 is the
# Zwift capture in test_api.c (`ASSIGN_CHANNEL 00 40 00 01`) - rx-only rather
# than bidirectional, same search behaviour, tried in case a stock dongle is
# fussier about the pair. #3 is ant_scan.py's plain 3-byte assign - a
# **failure mode, not a fallback**: it locks onto the first device heard, so
# it disqualifies the run (see grade()) but at least yields a diagnosis
# instead of a traceback on a dongle with no extended assignment.
ASSIGNMENTS = (
    (bytes([CHANNEL, CHANNEL_TYPE_SLAVE, 0x00, EXT_PARAM_ALWAYS_SEARCH]),
     "assign channel (slave + background scan)", True),
    (bytes([CHANNEL, CHANNEL_TYPE_SLAVE_RX_ONLY, 0x00, EXT_PARAM_ALWAYS_SEARCH]),
     "assign channel (rx-only slave + background scan)", True),
    (bytes([CHANNEL, CHANNEL_TYPE_SLAVE, 0x00]),
     "assign channel (plain slave, NO background scan)", False),
)


def device_number(text: str) -> int:
    """Parse a device number written the way the experiment writes it.

    `0x60A0` and `24736` are the same device. The frames under test are named
    in hex everywhere they are discussed, and the summary table prints decimal
    because that is what a sensor's case is printed with, so both are accepted
    and the base prefix decides.
    """
    try:
        value = int(text, 0)
    except ValueError:
        raise argparse.ArgumentTypeError(
            f"{text!r} is not a device number. Write it in decimal (24736) or "
            "0x-prefixed hex (0x60A0).") from None
    if not 0 <= value <= 0xFFFF:
        raise argparse.ArgumentTypeError(
            f"device number {value} is out of range; ANT device numbers are "
            "16-bit (0..65535).")
    return value


def assign_background_scan(dev, reader) -> tuple[bool, str]:
    """Assign channel 0 as a never-locking background scan.

    Returns (scanning, description). `scanning` is False if only the plain
    3-byte assign was accepted, meaning the channel tracks one device and the
    run cannot be graded. Rejection prints the response code by name -
    INVALID_MESSAGE vs INVALID_PARAMETER_PROVIDED distinguishes "no extended
    assignment at all" from "dislikes this combination".
    """
    for payload, name, scanning in ASSIGNMENTS:
        dev.write(EP_OUT, frame(MESG_ASSIGN_CHANNEL_ID, payload))

        deadline = time.monotonic() + 2.0
        code = None
        while True:
            result = reader.next_frame(deadline)
            if result is None:
                break
            got_id, body = result
            if got_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3 \
                    and body[1] == MESG_ASSIGN_CHANNEL_ID:
                code = body[2]
                break

        if code == 0:
            print(f"  OK: {name}  [{payload.hex()}]")
            return scanning, name

        if code is None:
            print(f"  FAIL: {name} was not acknowledged  [{payload.hex()}]")
        else:
            print(f"  REJECTED: {name}  [{payload.hex()}] "
                  f"-> {RESPONSE_CODES_BY_VALUE.get(code, f'code {code}')}")

    return False, "nothing"


def summarise(seen: dict, started: float) -> None:
    """Print the per-device table, sorted by device number."""
    print(f"{'device':>8}{'type':>8}{'broadcasts':>12}{'first':>8}{'last':>8}"
          "   rssi(min/mean/max)")
    for (number, dtype), rec in sorted(seen.items()):
        rssi = rec["rssi"]
        # A dongle is free to carry no RSSI field; not a defect.
        signal = (f"{min(rssi)}/{round(sum(rssi) / len(rssi))}/{max(rssi)}"
                  if rssi else "-")
        first_s = f"{rec['first'] - started:.1f}s"
        last_s = f"{rec['last'] - started:.1f}s"
        print(f"{'#' + str(number):>8}{f'0x{dtype:02X}':>8}{rec['count']:>12}"
              f"{first_s:>8}{last_s:>8}   {signal}")


def grade(seen: dict, expect_seen: list, expect_unseen: list,
          scanning: bool) -> int:
    """Print the PASS/FAIL verdict and return the process exit status.

    The order of the checks is the whole point. The control is checked first
    and, if it is missing, the run is declared void before the interesting
    result is even looked at - because "0x60B0 was not heard" is only evidence
    of anything if the rig was demonstrably able to hear something.
    """
    # Grade on the device number alone. A device that answered on an
    # unexpected device type still answered, and an --expect-unseen frame that
    # arrived under a mangled type byte is still a frame that got through.
    heard: dict[int, int] = {}
    for (number, _), rec in seen.items():
        heard[number] = heard.get(number, 0) + rec["count"]

    if not expect_seen and not expect_unseen:
        print("No expectations given (--expect-seen / --expect-unseen), so "
              "nothing was graded.\nThis run is a survey of what is on the "
              "air, not an RF-6 result.")
        return 0

    missing = [num for num in expect_seen if num not in heard]
    if missing:
        print("VERDICT: FAIL - THE RIG IS BROKEN, NO CONCLUSION MAY BE DRAWN.")
        print("  never heard: " +
              ", ".join(f"0x{n:04X} (#{n})" for n in missing))
        print()
        print("  A control frame that was expected and did not arrive means "
              "this run says\n  NOTHING about the length extension. It is not "
              "evidence that the long frame\n  was correctly ignored - a dead "
              "transmitter, the wrong frequency, the wrong\n  network key or a "
              "sick dongle all look exactly like this. Fix the rig and\n  run "
              "it again; do not record this as a pass.")
        return 1

    if not scanning:
        print("VERDICT: FAIL - THE RIG IS BROKEN, NO CONCLUSION MAY BE DRAWN.")
        print("  The dongle refused the extended assignment, so the channel "
              "locked onto the\n  first device it heard instead of scanning. "
              "Anything it did not hear was not\n  listened for.")
        return 1

    forbidden = [num for num in expect_unseen if num in heard]
    if forbidden:
        print("VERDICT: FAIL - THE LENGTH EXTENSION IS NOT INTEROP-SAFE ON 1 M.")
        for num in forbidden:
            print(f"  heard 0x{num:04X} (#{num}) {heard[num]} time(s), and it "
                  "must never be heard at all")
        print()
        print("  A stock dongle decoded the length-extended frame. ADR 0007's "
              "exclusion of\n  ANT+ compatibility channels and of 1 M from the "
              "length extension stands\n  exactly as written; the coded PHY's "
              "physical invisibility remains the only\n  justification this "
              "project has. Record the result in ADR 0007 and close the\n  "
              "open question as answered NO.")
        return 1

    print("VERDICT: PASS - the 1 M length extension is interop-safe here.")
    for num in expect_seen:
        print(f"  control 0x{num:04X} (#{num}) heard {heard[num]} time(s) - "
              "the rig was live")
    for num in expect_unseen:
        print(f"  long frame 0x{num:04X} (#{num}) never heard - dropped "
              "before the host, as predicted")
    others = sorted({n for n, _ in seen}
                    - set(expect_seen) - set(expect_unseen))
    if others:
        print(f"  {len(others)} other device(s) kept being reported "
              "throughout: " +
              ", ".join(f"#{n}" for n in others))
        print("  - nothing else on the air was damaged.")
    else:
        print("  ! no other devices were heard, so the 'does it damage "
              "anything else' half\n    of the question is UNANSWERED. Rerun "
              "with a real ANT+ sensor awake in the\n    room before recording "
              "this as a complete result.")
    print()
    print("  This permits a second, length-extended 1 M format as an ADDITION "
          "to ADR 0007,\n  not a revision of it - and on a weaker "
          "justification (device-type isolation\n  rather than physical "
          "invisibility) that owes its own decision record.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=60.0,
                        help="how long to capture (default: 60)")
    parser.add_argument("--serial",
                        help="match a device whose serial ends with this")
    parser.add_argument(
        "--port",
        help="talk to a UART build over this serial port (e.g. COM8, "
             "/dev/ttyACM1) instead of over USB",
    )
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument(
        "--expect-seen", type=device_number, action="append", default=[],
        metavar="DEVNUM",
        help="a device number that MUST be heard at least once, or the run is "
             "declared broken rather than passed. Repeatable. Decimal or "
             "0x-hex.",
    )
    parser.add_argument(
        "--expect-unseen", type=device_number, action="append", default=[],
        metavar="DEVNUM",
        help="a device number that must be heard ZERO times. Repeatable. "
             "Decimal or 0x-hex.",
    )
    args = parser.parse_args()

    dev = open_device(True, serial=args.serial, port=args.port,
                      baud=args.baud)
    reader = FrameReader(dev)

    print("\nOpening a background-scan ANT+ receive channel")
    # Start from a known state, or a channel another tool left assigned makes
    # the assign below fail with CHANNEL_IN_WRONG_STATE.
    if not reset_stack(dev, reader):
        print("  FAIL: no startup message after reset")
        return 1

    # 0xE0 is all three extended fields: channel id, RSSI, receive timestamp.
    # The channel id is not optional here - it IS the measurement, and without
    # it every broadcast arrives anonymous and no device number can be graded.
    # The other two are free for the asking and keep this tool on the same lib
    # config as every other one, so its RSSI figures are read on the same scale
    # as ant_verify.py's.
    if not command(dev, reader, MESG_ANTLIB_CONFIG_ID,
                   bytes([0x00, LIB_CONFIG_ALL_EXT_FIELDS]),
                   "extended messages"):
        print("  without channel ids nothing can be identified; stopping")
        return 1

    if not command(dev, reader, MESG_NETWORK_KEY_ID,
                   bytes([0]) + ANT_PLUS_KEY, "network key"):
        return 1

    scanning, _ = assign_background_scan(dev, reader)
    if not scanning:
        print("\n  ! THE DONGLE WOULD NOT SCAN. Whatever this run hears, it "
              "will lock onto the\n  ! first device it finds and report only "
              "that one. The capture is not an\n  ! RF-6 result and will be "
              "graded as a broken rig.")

    steps = [
        # Device number 0, type 0, transmission type 0: match anything. The
        # experiment's frames are device type 0x60 and a wildcard is what
        # proves a stock receiver would have heard them if it could.
        (MESG_CHANNEL_ID_ID, bytes([CHANNEL, 0, 0, 0, 0]),
         "wildcard channel id"),
        (MESG_CHANNEL_RADIO_FREQ_ID, bytes([CHANNEL, ANT_PLUS_FREQ]),
         "radio frequency 2457 MHz"),
        # 8070 counts of 32768 Hz, the ANT+ heart rate period. A background
        # scan hears faster and slower profiles regardless; the period is
        # still required before the channel will open.
        (MESG_CHANNEL_MESG_PERIOD_ID, bytes([CHANNEL, 0x86, 0x1F]),
         "message period"),
        # Belt and braces: the always-search bit already means the channel
        # never gives up, and this says the same thing a second way, so a
        # dongle that honoured only one of them still runs the full window.
        (MESG_CHANNEL_SEARCH_TIMEOUT_ID, bytes([CHANNEL, 0xFF]),
         "infinite search timeout"),
        (MESG_OPEN_CHANNEL_ID, bytes([CHANNEL]), "open channel"),
    ]

    for msg_id, payload, name in steps:
        if not command(dev, reader, msg_id, payload, name):
            return 1

    print(f"\nCapturing for {args.seconds:.0f} s")
    for num in args.expect_seen:
        print(f"  expecting to hear    0x{num:04X} (#{num})")
    for num in args.expect_unseen:
        print(f"  expecting silence on 0x{num:04X} (#{num})")

    seen: dict[tuple[int, int], dict] = {}
    events: dict[int, int] = {}
    packets = 0
    anonymous = 0
    started = time.monotonic()
    deadline = started + args.seconds

    try:
        while time.monotonic() < deadline:
            # The 1 s cap is on how long one wait blocks, not on the USB read
            # itself: FrameReader keeps ant_probe.READ_TIMEOUT_MS, which is
            # deliberately not shortened. A read timeout racing the packet it
            # is waiting for has invented a firmware bug in this project twice.
            result = reader.next_frame(min(deadline, time.monotonic() + 1.0))
            if result is None:
                continue

            msg_id, body = result
            if msg_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3 \
                    and body[1] == MESG_EVENT_ID:
                events[body[2]] = events.get(body[2], 0) + 1
                continue

            if msg_id not in (MESG_BROADCAST_DATA_ID, MESG_ACKNOWLEDGED_DATA_ID,
                              MESG_BURST_DATA_ID):
                continue

            packets += 1
            fields = extended_fields(body)
            if "device_number" not in fields:
                anonymous += 1
                continue

            now = time.monotonic()
            key = (fields["device_number"], fields["device_type"])
            rec = seen.get(key)
            if rec is None:
                label = DEVICE_TYPES.get(key[1], f"device type {key[1]}")
                print(f"  found: #{key[0]} (0x{key[0]:04X}) type "
                      f"0x{key[1]:02X} - {label}  at {now - started:.1f}s")
                rec = {"count": 0, "first": now, "last": now, "rssi": []}
                seen[key] = rec
            rec["count"] += 1
            rec["last"] = now
            if "rssi_dbm" in fields:
                rec["rssi"].append(fields["rssi_dbm"])
    except KeyboardInterrupt:
        # Report what was collected rather than losing it. The verdict below
        # still runs, and a short window is exactly how an expected device goes
        # unheard - so an interrupted run grades as a broken rig, which is the
        # honest answer.
        print(f"\n  interrupted after {time.monotonic() - started:.1f}s")

    print("\nClosing")
    command(dev, reader, MESG_CLOSE_CHANNEL_ID, bytes([CHANNEL]),
            "close channel")
    close_device(dev)

    print()
    if events:
        print("channel events: " + ", ".join(
            f"{EVENT_CODES_BY_VALUE.get(code, code)} x{count}"
            for code, count in sorted(events.items())))
        # Either of these on a background scan means the channel stopped
        # listening part way through, which silently shortens the window.
        for code in (EVENT_RX_SEARCH_TIMEOUT, EVENT_CHANNEL_CLOSED):
            if code in events:
                print(f"  ! {EVENT_CODES_BY_VALUE.get(code, code)} on a "
                      "channel that was told never to stop searching - part "
                      "of\n  ! this window was not listening. Treat the "
                      "capture as short.")
    if anonymous:
        print(f"{anonymous} broadcast(s) arrived without a channel id and "
              "could not be attributed.")

    print(f"Heard {packets} broadcast(s) from {len(seen)} device(s) in "
          f"{time.monotonic() - started:.1f}s\n")
    if seen:
        summarise(seen, started)
        print()

    return grade(seen, args.expect_seen, args.expect_unseen, scanning)


if __name__ == "__main__":
    sys.exit(main())
