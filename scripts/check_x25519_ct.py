#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check that the X25519 conditional swap compiled branch-free.

Why this exists (security review 2026-11-20, item 1): fe_cswap() in
radiant/src/ext/radiant_x25519.c is the Montgomery ladder's conditional swap,
called once per secret scalar bit. Its masked-XOR C reads as constant time,
but GCC 14.3 at -Os and -O2 proved the mask was 0 or -1 (every caller passes
0/1) and emitted `cmp ip,#0 / beq` inside the limb loop - a branch on the
scalar. The fix hides the mask behind a compiler value barrier; this script
checks the result, because C semantics cannot.

Rules, on the disassembly of every `fe_cswap` symbol (and any GCC clone of
it, `fe_cswap.constprop.0` and the like):

  * no IT block (Thumb predication is a branch in disguise);
  * no cbz / cbnz, no tbb / tbh;
  * no register-indexed load or store (`[r3, r2, lsl #2]`): a mask read
    from a table indexed by the flag is branch-free but leaks through the
    access address;
  * at most ONE conditional branch, and it must jump backwards inside the
    function (the limb loop's edge);
  * the flags that branch tests must not come from the swap argument (r2:r3
    on entry; every argument register in a GCC clone, whose -O3 form takes
    the flag in r0), by a register-only taint pass: the last flag-setting
    instruction before the branch may not read a register whose value was
    computed from the flag, with the loop's back edge iterated to a
    fixpoint.

And on radiant_sec_x25519, the ladder that calls it:

  * at least MIN_SWAP_CALLS (5) calls to fe_cswap* symbols - two per ladder
    step, two after the ladder, one from fe_pack(), observed at every level
    of the matrix (fe_cswap is noinline, so inlining fe_pack moves its call
    rather than removing it). Fewer means a swap was done some other way,
    which nothing here checks;
  * no IT block, cbz or cbnz (none at -Os/-O2/-O3 today).

A missing symbol is a failure, not a pass: a check that finds nothing to
check must not report green.

WHAT THIS DOES NOT PROVE. It is a pattern check, not data-flow analysis. The
taint pass follows registers only: a secret spilled to the stack and
reloaded is not followed, so a loop whose trip count is derived from the
swap flag through memory, or hand-written asm that hides the flag the same
way, could pass. It does not prove the loop runs a fixed 16 times, only
that its exit test does not visibly read the flag. Nor does it look at the
field arithmetic, memory access patterns or instruction timing: those rest
on review of straight-line code (and on Cortex-M4/M33's fixed-time
multiplier; M3's umull exits early and is not covered). The matrix mode
compiles the file standalone, without Zephyr's real flags; the ELF mode on
the release pairing image is the production check.

Two modes:

  ELF (CI):     check_x25519_ct.py --elf zephyr.elf --objdump ... --nm ...
  matrix (local and push CI, the default): compile the source standalone
                with every compiler found (or each --gcc) x {-Os,-O2,-O3}
                x {cortex-m4,cortex-m33} and check each object.

  --source PATH checks another copy of the file (e.g. master's, extracted
  with `git show`), which is how the check is shown to fail on the unfixed
  code.

Exit status 0 = every target clean, 1 = a rule failed or a target could not
be checked, 2 = usage / tool error (including a missing compiler or tool).
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

LADDER = "radiant_sec_x25519"
MIN_SWAP_CALLS = 5

COND = "eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le"
RE_BCOND = re.compile(r"^b(?:%s)(?:\.[nw])?$" % COND)
RE_IT = re.compile(r"^it[te]{0,3}$")
RE_CBZ = re.compile(r"^cbn?z(?:\.[nw])?$")
RE_TB = re.compile(r"^tb[bh](?:\.[nw])?$")
RE_CALL = re.compile(r"^(?:bl|blx|b|b\.w)$")
# "  40:\td01b      \tbeq.n\t7a <fe_cswap+0x5a>"
RE_INSN = re.compile(
    r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{4,8}\s)+)\s*([a-z][a-z0-9.]*)\s*(.*)$")
# "\t\t\t2c2: R_ARM_THM_CALL\tfe_cswap" (objdump -r, unlinked objects)
RE_RELOC = re.compile(r"^\s*[0-9a-f]+:\s+R_ARM_\S+\s+(\S+)")
# `[r3, r2, lsl #2]`: an address with a register offset.
RE_MEMREG = re.compile(r"\[\s*(?:r\d+|ip|lr|sp|fp|sl|sb)\s*,\s*-?(?:r\d+|ip|lr|fp|sl|sb)\b")
RE_SYM = re.compile(r"^fe_cswap(?:\.[A-Za-z0-9_.]+)?$")
RE_REG = re.compile(r"\b(r1[0-5]|r[0-9]|ip|lr|sp|fp|sl|sb)\b")
REG_ALIAS = {"sb": "r9", "sl": "r10", "fp": "r11", "ip": "r12",
             "sp": "r13", "lr": "r14"}

# Data-processing mnemonics whose `s` form sets the flags.
FLAG_OPS = {"add", "adc", "sub", "sbc", "rsb", "and", "orr", "orn", "eor",
            "bic", "mov", "mvn", "lsl", "lsr", "asr", "ror", "rrx", "neg",
            "mul", "umull", "smull", "umlal", "smlal"}
CMP_OPS = {"cmp", "cmn", "tst", "teq"}
UNARY_OPS = {"mov", "mvn", "neg", "rrx", "movw", "sxtb", "sxth",
             "uxtb", "uxth", "clz", "rev", "rbit"}
TWO_DEST = {"umull", "smull", "umlal", "smlal", "ldrd", "ldrexd"}


def run(cmd):
    try:
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           universal_newlines=True)
    except OSError as e:
        sys.stderr.write("cannot run %s: %s\n" % (cmd[0], e))
        sys.exit(2)
    if r.returncode != 0:
        sys.stderr.write("command failed: %s\n%s" % (" ".join(cmd), r.stderr))
        sys.exit(2)
    return r.stdout


