#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Drive one board through broadcast, acknowledged data and a burst, for Spike B.

Clean-room: built from this repo's own tools/ant_probe.py (transport) and
tools/ant_scan.py / tools/ant_sim.py (channel bring-up sequence), and the
free ANT Message Protocol and Usage Rev 5.1 for message meanings. Nothing
derives from sdk-ant or an ANT+ device profile document.

Spike A asked whether a bare nRF RADIO can hear an ANT broadcast; Spike B
asks where the *packet type* lives, which needs something other than a
broadcast on the air. This script puts it there; radiant_core/spike/promisc
does the listening, on a third board.

  --role master   Transmit broadcast, then acknowledged data, then a burst.
                  Needs no second radio - a master sends acknowledged/burst
                  packets regardless of a listener and just reports
                  TRANSFER_TX_FAILED, so a two-board rig suffices.
  --role slave    Track a master and send acknowledged data / a burst back
                  to it - the direction Zwift uses for trainer resistance,
                  and the only one producing a slave-to-master turnaround.

Every payload is marked in byte 0 (0xB0 broadcast, 0xA0 acknowledged, 0xC0
burst) with a sequence number in byte 1, so a captured frame traces back to
a specific sent message without guessing.

    py spike_b_drive.py --role master --serial 6183
    py spike_b_drive.py --role slave  --serial 6183 --device-number 14871
"""

from __future__ import annotations

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "..", "tools"))

from ant_probe import (  # noqa: E402
    EP_OUT,
    FrameReader,
    close_device,
    frame,
    open_device,
    reset_stack,
)
from ant_scan import ANT_PLUS_FREQ, ANT_PLUS_KEY, command  # noqa: E402
from ant_wire import (  # noqa: E402
    ADV_BURST_MODE_ENABLE,
    ADV_BURST_MODES_SIZE_24_BYTES,
    BURST_HEADER_LAST,
    BURST_HEADER_SEQ_SHIFT,
    EVENT_CODES_BY_VALUE,
    EVENT_TRANSFER_TX_COMPLETED,
    EVENT_TRANSFER_TX_FAILED,
    MESG_ACKNOWLEDGED_DATA_ID,
    MESG_ADV_BURST_DATA_ID,
    MESG_ASSIGN_CHANNEL_ID,
    MESG_BROADCAST_DATA_ID,
    MESG_BURST_DATA_ID,
    MESG_CHANNEL_ID_ID,
    MESG_CHANNEL_MESG_PERIOD_ID,
    MESG_CHANNEL_RADIO_FREQ_ID,
    MESG_CHANNEL_SEARCH_TIMEOUT_ID,
    MESG_CLOSE_CHANNEL_ID,
    MESG_CONFIG_ADV_BURST_ID,
    MESG_EVENT_ID,
    MESG_NETWORK_KEY_ID,
    MESG_OPEN_CHANNEL_ID,
    MESG_RESPONSE_EVENT_ID,
    MESG_UNASSIGN_CHANNEL_ID,
)

CHANNEL_TYPE_SLAVE = 0x00
CHANNEL_TYPE_MASTER = 0x10
EVENT_TX = 0x03
EVENT_CHANNEL_CLOSED = 0x07

MARK_BROADCAST = 0xB0
MARK_ACK = 0xA0
MARK_BURST = 0xC0

T0 = time.monotonic()


def stamp() -> str:
    """Host clock, seconds since this script started.

    Deliberately not claimed to be commensurate with the capture board's
    microsecond clock: the two are free-running and USB adds milliseconds of
    jitter. These timestamps order host *actions*; the capture orders frames.
    """
    return f"{time.monotonic() - T0:9.3f}"


def note(text: str) -> None:
    print(f"[{stamp()}] {text}", flush=True)


def payload(mark: int, seq: int, size: int = 8) -> bytes:
    """A marked, self-describing block.

    Byte 0 says which host message produced it and byte 1 which one of them, so
    a captured frame can be traced back to a line of this script's output with
    no timing argument at all. The filler is a fixed pattern rather than zeroes
    because a run of zeroes is what a truncated or mis-framed capture also looks
    like.
    """
    body = bytes([mark, seq & 0xFF, 0x5A, 0xA5, 0x00, 0x11, 0x22, 0x33])
    while len(body) < size:
        body += bytes([mark ^ 0xFF, len(body), 0x5A, 0xA5,
                       0x00, 0x11, 0x22, 0x33])[:size - len(body)]
    return body[:size]


def drain(reader: FrameReader, seconds: float, counters: dict) -> None:
    """Read and tally events for `seconds`, printing the interesting ones."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = reader.next_frame(min(deadline, time.monotonic() + 0.25))
        if result is None:
            continue
        tally(result, counters)


