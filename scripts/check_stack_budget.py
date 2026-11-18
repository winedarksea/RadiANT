# SPDX-License-Identifier: Apache-2.0

"""Sum the compiler's stack frames along named call chains, and fail over budget.

Why this exists (security review 2026-11-17, item 1): the X25519 ladder kept
fifteen field elements on the stack, a 2,248 B frame, and the dongle runs 0xF5
pairing on the 2,048 B serial bridge thread. Every test passed, because every
test ran on a 4 KB ztest stack, and on nRF52840 the frame stepped clean over the
32 B MPU guard instead of faulting. Nothing measured the chain. This does, from
the `.su` files GCC writes with CONFIG_STACK_USAGE=y (-fstack-usage).

A chain is named by hand, caller first:

    --chain 'peer=bridge_thread_fn,antr_sec_pair,radiant_sec_pair_peer,
             radiant_sec_x25519,fe_mul,fe_carry<=CONFIG_ANT_DONGLE_BRIDGE_STACK_SIZE'

and its total is the sum of those frames plus one exception frame (an interrupt
stacks 32 B on the thread's stack on Cortex-M, 104 B with FP context; taken
from CONFIG_FPU in the build's .config unless --exception-frame says).

The limit is a number, a CONFIG_ symbol read from the build's .config, or a
macro read from a --header file (RADIANT_SEC_PAIR_STACK_BYTES).

What fails, and why each one is a failure rather than a warning:

  * a chain function with no frame at all - it was inlined, renamed or
    removed, so the chain no longer describes the code and its sum means
    nothing. Rename it in the chain; do not delete it.
  * a `dynamic` frame on a chain - alloca or a VLA, whose size the compiler
    cannot state, so the sum is a lower bound and not a budget.
  * any frame over --frame-limit among the functions defined in a
    --frame-limit-path source: one big frame is what jumped the guard, and it
    is the cheapest thing to catch before it is on any chain.
  * a chain total over its limit.

Not a call-graph analysis: a chain nobody names is not checked. The chains in
release.yml are the deepest ones measured when this was written (objdump call
graph over the pairing build); the frame limit is the net under the rest.
"""

import argparse
import re
import sys
from pathlib import Path

# GCC appends these to a function it has cloned or split; the frame still
# belongs to the function the chain names.
_CLONE = re.compile(r"\.(constprop|isra|part|cold|lto_priv)(\.\d+)*$")


class Frame:
    __slots__ = ("name", "size", "kind", "source")

    def __init__(self, name, size, kind, source):
        self.name = name
        self.size = size
        self.kind = kind
        self.source = source


def parse_su(text, frames=None):
    """`path:line:col:function<TAB>bytes<TAB>static|dynamic[,bounded]` lines.

    The same name can appear more than once (two static functions in different
    files, or a clone); the largest frame wins, which is the safe direction for
    a budget.
    """
    frames = {} if frames is None else frames
    for line in text.splitlines():
        parts = line.rstrip("\r").split("\t")
        if len(parts) < 3:
            continue
        loc, size, kind = parts[0], parts[1], parts[2]
        try:
            nbytes = int(size)
        except ValueError:
            continue
        source, _, name = loc.rpartition(":")
        # path:line:col -> path
        source = source.rsplit(":", 2)[0] if source.count(":") >= 2 else source
        base = _CLONE.sub("", name)
        prev = frames.get(base)
        if prev is None or nbytes > prev.size or (
                nbytes == prev.size and "dynamic" in kind):
            frames[base] = Frame(base, nbytes, kind, source)
    return frames


def load_frames(build):
    frames = {}
    paths = sorted(Path(build).rglob("*.su"))
    for path in paths:
        parse_su(path.read_text(encoding="utf-8", errors="replace"), frames)
    return frames, len(paths)


def parse_config(text):
    values = {}
    for line in text.splitlines():
        m = re.match(r"^(CONFIG_[A-Za-z0-9_]+)=(.*)$", line.strip())
        if m:
            values[m.group(1)] = m.group(2).strip('"')
    return values


def parse_header_macros(text):
    values = {}
    for m in re.finditer(r"^\s*#\s*define\s+([A-Z][A-Z0-9_]*)\s+(\d+)u?\s*$",
                         text, re.MULTILINE):
        values[m.group(1)] = int(m.group(2))
    return values


