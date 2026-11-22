#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Rename RadiANT's pre-release API names to the radiant_ namespace.

Before the first release that ships the module's ANT API, three families of
names moved into RadiANT's own namespace (TODO "Module packaging follow-ups",
ADR 0003 amendment 2026-11-22):

    antr_* / ANTR_*          -> radiant_ant_* / RADIANT_ANT_*
    <radiant/ant_radio.h>    -> <radiant/radiant_ant.h>
    CONFIG_RADIANT_ANTR_API  -> CONFIG_RADIANT_ANT_API
    ANTW_*, <radiant/ant_wire.h>
                             -> RADIANT_WIRE_*, <radiant/radiant_wire.h>
    profile_* / PROFILE_*    -> radiant_profile_* / RADIANT_PROFILE_*
    node_* / NODE_*          -> radiant_node_* / RADIANT_NODE_*
                                (only the names the module's headers export)

Run it on your own application code if you tracked RadiANT's master branch
between 2026-11-09 and this rename:

    python scripts/migrate_radiant_names.py --module <radiant checkout> <your dirs>

With no paths it rewrites this repository's tracked files (minus archive/,
captures/, bench-logs/, build*/, dist/, TODO.md and released CHANGELOG
sections). --check reports leftover old names and exits 1 if any remain.

The profile/node map is read from the module's headers, in either their old
(profile_hr.h) or new (radiant_profile_hr.h) form, so the script works before
and after the module itself has been migrated. A blind prefix regex would
rename unrelated locals such as `node_id`; this only renames what a header
declares.

The script refuses to run if a target name already exists alongside its old
name, which would merge two distinct identifiers.

This script is temporary: delete it after the first release that ships
radiant_ant_* (see TODO.md).
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

EXCLUDE_DIRS = ("archive", "captures", "bench-logs", "dist", ".git")
EXCLUDE_FILES = {
    "TODO.md",  # open items describe the rename itself
    "scripts/migrate_radiant_names.py",
    "scripts/gen_ant_wire.py",  # its generated outputs change by hand
    "docs/decisions/0003-naming-trademark-and-usb-identity.md",  # amended by hand
}
TEXT_SUFFIXES = {
    ".c", ".h", ".cpp", ".hpp", ".py", ".md", ".txt", ".cmake", ".yml", ".yaml",
    ".conf", ".overlay", ".ld", ".ps1", ".sh", ".json", ".rst", ".toml", ".ini",
    ".defconfig", ".dts", ".dtsi", ".cfg",
}
TEXT_NAMES = {"CMakeLists.txt", "Kconfig", "west.yml"}

PHASES = ("wire", "ant", "profiles")

# ---------------------------------------------------------------------------
# Fixed rewrites, applied in order before the prefix rules. Paths first, so a
# file name is rewritten whole rather than piecewise.
# ---------------------------------------------------------------------------
WIRE_FIXED = [
    (r"\bANTW_EQ\b", "WIRE_EQ"),
    (r'#include\s+[<"](?:radiant/|ant/)?ant_wire\.h[>"]', "#include <radiant/radiant_wire.h>"),
    (r"\bradiant/include/radiant/ant_wire\.h\b", "radiant/include/radiant/radiant_wire.h"),
    (r"<radiant/ant_wire\.h>", "<radiant/radiant_wire.h>"),
    (r"(?<![\w/])src/ant_wire\.h\b", "radiant/include/radiant/radiant_wire.h"),
    (r"(?<![\w/])ant_wire\.h\b", "radiant_wire.h"),
    (r"\bANTW_", "RADIANT_WIRE_"),
]

