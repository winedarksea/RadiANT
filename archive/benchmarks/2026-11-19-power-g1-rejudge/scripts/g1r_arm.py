"""G1 re-judge (2026-11-19), from the G1 H3 h3_arm.py: one on-air arm.

Flash the L15 (scripts/flash_sim_jlink.ps1), wait a random 20-50 s, read the
L15 over SWD (scripts/jlink_peek.ps1 only: radiant_power_diag + 10 register
samples = S0; for an LP image also radiant_nrf_lp_stats = S1_lp), snapshot the
TI's 0xF7 counters, run both receivers (the G1 wrapper around ant_verify) for
--seconds, snapshot the TI again, then read the L15 again (S3_lp, S4). No SWD
during the receive window.

Usage: python g1r_arm.py LABEL BUILD OUTDIR [--lp] [--no-flash] [--seconds N]
"""
import argparse, os, random, subprocess, time

PY = r"C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe"
REPO = r"C:\Users\Colin\ant_dongle"
G1 = os.path.join(REPO, "archive", "benchmarks", "2026-11-18-power-g1", "scripts")
SERIAL = "001057737173"
PEEK = os.path.join(REPO, "scripts", "jlink_peek.ps1")
FLASH = os.path.join(REPO, "scripts", "flash_sim_jlink.ps1")

ap = argparse.ArgumentParser()
ap.add_argument("label"); ap.add_argument("build"); ap.add_argument("outdir")
ap.add_argument("--lp", action="store_true")
ap.add_argument("--no-flash", action="store_true")
ap.add_argument("--seconds", type=float, default=600)
a = ap.parse_args()
out = os.path.abspath(a.outdir); os.makedirs(out, exist_ok=True)
lab = a.label
zd = a.build if os.path.isabs(a.build) else os.path.join(REPO, "build", a.build, "hr_transmitter", "zephyr")
plan = open(os.path.join(out, f"{lab}_plan.txt"), "a", buffering=1)


def log(m):
    line = f"{time.strftime('%H:%M:%S')} {m}"; print(line, flush=True); plan.write(line + "\n")


def ps(cmd, outfile):
    full = (f"$ErrorActionPreference = 'Stop'; $global:LASTEXITCODE = 99; "
            f"& {{ {cmd} }} 6>&1 | Out-File -Encoding ascii '{outfile}'; exit $LASTEXITCODE")
    return subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", full],
                          cwd=REPO).returncode


def peek(tag, symbol=None, samples=1):
    cmd = (f"& '{PEEK}' -Preset nrf54l15 -Serial {SERIAL} -Samples {samples} "
           f"-ElfPath '{os.path.join(zd, 'zephyr.elf')}'")
    if symbol:
        cmd += f" -Symbol {symbol}"
    cmd += f" -Json '{os.path.join(out, f'{lab}_{tag}.json')}'"
    rc = ps(cmd, os.path.join(out, f"{lab}_{tag}_report.txt"))
    log(f"peek {tag} rc {rc}")


def ti_diag(tag):
    j = os.path.join(out, f"{lab}_tidiag_{tag}.json")
    with open(os.path.join(out, f"{lab}_tidiag_{tag}.txt"), "w") as fo:
        rc = subprocess.run([PY, os.path.join(REPO, "tools", "ant_diag.py"), "--port", "COM11",
                             "--baud", "57600", "--json", j], stdout=fo, stderr=subprocess.STDOUT,
                            cwd=REPO, timeout=120).returncode
    log(f"ti diag {tag} rc {rc}")


log(f"arm {lab} build {a.build} lp={a.lp} hex={os.path.join(zd, 'zephyr.hex')}")
if not a.no_flash:
    rc = ps(f"& '{FLASH}' -HexPath '{os.path.join(zd, 'zephyr.hex')}' -Device nRF54L15_M33 -Serial {SERIAL}",
            os.path.join(out, f"{lab}_flash.txt"))
    log(f"flash rc {rc}")
    if rc != 0:
        raise SystemExit("flash failed")
off = random.uniform(20.0, 50.0); log(f"start offset {off:.1f} s"); time.sleep(off)
peek("S0", samples=10)
if a.lp:
    peek("S1_lp", symbol="radiant_nrf_lp_stats")
ti_diag("pre")
secs = f"{a.seconds:.0f}"
common = ["--profile", "heart-rate", "--device-number", "12345", "--seconds", secs, "--expect-bpm", "120"]
rx = {}
log(f"receivers start, {secs} s")
for name, dev in (("promicro", ["--serial", "EE6986854FD72803"]),
                  ("ti", ["--port", "COM11", "--baud", "57600"])):
    fo = open(os.path.join(out, f"{lab}_verify_{name}.txt"), "w")
    rx[name] = (subprocess.Popen([PY, "-u", os.path.join(G1, "g1_verify.py")] + common + dev +
                                 ["--record", os.path.join(out, f"{lab}_verify_{name}.rec"),
                                  "--json", os.path.join(out, f"{lab}_verify_{name}.json")],
                                 stdout=fo, stderr=subprocess.STDOUT, cwd=REPO), fo)
for name, (p, fo) in rx.items():
    rc = p.wait(); fo.close(); log(f"verify {name} rc {rc}")
ti_diag("post")
if a.lp:
    peek("S3_lp", symbol="radiant_nrf_lp_stats")
peek("S4", samples=10)
log("arm done")
