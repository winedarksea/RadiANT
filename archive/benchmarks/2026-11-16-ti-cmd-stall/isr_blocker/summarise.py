"""Tabulate lat.json + 0xF7 before/after for each run dir given."""
import json
import os
import sys

MAXF = {"drain_max_us", "min_lead_us", "pass_max_us", "rx_cb_max_us", "frame_max_us",
        "crc_max_us", "pass_steps_max", "trk_open_late_max_us", "drain_max_frames",
        "pass_to_arm_max_us", "arm_slack_min_us", "t_sync_max_us", "match_max_us",
        "turn_min_us", "turn_max_us", "tx_margin_min_us"}


def fields(path):
    d = json.load(open(path))
    out = {}
    for p in d["pages"].values():
        for k, v in p["fields"].items():
            out[f"p{p['page']}.{k}"] = v
    return out


def main():
    rows = {}
    for run in sys.argv[1:]:
        lat = json.load(open(os.path.join(run, "lat.json")))
        a = fields(os.path.join(run, "ti_diag_after.json"))
        b = fields(os.path.join(run, "ti_diag_before.json"))
        r = {"tracks": len(lat["tracks"]), "lat_max_us": lat["lat"]["max_us"],
             "lat_hist": lat["lat"]["hist"], "samples": lat["lat"]["samples"],
             "sites": [(s["pc"], s["lr"], s["ipsr"], s["max_us"], s["n"])
                       for s in lat["lat"]["sites"]]}
        for k, v in a.items():
            name = k.split(".", 1)[1]
            if name in MAXF or not isinstance(v, int) or not isinstance(b.get(k), int):
                r[k] = v
            else:
                r[k] = v - b[k]
        rx = lat.get("rx", {})
        r["rx_frames"] = {k: v for k, v in rx.items() if isinstance(k, int) or str(k).isdigit()}
        r["rx_fail"] = {k: v for k, v in rx.items() if str(k).startswith("ev")}
        rows[os.path.basename(run.rstrip("/\\"))] = r
    json.dump(rows, open(os.path.join(os.path.dirname(sys.argv[1].rstrip("/\\")), "summary.json"), "w"), indent=1)
    keys = ["tracks", "lat_max_us", "lat_hist", "p8.pass_max_us", "p8.pass_over_100us",
            "p0.sched_dones", "p0.sched_missed", "p0.slots_missed", "p8.rx_cb_max_us",
            "p8.drain_over_250us", "p4.drain_max_us", "p4.min_lead_us", "p9.pb_sweep_max_us",
            "p9.pass_to_arm_max_us", "p9.arm_slack_min_us", "p9.early_etime",
            "p7.trk_open_late", "p7.trk_open_late_max_us", "p11.arm_rejected"]
    for k in keys:
        print(f"{k:28}", " | ".join(str(rows[r].get(k, "-")) for r in rows))
    for r in rows:
        print(r, rows[r]["sites"], rows[r]["rx_fail"])


if __name__ == "__main__":
    main()
