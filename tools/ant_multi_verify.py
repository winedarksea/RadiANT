#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Run ant_verify.py against several receivers at once, against one live
transmitter, and print their results side by side.

This answers a different question than tools/ant_ab.py. ant_ab.py compares
two *sittings* (a build vs. a build, minutes or days apart) and refuses to
trust the comparison unless they carry matching rig metadata, because
run-to-run drift on this bench rivals the effect being measured. This script
instead compares several *receivers* against one shared transmitter in the
same window, so there is no run-to-run drift to distrust: every target hears
the identical RF at the identical second, and any spread between them is a
receiver-side or link-margin difference, not a room-changed-between-runs one.

    python tools/ant_multi_verify.py --seconds 300 --device-number 33333 \\
        --target "Feather=usb:755972D7183A6183" \\
        --target "nRF52840 dongle=usb:3D55F77818BE772A" \\
        --target "L15 core DUT=port:COM8"

Each --target is NAME=usb:SERIAL (matched the way ant_verify.py's --serial
does, by suffix) or NAME=port:COMn (a UART build, e.g. the nRF54L15 DK's
core+nrf image talked to directly per [[hardware-bench]]). Extra arguments
after -- are forwarded verbatim to every child ant_verify.py invocation, so
--expect-watts, --profile, etc. all work here too.

Each target runs as its own ant_verify.py subprocess - the loss/jitter/
accounting logic is not reimplemented here, only launched concurrently and
summarised. A child's stdout/stderr is captured to a per-target log next to
its --json output so a failure can be read in full; this script only prints
the numbers that matter for a side-by-side read.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

_HERE = Path(__file__).resolve().parent


@dataclass
class Target:
    name: str
    kind: str  # "usb" or "port"
    value: str


def parse_target(spec: str) -> Target:
    if "=" not in spec:
        raise argparse.ArgumentTypeError(
            f"--target needs NAME=usb:SERIAL or NAME=port:COMn, got {spec!r}")
    name, rest = spec.split("=", 1)
    if ":" not in rest:
        raise argparse.ArgumentTypeError(
            f"--target value needs usb:SERIAL or port:COMn, got {rest!r}")
    kind, value = rest.split(":", 1)
    if kind not in ("usb", "port"):
        raise argparse.ArgumentTypeError(f"--target kind must be usb or port, got {kind!r}")
    return Target(name=name, kind=kind, value=value)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--target", type=parse_target, action="append", required=True,
                         help="NAME=usb:SERIAL or NAME=port:COMn; repeatable")
    parser.add_argument("--seconds", type=float, default=300.0)
    parser.add_argument("--device-number", type=int, action="append", default=[],
                         help="forwarded to every child as --device-number "
                              "(repeat to match ant_verify.py's multi-channel form)")
    parser.add_argument("--out-dir", default=None,
                         help="where to write each target's --json/log; "
                              "defaults to a fresh temp directory")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("extra", nargs=argparse.REMAINDER,
                         help="anything after -- is forwarded to every child ant_verify.py")
    args = parser.parse_args()

    extra = args.extra
    if extra and extra[0] == "--":
        extra = extra[1:]

    out_dir = Path(args.out_dir) if args.out_dir else Path(tempfile.mkdtemp(prefix="ant_multi_"))
    out_dir.mkdir(parents=True, exist_ok=True)

    procs = []
    for t in args.target:
        slug = "".join(c if c.isalnum() else "_" for c in t.name)
        json_path = out_dir / f"{slug}.json"
        log_path = out_dir / f"{slug}.log"
        cmd = [sys.executable, str(_HERE / "ant_verify.py"),
               "--seconds", str(args.seconds),
               "--json", str(json_path), "-q"]
        for d in args.device_number:
            cmd += ["--device-number", str(d)]
        if t.kind == "usb":
            cmd += ["--serial", t.value]
        else:
            cmd += ["--port", t.value, "--baud", str(args.baud)]
        cmd += extra
        log_f = open(log_path, "w", encoding="utf-8")
        print(f"launching {t.name}: {' '.join(cmd)}")
        proc = subprocess.Popen(cmd, stdout=log_f, stderr=subprocess.STDOUT)
        procs.append((t, proc, json_path, log_path, log_f))

    print(f"\n{len(procs)} target(s) running concurrently for {args.seconds:.0f} s ...")
    results = []
    for t, proc, json_path, log_path, log_f in procs:
        rc = proc.wait()
        log_f.close()
        entry = {"target": t.name, "exit": rc, "json": str(json_path), "log": str(log_path)}
        if json_path.exists():
            try:
                entry["data"] = json.loads(json_path.read_text(encoding="utf-8"))
            except json.JSONDecodeError as e:
                entry["error"] = f"bad json: {e}"
        else:
            entry["error"] = "no json written - see log"
        results.append(entry)

    print()
    header = f"{'target':<24} {'loss_exact%':>11} {'unacct':>7} {'rssi dBm':>9} {'pass':>5}"
    print(header)
    print("-" * len(header))
    for r in results:
        d = r.get("data", {}).get("derived", {})
        loss = d.get("loss_exact_pct")
        unacct = r.get("data", {}).get("accounting", {}).get("unaccounted")
        rssi = d.get("rssi_dbm_mean")
        loss_s = f"{loss:.2f}" if loss is not None else "n/a"
        unacct_s = str(unacct) if unacct is not None else "n/a"
        rssi_s = f"{rssi:.1f}" if rssi is not None else "n/a"
        pass_s = "OK" if r.get("data", {}).get("pass") else "FAIL" if "data" in r else "ERR"
        print(f"{r['target']:<24} {loss_s:>11} {unacct_s:>7} {rssi_s:>9} {pass_s:>5}")

    print(f"\nresults directory: {out_dir}")
    return 0 if all(r.get("data", {}).get("pass") for r in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