ANT_FIXED = [
    (r"\bKconfig\.antr_api\b", "Kconfig.ant_api"),
    # No leading \b: the usual spelling is CONFIG_RADIANT_ANTR_API.
    (r"RADIANT_ANTR_API\b", "RADIANT_ANT_API"),
    (r'#include\s+[<"](?:radiant/|ant/)?ant_radio\.h[>"]', "#include <radiant/radiant_ant.h>"),
    (r"<radiant/ant_radio\.h>", "<radiant/radiant_ant.h>"),
    (r"\bradiant/include/radiant/ant_radio\.h\b", "radiant/include/radiant/radiant_ant.h"),
    (r"\bANT_RADIO_H_\b", "RADIANT_RADIANT_ANT_H_"),
    (r"\bANT_RADIO_RADIANT_PRIV_H_\b", "RADIANT_ANT_PRIV_H_"),
    (r"\bANT_RADIO_RADIANT_H_\b", "RADIANT_ANT_INTERNAL_H_"),
    (r"\bsrc/antr/", "src/ant_api/"),
    (r"\bantr/(?=ant_)", "ant_api/"),
    (r"\bant_radio_radiant_priv\.h\b", "radiant_ant_priv.h"),
    (r"\bant_radio_radiant\.h\b", "radiant_ant_internal.h"),
    (r"\bant_radio_radiant\.c\b", "radiant_ant.c"),
    (r"\bant_radio_radiant\b", "radiant_ant"),
    (r"\bant_sec_host\.c\b", "radiant_ant_sec.c"),
    (r"\bant_radio\.h\b", "radiant_ant.h"),
    (r"\bantr_", "radiant_ant_"),
    (r"\bANTR_", "RADIANT_ANT_"),
]

# Old-name detectors for --check and the collision check.
OLD_PATTERNS = {
    "wire": r"\bANTW_\w*",
    "ant": r"\b(?:antr_\w*|ANTR_\w*)|RADIANT_ANTR_API\b",
}

IDENT_RE = re.compile(r"\b(?:radiant_)?(?:profile|node)_\w+|\b(?:RADIANT_)?(?:PROFILE|NODE)_\w+")


def strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", " ", src)


def old_form(name: str) -> str:
    if name.startswith("radiant_"):
        return name[len("radiant_"):]
    if name.startswith("RADIANT_"):
        return name[len("RADIANT_"):]
    return name


def new_form(old: str) -> str:
    return ("RADIANT_" if old[0].isupper() else "radiant_") + old


def profile_map(module: Path) -> tuple[dict[str, str], dict[str, str]]:
    """Identifier map and file-stem map, built from the module's headers."""
    ids: dict[str, str] = {}
    stems: dict[str, str] = {}
    for sub in ("profiles", "node"):
        d = module / "src" / sub
        if not d.is_dir():
            continue
        prefix = "profile_" if sub == "profiles" else "node_"
        for f in sorted(d.iterdir()):
            if f.suffix not in (".c", ".h"):
                continue
            stem = old_form(f.stem)
            if stem.startswith(prefix):
                stems[stem] = new_form(stem)
            if f.suffix != ".h":
                continue
            for m in IDENT_RE.finditer(strip_comments(f.read_text(encoding="utf-8"))):
                old = old_form(m.group(0))
                if old.endswith("_H_"):
                    continue  # header guards, handled below
                ids[old] = new_form(old)
    if ids:
        ids["NODE_IDENT_H_"] = "RADIANT_NODE_IDENT_H_"
        ids["NODE_NVM_H_"] = "RADIANT_NODE_NVM_H_"
    return ids, stems


def tracked_files(root: Path, paths: list[str]) -> list[Path]:
    if not paths:
        out = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z"], check=True, capture_output=True
        ).stdout.decode("utf-8")
        rels = [p for p in out.split("\0") if p]
        files = [root / p for p in rels]
    else:
        files = []
        for p in paths:
            pp = Path(p)
            if pp.is_file():
                files.append(pp)
            else:
                for dirpath, dirnames, filenames in os.walk(pp):
                    dirnames[:] = [
                        d for d in dirnames
                        if d not in EXCLUDE_DIRS and not d.startswith("build")
                    ]
                    files += [Path(dirpath) / f for f in filenames]
    result = []
    for f in files:
        try:
            rel = f.resolve().relative_to(root).as_posix()
        except ValueError:
            rel = f.as_posix()
        top = rel.split("/", 1)[0]
        if top in EXCLUDE_DIRS or top.startswith("build") or rel in EXCLUDE_FILES:
            continue
        if f.suffix not in TEXT_SUFFIXES and f.name not in TEXT_NAMES \
                and not f.name.startswith("Kconfig"):
            continue
        if not f.is_file():
            continue
        result.append(f)
    return result


def split_changelog(text: str) -> tuple[str, str]:
    """Only the [Unreleased] section is rewritten; released entries are history."""
    heads = [m.start() for m in re.finditer(r"(?m)^## \[", text)]
    if len(heads) < 2:
        return text, ""
    return text[:heads[1]], text[heads[1]:]


