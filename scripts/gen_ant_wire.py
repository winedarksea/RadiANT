#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Render protocol/ant_wire.yaml into the four committed outputs.

    C:\\ncs\\toolchains\\dcbdc366a1\\opt\\bin\\python.exe scripts\\gen_ant_wire.py
    C:\\ncs\\toolchains\\dcbdc366a1\\opt\\bin\\python.exe scripts\\gen_ant_wire.py --check

Outputs:

    radiant/include/radiant/radiant_wire.h
                                    RADIANT_WIRE_* macros - the module's own
                                    copy, so radiant never has to reach
                                    outside itself for a wire constant
    src/ant_wire.h                 ANTW_* macros for the firmware, now a thin
                                    alias of radiant_wire.h's definitions
    tools/ant_wire.py              module constants and lookup tables
    docs/ant-serial-protocol.md    only the region between the GENERATED markers

All four are committed, so a clone builds and the tools run without anyone
having a YAML parser installed. ``--check`` renders into memory, diffs against
what is on disk, prints a unified diff of any drift and exits 1. CI calls it.

Why generation, and not a test that cross-checks the header against the module
--------------------------------------------------------------------------

A cross-check test is the obvious alternative and it is worse in four ways. It
costs about the same to write. It leaves two files to hand-edit, so the work is
still duplicated and only the *disagreement* is caught. It is structurally
blind to a constant that exists on one side only, which is the commonest way
the two drift. And decisively: **it would have passed during the checksum
bug.** Both the firmware and the Python tools had ``SYNC = 0xA4`` and agreed
with each other perfectly, while every frame either of them produced was
rejected by every real ANT host - because the bug was in a *rule* (does the XOR
include the SYNC byte) and not in a constant. A generator cannot catch that
either, but it does not pretend to: what catches rule-level drift is a third
implementation, which is what the golden frame vectors in tools/test_ant_wire.py
are for. Generation is for making one number appear in three places without
anyone typing it three times, and that is all it claims.

Determinism
-----------

Output order is YAML document order; there are no timestamps, no hostnames and
no dict re-ordering. Every file is written with LF endings whatever the
platform, so ``--check`` is idempotent on Windows and in Linux CI alike.

Dependencies: the standard library, plus PyYAML - which the NCS toolchain's
interpreter above already has (verified: PyYAML 6.0.3).
"""

from __future__ import annotations

import argparse
import difflib
import sys
import textwrap
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover - user-facing guidance
    sys.exit(
        "PyYAML is not available to this interpreter. Use the NCS toolchain's:\n"
        "  C:\\ncs\\toolchains\\dcbdc366a1\\opt\\bin\\python.exe scripts\\gen_ant_wire.py"
    )

ROOT = Path(__file__).resolve().parent.parent
SPEC_PATH = ROOT / "protocol" / "ant_wire.yaml"
# apps/common/ant/, NOT src/. This said `ROOT / "src" / "ant_wire.h"` until
# 2026-08-14, months after the apps/ reorg moved the header. The consequence was
# worse than a broken path: the generator CREATED a stray src/ant_wire.h at the
# repository root on every run, --check compared against that file, found it in
# sync with itself, and passed - while the header the firmware actually compiles
# drifted freely. The `generated-drift` CI job was green the entire time.
#
# That is the exact failure mode the plan's cross-cutting section warns about
# for the tolerant CI greps: a rename does not break a check, it silently
# disarms one. Anything added here must fail loudly when its target is missing
# rather than quietly write a new file beside it - see check_outputs() below.
HEADER_PATH = ROOT / "apps" / "common" / "ant" / "ant_wire.h"
RADIANT_WIRE_HEADER_PATH = ROOT / "radiant" / "include" / "radiant" / "radiant_wire.h"
PYTHON_PATH = ROOT / "tools" / "ant_wire.py"
DOC_PATH = ROOT / "docs" / "ant-serial-protocol.md"

DOC_BEGIN = "<!-- BEGIN GENERATED: ant_wire (scripts/gen_ant_wire.py) -->"
DOC_END = "<!-- END GENERATED: ant_wire -->"

BANNER_LINES = [
    "DO NOT EDIT - generated from protocol/ant_wire.yaml",
    "by scripts/gen_ant_wire.py. Edit the YAML and re-run the generator;",
    "`scripts/gen_ant_wire.py --check` fails the build if these drift apart.",
]

PREFIX_RATIONALE = """\
Why every name here carries an ANTW_ prefix

This is not house style. sdk-ant's ant_parameters.h defines its error codes as
computed expressions - NRF_ANT_ERROR_OFFSET + INVALID_MESSAGE - rather than as
literals. C permits a macro to be redefined only with an identical token
sequence, so a header of ours that spelled a constant INVALID_MESSAGE could
never share a translation unit with sdk-ant's: it is a hard redefinition error,
not a warning, and no include order fixes it.

The prefix is exactly what makes the two coexist. src/ant_radio_sdk_ant.c is
the one file permitted to include both, and it is where every ANTW_* constant
is BUILD_ASSERTed against its sdk-ant counterpart. That check costs nothing,
runs only where sdk-ant is present - which is the only place it can run - and
is why keeping the sdk-ant backend is worth more than a fallback.

It also makes src/ant_serial_bridge.c textually free of Garmin's API names,
which is not cosmetic either: this is a clean-room rebuild, and the boundary is
easier to defend when it is visible in the source.

The Python module deliberately does NOT carry the prefix. The collision it
solves is a C preprocessor problem, and Python already has module namespaces:
`ant_wire.MESG_ASSIGN_CHANNEL_ID` is unambiguous, and keeping the bare spelling
makes converting the five tools a matter of deleting a local definition and
adding an import, with no renaming at all.

Every ANTW_* name below is now an alias of radiant's own
RADIANT_WIRE_* definition (radiant/include/radiant/radiant_wire.h) -
not a second, independently-generated copy of the same value. That is what
lets radiant reach no path outside itself for a wire constant while this
file, and every ANTW_* call site in this application, stays untouched."""

MODULE_RATIONALE = """\
Why this header exists inside radiant, generated separately from
src/ant_wire.h

radiant is a Zephyr module meant to be dropped into a project with
nothing else from this repository on its include path - see
docs/decisions/0002-clean-room-policy.md and the module's own README. Before
this header existed, every module source that needed a wire constant reached
outside the module (`zephyr_include_directories(... ../src)`, then
`#include "ant_wire.h"`) to get one - a self-containment violation that a
`git filter-repo` split of this directory alone, or any out-of-tree consumer,
would fail on at the first ANTW_ use.

