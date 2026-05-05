The primary goal of this project is to test writing to a Nordic device a zephyr program with ANT+ inside it.

/Users/colincatlin/Documents-NoCloud/sdk-ant is a route to the official SDK for ANT locally from NRF

The Nordic SDK for VSCode is being used for development, so the project should align with that for build and board configuration.

Deliverable: a fully functional ANT+ dongle that can be used for Zwift or similar computer software (Windows, Linux, Android, Mac).
It needs to support three device types: heart rate, power meter, and speed/cadence sensor.
It may need to spoof a garmin device id to be recognized by default and use those drivers.
The target is an Adafruit Feather, aiming for a compatible uf2 file build. If this fails we can switch to using a Nordic Semiconder debugger programmer input.

In bootloader mode the board may expose a serial device or removable volume, but the final ANT dongle firmware is vendor-specific USB and should not create a `/dev/tty.usbmodem*` device.

Target Device: Adafruit Feather with NRF52840, although ideally this would work more generically on as many nordic devices as possible with a simple flash.

Ideally the onboard LED will be used as an activity signal, but in such a way that on another board that may not have this LED (this board actually has two, an RGB LED and D3 a regular LED) the code should still run without issue (being a PWM signal likely, it probably will be fine anyway on an unconnected pin).

## Implementation Notes (unverified)
Spoof ideas appear to be:
VID: 0x0FCF (Dynastream Innovations)
PID: 0x1008 (ANT2USB) or 0x1009 (ANTUSB-m) Zwift uses libusb (Mac/Linux) or WinUSB (Windows) to scan the USB bus specifically for these IDs. If you don't spoof them, Zwift will never attempt to connect to the dongle.

The nRF52840 supports many simultaneous channels, but in this sdk-ant integration the relevant setting is `CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED`. It should be set to at least 8 so Zwift has enough headroom to open multiple device connections concurrently.

The nRF SDK for ANT usually provides a serialization library (often called ant_serial or ant_bootloader_serial) that handles formatting the nRF ANT structs into the raw hex byte format expected by the PC, and vice-versa.

zephyr/samples/net/openthread/coprocessor may be useful reference
https://github.com/vincent290587/Climber is not the same but a GitHub project with ant on an nrf

## Current Status

The firmware builds successfully for `adafruit_feather_nrf52840/nrf52840/uf2` against NCS `v3.2.4` and emits a UF2 image at `build/ant_dongle/zephyr/zephyr.uf2`.

The current application is configured to:

- Enumerate as `VID 0x0FCF / PID 0x1008`
- Disable the board's default CDC ACM console so only the ANT vendor interface is exposed
- Register Microsoft OS 1.0 and 2.0 descriptors so Windows can bind WinUSB automatically
- Answer the common ANT request messages used by the sdk-ant network-processor bridge during host setup

On macOS and Linux, success should be checked in the USB device tree by VID/PID. A tty device is not expected once CDC ACM has been removed.

## Build

Use the Nordic-managed SDK install at `/opt/nordic/ncs/v3.2.4`:

```sh
export PATH=/opt/nordic/ncs/toolchains/185bb0e3b6/bin:/opt/nordic/ncs/toolchains/185bb0e3b6/usr/bin:/opt/nordic/ncs/toolchains/185bb0e3b6/usr/local/bin:/opt/nordic/ncs/toolchains/185bb0e3b6/opt/bin:/opt/nordic/ncs/toolchains/185bb0e3b6/opt/nanopb/generator-bin:/opt/nordic/ncs/toolchains/185bb0e3b6/nrfutil/bin:/opt/nordic/ncs/toolchains/185bb0e3b6/opt/zephyr-sdk/arm-zephyr-eabi/bin:/opt/nordic/ncs/toolchains/185bb0e3b6/opt/zephyr-sdk/riscv64-zephyr-elf/bin:$PATH
export GIT_EXEC_PATH=/opt/nordic/ncs/toolchains/185bb0e3b6/Cellar/git/2.37.3/libexec/git-core
export GIT_TEMPLATE_DIR=/opt/nordic/ncs/toolchains/185bb0e3b6/Cellar/git/2.37.3/share/git-core/templates
export NRFUTIL_HOME=/opt/nordic/ncs/toolchains/185bb0e3b6/nrfutil/home
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=/opt/nordic/ncs/toolchains/185bb0e3b6/opt/zephyr-sdk
west -z /opt/nordic/ncs/v3.2.4/zephyr build -b adafruit_feather_nrf52840/nrf52840/uf2 -p always
```

## Flash

The VS Code `uf2` runner currently fails with `ValueError: uf2 doesn't support --dev-id option`. Use the repo script instead.

1. Close any tool that still has the Feather open.
2. Double-tap RESET on the Feather until `/Volumes/FTHR840BOOT` appears.
3. Run:

```sh
./scripts/flash_uf2.sh
```

The script writes `build/ant_dongle/zephyr/zephyr.uf2` to the mounted bootloader volume using `dd bs=4k`, which was more reliable than `cat` during testing.

## Verification

After flashing the application image, do not expect `/dev/tty.usbmodem*`.

Check for the ANT USB device instead:

```sh
system_profiler SPUSBDataType | grep -A6 -B2 '0FCF\|1008\|Dynastream\|ANTUSB2'
```

or:

```sh
ioreg -p IOUSB -l -w 0 | grep -i '0fcf\|1008\|dynastream\|antusb'
```
