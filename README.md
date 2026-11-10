# RadiANT: ANT+ Spec Compatible RF Protocol Library
The goal of this project is to implement an open source radio library that is compatible with the subset of the ANT+ spec that is commonly used (ie by Zwift-compatible smart trainer setups). It also aims to provide a small superset of new items such as improved encryption support.

The main advantages of an ANT+ compatible spec are: simplicity (compare against the 3000+ page bluetooth specs), low battery usage, minimal airtime usage in congested environements, reliable "one to many" broadcasts, and standardized protocols for common fitness devices. While fitness devices are the main focus, other uses include sensor networks, asset tracking, and beacons.

Note that the primary ANT+ patents have expired and Garmin has made public many of the ANT+ spec documents in the middle of 2025. Some parts are still closed (libant.a for example) but this is a clean room design (we don't even have access to that source code) supported by sniffing and modeling the unencrypted ANT+ packets from real devices. 

This library is in no way officially linked to ANT+, this is simply "ANT+ compatible".

**Building your own Zephyr firmware?** See [Use RadiANT in your firmware](radiant/README.md#use-radiant-in-your-firmware). The rest of this page is about the USB dongle.

# ANT+ Compatible USB Dongle

Turns an **Adafruit Feather nRF52840 Express** (and other Nordic boards) into an
ANT+ compatible USB stick. Copy one file onto the board and Zwift, TrainerRoad,
Garmin Express, openant — anything that looks for an ANT stick — will find it.
No soldering, no wiring.

The firmware enumerates as `VID 0x0FCF / PID 0x1009` and speaks the ANT serial protocol over a bulk
vendor interface. Eight channels, acknowledged data and burst all work, so a
fitness app can run heart rate, power, cadence and trainer control at once.

This is an independent implementation. It is ANT+ *compatible*, not ANT+
certified, and is not licensed by, endorsed by or affiliated with Garmin or
Dynastream. See [Licensing and identity](#licensing-and-identity).

> **Windows users:** this needs the same libusb-win32 driver a retail ANT stick
> needs, and Windows will not offer it on its own. Read
> [Windows drivers](#windows-drivers) before anything else.

### Why?
Before this project, the 'latest' ANT+ dongles on the market are from 2012. They use old, weak nRF24AP2 (circa 2010) or the nRF51422 (circa 2012) chips. Switching to the NRF52840 (which is widely available and cheap) can lead to a 10 db improvement in sensitivity. That 10 dB improvement in sensitivity means the new nRF52840 chip can pick up a signal that is *ten times weaker* than what the old 2012 chips could detect. Big difference. While ANT+ is mostly your computer listening, in ERG mode there is a transmit element, and the +8 dBm signal strength vs the older chips means more range (more than double) and reliability there too. The dongle transmits at its maximum by default, which is what apps like Zwift get, since they never set the power. An app that explicitly asks for a lower level, including the standard 0 dBm, gets what it asked for.

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
3. Drag `radiant_dongle.uf2` onto that drive.

The drive ejects itself and the board reboots as an ANT+ compatible dongle. That's the
whole install — the firmware lives on the board, and there is nothing to
configure.

Grab `radiant_dongle.uf2` from the
[latest release](https://github.com/winedarksea/RadiANT/releases/latest), or [build it yourself](#building-from-source).

### 3. Set up your platform

| Platform | What you need to do |
|---|---|
| **Windows** | Install a libusb-win32 driver — see [Windows drivers](#windows-drivers). Same requirement as a retail ANT stick. |
| **macOS** | Nothing for apps such as Zwift. It is a vendor-class device, so libusb claims it directly. To run this repository's Python tools, `brew install libusb` first. |
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
takes the Nordic DFU package `radiant_dongle_nrf52840dongle.zip`:

1. Insert the dongle and press the small side RESET button. The red LED pulses
   slowly: that is the bootloader waiting.
2. [nRF Connect for Desktop](https://www.nordicsemi.com/Products/Development-tools/nrf-connect-for-desktop)
   → **Programmer** → select the device → **Add file** → **Write**.

The **Pro Micro nRF52840** footprint — nice!nano and the unbranded "SuperMini"
clones, a few dollars on AliExpress — works too, and takes
`radiant_dongle_promicro.uf2` by the same double-tap-and-drag as the Feather. Its
bootloader uses the same `0x26000` layout, but its drive is named `NICENANO`
(some clones: `NRF52BOOT`) rather than `FTHR840BOOT`. Most of these boards have
no RESET button: bridge the `RST` pad to `GND` twice in quick succession. On
macOS or Linux, `scripts/flash_uf2.sh path/to/radiant_dongle_promicro.uf2`
finds any of these drives; on Windows pass
`scripts\flash_uf2.ps1 -Uf2Path path\to\radiant_dongle_promicro.uf2 -VolumeLabel NICENANO`.

Once a release image with this feature is on the board, later reflashes need no
shorting: `python tools/ant_probe.py --reboot-to-bootloader` reboots it into the
same drive over USB. Any software on the host that can open the device can send
that request, so a Pro Micro running these images can be reflashed by whatever
it is plugged into.

The catch is that these are not one board. What they have in common is a
schematic; what they leave off varies by seller, and the **32.768 kHz crystal
is the part most likely to be missing** — it is exactly the component a $2 board
saves money on. ANT needs that clock inside ±50 ppm.

If the board never enumerates at all — no ANT+ device, no COM port, nothing —
that is the missing crystal, not a bad cable. `ant_init()` gates `usb_enable()`
in [main.c](apps/dongle/src/main.c), so a low-frequency clock that never starts stops
the firmware before USB comes up. Flash `radiant_dongle_promicro_synth.uf2`
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

Why not WinUSB, Windows not offering the driver, no optional updates showing,
and a driver that's installed but still not working — all four traps, with the
exact log lines and registry keys, are in
[`archive/drivers/install-notes.md`](archive/drivers/install-notes.md).

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
| `apps/dongle/` | The dongle application: build config (`CMakeLists.txt`, `Kconfig`, `prj.conf`, conf fragments) and `src/main.c` |
| `apps/common/` | The firmware shared by all three dongle applications — the bridge, the transports, the USB class |
| `radiant/` | The clean-room ANT-compatible link layer, as a Zephyr module: `include/radiant/` is its public surface, `Kconfig` selects its HAL backend, and `tests/` runs under ztest in CI as a standalone application that consumes the module the way any third party would |
| `protocol/` | `ant_wire.yaml`, the single source of truth for protocol constants. `scripts/gen_ant_wire.py` generates `radiant/include/radiant/ant_wire.h`, `tools/ant_wire.py` and a marked region of the protocol doc from it |
| `tools/` | Host-side Python: probe, scan, session, bench, features, sim, verify, and the `test_*.py` that run in CI |
| `apps/sim/` | Standalone ANT+ sensor firmware for the nRF54L15 DK. Not a dongle build — it is the reference transmitter every bench measurement is made against |
| `boards/` | Per-board `.conf`/`.overlay`: output format, code partition, and prising the console off the USB device we need |
| `scripts/` | Windows helpers: environment, flashing, DFU packaging, USB cache reset, and `build_all.ps1` |
| `host/` | Linux udev rule, Android device filter |
| `docs/` | Everything this file links to. Start at [`docs/README.md`](docs/README.md) |
| `archive/` | Preserved artefacts, because they are perishable and the facts are what you need: driver provenance and our own `.inf`, spec pointers with hashes, the `ANT_DLL` export tables, golden wire captures, benchmark baselines, and the two bootloader `.uf2` readbacks in `archive/firmware/`. 10 MB budget, stated in [`docs/preservation.md`](docs/preservation.md) |
| `dist/` | Build outputs. Gitignored — rebuild rather than trusting what is sitting there |
| `.github/` | CI in two lanes — `workflows/ci.yml` on every PR, `workflows/release.yml` on `v*` tags — plus the composite `actions/` they share |
| `apps/dongle/CMakeLists.txt` | Module wiring and the `ANT_RADIO` backend choice — `core` (ships) \| `sdk_ant` (A/B reference only) \| `stub` |
| `apps/dongle/Kconfig` | `CONFIG_ANT_DONGLE_*` — transport choice, descriptors, optional features |
| `apps/dongle/prj.conf` | Base configuration. The USB work-queue stack sizes here are load-bearing; the header comment says why |
| `rgb.conf` + `rgb_feather.overlay` | The activity RGB indicator, applied together by the shipping Feather row. Either alone fails the build |
| `apps/common/ant_heartbeat_led.c` | The mono `led0` flash — 30 ms every 4 s, gap scaled by receive activity, and `CONFIG_ANT_DONGLE_HEARTBEAT_LED=n` to have no LED at all. Shared by all three dongle applications |
| `next.conf`, `stub.conf`, `synth.conf`, `diag.conf`, `encryption.conf` | Extra conf fragments: new USB stack, no radio, synthesized 32.768 kHz clock, flash-backed logging, encryption writes. Each explains itself at the top of the file |
| `sysbuild.cmake`, `pm_static_*.yml` | Pin the application where each board's bootloader expects it — `0x26000` on the Feather and Pro Micro, `0x1000` on the dongle. See [the gotchas](docs/gotchas.md) |
| `sample.yaml` | Twister metadata |
| `apps/dongle/src/main.c` | Init ordering: `ant_init()` before the transport comes up, so ANT/MPSL owns HFXO startup |
| `apps/common/ant_transport.h` | The three-function contract the bridge talks to. One of the three transports below implements it |
| `apps/common/usb_ant_class.c` | Bulk vendor USB class on the **legacy** stack, plus the optional MS OS 1.0/2.0 descriptors. The proven one |
| `apps/common/usb_ant_class_next.c` | The same class on the **new** USBD/UDC stack. Required on nRF54; see [Transports](docs/backends.md#transports) |
| `apps/common/ant_uart_transport.c` | ANT serial over a plain UART, for parts with no USB peripheral |
| `apps/common/ant_serial_bridge.c` | ANT serial protocol (`0xA4` framing) ↔ the radio backend |
| `apps/common/ant_radio_stub.c` | No-op radio, compiled only when the stub backend is selected |
| `apps/common/diag_flash_log.c` | Log backend that commits to flash, readable back over UF2 |
| `radiant/include/radiant/ant_radio.h`, `radiant/include/radiant/ant_wire.h` | Our ~50-function radio contract and our protocol constants — the seam that lets the bridge speak to any backend. See [Backends](docs/backends.md) |

### Building from source

**Nothing private is needed.** The shipping radio is `radiant`, the clean-room
stack in this repository, so a fresh clone builds every released image with no
token, no secret and no access to anything. Build against NCS **v3.4.0**, then
flash:

```powershell
. .\scripts\env.ps1 -NcsVersion v3.4.0
.\scripts\build_all.ps1                      # -Backend core default; writes dist\
.\scripts\flash_uf2.ps1 -TimeoutSeconds 30   # double-tap RESET when prompted
```

The shipping Feather image carries the activity-rate RGB indicator: its build
row applies `rgb.conf` and `rgb_feather.overlay` together, and either one
without the other fails the build on purpose.

#### The LED

Every board with a `led0` alias flashes it for **30 ms every 4 seconds** — a
liveness heartbeat rather than a lamp, and dark 99 % of the time. By default the
gap then *shortens* with how much sensor traffic the dongle is hearing: about
4 s idle, 1.6 s with one sensor, 400 ms with four, down to a 150 ms floor. So
one LED answers both "did it boot" and "is it hearing anything".

It is one `gpio_pin_set_dt()` from the main thread twice per cycle, with no
`irq_lock()`, no DMA and nothing in interrupt context, so it costs the radio
nothing — and at a quarter of a hertz it is a *quarter* the wakeups of the 1 Hz
blink it replaced.

| To get | Set |
|---|---|
| No LED activity at all | `CONFIG_ANT_DONGLE_HEARTBEAT_LED=n` — the file is not compiled, no pin is ever driven |
| A flat blink, no activity scaling | `CONFIG_ANT_DONGLE_HEARTBEAT_LED_ACTIVITY=n` |
| A different pattern | `CONFIG_ANT_DONGLE_HEARTBEAT_LED_ON_MS` / `_OFF_MS` (`_OFF_MS` is also the idle gap the activity curve is anchored to) |

On the Feather the NeoPixel is the richer indicator and takes the activity
signal, so there the mono LED stays on its flat flash — `CONFIG_ANT_DONGLE_
HEARTBEAT_LED_ACTIVITY` depends on the RGB indicator being off, because the two
drain one counter and both would read a busy room as quiet.

<details>
<summary><b>Comparison builds against sdk-ant</b> — not needed to build or ship</summary>

Nordic's prebuilt `libant.a` is the A/B reference every gate in
`tools/ab_gates.toml` is phrased against, and the only build whose
`BUILD_ASSERT`s check our protocol constants against Garmin's. It is **not**
built by CI and is in no released image — see
[ADR 0001](docs/decisions/0001-backend-selection-and-release-default.md).

```powershell
.\scripts\build_all.ps1 -Backend sdk_ant -NcsVersion v3.2.4   # writes nothing to dist\
```

The **v3.2.4** pin is a hard constraint: sdk-ant v2.1.0's `Kconfig` selects its
library directory with `default "nrf52" if SOC_SERIES_NRF52X`, and v3.4.0's
Zephyr renamed that symbol, so `CONFIG_ANT_LIB_DIR` comes out empty and the link
looks for `lib/soft-float/libant.a`. Point the build at your checkout with
`SDK_ANT_DIR`, a sibling clone at `../sdk-ant`, or `-DANT_MODULE_DIR=<path>` —
each naming the directory holding `zephyr/module.yml`. `-DANT_RADIO=sdk_ant` is
required explicitly: the build no longer picks this backend just because a
checkout is on disk, which made the radio a property of the machine.

</details>

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

**Cutting the `v*` tag is what runs the firmware matrix and publishes the
artifacts** — nothing on `master` builds them. Dispatch
[`release.yml`](.github/workflows/release.yml) by hand and confirm it is green
*before* tagging.

Everything up to "UF2 loads" is done in one step by
[`scripts/build_all.ps1`](scripts/build_all.ps1), which walks the same matrix
as CI, makes the same two assertions after each build, and leaves the artifacts
in `dist\`:

```powershell
. .\scripts\env.ps1 -NcsVersion v3.4.0
.\scripts\build_all.ps1
```

v3.4.0, not v3.2.4: that is what `west.yml` pins, what `build_all.ps1`
defaults to, and what the TI rows need (`hal_ti` is absent under v3.2.4).
v3.2.4 is only for the `-Backend sdk_ant` comparison build above.

`dist\` is gitignored — it is a build output, not a tracked one. Rebuild it
rather than trusting whatever is sitting there, since nothing warns when an
artifact is older than the source it was built from.

| Check | How |
|---|---|
| Builds clean | `scripts\build_all.ps1` exits 0 — every dongle target, all three transports |
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

**Two lanes.** PRs and pushes to `master` run the cheap correctness gate; the
firmware matrix and every published artifact run on `v*` tags and manual
dispatch only — so **a board-specific build break is invisible until a tag or a
manual run.** That is the trade taken.

| Lane | File | Runs on | Jobs |
|---|---|---|---|
| Fast | [`ci.yml`](.github/workflows/ci.yml) | PR, push to `master`, dispatch | `host-tests`, `ztest`, `generated-drift`, `readme-cap`, `import-smoke`, `upstream-zephyr` |
| Release | [`release.yml`](.github/workflows/release.yml) | `v*` tags, dispatch | `build-core` (publishes), `build-new-apps`, `build-node`, `build-treadmill`, `zero-cost` |
| Links | [`linkcheck.yml`](.github/workflows/linkcheck.yml) | weekly, dispatch | external URLs, `archive/` budget |

**No fast-lane job needs a secret**, so it runs in full on a fork — that is what
`ztest` exists for, and it is the only C verification that runs on every change
(`native_sim` needs Linux; [`docs/testing.md`](docs/testing.md) covers running it yourself).

The release lane attaches four artifacts to a `v*` release:

| Artifact | Board |
|---|---|
| `radiant_dongle.uf2` | Adafruit Feather nRF52840 Express |
| `radiant_dongle_nrf52840dongle.zip` | Nordic nRF52840 Dongle (DFU package) |
| `radiant_dongle_promicro.uf2` | Pro Micro nRF52840, with a 32.768 kHz crystal |
| `radiant_dongle_promicro_synth.uf2` | Pro Micro nRF52840, clock synthesized for boards without one |

Three more are built by CI to keep them compiling but not uploaded;
`scripts\build_all.ps1` writes them to `dist\` locally:
`radiant_dongle_feather_usbd.uf2` (new USB stack), `radiant_dongle_nrf54l15dk.hex`
(nRF54L, UART — the part has no USB) and `radiant_dongle_nrf54lm20dk.hex`. Each
entry asserts, before packaging, that `CONFIG_FLASH_LOAD_OFFSET` and Partition
Manager's `app` address agree — disagreement is silent at build time and
produces an image that installs cleanly and then boots into nothing — and that
the transport compiled is the one that entry expects.

Those read-back assertions live in composite actions under
[`.github/actions/`](.github/actions/) rather than copy-pasted into each job:
they were copy-pasted, and a fix applied to one copy left five other assertions
unsatisfiable for months. `assert-kconfig` now rejects that shape by name.
**No job in either lane needs a secret**, including the one that publishes.
That became true when release artifacts moved to the clean-room backend and the
token-gated `build-sdk-ant` job was deleted — so a fork now gets the whole
release matrix, artifacts included, rather than a skipped-build notice.

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
| [RadiANT security](docs/radiant-security.md) | Threat model, the two independent payload switches and the identity rule, and the honest limits |
| [Preservation](docs/preservation.md) | What `archive/` holds, and why each item is or is not redistributable |
| [Third-party dependencies](docs/third-party.md) | What this project uses that it does not own, and the licence position of each |
| [Decisions](docs/decisions/) | The ADRs: release default, clean-room policy, naming and identity, licence, extension placement |

The full index, and the four rules that keep those files from rotting, are in
[`docs/README.md`](docs/README.md).

---

## Licensing and identity

RadiANT is an open-source, clean-room implementation compatible with the
published ANT+ specifications. It is not an ANT+ certified product, and is not
licensed by, endorsed by or affiliated with Garmin or Dynastream. "ANT" and
"ANT+" are trademarks of Garmin, used here only to describe compatibility.