RADIANT_WIRE_* is the fix: the same values, generated from the same
protocol/ant_wire.yaml entries, defined here instead so nothing in this module
ever needs to leave it. src/ant_wire.h - the application-layer header every
ant_radio_*.c backend and src/ant_serial_bridge.c already use - becomes a thin
alias of this header under the ANTW_ names, so no application code changes;
see that file for why ANTW_ is the name application code keeps."""


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------


def load_spec() -> dict:
    with open(SPEC_PATH, "r", encoding="utf-8") as handle:
        return yaml.safe_load(handle)


def fmt_value(value, fmt: str) -> str:
    """Render a number the way the YAML asked for."""
    if value is None:
        return "(unresolved)"
    if fmt == "dec":
        return str(value)
    if value <= 0xFF:
        return "0x%02X" % value
    return "0x%04X" % value


def c_comment(text: str, indent: str = " ", width: int = 78) -> list[str]:
    """A block comment, wrapped, with */ neutralised."""
    body = " ".join(text.split()).replace("*/", "* /")
    lines = textwrap.wrap(body, width=width - len(indent) - 3) or [""]
    if len(lines) == 1:
        return ["%s/* %s */" % (indent, lines[0])]
    out = ["%s/*" % indent]
    out += ["%s * %s" % (indent, line) for line in lines]
    out.append("%s */" % indent)
    return out


def py_comment(text: str, indent: str = "", width: int = 79) -> list[str]:
    body = " ".join(text.split())
    return [
        "%s# %s" % (indent, line)
        for line in (textwrap.wrap(body, width=width - len(indent) - 2) or [""])
    ]


def one_line(text) -> str:
    return " ".join(str(text or "").split())


def md_cell(text) -> str:
    return one_line(text).replace("|", "\\|")


def provenance(entry: dict) -> str:
    """The `source` tag, plus the shim flag when there is one."""
    src = one_line(entry.get("source", "")) or "-"
    if entry.get("verify"):
        src += " + verify:%s" % entry["verify"]
    return src


# ---------------------------------------------------------------------------
# reply_len - request/response pairs that share a message id
# ---------------------------------------------------------------------------
#
# A message id is not a message. MESG_REQUEST (0x4D) names an id and what comes
# back is a frame carrying that same id with a payload no host ever sends, so a
# single payload_len per id describes the command and silently mis-describes
# the reply. `reply_len` is the second half; see the schema note in the YAML.
#
# The lengths are named, not written: every one of them already exists in
# `message_sizes` as the LEN byte the bridge composes, with the prose that
# explains it. Resolving the name here keeps one number in one place, and turns
# those constants from things nothing machine-checks into things a transcript
# replay checks on every run.

REPLY_SELECTORS = {
    "reply_byte_0": "the info type the reply echoes at byte 0",
    "request_index": "byte 0 of the MESG_REQUEST that asked, which the reply "
                     "does not repeat",
    "none": "nothing - there is only one shape",
}


def size_table(spec: dict) -> dict:
    return {e["name"]: e.get("value") for e in spec["message_sizes"]["values"]}


def resolve_reply_len(spec: dict, message: dict):
    """The reply shapes of one message, with every `size:` name resolved.

    Returns None for the overwhelming majority of messages, whose reply - if
    they have one at all - is the same shape as their command. Raises rather
    than warns on a malformed block: a reply shape that silently vanished would
    turn a length check back into no check, which is the failure mode this
    field exists to fix.
    """
    block = message.get("reply_len")
    if block is None:
        return None

    where = "%s reply_len" % message["name"]
    selector = block.get("selector")
    if selector not in REPLY_SELECTORS:
        raise SystemExit(
            "%s: selector %r is not one of %s"
            % (where, selector, ", ".join(sorted(REPLY_SELECTORS)))
        )

    sizes = size_table(spec)
    shapes: list[dict] = []
    seen: set = set()
    for shape in block.get("shapes") or []:
        name = shape.get("size")
        if name not in sizes:
            raise SystemExit(
                "%s: size %r names no entry in message_sizes" % (where, name)
            )
        if sizes[name] is None:
            raise SystemExit(
                "%s: size %r is unresolved, so no reply length can be checked "
                "against it" % (where, name)
            )
        when = shape.get("when")
        if selector == "none":
            if when is not None:
                raise SystemExit(
                    "%s: selector 'none' but shape %r carries a `when`"
                    % (where, name)
                )
        else:
            if when is None:
                raise SystemExit(
                    "%s: selector %r but shape %r has no `when`"
                    % (where, selector, name)
                )
            if when in seen:
                raise SystemExit(
                    "%s: two shapes both claim `when: %s`" % (where, when)
                )
            seen.add(when)
        shapes.append({
            "when": when,
            "len": sizes[name],
            "size": name,
            "desc": one_line(shape.get("desc")),
        })

    if not shapes:
        raise SystemExit("%s: no shapes" % where)
    if selector == "none" and len(shapes) != 1:
        raise SystemExit(
            "%s: selector 'none' but %d shapes" % (where, len(shapes))
        )
    return {
        "selector": selector,
        "desc": one_line(block.get("desc")),
        "shapes": shapes,
    }


def reply_len_note(reply: dict, prefix: str = "") -> str:
    """One sentence naming every reply shape and what picks it."""
    parts = []
    for shape in reply["shapes"]:
        macro = prefix + shape["size"]
        if shape["when"] is None:
            parts.append("%d bytes (%s)" % (shape["len"], macro))
        else:
            parts.append("0x%02X -> %d bytes (%s)"
                         % (shape["when"], shape["len"], macro))
    return (
        "REQUEST REPLY, a different shape from the command above: %s, selected "
        "by %s. %s" % ("; ".join(parts), REPLY_SELECTORS[reply["selector"]],
                       reply["desc"])
    )


def message_desc(spec: dict, message: dict, prefix: str = "") -> str:
    """A message's prose, with its reply shapes appended when it has any."""
    desc = one_line(message.get("desc"))
    reply = resolve_reply_len(spec, message)
    if reply is None:
        return desc
    return "%s %s" % (desc, reply_len_note(reply, prefix))


def group_entries(group: dict) -> list[dict]:
    fmt = group.get("format", "hex")
    out = []
    for value in group["values"]:
        item = dict(value)
        item.setdefault("format", fmt)
        out.append(item)
    return out


