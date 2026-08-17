#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Drive an ANT+ FE-C machine's CONTROL surface, as a head unit would.

Every other tool here observes. This one *commands*: it opens a slave channel
pinned to one fitness-equipment master, sends an acknowledged control page into
it, and reads the page 71 Command Status that FE-C SS8.8 obliges the machine to
answer with.

    python tools/ant_fec_control.py --device 4243 --grade 3.0
    python tools/ant_fec_control.py --device 52233 --request-page 54

WHY IT IS PINNED AND NOT A WILDCARD, and this is not a preference. A wildcard
slave latches onto the FIRST sensor it hears and then reports only that one; on
this bench it locked to a commercial trainer and reported the treadmill under
test as absent, twice, while that board was transmitting perfectly. --device is
mandatory for exactly that reason - see docs/treadmill-reference-design.md
SS7.4.

THE RETRY IS HERE AND DELIBERATELY NOT IN THE FIRMWARE. radiant_burst.c fails an
unacknowledged transfer once and does not retry, which is an evidence-based
decision recorded in that file: inventing a sender-side retry would put an
unmeasured frame on the air at an unmeasured instant. A controller retrying its
own command on a later slot is a different thing entirely - it is what a real
head unit does - and it belongs in the controller. --attempts bounds it.

WHAT A FAILURE HERE MEANS. An acknowledged transfer from a slave has to land in
the master's reply window, ~2190 us after the master's own transmission and only
+-250 us wide. If every attempt returns TRANSFER_TX_FAILED with no page 71, the
question is whether the transmit was SLOT-ALIGNED at all - which was a real
defect in apps/common/ant_radio_radiant.c until 2026-08-17, and which showed up
identically against a commercial trainer. Run --request-page 70 against a
known-good commercial machine first: it is read-only, it changes nothing, and it
separates "this dongle cannot originate" from "that machine refused".
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

from ant_wire import (  # noqa: E402
    EVENT_RX_FAIL,
    EVENT_RX_SEARCH_TIMEOUT,
    EVENT_TRANSFER_TX_COMPLETED,
    EVENT_TRANSFER_TX_FAILED,
    LIB_CONFIG_ALL_EXT_FIELDS,
    MESG_ACKNOWLEDGED_DATA_ID,
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
)

ANT_PLUS_KEY = bytes([0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45])
ANT_PLUS_FREQ = 57
CHANNEL = 0

FEC_DEVICE_TYPE = 17          # 0x11
FEC_PERIOD = 8192             # 4.00 Hz. NOT 8134, which is SDM (0x7C).
FEC_TRANS_TYPE = 5

PAGE_GENERAL = 16
PAGE_SETTINGS = 17
PAGE_TRACK_RESISTANCE = 0x33  # 51
PAGE_REQUEST = 0x46           # 70
PAGE_CMD_STATUS = 0x47        # 71

# SS8.8's page 71 status byte. REJECTED (3) is what a machine answers when the
# command is understood and refused - which is what the treadmill's
# cross-transport ownership token uses when a BLE client holds control.
CMD_STATUS = {
    0: "PASS",
    1: "FAIL",
    2: "NOT_SUPPORTED",
    3: "REJECTED",
    4: "PENDING",
    0xFF: "UNINITIALISED",
}


def command(dev, reader, msg_id: int, payload: bytes, name: str,
            timeout: float = 2.0) -> bool:
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


def page51(grade_pct: float) -> bytes:
    """SS8.8.4.1 Track Resistance.

    Byte 0 page, 1..4 reserved 0xFF, 5..6 grade as a uint16 in 0.01 % with a
    -200.00 % offset, 7 coefficient of rolling resistance.

    THE OFFSET IS THE THING TO GET RIGHT. The field is unsigned and zero grade
    is 0x4E20 (20000), not 0. Sending a signed value straight would command
    -200 % and the machine would answer PASS to it.
    """
    raw = int(round(grade_pct * 100.0)) + 20000
    raw = max(0, min(0xFFFE, raw))
    return bytes([PAGE_TRACK_RESISTANCE, 0xFF, 0xFF, 0xFF, 0xFF,
                  raw & 0xFF, (raw >> 8) & 0xFF, 0xFF])


def page70(requested: int, count: int = 4) -> bytes:
    """SS7.2 Request Data Page - READ-ONLY, which is what makes it the right
    control probe against somebody else's machine."""
    return bytes([PAGE_REQUEST, 0xFF, 0xFF, 0xFF, 0xFF,
                  (count & 0x7F) << 1 | 0x00, requested & 0xFF, 0x01])


