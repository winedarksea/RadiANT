#!/usr/bin/env python3
"""Assert 69:108 bench leg runner.

Adapted from archive/benchmarks/2026-11-14-mpsl-soak/soak_run.py.  One leg =
(optional) flash of the L15 DUT, console capture, optional Thread-peer checks,
optional Pro Micro ant_sim power master, DUT ant_verify (ch0 tracked, ch1
searching an absent device), optional Garmin reference verify.  The leg ends
at the first "MPSL ASSERT" line on the DUT console (plus a short grace period
so the dump lands in the log) or when --seconds run out.

Every child process is terminated on every exit path (try/finally).  One
summary JSON per leg (<base>-summary.json) and one row per leg appended to
legs.tsv beside <base>.

Hidden: --selftest <console.log> feeds an existing capture through the same
detection code and prints the summary row (no hardware, no serial import).
"""
import argparse, datetime, hashlib, json, os, re, subprocess, sys, threading, time

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
PY = sys.executable

ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
LINE_RE = re.compile(r"^\[(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d+)?)\] ?(.*)$")
KV_RE = re.compile(r"(?<![\w.])([A-Za-z_][A-Za-z0-9_.]*)=([^\s,;]+)")
INT_RE = re.compile(r"^-?\d+$")
ASSERT_MARK = "MPSL ASSERT"
BANNER_MARK = "*** Booting"
ZLOG_NOISE = ("<inf>", "<dbg>")   # periodic Zephyr log lines: kept in the dump, not parsed for fields
TSV_COLS = ["leg", "image", "sha12", "start", "result", "since_open_s", "placed", "granted",
            "blocked", "ext", "skip", "reboots", "asserts"]


def parse_kv(text):
    """All key=value pairs on a line, in order, as (key, value); digit values become int."""
    out = []
    for k, v in KV_RE.findall(text):
        out.append((k, int(v) if INT_RE.match(v) else v))
    return out


def merge_fields(fields, pairs):
    for k, v in pairs:
        fields.setdefault(k, []).append(v)


def collapse(fields):
    """Single occurrence -> scalar; later duplicates of a key -> list in order."""
    return {k: (v[0] if len(v) == 1 else v) for k, v in fields.items()}


def secs(a, b):
    return None if a is None or b is None else round((a - b).total_seconds(), 3)


