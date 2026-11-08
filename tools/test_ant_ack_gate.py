# SPDX-License-Identifier: Apache-2.0
"""The hardware gate must fail even when an originator alone claims success."""
import copy
import unittest

import ant_wire as w

try:
    from ant_ack_gate import assess, assess_parts, exit_code, named_events, reader_timeout_ms, receive_loss
    from ant_probe import READ_TIMEOUT_MS, SerialDevice
except SystemExit:  # pyusb missing: ant_probe exits rather than raising
    # Same reason as test_ant_features.py; otherwise this one module takes
    # the whole discover run down with an ERROR instead of a skip.
    raise unittest.SkipTest("pyusb is not installed") from None


class ReaderTimeoutSplit(unittest.TestCase):
    def test_a_usb_endpoint_never_gets_a_read_short_enough_to_race_startup(self):
        # STARTUP arrives 52 ms after SYSTEM_RESET; a 50 ms libusb0 read lost
        # it 11 times in 1000 (TODO.md item 2). Anything under ~200 ms races.
        usb_dev = object()
        self.assertEqual(reader_timeout_ms(usb_dev), READ_TIMEOUT_MS)
        self.assertGreaterEqual(reader_timeout_ms(usb_dev), 1000)

    def test_a_serial_endpoint_keeps_the_short_read(self):
        # pyserial loses nothing on a timeout, so short is free there.
        serial_dev = SerialDevice.__new__(SerialDevice)
        self.assertEqual(reader_timeout_ms(serial_dev), 50)


class AckGateTests(unittest.TestCase):
    def test_event_metadata_is_not_mistaken_for_an_integer_code(self):
        self.assertEqual(named_events({(1, w.EVENT_RX_FAIL): 3, (2, 255): 1}),
                         {'ch1.EVENT_RX_FAIL': 3, 'ch2.0xff': 1})

    def test_host_latency_crossing_measurement_boundaries_is_not_loss(self):
        result = receive_loss([(0.51, 1, b'a'), (9.49, 1, b'b')],
                              [(0.49, 1, b'a'), (9.51, 1, b'b')], 1, 0, 10)
        self.assertEqual(result['missing'], 0)
        self.assertEqual(result['received'], 2)

    def test_overwritten_source_payload_does_not_invent_a_radio_loss(self):
        result = receive_loss([(1, 1, b'new')], [(1.01, 1, b'previous')], 1, 0, 3)
        self.assertEqual(result['missing'], 0)
        self.assertEqual(result['staged_payload_missing'], 1)

    def test_neighbouring_slot_cannot_satisfy_a_missing_broadcast(self):
        result = receive_loss([(1, 1, b'a'), (1.25, 1, b'b')],
                              [(1.25, 1, b'b')], 1, 0, 3)
        self.assertEqual(result['missing'], 1)

    def test_repeated_payload_cannot_hide_a_missed_slot_or_match_another_channel(self):
        result = receive_loss([(1, 1, b'a'), (1.25, 1, b'a'), (3, 1, b'b')],
                              [(1.1, 1, b'a'), (3.1, 2, b'b'), (8, 1, b'b')], 1, 0, 10)
        self.assertEqual(result['expected'], 3)
        self.assertEqual(result['missing'], 2)

    def setUp(self):
        self.report = dict(seconds=300, sent=300, accepted=300, completed=300,
                           terminal_events=300, peer_received=300,
                           duplicate_commands=0, unexpected_commands=0,
                           protocol_errors=0, errors=[],
                           receive={'1': dict(expected=1200, loss_pct=0.5)})

    def test_clean_independently_observed_exchange_passes(self):
        self.assertEqual(assess(self.report, 1.5), [])

    def test_failures_cannot_be_hidden_by_success_at_originator(self):
        cases = [dict(peer_received=299), dict(duplicate_commands=1),
                 dict(terminal_events=301), dict(terminal_events=299),
                 dict(accepted=299), dict(protocol_errors=1), dict(receive={}),
                 dict(seconds=299), dict(completed=296),
                 dict(receive={'1': dict(expected=1200, loss_pct=1.6)})]
        for changes in cases:
            report = copy.deepcopy(self.report)
            report.update(changes)
            with self.subTest(changes=changes):
                self.assertTrue(assess(report, 1.5))

    def _verdict(self, **changes):
        report = copy.deepcopy(self.report)
        report.update(changes)
        sufficiency, quality = assess_parts(report, 1.5)
        report['sufficiency_failures'] = sufficiency
        report['quality_failures'] = quality
        return report

    def test_a_short_clean_run_is_only_insufficient(self):
        report = self._verdict(seconds=60, sent=50, accepted=50, completed=50,
                               terminal_events=50, peer_received=50)
        self.assertEqual(len(report['sufficiency_failures']), 1)
        self.assertEqual(report['quality_failures'], [])
        self.assertEqual(exit_code(report, gate=False), 0)
        self.assertEqual(exit_code(report, gate=True), 1)

    def test_a_short_run_with_a_quality_failure_exits_nonzero_without_gate(self):
        report = self._verdict(seconds=60, sent=50, accepted=50, completed=40,
                               terminal_events=50, peer_received=50)
        self.assertTrue(report['quality_failures'])
        self.assertEqual(exit_code(report, gate=False), 1)
        self.assertEqual(exit_code(report, gate=True), 1)

    def test_a_clean_full_run_exits_zero_either_way(self):
        report = self._verdict()
        self.assertEqual(exit_code(report, gate=False), 0)
        self.assertEqual(exit_code(report, gate=True), 0)

    def test_assess_is_both_halves(self):
        report = self._verdict(seconds=60, completed=0)
        self.assertEqual(assess(report, 1.5),
                         report['sufficiency_failures'] + report['quality_failures'])


