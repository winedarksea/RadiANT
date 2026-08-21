# Making the CC2652P stick visible to Zwift

**Status (2026-08-21): `cap_zwift.ps1` — done and verified. Zwift — blocked on
three third-party pieces, with the route now measured rather than guessed.**

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
driver change required (see `tools/` and the scratch `usb_desc.py` this came
from). **A CP210x's endpoint addresses are fixed in silicon.** No amount of
reprogramming moves them off `0x02`/`0x82`.

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

**Only two of them are reachable.** Choosing the VCP transport requires
`ANT_InitExt`'s `ucPortType` argument, and `ANT_InitExt` **is not exported** —
the export table has 154 names, `ANT_Init` among them, and nothing that takes a
port type. So "it is already on COM15, just point Zwift at the COM port" is not
an option, however inviting it looks.

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

The tempting plan, and the one recorded in earlier notes, is: reprogram the
CP2102N to `0FCF:1009`, let the already-installed `ant_libusb.inf` bind
`libusb0.sys` to it, done. It fails twice over.

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

## The route that remains

`USBDeviceSI` — Silicon Labs USBXpress. On that path the endpoints are
irrelevant, because `SiUSBXp.sys` drives the chip natively, and the UART
configuration is done through `SI_SetBaudRate` / `SI_SetFlowControl` /
`SI_SetLineControl` rather than by the caller. ANT_DLL uses
`CP210x_GetDeviceVid` / `CP210x_GetDevicePid` to identify what USBXpress
enumerated. This is how the CP210x-based Dynastream sticks worked.

Three pieces are needed, and **all four candidate files were confirmed absent
from the entire drive**:

| Piece | Goes where | From |
|---|---|---|
| `DSI_SiUSBXp_3_1.DLL` | beside `ANT_DLL.dll` | ANT SDK / ANT PC libraries |
| `DSI_CP210xManufacturing_3_1.dll` | beside `ANT_DLL.dll` | same |
| CP210x **USBXpress** driver, signed INF listing the chosen VID/PID | installed | Silicon Labs, or the ANT driver package |
| a CP2102N configuration tool | to write the VID/PID | Simplicity Studio Xpress Configurator, or AN721 |

**Pick a PID that `ant_libusb.inf` does not claim.** That INF (Dynastream,
2012, `oem79.inf`) binds `libusb0.sys` to `0FCF:1008` and `0FCF:1009` only. Use
something else — `0FCF:1004` (ANT2USB) is the natural choice — so the nRF
sticks keep working untouched and `libusb0` does not race USBXpress for the
device. Since ANT_DLL ignores the PID entirely, the choice is free.

### Recovery, to be written down before anything is programmed

Changing the VID/PID **removes COM15**, which is the only host path to this
board and the one every tool in `tools/` uses. JTAG still reaches the CC2652P,
so the board is never unreachable — but its USB identity can only be changed
back through whatever driver is bound to it afterwards. Install the USBXpress
driver for the new ID as part of the same operation, so
`CP210xManufacturing` can still see the chip. If nothing binds, a WinUSB or
libusb binding plus a vendor-request tool is the way back.

## The agreed next step: one cheap decisive test

Rather than assemble the whole USBXpress stack on a hypothesis, do the one
experiment that settles the raw-bulk question outright, because it needs a
single free download and **no driver work at all**.

1. **Get write access to the configuration block.** Two ways, and the second
   needs no download:

   **(a) Silicon Labs' CP21xx Device Customization Software** (AN721), or
   Xpress Configurator in Simplicity Studio. Works over the VCP driver, so
   nothing else changes.

   **(b) Bind `libusb0` to the chip and use `tools/cp2102n_ids.py`.** Every
   piece of this is already on the machine: `libusb0.sys` is installed and
   signed (Dynastream, 2012, `oem79.inf`), and pyusb's libusb0 backend
   already works. In Device Manager, on *Silicon Labs CP210x USB to UART
   Bridge (COM15)*: **Update driver -> Browse -> Let me pick from a list ->
   untick "Show compatible hardware" -> "ANT LibUSB Drivers" / "ANT USB-m"**.

   Forcing a driver whose INF does not list the hardware ID is only possible
   through that dialog - `pnputil` will not do it and `devcon` is not
   installed here - which is why this step is manual.

   **It is trivially reversible, and that is the point:** the hardware ID
   stays `10C4:EA60` throughout, and `silabser.inf` still matches it. Device
   Manager -> uninstall the device -> Scan for hardware changes puts COM15
   straight back.
2. **Set the chip to VID `0x0FCF`, PID `0x1008`.** The PID choice is
   deliberate: `oem79.inf` is already installed and already binds
   `libusb0.sys` to `0FCF:1008` with a 2012 Dynastream signature, so the
   device gets a working driver with no INF editing and no signing. And it
   leaves `0x1009` free, so this board can never be confused with the real
   nRF52840 sticks.
3. **Run `python tools/ant_zwift_visible.py`.** It drives Zwift's own DLL, so
   its verdict is Zwift's verdict.
   * *PASS* — ANT_DLL discovers endpoints from the descriptor after all, and
     we are done.
   * *enumerated but `ANT_Init` fails* — the `0x02`/`0x82` and `IFC_ENABLE`
     blockers are confirmed fatal, and the USBXpress route is the remaining
     one. Expected.
4. **Either way, `tools/cp2102n_ids.py` becomes usable at that point**, because
   `libusb0` gives raw control-transfer access to the configuration block. Run
   it with no arguments first: it cross-checks the stored IDs against what the
   device enumerated as, and reproduces the stored Fletcher checksum. Only when
   both hold will it write.

**Do not skip step 4's dry run.** Changing the IDs removes COM15 and with it
the Silicon Labs utility's view of the chip, so `cp2102n_ids.py` is the way
back. It refuses to write until it has proved it understands the block, which
is what makes it safe to rely on. If it is ever wrong, a Linux host can reach
the same chip by VID/PID with `cp210x-cfg`, and JTAG always reaches the
CC2652P, so the board itself is never lost.

If the USBXpress route does become necessary, note that **the ANT SDK's own
`DSI_SiUSBXp_3_1.dll` and `DSI_CP210xManufacturing_3_1.dll` are 32-bit and
therefore useless here** — Zwift's `ANT_DLL.dll` is x86-64. The x64 builds have
to come from Silicon Labs' current SDKs and be renamed. They can go in
`System32`, which is on ANT_DLL's search path, rather than into Zwift's own
directory.

## `cap_zwift.ps1` — fixed, and the bug was ours

The TI stick failed capture with "No device matching /ANT USB-m/", which reads
exactly like an unplugged or unflashed board. Both readings were wrong, and so
was the note in this repo claiming USBPcap omits the device. USBPcap lists it
perfectly well:

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
