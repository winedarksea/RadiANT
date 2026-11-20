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

SCOPE. This guards against COMPILER changes - a new GCC, new flags, a new
core - turning REVIEWED source back into secret-dependent code. It is not a
defence against adversarial edits to the source or hand-written asm; those
go through code review. Several rules below can be defeated on purpose by a
source change, and that is accepted.

Rules, on the disassembly of every `fe_cswap` symbol (and any GCC clone of
it, `fe_cswap.constprop.0` and the like):

  * no IT block (Thumb predication is a branch in disguise), no cbz / cbnz,
    no tbb / tbh, no bl / blx, no msr / vmrs APSR_nzcv (flags written
    directly);
  * at most ONE conditional branch, and it must jump backwards inside the
    function (the limb loop's edge);
  * a forward taint pass over the function's control-flow graph (branch
    targets followed, so a block laid out after the loop edge that sets the
    flags and jumps back is seen) must find the flags that branch tests
    clean of the swap argument (r2:r3 on entry; every argument register in
    a GCC clone, whose -O3 form takes the flag in r0). The flags are also
    tainted on entry;
  * no memory access whose address registers are tainted at that point (a
    mask read from a table indexed by the flag is branch-free but leaks
    through the address).

The taint pass is an allow-list: instructions it models exactly (compares,
data processing, loads, stores, moves, VFP moves - the production build is
hard-float) propagate taint from sources to destinations and to the flags
for their flag-setting forms; bfi/bfc/movt read their destination; anything
it does not recognise taints every register it names and the flags if any
of them is tainted, and never clears anything. Memory is one cell: storing
a tainted register taints it and every non-literal-pool load after that
returns tainted.

On radiant_sec_x25519, the ladder that calls it:

  * at least LADDER_SWAP_CALLS (4) direct calls to fe_cswap* in
    radiant_sec_x25519 (two in the ladder step, two after it) and at least
    MIN_SWAP_CALLS (5) counting fe_pack* when it is out of line (its one
    freeze swap; at -Os/-O2/-O3 it is inlined, at -Og and -fno-inline it is
    not). This catches a compiler inlining fe_cswap into the ladder despite
    noinline, or cloning it out of reach of the per-symbol rules above;
  * no IT block, cbz or cbnz in radiant_sec_x25519. Blunt: none exist at
    -Og/-Os/-O2/-O3 today, but -funroll-all-loops can introduce them on
    public loop counters and would then need this rule refined.

A missing symbol is a failure, not a pass: a check that finds nothing to
check must not report green. An LTO build must therefore keep
radiant_sec_x25519 out of line (it fails closed with "no symbol").

KNOWN LIMITATIONS (out of scope: they need a source or asm edit, which code
review covers). It does not prove the loop runs a fixed 16 times, only that
its exit test does not visibly derive from the flag; memory is one cell, so
it does not separate the stack from the limbs (so -O0, which spills the
flag and reloads every pointer from the stack, fails closed and is not
supported) and catches a flag spilled to the stack only coarsely; msr is
failed outright rather than modelled; a conditional call to
fe_cswap from the ladder, or a swap done in source some other way, is not
detected. Nor does it look at the field arithmetic, memory access patterns
outside fe_cswap, or instruction timing: those rest on review of
straight-line code, and on Cortex-M4/M33's fixed-time multiplier (M3's
umull exits early and is not covered). The matrix mode compiles the file
standalone without Zephyr's real flags; the ELF mode on the release pairing
image is the production check.

Two modes:

  ELF (CI):     check_x25519_ct.py --elf zephyr.elf --objdump ... --nm ...
  matrix (local and push CI, the default): compile the source standalone
                with every compiler found (or each --gcc) x
                {-Og,-Os,-O2,-O3} x {cortex-m4,cortex-m33} and check each
                object.

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
OPT_LEVELS = ["-Og", "-Os", "-O2", "-O3"]
CPUS = ["cortex-m4", "cortex-m33"]

LADDER = "radiant_sec_x25519"
LADDER_SWAP_CALLS = 4
MIN_SWAP_CALLS = 5

COND = "eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le"
RE_BCOND = re.compile(r"^b(?:%s)(?:\.[nw])?$" % COND)
RE_B = re.compile(r"^b(?:\.[nw])?$")
RE_IT = re.compile(r"^it[te]{0,3}$")
RE_CBZ = re.compile(r"^cbn?z(?:\.[nw])?$")
RE_TB = re.compile(r"^tb[bh](?:\.[nw])?$")
RE_CALL = re.compile(r"^(?:bl|blx|b|b\.w)$")
# "  40:\td01b      \tbeq.n\t7a <fe_cswap+0x5a>"
RE_INSN = re.compile(
    r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{4,8}\s)+)\s*([a-z][a-z0-9.]*)\s*(.*)$")