class Rewriter:
    def __init__(self, phases: list[str], module: Path):
        self.fixed: list[tuple[re.Pattern, str]] = []
        if "wire" in phases:
            self.fixed += [(re.compile(a), b) for a, b in WIRE_FIXED]
        if "ant" in phases:
            self.fixed += [(re.compile(a), b) for a, b in ANT_FIXED]
        self.ids: dict[str, str] = {}
        self.stems: dict[str, str] = {}
        if "profiles" in phases:
            self.ids, self.stems = profile_map(module)
            if not self.ids:
                sys.exit("no profile/node headers found under %s/src" % module)
        self.id_re = self._alt(self.ids, r"\b(%s)\b")
        # A file stem only when followed by .c/.h, so `profile_power` the struct
        # and `profile_power_decode.c` the file are each handled once.
        self.stem_re = self._alt(self.stems, r"\b(%s)(?=\.[ch]\b)")
        self.phases = phases

    @staticmethod
    def _alt(names: dict[str, str], shape: str):
        if not names:
            return None
        alt = "|".join(re.escape(n) for n in sorted(names, key=len, reverse=True))
        return re.compile(shape % alt)

    def apply(self, text: str) -> str:
        for rx, rep in self.fixed:
            text = rx.sub(rep, text)
        if self.stem_re:
            text = self.stem_re.sub(lambda m: self.stems[m.group(1)], text)
        if self.id_re:
            text = self.id_re.sub(lambda m: self.ids[m.group(1)], text)
        return text

    def old_names(self, text: str) -> set[str]:
        found: set[str] = set()
        for ph in self.phases:
            if ph in OLD_PATTERNS:
                found |= set(re.findall(OLD_PATTERNS[ph], text))
        if self.id_re:
            found |= {m.group(1) for m in self.id_re.finditer(text)}
        return found


def collision_targets(rw: Rewriter, old: set[str]) -> dict[str, str]:
    """old name -> the name it becomes, for the names present in the tree."""
    out = {}
    for name in old:
        if name in rw.ids:
            out[name] = rw.ids[name]
        else:
            out[name] = rw.apply(name)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("paths", nargs="*", help="files/dirs to rewrite (default: this repo)")
    ap.add_argument("--phase", choices=PHASES + ("all",), default="all")
    ap.add_argument("--module", type=Path, default=ROOT / "radiant",
                    help="radiant module dir whose headers give the profile/node map")
    ap.add_argument("--check", action="store_true",
                    help="report old names left; change nothing; exit 1 if any")
    args = ap.parse_args()

    phases = list(PHASES) if args.phase == "all" else [args.phase]
    rw = Rewriter(phases, args.module)
    files = tracked_files(ROOT, args.paths)

    texts: dict[Path, tuple[str, str]] = {}
    old_seen: dict[str, set[Path]] = {}
    all_words: set[str] = set()
    for f in files:
        try:
            raw = f.read_bytes()
            text = raw.decode("utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        head, tail = split_changelog(text) if f.name == "CHANGELOG.md" else (text, "")
        texts[f] = (head, tail)
        all_words |= set(re.findall(r"\b\w+\b", head))
        for n in rw.old_names(head):
            old_seen.setdefault(n, set()).add(f)

    if args.check:
        for n in sorted(old_seen):
            where = ", ".join(sorted(p.as_posix() for p in old_seen[n])[:3])
            print("%-48s %s" % (n, where))
        print("%d old name(s) in %d file(s)" % (
            len(old_seen), len({p for s in old_seen.values() for p in s})))
        return 1 if old_seen else 0

    targets = collision_targets(rw, set(old_seen))
    # Every ANTW_X was defined as an alias of RADIANT_WIRE_X, so for the wire
    # phase "the target exists" is the point, not a collision.
    clashes = sorted(
        (o, n) for o, n in targets.items()
        if n != o and n in all_words and not o.startswith("ANTW_")
    )
    if clashes:
        for o, n in clashes:
            print("collision: %s -> %s, but %s already exists" % (o, n, n), file=sys.stderr)
        return 2

    changed = 0
    for f, (head, tail) in texts.items():
        new = rw.apply(head)
        if new != head:
            f.write_bytes((new + tail).encode("utf-8"))
            changed += 1
    print("rewrote %d file(s); %d distinct old name(s)" % (changed, len(old_seen)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
