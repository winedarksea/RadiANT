#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Machine-check the RadiANT profile registry and the docs that mirror it.

    C:\\ncs\\toolchains\\dcbdc366a1\\opt\\bin\\python.exe scripts\\check_profile_registry.py

A device type registry is a coordination point, and a coordination point that
nobody validates decays into a list of near-misses: two rows claiming 0x60, a
page range that overlaps the one above it, a period that stopped matching the
code six months ago. None of those produce an error on the air - the CRC still
passes, the channel still opens, the numbers are just wrong - which is exactly
the class of mistake worth spending a script on.

What it checks:

  * every required column is present and non-empty on every row
  * device types parse as hex, are unique, and lie in 1..127 (the MSB of the
    device type field is the pairing bit, so 0x80 and above do not exist)
  * page numbers and page ranges parse, do not overlap, and do not repeat
    within a device type
  * a device type with status `radiant` accounts for all 256 page numbers, and
    keeps 0x20-0x2F reserved for the security envelope - that reservation is
    the part of docs/radiant-security.md that cannot be retrofitted
  * EVERY device type with status `radiant` has a page map registered in
    RADIANT_PAGE_MAPS, and that page map agrees with the registry's page rows
    number for number and name for name - 0x60's in docs/radiant-telemetry.md,
    each profile type's in docs/device-profiles.md. The relation is checked in
    both directions, so a claimed type with no page map and a page map with no
    claim are both errors
  * the device type table in docs/device-profiles.md agrees with the registry
  * every device type and period that tools/ant_pages.py defines appears in the
    registry with the same period, so the registry cannot drift from the code
  * the common-page cadence stated in the docs still matches
    ant_pages.COMMON_PAGE_INTERVAL
  * the security pins are still stated - the reserved epoch and counter fields,
    the trailing tag space, the nonce's domain byte, the legal MAC window set,
    the tag length, the epoch-advance rules, the descriptor authentication
    frame, and the page 80/81/82 privacy rules. See SPEC_PINS for why each one
    is the kind of thing that disappears in an edit and breaks nothing until it
    breaks interoperability
  * the compat pins of docs/decisions/0008 - the compat domain byte and its
    subtypes, the two tag lengths, the legal N and K sets, the derived-locator
    formula, the policy default and its precedence, the two-page allocation and
    0x79's permanent exclusion - plus the arity and range rules that any compat
    page row and any compat constant must satisfy the day C2/C3 create them

Standard library only, and it imports tools/ant_pages.py by path rather than by
package, because tools/ is not a package and this script has to run from
anywhere.

