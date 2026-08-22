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
 * exactly which of these ANT_DLL actually calls and in what order.
 */
#include "shim_common.h"
#include <stdarg.h>

static HANDLE g_port = INVALID_HANDLE_VALUE;
static DWORD g_read_timeout = 1000;
static DWORD g_write_timeout = 1000;
static CRITICAL_SECTION g_lock;
static int g_lock_ready = 0;

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
                         OPEN_EXISTING, 0, NULL);
    if (g_port == INVALID_HANDLE_VALUE) {
        shim_log("open %s FAILED err=%lu", path, GetLastError());
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
    sprintf_s(path, sizeof(path), "\\\\.\\%s", shim_port());
    /* Report a device only if the port is really there, so ANT_DLL fails the
     * way it would with no stick plugged in rather than opening a ghost. */
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE && g_port == INVALID_HANDLE_VALUE) {
        if (lpdwNumDevices) *lpdwNumDevices = 0;
        shim_log("SI_GetNumDevices -> 0 (%s absent)", shim_port());
        return SI_SUCCESS;
    }
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    if (lpdwNumDevices) *lpdwNumDevices = 1;
    shim_log("SI_GetNumDevices -> 1 (%s)", shim_port());
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
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = 1;
    }
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
    (void)cyHandle; (void)o;
    if (lpdwBytesReturned) *lpdwBytesReturned = 0;
    if (g_port == INVALID_HANDLE_VALUE || !lpBuffer)
        return SI_INVALID_HANDLE;
    if (!ReadFile(g_port, lpBuffer, dwBytesToRead, &got, NULL)) {
        shim_log("SI_Read err=%lu", GetLastError());
        return SI_READ_ERROR;
    }
    if (lpdwBytesReturned) *lpdwBytesReturned = got;
    if (got)
        shim_log("SI_Read %lu/%lu", got, dwBytesToRead);
    return SI_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI SI_Write(HANDLE cyHandle, LPVOID lpBuffer,
                                            DWORD dwBytesToWrite,
                                            LPDWORD lpdwBytesWritten,
                                            OVERLAPPED *o)
{
    DWORD put = 0;
    (void)cyHandle; (void)o;
    if (lpdwBytesWritten) *lpdwBytesWritten = 0;
    if (g_port == INVALID_HANDLE_VALUE || !lpBuffer)
        return SI_INVALID_HANDLE;
    if (!WriteFile(g_port, lpBuffer, dwBytesToWrite, &put, NULL)) {
        shim_log("SI_Write err=%lu", GetLastError());
        return SI_WRITE_ERROR;
    }
    if (lpdwBytesWritten) *lpdwBytesWritten = put;
    shim_log("SI_Write %lu/%lu (first byte 0x%02X)", put, dwBytesToWrite,
             dwBytesToWrite ? ((unsigned char *)lpBuffer)[0] : 0);
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
    if (!ClearCommError(g_port, &errs, &st))
        return SI_READ_ERROR;
    if (lpdwNumBytesInQueue) *lpdwNumBytesInQueue = st.cbInQue;
    if (lpdwQueueStatus) *lpdwQueueStatus = st.cbInQue ? 1 : 0;
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
    if (reason == DLL_PROCESS_ATTACH)
        shim_log("=== DSI_SiUSBXp_3_1 shim loaded, port %s ===", shim_port());
    else if (reason == DLL_PROCESS_DETACH)
        close_port();
    return TRUE;
}