def all_constants(spec: dict) -> list[tuple[str, dict]]:
    """(section label, entry) for every named constant, in document order.

    Messages carry their number in `id`; everything else in `value`. They are
    normalised here so the three renderers and the provenance table all walk
    the same list.
    """
    out: list[tuple[str, dict]] = []

    for entry in spec["framing"]["constants"]:
        item = dict(entry)
        item.setdefault("format", "hex")
        out.append(("framing", item))

    for message in spec["messages"]:
        item = dict(message)
        item["value"] = message.get("id")
        item["format"] = "hex"
        out.append(("messages", item))

    sizes = spec["message_sizes"]
    for entry in sizes["values"]:
        item = dict(entry)
        item.setdefault("format", sizes.get("format", "dec"))
        out.append(("message_sizes", item))

    for group in spec["constant_groups"]:
        for item in group_entries(group):
            out.append((group["name"], item))

    for byte in spec["capabilities"]["bytes"]:
        for bit in byte.get("bits") or []:
            item = dict(bit)
            item.setdefault("format", "hex")
            out.append(("capabilities", item))

    for message in spec["radiant_messages"]["values"]:
        item = dict(message)
        item["value"] = message.get("id")
        item["format"] = "hex"
        out.append(("radiant_messages", item))

    return out


def unresolved(spec: dict) -> list[tuple[str, dict]]:
    return [(sec, e) for sec, e in all_constants(spec) if e.get("value") is None]


def needs_shim(spec: dict) -> list[tuple[str, dict]]:
    return [(sec, e) for sec, e in all_constants(spec) if e.get("verify")]


# ---------------------------------------------------------------------------
# src/ant_wire.h
# ---------------------------------------------------------------------------


def render_wire_header(
    spec: dict,
    *,
    prefix_name: str,
    guard: str,
    title_lines: list[str],
    rationale: str,
    alias_of: str | None = None,
) -> str:
    """Render one wire-constants header.

    Two callers, two very different files from the same walk of `spec`:

      alias_of=None       radiant/include/radiant/radiant_wire.h,
                           the canonical definitions, RADIANT_WIRE_* named.
      alias_of="RADIANT_WIRE"
                           src/ant_wire.h, ANTW_* named, every #define a
                           one-line alias of the constant above rather than a
                           second literal - so the two can never independently
                           drift the way two hand-written copies would.

    The alias file skips the long per-constant descriptive comments (the
    canonical header already carries them) but keeps the section dividers,
    so it stays readable as a map from ANTW_ to RADIANT_WIRE_ names rather
    than a wall of #defines.
    """
    prefix = prefix_name + "_"
    alias_prefix = (alias_of + "_") if alias_of else None
    out: list[str] = []
    add = out.append

    add("/* SPDX-License-Identifier: Apache-2.0 */")
    add("/*")
    for line in BANNER_LINES:
        add(" * " + line)
    add(" *")
    for line in title_lines:
        add(" * " + line)
    add(" *")
    for line in rationale.splitlines():
        add((" * " + line).rstrip())
    add(" *")
    add(" * Nothing here describes the on-air link layer. This header is about")
    add(" * bytes on a USB bulk endpoint or a UART, and about nothing else.")
    add(" */")
    add("")
    add("#ifndef %s" % guard)
    add("#define %s" % guard)
    add("")
    if alias_of:
        add("#include <radiant/radiant_wire.h>")
    else:
        add("#include <stdint.h>")
    add("")

    def emit(entry: dict) -> None:
        name = prefix + entry["name"]
        if entry.get("value") is None:
            if not alias_of:
                desc = one_line(entry.get("desc"))
                note = "[%s]" % provenance(entry)
                out.extend(
                    c_comment(
                        "%s is UNRESOLVED. %s Recover it once the value is "
                        "known, in protocol/ant_wire.yaml, and regenerate; "
                        "see the unresolved table in "
                        "docs/ant-serial-protocol.md. %s" % (name, desc, note),
                        indent="",
                    )
                )
                add("")
            return
        if alias_of:
            add("#define %-52s %s" % (name, alias_prefix + entry["name"]))
        else:
            desc = one_line(entry.get("desc"))
            note = "[%s]" % provenance(entry)
            out.extend(c_comment("%s %s" % (desc, note), indent=""))
            add("#define %-52s %s" % (name, fmt_value(entry["value"], entry.get("format", "hex"))))
        add("")

    def section(title: str, note: str = "") -> None:
        add("/* " + "-" * 72)
        add(" * " + title)
        if note:
            for line in textwrap.wrap(one_line(note), 72):
                add(" * " + line)
        add(" * " + "-" * 72 + " */")
        add("")

    section("Framing", spec["framing"]["checksum_rule"])
    for entry in spec["framing"]["constants"]:
        item = dict(entry)
        item.setdefault("format", "hex")
        emit(item)

    section(
        "Message identifiers",
        "The ID byte of a frame. Direction, payload length and whether the "
        "bridge implements each one are tabulated in docs/ant-serial-protocol.md.",
    )
    for message in spec["messages"]:
        item = dict(message)
        item["value"] = message.get("id")
        item["format"] = "hex"
        item["desc"] = message_desc(spec, message, prefix)
        emit(item)

    sizes = spec["message_sizes"]
    section(
        "Reply sizes",
        "The LEN byte the bridge puts on each reply it composes itself. Where "
        "a request reply is a different shape from the command sharing its id, "
        "the message's reply_len block in protocol/ant_wire.yaml points at the "
        "constant here rather than restating the number.",
    )
    for entry in sizes["values"]:
        item = dict(entry)
        item.setdefault("format", sizes.get("format", "dec"))
        emit(item)

    for group in spec["constant_groups"]:
        section(group["title"], group.get("desc", ""))
        for item in group_entries(group):
            if item.get("emit_c") is False:
                out.extend(
                    c_comment(
                        "%s%s is a Python-side alias of an identical value and is "
                        "deliberately not defined here. %s"
                        % (prefix, item["name"], one_line(item.get("desc"))),
                        indent="",
                    )
                )
                add("")
                continue
            emit(item)

    caps = spec["capabilities"]
    section(
        "Capabilities reply (MESG_CAPABILITIES)",
        "%d bytes. Observed from this firmware: %s. Byte and bit layout in "
        "docs/ant-serial-protocol.md."
        % (caps["reply_len"], caps["observed"]["hex"]),
    )
    for byte in caps["bytes"]:
        cap_field = "CAPABILITIES_OFFSET_" + byte["field"].upper()
        if alias_of:
            add("#define %-52s %s" % (prefix + cap_field, alias_prefix + cap_field))
        else:
            out.extend(
                c_comment(
                    "byte %d - %s. %s" % (byte["index"], byte["field"], one_line(byte.get("desc"))),
                    indent="",
                )
            )
            add("#define %-52s %s" % (prefix + cap_field, byte["index"]))
        add("")
        for bit in byte.get("bits") or []:
            item = dict(bit)
            item.setdefault("format", "hex")
            emit(item)

    radiant = spec["radiant_messages"]
    section(
        "RadiANT extension messages - NOT ANT protocol",
        "Ours, not Garmin's: not in Rev 5.1, not answered by any ANT device. "
        "%s is reserved for the rest of the family. Semantics live in "
        "docs/radiant-security.md; only the numbering is decided in the YAML. "
        "These were first proposed at 0xE0-0xE4 and moved: sdk-ant defines "
        "MESG_EXT_ID_0 .. MESG_EXT_ID_4 as exactly 0xE0-0xE4, an "
        "extended-message-id block selected by MSG_EXT_ID_MASK. Do not "
        "re-propose that range. Lib config 0xE0 "
        "(%sLIB_CONFIG_ALL_EXT_FIELDS) is a third, unrelated namespace and "
        "has not moved - one is a frame's ID field, the other a payload byte "
        "of message 0x6E." % (radiant["reserved_range"], prefix),
    )
    for message in radiant["values"]:
        item = dict(message)
        item["value"] = message.get("id")
        item["format"] = "hex"
        emit(item)

    add("#endif /* %s */" % guard)
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------
# tools/ant_wire.py
# ---------------------------------------------------------------------------

