"""Gate G2 Garmin rerun, one arm: flash a saved G2 apps/trainer image on the L15, then run
phases with a chosen slave, clearing the trainer's reply-turnaround histogram
(`ackfault turn`, G2 bench instrumentation) after each phase so every phase has its own.

Phases (comma list, in order):
  gsweep  fec_ack_sweep, Garmin (USB serial 006) as the FE-C slave, page 51 acked at 1 Hz
  gburst  g2g_burst.py, Garmin, inbound bursts of 8
  tsweep  fec_ack_sweep, TI (COM11 @57600)
  tburst  g2g_burst.py, TI

usage: python g2g_arm.py LABEL IMAGE OUTDIR --phases gsweep,gburst,tburst [--lp] [--no-flash]
IMAGE is g2_A or g2_B under build/power_images_2026-11-19/.
"""
import argparse
import os
import subprocess
import sys
import time

PY = r"C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe"
REPO = r"C:\Users\Colin\ant_dongle"
HERE = os.path.dirname(os.path.abspath(__file__))
SERIAL = "001057737173"
PEEK = os.path.join(REPO, "scripts", "jlink_peek.ps1")
FLASH = os.path.join(REPO, "scripts", "flash_sim_jlink.ps1")
IMAGES = os.path.join(REPO, "build", "power_images_2026-11-19")
sys.path.insert(0, os.path.join(REPO, "tools"))
from fec_ack_sweep import SimShell  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("label")
ap.add_argument("image")
ap.add_argument("outdir")
ap.add_argument("--phases", required=True)
ap.add_argument("--lp", action="store_true")
ap.add_argument("--no-flash", action="store_true")
ap.add_argument("--sim-port", default="COM3")
ap.add_argument("--garmin", default="006")
ap.add_argument("--ti-port", default="COM11")
ap.add_argument("--count", type=int, default=300)
ap.add_argument("--bursts", type=int, default=100)
ap.add_argument("--ti-bursts", type=int, default=50)
ap.add_argument("--ti-count", type=int, default=100)
ap.add_argument("--packets", type=int, default=8)
a = ap.parse_args()
out = os.path.abspath(a.outdir)
os.makedirs(out, exist_ok=True)
lab = a.label
hexp = os.path.join(IMAGES, a.image, "trainer_zephyr_zephyr.hex")
elf = os.path.join(IMAGES, a.image, "trainer_zephyr_zephyr.elf")
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
                for ln in txt.splitlines():
                    if ln.startswith("TURN "):
                        log(f"{tag}: {ln.strip()}")
                if c == "ackfault stats":
                    log(f"{tag}: {txt.strip().splitlines()[-1] if txt.strip() else '(none)'}")
    finally:
        sim.close()


def slave_args(who):
    return ["--serial", a.garmin] if who == "g" else ["--port", a.ti_port, "--baud", "57600"]


def run_phase(ph):
    who, kind = ph[0], ph[1:]
    logf = open(os.path.join(out, f"{lab}_{ph}.txt"), "w")
    js = os.path.join(out, f"{lab}_{ph}.json")
    if kind == "sweep":
        n = a.count if who == "g" else a.ti_count
        cmd = [PY, "-u", os.path.join(REPO, "tools", "fec_ack_sweep.py"), "--sim-port", a.sim_port,
               *slave_args(who), "--preset", "ideal", "--count", str(n), "--json", js]
    elif kind == "burst":
        n = a.bursts if who == "g" else a.ti_bursts
        cmd = [PY, "-u", os.path.join(HERE, "g2g_burst.py"), *slave_args(who),
               "--bursts", str(n), "--packets", str(a.packets), "--json", js]
    else:
        raise SystemExit(f"unknown phase {ph}")
    log(f"{ph}: " + " ".join(cmd))
    t0 = time.time()
    rc = subprocess.run(cmd, stdout=logf, stderr=subprocess.STDOUT, cwd=REPO).returncode
    logf.close()
    log(f"{ph} rc {rc}, {time.time() - t0:.0f} s")
    sim_cmd(f"sim_{ph}", ["ackfault turn reset", "ackfault stats"])


log(f"arm {lab} image {a.image} lp={a.lp} phases={a.phases}")
if not a.no_flash:
    rc = ps(f"& '{FLASH}' -HexPath '{hexp}' -Device nRF54L15_M33 -Serial {SERIAL}",
            os.path.join(out, f"{lab}_flash.txt"))
    log(f"flash rc {rc}")
    if rc != 0:
        raise SystemExit("flash failed")
    time.sleep(4.0)

sim_cmd("sim_pre", ["ackfault show", "ackfault preset ideal", "ackfault turn reset", "ackfault stats"])
peek("S0", 10)
if a.lp:
    peek("S1_lp", 10, symbol="radiant_nrf_lp_stats")
for ph in a.phases.split(","):
    run_phase(ph.strip())
if a.lp:
    peek("S5_lp", 10, symbol="radiant_nrf_lp_stats")
peek("S4", 10)
log("arm done")
