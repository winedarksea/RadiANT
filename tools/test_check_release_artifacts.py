# SPDX-License-Identifier: Apache-2.0

"""Tests for scripts/check_release_artifacts.py, the publish job's check.

The 2026-11-21 security review's case is the one that matters: a release-*
artifact under a different name, with a matching sidecar, used to stand in
for a required board image and pass a count-based check. Here that case, an
extra file, a missing row and a bad sum each run against fake download
directories laid out the way download-artifact (merge-multiple off) writes
them, and the real release.yml is parsed so the expected set tracks the
matrix rather than a list in this file.

Here rather than beside the script because host-tests runs
`python -m unittest discover -s tools -p "test_*.py"`.
"""

import hashlib
import io
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))

import check_release_artifacts as cra  # noqa: E402

WORKFLOW = REPO / ".github" / "workflows" / "release.yml"

MINI = """\
name: x
jobs:
  build-core:
    strategy:
      matrix:
        include:
          - name: "a"
            artifact: a.uf2
            release: true
            assert_config: |
              # release: true inside a block scalar is not a key
              CONFIG_X=y
          - name: "b, not shipped"
            artifact: b.uf2
          - name: "c"
            board: foo
            artifact: "c.zip"
            release: true
    steps:
      - run: true
  publish:
    steps:
      - run: true
"""


def sum_line(name, data):
    return f"{hashlib.sha256(data).hexdigest()}  {name}\n"


def make_artifact(root, name, data=None, files=None):
    """One download-artifact directory: release-<name>/{name, name.sha256}."""
    data = b"image " + name.encode() if data is None else data
    d = Path(root) / (cra.PREFIX + name)
    d.mkdir(parents=True)
    if files is None:
        files = {name: data, name + ".sha256": sum_line(name, data).encode()}
    for fn, content in files.items():
        (d / fn).write_bytes(content)
    return d


class ParseTest(unittest.TestCase):
    def test_real_workflow_has_the_five_shipping_rows(self):
        names = cra.release_artifacts(WORKFLOW.read_text(encoding="utf-8"))
        self.assertEqual(names, [
            "radiant_dongle.uf2",
            "radiant_dongle_feather_quiet.uf2",
            "radiant_dongle_nrf52840dongle.zip",
            "radiant_dongle_promicro.uf2",
            "radiant_dongle_promicro_synth.uf2",
        ])

    def test_mini(self):
        self.assertEqual(cra.release_artifacts(MINI), ["a.uf2", "c.zip"])

    def test_release_row_without_artifact(self):
        bad = MINI.replace('            artifact: "c.zip"\n', "")
        with self.assertRaisesRegex(cra.CheckError, "has no artifact"):
            cra.release_artifacts(bad)

    def test_release_not_true(self):
        bad = MINI.replace("            artifact: a.uf2\n            release: true",
                           "            artifact: a.uf2\n            release: yes")
        with self.assertRaisesRegex(cra.CheckError, "release must be"):
            cra.release_artifacts(bad)

    def test_duplicate_name(self):
        bad = MINI.replace('"c.zip"', "a.uf2")
        with self.assertRaisesRegex(cra.CheckError, "appears twice"):
            cra.release_artifacts(bad)

    def test_path_in_name(self):
        bad = MINI.replace('"c.zip"', "../c.zip")
        with self.assertRaisesRegex(cra.CheckError, "plain file name"):
            cra.release_artifacts(bad)

    def test_release_row_outside_build_core_fails_the_cross_check(self):
        bad = MINI.replace("  publish:\n",
                           "  other:\n    strategy:\n      matrix:\n        include:\n"
                           "          - artifact: d.uf2\n            release: true\n"
                           "  publish:\n")
        with self.assertRaisesRegex(cra.CheckError, "3 `release: true` line"):
            cra.release_artifacts(bad)

    def test_layout_the_parser_misses_fails_rather_than_shrinking(self):
        # A row written flow-style is valid YAML the line parser does not
        # read; the whole-file count catches it.
        bad = MINI.replace("          - name: \"b, not shipped\"\n            artifact: b.uf2\n",
                           "          - {name: b, artifact: b.uf2,\n"
                           "            release: true}\n")
        with self.assertRaises(cra.CheckError):
            cra.release_artifacts(bad)


