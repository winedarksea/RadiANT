#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Tests for tools/ant_conformance.py. No hardware, no device, no pyusb device access.

The tier this tool implements is worth exactly as much as its determinism, so
that is what most of this file checks: the same case list twice, the same bytes
twice, the timestamp column suppressed everywhere, and a comparison that names
the case a difference fell in rather than the byte offset.

The run loop is exercised against a fake device that answers every write from a
script. That is not a substitute for a bench run - nothing here knows what the
firmware actually replies - but it does check the thing a bench run cannot check
cheaply: that two runs of the same tool against the same answers produce
byte-identical files.
"""

from __future__ import annotations

import contextlib
import io
import os
import tempfile
import unittest

import ant_conformance as conf
import ant_trace
import ant_wire as wire


def quiet():
    """Swallow a tool's terminal output. The report is tested, not printed."""
    return contextlib.redirect_stdout(io.StringIO())


class FakeDevice:
    """Enough of the pyusb device surface for run_cases(), and no more."""

    def __init__(self, replies):
        # replies: msg_id of a written frame -> list of (id, payload) to answer
        self.replies = replies
        self.written: list[bytes] = []
        self.queue: list[tuple[int, bytes]] = []

    def write(self, endpoint, data, timeout=None):
        self.written.append(bytes(data))
        parsed = wire.unframe(bytes(data))
        if parsed is None:
            return len(data)   # a malformed frame is dropped silently
        self.queue.extend(self.replies.get(parsed[0], []))
        return len(data)


class FakeReader:
    def __init__(self, dev):
        self.dev = dev

    def next_frame(self, deadline):
        if self.dev.queue:
            return self.dev.queue.pop(0)
        return None


def fake_pair():
    startup = (wire.MESG_STARTUP_MESG_ID,
               bytes([wire.STARTUP_REASONS["STARTUP_COMMAND_RESET"]]))
    ok = (wire.MESG_RESPONSE_EVENT_ID,
          bytes([0, 0x42, wire.RESPONSE_CODES["RESPONSE_NO_ERROR"]]))
    dev = FakeDevice({wire.MESG_SYSTEM_RESET_ID: [startup],
                      wire.MESG_ASSIGN_CHANNEL_ID: [ok]})
    return dev, FakeReader(dev)


