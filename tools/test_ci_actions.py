# SPDX-License-Identifier: Apache-2.0

"""Run the composite actions' shell bodies against fixtures.

WHY THIS EXISTS. Four copies of a .config read-back loop lived in build.yml,
they drifted, and the drift was invisible: a fix landed on one assertion line
while five others kept a form that `grep -qxF` can NEVER match, so those five
were not weak checks - they could only ever fail. They did, for months. The loop
is now one composite action, and this is what keeps the guard honest.

The bodies are extracted from the YAML and run under bash directly, so what is
tested is the same text the runner executes. There is no yaml dependency: the
host-tests job installs only pyusb, and a check that needs a package the job
does not install is a check that gets deleted.

On Windows there is no system bash; the NCS toolchain ships one and this finds
it. If no bash is found at all the tests skip rather than fail - a skipped test
says so, whereas a green test on a machine that ran nothing does not.
"""

import os
import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ACTIONS = REPO / ".github" / "actions"


def find_bash():
    found = shutil.which("bash")
    if found and os.name != "nt":
        return found
    # C:\Windows\System32\bash.exe is the WSL launcher, and this machine has no
    # distribution installed - it exits non-zero with a wall of UTF-16. The NCS
    # toolchain's MSYS2 bash is a real one.
    for root in (Path("C:/ncs/toolchains"),):
        if root.is_dir():
            for candidate in sorted(root.glob("*/bin/bash.exe")):
                return str(candidate)
    return found


BASH = find_bash()


def extract_run(action: Path) -> str:
    """The `run: |` body of the action's single composite step, dedented.

    Hand-rolled rather than via PyYAML on purpose - see the module docstring.
    """
    lines = action.read_text(encoding="utf-8").splitlines()
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped in ("run: |", "run: |-"):
            indent = len(line) - len(line.lstrip())
            body = []
            for rest in lines[i + 1:]:
                if rest.strip() and (len(rest) - len(rest.lstrip())) <= indent:
                    break
                body.append(rest)
            return textwrap.dedent("\n".join(body))
    raise AssertionError(f"no `run: |` block in {action}")


KCONFIG_BODY = extract_run(ACTIONS / "assert-kconfig" / "action.yml")
SYMBOLS_BODY = extract_run(ACTIONS / "assert-symbols" / "action.yml")


@unittest.skipIf(BASH is None, "no bash available to run the action bodies")
class ActionHarness(unittest.TestCase):
    def run_body(self, body, env, cwd):
        script = Path(cwd) / "_body.sh"
        script.write_text(body, encoding="utf-8", newline="\n")
        full = dict(os.environ)
        full.update({k: str(v) for k, v in env.items()})
        proc = subprocess.run(
            [BASH, "./_body.sh"], cwd=cwd, env=full,
            capture_output=True, text=True)
        return proc.returncode, proc.stdout + proc.stderr


class AssertKconfig(ActionHarness):
    CONFIG = "\n".join([
        "CONFIG_RADIANT_BACKEND_NRF=y",
        "CONFIG_HRM_BLE_HRS=y",
        "# CONFIG_BT is not set",
        "CONFIG_MPSL_TIMESLOT_SESSION_COUNT=1",
    ]) + "\n"

    def check(self, expect, config=None, **env):
        with tempfile.TemporaryDirectory() as tmp:
            cfg_dir = Path(tmp) / "build" / "hrm_ble" / "zephyr"
            cfg_dir.mkdir(parents=True)
            (cfg_dir / ".config").write_text(
                self.CONFIG if config is None else config,
                encoding="utf-8", newline="\n")
            base = {"BUILD_DIR": "build", "PATH_FILTER": "", "APP": "",
                    "WANT": expect}
            base.update(env)
            return self.run_body(KCONFIG_BODY, base, tmp)

    def test_a_symbol_that_is_on_passes(self):
        rc, out = self.check("CONFIG_RADIANT_BACKEND_NRF=y")
        self.assertEqual(rc, 0, out)
        self.assertIn("ok: CONFIG_RADIANT_BACKEND_NRF=y", out)

    def test_kconfigs_negative_form_passes_with_its_leading_hash(self):
        rc, out = self.check("# CONFIG_BT is not set")
        self.assertEqual(rc, 0, out)

    def test_the_same_assertion_without_the_hash_is_named_as_malformed(self):
        """THE BUG CLASS. Five of these were live in build.yml at once."""
        rc, out = self.check("CONFIG_BT is not set")
        self.assertEqual(rc, 1)
        self.assertIn("malformed expectation", out)
        # Not reported as an ordinary mismatch - that reads like a real
        # configuration problem and is what made these survive review.
        self.assertNotIn("actual:", out)

    def test_a_rationale_comment_is_skipped_not_asserted(self):
        rc, out = self.check(
            "# why this row exists at all\nCONFIG_HRM_BLE_HRS=y")
        self.assertEqual(rc, 0, out)

    def test_absent_passes_when_the_symbol_is_missing_entirely(self):
        rc, out = self.check("absent:CONFIG_HRM_SENSOR_SIM_BPM")
        self.assertEqual(rc, 0, out)
        self.assertIn("absent", out)

    def test_absent_fails_when_the_symbol_is_merely_off(self):
        """`# CONFIG_BT is not set` present means BT is NOT absent."""
        rc, out = self.check("absent:CONFIG_BT")
        self.assertEqual(rc, 1)
        self.assertIn("to be absent", out)

    def test_a_missing_symbol_fails_and_says_so(self):
        rc, out = self.check("CONFIG_NOT_A_REAL_SYMBOL=y")
        self.assertEqual(rc, 1)
        self.assertIn("symbol absent entirely", out)

    def test_a_whole_line_compare_not_a_prefix(self):
        """`CONFIG_X=y` must not be satisfied by `CONFIG_X=yes`."""
        rc, out = self.check("CONFIG_MPSL_TIMESLOT_SESSION_COUNT=1",
                             config="CONFIG_MPSL_TIMESLOT_SESSION_COUNT=16\n")
        self.assertEqual(rc, 1, out)

    def test_the_app_basename_becomes_the_path_filter(self):
        rc, out = self.check("CONFIG_HRM_BLE_HRS=y", APP="apps/hrm_ble")
        self.assertEqual(rc, 0, out)
        self.assertIn("hrm_ble", out)

    def test_no_config_at_all_is_a_named_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "build").mkdir()
            rc, out = self.run_body(
                KCONFIG_BODY,
                {"BUILD_DIR": "build", "PATH_FILTER": "", "APP": "",
                 "WANT": "CONFIG_X=y"}, tmp)
        self.assertEqual(rc, 1)
        self.assertIn("no .config", out)

    def test_every_failing_expectation_is_reported_not_just_the_first(self):
        rc, out = self.check("CONFIG_NOPE_ONE=y\nCONFIG_NOPE_TWO=y")
        self.assertEqual(rc, 1)
        self.assertIn("CONFIG_NOPE_ONE", out)
        self.assertIn("CONFIG_NOPE_TWO", out)


