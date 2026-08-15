# SPDX-License-Identifier: Apache-2.0

"""Assert that the paths this project's documentation points at exist.

The reorganisation into apps/ moved most of the tree and left the prose behind:
a document that says `src/main.c` now names nothing at all, and there was no way
to tell which of several hundred such mentions were stale, which were historical
on purpose, and which had simply always been wrong.

Fixing them by hand first would have left no way to know the job was finished
and no way to stop it recurring, so the checker comes first. It has three
checks at three different confidence levels, and only the first is allowed to
fail the build:

  (a) Markdown links with a relative target must resolve. This is mechanical
      and unambiguous - a broken [text](path) is a broken link, always.

  (b) Backtick-quoted things that LOOK like repository paths. Reported, never
      fatal. This one has real false positives: docs legitimately name files
      that no longer exist when describing history ("radiant_api.c, before it
      moved"), and there is no way to tell that from a stale reference by
      inspection. Promoting it to fatal needs the allowlist below to be worked
      through first; until then a warning that a human reads is worth more than
      a red check they learn to ignore.

  (c) `#L<n>` line anchors are REPORTED FOR DELETION rather than validated.
      They rot on the next edit above the line they name, and a symbol name is
      both stabler and more useful to the reader. Checking them would only
      make the rot loud; removing them fixes it.

Standard library only and sub-second, matching check_profile_registry.py and
check_license.py, which are its neighbours in the host-tests job.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

SKIP_DIRS = {
    ".git",
    "__pycache__",
    ".ruff_cache",
    ".pytest_cache",
    ".venv",
    ".vscode",
    "bench-logs",
    "dist",
}
SKIP_DIR_PREFIXES = ("build", "twister-out")

# [text](target) - the target group stops at whitespace or the closing paren so
# that a title (`[t](p "title")`) does not end up in the path.
MD_LINK = re.compile(r"\[[^\]]*\]\(\s*([^)\s]+)")

# Anything in backticks that looks like a path into this repository: contains a
# slash, or ends in one of the extensions this tree actually uses.
PATH_SUFFIXES = (
    ".c", ".h", ".py", ".ps1", ".cmake", ".md", ".yaml", ".yml", ".conf",
    ".overlay", ".ld", ".json", ".toml", ".txt", ".antser", ".antcap",
)
BACKTICK = re.compile(r"`([^`\n]+)`")

# Computed once at import: the real top-level directories of this repository.
# Deriving it beats hardcoding a list that goes stale the next time the tree is
# reorganised - which is the exact event that created this checker's workload.
TOP_LEVEL_DIRS = {
    p.name for p in REPO.iterdir()
    if p.is_dir() and not p.name.startswith(".")
    and p.name not in SKIP_DIRS
    and not p.name.startswith(SKIP_DIR_PREFIXES)
}

# Skipped by (a): not repository paths.
EXTERNAL_PREFIXES = ("http://", "https://", "mailto:", "#", "ftp://")

# Lines carrying this comment exempt the NEXT line from check (b). The escape
# exists so a document can name a file it is describing the removal of.
IGNORE_NEXT = "<!-- link-check: ignore-next -->"

# Deliberately-historical mentions, exempt from (b) only. Each entry is a claim
# that the path is named on purpose and does not exist - keep the reason.
BACKTICK_ALLOWLIST: dict[str, str] = {
    # ADR 0002 uses this as a normative policy boundary, not as a pointer at a
    # file. An ADR is a dated record of a decision; annotating it is fine,
    # editing the decision text is not.
    "src/profiles/": "ADR 0002 policy boundary, not a live path",
}


def iter_markdown() -> list[Path]:
    out = []
    for path in sorted(REPO.rglob("*.md")):
        parts = path.relative_to(REPO).parts
        if any(p in SKIP_DIRS for p in parts):
            continue
        if any(p.startswith(SKIP_DIR_PREFIXES) for p in parts[:-1]):
            continue
        out.append(path)
    return out


def looks_like_repo_path(text: str) -> bool:
    """Is this backticked text a path INTO THIS REPOSITORY?

    Deliberately strict, because the first version of this function was not and
    the result was 766 reports that no one would ever read. Three quarters of
    them were not repository paths at all: bare extensions (`.conf`, `.antcap`)
    used as nouns, glob patterns (`tools/test_*.py`), filename placeholders
    (`<date>-<backend>.json`), and paths into the NCS tree
    (`nrf/samples/openthread/cli`). A check that cannot be read is not a check,
    so the rule is now: it must start with a real top-level directory of this
    repository. Bare filenames like `ant_verify.py` are dropped on purpose -
    they are ambiguous, extremely common, and almost never the stale ones.
    """
    if text.startswith(EXTERNAL_PREFIXES) or " " in text:
        return False
    if "/" not in text:
        return False
    # Globs, placeholders and command fragments are prose, not paths.
    if any(c in text for c in "*?<>;|$"):
        return False
    first = text.split("/", 1)[0]
    return first in TOP_LEVEL_DIRS


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--strict-backticks", action="store_true",
                        help="promote check (b) from warning to failure")
    args = parser.parse_args()

    broken: list[str] = []       # (a) - fatal
    suspect: list[str] = []      # (b) - warning by default
    anchors: list[str] = []      # (c) - reported

    files = iter_markdown()
    n_links = 0

    for path in files:
        rel = path.relative_to(REPO).as_posix()
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()

        for lineno, line in enumerate(lines, start=1):
            ignored = lineno >= 2 and IGNORE_NEXT in lines[lineno - 2]

            # (a) markdown links
            for target in MD_LINK.findall(line):
                if target.startswith(EXTERNAL_PREFIXES):
                    continue
                n_links += 1
                bare, _, anchor = target.partition("#")
                if anchor and re.fullmatch(r"L\d+(-L\d+)?", anchor):
                    anchors.append(f"{rel}:{lineno}: {target}")
                if not bare:
                    continue
                resolved = (path.parent / bare).resolve()
                if not resolved.exists():
                    broken.append(f"{rel}:{lineno}: {target}")

            # (b) backtick-quoted paths
            if ignored:
                continue
            for text in BACKTICK.findall(line):
                if not looks_like_repo_path(text):
                    continue
                if text in BACKTICK_ALLOWLIST:
                    continue
                candidate = REPO / text
                if candidate.exists():
                    continue
                # Also accept it as relative to the document itself.
                if (path.parent / text).exists():
                    continue
                suspect.append(f"{rel}:{lineno}: `{text}`")

    print(f"check_links: {len(files)} markdown files, {n_links} relative links")

    if anchors:
        print(f"\n{len(anchors)} line anchor(s) - prefer a symbol name; these "
              f"rot on the next edit above the line:", file=sys.stderr)
        for a in anchors[:40]:
            print(f"  {a}", file=sys.stderr)

    if suspect:
        label = "ERROR" if args.strict_backticks else "warning"
        print(f"\n{len(suspect)} backtick-quoted path(s) that do not resolve "
              f"({label} - some are deliberately historical):", file=sys.stderr)
        for s in suspect[:60]:
            print(f"  {s}", file=sys.stderr)
        if len(suspect) > 60:
            print(f"  ... and {len(suspect) - 60} more", file=sys.stderr)

    if broken:
        print(f"\n{len(broken)} BROKEN markdown link(s):", file=sys.stderr)
        for b in broken:
            print(f"  {b}", file=sys.stderr)
        return 1

    if suspect and args.strict_backticks:
        return 1

    print("check_links: OK - every relative markdown link resolves")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