PY_HELPERS = '''
# ---------------------------------------------------------------------------
# The one rule that is not a constant
# ---------------------------------------------------------------------------


def checksum(data: bytes) -> int:
    """XOR of every byte handed in, SYNC included.

    Pass the whole frame except the checksum byte itself. Seeding at 0 and
    starting after SYNC - which is the natural-looking mistake - produces a
    value exactly 0xA4 off, and two implementations that both make it agree
    with each other and with nobody else. That cost this project a week.
    """
    total = 0
    for byte in data:
        total ^= byte
    return total


def frame(msg_id: int, payload: bytes = b"") -> bytes:
    """Wrap a message: SYNC, LEN, ID, payload, XOR."""
    head = bytes([SYNC_TX, len(payload), msg_id]) + bytes(payload)
    return head + bytes([checksum(head)])


def unframe(buf: bytes):
    """(msg_id, payload) for one complete frame, or None if it is not valid.

    Rejects on SYNC, on length, and on the checksum. It does not resynchronise:
    that is the caller's ring buffer's job.
    """
    if len(buf) < MSG_OVERHEAD or buf[0] != SYNC_TX:
        return None
    total = buf[1] + MSG_OVERHEAD
    if len(buf) < total or checksum(buf[: total - 1]) != buf[total - 1]:
        return None
    return buf[2], bytes(buf[3 : total - 1])
'''


