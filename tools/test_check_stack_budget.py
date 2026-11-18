# SPDX-License-Identifier: Apache-2.0

"""scripts/check_stack_budget.py: the checks that would have caught item 1.

Security review 2026-11-17, item 1: a 2,248 B X25519 frame on the dongle's
2,048 B serial bridge thread, unnoticed because every test ran on a bigger
stack. The `.su` lines below are the real ones from that build (nRF52840, -Os),
so the first test is the regression: the old chain must fail, the folded one
must pass. The rest pin the refusals - an inlined anchor, a dynamic frame, a
limit nobody can resolve - each of which would otherwise turn the check into
one that passes for the wrong reason.

Here rather than beside the script because host-tests runs
`python -m unittest discover -s tools -p "test_*.py"`.
"""

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))

import check_stack_budget as csb  # noqa: E402

BRIDGE = "C:/r/apps/common/ant_serial_bridge.c:1837:13:bridge_thread_fn\t144\tstatic\n"
HOST = "C:/r/radiant/src/antr/ant_sec_host.c:412:12:antr_sec_pair\t40\tstatic\n"
PEER = "C:/r/radiant/src/radiant_sec_pair.c:429:5:radiant_sec_pair_peer\t72\tstatic\n"
FE_MUL = "C:/r/radiant/src/ext/radiant_x25519.c:158:13:fe_mul\t296\tstatic\n"
FE_CARRY = "C:/r/radiant/src/ext/radiant_x25519.c:143:13:fe_carry\t20\tstatic\n"
OLD_CURVE = "C:/r/radiant/src/ext/radiant_x25519.c:281:5:radiant_sec_x25519\t2248\tstatic\n"
NEW_CURVE = "C:/r/radiant/src/ext/radiant_x25519.c:299:5:radiant_sec_x25519\t72\tstatic\n"
FE_MUL_NEW = "C:/r/radiant/src/ext/radiant_x25519.c:158:13:fe_mul\t288\tstatic\n"

PEER_CHAIN = ("peer=bridge_thread_fn,antr_sec_pair,radiant_sec_pair_peer,"
              "radiant_sec_x25519,fe_mul,fe_carry<=2048")


def _run(su_text, chains, **kw):
    frames = csb.parse_su(su_text)
    parsed = [csb.parse_chain(c) for c in chains]
    resolved = [(n, f, csb.resolve_limit(t, kw.pop("config", {}),
                                         kw.pop("macros", {})))
                for n, f, t in parsed]
    lines = []
    failures = csb.check(frames, resolved, kw.pop("exception_frame", 32),
                         out=lines.append, **kw)
    return failures, lines


class TheReviewFinding(unittest.TestCase):

    def test_the_old_ladder_overruns_the_bridge_stack(self):
        su = BRIDGE + HOST + PEER + OLD_CURVE + FE_MUL + FE_CARRY
        failures, lines = _run(su, [PEER_CHAIN])
        self.assertEqual(1, failures, lines)
        self.assertIn("2852 B of 2048", lines[0])

    def test_the_folded_ladder_fits(self):
        su = BRIDGE + HOST + PEER + NEW_CURVE + FE_MUL_NEW + FE_CARRY
        failures, lines = _run(su, [PEER_CHAIN])
        self.assertEqual(0, failures, lines)
        self.assertIn("668 B of 2048", lines[0])

    def test_one_frame_over_the_limit_fails_off_any_chain(self):
        failures, lines = _run(OLD_CURVE + FE_MUL, [], frame_limit=512,
                               frame_limit_paths=["radiant/src"])
        self.assertEqual(1, failures, lines)
        self.assertIn("radiant_sec_x25519 is 2248 B", lines[0])

    def test_the_frame_limit_only_looks_where_it_is_told(self):
        zephyr = "/z/subsys/shell/shell.c:10:1:shell_big\t900\tstatic\n"
        failures, lines = _run(zephyr + FE_MUL, [], frame_limit=512,
                               frame_limit_paths=["radiant/src"])
        self.assertEqual(0, failures, lines)


