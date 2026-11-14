#!/usr/bin/env python3
"""Step-3 soak (assert 69:108): L15 thread.conf DUT attached to the 5340 OT leader,
Pro Micro as ant_sim power master, DUT ch0 tracks it + ch1 searches an absent device,
Garmin tracks the same master as a same-window reference, DUT console captured.
Adapted from the phase-0a soak_arm.py."""
import argparse, datetime, json, os, subprocess, sys, threading, time
import serial

TOOLS = r"C:\wt_mpsl\tools"
PY = sys.executable


class ResilientConsole:
    def __init__(self, port, path):
        self.port, self.path = port, path
        self.stop_ev = threading.Event()
        self.deaths = []
        self.th = threading.Thread(target=self.run, daemon=True)
        self.th.start()

    def run(self):
        with open(self.path, "a", encoding="utf-8", errors="replace") as f:
            buf = b""
            while not self.stop_ev.is_set():
                try:
                    s = serial.Serial(self.port, 115200, timeout=0.2)
                    s.dtr = True
                    s.rts = True
                    f.write(f"[{datetime.datetime.now().isoformat()}] [cap] opened {self.port}\n")
                    f.flush()
                    while not self.stop_ev.is_set():
                        data = s.read(4096)
                        if data:
                            buf += data
                            while b"\n" in buf:
                                line, buf = buf.split(b"\n", 1)
                                ts = datetime.datetime.now().isoformat(timespec="milliseconds")
                                f.write(f"[{ts}] {line.decode('utf-8', 'replace').rstrip()}\n")
                            f.flush()
                    s.close()
                except Exception as e:
                    self.deaths.append(datetime.datetime.now().isoformat())
                    f.write(f"[{datetime.datetime.now().isoformat()}] [cap] PORT DIED: {e!r}\n")
                    f.flush()
                    time.sleep(2)

    def stop(self):
        self.stop_ev.set()
        self.th.join(5)
        return {"deaths": self.deaths}


def peer_cmds(port, cmds, path, note):
    out = [f"== {note} {datetime.datetime.now().isoformat()}"]
    try:
        s = serial.Serial(port, 115200, timeout=0.3)
        s.dtr = True
        s.rts = True
        s.reset_input_buffer()
        for c in cmds:
            s.write((c + "\n").encode())
            s.flush()
            t0 = time.time()
            buf = b""
            while time.time() - t0 < 1.5:
                buf += s.read(4096)
            out.append(f"$ {c}\n" + buf.decode("utf-8", "replace"))
        s.close()
    except Exception as e:
        out.append(f"PEER ERROR: {e!r}")
    text = "\n".join(out)
    with open(path, "a", encoding="utf-8") as f:
        f.write(text + "\n")
    print(text, flush=True)
    return text


ap = argparse.ArgumentParser()
ap.add_argument("--base", required=True)
ap.add_argument("--seconds", type=int, default=7200)
ap.add_argument("--hex", required=True)
ap.add_argument("--devnum", type=int, default=2002)
ap.add_argument("--absent", type=int, default=43210)
ap.add_argument("--attach-wait", type=int, default=75)
ap.add_argument("--peer-port", default="COM6")
ap.add_argument("--ant-port", default="COM4")
ap.add_argument("--log-port", default="COM3")
ap.add_argument("--master-serial", default="EE6986854FD72803")
args = ap.parse_args()

base = args.base
os.makedirs(os.path.dirname(base), exist_ok=True)
summ = {"hex": args.hex, "seconds": args.seconds, "base": base,
        "start": datetime.datetime.now().isoformat()}
peer_log = base + "-peer.log"
peer_cmds(args.peer_port, ["ot state", "ot channel", "ot child table"], peer_log, "peer before flash")

rc = subprocess.call(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                      r"C:\wt_mpsl\scripts\flash_sim_jlink.ps1", "-HexPath", args.hex,
                      "-Device", "nRF54L15_M33", "-Serial", "1057737173"])
print("flash rc", rc, flush=True)
if rc != 0:
    sys.exit(rc)

cap = ResilientConsole(args.log_port, base + "-console.log")
print(f"waiting {args.attach_wait}s for DUT boot + attach", flush=True)
time.sleep(args.attach_wait)
summ["peer_after_boot"] = peer_cmds(args.peer_port, ["ot state", "ot child table"], peer_log,
                                    "peer after boot wait")

sim_log = open(base + "-sim.log", "w", encoding="utf-8")
sim = subprocess.Popen([PY, "-u", os.path.join(TOOLS, "ant_sim.py"), "--serial", args.master_serial,
                        "--profile", "power", "--device-number", str(args.devnum),
                        "--seconds", str(args.seconds + 150)],
                       stdout=sim_log, stderr=subprocess.STDOUT, cwd=TOOLS)
time.sleep(10)

dut_log = open(base + "-dut.log", "w", encoding="utf-8")
ref_log = open(base + "-ref.log", "w", encoding="utf-8")
t0 = time.time()
dut_cmd = [PY, os.path.join(TOOLS, "ant_verify.py"), "--profile", "power", "--profile", "power",
           "--channel", "0", "--device-number", str(args.devnum),
           "--channel", "1", "--device-number", str(args.absent),
           "--port", args.ant_port, "-q"]
dut = subprocess.Popen(dut_cmd + ["--seconds", str(args.seconds), "--json", base + "-dut.json"],
                       stdout=dut_log, stderr=subprocess.STDOUT, cwd=TOOLS)
ref = subprocess.Popen([PY, os.path.join(TOOLS, "ant_verify.py"), "--profile", "power",
                        "--device-number", str(args.devnum), "--serial", "006",
                        "--seconds", str(args.seconds), "--json", base + "-ref.json", "-q"],
                       stdout=ref_log, stderr=subprocess.STDOUT, cwd=TOOLS)
segments = 0
while True:
    dut_rc = dut.wait()
    remaining = args.seconds - (time.time() - t0)
    if dut_rc == 0 or remaining < 60:
        print(f"dut segment {segments} ended rc={dut_rc}, remaining {remaining:.0f}s", flush=True)
        break
    segments += 1
    print(f"DUT verify ended rc={dut_rc}; restarting segment {segments} with {remaining:.0f}s left", flush=True)
    time.sleep(25)
    remaining = args.seconds - (time.time() - t0)
    dut = subprocess.Popen(dut_cmd + ["--seconds", str(int(remaining)),
                                      "--json", base + f"-dut-seg{segments}.json"],
                           stdout=dut_log, stderr=subprocess.STDOUT, cwd=TOOLS)
summ["dut_segments"] = segments
ref_rc = ref.wait()
sim.terminate()
try:
    sim.wait(timeout=10)
except subprocess.TimeoutExpired:
    sim.kill()
peer_cmds(args.peer_port, ["ot state", "ot child table"], peer_log, "peer after")
time.sleep(8)
summ.update(dut_rc=dut_rc, ref_rc=ref_rc, cap=str(cap.stop()), elapsed=time.time() - t0,
            end=datetime.datetime.now().isoformat())
with open(base + "-summary.json", "w", encoding="utf-8") as f:
    json.dump(summ, f, indent=2)
print("DONE", base, flush=True)
