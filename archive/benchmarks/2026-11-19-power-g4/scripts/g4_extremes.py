"""Where do start_err min/max (-33/+31 in the hour) come from? (2026-11-19)
1) --steady N: N single radiant_nrf_lp_stats snapshots of the running node (start_err_last).
2) --reboot: reflash the same hex (scripts/flash_sim_jlink.ps1 = reset), read lp stats
   repeatedly for --adv-s seconds with no central (advertising only), then connect the host
   central (scripts/ble_central.ps1 connect) and keep reading for --conn-s seconds.
Usage: python g4_extremes.py OUTDIR ELFDIR [--steady N] [--reboot]"""
import argparse, json, os, subprocess, sys, time
REPO = r"C:\Users\Colin\ant_dongle"
sys.path.insert(0, os.path.join(REPO, "archive", "benchmarks", "2026-11-18-power-g1", "scripts"))
from g1_lpdecode import decode  # noqa: E402
PEEK = os.path.join(REPO, "scripts", "jlink_peek.ps1")
FLASH = os.path.join(REPO, "scripts", "flash_sim_jlink.ps1")
ap = argparse.ArgumentParser()
ap.add_argument("outdir"); ap.add_argument("elfdir")
ap.add_argument("--steady", type=int, default=0); ap.add_argument("--reboot", action="store_true")
ap.add_argument("--adv-s", type=int, default=100); ap.add_argument("--conn-s", type=int, default=90)
a = ap.parse_args()
out = os.path.abspath(a.outdir); os.makedirs(out, exist_ok=True)
rows = open(os.path.join(out, "extremes.tsv"), "a", buffering=1)
n = [0]


def ps(cmd, outfile):
    full = (f"$ErrorActionPreference = 'Stop'; $global:LASTEXITCODE = 99; & {{ {cmd} }} 6>&1 | "
            f"Out-File -Encoding ascii '{outfile}'; exit $LASTEXITCODE")
    return subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", full],
                          cwd=REPO).returncode


def snap(phase, t0):
    n[0] += 1
    j = os.path.join(out, f"x{n[0]:03d}_{phase}.json")
    rc = ps(f"& '{PEEK}' -Preset nrf54l15 -Serial 001057737173 -ElfPath "
            f"'{os.path.join(a.elfdir, 'zephyr.elf')}' -Symbol radiant_nrf_lp_stats -Json '{j}'",
            j.replace(".json", "_report.txt"))
    try:
        s = json.load(open(j, encoding="utf-8-sig"))["diag"]["snapshots"][0]["words"]
        d = decode(s)
        line = (f"{phase}\t{time.time() - t0:.1f}\t{rc}\t{d['start_err_n']}\t{d['start_err_min_us']}\t"
                f"{d['start_err_max_us']}\t{d['start_err_last_us']}\t{d['missed_releases']}\t{d['sleeps']}")
    except Exception as e:  # noqa: BLE001
        line = f"{phase}\t{time.time() - t0:.1f}\t{rc}\tERR {e!r}"
    print(line, flush=True); rows.write(line + "\n")


rows.write("phase\tt_s\trc\tstart_err_n\tmin_us\tmax_us\tlast_us\tmissed_releases\tsleeps\n")
t0 = time.time()
for _ in range(a.steady):
    snap("steady", t0)
if a.reboot:
    hexp = os.path.join(a.elfdir, "zephyr.hex")
    print("flash rc", ps(f"& '{FLASH}' -HexPath '{hexp}' -Device nRF54L15_M33 -Serial 001057737173",
                         os.path.join(out, "flash.txt")), flush=True)
    t0 = time.time()
    while time.time() - t0 < a.adv_s:
        snap("adv", t0)
    bf = open(os.path.join(out, "ble_central.txt"), "w")
    ble = subprocess.Popen(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                            os.path.join(REPO, "scripts", "ble_central.ps1"), "-Mode", "connect",
                            "-Seconds", str(a.conn_s + 30)], stdout=bf, stderr=subprocess.STDOUT, cwd=REPO)
    t1 = time.time()
    while time.time() - t1 < a.conn_s:
        snap("conn", t0)
    ble.wait(timeout=a.conn_s + 200)
    subprocess.run(["taskkill", "/F", "/IM", "ble_central.exe"], capture_output=True)
print("done", flush=True)
