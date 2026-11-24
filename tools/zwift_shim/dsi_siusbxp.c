/* SPDX-License-Identifier: Apache-2.0
 *
 * DSI_SiUSBXp_3_1.DLL - the Silicon Labs USBXpress host API, implemented over
 * the CP2102N's COM port instead of the USBXpress kernel driver.
 *
 * ANT_DLL resolves these by name through LoadLibrary and uses the result as a
 * byte pipe: open, set baud, read, write, check the receive queue. Every one
 * of those maps directly onto Win32 serial calls, which is why this shim is
 * small and why it needs no driver, no elevation and no change to the dongle.
 *
 * Every entry point logs, because the point of the first run is to learn
 * exactly which of these ANT_DLL actually calls and in what order. Reads and
 * writes are timed with QPC (log level 1), so host turnaround is the gap
 * between an SI_Write line and the next non-empty SI_Read line.
 */
#include "shim_common.h"
#include <stdarg.h>

const char shim_dll_tag[] = "usbxp";

static HANDLE g_port = INVALID_HANDLE_VALUE;
static DWORD g_read_timeout = 1000;
static DWORD g_write_timeout = 1000;

/* ANT_DLL writes from more than one thread. The synchronous handle this shim
 * used to open kept each write's bytes together; with overlapped I/O this lock
 * does that for writers, without making a write wait for a pending read. */
static SRWLOCK g_write_lock = SRWLOCK_INIT;

/* One manual-reset event per thread for its overlapped reads and writes,
 * created on first use and closed when the thread (or the DLL) goes. */
static __declspec(thread) HANDLE t_ev;

/* Empty reads come in floods (one per read timeout while the radio is quiet),
 * so each thread folds a run of them into one line, emitted when the run ends
 * or once a second while it lasts. Per-thread because ANT_DLL reads and
 * writes from different threads, and a run belongs to the thread reading. */
static __declspec(thread) DWORD t_zero_n;
static __declspec(thread) DWORD t_zero_req;
static __declspec(thread) double t_zero_ms;
static __declspec(thread) LONGLONG t_zero_start;

static void flush_zero_run(void)
{
    if (!t_zero_n)
        return;
    if (shim_log_on(1))
        shim_log("SI_Read 0/%lu x%lu over %.1f ms (timeouts)",
                 t_zero_req, t_zero_n, t_zero_ms);
    t_zero_n = 0;
    t_zero_ms = 0;
}

/* The length of the 0xA4 frame at b[i] if it lies wholly inside the buffer
 * and its checksum holds, else 0. Nothing past a frame that fails is trusted:
 * a LEN that lies, or key bytes that happen to start with 0xA4, would
 * otherwise send the walk through key material as if it were framing. */
static DWORD frame_ok(const unsigned char *b, DWORD n, DWORD i)
{
    DWORD flen, k;
    unsigned char x = 0;

    if (i + 4 > n || b[i] != 0xA4)
        return 0;
    flen = (DWORD)b[i + 1] + 4;     /* sync, len, id, payload, checksum */
    if (i + flen > n)
        return 0;
    for (k = 0; k + 1 < flen; k++)
        x ^= b[i + k];
    return x == b[i + flen - 1] ? flen : 0;
}

/* "id=0x4E" or "id=0x4F,0x46" for each leading frame that passes frame_ok(),
 * or "unframed" when none does. No byte of a write that did not frame is
 * named: it may be the middle of a key split across two writes. */
static void ant_ids(const unsigned char *b, DWORD n, char *out, size_t cap)
{
    DWORD i = 0, flen;
    size_t o = 0;
    out[0] = '\0';
    while ((flen = frame_ok(b, n, i)) != 0 && o + 8 < cap) {
        o += sprintf_s(out + o, cap - o, "%s0x%02X", o ? "," : "id=", b[i + 2]);
        i += flen;
    }
    if (!o && n)
        strcpy_s(out, cap, "unframed");
}

/* -1 for a message that carries no key; for one that does, how many leading
 * body bytes are not key and may be logged: the network number of a network
 * key (0x46, which in a Zwift session is the licensed ANT+ key), the key
 * number of an encryption key (0x7E), the info type of an encryption-info
 * message setting the RNG seed (0x7F type 0x02), the channel and key length
 * of a RadiANT root key (0xF2), nothing of a pairing message (0xF5, whose
 * sub-command 0x02 is a private X25519 scalar). f is a whole frame. Mirrors
 * key_msg_keep() in apps/common/ant_serial_bridge.c. */
