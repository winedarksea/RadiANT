"""Bench-only: hold one transmitter power rung and check every payload the
receiver delivers against the payloads the transmitter actually sent.

Reuses tools/ant_sens.py's master driver (ant_sim pages, EVENT_TX pacing,
custom TX power byte). Prints counts as JSON. A delivered payload that was
never transmitted is a mis-repair (or a mis-decode) that got through.
"""
import argparse
import json
import sys
import threading
import time

sys.path.insert(0, r"C:\wt_perf\tools")

import ant_sens  # noqa: E402
from ant_probe import close_device  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--tx-port", default="COM4")
ap.add_argument("--tx-part", default="nrf54l15")
ap.add_argument("--rx-port", default="COM11")
ap.add_argument("--rx-baud", type=int, default=57600)
ap.add_argument("--dbm", type=int, required=True)
ap.add_argument("--seconds", type=float, default=120.0)
ap.add_argument("--device-number", type=int, default=0x3A17)
ap.add_argument("--json")
a = ap.parse_args()

args = argparse.Namespace(
    tx_serial=None, tx_port=a.tx_port, tx_baud=None, baud=115200,
    rx_serial=None, rx_port=a.rx_port, rx_baud=a.rx_baud, rx=[],
    profile="power", seed=1, channel=0, device_number=a.device_number,
    trans_type=5, watts=100.0, cadence=80.0, noise=3.0, rf_freq=57)

tx_dev, tx_reader, sensor = ant_sens.open_master(args, False)
sent = set()
lock = threading.Lock()
orig = sensor.next_payload


def recording_next_payload():
    p = bytes(orig())
    with lock:
        sent.add(p)
    return p


sensor.next_payload = recording_next_payload
master = ant_sens.MasterDriver(tx_dev, tx_reader, sensor)
master.start()
level, custom = ant_sens.encode_power(a.dbm, a.tx_part)
if not master.set_power(level, custom):
    sys.exit("transmitter refused the power setting")

# Pinned to the transmitter's device number, unlike ant_sens.open_receiver()
# (wildcard), which can lock onto the room's trainer (#52233, also type 0x0B).
_orig_open_slave = ant_sens.ant_verify.open_slave
ant_sens.ant_verify.open_slave = (
    lambda dev, rd, ch, dt, per, dn, tt, rf=57:
    _orig_open_slave(dev, rd, ch, dt, per, a.device_number, 5, rf))
rx_dev, rx_reader = ant_sens.open_receiver(args, False)
received = 0
mismatched = []
rssi = []
t_end = time.monotonic() + a.seconds
while time.monotonic() < t_end:
    r = rx_reader.next_frame(time.monotonic() + 0.5)
    if r is None:
        continue
    msg_id, body = r
    if msg_id != 0x4E or len(body) < 9 or body[0] != 0:
        continue
    payload = bytes(body[1:9])
    received += 1
    if len(body) > 9 and body[9] & 0x40 and len(body) >= 15:
        # 0x80 channel id (4 bytes) precedes RSSI when present.
        off = 10 + (4 if body[9] & 0x80 else 0)
        rssi.append((body[off + 1] ^ 0x80) - 0x80)
    with lock:
        ok = payload in sent
    if not ok:
        mismatched.append(payload.hex())

master.stop()
master.join(2.0)
sent_count = master.sent
ant_sens.close_channel(rx_dev, rx_reader, 0, "rx")
ant_sens.close_channel(tx_dev, tx_reader, 0, "tx")
close_device(rx_dev)
close_device(tx_dev)

out = {"dbm": a.dbm, "seconds": a.seconds, "tx_sent": sent_count,
       "unique_payloads_sent": len(sent), "rx_delivered": received,
       "rx_not_transmitted": len(mismatched),
       "examples": mismatched[:10],
       "rssi_mean": (sum(rssi) / len(rssi)) if rssi else None,
       "loss_pct_vs_sent": (100.0 * (1 - received / sent_count)
                            if sent_count else None)}
print(json.dumps(out, indent=2))
if a.json:
    with open(a.json, "w", encoding="utf-8") as fh:
        json.dump(out, fh, indent=2)
