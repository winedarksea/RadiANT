"""Pass B5 bench gate (2026-11-19): one on-air window with the Pro Micro as the HR master
(hrtx_promicro_lp_pd, #12345, already flashed and running) and two receivers in the same
window: the TI (crcfix_ti_rx, COM11 @57600) and the L15 (apps/dongle direct c2gate_l15,
COM4 @115200). The Pro Micro's CDC console (power-diag line every 10 s) is captured by a
separate b5_cdc_capture.py process for the whole session. TI 0xF7 snapshot before/after.

Usage: python b5_arm.py LABEL OUTDIR [--seconds N]
"""
import argparse, os, random, subprocess, time

PY = r"C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe"
REPO = r"C:\Users\Colin\ant_dongle"
G1 = os.path.join(REPO, "archive", "benchmarks", "2026-11-18-power-g1", "scripts")

ap = argparse.ArgumentParser()
ap.add_argument("label"); ap.add_argument("outdir")
ap.add_argument("--seconds", type=float, default=600)
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
rx = {}
log(f"receivers start, {secs} s")
for name, dev in (("l15", ["--port", "COM4", "--baud", "115200"]),
                  ("ti", ["--port", "COM11", "--baud", "57600"])):
    fo = open(os.path.join(out, f"{lab}_verify_{name}.txt"), "w")
    rx[name] = (subprocess.Popen([PY, "-u", os.path.join(G1, "g1_verify.py")] + common + dev +
                                 ["--record", os.path.join(out, f"{lab}_verify_{name}.rec"),
                                  "--json", os.path.join(out, f"{lab}_verify_{name}.json")],
                                 stdout=fo, stderr=subprocess.STDOUT, cwd=REPO), fo)
for name, (p, fo) in rx.items():
    rc = p.wait(); fo.close(); log(f"verify {name} rc {rc}")
ti_diag("post")
log("arm done")
