"""Key Pro Micro diag-diff fields per leg. python diag_table.py <legdir>..."""
import re, sys, os
F = ["rx_posted", "term_ok", "term_empty", "term_error", "crc_fail", "sw_addr_mismatch", "crc_repaired",
     "crc_unrepairable", "slots_missed", "windows_merged", "trk_open_late", "late_isr_drops",
     "arm_call_max_us", "pass_to_arm_max_us", "early_etime", "rx_start_missed"]
rows = {}
for d in sys.argv[1:]:
    v = {}
    page = ""
    for line in open(os.path.join(d, "pm_diag_diff.txt"), encoding="utf-8", errors="replace"):
        m = re.match(r"page (\d+)", line)
        if m:
            page = m.group(1)
            continue
        m = re.match(r"\s+(\w+)\s+(\S+)", line)
        if m and m.group(1) in F:
            k = m.group(1) if not (m.group(1) == "crc_fail" and page != "4") else None
            if k and k not in v:
                v[k] = m.group(2)
    rows[os.path.basename(d.rstrip("/\\"))] = v
names = list(rows)
print("%-20s" % "field" + "".join("%14s" % n[:13] for n in names))
for f in F:
    print("%-20s" % f + "".join("%14s" % rows[n].get(f, "-") for n in names))