class VerifyTest(unittest.TestCase):
    NAMES = ["a.uf2", "c.zip"]

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.dl = self.tmp / "dist"
        self.dl.mkdir()
        self.out = self.tmp / "publish"

    def tearDown(self):
        self._tmp.cleanup()

    def run_check(self, names=None):
        with redirect_stdout(io.StringIO()):
            return cra.stage(cra.verify(names or self.NAMES, self.dl), self.out)

    def good(self):
        for n in self.NAMES:
            make_artifact(self.dl, n)

    def test_good(self):
        self.good()
        staged = self.run_check()
        self.assertEqual(sorted(p.name for p in self.out.iterdir()),
                         ["a.uf2", "a.uf2.sha256", "c.zip", "c.zip.sha256"])
        self.assertEqual([p.name for p in staged],
                         ["a.uf2", "a.uf2.sha256", "c.zip", "c.zip.sha256"])

    def test_binary_mode_sidecar_accepted(self):
        make_artifact(self.dl, "a.uf2")
        data = b"zip"
        make_artifact(self.dl, "c.zip", files={
            "c.zip": data,
            "c.zip.sha256": f"{hashlib.sha256(data).hexdigest()} *c.zip\n".encode()})
        self.run_check()

    def test_renamed_artifact_substitution(self):
        # The review's case: a required image replaced by a differently
        # named one with a perfectly good sidecar. Count is right, sums pass.
        make_artifact(self.dl, "a.uf2")
        make_artifact(self.dl, "evil.zip")
        with self.assertRaisesRegex(cra.CheckError,
                                    "unexpected entry.*release-evil.zip.*missing artifact: release-c.zip"):
            self.run_check()
        self.assertFalse(self.out.exists())

    def test_renamed_image_inside_the_right_artifact(self):
        make_artifact(self.dl, "a.uf2")
        data = b"evil"
        make_artifact(self.dl, "c.zip", files={
            "evil.zip": data, "evil.zip.sha256": sum_line("evil.zip", data).encode()})
        with self.assertRaisesRegex(cra.CheckError, "release-c.zip must hold exactly"):
            self.run_check()

    def test_extra_file_in_an_artifact(self):
        self.good()
        (self.dl / "release-a.uf2" / "README.txt").write_bytes(b"hi")
        with self.assertRaisesRegex(cra.CheckError, r"extra \['README.txt'\]"):
            self.run_check()

    def test_extra_artifact(self):
        self.good()
        make_artifact(self.dl, "sixth.uf2")
        with self.assertRaisesRegex(cra.CheckError, "unexpected entry.*release-sixth.uf2"):
            self.run_check()

    def test_stray_file_at_the_top(self):
        self.good()
        (self.dl / "loose.uf2").write_bytes(b"x")
        with self.assertRaisesRegex(cra.CheckError, "unexpected entry.*loose.uf2"):
            self.run_check()

    def test_missing_row(self):
        make_artifact(self.dl, "a.uf2")
        with self.assertRaisesRegex(cra.CheckError, "missing artifact: release-c.zip"):
            self.run_check()

    def test_missing_sidecar(self):
        make_artifact(self.dl, "a.uf2")
        make_artifact(self.dl, "c.zip", files={"c.zip": b"x"})
        with self.assertRaisesRegex(cra.CheckError, r"missing \['c.zip.sha256'\]"):
            self.run_check()

    def test_bad_sum(self):
        make_artifact(self.dl, "a.uf2")
        make_artifact(self.dl, "c.zip", files={
            "c.zip": b"tampered", "c.zip.sha256": sum_line("c.zip", b"orig").encode()})
        with self.assertRaisesRegex(cra.CheckError, "does not match its sidecar"):
            self.run_check()

    def test_sidecar_naming_another_file(self):
        make_artifact(self.dl, "a.uf2")
        make_artifact(self.dl, "c.zip", files={
            "c.zip": b"x", "c.zip.sha256": sum_line("a.uf2", b"x").encode()})
        with self.assertRaisesRegex(cra.CheckError, "names 'a.uf2'"):
            self.run_check()

    def test_sidecar_with_two_lines(self):
        make_artifact(self.dl, "a.uf2")
        make_artifact(self.dl, "c.zip", files={
            "c.zip": b"x",
            "c.zip.sha256": (sum_line("c.zip", b"x") + sum_line("d.zip", b"y")).encode()})
        with self.assertRaisesRegex(cra.CheckError, "want one line, got 2"):
            self.run_check()

    def test_subdirectory_in_an_artifact(self):
        self.good()
        (self.dl / "release-a.uf2" / "nested").mkdir()
        with self.assertRaisesRegex(cra.CheckError, "nested"):
            self.run_check()

    def test_out_dir_must_be_empty(self):
        self.good()
        self.out.mkdir()
        (self.out / "old.uf2").write_bytes(b"x")
        with self.assertRaisesRegex(cra.CheckError, "not empty"):
            self.run_check()


class MainTest(unittest.TestCase):
    def test_end_to_end_against_the_real_workflow(self):
        names = cra.release_artifacts(WORKFLOW.read_text(encoding="utf-8"))
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            for n in names:
                make_artifact(t / "dist", n)
            gho = t / "gh_output"
            buf = io.StringIO()
            with redirect_stdout(buf):
                rc = cra.main(["--workflow", str(WORKFLOW), "--downloads", str(t / "dist"),
                               "--out", str(t / "publish"), "--github-output", str(gho)])
            self.assertEqual(rc, 0, buf.getvalue())
            lines = gho.read_text(encoding="utf-8").splitlines()
            self.assertTrue(lines[0].startswith("files<<EOF_"))
            self.assertEqual(lines[-1], lines[0][len("files<<"):])
            listed = [Path(p).name for p in lines[1:-1]]
            self.assertEqual(listed, [x for n in names for x in (n, n + ".sha256")])

            # And the same tree with one image renamed fails, writing nothing.
            (t / "dist" / ("release-" + names[0])).rename(t / "dist" / "release-other.uf2")
            gho.unlink()
            with redirect_stdout(io.StringIO()):
                rc = cra.main(["--workflow", str(WORKFLOW), "--downloads", str(t / "dist"),
                               "--out", str(t / "publish2"), "--github-output", str(gho)])
            self.assertEqual(rc, 1)
            self.assertFalse(gho.exists())


if __name__ == "__main__":
    unittest.main()
