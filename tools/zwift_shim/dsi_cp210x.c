/* SPDX-License-Identifier: Apache-2.0
 *
 * DSI_CP210xManufacturing_3_1.dll - the identity half of the pair.
 *
 * ANT_DLL uses this only to ask a USBXpress-enumerated device who it is, then
 * accepts or rejects it on the VENDOR ID ALONE - 0x0FCF or 0x1915, with no
 * product-id test anywhere in its binary. That is the whole reason this shim
 * works without touching the hardware: the answer comes from here, so the
 * CP2102N's stored 10C4:EA60 never has to be reprogrammed, COM15 survives, and
 * every tool in tools/ keeps working exactly as before.
 */
#include "shim_common.h"
#include <string.h>

static void copy_out(LPVOID dst, LPBYTE plen, const char *s, BOOL ascii)
{
    size_t n = strlen(s);
    if (!dst)
        return;
    if (ascii) {
        memcpy(dst, s, n + 1);
    } else {
        /* The non-ASCII form is a USB string descriptor: length, type, UTF-16. */
        unsigned char *p = (unsigned char *)dst;
        size_t i;
        p[0] = (unsigned char)(2 + n * 2);
        p[1] = 0x03;
        for (i = 0; i < n; i++) {
            p[2 + i * 2] = (unsigned char)s[i];
            p[3 + i * 2] = 0;
        }
    }
    if (plen)
        *plen = (BYTE)n;
}

__declspec(dllexport) DWORD WINAPI CP210x_GetDeviceVid(HANDLE h, LPWORD lpwVid)
{
    (void)h;
    if (!lpwVid)
        return CP210x_INVALID_HANDLE;
    *lpwVid = SHIM_VID;
    shim_log("CP210x_GetDeviceVid -> %04X", SHIM_VID);
    return CP210x_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI CP210x_GetDevicePid(HANDLE h, LPWORD lpwPid)
{
    (void)h;
    if (!lpwPid)
        return CP210x_INVALID_HANDLE;
    *lpwPid = SHIM_PID;
    shim_log("CP210x_GetDevicePid -> %04X", SHIM_PID);
    return CP210x_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI CP210x_GetDeviceProductString(
    HANDLE h, LPVOID lpProduct, LPBYTE lpbLength, BOOL bConvertToASCII)
{
    (void)h;
    copy_out(lpProduct, lpbLength, "ANT USB Stick 2", bConvertToASCII);
    shim_log("CP210x_GetDeviceProductString(ascii=%d)", bConvertToASCII);
    return CP210x_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI CP210x_GetDeviceSerialNumber(
    HANDLE h, LPVOID lpSerial, LPBYTE lpbLength, BOOL bConvertToASCII)
{
    (void)h;
    copy_out(lpSerial, lpbLength, "RADIANT-CC2652P-0001", bConvertToASCII);
    shim_log("CP210x_GetDeviceSerialNumber(ascii=%d)", bConvertToASCII);
    return CP210x_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI CP210x_Reset(HANDLE h)
{
    (void)h;
    /* A USB re-enumeration is neither possible nor wanted here: the transport
     * is a COM port that stays open across an ANT reset, and the dongle's own
     * MESG_SYSTEM_RESET does the real work. Reporting success is honest about
     * what the caller needs, which is "the device is ready to talk". */
    shim_log("CP210x_Reset (no-op; ANT-level reset does the work)");
    return CP210x_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI CP210x_GetNumDevices(LPDWORD lpdwNumDevices)
{
    if (lpdwNumDevices)
        *lpdwNumDevices = 1;
    return CP210x_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)h; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        shim_log("=== DSI_CP210xManufacturing_3_1 shim loaded ===");
    return TRUE;
}