def render_python(spec: dict) -> str:
    out: list[str] = []
    add = out.append

    add("# SPDX-License-Identifier: Apache-2.0")
    add('"""ANT serial protocol constants and lookup tables.')
    add("")
    for line in BANNER_LINES:
        add(line)
    add("")
    add("Source of truth: protocol/ant_wire.yaml")
    add("Naming authority: " + spec["meta"]["spec"])
    add("Human-readable companion: docs/ant-serial-protocol.md")
    add("")
    add(
        textwrap.fill(
            "Standard library only, and no imports at all - this module is meant "
            "to be copied into another project on its own. The names are the "
            "bare sdk-ant spellings rather than the ANTW_ ones the C header "
            "uses: the prefix exists to dodge a C preprocessor collision, and "
            "Python already has module namespaces.",
            79,
        )
    )
    add("")
    add(
        textwrap.fill(
            "Nothing here describes the on-air link layer - this is bytes on a "
            "USB bulk endpoint or a UART, and nothing else.",
            79,
        )
    )
    add('"""')
    add("")

    def emit(entry: dict) -> None:
        desc = one_line(entry.get("desc"))
        note = "[%s]" % provenance(entry)
        if entry.get("value") is None:
            out.extend(py_comment("%s: UNRESOLVED. %s %s" % (entry["name"], desc, note)))
            add("")
            return
        out.extend(py_comment("%s %s" % (desc, note)))
        add("%s = %s" % (entry["name"], fmt_value(entry["value"], entry.get("format", "hex"))))
        add("")

    def section(title: str, note: str = "") -> None:
        add("# " + "-" * 75)
        add("# " + title)
        if note:
            out.extend(py_comment(note))
        add("# " + "-" * 75)
        add("")

    section("Framing", spec["framing"]["checksum_rule"])
    for entry in spec["framing"]["constants"]:
        item = dict(entry)
        item.setdefault("format", "hex")
        emit(item)

    section("Message identifiers")
    for message in spec["messages"]:
        item = dict(message)
        item["value"] = message.get("id")
        item["format"] = "hex"
        item["desc"] = message_desc(spec, message)
        emit(item)

    sizes = spec["message_sizes"]
    section("Reply sizes")
    for entry in sizes["values"]:
        item = dict(entry)
        item.setdefault("format", sizes.get("format", "dec"))
        emit(item)

    for group in spec["constant_groups"]:
        section(group["title"], group.get("desc", ""))
        for item in group_entries(group):
            emit(item)

    caps = spec["capabilities"]
    section(
        "Capabilities reply (MESG_CAPABILITIES)",
        "%d bytes. Observed from this firmware: %s"
        % (caps["reply_len"], caps["observed"]["hex"]),
    )
    for byte in caps["bytes"]:
        add(
            "CAPABILITIES_OFFSET_%s = %d"
            % (byte["field"].upper(), byte["index"])
        )
    add("")
    for byte in caps["bytes"]:
        for bit in byte.get("bits") or []:
            item = dict(bit)
            item.setdefault("format", "hex")
            emit(item)

    radiant = spec["radiant_messages"]
    section(
        "RadiANT extension messages - NOT ANT protocol",
        "Ours, not Garmin's: not in Rev 5.1, not answered by any ANT device. "
        "%s is reserved for the rest of the family. Kept out of MESSAGES on "
        "purpose, so a tool that walks the ANT protocol never sees them. "
        "These were first proposed at 0xE0-0xE4 and moved, because sdk-ant's "
        "MESG_EXT_ID_0 .. MESG_EXT_ID_4 occupy exactly that range. Lib config "
        "0xE0 (LIB_CONFIG_ALL_EXT_FIELDS) is a third, unrelated namespace and "
        "has not moved." % radiant["reserved_range"],
    )
    for message in radiant["values"]:
        item = dict(message)
        item["value"] = message.get("id")
        item["format"] = "hex"
        emit(item)

    # ---- lookup tables -----------------------------------------------------
    section(
        "Lookup tables",
        "Built here rather than in each tool, so a name printed by ant_probe.py "
        "and a name printed by ant_verify.py cannot disagree.",
    )

    add("# Every message the protocol defines, keyed by id.")
    add("#")
    out.extend(py_comment(
        "`payload_len` is the command - the h2d direction. `reply_len` is "
        "present only where a request's reply is a different shape from the "
        "command sharing its id, and a reader that has a record's direction in "
        "hand must consult it rather than payload_len for a d2h record. Absent "
        "means the reply, if there is one, has the command's shape."))
    add("MESSAGES = {")
    for message in spec["messages"]:
        if message.get("id") is None:
            continue
        add("    0x%02X: {" % message["id"])
        add('        "name": %r,' % message["name"])
        add('        "direction": %r,' % message["direction"])
        add('        "payload_len": %r,' % message["payload_len"])
        add('        "bridged": %r,' % bool(message.get("bridged")))
        reply_len = resolve_reply_len(spec, message)
        if reply_len:
            add('        "reply_len": {')
            add('            "selector": %r,' % reply_len["selector"])
            add('            "desc": %r,' % reply_len["desc"])
            add('            "shapes": (')
            for shape in reply_len["shapes"]:
                add('                {"when": %s, "len": %d, "size": %r,'
                    % ("None" if shape["when"] is None
                       else "0x%02X" % shape["when"],
                       shape["len"], shape["size"]))
                add('                 "desc": %r},' % shape["desc"])
            add('            ),')
            add('        },')
        reply = message.get("reply")
        if reply:
            add('        "reply": %r,' % reply)
        host_api = message.get("host_api")
        if host_api:
            add('        "host_api": %r,' % host_api)
        add('        "desc": %r,' % one_line(message.get("desc")))
        add("    },")
    add("}")
    add("")
    add("# RadiANT extensions, kept out of MESSAGES on purpose.")
    add("RADIANT_MESSAGES = {")
    for message in spec["radiant_messages"]["values"]:
        add("    0x%02X: {" % message["id"])
        add('        "name": %r,' % message["name"])
        add('        "direction": %r,' % message["direction"])
        add('        "payload_len": %r,' % message["payload_len"])
        add('        "desc": %r,' % one_line(message.get("desc")))
        add("    },")
    add("}")
    add("RADIANT_RESERVED_RANGE = %r" % spec["radiant_messages"]["reserved_range"])
    add("")
    add("MESSAGE_NAMES = {mid: info[\"name\"] for mid, info in MESSAGES.items()}")
    add("MESSAGE_IDS = {info[\"name\"]: mid for mid, info in MESSAGES.items()}")
    add("")
    add("# Message ids the bridge's dispatch() implements today.")
    add(
        "BRIDGED_MESSAGE_IDS = frozenset(\n"
        "    mid for mid, info in MESSAGES.items() if info[\"bridged\"]\n"
        ")"
    )
    add("")

    for group in spec["constant_groups"]:
        entries = [e for e in group_entries(group) if e.get("value") is not None]
        if not entries:
            continue
        table = group["name"].upper()
        add("%s = {" % table)
        for item in entries:
            add(
                "    %r: %s,"
                % (item["name"], fmt_value(item["value"], item.get("format", "hex")))
            )
        add("}")
        values = [e["value"] for e in entries]
        if len(set(values)) == len(values):
            add("%s_BY_VALUE = {value: name for name, value in %s.items()}" % (table, table))
        else:
            out.extend(
                py_comment(
                    "No %s_BY_VALUE: two names in this group share a value on "
                    "purpose, and a reverse lookup would silently pick one."
                    % table
                )
            )
        add("")

    add("# Capabilities: byte index -> (field name, ((bit, name, description), ...)).")
    add("CAPABILITY_BYTES = {")
    for byte in caps["bytes"]:
        add("    %d: (%r, (" % (byte["index"], byte["field"]))
        for bit in byte.get("bits") or []:
            add(
                "        (0x%02X, %r, %r),"
                % (bit["value"], bit["name"], one_line(bit.get("desc")))
            )
        add("    )),")
    add("}")
    add("")
    add("# The reply this firmware actually gives, for tools that want to diff it.")
    add("OBSERVED_CAPABILITIES = bytes.fromhex(%r)" % caps["observed"]["hex"])
    add("")

    add("# Constants whose value is not recoverable without sdk-ant. Name -> why.")
    add("UNRESOLVED = {")
    for _section, entry in unresolved(spec):
        add("    %r: %r," % (entry["name"], one_line(entry.get("desc"))))
    add("}")
    add("")
    add("# Constants with no witness in this repository. The Wave 2 shim asserts them.")
    add("VERIFY_IN_SHIM = (")
    for _section, entry in needs_shim(spec):
        add("    %r," % entry["name"])
    add(")")

    add(PY_HELPERS.rstrip("\n"))
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------
# docs/ant-serial-protocol.md - the generated region only
# ---------------------------------------------------------------------------