class ConsoleWatcher:
    """Consumes DUT console lines (timestamp, text).  Thread-safe."""

    def __init__(self, continue_after_assert=False, stop_on_reboot=False, window=3.0):
        self.continue_after_assert = continue_after_assert
        self.stop_on_reboot = stop_on_reboot
        self.window = window
        self.lock = threading.Lock()
        self.stop_ev = threading.Event()      # set when the leg should end
        self.first_ts = None                  # first device line (not a [cap] line)
        self.first_is_banner = None
        self.banners = 0
        self.reboots = []                     # datetimes of banners after the first
        self.asserts = []                     # dicts: ts, dump, fields (raw lists), line_fields
        self.stats = {}                       # last periodic gate stats
        self.stats_ts = None
        self.lines = 0
        self.flash_done = None                # set by the runner
        self.dut_open = None                  # set by the runner

    def feed(self, ts, text):
        clean = ANSI_RE.sub("", text).rstrip()
        if clean.startswith("[cap]"):
            return
        with self.lock:
            self.lines += 1
            if self.first_ts is None:
                self.first_ts = ts
                self.first_is_banner = BANNER_MARK in clean
            if ASSERT_MARK in clean:
                a = {"ts": ts, "dump": [clean], "fields": {}}
                merge_fields(a["fields"], parse_kv(clean))
                a["line_fields"] = collapse({k: list(v) for k, v in a["fields"].items()})
                self.asserts.append(a)
                if not self.continue_after_assert:
                    self.stop_ev.set()
            else:
                cur = self.asserts[-1] if self.asserts else None
                if cur is not None and (ts - cur["ts"]).total_seconds() <= self.window:
                    cur["dump"].append(clean)
                    if not any(m in clean for m in ZLOG_NOISE):
                        merge_fields(cur["fields"], parse_kv(clean))
            if BANNER_MARK in clean:
                self.banners += 1
                # The first banner is the flash's own boot only if the capture
                # caught it as its first line.  A capture that opened late (the
                # boot already printed) starts mid-run, so ANY banner it sees is
                # a reboot - counting only banners after the first missed it.
                if self.banners > 1 or not self.first_is_banner:
                    self.reboots.append(ts)
                    if self.stop_on_reboot:
                        self.stop_ev.set()
            # periodic gate stats: D2STAT carries pl/gr/bl/ext, D2REL carries ext_skipped
            if "D2STAT" in clean or "D2REL" in clean:
                kv = dict(parse_kv(clean))
                if "D2STAT" in clean:
                    for src, dst in (("pl", "placed"), ("gr", "granted"), ("bl", "blocked"), ("ext", "ext")):
                        if src in kv:
                            self.stats[dst] = kv[src]
                    self.stats_ts = ts
                if "ext_skipped" in kv:
                    self.stats["skip"] = kv["ext_skipped"]

    def summarize(self, end_ts=None):
        with self.lock:
            asserts = []
            for a in self.asserts:
                asserts.append({
                    "time": a["ts"].isoformat(timespec="milliseconds"),
                    "since_boot_s": secs(a["ts"], self.first_ts),
                    "since_flash_s": secs(a["ts"], self.flash_done),
                    "since_open_s": secs(a["ts"], self.dut_open),
                    "line_fields": a["line_fields"],
                    "fields": collapse(a["fields"]),
                    "dump": list(a["dump"]),
                })
            out = {
                "console_lines": self.lines,
                "first_console_line": self.first_ts.isoformat(timespec="milliseconds") if self.first_ts else None,
                "first_line_is_banner": self.first_is_banner,
                "flash_done": self.flash_done.isoformat(timespec="milliseconds") if self.flash_done else None,
                "dut_open": self.dut_open.isoformat(timespec="milliseconds") if self.dut_open else None,
                "reboots": len(self.reboots),
                "reboot_times": [t.isoformat(timespec="milliseconds") for t in self.reboots],
                "reboot_since_boot_s": [secs(t, self.first_ts) for t in self.reboots],
                "assert_count": len(asserts),
                "assert_times": [a["time"] for a in asserts],
                "asserts": asserts,
                "last_stats": dict(self.stats),
                "end_since_boot_s": secs(end_ts, self.first_ts),
                "end_since_flash_s": secs(end_ts, self.flash_done),
            }
            if asserts:
                f = asserts[0]
                out.update(assert_time=f["time"], assert_since_boot_s=f["since_boot_s"],
                           assert_since_flash_s=f["since_flash_s"], assert_since_open_s=f["since_open_s"],
                           assert_fields=f["fields"], assert_dump=f["dump"])
            return out


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def make_row(leg, label, sha, start, wsum, ended_by=None, deaths=0):
    """The one-line-per-leg legs.tsv row, from watcher.summarize().

    deaths: how many times the console capture lost its port.  A leg with any
    is marked in its result, so a console that went dark cannot read as
    "clean" (no assert seen is not no assert happened)."""
    asserts = wsum["asserts"]
    if asserts:
        a = asserts[0]
        t = a["since_boot_s"]
        if t is None:
            t = a["since_flash_s"]
        result = f"assert at {t:.1f} s" if t is not None else "assert"
        lf = a["line_fields"]
        vals = {k: lf.get(k, "") for k in ("placed", "granted", "blocked", "ext", "skip")}
        # a key repeated on the assert line itself would be a list: keep the first
        vals = {k: (v[0] if isinstance(v, list) else v) for k, v in vals.items()}
        since_open = a["since_open_s"]
    else:
        t = wsum["end_since_boot_s"]
        if t is None:
            t = wsum["end_since_flash_s"]
        if wsum["reboots"] and ended_by == "reboot":
            rt = wsum["reboot_since_boot_s"][0]
            result = f"reboot at {rt:.1f} s" if rt is not None else "reboot"
        else:
            result = f"clean at {t:.1f} s" if t is not None else "clean"
        st = wsum["last_stats"]
        vals = {k: st.get(k, "") for k in ("placed", "granted", "blocked", "ext", "skip")}
        since_open = ""
    if deaths:
        result += f" (console lost {deaths}×)"
    row = {"leg": leg, "image": label or "", "sha12": (sha or "")[:12], "start": start,
           "result": result, "since_open_s": "" if since_open is None else since_open,
           "reboots": wsum["reboots"], "asserts": wsum["assert_count"]}
    row.update(vals)
    return {c: str(row.get(c, "")).replace("\t", " ").replace("\n", " ") for c in TSV_COLS}


