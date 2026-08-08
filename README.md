# ANT+ USB Dongle

Turns an **Adafruit Feather nRF52840 Express** into an ANT+ USB stick. Copy one
file onto the board and Zwift, TrainerRoad, Garmin Express, openant — anything
that looks for an ANT stick — will find it. No soldering, no wiring.

The firmware enumerates as `VID 0x0FCF / PID 0x1009`, the identity of a
Dynastream ANT USB-m, and speaks the standard ANT serial protocol over a bulk
vendor interface. Eight channels, acknowledged data and burst all work, so a
fitness app can run heart rate, power, cadence and trainer control at once.

> **Windows users:** this needs the same libusb-win32 driver a retail ANT stick
> needs, and Windows will not offer it on its own. Read
> [Windows drivers](#windows-drivers) before anything else.

---

## Getting started

### 1. Get the right board

**[Adafruit Feather nRF52840 Express](https://www.adafruit.com/product/4062)**
(product 4062, about $25, or the ItsyBitsy version, product 4481).

> **Not the Feather Sense (4516).** The Sense has no 32.768 kHz crystal and
> falls back to an internal RC oscillator at ±500 ppm. ANT requires ±50 ppm —
> the Sense is 10× outside tolerance and cannot hold a channel synchronised.

Check your cable too. Plenty of USB cables are charge-only, and with one of
those the board never appears at all.

### 2. Flash it

1. Plug the Feather in.
2. **Double-tap the RESET button.** A USB drive named `FTHR840BOOT` appears.
3. Drag `ant_dongle.uf2` onto that drive.

The drive ejects itself and the board reboots as an ANT+ dongle. That's the
whole install — the firmware lives on the board, and there is nothing to
configure.

Grab `ant_dongle.uf2` from the
[latest release](../../releases/latest), or [build it yourself](#building-from-source).

### 3. Set up your platform

| Platform | What you need to do |
|---|---|
| **Windows** | Install a libusb-win32 driver — see [Windows drivers](#windows-drivers). Same requirement as a genuine ANT stick. |
| **macOS** | Nothing. It is a vendor-class device, so libusb claims it directly. |
| **Linux** | Install the udev rule so you don't need root: `sudo cp host/linux/99-ant-usb.rules /etc/udev/rules.d/ && sudo udevadm control --reload-rules && sudo udevadm trigger`, then replug. |
| **Android** | Needs USB host/OTG. Apps should include [`host/android/device_filter.xml`](host/android/device_filter.xml) so the dongle is recognised on attach. |

### 4. Check it works

```sh
pip install pyusb
python tools/ant_probe.py
```

Expected output: `PASS: reset -> startup -> capabilities -> version`.

Then open your app of choice and pair a sensor as you would with any ANT+
stick. To confirm the radio itself is hearing sensors, see
[Testing the radio](#testing-the-radio).

### Other boards

The **[Nordic nRF52840 Dongle](https://www.nordicsemi.com/Products/Development-hardware/nRF52840-Dongle)**
(PCA10059, about $10) also works, and is already a USB stick if you would rather
not have a Feather hanging off the port. It has the 32.768 kHz crystal ANT
needs. Flashing is less convenient — there is no drag-and-drop drive, so it
takes the Nordic DFU package `ant_dongle_nrf52840dongle.zip`:

1. Insert the dongle and press the small side RESET button. The red LED pulses
   slowly: that is the bootloader waiting.
2. [nRF Connect for Desktop](https://www.nordicsemi.com/Products/Development-tools/nrf-connect-for-desktop)
   → **Programmer** → select the device → **Add file** → **Write**.

The **Pro Micro nRF52840** footprint — nice!nano and the unbranded "SuperMini"
clones, a few dollars on AliExpress — works too, and takes
`ant_dongle_promicro.uf2` by the same double-tap-and-drag as the Feather. Its
bootloader uses the same `0x26000` layout.

The catch is that these are not one board. What they have in common is a
schematic; what they leave off varies by seller, and the **32.768 kHz crystal
is the part most likely to be missing** — it is exactly the component a $2 board
saves money on. ANT needs that clock inside ±50 ppm.

If the board never enumerates at all — no ANT+ device, no COM port, nothing —
that is the missing crystal, not a bad cable. `ant_init()` gates `usb_enable()`
in [main.c](src/main.c#L96), so a low-frequency clock that never starts stops
the firmware before USB comes up. Flash `ant_dongle_promicro_synth.uf2`
instead: it synthesizes the 32.768 kHz clock from the 32 MHz crystal, which
every nRF52840 must have for its radio to work at all.

Everything after that is identical; the firmware is the same. Any other
nRF52840 board needs a partition map of its own — see
[Building from source](#building-from-source).

---

## Windows drivers

A retail ANT+ stick does not work on Windows out of the box either, and this
one is deliberately no different. The release build ships **no MS OS
descriptors**, so Windows does not bind `winusb.sys` and the device arrives
with no driver, showing problem code 28 — exactly like a genuine stick.

Bind libusb-win32 by whichever route suits you:

- **Windows Update** *(easiest, nothing to download)*. Dynastream still
  publishes WHQL drivers for both PIDs; they appear as *ANT USB-m* and
  *ANT USB Stick 2* under **Settings → Windows Update → Advanced options →
  Optional updates → Driver updates**.
- **Garmin's ANT+ driver package**, which matches on the hardware ID:
  `pnputil /add-driver ANT_LibUsb.inf /install` from an elevated shell. Zwift
  bundles the same package at
  `C:\Program Files (x86)\Zwift\Windows ANT Dongle Driver\`.
- **[Zadig](https://zadig.akeo.ie)**: *Options → List All Devices*, select the
  ANT stick, choose **libusb-win32** — not WinUSB, not libusbK, because
  `ANT_DLL` calls the libusb0 API specifically.

After installing, **replug the dongle**.

<details>
<summary><b>Why not WinUSB?</b></summary>

Zwift's `ANT_DLL.dll` reaches USB only through `libusb0.dll`,
`DSI_SiUSBXp_3_1.DLL` and `DSI_CP210xManufacturing_3_1.dll`. There is no WinUSB
path in it at all, so a WinUSB-bound device is invisible to Zwift no matter how
correct the ANT protocol behaviour is. Worse, advertising a WinUSB compatible ID
makes Windows match `USB\MS_COMP_WINUSB`, consider the device driven, and stop
looking for anything better.

If you would rather have WinUSB and only use the Python tools in `tools/`,
build with `CONFIG_ANT_DONGLE_MSOS_DESCRIPTORS=y`. Zwift will not see it.

</details>

<details>
<summary><b>Windows won't offer the driver, and Device Manager can't find it</b></summary>

That is expected and says nothing about the device. The driver is an *optional*
update (`AutoSelectOnWebSites = False`), so it is never installed
automatically; and Device Manager's *Update driver* wizard on Windows 11 does
not query Windows Update at all, searching only the local driver store. It logs
this to `%windir%\INF\setupapi.dev.log` as

```
!    ndv:      Searching Windows Update has been disabled for the Update Wizard.
!    ndv:      Policy has been set to prevent searching Windows Update for drivers.
!    dvi:      Error 0xe0000228: There are no compatible drivers for this device.
```

despite no such policy being set. Use the Settings → Optional updates path
above instead.

</details>

<details>
<summary><b>"There are no optional updates available at this time"</b></summary>

Check whether Windows Update is paused. A pause empties that page completely —
every pending driver, not just this one — so it looks identical to the driver
not existing. Settings → Windows Update shows a *Resume updates* button when
paused; the underlying dates are at
`HKLM:\SOFTWARE\Microsoft\WindowsUpdate\UX\Settings` in `PauseUpdatesStartTime`
/ `PauseUpdatesExpiryTime`.

To see what is really on offer regardless of the pause, ask the update agent
directly:

```powershell
$s = New-Object -ComObject Microsoft.Update.Session
($s.CreateUpdateSearcher().Search("IsInstalled=0 and Type='Driver'")).Updates |
  ForEach-Object { $_.Title }
```

</details>

<details>
<summary><b>Driver installed, still not working</b></summary>

A device node that already failed a driver search carries `ConfigFlags = 64`
(`CONFIGFLAG_FAILEDINSTALL`) and reuses that cached verdict. Check with:

```powershell
Get-PnpDeviceProperty -InstanceId <id> -KeyName DEVPKEY_Device_ConfigFlags
```

Replugging usually clears it. Note also that Zwift's own installer
(`DriverPackagePreinstallW`) can fail with `0xE000024B` — check
`%windir%\DPINST.LOG`, and fall back to `pnputil` or Windows Update. The
package's 2012 catalog is signed by *Microsoft Windows Hardware Compatibility
Publisher* and still verifies as `Valid` on Windows 11, inside the
pre-2015-07-29 grandfather window for driver signing, so `pnputil` accepts it.

</details>

---

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Board never appears at all | Charge-only USB cable, or the UF2 was never copied. Double-tap RESET — if the bootloader drive appears, the board is fine. On a Pro Micro clone that flashes but never enumerates, suspect a missing 32.768 kHz crystal and flash the `_synth` build — `ant_init()` gates `usb_enable()`, so a clock that never starts stops the firmware before USB. |
| Enumerates, but the app can't see it | On Windows, no libusb-win32 driver bound. See [Windows drivers](#windows-drivers). |
| `ant_probe.py` reports a version starting with `STUB` | You flashed a radio-stub build. It enumerates correctly but has no radio — flash a release image. |
| Two dongles attached, tools refuse to run | Both are `0FCF:1009`. Pass `--serial` to pick one; the tools list the serials they found. |
| `ant_scan.py` hears nothing | Sensors are only audible while transmitting. A strap has to be worn, cranks have to turn. |
| Channels won't open after a crashed session | The stack keeps its channel state. Every host should open with a system reset; `ant_probe.py` does. |
| App sees the dongle but lists no ANT+ sensors, only Bluetooth ones | Something the app sends is going unanswered, so its search never starts. Check the app's log for a stuck retry loop (Zwift: `Stopping ANT search` repeating in `%LOCALAPPDATA%\Zwift\Logs\Log.txt`). If *nothing* the app sends is answered, suspect framing before logic — the checksum must include the `0xA4` SYNC byte. If only some messages are, every one the app uses should answer `0`, not `0x28`. |

---

## For developers

### Layout

| Path | Purpose |
|---|---|
| `src/main.c` | Init ordering: `ant_init()` before the transport comes up, so ANT/MPSL owns HFXO startup |
| `src/ant_transport.h` | The three-function contract the bridge talks to. One of the three files below implements it |
| `src/usb_ant_class.c` | Bulk vendor USB class on the **legacy** stack, plus the optional MS OS 1.0/2.0 descriptors. The proven one |
| `src/usb_ant_class_next.c` | The same class on the **new** USBD/UDC stack. Required on nRF54; see [Transports](#transports) |
| `src/ant_uart_transport.c` | ANT serial over a plain UART, for parts with no USB peripheral |
| `src/ant_serial_bridge.c` | ANT serial protocol (`0xA4` framing) ↔ sdk-ant API |
| `src/ant_stub.c` | No-op radio, compiled only when `CONFIG_ANT_DONGLE_RADIO_STUB=y` |
| `src/diag_flash_log.c` | Log backend that commits to flash, readable back over UF2 |
| `pm_static_*.yml`, `sysbuild.cmake` | Pin the application where each board's bootloader expects it — `0x26000` on the Feather, `0x1000` on the dongle. See the gotchas |
| `boards/` | Per-board `.conf`/`.overlay`: output format, code partition, and prising the console off the USB device we need |
| `scripts/` | Windows helpers: environment, flashing, DFU packaging, USB cache reset |
| `tools/ant_probe.py` | Protocol smoke test |
| `tools/ant_scan.py` | Wildcard ANT+ channel; reports the sensors it hears |
| `tools/ant_session.py` | The eight-channel session a fitness app runs, including ack and burst |
| `tools/ant_bench.py` | Round-trip latency and throughput, for comparing one USB stack against the other |
| `host/` | Linux udev rule, Android device filter |

### Transports

The bridge does not know what carries its bytes. It needs a byte sink, a byte
source, and a way to signal that it has drained its input —
[ant_transport.h](src/ant_transport.h) — and exactly one of three
implementations is compiled in, chosen by `CONFIG_ANT_DONGLE_TRANSPORT_*`.

| Transport | Where it applies | Status |
|---|---|---|
| `USB_LEGACY` | nRF52/nRF53 — anything with a Nordic USBD peripheral | The shipping build. Verified against Zwift |
| `USB_NEXT` | Same, plus every nRF54 part that has USB at all | Verified on a Feather against a real host; DWC2 itself untested |
| `UART` | Parts with no USB device peripheral, e.g. nRF54L15 | Builds and links; not yet run on hardware |

The default follows the devicetree rather than the board name: a
`nordic,nrf-usbd` node selects the legacy stack, an `snps,dwc2` node selects the
new one, and neither means the part has no USB device peripheral and the ANT
serial protocol goes out a UART instead. CI asserts the resulting choice per
board, because a defaulted-from-hardware decision fails silently — a renamed
node would just quietly build something else.

**The new stack is not an upgrade, it is a requirement.** Every nRF54 part with
USB carries a DesignWare DWC2 controller, and Zephyr's only DWC2 driver is
[udc_dwc2.c](https://github.com/zephyrproject-rtos/zephyr/blob/main/drivers/usb/udc/udc_dwc2.c),
which belongs to the new stack. There is no `usb_dc_dwc2.c`. So on those parts
the legacy stack is not older-but-available, it is absent.

That is also why nRF52840 has not been moved over. The legacy build is the one
with hours on it against Zwift, and the new stack buys nothing there — the same
controller, the same descriptors, the same endpoints. What it buys is that
`usb_ant_class_next.c` can be tested on hardware that exists, ahead of hardware
that does not, and that the two can be measured against each other:

```powershell
west -z C:\ncs\v3.2.4\zephyr build -s C:\Users\Colin\ant_dongle `
  -d C:\Users\Colin\ant_dongle\build\feather_next `
  -b adafruit_feather_nrf52840/nrf52840/uf2 -p always -- "-DEXTRA_CONF_FILE=next.conf"
```

That build enumerates as the same `0FCF:1009` with the same 16-character serial
and passes `ant_probe.py` and `ant_scan.py` identically to the legacy one. One
Kconfig warning is expected and harmless — `USB_NRFX_ATTACHED_EVENT_DELAY` lives
inside `if USB_DEVICE_DRIVER` and so does not exist in a new-stack build;
[next.conf](next.conf) explains why it needs no replacement.

One respect in which the impersonation stops being exact: DWC2 is a high-speed
controller, so an nRF54LM20A dongle enumerates at 480 Mbit/s with 512-byte bulk
endpoints where a retail stick is full speed with 64. The frames are identical
and libusb hides the packet size, but it is a visible difference.

#### Which stack is faster

[`tools/ant_bench.py`](tools/ant_bench.py) times `request capabilities` round
trips — the cheapest message with a guaranteed reply and no side effects, so
what it measures is the USB path and the bridge, not the radio. Four runs of
500 requests each, same Feather, same host, same session:

| | Legacy | New (USBD) |
|---|---|---|
| Latency mean | **0.381 ms** | 0.436 ms |
| Latency p50 | **0.361 ms** | 0.417 ms |
| Latency p90 | **0.449 ms** | 0.479 ms |
| Latency p99 | 0.796 ms | **0.787 ms** |
| Worst case seen | 2.137 ms | **1.274 ms** |
| Throughput, depth 8 | **3334 msg/s** | 2423 msg/s |
| Timeouts | 0 | 0 |
| Flash | **115 960 B** | 125 720 B |
| RAM | 42 880 B | **41 280 B** |

Run-to-run spread was under 2% on the legacy build and under 6% on the new one,
so the gaps in the central figures are real: the new stack is about 15% slower
per round trip and about 27% lower in sustained throughput, and costs ~9.8 KB
more flash while saving ~1.6 KB of RAM. Its only repeatable advantage is a
tighter worst case; p99 is a tie.

**None of this matters for the workload.** Eight ANT+ channels at 4 Hz is
32 messages a second, and the slower of the two stacks does 2400 — a margin of
75×. Both are latency-irrelevant against ANT's 250 ms message period. So the
numbers are not an argument for switching, and not much of an argument against
one either; they exist so the choice is not made on a guess.

What would eventually force the move is that the legacy stack is deprecated —
every build of it prints `Deprecated symbol USB_DEVICE_STACK is enabled` — and
one Zephyr release will remove it. Until then, shipping images stay on the
stack with hours on it against a real application, because being 27% faster at
something with 75× headroom is worth less than being known-good.

### Building from source

sdk-ant **v2.1.0** pairs with sdk-nrf **v3.2.4** — both its `west.yml` and
`doc/compatibility.rst` say so, and that is the pairing to build releases with.
Its prebuilt `libant.a` is compiled for that ABI, so mixing toolchains tends to
produce silently wrong radio behaviour rather than a clean link failure.

If you installed NCS the usual way (nRF Connect extension or
`nrfutil toolchain-manager install --ncs-version v3.2.4`) it is already a
complete west workspace, and sdk-ant is consumed as an extra module:

```powershell
. .\scripts\env.ps1 -NcsVersion v3.2.4
Push-Location C:\ncs\v3.2.4
west -z C:\ncs\v3.2.4\zephyr build -s C:\Users\Colin\ant_dongle `
  -d C:\Users\Colin\ant_dongle\build\release `
  -b adafruit_feather_nrf52840/nrf52840/uf2 -p always
Pop-Location
```

Then flash:

```powershell
.\scripts\flash_uf2.ps1 -TimeoutSeconds 30   # double-tap RESET when prompted
```

*Alternative*, if you would rather let sdk-ant's own manifest pull the SDK: its
`west.yml` declares `self: path: ant`, so the checkout must sit at
`<topdir>/ant`. Put the clone at `C:\ant-ws\ant`, then `west init -l
C:\ant-ws\ant; west update` — that fetches sdk-nrf v3.2.4 and Zephyr beneath
it. Build with `-DANT_MODULE_DIR=C:/ant-ws/ant`. This is what CI does.

**Don't build against NCS v3.4.0.** sdk-ant v2.1.0 fails outright there: its
`Kconfig` selects the library directory with
`default "nrf52" if SOC_SERIES_NRF52X`, but v3.4.0's Zephyr renamed that symbol
to `SOC_SERIES_NRF52`. `CONFIG_ANT_LIB_DIR` comes out empty and the link goes
looking for `lib/soft-float/libant.a` instead of `lib/nrf52/soft-float/libant.a`.

#### nRF52840 Dongle build

Same source, different board target, and a DFU package instead of a UF2:

```powershell
west -z C:\ncs\v3.2.4\zephyr build -s C:\Users\Colin\ant_dongle `
  -d C:\Users\Colin\ant_dongle\build\dongle `
  -b nrf52840dongle/nrf52840 -p always

.\scripts\package_dfu.ps1     # -> dist\ant_dongle_nrf52840dongle.zip
```

The package is unsigned, which is correct here: the dongle ships a
signature-less bootloader, and that is why it takes firmware with no debugger
and no cable. `package_dfu.ps1` refuses an image that does not start at
`0x1000` — a Feather build starts at `0x26000` and would otherwise package
happily into a zip that installs cleanly and then does nothing.

[`boards/nrf52840dongle_nrf52840.conf`](boards/nrf52840dongle_nrf52840.conf)
settles the two differences: no UF2 output, and `CONFIG_USE_DT_CODE_PARTITION`
left *off* so the offset comes out `0x1000` rather than the `slot0_partition`
the board's devicetree names for MCUboot, which we do not use.

#### Pro Micro nRF52840 build

The board's own `/uf2` variant already sets `CONFIG_BUILD_OUTPUT_UF2` and takes
its offset from `nrf52840_partition_uf2_sdv6.dtsi`, the same `0x26000` layout
the Feather uses — so it shares
[`pm_static_nrf52840_uf2_sdv6.yml`](pm_static_nrf52840_uf2_sdv6.yml) and needs
no map of its own. Build two images:

```powershell
west -z C:\ncs\v3.2.4\zephyr build -s C:\Users\Colin\ant_dongle `
  -d C:\Users\Colin\ant_dongle\build\promicro `
  -b promicro_nrf52840/nrf52840/uf2 -p always

west -z C:\ncs\v3.2.4\zephyr build -s C:\Users\Colin\ant_dongle `
  -d C:\Users\Colin\ant_dongle\build\promicro_synth `
  -b promicro_nrf52840/nrf52840/uf2 -p always -- "-DEXTRA_CONF_FILE=synth.conf"
```

The default uses the 32.768 kHz crystal. `synth.conf` derives that clock from
the 32 MHz crystal instead, for boards that do not have one — see the file for
what it costs and why it is not the default.

Two images rather than one because there is no way to detect this at build
time and no good way to fail at run time: without the crystal the LFXO simply
never starts, `ant_init()` never returns, and USB never comes up. The board
looks dead. Shipping both makes that a one-file retry instead of a diagnosis.

#### nRF54 builds

Two nRF54 targets exist. Neither is a product yet — cheap nRF54 dongles are not
on sale — but both build against the real `lib/nrf54l/libant.a`, which is
shipped in sdk-ant and does apply here: `SOC_NRF54LM20A` and `SOC_NRF54L15`
both select `SOC_SERIES_NRF54LX`, and that is what `ANT_LIB_DIR` keys on.

```powershell
west -z C:\ncs\v3.2.4\zephyr build -s C:\Users\Colin\ant_dongle `
  -d C:\Users\Colin\ant_dongle\build\l15 -b nrf54l15dk/nrf54l15/cpuapp -p always

west -z C:\ncs\v3.2.4\zephyr build -s C:\Users\Colin\ant_dongle `
  -d C:\Users\Colin\ant_dongle\build\lm20 -b nrf54lm20dk/nrf54lm20a/cpuapp -p always
```

**nRF54L15 has no USB device peripheral at all.** This is not a DK leaving a
connector unpopulated — there is no `usbd` node and no `usbhs` node in
`nrf54l_05_10_15.dtsi`, because there is nothing on the die. The L05 and L10 are
the same. So that target cannot be a dongle, and it is built with the UART
transport instead: `uart30` is the DK's second VCOM, so the ANT byte stream and
the log leave the board as two separate COM ports over the one debugger cable.
What it proves is the half the nRF52840 boards cannot — that ANT, MPSL and the
clock work on nRF54L silicon.

**nRF54LM20A is the one nRF54L part that does have USB**, as `usbhs@5a000`,
`compatible = "nordic,nrf-usbhs-nrf54l", "snps,dwc2"`, wired up by the DK as
`zephyr_udc0`. That target is a real dongle build and uses the new USB stack
because DWC2 leaves no alternative. Build-only so far — no hardware here.

Flashing either DK needs SEGGER J-Link installed. `nrfutil device list` will
find the board over its board-controller interface without it, and the `JLINK`
mass-storage volume will accept a copied hex and then reject it with
`FAIL.TXT: The currently active SWD interface does not support MSD drag and
drop`, which reads like a broken image rather than a missing tool.

#### Stub build

`stub.conf` compiles `src/ant_stub.c` in place of the radio and turns on the MS
OS descriptors, so the USB half builds and enumerates against whatever NCS you
have installed. Useful for working on the USB class, or for USB debugging on a
board sdk-ant does not target:

```powershell
. .\scripts\env.ps1 -NcsVersion v3.4.0
Push-Location C:\ncs\v3.4.0
west -z C:\ncs\v3.4.0\zephyr build -s C:\Users\Colin\ant_dongle `
  -d C:\Users\Colin\ant_dongle\build\stub `
  -b adafruit_feather_nrf52840/nrf52840/uf2 -p always -- "-DEXTRA_CONF_FILE=stub.conf"
Pop-Location
```

### Gotchas worth knowing

These are the non-obvious constraints the code is shaped around. Each one cost
real time to find, and several look like a generic "USB doesn't work" failure
from the outside.

- **`west build` needs its cwd inside the workspace.** It is an extension
  command discovered through the workspace manifest, so `-s`/`-d`/`-z` pointing
  elsewhere is not enough — `Push-Location` into the NCS topdir first.
- **Quote `-D` arguments.** PowerShell splits `-DEXTRA_CONF_FILE=stub.conf` at
  the dot and CMake receives two mangled arguments. Always
  `"-DEXTRA_CONF_FILE=stub.conf"`.
- **Don't use `west flash`.** The `uf2` runner fails with
  `ValueError: uf2 doesn't support --dev-id option`. Use `scripts/flash_uf2.ps1`.
- **PowerShell 5.1 reads `.ps1` as ANSI** unless the file has a BOM, so the
  scripts here are deliberately ASCII-only. A stray em-dash is a parse error.
- **Windows caches USB descriptor verdicts, permanently.** The result of its
  first MS OS descriptor query is stored under
  `HKLM\SYSTEM\CurrentControlSet\Control\usbflags\0fcf1009<bcdDevice>`, and per
  Microsoft's documentation a failed first query is *never retried* — so one bad
  build poisons the VID/PID and every later correct build looks equally broken.
  Between iterations either bump `CONFIG_ANT_DONGLE_BCD_DEVICE` (no elevation
  needed) or run `scripts/reset_usb_cache.ps1` from an elevated shell.
- **Endpoint descriptors must say `AUTO_EP_IN`/`AUTO_EP_OUT`, not `0x81`/`0x01`.**
  `usb_fix_descriptor()` pairs each endpoint descriptor with its
  `usb_ep_cfg_data` entry by matching `bEndpointAddress` against `ep_addr`, and
  only then rewrites both with the address it allocates. Writing the final
  ANTUSB-2 addresses straight into the descriptor matches nothing, so
  `usb_get_device_descriptor()` returns NULL and `usb_enable()` fails with -1
  before anything reaches the bus. Allocation starts at endpoint 1 in each
  direction, so `AUTO_EP_*` still yields 0x01 and 0x81.
- **Partition Manager decides the link address, not `zephyr,code-partition`.**
  sdk-ant is an NCS module, so sysbuild turns Partition Manager on, and left
  alone it hands the whole 1 MB to `app` at `0x0`. `CONFIG_FLASH_LOAD_OFFSET`
  stays at `0x26000` from the devicetree, and that is what the UF2 converter
  stamps into the file — so the image is linked for `0x0`, written to
  `0x26000`, and boots into whatever the vector table happens to point at. It
  never enumerates and looks exactly like a USB fault.
  [`pm_static_adafruit_feather_nrf52840.yml`](pm_static_adafruit_feather_nrf52840.yml)
  restates the board's layout in the form PM reads; `sysbuild.cmake` selects it
  by board. Check `build/<d>/partitions.yml` says `app: address: 0x26000`, or
  that `zephyr.hex` opens with an extended-address record rather than
  `:10000000`.
- **Reserving a region Partition Manager already knows about moves your app
  instead of protecting it.** Adding a new board means writing a static map, and
  the obvious first draft reserves the regions the bootloader owns. But PM may
  already define one of them — on the nRF52840 Dongle, `nrf5_mbr`
  (`nrf/subsys/partition_manager/pm.yml.nrf5_mbr`), added whenever
  `CONFIG_BOARD_HAS_NRF5_BOOTLOADER` is set and placed `{after: [start]}`. A
  static entry covering the same bytes wins the address, PM slides its own copy
  along to sit after it, and `app` gets pushed a page further than
  `CONFIG_FLASH_LOAD_OFFSET` says. There is no warning; a static partition being
  honoured is exactly what you asked for. `app` cannot be pinned against this,
  since it is the one that absorbs whatever space the others leave. The fix is
  to delete a partition, not add one. Check `build/<d>/partitions.yml` and
  `.config` agree on the app address.
- **The USB driver's work queue needs more than its 1 KB default.** The nrfx
  driver dispatches `SET_CONFIGURATION` on its own work queue, and the
  `ant_status_cb()` → `arm_rx()` → `usb_transfer()` chain runs there. At the
  default it overflows the moment the host configures the device; the fatal
  error halts everything at once, so the board goes dark on the bus, the LED
  stops and nothing more is logged. See the stack sizes in `prj.conf`.
- **`MESG_SYSTEM_RESET` resets the ANT stack, not the MCU.** Every host library
  opens the device, resets it, then keeps using the same handle. Rebooting
  makes that handle stale and the next transfer fails with a pipe error — at
  the exact point every session begins.
- **UF2 cannot chip-erase.** It only rewrites `0x26000`–`0xEC000`, so anything
  outside that window survives a reflash.
- **The frame checksum covers the `0xA4` SYNC byte, and a test suite cannot tell
  you otherwise.** The parser seeded `running_xor = 0` instead of the SYNC byte
  it had just consumed, so every checksum it computed was off by exactly `0xA4`
  and every frame a real host sent was dropped without a word. Nothing else
  looked wrong: the dongle enumerated, bound its driver, and Zwift listed it as
  present - it simply never executed a single command, so it never replied,
  Zwift timed out after five seconds and fell into its stop path, writing 70,000
  identical `Stopping ANT search` lines and showing no ANT+ sensors at all. It
  hid for as long as it did because `ant_probe.py` made the *same* mistake in
  `frame()`. Firmware and tools agreed perfectly with each other and with
  nothing else in the world, so the whole suite passed - probe, scan, all eight
  channels, real sensors, ack and burst. Every green test was two wrong
  implementations shaking hands. If you change the framing on one side, change
  it on the other in the same commit, and check the result against an
  implementation neither of you wrote: `ANT_DLL.dll` exports
  `ANT_SetDebugLogDirectory`, and the `Device0.txt` it writes gives you Garmin's
  own `Tx`/`Rx` bytes to XOR by hand.
- **A host gives up on one unanswered message, and says nothing useful about
  which.** Zwift calls `ANT_SetTransmitPower`, the device-wide `0x47`, while
  setting a search up. The bridge implemented only the per-channel `0x60` and
  answered `0x47` with `INVALID_MESSAGE`. That was masked by the checksum bug
  above - the message never reached `dispatch()` to be rejected - but it would
  have stalled the search on its own once framing was fixed. What the host
  actually calls is discoverable without guessing: `ANT_DLL.dll` is loaded by
  name, so the ANT functions Zwift resolves are plain strings in `ZwiftApp.exe`
  (`ANT_SetTransmitPower`, `ANT_OpenRxScanMode`, `ANT_EnableLED`, ...).
- **Capabilities are the stack's, coverage is the bridge's, and nothing checks
  they agree.** `ant_capabilities_get()` reports what the ANT stack can do,
  which is a superset of the serial messages `dispatch()` implements - the
  advertisement is what tells a host the feature is safe to use. Two bits
  already carry their weight: scan mode and LED are both reported off, which is
  why Zwift never sends `0x5B`/`0x68` even though it has the calls. Anything
  advertised *and* unimplemented is a trap of the kind above.

### Testing the radio

`ant_probe.py` only proves the dongle answers questions about itself. To
exercise the radio, `tools/ant_scan.py` runs the sequence a fitness app opens
with — ANT+ network key, wildcard slave channel, open, listen — and reports
what it hears:

```sh
python tools/ant_scan.py --seconds 30
```

But one channel is not what a fitness app does. Zwift opens a channel per
sensor it cares about and runs them simultaneously for the length of a ride, so
anything that only ever gets exercised on channel 0 stays untested.
`tools/ant_session.py` runs that session instead — all eight channels at their
real profile message rates — and additionally checks the two host-to-sensor
paths:

```sh
python tools/ant_session.py --seconds 30
```

- **Acknowledged data** is how Zwift sets trainer resistance. Sent at a paired
  channel, `TRANSFER_TX_COMPLETED` means a real sensor acknowledged it.
- **Burst** is probed at a closed channel on purpose. A dispatcher that does
  not implement the message answers `INVALID_MESSAGE`; one that does gets
  `CHANNEL_IN_WRONG_STATE` back from the stack. Both are errors, but only the
  second proves it reached the radio — and nothing goes on the air, which
  matters when the sensor in range is someone's trainer.

Two lessons for anyone writing another tool, both of which first showed up as
apparent firmware bugs: assigning a channel that a previous run left assigned
is refused with `CHANNEL_IN_WRONG_STATE`, which is why every host opens with a
system reset; and a close is asynchronous, so unassigning before
`EVENT_CHANNEL_CLOSED` arrives is refused for the same reason.

### Reading logs without a debugger

The Feather has no debugger, so `CONFIG_USE_SEGGER_RTT=y` logs are unreadable
on it. `diag.conf` swaps RTT for a log backend that buffers to RAM and
periodically commits to a reserved flash region. The Adafruit bootloader
exposes that region inside `CURRENT.UF2`, so the log can be read back over the
same USB drive used for flashing:

```powershell
west ... -d build\diagstub -- "-DEXTRA_CONF_FILE=stub.conf;diag.conf"
.\scripts\flash_uf2.ps1 -Uf2Path build\diagstub\ant_dongle\zephyr\zephyr.uf2
# let it run a few seconds, then double-tap RESET
.\scripts\read_flash_log.ps1
```

It also overrides `k_sys_fatal_error_handler`, so an early fault is captured
with its PC/LR rather than silently halting the CPU — from the outside a halt
is indistinguishable from a hang, since it kills USB, the LED heartbeat and the
periodic flush all at once.

Two things pin down `CONFIG_ANT_DONGLE_FLASH_LOG_OFFSET`: it must sit above the
image, and inside the window the bootloader actually dumps. Bootloader 0.8.0
dumps `0x1000`–`0xEA000`, which stops short of the `0xEC000` end of the code
partition — so a slot at `0xEB000` is written correctly and is simply invisible
in the readback. A second copy is written at `0x40000` in case another
bootloader version exposes a narrower window.

### Debugging on an nRF5340 DK instead

A fault inside `usb_enable()` is invisible on the Feather, and the flash log
cannot capture what never got scheduled. An nRF5340 DK has an onboard J-Link
*and* a separate "nRF USB" connector wired to the SoC's own device peripheral,
so the same class code can be enumerated by a real host while its own account
of events comes out the VCOM port. Both cables at once, no conflict. Two of the
gotchas above were found this way in minutes after days of guessing from the
outside.

```powershell
west -z C:\ncs\v3.2.4\zephyr build -s . -d build\dk5340 `
  -b nrf5340dk/nrf5340/cpuapp -- "-DEXTRA_CONF_FILE=stub.conf"
Copy-Item build\dk5340\merged.hex D:\     # the JLINK drive
```

Logs go to the VCOM COM port at 115200 (the *second* of the two the J-Link
exposes), not RTT — RTT would need SEGGER's `JLinkARM` DLL installed, whereas
the VCOM is just a COM port carried by the cable that already programs the
board. [`boards/nrf5340dk_nrf5340_cpuapp.conf`](boards/nrf5340dk_nrf5340_cpuapp.conf)
sets that up along with `CONFIG_LOG_MODE_IMMEDIATE=y`, so the last line before
a hang has already been emitted rather than sitting in a queue that is about to
be discarded.

Build the DK with `stub.conf`. sdk-ant on an nRF5340 is the dual-core `ANT_NP`
path, which is not what the Feather runs; the DK is here for the USB half.

An nRF54L15 DK cannot substitute: no chip in the nRF54L05/L10/L15 family has a
USB device peripheral at all.

When a build is worth bisecting, Zephyr's own
`samples/subsys/usb/legacy/cdc_acm` is the reference: it enumerates on this
board under NCS v3.2.4 as two COM ports, which separates "USB is broken on this
board" from "our class is broken".

### Release checklist

| Check | How |
|---|---|
| Builds clean | `west build` exits 0 for both stub and real-ANT configs, on both boards |
| Linked where it is written | `build/<d>/partitions.yml` and `.config` agree on the app address: `0x26000` Feather, `0x1000` dongle |
| UF2 loads | Copies to `FTHR840BOOT`; drive auto-ejects, board re-enumerates |
| DFU package loads | `scripts\package_dfu.ps1` exits 0; Programmer writes the zip and the dongle re-enumerates |
| Correct identity | `Get-PnpDevice` shows `USB\VID_0FCF&PID_1009`, and the compatible IDs contain no `MS_COMP_WINUSB` |
| Driver binds | After installing libusb-win32, `DEVPKEY_Device_Service` reads `libusb0` and problem code is 0 |
| Protocol handshake | `tools/ant_probe.py` — reset → startup → capabilities → version |
| Radio live | `tools/ant_scan.py` hears broadcasts from a real ANT+ sensor |
| Full session | `tools/ant_session.py` — eight channels at once, ack and burst paths reach the radio |
| Consumer app | Zwift or TrainerRoad detects the dongle and pairs a sensor |

### CI

[`.github/workflows/build.yml`](.github/workflows/build.yml) builds a matrix of
seven. The first four are attached to `v*` tag releases:

| Artifact | Board |
|---|---|
| `ant_dongle.uf2` | Adafruit Feather nRF52840 Express |
| `ant_dongle_nrf52840dongle.zip` | Nordic nRF52840 Dongle (DFU package) |
| `ant_dongle_promicro.uf2` | Pro Micro nRF52840, with a 32.768 kHz crystal |
| `ant_dongle_promicro_synth.uf2` | Pro Micro nRF52840, clock synthesized for boards without one |

The other three are built to keep them compiling and are downloadable from the
run, but are not released — none of them is an image to hand anyone:

| Artifact | Board | Why |
|---|---|---|
| `ant_dongle_feather_usbd.uf2` | Adafruit Feather nRF52840 Express | The new USB stack on hardware that can be tested against a host |
| `ant_dongle_nrf54l15dk.hex` | nRF54L15 DK | ANT on nRF54L silicon, over a UART — the part has no USB |
| `ant_dongle_nrf54lm20dk.hex` | nRF54LM20A DK | The first nRF54 target that can be a dongle |

Each entry asserts two things before packaging. First, that
`CONFIG_FLASH_LOAD_OFFSET` and Partition Manager's `app` address agree — that
disagreement is silent at build time and produces an image that installs
cleanly and then boots into nothing, which is not something to discover from a
release artifact. Second, that the transport that got compiled is the one that
entry expects, since the choice is defaulted from devicetree and a moved or
renamed USB node would otherwise fall through to a different one and still
build green.

sdk-ant is private, so the build needs one repository **secret**:
`SDK_ANT_CHECKOUT_TOKEN`, a classic PAT with `repo` scope from an account that
has been granted access to `ant-nrfconnect/sdk-ant`. Without it the job reports
a skip rather than failing red.

The automatic `GITHUB_TOKEN` **cannot** be used for this. It is an installation
token scoped to this repository alone and carries no access to any other repo,
so an account's own access has to be delegated explicitly via a PAT. The
alternatives — a GitHub App installation token, or a deploy key — both require
admin rights on `ant-nrfconnect/sdk-ant`, which adopters don't have. A
fine-grained PAT only works if that organisation has opted into them, so a
classic PAT is the reliable choice. If the organisation enforces SAML SSO, the
PAT also has to be authorised for it — otherwise the checkout fails as though
the repository does not exist.

To build against a fork of sdk-ant, set repository variable `SDK_ANT_REPO` to
`owner/name`; it defaults to `ant-nrfconnect/sdk-ant`.

No network-key secret is required — a dongle receives its ANT+ network key from
the host over `MESG_NETWORK_KEY_ID`.

---

## Licensing and identity

Two constraints to settle before distributing builds of this publicly:

1. **`CONFIG_ANT_EVALUATION_KEY` is a non-commercial development key.**
   sdk-ant's `init/Kconfig` is explicit that a commercial licence is required
   before shipping a product.
2. **`VID 0x0FCF` belongs to Garmin/Dynastream.** Presenting their vendor ID to
   third parties is a different question from using it privately.

`CONFIG_USB_DEVICE_SN` is not the serial a host sees, despite looking like it.
Zephyr's `usb_update_sn_string_descriptor()` replaces it at runtime with the
HWINFO device ID — the nRF52840's 8-byte FICR DEVICEID — so every dongle has
always had a per-unit serial. What the literal controls is how much of it
survives: Zephyr keeps the low `sizeof(SN)/2` bytes and then copies only
`strlen(SN)` characters of the hex. At the old 7-character `"ANT0001"` that
truncated a 16-character ID to `183A618`, and logged a length-mismatch warning
on every boot. It is now a 16-character placeholder, which is the width that
matches DEVICEID exactly.
