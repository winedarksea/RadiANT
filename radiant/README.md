# radiant

A clean-room, ANT+-compatible link layer for Zephyr, as a self-contained Zephyr
module. It decides what goes on the air and when; a radio backend decides how. No
Nordic sdk-ant and no proprietary radio stack. RadiANT is an independent
implementation: it is not certified by, licensed by or affiliated with Garmin or
Dynastream.

## Use RadiANT in your firmware

Receive your first packet in three steps.

**1. Try a sample.** You need a [supported board](#supported-hardware) and a
heart-rate strap, or a second board to act as one. From your west workspace:

```sh
west build -b nrf52840dk/nrf52840 path/to/radiant/samples/hr_receiver
west flash
```

[`samples/hr_receiver`](samples/hr_receiver/) prints each sensor it hears.
[`samples/hr_transmitter`](samples/hr_transmitter/) pretends to be a strap.

**2. Add the module to your project.** With west, add this to your manifest and
run `west update`:

```yaml
manifest:
  projects:
    - name: radiant
      url: https://github.com/winedarksea/RadiANT
      revision: master  # pin a tag or commit once you depend on it
      path: modules/lib/radiant
```

The first release containing the API below is the one after v1.2.0.

Then, before `find_package(Zephyr)` in your `CMakeLists.txt`:

```cmake
list(APPEND ZEPHYR_EXTRA_MODULES $ENV{ZEPHYR_BASE}/../modules/lib/radiant/radiant)
```

The final `radiant` is the module directory inside the repository. Without west,
copy `radiant/` anywhere and point that line at it.

In `prj.conf`:

```
CONFIG_RADIANT=y
CONFIG_RADIANT_ANTR_API=y
```

On nRF, also copy a devicetree overlay from the samples' `boards/` directory. It
names the timer the radio uses, and the build stops with an error if it is missing.

**3. Write your application.** Call `antr_init()`, configure a channel, and
implement `antr_on_message()`. The [integration guide](docs/integration.md) walks
through it, and the receiver sample is under 100 lines including comments.

**Check you got a real radio:**

```sh
grep CONFIG_RADIANT_BACKEND_ build/zephyr/.config
```

(With sysbuild the file is at `build/<your app>/zephyr/.config`.) The backend is
chosen from your SoC. `CONFIG_RADIANT_BACKEND_NULL=y` means the image cannot
transmit or receive, and on real hardware the build warns you. The usual cause is
`CONFIG_BT=y` without `CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL=y`, which needs the
Nordic Connect SDK.

## Supported hardware

| Hardware | Backend | Status |
|---|---|---|
| nRF52840 | `nrf` | Used on air by the RadiANT dongle on three boards. Samples build for the nRF52840 DK; the transmitter also for the Pro Micro |
| nRF54L15, nRF54LM20 | `nrf` | nRF54L15 on-air verified, including both samples on the nRF54L15 DK |
| nRF5340 network core | `nrf` | On-air verified. No sample overlay |
| TI CC13x2 / CC26x2 | `cc26xx` | On-air verified on the CC2652P. Needs the `hal_ti` module. No sample overlay |

Builds are verified on Nordic Connect SDK v3.4.0 and on stock Zephyr v4.4.0, where
CI compiles both samples. On-air testing has been on the Nordic Connect SDK.

Sharing the radio with Bluetooth or 802.15.4 on nRF needs Nordic's MPSL
(`CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL`), so it is only available on that SDK.

The nRF backend's ramp-up figures come from the datasheet and its timing
calibration is seeded from Zephyr's per-SoC BLE chain delays, not measured with
the wired two-board trigger the HAL contract asks for. Expect timing to be
approximately calibrated rather than exact. The header of
[`src/radiant_radio_nrf.c`](src/radiant_radio_nrf.c) says what is measured and
what is not.

## What's in the module

| Part | Enable with | Include |
|---|---|---|
| Link layer and radio backend | `CONFIG_RADIANT` | `<radiant/radiant_channel.h>` and others |
| `antr_*` channel API and runtime | `CONFIG_RADIANT_ANTR_API` | `<radiant/ant_radio.h>` |
| ANT+ device profile pages | `CONFIG_RADIANT_PROFILES` | `"profile_hr.h"` and others |
| Sample bus, sinks, profile adapters | `CONFIG_RADIANT_BRIDGE` | `"radiant_bridge.h"` and others |
| Payload authentication and encryption | `CONFIG_RADIANT_SEC` | `<radiant/radiant_sec.h>` |

Code you do not call is dropped at link time, so the profile and bridge switches
cost nothing until you use them.

- Up to 32 channels, with background scan mode.
- A vendor-neutral radio HAL
  ([`include/radiant/radiant_radio_hal.h`](include/radiant/radiant_radio_hal.h)),
  with a capability query so core policy never tests for a backend by name.
- Optional security: secured channels do **not** interoperate with standard ANT+
  profiles. See [`../docs/radiant-security.md`](../docs/radiant-security.md).

`CONFIG_RADIANT_ANTR_API` is the usual choice. Without it you write the runtime
yourself against the low-level API, which the end of the
[integration guide](docs/integration.md) describes.

## Further reading

- [`../docs/backends.md`](../docs/backends.md): the backend and HAL contract
- [`../docs/ant-radio-link.md`](../docs/ant-radio-link.md): the on-air reference
  this module was built against
- [`../docs/device-profiles.md`](../docs/device-profiles.md): every profile's pages
- [`../docs/decisions/`](../docs/decisions/): design decisions, including naming,
  the clean-room policy and the security scope