def append_row(path, row):
    new = not os.path.exists(path)
    with open(path, "a", encoding="utf-8", newline="") as f:
        if new:
            f.write("\t".join(TSV_COLS) + "\n")
        f.write("\t".join(row[c] for c in TSV_COLS) + "\n")


def selftest(path, args):
    w = ConsoleWatcher(args.continue_after_assert, args.stop_on_reboot, args.grace)
    last = None
    deaths = 0
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            m = LINE_RE.match(raw.rstrip("\r\n"))
            if not m:
                continue
            ts = datetime.datetime.fromisoformat(m.group(1))
            last = ts
            if "[cap] PORT DIED" in m.group(2):
                deaths += 1
            w.feed(ts, m.group(2))
    wsum = w.summarize(last)
    leg = os.path.basename(path).replace("-console.log", "")
    row = make_row(leg, args.image_label, args.image_sha256, "", wsum, deaths=deaths)
    print("selftest:", path)
    print("console_lines", wsum["console_lines"], "first_console_line", wsum["first_console_line"],
          "first_line_is_banner", wsum["first_line_is_banner"], "| port_deaths", deaths)
    print("reboots", wsum["reboots"], wsum["reboot_times"], "| assert_count", wsum["assert_count"])
    for i, a in enumerate(wsum["asserts"]):
        sb = a["since_boot_s"]
        print(f"assert[{i}] time={a['time']} since_boot={sb} s "
              f"({sb / 60:.2f} min) since_flash={a['since_flash_s']} since_open={a['since_open_s']}")
        print("  line_fields:", json.dumps(a["line_fields"]))
        print("  fields (all dump lines):", json.dumps(a["fields"]))
        print("  dump:")
        for d in a["dump"]:
            print("    " + d)
    print("last_stats:", json.dumps(wsum["last_stats"]), "end_since_boot_s", wsum["end_since_boot_s"])
    print("TSV " + "\t".join(TSV_COLS))
    print("TSV " + "\t".join(row[c] for c in TSV_COLS))
    return 0


class ResilientConsole:
    def __init__(self, port, path, watcher):
        self.port, self.path, self.watcher = port, path, watcher
        self.stop_ev = threading.Event()
        self.deaths = []
        self.th = threading.Thread(target=self.run, daemon=True)
        self.th.start()

    def run(self):
        import serial
        with open(self.path, "a", encoding="utf-8", errors="replace") as f:
            buf = b""
            while not self.stop_ev.is_set():
                s = None
                try:
                    s = serial.Serial(self.port, 115200, timeout=0.2)
                    s.dtr = True   # the log VCOM prints nothing without DTR
                    s.rts = True
                    f.write(f"[{datetime.datetime.now().isoformat()}] [cap] opened {self.port}\n")
                    f.flush()
                    while not self.stop_ev.is_set():
                        data = s.read(4096)
                        if data:
                            buf += data
                            while b"\n" in buf:
                                line, buf = buf.split(b"\n", 1)
                                now = datetime.datetime.now()
                                text = line.decode("utf-8", "replace").rstrip()
                                f.write(f"[{now.isoformat(timespec='milliseconds')}] {text}\n")
                                try:
                                    self.watcher.feed(now, text)
                                except Exception as e:
                                    f.write(f"[{now.isoformat()}] [cap] WATCHER ERROR: {e!r}\n")
                            f.flush()
                    s.close()
                except Exception as e:
                    # Close the dead handle first: on Windows a COM port that is
                    # still open in this process cannot be opened again, so every
                    # retry would fail until the leg ended.
                    if s is not None:
                        try:
                            s.close()
                        except Exception:
                            pass
                    self.deaths.append(datetime.datetime.now().isoformat())
                    f.write(f"[{datetime.datetime.now().isoformat()}] [cap] PORT DIED: {e!r}\n")
                    f.flush()
                    time.sleep(2)

    def stop(self):
        self.stop_ev.set()
        self.th.join(5)
        return {"deaths": self.deaths}