# "\t\t\t2c2: R_ARM_THM_CALL\tfe_cswap" (objdump -r, unlinked objects)
RE_RELOC = re.compile(r"^\s*[0-9a-f]+:\s+R_ARM_\S+\s+(\S+)")
RE_SYM = re.compile(r"^fe_cswap(?:\.[A-Za-z0-9_.]+)?$")
RE_PACK = re.compile(r"^fe_pack(?:\.[A-Za-z0-9_.]+)?$")
RE_REG = re.compile(r"\b(r1[0-5]|r[0-9]|ip|lr|sp|fp|sl|sb|pc|[sd][0-9]+)\b")
REG_ALIAS = {"sb": "r9", "sl": "r10", "fp": "r11", "ip": "r12",
             "sp": "r13", "lr": "r14", "pc": "r15"}

# The allow-list. Plain data processing: first operand(s) written, the rest
# read. A trailing `s` sets the flags.
DP_OPS = {"add", "adc", "sub", "sbc", "rsb", "and", "orr", "orn", "eor",
          "bic", "mov", "mvn", "lsl", "lsr", "asr", "ror", "rrx", "neg",
          "mul", "mla", "mls", "sdiv", "udiv", "adr", "addw", "subw",
          "movw", "movt", "bfi", "bfc", "ubfx", "sbfx", "sxtb", "sxth",
          "uxtb", "uxth", "clz", "rev", "rbit", "umull", "smull", "umlal",
          "smlal", "vmov"}
FLAG_OPS = {"add", "adc", "sub", "sbc", "rsb", "and", "orr", "orn", "eor",
            "bic", "mov", "mvn", "lsl", "lsr", "asr", "ror", "rrx", "neg",
            "mul", "umull", "smull", "umlal", "smlal"}
CMP_OPS = {"cmp", "cmn", "tst", "teq"}
# Unary: a two-operand form does NOT read the destination.
UNARY_OPS = {"mov", "mvn", "neg", "rrx", "movw", "sxtb", "sxth", "uxtb",
             "uxth", "clz", "rev", "rbit", "adr", "vmov"}
# Always read the destination (keep part of it).
READS_DEST = {"bfi", "bfc", "movt", "umlal", "smlal", "mla", "mls"}
TWO_DEST = {"umull", "smull", "umlal", "smlal"}
NO_EFFECT = {"nop"}


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


# --- the taint pass ---------------------------------------------------------

def regs(ops):
    """Registers named in an operand string, aliases folded."""
    ops = ops.split("<")[0].split("@")[0]
    return [REG_ALIAS.get(x, x) for x in RE_REG.findall(ops)]


def mem_parts(ops):
    """(data regs, address regs, writeback base or None, pc_relative)."""
    ops = ops.split("@")[0]
    if "[" not in ops:
        return regs(ops), [], None, False
    pre, rest = ops.split("[", 1)
    inside, post = rest.split("]", 1)
    addr = regs(inside) + regs(post)
    base = regs(inside)[:1]
    wb = base[0] if base and ("!" in post or regs(post) or "#" in post) \
        else None
    return regs(pre), addr, wb, bool(base) and base[0] == "r15"


def n_operands(ops):
    ops = ops.split("@")[0].split("<")[0]
    return len([x for x in re.split(r",(?![^\[{]*[\]}])", ops) if x.strip()])


def strip_op(mn):
    return mn.split(".")[0]


