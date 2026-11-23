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

Any nRF52, nRF53 network core or nRF54L board builds as it is. The radio takes
a default hardware timer (TIMER4 on nRF52, TIMER20 on nRF54L, TIMER2 on the
nRF5340 network core). Only if your board already uses that timer, or the part
has no TIMER4 (nRF52805/810/811/820), does the build stop and ask for an overlay
in `boards/` naming another timer with at least five compare channels. The
module's [README](../../README.md) shows the two lines.

Check that the build picked a real radio:

```sh
grep CONFIG_RADIANT_BACKEND_ build/hr_receiver/zephyr/.config
```

(Without sysbuild, the file is at `build/zephyr/.config`.) You want
`CONFIG_RADIANT_BACKEND_NRF=y`. `CONFIG_RADIANT_BACKEND_NULL=y` means the image
cannot use the radio, and the build prints a warning.
