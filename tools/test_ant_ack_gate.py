# SPDX-License-Identifier: Apache-2.0
"""The hardware gate must fail even when an originator alone claims success."""
import copy
import unittest

import ant_wire as w
from ant_ack_gate import assess, named_events, reader_timeout_ms, receive_loss
from ant_probe import READ_TIMEOUT_MS, SerialDevice


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


if __name__ == '__main__':
    unittest.main()
