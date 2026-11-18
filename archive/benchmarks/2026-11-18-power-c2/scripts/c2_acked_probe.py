"""C2 gate: probe the TI hr_transmitter's own RX windows from a slave.

Opens a BIDIRECTIONAL slave (0x00) on HR #12345 (type 0x78, trans 5,
period 8070, 2457 MHz) and, while tracking, sends one acknowledged message
(a manufacturer page 0xF0 carrying a sequence number) at a randomised
interval. The master must receive it in the listen window after its
broadcast and answer with an ACK; the slave reports EVENT_TRANSFER_TX_COMPLETED
for an ACK heard and EVENT_TRANSFER_TX_FAILED otherwise. So the success rate is
a round trip through the TI's in-grant RX arm and its ACK transmit, the path a
late RX arm (RCOSC_HF recalibration at the RF driver's priority) would hurt,
and which `cold`/`txl` on the TI's power line do not see.

Also counts the master's broadcasts and RX_FAIL / RX_FAIL_GO_TO_SEARCH, so a
failed transfer can be told from a lost track.

Usage: python c2_acked_probe.py --port COM4 --baud 115200 --seconds 600 --json out.json
   or: python c2_acked_probe.py --serial EE69... --seconds 600 --json out.json
"""
import argparse
import json
import random
import sys
import time

sys.path.insert(0, r"C:\Users\Colin\ant_dongle\tools")