static int key_keep(const unsigned char *f)
{
    switch (f[2]) {
    case 0x46: case 0x7E: return 1;
    case 0x7F:            return f[1] >= 1 && f[3] == 0x02 ? 1 : -1;
    case 0xF2:            return 2;
    case 0xF5:            return 0;
    default:              return -1;
    }
}

#define WRITE_DUMP_MAX 256          /* bytes; longer writes end in " ..." */

/* The level-2 hex of a write, with each key and its checksum (the XOR of the
 * key bytes once the header is known) shown as "..". Walks whole 0xA4 frames
 * that pass frame_ok() from the start of the buffer; from the first byte that
 * does not, only the count of what is left is logged, since no frame boundary
 * is known past it. Stateless: Zwift writes whole frames, so that tail is
 * empty in practice. */
static const char *write_dump(const unsigned char *b, DWORD n)
{
    static __declspec(thread) char buf[WRITE_DUMP_MAX * 3 + 48];
    static const char digits[] = "0123456789ABCDEF";
    DWORD i = 0, k, shown = 0;
    char *o = buf;

    DWORD flen;

    while ((flen = frame_ok(b, n, i)) != 0) {
        int keep = key_keep(b + i);
        for (k = 0; k < flen; k++) {
            int mask = keep >= 0 && k >= 3 + (DWORD)keep;
            if (shown++ == WRITE_DUMP_MAX) {
                memcpy(o, " ...", 5);       /* with its NUL */
                return buf;
            }
            if (o != buf)
                *o++ = ' ';
            *o++ = mask ? '.' : digits[b[i + k] >> 4];
            *o++ = mask ? '.' : digits[b[i + k] & 15];
        }
        i += flen;
    }
    *o = '\0';
    if (i < n)
        sprintf_s(o, sizeof(buf) - (size_t)(o - buf),
                  "%s(+%lu bytes not whole frames)", o == buf ? "" : " ",
                  n - i);
    return buf;
}

/* Extra time past the read or write timeout before port_io() cancels an I/O
 * itself. On a COM port COMMTIMEOUTS always end the I/O first; the backstop
 * is for handles they do not apply to, like the tests' named pipe. */
#define IO_BACKSTOP_MS 500

/* The backstop for a read or write timeout. 0 means no total timeout to
 * COMMTIMEOUTS, and INFINITE is infinite, so neither gets one here: adding
 * to them would cut a wait short (or wrap to 499 ms). */
static DWORD io_backstop(DWORD timeout_ms)
{
    if (timeout_ms == 0 || timeout_ms > INFINITE - IO_BACKSTOP_MS - 1)
        return INFINITE;
    return timeout_ms + IO_BACKSTOP_MS;
}

/* One read or write on g_port, waited for here so callers keep blocking
 * semantics. The port is opened FILE_FLAG_OVERLAPPED because Windows runs
 * synchronous I/O on one handle one call at a time: a write then waits for
 * ANT_DLL's pending 1-byte read, which on a quiet radio lasts the whole read
 * timeout (89 of 250 writes held >=50 ms, up to 1348 ms, in the 2026-11-23
 * TI/Zwift session). A wait that passes backstop_ms cancels the I/O and
 * returns what arrived as success. On failure, GetLastError() holds the
 * I/O's error. */
static BOOL port_io(BOOL write, void *buf, DWORD n, DWORD *done,
                    DWORD backstop_ms)
{
    HANDLE h = g_port;
    OVERLAPPED ov;
    BOOL ok;
    DWORD err;

    *done = 0;
    if (!t_ev) {
        t_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!t_ev)
            return FALSE;
    }
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = t_ev;
    ok = write ? WriteFile(h, buf, n, NULL, &ov)
               : ReadFile(h, buf, n, NULL, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING)
        return FALSE;
    if (GetOverlappedResultEx(h, &ov, done, backstop_ms, FALSE))
        return TRUE;
    err = GetLastError();
    if (err != WAIT_TIMEOUT)
        return FALSE;               /* completed, with an error */
    CancelIoEx(h, &ov);
    /* Wait for it to finish either way: ov lives on this stack. */
    if (GetOverlappedResult(h, &ov, done, TRUE))
        return TRUE;                /* it completed before the cancel */
    err = GetLastError();
    if (err == ERROR_OPERATION_ABORTED)
        return TRUE;                /* our cancel: a timeout, not an error */
    SetLastError(err);
    return FALSE;
}

static void close_thread_event(void)
{
    if (t_ev) {
        CloseHandle(t_ev);
        t_ev = NULL;
    }
}

/* A handle value ANT_DLL can hold that is obviously ours and never a real
 * kernel handle it might accidentally pass elsewhere. */