def sibling(gcc, tool):
    """arm-zephyr-eabi-gcc[.exe] -> arm-zephyr-eabi-<tool>[.exe]."""
    d, base = os.path.split(gcc)
    return os.path.join(d, base.replace("-gcc", "-" + tool))


def symbols(nm, obj, pattern):
    found = []
    for line in run([nm, obj]).splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tT" and pattern.match(parts[2]):
            found.append(parts[2])
    return sorted(set(found))


def disassemble(objdump, obj, sym):
    """[(addr, mnemonic, operands, line, reloc_symbol_or_None)], listing."""
    text = run([objdump, "-dr", "--disassemble=" + sym, obj])
    insns = []
    for line in text.splitlines():
        m = RE_INSN.match(line)
        if m:
            insns.append([int(m.group(1), 16), m.group(3), m.group(4),
                          line, None])
            continue
        r = RE_RELOC.match(line)
        if r and insns:
            insns[-1][4] = r.group(1)
    return insns, text


def regs(ops):
    """Registers named in an operand string, aliases folded."""
    # Drop shift amounts and the `<sym+off>` branch annotation.
    ops = ops.split("<")[0].split("@")[0]
    return [REG_ALIAS.get(x, x) for x in RE_REG.findall(ops)]


def base_op(mn):
    mn = mn.split(".")[0]
    return mn


def effect(mn, ops):
    """(dests, sources, sets_flags) for the register taint pass."""
    op = base_op(mn)
    rs = regs(ops)
    if op in CMP_OPS:
        return [], rs, True
    if op.startswith("str") or op.startswith("stm") or op == "push":
        return [], [], False
    if op.startswith("ldr") or op.startswith("ldm") or op == "pop":
        # Memory is not tracked: a loaded value is clean (see docstring).
        inside = ops.split("[")[0] if "[" in ops else ops
        d = regs(inside)
        if op not in TWO_DEST and not op.startswith("ldm") and op != "pop":
            d = d[:1]
        return d, [], False
    if op in ("bl", "blx"):
        return ["r0", "r1", "r2", "r3", "r12", "r14"], [], False
    if not rs:
        return [], [], False
    sets = op.endswith("s") and op[:-1] in FLAG_OPS
    core = op[:-1] if sets else op
    n = 2 if core in TWO_DEST else 1
    dests = rs[:n]
    srcs = rs[n:]
    # Accumulating long multiplies read their destinations, and so does
    # Thumb's two-operand form (`eors r5, r7` is r5 ^= r7; `adds r0, #8`).
    # Reading the destination only adds taint, so when in doubt it is read.
    nops = len([x for x in re.split(r",(?![^\[{]*[\]}])", ops.split("@")[0])
                if x.strip()])
    if core in ("umlal", "smlal") or \
       (nops == 2 and core not in UNARY_OPS):
        srcs = srcs + dests
    return dests, srcs, sets


