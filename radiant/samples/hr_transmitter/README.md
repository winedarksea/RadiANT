# Heart-rate transmitter

Broadcasts a simulated 120 bpm heart rate as device 12345. Any ANT+ compatible
receiver will see it as a heart-rate strap, including a second board running
[`hr_receiver`](../hr_receiver/).

This is a demonstration: the heart rate is made up. It is not a certified ANT+
product.

## Build and flash

From your west workspace:

```sh
west build -b nrf52840dk/nrf52840 path/to/radiant/samples/hr_transmitter
west flash
```

For the nRF54L15 DK, use `-b nrf54l15dk/nrf54l15/cpuapp`.

For a Pro Micro nRF52840, use `-b promicro_nrf52840/nrf52840/uf2`, then double-tap
reset and copy `zephyr.uf2` from the build directory onto the drive that appears.
That board has no console in this sample.

See the [receiver's README](../hr_receiver/README.md) for other boards and for
checking the build picked a real radio.

## Estimating power without a meter

Two fragments turn on RadiANT's software power proxy (`CONFIG_RADIANT_POWER_DIAG`):
how long the HFXO request was held, radio transmit and receive on-time, CPU
load and idle entries.

```sh
# A "power dt_ms=... hfxo_pm=... tx_us=..." log line every 10 s:
west build -b nrf54l15dk/nrf54l15/cpuapp path/to/radiant/samples/hr_transmitter -- -DEXTRA_CONF_FILE=power_diag.conf
# No console at all; read the radiant_power_diag struct over SWD instead:
west build -b nrf54l15dk/nrf54l15/cpuapp path/to/radiant/samples/hr_transmitter -- -DEXTRA_CONF_FILE=power_diag_quiet.conf
```

To combine one with another fragment, separate them with a semicolon:
`-DEXTRA_CONF_FILE="other.conf;power_diag_quiet.conf"`. The struct layout
and how to read it without a torn read are in
`radiant/include/radiant/radiant_power_diag.h`. These numbers are proxies.
Multiply them by datasheet currents to get an estimate, not a measurement.
