"""Render the B4 pair table from summary.json (analyse.py output).

    python table.py summary.json > table.txt
"""
import json
import math
import sys

legs = json.load(open(sys.argv[1], encoding="utf-8"))
names = [l["leg"] for l in legs]


def val(l, page, field, kind="after"):
    return l["pages"][page][kind].get(field) if kind != "diff" else l["pages"][page]["diff"].get(field)


ROWS = [
    ("p8 pass_max_us", "8", "pass_max_us", "after"),
    ("p8 pass_steps_max", "8", "pass_steps_max", "after"),
    ("p8 pass_over_100us", "8", "pass_over_100us", "diff"),
    ("p0 sched_dones", "0", "sched_dones", "diff"),
    ("p9[0] sweep_max_us", "9", "sweep_max_us", "after"),
    ("p9[1] decide_max_us", "9", "decide_max_us", "after"),
    ("p9[2] arm_call_max_us", "9", "arm_call_max_us", "after"),
    ("p9[3] done_cb_max_us", "9", "done_cb_max_us", "after"),
    ("p9[4] book_max_us", "9", "book_max_us", "after"),
    ("p9[5] pass_to_arm_max_us", "9", "pass_to_arm_max_us", "after"),
    ("p9[6] arm_slack_min_us", "9", "arm_slack_min_us", "after"),
    ("p9[7] early_etime", "9", "early_etime", "diff"),
    ("p7 srch_cut_close", "7", "srch_cut_close", "diff"),
    ("p7 srch_end_near_close", "7", "srch_end_near_close", "diff"),
    ("p7 trk_open_late", "7", "trk_open_late", "diff"),
    ("p7 trk_open_late_max_us", "7", "trk_open_late_max_us", "after"),
    ("p7 trk_missed_behind_scan", "7", "trk_missed_behind_scan", "diff"),
    ("p7 tx_start_missed", "7", "tx_start_missed", "diff"),
    ("p7 rx_start_missed", "7", "rx_start_missed", "diff"),
    ("p10 crc_repaired", "10", "crc_repaired", "diff"),
    ("p10 crc_unrepairable", "10", "crc_unrepairable", "diff"),
    ("p10 crc_repair_refuted", "10", "crc_repair_refuted", "diff"),
    ("p10 crc_repair_implausible", "10", "crc_repair_implausible", "diff"),
    ("p10 late_isr_drops", "10", "late_isr_drops", "diff"),
    ("p10 end_late_max_us", "10", "end_late_max_us", "after"),
    ("p11 windows_merged", "11", "windows_merged", "diff"),
    ("p11 preempted", "11", "preempted", "diff"),
    ("p11 replans", "11", "replans", "diff"),
    ("p11 arm_rejected", "11", "arm_rejected", "diff"),
    ("p11 stale_events", "11", "stale_events", "diff"),
    ("p11 slots_collided", "11", "slots_collided", "diff"),
    ("p0 sched_missed", "0", "sched_missed", "diff"),
    ("p0 slots_missed", "0", "slots_missed", "diff"),
    ("p2 sched_arm_rejected", "2", "sched_arm_rejected", "diff"),
    ("p3 rx_posted", "3", "rx_posted", "diff"),
    ("p3 rx_refused", "3", "rx_refused", "diff"),
    ("p4 crc_fail", "4", "crc_fail", "diff"),
]


def fmt(v):
    if isinstance(v, float):
        return f"{v:.2f}"
    return "-" if v is None else str(v)


w = 28
print("field".ljust(w) + "".join(n.rjust(10) for n in names))
for label, page, field, kind in ROWS:
    try:
        cells = [fmt(val(l, page, field, kind)) for l in legs]
    except KeyError:
        cells = ["n/a"] * len(legs)
    print(label.ljust(w) + "".join(c.rjust(10) for c in cells))
print()
print("B4 estimate [5]-[0]-[1]".ljust(w) + "".join(fmt(l["b4_estimate_us"]).rjust(10) for l in legs))
print("C4 candidate".ljust(w) + "".join(fmt(l["c4_candidate_us"]).rjust(10) for l in legs))
print()
print("0xF6 tx_high_water b->a".ljust(w) + "".join(f"{l['health']['tx_high_water']['before']}->{l['health']['tx_high_water']['after']}".rjust(10) for l in legs))
print("0xF6 tx_drops b->a".ljust(w) + "".join(f"{l['health']['tx_drops']['before']}->{l['health']['tx_drops']['after']}".rjust(10) for l in legs))
print()
print("scan seconds".ljust(w) + "".join(fmt(l["scan"]["seconds"]).rjust(10) for l in legs))
print("scan opens".ljust(w) + "".join(fmt(l["scan"]["opens"]).rjust(10) for l in legs))
print("scan sightings/s".ljust(w) + "".join(fmt(l["scan"]["sightings_per_s"]).rjust(10) for l in legs))
keys = list(legs[0]["scan"]["tracked"].keys())
for k in keys:
    print(f"loss % {k}".ljust(w) + "".join(fmt(l["scan"]["tracked"].get(k, {}).get("loss_pct")).rjust(10) for l in legs))
for k in keys:
    print(f"acct short {k}".ljust(w) + "".join(fmt(l["scan"]["accounting"].get(k, {}).get("short")).rjust(10) for l in legs))
print()
for l in legs:
    print(l["leg"], "pre", {b: len(v) for b, v in l["count_pre"].items()},
          "post", {b: len(v) for b, v in l["count_post"].items()}, "post_full", l["count_post_full"],
          "window", {k: round(v["secs"], 1) for k, v in (l["window"] or {}).items()},
          "sims", {k: (v["rc"], round(v["secs"], 1)) for k, v in (l["sims"] or {}).items()})
