/* SPDX-License-Identifier: Apache-2.0
 *
 * Shared plumbing for the two shim DLLs that give Zwift's ANT_DLL a working
 * transport to the CC2652P dongle.
 *
 * The dongle reaches this host as a CP2102N UART bridge (10C4:EA60, a COM
 * port). ANT_DLL cannot use it: its raw-bulk backend drives endpoints
 * 0x01/0x81 where the chip offers 0x02/0x82, and it has no usb_control_msg at
 * all, so it can never send the IFC_ENABLE a CP210x needs. Its third backend,
 * Silicon Labs USBXpress, has neither problem - but its two DLLs are absent.
 *
 * So we supply them, backed by the COM port. Nothing of Zwift's is replaced or
 * shadowed: these filenames do not exist, so this is purely additive and
 * deleting the two files undoes it completely.
 */
#ifndef SHIM_COMMON_H
#define SHIM_COMMON_H

#include <windows.h>
#include <stdio.h>

/* ANT_DLL accepts vendor 0x0FCF (Dynastream) or 0x1915 (Nordic) and never
 * looks at the product id - measured in its own code. So reporting the vendor
 * from here is enough, and the chip's stored identity never has to change. */
#define SHIM_VID 0x0FCF
#define SHIM_PID 0x1008

#define SI_SUCCESS            0x00
#define SI_DEVICE_NOT_FOUND   0xFF
#define SI_INVALID_HANDLE     0x01
#define SI_READ_ERROR         0x02
#define SI_WRITE_ERROR        0x04
#define SI_INVALID_PARAMETER  0x03

#define CP210x_SUCCESS        0x00
#define CP210x_INVALID_HANDLE 0x01

/* SI_GetProductString flags */
#define SI_RETURN_SERIAL_NUMBER 0x00
#define SI_RETURN_DESCRIPTION   0x01
#define SI_RETURN_LINK_NAME     0x02
#define SI_RETURN_VID           0x03
#define SI_RETURN_PID           0x04

/* Each DLL defines its own short tag ("usbxp", "cp210x"); every log line
 * carries it after the timestamp and thread id. */
extern const char shim_dll_tag[];

/* One atomic line per call (see shim_util.c). Always written when
 * RADIANT_SHIM_LOG is set, so it is the level-0 path: setup, identity,
 * errors. Gate anything chattier on shim_log_on(). */
void shim_log(const char *fmt, ...);
void shim_log_loaded(const char *dll);

/* RADIANT_SHIM_LOG_LEVEL, read once (default 1):
 *   0  setup/identity calls, errors, the "absent" reason
 *   1  + timed I/O lines (SI_Read/SI_Write, empty-read runs)
 *   2  + hex payload of every read and write (keys masked, and only whole
 *        frames whose checksum holds shown), and SI_CheckRXQueue */
int shim_log_level(void);
int shim_log_on(int level);          /* logging enabled at least at `level` */

/* Hex dump of a payload into a per-thread buffer, for level-2 lines. */
const char *shim_hex(const void *p, DWORD n);

/* "FILE_NOT_FOUND: not present" etc., for the GetLastError() of a port open. */
const char *shim_err_name(DWORD err);

/* QPC ticks and the milliseconds between two of them. */
LONGLONG shim_ticks(void);
double shim_ms(LONGLONG from, LONGLONG to);

/* Room for any port name shim_select_port() returns, with its NUL. */
#define SHIM_PORT_MAX 72

/* The port the user selected, in out ("COM11"), and 1; or 0 when there is no
 * usable selection, with the reason logged once per change. Never a guess:
 *   RADIANT_ANT_COM     a port name, COM1..COM999
 *   RADIANT_ANT_DEVICE  a USB serial number, or a device instance id, of the
 *                       port or of a device above it; it must name exactly
 *                       one present COM port
 * When both are set they must agree. Resolves afresh on every call (the
 * device form survives a renumbered port), so never call it from DllMain:
 * the device form walks the device tree. In shim_port.c, linked into the
 * USBXpress DLL only. */
int shim_select_port(char *out, size_t cap);

/* "COM" and 1-3 digits, any case. */
int shim_port_name_ok(const char *s);

#endif /* SHIM_COMMON_H */