def transfer(mn, ops, state):
    """One instruction. state = (tainted regs, flags tainted, memory tainted).
    Returns (new state, address registers read by a memory access)."""
    regs_t, flags, mem = state
    regs_t = set(regs_t)
    op = strip_op(mn)
    t = lambda rs: any(r in regs_t for r in rs)
    addr = []

    def write(dests, tainted):
        for d in dests:
            if d == "r15":
                continue
            if tainted:
                regs_t.add(d)
            else:
                regs_t.discard(d)

    if op in NO_EFFECT or RE_BCOND.match(mn) or RE_B.match(mn) or \
       RE_IT.match(mn) or op in ("bx",):
        pass
    elif op in CMP_OPS:
        flags = t(regs(ops))
    elif op in ("push", "stmdb", "stmia", "stm", "stmfd") or \
            (op.startswith("str") and "[" in ops):
        if op.startswith("str"):
            data, addr, wb, _ = mem_parts(ops)
        else:
            parts = ops.split("{", 1)
            data = regs(parts[1]) if len(parts) > 1 else []
            addr = regs(parts[0]) if len(parts) > 1 else []
            wb = None
        if t(data):
            mem = True
        if wb:
            write([wb], t([wb] + addr[1:]))
    elif op in ("pop", "ldmia", "ldm", "ldmfd", "ldmdb") or \
            (op.startswith("ldr") and "[" in ops):
        if op.startswith("ldr"):
            data, addr, wb, pcrel = mem_parts(ops)
            val = (mem and not pcrel) or t(addr)
            write(data, val)
            if wb and wb not in data:
                write([wb], t([wb] + addr[1:]))
        else:
            parts = ops.split("{", 1)
            data = regs(parts[1]) if len(parts) > 1 else []
            addr = regs(parts[0]) if len(parts) > 1 else []
            write(data, mem or t(addr))
    elif op in ("bl", "blx"):
        # Should not occur (a rule fails it); model it pessimistically.
        any_arg = t(["r0", "r1", "r2", "r3"])
        write(["r0", "r1", "r2", "r3", "r12", "r14"], any_arg)
        flags = flags or any_arg
    elif op == "msr" or (op == "vmrs" and "apsr" in ops.lower()):
        flags = flags or t(regs(ops)) or op == "vmrs"
    else:
        sets = False
        core = op
        if op not in DP_OPS and op.endswith("s") and op[:-1] in FLAG_OPS:
            sets, core = True, op[:-1]
        rs = regs(ops)
        if core in DP_OPS:
            n = 2 if core in TWO_DEST else 1
            if core == "vmov" and n_operands(ops) == 4:
                n = 2           # vmov r0, r1, d0 / vmov d0, r0, r1
            dests, srcs = rs[:n], rs[n:]
            if core in READS_DEST or \
               (n_operands(ops) == 2 and core not in UNARY_OPS):
                srcs = srcs + dests
            val = t(srcs)
            write(dests, val)
            if sets:
                flags = val
        else:
            # Unknown: everything it names becomes tainted if any of it is,
            # and so do the flags. Nothing is ever cleared.
            if t(rs):
                regs_t.update(r for r in rs if r != "r15")
                flags = True
    return (frozenset(regs_t), flags, mem), addr


def successors(insns, i, index):
    """Indices control can reach after insns[i]; out-of-function = exit."""
    addr, mn, ops, _, _ = insns[i]
    op = strip_op(mn)
    nxt = [i + 1] if i + 1 < len(insns) else []
    tgt = re.match(r"([0-9a-f]+)\b", ops.strip())
    tidx = index.get(int(tgt.group(1), 16)) if tgt else None
    if RE_BCOND.match(mn) or RE_CBZ.match(mn):
        return nxt + ([tidx] if tidx is not None else [])
    if RE_B.match(mn):
        return [tidx] if tidx is not None else []
    if op == "bx" or (op in ("pop", "ldm", "ldmia", "ldmfd") and "pc" in ops) \
       or (op.startswith("ldr") and re.match(r"\s*pc\b", ops)):
        return []
    return nxt


def taint_states(insns, entry):
    """In-state of every instruction, by a worklist over the CFG."""
    index = {x[0]: i for i, x in enumerate(insns)}
    ins = [None] * len(insns)
    ins[0] = (frozenset(entry), True, False)
    work = [0]
    while work:
        i = work.pop()
        out, _ = transfer(insns[i][1], insns[i][2], ins[i])
        for s in successors(insns, i, index):
            old = ins[s]
            new = out if old is None else \
                (old[0] | out[0], old[1] or out[1], old[2] or out[2])
            if new != old:
                ins[s] = new
                work.append(s)
    return ins


# --- the rules ----------------------------------------------------------------

