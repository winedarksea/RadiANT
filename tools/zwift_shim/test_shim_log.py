# SPDX-License-Identifier: Apache-2.0
"""Stress test for the Zwift shim's log: atomic lines across threads and DLLs.

Loads both built shim DLLs from tools/zwift_shim/out/ in a child process and
hammers them from 8 threads at once (ctypes releases the GIL around foreign
calls, so the calls really overlap). Then checks that every line of the log
is whole: one prefix per line, no blank or glued lines, an exact line count,
and QPC time that never runs backwards within a thread.

Build the DLLs first:  scripts/zwift_ant_shim.ps1 -BuildOnly
Run:  C:/ncs/toolchains/dcbdc366a1/opt/bin/python.exe tools/zwift_shim/test_shim_log.py
"""
import ctypes
import os
import re
import subprocess
import sys
import tempfile
import threading
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'out')
XP_DLL = os.path.join(OUT, 'DSI_SiUSBXp_3_1.DLL')
CP_DLL = os.path.join(OUT, 'DSI_CP210xManufacturing_3_1.dll')

THREADS = 8
CALLS = 2000
PREFIX = re.compile(r'^(\d+)\.(\d{6}) t(\d+) (usbxp|cp210x) \S')


def child():
    """Run in the subprocess: load both DLLs and call them from THREADS."""
    xp = ctypes.WinDLL(XP_DLL)
    cp = ctypes.WinDLL(CP_DLL)
    start = threading.Barrier(THREADS)

    def worker(k):
        n = ctypes.c_ulong()
        vid = ctypes.c_ushort()
        start.wait()
        for i in range(CALLS):
            if (i + k) % 2:
                xp.SI_GetNumDevices(ctypes.byref(n))
            else:
                cp.CP210x_GetDeviceVid(None, ctypes.byref(vid))

    ts = [threading.Thread(target=worker, args=(k,)) for k in range(THREADS)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()


@unittest.skipUnless(sys.platform == 'win32', 'Windows DLLs')
@unittest.skipUnless(os.path.exists(XP_DLL) and os.path.exists(CP_DLL),
                     'shim not built: run scripts/zwift_ant_shim.ps1 -BuildOnly')
class ShimLogTest(unittest.TestCase):
    def test_lines_atomic_under_threads(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = os.path.join(tmp, 'shim.log')
            env = dict(os.environ,
                       RADIANT_SHIM_LOG=log,
                       RADIANT_ANT_COM='COM250',
                       RADIANT_SHIM_LOG_LEVEL='2')
            subprocess.run([sys.executable, os.path.abspath(__file__),
                            '--child'], env=env, check=True, timeout=300)
            with open(log, 'rb') as f:
                raw = f.read()

        self.assertTrue(raw.endswith(b'\r\n'))
        lines = raw.decode('ascii').split('\r\n')[:-1]
        self.assertNotIn('', lines, 'blank line')
        self.assertNotIn('\n', raw.decode('ascii').replace('\r\n', ''),
                         'bare LF')
        bad = [l for l in lines if not PREFIX.match(l)]
        self.assertEqual(bad, [], 'lines without the prefix')
        glued = [l for l in lines
                 if len(re.findall(r'\d+\.\d{6} t\d+ (?:usbxp|cp210x) ', l)) != 1]
        self.assertEqual(glued, [], 'glued lines')

        # Two "loaded" banners, one port-source line, one line per call.
        self.assertEqual(len(lines), 2 + 1 + THREADS * CALLS)
        self.assertEqual(sum('shim loaded: pid' in l for l in lines), 2)
        self.assertEqual(
            [l.split(' ', 3)[3] for l in lines if ' port COM250 ' in l],
            ['port COM250 (from RADIANT_ANT_COM)'])
        absent = [l for l in lines if 'SI_GetNumDevices' in l]
        self.assertEqual(len(absent), THREADS * CALLS // 2)
        for l in absent:
            self.assertIn('-> 0 (COM250 absent, err=2 FILE_NOT_FOUND', l)
        self.assertEqual(sum('CP210x_GetDeviceVid -> 0FCF' in l for l in lines),
                         THREADS * CALLS // 2)

        last = {}
        for l in lines:
            m = PREFIX.match(l)
            t = (int(m.group(1)), int(m.group(2)))
            tid = m.group(3)
            self.assertGreaterEqual(t, last.get(tid, t), l)
            last[tid] = t
        self.assertGreaterEqual(len(last), THREADS)


if __name__ == '__main__':
    if sys.argv[1:] == ['--child']:
        child()
    else:
        unittest.main()