def peer_cmds(port, cmds, path, note):
    import serial
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


def stop_procs(procs):
    """Terminate (then kill) every still-running child, newest first."""
    for p in reversed(procs):
        try:
            if p.poll() is None:
                p.terminate()
        except Exception as e:
            print(f"terminate failed: {e!r}", flush=True)
    for p in reversed(procs):
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            try:
                p.kill()
                p.wait(timeout=10)
            except Exception as e:
                print(f"kill failed: {e!r}", flush=True)
        except Exception as e:
            print(f"wait failed: {e!r}", flush=True)


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    ap.add_argument("--base", help="output path prefix for this leg, e.g. D:\\out\\leg1 (required)")
    ap.add_argument("--hex", help="firmware hex to flash (required unless --no-flash)")
    ap.add_argument("--seconds", type=int, default=7200, help="leg duration if no assert")
    ap.add_argument("--devnum", type=int, default=2002, help="tracked device number (ch0, ref, master)")
    ap.add_argument("--absent", type=int, default=43210, help="absent device number searched on ch1")
    ap.add_argument("--attach-wait", type=int, default=75, help="seconds to wait for boot + Thread attach")
    ap.add_argument("--boot-wait", type=int, default=8, help="seconds to wait for boot with --no-peer")
    ap.add_argument("--peer-port", default="COM6")
    ap.add_argument("--ant-port", default="COM4")
    ap.add_argument("--log-port", default="COM3")
    ap.add_argument("--master-serial", default="EE6986854FD72803")
    ap.add_argument("--tools", default=os.path.join(REPO, "tools"),
                    help="directory with ant_sim.py / ant_verify.py")
    ap.add_argument("--flash-script", default=os.path.join(REPO, "scripts", "flash_sim_jlink.ps1"))
    ap.add_argument("--jlink-device", default="nRF54L15_M33")
    ap.add_argument("--jlink-serial", default="1057737173")
    ap.add_argument("--image-label", default="", help="free text for the summary, e.g. baseline, arm1_lock")
    ap.add_argument("--image-sha256", default="", help="default: sha256 of --hex")
    ap.add_argument("--no-peer", action="store_true",
                    help="image without Thread: skip peer commands and the attach wait")
    ap.add_argument("--no-ref", action="store_true", help="skip the Garmin reference verify")
    ap.add_argument("--no-search", action="store_true",
                    help="DUT opens only ch0 (no ch1 searching the absent device)")
    ap.add_argument("--no-master", action="store_true", help="do not start ant_sim (search-only workload)")
    ap.add_argument("--no-flash", action="store_true", help="reuse the image on the board; still records --hex")
    ap.add_argument("--stop-on-reboot", action="store_true", help="end the leg at a second boot banner")
    ap.add_argument("--continue-after-assert", action="store_true",
                    help="do not end at an assert: count asserts and run the full duration")
    ap.add_argument("--grace", type=float, default=3.0,
                    help="seconds of console kept after the assert/reboot")
    ap.add_argument("--selftest", metavar="CONSOLE_LOG", help=argparse.SUPPRESS)
    return ap