def swap_registers(insns, sym):
    """Where the int64 swap flag arrives.

    fe_cswap(a, b, swap): r2:r3. A GCC .constprop clone drops the constant
    pointer arguments and keeps the rest in order, so the flag is the last
    argument: r0:r1 when it is the only one (the ladder's clones at -O3),
    r2:r3 (an int64 is pair-aligned) when a pointer is still passed in r0
    (fe_pack's clone). r0 used as an address before anything writes it means
    it holds a pointer; otherwise every argument register is treated as the
    flag, which can only fail closed.
    """
    if sym == "fe_cswap":
        return {"r2", "r3"}
    for _, mn, ops, _, _ in insns:
        op = strip_op(mn)
        if "[" in ops and (op.startswith("ldr") or op.startswith("str")):
            _, addr, _, _ = mem_parts(ops)
            if addr[:1] == ["r0"]:
                return {"r2", "r3"}
        rs = regs(ops)
        if rs[:1] == ["r0"] and op not in CMP_OPS and \
           not op.startswith("str") and op != "push":
            break
    return {"r0", "r1", "r2", "r3"}

def check_swap(insns, sym):
    problems = []
    if not insns:
        return ["no instructions disassembled"]
    entry = swap_registers(insns, sym)
    states = taint_states(insns, entry)
    lo = insns[0][0]
    cond = []
    for idx, (addr, mn, ops, line, _) in enumerate(insns):
        op = strip_op(mn)
        if RE_IT.match(mn):
            problems.append("IT block: " + line.strip())
        elif RE_CBZ.match(mn):
            problems.append("cbz/cbnz: " + line.strip())
        elif RE_TB.match(mn):
            problems.append("table branch: " + line.strip())
        elif op in ("bl", "blx"):
            problems.append("call inside the swap: " + line.strip())
        elif op == "msr" or (op == "vmrs" and "apsr" in ops.lower()):
            problems.append("flags written directly: " + line.strip())
        elif RE_BCOND.match(mn):
            cond.append((idx, addr, ops, line))
        if states[idx] is not None:
            _, a = transfer(mn, ops, states[idx])
            if any(r in states[idx][0] for r in a):
                problems.append("memory address derived from the swap "
                                "argument: " + line.strip())
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
        elif states[idx] is None or states[idx][1]:
            problems.append("loop edge tests flags derived from the swap "
                            "argument (%s): %s"
                            % ("/".join(sorted(entry)), line.strip()))
    return problems


def swap_calls(insns):
    calls = 0
    for addr, mn, ops, line, reloc in insns:
        if RE_CALL.match(mn):
            named = re.search(r"<(fe_cswap[^>+]*)>", ops)
            if (named and RE_SYM.match(named.group(1))) or \
               (reloc and RE_SYM.match(reloc)):
                calls += 1
    return calls


def check_ladder(insns, pack_calls):
    problems = []
    if not insns:
        return ["no instructions disassembled"], 0
    preds = [x[3].strip() for x in insns
             if RE_IT.match(x[1]) or RE_CBZ.match(x[1])]
    if preds:
        problems.append("%d IT block(s) / cbz / cbnz, first: %s"
                        % (len(preds), "; ".join(preds[:3])))
    calls = swap_calls(insns)
    if calls < LADDER_SWAP_CALLS:
        problems.append("only %d calls to fe_cswap* in %s (at least %d "
                        "expected)" % (calls, LADDER, LADDER_SWAP_CALLS))
    if calls + pack_calls < MIN_SWAP_CALLS:
        problems.append("only %d calls to fe_cswap* with fe_pack's %d (at "
                        "least %d expected)"
                        % (calls + pack_calls, pack_calls, MIN_SWAP_CALLS))
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
    pack_calls = sum(swap_calls(disassemble(objdump, obj, p)[0])
                     for p in symbols(nm, obj, RE_PACK))
    insns, _ = disassemble(objdump, obj, LADDER)
    problems, calls = check_ladder(insns, pack_calls)
    print("%s  %s: %s (%d fe_cswap calls%s)"
          % ("FAIL" if problems else "ok  ", label, LADDER, calls,
             " + %d in fe_pack" % pack_calls if pack_calls else ""))
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
                for opt in args.opt or OPT_LEVELS:
                    obj = os.path.join(tmp, "x_%s_%s_%s.o"
                                       % (ver, cpu, opt.lstrip("-")))
                    run([gcc, "-c", "-mcpu=" + cpu, "-mthumb", opt,
                         "-std=c11", "-ffunction-sections", "-fdata-sections",
                         "-DCONFIG_RADIANT_SEC=1",
                         "-DCONFIG_RADIANT_SEC_PAIRING_X25519=1"]
                        + (args.cflag or []) +
                        ["-I", INCLUDE, args.source, "-o", obj])
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
    ap.add_argument("--opt", action="append",
                    help="optimisation level(s) instead of the default set")
    ap.add_argument("--cflag", action="append",
                    help="extra compiler flag (repeatable), e.g. -fno-inline")
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
