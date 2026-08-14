# apps/dongle_thread

The same ANT+ USB dongle as [apps/dongle](../dongle), plus the P3/P4
multiprotocol coexistence bench instruments: a BLE advertiser
(`ble_coex_load.c`) and a Thread node (`thread_coex_load.c`) that each
contend for the radio with radiant's arbitrated backend
(`CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL`).

This is a separate application, not a Kconfig variant of `apps/dongle`, on
purpose: `apps/dongle` is ANT+-only by design and carries none of this - not
even linked-but-idle - because every byte and every interrupt priority there
exists to answer ANT+ traffic as fast as possible.

## Building

A plain build with no extra configuration is an ordinary ANT+-only dongle
image; the second stack is opt-in through `EXTRA_CONF_FILE`:

```
west build -s apps/dongle_thread -b nrf54l15dk/nrf54l15/cpuapp -- \
    -DANT_RADIO=core -DRADIANT_BACKEND=nrf -DEXTRA_CONF_FILE=gate.conf
```

`gate.conf` is the arbiter with no second stack (the A of the A/B).
`thread.conf` (optionally with `thread_sed.conf` layered on top) is the
contended half. See each `.conf` fragment's own header for what it measures
and why the flags above are not optional.

`scripts/build_p4.ps1` drives the three P4 arms (`p4ctrl`, `p4med`, `p4sed`)
end to end, including the backend read-back that catches a silent fallback to
the null radio. `scripts/p4_bench.ps1` flashes and captures a run.

## Boards

`boards/` carries the nRF54L15 DK and nRF5340 DK fragments the P4 gate runs
on - neither has a USB device peripheral this application uses, so both land
on the plain-UART transport. See `docs/testing.md` for the bench setup
(the second board, credentials, VCOM wiring).
