# SPDX-License-Identifier: Apache-2.0

"""Assert the licence the tree claims is the licence the tree ships.

This exists because it already went wrong. Until 2026-08-14 the LICENSE file at
the repository root was the **Mozilla Public License 2.0**, complete with
Exhibit B's "Incompatible With Secondary Licenses" notice, while NOTICE,
docs/decisions/0004-license-apache-2-0.md, docs/third-party.md and every one of
the 231 source files carrying an SPDX tag said Apache-2.0 - and ADR 0004
*rejects MPL-2.0 by name*, on the grounds that per-file copyleft on firmware
that gets statically linked into a customer image creates ongoing compliance
questions. Nothing caught it, because nothing was looking.

ADR 0004's own `Checked by:` line asks for exactly this check and notes it
"should become a CI check when there is a job to hang it on". host-tests is
that job: no secret, so it runs on forks and on every pull request.

Three assertions:

  1. LICENSE is the Apache-2.0 text and contains no trace of another licence.
  2. NOTICE agrees with it.
  3. Every source file either carries `SPDX-License-Identifier: Apache-2.0` or
     is on an explicit, reasoned allowlist.

Standard library only and sub-second, matching check_profile_registry.py, which
is the neighbouring check in the same job.
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Extensions we assert on. Deliberately not .md, .yaml, .conf, .overlay or
# .ld: this project puts load-bearing prose in configuration files and a
# mandatory licence banner on top of a .conf whose first line is a heading
# would be noise that hides the heading. The rule is the same one ADR 0002
# applies to Provenance: cover the files a court would call source.
SOURCE_SUFFIXES = {".c", ".h", ".py", ".ps1", ".cmake"}
SOURCE_NAMES = {"CMakeLists.txt"}

# Directories never walked. bench-logs/ and archive/ hold recordings and
# preserved third-party artefacts, not our source.
SKIP_DIRS = {
    ".git",
    "__pycache__",
    ".ruff_cache",
    ".pytest_cache",
    ".venv",
    ".vscode",
    "bench-logs",
    "archive",
    "dist",
}

# Build trees, matched by prefix rather than by name. A west build drops tens of
# thousands of generated .c/.h/.cmake files - Zephyr's syscall headers alone are
# hundreds - and none of them is ours to license. This mirrors .gitignore's
# `build*/` and `twister-out*/` rather than inventing a second rule, and it is a
# prefix test because the tree carries build/, build_p4 output dirs and
# twister-out/ all at once.
SKIP_DIR_PREFIXES = ("build", "twister-out")

# Files exempt from the SPDX rule, each with the reason. An entry here is a
# claim that needs to survive review - "it was awkward" is not one.
#
# The rule these entries share: VENDORED THIRD-PARTY CODE KEEPS ITS UPSTREAM
# HEADER, VERBATIM. Retagging somebody else's file with our own SPDX line to
# make a checker happy would be a false licence claim - the exact failure mode
# this script was written for, pointed the other way. So the file keeps its
# header and the exemption is recorded here, by name, with the licence it
# actually carries.
#
# Note what is NOT on this list: every vendored .cpp. This checker's
# SOURCE_SUFFIXES covers .c and not .cpp, so the C++ half of the same vendored
# directory is never examined. That is a gap in the checker rather than a
# decision about those files, and it is left alone here because widening
# SOURCE_SUFFIXES is a change to what the whole tree is asserted about and
# belongs in its own commit with its own reasoning.
#
# radiant/coex154_ti/ieee802154_cc13xx_cc26xx.c, a fork of Zephyr's TI 802.15.4
# driver, is the other vendored file in the tree and needs no entry: it is
# itself Apache-2.0 and satisfies the rule as it stands.
ALLOWLIST: dict[str, str] = {
    # ── NCS's Matter bridge core (package E3) ──────────────────────────────
    #
    # Copied byte-for-byte from
    # C:\ncs\v3.4.0\nrf\applications\matter_bridge\src\core\ - see
    # apps/dongle_thread/src/matter/README.md for the provenance table and the
    # list of what was deliberately left behind. These carry Nordic's
    # LicenseRef-Nordic-5-Clause, which permits redistribution in source and
    # binary form for use with Nordic silicon; docs/third-party.md is where
    # that obligation is tracked.
    "apps/dongle_thread/src/matter/core/bridge_manager.h": "NCS matter_bridge, LicenseRef-Nordic-5-Clause, vendored verbatim",
    "apps/dongle_thread/src/matter/core/bridge_storage_manager.h": "NCS matter_bridge, LicenseRef-Nordic-5-Clause, vendored verbatim",
    "apps/dongle_thread/src/matter/core/bridged_device_data_provider.h": "NCS matter_bridge, LicenseRef-Nordic-5-Clause, vendored verbatim",
    "apps/dongle_thread/src/matter/core/matter_bridged_device.h": "NCS matter_bridge, LicenseRef-Nordic-5-Clause, vendored verbatim",
    "apps/dongle_thread/src/matter/core/util/bridge_util.h": "NCS matter_bridge, LicenseRef-Nordic-5-Clause, vendored verbatim",
    # ── ZAP output, committed (package E3) ─────────────────────────────────
    #
    # These ARE Apache-2.0 - "Copyright (c) 2022 Project CHIP Authors" and the
    # full licence text in the banner - but ZAP's templates emit the long form
    # and no SPDX identifier, so the checker's first-15-lines test cannot see
    # it. They are generated output committed on purpose: the build runs with
    # BYPASS_IDL, so this directory IS the data model rather than a cache of
    # it, and it must not be edited to add a tag any more than it may be
    # edited for anything else. See src/matter/README.md, trap 13.
    "apps/dongle_thread/src/matter/default_zap/zap-generated/access.h": "ZAP output, Apache-2.0 in long form without an SPDX line",
    "apps/dongle_thread/src/matter/default_zap/zap-generated/CodeDrivenCallback.h": "ZAP output, Apache-2.0 in long form without an SPDX line",
    "apps/dongle_thread/src/matter/default_zap/zap-generated/endpoint_config.h": "ZAP output, Apache-2.0 in long form without an SPDX line",
    "apps/dongle_thread/src/matter/default_zap/zap-generated/gen_config.h": "ZAP output, Apache-2.0 in long form without an SPDX line",
    "apps/dongle_thread/src/matter/default_zap/zap-generated/PluginApplicationCallbacks.h": "ZAP output, Apache-2.0 in long form without an SPDX line",
}

APACHE_MARKERS = ("Apache License", "Version 2.0, January 2004")

# Substrings whose presence in LICENSE means it is not (only) Apache-2.0.
# "Mozilla" is here because that is the mistake this check was written for.
FOREIGN_LICENCE_MARKERS = (
    "Mozilla",
    "GNU GENERAL PUBLIC LICENSE",
    "Permission is hereby granted, free of charge",  # MIT
    "Redistribution and use in source and binary forms",  # BSD
)


def iter_sources():
    for path in sorted(REPO.rglob("*")):
        if not path.is_file():
            continue
        parts = path.relative_to(REPO).parts
        if any(part in SKIP_DIRS for part in parts):
            continue
        if any(part.startswith(SKIP_DIR_PREFIXES) for part in parts[:-1]):
            continue
        if path.suffix in SOURCE_SUFFIXES or path.name in SOURCE_NAMES:
            yield path


def main() -> int:
    problems: list[str] = []

    # 1. LICENSE itself.
    license_path = REPO / "LICENSE"
    if not license_path.is_file():
        problems.append("LICENSE: missing")
    else:
        text = license_path.read_text(encoding="utf-8", errors="replace")
        for marker in APACHE_MARKERS:
            if marker not in text:
                problems.append(f"LICENSE: does not look like Apache-2.0 (no {marker!r})")
        for marker in FOREIGN_LICENCE_MARKERS:
            if marker in text:
                problems.append(
                    f"LICENSE: contains {marker!r}, so it is not purely Apache-2.0. "
                    "See docs/decisions/0004-license-apache-2-0.md."
                )

    # 2. NOTICE agrees.
    notice_path = REPO / "NOTICE"
    if not notice_path.is_file():
        problems.append("NOTICE: missing")
    else:
        notice = notice_path.read_text(encoding="utf-8", errors="replace")
        if "Apache License, Version 2.0" not in notice:
            problems.append("NOTICE: does not name Apache-2.0")

    # 3. SPDX tags. Read only the head: a tag buried 200 lines down is not a
    # licence banner, and requiring it near the top is what makes it findable.
    checked = 0
    for path in iter_sources():
        rel = path.relative_to(REPO).as_posix()
        if rel in ALLOWLIST:
            continue
        checked += 1
        head = "".join(path.read_text(encoding="utf-8", errors="replace").splitlines(keepends=True)[:15])
        if "SPDX-License-Identifier: Apache-2.0" in head:
            continue
        if "SPDX-License-Identifier:" in head:
            problems.append(f"{rel}: SPDX tag present but not Apache-2.0")
        else:
            problems.append(f"{rel}: no SPDX-License-Identifier in the first 15 lines")

    if problems:
        print(f"check_license: {len(problems)} problem(s) across {checked} source files\n", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1

    print(f"check_license: OK - LICENSE and NOTICE are Apache-2.0, {checked} source files tagged")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
