# SPDX-License-Identifier: Apache-2.0
"""tools/ant_record.py parsing, and ant_sim.py --replay, without a radio."""
from __future__ import annotations

import os
import sys
import tempfile
import unittest

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])

import ant_pages as ap  # noqa: E402
import ant_record as ar  # noqa: E402


class TestParseRx(unittest.TestCase):
    PAYLOAD = bytes.fromhex("30050a1b000000ff")

    def test_plain_broadcast(self):
        rx = ar.parse_rx(0x4E, bytes([2]) + self.PAYLOAD)
        self.assertEqual((rx.channel, rx.kind, rx.payload), (2, "B",
                                                             self.PAYLOAD))
        self.assertIsNone(rx.device_number)
        self.assertIsNone(rx.rssi)

    def test_all_ext_fields_dbm(self):
        # flag 0xE0: id(4) rssi type 0x20 (3) timestamp(2)
        body = (bytes([0]) + self.PAYLOAD + bytes([0xE0])
                + bytes([0x34, 0x12, 0x28, 0x25])
                + bytes([0x20, 0xC3, 0xB0]) + bytes([0x10, 0x27]))
        rx = ar.parse_rx(0x4F, body)
        self.assertEqual(rx.kind, "A")
        self.assertEqual((rx.device_number, rx.device_type, rx.trans_type),
                         (0x1234, 0x28, 0x25))
        self.assertEqual(rx.rssi, -61)
        self.assertEqual(rx.rx_ticks, 0x2710)

    def test_garmin_four_byte_rssi_keeps_the_timestamp_aligned(self):
        body = (bytes([0]) + self.PAYLOAD + bytes([0xE0])
                + bytes([0x34, 0x12, 0x28, 0x05])
                + bytes([0x10, 0x55, 0x68, 0x00]) + bytes([0x10, 0x27]))
        rx = ar.parse_rx(0x4E, body)
        self.assertIsNone(rx.rssi)      # type 0x10 is not dBm
        self.assertEqual(rx.rx_ticks, 0x2710)

    def test_burst_channel_strips_sequence_bits(self):
        rx = ar.parse_rx(0x50, bytes([0xA3]) + self.PAYLOAD)
        self.assertEqual((rx.channel, rx.kind), (3, "S"))

    def test_non_data_is_none(self):
        self.assertIsNone(ar.parse_rx(0x40, bytes([0, 1, 3])))
        self.assertIsNone(ar.parse_rx(0x4E, bytes(5)))


class TestSendSpec(unittest.TestCase):
    def test_raw_hex_acked(self):
        s = ar.parse_send("02fdffffffffffff@20")
        self.assertEqual(s.payload, bytes.fromhex("02fdffffffffffff"))
        self.assertTrue(s.acked)
        self.assertEqual((s.at, s.chan_index), (20.0, 0))

    def test_page70_forms(self):
        s = ar.parse_send("p70:81@5#1")
        self.assertEqual(s.payload, bytes([0x46, 0xFF, 0xFF, 0xFF, 0xFF, 4,
                                           81, 1]))
        self.assertEqual((s.at, s.chan_index), (5.0, 1))
        s = ar.parse_send("p70:0x52:2:ack")
        self.assertEqual(s.payload[5], 0x82)
        self.assertEqual(s.payload[6], 0x52)
        self.assertIsNone(s.at)

    def test_broadcast_prefix(self):
        self.assertFalse(ar.parse_send("b:p70:80").acked)

    def test_wrong_length_is_refused(self):
        with self.assertRaises(ValueError):
            ar.parse_send("0102")

    def test_chan_spec(self):
        self.assertEqual(ar.parse_chan("0x28:0:0"), (0x28, 0, 0, None))
        self.assertEqual(ar.parse_chan("35:123:5:4084"), (35, 123, 5, 4084))
        with self.assertRaises(ValueError):
            ar.parse_chan("0x28:0")


class TestTickClock(unittest.TestCase):
    def test_refines_jittered_host_time_and_unwraps(self):
        clock = ar.TickClock()
        period = 4084
        raw, out = 60000, []
        for i in range(40):           # crosses the 16-bit wrap several times
            host = 1.0 + i * period / 32768 + (0.008 if i % 3 else 0.0)
            out.append(clock.t(host, raw & 0xFFFF))
            raw += period
        steps = [b - a for a, b in zip(out, out[1:], strict=False)]
        for step in steps:
            self.assertAlmostEqual(step, period / 32768, places=6)

    def test_gap_longer_than_a_wrap(self):
        clock = ar.TickClock()
        clock.t(0.0, 0)
        # 2.5 s later: raw step is 2.5*32768 mod 65536
        t = clock.t(2.5, int(2.5 * 32768) & 0xFFFF)
        self.assertAlmostEqual(t, 2.5, places=4)

    def test_no_timestamp_is_host_time(self):
        self.assertEqual(ar.TickClock().t(3.25, None), 3.25)


