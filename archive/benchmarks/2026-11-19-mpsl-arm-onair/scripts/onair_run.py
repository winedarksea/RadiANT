"""MPSL arm-guard on-air check (2026-11-19). L15 = ackps_l15_mpsl (apps/dongle_thread
gate.conf + uart_57600 + diag_counters, built at dd28bde), already flashed.
Console COM3 captured (DTR) for the whole run; L15 0xF7 diag before/mid/after.
Phase 1: tracking + search: L15 ch0 tracks Pro Micro HR #12345, ch1 searches absent
#43210; TI (COM11) tracks #12345 in the same window as reference.
Phase 2: acknowledged data with the TI as partner (ant_ack_pair, device 2002):
  a) L15 master receives slave-originated acked (gated runner, 60 sends)
  b) L15 slave originates acked to the TI master (Zwift's FE-C path)
  c) L15 master originates acked (--master-acked 60)
Usage: python onair_run.py OUTDIR [--seconds 300]"""
import argparse, datetime, os, subprocess, sys, threading, time
import serial

PY = r"C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe"
REPO = r"C:\Users\Colin\ant_dongle"
TOOLS = os.path.join(REPO, "tools")
ap = argparse.ArgumentParser()
ap.add_argument("outdir"); ap.add_argument("--seconds", type=int, default=300)
ap.add_argument("--sends", type=int, default=60)
a = ap.parse_args()
out = os.path.abspath(a.outdir); os.makedirs(out, exist_ok=True)
planf = open(os.path.join(out, "plan.txt"), "a", buffering=1)


def log(m):
    line = f"{time.strftime('%H:%M:%S')} {m}"; print(line, flush=True); planf.write(line + "\n")


class Console:
    def __init__(self, port, path):
        self.port, self.path, self.ev, self.deaths = port, path, threading.Event(), []
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
                                ts = datetime.datetime.now().isoformat(timespec="milliseconds")
                                f.write(f"[{ts}] {ln.decode('utf-8', 'replace').rstrip()}\n")
                            f.flush()
                    s.close()
                except Exception as e:
                    self.deaths.append(datetime.datetime.now().isoformat())
                    f.write(f"[{datetime.datetime.now().isoformat()}] [cap] PORT DIED {e!r}\n"); f.flush()
                    time.sleep(2)

    def stop(self):
        self.ev.set(); self.th.join(5); return self.deaths


def run(name, cmd, timeout=None):
    with open(os.path.join(out, name + ".txt"), "w", encoding="utf-8") as fo:
        rc = subprocess.run(cmd, stdout=fo, stderr=subprocess.STDOUT, cwd=REPO, timeout=timeout).returncode
    log(f"{name} rc {rc}"); return rc


def diag(tag, port="COM4"):
    run(f"diag_{'l15' if port == 'COM4' else 'ti'}_{tag}",
        [PY, os.path.join(TOOLS, "ant_diag.py"), "--port", port, "--baud", "57600",
         "--json", os.path.join(out, f"diag_{'l15' if port == 'COM4' else 'ti'}_{tag}.json")], 120)


con = Console("COM3", os.path.join(out, "l15_console.log"))
time.sleep(1)
diag("pre"); diag("pre", "COM11")
s = str(a.seconds)
log(f"phase 1 start, {s} s")
dut = subprocess.Popen([PY, os.path.join(TOOLS, "ant_verify.py"), "--profile", "heart-rate",
                        "--profile", "heart-rate", "--channel", "0", "--device-number", "12345",
                        "--channel", "1", "--device-number", "43210", "--port", "COM4", "--baud", "57600",
                        "--seconds", s, "--json", os.path.join(out, "p1_l15.json")],
                       stdout=open(os.path.join(out, "p1_l15.txt"), "w"), stderr=subprocess.STDOUT, cwd=REPO)
ref = subprocess.Popen([PY, os.path.join(TOOLS, "ant_verify.py"), "--profile", "heart-rate",
                        "--device-number", "12345", "--port", "COM11", "--baud", "57600",
                        "--seconds", s, "--json", os.path.join(out, "p1_ti.json")],
                       stdout=open(os.path.join(out, "p1_ti.txt"), "w"), stderr=subprocess.STDOUT, cwd=REPO)
log(f"p1 l15 rc {dut.wait()} ti rc {ref.wait()}")
diag("mid")
common = ["--device", "2002", "--master-baud", "57600", "--slave-baud", "57600"]
n = str(a.sends)
run("p2a_l15master_rx", [PY, os.path.join(TOOLS, "ant_ack_pair.py"), "--master-port", "COM4",
                         "--slave-port", "COM11", "--sends", n, "--seconds", str(a.sends + 30),
                         "--interval", "1", "--json", os.path.join(out, "p2a.json")] + common, 400)
run("p2b_l15slave_tx", [PY, os.path.join(TOOLS, "ant_ack_pair.py"), "--master-port", "COM11",
                        "--slave-port", "COM4", "--sends", n, "--seconds", str(a.sends + 30),
                        "--interval", "1", "--json", os.path.join(out, "p2b.json")] + common, 400)
run("p2c_l15master_acked", [PY, os.path.join(TOOLS, "ant_ack_pair.py"), "--master-port", "COM4",
                            "--slave-port", "COM11", "--master-acked", n,
                            "--json-bursts", os.path.join(out, "p2c.json")] + common, 400)
diag("post"); diag("post", "COM11")
log(f"console deaths {con.stop()}")
log("done")
