"""Tabulate the gate_extra TX legs: python analyse_tx.py leg1 leg2 ... > table_tx.txt"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DIFF = [(0, "sched_missed"), (0, "slots_missed"), (0, "xfer_no_ack"),
        (1, "mrx_posted"), (1, "mrx_done_ok"), (1, "mrx_done_missed"), (1, "mrx_refused"),
        (2, "tx_expired"), (2, "sched_arm_rejected"),
        (3, "rx_posted"), (3, "rx_refused"), (3, "term_ok"), (3, "term_error"),
        (4, "sw_addr_mismatch"), (4, "crc_fail"),
        (7, "tx_start_missed"), (7, "rx_start_missed"), (7, "trk_open_late"),
        (10, "crc_repaired"), (10, "crc_unrepairable"), (10, "crc_repair_implausible"),
        (10, "late_isr_drops"),
        (11, "windows_merged"), (11, "arm_rejected"), (11, "tx_armed"), (11, "stale_events"),
        (11, "slots_collided"), (11, "turn_yields"), (12, "stale_tx")]
LEVEL = [(2, "tx_margin_min_us"), (7, "trk_open_late_max_us"), (8, "pass_max_us"),
         (8, "pass_steps_max"), (9, "sweep_max_us"), (9, "decide_max_us"),
         (9, "arm_call_max_us"), (9, "done_cb_max_us"), (9, "pass_to_arm_max_us"),
         (9, "arm_slack_min_us"), (9, "early_etime"), (10, "end_late_max_us")]


def load(p):
    with open(p) as f:
        return json.load(f)


def field(d, page, name):
    return d["pages"][str(page)]["fields"].get(name)


def health(path):
    txt = open(path, encoding="utf-8", errors="replace").read()
    m = re.search(r"\{.*\}", txt, re.S)
    return json.loads(m.group(0)) if m else {}


legs = sys.argv[1:]
rows = {}
for leg in legs:
    d = os.path.join(HERE, leg)
    b, a = load(os.path.join(d, "diag_before.json")), load(os.path.join(d, "diag_after.json"))
    pm, ti = load(os.path.join(d, "role_pm.json")), load(os.path.join(d, "role_ti.json"))
    hb, ha = health(os.path.join(d, "health_before.txt")), health(os.path.join(d, "health_after.txt"))
    r = {}
    m = pm["master"]
    for k in ("event_tx", "broadcasts_loaded", "acked_loaded", "acked_completed",
              "acked_failed", "acked_no_terminal", "fallback_sends"):
        r[f"pm master {k}"] = m[k]
    r["pm master other_events"] = m["other_events"]
    for who, res in (("pm", pm), ("ti", ti)):
        for key, t in res["tracks"].items():
            r[f"{who} trk {key} rx/fail/loss%"] = f"{t['received']}/{t['rx_fail']}/{t['loss_pct']}"
            if t["acked"]:
                r[f"{who} trk {key} acked distinct/dups"] = f"{t['acked_distinct']}/{t['acked_dups']}"
            if t["go_to_search"]:
                r[f"{who} trk {key} go_to_search"] = t["go_to_search"]
    for page, name in DIFF:
        vb, va = field(b, page, name), field(a, page, name)
        r[f"p{page} {name} (diff)"] = None if va is None else va - (vb or 0)
    for page, name in LEVEL:
        r[f"p{page} {name} (level after)"] = field(a, page, name)
    for k in ("tx_drops", "tx_timeout", "tx_high_water", "host_csum", "host_desync",
              "event_overflows"):
        r[f"0xF6 {k} before->after"] = f"{hb.get(k)}->{ha.get(k)}"
    rows[leg] = r

keys = []
for r in rows.values():
    for k in r:
        if k not in keys:
            keys.append(k)
w = max(len(k) for k in keys)
print(f"{'field':<{w}}  " + "  ".join(f"{leg:>18}" for leg in legs))
for k in keys:
    print(f"{k:<{w}}  " + "  ".join(f"{str(rows[leg].get(k, '-')):>18}" for leg in legs))