def run_leg(args, base, watcher, procs, files, summ, state):
    peer_log = base + "-peer.log"
    tools = args.tools

    if not args.no_peer:
        peer_cmds(args.peer_port, ["ot state", "ot channel", "ot child table"], peer_log, "peer before flash")

    if args.no_flash:
        print("flash skipped (--no-flash)", flush=True)
    else:
        rc = subprocess.call(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                              args.flash_script, "-HexPath", args.hex,
                              "-Device", args.jlink_device, "-Serial", args.jlink_serial])
        print("flash rc", rc, flush=True)
        summ["flash_rc"] = rc
        if rc != 0:
            state["ended_by"] = "flash failed"
            state["rc"] = rc
            return
    watcher.flash_done = datetime.datetime.now()

    state["cap"] = ResilientConsole(args.log_port, base + "-console.log", watcher)
    wait = args.boot_wait if args.no_peer else args.attach_wait
    print(f"waiting {wait}s for DUT boot" + ("" if args.no_peer else " + attach"), flush=True)
    if watcher.stop_ev.wait(wait):
        return
    if not args.no_peer:
        summ["peer_after_boot"] = peer_cmds(args.peer_port, ["ot state", "ot child table"], peer_log,
                                            "peer after boot wait")

    if not args.no_master:
        sim_log = open(base + "-sim.log", "w", encoding="utf-8")
        files.append(sim_log)
        procs.append(subprocess.Popen(
            [PY, "-u", os.path.join(tools, "ant_sim.py"), "--serial", args.master_serial,
             "--profile", "power", "--device-number", str(args.devnum),
             "--seconds", str(args.seconds + 150)],
            stdout=sim_log, stderr=subprocess.STDOUT, cwd=tools))
        if watcher.stop_ev.wait(10):
            return

    dut_log = open(base + "-dut.log", "w", encoding="utf-8")
    files.append(dut_log)
    dut_cmd = [PY, os.path.join(tools, "ant_verify.py"), "--profile", "power",
               "--channel", "0", "--device-number", str(args.devnum)]
    if not args.no_search:
        dut_cmd += ["--profile", "power", "--channel", "1", "--device-number", str(args.absent)]
    dut_cmd += ["--port", args.ant_port, "-q"]

    t0 = time.time()
    state["t0"] = t0
    watcher.dut_open = datetime.datetime.now()
    summ["dut_starts"] = [watcher.dut_open.isoformat(timespec="milliseconds")]
    dut = subprocess.Popen(dut_cmd + ["--seconds", str(args.seconds), "--json", base + "-dut.json"],
                           stdout=dut_log, stderr=subprocess.STDOUT, cwd=tools)
    procs.append(dut)
    ref = None
    if not args.no_ref:
        ref_log = open(base + "-ref.log", "w", encoding="utf-8")
        files.append(ref_log)
        ref = subprocess.Popen([PY, os.path.join(tools, "ant_verify.py"), "--profile", "power",
                                "--device-number", str(args.devnum), "--serial", "006",
                                "--seconds", str(args.seconds), "--json", base + "-ref.json", "-q"],
                               stdout=ref_log, stderr=subprocess.STDOUT, cwd=tools)
        procs.append(ref)

    segments = 0
    while True:
        while dut.poll() is None and not watcher.stop_ev.is_set():
            watcher.stop_ev.wait(0.5)
        if watcher.stop_ev.is_set():
            return
        dut_rc = dut.returncode
        state["dut_rc"] = dut_rc
        remaining = args.seconds - (time.time() - t0)
        if dut_rc == 0 or remaining < 60:
            print(f"dut segment {segments} ended rc={dut_rc}, remaining {remaining:.0f}s", flush=True)
            break
        segments += 1
        print(f"DUT verify ended rc={dut_rc}; restarting segment {segments} with {remaining:.0f}s left",
              flush=True)
        if watcher.stop_ev.wait(25):
            return
        remaining = args.seconds - (time.time() - t0)
        summ["dut_starts"].append(datetime.datetime.now().isoformat(timespec="milliseconds"))
        dut = subprocess.Popen(dut_cmd + ["--seconds", str(int(remaining)),
                                          "--json", base + f"-dut-seg{segments}.json"],
                               stdout=dut_log, stderr=subprocess.STDOUT, cwd=tools)
        procs.append(dut)
    summ["dut_segments"] = segments

    if ref is not None:
        deadline = t0 + args.seconds + 180
        while ref.poll() is None and not watcher.stop_ev.is_set() and time.time() < deadline:
            watcher.stop_ev.wait(0.5)
        if ref.poll() is not None:
            state["ref_rc"] = ref.returncode
    state["natural_end"] = not watcher.stop_ev.is_set()


