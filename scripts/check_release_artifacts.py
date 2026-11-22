# SPDX-License-Identifier: Apache-2.0
"""Check the shipping images handed to release.yml's `publish` job, and stage
exactly those files for the release.

The expected names come from one place: the `build-core` matrix rows in
.github/workflows/release.yml that carry `release: true`, read from the same
checkout the run was started from. Each such row uploads one artifact named
`release-<artifact>` holding `<artifact>` and `<artifact>.sha256`. This
script requires that, for every row and for nothing else:

  - the downloads directory holds exactly one `release-<artifact>` directory
    per release row, and no other entry;
  - each of those holds exactly `<artifact>` and `<artifact>.sha256`, as
    regular files, and nothing else;
  - the sidecar is one sha256sum line naming `<artifact>` by its bare name,
    and the image matches it.

Then it copies those files, and only those, into the output directory and
(with --github-output) publishes the list of paths for the release action.

Why the names and not only a count (security review 2026-11-21): the old
check merged every `release-*` artifact into one directory and counted the
sidecars. A differently named image with a matching sidecar could stand in
for a required board image and pass every check.

Standard library only: the publish job runs it on a bare setup-python, with
nothing installed, so the matrix is read with a narrow line parser rather
than PyYAML. The parser is cross-checked against a plain count of
`release: true` lines in the whole file, so a layout it does not understand
fails instead of shrinking the expected set.
"""

import argparse
import hashlib
import os
import re
import secrets
import shutil
import sys
from pathlib import Path

JOB = "build-core"
PREFIX = "release-"
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
SUM_RE = re.compile(r"^([0-9a-f]{64}) [ *](.+)$")


class CheckError(Exception):
    pass


def _value(raw):
    v = raw.strip()
    if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
        v = v[1:-1]
    return v


