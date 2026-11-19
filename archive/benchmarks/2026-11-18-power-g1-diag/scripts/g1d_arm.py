"""G1 diag: one short on-air arm. Flash the L15 (unless --no-flash), wait a
random 20-50 s, snapshot the TI's 0xF7 counters, run both receivers (the G1
wrapper around ant_verify) for --seconds, snapshot the TI again, and for an
LP image dump radiant_nrf_lp_stats once (scripts/jlink_peek.ps1 only).

Usage: python g1d_arm.py LABEL BUILD_DIR OUTDIR [--lp] [--no-flash] [--seconds N]
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
ap.add_argument("--seconds", type=float, default=300)
a = ap.parse_args()
out = os.path.abspath(a.outdir); os.makedirs(out, exist_ok=True)
lab = a.label
zd = os.path.join(REPO, "build", a.build, "hr_transmitter", "zephyr")
plan = open(os.path.join(out, f"{lab}_plan.txt"), "a", buffering=1)


def log(m):
    line = f"{time.strftime('%H:%M:%S')} {m}"; print(line, flush=True); plan.write(line + "\n")


def ps(cmd, outfile):
    full = (f"$ErrorActionPreference = 'Stop'; $global:LASTEXITCODE = 99; "
            f"& {{ {cmd} }} 6>&1 | Out-File -Encoding ascii '{outfile}'; exit $LASTEXITCODE")
    return subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", full],
                          cwd=REPO).returncode


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
    rc = ps(f"& '{PEEK}' -Preset nrf54l15 -Serial {SERIAL} -Samples 10 -ElfPath '{os.path.join(zd, 'zephyr.elf')}'"
            f" -Symbol radiant_nrf_lp_stats -Json '{os.path.join(out, f'{lab}_lp.json')}'",
            os.path.join(out, f"{lab}_lp_report.txt"))
    log(f"lp peek rc {rc}")
log("arm done")