def parse_chain(spec):
    """'name=f1,f2,...<=LIMIT' -> (name, [functions], limit token)."""
    m = re.match(r"^\s*([\w.-]+)\s*=\s*(.+?)\s*<=\s*(\S+)\s*$", spec, re.DOTALL)
    if not m:
        raise ValueError(f"chain {spec!r} is not 'name=f1,f2,...<=LIMIT'")
    funcs = [f.strip() for f in m.group(2).split(",") if f.strip()]
    if not funcs:
        raise ValueError(f"chain {m.group(1)!r} names no functions")
    return m.group(1), funcs, m.group(3)


def resolve_limit(token, config, macros):
    if re.fullmatch(r"\d+", token):
        return int(token)
    if token.startswith("CONFIG_"):
        if token not in config:
            raise ValueError(f"{token} is not set in the build's .config")
        return int(config[token], 0)
    if token in macros:
        return macros[token]
    raise ValueError(f"limit {token!r} is neither a number, a CONFIG_ symbol "
                     f"in the .config, nor a macro in a --header")


def check(frames, chains, exception_frame, frame_limit=None,
          frame_limit_paths=(), out=print):
    """Returns the number of failures; prints one line per finding."""
    failures = 0
    for name, funcs, limit in chains:
        total = exception_frame
        missing = []
        parts = []
        for func in funcs:
            frame = frames.get(func)
            if frame is None:
                missing.append(func)
                continue
            if "dynamic" in frame.kind:
                out(f"FAIL {name}: {func} has a dynamic frame ({frame.kind}); "
                    f"its size is not known at compile time")
                failures += 1
            total += frame.size
            parts.append(f"{func} {frame.size}")
        if missing:
            out(f"FAIL {name}: no frame for {', '.join(missing)} - inlined, "
                f"renamed or removed, so this chain no longer describes the "
                f"code")
            failures += 1
            continue
        verdict = "ok  " if total <= limit else "FAIL"
        out(f"{verdict} {name}: {total} B of {limit} ("
            f"{' + '.join(parts)} + exception frame {exception_frame})")
        if total > limit:
            failures += 1
    if frame_limit is not None:
        for frame in sorted(frames.values(), key=lambda f: -f.size):
            if frame.size <= frame_limit:
                break
            src = frame.source.replace("\\", "/")
            if any(p in src for p in frame_limit_paths):
                out(f"FAIL frame: {frame.name} is {frame.size} B, over the "
                    f"{frame_limit} B per-function limit ({src})")
                failures += 1
    return failures


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--build", required=True,
                    help="build directory (searched for *.su and .config)")
    ap.add_argument("--chain", action="append", required=True,
                    help="'name=f1,f2,...<=LIMIT', caller first; repeatable")
    ap.add_argument("--header", action="append", default=[],
                    help="header to read numeric #define limits from")
    ap.add_argument("--config", help="the .config to read CONFIG_ limits and "
                    "CONFIG_FPU from (default: the one under --build that "
                    "sits beside a zephyr.elf, i.e. the application's and "
                    "not sysbuild's)")
    ap.add_argument("--exception-frame", type=int,
                    help="bytes an interrupt stacks on the thread (default: "
                    "104 with CONFIG_FPU=y, else 32)")
    ap.add_argument("--frame-limit", type=int,
                    help="fail on any single frame over this many bytes ...")
    ap.add_argument("--frame-limit-path", action="append", default=[],
                    help="... defined in a source whose path contains this")
    args = ap.parse_args(argv)

    frames, nfiles = load_frames(args.build)
    if nfiles == 0:
        print(f"FAIL no .su files under {args.build}: build with "
              f"CONFIG_STACK_USAGE=y")
        return 1

    config = {}
    cfg_path = Path(args.config) if args.config else None
    if cfg_path is None:
        for cand in sorted(Path(args.build).rglob(".config")):
            if (cand.parent / "zephyr.elf").is_file():
                cfg_path = cand
                break
    if cfg_path is not None and cfg_path.is_file():
        config = parse_config(cfg_path.read_text(encoding="utf-8",
                                                  errors="replace"))
    macros = {}
    for header in args.header:
        macros.update(parse_header_macros(Path(header).read_text(
            encoding="utf-8", errors="replace")))

    exc = args.exception_frame
    if exc is None:
        exc = 104 if config.get("CONFIG_FPU") == "y" else 32

    try:
        chains = []
        for spec in args.chain:
            name, funcs, token = parse_chain(spec)
            chains.append((name, funcs, resolve_limit(token, config, macros)))
    except ValueError as err:
        print(f"FAIL {err}")
        return 1

    print(f"{nfiles} .su files, {len(frames)} functions, exception frame "
          f"{exc} B ({cfg_path if cfg_path else 'no .config'})")
    failures = check(frames, chains, exc, args.frame_limit,
                     args.frame_limit_path)
    print("stack budget: " + ("FAIL" if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
