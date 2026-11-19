"""Gate G2, one arm: flash an apps/trainer image on the L15, run fec_ack_sweep (TI as
the FE-C slave), then inbound bursts, with jlink_peek register/struct/lp_stats reads
and the trainer's reply-turnaround histogram (`ackfault turn`, bench instrumentation).

usage: python g2_arm.py LABEL BUILD_DIR OUTDIR [--lp] [--no-flash] --sim-port COM3
"""
import argparse
import os
import random
import subprocess
import sys
import time

PY = r"C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe"
WT = r"C:\wt_power_g2"
REPO = r"C:\Users\Colin\ant_dongle"
HERE = os.path.dirname(os.path.abspath(__file__))
SERIAL = "001057737173"
PEEK = os.path.join(REPO, "scripts", "jlink_peek.ps1")
FLASH = os.path.join(REPO, "scripts", "flash_sim_jlink.ps1")
sys.path.insert(0, os.path.join(WT, "tools"))
from fec_ack_sweep import SimShell  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("label")
ap.add_argument("build")
ap.add_argument("outdir")
ap.add_argument("--lp", action="store_true")
ap.add_argument("--no-flash", action="store_true")
ap.add_argument("--sim-port", required=True)
ap.add_argument("--ti-port", default="COM11")
ap.add_argument("--count", type=int, default=300)
ap.add_argument("--bursts", type=int, default=50)
ap.add_argument("--packets", type=int, default=8)
ap.add_argument("--peek-bursts", type=int, default=4)
a = ap.parse_args()
out = os.path.abspath(a.outdir)
os.makedirs(out, exist_ok=True)
lab = a.label
hexp = os.path.join(WT, "build", a.build, "trainer", "zephyr", "zephyr.hex")
elf = os.path.join(WT, "build", a.build, "trainer", "zephyr", "zephyr.elf")
plan = open(os.path.join(out, f"{lab}_plan.txt"), "a", buffering=1)


def log(msg):
    line = f"{time.strftime('%H:%M:%S')} {msg}"
    print(line, flush=True)
    plan.write(line + "\n")


def ps(cmd, outfile):
    full = (f"$ErrorActionPreference = 'Stop'; $global:LASTEXITCODE = 99; "
            f"& {{ {cmd} }} 6>&1 | Out-File -Encoding ascii '{outfile}'; exit $LASTEXITCODE")
    r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", full],
                       cwd=REPO)
    return r.returncode


def peek(tag, samples, symbol=None):
    j = os.path.join(out, f"{lab}_{tag}.json")
    cmd = (f"& '{PEEK}' -Preset nrf54l15 -Serial {SERIAL} -Samples {samples} "
           f"-ElfPath '{elf}'")
    cmd += f" -Symbol {symbol}" if symbol else " -Decode"
    cmd += f" -Json '{j}'"
    t = time.time()
    rc = ps(cmd, os.path.join(out, f"{lab}_{tag}_report.txt"))
    log(f"peek {tag}: rc {rc}, {time.time() - t:.1f} s")
    return rc


def sim_cmd(tag, cmds):
    sim = SimShell(a.sim_port)
    try:
        with open(os.path.join(out, f"{lab}_{tag}.txt"), "w", encoding="utf-8") as fh:
            for c in cmds:
                until = "TURN-END" if c.startswith("ackfault turn") else None
                txt = sim.run(c, until=until, timeout=8.0)
                fh.write(f"### {c}\n{txt}\n")
                if "TURN " in txt:
                    for ln in txt.splitlines():
                        if ln.startswith("TURN "):
                            log(f"{tag}: {ln.strip()}")
                if c == "ackfault stats":
                    log(f"{tag}: {txt.strip().splitlines()[-1] if txt.strip() else '(none)'}")
    finally:
        sim.close()


log(f"arm {lab} build {a.build} lp={a.lp}")
if not a.no_flash:
    rc = ps(f"& '{FLASH}' -HexPath '{hexp}' -Device nRF54L15_M33 -Serial {SERIAL}",
            os.path.join(out, f"{lab}_flash.txt"))
    log(f"flash rc {rc}")
    if rc != 0:
        raise SystemExit("flash failed")
    time.sleep(4.0)
    if a.lp:
        peek("Sboot_lp", 10, symbol="radiant_nrf_lp_stats")

sim_cmd("sim_pre", ["ackfault show", "ackfault preset ideal", "ackfault turn reset", "ackfault stats"])
peek("S0", 10)
if a.lp:
    peek("S1_lp", 10, symbol="radiant_nrf_lp_stats")

# --- the ACK sweep (TI = pinned bidirectional FE-C slave = originator) ---
sweep_log = open(os.path.join(out, f"{lab}_sweep.txt"), "w")
cmd = [PY, "-u", os.path.join(WT, "tools", "fec_ack_sweep.py"), "--sim-port", a.sim_port,
       "--port", a.ti_port, "--baud", "57600", "--preset", "ideal", "--count", str(a.count),
       "--json", os.path.join(out, f"{lab}_sweep.json")]
log("sweep: " + " ".join(cmd))
t0 = time.time()
sw = subprocess.Popen(cmd, stdout=sweep_log, stderr=subprocess.STDOUT, cwd=WT)
for i in range(a.peek_bursts):
    time.sleep(random.uniform(40.0, 70.0))
    if sw.poll() is not None:
        break
    peek(f"S2_{i}", 25)
rc = sw.wait()
sweep_log.close()
log(f"sweep rc {rc}, {time.time() - t0:.0f} s")
if a.lp:
    peek("S3_lp", 10, symbol="radiant_nrf_lp_stats")
sim_cmd("sim_sweep", ["ackfault turn reset", "ackfault stats"])

# --- inbound bursts ---
bl = open(os.path.join(out, f"{lab}_burst.txt"), "w")
cmd = [PY, "-u", os.path.join(HERE, "g2_burst.py"), "--port", a.ti_port, "--baud", "57600",
       "--bursts", str(a.bursts), "--packets", str(a.packets),
       "--json", os.path.join(out, f"{lab}_burst.json")]
log("bursts: " + " ".join(cmd))
rc = subprocess.run(cmd, stdout=bl, stderr=subprocess.STDOUT, cwd=WT).returncode
bl.close()
log(f"bursts rc {rc}")
sim_cmd("sim_burst", ["ackfault turn", "ackfault stats"])
if a.lp:
    peek("S5_lp", 10, symbol="radiant_nrf_lp_stats")
peek("S4", 10)
log("arm done")
