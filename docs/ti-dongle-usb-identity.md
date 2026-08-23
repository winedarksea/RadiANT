# Making the CC2652P stick visible to Zwift

Zwift's own ANT_DLL opens the CC2652P stick and talks ANT to it — with no
change to the dongle, no driver change, no reprogramming and no elevation.

The fix is two small DLLs supplying ANT_DLL's USBXpress backend, backed by the
COM port the dongle is already on: `scripts/zwift_ant_shim.ps1` builds and
installs them, `-Uninstall` removes them. Verified end to end:

    ANT_Init(0, 57600, port_type=0, framer=0) -> True
    ANT_ResetSystem -> True          (7-byte 0xA4 frame out, replies read back)
    ant_probe.py --port COM15        -> still PASS, unaffected

**`ANT_Init` takes FOUR arguments, not two.** Its prologue reads `cl`, `edx`,
`r8b`, `r9b` — the export named `ANT_Init` is really `ANT_InitExt(deviceNum,
baud, ucPortType, ucFramerType)`, and `ANT_InitExt` itself is **not exported**
(the export table has 154 names and nothing that takes a port type). Called
with two arguments it fails **without performing a single I/O operation**,
which reads exactly like "no dongle present" and is nothing of the sort. Read
the disassembly before concluding a library does not do something.

This file exists because "the nRF dongle works, so copy that" is the obvious
plan and it cannot work. The reason is hardware, and everything below was
measured on the bench rather than reasoned from datasheets.

## The two dongles are not the same shape of device

| | nRF52840 stick (`apps/dongle`) | CC2652P stick (`apps/dongle_ti`) |
|---|---|---|
| USB peripheral | in the SoC | **none — the CC2652P has no USB at all** |
| reaches the host as | native USB, `0FCF:1009` "ANT USB-m" | a CP2102N UART bridge, `10C4:EA60`, a COM port |
| bulk endpoints | `0x01` OUT / `0x81` IN | **`0x02` OUT / `0x82` IN** |
| Windows driver | `libusb0.sys` via `ant_libusb.inf` | `silabser.sys` (Silicon Labs VCP) |

The endpoint addresses are the first hard fact. They were read off the live
device from its hub with `IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX`, no
driver change required. **A CP210x's endpoint addresses are fixed in silicon.**
No amount of reprogramming moves them off `0x02`/`0x82`.

The part is a **CP2102N**, not a classic CP2102 — confirmed from its product
string, `'CP2102N USB to UART Bridge Controller'`. That matters for safety, not
function: the CP2102N keeps its configuration in **rewritable flash**, so a
VID/PID change is reversible. On a classic CP2102 it is one-time-programmable
EPROM and there is no way back.

## What ANT_DLL will and will not accept

Measured against Zwift's own `ANT_DLL.dll` (ALU3.200, x86-64, SHA-256
`db4d99c5…62f9`). `tools/ant_zwift_visible.py` drives this binary directly, so
"would Zwift see it?" is a command rather than an opinion.

**It has three transports**, visible in its RTTI:

    USBDeviceLibusb + LibusbLibrary   raw bulk through libusb0.dll
    USBDeviceSI     + SiLabsLibrary   Silicon Labs USBXpress (SI_Open/SI_Read/…)
    DSISerialVCP                      a COM port, opened as "\\.\COM%u"

All three are compiled in and reachable. ANT_DLL statically imports the whole
Win32 serial API (`CreateFileA/W`, `GetCommState`, `SetCommState`,
`GetCommTimeouts`, `SetCommTimeouts`, `PurgeComm`, `SetCommMask`,
`WaitCommEvent`, `EscapeCommFunction`), which together with the `\\.\COM%u`
format string is `DSISerialVCP` — not dead code. Which transport you get is
`ucPortType`'s to decide, and that is the argument the unexported `ANT_InitExt`
signature hides.

**The acceptance test is on the vendor ID alone.** Two helpers do it, at
`0x00ACDE` and `0x00ACFC`:

