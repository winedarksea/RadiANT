/* SPDX-License-Identifier: Apache-2.0
 *
 * Logging and COM-port discovery, compiled into BOTH shim DLLs so neither
 * depends on the other. They append to the same file, which is what makes the
 * log readable as one conversation: ANT_DLL interleaves identity queries
 * (CP210x_*) with transport calls (SI_*), and the order is the interesting
 * part.
 *
 * Each DLL links its OWN copy of this file, so the statics below exist twice
 * and no lock taken here could serialise one DLL against the other. Lines are
 * made atomic instead: the whole line - prefix, text and CRLF - is formatted
 * into one stack buffer and written with ONE WriteFile on a handle opened with
 * FILE_APPEND_DATA only. Such a write lands at end of file as a unit, so lines
 * from ANT_DLL's read and write threads, and from the two DLLs, can neither
 * glue together nor blank each other. There is no CRT buffering either: a line
 * is on disk the moment its call returns, so a crash loses nothing.
 */
#include "shim_common.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define SHIM_LINE_MAX 1024
#define SHIM_HEX_MAX  256           /* bytes; longer payloads end in " ..." */

static HANDLE g_log = INVALID_HANDLE_VALUE;
static int g_level = 1;
static LARGE_INTEGER g_qpf;
static INIT_ONCE g_log_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK log_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    char path[MAX_PATH], lv[16], *end;
    DWORD n;
    long v;
    (void)once; (void)param; (void)ctx;

    QueryPerformanceFrequency(&g_qpf);
    n = GetEnvironmentVariableA("RADIANT_SHIM_LOG_LEVEL", lv, sizeof(lv));
    if (n > 0 && n < sizeof(lv)) {
        v = strtol(lv, &end, 10);
        if (end != lv && *end == '\0')
            g_level = v < 0 ? 0 : v > 2 ? 2 : (int)v;
    }
    n = GetEnvironmentVariableA("RADIANT_SHIM_LOG", path, sizeof(path));
    if (n == 0 || n >= sizeof(path))
        return TRUE;                /* silent unless a log path is asked for */
    g_log = CreateFileA(path, FILE_APPEND_DATA,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    return TRUE;
}

static void log_ready(void)
{
    InitOnceExecuteOnce(&g_log_once, log_init, NULL, NULL);
}

int shim_log_level(void)
{
    log_ready();
    return g_level;
}

int shim_log_on(int level)
{
    log_ready();
    return g_log != INVALID_HANDLE_VALUE && g_level >= level;
}

