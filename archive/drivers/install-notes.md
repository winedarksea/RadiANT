# Windows driver install notes

Checked by: nothing — treat as narrative. Every command below was run on
Windows 11 during bring-up, but nothing re-runs them. The one thing that *is*
machine-checked is the release checklist item in `README.md` asserting
`DEVPKEY_Device_Service` reads `libusb0` and the compatible IDs contain no
`MS_COMP_WINUSB`.

Two facts here cost days. Neither is discoverable from a symptom, because both
produce the same symptom as a dozen ordinary mistakes: *the dongle enumerates
and the app cannot see it.*

1. **The driver must be libusb-win32.** Not WinUSB. Not libusbK.
2. **Windows caches its verdict about a device and never re-asks.** A single
   bad firmware build poisons the VID/PID/`bcdDevice` triple, and every later,
   correct build then looks equally broken.

---

## 1. libusb-win32, and nothing else

### Why

Every ANT application on Windows reaches the stick through `ANT_DLL.dll`.
`ANT_DLL.dll` reaches USB through exactly three DLLs — `libusb0.dll`,
`DSI_SiUSBXp_3_1.DLL` and `DSI_CP210xManufacturing_3_1.dll` — the last two
being Silicon Labs serial paths for the older UART-based sticks. **There is no
WinUSB path in it at all.** A device bound to `winusb.sys` is therefore
invisible to Zwift, TrainerRoad and Garmin Express no matter how correct its
ANT protocol behaviour is. The failure has no error message anywhere; the app
simply lists no ANT devices.

It is worse than merely not working. Advertising a WinUSB compatible ID makes
Windows match `USB\MS_COMP_WINUSB`, decide the device is driven, and **stop
looking for anything better** — including the correct driver sitting on Windows
Update. So the wrong choice is self-concealing.

That is why the release build ships **no MS OS descriptors**
(`CONFIG_ANT_DONGLE_MSOS_DESCRIPTORS=n`, and `CONFIG_USB_DEVICE_BOS=n` with it,
which also drops `bcdUSB` from `0x0210` back to a plain `0x0200`). The device
arrives with no driver and problem code 28, exactly like a genuine retail
stick, and Windows then looks up `USB\VID_0FCF&PID_1009` and offers the real
driver. Behaving like the stick being impersonated is the *point*, not a
shortcoming — see the `ANT_DONGLE_MSOS_DESCRIPTORS` help text in
[`Kconfig`](../../Kconfig).

Turn it on only for the opposite case: bring-up on a machine with no ANT driver
at all, where you want `pyusb` and `tools/` to work with nothing installed.
`stub.conf` does exactly that. Zwift will not see that build.

### libusbK is not the same choice

Zadig's *Options > List All Devices* menu offers four drivers. Two of them look
plausible and are not:

| Choice | Binds | Verdict |
|---|---|---|
| **libusb-win32** | `libusb0.sys` + `libusb0.dll` | **This one.** It is the API `ANT_DLL.dll` calls, by name |
| libusbK | `libusbK.sys` | No. Different kernel driver and different user-mode library. libusbK does ship a libusb0-compatible shim, but that is not what Garmin's package installs and it is not what has ever been verified against `ANT_DLL.dll` here. Do not gamble a debugging session on it |
| WinUSB | `winusb.sys` | No, for the reasons above |
| USB Serial (CDC) | `usbser.sys` | No. Wrong device class entirely |

### Confirming you got it right

Three checks, in the order they fail:

```powershell
# 1. Right identity, and no WinUSB compatible ID hiding in there
Get-PnpDevice | Where-Object InstanceId -match 'VID_0FCF' |
  Format-List FriendlyName,Class,Status,InstanceId

# 2. Right function driver
Get-PnpDeviceProperty -InstanceId '<id>' -KeyName DEVPKEY_Device_Service

# 3. Right verdict cached against the device node
Get-PnpDeviceProperty -InstanceId '<id>' -KeyName DEVPKEY_Device_ConfigFlags
```

`DEVPKEY_Device_Service` must read `libusb0`. `ConfigFlags = 64` is
`CONFIGFLAG_FAILEDINSTALL`: the node already failed a driver search once and is
reusing that verdict. Replugging usually clears it; if it does not, see the
next section.

Then, and only then:

```powershell
python tools\ant_probe.py
```

## 2. The `usbflags` cache, and `bcdDevice` as the lever

### What Windows does

Windows queries a device's MS OS descriptors **exactly once** and records the
outcome under

```
HKLM\SYSTEM\CurrentControlSet\Control\usbflags\<vid><pid><bcdDevice>
```

in the `OSVC` value — for example `0FCF10090100`. Per Microsoft's own
documentation, **if that first query fails, Windows records "this device has no
MS OS descriptors" and never asks again.** A related value,
`SkipBOSDescriptorQuery`, suppresses the USB 2.1 BOS query that carries the MS
OS 2.0 platform capability, with the same effect.

