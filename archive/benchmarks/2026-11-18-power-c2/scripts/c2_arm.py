"""C2 gate: one arm. Capture the TI console from boot (gc1_capture.py:
open COM11 with DTR/RTS, then pulse xds110reset), wait a random offset, then
run the two receivers concurrently for --rx-seconds:

  Pro Micro (USB EE6986854FD72803): tools/ant_verify.py heart-rate #12345
  nRF54L15 DK (apps/dongle, COM4):   c2_acked_probe.py (acked into the TI's RX)

Usage: python c2_arm.py LABEL OUTDIR [--rx-seconds 600]
"""
import argparse
import os
import random
import subprocess
import sys
import time

PY = r"C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe"
REPO = r"C:\Users\Colin\ant_dongle"
HERE = os.path.dirname(os.path.abspath(__file__))
RESET = r"C:\ti\ccs_base\common\uscif\xds110\xds110reset.exe"

ap = argparse.ArgumentParser()
ap.add_argument("label")
ap.add_argument("outdir")
ap.add_argument("--rx-seconds", type=float, default=600)
ap.add_argument("--reset", default=RESET)
ap.add_argument("--l15-port", default="COM4")
ap.add_argument("--l15-baud", type=int, default=115200)
a = ap.parse_args()
a.outdir = os.path.abspath(a.outdir)
os.makedirs(a.outdir, exist_ok=True)
lab = a.label
offset = random.uniform(25.0, 55.0)
cap_secs = offset + a.rx_seconds + 25.0

with open(os.path.join(a.outdir, f"{lab}_plan.txt"), "w") as f:
    f.write(f"start {time.strftime('%Y-%m-%d %H:%M:%S')}\noffset_s {offset:.1f}\n"
            f"rx_seconds {a.rx_seconds}\ncapture_s {cap_secs:.1f}\n")

cap = subprocess.Popen([PY, os.path.join(HERE, "gc1_capture.py"), "COM11", "115200",
                        f"{cap_secs:.1f}", os.path.join(a.outdir, f"{lab}_ti_console.log"),
                        a.reset])
time.sleep(offset)
t_rx = time.time()
with open(os.path.join(a.outdir, f"{lab}_plan.txt"), "a") as f:
    f.write(f"receivers start {time.strftime('%H:%M:%S', time.localtime(t_rx))} "
            f"(capture +{offset:.1f} s)\n")
with open(os.path.join(a.outdir, f"{lab}_verify_promicro.txt"), "w") as vo, \
        open(os.path.join(a.outdir, f"{lab}_acked_l15.txt"), "w") as ao:
    ver = subprocess.Popen([PY, os.path.join(REPO, "tools", "ant_verify.py"), "--profile", "heart-rate",
                            "--device-number", "12345", "--seconds", f"{a.rx_seconds:.0f}",
                            "--serial", "EE6986854FD72803", "--expect-bpm", "120",
                            "--record", os.path.join(a.outdir, f"{lab}_verify.rec"),
                            "--json", os.path.join(a.outdir, f"{lab}_verify_promicro.json")],
                           stdout=vo, stderr=subprocess.STDOUT, cwd=REPO)
    ack = subprocess.Popen([PY, os.path.join(HERE, "c2_acked_probe.py"), "--port", a.l15_port,
                            "--baud", str(a.l15_baud), "--seconds", f"{a.rx_seconds:.0f}",
                            "--label", lab, "--json", os.path.join(a.outdir, f"{lab}_acked_l15.json")],
                           stdout=ao, stderr=subprocess.STDOUT, cwd=REPO)
    rv, ra = ver.wait(), ack.wait()
rc = cap.wait()
with open(os.path.join(a.outdir, f"{lab}_plan.txt"), "a") as f:
    f.write(f"end {time.strftime('%H:%M:%S')} verify rc {rv} acked rc {ra} capture rc {rc}\n")
print(f"{lab}: verify rc {rv}, acked rc {ra}, capture rc {rc}")
