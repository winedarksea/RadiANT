#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prove fe_cswap() in radiant/src/ext/radiant_x25519.c compiled branch-free.

Why this exists (security review 2026-11-20, item 1): fe_cswap() is the
Montgomery ladder's conditional swap, called once per secret scalar bit. Its
masked-XOR C reads as constant time, but GCC 14.3 at -Os proved the mask was
0 or -1 (every caller passes 0/1) and emitted `cmp ip,#0 / beq` inside the
limb loop - a branch on the scalar. The fix hides the mask behind a compiler
value barrier; this script is what holds the claim, because C semantics
cannot.

Rule, applied to the disassembly of every `fe_cswap` symbol (and any GCC
clone of it, `fe_cswap.constprop.0` and the like):

  * no IT block (Thumb predication is a branch in disguise);
  * no cbz / cbnz;
  * no tbb / tbh;
  * at most ONE conditional branch, and it must jump backwards inside the
    function - the limb loop's edge, whose count is fixed at 16.

A missing symbol is a failure, not a pass: a check that finds nothing to
check must not report green.

Two modes:

  ELF (CI):     check_x25519_ct.py --elf zephyr.elf --objdump ... --nm ...
  matrix (local, the default): compile the source standalone with every
                compiler found x {-Os,-O2,-O3} x {cortex-m4,cortex-m33}
                and check each object.

  --source PATH checks another copy of the file (e.g. master's, extracted
  with `git show`), which is how the check is shown to fail on the unfixed
  code.

Exit status 0 = every target clean, 1 = a branch was found or a target could
not be checked, 2 = usage / tool error.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SOURCE = os.path.join(REPO, "radiant", "src", "ext", "radiant_x25519.c")
INCLUDE = os.path.join(REPO, "radiant", "include")

# Compilers for the local matrix. GCC 14.3 is the production compiler (NCS
# v3.4.0, Zephyr SDK 1.0.1); GCC 12.2 is Zephyr SDK 0.17.0, the older bundle
# still on this bench. More can be added with --gcc.
KNOWN_GCC = [
    r"C:\ncs\toolchains\dcbdc366a1\opt\zephyr-sdk\gnu\arm-zephyr-eabi\bin"
    r"\arm-zephyr-eabi-gcc.exe",
    r"C:\ncs\toolchains\fd21892d0f\opt\zephyr-sdk\arm-zephyr-eabi\bin"
    r"\arm-zephyr-eabi-gcc.exe",
]
OPT_LEVELS = ["-Os", "-O2", "-O3"]
CPUS = ["cortex-m4", "cortex-m33"]

COND = "eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le"
RE_BCOND = re.compile(r"^b(?:%s)(?:\.[nw])?$" % COND)
RE_IT = re.compile(r"^it[te]{0,3}$")
RE_CBZ = re.compile(r"^cbn?z(?:\.[nw])?$")
RE_TB = re.compile(r"^tb[bh](?:\.[nw])?$")
# "  40:\td01b      \tbeq.n\t7a <fe_cswap+0x5a>"
RE_INSN = re.compile(
    r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{4,8}\s)+)\s*([a-z][a-z0-9.]*)\s*(.*)$")
RE_SYM = re.compile(r"^fe_cswap(?:\.[A-Za-z0-9_.]+)?$")


def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       universal_newlines=True)
    if r.returncode != 0:
        sys.stderr.write("command failed: %s\n%s" % (" ".join(cmd), r.stderr))
        sys.exit(2)
    return r.stdout


def sibling(gcc, tool):
    """arm-zephyr-eabi-gcc[.exe] -> arm-zephyr-eabi-<tool>[.exe]."""
    d, base = os.path.split(gcc)
    return os.path.join(d, base.replace("-gcc", "-" + tool))


def symbols(nm, obj):
    found = []
    for line in run([nm, obj]).splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tT" and RE_SYM.match(parts[2]):
            found.append(parts[2])
    return sorted(set(found))