The key is `<vid><pid><bcdDevice>` — not the serial number, not the port. So
the cache is shared by every physical device with that triple, and it survives
reflashing, replugging, and rebooting.

### Why this ruins a debugging session

Flash one build whose descriptors are wrong. Windows asks once, gets a bad
answer, writes it down. Fix the firmware, flash again, replug: **Windows does
not ask.** It reads its note. The new build is indistinguishable from the
broken one, from the outside, forever.

Zephyr makes this worse by accident: it derives `bcdDevice` from the *kernel*
version, so it is identical across every rebuild of every configuration you
will ever make. One machine's poisoned entry then applies to all of them.

### The lever

Two ways out. The second needs no elevation and is the one to reach for first.

**Clear the cache.** [`scripts/reset_usb_cache.ps1`](../../scripts/reset_usb_cache.ps1)
removes the matching `usbflags` keys and un-enrolls any attached instance with
`pnputil /remove-device`, so the next plug-in re-enumerates from scratch. It
needs an elevated PowerShell — the `usbflags` key is not writable otherwise —
and supports `-WhatIf`:

```powershell
.\scripts\reset_usb_cache.ps1 -WhatIf          # show what would go
.\scripts\reset_usb_cache.ps1                  # 0FCF:1009 by default
.\scripts\reset_usb_cache.ps1 -ProductId 1008  # the other stick
```

Then unplug, wait a couple of seconds, and plug back in.

**Or move the key.** `CONFIG_ANT_DONGLE_BCD_DEVICE` overrides `bcdDevice`
directly (`hex`, default `0x0000` meaning "keep Zephyr's"). Any fresh value
produces a cache key Windows has never seen, so it queries the device properly
— with no registry edit and no administrator:

```powershell
west ... -- "-DCONFIG_ANT_DONGLE_BCD_DEVICE=0x0201"
```

Bump it on every descriptor-affecting build during bring-up. It costs nothing,
it cannot poison anything, and it is the difference between testing your
firmware and testing a registry value from last Tuesday.

**This is also a shipping concern, not only a bench one.** The composite-device
experiment in
[`docs/decisions/0003`](../../docs/decisions/0003-naming-trademark-and-usb-identity.md)
— adding a second vendor interface with MS OS 2.0 descriptors so Windows
auto-binds WinUSB to it, while the legacy `0FCF:1009` interface stays
byte-identical — changes what the descriptor query returns. Shipping that under
the *same* `bcdDevice` would write a new verdict into the cache of every
already-deployed dongle. So the experiment bumps `bcdDevice`, and that is a
release requirement rather than a nicety.

## Using our own INF

[`ant_libusb_win32.inf`](ant_libusb_win32.inf) is complete and correct, and by
itself will not install on 64-bit Windows, because 64-bit Windows requires a
signed catalog and we hold no certificate. The file is still worth having: it
is the authoritative statement of which hardware IDs bind `libusb0`, and it is
the starting point for the three cases where the vendor package is not an
option.

You will need `libusb0.sys` and the two DLLs beside it, laid out as the
`SourceDisksFiles` sections expect (`x86\`, `amd64\`), from a libusb-win32
release.

**Test-signed, for a development machine:**

```powershell
# One-off: a self-signed code-signing certificate, trusted locally
$c = New-SelfSignedCertificate -Type CodeSigningCert `
       -Subject "CN=RadiANT driver signing (test)" `
       -CertStoreLocation Cert:\CurrentUser\My
Export-Certificate -Cert $c -FilePath radiant-test.cer
certutil -addstore -f Root radiant-test.cer
certutil -addstore -f TrustedPublisher radiant-test.cer

# Per package: build the catalog the INF names, then sign it
inf2cat /driver:. /os:10_X64,10_X86
signtool sign /fd sha256 /a /n "RadiANT driver signing (test)" ant_libusb_win32.cat

pnputil /add-driver ant_libusb_win32.inf /install
```

64-bit Windows still refuses a non-WHQL kernel driver unless test signing is
on, which needs a reboot and puts a watermark on the desktop:

```powershell
bcdedit /set testsigning on     # then reboot
```

**Prefer these instead, in this order, unless you specifically need our INF:**

1. **Windows Update.** Settings > Windows Update > Advanced options > Optional
   updates > Driver updates. Zero effort, properly signed, and the same driver.
   If that page is empty, check whether updates are *paused* — a pause empties
   it completely and looks identical to the driver not existing.
2. **Zwift's bundled package**, `pnputil /add-driver ANT_LibUsb.inf /install`
   from `C:\Program Files (x86)\Zwift\Windows ANT Dongle Driver\`.
3. **Zadig**, choosing **libusb-win32**. Zadig generates and self-signs the
   catalog for you and installs its own certificate, which is the whole problem
   above solved by somebody else.

Our INF earns its place when none of those apply: a PID Windows Update does not
cover, an offline or imaged deployment where you already have a signing
process, or simply as the record of what the binding is supposed to look like.

## After any of this

**Replug the dongle.** Driver binding is decided at enumeration, and nothing
above takes effect on an already-attached device.