class RunExitStatus(unittest.TestCase):
    """run() itself, on fake endpoints: the callsite that maps the verdict to
    the process exit status. A short run that sends nothing has no runner
    error but fails quality (no transfers completed, no receive stimulus)."""

    def _run(self, gate):
        import argparse
        from unittest import mock

        import ant_ack_gate as g

        class Dev:
            def __init__(self):
                self.last = None

            def write(self, ep, data):
                self.last = data[2] if len(data) > 2 else None

        class Reader:
            def __init__(self, dev, timeout_ms=None):
                self.dev = dev
                self.checksum_errors = 0
                self.discarded_bytes = 0

            def next_frame(self, deadline=None):
                import time
                time.sleep(0.001)
                msg, self.dev.last = self.dev.last, None
                if msg is not None and msg != w.MESG_BROADCAST_DATA_ID:
                    return (w.MESG_RESPONSE_EVENT_ID, bytes([0, msg, 0]))
                return (w.MESG_BROADCAST_DATA_ID, bytes(9))

        args = argparse.Namespace(
            master_port='M', master_serial=None, slave_port='S',
            slave_serial=None, master_baud=115200, slave_trans_type=0,
            zwift_load=False, tx_power=4, device=14871, sends=0,
            seconds=0, interval=1.0, acquire_seconds=5, settle_seconds=0,
            json_out=None, gate=gate)
        with mock.patch.object(g, 'open_device', lambda *a, **k: Dev()),                 mock.patch.object(g, 'close_device', lambda d: None),                 mock.patch.object(g, 'reset_stack', lambda d, r: True),                 mock.patch.object(g, 'FrameReader', Reader),                 mock.patch('builtins.print'):
            report = {}
            real = g.exit_code

            def spy(r, gt):
                report.update(r)
                return real(r, gt)

            with mock.patch.object(g, 'exit_code', spy):
                code = g.run(args)
        return code, report

    def test_a_quality_failure_exits_nonzero_without_gate(self):
        code, report = self._run(gate=False)
        self.assertEqual(report['errors'], [])
        self.assertTrue(report['quality_failures'])
        self.assertEqual(code, 1)

    def test_a_short_run_that_only_lacks_sufficiency_is_not_a_failure_here(self):
        # Sanity for the fake: the sufficiency reason is present too.
        code, report = self._run(gate=True)
        self.assertTrue(report['sufficiency_failures'])
        self.assertEqual(code, 1)


if __name__ == '__main__':
    unittest.main()
