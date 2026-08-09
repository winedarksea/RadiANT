# `archive/drivers/` — the Windows driver situation

Checked by: [`../../.github/workflows/linkcheck.yml`](../../.github/workflows/linkcheck.yml)
for the URLs. The hashes below were taken by hand on the retrieval date and
nothing re-checks them; the command that produced each one is given so they can
be re-taken.

**Garmin's ANT libusb-win32 driver package is not redistributed here, and will
not be.** The package is Garmin's. libusb0 inside it is LGPL, but LGPL on a
component is a constraint on what the vendor owed *us*, not a grant letting us
hand the vendor's package on. That decision, and the general rule it comes
from, are in [`docs/preservation.md`](../../docs/preservation.md).

What this directory holds instead is worth more than a copy of the package
would be:

| File | What it is |
|---|---|
| `README.md` | This: what the package is, its per-file SHA-256, both routes to obtaining it, and snapshot URLs |
| [`ant_libusb_win32.inf`](ant_libusb_win32.inf) | **Ours, written from scratch.** Binds `libusb0.sys` to `0FCF:1009`, `0FCF:1008` and `0FCF:1004`. Covers this dongle by construction, which the vendor package only does by accident of the PID we impersonate |
| [`install-notes.md`](install-notes.md) | The two things that cost real time: pick libusb-win32 and nothing else, and the `usbflags` cache that makes Windows never re-ask |

---

## What the package is

`ANT LibUsb Windows Drivers`, current release **11 April 2012**, published by
Dynastream (now Garmin) for the ANT USB Stick 2 and the ANT USB-m. Its own
`README.txt` states plainly what is inside it:

> The actual drivers are the libusb-win32 drivers version 1.2.40.

— that is libusb-win32 **1.2.4.0**, LGPL, wrapped in a Garmin-branded INF, a
Microsoft-signed catalog, a co-installer DLL and `dpinst64.exe`. The catalog is
signed by *Microsoft Windows Hardware Compatibility Publisher* and still
verifies as `Valid` on Windows 11, because it falls inside the pre-2015-07-29
grandfather window for driver signing. That signature is the entire reason the
2012 package is still installable in 2026, and the reason a modern
self-produced package is harder to install than a fourteen-year-old vendor one.

## SHA-256

Taken from the copy Zwift installs, which is the provenance path most people
already have on disk:

```powershell
Get-ChildItem -Recurse "C:\Program Files (x86)\Zwift\Windows ANT Dongle Driver" |
  Where-Object { -not $_.PSIsContainer } |
  ForEach-Object { "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash, $_.Name }
```

Retrieved 2026-08-08:

| File | Bytes | SHA-256 |
|---|---|---|
| `ANT_LibUsb.inf` | 12,716 | `c2752c2ca34ec02c51b4679da51bc5fb8be4047598299f6aa74f69c68595729a` |
| `ANT_LibUsb.cat` | 10,713 | `0ee4ca9ca94127f953e569fa6be9596d12b123c0cefa0927e68ad8f8287dae74` |
| `README.txt` | 1,682 | `8b45a1870d87afc36fb263ec3304d6f1ded5397e44c7e79a8548f7d964817f5b` |
| `dpinst64.exe` | 1,047,632 | `c20a5d3f5be543a8e73cd25f9dbf14aa0fc4ba1fdc249ee4ff91d159d174d0ea` |
| `amd64\libusb0.sys` | 44,480 | `2e2b60e5fb7a274f4945444d5edb058e62cac268c5336ff8f4b9e82245095211` |
| `amd64\libusb0.dll` | 75,200 | `6b34b5fc18d2985c4e0909dd4a07b1058d351cd53f2a82565128c41d25c2685e` |
| `amd64\AntUsbCoInstall_x64.dll` | 49,152 | `55880aba57e6f35960b7e810dcd6fd4f65ff4c1e49d96ec2172bfb1800f7197f` |
| `x86\libusb0.sys` | 35,776 | `720374de3c3e930b3c679def41a7073477f0c9c3156a0400f2f23672ccfcc981` |
| `x86\libusb0_x86.dll` | 67,008 | `5d675c21d0eb0a4bb98f21c13e369ec72163ae3ab1aed7bfe92caeef38eca5d6` |
| `x86\AntUsbCoInstall_x86.dll` | 47,104 | `46986370c17ba4b3cc38700e4f9b52609dcc02bf4d2e00dd516115d0135688c2` |
| `ia64\libusb0.sys` | 90,048 | `3a21c21274d41c35f2568c08b66768b54436bcd1d371d7749c3e399b337451cb` |
| `ia64\libusb0.dll` | 157,120 | `2b25b3b3e7eda2c5e715926f444a5ee834753230a325ef61e40026693693d7d3` |

