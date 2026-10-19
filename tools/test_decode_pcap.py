# SPDX-License-Identifier: Apache-2.0
"""Small synthetic USBPcap streams; no dependency on private ride captures."""
import contextlib
import io
import struct
import tempfile
import unittest
from pathlib import Path

import ant_wire as aw
import decode_pcap as dp


def usb(payload, bus=1, device=7, endpoint=0x81, status=0, declared=None):
    return struct.pack('<HQIHBHHBBI', 27, 1, status, 0, 1, bus, device,
                       endpoint, dp.TRANSFER_BULK,
                       len(payload) if declared is None else declared) + payload


def pcap(*packets, tail=b''):
    header = struct.pack('<IHHIIII', dp.PCAP_MAGIC_US, 2, 4, 0, 0, 65535, 249)
    return header + b''.join(struct.pack('<IIII', 1, i * 1000, len(p), len(p)) + p
                             for i, p in enumerate(packets)) + tail


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

    def test_mid_session_and_pending_transfer_accounting(self):
        def event(ref, code):
            return usb(aw.frame(aw.MESG_RESPONSE_EVENT_ID, bytes([1, ref, code])))
        header, _ = self.decode(pcap(
            event(aw.MESG_EVENT_ID, aw.EVENT_TRANSFER_TX_FAILED),
            event(aw.MESG_ACKNOWLEDGED_DATA_ID, 0),
            event(aw.MESG_EVENT_ID, aw.EVENT_TRANSFER_TX_COMPLETED),
            event(aw.MESG_ACKNOWLEDGED_DATA_ID, 0)))
        self.assertIn('accepted=2 completed=1 failed=0 pending=1', header)
        self.assertIn('unmatched_terminal_events=1', header)
        self.assertIn('EVENT_TRANSFER_TX_FAILED=1', header)

    def test_summary_contains_corruption_diagnostics(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'sample.pcap'
            path.write_bytes(pcap(usb(b'\xa4\x00\x6f\x00')))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(dp.main([str(path), '--summary']), 0)
            self.assertIn('bad_checksum_candidates=1', out.getvalue())


if __name__ == '__main__':
    unittest.main()