class AssertSymbols(ActionHarness):
    """Driven against a stub `nm`, so the section arithmetic is testable."""

    NM_OUTPUT = "\n".join([
        "20000100 D _radiant_sink_list_start",
        "20000100 D radiant_liveness_sink",
        "20000120 D radiant_rules_sink",
        "20000140 D _radiant_sink_list_end",
        "00001234 T radiant_matter_convert_with",
        "00002000 T radiant_bridge_pump",
    ]) + "\n"

    def setup_tree(self, tmp, nm_output=None):
        elf_dir = Path(tmp) / "build" / "zephyr"
        elf_dir.mkdir(parents=True)
        (elf_dir / "zephyr.elf").write_text("not really an elf",
                                            encoding="utf-8")
        nm = Path(tmp) / "fake_nm"
        # Ignores its arguments, including --defined-only. Every call site in
        # the action wants the same table.
        nm.write_text(
            "#!/bin/sh\ncat <<'EOF'\n"
            + (self.NM_OUTPUT if nm_output is None else nm_output)
            + "EOF\n", encoding="utf-8", newline="\n")
        nm.chmod(0o755)
        return "./fake_nm"

    def check(self, tmp, **env):
        nm = self.setup_tree(tmp, env.pop("nm_output", None))
        base = {"NM": nm, "BUILD_DIR": "build", "ELF_IN": "",
                "WANT_DEFINED": "", "SINK_SECTION": "false", "WANT_SINKS": ""}
        base.update(env)
        return self.run_body(SYMBOLS_BODY, base, tmp)

    def test_a_missing_tool_is_a_toolchain_error_not_a_missing_symbol(self):
        """The exact misdiagnosis this action was written to end.

        The predecessor called a bare `arm-zephyr-eabi-nm`, which is not on
        PATH under action-zephyr-setup, and reported `command not found` as
        `_radiant_sink_list_start absent - the linker fragment was not added`.
        """
        with tempfile.TemporaryDirectory() as tmp:
            self.setup_tree(tmp)
            rc, out = self.run_body(
                SYMBOLS_BODY,
                {"NM": "", "BUILD_DIR": "build", "ELF_IN": "",
                 "WANT_DEFINED": "", "SINK_SECTION": "true",
                 "WANT_SINKS": ""}, tmp)
        self.assertEqual(rc, 1)
        self.assertIn("TOOLCHAIN fault", out)
        self.assertNotIn("linker fragment", out)

    def test_a_surviving_symbol_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            rc, out = self.check(tmp, WANT_DEFINED="radiant_bridge_pump")
        self.assertEqual(rc, 0, out)

    def test_a_near_miss_does_not_satisfy_a_symbol(self):
        """`radiant_matter_convert` must not be met by `..._convert_with`."""
        with tempfile.TemporaryDirectory() as tmp:
            rc, out = self.check(tmp, WANT_DEFINED="radiant_matter_convert")
        self.assertEqual(rc, 1)
        self.assertIn("--gc-sections discarded it", out)

    def test_the_sink_section_span_and_exact_set(self):
        with tempfile.TemporaryDirectory() as tmp:
            rc, out = self.check(
                tmp, SINK_SECTION="true",
                WANT_SINKS="radiant_liveness_sink\nradiant_rules_sink")
        self.assertEqual(rc, 0, out)
        self.assertIn("sink list spans 64 bytes", out)

    def test_the_sink_set_is_exact_not_a_subset(self):
        with tempfile.TemporaryDirectory() as tmp:
            rc, out = self.check(tmp, SINK_SECTION="true",
                                 WANT_SINKS="radiant_liveness_sink")
        self.assertEqual(rc, 1)
        self.assertIn("not the one this row expects", out)

    def test_a_missing_boundary_symbol_blames_the_linker_fragment(self):
        with tempfile.TemporaryDirectory() as tmp:
            rc, out = self.check(
                tmp, SINK_SECTION="true",
                nm_output="00002000 T radiant_bridge_pump\n")
        self.assertEqual(rc, 1)
        self.assertIn("linker fragment", out)

    def test_an_empty_section_is_caught(self):
        with tempfile.TemporaryDirectory() as tmp:
            rc, out = self.check(
                tmp, SINK_SECTION="true",
                nm_output="20000100 D _radiant_sink_list_start\n"
                          "20000100 D _radiant_sink_list_end\n")
        self.assertEqual(rc, 1)
        self.assertIn("iterate zero sinks", out)


