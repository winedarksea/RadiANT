#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Read MESG_RADIANT_HEALTH (0xF6) - what the dongle threw away.

    python tools/ant_health.py --port COM8
    python tools/ant_health.py --json

Every number here is loss or near-loss that happened INSIDE the dongle or on
the host link, and therefore produced no RX_FAIL for tools/ant_verify.py to
account for. That is the whole point of it: tools/ab_gates.toml's
`unexplained_loss.max = 0` is a *sitting invalidator* rather than a gate,
because a hole in the accumulator with no RX_FAIL beside it could equally be
the host's fault or the dongle's, and loss_accounting() cannot tell those
apart. This is the firmware-side witness that splits that bucket.

So the intended use is BEFORE AND AFTER a measured run, not on its own: a
non-zero delta in `tx_drops` over the run explains missing packets that the
radio never lost, and a zero delta says the loss was real air.

The reply is request-only - the dongle never sends 0xF6 unsolicited - so
running this alongside another tool cannot perturb that tool's stream.

Needs a build with CONFIG_ANT_DONGLE_HEALTH_COUNTERS=y. Without it the dongle
answers INVALID_MESSAGE, which this reports as such rather than as a zero
reading: "the feature is absent" and "nothing has been dropped" are completely
different answers and must never be printed the same way.

