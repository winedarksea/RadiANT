# SPDX-License-Identifier: Apache-2.0
"""Tests for the Zwift shim: atomic log lines, no key in a read or write
dump, port selection, and the port handle's locking.

Loads both built shim DLLs from tools/zwift_shim/out/ in a child process and
hammers them from 8 threads at once (ctypes releases the GIL around foreign
calls, so the calls really overlap). Then checks that every line of the log
is whole: one prefix per line, no blank or glued lines, an exact line count,
and QPC time that never runs backwards within a thread.

The second test opens the USBXpress shim on a named pipe instead of a COM
port (RADIANT_ANT_COM names the pipe, which the shim opens as it would a
COM port), writes key-carrying frames at log level 2 and checks that the log
holds none of the key bytes while the pipe received every byte unchanged.
ShimReadDumpTest does the same for reads: a device that echoes (a console on
another port) must not put a host's key into the log. Pipe tests load
out/test/, a build with SHIM_TEST_PIPES; the installed shim opens COM1..COM999
only, which ShimSelectTest checks along with "no selection, no device".

ShimLockTest races SI_Open from 8 threads on a one-instance pipe (a failed
open used to overwrite the winner's handle) and closes the port under a
pending read.

ShimOverlappedTest checks, on a named pipe, that an SI_Write is not held
behind a pending SI_Read (the 2026-11-23 TI/Zwift stall) and that a read
nobody answers ends at the shim's backstop as a timeout. ShimHardwareTest
repeats the first on a dongle when RADIANT_SHIM_TEST_COM=COMn is set.

Build the DLLs first:  scripts/zwift_ant_shim.ps1 -BuildOnly
The tests refuse to run (setUpModule fails) when a source file is newer than
the DLLs in out/, or a DLL lacks the shim's version resource: a stale out/
once let these tests pass on a build without the key masking. Set
RADIANT_SHIM_OUT=<dir> to test DLLs elsewhere (that dir and its test/).
Run:  C:/ncs/toolchains/dcbdc366a1/opt/bin/python.exe tools/zwift_shim/test_shim_log.py
"""
import ctypes
import ctypes.wintypes
import glob
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.environ.get('RADIANT_SHIM_OUT') or os.path.join(HERE, 'out')
XP_DLL = os.path.join(OUT, 'DSI_SiUSBXp_3_1.DLL')
CP_DLL = os.path.join(OUT, 'DSI_CP210xManufacturing_3_1.dll')
# Built with SHIM_TEST_PIPES: also opens pipe\radiant_shim_*. Never installed.
XP_TEST_DLL = os.path.join(OUT, 'test', 'DSI_SiUSBXp_3_1.DLL')
PRODUCT_NAME = 'RadiANT Zwift ANT shim'     # shim_version.h


def product_name(path):
    """The ProductName in a DLL's version resource, read without loading
    it, or None."""
    ver = ctypes.WinDLL('version')
    size = ver.GetFileVersionInfoSizeW(path, None)
    if not size:
        return None
    buf = ctypes.create_string_buffer(size)
    if not ver.GetFileVersionInfoW(path, 0, size, buf):
        return None
    val = ctypes.c_void_p()
    n = ctypes.c_uint()
    if not ver.VerQueryValueW(buf, '\\StringFileInfo\\040904b0\\ProductName',
                              ctypes.byref(val), ctypes.byref(n)) or not n.value:
        return None
    return ctypes.wstring_at(val.value, n.value).rstrip('\0')


def out_problem():
    """Why the DLLs in OUT must not be tested, or None."""
    dlls = [XP_DLL, CP_DLL, XP_TEST_DLL]
    present = [d for d in dlls if os.path.exists(d)]
    if not present:
        return None                 # not built: the classes skip
    if len(present) < len(dlls):
        return ('%s is incomplete (%s missing): rebuild with '
                'scripts/zwift_ant_shim.ps1 -BuildOnly'
                % (OUT, ', '.join(os.path.relpath(d, OUT) for d in dlls
                                  if d not in present)))
    for d in dlls:
        if product_name(d) != PRODUCT_NAME:
            return ('%s has no %r version resource: an old build. Rebuild '
                    'with scripts/zwift_ant_shim.ps1 -BuildOnly' % (d, PRODUCT_NAME))
    if os.environ.get('RADIANT_SHIM_OUT'):
        return None
    sources = [f for ext in ('*.c', '*.h', '*.rc')
               for f in glob.glob(os.path.join(HERE, ext))]
    newest = max(sources, key=os.path.getmtime)
    oldest = min(dlls, key=os.path.getmtime)
    if os.path.getmtime(newest) > os.path.getmtime(oldest):
        return ('%s is newer than %s: out/ is stale. Rebuild with '
                'scripts/zwift_ant_shim.ps1 -BuildOnly'
                % (os.path.basename(newest), os.path.relpath(oldest, HERE)))
    return None


def setUpModule():
    if sys.platform != 'win32':
        return
    problem = out_problem()
    if problem:
        raise RuntimeError(problem)

THREADS = 8
CALLS = 2000
ROUNDS = 40
PREFIX = re.compile(r'^(\d+)\.(\d{6}) t(\d+) (usbxp|cp210x) \S')

