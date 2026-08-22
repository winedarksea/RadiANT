/* SPDX-License-Identifier: Apache-2.0
 *
 * Logging and COM-port discovery, compiled into BOTH shim DLLs so neither
 * depends on the other. They append to the same file, which is what makes the
 * log readable as one conversation: ANT_DLL interleaves identity queries
 * (CP210x_*) with transport calls (SI_*), and the order is the interesting
 * part.
 */
#include "shim_common.h"
#include <stdarg.h>
#include <string.h>

void shim_log(const char *fmt, ...)
{
    static FILE *fh = NULL;
    static int tried = 0;
    va_list ap;

    if (!fh) {
        char path[MAX_PATH];
        DWORD n;
        if (tried)
            return;
        tried = 1;
        n = GetEnvironmentVariableA("RADIANT_SHIM_LOG", path, sizeof(path));
        if (n == 0 || n >= sizeof(path))
            return;                 /* silent unless a log path is asked for */
        fh = fopen(path, "a");
        if (!fh)
            return;
    }
    va_start(ap, fmt);
    vfprintf(fh, fmt, ap);
    va_end(ap);
    fputc('\n', fh);
    fflush(fh);
}

/* RADIANT_ANT_COM wins; otherwise ask the registry which port the CP2102N was
 * given, so this survives the port number changing. */
const char *shim_port(void)
{
    static char port[64];
    HKEY parent, child;
    char sub[256];
    DWORD i, len, type, cb;

    if (port[0])
        return port;

    len = GetEnvironmentVariableA("RADIANT_ANT_COM", port, sizeof(port));
    if (len > 0 && len < sizeof(port))
        return port;

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
                return port;
            }
            RegCloseKey(child);
        }
        RegCloseKey(parent);
    }
    strcpy_s(port, sizeof(port), "COM15");
    return port;
}