Exit status is 0 when everything agrees, 1 otherwise, with one line per
problem naming the file and the line.
"""

from __future__ import annotations

import datetime
import importlib.util
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

REGISTRY = REPO / "docs" / "profile-registry.md"
TELEMETRY = REPO / "docs" / "radiant-telemetry.md"
PROFILES = REPO / "docs" / "device-profiles.md"
# Was docs/ant-plus-profiles.md, which docs/device-profiles.md absorbed. The
# name is kept because it names the ROLE this path plays in the checks below -
# it is where the ANT+ device type table lives - and that role did not move.
ANT_PLUS = PROFILES
SECURITY = REPO / "docs" / "radiant-security.md"
ANT_PAGES = REPO / "tools" / "ant_pages.py"
RADIANT_CRYPTO = REPO / "tools" / "radiant_crypto.py"
SEC_HEADER = REPO / "radiant_core" / "include" / "radiant_core" / "radiant_sec.h"
SEC_COMPAT_HEADER = (REPO / "radiant_core" / "include" / "radiant_core" /
                     "radiant_sec_compat.h")
COMPAT_SOURCE = REPO / "radiant_core" / "src" / "radiant_sec_compat.c"

MARKER = re.compile(r"^<!--\s*radiant-registry:\s*([a-z0-9-]+)\s*-->\s*$")
SEPARATOR = re.compile(r"^[\s:|-]+$")
DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
HEX8 = re.compile(r"^0x([0-9A-Fa-f]{2})$")
HEX8_RANGE = re.compile(r"^0x([0-9A-Fa-f]{2})-0x([0-9A-Fa-f]{2})$")

# An em dash in a table cell means "this project has not implemented it and
# will not record a number it has not verified". Spelled as an escape so this
# file stays ASCII.
UNRECORDED = "\u2014"
UNRECORDED_SPELLINGS = {UNRECORDED, "-", "--", "n/a", "none recorded"}

DEVICE_TYPE_COLUMNS = ["type", "name", "status", "claimant", "date", "period",
                       "lr phy", "adaptive freq", "notes"]
PAGE_COLUMNS = ["type", "page", "name", "schema", "notes"]
PAGEMAP_COLUMNS = ["page", "name", "summary"]
ANT_PLUS_COLUMNS = ["type", "name", "period", "implemented in tools/ant_pages.py"]

STATUS_VALUES = {"radiant", "third-party", "ant-plus-reserved"}

# `Adaptive freq` keeps the plain three-value opt-in: a type is on RF 57, it is
# not, or its nodes each say. There is nothing to budget from the answer.
OPT_IN_VALUES = {"no", "yes", "per-node"}

# `LR PHY` does NOT, and the split is ADR 0007's.
#
# A consumer cannot size a receive window from "yes": at eight bytes an S=8
# frame is ~1.3 ms against ~150 us at 1 M, which is the difference between a
# slot that fits and one that does not. So the column carries a CODING RATE, in
# the vocabulary RF-5a defined ahead of time for exactly this moment - the same
# names as `enum profile_sched_coding` in src/profiles/profile_schedule.h and
# `TLM_CODING_*` in tools/ant_pages.py, which are one vocabulary and not three.
#
# `yes` IS DELIBERATELY GONE rather than accepted as a synonym. It is the value
# that cannot be budgeted from, so keeping it would preserve exactly the defect
# the split exists to remove - and no row has ever used it, so nothing breaks.
# `s2` is listed because the vocabulary defines it; no build implements it, and
# a row claiming it would be a type nothing can talk to.
LR_PHY_VALUES = {"no", "s8", "s2", "per-node"}

# The page range docs/radiant-security.md depends on staying free. Reserving it
# costs nothing today and is a format break for every deployed node later.
SECURITY_RESERVED_RANGE = "0x20-0x2f"

# Device type constant -> period constant, in tools/ant_pages.py. The registry
# has to agree with whichever values that file holds.
ANT_PAGES_TYPES = [
    ("BPWR_DEVICE_TYPE", "BPWR_PERIOD"),
    ("BSC_SPEED_DEVICE_TYPE", "BSC_SPEED_PERIOD"),
    ("BSC_CADENCE_DEVICE_TYPE", "BSC_CADENCE_PERIOD"),
    ("BSC_COMBINED_DEVICE_TYPE", "BSC_COMBINED_PERIOD"),
    ("HRM_DEVICE_TYPE", "HRM_PERIOD"),
    # The profile device types. 0x60 is absent from this list on purpose - its
    # period is per-node and announced in the descriptor, so there is no number
    # to agree on. A profile type FIXES a period, which is most of what a
    # profile is for, so both of these do have one and it is checked.
    ("CGM_DEVICE_TYPE", "CGM_PERIOD"),
    ("FAN_DEVICE_TYPE", "FAN_PERIOD"),
]

# Device types whose registry Period column records a DEFAULT out of a permitted
# set, and the tuple in tools/ant_pages.py that holds the set. `period` is a
# manufacturer setting under docs/decisions/0008, and it is the one setting in
# that design a receiver cannot skip past: get it wrong and the channel does not
# open at all, which is a harder failure than an unknown page number can
# produce. So the alternates are enumerated in the code and the registry's
# number has to be one of them.
ANT_PAGES_PERIOD_SETS = [
    ("BPWR_DEVICE_TYPE", "BPWR_PERIOD", "BPWR_PERIODS"),
    ("HRM_DEVICE_TYPE", "HRM_PERIOD", "HRM_PERIODS"),
]


class Problems:
    """Collect every problem rather than stopping at the first."""

    def __init__(self):
        self.items = []

    def add(self, path, line, message):
        where = str(path.relative_to(REPO)) if isinstance(path, Path) else str(path)
        if line:
            self.items.append("%s:%d: %s" % (where, line, message))
        else:
            self.items.append("%s: %s" % (where, message))

    def __len__(self):
        return len(self.items)


class Row:
    __slots__ = ("cells", "line")

    def __init__(self, cells, line):
        self.cells = cells
        self.line = line

    def get(self, column):
        return self.cells.get(column, "")


def normalise(cell: str) -> str:
    """Strip markdown decoration a human adds and a comparison must not see."""
    cell = cell.replace("`", "").replace("**", "").strip()
    if cell.lower() in UNRECORDED_SPELLINGS:
        return UNRECORDED
    return cell


def split_row(line: str):
    """Cells of one markdown table row, or None if this is not a table row."""
    stripped = line.strip()
    if not stripped.startswith("|"):
        return None
    if stripped.endswith("|"):
        stripped = stripped[:-1]
    return [normalise(cell) for cell in stripped[1:].split("|")]


def read_tables(path: Path, problems: Problems) -> dict:
    """Every table preceded by a <!-- radiant-registry: NAME --> marker."""
    if not path.exists():
        problems.add(path, 0, "file does not exist")
        return {}

    lines = path.read_text(encoding="utf-8").splitlines()
    tables = {}
    index = 0
    while index < len(lines):
        match = MARKER.match(lines[index].strip())
        if not match:
            index += 1
            continue
        name = match.group(1)
        marker_line = index + 1

        index += 1
        while index < len(lines) and not lines[index].strip():
            index += 1
        if index >= len(lines) or not lines[index].strip().startswith("|"):
            problems.add(path, marker_line,
                         "marker '%s' is not followed by a table" % name)
            continue

        header = [cell.lower() for cell in split_row(lines[index])]
        index += 1
        if index >= len(lines) or not SEPARATOR.match(lines[index].strip()):
            problems.add(path, index + 1,
                         "table '%s' has no header separator row" % name)
            continue
        index += 1

        rows = []
        while index < len(lines) and lines[index].strip().startswith("|"):
            cells = split_row(lines[index])
            if len(cells) != len(header):
                problems.add(path, index + 1,
                             "table '%s' row has %d cells, header has %d"
                             % (name, len(cells), len(header)))
            else:
                rows.append(Row(dict(zip(header, cells)), index + 1))
            index += 1

        if name in tables:
            problems.add(path, marker_line, "marker '%s' appears twice" % name)
        tables[name] = (header, rows)

    return tables


def require_columns(path, name, header, required, problems) -> bool:
    missing = [column for column in required if column not in header]
    if missing:
        problems.add(path, 0, "table '%s' is missing required column(s): %s"
                     % (name, ", ".join(missing)))
        return False
    return True


def require_non_empty(path, row, columns, problems) -> None:
    for column in columns:
        if not row.get(column):
            problems.add(path, row.line, "column '%s' is empty" % column)


def parse_device_type(text: str):
    match = HEX8.match(text)
    if not match:
        return None
    return int(match.group(1), 16)


def parse_page_cell(text: str):
    """A page cell is 0xNN, 0xNN-0xMM, or the literal 'none'.

    'none' is not a placeholder. Device type 0x79 genuinely has no page-number
    byte, and a registry that could not say so would have to lie about it.
    """
    if text.lower() == "none":
        return "none", []
    match = HEX8.match(text)
    if match:
        value = int(match.group(1), 16)
        return "single", [value]
    match = HEX8_RANGE.match(text)
    if match:
        low = int(match.group(1), 16)
        high = int(match.group(2), 16)
        if high <= low:
            return "bad-range", []
        return "range", list(range(low, high + 1))
    return None, []


def check_device_types(path, header, rows, problems) -> dict:
    if not require_columns(path, "device-types", header, DEVICE_TYPE_COLUMNS,
                           problems):
        return {}

    seen = {}
    for row in rows:
        require_non_empty(path, row, DEVICE_TYPE_COLUMNS, problems)

        device_type = parse_device_type(row.get("type"))
        if device_type is None:
            problems.add(path, row.line, "device type %r is not 0xNN"
                         % row.get("type"))
            continue
        if not 1 <= device_type <= 127:
            problems.add(path, row.line,
                         "device type 0x%02X is outside 1..127; the MSB of the "
                         "device type field is the pairing bit" % device_type)
            continue
        if device_type in seen:
            problems.add(path, row.line,
                         "duplicate device type 0x%02X, first claimed at line %d"
                         % (device_type, seen[device_type].line))
            continue
        seen[device_type] = row

        status = row.get("status").lower()
        if status not in STATUS_VALUES:
            problems.add(path, row.line, "status %r is not one of %s"
                         % (row.get("status"), sorted(STATUS_VALUES)))

        date_text = row.get("date")
        if not DATE.match(date_text):
            problems.add(path, row.line, "date %r is not YYYY-MM-DD" % date_text)
        else:
            try:
                datetime.date.fromisoformat(date_text)
            except ValueError:
                problems.add(path, row.line, "date %r is not a real date"
                             % date_text)

        period = row.get("period")
        if period not in (UNRECORDED, "per-node"):
            if not period.isdigit() or not 1 <= int(period) <= 65535:
                problems.add(path, row.line,
                             "period %r is not 1..65535 counts of 1/32768 s, "
                             "'per-node', or unrecorded" % period)

        # Two columns, two vocabularies, two checks. They shared one loop while
        # both were plain opt-ins; ADR 0007 made 'lr phy' carry a coding rate,
        # and a shared value set would have quietly accepted 's8' for
        # 'adaptive freq' as well.
        if row.get("lr phy").lower() not in LR_PHY_VALUES:
            problems.add(path, row.line,
                         "column 'lr phy' is %r, not one of %s - it carries a "
                         "coding rate now, not yes/no (ADR 0007)"
                         % (row.get("lr phy"), sorted(LR_PHY_VALUES)))

        if row.get("adaptive freq").lower() not in OPT_IN_VALUES:
            problems.add(path, row.line, "column '%s' is %r, not one of %s"
                         % ("adaptive freq", row.get("adaptive freq"),
                            sorted(OPT_IN_VALUES)))

    return seen


def check_pages(path, header, rows, types, problems) -> dict:
    if not require_columns(path, "pages", header, PAGE_COLUMNS, problems):
        return {}

    claimed = {}
    per_type_rows = {}
    for row in rows:
        require_non_empty(path, row, PAGE_COLUMNS, problems)

        device_type = parse_device_type(row.get("type"))
        if device_type is None:
            problems.add(path, row.line, "device type %r is not 0xNN"
                         % row.get("type"))
            continue
        if device_type not in types:
            problems.add(path, row.line,
                         "page claimed for device type 0x%02X, which has no row "
                         "in the device-types table" % device_type)
            continue

        kind, values = parse_page_cell(row.get("page"))
        if kind is None:
            problems.add(path, row.line,
                         "page %r is not 0xNN, 0xNN-0xMM or 'none'"
                         % row.get("page"))
            continue
        if kind == "bad-range":
            problems.add(path, row.line,
                         "page range %r does not ascend" % row.get("page"))
            continue

        bucket = claimed.setdefault(device_type, {})
        per_type_rows.setdefault(device_type, []).append(row)

        if kind == "none":
            if "none" in bucket:
                problems.add(path, row.line,
                             "device type 0x%02X already has a 'none' page row "
                             "at line %d" % (device_type, bucket["none"]))
            else:
                bucket["none"] = row.line
            continue

        for value in values:
            if value in bucket:
                problems.add(path, row.line,
                             "device type 0x%02X page 0x%02X is already claimed "
                             "at line %d" % (device_type, value, bucket[value]))
                break
            bucket[value] = row.line

    # A RadiANT type accounts for every page number. An unassigned page should
    # be a recorded decision, because the unclaimed one is the one two
    # implementations both reach for.
    for device_type, row in types.items():
        if row.get("status").lower() != "radiant":
            continue
        bucket = claimed.get(device_type, {})
        numbers = set(key for key in bucket if key != "none")
        gaps = sorted(set(range(256)) - numbers)
        if gaps:
            problems.add(path, row.line,
                         "device type 0x%02X has status 'radiant' but does not "
                         "account for %d page number(s), first 0x%02X"
                         % (device_type, len(gaps), gaps[0]))
        reserved = [candidate for candidate in per_type_rows.get(device_type, [])
                    if candidate.get("page").lower() == SECURITY_RESERVED_RANGE]
        if not reserved:
            problems.add(path, row.line,
                         "device type 0x%02X has status 'radiant' but does not "
                         "reserve %s for the security envelope; see "
                         "docs/radiant-security.md"
                         % (device_type, SECURITY_RESERVED_RANGE.upper()))

    return per_type_rows


# Every device type with status `radiant` owes a page map, and this says where
# each one's lives. It used to be an assertion that there was exactly ONE such
# type, with a message telling the next person to extend this function; the
# profile device types of docs/device-profiles.md are that moment arriving.
#
# The page map is not in one file because the two documents own different
# things: 0x60 IS the envelope and its map belongs with it, while a profile type
# is the envelope plus a pinned schema and belongs with the other profiles. A
# claim with no entry here fails loudly rather than going unchecked, which is
# the half that matters - an unchecked page map is how a registry and a
# specification drift apart in the first place.
RADIANT_PAGE_MAPS = {
    0x60: (TELEMETRY, "pagemap"),
    0x61: (PROFILES, "pagemap-0x61"),
    0x62: (PROFILES, "pagemap-0x62"),
}


def check_pagemap(types, per_type_rows, problems) -> None:
    radiant = sorted(device_type for device_type, row in types.items()
                     if row.get("status").lower() == "radiant")
    for device_type in radiant:
        if device_type not in RADIANT_PAGE_MAPS:
            problems.add(REGISTRY, types[device_type].line,
                         "device type 0x%02X has status 'radiant' but no page "
                         "map is registered for it; add it to "
                         "RADIANT_PAGE_MAPS in "
                         "scripts/check_profile_registry.py naming the document "
                         "and marker that hold its page map" % device_type)
            continue
        path, marker = RADIANT_PAGE_MAPS[device_type]
        check_one_pagemap(device_type, path, marker, per_type_rows, problems)

    for device_type in sorted(RADIANT_PAGE_MAPS):
        if device_type not in radiant:
            path, marker = RADIANT_PAGE_MAPS[device_type]
            problems.add(REGISTRY, 0,
                         "RADIANT_PAGE_MAPS expects a page map for device type "
                         "0x%02X at marker '%s' in %s, but no device-type row "
                         "claims 0x%02X with status 'radiant'"
                         % (device_type, marker,
                            path.relative_to(REPO), device_type))


def check_one_pagemap(device_type, path, marker, per_type_rows, problems) -> None:
    tables = read_tables(path, problems)
    if marker not in tables:
        problems.add(path, 0, "no <!-- radiant-registry: %s --> table, which is "
                     "where device type 0x%02X's page map belongs"
                     % (marker, device_type))
        return
    header, rows = tables[marker]
    if not require_columns(path, marker, header, PAGEMAP_COLUMNS, problems):
        return

    registry_map = {}
    for row in per_type_rows.get(device_type, []):
        registry_map[row.get("page").lower()] = (row.get("page"),
                                                 row.get("name"))

    doc_map = {}
    for row in rows:
        require_non_empty(path, row, PAGEMAP_COLUMNS, problems)
        key = row.get("page").lower()
        if key in doc_map:
            problems.add(path, row.line, "page %r appears twice"
                         % row.get("page"))
        doc_map[key] = (row.get("page"), row.get("name"), row.line)

    for key, (shown, name) in sorted(registry_map.items()):
        if key not in doc_map:
            problems.add(path, 0,
                         "page %s is claimed for device type 0x%02X in "
                         "docs/profile-registry.md but is missing from the "
                         "'%s' page map" % (shown, device_type, marker))
        elif doc_map[key][1] != name:
            problems.add(path, doc_map[key][2],
                         "page %s of device type 0x%02X is named %r here and "
                         "%r in docs/profile-registry.md"
                         % (shown, device_type, doc_map[key][1], name))

    for key, (shown, name, line) in sorted(doc_map.items()):
        if key not in registry_map:
            problems.add(path, line,
                         "page %s is in the '%s' page map but is not claimed "
                         "for device type 0x%02X in docs/profile-registry.md"
                         % (shown, marker, device_type))


def check_ant_plus_doc(types, problems) -> None:
    tables = read_tables(ANT_PLUS, problems)
    if "ant-plus-types" not in tables:
        problems.add(ANT_PLUS, 0,
                     "no <!-- radiant-registry: ant-plus-types --> table")
        return
    header, rows = tables["ant-plus-types"]
    if not require_columns(ANT_PLUS, "ant-plus-types", header,
                           ANT_PLUS_COLUMNS, problems):
        return

    for row in rows:
        require_non_empty(ANT_PLUS, row, ANT_PLUS_COLUMNS, problems)
        device_type = parse_device_type(row.get("type"))
        if device_type is None:
            problems.add(ANT_PLUS, row.line, "device type %r is not 0xNN"
                         % row.get("type"))
            continue
        registry_row = types.get(device_type)
        if registry_row is None:
            problems.add(ANT_PLUS, row.line,
                         "device type 0x%02X has no row in "
                         "docs/profile-registry.md" % device_type)
            continue
        if registry_row.get("status").lower() != "ant-plus-reserved":
            problems.add(ANT_PLUS, row.line,
                         "device type 0x%02X is listed here as an ANT+ profile "
                         "but its registry status is %r"
                         % (device_type, registry_row.get("status")))
        if row.get("name") != registry_row.get("name"):
            problems.add(ANT_PLUS, row.line,
                         "device type 0x%02X is named %r here and %r in "
                         "docs/profile-registry.md"
                         % (device_type, row.get("name"),
                            registry_row.get("name")))
        if row.get("period") != registry_row.get("period"):
            problems.add(ANT_PLUS, row.line,
                         "device type 0x%02X has period %r here and %r in "
                         "docs/profile-registry.md"
                         % (device_type, row.get("period"),
                            registry_row.get("period")))


def load_module(path, problems):
    """Import a tools/*.py by path, reporting rather than raising."""
    if not path.exists():
        problems.add(path, 0, "file does not exist")
        return None
    spec = importlib.util.spec_from_file_location(path.stem, path)
    module = importlib.util.module_from_spec(spec)
    # Registered BEFORE exec_module, which is the documented recipe and not
    # tidiness. A module executed outside sys.modules cannot use anything that
    # looks itself up by __module__ during class creation - @dataclass is the
    # one that bites, since it resolves its own module's globals and gets None.
    # The symptom is "'NoneType' object has no attribute '__dict__'" reported
    # against ant_pages.py, which points at the wrong file entirely.
    sys.modules[spec.name] = module
    try:
        spec.loader.exec_module(module)
    except Exception as error:               # noqa: BLE001 - report, don't crash
        problems.add(path, 0, "cannot be imported: %s" % error)
        return None
    return module


def load_ant_pages(problems):
    return load_module(ANT_PAGES, problems)


def check_against_ant_pages(types, module, problems) -> None:
    if module is None:
        return

    for type_constant, period_constant in ANT_PAGES_TYPES:
        if not hasattr(module, type_constant):
            problems.add(ANT_PAGES, 0, "%s is gone; update ANT_PAGES_TYPES in "
                         "scripts/check_profile_registry.py" % type_constant)
            continue
        device_type = getattr(module, type_constant)
        period = getattr(module, period_constant)
        row = types.get(device_type)
        if row is None:
            problems.add(REGISTRY, 0,
                         "tools/ant_pages.py defines %s = 0x%02X but the "
                         "registry has no row for it"
                         % (type_constant, device_type))
            continue
        if row.get("period") != str(period):
            problems.add(REGISTRY, row.line,
                         "device type 0x%02X has period %r but "
                         "tools/ant_pages.py %s is %d"
                         % (device_type, row.get("period"), period_constant,
                            period))

    # A profile's permitted period set, and the registry's number inside it.
    # Every value in the set has to be a legal channel period on its own, and
    # the default has to be one of them - a registry recording a rate the
    # profile does not permit produces a channel that never opens, with no
    # error anywhere that names the period.
    for type_constant, period_constant, set_constant in ANT_PAGES_PERIOD_SETS:
        if not all(hasattr(module, name) for name in
                   (type_constant, period_constant, set_constant)):
            problems.add(ANT_PAGES, 0,
                         "%s is gone; update ANT_PAGES_PERIOD_SETS in "
                         "scripts/check_profile_registry.py" % set_constant)
            continue
        periods = tuple(getattr(module, set_constant))
        default = getattr(module, period_constant)
        if not periods:
            problems.add(ANT_PAGES, 0,
                         "%s is empty; a profile with no enumerated period is "
                         "a profile whose channel may never open" % set_constant)
            continue
        if default not in periods:
            problems.add(ANT_PAGES, 0,
                         "%s is %d, which is not in %s = %r"
                         % (period_constant, default, set_constant, periods))
        for value in periods:
            if not 1 <= value <= 65535:
                problems.add(ANT_PAGES, 0,
                             "%s contains %r, which is not 1..65535 counts of "
                             "1/32768 s" % (set_constant, value))

    # The common-page cadence trap: 119 / 120 / a 121-message cycle, not the 65
    # the generic guidance claims. If the constant moves, the prose that
    # explains it has to move with it.
    interval = getattr(module, "COMMON_PAGE_INTERVAL", None)
    if interval != 120:
        problems.add(ANT_PAGES, 0,
                     "COMMON_PAGE_INTERVAL is %r, not 120; docs/ant-plus-"
                     "profiles.md and docs/radiant-telemetry.md both state a "
                     "121-message cycle and now disagree with the code"
                     % interval)
    else:
        for path in (TELEMETRY, ANT_PLUS):
            if path.exists() and "121 messages" not in path.read_text(
                    encoding="utf-8"):
                problems.add(path, 0,
                             "does not state the 121-message common-page "
                             "cadence that ant_pages.COMMON_PAGE_INTERVAL "
                             "implies")


# ── The pins ────────────────────────────────────────────────────────────────
#
# Two kinds of thing are checked here, and they fail for the same reason.
#
# The RESERVATIONS are fields that cannot be added later without a format break
# for every deployed node: the descriptor's epoch, the data page's counter, and
# the trailing tag space. Those were always the point of this check.
#
# The PINS are the parts of docs/radiant-security.md that two implementations
# would otherwise differ on invisibly - the nonce's domain byte, the legal
# window set, the tag length, the epoch-advance rules - plus the two privacy
# rules that cost nothing and are therefore the first things to fall out of a
# document during an edit. A pin that is silently dropped is not a compile
# error and not a test failure; it is a scheme that still works and no longer
# interoperates, which is exactly the class of mistake this script exists for.
#
# Needles are matched against a whitespace- and markdown-normalised copy of the
# document, so a needle survives re-wrapping, bolding and block-quoting. Write
# them as the prose reads.

PIN_SEPARATORS = re.compile(r"[\s>*`_|]+")


def normalise_prose(text: str) -> str:
    """Collapse whitespace and markdown decoration so a needle survives a re-wrap."""
    return PIN_SEPARATORS.sub(" ", text).lower()


SPEC_PINS = [
    (TELEMETRY, "epoch (u32 LE)",
     "the descriptor's 4-byte epoch, which docs/radiant-security.md needs as "
     "its key-rotation clock and for command replay rejection"),
    (TELEMETRY, "event counter (u8)",
     "the data page's mandatory counter, which is the X_CONF nonce source and "
     "the loss detector"),
    (TELEMETRY, "trailing byte(s) are tag space",
     "the trailing-tag-space invariant, without which X_AUTH's tag byte "
     "collides with a field some descriptor already placed there"),
    (TELEMETRY, "bit 3 INFO reserved, must be 0",
     "descriptor flag bit 3 as reserved-must-be-zero. It was X_PRIV's "
     "rotation announcement; X_PRIV is withdrawn and the bit stays reserved, "
     "because announcing a privacy posture in the clear is itself a leak"),
    (TELEMETRY, "W in {2, 4, 8}",
     "the v1 window set. W must divide 256 and 65536 or the spread MAC stops "
     "resynchronising after a lost packet"),
    (TELEMETRY, "descriptor authentication frame",
     "the descriptor authentication frame, without which forging one "
     "descriptor yields wrong readings from correctly authenticated packets"),
    (TELEMETRY, "a counter wrap advances the epoch by 1",
     "the counter-wrap rule, without which a 16-bit counter wraps inside one "
     "epoch and reuses keystream after about 4.5 hours at 4 Hz"),
    (TELEMETRY, "serial = 0xFFFFFFFF",
     "the page 81 rule. A 32-bit serial in the clear is strictly more "
     "identifying than the device number and defeats every identity tier"),
    (TELEMETRY, "page 82 is suppressed",
     "the page 82 rule. A monotone operating-time counter survives an "
     "identity change and fingerprints a battery swap"),
    (TELEMETRY, "The handoff page carries no epoch",
     "the sync-handoff page's no-epoch rule. It is the obvious field to add - "
     "a receiver handing over everything else it knows would naturally "
     "include it - and for a hostless node the epoch IS the boot counter, so a "
     "page broadcast in the clear that carried it would fingerprint the device "
     "across sessions and defeat per-boot device-number rotation on its own"),
    (TELEMETRY, "measured from the t_sync of THIS frame",
     "the sync-handoff page's relative-phase rule. There is no shared time "
     "base between two receivers, so an absolute slot instant is not merely "
     "imprecise, it is uninterpretable on the receiver that consumes it"),

    (SECURITY, "dom = 0x01 CTR keystream",
     "the nonce's domain byte, which is the only thing stopping a keystream "
     "block and a MAC block ever coinciding"),
    (SECURITY, "trunc_{8*W bits}",
     "the tag length. It is 8*W bits - 16 at the default W=2 - and not the "
     "fixed 32 the first draft's prose implied"),
    (SECURITY, "W in {2, 4, 8}",
     "the v1 window set, and the reason for it"),
    (SECURITY, "authenticates window k-1",
     "the one-window tag lag. Encrypt-then-MAC means a sender cannot know a "
     "window's tag until its last packet is built, so the tag necessarily "
     "lags - and an implementation that assumed otherwise would be "
     "unimplementable rather than merely wrong"),
    (SECURITY, "rejects any epoch <= the current one",
     "the epoch monotonicity rule. Without it a reboot restarts the counter "
     "under an unchanged epoch, which is a two-time pad for X_CONF and a full "
     "session replay against X_AUTH"),
    (SECURITY, "a counter wrap advances the epoch by 1",
     "the counter-wrap rule"),
    (SECURITY, "in the secured range is dropped and counted",
     "the non-broadcast drop guard. Without it an injector sends a "
     "secured-range page as acknowledged data and skips the MAC entirely"),
    (SECURITY, "descriptor authentication frame",
     "the descriptor authentication frame"),
    (SECURITY, "serial = 0xFFFFFFFF",
     "the page 81 rule"),
    (SECURITY, "page 82 is suppressed",
     "the page 82 rule"),
    (SECURITY, "must re-pair every session",
     "identity Tier 2's cost to standard receivers. It is the reason the tier "
     "is opt-in and off by default, and a spec that dropped the sentence "
     "would read as though re-rolling every boot were free"),
]


def check_pins(pins, problems) -> None:
    """Assert every needle still appears in the document that has to state it."""
    cache = {}
    for path, needle, why in pins:
        if path not in cache:
            if not path.exists():
                problems.add(path, 0, "file does not exist")
                cache[path] = None
            else:
                cache[path] = normalise_prose(path.read_text(encoding="utf-8"))
        text = cache[path]
        if text is None:
            continue
        if normalise_prose(needle).strip() not in text:
            problems.add(path, 0,
                         "no longer states %s (looked for %r)" % (why, needle))


def check_reserved_space(problems) -> None:
    """The reservations and pins that cannot be dropped without a silent break."""
    check_pins(SPEC_PINS, problems)


# ── The compat layer ────────────────────────────────────────────────────────
#
# docs/decisions/0008 puts RadiANT pages on ANT+ device types and
# docs/decisions/0009 gives a hostless node an epoch. Three kinds of thing are
# checked, and the order matters because only the first two bite today.
#
# COMPAT_PINS is check_reserved_space()'s mechanism, unchanged: prose that two
# implementations would otherwise differ on invisibly, in the document that has
# to keep stating it.
#
# check_compat_allocation() is a REGISTRY rule and is exercised now: whatever
# C3 eventually registers, the allocation is two page numbers rather than
# three, it is the same numbers in every compat profile, no compat page is
# 7-bit-illegal, and none of it ever lands on 0x79.
#
# check_compat_constants() was the forward one and is now half live. C2 landed
# the domain byte, the subtypes, the two tag lengths and the Python mirror, so
# those are REQUIRED here rather than skipped-while-absent - a check that can
# pass by not being reached is a check that passes on the day the file is
# deleted. C3 still owns tools/ant_pages.py's copies, which land with the page
# codecs, so that module is checked only for what it already has. The names are
# not guesses: ADR 0008 pins them as a naming convention precisely so that this
# check has something to look up.

COMPAT_DOM_BYTE = 0x04
COMPAT_SUBTYPES = {"COMPAT_SUBTYPE_TIER_I": 0x01,
                   "COMPAT_SUBTYPE_TIER_II": 0x02,
                   "COMPAT_SUBTYPE_ANNOUNCE": 0x03,
                   # compat-C8's, and the one that is not under K_auth: an
                   # inbound command is verified under K_cmd. Pinned here for
                   # the reason 0x03 was pinned before the layer that emits it
                   # existed - a nibble spent twice is two claims one tag
                   # cannot tell apart.
                   "COMPAT_SUBTYPE_COMMAND": 0x04}
COMPAT_WINDOW_SIZES = (4, 8, 16, 32)
COMPAT_COUNTDOWN_LENGTHS = (16, 32, 64, 128)
COMPAT_TIER_I_TAG_BITS = 40
COMPAT_TIER_II_TAG_BITS = 48
# An inbound command carries three bytes of its own before its tag, so 8 - 3 = 5
# is what is left. The announcement's tag is Tier II's width for the same kind
# of reason: it has a frame to itself and bytes [2..7] are what is left.
COMPAT_CMD_TAG_BITS = 40

# Two page numbers, not three: SWITCH and RETURN ride the beacon page's frame
# set. A row claiming a third is the drift this exists to catch.
COMPAT_PAGE_TOKEN = "radiant compat"
COMPAT_PAGE_ROLES = ("beacon", "attestation")
COMPAT_MAX_PAGE = 0x7F
NO_ADDITIONAL_PAGE_TYPE = 0x79


def legal_set_needle(name: str, values) -> str:
    """'N in {4, 8, 16, 32}', spelled the way docs/radiant-security.md spells W.

    Generated from the tuple above rather than typed twice, so the script's
    idea of the legal set and the document's cannot drift apart.
    """
    return "%s in {%s}" % (name, ", ".join(str(value) for value in values))


COMPAT_PINS = [
    (TELEMETRY, "Additive pages on ANT+ device types are permitted",
     "section 2 clause 2's amendment. Without it the clause reads as the "
     "original blanket 'nothing in this document applies to them', which "
     "forbids the whole compat layer"),
    (TELEMETRY, "No existing ANT+ page layout may be modified",
     "the half of clause 2 that was always right. The amendment permits ADDING "
     "a page and must never be read as permitting a change to one"),
    (TELEMETRY, "Device type 0x79 can never carry an additional page",
     "0x79's permanent exclusion. It has no page-number byte, so an inserted "
     "page decodes as speed and cadence and steps four accumulators - a "
     "structural exclusion, not a scheduling decision"),

    (SECURITY, "0x04 compat MAC",
     "the compat domain byte. Without it a compat tag and a spread tag can "
     "coincide, which is the same omission section 3.3 exists to prevent"),
    (SECURITY, "sub = 0x01 Tier I",
     "the attestation subtypes. Both tiers share dom 0x04, so the subtype is "
     "the only thing separating a Tier I tag from a Tier II tag"),
    (SECURITY, "The subtype is inside the nonce and not only in the page",
     "the rule that makes the subtype load-bearing. A subtype written only "
     "into the page byte is chosen by whoever sends the page"),
    (SECURITY, legal_set_needle("N", COMPAT_WINDOW_SIZES),
     "the legal Tier II window set. N is simultaneously the airtime cost, the "
     "verification latency and the DoS amplification factor, so an "
     "unenumerated N is three surprises"),
    (SECURITY, legal_set_needle("K", COMPAT_COUNTDOWN_LENGTHS),
     "the legal switch-countdown set. Every keyed receiver retunes on the same "
     "message, which needs both ends to agree on how long the countdown can be"),
    (SECURITY, "trunc40( CMAC(K_auth, nonce_block) )",
     "Tier I's 40-bit tag and the fact that it covers nothing but its own "
     "nonce - the property that makes it verifiable on receipt"),
    (SECURITY, "trunc48( CMAC(K_auth, nonce_block",
     "Tier II's 48-bit window tag"),
    (SECURITY, "two page numbers, not three",
     "the allocation's arity. SWITCH and RETURN are frame indices in the "
     "beacon page's set; a third page number is refused on the 7-bit "
     "namespace alone"),
    (SECURITY, 'trunc16( CMAC(K_id, "priv" || epoch) )',
     "the derived private-mode locator, which is what makes the SWITCH "
     "announcement an optimisation rather than the only way to find the node"),
    (SECURITY, "0x0000 excluded",
     "the locator's wildcard exclusion. 0x0000 is the ANT wildcard device "
     "number and a node that derived it would be unaddressable"),
    (SECURITY, "private_policy defaults to never",
     "the default policy. A shipped strap is a plain ANT+ sensor unless "
     "somebody configures it otherwise, out of band"),
    (SECURITY, "NVM if provisioned, else Kconfig; the host message writes NVM",
     "the policy precedence rule. Three sources with an unstated precedence is "
     "a bug report waiting six months"),
    (SECURITY, "physical + silent is the one exemption",
     "the key dependency's single exemption. Every other combination needs "
     "K_auth and Tier I, because a command trigger and a broadcast "
     "announcement both need something to authenticate with"),
    (SECURITY, "The beacon carries no epoch",
     "the no-epoch rule. For a hostless node the epoch IS the boot counter, so "
     "broadcasting it every 30 s fingerprints the device across sessions and "
     "defeats per-boot device-number rotation on its own"),
    (SECURITY, "forward search against the key-group hint",
     "epoch recovery path 1, which is what the beacon's epoch field was "
     "removed in favour of"),
    (SECURITY, "trial-verify candidate epochs against the attestation tag",
     "epoch recovery path 2, for advertise = off. It is the path that "
     "otherwise gets written and never exercised"),
    (SECURITY, "verification rate equals page delivery rate",
     "Tier I's loss independence, which is the entire reason the tier exists "
     "and the one claim a bench run can falsify in a single capture"),
    (SECURITY, "2.0% of its slots",
     "the default configuration's total airtime cost - 0.8% beacon plus 1.2% "
     "Tier I - which is the number the compatibility claim rests on"),
    (SECURITY, "1.65%",
     "what ANT+ itself spends on common pages 80 and 81. The comparison is the "
     "argument; the bare 2.0% is just a number"),
    (SECURITY, "beacon slots are promoted to 1 in 8",
     "the countdown's beacon promotion. At the steady 1-in-121 cadence a "
     "countdown would carry one or two copies"),
    (SECURITY, "reports UNVERIFIED rather than CLEAR",
     "receiver-side downgrade protection. Strip the beacon and a naive "
     "receiver falls back to clear, so CLEAR must keep meaning 'no key here'"),
    (SECURITY, "A receiver acts on a SWITCH frame only after its tag verifies",
     "the announcement's authentication rule. Without it the SWITCH frame is a "
     "one-packet herding attack strictly worse than the mute attack this "
     "design already refuses"),
    (SECURITY, "attestation tiers are roman",
     "the numeral convention. ADR 0006's identity tiers are arabic and the two "
     "axes are unrelated; one numeral system for both is a collision that has "
     "already nearly happened"),
    (SECURITY, "Tier 2 unlinkability does not survive a switch that an observer watches",
     "the switch-time linkage. The announcement does not create it - timing "
     "would - but it makes it cheap, and it is a cost of ADR 0006's Tier 2 "
     "rather than of this layer alone"),
    (SECURITY, "silent buys unlinkability and pays in availability",
     "the announce trade, stated as a trade. Neither value is 'more secure'"),
    (SECURITY, "can only remove one by re-provisioning the network",
     "the add-is-cheap / remove-is-not asymmetry. Enrolment makes the "
     "revocation gap more visible rather than less"),

    (SECURITY, "0009-hostless-node-identity",
     "the pointer from section 7.4's host-supplied-scalar limit to the "
     "decision that closes it. Without it the limit reads as still open"),
    (SECURITY, 'KDF(K_dev, "pair" || pair_counter)',
     "the hostless node's deterministic pairing scalar, which is the "
     "replacement for a host CSPRNG and is deliberately not a weak on-node PRNG"),
    (SECURITY, "the counter advanced before the public key is transmitted",
     "the counter-advance rule. Advance-after reuses a scalar on every "
     "abandoned pairing, which destroys forward secrecy and turns the pairing "
     "pubkey into a stable cross-session identifier"),
]


def check_compat_pins(problems) -> None:
    """ADR 0008 and ADR 0009's pins, in the documents that have to state them."""
    check_pins(COMPAT_PINS, problems)


def compat_role(row) -> str:
    """Which compat page a registry row claims, or '' if it is not one."""
    name = normalise_prose(row.get("name"))
    if COMPAT_PAGE_TOKEN not in name:
        return ""
    found = [role for role in COMPAT_PAGE_ROLES if role in name]
    return found[0] if len(found) == 1 else "?"


def check_compat_allocation(per_type_rows, problems) -> None:
    """The allocation rules, whatever numbers C3 eventually confirms.

    Zero compat rows is the state today and passes: the numbers are confirmed
    against the profile documents in C3. What cannot happen at any point is a
    third page number, a number a heart-rate receiver cannot express, two
    profiles disagreeing about which numbers they are, or anything at all on
    0x79.
    """
    per_type_pages = {}

    for device_type, rows in sorted(per_type_rows.items()):
        claimed = {}
        for row in rows:
            role = compat_role(row)
            if not role:
                continue
            if role == "?":
                problems.add(REGISTRY, row.line,
                             "compat page row %r must name exactly one of %s; "
                             "see docs/decisions/0008"
                             % (row.get("name"), list(COMPAT_PAGE_ROLES)))
                continue
            if device_type == NO_ADDITIONAL_PAGE_TYPE:
                problems.add(REGISTRY, row.line,
                             "device type 0x%02X can never carry an additional "
                             "page - it has no page-number byte - so a compat "
                             "%s page cannot be claimed for it; see "
                             "docs/decisions/0008"
                             % (device_type, role))
                continue
            if role in claimed:
                problems.add(REGISTRY, row.line,
                             "device type 0x%02X claims a second compat %s page; "
                             "the allocation is two page numbers, not three"
                             % (device_type, role))
                continue
            # Recorded before it is validated, so that one malformed row is one
            # problem rather than also a spurious "the allocation is both or
            # neither" against the row above it.
            claimed[role] = []

            kind, values = parse_page_cell(row.get("page"))
            if kind in (None, "bad-range", "none") or not values:
                problems.add(REGISTRY, row.line,
                             "compat %s page for device type 0x%02X is %r, "
                             "which is not a page number or a page range"
                             % (role, device_type, row.get("page")))
                continue
            if role == "beacon" and len(values) != 1:
                problems.add(REGISTRY, row.line,
                             "the compat beacon is one page number, not %d; "
                             "SWITCH and RETURN are frame indices inside its "
                             "frame set" % len(values))
                continue
            if role == "attestation" and len(values) > 2:
                problems.add(REGISTRY, row.line,
                             "the compat attestation claim covers %d page "
                             "numbers; both tiers ride one contiguous "
                             "nibble-aligned claim, so it is one or two"
                             % len(values))
                continue
            for value in values:
                if value > COMPAT_MAX_PAGE:
                    problems.add(REGISTRY, row.line,
                                 "compat page 0x%02X is above 0x%02X; heart "
                                 "rate's byte 0 carries a page-change toggle in "
                                 "bit 7, so compat page numbers are 7-bit and "
                                 "must be the same in every compat profile"
                                 % (value, COMPAT_MAX_PAGE))
            claimed[role] = sorted(values)

        if not claimed:
            continue
        missing = [role for role in COMPAT_PAGE_ROLES if role not in claimed]
        if missing:
            problems.add(REGISTRY, rows[0].line,
                         "device type 0x%02X claims a compat page but not a "
                         "compat %s page; the allocation is both or neither"
                         % (device_type, ", ".join(missing)))
        per_type_pages[device_type] = claimed

    # One rule for a receiver means the same numbers everywhere.
    distinct = {}
    for device_type, claimed in sorted(per_type_pages.items()):
        key = tuple(sorted((role, tuple(pages))
                           for role, pages in claimed.items()))
        distinct.setdefault(key, []).append(device_type)
    if len(distinct) > 1:
        spread = "; ".join("0x%02X" % device_type
                           for group in distinct.values() for device_type in group)
        problems.add(REGISTRY, 0,
                     "compat pages are allocated differently across device "
                     "types (%s); they must be the same numbers in every compat "
                     "profile so a receiver has one rule" % spread)

    # 0x79 has one row, and that row says 'none'. Checked here rather than
    # inferred from the absence of a compat row, because the reason it has no
    # page numbers is the same reason it can never gain one.
    for row in per_type_rows.get(NO_ADDITIONAL_PAGE_TYPE, []):
        if row.get("page").lower() != "none":
            problems.add(REGISTRY, row.line,
                         "device type 0x%02X claims page %r, but it has no "
                         "page-number byte at all; see docs/decisions/0008 and "
                         "docs/device-profiles.md"
                         % (NO_ADDITIONAL_PAGE_TYPE, row.get("page")))


COMPAT_PERIOD_COLUMNS = ["type", "rate", "period",
                         "constant in tools/ant_pages.py"]


def check_compat_periods(tables, module, problems) -> None:
    """The permitted-period table, against the tuples in tools/ant_pages.py.

    A page number a receiver does not know is skipped. A period it does not
    share means the channel never opens, and nothing on either side reports the
    period as the reason - so of everything docs/decisions/0008 makes a
    manufacturer setting, this is the one whose failure is hardest to diagnose
    and the one worth pinning from both ends.
    """
    if module is None:
        return
    if "compat-periods" not in tables:
        problems.add(REGISTRY, 0,
                     "no <!-- radiant-registry: compat-periods --> table; "
                     "docs/decisions/0008 makes `period` a manufacturer "
                     "setting, so each compat profile's permitted set is "
                     "registered rather than left to the code alone")
        return

    header, rows = tables["compat-periods"]
    if not require_columns(REGISTRY, "compat-periods", header,
                           COMPAT_PERIOD_COLUMNS, problems):
        return

    registered = {}
    for row in rows:
        require_non_empty(REGISTRY, row, COMPAT_PERIOD_COLUMNS, problems)
        device_type = parse_device_type(row.get("type"))
        if device_type is None:
            problems.add(REGISTRY, row.line,
                         "device type %r is not 0xNN" % row.get("type"))
            continue
        period = row.get("period")
        if not period.isdigit() or not 1 <= int(period) <= 65535:
            problems.add(REGISTRY, row.line,
                         "period %r is not 1..65535 counts of 1/32768 s"
                         % period)
            continue
        name = row.get("constant in tools/ant_pages.py")
        if not hasattr(module, name):
            problems.add(REGISTRY, row.line,
                         "tools/ant_pages.py defines no %s" % name)
        elif getattr(module, name) != int(period):
            problems.add(REGISTRY, row.line,
                         "%s is %r in tools/ant_pages.py and %s here"
                         % (name, getattr(module, name), period))
        registered.setdefault(device_type, set()).add(int(period))

    for type_constant, _period, set_constant in ANT_PAGES_PERIOD_SETS:
        if not all(hasattr(module, name)
                   for name in (type_constant, set_constant)):
            continue
        device_type = getattr(module, type_constant)
        periods = set(getattr(module, set_constant))
        missing = sorted(periods - registered.get(device_type, set()))
        if missing:
            problems.add(REGISTRY, 0,
                         "device type 0x%02X permits period(s) %s in "
                         "tools/ant_pages.py %s that no row registers"
                         % (device_type, missing, set_constant))
        extra = sorted(registered.get(device_type, set()) - periods)
        if extra:
            problems.add(REGISTRY, 0,
                         "device type 0x%02X registers period(s) %s that "
                         "tools/ant_pages.py %s does not permit"
                         % (device_type, extra, set_constant))


COMPAT_CONSTANT_RULES = [
    ("COMPAT_DOM", COMPAT_DOM_BYTE,
     "the compat domain byte, which must match RADIANT_SEC_DOM_COMPAT_MAC"),
    ("COMPAT_TIER_I_TAG_BITS", COMPAT_TIER_I_TAG_BITS,
     "Tier I's tag length. 40 bits, not 48: the counter needs two in-page "
     "bytes now that no window index is implied"),
    ("COMPAT_TIER_II_TAG_BITS", COMPAT_TIER_II_TAG_BITS,
     "Tier II's tag length"),
    ("COMPAT_CMD_TAG_BITS", COMPAT_CMD_TAG_BITS,
     "the inbound command's tag length"),
    ("COMPAT_WINDOW_SIZES", COMPAT_WINDOW_SIZES,
     "the legal Tier II window set"),
    ("COMPAT_COUNTDOWN_LENGTHS", COMPAT_COUNTDOWN_LENGTHS,
     "the legal switch-countdown set"),
] + [(name, value,
      "the attestation subtype that reaches the nonce at position 9")
     for name, value in sorted(COMPAT_SUBTYPES.items())]

# The C constants, and the one rule that is not a value: the subtype has to be
# at nonce_block[9] rather than merely defined. A #define agreeing with ADR 0008
# proves the name and the number; only the line that writes it proves the
# position, and the position is the whole pin.
COMPAT_C_DEFINES = {
    "RADIANT_SEC_COMPAT_SUBTYPE_TIER_I": COMPAT_SUBTYPES["COMPAT_SUBTYPE_TIER_I"],
    "RADIANT_SEC_COMPAT_SUBTYPE_TIER_II": COMPAT_SUBTYPES["COMPAT_SUBTYPE_TIER_II"],
    "RADIANT_SEC_COMPAT_SUBTYPE_ANNOUNCE": COMPAT_SUBTYPES["COMPAT_SUBTYPE_ANNOUNCE"],
    "RADIANT_SEC_COMPAT_SUBTYPE_COMMAND": COMPAT_SUBTYPES["COMPAT_SUBTYPE_COMMAND"],
    "RADIANT_SEC_COMPAT_ANNOUNCE_TAG_BYTES": COMPAT_TIER_II_TAG_BITS // 8,
    "RADIANT_SEC_COMPAT_CMD_TAG_BYTES": COMPAT_CMD_TAG_BITS // 8,
    "RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES": COMPAT_TIER_I_TAG_BITS // 8,
    "RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES": COMPAT_TIER_II_TAG_BITS // 8,
    "RADIANT_SEC_COMPAT_N_MIN": min(COMPAT_WINDOW_SIZES),
    "RADIANT_SEC_COMPAT_N_MAX": max(COMPAT_WINDOW_SIZES),
}


def check_compat_module_constants(module, path, required, problems) -> None:
    """The value rules, against whichever module claims to hold them.

    `required` is what separates a phase that owes these constants from one that
    does not: tools/radiant_crypto.py mirrors the C primitives and must have
    them, while tools/ant_pages.py gets them with the page codecs in C3 and is
    checked only for what it has already.
    """
    if module is None:
        return
    for name, expected, why in COMPAT_CONSTANT_RULES:
        if not hasattr(module, name):
            if required:
                problems.add(path, 0,
                             "does not define %s - %s; see docs/decisions/0008"
                             % (name, why))
            continue
        actual = getattr(module, name)
        if isinstance(expected, tuple):
            actual = tuple(actual)
        if actual != expected:
            problems.add(path, 0,
                         "%s is %r, not %r - %s; see docs/decisions/0008"
                         % (name, actual, expected, why))

    subtypes = {name: getattr(module, name)
                for name in COMPAT_SUBTYPES if hasattr(module, name)}
    if len(set(subtypes.values())) != len(subtypes):
        problems.add(path, 0,
                     "the compat subtypes are not distinct (%r); they share "
                     "dom 0x%02X and nothing else separates them"
                     % (subtypes, COMPAT_DOM_BYTE))
    for name, value in sorted(subtypes.items()):
        if not 1 <= value <= 0x0F:
            problems.add(path, 0,
                         "%s is 0x%02X, which is not a nibble in 1..15" % (name, value))


def check_compat_constants(module, problems) -> None:
    """Rules the compat constants must satisfy, in C and in both mirrors.

    C2 defined the domain byte in radiant_sec.h, the subtypes and tag lengths in
    radiant_sec_compat.h, the tags themselves in radiant_sec_compat.c, and the
    Python mirror in tools/radiant_crypto.py - so all four are required here
    rather than skipped. tools/ant_pages.py is C3's and is still checked only
    for the constants it has already: the page codecs land with the page
    numbers, and demanding them before that phase would be demanding a guess.

    The domain-byte reservation is also checked from the other direction -
    nothing but RADIANT_SEC_DOM_COMPAT_MAC may take 0x04 - because a collision
    there is invisible until two tags coincide on the air.
    """
    if not SEC_HEADER.exists():
        problems.add(SEC_HEADER, 0, "file does not exist")
    else:
        text = SEC_HEADER.read_text(encoding="utf-8")
        doms = {}
        for name, value in re.findall(
                r"^#define\s+(RADIANT_SEC_DOM_[A-Z0-9_]+)\s+0x([0-9A-Fa-f]{2})",
                text, re.M):
            doms.setdefault(int(value, 16), []).append(name)
        for value, names in sorted(doms.items()):
            if len(names) > 1:
                problems.add(SEC_HEADER, 0,
                             "domain byte 0x%02X is defined by %s; the domain "
                             "byte is the only thing keeping two MAC blocks "
                             "apart, so two names for one value is a collision"
                             % (value, " and ".join(sorted(names))))
        owner = doms.get(COMPAT_DOM_BYTE, [])
        if not owner:
            problems.add(SEC_HEADER, 0,
                         "no domain byte is defined as 0x%02X; "
                         "docs/decisions/0008 reserves it for "
                         "RADIANT_SEC_DOM_COMPAT_MAC and C2 landed it"
                         % COMPAT_DOM_BYTE)
        elif owner != ["RADIANT_SEC_DOM_COMPAT_MAC"]:
            problems.add(SEC_HEADER, 0,
                         "0x%02X is taken by %s; docs/decisions/0008 reserves it "
                         "for RADIANT_SEC_DOM_COMPAT_MAC"
                         % (COMPAT_DOM_BYTE, ", ".join(sorted(owner))))

    if not SEC_COMPAT_HEADER.exists():
        problems.add(SEC_COMPAT_HEADER, 0, "file does not exist")
    else:
        text = SEC_COMPAT_HEADER.read_text(encoding="utf-8")
        for name, expected in sorted(COMPAT_C_DEFINES.items()):
            found = re.search(
                r"^#define\s+%s\s+(0x[0-9A-Fa-f]+|\d+)" % re.escape(name),
                text, re.M)
            if found is None:
                problems.add(SEC_COMPAT_HEADER, 0,
                             "does not define %s; see docs/decisions/0008" % name)
            elif int(found.group(1), 0) != expected:
                problems.add(SEC_COMPAT_HEADER, 0,
                             "%s is %s, not %d; see docs/decisions/0008"
                             % (name, found.group(1), expected))

    if not COMPAT_SOURCE.exists():
        problems.add(COMPAT_SOURCE, 0, "file does not exist")
    else:
        text = COMPAT_SOURCE.read_text(encoding="utf-8")
        # The subtype has to REACH the block. Written into a page byte instead,
        # it is chosen by whoever sends the page and separates nothing.
        if re.search(r"^\s*out\[9\]\s*=\s*sub\s*;", text, re.M) is None:
            problems.add(COMPAT_SOURCE, 0,
                         "nothing writes the subtype to nonce_block[9]; ADR "
                         "0008 pins the position, not just the value - a "
                         "subtype only in the page is chosen by the sender")

    check_compat_module_constants(load_module(RADIANT_CRYPTO, problems),
                                  RADIANT_CRYPTO, True, problems)
    check_compat_module_constants(module, ANT_PAGES, False, problems)


def main() -> int:
    problems = Problems()

    tables = read_tables(REGISTRY, problems)
    types = {}
    per_type_rows = {}

    if "device-types" not in tables:
        problems.add(REGISTRY, 0,
                     "no <!-- radiant-registry: device-types --> table")
    else:
        header, rows = tables["device-types"]
        types = check_device_types(REGISTRY, header, rows, problems)

    if "pages" not in tables:
        problems.add(REGISTRY, 0, "no <!-- radiant-registry: pages --> table")
    elif types:
        header, rows = tables["pages"]
        per_type_rows = check_pages(REGISTRY, header, rows, types, problems)

    # Loaded once and passed down: two checks need it, and importing it twice
    # would report an import failure twice.
    module = load_ant_pages(problems)

    if types:
        check_pagemap(types, per_type_rows, problems)
        check_ant_plus_doc(types, problems)
        check_against_ant_pages(types, module, problems)
    check_reserved_space(problems)
    check_compat_pins(problems)
    check_compat_allocation(per_type_rows, problems)
    check_compat_periods(tables, module, problems)
    check_compat_constants(module, problems)

    if problems:
        print("profile registry: %d problem(s)" % len(problems))
        for item in problems.items:
            print("  " + item)
        return 1

    claimed = sum(1 for row in types.values()
                  if row.get("status").lower() != "ant-plus-reserved")
    print("profile registry: %d device type(s) recorded, %d claimed by RadiANT "
          "or third parties, %d page map(s) agree with the registry and periods "
          "agree with tools/ant_pages.py"
          % (len(types), claimed, len(RADIANT_PAGE_MAPS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