# A key whose bytes appear nowhere else in the writes below (no header and no
# unmasked checksum among 0xC1-0xD0), so a key byte anywhere in a dump is a
# leak. A leaked checksum is caught by the exact dumps asserted at the end.
KEY8 = bytes(range(0xC1, 0xC9))
KEY16 = bytes(range(0xC1, 0xD1))
KEY_TOKENS = {'%02X' % b for b in KEY16}
# A key byte in any spelling: a bare token, 0xC1, or inside a word.
KEY_RE = re.compile(r'(?i)(?<![0-9A-F])(?:%s)(?![0-9A-F])'
                    % '|'.join(sorted(KEY_TOKENS)))


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
    NETKEY[:4],                                 # a 0x46 split in two writes:
    NETKEY[4:],                                 # the second starts on the key
    bytes([0xA4, 0x08, 0x45, 0x00, 0x39]) + NETKEY,  # a LEN that lies
    bytes([0xA4, 0x03, 0x4E]) + KEY8[:4],       # key bytes that frame by luck
    frame(0x7F, b'\x02' + KEY16),               # set-info: an RNG seed
    frame(0x7F, b'\x00\x11\x22\x33\x44'),       # set-info: a crypto id
    frame(0x76, b'\x01' + KEY16),               # 128-bit network key
    frame(0x83, b'\x01\x02' + KEY16),           # NVM crypto key: store
    frame(0x83, b'\x00\x01\x00'),               # NVM crypto key: load
]

PLAIN = frame(0x40, [0x00, 0x46, 0x00])         # a channel response
BAD = PLAIN[:-1] + bytes([PLAIN[-1] ^ 0x01])     # its checksum broken
# A 0x7E whose LEN lies short: the failed "frame" it claims ends inside the
# key, and the key holds an 0xA4 that starts a frame which checks by luck.
LUCKY = (bytes([0xA4, 0x05, 0x7E, 0x00]) + bytes([0xC1, 0xC2, 0xC3, 0xC4])
         + frame(0x4E, [0xC5, 0xC6]))