#define SHIM_HANDLE ((HANDLE)(ULONG_PTR)0x51D0BEEF)

static int open_port(void)
{
    char path[64];
    DCB dcb;
    COMMTIMEOUTS to;

    if (g_port != INVALID_HANDLE_VALUE)
        return 1;

    sprintf_s(path, sizeof(path), "\\\\.\\%s", shim_port());
    g_port = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                         OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (g_port == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        shim_log("open %s FAILED err=%lu %s", path, err, shim_err_name(err));
        return 0;
    }

    ZeroMemory(&dcb, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(g_port, &dcb)) {
        shim_log("GetCommState failed err=%lu", GetLastError());
    }
    dcb.BaudRate = 57600;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    SetCommState(g_port, &dcb);

    to.ReadIntervalTimeout = 10;
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant = g_read_timeout;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = g_write_timeout;
    SetCommTimeouts(g_port, &to);

    PurgeComm(g_port, PURGE_RXCLEAR | PURGE_TXCLEAR);
    shim_log("opened %s", path);
    return 1;
}

static void close_port(void)
{
    if (g_port != INVALID_HANDLE_VALUE) {
        CloseHandle(g_port);
        g_port = INVALID_HANDLE_VALUE;
        shim_log("closed port");
    }
}

/* ---- the USBXpress host API ------------------------------------------- */