class TheRefusals(unittest.TestCase):

    def test_an_inlined_anchor_fails_rather_than_counting_zero(self):
        su = BRIDGE + HOST + PEER + FE_MUL + FE_CARRY   # no curve frame
        failures, lines = _run(su, [PEER_CHAIN])
        self.assertEqual(1, failures, lines)
        self.assertIn("no frame for radiant_sec_x25519", lines[0])

    def test_a_dynamic_frame_fails(self):
        su = (BRIDGE + HOST + PEER + FE_MUL + FE_CARRY +
              "C:/r/radiant/src/ext/radiant_x25519.c:300:5:"
              "radiant_sec_x25519\t88\tdynamic,bounded\n")
        failures, lines = _run(su, [PEER_CHAIN])
        self.assertGreaterEqual(failures, 1, lines)
        self.assertTrue(any("dynamic" in line for line in lines), lines)

    def test_a_clone_suffix_still_counts_as_the_function(self):
        su = "C:/r/x.c:1:1:pair_map_init.part.0\t8\tstatic\n"
        self.assertEqual(8, csb.parse_su(su)["pair_map_init"].size)

    def test_the_largest_of_two_same_named_frames_wins(self):
        su = ("C:/r/a.c:1:1:radiant_sec_wipe\t0\tstatic\n"
              "C:/r/b.c:1:1:radiant_sec_wipe\t16\tstatic\n")
        self.assertEqual(16, csb.parse_su(su)["radiant_sec_wipe"].size)

    def test_limits_resolve_from_config_and_header(self):
        cfg = csb.parse_config("CONFIG_ANT_DONGLE_BRIDGE_STACK_SIZE=2048\n"
                               "CONFIG_FPU=y\n")
        macros = csb.parse_header_macros(
            "#define RADIANT_SEC_PAIR_STACK_BYTES 768\n")
        self.assertEqual(2048, csb.resolve_limit(
            "CONFIG_ANT_DONGLE_BRIDGE_STACK_SIZE", cfg, macros))
        self.assertEqual(768, csb.resolve_limit(
            "RADIANT_SEC_PAIR_STACK_BYTES", cfg, macros))
        with self.assertRaises(ValueError):
            csb.resolve_limit("CONFIG_NOT_THERE", cfg, macros)
        with self.assertRaises(ValueError):
            csb.resolve_limit("NOT_A_MACRO", cfg, macros)

    def test_a_malformed_chain_is_refused(self):
        with self.assertRaises(ValueError):
            csb.parse_chain("peer=bridge_thread_fn")
        with self.assertRaises(ValueError):
            csb.parse_chain("peer=<=2048")


class TheCommandLine(unittest.TestCase):

    def _build(self, su_text, config_text):
        tmp = tempfile.TemporaryDirectory()
        root = Path(tmp.name)
        (root / "zephyr").mkdir()
        (root / "x.c.su").write_text(su_text, encoding="utf-8")
        (root / "zephyr" / ".config").write_text(config_text, encoding="utf-8")
        (root / "zephyr" / "zephyr.elf").write_bytes(b"")
        # Sysbuild's own .config, beside no image: must not be the one read.
        (root / "_sysbuild").mkdir()
        (root / "_sysbuild" / ".config").write_text(
            "CONFIG_ANT_DONGLE_BRIDGE_STACK_SIZE=64\n", encoding="utf-8")
        return tmp

    def _main(self, argv):
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = csb.main(argv)
        return rc, buf.getvalue()

    def test_fpu_sets_the_exception_frame_and_the_chain_passes(self):
        su = BRIDGE + HOST + PEER + NEW_CURVE + FE_MUL_NEW + FE_CARRY
        with self._build(su, "CONFIG_ANT_DONGLE_BRIDGE_STACK_SIZE=2048\n"
                             "CONFIG_FPU=y\n") as d:
            rc, out = self._main([
                "--build", d, "--chain",
                PEER_CHAIN.replace("2048", "CONFIG_ANT_DONGLE_BRIDGE_STACK_SIZE")])
        self.assertEqual(0, rc, out)
        self.assertIn("740 B of 2048", out)   # 636 + 104 with FP context

    def test_no_su_files_is_a_failure_not_a_pass(self):
        with tempfile.TemporaryDirectory() as d:
            rc, out = self._main(["--build", d, "--chain", PEER_CHAIN])
        self.assertEqual(1, rc)
        self.assertIn("CONFIG_STACK_USAGE", out)


if __name__ == "__main__":
    unittest.main()