class CaseGeneration(unittest.TestCase):
    def setUp(self):
        self.cases = conf.generate_cases()

    def test_two_calls_produce_identical_cases(self):
        again = conf.generate_cases()
        self.assertEqual([(c.name, c.frames) for c in self.cases],
                         [(c.name, c.frames) for c in again])

    def test_case_names_are_unique(self):
        names = [case.name for case in self.cases]
        self.assertEqual(len(names), len(set(names)))

    def test_every_host_to_dongle_message_is_driven(self):
        """The set comes from the YAML, so a new message enters the run itself."""
        expected = {
            msg_id for msg_id, info in wire.MESSAGES.items()
            if info["direction"] in ("h2d", "both")
            and msg_id not in conf.EXCLUDED
            and msg_id != wire.MESG_REQUEST_ID
        }
        seen = {int(case.name.split("-")[0], 16) for case in self.cases
                if not case.name.startswith(("frame/", "4d-request"))}
        self.assertEqual(seen, expected)

    def test_bridged_messages_are_all_covered(self):
        # A subset of the check above, spelled out because BRIDGED_MESSAGE_IDS
        # is the set the tier is actually about.
        bridged = {msg_id for msg_id in wire.BRIDGED_MESSAGE_IDS
                   if wire.MESSAGES[msg_id]["direction"] in ("h2d", "both")
                   and msg_id not in conf.EXCLUDED
                   and msg_id != wire.MESG_REQUEST_ID}
        seen = {int(case.name.split("-")[0], 16) for case in self.cases
                if not case.name.startswith(("frame/", "4d-request"))}
        self.assertTrue(bridged <= seen, bridged - seen)

    def test_nothing_that_could_key_the_transmitter_is_sent(self):
        """The three exclusions are the reason a run is reproducible at all."""
        sent_ids = set()
        for case in self.cases:
            for data in case.frames:
                parsed = wire.unframe(data)
                if parsed is not None:
                    sent_ids.add(parsed[0])
        for excluded in conf.EXCLUDED:
            self.assertNotIn(excluded, sent_ids,
                             wire.MESSAGE_NAMES.get(excluded))

    def test_open_channel_is_never_fully_configured_first(self):
        """An opened, configured channel searches, and a search hears the room."""
        for case in self.cases:
            if not case.name.startswith(f"{wire.MESG_OPEN_CHANNEL_ID:02x}-"):
                continue
            preamble_ids = {wire.unframe(f)[0] for f in case.frames[:-1]
                            if wire.unframe(f) is not None}
            self.assertNotIn(wire.MESG_CHANNEL_ID_ID, preamble_ids)
            self.assertNotIn(wire.MESG_CHANNEL_RADIO_FREQ_ID, preamble_ids)

    def test_every_message_has_a_valid_and_a_bad_checksum_case(self):
        stems = {case.name.split("/")[0] for case in self.cases
                 if not case.name.startswith("frame/")}
        names = {case.name for case in self.cases}
        for stem in stems:
            self.assertIn(f"{stem}/valid", names)
            if not stem.startswith("4d-request"):
                self.assertIn(f"{stem}/{conf.MALFORMED}-checksum", names)

    def test_every_deliberately_wrong_case_carries_the_shared_token(self):
        """`tools/test_ant_golden.py` keys on this word to replay a transcript."""
        for case in self.cases:
            if case.name.endswith("/valid") or "sync-in-payload" in case.name:
                continue
            self.assertIn(conf.MALFORMED, case.name)

    def test_short_and_long_bracket_the_declared_length(self):
        bounds = conf.payload_bounds(wire.MESG_ASSIGN_CHANNEL_ID)
        self.assertEqual(bounds, (3, 4))
        by_name = {case.name: case for case in self.cases}
        short = by_name["42-assign-channel/malformed-short"].frames[-1]
        long = by_name["42-assign-channel/malformed-long"].frames[-1]
        self.assertEqual(len(wire.unframe(short)[1]), 2)
        self.assertEqual(len(wire.unframe(long)[1]), 5)

    def test_long_never_exceeds_the_parsers_body_buffer(self):
        for case in self.cases:
            if not case.name.endswith("-long"):
                continue
            payload = wire.unframe(case.frames[-1])[1]
            self.assertLessEqual(len(payload), wire.MAX_SIZE_VALUE)

    def test_the_bad_checksum_case_really_is_invalid(self):
        by_name = {case.name: case for case in self.cases}
        frame = by_name["42-assign-channel/malformed-checksum"].frames[-1]
        self.assertIsNone(wire.unframe(frame))
        # Wrong by one bit, not by a byte of garbage: the frame must otherwise
        # be perfectly well formed or the test is about the wrong thing.
        repaired = bytearray(frame)
        repaired[-1] ^= 0x01
        self.assertIsNotNone(wire.unframe(bytes(repaired)))

    def test_every_other_frame_is_well_formed(self):
        deliberately_broken = ("malformed-checksum", "frame/malformed-sync",
                               "frame/malformed-partial-then-valid")
        for case in self.cases:
            if any(mark in case.name for mark in deliberately_broken):
                continue
            for data in case.frames:
                self.assertIsNotNone(wire.unframe(data),
                                     f"{case.name}: {data.hex()}")

    def test_the_out_of_range_index_is_out_of_range_for_every_meaning(self):
        self.assertEqual(conf.BAD_INDEX, 0xFF)
        self.assertEqual(conf.BAD_INDEX, wire.SDU["INVALID_SDU_MASK"])

    def test_the_network_key_case_carries_the_published_ant_plus_key(self):
        # A made-up key would manufacture a divergence: radiant_core holds a table
        # seeded with the ANT+ pair and refuses keys it does not know.
        payload = conf.CANONICAL[wire.MESG_NETWORK_KEY_ID]
        self.assertEqual(payload,
                         bytes([0]) + bytes.fromhex("b9a521fbbd72c345"))

    def test_lib_config_asks_for_all_three_extended_fields(self):
        payload = conf.CANONICAL[wire.MESG_ANTLIB_CONFIG_ID]
        self.assertEqual(payload[1], 0xE0)

    def test_request_cases_cover_the_leading_index_byte_shapes(self):
        names = {case.name for case in self.cases}
        self.assertIn("4d-request-sdu-set-mask-0/valid", names)
        self.assertIn("4d-request-encrypt-enable-2/valid", names)
        self.assertIn("4d-request-config-adv-burst-1/valid", names)

    def test_frame_cases_come_last(self):
        # Two of them deliberately leave junk in the parser's input; the next
        # thing down the wire should be a reset, not another message case.
        frame_indices = [i for i, c in enumerate(self.cases)
                         if c.name.startswith("frame/")]
        self.assertEqual(frame_indices,
                         list(range(len(self.cases) - len(frame_indices),
                                    len(self.cases))))


class Determinism(unittest.TestCase):
    def run_once(self):
        dev, reader = fake_pair()
        state = conf.run_cases(dev, reader, conf.generate_cases(), settle=0.0,
                               reset_each=True, verbose=False)
        return ant_trace.format_antser(state.records), state

    def test_two_runs_are_byte_identical(self):
        first, _ = self.run_once()
        second, _ = self.run_once()
        self.assertEqual(first, second)

    def test_the_timestamp_column_is_always_suppressed(self):
        text, _ = self.run_once()
        for line in text.splitlines():
            if line.startswith("#"):
                continue
            self.assertTrue(line.startswith("- "), line)

    def test_every_record_is_attributed_to_a_case(self):
        _, state = self.run_once()
        self.assertTrue(all(record.case for record in state.records))

    def test_a_transcript_reads_back_to_the_same_records(self):
        text, state = self.run_once()
        back = ant_trace.read_antser(text)
        self.assertEqual([(r.direction, r.data, r.case) for r in back],
                         [(r.direction, r.data, r.case)
                          for r in state.records])

    def test_a_reset_that_goes_unanswered_stops_the_run(self):
        dev = FakeDevice({})          # answers nothing at all
        with self.assertRaises(SystemExit):
            conf.run_cases(dev, FakeReader(dev), conf.generate_cases()[:1],
                           settle=0.0, reset_each=True, verbose=False)

    def test_silent_and_talkative_cases_are_both_recorded(self):
        _, state = self.run_once()
        # The fake answers only assign-channel, so most valid cases are silent
        # and the bad-checksum ones are silent by design.
        self.assertTrue(state.silent_cases)
        self.assertFalse(state.unexpected_replies)