# What a device that echoes would send back: whole frames, coalesced ones,
# a key frame cut across two reads, junk, and a frame that fails its
# checksum just before a key.
READS = [
    PLAIN,
    NETKEY,
    PLAIN + NETKEY + PLAIN,
    frame(0x76, b'\x01' + KEY16),
    frame(0x83, b'\x01\x02' + KEY16),
    frame(0xF5, b'\x00\x03' + KEY16 + KEY16),
    NETKEY[:7],
    NETKEY[7:],
    b'\x00\x11' + PLAIN,
    BAD + NETKEY,
    bytes([0xA4, 0xF0]) + PLAIN,
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

    xp = ctypes.WinDLL(XP_TEST_DLL)
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


def make_pipe(k32):
    """The server end of the named pipe RADIANT_ANT_COM names (blocking)."""
    k32.CreateNamedPipeW.restype = ctypes.wintypes.HANDLE
    k32.CreateNamedPipeW.argtypes = [
        ctypes.wintypes.LPCWSTR, ctypes.wintypes.DWORD, ctypes.wintypes.DWORD,
        ctypes.wintypes.DWORD, ctypes.wintypes.DWORD, ctypes.wintypes.DWORD,
        ctypes.wintypes.DWORD, ctypes.c_void_p]
    k32.ReadFile.argtypes = [
        ctypes.wintypes.HANDLE, ctypes.c_void_p, ctypes.wintypes.DWORD,
        ctypes.POINTER(ctypes.wintypes.DWORD), ctypes.c_void_p]
    k32.WriteFile.argtypes = k32.ReadFile.argtypes
    pipe = k32.CreateNamedPipeW('\\\\.\\' + os.environ['RADIANT_ANT_COM'],
                                3, 0, 1, 65536, 65536, 0, None)
    if pipe is None or pipe == ctypes.wintypes.HANDLE(-1).value:
        raise OSError(ctypes.get_last_error(), 'CreateNamedPipeW')
    return pipe


def child_write_vs_read():
    """Run in the subprocess: a thread waits in SI_Read(1) on a silent pipe;
    100 ms later the main thread calls SI_Write. Before the shim used
    overlapped I/O, the write queued behind the read on the one synchronous
    handle, and on a pipe (which has no read timeout) waited forever. Prints
    JSON: the write's duration, the bytes the pipe got, the byte the reader
    got."""
    import json
    import time
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    pipe = make_pipe(k32)
    xp = ctypes.WinDLL(XP_TEST_DLL)
    h = ctypes.c_void_p()
    if xp.SI_Open(0, ctypes.byref(h)) != 0:
        raise RuntimeError('SI_Open failed')
    xp.SI_SetTimeouts(1000, 1000)

    got = {}

    def reader():
        b = ctypes.create_string_buffer(1)
        n = ctypes.c_ulong()
        rc = xp.SI_Read(h, b, 1, ctypes.byref(n), None)
        got['rc'], got['byte'] = rc, b.raw[:n.value].hex()

    t = threading.Thread(target=reader)
    t.start()
    time.sleep(0.1)
    put = ctypes.c_ulong()
    buf = ctypes.create_string_buffer(OPEN, len(OPEN))
    w0 = time.perf_counter()
    rc = xp.SI_Write(h, buf, len(OPEN), ctypes.byref(put), None)
    write_ms = (time.perf_counter() - w0) * 1000
    if rc != 0 or put.value != len(OPEN):
        raise RuntimeError('SI_Write failed')

    one = ctypes.create_string_buffer(b'\x5a', 1)
    n = ctypes.wintypes.DWORD()
    k32.WriteFile(pipe, one, 1, ctypes.byref(n), None)
    t.join()

    wire = b''
    chunk = ctypes.create_string_buffer(len(OPEN))
    while len(wire) < len(OPEN):
        if not k32.ReadFile(pipe, chunk, len(OPEN) - len(wire),
                            ctypes.byref(n), None) or not n.value:
            break
        wire += chunk.raw[:n.value]
    xp.SI_Close(h)
    sys.stdout.write(json.dumps({'write_ms': write_ms, 'wire': wire.hex(),
                                 'read_rc': got['rc'],
                                 'read_byte': got['byte']}))


def child_silent_read():
    """Run in the subprocess: SI_Read(1) on a silent pipe with a 200 ms read
    timeout. A pipe has no COMMTIMEOUTS, so only the shim's own backstop can
    end the read. Prints JSON: rc, bytes, duration."""
    import json
    import time
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    pipe = make_pipe(k32)
    xp = ctypes.WinDLL(XP_TEST_DLL)
    h = ctypes.c_void_p()
    if xp.SI_Open(0, ctypes.byref(h)) != 0:
        raise RuntimeError('SI_Open failed')
    xp.SI_SetTimeouts(200, 200)
    b = ctypes.create_string_buffer(1)
    n = ctypes.c_ulong(99)
    r0 = time.perf_counter()
    rc = xp.SI_Read(h, b, 1, ctypes.byref(n), None)
    read_ms = (time.perf_counter() - r0) * 1000
    xp.SI_Close(h)
    k32.CloseHandle(ctypes.wintypes.HANDLE(pipe))
    sys.stdout.write(json.dumps({'rc': rc, 'n': n.value, 'read_ms': read_ms}))


def child_read():
    """Run in the subprocess: the pipe plays a device that echoes; each of
    READS is written to it and then read back through SI_Read, in reads of
    RADIANT_SHIM_TEST_READ bytes (default 4096; ANT_DLL reads 1). Prints the
    hex of what SI_Read returned."""
    size = int(os.environ.get('RADIANT_SHIM_TEST_READ', '4096'))
    reads = READS
    if os.environ.get('RADIANT_SHIM_TEST_READS'):
        reads = [bytes.fromhex(x)
                 for x in os.environ['RADIANT_SHIM_TEST_READS'].split(',')]
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    pipe = make_pipe(k32)
    xp = ctypes.WinDLL(XP_TEST_DLL)
    h = ctypes.c_void_p()
    if xp.SI_Open(0, ctypes.byref(h)) != 0:
        raise RuntimeError('SI_Open failed')
    xp.SI_SetTimeouts(200, 200)
    got = b''
    buf = ctypes.create_string_buffer(4096)
    n = ctypes.wintypes.DWORD()
    for r in reads:
        k32.WriteFile(pipe, ctypes.create_string_buffer(r, len(r)), len(r),
                      ctypes.byref(n), None)
        want = len(got) + len(r)
        while len(got) < want:
            m = ctypes.c_ulong()
            if xp.SI_Read(h, buf, size, ctypes.byref(m), None) != 0:
                raise RuntimeError('SI_Read failed')
            if not m.value:
                raise RuntimeError('SI_Read timed out')
            got += buf.raw[:m.value]
    xp.SI_Close(h)
    sys.stdout.write(got.hex())


def child_select():
    """Run in the subprocess, with whatever selection the parent set: the
    release DLL's answer. Prints JSON: SI_GetNumDevices' count, SI_Open's
    status."""
    xp = ctypes.WinDLL(XP_DLL)
    n = ctypes.c_ulong(99)
    xp.SI_GetNumDevices(ctypes.byref(n))
    h = ctypes.c_void_p()
    rc = xp.SI_Open(0, ctypes.byref(h))
    if rc == 0:
        xp.SI_Close(h)
    sys.stdout.write(json.dumps({'num': n.value, 'open_rc': rc}))


def child_zero_timeout():
    """Run in the subprocess: SI_SetTimeouts(0, ...) on a silent pipe. The
    read must still end (0 used to mean "wait for a first byte forever").
    Prints JSON: the timeout read back, rc, bytes, duration."""
    import time
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    pipe = make_pipe(k32)
    xp = ctypes.WinDLL(XP_TEST_DLL)
    h = ctypes.c_void_p()
    if xp.SI_Open(0, ctypes.byref(h)) != 0:
        raise RuntimeError('SI_Open failed')
    xp.SI_SetTimeouts(0, 200)
    r = ctypes.c_ulong()
    w = ctypes.c_ulong()
    xp.SI_GetTimeouts(ctypes.byref(r), ctypes.byref(w))
    b = ctypes.create_string_buffer(1)
    n = ctypes.c_ulong(99)
    r0 = time.perf_counter()
    rc = xp.SI_Read(h, b, 1, ctypes.byref(n), None)
    read_ms = (time.perf_counter() - r0) * 1000
    xp.SI_Close(h)
    k32.CloseHandle(ctypes.wintypes.HANDLE(pipe))
    sys.stdout.write(json.dumps({'r': r.value, 'w': w.value, 'rc': rc,
                                 'n': n.value, 'read_ms': read_ms}))


def child_open_race():
    """Run in the subprocess: ROUNDS times, a one-instance pipe and THREADS
    threads calling SI_Open at once. One CreateFile wins and the rest fail
    (ERROR_PIPE_BUSY); a loser must not clobber the winner's handle. Each
    round then writes through the shim and checks the pipe got it. Prints
    JSON: rounds where an SI_Open said ok and SI_Write then failed or the
    bytes did not arrive."""
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    xp = ctypes.WinDLL(XP_TEST_DLL)
    bad = []
    for rnd in range(ROUNDS):
        pipe = make_pipe(k32)
        start = threading.Barrier(THREADS)
        rcs = [None] * THREADS
        hs = [ctypes.c_void_p() for _ in range(THREADS)]

        def opener(k):
            start.wait()
            rcs[k] = xp.SI_Open(0, ctypes.byref(hs[k]))

        ts = [threading.Thread(target=opener, args=(k,))
              for k in range(THREADS)]
        for t in ts:
            t.start()
        for t in ts:
            t.join()
        put = ctypes.c_ulong()
        h = next((hs[k] for k in range(THREADS) if rcs[k] == 0), None)
        wrc = None
        wire = b''
        if h is not None:
            buf = ctypes.create_string_buffer(OPEN, len(OPEN))
            wrc = xp.SI_Write(h, buf, len(OPEN), ctypes.byref(put), None)
            if wrc == 0:
                chunk = ctypes.create_string_buffer(64)
                n = ctypes.wintypes.DWORD()
                if k32.ReadFile(pipe, chunk, len(OPEN), ctypes.byref(n), None):
                    wire = chunk.raw[:n.value]
            xp.SI_Close(h)
        if h is not None and (wrc != 0 or wire != OPEN):
            bad.append({'round': rnd, 'open_rcs': rcs, 'write_rc': wrc})
        k32.CloseHandle(ctypes.wintypes.HANDLE(pipe))
    sys.stdout.write(json.dumps({'bad': bad}))


def child_close_under_read():
    """Run in the subprocess: a thread waits in SI_Read(1) on a silent pipe
    with a 2000 ms timeout; 200 ms in, the main thread calls SI_Close. The
    close must not wait the read out, the read must end, and a write after
    the close must be refused, not sent to a dead (or reused) handle.
    Prints JSON."""
    import time
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    pipe = make_pipe(k32)
    xp = ctypes.WinDLL(XP_TEST_DLL)
    h = ctypes.c_void_p()
    if xp.SI_Open(0, ctypes.byref(h)) != 0:
        raise RuntimeError('SI_Open failed')
    xp.SI_SetTimeouts(2000, 2000)
    got = {}

    def reader():
        b = ctypes.create_string_buffer(1)
        n = ctypes.c_ulong()
        r0 = time.perf_counter()
        got['rc'] = xp.SI_Read(h, b, 1, ctypes.byref(n), None)
        got['read_ms'] = (time.perf_counter() - r0) * 1000

    t = threading.Thread(target=reader)
    t.start()
    time.sleep(0.2)
    c0 = time.perf_counter()
    xp.SI_Close(h)
    close_ms = (time.perf_counter() - c0) * 1000
    t.join(10)
    put = ctypes.c_ulong()
    buf = ctypes.create_string_buffer(OPEN, len(OPEN))
    wrc = xp.SI_Write(h, buf, len(OPEN), ctypes.byref(put), None)
    k32.CloseHandle(ctypes.wintypes.HANDLE(pipe))
    sys.stdout.write(json.dumps({'close_ms': close_ms, 'read_rc': got.get('rc'),
                                 'read_ms': got.get('read_ms'),
                                 'write_after_close_rc': wrc}))


def child_hw_capabilities():
    """Run in the subprocess, on the real port RADIANT_ANT_COM names: a
    thread keeps SI_Read(1) pending the way ANT_DLL does; after 2 s idle the
    main thread sends REQUEST(capabilities). Prints JSON: the write's
    duration and how long after it the CAPABILITIES reply was complete."""
    import json
    import time
    xp = ctypes.WinDLL(XP_DLL)
    h = ctypes.c_void_p()
    if xp.SI_Open(0, ctypes.byref(h)) != 0:
        raise RuntimeError('SI_Open failed')
    xp.SI_SetTimeouts(1000, 1000)
    xp.SI_FlushBuffers(h, 1, 1)
    rx = []                 # (perf_counter, byte)
    stop = threading.Event()

    def reader():
        b = ctypes.create_string_buffer(1)
        n = ctypes.c_ulong()
        while not stop.is_set():
            if xp.SI_Read(h, b, 1, ctypes.byref(n), None) != 0:
                return
            if n.value:
                rx.append((time.perf_counter(), b.raw[0]))

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    time.sleep(2.0)
    req = frame(0x4D, [0x00, 0x54])
    put = ctypes.c_ulong()
    buf = ctypes.create_string_buffer(req, len(req))
    w0 = time.perf_counter()
    rc = xp.SI_Write(h, buf, len(req), ctypes.byref(put), None)
    write_ms = (time.perf_counter() - w0) * 1000
    time.sleep(1.0)
    stop.set()
    t.join(3.0)
    xp.SI_Close(h)

    # The first whole CAPABILITIES (0x54) frame, timed by its last byte.
    reply_ms = None
    got = [b for _t, b in rx]
    i = 0
    while i + 3 < len(got):
        if got[i] != 0xA4:
            i += 1
            continue
        end = i + got[i + 1] + 4
        if end > len(got):
            break
        if got[i + 2] == 0x54:
            reply_ms = (rx[end - 1][0] - w0) * 1000
            break
        i = end
    sys.stdout.write(json.dumps({'write_rc': rc, 'write_ms': write_ms,
                                 'reply_ms': reply_ms}))


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
@unittest.skipUnless(os.path.exists(XP_TEST_DLL),
                     'shim not built: run scripts/zwift_ant_shim.ps1 -BuildOnly')
class ShimWriteDumpTest(unittest.TestCase):
    def run_writes(self, level):
        with tempfile.TemporaryDirectory() as tmp:
            log = os.path.join(tmp, 'shim.log')
            env = dict(os.environ,
                       RADIANT_SHIM_LOG=log,
                       RADIANT_ANT_COM='pipe\\radiant_shim_test_%d' % os.getpid(),
                       RADIANT_SHIM_LOG_LEVEL=level)
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
        leaks = ['write %d: %s' % (k, d) for k, d in enumerate(dumps)
                 if KEY_RE.search(d)]
        self.assertEqual(leaks, [], 'key bytes in the log')
        return dumps

    def test_level1_write_line_holds_no_key(self):
        dumps = self.run_writes('1')
        self.assertTrue(dumps[0].endswith(' id=0x46'), dumps[0])
        self.assertTrue(dumps[6].endswith(' unframed'), dumps[6])
        self.assertTrue(dumps[8].endswith(' unframed'), dumps[8])

    def test_level2_write_dump_holds_no_key(self):
        dumps = self.run_writes('2')

        def mask(n):
            return ' '.join(['..'] * n)
        net = 'A4 09 46 00 ' + mask(9)
        freq = 'A4 02 45 00 39 DA'
        crypto_id = frame(0x7F, b'\x00\x11\x22\x33\x44')
        self.assertTrue(dumps[0].endswith('id=0x46: ' + net), dumps[0])
        self.assertTrue(dumps[1].endswith(
            'id=0x45,0x46,0x4B: %s %s A4 01 4B 00 EE' % (freq, net)), dumps[1])
        self.assertTrue(dumps[2].endswith(': A4 11 7E 03 ' + mask(17)),
                        dumps[2])
        self.assertTrue(dumps[3].endswith(': A4 12 F2 01 80 ' + mask(17)),
                        dumps[3])
        self.assertTrue(dumps[4].endswith(': A4 22 F5 ' + mask(35)), dumps[4])
        self.assertTrue(dumps[5].endswith(
            ' id=0x45: %s (+7 bytes not whole frames)' % freq), dumps[5])
        for k, n in ((6, 9), (7, 4), (8, 9), (9, 18), (10, 7)):
            self.assertTrue(dumps[k].endswith(
                ' unframed: (+%d bytes not whole frames)' % n), dumps[k])
        self.assertTrue(dumps[11].endswith(
            ' id=0x7F: A4 11 7F 02 ' + mask(17)), dumps[11])
        self.assertTrue(dumps[12].endswith(
            ' id=0x7F: ' + ' '.join('%02X' % b for b in crypto_id)),
            dumps[12])
        self.assertTrue(dumps[13].endswith(' id=0x76: A4 11 76 01 ' + mask(17)),
                        dumps[13])
        self.assertTrue(dumps[14].endswith(
            ' id=0x83: A4 12 83 01 02 ' + mask(17)), dumps[14])
        # A load carries no key, but the operation byte does not decide.
        self.assertTrue(dumps[15].endswith(
            ' id=0x83: A4 03 83 00 01 ' + mask(2)), dumps[15])


def run_child(flag, port, timeout, level='1'):
    """Run this file with `flag` against `port`; (parsed stdout, log lines)."""
    import json
    with tempfile.TemporaryDirectory() as tmp:
        log = os.path.join(tmp, 'shim.log')
        env = dict(os.environ, RADIANT_SHIM_LOG=log, RADIANT_ANT_COM=port,
                   RADIANT_SHIM_LOG_LEVEL=level)
        r = subprocess.run([sys.executable, os.path.abspath(__file__), flag],
                           env=env, check=True, timeout=timeout,
                           capture_output=True, text=True)
        with open(log, 'rb') as f:
            lines = f.read().decode('ascii').split('\r\n')[:-1]
    return json.loads(r.stdout), lines


@unittest.skipUnless(sys.platform == 'win32', 'Windows DLLs')
@unittest.skipUnless(os.path.exists(XP_TEST_DLL),
                     'shim not built: run scripts/zwift_ant_shim.ps1 -BuildOnly')
class ShimOverlappedTest(unittest.TestCase):
    """2026-11-23 TI/Zwift session: with a synchronous handle every SI_Write
    waited for ANT_DLL's pending SI_Read, up to the 1000 ms read timeout."""

    def pipe(self, tag):
        return 'pipe\\radiant_shim_%s_%d' % (tag, os.getpid())

    def test_write_not_blocked_by_pending_read(self):
        try:
            out, lines = run_child('--child-write-vs-read', self.pipe('wvr'),
                                   timeout=15)
        except subprocess.TimeoutExpired:
            self.fail('SI_Write never returned while an SI_Read was pending')
        self.assertLess(out['write_ms'], 50.0, out)
        self.assertEqual(bytes.fromhex(out['wire']), OPEN)
        self.assertEqual((out['read_rc'], out['read_byte']), (0, '5a'), out)
        self.assertFalse([l for l in lines
                          if 'SI_Read err' in l or 'SI_Write err' in l], lines)

    def test_silent_read_ends_at_the_backstop_as_a_timeout(self):
        try:
            out, lines = run_child('--child-silent-read', self.pipe('sr'),
                                   timeout=15)
        except subprocess.TimeoutExpired:
            self.fail('SI_Read on a silent pipe never returned')
        self.assertEqual((out['rc'], out['n']), (0, 0), out)
        # Backstop = read timeout (200) + 500 ms.
        self.assertGreater(out['read_ms'], 600.0, out)
        self.assertLess(out['read_ms'], 2000.0, out)
        self.assertFalse([l for l in lines if 'SI_Read err' in l], lines)
        self.assertEqual(len([l for l in lines
                              if 'SI_Read 0/1 x1 over' in l
                              and l.endswith('(timeouts)')]), 1, lines)


def hexs(b):
    return ' '.join('%02X' % x for x in b)


def mask(n):
    return ' '.join(['..'] * n)


@unittest.skipUnless(sys.platform == 'win32', 'Windows DLLs')
@unittest.skipUnless(os.path.exists(XP_TEST_DLL),
                     'shim not built: run scripts/zwift_ant_shim.ps1 -BuildOnly')
class ShimReadDumpTest(unittest.TestCase):
    """A device that echoes (or anything else on a port the shim was pointed
    at) can send a host's key back; the level-2 read dump masks it as the
    write dump does."""

    def read_lines(self, tag, level, size):
        with tempfile.TemporaryDirectory() as tmp:
            log = os.path.join(tmp, 'shim.log')
            env = dict(os.environ, RADIANT_SHIM_LOG=log,
                       RADIANT_ANT_COM='pipe\\radiant_shim_%s_%d'
                       % (tag, os.getpid()),
                       RADIANT_SHIM_LOG_LEVEL=level,
                       RADIANT_SHIM_TEST_READ=str(size))
            r = subprocess.run([sys.executable, os.path.abspath(__file__),
                                '--child-read'], env=env, check=True,
                               timeout=60, capture_output=True, text=True)
            with open(log, 'rb') as f:
                lines = f.read().decode('ascii').split('\r\n')[:-1]
        # The caller gets every byte: the masking is for the log only.
        self.assertEqual(bytes.fromhex(r.stdout), b''.join(READS))
        reads = [l.split(' SI_Read ', 1)[1] for l in lines
                 if ' SI_Read ' in l and 'timeouts' not in l]
        leaks = [d for d in reads if KEY_RE.search(d)]
        self.assertEqual(leaks, [], 'key bytes in the log')
        return reads

    NET = 'A4 09 46 00 ' + mask(9)
    PLAIN_HEX = hexs(PLAIN)

    def test_level2_read_dump_holds_no_key(self):
        dumps = self.read_lines('rd', '2', 4096)
        self.assertEqual(len(dumps), len(READS), dumps)
        net, plain = self.NET, self.PLAIN_HEX
        want = [
            ': ' + plain,
            ': ' + net,
            ': %s %s %s' % (plain, net, plain),
            ': A4 11 76 01 ' + mask(17),
            ': A4 12 83 01 02 ' + mask(17),
            ': A4 22 F5 ' + mask(35),
            ' ms',                  # half a frame: nothing yet
            ': ' + net,             # the read that completes it
            ': (+2 bytes not whole frames) ' + plain,
            ': (+7 bytes not whole frames) ' + net,
            ': (+2 bytes not whole frames) ' + plain,
        ]
        for k, w in enumerate(want):
            self.assertTrue(dumps[k].endswith(w), (k, dumps[k], w))

    def test_byte_at_a_time_reads_reassemble(self):
        """ANT_DLL reads one byte per SI_Read: the frames still show, each
        on the read that completes it, and keys stay masked."""
        lines = self.read_lines('rb', '2', 1)
        self.assertEqual(len(lines), len(b''.join(READS)), lines[:5])
        dumps = [l.split(': ', 1)[1] for l in lines if ': ' in l]
        net, plain = self.NET, self.PLAIN_HEX
        self.assertEqual(dumps, [
            plain, net, plain, net, plain,
            'A4 11 76 01 ' + mask(17),
            'A4 12 83 01 02 ' + mask(17),
            'A4 22 F5 ' + mask(35),
            net,
            '(+1 bytes not whole frames)', '(+1 bytes not whole frames)',
            plain,
            '(+7 bytes not whole frames)', net,
            '(+2 bytes not whole frames)', plain])

    def test_key_bytes_inside_a_failed_frame_are_not_reframed(self):
        """Every byte a failed frame claims is dropped unseen, so an 0xA4
        among the key bytes cannot start a frame that checks by luck."""
        dumps = self.read_lines_inline([LUCKY, PLAIN])
        self.assertEqual(dumps, ['(+14 bytes not whole frames)',
                                 self.PLAIN_HEX])
        self.assertFalse([d for d in dumps if KEY_RE.search(d)], dumps)

    def read_lines_inline(self, reads):
        with tempfile.TemporaryDirectory() as tmp:
            log = os.path.join(tmp, 'shim.log')
            env = dict(os.environ, RADIANT_SHIM_LOG=log,
                       RADIANT_ANT_COM='pipe\\radiant_shim_rl_%d' % os.getpid(),
                       RADIANT_SHIM_LOG_LEVEL='2',
                       RADIANT_SHIM_TEST_READS=','.join(r.hex() for r in reads))
            r = subprocess.run([sys.executable, os.path.abspath(__file__),
                                '--child-read'], env=env, check=True,
                               timeout=60, capture_output=True, text=True)
            with open(log, 'rb') as f:
                lines = f.read().decode('ascii').split('\r\n')[:-1]
        self.assertEqual(bytes.fromhex(r.stdout), b''.join(reads))
        return [l.split(' ms: ', 1)[1] for l in lines
                if ' SI_Read ' in l and ' ms: ' in l]

    def test_level1_read_line_has_no_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = os.path.join(tmp, 'shim.log')
            env = dict(os.environ, RADIANT_SHIM_LOG=log,
                       RADIANT_ANT_COM='pipe\\radiant_shim_r1_%d' % os.getpid(),
                       RADIANT_SHIM_LOG_LEVEL='1')
            subprocess.run([sys.executable, os.path.abspath(__file__),
                            '--child-read'], env=env, check=True, timeout=60,
                           capture_output=True)
            with open(log, 'rb') as f:
                lines = f.read().decode('ascii').split('\r\n')[:-1]
        reads = [l for l in lines if ' SI_Read ' in l and 'timeouts' not in l]
        self.assertEqual(len(reads), len(READS), lines)
        for l in reads:
            self.assertRegex(l, r' SI_Read \d+/4096 in [0-9.]+ ms$')


def run_select(**env_over):
    """child_select under exactly the selection given (others removed)."""
    with tempfile.TemporaryDirectory() as tmp:
        log = os.path.join(tmp, 'shim.log')
        env = {k: v for k, v in os.environ.items()
               if k not in ('RADIANT_ANT_COM', 'RADIANT_ANT_DEVICE')}
        env.update(RADIANT_SHIM_LOG=log, RADIANT_SHIM_LOG_LEVEL='0')
        env.update(env_over)
        r = subprocess.run([sys.executable, os.path.abspath(__file__),
                            '--child-select'], env=env, check=True,
                           timeout=60, capture_output=True, text=True)
        with open(log, 'rb') as f:
            lines = f.read().decode('ascii').split('\r\n')[:-1]
    return json.loads(r.stdout), lines


SI_DEVICE_NOT_FOUND = 0xFF


@unittest.skipUnless(sys.platform == 'win32', 'Windows DLLs')
@unittest.skipUnless(os.path.exists(XP_DLL),
                     'shim not built: run scripts/zwift_ant_shim.ps1 -BuildOnly')
class ShimSelectTest(unittest.TestCase):
    """The shim forwards every host write, keys included, so it drives only
    the port the user selected: no registry guess, no COM15 fallback."""

    def assert_no_device(self, out, lines, reason):
        self.assertEqual(out, {'num': 0, 'open_rc': SI_DEVICE_NOT_FOUND})
        self.assertEqual(sum(reason in l for l in lines), 1, lines)
        self.assertFalse([l for l in lines if 'COM15' in l or 'opened' in l],
                         lines)

    def test_no_selection_no_device(self):
        out, lines = run_select()
        self.assert_no_device(out, lines, 'no port: none selected')

    def test_port_name_must_be_comn(self):
        for bad in ('COM', 'COM1234', 'COM1x', 'LPT1', 'C:\\Windows\\x',
                    'pipe\\radiant_shim_x', 'COM1 ', 'A' * 80):
            with self.subTest(bad=bad):
                out, lines = run_select(RADIANT_ANT_COM=bad)
                self.assert_no_device(out, lines, 'RADIANT_ANT_COM is set but'
                                      ' is not a port name')
                # An invalid value is not echoed into the log.
                if len(bad) > 4:
                    self.assertFalse([l for l in lines if bad in l], lines)

    def test_port_name_any_case(self):
        out, lines = run_select(RADIANT_ANT_COM='com250')
        self.assertEqual(out['num'], 0)    # COM250 does not exist here
        self.assertIn('port COM250 (from RADIANT_ANT_COM)',
                      [l.split(' ', 3)[3] for l in lines])

    def test_device_that_names_no_port(self):
        out, lines = run_select(RADIANT_ANT_DEVICE='NO-SUCH-SERIAL-0042')
        self.assert_no_device(out, lines, 'RADIANT_ANT_DEVICE=NO-SUCH-SERIAL-'
                              '0042 names no present COM port')

    def test_device_value_must_be_printable(self):
        out, lines = run_select(RADIANT_ANT_DEVICE='two words')
        self.assert_no_device(out, lines, 'RADIANT_ANT_DEVICE is set but is'
                              ' not a serial number')

    def test_both_set_and_device_absent(self):
        out, lines = run_select(RADIANT_ANT_COM='COM250',
                                RADIANT_ANT_DEVICE='NO-SUCH-SERIAL-0042')
        self.assert_no_device(out, lines, 'names no present COM port')


@unittest.skipUnless(sys.platform == 'win32', 'Windows DLLs')
@unittest.skipUnless(os.path.exists(XP_TEST_DLL),
                     'shim not built: run scripts/zwift_ant_shim.ps1 -BuildOnly')
class ShimLockTest(unittest.TestCase):
    def pipe(self, tag):
        return 'pipe\\radiant_shim_%s_%d' % (tag, os.getpid())

    def test_racing_opens_keep_the_winners_handle(self):
        out, lines = run_child('--child-open-race', self.pipe('race'),
                               timeout=120, level='0')
        self.assertEqual(out['bad'], [], 'a failed SI_Open clobbered the '
                         'port another SI_Open had just opened')

    def test_close_under_a_pending_read(self):
        out, _ = run_child('--child-close-under-read', self.pipe('cur'),
                           timeout=30, level='1')
        self.assertLess(out['close_ms'], 500.0, out)
        self.assertIsNotNone(out['read_rc'], 'SI_Read never returned')
        self.assertLess(out['read_ms'], 1000.0, out)
        self.assertEqual(out['write_after_close_rc'], 1, out)  # INVALID_HANDLE

    def test_zero_read_timeout_is_clamped(self):
        try:
            out, lines = run_child('--child-zero-timeout', self.pipe('zt'),
                                   timeout=15)
        except subprocess.TimeoutExpired:
            self.fail('SI_Read with a zero read timeout never returned')
        self.assertEqual((out['r'], out['w']), (1, 200), out)
        self.assertEqual((out['rc'], out['n']), (0, 0), out)
        self.assertLess(out['read_ms'], 2000.0, out)
        self.assertTrue([l for l in lines if 'SI_SetTimeouts r=0 (clamped to 1'
                         in l], lines)


@unittest.skipUnless(sys.platform == 'win32', 'Windows DLLs')
@unittest.skipUnless(os.path.exists(XP_DLL),
                     'shim not built: run scripts/zwift_ant_shim.ps1 -BuildOnly')
@unittest.skipUnless(os.environ.get('RADIANT_SHIM_TEST_COM'),
                     'set RADIANT_SHIM_TEST_COM=COMn to run on a dongle')
class ShimHardwareTest(unittest.TestCase):
    """ANT_DLL's pattern on a real dongle, without Zwift."""

    def test_request_while_read_pending(self):
        out, _ = run_child('--child-hw-capabilities',
                           os.environ['RADIANT_SHIM_TEST_COM'], timeout=30)
        self.assertEqual(out['write_rc'], 0, out)
        self.assertLess(out['write_ms'], 20.0, out)
        self.assertIsNotNone(out['reply_ms'], 'no CAPABILITIES reply')
        self.assertLess(out['reply_ms'], 50.0, out)


if __name__ == '__main__':
    if sys.argv[1:] == ['--child']:
        child()
    elif sys.argv[1:] == ['--child-write']:
        child_write()
    elif sys.argv[1:] == ['--child-write-vs-read']:
        child_write_vs_read()
    elif sys.argv[1:] == ['--child-silent-read']:
        child_silent_read()
    elif sys.argv[1:] == ['--child-hw-capabilities']:
        child_hw_capabilities()
    elif sys.argv[1:] == ['--child-read']:
        child_read()
    elif sys.argv[1:] == ['--child-select']:
        child_select()
    elif sys.argv[1:] == ['--child-zero-timeout']:
        child_zero_timeout()
    elif sys.argv[1:] == ['--child-open-race']:
        child_open_race()
    elif sys.argv[1:] == ['--child-close-under-read']:
        child_close_under_read()
    else:
        unittest.main()
