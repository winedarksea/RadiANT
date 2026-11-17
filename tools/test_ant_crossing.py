# SPDX-License-Identifier: Apache-2.0

"""Tests for tools/ant_crossing.py's analysis, on synthetic decode_pcap-format
logs (no device)."""
from __future__ import annotations

import io
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ant_crossing as x  # noqa: E402
import ant_wire as w  # noqa: E402
import decode_pcap as dp  # noqa: E402

HZ = 32768


def rx_line(t: float, ch: int, ticks: int) -> str:
    body = bytes([ch]) + bytes(8) + bytes([0x20]) + (ticks & 0xFFFF).to_bytes(2, 'little')
    return f'{t:10.3f}  dev->host  {dp.MESSAGE_NAMES[w.MESG_BROADCAST_DATA_ID]} {body.hex()}'


def period_line(ch: int, period: int) -> str:
    body = bytes([ch, period & 255, period >> 8])
    return f'{0.0:10.3f}  host->dev  {dp.MESSAGE_NAMES[w.MESG_CHANNEL_MESG_PERIOD_ID]} {body.hex()}'


def synthetic(seconds: float, fec_lost_ms: float | None, power_start: int,
              epoch: float = 0.0, power: bool = True, shift: float = 0.0):
    """FE-C (8192) and power (8182) frames; FE-C frames whose power offset is
    within fec_lost_ms are dropped. shift moves host time (a control log that
    started later reads earlier times for the same instant)."""
    lines = [f'# ant_crossing epoch={epoch:.6f} channels=power,fec target=t label=-',
             period_line(3, 8192)]
    if power:
        lines.append(period_line(2, 8182))
    events = []
    k = 0
    while k < seconds * HZ:
        events.append((k, 3))
        k += 8192
    k = power_start
    while power and k < seconds * HZ:
        events.append((k, 2))
        k += 8182
    pk = sorted(k for k, ch in events if ch == 2)
    for k, ch in sorted(events):
        if ch == 3 and fec_lost_ms is not None and pk:
            near = min(pk, key=lambda p: abs(p - k))
            if abs(near - k) / 32.768 <= fec_lost_ms:
                continue
        lines.append(rx_line(k / HZ - shift, ch, k))
    return lines


class CrossingTests(unittest.TestCase):
    def test_groups_in_band_rows(self):
        rows = [(0.0, True, 5.0), (0.25, False, 2.0), (0.5, True, 1.0),
                (0.75, False, -2.5), (1.0, True, -4.0),
                (10.0, False, 0.5), (10.25, True, 9.0)]
        self.assertEqual(x.crossings(rows, 3.0), [(1, 3), (5, 5)])

    def test_synthetic_crossing_found_and_counted(self):
        # Power starts 100 ms (3277 ticks) after FE-C and gains 10 ticks per
        # period: one crossing about 327 periods (~82 s) in.
        lines = synthetic(160, 1.0, 3277)
        out = io.StringIO()
        rows = x.analyse(lines, controls=[], out=out)
        text = out.getvalue()
        self.assertIn('1 crossing(s)', text)
        found = x.crossings(rows, 3.0)
        self.assertEqual(len(found), 1)
        a, b = found[0]
        band = rows[a:b + 1]
        missed = [r for r in band if not r[1]]
        self.assertTrue(missed)
        self.assertTrue(all(abs(r[2]) <= 1.1 for r in missed))

    def test_control_aligned_by_epoch(self):
        dut = synthetic(160, 1.0, 3277, epoch=1000.0)
        # The control started 5 s later, so its host times read 5 s earlier;
        # it heard every FE-C frame.
        ctl = synthetic(160, None, 0, epoch=1005.0, power=False, shift=5.0)
        out = io.StringIO()
        x.analyse(dut, controls=[('ctl', ctl)], out=out)
        text = out.getvalue()
        self.assertIn('offset to DUT +5.000 s', text)
        self.assertRegex(text, r'control ctl: 0 / \d+ missed, longest run 0')
        line = [s for s in text.splitlines() if 'control ctl: 0 /' in s][0]
        self.assertGreater(int(line.split('/')[1].split()[0]), 15)

    def test_snapshot_delta_and_baseline(self):
        def pages(missed, collided):
            return {'0': {'fields': {'slots_missed': missed, 'slots_denied': 0,
                                     'sched_missed': 0, 'sched_denied': 0}},
                    '11': {'fields': {'slots_collided': collided, 'turn_yields': 0,
                                      'arm_rejected': 0, 'preempted': 0,
                                      'windows_merged': 0, 'replans': None}}}
        snaps = [(0.0, pages(0, 0)), (10.0, pages(2, 0)), (20.0, pages(15, 4)),
                 (30.0, pages(17, 4))]
        before, after, delta = x.snapshot_delta(snaps, 12.0, 18.0)
        self.assertEqual((before, after), (10.0, 20.0))
        self.assertEqual(delta['0']['slots_missed'], 13)
        self.assertEqual(delta['11']['slots_collided'], 4)
        self.assertNotIn('replans', delta['11'])
        rates, seconds = x.baseline_rate(snaps, [(12.0, 18.0)])
        self.assertEqual(seconds, 20.0)
        self.assertAlmostEqual(rates['0']['slots_missed'], 0.2)
        self.assertIsNone(x.snapshot_delta(snaps, -1.0, 5.0))

    def test_counter_wrap(self):
        b = {'0': {'fields': {'slots_missed': 0xFFFFFFFE}}}
        a = {'0': {'fields': {'slots_missed': 1}}}
        self.assertEqual(x._delta(b, a)['0']['slots_missed'], 3)

    def test_parse_reads_header_and_diag(self):
        lines = ['# ant_crossing epoch=12.5 channels=fec target=COM11 label=-',
                 '# diag 3.250 ' + json.dumps({'0': {'fields': {}}}),
                 rx_line(1.0, 3, 100), '# end commands sent=0']
        epoch, snaps, events = x.parse(lines)
        self.assertEqual(epoch, 12.5)
        self.assertEqual(snaps[0][0], 3.25)
        self.assertEqual(len(events), 1)

    def test_grade_page(self):
        self.assertEqual(x.grade_page(0.0).hex(), '33ffffffff204eff')
        self.assertEqual(x.grade_page(1.5)[5:7], (20150).to_bytes(2, 'little'))

    def test_logged_device_logs_writes(self):
        from ant_wire import frame
        log = x.Log(None)

        class Sink:
            def write(self, ep, data):
                return len(data)
        dev = x.LoggedDevice(Sink(), log)
        dev.write(1, frame(w.MESG_OPEN_CHANNEL_ID, b'\x03'))
        self.assertTrue(log.lines[-1].endswith(
            f'host->dev  {dp.MESSAGE_NAMES[w.MESG_OPEN_CHANNEL_ID]} 03'))


if __name__ == '__main__':
    unittest.main()