def main():
    args = build_parser().parse_args()
    if args.selftest:
        return selftest(args.selftest, args)
    if not args.base:
        sys.exit("--base is required")
    if not args.hex and not args.no_flash:
        sys.exit("--hex is required unless --no-flash")

    base = os.path.abspath(args.base)
    os.makedirs(os.path.dirname(base), exist_ok=True)
    sha = args.image_sha256
    if not sha and args.hex and os.path.isfile(args.hex):
        sha = sha256_file(args.hex)
    start = datetime.datetime.now().isoformat(timespec="seconds")
    summ = {"hex": args.hex, "image_label": args.image_label, "image_sha256": sha,
            "seconds": args.seconds, "base": base, "start": start,
            "flags": {k: getattr(args, k) for k in (
                "no_peer", "no_ref", "no_search", "no_master", "no_flash",
                "stop_on_reboot", "continue_after_assert")},
            "jlink": {"device": args.jlink_device, "serial": args.jlink_serial},
            "tools": args.tools, "flash_script": args.flash_script}
    watcher = ConsoleWatcher(args.continue_after_assert, args.stop_on_reboot, args.grace)
    procs, files, state = [], [], {}
    rc = 0
    tstart = time.time()
    try:
        run_leg(args, base, watcher, procs, files, summ, state)
        rc = state.get("rc", 0)
        if watcher.stop_ev.is_set():
            print(f"leg ends: {'assert' if watcher.asserts else 'reboot'} seen; "
                  f"keeping console {args.grace:.0f}s for the dump", flush=True)
            time.sleep(args.grace)
            state["ended_by"] = "assert" if watcher.asserts else "reboot"
        else:
            state.setdefault("ended_by", "duration" if state.get("natural_end") else "ended")
    except KeyboardInterrupt:
        state["ended_by"] = "interrupted"
        rc = 130
        print("interrupted; stopping children", flush=True)
    except BaseException as e:
        state["ended_by"] = "error"
        summ["error"] = repr(e)
        rc = 1
        raise
    finally:
        # Every exit path: no child may outlive the leg (a leftover ant_sim holds the USB stick).
        try:
            stop_procs(procs)
        finally:
            try:
                if not args.no_peer and state.get("cap") is not None:
                    peer_cmds(args.peer_port, ["ot state", "ot child table"], base + "-peer.log", "peer after")
                if state.get("ended_by") == "duration":
                    time.sleep(8)   # tail of the console after a clean run
            except BaseException as e:
                print(f"post-leg peer check failed: {e!r}", flush=True)
            cap = state.get("cap")
            capres = cap.stop() if cap else None
            summ["cap"] = str(capres) if capres else None
            deaths = len(capres["deaths"]) if capres else 0
            for fh in files:
                try:
                    fh.close()
                except Exception:
                    pass
            end = datetime.datetime.now()
            wsum = watcher.summarize(end)
            summ.update(wsum)
            summ.update(dut_rc=state.get("dut_rc"), ref_rc=state.get("ref_rc"),
                        ended_by=state.get("ended_by"),
                        elapsed=(time.time() - state["t0"]) if "t0" in state else time.time() - tstart,
                        end=end.isoformat(timespec="seconds"))
            row = make_row(os.path.basename(base), args.image_label, sha, start, wsum, state.get("ended_by"),
                           deaths=deaths)
            summ["row"] = row
            try:
                with open(base + "-summary.json", "w", encoding="utf-8") as f:
                    json.dump(summ, f, indent=2, default=str)
                append_row(os.path.join(os.path.dirname(base), "legs.tsv"), row)
            except Exception as e:
                print(f"summary write failed: {e!r}", flush=True)
            print("ROW", "\t".join(row[c] for c in TSV_COLS), flush=True)
            print("DONE", base, flush=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
