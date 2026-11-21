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

### The log

`RADIANT_SHIM_LOG_LEVEL` sets how much goes in
(`scripts/zwift_ant_shim.ps1 -LogLevel N` pins it for your user):

| level | logs |
|---|---|
| `0` | setup and identity calls, errors, and why the port is absent. Use it for long rides. |
| `1` (default) | level 0, plus one timed line per non-empty read and per write, and runs of empty reads folded into one line |
| `2` | level 1, plus the hex of every read and write, and `SI_CheckRXQueue` calls, so the log can be re-framed like a pcap. Key material in a write (0x46, 0x7E, 0xF2, 0xF5 bodies and their checksums) shows as `..`, and a write that is not whole 0xA4 frames logs only the byte count past the last whole frame |

Every line has the same prefix:

    12345.678901 t4321 usbxp SI_Write 13/13 in 0.081 ms id=0x4F
    12345.689410 t5120 usbxp SI_Read 7/64 in 10.204 ms
    12346.702113 t5120 usbxp SI_Read 0/64 x37 over 1003.2 ms (timeouts)

* **QPC seconds since boot**, 1 µs resolution. Both DLLs read the same clock,
  so their lines sort into one timeline. The `=== … loaded ===` line also gives
  the wall-clock time and PID, for lining the log up with a pcap or Zwift's log.
* **`t<id>`**: the calling thread. ANT_DLL reads on one thread and writes on
  another.
* **`usbxp` / `cp210x`**: which of the two DLLs wrote the line.

Each line goes out as one append-only `WriteFile`, so lines from the two
threads and the two DLLs cannot glue together or blank each other, and nothing
is lost if the process dies.

Reading it:

* **Host turnaround** is the gap between an `SI_Write` line and the next
  non-empty `SI_Read` line. For example, from the `SYSTEM_RESET` write
  (`id=0x4A`) to the read carrying `STARTUP_MESG`.
* **`in N ms`** is how long the call blocked. A read returns about 10 ms after
  its last byte (the inter-byte timeout), so a non-empty read shows that much
  on top of the wait for data.
* **`id=`** lists the ANT message id of each `0xA4` frame in a write.
* **`absent, err=N`** on `SI_GetNumDevices` says why the port could not be
  opened: `2` not present, `5` held by another process (close `ant_scan` and
  friends), `31` device error. The line before it says where the port name
  came from: `RADIANT_ANT_COM`, the registry, or the `COM15` fallback.

`tools/zwift_shim/test_shim_log.py` checks the line format under 8 threads
calling both DLLs at once. Build first with
`scripts/zwift_ant_shim.ps1 -BuildOnly`.