__declspec(dllexport) DWORD WINAPI SI_GetNumDevices(LPDWORD lpdwNumDevices)
{
    char path[64];
    HANDLE h;
    DWORD err;
    sprintf_s(path, sizeof(path), "\\\\.\\%s", shim_port());
    /* Report a device only if the port is really there, so ANT_DLL fails the
     * way it would with no stick plugged in rather than opening a ghost. */
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                    OPEN_EXISTING, 0, NULL);
    err = GetLastError();
    if (h == INVALID_HANDLE_VALUE && g_port == INVALID_HANDLE_VALUE) {
        if (lpdwNumDevices) *lpdwNumDevices = 0;
        shim_log("SI_GetNumDevices -> 0 (%s absent, err=%lu %s)", shim_port(),
                 err, shim_err_name(err));
        return SI_SUCCESS;
    }
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    if (lpdwNumDevices) *lpdwNumDevices = 1;
    if (h != INVALID_HANDLE_VALUE)
        shim_log("SI_GetNumDevices -> 1 (%s)", shim_port());
    else if (err == ERROR_ACCESS_DENIED)
        shim_log("SI_GetNumDevices -> 1 (%s, already open here)", shim_port());
    else
        /* The probe's error is not "held by us", so the port this shim holds
         * may have left USB. The answer stays 1 (ANT_DLL behaves as before);
         * the line is evidence: the XDS110 left at 18:47:06 on 2026-11-23
         * and the old "already open here" line hid it. */
        shim_log("SI_GetNumDevices -> 1 (%s open here, but probe err=%lu %s: "
                 "port may be gone)", shim_port(), err, shim_err_name(err));
    return SI_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI SI_GetProductString(DWORD dwDeviceNum,
                                                       LPVOID lpvDeviceString,
                                                       DWORD dwFlags)
{
    if (!lpvDeviceString || dwDeviceNum != 0)
        return SI_DEVICE_NOT_FOUND;
    switch (dwFlags) {
    case SI_RETURN_SERIAL_NUMBER:
        strcpy_s((char *)lpvDeviceString, 256, "RADIANT-CC2652P-0001");
        break;
    case SI_RETURN_DESCRIPTION:
        strcpy_s((char *)lpvDeviceString, 256, "ANT USB Stick 2");
        break;
    case SI_RETURN_LINK_NAME:
        strcpy_s((char *)lpvDeviceString, 256, "RADIANT");
        break;
    /* SI_GetProductString returns ASCII for every flag, VID and PID included -
     * they come back as 4-character hex, not as a binary WORD. Returning a
     * WORD here is what made ANT_DLL reject the device after enumerating it:
     * it read the VID as a string, got nothing that parsed as 0FCF, and never
     * reached SI_Open. */
    case SI_RETURN_VID:
        sprintf_s((char *)lpvDeviceString, 256, "%04X", SHIM_VID);
        break;
    case SI_RETURN_PID:
        sprintf_s((char *)lpvDeviceString, 256, "%04X", SHIM_PID);
        break;
    default:
        return SI_INVALID_PARAMETER;
    }
    shim_log("SI_GetProductString(dev=%lu, flags=%lu)", dwDeviceNum, dwFlags);
    return SI_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI SI_Open(DWORD dwDevice, HANDLE *cyHandle)
{
    if (!cyHandle || dwDevice != 0)
        return SI_DEVICE_NOT_FOUND;
    if (!open_port()) {
        shim_log("SI_Open -> DEVICE_NOT_FOUND");
        return SI_DEVICE_NOT_FOUND;
    }
    *cyHandle = SHIM_HANDLE;
    shim_log("SI_Open(%lu) -> ok", dwDevice);
    return SI_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI SI_Close(HANDLE cyHandle)
{
    (void)cyHandle;
    flush_zero_run();
    close_port();
    shim_log("SI_Close");
    return SI_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI SI_Read(HANDLE cyHandle, LPVOID lpBuffer,
                                           DWORD dwBytesToRead,
                                           LPDWORD lpdwBytesReturned,
                                           OVERLAPPED *o)
{
    DWORD got = 0;
    LONGLONG t0, t1;
    BOOL ok;
    (void)cyHandle; (void)o;
    if (lpdwBytesReturned) *lpdwBytesReturned = 0;
    if (g_port == INVALID_HANDLE_VALUE || !lpBuffer)
        return SI_INVALID_HANDLE;
    t0 = shim_ticks();
    ok = port_io(FALSE, lpBuffer, dwBytesToRead, &got,
                 io_backstop(g_read_timeout));
    t1 = shim_ticks();
    if (!ok) {
        DWORD err = GetLastError();
        flush_zero_run();
        shim_log("SI_Read err=%lu %s after %.3f ms", err, shim_err_name(err),
                 shim_ms(t0, t1));
        return SI_READ_ERROR;
    }
    if (lpdwBytesReturned) *lpdwBytesReturned = got;
    if (!got) {
        if (!t_zero_n)
            t_zero_start = t0;
        t_zero_n++;
        t_zero_req = dwBytesToRead;
        t_zero_ms += shim_ms(t0, t1);
        if (shim_ms(t_zero_start, t1) >= 1000.0)
            flush_zero_run();
        return SI_SUCCESS;
    }
    flush_zero_run();
    if (shim_log_on(2))
        shim_log("SI_Read %lu/%lu in %.3f ms: %s", got, dwBytesToRead,
                 shim_ms(t0, t1), shim_hex(lpBuffer, got));
    else if (shim_log_on(1))
        shim_log("SI_Read %lu/%lu in %.3f ms", got, dwBytesToRead,
                 shim_ms(t0, t1));
    return SI_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI SI_Write(HANDLE cyHandle, LPVOID lpBuffer,
                                            DWORD dwBytesToWrite,
                                            LPDWORD lpdwBytesWritten,
                                            OVERLAPPED *o)
{
    DWORD put = 0;
    LONGLONG t0, t1;
    BOOL ok;
    char ids[64];
    (void)cyHandle; (void)o;
    if (lpdwBytesWritten) *lpdwBytesWritten = 0;
    if (g_port == INVALID_HANDLE_VALUE || !lpBuffer)
        return SI_INVALID_HANDLE;
    AcquireSRWLockExclusive(&g_write_lock);
    t0 = shim_ticks();
    ok = port_io(TRUE, lpBuffer, dwBytesToWrite, &put,
                 io_backstop(g_write_timeout));
    t1 = shim_ticks();
    ReleaseSRWLockExclusive(&g_write_lock);
    ant_ids((const unsigned char *)lpBuffer, dwBytesToWrite, ids, sizeof(ids));
    if (!ok) {
        DWORD err = GetLastError();
        shim_log("SI_Write err=%lu %s after %.3f ms %s", err,
                 shim_err_name(err), shim_ms(t0, t1), ids);
        return SI_WRITE_ERROR;
    }
    if (lpdwBytesWritten) *lpdwBytesWritten = put;
    if (shim_log_on(2))
        shim_log("SI_Write %lu/%lu in %.3f ms %s: %s", put, dwBytesToWrite,
                 shim_ms(t0, t1), ids,
                 write_dump((const unsigned char *)lpBuffer, dwBytesToWrite));
    else if (shim_log_on(1))
        shim_log("SI_Write %lu/%lu in %.3f ms %s", put, dwBytesToWrite,
                 shim_ms(t0, t1), ids);
    return SI_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI SI_CheckRXQueue(HANDLE cyHandle,
                                                   LPDWORD lpdwNumBytesInQueue,
                                                   LPDWORD lpdwQueueStatus)
{
    COMSTAT st;
    DWORD errs = 0;
    (void)cyHandle;
    if (lpdwNumBytesInQueue) *lpdwNumBytesInQueue = 0;
    if (lpdwQueueStatus) *lpdwQueueStatus = 0;
    if (g_port == INVALID_HANDLE_VALUE)
        return SI_INVALID_HANDLE;
    if (!ClearCommError(g_port, &errs, &st)) {
        DWORD err = GetLastError();
        shim_log("SI_CheckRXQueue err=%lu %s", err, shim_err_name(err));
        return SI_READ_ERROR;
    }
    if (lpdwNumBytesInQueue) *lpdwNumBytesInQueue = st.cbInQue;
    if (lpdwQueueStatus) *lpdwQueueStatus = st.cbInQue ? 1 : 0;
    if (shim_log_on(2))
        shim_log("SI_CheckRXQueue -> %lu (comm errors 0x%lX)", st.cbInQue, errs);
    return SI_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI SI_FlushBuffers(HANDLE cyHandle,
                                                   BYTE FlushTransmit,
                                                   BYTE FlushReceive)
{
    (void)cyHandle;
    if (g_port == INVALID_HANDLE_VALUE)
        return SI_INVALID_HANDLE;
    PurgeComm(g_port, (FlushTransmit ? PURGE_TXCLEAR : 0)
                    | (FlushReceive ? PURGE_RXCLEAR : 0));
    shim_log("SI_FlushBuffers tx=%d rx=%d", FlushTransmit, FlushReceive);
    return SI_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI SI_SetTimeouts(DWORD dwReadTimeout,
                                                  DWORD dwWriteTimeout)
{
    COMMTIMEOUTS to;
    g_read_timeout = dwReadTimeout;
    g_write_timeout = dwWriteTimeout;
    if (g_port != INVALID_HANDLE_VALUE) {
        to.ReadIntervalTimeout = 10;
        to.ReadTotalTimeoutMultiplier = 0;
        to.ReadTotalTimeoutConstant = dwReadTimeout;
        to.WriteTotalTimeoutMultiplier = 0;
        to.WriteTotalTimeoutConstant = dwWriteTimeout;
        SetCommTimeouts(g_port, &to);
    }
    shim_log("SI_SetTimeouts r=%lu w=%lu", dwReadTimeout, dwWriteTimeout);
    return SI_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI SI_GetTimeouts(LPDWORD lpdwReadTimeout,
                                                  LPDWORD lpdwWriteTimeout)
{
    if (lpdwReadTimeout) *lpdwReadTimeout = g_read_timeout;
    if (lpdwWriteTimeout) *lpdwWriteTimeout = g_write_timeout;
    return SI_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI SI_SetBaudRate(HANDLE cyHandle,
                                                  DWORD dwBaudRate)
{
    DCB dcb;
    (void)cyHandle;
    if (g_port == INVALID_HANDLE_VALUE)
        return SI_INVALID_HANDLE;
    ZeroMemory(&dcb, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (GetCommState(g_port, &dcb)) {
        dcb.BaudRate = dwBaudRate;
        SetCommState(g_port, &dcb);
    }
    shim_log("SI_SetBaudRate %lu", dwBaudRate);
    return SI_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI SI_SetLineControl(HANDLE cyHandle,
                                                     WORD wLineControl)
{
    (void)cyHandle;
    shim_log("SI_SetLineControl 0x%04X (accepted, 8N1 kept)", wLineControl);
    return SI_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI SI_SetFlowControl(HANDLE cyHandle,
                                                     BYTE bCTS, BYTE bRTS,
                                                     BYTE bDTR, BYTE bDSR,
                                                     BYTE bDCD,
                                                     BOOL bFlowXonXoff)
{
    (void)cyHandle; (void)bCTS; (void)bRTS; (void)bDTR; (void)bDSR;
    (void)bDCD; (void)bFlowXonXoff;
    shim_log("SI_SetFlowControl (accepted, none used)");
    return SI_SUCCESS;
}

/* Present so a resolve cannot fail; ANT_DLL may probe for them. */
__declspec(dllexport) DWORD WINAPI SI_GetDLLVersion(DWORD *hi, DWORD *lo)
{
    if (hi) *hi = 3;
    if (lo) *lo = 1;
    return SI_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI SI_GetDriverVersion(DWORD *hi, DWORD *lo)
{
    if (hi) *hi = 3;
    if (lo) *lo = 1;
    return SI_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)h; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        shim_log_loaded("DSI_SiUSBXp_3_1");
        shim_port();                /* logs where the port came from */
    } else if (reason == DLL_THREAD_DETACH) {
        flush_zero_run();
        close_thread_event();
    } else if (reason == DLL_PROCESS_DETACH) {
        flush_zero_run();
        close_thread_event();
        close_port();
    }
    return TRUE;
}
