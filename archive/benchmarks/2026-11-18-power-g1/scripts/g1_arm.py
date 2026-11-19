"""Gate G1: one arm of the nRF54L15 RADIANT_NRF_LOW_POWER A/B/A.

Flash the L15 (hr_transmitter, HR master #12345), wait a random offset, then
run two receivers for --rx-seconds while J-Link sessions (scripts/jlink_peek.ps1
only) sample the L15:

  (B, right after the flash: radiant_nrf_lp_stats dump, Sboot_lp)
  S0  power_diag dump + 10 register samples        (start of the window)
  --  no debugger for a random 90-130 s            (unattached span)
  S1  radiant_nrf_lp_stats dump + 10 samples       (B only)
  S2  power_diag dump + N samples, ~3.2 s apart    (the main sampling run)
      or, with --burst: S2_0..S2_5, 25 samples each at 150-450 ms, 40-70 s apart
  S3  radiant_nrf_lp_stats dump + 10 samples       (B only)
  S4  power_diag dump + 10 samples                 (end of the window)

Usage: python g1_arm.py LABEL BUILD_DIR OUTDIR [--lp] [--no-flash]
"""
import argparse
import os
import random
import subprocess
import time

PY = r"C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe"
REPO = r"C:\Users\Colin\ant_dongle"
HERE = os.path.dirname(os.path.abspath(__file__))
SERIAL = "001057737173"
PEEK = os.path.join(REPO, "scripts", "jlink_peek.ps1")
FLASH = os.path.join(REPO, "scripts", "flash_sim_jlink.ps1")

ap = argparse.ArgumentParser()
ap.add_argument("label")
ap.add_argument("build")
ap.add_argument("outdir")
ap.add_argument("--lp", action="store_true", help="image has radiant_nrf_lp_stats")
ap.add_argument("--no-flash", action="store_true")
ap.add_argument("--rx-seconds", type=float, default=600)
ap.add_argument("--main-samples", type=int, default=130)
ap.add_argument("--burst", action="store_true", help="S2 as short bursts (see below)")
ap.add_argument("--bursts", type=int, default=6)
ap.add_argument("--ti-port", default="COM11")
a = ap.parse_args()
out = os.path.abspath(a.outdir)
os.makedirs(out, exist_ok=True)
lab = a.label
hexp = os.path.join(REPO, "build", a.build, "hr_transmitter", "zephyr", "zephyr.hex")
elf = os.path.join(REPO, "build", a.build, "hr_transmitter", "zephyr", "zephyr.elf")
plan = open(os.path.join(out, f"{lab}_plan.txt"), "a", buffering=1)


def log(msg):
    line = f"{time.strftime('%H:%M:%S')} {msg}"
    print(line, flush=True)
    plan.write(line + "\n")


def ps(cmd, outfile):
    """Run a PowerShell command line from the repo; Write-Host (stream 6) into outfile."""
    # A command that cannot even start (bad path) must not read as rc 0.
    full = (f"$ErrorActionPreference = 'Stop'; $global:LASTEXITCODE = 99; "
            f"& {{ {cmd} }} 6>&1 | Out-File -Encoding ascii '{outfile}'; exit $LASTEXITCODE")
    r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", full],
                       cwd=REPO)
    return r.returncode


def peek(tag, samples, symbol=None, interval=None, jitter=None):
    j = os.path.join(out, f"{lab}_{tag}.json")
    cmd = (f"& '{PEEK}' -Preset nrf54l15 -Serial {SERIAL} -Samples {samples} "
           f"-ElfPath '{elf}'")
    if symbol:
        cmd += f" -Symbol {symbol}"
    else:
        cmd += " -Decode"
    if interval:
        cmd += f" -IntervalMs {interval} -JitterMs {jitter}"
    cmd += f" -Json '{j}'"
    t = time.time()
    rc = ps(cmd, os.path.join(out, f"{lab}_{tag}_report.txt"))
    log(f"peek {tag}: rc {rc}, {time.time() - t:.1f} s")
    return rc


log(f"arm {lab} build {a.build} lp={a.lp}")
if not a.no_flash:
    rc = ps(f"& '{FLASH}' -HexPath '{hexp}' -Device nRF54L15_M33 -Serial {SERIAL}",
            os.path.join(out, f"{lab}_flash.txt"))
    log(f"flash rc {rc}")
    if rc != 0:
        raise SystemExit("flash failed")
    if a.lp:
        # Right after boot: shows whether the since-boot gauges (max_disagree_us)
        # were set at start-up rather than in the measured window.
        peek("Sboot_lp", 10, symbol="radiant_nrf_lp_stats")
offset = random.uniform(20.0, 50.0)
log(f"start offset {offset:.1f} s")
time.sleep(offset)

secs = f"{a.rx_seconds:.0f}"
common = ["--profile", "heart-rate", "--device-number", "12345", "--seconds", secs, "--expect-bpm", "120"]
rx = {}
t0 = time.time()
log(f"receivers start, {secs} s")
for name, dev in (("promicro", ["--serial", "EE6986854FD72803"]),
                  ("ti", ["--port", a.ti_port, "--baud", "57600"])):
    fo = open(os.path.join(out, f"{lab}_verify_{name}.txt"), "w")
    rx[name] = (subprocess.Popen([PY, "-u", os.path.join(HERE, "g1_verify.py")] + common + dev +
                                 ["--record", os.path.join(out, f"{lab}_verify_{name}.rec"),
                                  "--json", os.path.join(out, f"{lab}_verify_{name}.json")],
                                 stdout=fo, stderr=subprocess.STDOUT, cwd=REPO), fo)

time.sleep(random.uniform(2.0, 10.0))
peek("S0", 10)
gap = random.uniform(90.0, 130.0)
log(f"unattached gap {gap:.1f} s")
time.sleep(gap)
if a.lp:
    peek("S1_lp", 10, symbol="radiant_nrf_lp_stats")
if a.burst:
    # B's first run lost the SWD link a few reads into S2 (3.2 s spacing, see
    # README): sample in short bursts at jlink_peek's default 150-450 ms
    # spacing instead, spread at random over the rest of the window.
    for i in range(a.bursts):
        peek(f"S2_{i}", 25)
        if i < a.bursts - 1:
            g = random.uniform(40.0, 70.0)
            log(f"burst gap {g:.1f} s")
            time.sleep(g)
else:
    peek("S2", a.main_samples, interval=3200, jitter=1500)
if a.lp:
    peek("S3_lp", 10, symbol="radiant_nrf_lp_stats")
peek("S4", 10)
log(f"J-Link done at +{time.time() - t0:.0f} s")
for name, (p, fo) in rx.items():
    rc = p.wait()
    fo.close()
    log(f"verify {name} rc {rc}")
log("arm done")