def tally(result, counters: dict) -> tuple[int, int] | None:
    msg_id, body = result
    if msg_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3 and body[1] == MESG_EVENT_ID:
        counters[body[2]] = counters.get(body[2], 0) + 1
        return (body[0], body[2])
    return None


def wait_event(reader: FrameReader, wanted: set, timeout_s: float,
               counters: dict) -> int | None:
    deadline = time.monotonic() + timeout_s
    while True:
        result = reader.next_frame(deadline)
        if result is None:
            return None
        got = tally(result, counters)
        if got is not None and got[1] in wanted:
            return got[1]


def enable_adv_burst(dev, reader, size_code: int = ADV_BURST_MODES_SIZE_24_BYTES) -> bool:
    """Turn on advanced burst so a single packet can carry 24 payload bytes.

    A payload other than 8 bytes is the only way to give bits 2:0 of the
    control byte a measured meaning. It originally tested a 0x1A/0x9A length
    prediction; both are withdrawn (byte 3 is a control byte - see
    docs/spike-b-part2-results.md), but the run is still worth doing.

    Body layout: [filler, enable, rf payload size, required modes, 0, 0,
    optional modes, 0, 0] - see protocol/ant_wire.yaml, MESG_CONFIG_ADV_BURST.
    """
    body = bytes([0x00, ADV_BURST_MODE_ENABLE, size_code,
                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00])
    return command(dev, reader, MESG_CONFIG_ADV_BURST_ID, body,
                   "enable advanced burst (24-byte packets)")


def burst_seq(index: int) -> int:
    """The host-side burst sequence number for the index'th block.

    Two bits on the wire (`ANTW_BURST_HEADER_SEQ_MASK` is 3), and it does
    *not* count modulo 4: sequence 0 marks only the first packet of a
    transfer, so a long burst runs 0, 1, 2, 3, 1, 2, 3, ... (wrap skips 0).
    `src/ant_serial_bridge.c` derives ANTR_BURST_SEGMENT_START from
    `seq == 0`, so a naive `index % 4` would tell the stack blocks 4/8/12
    each start a new transfer mid-burst. Whether the on-air field is the
    same width is what this run is for.
    """
    return 0 if index == 0 else ((index - 1) % 3) + 1


def send_burst(dev, reader, ch: int, blocks: int, counters: dict,
               block_size: int = 8, pace: float = 0.0) -> str:
    """Stream one burst of `blocks` packets of `block_size` bytes.

    MESG_BURST_DATA packets carry the channel in header bits 0-4, a two-bit
    sequence number in bits 5-6, and the last-packet flag in bit 7.

    **Every packet is written back to back, no wait between them** - a fix,
    not an optimisation. Waiting for EVENT_TRANSFER_NEXT_DATA_BLOCK after
    each block (the event `src/ant_radio.h` rules B1-B5 turn on) never
    worked: the bridge consumes that event internally and never puts it on
    the wire (`src/ant_serial_bridge.c` returns right after `k_sem_give()`,
    since a real ANT stick never exposes its internal flow control). Waiting
    for it starved every multi-block burst after packet 0
    (EVENT_TRANSFER_TX_FAILED). Back-pressure still works when streaming:
    the bridge blocks in `k_sem_take(&burst_block_free, K_MSEC(1000))` before
    each block, throttling the host exactly as a real stick would.

    `pace` inserts a fixed delay between packets, only to answer "was it a
    race?" if a burst fails again - it starves the transfer above zero, so
    it's a diagnostic, not a setting.

    `blocks == 1` is a separate experiment, not a degenerate case: a
    one-packet burst carries sequence 0 *with* the last-packet flag, where a
    longer burst's first packet carries it *without* - isolating where the
    last-packet flag lives on the air.
    """
    msg_id = MESG_ADV_BURST_DATA_ID if block_size > 8 else MESG_BURST_DATA_ID
    note(f"burst: {blocks} blocks x {block_size} bytes on channel {ch} "
         f"(msg 0x{msg_id:02X})")
    t_first = time.monotonic()
    for i in range(blocks):
        last = (i == blocks - 1)
        header = (ch & 0x1F) | (burst_seq(i) << BURST_HEADER_SEQ_SHIFT)
        if last:
            header |= BURST_HEADER_LAST
        dev.write(EP_OUT,
                  frame(msg_id,
                        bytes([header]) + payload(MARK_BURST, i, block_size)))
        if pace > 0.0 and not last:
            time.sleep(pace)
    note(f"  {blocks} blocks written in "
         f"{1e3 * (time.monotonic() - t_first):.1f} ms")

    got = wait_event(reader,
                     {EVENT_TRANSFER_TX_COMPLETED, EVENT_TRANSFER_TX_FAILED},
                     10.0, counters)
    verdict = EVENT_CODES_BY_VALUE.get(got, "no outcome event")
    note(f"  burst outcome: {verdict}")
    return verdict