from ant_probe import EP_OUT, FrameReader, close_device, frame, open_device, reset_stack  # noqa: E402
from ant_wire import (  # noqa: E402
    EVENT_RX_FAIL,
    EVENT_RX_FAIL_GO_TO_SEARCH,
    EVENT_TRANSFER_TX_COMPLETED,
    EVENT_TRANSFER_TX_FAILED,
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

KEY = bytes([0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45])
CH = 0


def command(dev, reader, msg_id, payload, name, timeout=2.0):
    dev.write(EP_OUT, frame(msg_id, payload))
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        r = reader.next_frame(min(end, time.monotonic() + 0.25))
        if r is None:
            continue
        rid, body = r
        if rid == MESG_RESPONSE_EVENT_ID and len(body) >= 3 and body[1] == msg_id:
            ok = body[2] == 0
            print(f"  {'OK' if ok else 'FAIL'}: {name} (code {body[2]})")
            return ok
    print(f"  FAIL: {name} not answered")
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--serial")
    ap.add_argument("--seconds", type=float, default=600)
    ap.add_argument("--device", type=int, default=12345)
    ap.add_argument("--min-gap", type=float, default=1.0)
    ap.add_argument("--max-gap", type=float, default=2.0)
    ap.add_argument("--json")
    ap.add_argument("--label", default="")
    a = ap.parse_args()

    dev = open_device(True, port=a.port, baud=a.baud) if a.port else open_device(True, serial=a.serial)
    rd = FrameReader(dev)
    if not reset_stack(dev, rd):
        print("FAIL: no startup message")
        return 1
    command(dev, rd, MESG_ANTLIB_CONFIG_ID, bytes([0x00, 0xE0]), "ext messages")
    steps = [
        (MESG_NETWORK_KEY_ID, bytes([0]) + KEY, "network key"),
        (MESG_ASSIGN_CHANNEL_ID, bytes([CH, 0x00, 0x00]), "assign bidirectional slave"),
        (MESG_CHANNEL_ID_ID, bytes([CH, a.device & 0xFF, a.device >> 8, 0x78, 5]), f"channel id #{a.device}/0x78/5"),
        (MESG_CHANNEL_RADIO_FREQ_ID, bytes([CH, 57]), "rf 57"),
        (MESG_CHANNEL_MESG_PERIOD_ID, bytes([CH, 8070 & 0xFF, 8070 >> 8]), "period 8070"),
        (MESG_CHANNEL_SEARCH_TIMEOUT_ID, bytes([CH, 0xFF]), "infinite search"),
        (MESG_OPEN_CHANNEL_ID, bytes([CH]), "open"),
    ]
    for mid, pl, name in steps:
        if not command(dev, rd, mid, pl, name):
            return 1

    st = dict(label=a.label, device=a.device, broadcasts=0, rx_fail=0, rx_fail_go_to_search=0,
              sent=0, completed=0, failed=0, unresolved=0, other_events={}, rssi=[],
              per_minute=[], host_start_unix=time.time())
    t0 = time.monotonic()
    end = t0 + a.seconds
    in_flight = None          # (seq, t_sent)
    next_send = t0 + 5.0      # let it track first
    seq = 0
    minute = dict(t=0, b=0, s=0, c=0, f=0, rxf=0)
    first_bcast = None
    while time.monotonic() < end:
        now = time.monotonic()
        if in_flight is None and now >= next_send and first_bcast is not None:
            seq = (seq + 1) & 0xFF
            dev.write(EP_OUT, frame(MESG_ACKNOWLEDGED_DATA_ID,
                                    bytes([CH, 0xF0, seq, 0xC2, 0x00, 0xFF, 0xFF, 0xFF, 0xFF])))
            in_flight = (seq, now)
            st["sent"] += 1
            minute["s"] += 1
        if in_flight is not None and now - in_flight[1] > 5.0:
            st["unresolved"] += 1
            in_flight = None
            next_send = now + random.uniform(a.min_gap, a.max_gap)
        r = rd.next_frame(min(end, now + 0.25))
        if int((time.monotonic() - t0) // 60) > minute["t"]:
            st["per_minute"].append(minute)
            minute = dict(t=minute["t"] + 1, b=0, s=0, c=0, f=0, rxf=0)
        if r is None:
            continue
        mid, body = r
        if mid == MESG_BROADCAST_DATA_ID:
            st["broadcasts"] += 1
            minute["b"] += 1
            if first_bcast is None:
                first_bcast = time.monotonic() - t0
            # ext fields 0xE0: flag byte after the 8 data bytes, then 4 B id, 3 B RSSI, 2 B ts
            if len(body) >= 10 and body[9] & 0x40:
                off = 10 + (4 if body[9] & 0x80 else 0)
                if len(body) > off + 1:
                    v = body[off + 1]
                    st["rssi"].append(v - 256 if v > 127 else v)
        elif mid == MESG_RESPONSE_EVENT_ID and len(body) >= 3 and body[1] == MESG_EVENT_ID:
            code = body[2]
            if code == EVENT_RX_FAIL:
                st["rx_fail"] += 1
                minute["rxf"] += 1
            elif code == EVENT_RX_FAIL_GO_TO_SEARCH:
                st["rx_fail_go_to_search"] += 1
            elif code in (EVENT_TRANSFER_TX_COMPLETED, EVENT_TRANSFER_TX_FAILED):
                if code == EVENT_TRANSFER_TX_COMPLETED:
                    st["completed"] += 1
                    minute["c"] += 1
                else:
                    st["failed"] += 1
                    minute["f"] += 1
                in_flight = None
                next_send = time.monotonic() + random.uniform(a.min_gap, a.max_gap)
            else:
                st["other_events"][str(code)] = st["other_events"].get(str(code), 0) + 1
    st["per_minute"].append(minute)
    secs = time.monotonic() - t0
    st["seconds"] = secs
    st["first_broadcast_s"] = first_bcast
    st["host_end_unix"] = time.time()
    rs = st.pop("rssi")
    st["rssi_mean"] = sum(rs) / len(rs) if rs else None
    st["rssi_min"] = min(rs) if rs else None
    st["rssi_max"] = max(rs) if rs else None
    st["rssi_n"] = len(rs)
    done = st["completed"] + st["failed"]
    st["ack_success_pct"] = 100.0 * st["completed"] / done if done else None
    try:
        dev.write(EP_OUT, frame(MESG_CLOSE_CHANNEL_ID, bytes([CH])))
        time.sleep(0.3)
        close_device(dev)
    except Exception:  # noqa: BLE001
        pass
    print(json.dumps({k: v for k, v in st.items() if k != "per_minute"}, indent=1))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(st, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
