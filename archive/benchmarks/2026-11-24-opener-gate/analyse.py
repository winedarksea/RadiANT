#!/usr/bin/env python3
"""Counts per arm from tools/ant_opener_ab.py JSONL logs.

ACK payload: the Garmin slave's ACK carries its own (never-written) buffer,
0000515b7dbffb16, not an echo of the acknowledged data. A first post with
that payload is an ACK posted as BROADCAST_DATA.
"""
import json
import sys
from pathlib import Path

ACK_PAYLOAD = "0000515b7dbffb16"


def arm(path):
    recs = [json.loads(l) for l in open(path, encoding="utf-8")]
    trials = [r for r in recs if "i" in r]
    tail = next((r["tail"] for r in recs if "tail" in r), {})
    acq = [r for r in trials if r["acq_s"] is not None]
    off = [r for r in acq if r["off_grid"]]
    ack_first = [r for r in acq if r["first_payload"] == ACK_PAYLOAD]
    ack_any = [r for r in acq
               if any(len(e) > 3 and e[1] == "BC_X" and e[3] == ACK_PAYLOAD[:6]
                      for e in r["events"])]
    gts = [r for r in acq if r["n_gts"]]
    rxf_any = [r for r in acq if r["n_rxf"]]
    rxf_heavy = [r for r in acq if r["n_rxf"] >= 8]
    on = [r for r in acq if not r["off_grid"]]
    mean_rxf_on = sum(r["n_rxf"] for r in on) / max(1, len(on))
    mean_master_on = sum(r["n_master"] for r in on) / max(1, len(on))
    return {
        "trials": len(trials), "acquired": len(acq), "off_grid": len(off),
        "ack_payload_first_post": len(ack_first),
        "ack_payload_any_post": len(ack_any),
        "with_gts": len(gts), "with_any_rxf": len(rxf_any),
        "with_rxf_ge8": len(rxf_heavy),
        "on_grid_mean_rxf": round(mean_rxf_on, 2),
        "on_grid_mean_master_posts": round(mean_master_on, 1),
        "off_grid_trials": [r["i"] for r in off],
        "master": tail.get("master"), "slave": tail.get("slave"),
    }


def main():
    for p in sys.argv[1:]:
        print(Path(p).stem, json.dumps(arm(p)))


if __name__ == "__main__":
    main()