def send_ack(dev, reader, ch: int, seq: int, counters: dict) -> str:
    dev.write(EP_OUT, frame(MESG_ACKNOWLEDGED_DATA_ID,
                            bytes([ch]) + payload(MARK_ACK, seq)))
    note(f"ack #{seq} queued (payload marker {MARK_ACK:02X} {seq:02X})")
    got = wait_event(reader,
                     {EVENT_TRANSFER_TX_COMPLETED, EVENT_TRANSFER_TX_FAILED},
                     8.0, counters)
    verdict = EVENT_CODES_BY_VALUE.get(got, "no outcome event")
    note(f"  ack #{seq} outcome: {verdict}")
    return verdict


def run_burst_plan(dev, reader, args, counters: dict) -> None:
    """Send every burst in --burst-plan, waiting between them.

    The plan is a list of block counts because the interesting comparisons are
    between whole bursts, not within one: 1 block versus 12 blocks isolates the
    last-packet flag, and 8-byte versus 24-byte blocks isolates payload length.
    """
    for blocks in args.burst_plan:
        send_burst(dev, reader, args.channel, blocks, counters, 8,
                   args.burst_pace)
        drain(reader, args.ack_gap, counters)

    if args.adv_burst:
        if enable_adv_burst(dev, reader):
            for blocks in args.burst_plan:
                send_burst(dev, reader, args.channel, blocks, counters, 24,
                           args.burst_pace)
                drain(reader, args.ack_gap, counters)
        else:
            note("advanced burst could not be enabled; 24-byte frames skipped")


def bring_up(dev, reader, args, chan_type: int) -> bool:
    steps = [
        (MESG_ASSIGN_CHANNEL_ID, bytes([args.channel, chan_type, 0]), "assign"),
        (MESG_CHANNEL_ID_ID,
         bytes([args.channel,
                args.device_number & 0xFF, (args.device_number >> 8) & 0xFF,
                args.device_type, args.trans_type]), "channel id"),
        (MESG_CHANNEL_RADIO_FREQ_ID, bytes([args.channel, ANT_PLUS_FREQ]),
         "radio frequency"),
        (MESG_CHANNEL_MESG_PERIOD_ID,
         bytes([args.channel, args.period & 0xFF, args.period >> 8]), "period"),
    ]
    if chan_type == CHANNEL_TYPE_SLAVE:
        steps.append((MESG_CHANNEL_SEARCH_TIMEOUT_ID,
                      bytes([args.channel, 0xFF]), "search timeout"))
    return all(command(dev, reader, msg_id, body, what) for msg_id, body, what in steps)


