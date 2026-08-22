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

void shim_log(const char *fmt, ...);
const char *shim_port(void);

#endif /* SHIM_COMMON_H */