class Comparison(unittest.TestCase):
    def records(self):
        dev, reader = fake_pair()
        state = conf.run_cases(dev, reader, conf.generate_cases()[:8],
                               settle=0.0, reset_each=True, verbose=False)
        return state.records

    def test_identical_transcripts_have_no_differences(self):
        records = self.records()
        self.assertEqual(conf.compare_records(records, list(records)), [])

    def test_a_changed_reply_is_attributed_to_its_case(self):
        a = self.records()
        b = list(a)
        index = next(i for i, r in enumerate(b) if r.direction == "<")
        broken = wire.frame(wire.MESG_RESPONSE_EVENT_ID,
                            bytes([0, 0x42,
                                   wire.RESPONSE_CODES["INVALID_MESSAGE"]]))
        b[index] = ant_trace.Record(None, "<", broken, a[index].case)
        diffs = conf.compare_records(a, b)
        self.assertEqual(len(diffs), 1)
        self.assertEqual(diffs[0].case, a[index].case)

    def test_a_missing_reply_shows_up_as_a_run_of_differences(self):
        a = self.records()
        b = [r for i, r in enumerate(a) if i != 3]
        self.assertTrue(conf.compare_records(a, b))

    def test_compare_end_to_end(self):
        records = self.records()
        with tempfile.TemporaryDirectory() as tmp:
            path_a = os.path.join(tmp, "a.antser")
            path_b = os.path.join(tmp, "b.antser")
            ant_trace.write_antser(path_a, records)
            ant_trace.write_antser(path_b, records)

            with quiet():
                result = conf.compare(path_a, path_b, set())
            self.assertTrue(result["byte_identical"])
            self.assertTrue(result["pass"])
            self.assertEqual(result["differing_cases"], [])

            # Change one reply and the gate must fail, naming the case.
            changed = list(records)
            index = next(i for i, r in enumerate(changed)
                         if r.direction == "<")
            changed[index] = ant_trace.Record(
                None, "<",
                wire.frame(wire.MESG_RESPONSE_EVENT_ID, bytes([1, 2, 3])),
                changed[index].case)
            ant_trace.write_antser(path_b, changed)

            with quiet():
                result = conf.compare(path_a, path_b, set())
            self.assertFalse(result["byte_identical"])
            self.assertFalse(result["pass"])
            self.assertEqual(result["unexpected_differing_cases"],
                             result["differing_cases"])

            # ... and must pass once that case is explicitly allowed to differ.
            allowed = set(result["differing_cases"])
            with quiet():
                result = conf.compare(path_a, path_b, allowed)
            self.assertFalse(result["byte_identical"])
            self.assertTrue(result["pass"])
            self.assertEqual(result["unexpected_differing_cases"], [])


class Summary(unittest.TestCase):
    def test_summary_names_what_was_skipped_and_why(self):
        dev, reader = fake_pair()
        cases = conf.generate_cases()
        state = conf.run_cases(dev, reader, cases, settle=0.0,
                               reset_each=True, verbose=False)
        text = ant_trace.format_antser(state.records)
        summary = conf.summarise(cases, state, "x.antser", text)

        self.assertEqual(summary["cases"], len(cases))
        self.assertEqual(len(summary["sha256"]), 64)
        self.assertGreater(summary["messages_exercised"], 30)
        skipped = {entry["what"] for entry in summary["skipped"]}
        # One skip from each of the two reasons a message can be left out: a
        # deliberate exclusion, and an id nobody has recovered yet. The second
        # was MESG_CHANNEL_CRC_MODE_ID until the sdk-ant shim resolved it to
        # 0x58, at which point it started being exercised rather than skipped.
        # MESG_RSSI_SEARCH_THRESHOLD_ID is genuinely still unresolved - it
        # appears nowhere in sdk-ant either - so it is the durable example.
        self.assertIn("MESG_RADIO_CW_MODE_ID", skipped)
        self.assertIn("MESG_RSSI_SEARCH_THRESHOLD_ID", skipped)
        for entry in summary["skipped"]:
            self.assertTrue(entry["why"], entry["what"])

    def test_the_case_index_hashes_each_case_separately(self):
        dev, reader = fake_pair()
        cases = conf.generate_cases()[:4]
        state = conf.run_cases(dev, reader, cases, settle=0.0,
                               reset_each=True, verbose=False)
        summary = conf.summarise(cases, state, "x.antser", "")
        self.assertEqual(len(summary["case_index"]), len(cases))
        self.assertEqual({entry["name"] for entry in summary["case_index"]},
                         {case.name for case in cases})


if __name__ == "__main__":
    unittest.main()
