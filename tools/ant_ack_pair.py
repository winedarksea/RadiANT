#!/usr/bin/env python3
"""Drive both ends of an acknowledged ANT+ exchange and count at BOTH ends.

WHY THIS EXISTS. An originator physically cannot tell "my packet never arrived"
from "it arrived and the reply was lost" - every failure mode of an
acknowledged transfer surfaces at the sender as the same FAIL_NO_ACK. Two whole
bench sessions were spent inside that ambiguity, and one of them produced a
confident, wrong "hypothesis REFUTED" that got a correct fix reverted. So this
tool holds BOTH boards and reports what each one actually saw.

AND IT KEEPS THREE NUMBERS APART, which is the other half of that mistake:

    slots transmitted        the receiver's own channel period, 4 Hz
    broadcasts received      the peer's ordinary data
    acknowledged received    the thing under test

Conflating the first and third is what produced the wrong conclusion: ~219
events in 55 s is 4 Hz - the receiver's own slots - not a delivery count. Any
number near `4 x seconds` should be assumed to be a slot count until proven
otherwise.

THE RIG. Two boards, both running apps/dongle, both host-controlled here:

    --master-port   the receiver under test, an FE-C master on a UART build
                    (nRF54L15 DK: ANT on COM8, log on COM7)
    --slave-serial  the originator, a USB stick opened as a BIDIRECTIONAL
                    slave (0x00, never 0x40 - a receive-only slave has no
                    transmitter and an acknowledged message is refused with
                    CHANNEL_IN_WRONG_STATE)
    --log-port      optional console of the master, captured concurrently so
                    firmware counters line up with what the hosts saw

Example:

    python tools/ant_ack_pair.py --master-port COM8 \
        --slave-serial 755972D7183A6183 --log-port COM7 --sends 8
"""
from __future__ import annotations

import argparse
import sys
import threading
import time

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
    EVENT_TRANSFER_TX_COMPLETED,
    EVENT_TRANSFER_TX_FAILED,
    EVENT_TX,
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
)

ANT_PLUS_KEY = bytes([0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45])
ANT_PLUS_FREQ = 57
CHANNEL = 0

FEC_DEVICE_TYPE = 17          # 0x11
FEC_PERIOD = 8192             # 4.00 Hz. NOT 8134, which is SDM (0x7C).
FEC_TRANS_TYPE = 5

CHANNEL_TYPE_MASTER = 0x10    # bidirectional master: transmits AND listens
CHANNEL_TYPE_SLAVE = 0x00     # bidirectional slave

# FE-C page 16, a plausible general-FE broadcast for the master to hold.
PAGE16 = bytes([16, 25, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x24])
# Page 51 Track Resistance, flat.
PAGE51 = bytes([0x33, 0xFF, 0xFF, 0xFF, 0xFF, 0x84, 0x4E, 0xFF])


class Counters:
    """Three numbers kept apart, plus everything else that arrived."""

    def __init__(self) -> None:
        self.total = 0            # every host message, the flood measure
        self.slots = 0            # EVENT_TX: our own channel period
        self.broadcast = 0        # peer broadcasts received
        self.acked = 0            # acknowledged data received
        self.burst = 0            # burst data received
        self.events: dict[int, int] = {}


def note(counters: Counters, msg_id: int, body: bytes) -> None:
    counters.total += 1
    if msg_id == MESG_BROADCAST_DATA_ID:
        counters.broadcast += 1
    elif msg_id == MESG_ACKNOWLEDGED_DATA_ID:
        counters.acked += 1
    elif msg_id == MESG_BURST_DATA_ID:
        counters.burst += 1
    elif msg_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3:
        # body[1] IS MESG_EVENT_ID (0x01) for an event, not 0. Written as
        # `== 0` every terminal event is discarded and the tool reports "no
        # terminal event", which reads as a radio or scheduling fault.
        if body[1] == MESG_EVENT_ID:
            code = body[2]
            counters.events[code] = counters.events.get(code, 0) + 1
            if code == EVENT_TX:
                counters.slots += 1


def command(dev, reader, msg_id: int, payload: bytes, name: str,
            timeout: float = 2.0) -> bool:
    dev.write(EP_OUT, frame(msg_id, payload))
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        result = reader.next_frame(min(end, time.monotonic() + 0.25))
        if result is None:
            continue
        rid, body = result
        if rid == MESG_RESPONSE_EVENT_ID and len(body) >= 3 and body[1] == msg_id:
            if body[2] != 0:
                print(f"  FAIL: {name} -> code {body[2]}")
                return False
            print(f"  OK: {name}")
            return True
    print(f"  FAIL: {name} was not answered")
    return False


