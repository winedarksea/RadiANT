"""G4 same-image control (2026-11-19), run straight after g4_run.py with the BLE central
gone: the node keeps advertising (no connection), the TI tracks #4242 for --seconds, with
radiant_nrf_lp_stats read before and after. Usage: python g4_ctrl.py OUTDIR ELFDIR"""
import os, subprocess, sys, time
PY = r"C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe"
REPO = r"C:\Users\Colin\ant_dongle"
PEEK = os.path.join(REPO, "scripts", "jlink_peek.ps1")
out, elfdir = os.path.abspath(sys.argv[1]), sys.argv[2]
secs = int(sys.argv[3]) if len(sys.argv) > 3 else 600
os.makedirs(out, exist_ok=True)


def peek(tag, symbol=None, samples=1):
    cmd = (f"& '{PEEK}' -Preset nrf54l15 -Serial 001057737173 -Samples {samples} "
           f"-ElfPath '{os.path.join(elfdir, 'zephyr.elf')}'" + (f" -Symbol {symbol}" if symbol else "") +
           f" -Json '{os.path.join(out, tag + '.json')}'")
    full = (f"$ErrorActionPreference = 'Stop'; $global:LASTEXITCODE = 99; & {{ {cmd} }} 6>&1 | "
            f"Out-File -Encoding ascii '{os.path.join(out, tag + '_report.txt')}'; exit $LASTEXITCODE")
    print(tag, subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", full],
                              cwd=REPO).returncode, flush=True)


peek("C0_lp", "radiant_nrf_lp_stats")
with open(os.path.join(out, "ctrl_ti.txt"), "w") as fo:
    rc = subprocess.run([PY, os.path.join(REPO, "tools", "ant_verify.py"), "--profile", "heart-rate",
                         "--device-number", "4242", "--port", "COM11", "--baud", "57600", "--seconds",
                         str(secs), "--json", os.path.join(out, "ctrl_ti.json")],
                        stdout=fo, stderr=subprocess.STDOUT, cwd=REPO).returncode
print("ctrl ti rc", rc, flush=True)
peek("C1_lp", "radiant_nrf_lp_stats")
peek("C1_pd", samples=10)
print("done", flush=True)