def taint_ok(insns, branch_idx, target_idx, entry):
    """True if the flags the backward branch tests are clear of `entry`."""
    def walk(state, start, stop, flags):
        state = set(state)
        for i in range(start, stop):
            _, mn, ops, _, _ = insns[i]
            dests, srcs, sets = effect(mn, ops)
            t = any(s in state for s in srcs)
            if sets:
                flags = t
            for d in dests:
                if t:
                    state.add(d)
                else:
                    state.discard(d)
        return state, flags

    def join(x, y):
        if x is True or y is True:
            return True
        if x is None or y is None:
            return None
        return False

    head, fhead = walk(entry, 0, target_idx, None)
    state, flags = walk(head, target_idx, branch_idx, fhead)
    for _ in range(32):  # the back edge, to a fixpoint; taint only grows
        nhead, nfhead = head | state, join(fhead, flags)
        if nhead == head and nfhead == fhead:
            break
        head, fhead = nhead, nfhead
        state, flags = walk(head, target_idx, branch_idx, fhead)
    # None: no flag-setting instruction found - cannot clear it, so fail.
    return flags is False


def check_swap(insns, sym):
    problems = []
    if not insns:
        return ["no instructions disassembled"]
    # fe_cswap(a, b, swap): the int64 flag is r2:r3. A GCC clone
    # (.constprop/.isra) drops or reorders arguments - the -O3 clones take
    # the flag in r0 - so every argument register is treated as the flag.
    entry = {"r2", "r3"} if sym == "fe_cswap" else {"r0", "r1", "r2", "r3"}
    lo = insns[0][0]
    cond = []
    for idx, (addr, mn, ops, line, _) in enumerate(insns):
        if RE_MEMREG.search(ops) and not mn.startswith(("push", "pop")):
            problems.append("register-indexed memory access (address may "
                            "depend on the flag): " + line.strip())
        if RE_IT.match(mn):
            problems.append("IT block: " + line.strip())
        elif RE_CBZ.match(mn):
            problems.append("cbz/cbnz: " + line.strip())
        elif RE_TB.match(mn):
            problems.append("table branch: " + line.strip())
        elif RE_BCOND.match(mn):
            cond.append((idx, addr, ops, line))
    if len(cond) > 1:
        for _, _, _, line in cond:
            problems.append("conditional branch (%d in function, 1 allowed): %s"
                            % (len(cond), line.strip()))
    elif cond:
        idx, addr, ops, line = cond[0]
        t = re.match(r"([0-9a-f]+)", ops)
        target = int(t.group(1), 16) if t else None
        if target is None or not (lo <= target < addr):
            problems.append("conditional branch is not a backward loop edge: "
                            + line.strip())
        else:
            tidx = next((i for i, x in enumerate(insns) if x[0] == target),
                        None)
            if tidx is None:
                problems.append("loop edge target is not an instruction: "
                                + line.strip())
            elif not taint_ok(insns, idx, tidx, entry):
                problems.append("loop edge tests flags derived from the swap "
                                "argument (%s): %s"
                                % ("/".join(sorted(entry)), line.strip()))
    return problems


def check_ladder(insns):
    problems = []
    if not insns:
        return ["no instructions disassembled"]
    calls = 0
    preds = []
    for addr, mn, ops, line, reloc in insns:
        if RE_IT.match(mn) or RE_CBZ.match(mn):
            preds.append(line.strip())
        elif RE_CALL.match(mn):
            named = re.search(r"<(fe_cswap[^>+]*)>", ops)
            if (named and RE_SYM.match(named.group(1))) or \
               (reloc and RE_SYM.match(reloc)):
                calls += 1
    if preds:
        problems.append("%d IT block(s) / cbz / cbnz, first: %s"
                        % (len(preds), "; ".join(preds[:3])))
    if calls < MIN_SWAP_CALLS:
        problems.append("only %d calls to fe_cswap* (at least %d expected): "
                        "a swap is being done some other way"
                        % (calls, MIN_SWAP_CALLS))
    return problems, calls


def check_object(objdump, nm, obj, label, verbose):
    ok = True
    syms = symbols(nm, obj, RE_SYM)
    if not syms:
        print("FAIL  %s: no fe_cswap symbol (inlined or renamed?)" % label)
        ok = False
    for sym in syms:
        insns, listing = disassemble(objdump, obj, sym)
        problems = check_swap(insns, sym)
        print("%s  %s: %s" % ("FAIL" if problems else "ok  ", label, sym))
        for p in problems:
            print("        " + p)
        ok &= not problems
        if verbose or problems:
            for line in listing.splitlines():
                if RE_INSN.match(line):
                    print("          | " + line.rstrip())

    if not symbols(nm, obj, re.compile("^%s$" % LADDER)):
        print("FAIL  %s: no %s symbol" % (label, LADDER))
        return False
    insns, _ = disassemble(objdump, obj, LADDER)
    problems, calls = check_ladder(insns)
    print("%s  %s: %s (%d fe_cswap calls)"
          % ("FAIL" if problems else "ok  ", label, LADDER, calls))
    for p in problems:
        print("        " + p)
    return ok and not problems


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
