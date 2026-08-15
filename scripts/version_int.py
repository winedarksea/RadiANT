# SPDX-License-Identifier: Apache-2.0

"""The repository's version, in the two forms other tools need.

The root VERSION file is the single source of truth. This exists so that the
release workflow and scripts/package_dfu.ps1 cannot disagree about what the
version is, and so that a git tag cannot disagree with either.

Why it matters that the DFU number is derived rather than written down: until
2026-08-14 both the workflow and the packaging script passed the literal `1`,
so every DFU package ever published declared itself version 1. The nRF52840
Dongle's bootloader uses that number to refuse a downgrade, and comparing 1
against 1 makes the protection inert - without any symptom. Deriving it is the
fix; the tag check below is what stops the source of truth drifting from the
thing people actually read.

Usage:
    python scripts/version_int.py              -> 100
    python scripts/version_int.py --string     -> 0.1.0
    python scripts/version_int.py --check-tag v0.1.0

Standard library only and sub-second, matching the other checks in host-tests.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
VERSION_FILE = REPO / "VERSION"

# Two decimal digits each for minor and patch. Must stay in step with
# Get-RadiantAppVersion in scripts/package_dfu.ps1 - the comment there says so
# too, because a change to one and not the other produces packages whose
# versions are ordered differently from the tags they came from.
MINOR_SCALE = 100
MAJOR_SCALE = 10000


def read_version() -> tuple[int, int, int]:
    if not VERSION_FILE.is_file():
        raise SystemExit(f"No VERSION file at {VERSION_FILE}")

    fields: dict[str, str] = {}
    for line in VERSION_FILE.read_text(encoding="utf-8").splitlines():
        m = re.match(r"^\s*([A-Z_]+)\s*=\s*(.*?)\s*$", line)
        if m:
            fields[m.group(1)] = m.group(2)

    try:
        major = int(fields["VERSION_MAJOR"])
        minor = int(fields["VERSION_MINOR"])
        patch = int(fields["PATCHLEVEL"])
    except (KeyError, ValueError) as exc:
        raise SystemExit(
            f"VERSION is not in Zephyr's format (VERSION_MAJOR/VERSION_MINOR/"
            f"PATCHLEVEL): {exc}"
        ) from exc

    for name, value in (("VERSION_MINOR", minor), ("PATCHLEVEL", patch)):
        if not 0 <= value <= 99:
            raise SystemExit(
                f"VERSION field {name} is {value}; the DFU encoding gives it "
                f"two decimal digits, so it must be 0-99. Widen it here and in "
                f"scripts/package_dfu.ps1 together."
            )
    if major < 0:
        raise SystemExit(f"VERSION_MAJOR is {major}")

    return major, minor, patch


def version_int(major: int, minor: int, patch: int) -> int:
    return major * MAJOR_SCALE + minor * MINOR_SCALE + patch


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--string", action="store_true",
                       help="print MAJOR.MINOR.PATCH instead of the integer")
    group.add_argument("--check-tag", metavar="TAG",
                       help="assert a v-prefixed git tag matches VERSION")
    args = parser.parse_args()

    major, minor, patch = read_version()
    text = f"{major}.{minor}.{patch}"

    if args.check_tag:
        tag = args.check_tag
        if not tag.startswith("v"):
            print(f"tag {tag!r} does not start with 'v'", file=sys.stderr)
            return 1
        if tag[1:] != text:
            print(
                f"tag {tag} disagrees with VERSION ({text}). A release whose "
                f"tag and VERSION file differ produces a DFU package whose "
                f"declared version does not match the release it is attached "
                f"to - fix VERSION and re-tag rather than overriding this.",
                file=sys.stderr,
            )
            return 1
        print(f"tag {tag} agrees with VERSION ({text})")
        return 0

    print(text if args.string else version_int(major, minor, patch))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
