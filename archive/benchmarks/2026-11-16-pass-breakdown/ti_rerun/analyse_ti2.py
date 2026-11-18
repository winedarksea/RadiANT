"""Summarise the ti_rerun legs: TI diag diff (pages 0,3,4,5,7-12), TI/PM 0xF6
after, TI vs Pro Micro same-window tracked loss, window facts, master counts,
flash md5. Writes summary.json and table.txt next to this file.

    python analyse_ti2.py leg1_Os:A leg2_O2:B ...
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, r"C:\wt_c2\tools")
import ant_diag  # noqa: E402

PAGES = ("0", "3", "4", "5", "7", "8", "9", "10", "11", "12")


def load(p):
    with open(p, encoding="utf-8") as f:
        return json.load(f)


def health(p):
    try:
        t = open(p, encoding="utf-8").read()
        return json.loads(t[t.index("{"):t.rindex("}") + 1])
    except (OSError, ValueError):
        return None


def scan_summary(p):
    if not os.path.exists(p):
        return None
    s = load(p)
    for c in s["dongles"].values():
        if isinstance(c, dict) and "summary" in c:
            sm = c["summary"]
            return {"seconds": sm["seconds"], "opens": sm["opens"],
                    "tracked": {k: {"loss_pct": v["loss_pct"],
                                    **{x: v[x] for x in ("received", "rx_fail") if x in v}}
                                for k, v in sm["tracked"].items()}}
        return {"error": c if not isinstance(c, dict) else c.get("error", str(c)[:300])}
    return None


def leg_row(name, image):
    d = os.path.join(HERE, name)
    meta = load(os.path.join(d, "meta.json"))
    row = {"leg": name, "image": image}
    fl = open(os.path.join(d, "flash.log"), encoding="ascii", errors="replace").read()
    m = re.search(r"^md5 (\w+)", fl, re.M)
    row["flash_md5"] = m.group(1).lower() if m else None
    row["flash_probe_pass"] = bool(re.search(r"^probe rc=0", fl, re.M))
    row["count_pre"] = {k: len(v) for k, v in meta.get("count_pre", {}).items() if not k.startswith("_")}
    row["count_post"] = {k: len(v) for k, v in meta.get("count_post", {}).items() if not k.startswith("_")}
    row["count_pre_devices"] = meta.get("count_pre", {}).get("_all_devices")
    row["count_post_devices"] = meta.get("count_post", {}).get("_all_devices")
    row["window"] = meta.get("window")
    row["steps_nonzero_rc"] = [(s["label"], s["rc"]) for s in meta["steps"] if s["rc"] != 0]
    for who in ("ti", "pm"):
        try:
            b = load(os.path.join(d, f"{who}_diag_before.json"))["pages"]
            a = load(os.path.join(d, f"{who}_diag_after.json"))["pages"]
            df = ant_diag.diff(b, a)
            row[f"{who}_pages"] = {p: df[p]["fields"] for p in PAGES if p in df}
        except (OSError, KeyError, ValueError) as exc:
            row[f"{who}_pages"] = {"error": repr(exc)}
        row[f"{who}_health_before"] = health(os.path.join(d, f"{who}_health_before.txt"))
        row[f"{who}_health_after"] = health(os.path.join(d, f"{who}_health_after.txt"))
        row[f"{who}_scan"] = scan_summary(os.path.join(d, f"scan_{who}.json"))
    txt = os.path.join(d, "scan_ti.txt")
    if os.path.exists(txt):
        lines = open(txt, encoding="utf-8", errors="replace").read().splitlines()
        row["ti_scan_no_answer_lines"] = [ln for ln in lines if "no answer" in ln.lower()]
    return row


def main():
    rows = [leg_row(*a.split(":", 1)) for a in sys.argv[1:]]
    with open(os.path.join(HERE, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(rows, f, indent=1, default=str)

    fields = [("8", f) for f in ("crc_max_us", "frame_max_us", "rx_cb_max_us", "drain_max_frames",
                                  "pass_max_us", "pass_steps_max", "pass_over_100us", "drain_over_250us")]
    fields += [("9", f) for f in ("sweep_max_us", "decide_max_us", "arm_call_max_us", "done_cb_max_us",
                                  "book_max_us", "pass_to_arm_max_us", "arm_slack_min_us", "early_etime")]
    fields += [("10", f) for f in ("crc_repaired", "crc_unrepairable", "crc_repair_refuted",
                                   "crc_repair_implausible", "late_isr_drops", "end_late_max_us",
                                   "t_sync_max_us", "match_max_us")]
    fields += [("7", f) for f in ("srch_cut_close", "srch_cut_abort", "srch_end_near_close", "trk_open_late",
                                  "trk_open_late_max_us", "trk_missed_behind_scan", "tx_start_missed",
                                  "rx_start_missed")]
    fields += [("11", f) for f in ("windows_merged", "preempted", "replans", "arm_rejected", "tx_armed",
                                   "stale_events", "slots_collided", "turn_yields")]
    fields += [("12", f) for f in ("stale_rx_frame", "stale_rx_end", "stale_rx_filter_index", "stale_tx",
                                   "stale_ed")]
    fields += [("0", f) for f in ("sched_dones", "sched_missed", "slots_missed")]
    fields += [("4", f) for f in ("drain_max_us", "min_lead_us", "crc_fail")]
    fields += [("5", f) for f in ("windows", "frames_ok", "acquired")]
    out = []
    hdr = f"{'field':34}" + "".join(f"{r['leg'] + ' ' + r['image']:>18}" for r in rows)
    out.append(hdr)

    def get(r, p, f):
        v = r["ti_pages"].get(p, {}).get(f, "-") if isinstance(r["ti_pages"], dict) else "-"
        return "n/m" if v is None else v

    for p, f in fields:
        out.append(f"{'p' + p + ' ' + f:34}" + "".join(f"{str(get(r, p, f)):>18}" for r in rows))
    for k in ("host_csum", "host_desync", "uart_rx_overruns", "tx_high_water", "tx_drops", "tx_timeout",
              "event_overflows", "sched_missed"):
        out.append(f"{'0xF6 ' + k + ' (after)':34}" + "".join(
            f"{str((r['ti_health_after'] or {}).get(k, '-')):>18}" for r in rows))
    out.append("")
    out.append("Window: rc / secs (TI, PM); TI scan opens/seconds; master counts pre/post (Pro Micro 20 s)")
    for r in rows:
        w = r["window"] or {}
        ti, pm = r["ti_scan"] or {}, r["pm_scan"] or {}
        out.append(f"  {r['leg']}: TI rc={w.get('scan_ti', {}).get('rc')} {w.get('scan_ti', {}).get('secs', 0):.1f}s "
                   f"PM rc={w.get('scan_pm', {}).get('rc')} {w.get('scan_pm', {}).get('secs', 0):.1f}s | "
                   f"TI opens {ti.get('opens')} {ti.get('seconds')} s, PM opens {pm.get('opens')} {pm.get('seconds')} s | "
                   f"pre {r['count_pre']} post {r['count_post']} | md5 {r['flash_md5']} | "
                   f"no-answer lines {len(r.get('ti_scan_no_answer_lines', []))} | nonzero rc {r['steps_nonzero_rc']}")
    out.append("")
    out.append("Tracked loss % (TI / Pro Micro / TI-PM), same window")
    keys = []
    for r in rows:
        for s in (r["ti_scan"], r["pm_scan"]):
            if s and "tracked" in s:
                keys += [k for k in s["tracked"] if k not in keys]
    for k in keys:
        cells = []
        for r in rows:
            t = (r["ti_scan"] or {}).get("tracked", {}).get(k, {}).get("loss_pct")
            q = (r["pm_scan"] or {}).get("tracked", {}).get(k, {}).get("loss_pct")
            cells.append("-" if t is None or q is None else f"{t:.2f}/{q:.2f}/{t - q:+.2f}")
        out.append(f"  {k:14}" + "".join(f"{c:>24}" for c in cells))
    txt = "\n".join(out)
    with open(os.path.join(HERE, "table.txt"), "w", encoding="utf-8") as f:
        f.write(txt + "\n")
    print(txt)


if __name__ == "__main__":
    main()
