# SPDX-License-Identifier: Apache-2.0
"""Tests for the Zwift shim's log: atomic lines, and no key in a write dump.

Loads both built shim DLLs from tools/zwift_shim/out/ in a child process and
hammers them from 8 threads at once (ctypes releases the GIL around foreign
calls, so the calls really overlap). Then checks that every line of the log
is whole: one prefix per line, no blank or glued lines, an exact line count,
and QPC time that never runs backwards within a thread.

The second test opens the USBXpress shim on a named pipe instead of a COM
port (RADIANT_ANT_COM names the pipe, which the shim opens as it would a
COM port), writes key-carrying frames at log level 2 and checks that the log
holds none of the key bytes while the pipe received every byte unchanged.

Build the DLLs first:  scripts/zwift_ant_shim.ps1 -BuildOnly
Run:  C:/ncs/toolchains/dcbdc366a1/opt/bin/python.exe tools/zwift_shim/test_shim_log.py
"""
import ctypes
import ctypes.wintypes
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

# A key whose bytes appear nowhere else in the writes below, so a key byte
# anywhere in a dump is a leak. (The 0x46 frame's checksum happens to be 0xE3,
# one of them, so a leaked checksum is caught the same way.)
KEY8 = bytes(range(0xE1, 0xE9))
KEY16 = bytes(range(0xE1, 0xF1))
KEY_TOKENS = {'%02X' % b for b in KEY16}


def frame(msg_id, body):
    """A4 len id body... xor"""
    f = bytes([0xA4, len(body), msg_id]) + bytes(body)
    x = 0
    for b in f:
        x ^= b
    return f + bytes([x])


FREQ = frame(0x45, [0x00, 0x39])
OPEN = frame(0x4B, [0x00])
NETKEY = frame(0x46, b'\x00' + KEY8)
WRITES = [
    NETKEY,                                     # alone
    FREQ + NETKEY + OPEN,                       # coalesced
    frame(0x7E, b'\x03' + KEY16),               # ANT encryption key
    frame(0xF2, b'\x01\x80' + KEY16),           # RadiANT root key
    frame(0xF5, b'\x00\x02' + KEY16 + KEY16),   # pairing: the scalar
    FREQ + NETKEY[:7],                          # a key frame cut short
    b'\x00' + KEY8,                             # not 0xA4-framed at all
]


def child_write():
    """Run in the subprocess: SI_Open on a named pipe, then WRITES."""
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.CreateNamedPipeW.restype = ctypes.wintypes.HANDLE
    k32.CreateNamedPipeW.argtypes = [
        ctypes.wintypes.LPCWSTR, ctypes.wintypes.DWORD, ctypes.wintypes.DWORD,
        ctypes.wintypes.DWORD, ctypes.wintypes.DWORD, ctypes.wintypes.DWORD,
        ctypes.wintypes.DWORD, ctypes.c_void_p]
    k32.ReadFile.argtypes = [
        ctypes.wintypes.HANDLE, ctypes.c_void_p, ctypes.wintypes.DWORD,
        ctypes.POINTER(ctypes.wintypes.DWORD), ctypes.c_void_p]
    pipe = k32.CreateNamedPipeW('\\\\.\\' + os.environ['RADIANT_ANT_COM'],
                                3,          # PIPE_ACCESS_DUPLEX: the shim opens R+W
                                0, 1, 65536, 65536, 0, None)
    if pipe is None or pipe == ctypes.wintypes.HANDLE(-1).value:
        raise OSError(ctypes.get_last_error(), 'CreateNamedPipeW')

    xp = ctypes.WinDLL(XP_DLL)
    h = ctypes.c_void_p()
    if xp.SI_Open(0, ctypes.byref(h)) != 0:
        raise RuntimeError('SI_Open failed')
    for w in WRITES:
        put = ctypes.c_ulong()
        buf = ctypes.create_string_buffer(w, len(w))
        if (xp.SI_Write(h, buf, len(w), ctypes.byref(put), None) != 0
                or put.value != len(w)):
            raise RuntimeError('SI_Write failed')
    xp.SI_Close(h)

    got = b''
    chunk = ctypes.create_string_buffer(65536)
    n = ctypes.wintypes.DWORD()
    while (k32.ReadFile(pipe, chunk, len(chunk), ctypes.byref(n), None)
           and n.value):
        got += chunk.raw[:n.value]
    sys.stdout.write(got.hex())


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


@unittest.skipUnless(sys.platform == 'win32', 'Windows DLLs')
@unittest.skipUnless(os.path.exists(XP_DLL),
                     'shim not built: run scripts/zwift_ant_shim.ps1 -BuildOnly')
class ShimWriteDumpTest(unittest.TestCase):
    def test_level2_write_dump_holds_no_key(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = os.path.join(tmp, 'shim.log')
            env = dict(os.environ,
                       RADIANT_SHIM_LOG=log,
                       RADIANT_ANT_COM='pipe\\radiant_shim_test_%d' % os.getpid(),
                       RADIANT_SHIM_LOG_LEVEL='2')
            r = subprocess.run([sys.executable, os.path.abspath(__file__),
                                '--child-write'], env=env, check=True,
                               timeout=60, capture_output=True, text=True)
            with open(log, 'rb') as f:
                lines = f.read().decode('ascii').split('\r\n')[:-1]

        # The wire is untouched: the redaction is for the log only.
        self.assertEqual(bytes.fromhex(r.stdout), b''.join(WRITES))

        dumps = [l.split(' SI_Write ', 1)[1] for l in lines
                 if ' SI_Write ' in l]
        self.assertEqual(len(dumps), len(WRITES), lines)
        for d in dumps:
            leaked = KEY_TOKENS & set(d.replace(':', ' ').split())
            self.assertEqual(leaked, set(), d)

        def mask(n):
            return ' '.join(['..'] * n)
        net = 'A4 09 46 00 ' + mask(9)
        freq = 'A4 02 45 00 39 DA'
        self.assertTrue(dumps[0].endswith('id=0x46: ' + net), dumps[0])
        self.assertTrue(dumps[1].endswith(
            'id=0x45,0x46,0x4B: %s %s A4 01 4B 00 EE' % (freq, net)), dumps[1])
        self.assertTrue(dumps[2].endswith(': A4 11 7E 03 ' + mask(17)),
                        dumps[2])
        self.assertTrue(dumps[3].endswith(': A4 12 F2 01 80 ' + mask(17)),
                        dumps[3])
        self.assertTrue(dumps[4].endswith(': A4 22 F5 ' + mask(35)), dumps[4])
        self.assertTrue(dumps[5].endswith(
            ': %s (+7 bytes not whole frames)' % freq), dumps[5])
        self.assertTrue(dumps[6].endswith(': (+9 bytes not whole frames)'),
                        dumps[6])


if __name__ == '__main__':
    if sys.argv[1:] == ['--child']:
        child()
    elif sys.argv[1:] == ['--child-write']:
        child_write()
    else:
        unittest.main()
