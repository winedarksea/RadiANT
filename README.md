# RadiANT: ANT+ Spec Compatible RF Protocol Library
The goal of this project is to implement an open source radio library that is compatible with the subset of the ANT+ spec that is commonly used (ie by Zwift-compatible smart trainer setups). It also aims to provide a small superset of new items such as improved encryption support.

The main advantages of an ANT+ compatible spec are: simplicity (compare against the 3000+ page bluetooth specs), low battery usage, minimal airtime usage in congested environements, reliable "one to many" broadcasts, and standardized protocols for common fitness devices. While fitness devices are the main focus, other uses include sensor networks, asset tracking, and beacons.

Note that the primary ANT+ patents have expired and Garmin has made public many of the ANT+ spec documents in the middle of 2025. Some parts are still closed (libant.a for example) but this is a clean room design (we don't even have access to that source code) supported by sniffing and modeling the unencrypted ANT+ packets from real devices. 

This library is in no way officially linked to ANT+, this is simply "ANT+ compatible".

# ANT+ Compatible USB Dongle

Turns an **Adafruit Feather nRF52840 Express** (and other Nordic boards) into an
ANT+ compatible USB stick. Copy one file onto the board and Zwift, TrainerRoad,
Garmin Express, openant — anything that looks for an ANT stick — will find it.
No soldering, no wiring.

The firmware enumerates as `VID 0x0FCF / PID 0x1009`, the numeric identity of a
Dynastream ANT USB-m, and speaks the ANT serial protocol over a bulk
vendor interface. Eight channels, acknowledged data and burst all work, so a
fitness app can run heart rate, power, cadence and trainer control at once.

This is an independent implementation. It is ANT+ *compatible*, not ANT+
certified, and is not licensed by, endorsed by or affiliated with Garmin or
Dynastream. See [Licensing and identity](#licensing-and-identity).

> **Windows users:** this needs the same libusb-win32 driver a retail ANT stick
> needs, and Windows will not offer it on its own. Read
> [Windows drivers](#windows-drivers) before anything else.

### Why?
Before this project, the 'latest' ANT+ dongles on the market are from 2012. They use old, weak nRF24AP2 (circa 2010) or the nRF51422 (circa 2012) chips. Switching to the NRF52840 (which is widely available and cheap) can lead to a 10 db improvement in sensitivity. That 10 dB improvement in sensitivity means the new nRF52840 chip can pick up a signal that is *ten times weaker* than what the old 2012 chips could detect. Big difference. While ANT+ is mostly your computer listening, in ERG mode there is a transmit element, and the +8 dBm signal strength vs the older chips means more range (more than double) and reliability there too.

This uses Zephyr as the base. Zephyr provides a modern, Linux Foundation-backed RTOS. That is heavily vetted, actively maintained USB stacks, superior power management, advanced radio libraries, and true concurrency. Combined with the newer chips that have EasyDMA direct memory access and built-in USB peripherals, this can lead to up to a 90% reduction in latency and packet loss.

In addition to these features, the tiny old ANT+ USB dongles have terrible antennas, sacrificing ground plane and antenna quality for tiny size. That's not a worthwhile tradeoff for most fixed indoor ride setups. Even a switch to the not-that-much-bigger official NRF52840 USB dongle can yield a major boost in antenna quality with its MIFA PCB antenna.

The result is a rock-solid, latency-free connection for your longest endurance rides.

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

The drive ejects itself and the board reboots as an ANT+ compatible dongle. That's the
whole install — the firmware lives on the board, and there is nothing to
configure.

Grab `ant_dongle.uf2` from the
[latest release](https://github.com/winedarksea/RadiANT/releases/latest), or [build it yourself](#building-from-source).

### 3. Set up your platform

| Platform | What you need to do |
|---|---|
| **Windows** | Install a libusb-win32 driver — see [Windows drivers](#windows-drivers). Same requirement as a retail ANT stick. |
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
[Testing the radio](docs/testing.md).

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
with no driver, showing problem code 28 — exactly like a retail stick.

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

One row per top-level directory and per file worth knowing about. The tools in
`tools/` get a table of their own in [Testing](docs/testing.md); the per-board
files in `boards/` are one `.conf` and one `.overlay` each.

| Path | Purpose |
|---|---|
| `src/` | The dongle firmware — the bridge, the transports, the USB class |
| `radiant/` | The clean-room ANT-compatible link layer, as a Zephyr module: `include/radiant/` is its public surface, `Kconfig` selects its HAL backend, and `tests/` runs under ztest in CI as a standalone application that consumes the module the way any third party would |
| `protocol/` | `ant_wire.yaml`, the single source of truth for protocol constants. `scripts/gen_ant_wire.py` generates `src/ant_wire.h`, `tools/ant_wire.py` and a marked region of the protocol doc from it |
| `tools/` | Host-side Python: probe, scan, session, bench, features, sim, verify, and the `test_*.py` that run in CI |
| `sim/` | Standalone ANT+ sensor firmware for the nRF54L15 DK. Not a dongle build — it is the reference transmitter every bench measurement is made against |
| `boards/` | Per-board `.conf`/`.overlay`: output format, code partition, and prising the console off the USB device we need |
| `scripts/` | Windows helpers: environment, flashing, DFU packaging, USB cache reset, and `build_all.ps1` |
| `host/` | Linux udev rule, Android device filter |
| `docs/` | Everything this file links to. Start at [`docs/README.md`](docs/README.md) |
| `archive/` | Preserved artefacts, because they are perishable and the facts are what you need: driver provenance and our own `.inf`, spec pointers with hashes, the `ANT_DLL` export tables, golden wire captures, benchmark baselines, and the two bootloader `.uf2` readbacks in `archive/firmware/`. 10 MB budget, stated in [`docs/preservation.md`](docs/preservation.md) |
| `dist/` | Build outputs. Gitignored — rebuild rather than trusting what is sitting there |
| `.github/workflows/` | CI: the build matrix, the host tests, and the weekly link check |
| `CMakeLists.txt` | Module wiring and the `ANT_RADIO` backend choice (`sdk_ant` \| `core` \| `stub`) |
| `Kconfig` | `CONFIG_ANT_DONGLE_*` — transport choice, descriptors, optional features |
| `prj.conf` | Base configuration. The USB work-queue stack sizes here are load-bearing; the header comment says why |
| `next.conf`, `stub.conf`, `synth.conf`, `diag.conf`, `encryption.conf` | Extra conf fragments: new USB stack, no radio, synthesized 32.768 kHz clock, flash-backed logging, encryption writes. Each explains itself at the top of the file |
| `sysbuild.cmake`, `pm_static_*.yml` | Pin the application where each board's bootloader expects it — `0x26000` on the Feather and Pro Micro, `0x1000` on the dongle. See [the gotchas](docs/gotchas.md) |
| `sample.yaml` | Twister metadata |
| `src/main.c` | Init ordering: `ant_init()` before the transport comes up, so ANT/MPSL owns HFXO startup |
| `src/ant_transport.h` | The three-function contract the bridge talks to. One of the three transports below implements it |
| `src/usb_ant_class.c` | Bulk vendor USB class on the **legacy** stack, plus the optional MS OS 1.0/2.0 descriptors. The proven one |
| `src/usb_ant_class_next.c` | The same class on the **new** USBD/UDC stack. Required on nRF54; see [Transports](docs/backends.md#transports) |
| `src/ant_uart_transport.c` | ANT serial over a plain UART, for parts with no USB peripheral |
| `src/ant_serial_bridge.c` | ANT serial protocol (`0xA4` framing) ↔ the radio backend |
| `src/ant_radio_stub.c` | No-op radio, compiled only when the stub backend is selected |
| `src/diag_flash_log.c` | Log backend that commits to flash, readable back over UF2 |
| `src/ant_radio.h`, `src/ant_wire.h` | Our ~50-function radio contract and our protocol constants — the seam that lets the bridge speak to any backend. See [Backends](docs/backends.md) |

### Building from source

sdk-ant **v2.1.0** pairs with sdk-nrf **v3.2.4** — both its `west.yml` and
`doc/compatibility.rst` say so, and that is the pairing to build releases with.
Its prebuilt `libant.a` is compiled for that ABI, so mixing toolchains tends to
produce silently wrong radio behaviour rather than a clean link failure.

**Tell the build where sdk-ant is.** There is no hardcoded path: set
`SDK_ANT_DIR` once in your shell profile and every build in the tree picks it
up (including `sim/`), or clone sdk-ant as a **sibling of this repo** — the
default is `../sdk-ant` — or pass `-DANT_MODULE_DIR=<path>` for a single build.
Each must point at the directory holding `zephyr/module.yml`.

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

Every other target differs only in the board argument and an extra conf file.
The commands, and the board-specific reasoning worth more than the commands,
are in [Build targets](docs/backends.md#build-targets):

| Target | Board argument | Extra conf |
|---|---|---|
| Feather, Pro Micro | `adafruit_feather_nrf52840/nrf52840/uf2`, `promicro_nrf52840/nrf52840/uf2` | — |
| Pro Micro with no 32.768 kHz crystal | `promicro_nrf52840/nrf52840/uf2` | `synth.conf` |
| nRF52840 Dongle (DFU package) | `nrf52840dongle/nrf52840` | then `scripts\package_dfu.ps1` |
| New USB stack, on a Feather | `adafruit_feather_nrf52840/nrf52840/uf2` | `next.conf` |
| nRF54L15 DK — UART transport, no USB in silicon | `nrf54l15dk/nrf54l15/cpuapp` | — |
| nRF54LM20A DK | `nrf54lm20dk/nrf54lm20a/cpuapp` | — |
| No radio, USB half only | any | `stub.conf` |
| Flash-backed logging with no debugger | any | `diag.conf` |
| Encryption writes compiled in | any | `encryption.conf` |

### Release checklist

Everything up to "UF2 loads" is done in one step by
[`scripts/build_all.ps1`](scripts/build_all.ps1), which walks the same matrix
as CI, makes the same two assertions after each build, and leaves the artifacts
in `dist\`:

```powershell
. .\scripts\env.ps1 -NcsVersion v3.2.4
.\scripts\build_all.ps1
```

`dist\` is gitignored — it is a build output, not a tracked one. Rebuild it
rather than trusting whatever is sitting there, since nothing warns when an
artifact is older than the source it was built from.

| Check | How |
|---|---|
| Builds clean | `scripts\build_all.ps1` exits 0 — seven targets, all three transports |
| Linked where it is written | Asserted per target by `build_all.ps1`: `0x26000` Feather and Pro Micro, `0x1000` dongle, `0x0` nRF54 DKs |
| Right transport compiled | Asserted per target by `build_all.ps1`; the choice is defaulted from devicetree, so it can drift silently |
| UF2 loads | Copies to `FTHR840BOOT`; drive auto-ejects, board re-enumerates |
| DFU package loads | `scripts\package_dfu.ps1` exits 0; Programmer writes the zip and the dongle re-enumerates |
| Correct identity | `Get-PnpDevice` shows `USB\VID_0FCF&PID_1009`, and the compatible IDs contain no `MS_COMP_WINUSB` |
| Driver binds | After installing libusb-win32, `DEVPKEY_Device_Service` reads `libusb0` and problem code is 0 |
| Protocol handshake | `tools/ant_probe.py` — reset → startup → capabilities → version |
| Radio live | `tools/ant_scan.py` hears broadcasts from a real ANT+ sensor |
| Full session | `tools/ant_session.py` — eight channels at once, ack and burst paths reach the radio |
| Consumer app | Zwift or TrainerRoad detects the dongle and pairs a sensor |

### CI

[`.github/workflows/build.yml`](.github/workflows/build.yml) runs the host
tests — `tools/test_*.py` on `ubuntu-24.04`, the only job that runs on a fork —
and a build matrix of seven. The first four are attached to `v*` tag releases:

| Artifact | Board |
|---|---|
| `ant_dongle.uf2` | Adafruit Feather nRF52840 Express |
| `ant_dongle_nrf52840dongle.zip` | Nordic nRF52840 Dongle (DFU package) |
| `ant_dongle_promicro.uf2` | Pro Micro nRF52840, with a 32.768 kHz crystal |
| `ant_dongle_promicro_synth.uf2` | Pro Micro nRF52840, clock synthesized for boards without one |

The other three are built to keep them compiling and are downloadable from the
run, but not released — none of them is an image to hand anyone:
`ant_dongle_feather_usbd.uf2` (the new USB stack, on hardware that can be
tested against a real host), `ant_dongle_nrf54l15dk.hex` (ANT on nRF54L
silicon, over a UART — the part has no USB) and `ant_dongle_nrf54lm20dk.hex`
(the first nRF54 target that can be a dongle).

Each entry asserts two things before packaging. First, that
`CONFIG_FLASH_LOAD_OFFSET` and Partition Manager's `app` address agree — that
disagreement is silent at build time and produces an image that installs
cleanly and then boots into nothing. Second, that the transport that got
compiled is the one that entry expects, since the choice is defaulted from
devicetree and a moved or renamed USB node would otherwise fall through to a
different one and still build green.

sdk-ant is private, so `build-sdk-ant` needs one repository secret,
`SDK_ANT_CHECKOUT_TOKEN`, and skips rather than failing red without it.
`host-tests`, `ztest` and `build-core` need no secret at all, which is what
makes the build green on a fork. Why it has to be a classic PAT, and why every
cheaper option fails, is in [`docs/testing.md`](docs/testing.md#host-tests-in-ci).

---

## Documentation

The reasoning, the measurements and the decisions live in [`docs/`](docs/).
This file links; `docs/` owns; CI keeps this file under 450 lines so it stays
that way.

| Document | What it covers |
|---|---|
| [Gotchas](docs/gotchas.md) | The non-obvious constraints the code is shaped around. Read it before debugging anything |
| [Testing](docs/testing.md) | Every tool, the four verification tiers, and how to read a bench result without measuring the instrument |
| [Backends, transports and build targets](docs/backends.md) | The radio seam, the USB/UART transports, the per-board builds, the optional ANT features |
| [sdk-ant contract](docs/sdk-ant-contract.md) | The ~50 functions the bridge calls — the specification the rebuild must satisfy |
| [ANT serial protocol](docs/ant-serial-protocol.md) | Framing, the SYNC-in-checksum rule, the message tables, the capabilities reply decoded |
| [ANT radio link](docs/ant-radio-link.md) | The clean-room on-air reference, every fact carrying its provenance |
| [Device profiles](docs/device-profiles.md) | Every profile in one place: ANT+ page layouts, accumulators, common pages and rates, plus the RadiANT profile device types and `0x60` schema recipes |
| [Profile registry](docs/profile-registry.md) | Device types and pages claimed, and how a third party claims one |
| [RadiANT telemetry](docs/radiant-telemetry.md) | The generic telemetry envelope and its MQTT mapping |
| [RadiANT security](docs/radiant-security.md) | Threat model, the three independent switches, and the honest limits |
| [Preservation](docs/preservation.md) | What `archive/` holds, and why each item is or is not redistributable |
| [Third-party dependencies](docs/third-party.md) | What this project uses that it does not own, and the licence position of each |
| [Decisions](docs/decisions/) | The ADRs: release default, clean-room policy, naming and identity, licence, extension placement |

The full index, and the four rules that keep those files from rotting, are in
[`docs/README.md`](docs/README.md).

---

## Licensing and identity

Two constraints to settle before distributing builds of this publicly, both
recorded in [`docs/decisions/`](docs/decisions/):

1. **`CONFIG_ANT_EVALUATION_KEY` is a non-commercial development key.**
   sdk-ant's `init/Kconfig` is explicit that a commercial licence is required
   before shipping a product. Removing `libant.a` also removes the $0.08/unit
   royalty that comes with it.
2. **`VID 0x0FCF` belongs to Garmin/Dynastream.** Presenting their vendor ID to
   third parties is a different question from using it privately — and it is
   exactly why Windows driver matching works at all, so it is an open risk
   rather than a solved one. The descriptor *strings* are the project's own:
   every host matches on the numbers, so the strings never had to be theirs.

RadiANT is an open-source, clean-room implementation compatible with the
published ANT+ specifications. It is not an ANT+ certified product, and is not
licensed by, endorsed by or affiliated with Garmin or Dynastream. "ANT" and
"ANT+" are trademarks of Garmin, used here only to describe compatibility.