def run_master(dev, reader, args) -> int:
    counters: dict = {}

    if not bring_up(dev, reader, args, CHANNEL_TYPE_MASTER):
        return 1

    # A master starts transmitting the instant it opens, and whatever is in the
    # buffer at that moment goes on the air. Load a marked broadcast first so
    # the very first frame in the capture is identifiable.
    dev.write(EP_OUT, frame(MESG_BROADCAST_DATA_ID,
                            bytes([args.channel]) + payload(MARK_BROADCAST, 0)))

    # Open-time. A master that probes for a collision before its first
    # transmission would show up as a gap between OPEN_CHANNEL and the first
    # EVENT_TX. This bound is host-side and therefore carries the USB round
    # trip - it can say "no probe longer than a few milliseconds", and cannot
    # say anything finer. Saying which is the point of measuring it at all.
    t_open = time.monotonic()
    note("OPEN_CHANNEL sent - the first frame on air follows this line")
    if not command(dev, reader, MESG_OPEN_CHANNEL_ID, bytes([args.channel]),
                   "open"):
        return 1
    t_ack = time.monotonic()
    first_tx = wait_event(reader, {EVENT_TX}, 5.0, counters)
    t_tx = time.monotonic()
    if first_tx is None:
        note("no EVENT_TX within 5 s of opening - the master is not transmitting")
    else:
        note(f"open-time bound: OPEN->response {1e3 * (t_ack - t_open):.1f} ms, "
             f"OPEN->first EVENT_TX {1e3 * (t_tx - t_open):.1f} ms "
             "(includes the USB round trip both ways)")
        dev.write(EP_OUT,
                  frame(MESG_BROADCAST_DATA_ID,
                        bytes([args.channel]) + payload(MARK_BROADCAST, 1)))

    # Phase 1 - plain broadcast, paced by EVENT_TX exactly as ant_sim.py does.
    note(f"phase 1: broadcast for {args.broadcast_secs:.0f} s")
    seq = 2
    deadline = time.monotonic() + args.broadcast_secs
    while time.monotonic() < deadline:
        result = reader.next_frame(min(deadline, time.monotonic() + 0.5))
        if result is None:
            continue
        got = tally(result, counters)
        if got is not None and got[1] == EVENT_TX:
            dev.write(EP_OUT,
                      frame(MESG_BROADCAST_DATA_ID,
                            bytes([args.channel]) + payload(MARK_BROADCAST, seq)))
            seq = (seq + 1) & 0xFF

    # Phase 2 - acknowledged data. Spaced out so each one is separable in the
    # capture even if the stack retries it.
    note(f"phase 2: {args.acks} acknowledged messages")
    for i in range(args.acks):
        send_ack(dev, reader, args.channel, i, counters)
        drain(reader, args.ack_gap, counters)

    # Phase 3 - the bursts.
    note("phase 3: bursts")
    run_burst_plan(dev, reader, args, counters)

    # Phase 4 - back to broadcast, so the capture has a clean "after" as well
    # as a clean "before". A control byte that only ever differs during the
    # middle of the run is a much stronger result than one that differs after
    # some irreversible state change.
    note(f"phase 4: broadcast for {args.tail_secs:.0f} s")
    deadline = time.monotonic() + args.tail_secs
    while time.monotonic() < deadline:
        result = reader.next_frame(min(deadline, time.monotonic() + 0.5))
        if result is None:
            continue
        got = tally(result, counters)
        if got is not None and got[1] == EVENT_TX:
            dev.write(EP_OUT,
                      frame(MESG_BROADCAST_DATA_ID,
                            bytes([args.channel]) + payload(MARK_BROADCAST, seq)))
            seq = (seq + 1) & 0xFF

    note("closing")
    command(dev, reader, MESG_CLOSE_CHANNEL_ID, bytes([args.channel]), "close")
    wait_event(reader, {EVENT_CHANNEL_CLOSED}, 3.0, counters)
    command(dev, reader, MESG_UNASSIGN_CHANNEL_ID, bytes([args.channel]),
            "unassign")

    print("\nevents: " + ", ".join(
        f"{EVENT_CODES_BY_VALUE.get(code, code)} x{count}"
        for code, count in sorted(counters.items())))
    return 0