def decode_grade(body: bytes) -> float | None:
    """Page 17's incline, 0.01 % with a -200.00 % offset; 0x7FFF is 'invalid'."""
    raw = body[3] | (body[4] << 8)
    if raw == 0x7FFF:
        return None
    return (raw - 20000) / 100.0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--device", type=int, required=True,
                        help="ANT device number to pin to. MANDATORY - a "
                             "wildcard slave latches onto the first sensor it "
                             "hears and has reported this bench's own node "
                             "absent twice")
    parser.add_argument("--grade", type=float,
                        help="send page 51 Track Resistance with this grade, "
                             "in percent (e.g. 3.0)")
    parser.add_argument("--request-page", type=int,
                        help="send page 70 Request Data Page for this page "
                             "number instead. READ-ONLY: the right probe for "
                             "'can this dongle originate at all' against a "
                             "machine you do not own")
    parser.add_argument("--attempts", type=int, default=3,
                        help="bounded retries on successive periods (default: "
                             "3). The retry is here and not in radiant_burst.c "
                             "on purpose - see the module docstring")
    parser.add_argument("--acquire-seconds", type=float, default=15.0,
                        help="how long to wait for the channel to track "
                             "before commanding (default: 15)")
    parser.add_argument("--watch-seconds", type=float, default=10.0,
                        help="how long to watch for page 71 and page 17 after "
                             "the command (default: 10)")
    parser.add_argument("--serial",
                        help="match a dongle whose serial ends with this")
    parser.add_argument("--port", help="UART build on this serial port")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    if (args.grade is None) == (args.request_page is None):
        parser.error("pass exactly one of --grade or --request-page")

    if args.grade is not None:
        payload = page51(args.grade)
        what = f"page 51 Track Resistance, grade {args.grade:+.2f} %"
    else:
        payload = page70(args.request_page)
        what = f"page 70 Request Data Page for page {args.request_page}"

    dev = open_device(True, serial=args.serial, port=args.port, baud=args.baud)
    reader = FrameReader(dev)

    print(f"\nOpening a PINNED FE-C slave channel to #{args.device}")
    if not reset_stack(dev, reader):
        print("  FAIL: no startup message after reset")
        return 1

    command(dev, reader, MESG_ANTLIB_CONFIG_ID,
            bytes([0x00, LIB_CONFIG_ALL_EXT_FIELDS]), "extended messages")

    steps = [
        (MESG_NETWORK_KEY_ID, bytes([0]) + ANT_PLUS_KEY, "network key"),
        # 0x00 is a bidirectional SLAVE. It has to be bidirectional: a
        # receive-only slave (0x40) has no transmitter, so an acknowledged
        # message would be refused with CHANNEL_IN_WRONG_STATE and the whole
        # point of this tool would be unreachable.
        (MESG_ASSIGN_CHANNEL_ID, bytes([CHANNEL, 0x00, 0x00]),
         "assign channel (bidirectional slave)"),
        (MESG_CHANNEL_ID_ID,
         bytes([CHANNEL, args.device & 0xFF, (args.device >> 8) & 0xFF,
                FEC_DEVICE_TYPE, FEC_TRANS_TYPE]),
         f"channel id #{args.device} type {FEC_DEVICE_TYPE}"),
        (MESG_CHANNEL_RADIO_FREQ_ID, bytes([CHANNEL, ANT_PLUS_FREQ]),
         "radio frequency 2457 MHz"),
        # 8192 and NOT 8134. Eight counts apart in a way that reads as a typo:
        # 8192 is FE-C's 4.00 Hz, 8134 is SDM's ~4.03 Hz. A slave told the
        # wrong one never opens the channel and nothing names the period as
        # the reason.
        (MESG_CHANNEL_MESG_PERIOD_ID,
         bytes([CHANNEL, FEC_PERIOD & 0xFF, (FEC_PERIOD >> 8) & 0xFF]),
         f"message period {FEC_PERIOD}"),
        (MESG_CHANNEL_SEARCH_TIMEOUT_ID, bytes([CHANNEL, 0xFF]),
         "infinite search timeout"),
        (MESG_OPEN_CHANNEL_ID, bytes([CHANNEL]), "open channel"),
    ]
    for msg_id, payload_, name in steps:
        if not command(dev, reader, msg_id, payload_, name):
            return 1

    # ── Acquire ─────────────────────────────────────────────────────────────
    #
    # COMMANDING BEFORE THE CHANNEL TRACKS IS THE CLASSIC FALSE NEGATIVE. A
    # slave that has not synchronised has no slot to reply in, so the transfer
    # fails for a reason that has nothing to do with the machine's control
    # surface - and looks identical to one that does.
    print(f"\nWaiting up to {args.acquire_seconds:.0f} s to track #{args.device}")
    heard = 0
    deadline = time.monotonic() + args.acquire_seconds
    while time.monotonic() < deadline and heard < 4:
        result = reader.next_frame(min(deadline, time.monotonic() + 1.0))
        if result is None:
            continue
        msg_id, body = result
        if msg_id == MESG_BROADCAST_DATA_ID and len(body) >= 9:
            heard += 1
            if heard == 1:
                print(f"  tracking: first page {body[1]}")
    if heard < 4:
        print(f"  FAIL: only {heard} broadcasts in {args.acquire_seconds:.0f} s "
              f"- not tracking, so a control result would mean nothing")
        command(dev, reader, MESG_CLOSE_CHANNEL_ID, bytes([CHANNEL]), "close")
        close_device(dev)
        return 1
    print(f"  OK: tracking ({heard} broadcasts)")

    # ── Command ─────────────────────────────────────────────────────────────
    print(f"\nSending {what}")
    print(f"  bytes: {payload.hex()}")
    delivered = False
    for attempt in range(1, args.attempts + 1):
        dev.write(EP_OUT, frame(MESG_ACKNOWLEDGED_DATA_ID,
                                bytes([CHANNEL]) + payload))
        # One period is 250 ms; two of them is a generous bound on when the
        # stack must have reported a terminal event for this transfer.
        end = time.monotonic() + 2.0
        outcome = None
        while time.monotonic() < end:
            result = reader.next_frame(min(end, time.monotonic() + 0.25))
            if result is None:
                continue
            msg_id, body = result
            if msg_id != MESG_RESPONSE_EVENT_ID or len(body) < 3:
                continue
            # body[1] IS MESG_EVENT_ID (0x01) FOR AN EVENT, NOT 0. This is the
            # single most expensive line in the file to get wrong, and it was
            # wrong once: with `== 0` every terminal event is discarded, the
            # tool reports "no terminal event", and that reads as a radio or
            # scheduling fault rather than as a parsing bug. It cost a whole
            # A/B arm before the raw frame dump showed `resp msg=0x01 code=6`
            # going past unread. tools/ant_verify.py has always had it right;
            # copy from there, not from memory.
            #
            # Anything else in body[1] is a RESPONSE to a specific message. A
            # stack that refuses the acknowledged message outright answers 0x4F
            # with a code and then raises no terminal event at all, because
            # there is no transfer - which is a third outcome worth telling
            # apart from both success and failure.
            if body[1] == MESG_ACKNOWLEDGED_DATA_ID:
                if body[2] != 0:
                    outcome = (f"REFUSED by the stack, code {body[2]} "
                               f"- no transfer was ever started")
                    break
                continue
            if body[1] != MESG_EVENT_ID:
                continue
            code = body[2]
            if code == EVENT_TRANSFER_TX_COMPLETED:
                outcome = "TRANSFER_TX_COMPLETED"
                break
            if code == EVENT_TRANSFER_TX_FAILED:
                outcome = "TRANSFER_TX_FAILED"
                break
            if code in (EVENT_RX_FAIL, EVENT_RX_SEARCH_TIMEOUT):
                continue
        print(f"  attempt {attempt}/{args.attempts}: {outcome or 'no terminal event'}")
        if outcome == "TRANSFER_TX_COMPLETED":
            delivered = True
            break

    if not delivered:
        print("\nFAIL: the command was never acknowledged on air.\n"
              "  A slave's acknowledged transfer has to land in the master's\n"
              "  reply window - ~2190 us after its transmission, +-250 us wide.\n"
              "  If a read-only --request-page 70 against a KNOWN-GOOD commercial\n"
              "  machine also fails, the originator is not slot-aligned and the\n"
              "  machine under test is not implicated at all.")

    # ── Read the answer ─────────────────────────────────────────────────────
    #
    # WATCHED EVEN AFTER A FAILED TRANSFER, on purpose: a machine that received
    # the command but whose acknowledgement was lost still answers page 71, and
    # that distinction is invisible from the transfer result alone.
    print(f"\nWatching {args.watch_seconds:.0f} s for page 71 and page 17")
    status_seen = False
    deadline = time.monotonic() + args.watch_seconds
    last_grade = "unset"
    while time.monotonic() < deadline:
        result = reader.next_frame(min(deadline, time.monotonic() + 1.0))
        if result is None:
            continue
        msg_id, body = result
        if msg_id != MESG_BROADCAST_DATA_ID or len(body) < 9:
            continue
        page = body[1]
        if page == PAGE_CMD_STATUS and not status_seen:
            status_seen = True
            last_cmd, seq, status = body[2], body[3], body[4]
            echo = bytes(body[5:9])
            name = CMD_STATUS.get(status, f"unknown {status}")
            print(f"  page 71: last command {last_cmd}, sequence {seq}, "
                  f"status {status} ({name})")
            print(f"           echo bytes 5-7 of the command: {echo.hex()}")
            if last_cmd == 0xFF:
                print("           (0xFF = no command has been received since "
                      "power-on - this machine never saw it)")
        elif page == PAGE_SETTINGS:
            grade = decode_grade(bytes(body[1:9]))
            shown = "invalid" if grade is None else f"{grade:+.2f} %"
            if shown != last_grade:
                print(f"  page 17: incline now {shown}")
                last_grade = shown

    if not status_seen:
        print("  no page 71 seen. SS8.8 makes it an obligation, so either the "
              "command never arrived or the machine is not answering.")

    print("\nClosing")
    command(dev, reader, MESG_CLOSE_CHANNEL_ID, bytes([CHANNEL]), "close channel")
    close_device(dev)
    return 0 if (delivered and status_seen) else 1


if __name__ == "__main__":
    sys.exit(main())
