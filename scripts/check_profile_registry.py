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
  * the page map in docs/radiant-telemetry.md and the registry's page rows for
    the RadiANT device type agree, number for number and name for name
  * the device type table in docs/ant-plus-profiles.md agrees with the registry
  * every device type and period that tools/ant_pages.py defines appears in the
    registry with the same period, so the registry cannot drift from the code
  * the common-page cadence stated in the docs still matches
    ant_pages.COMMON_PAGE_INTERVAL

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
ANT_PLUS = REPO / "docs" / "ant-plus-profiles.md"
ANT_PAGES = REPO / "tools" / "ant_pages.py"

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
OPT_IN_VALUES = {"no", "yes", "per-node"}

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

        for column in ("lr phy", "adaptive freq"):
            if row.get(column).lower() not in OPT_IN_VALUES:
                problems.add(path, row.line, "column '%s' is %r, not one of %s"
                             % (column, row.get(column), sorted(OPT_IN_VALUES)))

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


def check_pagemap(types, per_type_rows, problems) -> None:
    tables = read_tables(TELEMETRY, problems)
    if "pagemap" not in tables:
        problems.add(TELEMETRY, 0, "no <!-- radiant-registry: pagemap --> table")
        return
    header, rows = tables["pagemap"]
    if not require_columns(TELEMETRY, "pagemap", header, PAGEMAP_COLUMNS,
                           problems):
        return

    radiant = [device_type for device_type, row in types.items()
               if row.get("status").lower() == "radiant"]
    if len(radiant) != 1:
        problems.add(REGISTRY, 0,
                     "expected exactly one device type with status 'radiant', "
                     "found %d; extend check_pagemap() to say which one the "
                     "page map in docs/radiant-telemetry.md belongs to"
                     % len(radiant))
        return
    device_type = radiant[0]

    registry_map = {}
    for row in per_type_rows.get(device_type, []):
        registry_map[row.get("page").lower()] = (row.get("page"),
                                                 row.get("name"))

    doc_map = {}
    for row in rows:
        require_non_empty(TELEMETRY, row, PAGEMAP_COLUMNS, problems)
        key = row.get("page").lower()
        if key in doc_map:
            problems.add(TELEMETRY, row.line, "page %r appears twice"
                         % row.get("page"))
        doc_map[key] = (row.get("page"), row.get("name"), row.line)

    for key, (shown, name) in sorted(registry_map.items()):
        if key not in doc_map:
            problems.add(TELEMETRY, 0,
                         "page %s is claimed for device type 0x%02X in "
                         "docs/profile-registry.md but is missing from the page "
                         "map" % (shown, device_type))
        elif doc_map[key][1] != name:
            problems.add(TELEMETRY, doc_map[key][2],
                         "page %s is named %r here and %r in "
                         "docs/profile-registry.md"
                         % (shown, doc_map[key][1], name))

    for key, (shown, name, line) in sorted(doc_map.items()):
        if key not in registry_map:
            problems.add(TELEMETRY, line,
                         "page %s is in the page map but is not claimed for "
                         "device type 0x%02X in docs/profile-registry.md"
                         % (shown, device_type))


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


def load_ant_pages(problems):
    if not ANT_PAGES.exists():
        problems.add(ANT_PAGES, 0, "file does not exist")
        return None
    spec = importlib.util.spec_from_file_location("ant_pages", ANT_PAGES)
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
    except Exception as error:               # noqa: BLE001 - report, don't crash
        problems.add(ANT_PAGES, 0, "cannot be imported: %s" % error)
        return None
    return module


def check_against_ant_pages(types, problems) -> None:
    module = load_ant_pages(problems)
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


def check_reserved_space(problems) -> None:
    """The two fields that cannot be added later without a format break."""
    if not TELEMETRY.exists():
        return
    text = TELEMETRY.read_text(encoding="utf-8")
    for needle, why in (
            ("epoch (u32 LE)",
             "the descriptor's 4-byte epoch, which docs/radiant-security.md "
             "needs for X_PRIV, X_CONF and command replay rejection"),
            ("event counter (u8)",
             "the data page's mandatory counter, which is the X_CONF nonce "
             "source and the loss detector"),
    ):
        if needle not in text:
            problems.add(TELEMETRY, 0,
                         "no longer reserves %s (looked for %r)" % (why, needle))


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

    if types:
        check_pagemap(types, per_type_rows, problems)
        check_ant_plus_doc(types, problems)
        check_against_ant_pages(types, problems)
    check_reserved_space(problems)

    if problems:
        print("profile registry: %d problem(s)" % len(problems))
        for item in problems.items:
            print("  " + item)
        return 1

    claimed = sum(1 for row in types.values()
                  if row.get("status").lower() != "ant-plus-reserved")
    print("profile registry: %d device type(s) recorded, %d claimed by RadiANT "
          "or third parties, page maps agree with docs/radiant-telemetry.md and "
          "periods agree with tools/ant_pages.py" % (len(types), claimed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