def configure(dev, reader, ch_type: int, device: int, label: str) -> bool:
    print(f"\nConfiguring {label}")
    if not reset_stack(dev, reader):
        print("  FAIL: no startup message after reset")
        return False
    command(dev, reader, MESG_ANTLIB_CONFIG_ID,
            bytes([0x00, LIB_CONFIG_ALL_EXT_FIELDS]), "extended messages")
    steps = [
        (MESG_NETWORK_KEY_ID, bytes([0]) + ANT_PLUS_KEY, "network key"),
        (MESG_ASSIGN_CHANNEL_ID, bytes([CHANNEL, ch_type, 0x00]),
         f"assign channel type 0x{ch_type:02X}"),
        (MESG_CHANNEL_ID_ID,
         bytes([CHANNEL, device & 0xFF, (device >> 8) & 0xFF,
                FEC_DEVICE_TYPE, FEC_TRANS_TYPE]),
         f"channel id #{device} type {FEC_DEVICE_TYPE}"),
        (MESG_CHANNEL_RADIO_FREQ_ID, bytes([CHANNEL, ANT_PLUS_FREQ]),
         "radio frequency 2457 MHz"),
        (MESG_CHANNEL_MESG_PERIOD_ID,
         bytes([CHANNEL, FEC_PERIOD & 0xFF, (FEC_PERIOD >> 8) & 0xFF]),
         f"message period {FEC_PERIOD}"),
        (MESG_CHANNEL_SEARCH_TIMEOUT_ID, bytes([CHANNEL, 0xFF]),
         "infinite search timeout"),
    ]
    for msg_id, payload, name in steps:
        if not command(dev, reader, msg_id, payload, name):
            return False

    # THE BROADCAST BUFFER, STAGED BEFORE OPEN AND CHECKED. antr_broadcast_
    # message_tx() is refused with CHANNEL_NOT_OPENED before the channel is
    # open, so staging a page here would silently set nothing - see the open
    # below, which is why this comes after it for the master.
    return command(dev, reader, MESG_OPEN_CHANNEL_ID, bytes([CHANNEL]),
                   "open channel")


def log_thread(port: str, path: str, stop: threading.Event) -> None:
    import serial

    try:
        s = serial.Serial(port, 115200, timeout=0.2)
    except Exception as exc:  # noqa: BLE001
        print(f"  log {port}: open FAILED ({type(exc).__name__}: {exc})")
        return
    # J-Link's CDC hands over one 128-byte chunk and then goes silent without
    # DTR, which looks exactly like a board that hung after boot.
    s.dtr = True
    s.rts = True
    print(f"  log {port}: capturing to {path}")
    with open(path, "wb") as fh:
        while not stop.is_set():
            data = s.read(4096)
            if data:
                fh.write(data)
                fh.flush()
    s.close()


