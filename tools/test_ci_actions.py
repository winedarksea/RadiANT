# SPDX-License-Identifier: Apache-2.0

"""Run the composite actions' shell bodies against fixtures.

Guards against silent drift between copies of a check: a form that
`grep -qxF` can NEVER match is not a weak check, it can only ever fail - and
four independent copies of one `.config` read-back loop drifted apart
invisibly before this existed. The loop is now one composite action, and this
is what keeps the guard honest.

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
BINUTILS_BODY = extract_run(ACTIONS / "arm-binutils" / "action.yml")


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

    def test_a_rationale_line_that_wrapped_onto_a_symbol_is_named(self):
        """release.yml's FTMS row, 2026-11-06: a comment wrapped so that
        `# CONFIG_BT=y on nRF54L15, ...` began a line, and was read as an
        assertion. Fail, but say why - not as a config mismatch."""
        rc, out = self.check(
            "# the SDC is the default for\n"
            "# CONFIG_BT=y on nRF54L15, verified from .config\n"
            "CONFIG_HRM_BLE_HRS=y")
        self.assertEqual(rc, 1, out)
        self.assertIn("ambiguous expectation", out)
        self.assertNotIn("actual:", out)

    def test_a_typo_in_a_real_negative_is_not_skipped(self):
        """Skipping it as rationale would drop the check silently."""
        rc, out = self.check("# CONFIG_BT is not  set")
        self.assertEqual(rc, 1, out)

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


class ArmBinutils(ActionHarness):
    """The SDK lookup. Fake tools under a fake SDK; HOME points at a temp dir
    so the CMake package registry read is the fixture's, not this machine's."""

    def resolve(self, layout, registry_points_at_cmake=True):
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp).as_posix()
            sdk = Path(tmp) / "zephyr-sdk"
            (sdk / "cmake").mkdir(parents=True)
            (sdk / "cmake" / "Zephyr-sdkConfig.cmake").write_text("")
            if layout:
                bindir = sdk / layout / "bin"
                bindir.mkdir(parents=True)
                for tool in ("arm-zephyr-eabi-nm", "arm-zephyr-eabi-size"):
                    p = bindir / tool
                    p.write_text("#!/bin/sh\n", newline="\n")
                    p.chmod(0o755)
            reg = Path(tmp) / ".cmake" / "packages" / "Zephyr-sdk"
            reg.mkdir(parents=True)
            target = sdk / "cmake" if registry_points_at_cmake else sdk
            (reg / "abc123").write_text(target.as_posix() + "\n")
            out = Path(tmp) / "out"
            rc, log = self.run_body(BINUTILS_BODY, {
                "HOME": t, "ZEPHYR_SDK_INSTALL_DIR": "",
                "GITHUB_WORKSPACE": t + "/nowhere",
                "GITHUB_OUTPUT": out.as_posix()}, tmp)
            outputs = out.read_text() if out.exists() else ""
            return rc, log, outputs, sdk.as_posix()

    def test_registry_entry_is_the_cmake_dir_not_the_root(self):
        """CI, 2026-11-06: 'arm-zephyr-eabi-nm not found (.../zephyr-sdk/cmake)'."""
        rc, log, outputs, sdk = self.resolve("arm-zephyr-eabi")
        self.assertEqual(rc, 0, log)
        self.assertIn(f"nm={sdk}/arm-zephyr-eabi/bin/arm-zephyr-eabi-nm", outputs)
        self.assertIn(f"sdk={sdk}\n", outputs)

    def test_sdk_1_0_gnu_layout(self):
        rc, log, outputs, sdk = self.resolve("gnu/arm-zephyr-eabi")
        self.assertEqual(rc, 0, log)
        self.assertIn(
            f"size={sdk}/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-size", outputs)

    def test_a_registry_entry_that_is_already_the_root_still_works(self):
        rc, log, outputs, sdk = self.resolve(
            "arm-zephyr-eabi", registry_points_at_cmake=False)
        self.assertEqual(rc, 0, log)
        self.assertIn(f"nm={sdk}/arm-zephyr-eabi/bin/", outputs)

    def test_no_tools_is_a_named_failure(self):
        rc, log, outputs, _ = self.resolve(None)
        self.assertEqual(rc, 1, log)
        self.assertIn("arm-zephyr-eabi-nm not found", log)
        self.assertEqual(outputs, "")


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

    def test_no_rationale_comment_begins_with_a_symbol(self):
        """A '#' line starting with CONFIG_ is read as Kconfig's negative form,
        so anything but exactly that is a wrapped comment the action rejects."""
        hashed = re.compile(r"^\s*#\s+CONFIG_")
        negative = re.compile(r"^\s*# CONFIG_[A-Za-z0-9_]+ is not set\s*$")
        offenders = []
        for wf in sorted(self.WORKFLOWS.glob("*.yml")):
            for n, line in self.block_scalar_lines(wf):
                if hashed.match(line) and not negative.match(line):
                    offenders.append(f"{wf.name}:{n}: {line.strip()}")
        self.assertEqual(
            offenders, [],
            "these comment lines start with a CONFIG_ symbol and will be read "
            "as assertions; rewrap them:\n  " + "\n  ".join(offenders))

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

    # ── build-core: no caches, a pinned toolchain (security review 2026-11-22)
    #
    # The release lane never runs on a pull request, so a cache or an unpinned
    # download put back into the job that cuts the shipping images would go
    # unseen until a tag shipped with it. These read the workflow text.

    RELEASE = WORKFLOWS / "release.yml"
    FROZEN = REPO / ".github" / "west-release-frozen.yml"
    LOCK = REPO / ".github" / "release-build-requirements.txt"
    SHA40 = re.compile(r"^[0-9a-f]{40}$")
    TAG_GUARD = "!startsWith(github.ref, 'refs/tags/v')"

    def job_steps(self, path, job):
        """The steps of one job as lists of (lineno, line), comments dropped.

        Comment lines are dropped so that a comment which names the setup
        action or a cache (as build-core's own explanation does) is not read
        as a use of it."""
        lines = path.read_text(encoding="utf-8").splitlines()
        header = re.compile(r"^  [A-Za-z0-9_-]+:\s*$")
        inside = False
        steps = []
        for n, line in enumerate(lines, 1):
            if not inside:
                inside = line.rstrip() == f"  {job}:"
                continue
            if header.match(line) or (line and not line[0].isspace()):
                break
            if line.lstrip().startswith("#") or not line.strip():
                continue
            if line.startswith("      - "):
                steps.append([])
            if steps:
                steps[-1].append((n, line))
        self.assertTrue(steps, f"no steps found for job {job} in {path.name}")
        return steps

    @staticmethod
    def step_text(step):
        return "\n".join(line for _, line in step)

    @staticmethod
    def step_name(step):
        m = re.search(r"-?\s*name:\s*(.+)$", step[0][1])
        return m.group(1).strip() if m else step[0][1].strip()

    def manifest_projects(self, path):
        """{name: revision or None} for the top-level `projects:` list of a
        west manifest. No yaml: entries are the `- name:` lines at the list's
        own indent, so a project's `submodules:` entries are not projects."""
        projects = {}
        indent = None
        current = None
        in_projects = False
        for line in path.read_text(encoding="utf-8").splitlines():
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            depth = len(line) - len(line.lstrip())
            if line.strip() == "projects:":
                in_projects, indent = True, None
                continue
            if not in_projects:
                continue
            m = re.match(r"^(\s*)- name:\s*(\S+)", line)
            if m and (indent is None or len(m.group(1)) == indent):
                indent = len(m.group(1))
                current = m.group(2)
                projects[current] = None
                continue
            if indent is not None and depth <= indent and not line.lstrip(
                    ).startswith("- "):
                in_projects = False
                continue
            r = re.match(r"^\s*revision:\s*(\S+)", line)
            if r and current and depth == indent + 2:
                projects[current] = r.group(1).strip("'\"")
        return projects

    def test_build_core_restores_no_cache_and_drops_the_setup_action(self):
        """No actions/cache restore on a tag, and no action-zephyr-setup (it
        restores its own SDK, pip and ccache caches)."""
        offenders = []
        for step in self.job_steps(self.RELEASE, "build-core"):
            text = self.step_text(step)
            line = step[0][0]
            if "action-zephyr-setup" in text:
                offenders.append(f"release.yml:{line}: action-zephyr-setup")
            if re.search(r"uses:\s*actions/cache[@/]", text) and \
                    self.TAG_GUARD not in text:
                offenders.append(f"release.yml:{line}: actions/cache with no "
                                 f"`if: {self.TAG_GUARD}`")
        self.assertEqual(offenders, [],
                         "build-core cuts the shipping images and must not "
                         "restore what master CI runs wrote:\n  "
                         + "\n  ".join(offenders))

    def test_build_core_build_step_turns_ccache_off(self):
        builds = [s for s in self.job_steps(self.RELEASE, "build-core")
                  if self.step_name(s) == "Build"]
        self.assertEqual(len(builds), 1, "build-core has no single Build step")
        self.assertIn("-DUSE_CCACHE=0", self.step_text(builds[0]))

    def test_every_build_core_download_is_digest_checked(self):
        """Each download goes through a <X>_URL env value with a <X>_SHA256
        beside it, and the step runs `sha256sum -c` on it. The SDK and the
        arm toolchain must be among them: they used to come from the setup
        action's unchecked wget."""
        offenders = []
        urls = []
        for step in self.job_steps(self.RELEASE, "build-core"):
            text = self.step_text(step)
            run = text.split("run: |", 1)[1] if "run: |" in text else ""
            if not re.search(r"\b(curl|wget)\b", run):
                continue
            where = f"release.yml:{step[0][0]} ({self.step_name(step)})"
            if re.search(r"https?://", run):
                offenders.append(f"{where}: a literal URL in run:, not in env")
            if "sha256sum -c" not in run:
                offenders.append(f"{where}: downloads with no sha256sum -c")
            env_urls = re.findall(r"^\s+([A-Z0-9_]+)_URL:\s*(\S+)\s*$", text,
                                  re.M)
            if not env_urls:
                offenders.append(f"{where}: downloads, but no *_URL env")
            for key, url in env_urls:
                urls.append(url)
                digest = re.search(rf"^\s+{key}_SHA256:\s*([0-9a-f]{{64}})\s*$",
                                   text, re.M)
                if not digest:
                    offenders.append(f"{where}: {key}_URL has no {key}_SHA256")
                elif f"${{{key}_SHA256}}" not in run:
                    offenders.append(f"{where}: {key}_SHA256 is never checked")
        self.assertEqual(offenders, [], "\n  ".join([""] + offenders))
        self.assertTrue(any("/zephyr-sdk-" in u and "_minimal." in u
                            for u in urls),
                        "build-core does not download a pinned Zephyr SDK")
        self.assertTrue(any("arm-zephyr-eabi" in u for u in urls),
                        "build-core does not download a pinned arm toolchain")

    def test_west_yml_pins_sdk_nrf_by_commit(self):
        rev = self.manifest_projects(REPO / "west.yml").get("nrf")
        self.assertIsNotNone(rev, "west.yml has no nrf project revision")
        self.assertRegex(rev, self.SHA40,
                         "west.yml pins sdk-nrf by a name that can be moved")

    def test_build_core_pip_installs_are_hash_locked(self):
        installs = []
        for step in self.job_steps(self.RELEASE, "build-core"):
            # Fold shell continuations so one command is one string.
            text = re.sub(r"\\\n\s*", " ", self.step_text(step))
            installs += [c for c in text.splitlines()
                         if re.search(r"\bpip3?\s+install\b", c)]
        self.assertTrue(installs, "build-core installs no Python packages; "
                                  "where does west come from?")
        for cmd in installs:
            for flag in ("--require-hashes", "--no-deps", "--force-reinstall",
                         "--only-binary :all:",
                         "-r app/.github/release-build-requirements.txt"):
                self.assertIn(flag, cmd, cmd.strip())
        reqs = re.sub(r"\\\n\s*", " ", self.LOCK.read_text(encoding="utf-8"))
        pins = [ln for ln in reqs.splitlines()
                if ln.strip() and not ln.lstrip().startswith("#")]
        self.assertTrue(pins)
        for pin in pins:
            self.assertRegex(pin, r"^[A-Za-z0-9_.-]+==\S+\s+"
                                  r"(--hash=sha256:[0-9a-f]{64}\s*)+$", pin)
        self.assertTrue(any(p.startswith("west==") for p in pins),
                        "the lock does not pin west")

    def test_build_core_uses_a_frozen_manifest_that_agrees_with_west_yml(self):
        """sdk-nrf's own manifest pins zephyr, mcuboot and others by tag, so
        pinning nrf in west.yml is not enough on its own."""
        init = [line for s in self.job_steps(self.RELEASE, "build-core")
                for _, line in s if "west init" in line]
        self.assertEqual(
            [i.strip() for i in init],
            ["west init -l app --mf .github/west-release-frozen.yml"])
        frozen = self.manifest_projects(self.FROZEN)
        self.assertIn("zephyr", frozen)
        loose = {k: v for k, v in frozen.items()
                 if not (v and self.SHA40.match(v))}
        self.assertEqual(loose, {}, "frozen manifest entries not pinned by SHA")
        self.assertNotRegex(self.FROZEN.read_text(encoding="utf-8"),
                            r"(?m)^\s*import:",
                            "a frozen manifest must not import anything")
        for name, rev in self.manifest_projects(REPO / "west.yml").items():
            if rev and self.SHA40.match(rev) and name in frozen:
                self.assertEqual(frozen[name], rev,
                                 f"{name}: west.yml and the frozen manifest "
                                 "disagree; regenerate the frozen file")


if __name__ == "__main__":
    unittest.main()
