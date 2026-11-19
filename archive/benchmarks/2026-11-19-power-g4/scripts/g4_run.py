"""Gate G4 runner (2026-11-19). L15 = hrm_ble node_ble_lp_pd + ble_nosuppress (already
flashed). Console COM3 (DTR) captured throughout; the Windows host radio holds a BLE
connection (scripts/ble_central.ps1 -Mode connect, subscribed to HRS) for the whole
window; the TI (COM11, crcfix_ti_rx) tracks the node's ANT+ HR channel in --segs
segments of --seg-s seconds. SWD (scripts/jlink_peek.ps1 only) between segments:
radiant_nrf_lp_stats each gap, radiant_power_diag + 10 register samples at start/end.
Usage: python g4_run.py OUTDIR ELFDIR [--segs 6] [--seg-s 600]"""
import argparse, datetime, os, re, subprocess, threading, time
import serial

PY = r"C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe"
REPO = r"C:\Users\Colin\ant_dongle"
SERIAL = "001057737173"
PEEK = os.path.join(REPO, "scripts", "jlink_peek.ps1")
ap = argparse.ArgumentParser()
ap.add_argument("outdir"); ap.add_argument("elfdir")
ap.add_argument("--dev", type=int, default=4242)
ap.add_argument("--segs", type=int, default=6); ap.add_argument("--seg-s", type=int, default=600)
a = ap.parse_args()
out = os.path.abspath(a.outdir); os.makedirs(out, exist_ok=True)
elf = os.path.join(a.elfdir, "zephyr.elf")
planf = open(os.path.join(out, "plan.txt"), "a", buffering=1)


def log(m):
    line = f"{time.strftime('%H:%M:%S')} {m}"; print(line, flush=True); planf.write(line + "\n")


class Console:
    def __init__(self, port, path):
        self.port, self.path, self.ev, self.deaths, self.lines = port, path, threading.Event(), [], []
        self.th = threading.Thread(target=self.run, daemon=True); self.th.start()

    def run(self):
        with open(self.path, "a", encoding="utf-8", errors="replace") as f:
            buf = b""
            while not self.ev.is_set():
                try:
                    s = serial.Serial(self.port, 115200, timeout=0.2); s.dtr = True; s.rts = True
                    f.write(f"[{datetime.datetime.now().isoformat()}] [cap] opened\n"); f.flush()
                    while not self.ev.is_set():
                        d = s.read(4096)
                        if d:
                            buf += d
                            while b"\n" in buf:
                                ln, buf = buf.split(b"\n", 1)
                                t = ln.decode('utf-8', 'replace').rstrip()
                                self.lines.append(t)
                                ts = datetime.datetime.now().isoformat(timespec="milliseconds")
                                f.write(f"[{ts}] {t}\n")
                            f.flush()
                    s.close()
                except Exception as e:
                    self.deaths.append(datetime.datetime.now().isoformat())
                    f.write(f"[{datetime.datetime.now().isoformat()}] [cap] PORT DIED {e!r}\n"); f.flush()
                    time.sleep(2)

    def stop(self):
        self.ev.set(); self.th.join(5); return self.deaths


def ps(cmd, outfile):
    full = (f"$ErrorActionPreference = 'Stop'; $global:LASTEXITCODE = 99; "
            f"& {{ {cmd} }} 6>&1 | Out-File -Encoding ascii '{outfile}'; exit $LASTEXITCODE")
    return subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", full],
                          cwd=REPO).returncode


def peek(tag, symbol=None, samples=1):
    cmd = f"& '{PEEK}' -Preset nrf54l15 -Serial {SERIAL} -Samples {samples} -ElfPath '{elf}'"
    if symbol:
        cmd += f" -Symbol {symbol}"
    cmd += f" -Json '{os.path.join(out, tag + '.json')}'"
    log(f"peek {tag} rc {ps(cmd, os.path.join(out, tag + '_report.txt'))}")


def ti_diag(tag):
    with open(os.path.join(out, f"tidiag_{tag}.txt"), "w") as fo:
        rc = subprocess.run([PY, os.path.join(REPO, "tools", "ant_diag.py"), "--port", "COM11", "--baud",
                             "57600", "--json", os.path.join(out, f"tidiag_{tag}.json")],
                            stdout=fo, stderr=subprocess.STDOUT, cwd=REPO, timeout=120).returncode
    log(f"ti diag {tag} rc {rc}")


con = Console("COM3", os.path.join(out, "l15_console.log"))
total = a.segs * a.seg_s
ap2 = None
dev = a.dev


def start_ble(n):
    f = open(os.path.join(out, f"ble_central_{n}.txt"), "w", encoding="utf-8")
    p = subprocess.Popen(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                          os.path.join(REPO, "scripts", "ble_central.ps1"), "-Mode", "connect",
                          "-Name", "RadiANT HR", "-Seconds", str(total + 600)],
                         stdout=f, stderr=subprocess.STDOUT, cwd=REPO)
    log(f"ble_central attempt {n} started, hold {total + 600} s")
    t0 = time.time()
    while time.time() - t0 < 400:
        if "subscribed" in open(os.path.join(out, f"ble_central_{n}.txt"), encoding="utf-8",
                                errors="replace").read():
            return p
        if p.poll() is not None:
            break
        time.sleep(2)
    log(f"ble_central attempt {n} failed rc={p.poll()}")
    if p.poll() is None:
        p.kill()
    return None


ble = None
for n in range(3):
    ble = start_ble(n)
    if ble:
        break
    time.sleep(10)
if not ble:
    raise SystemExit("no BLE connection after 3 attempts")
log(f"ble subscribed; console says connected={'BLE connected' in chr(10).join(con.lines)}; device {dev}")
time.sleep(5)
peek("S0_pd", samples=10)
peek("S0_lp", symbol="radiant_nrf_lp_stats")
ti_diag("pre")
for i in range(a.segs):
    log(f"segment {i} start; ble alive={ble.poll() is None}")
    with open(os.path.join(out, f"seg{i}_ti.txt"), "w") as fo:
        rc = subprocess.run([PY, os.path.join(REPO, "tools", "ant_verify.py"), "--profile", "heart-rate",
                             "--device-number", str(dev), "--port", "COM11", "--baud", "57600",
                             "--seconds", str(a.seg_s), "--json", os.path.join(out, f"seg{i}_ti.json"),
                             "--record", os.path.join(out, f"seg{i}_ti.rec")],
                            stdout=fo, stderr=subprocess.STDOUT, cwd=REPO, timeout=a.seg_s + 300).returncode
    log(f"segment {i} ti rc {rc}")
    peek(f"S{i + 1}_lp", symbol="radiant_nrf_lp_stats")
ti_diag("post")
peek("Send_pd", samples=10)
log(f"ble alive at end={ble.poll() is None}")
ble.terminate()
subprocess.run(["taskkill", "/F", "/IM", "ble_central.exe"], capture_output=True)
log(f"console deaths {con.stop()}")
log("done")
