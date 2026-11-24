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
VID/PID reprogramming, no driver change, no elevation, and the COM port keeps
working for every tool in `tools/`.

## Why this is additive, not a modification of Zwift

Both filenames are **absent** from a stock Zwift install. Nothing of Zwift's is
replaced or shadowed — ANT_DLL simply gains a transport it did not have.
`scripts/zwift_ant_shim.ps1 -Uninstall` deletes the two files and the install is
completely undone. Both DLLs carry a version resource (ProductName `RadiANT
Zwift ANT shim`, `shim_version.h`), and the installer replaces, and
`-Uninstall` deletes, only a file that has it, or the one earlier build it
knows by SHA-256 (903718a, installed 2026-11-23, which predates the resource).
So if a genuine Silicon Labs DLL ever appears it wins, and an older shim can
still be upgraded in place. Until 2026-11-24 "ours" meant "same file size",
which refused every rebuild and let `-Uninstall` delete by name.

The shim is always built here, from this checkout, with `/W4 /WX /sdl
/guard:cf`; it is not a release artifact (`SECURITY.md`).

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

The shim forwards every host write to its port, Zwift's ANT+ network key
included, so **it drives only the port you select and never guesses**. With
no selection, or one that is unusable or ambiguous, `SI_GetNumDevices`
answers 0 and Zwift sees no device; the log says why. Two variables, pinned
for your user by the installer:

| variable | installer | value |
|---|---|---|
| `RADIANT_ANT_COM` | `-Port COM11` | a port name, `COM1`..`COM999` (any case); nothing else is accepted |
| `RADIANT_ANT_DEVICE` | `-Device L1100TY2` | a USB serial number, or a device instance id, of the port or of a USB device above it |

The device form survives the port being renumbered, and is resolved against
**present** ports each time ANT_DLL enumerates. It must name exactly one: an
XDS110 has two ports (application and auxiliary UART) under one serial
number, so `-Device L1100TY2` is ambiguous and the log lists both ports with
their instance ids; give the instance id of the one you want
(`USB\VID_0451&PID_BEF3&MI_00\...`). Serial-number parts Windows made up
(they contain `&`) never match. With both variables set they must name the
same port. `scripts/zwift_ant_shim.ps1 -ListDevices` prints every present
port with its serial number and instance id.

Until 2026-11-24 the shim took the first CP2102N `PortName` in the registry
(present or not) and fell back to `COM15`, so it could send the network key
to whatever else was on that port.

`RADIANT_SHIM_LOG` names a file to log every call to — that log is how the
call sequence above was established, and it is off unless the variable is set.

### The log

`RADIANT_SHIM_LOG_LEVEL` sets how much goes in
(`scripts/zwift_ant_shim.ps1 -LogLevel N` pins it for your user):

| level | logs |
|---|---|
| `0` | setup and identity calls, errors, and why the port is absent. Use it for long rides. |
| `1` (default) | level 0, plus one timed line per non-empty read and per write, and runs of empty reads folded into one line |
| `2` | level 1, plus the hex of every read and write, and `SI_CheckRXQueue` calls, so the log can be re-framed like a pcap. Key material in a read or a write (0x46, 0x76, 0x7E, 0x7F RNG seed, 0x83, 0xF2, 0xF5 bodies and their checksums) shows as `..`. Only whole 0xA4 frames whose checksum holds are shown or named; past the first one that is not, only the byte count of the rest is logged (`unframed` on a write when no frame passed). Reads are not cut on frame boundaries, so a frame split across two reads shows as two counts: the price of not trusting a device that echoes |

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
  on top of the wait for data. A write's time is its own I/O only: the port is
  opened for overlapped I/O, so a write no longer waits for a pending read.
  Shims built before 2026-11-23 held each write behind ANT_DLL's read, up to
  the 1000 ms read timeout on a quiet radio, so older logs show long writes
  that ended about 0.15 ms after a read returned. On a working port a write
  takes well under 5 ms; much more is worth a look.
* **`id=`** lists the ANT message id of each `0xA4` frame in a write.
* **`absent, err=N`** on `SI_GetNumDevices` says why the port could not be
  opened: `2` not present, `5` held by another process (close `ant_scan` and
  friends), `31` device error. A `port COMn (from ...)` line says where the
  port came from (`RADIANT_ANT_COM` or `RADIANT_ANT_DEVICE`) whenever the
  selection changes; `SI_GetNumDevices -> 0 (no port selected)` follows a
  `no port: ...` line giving the reason.
* **`already open here`** on `SI_GetNumDevices` means this shim holds the port
  (the probe got `5`). **`open here, but probe err=N … port may be gone`**
  means the probe failed some other way while the shim holds the port, as
  when the dongle has left USB. The call still answers 1 either way.

`tools/zwift_shim/test_shim_log.py` checks the line format under 8 threads
calling both DLLs at once; key redaction in reads and writes; that no
selection means no device and only `COMn` is a port name; and, on a named
pipe, that a write is not held behind a pending read, that racing `SI_Open`s
keep the winner's handle, that a close does not wait out a pending read, and
that a zero read timeout cannot hang a read. The pipe tests load
`out/test/DSI_SiUSBXp_3_1.DLL`, built with `SHIM_TEST_PIPES` so it also opens
`pipe\radiant_shim_*`; that build is never installed. With
`RADIANT_SHIM_TEST_COM=COMn` it also runs the pending-read check on a dongle.
Build first with `scripts/zwift_ant_shim.ps1 -BuildOnly`: the tests refuse to
run when a source file is newer than `out/` or a DLL lacks the version
resource, since a stale `out/` once let them pass on a build without the key
masking.
