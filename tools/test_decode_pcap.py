# SPDX-License-Identifier: Apache-2.0
"""Small synthetic USBPcap streams; no dependency on private ride captures."""
import contextlib
import io
import struct
import tempfile
import unittest
from pathlib import Path

import ant_pages as ap
import ant_wire as aw
import decode_pcap as dp


def usb(payload, bus=1, device=7, endpoint=0x81, status=0, declared=None):
    return struct.pack('<HQIHBHHBBI', 27, 1, status, 0, 1, bus, device,
                       endpoint, dp.TRANSFER_BULK,
                       len(payload) if declared is None else declared) + payload


def pcap(*packets, tail=b'', times=None):
    """Packets 1 ms apart from t=1 s, or at `times` (seconds) if given."""
    header = struct.pack('<IHHIIII', dp.PCAP_MAGIC_US, 2, 4, 0, 0, 65535, 249)
    if times is None:
        times = [1 + i / 1000 for i in range(len(packets))]
    out = b''
    for t, p in zip(times, packets):
        sec = int(t)
        out += struct.pack('<IIII', sec, round((t - sec) * 1e6), len(p), len(p)) + p
    return header + out + tail


def host(msg_id, body):
    """A host->dev bulk URB carrying one message."""
    return usb(aw.frame(msg_id, bytes(body)), endpoint=0x01)


def dev(msg_id, body):
    return usb(aw.frame(msg_id, bytes(body)))


def event(ch, code):
    return dev(aw.MESG_RESPONSE_EVENT_ID, [ch, aw.MESG_EVENT_ID, code])


def response(ch, ref, code):
    return dev(aw.MESG_RESPONSE_EVENT_ID, [ch, ref, code])


def rx(ch, ticks, payload=bytes(8), device_type=11, device_number=0x1234):
    """A dev->host BROADCAST_DATA with device id, RSSI and timestamp."""
    return dev(aw.MESG_BROADCAST_DATA_ID,
               bytes([ch]) + payload + bytes([0xE0])
               + bytes([device_number & 0xFF, device_number >> 8, device_type, 5])
               + bytes(3) + bytes([ticks & 0xFF, (ticks >> 8) & 0xFF]))


def run_main(args):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = dp.main([str(a) for a in args])
    return rc, out.getvalue()