def release_artifacts(workflow_text):
    """The `artifact` of every build-core matrix row with `release: true`,
    in file order."""
    lines = workflow_text.splitlines()
    job_re = re.compile(r"^  ([A-Za-z0-9_-]+):\s*$")
    start = None
    end = len(lines)
    for i, line in enumerate(lines):
        m = job_re.match(line)
        if not m:
            continue
        if start is None and m.group(1) == JOB:
            start = i
        elif start is not None:
            end = i
            break
    if start is None:
        raise CheckError(f"no `{JOB}` job in the workflow")

    body = lines[start:end]
    try:
        inc = next(i for i, l in enumerate(body) if re.match(r"^\s+include:\s*$", l))
    except StopIteration:
        raise CheckError(f"`{JOB}` has no matrix include list") from None
    base = len(body[inc]) - len(body[inc].lstrip())

    rows = []
    row = None
    row_indent = None
    key_indent = None
    for line in body[inc + 1:]:
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        indent = len(line) - len(line.lstrip())
        if indent <= base:
            break  # left the include list
        if stripped.startswith("- "):
            if row_indent is None:
                row_indent = indent
                key_indent = indent + 2
            if indent == row_indent:
                row = {}
                rows.append(row)
                stripped = stripped[2:]
                indent = key_indent
        if row is None or indent != key_indent:
            continue  # nested value (a block scalar, a list) - not ours
        m = re.match(r"^([A-Za-z0-9_]+):(.*)$", stripped)
        if m:
            row[m.group(1)] = _value(m.group(2))

    names = []
    for r in rows:
        if "release" not in r:
            continue
        if r["release"] != "true":
            raise CheckError(f"row {r.get('name', '?')!r}: release must be "
                             f"`true` or absent, got {r['release']!r}")
        art = r.get("artifact", "")
        if not art:
            raise CheckError(f"release row {r.get('name', '?')!r} has no artifact")
        if not NAME_RE.match(art) or art.endswith(".sha256"):
            raise CheckError(f"release artifact name {art!r} is not a plain file name")
        if art in names:
            raise CheckError(f"release artifact {art!r} appears twice")
        names.append(art)

    flat = len(re.findall(r"^ +release: true\s*$", workflow_text, re.M))
    if flat != len(names):
        raise CheckError(f"{flat} `release: true` line(s) in the file but "
                         f"{len(names)} release row(s) parsed from `{JOB}`")
    if not names:
        raise CheckError("no release rows found")
    return names


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def verify(names, downloads):
    """Check the downloads directory against the expected names. Returns the
    (image, sidecar) paths in the order of `names`."""
    downloads = Path(downloads)
    want = {PREFIX + n: n for n in names}
    have = {e.name: e for e in os.scandir(downloads)}
    errors = []
    for d in sorted(set(have) - set(want)):
        errors.append(f"unexpected entry in the downloads: {d}")
    for d in sorted(set(want) - set(have)):
        errors.append(f"missing artifact: {d}")
    out = []
    for d, n in want.items():
        e = have.get(d)
        if e is None:
            continue
        if e.is_symlink() or not e.is_dir():
            errors.append(f"{d} is not a directory")
            continue
        files = {f.name: f for f in os.scandir(e.path)}
        expect = {n, n + ".sha256"}
        if set(files) != expect:
            extra = sorted(set(files) - expect)
            missing = sorted(expect - set(files))
            errors.append(f"{d} must hold exactly {sorted(expect)}; "
                          f"extra {extra}, missing {missing}")
            continue
        bad = [f.name for f in files.values()
               if f.is_symlink() or not f.is_file(follow_symlinks=False)]
        if bad:
            errors.append(f"{d}: not regular files: {sorted(bad)}")
            continue
        img = Path(files[n].path)
        side = Path(files[n + ".sha256"].path)
        text = side.read_bytes().decode("ascii", "replace")
        lines = text.split("\n")
        if lines and lines[-1] == "":
            lines.pop()
        if len(lines) != 1:
            errors.append(f"{side.name}: want one line, got {len(lines)}")
            continue
        m = SUM_RE.match(lines[0])
        if not m:
            errors.append(f"{side.name}: not a sha256sum line: {lines[0]!r}")
            continue
        if m.group(2) != n:
            errors.append(f"{side.name}: names {m.group(2)!r}, not {n!r}")
            continue
        got = _sha256(img)
        if got != m.group(1):
            errors.append(f"{n}: sha256 {got} does not match its sidecar {m.group(1)}")
            continue
        print(f"OK {n} {got}")
        out.append((img, side))
    if errors:
        raise CheckError("; ".join(errors))
    return out


def stage(pairs, out_dir):
    out_dir = Path(out_dir)
    if out_dir.exists() and any(out_dir.iterdir()):
        raise CheckError(f"{out_dir} is not empty")
    out_dir.mkdir(parents=True, exist_ok=True)
    staged = []
    for img, side in pairs:
        for src in (img, side):
            dst = out_dir / src.name
            shutil.copyfile(src, dst)
            staged.append(dst)
    return staged


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--workflow", required=True, help="release.yml to read the matrix from")
    ap.add_argument("--downloads", required=True,
                    help="directory download-artifact wrote the release-* artifacts into, "
                         "one subdirectory each (merge-multiple off)")
    ap.add_argument("--out", required=True, help="empty directory to stage the files in")
    ap.add_argument("--github-output", help="append `files=<list>` here (GITHUB_OUTPUT)")
    args = ap.parse_args(argv)
    try:
        names = release_artifacts(Path(args.workflow).read_text(encoding="utf-8"))
        print(f"release rows: {len(names)}: {' '.join(names)}")
        staged = stage(verify(names, args.downloads), args.out)
    except (CheckError, OSError) as e:
        print(f"::error::{e}")
        return 1
    if args.github_output:
        delim = "EOF_" + secrets.token_hex(8)
        with open(args.github_output, "a", encoding="utf-8", newline="\n") as f:
            f.write(f"files<<{delim}\n")
            for p in staged:
                f.write(p.as_posix() + "\n")
            f.write(f"{delim}\n")
    for p in staged:
        print(f"stage {p.as_posix()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