def master_thread(dev, reader, counters: Counters, stop: threading.Event) -> None:
    """Hold the master on the air and count everything it reports.

    Refills the broadcast buffer on every EVENT_TX, which is what a real host
    library does and what keeps a master's page current.
    """
    while not stop.is_set():
        result = reader.next_frame(time.monotonic() + 0.25)
        if result is None:
            continue
        msg_id, body = result
        note(counters, msg_id, body)
        if (msg_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3
                and body[1] == MESG_EVENT_ID and body[2] == EVENT_TX):
            try:
                dev.write(EP_OUT, frame(MESG_BROADCAST_DATA_ID,
                                        bytes([CHANNEL]) + PAGE16))
            except Exception:  # noqa: BLE001 - the port closing under us
                return


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    # Either end may be the UART board or the USB stick, because WHICH BOARD
    # ORIGINATES IS ITSELF A VARIABLE: the same pair measured 219 delivered in
    # one direction and 0 in the other, so a rig that can only be wired one way
    # cannot tell a link defect from a board defect.
    ap.add_argument("--master-port",
                    help="serial port of the MASTER (receiver), UART build")
    ap.add_argument("--master-serial", help="USB serial of the MASTER instead")
    ap.add_argument("--master-baud", type=int, default=115200)
    ap.add_argument("--slave-port",
                    help="serial port of the SLAVE (originator), UART build")
    ap.add_argument("--slave-serial", help="USB serial of the SLAVE instead")
    ap.add_argument("--log-port", help="console of the master, captured concurrently")
    ap.add_argument("--log-file", default="ack_pair_console.log")
    ap.add_argument("--device", type=int, default=14871,
                    help="ANT device number both ends use")
    ap.add_argument("--sends", type=int, default=8,
                    help="acknowledged packets the originator sends")
    ap.add_argument("--acquire-seconds", type=float, default=20.0)
    ap.add_argument("--settle-seconds", type=float, default=5.0,
                    help="quiet time AFTER the last send, where a flood shows")
    ap.add_argument("--lone-master", action="store_true",
                    help="REGRESSION CHECK. Open the master and open nothing "
                         "else at all, then require that it reports no "
                         "received data. A receive path that replays its last "
                         "frame once per window fails this with no peer in the "
                         "room, which is how the section 7.8 flood was found; "
                         "it needs one board and no originator.")
    ap.add_argument("--close-originator", action="store_true",
                    help="close the originator's channel before settling - the "
                         "control arm that tells 'still transmitting' apart "
                         "from 'the receiver invents one report per slot'")
    args = ap.parse_args()

    if bool(args.master_port) == bool(args.master_serial):
        ap.error("pass exactly one of --master-port / --master-serial")
    if not args.lone_master and bool(args.slave_port) == bool(args.slave_serial):
        ap.error("pass exactly one of --slave-port / --slave-serial")

    def open_end(port, serial, label):
        print(f"\nOpening the {label}")
        if port:
            return open_device(True, port=port, baud=args.master_baud)
        return open_device(True, serial=serial)

    stop = threading.Event()
    threads: list[threading.Thread] = []

    if args.log_port:
        t = threading.Thread(target=log_thread,
                             args=(args.log_port, args.log_file, stop),
                             daemon=True)
        t.start()
        threads.append(t)
        time.sleep(0.5)

    master = open_end(args.master_port, args.master_serial,
                      "MASTER (the receiver)")
    m_reader = FrameReader(master)
    if not configure(master, m_reader, CHANNEL_TYPE_MASTER, args.device, "master"):
        stop.set()
        return 1
    # Now that it is open, give it a page to transmit.
    master.write(EP_OUT, frame(MESG_BROADCAST_DATA_ID, bytes([CHANNEL]) + PAGE16))

    m_counts = Counters()
    mt = threading.Thread(target=master_thread,
                          args=(master, m_reader, m_counts, stop), daemon=True)
    mt.start()
    threads.append(mt)

    print("\nLetting the master reach the air")
    time.sleep(3.0)

    if args.lone_master:
        # NOTHING ELSE IS OPENED. Every frame reported from here is one the
        # receive path invented, because there is no second radio in the
        # exchange to have sent it.
        watch = max(args.settle_seconds, 15.0)
        print(f"\nLONE MASTER: no originator exists. Watching {watch:.0f} s.")
        before = (m_counts.total, m_counts.acked, m_counts.broadcast,
                  m_counts.burst)
        t0 = time.monotonic()
        time.sleep(watch)
        secs = time.monotonic() - t0
        stop.set()
        for t in threads:
            t.join(timeout=3.0)

        acked = m_counts.acked - before[1]
        bcast = m_counts.broadcast - before[2]
        burst = m_counts.burst - before[3]
        data = acked + bcast + burst
        print(f"\nOver {secs:.1f} s with no peer anywhere:")
        print(f"  acknowledged received    {acked}   ({acked / secs:.1f}/s)")
        print(f"  broadcasts received      {bcast}")
        print(f"  burst received           {burst}")
        print(f"  total host messages      {m_counts.total - before[0]}")
        if data:
            print(f"\n  FAIL: {data} data message(s) arrived from nobody. The "
                  f"receive\n        path is replaying a frame it heard "
                  f"earlier. At {data / secs:.1f}/s\n        this saturates any "
                  f"host that receives acknowledged data.")
        else:
            print("\n  PASS: nothing was received, which is the only correct "
                  "answer\n        when nothing was transmitted.")
        try:
            master.write(EP_OUT, frame(MESG_CLOSE_CHANNEL_ID, bytes([CHANNEL])))
            time.sleep(0.2)
            close_device(master)
        except Exception:  # noqa: BLE001
            pass
        return 1 if data else 0

    slave = open_end(args.slave_port, args.slave_serial,
                     "SLAVE (the originator)")
    s_reader = FrameReader(slave)
    if not configure(slave, s_reader, CHANNEL_TYPE_SLAVE, args.device, "slave"):
        stop.set()
        return 1

    print(f"\nWaiting up to {args.acquire_seconds:.0f} s for the slave to track")
    s_counts = Counters()
    heard = 0
    deadline = time.monotonic() + args.acquire_seconds
    while time.monotonic() < deadline and heard < 4:
        result = s_reader.next_frame(min(deadline, time.monotonic() + 1.0))
        if result is None:
            continue
        note(s_counts, result[0], result[1])
        if result[0] == MESG_BROADCAST_DATA_ID:
            heard += 1
    if heard < 4:
        print(f"  FAIL: only {heard} broadcasts - not tracking, so nothing "
              f"below would mean anything")
        stop.set()
        return 1
    print(f"  OK: tracking ({heard} broadcasts)")

    # Baseline: what the master's host sees per second with broadcast only.
    base_total, base_bcast, base_slots = (m_counts.total, m_counts.broadcast,
                                          m_counts.slots)
    t0 = time.monotonic()
    time.sleep(5.0)
    base_secs = time.monotonic() - t0
    print(f"\nBASELINE, broadcast only, {base_secs:.1f} s at the master's host:")
    print(f"  total={m_counts.total - base_total} "
          f"broadcasts={m_counts.broadcast - base_bcast} "
          f"slots(EVENT_TX)={m_counts.slots - base_slots}")

    print(f"\nOriginating {args.sends} acknowledged packets")
    pre_total, pre_acked = m_counts.total, m_counts.acked
    t_send = time.monotonic()
    outcomes: dict[str, int] = {}
    for i in range(args.sends):
        slave.write(EP_OUT, frame(MESG_ACKNOWLEDGED_DATA_ID,
                                  bytes([CHANNEL]) + PAGE51))
        end = time.monotonic() + 2.0
        got = None
        while time.monotonic() < end:
            result = s_reader.next_frame(min(end, time.monotonic() + 0.25))
            if result is None:
                continue
            note(s_counts, result[0], result[1])
            msg_id, body = result
            if (msg_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3
                    and body[1] == MESG_EVENT_ID):
                if body[2] == EVENT_TRANSFER_TX_COMPLETED:
                    got = "COMPLETED"
                    break
                if body[2] == EVENT_TRANSFER_TX_FAILED:
                    got = "FAILED"
                    break
        got = got or "no terminal event"
        outcomes[got] = outcomes.get(got, 0) + 1
        print(f"  {i + 1}/{args.sends}: {got}")

    # THE MEASUREMENT THAT SEPARATES THE TWO CANDIDATE DEFECTS. Everything the
    # master hears from here on arrives after the originator's host has been
    # told the transfer ended and has sent nothing further. A receive-path
    # amplifier would be quiet in this window; an originator that never stops
    # transmitting would not.
    # THE CONTROL ARM, and it is the one that matters. 4 messages a second is
    # also exactly the channel period, so "the master's host saw 4/s" is
    # equally consistent with the originator still transmitting and with the
    # receiver manufacturing one report per slot out of nothing. Closing the
    # originator's channel removes it from the air entirely: anything still
    # arriving afterwards cannot have come from it.
    if args.close_originator:
        print("\nCLOSING the originator's channel - it is now off the air")
        slave.write(EP_OUT, frame(MESG_CLOSE_CHANNEL_ID, bytes([CHANNEL])))
        time.sleep(1.0)

    print(f"\nSettling {args.settle_seconds:.0f} s with NOTHING being sent")
    settle_total, settle_acked = m_counts.total, m_counts.acked
    t_settle = time.monotonic()
    time.sleep(args.settle_seconds)
    settle_secs = time.monotonic() - t_settle
    settle_n = m_counts.acked - settle_acked
    elapsed = time.monotonic() - t_send

    stop.set()
    for t in threads:
        t.join(timeout=3.0)

    print("\n" + "=" * 68)
    print(f"THE MASTER'S HOST, over {elapsed:.1f} s covering {args.sends} sends")
    print("=" * 68)
    print(f"  total host messages      {m_counts.total - pre_total}"
          f"   ({(m_counts.total - pre_total) / elapsed:.1f}/s)")
    print(f"  acknowledged received    {m_counts.acked - pre_acked}"
          f"   (expected {args.sends})")
    print(f"  broadcasts received      {m_counts.broadcast - base_bcast}")
    print(f"  slots transmitted        {m_counts.slots - base_slots}")
    print(f"  events                   "
          f"{ {hex(k): v for k, v in sorted(m_counts.events.items())} }")
    print(f"\n  the originator was told  {outcomes}")

    amp = (m_counts.acked - pre_acked) / args.sends if args.sends else 0.0
    print(f"\n  amplification            {amp:.1f}x per acknowledged packet")

    print("\n" + "-" * 68)
    print(f"AFTER THE LAST SEND, {settle_secs:.1f} s with the host sending nothing")
    print("-" * 68)
    print(f"  acknowledged received    {settle_n}"
          f"   ({settle_n / settle_secs:.1f}/s)")
    print(f"  total host messages      {m_counts.total - settle_total}")
    if settle_n > 0 and args.close_originator:
        print("\n  => THE RECEIVER IS MANUFACTURING THESE. The originator's\n"
              "     channel was closed, so nothing was on the air to receive.")
    elif settle_n > 0:
        print("\n  => still arriving, but the originator was still on the air,\n"
              "     so this does NOT yet say which end produces them. Re-run\n"
              "     with --close-originator.")
    else:
        print("\n  => nothing arrives once the sends stop.")

    for dev in (slave, master):
        try:
            dev.write(EP_OUT, frame(MESG_CLOSE_CHANNEL_ID, bytes([CHANNEL])))
            time.sleep(0.2)
            close_device(dev)
        except Exception:  # noqa: BLE001
            pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