def render_doc_region(spec: dict) -> str:
    out: list[str] = []
    add = out.append

    add(DOC_BEGIN)
    add("")
    add(
        "*Everything from here to the end marker is generated from "
        "`protocol/ant_wire.yaml`. Edit the YAML, not this section.*"
    )
    add("")

    add("## Framing constants")
    add("")
    add("| Constant | Value | Meaning | Provenance |")
    add("|---|---|---|---|")
    for entry in spec["framing"]["constants"]:
        add(
            "| `ANTW_%s` | `%s` | %s | %s |"
            % (
                entry["name"],
                fmt_value(entry.get("value"), entry.get("format", "hex")),
                md_cell(entry.get("desc")),
                md_cell(provenance(entry)),
            )
        )
    add("")

    add("## Messages")
    add("")
    add(
        "`dir` is `h2d` host to dongle, `d2h` dongle to host, `both`, or "
        "`marker` for an identifier that is a field inside another message "
        "rather than a frame of its own. `len` is the LEN byte, so it counts "
        "the leading channel or index byte where the message has one. "
        "`br` is whether `dispatch()` in `src/ant_serial_bridge.c` implements "
        "it today."
    )
    add("")
    add("| ID | Name | dir | len | br | Reply | Meaning | Provenance |")
    add("|---|---|---|---|---|---|---|---|")
    for message in spec["messages"]:
        mid = message.get("id")
        add(
            "| %s | `ANTW_%s` | %s | %s | %s | %s | %s | %s |"
            % (
                "`0x%02X`" % mid if mid is not None else "**unresolved**",
                message["name"],
                message["direction"],
                md_cell(message["payload_len"]),
                "yes" if message.get("bridged") else "no",
                "`%s`" % message["reply"] if message.get("reply") else "-",
                md_cell(message_desc(spec, message, "ANTW_")),
                md_cell(provenance(message)),
            )
        )
    add("")

    # ---- request/response pairs --------------------------------------------
    paired = [(m, resolve_reply_len(spec, m)) for m in spec["messages"]
              if m.get("reply_len")]
    add("## Request and response share an id")
    add("")
    add(
        "**A message id is not a message.** `MESG_REQUEST` (`0x4D`) names an "
        "id, and what comes back is a frame carrying that same id with a "
        "payload no host ever sends. For most messages the two shapes happen "
        "to coincide and one `len` describes both. For the messages below they "
        "do not, and reading the `len` column as if it covered the reply is "
        "how a perfectly conformant transcript comes to look inconsistent — "
        "`0x78` takes a 9..12-byte command and answers a request with 4 bytes; "
        "`0x7D` takes a 4-byte command and answers with 2, 5 or 20."
    )
    add("")
    add(
        "The `selector` column is the part worth reading twice, because it is "
        "the difference between a reply a reader can check on its own and one "
        "it can only check in context:"
    )
    add("")
    add(
        "- `reply_byte_0` — the reply echoes the info type it is answering at "
        "byte 0, so it is self-describing."
    )
    add(
        "- `request_index` — byte 0 of the `MESG_REQUEST` picked the shape and "
        "the reply does not repeat it. The shape can only be pinned by pairing "
        "the reply with the request that drew it, which is a fact about the "
        "message and not a limitation of any particular reader."
    )
    add("- `none` — one shape; the reply simply differs from the command.")
    add("")
    add("| ID | Name | Command | Selector | Reply | Length | Size constant |")
    add("|---|---|---|---|---|---|---|")
    for message, reply in paired:
        for index, shape in enumerate(reply["shapes"]):
            add(
                "| %s | %s | %s | %s | %s | %d | `ANTW_%s` |"
                % (
                    "`0x%02X`" % message["id"] if index == 0 else "",
                    "`ANTW_%s`" % message["name"] if index == 0 else "",
                    md_cell(message["payload_len"]) if index == 0 else "",
                    "`%s`" % reply["selector"] if index == 0 else "",
                    "-" if shape["when"] is None else "`0x%02X`" % shape["when"],
                    shape["len"],
                    shape["size"],
                )
            )
    add("")
    add(
        "Every length above is *named*, not written: it resolves to an entry "
        "in **Reply sizes** below, which is where the bridge's own LEN byte is "
        "recorded together with the prose explaining it. One number, one place. "
        "The side effect is worth stating — those constants used to be "
        "witnessed only by the firmware that emits them, and are now checked "
        "against real bytes every time `tools/test_ant_golden.py` replays a "
        "transcript."
    )
    add("")

    add("## Reply sizes")
    add("")
    sizes = spec["message_sizes"]
    add("| Constant | Value | Meaning | Provenance |")
    add("|---|---|---|---|")
    for entry in sizes["values"]:
        add(
            "| `ANTW_%s` | %s | %s | %s |"
            % (
                entry["name"],
                fmt_value(entry.get("value"), entry.get("format", sizes.get("format", "dec"))),
                md_cell(entry.get("desc")),
                md_cell(provenance(entry)),
            )
        )
    add("")

    for group in spec["constant_groups"]:
        add("## %s" % group["title"])
        add("")
        if group.get("desc"):
            add(md_cell(group["desc"]))
            add("")
        add("| Constant | Value | Meaning | Provenance |")
        add("|---|---|---|---|")
        for item in group_entries(group):
            add(
                "| `ANTW_%s` | `%s` | %s | %s |"
                % (
                    item["name"],
                    fmt_value(item.get("value"), item.get("format", "hex")),
                    md_cell(item.get("desc")),
                    md_cell(provenance(item)),
                )
            )
        add("")

    # ---- capabilities ------------------------------------------------------
    caps = spec["capabilities"]
    observed = bytes.fromhex(caps["observed"]["hex"])
    add("## Capabilities reply, decoded")
    add("")
    add(
        "`MESG_CAPABILITIES` (`0x54`) carries %d bytes. %s"
        % (caps["reply_len"], md_cell(caps["observed"]["what"]))
    )
    add("")
    add("```")
    add("A4 %02X 54 %s %02X" % (
        caps["reply_len"],
        " ".join("%02X" % b for b in observed),
        _frame_checksum(bytes([0xA4, caps["reply_len"], 0x54]) + observed),
    ))
    add("^  ^  ^  %s^" % (" " * (3 * len(observed))))
    add("|  |  |  %s`- XOR checksum, SYNC included" % (" " * (3 * len(observed))))
    add("|  |  `- MESG_CAPABILITIES_ID")
    add("|  `- LEN = %d" % caps["reply_len"])
    add("`- SYNC")
    add("```")
    add("")
    add("| Byte | Value | Field | Meaning |")
    add("|---|---|---|---|")
    for byte in caps["bytes"]:
        add(
            "| %d | `0x%02X` | `%s` | %s |"
            % (
                byte["index"],
                observed[byte["index"]],
                byte["field"],
                md_cell(byte.get("desc")) or "-",
            )
        )
    add("")
    add("Bit by bit, for every byte that is a bitfield:")
    add("")
    for byte in caps["bytes"]:
        bits = byte.get("bits") or []
        value = observed[byte["index"]]
        add(
            "**Byte %d, `%s` = `0x%02X` (`0b%s`)**"
            % (byte["index"], byte["field"], value, format(value, "08b"))
        )
        add("")
        if byte["kind"] == "value":
            add("Not a bitfield: the value is %d." % value)
            add("")
            continue
        if not bits:
            add(md_cell(byte.get("desc")))
            add("")
            continue
        add("| Bit | Mask | Name | Set? | Meaning |")
        add("|---|---|---|---|---|")
        for bit in bits:
            mask = bit["value"]
            add(
                "| %d | `0x%02X` | `ANTW_%s` | %s | %s |"
                % (
                    mask.bit_length() - 1,
                    mask,
                    bit["name"],
                    "**yes**" if value & mask else "no",
                    md_cell(bit.get("desc")),
                )
            )
        named = 0
        for bit in bits:
            named |= bit["value"]
        leftover = value & ~named & 0xFF
        if leftover:
            add(
                "| - | `0x%02X` | *(unnamed)* | **yes** | Set in the observed "
                "reply and not named by any constant in this file. |" % leftover
            )
        add("")

    # ---- RadiANT -----------------------------------------------------------
    radiant = spec["radiant_messages"]
    add("## RadiANT extension messages")
    add("")
    add(
        "**These are ours, not Garmin's.** They are not in Rev 5.1, no ANT "
        "device answers them and no ANT host sends them. They are recorded "
        "before anything implements them so the id space is reserved rather "
        "than argued about later, and they are kept in their own table so that "
        "\"an ANT message\" and \"a RadiANT message\" cannot be confused. "
        "`tools/ant_wire.py` exposes them as `RADIANT_MESSAGES`, deliberately "
        "not in `MESSAGES`. Semantics belong to `docs/radiant-security.md`; "
        "only the numbering is decided here."
    )
    add("")
    add(
        "**These ids were first proposed at `0xE0`–`0xE4` and moved.** sdk-ant "
        "defines `MESG_EXT_ID_0` … `MESG_EXT_ID_4` as exactly `0xE0`–`0xE4` — "
        "an extended-message-id block selected by `MSG_EXT_ID_MASK` (`0xE0`) "
        "and reserved for ANT response and request messages that use extended "
        "message ids. Do not re-propose that range."
    )
    add("")
    add(
        "**Lib config `0xE0` (`ANTW_LIB_CONFIG_ALL_EXT_FIELDS` — channel id + "
        "RSSI + RX timestamp) is a third, unrelated namespace, and it has not "
        "moved.** One is the ID field of a frame; the other is a payload byte "
        "of message `0x6E`. Nothing relates them, and nothing ever did — the "
        "byte they used to share was a coincidence, not a connection."
    )
    add("")
    add("| ID | Name | dir | len | Meaning |")
    add("|---|---|---|---|---|")
    for message in radiant["values"]:
        add(
            "| `0x%02X` | `ANTW_%s` | %s | %s | %s |"
            % (
                message["id"],
                message["name"],
                message["direction"],
                md_cell(message["payload_len"]),
                md_cell(message.get("desc")),
            )
        )
    add(
        "| `%s` | *(reserved)* | - | - | %s |"
        % (radiant["reserved_range"], md_cell(radiant["reserved_desc"]))
    )
    add("")
    add("### Non-collision, checked")
    add("")
    add(
        "The first allocation reasoned that nothing at or above `0xC0` appears "
        "in the bridge's dispatch, in any `tools/` definition, or in the "
        "capabilities reply. Every one of those checks was correct, and the "
        "conclusion was still wrong: **this repository is not the whole "
        "protocol.** A message id is a claim about ANT, and the witnesses "
        "below only enumerate what this repo happens to know. That is the "
        "generalisable lesson, and it applies to every future allocation — "
        "the list is necessary, never sufficient."
    )
    add("")
    add("Every witness available was checked:")
    add("")
    bridged_max = max(
        m["id"] for m in spec["messages"] if m.get("id") is not None and m.get("bridged")
    )
    known_max = max(m["id"] for m in spec["messages"] if m.get("id") is not None)
    add(
        "- `src/ant_serial_bridge.c` `dispatch()` and `handle_request()` — "
        "highest id `0x%02X`." % bridged_max
    )
    add(
        "- Every `MESG_*` definition in `tools/ant_wire.py` — highest "
        "`0x%02X`." % known_max
    )
    add(
        "- `archive/host-api/ant_dll_exports.json`, taken from the real PE "
        "export directory of `ANT_DLL.dll` — 36 distinct message ids, highest "
        "`0x7B`. The entire host-callable ANT API tops out there."
    )
    add(
        "- The capabilities reply — carries no id space at all, so nothing in "
        "it can collide."
    )
    add(
        "- Rev 5.1's message table — highest ids known are the encryption block "
        "`0x7D`–`0x7F`, active search sharing `0x81`, NVM crypto key ops around "
        "`0x83`, the serial-layer `0x%02X`–`0xAF` and port-IO `0xB4`–`0xB5` "
        "messages, and an AP2-era extension block around `0xC0`–`0xC7` (RSSI "
        "search threshold, sleep, Garmin ESN, USB info)." % known_max
    )
    add(
        "- **sdk-ant itself**, read by the one agent permitted to open it — "
        "the extended-id block `MESG_EXT_ID_0` … `MESG_EXT_ID_4` at "
        "`0xE0`–`0xE4`, and `MESG_DEBUG_ID` at `0xF0`. Nothing else at or "
        "above `0xC8`, and nothing at all in `0xF1`–`0xFA`. This is the "
        "witness the first allocation could not consult, and it is the one "
        "that moved these ids."
    )
    add("")
    add(
        "**`0xF1`–`0xF5` is therefore the allocation**, one clear of sdk-ant's "
        "`0xF0`, `0x11` clear of the top of the extended-id block and `0x2A` "
        "clear of the top of the AP2-era block. The honest limit is on the "
        "Rev 5.1 bullet: it is recall of the public message table rather than "
        "a citation, which is exactly why the `0xC0` block is stated as a "
        "*range* and why `MESG_RSSI_SEARCH_THRESHOLD_ID` and `MESG_SLEEP_ID` "
        "carry no value in this file. `0xF1`–`0xF5` is also not confusable "
        "with SYNC (`0xA4`/`0xA5`) by a resynchronising parser — which an id "
        "of `0xA4` genuinely would be."
    )
    add("")

    # ---- unmapped host API -------------------------------------------------
    add("## `ANT_DLL` exports with no message id")
    add("")
    add(
        "Host-library entry points that must correspond to *some* serial "
        "message, where neither this repository nor Rev 5.1 names the id. "
        "Nothing is invented for them. Listed so the gap is visible, and so "
        "the `null` `mesg_id` fields in "
        "`archive/host-api/ant_dll_exports.json` read as researched rather "
        "than unfinished. `_RTO` variants are the same call with a response "
        "timeout and are not listed separately."
    )
    add("")
    add("| Export | Note |")
    add("|---|---|")
    for entry in spec["host_api_unmapped"]:
        add("| `%s` | %s |" % (entry["name"], md_cell(entry.get("desc"))))
    add("")

    # ---- unresolved --------------------------------------------------------
    add("## Unresolved constants")
    add("")
    add(
        "These are used at a visible site in this repository but their numeric "
        "value is not recoverable from it, and Rev 5.1 does not name them "
        "either. No macro is emitted for any of them. `src/ant_radio_sdk_ant.c` "
        "is the one translation unit permitted to include both `ant_wire.h` and "
        "sdk-ant's `ant_parameters.h`; it recovers each value and "
        "`BUILD_ASSERT`s it, and the value comes back here afterwards. "
        "Inventing a number would be worse than leaving the hole, because a "
        "wrong dispatch id fails silently and a missing macro fails at compile "
        "time."
    )
    add("")
    add(
        "**The rule for consuming these: an unresolved constant may never "
        "appear in a file that builds without sdk-ant.** `ant_radio_sdk_ant.c` "
        "may `#define` it from sdk-ant, because sdk-ant is present by "
        "construction there. `ant_radio_stub.c` and `radiant/**` may not, "
        "because the whole point of those builds is that sdk-ant is absent. "
        "Where the value never reaches the wire in a direction we originate, a "
        "file-local substitute is correct and the `Blocks` column says so; "
        "where it *is* a byte we transmit, there is no substitute and the "
        "message stays unimplemented until the shim resolves it."
    )
    add("")
    add("| Constant | Section | Blocks | Why it is unresolved | Provenance |")
    add("|---|---|---|---|---|")
    for section, entry in unresolved(spec):
        add(
            "| `%s` | %s | %s | %s | %s |"
            % (
                entry["name"],
                section,
                md_cell(entry.get("blocks")) or "-",
                md_cell(entry.get("desc")),
                md_cell(provenance(entry)),
            )
        )
    add("")

    # ---- provenance --------------------------------------------------------
    constants = all_constants(spec)
    shim = needs_shim(spec)
    add("## Provenance summary")
    add("")
    add(
        "%d constants in total. %d carry a `verify: sdk-ant-shim` flag, meaning "
        "no file in this repository witnesses the value and the Wave 2 shim's "
        "`BUILD_ASSERT` block is what confirms it. %d of those are unresolved "
        "outright."
        % (len(constants), len(shim), len(unresolved(spec)))
    )
    add("")
    counts: dict[str, int] = {}
    for _section, entry in constants:
        key = one_line(entry.get("source", "")) or "-"
        counts[key] = counts.get(key, 0) + 1
    add("| Source | Constants |")
    add("|---|---|")
    for key in sorted(counts):
        add("| %s | %d |" % (md_cell(key), counts[key]))
    add("")
    add(DOC_END)
    return "\n".join(out) + "\n"


