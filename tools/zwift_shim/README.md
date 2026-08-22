# `tools/zwift_shim/` — making the CC2652P stick visible to Zwift

Build and install with [`scripts/zwift_ant_shim.ps1`](../../scripts/zwift_ant_shim.ps1).
Full reasoning in [`docs/ti-dongle-usb-identity.md`](../../docs/ti-dongle-usb-identity.md).

## What this is

The CC2652P has **no USB peripheral**. It reaches the host as a CP2102N UART
bridge on a COM port, so `apps/dongle`'s trick — be a native USB ANT device —
cannot be copied to it. Zwift's `ANT_DLL.dll` cannot use that COM port through
its raw-bulk backend either, for two independently fatal reasons:

* the CP2102N's endpoints are `0x02`/`0x82`, fixed in silicon, and that backend
  drives `0x01`/`0x81`;
* **`usb_control_msg` is absent from ANT_DLL entirely** — it resolves 22 libusb
  entry points and not one can issue a control request, so it can never send
  the `IFC_ENABLE` a CP210x needs before passing a byte.

But ANT_DLL has a **third** backend: Silicon Labs USBXpress. It loads two DLLs
by name and drives them as a byte pipe — open, set baud, read, write, check the
receive queue. Those two files are simply absent from this machine. So we
supply them, backed by the COM port:

| file | role |
|---|---|
| `DSI_SiUSBXp_3_1.DLL` | the transport — `SI_Open`, `SI_Read`, `SI_Write`, `SI_SetBaudRate`, … over `\\.\COMn` |
| `DSI_CP210xManufacturing_3_1.dll` | the identity — `CP210x_GetDeviceVid` answers `0x0FCF` |

**ANT_DLL accepts a device on its vendor ID alone** — `0x0FCF` or `0x1915`,
with no product-id comparison anywhere in its binary. So the identity DLL
answering `0x0FCF` is sufficient, and **the dongle is never touched**: no
VID/PID reprogramming, no driver change, no elevation, and COM15 keeps working
for every tool in `tools/`.

## Why this is additive, not a modification of Zwift

Both filenames are **absent** from a stock Zwift install. Nothing of Zwift's is
replaced or shadowed — ANT_DLL simply gains a transport it did not have.
`scripts/zwift_ant_shim.ps1 -Uninstall` deletes the two files and the install is
completely undone. The installer also refuses to overwrite a file of a
different size, so if a genuine Silicon Labs DLL ever appears it wins.

No elevation is needed: Zwift self-updates, so `BUILTIN\Users` has Modify on
its directory.

## The trap that cost the most

**`ANT_Init` takes FOUR arguments, not two.** Its prologue reads `cl`, `edx`,
`r8b`, `r9b` — the export named `ANT_Init` is really
`ANT_InitExt(deviceNum, baud, ucPortType, ucFramerType)`, where `ucPortType` is
0 for USB and 1 for COM. Called with two arguments it returns FALSE **without
performing a single I/O operation** — no `LoadLibrary`, no `CreateFile` — which
reads exactly like "no dongle present" and is nothing of the sort.

That one mistake produced a whole chain of confident, wrong conclusions:
that ANT_DLL never tries its USBXpress backend, that supplying these DLLs could
not help, and that the COM transport was unreachable. All three are false.

A second, smaller trap: `SI_GetProductString` returns **ASCII** for every flag,
VID and PID included — 4-character hex, not a binary `WORD`. Returning a `WORD`
made ANT_DLL enumerate the device and then reject it before ever calling
`SI_Open`.

## Verifying

    python tools\ant_zwift_visible.py

Exit 0 means Zwift would open it. That tool loads Zwift's own `ANT_DLL.dll`, so
its verdict is Zwift's verdict rather than an inference. `--port-type 1` drives
the COM transport directly, where the device number **is** the COM port number.

## Choosing the port

By default the shim reads the CP2102N's port from
`HKLM\SYSTEM\CurrentControlSet\Enum\USB\VID_10C4&PID_EA60\…\Device Parameters\PortName`
at run time, so a renumbered port keeps working. `RADIANT_ANT_COM` overrides it
(`scripts/zwift_ant_shim.ps1 -Port COM15` pins it for your user).
`RADIANT_SHIM_LOG` names a file to log every call to — that log is how the
call sequence above was established, and it is off unless the variable is set.
