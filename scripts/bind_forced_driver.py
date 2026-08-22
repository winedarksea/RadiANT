#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Force a specific driver INF onto a device whose INF does not list it.

Called by scripts/ti_dongle_zwift.ps1; needs Administrator. Run that rather
than this, unless you are debugging the binding itself.

This is the programmatic form of Device Manager's
"Update driver -> Let me pick -> untick Show compatible hardware", which is the
only route that binds a driver package to a hardware ID the package does not
name. `pnputil` will not do it and `devcon` is not installed on this machine.

Why we want it: the CC2652P stick's CP2102N sits on `silabser.sys` as COM15,
which gives no raw control-transfer access, so its stored VID/PID cannot be
rewritten. `libusb0.sys` is already installed and signed here (Dynastream's
`ant_libusb.inf`), and pyusb's libusb0 backend already works - binding it to
the bridge opens the configuration block with nothing to download.

REVERSIBLE BY CONSTRUCTION: the hardware ID stays 10C4:EA60 throughout and
`silabser.inf` still matches it, so removing the device node and rescanning
restores COM15.

Two attempts, cheapest first:
  A. UpdateDriverForPlugAndPlayDevices + INSTALLFLAG_FORCE (what devcon does).
     Documented to force a driver that is "not a better match" - which may not
     extend to one that does not match at all.
  B. The real Have-Disk path: enumerate the INF's own driver nodes with
     DI_ENUMSINGLEINF, select one explicitly, and call the class installer.