LONGLONG shim_ticks(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

double shim_ms(LONGLONG from, LONGLONG to)
{
    log_ready();
    return (double)(to - from) * 1000.0 / (double)g_qpf.QuadPart;
}

/* Prefix: QPC seconds since boot (1 us resolution - GetTickCount's 15.6 ms
 * would hide exactly the latencies this log exists to measure), thread id,
 * and which DLL wrote the line. Both DLLs read the same QPC, so their lines
 * sort into one timeline. */
void shim_log(const char *fmt, ...)
{
    char line[SHIM_LINE_MAX];
    ULONGLONG c, f;
    int n, m;
    DWORD put;
    va_list ap;

    log_ready();
    if (g_log == INVALID_HANDLE_VALUE)
        return;
    c = (ULONGLONG)shim_ticks();
    f = (ULONGLONG)g_qpf.QuadPart;
    n = sprintf_s(line, sizeof(line), "%llu.%06llu t%lu %s ",
                  c / f, (c % f) * 1000000ULL / f,
                  GetCurrentThreadId(), shim_dll_tag);
    if (n < 0)
        return;
    va_start(ap, fmt);
    /* Leave two bytes for the CRLF after _TRUNCATE's terminator. */
    m = _vsnprintf_s(line + n, sizeof(line) - n - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (m < 0)
        m = (int)strlen(line + n);
    n += m;
    line[n++] = '\r';
    line[n++] = '\n';
    WriteFile(g_log, line, (DWORD)n, &put, NULL);
}

void shim_log_loaded(const char *dll)
{
    FILETIME ft, lft;
    SYSTEMTIME st;
    ULARGE_INTEGER u;

    /* Wall clock and PID, so the QPC timeline can be lined up with a pcap or
     * with Zwift's own log. */
    GetSystemTimePreciseAsFileTime(&ft);
    FileTimeToLocalFileTime(&ft, &lft);
    FileTimeToSystemTime(&lft, &st);
    u.LowPart = lft.dwLowDateTime;
    u.HighPart = lft.dwHighDateTime;
    shim_log("=== %s shim loaded: pid %lu, wall %04u-%02u-%02uT%02u:%02u:%02u.%06llu"
             " local, log level %d ===",
             dll, GetCurrentProcessId(), st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, (u.QuadPart / 10) % 1000000ULL,
             shim_log_level());
}

const char *shim_hex(const void *p, DWORD n)
{
    static __declspec(thread) char buf[SHIM_HEX_MAX * 3 + 8];
    static const char digits[] = "0123456789ABCDEF";
    const unsigned char *b = (const unsigned char *)p;
    DWORD i, k = n > SHIM_HEX_MAX ? SHIM_HEX_MAX : n;
    char *o = buf;

    for (i = 0; i < k; i++) {
        if (i)
            *o++ = ' ';
        *o++ = digits[b[i] >> 4];
        *o++ = digits[b[i] & 15];
    }
    if (k < n) {
        memcpy(o, " ...", 4);
        o += 4;
    }
    *o = '\0';
    return buf;
}

const char *shim_err_name(DWORD err)
{
    switch (err) {
    case ERROR_FILE_NOT_FOUND:        return "FILE_NOT_FOUND: not present";
    case ERROR_PATH_NOT_FOUND:        return "PATH_NOT_FOUND: not present";
    case ERROR_ACCESS_DENIED:         return "ACCESS_DENIED: port held by another process";
    case ERROR_SHARING_VIOLATION:     return "SHARING_VIOLATION: port held by another process";
    case ERROR_GEN_FAILURE:           return "GEN_FAILURE: device error";
    case ERROR_SEM_TIMEOUT:           return "SEM_TIMEOUT: device not responding";
    case ERROR_DEVICE_NOT_CONNECTED:  return "DEVICE_NOT_CONNECTED: unplugged";
    case ERROR_OPERATION_ABORTED:     return "OPERATION_ABORTED: purged or unplugged";
    default:                          return "see winerror.h";
    }
}

/* RADIANT_ANT_COM wins; otherwise ask the registry which port the CP2102N was
 * given, so this survives the port number changing. The source is logged once,
 * on first resolution, because "COMn absent" means little without knowing
 * whether COMn came from the user, the registry or the fallback. */
const char *shim_port(void)
{
    static char port[64];
    HKEY parent, child;
    char sub[256];
    DWORD i, len, type, cb;

    if (port[0])
        return port;

    len = GetEnvironmentVariableA("RADIANT_ANT_COM", port, sizeof(port));
    if (len > 0 && len < sizeof(port)) {
        shim_log("port %s (from RADIANT_ANT_COM)", port);
        return port;
    }
    port[0] = '\0';

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_10C4&PID_EA60",
                      0, KEY_READ, &parent) == ERROR_SUCCESS) {
        for (i = 0; ; i++) {
            cb = sizeof(sub);
            if (RegEnumKeyExA(parent, i, sub, &cb, NULL, NULL, NULL, NULL)
                != ERROR_SUCCESS)
                break;
            strcat_s(sub, sizeof(sub), "\\Device Parameters");
            if (RegOpenKeyExA(parent, sub, 0, KEY_READ, &child)
                != ERROR_SUCCESS)
                continue;
            cb = sizeof(port);
            if (RegQueryValueExA(child, "PortName", NULL, &type,
                                 (LPBYTE)port, &cb) == ERROR_SUCCESS
                && type == REG_SZ) {
                RegCloseKey(child);
                RegCloseKey(parent);
                shim_log("port %s (from registry: CP2102N VID_10C4&PID_EA60\\%s)",
                         port, sub);
                return port;
            }
            port[0] = '\0';
            RegCloseKey(child);
        }
        RegCloseKey(parent);
    }
    strcpy_s(port, sizeof(port), "COM15");
    shim_log("port %s (fallback: RADIANT_ANT_COM unset and no CP2102N PortName"
             " in the registry)", port);
    return port;
}