```asm
call [rax]          ; GetVid()
mov  ecx, 0FCFh     ; Dynastream
cmp  ax, cx
je   accept
mov  ecx, 1915h     ; Nordic Semiconductor
cmp  ax, cx
je   accept
xor  eax, eax       ; reject
```

There is **no product-ID comparison anywhere in the binary**. That is a gift:
the PID is free, so it can be picked to avoid colliding with the `libusb0`
binding the nRF sticks depend on.

## Why the cheap impersonation cannot work — and this is proven, not suspected

The tempting plan is: reprogram the CP2102N to `0FCF:1009`, let the
already-installed `ant_libusb.inf` bind `libusb0.sys` to it, done. It fails
twice over.

1. **Endpoints.** That path drives `0x01`/`0x81`; the chip offers `0x02`/`0x82`.
2. **The decisive one: `usb_control_msg` is ABSENT from the binary.** ANT_DLL
   resolves 22 libusb entry points by name — `usb_bulk_read`,
   `usb_interrupt_write`, `usb_claim_interface`, `usb_clear_halt` and so on —
   and **not one of them can issue a control request.** A CP210x passes no UART
   data until it receives the `IFC_ENABLE` vendor request. ANT_DLL has no means
   of sending it, to this or any other device.

So the raw-bulk backend can never drive a CP210x, and by the same token
`0FCF:1008` "ANT USB Stick 2" cannot be a CP210x design either. **Do not spend
another session on the libusb route.**

## The route that works: USBXpress, shimmed

`USBDeviceSI` — Silicon Labs USBXpress. On that path the endpoints are
irrelevant, because the driver drives the chip natively and the UART
configuration is done through `SI_SetBaudRate` / `SI_SetFlowControl` /
`SI_SetLineControl` rather than by the caller. ANT_DLL uses
`CP210x_GetDeviceVid` / `CP210x_GetDevicePid` to identify what USBXpress
enumerated, and accepts on the VID alone. `scripts/zwift_ant_shim.ps1` supplies
`DSI_SiUSBXp_3_1.DLL` and `DSI_CP210xManufacturing_3_1.dll` as shims over the
existing COM port, reporting VID `0FCF`; the ANT SDK's own copies of those two
names are 32-bit and therefore useless against an x86-64 `ANT_DLL.dll`.

Nothing about the dongle's own USB identity has to change for this. If a VID/PID
rewrite is ever wanted anyway, `tools/cp2102n_ids.py` is the tool: run it with
no arguments first, where it cross-checks the stored IDs against what the
device enumerated as and reproduces the stored Fletcher checksum, and it
refuses to write until both hold. Note that changing the IDs **removes COM15**,
the only host path every tool in `tools/` uses, so install a driver for the new
ID as part of the same operation. JTAG always reaches the CC2652P, so the board
itself is never lost.

## `cap_zwift.ps1` — the "no device" failure was ours

The TI stick failed capture with "No device matching /ANT USB-m/", which reads
exactly like an unplugged or unflashed board. USBPcap lists it perfectly well:

    value {arg=99}{value=6}{display=[6] Silicon Labs CP210x USB to UART
    Bridge}{enabled=true}{parent=2}

The script was discarding it. It filtered on "has no `{parent=}`", which
conflates two unrelated things:

* `value=6` — a real USB device, and `6` **is** its address for `--devices`.
* `value=4_2` — an *interface* of a composite device, not an address.

`{parent=…}` is a different axis: for a device it names the hub it is plugged
into. A dongle in a root port has no parent; the same dongle on an external hub
has one and is every bit as addressable. The nRF sticks were never affected
only because they happened to be in root ports. The test is now "is the value
an address", and the capture works end to end — 10,384 packets, 576 bulk
payloads, frames starting `a4`, `STARTUP_MESG 20` on the wire.

**Capture still needs Administrator.** Unelevated, USBPcapCMD relaunches itself
and writes a 0-byte file while the launching shell loses track of it. Use
`-Elevate` and accept the prompt. `-Seconds` defaults to 300; `-Seconds 0` runs
until Ctrl-C, which stops it cleanly and still leaves a usable file — worth
knowing because getting Zwift up and paired takes well over a minute.