Usage:  python bind_driver.py <hardware-id> <inf-path> [--restore]
"""
import ctypes as C
from ctypes import wintypes as W
import sys

newdev = C.WinDLL("newdev", use_last_error=True)
setupapi = C.WinDLL("setupapi", use_last_error=True)
cfgmgr = C.WinDLL("cfgmgr32", use_last_error=True)

INSTALLFLAG_FORCE = 0x00000001

DIGCF_PRESENT = 0x02
DIGCF_ALLCLASSES = 0x04
SPDIT_CLASSDRIVER = 0x00000001
DI_ENUMSINGLEINF = 0x00010000
DI_DONOTCALLCONFIGMG = 0x00020000
DIF_SELECTBESTCOMPATDRV = 0x00000017
DIF_INSTALLDEVICE = 0x00000002
DIF_REGISTERDEVICE = 0x00000019
SPDRP_HARDWAREID = 0x00000001
SPDRP_SERVICE = 0x00000004
SPDRP_FRIENDLYNAME = 0x0000000C
MAX_PATH = 260
ERROR_NO_MORE_ITEMS = 259


class GUID(C.Structure):
    _fields_ = [("d1", C.c_ulong), ("d2", C.c_ushort), ("d3", C.c_ushort),
                ("d4", C.c_ubyte * 8)]


class SP_DEVINFO_DATA(C.Structure):
    _fields_ = [("cbSize", W.DWORD), ("ClassGuid", GUID),
                ("DevInst", W.DWORD), ("Reserved", C.POINTER(W.ULONG))]


class SP_DRVINFO_DATA_W(C.Structure):
    _fields_ = [("cbSize", W.DWORD), ("DriverType", W.DWORD),
                ("Reserved", C.POINTER(W.ULONG)),
                ("Description", W.WCHAR * 256),
                ("MfgName", W.WCHAR * 256),
                ("ProviderName", W.WCHAR * 256),
                ("DriverDate", W.FILETIME), ("DriverVersion", C.c_ulonglong)]


class SP_DEVINSTALL_PARAMS_W(C.Structure):
    _fields_ = [("cbSize", W.DWORD), ("Flags", W.DWORD), ("FlagsEx", W.DWORD),
                ("hwndParent", W.HWND), ("InstallMsgHandler", C.c_void_p),
                ("InstallMsgHandlerContext", C.c_void_p),
                ("FileQueue", C.c_void_p), ("ClassInstallReserved",
                                            C.POINTER(W.ULONG)),
                ("Reserved", W.DWORD), ("DriverPath", W.WCHAR * MAX_PATH)]


setupapi.SetupDiGetClassDevsW.restype = C.c_void_p
setupapi.SetupDiGetClassDevsW.argtypes = [C.c_void_p, C.c_wchar_p, W.HWND,
                                          W.DWORD]
setupapi.SetupDiEnumDeviceInfo.argtypes = [C.c_void_p, W.DWORD,
                                           C.POINTER(SP_DEVINFO_DATA)]
setupapi.SetupDiGetDeviceRegistryPropertyW.argtypes = [
    C.c_void_p, C.POINTER(SP_DEVINFO_DATA), W.DWORD, C.POINTER(W.DWORD),
    C.c_void_p, W.DWORD, C.POINTER(W.DWORD)]
setupapi.SetupDiSetDeviceInstallParamsW.argtypes = [
    C.c_void_p, C.POINTER(SP_DEVINFO_DATA), C.POINTER(SP_DEVINSTALL_PARAMS_W)]
setupapi.SetupDiGetDeviceInstallParamsW.argtypes = [
    C.c_void_p, C.POINTER(SP_DEVINFO_DATA), C.POINTER(SP_DEVINSTALL_PARAMS_W)]
setupapi.SetupDiBuildDriverInfoList.argtypes = [
    C.c_void_p, C.POINTER(SP_DEVINFO_DATA), W.DWORD]
setupapi.SetupDiEnumDriverInfoW.argtypes = [
    C.c_void_p, C.POINTER(SP_DEVINFO_DATA), W.DWORD, W.DWORD,
    C.POINTER(SP_DRVINFO_DATA_W)]
setupapi.SetupDiSetSelectedDriverW.argtypes = [
    C.c_void_p, C.POINTER(SP_DEVINFO_DATA), C.POINTER(SP_DRVINFO_DATA_W)]
setupapi.SetupDiCallClassInstaller.argtypes = [
    W.DWORD, C.c_void_p, C.POINTER(SP_DEVINFO_DATA)]
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [C.c_void_p]
newdev.UpdateDriverForPlugAndPlayDevicesW.restype = W.BOOL
newdev.UpdateDriverForPlugAndPlayDevicesW.argtypes = [
    W.HWND, C.c_wchar_p, C.c_wchar_p, W.DWORD, C.POINTER(W.BOOL)]


def multi_sz(buf, n):
    raw = C.wstring_at(C.addressof(buf), n // 2)
    return [s for s in raw.split("\0") if s]


def find_device(hwid_wanted):
    """Return (hdevinfo, devinfo) for the device with this hardware ID."""
    h = setupapi.SetupDiGetClassDevsW(None, None, None,
                                      DIGCF_PRESENT | DIGCF_ALLCLASSES)
    if h == C.c_void_p(-1).value:
        sys.exit("SetupDiGetClassDevs failed: %d" % C.get_last_error())
    i = 0
    while True:
        info = SP_DEVINFO_DATA()
        info.cbSize = C.sizeof(SP_DEVINFO_DATA)
        if not setupapi.SetupDiEnumDeviceInfo(h, i, C.byref(info)):
            break
        i += 1
        buf = C.create_unicode_buffer(2048)
        need = W.DWORD(0)
        if not setupapi.SetupDiGetDeviceRegistryPropertyW(
                h, C.byref(info), SPDRP_HARDWAREID, None, buf,
                C.sizeof(buf), C.byref(need)):
            continue
        ids = [s.upper() for s in multi_sz(buf, need.value)]
        if hwid_wanted.upper() in ids:
            return h, info
    setupapi.SetupDiDestroyDeviceInfoList(h)
    return None, None


def current_service(h, info):
    buf = C.create_unicode_buffer(512)
    need = W.DWORD(0)
    if setupapi.SetupDiGetDeviceRegistryPropertyW(
            h, C.byref(info), SPDRP_SERVICE, None, buf, C.sizeof(buf),
            C.byref(need)):
        return buf.value
    return "<none>"


def attempt_a(hwid, inf):
    reboot = W.BOOL(0)
    ok = newdev.UpdateDriverForPlugAndPlayDevicesW(
        None, hwid, inf, INSTALLFLAG_FORCE, C.byref(reboot))
    err = C.get_last_error()
    print("  A: UpdateDriverForPlugAndPlayDevices -> %s (err %d / 0x%08X)"
          % (bool(ok), err, err & 0xFFFFFFFF))
    return bool(ok)


def attempt_b(h, info, inf):
    """The Have-Disk path: enumerate the INF's nodes and select one."""
    params = SP_DEVINSTALL_PARAMS_W()
    params.cbSize = C.sizeof(SP_DEVINSTALL_PARAMS_W)
    if not setupapi.SetupDiGetDeviceInstallParamsW(h, C.byref(info),
                                                   C.byref(params)):
        print("  B: GetDeviceInstallParams failed: %d" % C.get_last_error())
        return False
    params.Flags |= DI_ENUMSINGLEINF
    params.DriverPath = inf
    if not setupapi.SetupDiSetDeviceInstallParamsW(h, C.byref(info),
                                                   C.byref(params)):
        print("  B: SetDeviceInstallParams failed: %d" % C.get_last_error())
        return False
    if not setupapi.SetupDiBuildDriverInfoList(h, C.byref(info),
                                               SPDIT_CLASSDRIVER):
        print("  B: BuildDriverInfoList failed: %d" % C.get_last_error())
        return False

    chosen = None
    idx = 0
    while True:
        drv = SP_DRVINFO_DATA_W()
        drv.cbSize = C.sizeof(SP_DRVINFO_DATA_W)
        if not setupapi.SetupDiEnumDriverInfoW(h, C.byref(info),
                                               SPDIT_CLASSDRIVER, idx,
                                               C.byref(drv)):
            if C.get_last_error() == ERROR_NO_MORE_ITEMS:
                break
            break
        print("  B: node %d: %r (%s)" % (idx, drv.Description, drv.MfgName))
        if chosen is None:
            chosen = drv
        idx += 1
    if chosen is None:
        print("  B: the INF offered no driver nodes for this device")
        return False

    if not setupapi.SetupDiSetSelectedDriverW(h, C.byref(info),
                                              C.byref(chosen)):
        print("  B: SetSelectedDriver failed: %d" % C.get_last_error())
        return False
    print("  B: selected %r" % chosen.Description)
    for dif, name in ((DIF_REGISTERDEVICE, "REGISTERDEVICE"),
                      (DIF_INSTALLDEVICE, "INSTALLDEVICE")):
        ok = setupapi.SetupDiCallClassInstaller(dif, h, C.byref(info))
        print("  B: %s -> %s (err %d)" % (name, bool(ok), C.get_last_error()))
        if dif == DIF_INSTALLDEVICE and not ok:
            return False
    return True


def main():
    hwid = sys.argv[1]
    inf = sys.argv[2]
    print("target hardware id: %s" % hwid)
    print("driver INF:         %s" % inf)

    h, info = find_device(hwid)
    if h is None:
        sys.exit("No present device with that hardware ID.")
    print("found device, current service: %s" % current_service(h, info))

    ok = attempt_a(hwid, inf)
    if not ok:
        ok = attempt_b(h, info, inf)

    h2, info2 = find_device(hwid)
    if h2 is not None:
        print("after: service = %s" % current_service(h2, info2))
        setupapi.SetupDiDestroyDeviceInfoList(h2)
    print("RESULT: %s" % ("bound" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
