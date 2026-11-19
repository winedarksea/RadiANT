"""Gate G2 Garmin rerun: inbound bursts at the apps/trainer FE-C master (slave -> master).
Copy of G2's g2_burst.py; adds --serial so a USB ANT stick (the Garmin, 006) can be the slave.

The slave (Garmin over USB, or the TI on a UART) opens fec_ack_sweep's pinned bidirectional
FE-C slave to the trainer (#4244), then originates N bursts of K packets, each
written right after a received broadcast. Every burst packet is acknowledged by
the trainer: packet 0 lands in the master's reply window (+2190 from its own
frame), every later packet in the chained window after the trainer's ACK, so the
trainer must stay awake (HFXO held) through the whole chain. Counts the dongle's
terminal events (TRANSFER_TX_COMPLETED / _FAILED) and broadcasts heard.

usage: python g2g_burst.py (--serial 006 | --port COM11 --baud 57600) --bursts 50 --packets 8 --json out.json
"""
import argparse
import json
import os
import sys
import time

TOOLS = r"C:\Users\Colin\ant_dongle\tools"
sys.path.insert(0, TOOLS)

from fec_ack_sweep import open_originator  # noqa: E402
from ant_ack_pair import burst_host_messages  # noqa: E402
from ant_probe import EP_OUT, frame  # noqa: E402
from ant_wire import (EVENT_TRANSFER_TX_COMPLETED, EVENT_TRANSFER_TX_FAILED,  # noqa: E402
                      MESG_BROADCAST_DATA_ID, MESG_EVENT_ID,
                      MESG_RESPONSE_EVENT_ID, MESG_CLOSE_CHANNEL_ID)

ap = argparse.ArgumentParser()
ap.add_argument("--port")
ap.add_argument("--serial", help="USB ANT stick by serial suffix (Garmin: 006)")
ap.add_argument("--baud", type=int, default=57600)
ap.add_argument("--device", type=int, default=4244)
ap.add_argument("--bursts", type=int, default=50)
ap.add_argument("--packets", type=int, default=8)
ap.add_argument("--gap", type=float, default=0.6)
ap.add_argument("--json")
a = ap.parse_args()

dev, reader = open_originator(a.device, a.serial, a.port, a.baud)
outcomes = {}
other_events = {}
bcast = 0
rows = []
t0 = time.monotonic()


def wait_broadcast(limit):
    global bcast
    end = time.monotonic() + limit
    while time.monotonic() < end:
        r = reader.next_frame(min(end, time.monotonic() + 0.3))
        if r and r[0] == MESG_BROADCAST_DATA_ID:
            bcast += 1
            return True
    return False


for b in range(a.bursts):
    wait_broadcast(0.6)
    ts = time.monotonic()
    for msg_id, body in burst_host_messages(b, a.packets, 8):
        dev.write(EP_OUT, frame(msg_id, body))
    got = "no terminal event"
    end = time.monotonic() + 3.0 + 0.05 * a.packets
    while time.monotonic() < end:
        r = reader.next_frame(min(end, time.monotonic() + 0.25))
        if r is None:
            continue
        msg_id, body = r
        if msg_id == MESG_BROADCAST_DATA_ID:
            bcast += 1
            continue
        if msg_id == MESG_RESPONSE_EVENT_ID and len(body) >= 3:
            if body[1] != MESG_EVENT_ID:
                if body[2] != 0:
                    got = f"REFUSED msg 0x{body[1]:02X} code {body[2]}"
                    break
                continue
            if body[2] == EVENT_TRANSFER_TX_COMPLETED:
                got = "COMPLETED"
                break
            if body[2] == EVENT_TRANSFER_TX_FAILED:
                got = "FAILED"
                break
            k = f"event 0x{body[2]:02X}"
            other_events[k] = other_events.get(k, 0) + 1
    dt = time.monotonic() - ts
    outcomes[got] = outcomes.get(got, 0) + 1
    rows.append({"burst": b, "outcome": got, "host_s": round(dt, 3)})
    if (b + 1) % 10 == 0 or got != "COMPLETED":
        print(f"  {b + 1}/{a.bursts}: {got} ({dt * 1000:.0f} ms)  so far {outcomes}", flush=True)
    time.sleep(a.gap)

elapsed = time.monotonic() - t0
try:
    dev.write(EP_OUT, frame(MESG_CLOSE_CHANNEL_ID, bytes([0])))
    time.sleep(0.3)
except Exception:  # noqa: BLE001
    pass
summary = {"bursts": a.bursts, "packets": a.packets, "seconds": round(elapsed, 1),
           "outcomes": outcomes, "other_events": other_events, "broadcasts_heard": bcast}
print(json.dumps(summary))
if a.json:
    with open(a.json, "w", encoding="utf-8") as fh:
        json.dump({"summary": summary, "rows": rows}, fh, indent=1)