def _frame_checksum(data: bytes) -> int:
    total = 0
    for byte in data:
        total ^= byte
    return total


def splice_doc(existing: str, region: str) -> str:
    if DOC_BEGIN not in existing or DOC_END not in existing:
        raise SystemExit(
            "%s is missing the generated-region markers:\n  %s\n  %s"
            % (DOC_PATH, DOC_BEGIN, DOC_END)
        )
    head = existing.split(DOC_BEGIN)[0]
    tail = existing.split(DOC_END, 1)[1]
    return head + region.rstrip("\n") + tail


# ---------------------------------------------------------------------------
# drive
# ---------------------------------------------------------------------------


def read_text(path: Path) -> str:
    if not path.exists():
        return ""
    with open(path, "r", encoding="utf-8", newline="") as handle:
        return handle.read()


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "--check",
        action="store_true",
        help="render into memory, diff against what is committed, exit 1 on drift",
    )
    args = parser.parse_args()

    spec = load_spec()
    wanted = {
        RADIANT_WIRE_HEADER_PATH: render_wire_header(
            spec,
            prefix_name="RADIANT_WIRE",
            guard="RADIANT_RADIANT_WIRE_H_",
            title_lines=[
                "ANT serial protocol wire constants, generated for radiant's",
                "own internal use - see src/ant_wire.h for the application-facing",
                "ANTW_* names these values are aliased under.",
                "Source of truth: protocol/ant_wire.yaml",
                "Naming authority: " + spec["meta"]["spec"],
                "Human-readable companion: docs/ant-serial-protocol.md",
            ],
            rationale=MODULE_RATIONALE,
            alias_of=None,
        ),
        HEADER_PATH: render_wire_header(
            spec,
            prefix_name=spec["meta"]["prefix"],
            guard="ANT_WIRE_H_",
            title_lines=[
                "ANT serial protocol constants (host <-> dongle, 0xA4-framed).",
                "Source of truth: protocol/ant_wire.yaml",
                "Naming authority: " + spec["meta"]["spec"],
                "Human-readable companion: docs/ant-serial-protocol.md",
            ],
            rationale=PREFIX_RATIONALE,
            alias_of="RADIANT_WIRE",
        ),
        PYTHON_PATH: render_python(spec),
        DOC_PATH: splice_doc(read_text(DOC_PATH), render_doc_region(spec)),
    }

    # EVERY OUTPUT MUST ALREADY EXIST, and this refuses rather than creating
    # one. All four are committed files with committed readers; a path that no
    # longer resolves means the tree moved under this script, not that a new
    # file is wanted there. Writing one anyway is what let HEADER_PATH point at
    # a stale src/ for months while --check compared the stray against itself
    # and reported "in sync" - see the note on HEADER_PATH.
    #
    # This runs before both arms deliberately: --check has to fail on a missing
    # output too, or CI is exactly where the silence would survive.
    missing = [p.relative_to(ROOT).as_posix() for p in wanted if not p.exists()]
    if missing:
        print(
            "generated output(s) missing: %s\n"
            "This script does not create them. Either the path constants at the\n"
            "top of this file are stale after a move, or the file was deleted;\n"
            "fix the path rather than letting a new file appear beside the real\n"
            "one, which is a check that passes against itself." % ", ".join(missing)
        )
        return 1

    if not args.check:
        for path, text in wanted.items():
            write_text(path, text)
            print("wrote %s" % path.relative_to(ROOT).as_posix())
        return 0

    drifted = 0
    for path, text in wanted.items():
        current = read_text(path)
        if current == text:
            continue
        drifted += 1
        rel = path.relative_to(ROOT).as_posix()
        sys.stdout.writelines(
            difflib.unified_diff(
                current.splitlines(keepends=True),
                text.splitlines(keepends=True),
                fromfile="%s (committed)" % rel,
                tofile="%s (regenerated)" % rel,
            )
        )
        print()

    if drifted:
        print(
            "%d generated file(s) differ from protocol/ant_wire.yaml.\n"
            "Run: python scripts/gen_ant_wire.py" % drifted
        )
        return 1

    print("ant_wire: %s in sync with protocol/ant_wire.yaml" % ", ".join(
        p.relative_to(ROOT).as_posix() for p in wanted
    ))
    return 0


if __name__ == "__main__":
    sys.exit(main())
