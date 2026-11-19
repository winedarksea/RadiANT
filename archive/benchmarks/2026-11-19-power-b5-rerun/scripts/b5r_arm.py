"""Pass B5 rerun (2026-11-19): one on-air window with the Pro Micro as HR master #12345 and
up to three receivers in the same window: TI (crcfix_ti_rx, COM11 @57600), L15 (apps/dongle
direct, COM4 @115200) and, with --garmin, the Garmin USB-m stick (serial 006). TI 0xF7
snapshot before/after. Derived from ../../2026-11-19-power-b5-promicro/scripts/b5_arm.py.

Usage: python b5r_arm.py LABEL OUTDIR [--seconds N] [--garmin]
"""
import argparse, os, random, subprocess, time

PY = r"C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe"
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
G1 = os.path.join(REPO, "archive", "benchmarks", "2026-11-18-power-g1", "scripts")

ap = argparse.ArgumentParser()
ap.add_argument("label"); ap.add_argument("outdir")
ap.add_argument("--seconds", type=float, default=600)
ap.add_argument("--garmin", action="store_true")
a = ap.parse_args()
out = os.path.abspath(a.outdir); os.makedirs(out, exist_ok=True)
lab = a.label
plan = open(os.path.join(out, f"{lab}_plan.txt"), "a", buffering=1)


def log(m):
    line = f"{time.strftime('%H:%M:%S')} {m}"; print(line, flush=True); plan.write(line + "\n")


def ti_diag(tag):
    j = os.path.join(out, f"{lab}_tidiag_{tag}.json")
    with open(os.path.join(out, f"{lab}_tidiag_{tag}.txt"), "w") as fo:
        rc = subprocess.run([PY, os.path.join(REPO, "tools", "ant_diag.py"), "--port", "COM11",
                             "--baud", "57600", "--json", j], stdout=fo, stderr=subprocess.STDOUT,
                            cwd=REPO, timeout=120).returncode
    log(f"ti diag {tag} rc {rc}")


off = random.uniform(20.0, 50.0); log(f"arm {lab} start offset {off:.1f} s"); time.sleep(off)
ti_diag("pre")
secs = f"{a.seconds:.0f}"
common = ["--profile", "heart-rate", "--device-number", "12345", "--seconds", secs, "--expect-bpm", "120"]
rxs = [("l15", ["--port", "COM4", "--baud", "115200"]),
       ("ti", ["--port", "COM11", "--baud", "57600"])]
if a.garmin:
    rxs.append(("garmin", ["--serial", "006"]))
rx = {}
log(f"receivers start, {secs} s: {', '.join(n for n, _ in rxs)}")
for name, dev in rxs:
    fo = open(os.path.join(out, f"{lab}_verify_{name}.txt"), "w")
    rx[name] = (subprocess.Popen([PY, "-u", os.path.join(G1, "g1_verify.py")] + common + dev +
                                 ["--record", os.path.join(out, f"{lab}_verify_{name}.rec"),
                                  "--json", os.path.join(out, f"{lab}_verify_{name}.json")],
                                 stdout=fo, stderr=subprocess.STDOUT, cwd=REPO), fo)
for name, (p, fo) in rx.items():
    rc = p.wait(); fo.close(); log(f"verify {name} rc {rc}")
ti_diag("post")
log("arm done")
