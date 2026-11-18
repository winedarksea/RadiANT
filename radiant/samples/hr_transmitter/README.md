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

For the TI LAUNCHXL-CC26X2R1, add the hal_ti module, which the Nordic Connect
SDK's manifest leaves out (this repository's `scripts/fetch_hal_ti.ps1` fetches
it):

```sh
west build -b cc26x2r1_launchxl path/to/radiant/samples/hr_transmitter -- \
    -DEXTRA_ZEPHYR_MODULES=/path/to/modules/hal/ti
```

## Low power

The default build keeps the board's console and log on, which is right for a
first run and wrong for a battery. `lowpower.conf` turns off the log, the
console and the UART driver:

```sh
west build -b nrf54l15dk/nrf54l15/cpuapp path/to/radiant/samples/hr_transmitter \
    -- -DEXTRA_CONF_FILE=lowpower.conf
```

With no UART driver linked, no UART is ever switched on, so no board overlay is
needed. An enabled UART can keep the fast clock running, which can cost
more than the radio itself at 4 messages a second. `printk()` still compiles and
goes nowhere.

**One way or both ways.** By default the sample is a bidirectional master: after
every message it listens briefly in case the receiver answers. `one_way.conf`
makes it transmit only (`CONFIG_HR_TX_ONE_WAY`), which drops that listen:

```sh
west build ... -- -DEXTRA_CONF_FILE="lowpower.conf;one_way.conf"
```

Receivers decode it the same way, but nothing they send can arrive: no page
requests, no acknowledged messages. The ANT+ heart-rate profile asks a strap to
be a bidirectional master, so the default stays that way.

**Regulator.** Whether the chip runs its DC/DC converter or its LDO is set in
devicetree, and DC/DC needs an inductor on the board. The nRF52840 DK and the
nRF54L15 DK already turn it on. The Pro Micro board leaves the nRF52840 on LDO.
Enable DC/DC only on a board you know has the inductor; without it the chip
does not run:

```dts
&reg1 {
	regulator-initial-mode = <NRF5X_REG_MODE_DCDC>;
};
```

**Receivers.** A battery receiver should not search forever.
[`hr_receiver`](../hr_receiver/) searches without a limit
(`antr_channel_search_timeout_set(ch, 0xFF)`), which keeps the radio listening
until a sensor appears. Set a finite timeout instead, in 2.5 s steps, and open
the channel again later if nothing was found.