def run_slave(dev, reader, args) -> int:
    counters: dict = {}

    if not bring_up(dev, reader, args, CHANNEL_TYPE_SLAVE):
        return 1
    if not command(dev, reader, MESG_OPEN_CHANNEL_ID, bytes([args.channel]),
                   "open"):
        return 1
    note("slave channel open, searching")

    # Wait until the channel is genuinely tracking. A slave transmits only in
    # the receive slot of a master it has found, so an acknowledged message
    # queued before that just sits there and nothing reaches the air.
    heard = 0
    deadline = time.monotonic() + args.search_secs
    while time.monotonic() < deadline and heard < args.need_broadcasts:
        result = reader.next_frame(min(deadline, time.monotonic() + 0.5))
        if result is None:
            continue
        msg_id, body = result
        if msg_id == MESG_BROADCAST_DATA_ID and body and body[0] == args.channel:
            heard += 1
            if heard == 1:
                note("first broadcast received - tracking")
        else:
            tally(result, counters)

    if heard < args.need_broadcasts:
        note(f"FAIL: only {heard} broadcasts in {args.search_secs:.0f} s; "
             "the master is not being tracked, so nothing would go on the air")
        command(dev, reader, MESG_CLOSE_CHANNEL_ID, bytes([args.channel]),
                "close")
        return 1
    note(f"tracking confirmed ({heard} broadcasts)")

    note(f"phase 2: {args.acks} acknowledged messages, slave to master")
    for i in range(args.acks):
        send_ack(dev, reader, args.channel, i, counters)
        drain(reader, args.ack_gap, counters)

    note("phase 3: bursts, slave to master")
    run_burst_plan(dev, reader, args, counters)

    drain(reader, args.tail_secs, counters)

    note("closing")
    command(dev, reader, MESG_CLOSE_CHANNEL_ID, bytes([args.channel]), "close")
    wait_event(reader, {EVENT_CHANNEL_CLOSED}, 3.0, counters)
    command(dev, reader, MESG_UNASSIGN_CHANNEL_ID, bytes([args.channel]),
            "unassign")

    print("\nevents: " + ", ".join(
        f"{EVENT_CODES_BY_VALUE.get(code, code)} x{count}"
        for code, count in sorted(counters.items())))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--role", choices=("master", "slave"), required=True)
    parser.add_argument("--serial", help="USB device whose serial ends with this")
    parser.add_argument("--port", help="UART build on this serial port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--channel", type=int, default=0)
    parser.add_argument("--device-number", type=int, default=14871)
    parser.add_argument("--device-type", type=int, default=0x0B)
    parser.add_argument("--trans-type", type=int, default=5)
    parser.add_argument("--period", type=int, default=8182)
    parser.add_argument("--broadcast-secs", type=float, default=20.0)
    parser.add_argument("--tail-secs", type=float, default=10.0)
    parser.add_argument("--acks", type=int, default=4)
    parser.add_argument("--ack-gap", type=float, default=2.0)
    parser.add_argument("--burst-plan", default="1,12",
                        help="comma-separated block counts, one burst each "
                             "(default: 1,12 - a one-packet burst, which is "
                             "sequence 0 WITH the last flag, then a long one, "
                             "whose first packet is sequence 0 without it)")
    parser.add_argument("--burst-pace", type=float, default=0.0,
                        help="seconds between burst packets. Zero - stream "
                             "them, which is what the bridge's own "
                             "back-pressure expects. Non-zero is a diagnostic "
                             "for a burst that fails, not a setting.")
    parser.add_argument("--adv-burst", action="store_true",
                        help="also run the plan with advanced burst enabled "
                             "and 24-byte blocks, which is the only way to "
                             "move the payload length")
    parser.add_argument("--search-secs", type=float, default=30.0)
    parser.add_argument("--need-broadcasts", type=int, default=8)
    args = parser.parse_args()
    args.burst_plan = [int(x) for x in args.burst_plan.split(",") if x.strip()]

    dev = open_device(True, serial=args.serial, port=args.port, baud=args.baud)
    reader = FrameReader(dev)

    note(f"role={args.role} device #{args.device_number} "
         f"type 0x{args.device_type:02X} trans {args.trans_type} "
         f"period {args.period}")

    if not reset_stack(dev, reader):
        print("FAIL: no startup message after reset")
        return 1
    if not command(dev, reader, MESG_NETWORK_KEY_ID,
                   bytes([0]) + ANT_PLUS_KEY, "ANT+ network key"):
        return 1

    try:
        if args.role == "master":
            return run_master(dev, reader, args)
        return run_slave(dev, reader, args)
    finally:
        close_device(dev)


if __name__ == "__main__":
    sys.exit(main())
