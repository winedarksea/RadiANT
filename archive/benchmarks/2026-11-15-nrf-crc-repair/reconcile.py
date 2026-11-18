"""C1 per-block table: crc_truth counts against the 0xF7 page diffs.

fw_delivered estimates what the firmware handed the host on the tracked
channel: api_sched_rx entries (page 0 rx_sched_events) minus tracked CRC-fail
events offered to repair (page 10 [0..3]), plus the repaired ones (which are
delivered). It also counts the frames between the script's last read and its
channel close, which the script does not read, and the acquisition frame(s)
heard in the search window, so fw_delivered >= rx_delivered by a few.
Page 5 is not subtracted: its frames_ok fell from 5 to 3 across block 6, so it
is not a since-boot counter on this image and its difference means nothing.
"""
import glob
import json
import os
import re

A = os.path.dirname(os.path.abspath(__file__))


def fields(path):
    with open(path, encoding="utf-8") as fh:
        d = json.load(fh)
    out = {}
    for p in d["pages"].values():
        for k, v in p["fields"].items():
            out[(p["page"], k)] = v
    return out


def delta(b, a, key):
    x, y = b.get(key), a.get(key)
    if not isinstance(x, int) or not isinstance(y, int):
        return None
    return (y - x) & 0xFFFFFFFF


rows = []
for t in sorted(glob.glob(os.path.join(A, "truth_b*_*.json")),
                key=lambda p: int(re.search(r"truth_b(\d+)_", p).group(1))):
    n = int(re.search(r"truth_b(\d+)_", t).group(1))
    with open(t, encoding="utf-8") as fh:
        tr = json.load(fh)
    b = fields(os.path.join(A, f"diag_before_b{n}.json"))
    a = fields(os.path.join(A, f"diag_after_b{n}.json"))
    d = {k: delta(b, a, k) for k in [
        (0, "rx_sched_events"), (0, "slots_missed"), (0, "sched_dones"),
        (4, "crc_fail"), (5, "frames_ok"), (5, "crc_fail"),
        (10, "crc_repaired"), (10, "crc_unrepairable"),
        (10, "crc_repair_refuted"), (10, "crc_repair_implausible"),
        (10, "late_isr_drops"), (9, "early_etime")]}
    offered = sum(d[(10, k)] for k in ("crc_repaired", "crc_unrepairable",
                                       "crc_repair_refuted",
                                       "crc_repair_implausible"))
    fw = d[(0, "rx_sched_events")] - offered + d[(10, "crc_repaired")]
    rows.append({
        "block": n, "dbm": tr["dbm"], "tx_sent": tr["tx_sent"],
        "rx_delivered": tr["rx_delivered"],
        "rx_not_transmitted": tr["rx_not_transmitted"],
        "loss_pct": round(tr["loss_pct_vs_sent"], 2),
        "rssi_mean": round(tr["rssi_mean"], 1),
        "backend_crc_fail": d[(4, "crc_fail")],
        "offered_to_repair": offered,
        "crc_repaired": d[(10, "crc_repaired")],
        "crc_unrepairable": d[(10, "crc_unrepairable")],
        "refuted": d[(10, "crc_repair_refuted")],
        "implausible": d[(10, "crc_repair_implausible")],
        "late_isr_drops": d[(10, "late_isr_drops")],
        "early_etime": d[(9, "early_etime")],
        "rx_sched_events": d[(0, "rx_sched_events")],
        "fw_delivered_est": fw,
        "fw_minus_script": fw - tr["rx_delivered"],
    })

print(json.dumps(rows, indent=1))
with open(os.path.join(A, "reconcile.json"), "w", encoding="utf-8") as fh:
    json.dump(rows, fh, indent=1)