def check_function(objdump, obj, sym):
    """Return (problems, listing) for one symbol."""
    text = run([objdump, "-d", "--disassemble=" + sym, obj])
    insns = []
    for line in text.splitlines():
        m = RE_INSN.match(line)
        if m:
            insns.append((int(m.group(1), 16), m.group(3), m.group(4), line))
    problems = []
    if not insns:
        return ["no instructions disassembled for %s" % sym], text
    lo = insns[0][0]
    hi = insns[-1][0]
    cond = []
    for addr, mn, ops, line in insns:
        if RE_IT.match(mn):
            problems.append("IT block: " + line.strip())
        elif RE_CBZ.match(mn):
            problems.append("cbz/cbnz: " + line.strip())
        elif RE_TB.match(mn):
            problems.append("table branch: " + line.strip())
        elif RE_BCOND.match(mn):
            cond.append((addr, ops, line))
    if len(cond) > 1:
        for _, _, line in cond:
            problems.append("conditional branch (%d in function, 1 allowed): %s"
                            % (len(cond), line.strip()))
    elif cond:
        addr, ops, line = cond[0]
        t = re.match(r"([0-9a-f]+)", ops)
        target = int(t.group(1), 16) if t else None
        if target is None or not (lo <= target < addr):
            problems.append("conditional branch is not a backward loop edge: "
                            + line.strip())
    return problems, text


def check_object(objdump, nm, obj, label, verbose):
    syms = symbols(nm, obj)
    if not syms:
        print("FAIL  %s: no fe_cswap symbol (inlined or renamed?)" % label)
        return False
    ok = True
    for sym in syms:
        problems, listing = check_function(objdump, obj, sym)
        if problems:
            ok = False
            print("FAIL  %s: %s" % (label, sym))
            for p in problems:
                print("        " + p)
        else:
            print("ok    %s: %s" % (label, sym))
        if verbose or problems:
            for line in listing.splitlines():
                if RE_INSN.match(line):
                    print("          | " + line.rstrip())
    return ok


def matrix(args):
    gccs = args.gcc or [g for g in KNOWN_GCC if os.path.isfile(g)]
    if not gccs:
        sys.stderr.write("no arm-zephyr-eabi-gcc found; pass --gcc\n")
        return 2
    tmp = tempfile.mkdtemp(prefix="x25519_ct_")
    ok = True
    try:
        for gcc in gccs:
            ver = run([gcc, "-dumpversion"]).strip()
            objdump, nm = sibling(gcc, "objdump"), sibling(gcc, "nm")
            for cpu in CPUS:
                for opt in OPT_LEVELS:
                    obj = os.path.join(tmp, "x_%s_%s_%s.o"
                                       % (ver, cpu, opt.lstrip("-")))
                    run([gcc, "-c", "-mcpu=" + cpu, "-mthumb", opt,
                         "-std=c11", "-ffunction-sections", "-fdata-sections",
                         "-DCONFIG_RADIANT_SEC=1",
                         "-DCONFIG_RADIANT_SEC_PAIRING_X25519=1",
                         "-I", INCLUDE, args.source, "-o", obj])
                    label = "gcc %-6s %-10s %s" % (ver, cpu, opt)
                    ok &= check_object(objdump, nm, obj, label, args.verbose)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--elf", help="check this linked ELF instead of compiling")
    ap.add_argument("--objdump", help="arm objdump (ELF mode)")
    ap.add_argument("--nm", help="arm nm (ELF mode)")
    ap.add_argument("--gcc", action="append",
                    help="compiler for matrix mode (repeatable)")
    ap.add_argument("--source", default=DEFAULT_SOURCE,
                    help="radiant_x25519.c to compile (matrix mode)")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="print every listing, not only failing ones")
    args = ap.parse_args()

    if args.elf:
        if not (args.objdump and args.nm):
            ap.error("--elf needs --objdump and --nm")
        ok = check_object(args.objdump, args.nm, args.elf, args.elf,
                          args.verbose)
        return 0 if ok else 1
    return matrix(args)


if __name__ == "__main__":
    sys.exit(main())