class TheWorkflowsThemselvesAreWellFormed(unittest.TestCase):
    """A lint, and it is the half the harness above cannot cover.

    The release lane no longer runs on pull requests, so a malformed assertion
    added there would not be caught until somebody cut a tag - which is exactly
    how the five that broke CI survived. This runs in the FAST lane and reads
    the workflow text, so the mistake is caught at review time instead.

    No bash and no yaml: it is a text scan of the `assert_*: |` block scalars.
    """

    WORKFLOWS = REPO / ".github" / "workflows"

    def block_scalar_lines(self, path):
        """Yield (lineno, text) for every line inside an `assert_*: |` block."""
        lines = path.read_text(encoding="utf-8").splitlines()
        indent = None
        for n, line in enumerate(lines, 1):
            if indent is not None:
                if not line.strip():
                    continue
                if (len(line) - len(line.lstrip())) > indent:
                    yield n, line
                    continue
                indent = None
            stripped = line.strip()
            if stripped.startswith("assert_") and stripped.endswith(": |"):
                indent = len(line) - len(line.lstrip())

    def test_no_assertion_omits_kconfigs_leading_hash(self):
        bad_form = re.compile(r"^\s*CONFIG_[A-Za-z0-9_]+\s+is\s+not\s+set\s*$")
        offenders = []
        for wf in sorted(self.WORKFLOWS.glob("*.yml")):
            for n, line in self.block_scalar_lines(wf):
                if bad_form.match(line):
                    offenders.append(f"{wf.name}:{n}: {line.strip()}")
        self.assertEqual(
            offenders, [],
            "Kconfig writes '# CONFIG_X is not set'. Without the leading '# ' "
            "these can never match and the assertion only ever fails:\n  "
            + "\n  ".join(offenders))

    def test_every_local_action_reference_resolves(self):
        """`uses: ./app/...` is a path on disk; a typo is a run-time failure."""
        ref = re.compile(r"uses:\s*(\./\S+)")
        missing = []
        for wf in sorted(self.WORKFLOWS.glob("*.yml")):
            for n, line in enumerate(
                    wf.read_text(encoding="utf-8").splitlines(), 1):
                m = ref.search(line)
                if not m:
                    continue
                # Every job in these workflows checks this repository out into
                # app/, and a local action path resolves against the workspace
                # root rather than against the repository.
                rel = m.group(1).removeprefix("./app/").removeprefix("./")
                if not (REPO / rel / "action.yml").is_file():
                    missing.append(f"{wf.name}:{n}: {m.group(1)}")
        self.assertEqual(missing, [], f"unresolvable local actions: {missing}")

    def test_every_job_has_a_timeout(self):
        """A hung job with no ceiling burns a runner for six hours."""
        job = re.compile(r"^  ([A-Za-z0-9_-]+):\s*$")
        for wf in sorted(self.WORKFLOWS.glob("*.yml")):
            text = wf.read_text(encoding="utf-8")
            in_jobs = False
            current = None
            seen = {}
            for line in text.splitlines():
                if line.startswith("jobs:"):
                    in_jobs = True
                    continue
                if not in_jobs:
                    continue
                m = job.match(line)
                if m:
                    current = m.group(1)
                    seen.setdefault(current, False)
                elif current and line.strip().startswith("timeout-minutes:"):
                    seen[current] = True
            missing = [k for k, v in seen.items() if not v]
            self.assertEqual(missing, [],
                             f"{wf.name}: jobs with no timeout-minutes: {missing}")


if __name__ == "__main__":
    unittest.main()