**There is no package-level hash here, and that is deliberate rather than an
omission.** These are the files as *installed*; the original distributable
archive was not obtained, so hashing something and calling it "the package"
would be a guess wearing a hex string. If you have the original download, add
its hash with:

```powershell
Get-FileHash <downloaded-file> -Algorithm SHA256
```

Note the `ia64` directory. Itanium support, in a package still shipping in
2026. It is a good indicator of how long ago this stopped being maintained.

## Both provenance paths

**1. Windows Update — easiest, nothing to download.** Garmin still publishes
WHQL drivers for both PIDs. They appear as *ANT USB-m* and *ANT USB Stick 2*
under **Settings > Windows Update > Advanced options > Optional updates >
Driver updates**. They are marked `AutoSelectOnWebSites = False`, so they are
never installed automatically, and Windows 11's Device Manager *Update driver*
wizard does not query Windows Update at all — which is why the driver looks
absent when it is merely optional. `README.md`'s Windows drivers section has
the full account, including how to interrogate the update agent directly.

**2. Bundled with Zwift.** `C:\Program Files (x86)\Zwift\Windows ANT Dongle
Driver\`, installed by every Zwift install on Windows. This is where the hashes
above come from. Install it by hand from an elevated shell with:

```powershell
pnputil /add-driver ANT_LibUsb.inf /install
```

Zwift's own installer path (`DriverPackagePreinstallW`, via `dpinst64.exe`) can
fail with `0xE000024B` — check `%windir%\DPINST.LOG` and fall back to `pnputil`
or Windows Update.

A third route, [Zadig](https://zadig.akeo.ie), does not use Garmin's package at
all: it generates and self-signs a catalog for a driver of your choosing. It is
the right tool when Windows Update has nothing to offer, and the one place a
wrong click is expensive — see [`install-notes.md`](install-notes.md).

## Snapshots

Anonymous "Save Page Now" submission was attempted on 2026-08-08 and refused
(the endpoint closed the connection; it now wants an account). So these are
**pre-existing snapshots that were verified to exist**, not snapshots this
project created. Saying which is which matters: a snapshot we did not take is
a snapshot nobody guaranteed the freshness of.

| Page | Latest snapshot found | Snapshot URL |
|---|---|---|
| thisisant.com developer downloads | 2026-05-18 | <https://web.archive.org/web/20260518152929/https://www.thisisant.com/developer/resources/downloads/> |
| libusb-win32 on SourceForge | 2026-07-30 | <https://web.archive.org/web/20260730084343/https://sourceforge.net/projects/libusb-win32/> |
| libusb-win32 project site | 2026-06-07 | <https://web.archive.org/web/20260607004644/http://libusb-win32.sourceforge.net/> — a capture of a **301 redirect**, not of content. The project site now forwards to the SourceForge page above, which is the one to rely on |

To submit a fresh one with an account:

```powershell
Invoke-WebRequest -Uri "https://web.archive.org/save/<url>" -UseBasicParsing -TimeoutSec 180
```

Live URLs, all reachable on 2026-08-08:

- <https://www.thisisant.com/developer/resources/downloads/>
- <https://sourceforge.net/projects/libusb-win32/>
- <https://zadig.akeo.ie>

## Why our own INF exists at all

Three reasons, in order of how much they matter.

**It covers this device by construction.** Garmin's INF binds `0FCF:1009`
because that is an ANT USB-m. Ours binds it because that is *our dongle's PID*,
recorded deliberately — see
[`docs/decisions/0003`](../../docs/decisions/0003-naming-trademark-and-usb-identity.md).
The two happen to coincide today. If the PID ever changes, one of those files
can be edited and the other cannot.

**It covers `0FCF:1004` as well.** The original ANT USB stick. Adding a
hardware ID to a file we own is a one-line change; adding one to a vendor
package means re-signing it.

**It is a written record of what "the driver works" actually means.** Four
facts: the function driver is `libusb0.sys`; it is installed as a service named
`libusb0` with `SPSVCINST_ASSOCSERVICE`; the 32-bit `libusb0.dll` must land in
`SysWOW64` because every shipping ANT application is 32-bit; and the device
class is `{EB781AAF-9C70-4523-A5DF-642A87ECA567}`. Those four are what you
check when a bound device still does not work, and an INF is the most compact
way to state them.

It is **not** a drop-in replacement for the vendor package, because we cannot
sign it. [`install-notes.md`](install-notes.md) says what to do about that.