class TestReplay(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp(suffix=".antcap")
        os.close(fd)

    def tearDown(self):
        os.unlink(self.path)

    def test_replay_plays_master_frames_in_order_and_fills_gaps(self):
        import ant_sim
        p = 4084 / 32768
        recs = [(0 * p, 0x28, 77, bytes([0x30] + [1] * 7), "B", -50),
                (1 * p, 0x28, 77, bytes([0x30] + [2] * 7), "B", -50),
                (1.1 * p, 0x28, 77, bytes([0x02] + [0xFF] * 7), "a", None),
                (4 * p, 0x28, 77, bytes([0x01] + [3] * 7), "B", -51),
                (2 * p, 0x23, 9, bytes([0x01] + [9] * 7), "B", -40)]
        ap.write_capture(self.path, recs)
        sensor = ant_sim.ReplaySensor(0, 5, self.path, period=4084)
        self.assertEqual((sensor.device_type, sensor.device_number), (0x28, 77))
        got = [sensor.next_payload()[1] for _ in range(6)]
        # slot 0, 1, then 2 missed slots repeat payload 2, then 3, then loop.
        self.assertEqual(got, [1, 2, 2, 2, 3, 1])

    def test_replay_device_selection_and_dry_run(self):
        import ant_sim
        recs = [(i * 0.25, 0x23, 9, bytes([1, i] + [0] * 6)) for i in range(4)]
        recs += [(i * 0.125, 0x28, 77, bytes([0x30, i] + [0] * 6))
                 for i in range(10)]
        ap.write_capture(self.path, recs)
        sensor = ant_sim.ReplaySensor(0, 5, self.path, device=(0x23, 9),
                                      period=8192)
        out = ant_sim.dry_run([sensor], 1.0)
        self.assertEqual([r[3][1] for r in out], [0, 1, 2, 3])
        self.assertTrue(all(r[1] == 0x23 and r[2] == 9 for r in out))


class TestPromiscConvert(unittest.TestCase):
    """promisc_log.py: sniffer P lines -> v2 records with direction."""

    @staticmethod
    def p(t, b3, pl="0102030405060708", dn=1234, ty="28", crc="OK", dr=0):
        return (f"P n=1 dr={dr} q=0 t={t:010d} dt=   +0 {crc} m=5 rssi=-50 "
                f"dn={dn} ty={ty} tt=5 B3={b3} pl={pl} crc=0000 air=100")

    def test_master_broadcast_slave_acked_and_ack(self):
        import promisc_log as pl
        lines = [self.p(1000, "0A"), self.p(3200, "A2", "02fdffffffffffff"),
                 self.p(5000, "F2"), self.p(125600, "0A"),
                 self.p(127800, "02")]
        recs, notes, st = pl.convert(lines)
        self.assertEqual([r[4] for r in recs], ["B", "a", "B", "b"])
        self.assertEqual(recs[1][3], bytes.fromhex("02fdffffffffffff"))
        self.assertAlmostEqual(recs[1][0], 0.0022)
        self.assertEqual(st["acks"], 1)
        self.assertTrue(any(n.startswith("ACK ") for n in notes))

    def test_master_burst_continuation_is_master(self):
        import promisc_log as pl
        lines = [self.p(0, "8A"), self.p(2000, "D2"), self.p(4200, "92"),
                 self.p(6000, "F2"), self.p(8400, "A2")]
        recs, _, _ = pl.convert(lines)
        self.assertEqual([r[4] for r in recs], ["S", "S", "S"])

    def test_master_acked_is_A_and_crc_errors_dropped(self):
        import promisc_log as pl
        recs, _, st = pl.convert([self.p(0, "AA"), self.p(10, "0A", crc="ER"),
                                  self.p(20, "0A", dr=2)])
        self.assertEqual([r[4] for r in recs], ["A", "B"])
        self.assertEqual((st["crc_err"], st["max_dr"]), (1, 2))

    def test_host_prefix_and_mark_mapping(self):
        import promisc_log as pl
        lines = ["10.000 " + self.p(0, "0A"), "# MARK host 10.500 pass",
                 "10.600 " + self.p(600000, "0A")]
        recs, notes, _ = pl.convert(lines)
        self.assertEqual(len(recs), 2)
        self.assertIn("MARK 0.500000 pass", notes)


if __name__ == "__main__":
    unittest.main()