class DecodeTests(unittest.TestCase):
    def decode(self, data, **kwargs):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'sample.pcap'
            path.write_bytes(data)
            header, lines = dp.decode(path, **kwargs)
        return '\n'.join(header), lines

    def test_split_coalesced_and_embedded_sync(self):
        a = aw.frame(aw.MESG_BROADCAST_DATA_ID, bytes([1, 0xA4]) + bytes(7))
        b = aw.frame(aw.MESG_STARTUP_MESG_ID, b'\x00')
        header, lines = self.decode(pcap(usb(a[:2]), usb(a[2:] + b)))
        self.assertEqual(len(lines), 2)
        self.assertIn('bad_checksum_candidates=0', header)
        self.assertIn('broadcasts=1', header)

    def test_corrupt_candidate_is_reported_and_following_frame_survives(self):
        bad = bytearray(aw.frame(aw.MESG_STARTUP_MESG_ID, b'\x00'))
        bad[-1] ^= 1
        good = aw.frame(aw.MESG_STARTUP_MESG_ID, b'\x01')
        header, lines = self.decode(pcap(usb(b'junk' + bad + good)))
        self.assertEqual(len(lines), 1)
        self.assertTrue(lines[0].endswith('STARTUP_MESG 01'))
        self.assertIn('bad_checksum_candidates=1', header)
        self.assertIn('discarded_bytes=9', header)
        self.assertIn('bad checksum candidate', header)

    def test_eof_recovers_frames_behind_incomplete_false_sync(self):
        good = aw.frame(aw.MESG_STARTUP_MESG_ID, b'\x01')
        prefix = b'\xa4\xff\x00\xa4\xfe\x00'
        header, lines = self.decode(pcap(usb(prefix + good + good + b'\xa4\x09')))
        self.assertEqual(len(lines), 2)
        self.assertTrue(all(line.endswith('STARTUP_MESG 01  ' + dp.HEURISTIC_MARK)
                            for line in lines))
        self.assertIn('incomplete_candidates_resynchronized=1', header)
        self.assertIn('heuristic_messages=2', header)
        self.assertIn('discarded_bytes=6', header)
        self.assertIn('incomplete_ant_bytes=2', header)

    def test_embedded_complete_frame_waits_for_outer_frame_until_eof(self):
        inner = aw.frame(aw.MESG_STARTUP_MESG_ID, b'\x01')
        outer = aw.frame(aw.MESG_BROADCAST_DATA_ID, b'\x01' + inner + b'\x00')
        header, lines = self.decode(pcap(usb(outer[:-1]), usb(outer[-1:])))
        self.assertEqual(len(lines), 1)
        self.assertIn('BROADCAST_DATA', lines[0])
        self.assertIn('incomplete_candidates_resynchronized=0', header)

    def test_eof_recovered_frames_stay_out_of_transfer_summary(self):
        response = aw.frame(aw.MESG_RESPONSE_EVENT_ID,
                            bytes([1, aw.MESG_ACKNOWLEDGED_DATA_ID, 0]))
        header, lines = self.decode(pcap(usb(b'\xa4\xff\x00' + response)))
        self.assertEqual(len(lines), 1)
        self.assertTrue(lines[0].endswith(dp.HEURISTIC_MARK))
        self.assertNotIn('accepted=', header)

    def test_frame_inside_a_truncated_message_is_not_counted(self):
        # A real broadcast whose payload happens to hold a complete,
        # checksum-valid broadcast frame, cut off by the end of the capture.
        inner = aw.frame(aw.MESG_BROADCAST_DATA_ID, bytes([2]) + bytes(8))
        outer = aw.frame(aw.MESG_EXT_BROADCAST_DATA_ID,
                         b'\x01' + inner + bytes(20 - len(inner)))
        header, lines = self.decode(pcap(usb(outer[:-3])))
        self.assertEqual(len(lines), 1)
        self.assertTrue(lines[0].endswith(dp.HEURISTIC_MARK))
        self.assertNotIn('broadcasts=', header)
        self.assertIn('heuristic_messages=1', header)

    def test_summary_keeps_heuristic_frames_apart(self):
        good = aw.frame(aw.MESG_STARTUP_MESG_ID, b'\x01')
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'sample.pcap'
            path.write_bytes(pcap(usb(good + b'\xa4\xff\x00' + good)))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(dp.main([str(path), '--summary']), 0)
            text = out.getvalue()
        self.assertIn('1  dev->host STARTUP_MESG\n', text)
        self.assertIn('1  dev->host STARTUP_MESG (heuristic)', text)

    def test_eof_recovery_does_not_join_streams(self):
        good = aw.frame(aw.MESG_STARTUP_MESG_ID, b'\x01')
        header, lines = self.decode(pcap(usb(b'\xa4\xff\x00' + good[:3]),
                                        usb(good[3:], endpoint=0x82)))
        self.assertFalse(lines)
        self.assertIn('incomplete_candidates_resynchronized=0', header)

    def test_bus_device_endpoint_and_direction_do_not_splice(self):
        f = aw.frame(aw.MESG_STARTUP_MESG_ID, b'\x00')
        for other in ({'bus': 2}, {'device': 8}, {'endpoint': 0x82}, {'endpoint': 1}):
            with self.subTest(other=other):
                header, lines = self.decode(pcap(usb(f[:2]), usb(f[2:], **other)))
                self.assertFalse(lines)
                self.assertIn('incomplete_ant_streams=1', header)

    def test_truncated_pcap_headers_and_payloads(self):
        for tail in (b'\x01', struct.pack('<IIII', 1, 0, 40, 40) + b'short'):
            header, lines = self.decode(pcap(tail=tail))
            self.assertIn('truncated_pcap_records=1', header)
            self.assertFalse(lines)

    def test_usb_errors_and_malformed_payloads_are_not_clean_frames(self):
        f = aw.frame(aw.MESG_STARTUP_MESG_ID, b'\x00')
        header, lines = self.decode(pcap(usb(f, status=1), usb(f, declared=99), b'bad'))
        self.assertFalse(lines)
        self.assertIn('nonzero_usb_statuses=1', header)
        self.assertIn('truncated_usb_payloads=1', header)
        self.assertIn('malformed_usb_records=1', header)

    def test_failed_empty_urb_breaks_a_partial_stream(self):
        f = aw.frame(aw.MESG_STARTUP_MESG_ID, b'\x00')
        header, lines = self.decode(pcap(usb(f[:2]), usb(b'', status=1), usb(f[2:])))
        self.assertFalse(lines)
        self.assertIn('nonzero_usb_statuses=1', header)

    ACK = aw.MESG_ACKNOWLEDGED_DATA_ID

    def submit(self, ch=1, page=0x33):
        return host(self.ACK, bytes([ch, page]) + bytes(7))

    def test_mid_session_and_pending_transfer_accounting(self):
        # The first terminal event may settle a transfer submitted before the
        # capture began: it is presync, and the ledger starts after it.
        header, _ = self.decode(pcap(
            event(1, aw.EVENT_TRANSFER_TX_FAILED),
            self.submit(),
            response(1, self.ACK, 0),
            event(1, aw.EVENT_TRANSFER_TX_COMPLETED),
            self.submit(),
            response(1, self.ACK, 0)))
        self.assertIn('transfers submitted=2 accepted=2 refused=0 completed=1 '
                      'failed=0 pending=1 presync=1 confirmed=2', header)
        self.assertNotIn('unmatched', header)
        self.assertIn('EVENT_TRANSFER_TX_FAILED=1', header)

    def test_garmin_ledger_needs_no_response_no_error(self):
        # The Garmin USB-m never answers a 0x4F with RESPONSE_NO_ERROR.
        header, _ = self.decode(pcap(
            host(aw.MESG_ASSIGN_CHANNEL_ID, [1, 0, 0]),
            self.submit(),
            event(1, aw.EVENT_TRANSFER_TX_COMPLETED)))
        self.assertIn('transfers submitted=1 accepted=1 refused=0 completed=1 '
                      'failed=0 pending=0 presync=0 confirmed=0', header)

    def test_radiant_no_error_is_not_a_second_submission(self):
        header, _ = self.decode(pcap(
            host(aw.MESG_ASSIGN_CHANNEL_ID, [1, 0, 0]),
            self.submit(),
            response(1, self.ACK, aw.RESPONSE_NO_ERROR),
            event(1, aw.EVENT_TRANSFER_TX_COMPLETED)))
        self.assertIn('submitted=1 accepted=1 refused=0 completed=1 failed=0 '
                      'pending=0 presync=0 confirmed=1', header)

    def test_refusal_takes_back_the_newest_submission(self):
        header, _ = self.decode(pcap(
            host(aw.MESG_OPEN_CHANNEL_ID, [1]),
            self.submit(page=0x31),
            self.submit(page=0x33),
            response(1, self.ACK, aw.TRANSFER_IN_PROGRESS),
            event(1, aw.EVENT_TRANSFER_TX_FAILED)))
        self.assertIn('submitted=2 accepted=1 refused=1 completed=0 failed=1 '
                      'pending=0', header)

    def test_submit_crossing_go_to_search_is_tagged(self):
        # Garmin order: GO_TO_SEARCH, then the pending command's TX_FAILED.
        # A refusal behind it is "behind crossed"; a submit made while
        # searching and alive at the next broadcast is search/.../heard.
        header, _ = self.decode(pcap(
            host(aw.MESG_OPEN_CHANNEL_ID, [1]),
            rx(1, 0),
            self.submit(page=0x33),
            event(1, aw.EVENT_RX_FAIL_GO_TO_SEARCH),
            self.submit(page=0x37),
            response(1, self.ACK, aw.TRANSFER_IN_PROGRESS),
            event(1, aw.EVENT_TRANSFER_TX_FAILED),
            self.submit(page=0x31),
            rx(1, 8192),
            event(1, aw.EVENT_TRANSFER_TX_COMPLETED)))
        self.assertIn('ch=1: by_state track/crossed/unheard completed=0 failed=1 '
                      'refused=0 pending=0', header)
        self.assertIn('ch=1: by_state search/no_cross/unheard completed=0 failed=0 '
                      'refused=1 pending=0', header)
        self.assertIn('ch=1: by_state search/no_cross/heard completed=1 failed=0 '
                      'refused=0 pending=0', header)
        self.assertIn('ch=1: refused behind crossed=1', header)
        self.assertIn('ch=1: gts order gts_then_failed=1', header)

    def test_assign_seen_means_no_presync(self):
        header, _ = self.decode(pcap(
            host(aw.MESG_ASSIGN_CHANNEL_ID, [1, 0, 0]),
            event(1, aw.EVENT_TRANSFER_TX_COMPLETED)))
        self.assertIn('presync=0', header)
        self.assertIn('unmatched=1', header)

    def test_fec_page_breakdown(self):
        header, _ = self.decode(pcap(
            host(aw.MESG_ASSIGN_CHANNEL_ID, [1, 0, 0]),
            host(aw.MESG_CHANNEL_ID_ID, [1, 0, 0, ap.FEC_DEVICE_TYPE, 0]),
            self.submit(page=0x33),
            event(1, aw.EVENT_TRANSFER_TX_COMPLETED),
            self.submit(page=0x33),
            event(1, aw.EVENT_TRANSFER_TX_FAILED),
            self.submit(page=0x31),
            event(1, aw.EVENT_TRANSFER_TX_COMPLETED)))
        self.assertIn('ch=1: FE-C page 0x31 submitted=1 accepted=1 refused=0 '
                      'completed=1 failed=0 pending=0 completion=100.0 %', header)
        self.assertIn('ch=1: FE-C page 0x33 submitted=2 accepted=2 refused=0 '
                      'completed=1 failed=1 pending=0 completion=50.0 %', header)

    def test_zero_tail_is_padding_not_discarded(self):
        header, lines = self.decode(pcap(
            usb(aw.frame(aw.MESG_SYSTEM_RESET_ID, b'\x00') + b'\x00\x00',
                endpoint=0x01)))
        self.assertEqual(len(lines), 1)
        self.assertIn('discarded_bytes=0 padding_bytes=2', header)

    def test_mixed_junk_is_still_discarded(self):
        header, _ = self.decode(pcap(
            usb(b'\x00\x01' + aw.frame(aw.MESG_STARTUP_MESG_ID, b'\x00'))))
        self.assertIn('discarded_bytes=2 padding_bytes=0', header)

    def test_out_file_holds_only_the_body(self):
        f = aw.frame(aw.MESG_STARTUP_MESG_ID, b'\x00')
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'sample.pcap'
            path.write_bytes(pcap(usb(f)))
            out_file = Path(tmp) / 'out.txt'
            rc, stdout = run_main([path, '-o', out_file])
            body = out_file.read_text(encoding='utf-8')
        self.assertEqual(rc, 0)
        self.assertIn('devices seen', stdout)
        self.assertIn('diagnostics:', stdout)
        self.assertNotIn('devices seen', body)
        self.assertNotIn('diagnostics:', body)
        self.assertEqual(body.splitlines(), ['     0.000  dev->host  STARTUP_MESG 00'])

    def test_summary_contains_corruption_diagnostics(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'sample.pcap'
            path.write_bytes(pcap(usb(b'\xa4\x00\x6f\x00')))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(dp.main([str(path), '--summary']), 0)
            self.assertIn('bad_checksum_candidates=1', out.getvalue())


class SlotLossTests(unittest.TestCase):
    """The summary's slot-loss line, from the receive timestamps."""

    def decode(self, packets, times, **kwargs):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'sample.pcap'
            path.write_bytes(pcap(*packets, times=times))
            header, lines = dp.decode(path, **kwargs)
        return '\n'.join(header), lines

    @staticmethod
    def frames(slots, period, ch=1, t0=1.0):
        """(packets, times) for a frame in each of `slots`."""
        packets, times = [], []
        for n in slots:
            packets.append(rx(ch, (1000 + n * period) & 0xFFFF))
            times.append(t0 + n * period / 32768)
        return packets, times

    def test_silent_slots_count_without_rx_fail(self):
        pk, tm = self.frames([n for n in range(40) if not 10 <= n <= 12], 8192)
        header, _ = self.decode([host(aw.MESG_CHANNEL_MESG_PERIOD_ID, [1, 0x00, 0x20])] + pk,
                                [0.5] + tm)
        self.assertIn('ch=1: slots=40 heard=37 loss=7.5 % (rx_fail=0, 0.0 %) '
                      'longest_run=3 period 8192 (CHANNEL_PERIOD)', header)

    def test_quiet_gap_longer_than_a_wrap(self):
        # 8182 is not a divisor of 65536, so a wrong wrap count cannot hide.
        slots = list(range(10)) + list(range(30, 40))
        pk, tm = self.frames(slots, 8182)
        header, _ = self.decode([host(aw.MESG_CHANNEL_MESG_PERIOD_ID, [1, 0xF6, 0x1F])] + pk,
                                [0.5] + tm)
        self.assertIn('ch=1: slots=40 heard=20 loss=50.0 %', header)
        self.assertIn('longest_run=20', header)

    def test_close_and_reopen_gap_is_not_counted(self):
        pk, tm = self.frames(list(range(10)) + list(range(30, 40)), 8192)
        gap = tm[9] + 0.5
        packets = pk[:10] + [host(aw.MESG_CLOSE_CHANNEL_ID, [1]),
                             host(aw.MESG_OPEN_CHANNEL_ID, [1])] + pk[10:]
        times = tm[:10] + [gap, gap + 0.1] + tm[10:]
        header, _ = self.decode(packets, times)
        self.assertIn('ch=1: slots=20 heard=20 loss=0.0 %', header)

    def test_period_inferred_without_channel_period(self):
        pk, tm = self.frames([n for n in range(40) if n % 7 != 3], 8182)
        header, _ = self.decode(pk, tm)
        self.assertIn('period 8182 (inferred)', header)
        self.assertIn('slots=40 heard=34', header)

    def test_period_override_wins(self):
        pk, tm = self.frames(range(20), 8182)
        header, _ = self.decode([host(aw.MESG_CHANNEL_MESG_PERIOD_ID, [1, 0x00, 0x20])] + pk,
                                [0.5] + tm, periods={1: 8182})
        self.assertIn('period 8182 (--period)', header)
        self.assertNotIn('warning', header)

    def test_inferred_period_disagreeing_with_channel_period_warns(self):
        pk, tm = self.frames(range(20), 8070)
        header, _ = self.decode([host(aw.MESG_CHANNEL_MESG_PERIOD_ID, [1, 0x00, 0x20])] + pk,
                                [0.5] + tm)
        self.assertIn('warning: ch=1 inferred period 8070 differs from '
                      'CHANNEL_PERIOD 8192 by more than 1 %', header)

    def test_bunched_capture_timestamps_warn(self):
        # captures/zwift-20261123-184006.pcap at 118.269 s: under host load
        # USBPcap stamped 14 messages that arrived over 0.5 s with one time.
        pk, tm = self.frames(range(20), 2000)
        stamp = tm[5]
        tm = tm[:5] + [stamp + k * 0.00005 for k in range(8)] + tm[13:]
        # A message without RX ticks in the run counts, but adds no span.
        pk.insert(9, response(1, aw.MESG_CHANNEL_ID_ID, 0))
        tm.insert(9, tm[8])
        header, lines = self.decode(pk, tm)
        warn = [l for l in header.splitlines() if 'share one capture timestamp' in l]
        self.assertEqual(warn, [
            f'warning: {stamp - 1.0:.3f}s bus=1 device=7: 9 messages share one '
            'capture timestamp but span 427 ms of receive time (capture '
            'lagged; trust RX ticks, not this timeline)'])
        # Loss comes from the RX ticks, so it is untouched.
        self.assertIn('ch=1: slots=20 heard=20 loss=0.0 %', header)

    def test_spread_capture_timestamps_do_not_warn(self):
        pk, tm = self.frames(range(20), 8192)
        header, _ = self.decode(pk, tm)
        self.assertNotIn('share one capture timestamp', header)
        # Several frames from a scan channel in one URB burst are normal.
        packets = [rx(0, 1000 + n * 30, device_number=n + 1) for n in range(8)]
        header, _ = self.decode(packets, [1 + n * 0.0001 for n in range(8)])
        self.assertNotIn('share one capture timestamp', header)

    def test_scan_channel_has_no_slot_loss(self):
        packets = [rx(0, 1000 + n * 4000, device_number=n % 3 + 1) for n in range(10)]
        header, _ = self.decode(packets, [1 + n * 0.12 for n in range(10)])
        self.assertIn('ch=0: slots n/a (frames from 3 devices: a scan channel)', header)


class FecTests(unittest.TestCase):
    GRADE = ap.encode_fec_track_resistance(235, rolling_resistance=80)

    def decode(self, data):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'sample.pcap'
            path.write_bytes(data)
            return dp.decode(path)

    def test_track_resistance_on_a_fec_channel_is_annotated(self):
        _, lines = self.decode(pcap(
            host(aw.MESG_CHANNEL_ID_ID, [2, 0, 0, ap.FEC_DEVICE_TYPE | 0x80, 0]),
            host(aw.MESG_ACKNOWLEDGED_DATA_ID, bytes([2]) + self.GRADE)))
        self.assertTrue(lines[-1].endswith('  [FE-C 0x33 grade=+2.35% crr=0.0040]'),
                        lines[-1])

    def test_same_bytes_on_a_power_channel_are_not(self):
        _, lines = self.decode(pcap(
            host(aw.MESG_CHANNEL_ID_ID, [2, 0, 0, ap.BPWR_DEVICE_TYPE, 0]),
            host(aw.MESG_ACKNOWLEDGED_DATA_ID, bytes([2]) + self.GRADE)))
        self.assertNotIn('[FE-C', lines[-1])

    def test_received_frame_identified_by_its_ext_device_id(self):
        trainer = bytes([0x19, 0x07, 0x5A, 0x34, 0x12, 0xA3, 0x95, 0x32])
        _, lines = self.decode(pcap(rx(3, 100, trainer, device_type=ap.FEC_DEVICE_TYPE),
                                    rx(4, 200, trainer, device_type=ap.BPWR_DEVICE_TYPE)))
        self.assertTrue(lines[0].endswith('[FE-C 0x19 ev=7 cad=90 accp=4660 '
                                          'power=1443W status=0x9 state=IN_USE]'),
                        lines[0])
        self.assertNotIn('[FE-C', lines[1])

    def test_annotations(self):
        cases = {
            ap.encode_fec_target_power(1001): '[FE-C 0x31 target=250.25W]',
            ap.encode_fec_track_resistance(None): '[FE-C 0x33 grade=unset(flat) crr=default]',
            ap.encode_fec_track_resistance(-150, 80): '[FE-C 0x33 grade=-1.50% crr=0.0040]',
            ap.encode_fec_cmd_status(0x33, 9, ap.FEC_CMD_STATUS_REJECTED, b'\xff' * 4):
                '[FE-C 0x47 last=0x33 seq=9 status=REJECTED]',
            ap.encode_fec_general(ap.FEC_TYPE_TRAINER, 41, 120, 8333, None,
                                  ap.FEC_STATE_READY):
                '[FE-C 0x10 type=25 t=10.25s dist=120m speed=8.333m/s hr=- state=READY]',
            bytes([0x50]) + bytes(7): '[FE-C 0x50]',
        }
        for payload, text in cases.items():
            with self.subTest(text=text):
                self.assertEqual(dp.fec_annotation(payload), text)

    def test_annotation_does_not_disturb_phase_input(self):
        line = PhaseTests.bcast(3, 1000, 1.0) + '  [FE-C 0x19 ev=1]'
        timing = dp.timing_from_lines([line])
        self.assertEqual(timing.frames()[3], [(1.0, 1000)])


class PhaseTests(unittest.TestCase):
    @staticmethod
    def bcast(ch, ticks, t):
        body = (bytes([ch]) + bytes(8) + bytes([0xE0]) + bytes(4) + bytes(3)
                + bytes([ticks & 0xFF, (ticks >> 8) & 0xFF]))
        return f'{t:10.3f}  dev->host  BROADCAST_DATA {body.hex()}'

    def test_misses_are_placed_against_the_other_channels_offset(self):
        lines = [f'{0:10.3f}  host->dev  CHANNEL_PERIOD 030020',
                 f'{0:10.3f}  host->dev  CHANNEL_PERIOD 02f61f']
        t = 1.0
        for n in range(40):
            ka = (1000 + n * 8192) & 0xFFFF
            kb = (1000 + n * 8182 + 200) & 0xFFFF
            # A goes quiet for three slots while B is ahead of it.
            if not 25 <= n <= 27:
                lines.append(self.bcast(3, ka, t))
            lines.append(self.bcast(2, kb, t + 0.001))
            t += 0.25
        rows, report = dp.phase(lines, 3, 2)
        self.assertEqual(len(rows), 40)
        self.assertEqual([r[1] for r in rows].count(False), 3)
        self.assertIn('longest miss run: 3 slots', '|'.join(report))
        for _, heard, d in rows:
            if not heard:
                # B minus A at slot n is 200 - 10 n ticks: -1.5 to -2.2 ms.
                self.assertTrue(-2.3 < d < -1.4, d)

    def test_wraps_inside_a_gap_on_both_channels_are_counted(self):
        # Both channels silent for 4 s = exactly two counter wraps, so the raw
        # timestamps either side of the gap are identical.
        lines = []
        for n, t in ((0, 1.0), (16, 5.0)):
            lines.append(self.bcast(3, (1000 + n * 8192) & 0xFFFF, t))
            lines.append(self.bcast(2, (1200 + n * 8192) & 0xFFFF, t + 0.001))
        rows, report = dp.phase(lines, 3, 2)
        self.assertEqual(len(rows), 17)
        self.assertEqual([r[1] for r in rows].count(False), 15)

    def test_heuristic_frames_do_not_count_as_heard(self):
        lines = []
        for n in range(5):
            t = 1.0 + n * 0.25
            line = self.bcast(3, 1000 + n * 8192, t)
            if n == 2:
                line += '  ' + dp.HEURISTIC_MARK
            lines.append(line)
            lines.append(self.bcast(2, 1200 + n * 8192, t + 0.001))
        rows, _ = dp.phase(lines, 3, 2)
        self.assertEqual([r[1] for r in rows], [True, True, False, True, True])

    def test_garmin_timestamp_is_the_last_two_bytes(self):
        # From captures/zwift-20261115-220629.pcap (Garmin USB-m): one byte
        # longer than the flagged layout, ticks 0x3B62 at the very end.
        body = bytes.fromhex('02103eb7418b60fa00e0694f0b0510006800623b')
        self.assertEqual(dp.rx_ticks(body), (2, 0x3B62))
        radiant = bytes.fromhex('000aff368234852852e0b205788120c0000698')
        self.assertEqual(dp.rx_ticks(radiant), (0, 0x9806))

    def test_reports_a_channel_with_no_timestamps(self):
        rows, report = dp.phase([self.bcast(3, 5, 1.0)], 3, 2)
        self.assertEqual(rows, [])
        self.assertIn('no timestamped frames on channel 2', report[0])

if __name__ == '__main__':
    unittest.main()
