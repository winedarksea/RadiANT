# Heart-rate receiver

Hears the first ANT+ compatible heart-rate sensor in range and prints its device
number and heart rate about once a second:

```
Searching for a heart-rate sensor...
device 12345: 120 bpm
```

You need a heart-rate strap being worn, or a second board running
[`hr_transmitter`](../hr_transmitter/).

## Build and flash

From your west workspace:

```sh
west build -b nrf52840dk/nrf52840 path/to/radiant/samples/hr_receiver
west flash
```

For the nRF54L15 DK, use `-b nrf54l15dk/nrf54l15/cpuapp`. On that board, the
terminal program must assert DTR or the serial port shows nothing.

## Another board

Any nRF52, nRF53 network core or nRF54L board needs one thing: a devicetree
overlay in `boards/` naming a timer with at least five compare channels. Copy
one of the overlays here. Without it the build stops with an error saying so.

Check that the build picked a real radio:

```sh
grep CONFIG_RADIANT_BACKEND_ build/hr_receiver/zephyr/.config
```

(Without sysbuild, the file is at `build/zephyr/.config`.) You want
`CONFIG_RADIANT_BACKEND_NRF=y`. `CONFIG_RADIANT_BACKEND_NULL=y` means the image
cannot use the radio, and the build prints a warning.
