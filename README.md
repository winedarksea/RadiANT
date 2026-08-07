# ANT+ USB Dongle

Turns an **Adafruit Feather nRF52840 Express** into an ANT+ USB stick. Copy one
file onto the board and Zwift, TrainerRoad, Garmin Express, openant and anything
else that looks for an ANT stick will find it — no drivers, no Zadig, no
soldering.

The firmware enumerates as `VID 0x0FCF / PID 0x1008`, the identity of a
Dynastream ANTUSB2, and speaks the standard ANT serial protocol over a bulk
vendor interface.

---

## For users

### 1. Buy the right board

**[Adafruit Feather nRF52840 Express](https://www.adafruit.com/product/4062)
(product 4062, ~$25).**

> **Not the Feather Sense (4516).** The Sense has no 32.768 kHz crystal and
> falls back to an internal RC oscillator at ±500 ppm. ANT requires ±50 ppm —
> the Sense is 10× outside tolerance and will not keep a channel synchronised.

Also check your cable: plenty of USB-C cables are charge-only and the board will
never appear.

### 2. Flash it

1. Plug the Feather in.
2. **Double-tap the RESET button.** A USB drive named `FTHR840BOOT` appears.
3. Drag `ant_dongle.uf2` onto that drive.

The drive ejects itself and the board reboots as an ANT+ dongle. That's it.

### 3. Per-platform notes

| Platform | What you need to do |
|---|---|
| **Windows** | Nothing. Windows binds WinUSB automatically via the MS OS 2.0 descriptors in the firmware. It appears under *Universal Serial Bus devices* in Device Manager. |
| **macOS** | Nothing. It is a vendor-class device, so libusb claims it directly. |
| **Linux** | Install the udev rule so you don't need root: `sudo cp host/linux/99-ant-usb.rules /etc/udev/rules.d/ && sudo udevadm control --reload-rules && sudo udevadm trigger`, then replug. |
| **Android** | Needs USB host/OTG. Apps should include [`host/android/device_filter.xml`](host/android/device_filter.xml) so the dongle is recognised on attach. |

### 4. Check it works

```sh
pip install pyusb
python tools/ant_probe.py
```

Expected: `PASS: reset -> startup -> capabilities -> version`.

If the version string comes back starting with `STUB`, you have flashed a
Stage-1 stub build — it enumerates correctly but has no radio. Flash a release
image instead.

---

## For developers

### Layout

| Path | Purpose |
|---|---|
| `src/usb_ant_class.c` | Bulk vendor USB class; MS OS 1.0 + 2.0 descriptors for WinUSB auto-binding |
| `src/ant_serial_bridge.c` | ANT serial protocol (`0xA4` framing) ↔ sdk-ant API |
| `src/ant_stub.c` | No-op radio, compiled only when `CONFIG_ANT_DONGLE_RADIO_STUB=y` |
| `src/main.c` | Init ordering: `ant_init()` before `usb_enable()` so ANT/MPSL owns HFXO startup |
| `scripts/` | Windows helpers: environment, flashing, USB cache reset |
| `tools/ant_probe.py` | Cross-platform protocol smoke test |
| `host/` | Linux udev rule, Android device filter |

### SDK versions

sdk-ant **v2.1.0** pairs with sdk-nrf **v3.2.4** — both its `west.yml` and
`doc/compatibility.rst` say so, and that is the pairing to build releases with.
Its prebuilt `libant.a` is compiled for that ABI, so mixing toolchains tends to
produce silently wrong radio behaviour rather than a clean link failure.

The pairing is not merely advisory. Building sdk-ant v2.1.0 against NCS v3.4.0
fails outright: its `Kconfig` selects the library directory with
`default "nrf52" if SOC_SERIES_NRF52X`, but v3.4.0's Zephyr renamed that symbol
to `SOC_SERIES_NRF52`. `CONFIG_ANT_LIB_DIR` comes out empty and the link goes
looking for `lib/soft-float/libant.a` instead of `lib/nrf52/soft-float/libant.a`.
Use v3.2.4; the Stage-1 stub build is the way to work on the USB half in the
meantime.

### Two-stage bring-up

**Stage 1 — USB only.** Proves enumeration and WinUSB binding with the radio
stubbed out, so it builds against whatever NCS you already have installed:

```powershell
. .\scripts\env.ps1 -NcsVersion v3.4.0
Push-Location C:\ncs\v3.4.0
west -z C:\ncs\v3.4.0\zephyr build -s C:\Users\Colin\ant_dongle `
  -d C:\Users\Colin\ant_dongle\build\stage1 `
  -b adafruit_feather_nrf52840/nrf52840/uf2 -p always -- "-DEXTRA_CONF_FILE=stub.conf"
Pop-Location
```

**Stage 2 — real radio.** Drop the stub overlay and build against NCS v3.2.4.
If you installed it the usual way (nRF Connect extension or
`nrfutil toolchain-manager install --ncs-version v3.2.4`) it is already a
complete west workspace, and sdk-ant is consumed as an extra module exactly as
in Stage 1 — no relocation of the clone needed:

```powershell
. .\scripts\env.ps1 -NcsVersion v3.2.4
Push-Location C:\ncs\v3.2.4
west -z C:\ncs\v3.2.4\zephyr build -s C:\Users\Colin\ant_dongle `
  -d C:\Users\Colin\ant_dongle\build\stage2 `
  -b adafruit_feather_nrf52840/nrf52840/uf2 -p always
Pop-Location
```

*Alternative*, if you would rather let sdk-ant's own manifest pull the SDK:
its `west.yml` declares `self: path: ant`, so the checkout must sit at
`<topdir>/ant`. Move the clone to `C:\ant-ws\ant`, then `west init -l
C:\ant-ws\ant; west update` — that fetches sdk-nrf v3.2.4 and Zephyr beneath
it. Build with `-DANT_MODULE_DIR=C:/ant-ws/ant`. This is what CI does.

Then flash:

```powershell
.\scripts\flash_uf2.ps1 -TimeoutSeconds 30   # double-tap RESET when prompted
```

### Gotchas worth knowing

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
  `HKLM\SYSTEM\CurrentControlSet\Control\usbflags\0fcf1008<bcdDevice>`, and per
  Microsoft's documentation a failed first query is *never retried* — so one bad
  build poisons the VID/PID and every later correct build looks equally broken.
  Between iterations either bump `CONFIG_ANT_DONGLE_BCD_DEVICE` (no elevation
  needed) or run `scripts/reset_usb_cache.ps1` from an elevated shell.
- **No J-Link on the dev machine**, so `CONFIG_USE_SEGGER_RTT=y` logs are
  unreadable. Bring-up relies on `tools/ant_probe.py` and the `led0` heartbeat.
  UF2 also cannot chip-erase — it only rewrites `0x26000`–`0xEC000`.

### Verification checklist

| Check | How |
|---|---|
| Builds clean | `west build` exits 0 for both stub and real-ANT configs |
| UF2 loads | Copies to `FTHR840BOOT`; drive auto-ejects, board re-enumerates |
| Correct identity | `Get-PnpDevice` shows `USB\VID_0FCF&PID_1008` |
| WinUSB auto-binds | Device Manager → *Universal Serial Bus devices*, `winusb.sys`, no Zadig, no yellow bang |
| Protocol handshake | `tools/ant_probe.py` — reset → startup → capabilities → version |
| Radio live | openant receives broadcast data from a real ANT+ sensor |
| Consumer app | Zwift or TrainerRoad detects the dongle and pairs a sensor |

### CI

[`.github/workflows/build.yml`](.github/workflows/build.yml) produces
`ant_dongle.uf2` and attaches it to `v*` tag releases. sdk-ant is private, so it
needs repository variable `SDK_ANT_REPO_URL` and secret
`SDK_ANT_CHECKOUT_TOKEN`; without them the job reports a skip rather than
failing. No network-key secret is required — a dongle receives its ANT+ network
key from the host over `MESG_NETWORK_KEY_ID`.

---

## Before distributing this publicly

Two unresolved constraints, flagged rather than answered:

1. **`CONFIG_ANT_EVALUATION_KEY` is a non-commercial development key.**
   sdk-ant's `init/Kconfig` is explicit that a commercial licence is required
   before shipping a product.
2. **`VID 0x0FCF` belongs to Garmin/Dynastream.** Presenting their vendor ID to
   third parties is a different question from using it privately.

Also cosmetic but real: `CONFIG_USB_DEVICE_SN` is fixed at `"ANT0001"`, so two
dongles on one host share a serial number. A UICR-derived serial would fix it.