ON A UART BUILD, tx_drops, tx_high_water and rx_backpressure are structurally
zero and mean nothing. All three are counted in apps/common/usb_ant_class.c,
which a UART image does not compile at all - the transport choice decides. They
are the fields that matter most on the boards people actually flash (the
Feather, the Dongle) and the fields that cannot say anything on the nRF54L15
DK, which has no USB peripheral in silicon. Read them on the target they
describe.
"""

from __future__ import annotations

import argparse
import json
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

from ant_probe import (  # noqa: E402
    EP_OUT,
    FrameReader,
    close_device,
    expect,
    frame,
    open_device,
)
from ant_wire import (  # noqa: E402
    MESG_RADIANT_HEALTH_ID,
    MESG_REQUEST_ID,
)

# Byte offsets and widths, from protocol/ant_wire.yaml's 0xF6 entry. Kept as a
# table rather than as inline slicing so that adding a field is one row and so
# that the names here are greppable against the YAML that defines them.
#
# (name, offset, width, description, min_version)
#
# min_version is the structure version (payload[0]) that first carries this
# field. [22..23] were reserved-as-zero under version 1 and were given to
# host_desync under version 2 - a version-1 body still decodes cleanly
# because decode() below stops reading fields once the body runs out, rather
# than assuming every field it knows about is present.
FIELDS = (
    ("tx_drops", 2, 2, "ANT events dropped: USB TX queue full", 1),
    ("tx_high_water", 4, 1, "deepest the USB TX queue has been (of 32)", 1),
    ("event_high_water", 5, 1, "deepest the radio event queue has been", 1),
    ("event_overflows", 6, 2, "event-queue overflow marks", 1),
    ("rx_backpressure", 8, 2, "times bulk-OUT was left unarmed", 1),
    ("key_rejects", 10, 2, "frames refused for an unusable network key", 1),
    ("sched_missed", 12, 2, "scheduler windows missed", 1),
    ("sched_failed", 14, 2, "scheduler windows failed", 1),
    ("sched_denied", 16, 2, "scheduler windows denied by the arbiter", 1),
    ("burst_stalls", 18, 2, "burst transfers that hit the 1000 ms stall", 1),
    ("sweep_busy", 20, 2, "search posts declined: searching channel busy", 1),
    ("host_desync", 22, 2,
     "bytes discarded by the host parser hunting for SYNC", 2),
    ("host_csum", 24, 2,
     "framed messages that failed the trailing XOR checksum", 2),
    ("rx_not_owner", 26, 2,
     "radio RX events that arrived after their channel op moved on", 2),
    ("tx_timeout", 28, 2,
     "USB bulk-IN transfers that timed out waiting on the host", 2),
)

# The version-1 body length. Kept as its own name (rather than a bare 24)
# because it is a floor, not a fixed size: PAYLOAD_LEN below is what a
# current build sends, and this is the oldest shape this tool still accepts.
PAYLOAD_LEN_V1 = 24

# What ant_health_fill() writes today - apps/common/ant_health.h's
# ANT_HEALTH_PAYLOAD_LEN, which is the definition this mirrors.
PAYLOAD_LEN = 30


def decode(payload: bytes) -> dict:
    """Turn a 0xF6 body into a dict. Raises ValueError on a short body.

    Version-aware: a v1 (24-byte) body from an older stick decodes with only
    the fields v1 carries. A v2+ body decodes every field its length covers,
    even against a future version this tool has not been taught the new
    fields of - it just won't report them.
    """
    if len(payload) < PAYLOAD_LEN_V1:
        raise ValueError(
            "0xF6 body is %d bytes, expected at least %d"
            % (len(payload), PAYLOAD_LEN_V1)
        )

    version = payload[0]
    out = {
        "version": version,
        # Bit 0: at least one counter has saturated, so EVERY number below is a
        # floor rather than a total. Reported as its own key because a reader
        # that ignores it can silently treat 65535 as an exact count.
        "saturated": bool(payload[1] & 0x01),
    }
    for name, off, width, _desc, min_version in FIELDS:
        if version < min_version or off + width > len(payload):
            continue
        out[name] = int.from_bytes(payload[off:off + width], "little")
    return out


def request(dev, reader) -> bytes | None:
    """Send MESG_REQUEST for 0xF6 and return the body, or None if refused."""
    # [0] channel, [1] requested id. The channel byte is ignored by the dongle
    # - everything counted is device-wide - but the request message's shape
    # requires it.
    dev.write(EP_OUT, frame(MESG_REQUEST_ID, bytes([0x00, MESG_RADIANT_HEALTH_ID])))

    # expect() matches on the id rather than taking the first frame back, which
    # matters here: with sensors on the air a channel EVENT arrives constantly,
    # and taking the first frame is how tools in this directory have reported a
    # success as a failure before.
    #
    # A refusal (the feature not compiled in) arrives as a RESPONSE carrying
    # INVALID_MESSAGE, which is not 0xF6, so this times out and returns None -
    # correctly, since there is no reading to report.
    return expect(reader, MESG_RADIANT_HEALTH_ID, 2.0)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial",
                        help="match a device whose serial ends with this")
    parser.add_argument(
        "--port",
        help="talk to a UART build over this serial port (e.g. COM8) instead "
             "of over USB",
    )
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--json", action="store_true",
                        help="emit the decoded reply as JSON and nothing else")
    args = parser.parse_args()

    dev = open_device(True, serial=args.serial, port=args.port, baud=args.baud)
    reader = FrameReader(dev)

    # Deliberately NO reset_stack() here. A reset zeroes every counter this
    # tool exists to read, which would make it always report a healthy dongle.
    # It also drops any channel another tool has open, so this can be run
    # against a live session - which is the intended use.
    try:
        body = request(dev, reader)
    finally:
        close_device(dev)

    if body is None:
        print(
            "no 0xF6 reply.\n"
            "  Most likely the firmware was built without\n"
            "  CONFIG_ANT_DONGLE_HEALTH_COUNTERS=y - which is OFF by\n"
            "  default, so this is the common case, not a misconfiguration.\n"
            "  That is NOT the same as 'nothing was dropped' - it means the\n"
            "  question could not be asked.",
            file=sys.stderr,
        )
        return 2

    try:
        got = decode(body)
    except ValueError as exc:
        print("malformed 0xF6 reply: %s" % exc, file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(got, indent=2, sort_keys=True))
        return 0

    print("MESG_RADIANT_HEALTH (0xF6), structure version %d" % got["version"])
    if got["saturated"]:
        print("  SATURATED: at least one counter has pinned at its maximum.")
        print("  Every number below is a FLOOR, not a total.")
    print()
    width = max(len(name) for name, _o, _w, _d, _mv in FIELDS)
    for name, _off, _w, desc, min_version in FIELDS:
        if name not in got:
            print("  %-*s %8s   %s (needs v%d+)"
                  % (width, name, "-", desc, min_version))
            continue
        print("  %-*s %8d   %s" % (width, name, got[name], desc))
    print()
    if got["tx_drops"]:
        print("  tx_drops is non-zero: those ANT events never reached the")
        print("  host and produced no RX_FAIL, so they are invisible to")
        print("  ant_verify.py's loss accounting. This is the number that")
        print("  invalidates a sitting.")
        print("  It is NOT a request to deepen the queue - see")
        print("  ANT_TX_QUEUE_DEPTH in apps/common/usb_ant_class.c.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
