<!-- SPDX-License-Identifier: Apache-2.0 -->
# Spike A - `rx_raw`

Checked by: `docs/spike-a-results.md`, which records what this program actually
printed on the bench. If the two disagree, the capture is right and the prose is
stale.

Provenance: clean-room. Written from `docs/ant-radio-link.md`, from the public
Nordic MDK headers and product specifications, and from Zephyr's public
clock-control API. Nothing here derives from `sdk-ant`, from `libant.a`, or from
an ANT+ device profile document. See
`docs/decisions/0002-clean-room-policy.md`.

## What it is for

The RadiANT plan rests on one claim: **an nRF RADIO can be configured to hear a
byte-exact ANT frame with no software in the bit path.** `docs/ant-radio-link.md`
states that claim as a register mapping and marks every row a *prediction*. This
program is the cheapest available attempt to falsify it.

It is a go/no-go, written to fail loudly rather than succeed quietly. It does
not assume the predicted register arithmetic is right: it sweeps the two
documented details that are easy to get backwards, prints the register values
it actually programmed each time, and reports counters whether or not anything
was heard.

## Running it

Two boards. One transmits, one receives - a board cannot hear itself.

**Transmitter: the Feather**, already flashed with the shipping dongle firmware
and enumerating as `0FCF:1009`. It is driven as an ANT+ master from the host, so
this costs **no Feather flash**:

```powershell
& C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe tools\ant_sim.py --serial 6183 --seconds 720 -q
```

That transmits ANT+ bicycle power (device type 11) at 4.005 Hz. The device
number and transmission type it uses are compiled into this spike as
`SPIKE_DEVNUM` / `SPIKE_DTYPE` / `SPIKE_TTYPE`; confirm them independently with
`tools/ant_scan.py` before trusting a pass, because a spike that matches its own
assumption has proved nothing.

**Receiver: the nRF54L15 DK**, which has a J-Link and can be flashed unattended.

```powershell
. .\scripts\env.ps1 -NcsVersion v3.2.4
Push-Location C:\ncs\v3.2.4
west -z C:\ncs\v3.2.4\zephyr build -s C:\Users\Colin\ant_dongle\radiant\spike\rx_raw `
     -d C:\Users\Colin\ant_dongle\build\spike_rx_raw -b nrf54l15dk/nrf54l15/cpuapp -p always
Pop-Location
.\scripts\flash_sim_jlink.ps1 -HexPath build\spike_rx_raw\merged.hex -JLinkExe <shim>
```

Output is on **COM7** at 115200 (the DK's first VCOM, `uart20`). Not RTT: the
VCOM needs no SEGGER DLL on the host, and a result that can only be read through
vendor tooling is a result that is hard to archive.

Two things about the flashing step, both of which have cost bench time before:

- **Two J-Link probes are attached** and non-interactive J-Link cannot choose
  between them, so every command fails with "Cannot connect to the
  probe/programmer" unless one is selected. Point `-JLinkExe` at a one-line
  `.cmd` shim that injects `-SelectEmuBySN <serial>` and forwards `%*`, so the
  script's own `exec DisableAutoUpdateFW` still runs first.
- **Never let J-Link auto-update the probe firmware.** See the note at the top
  of `scripts/flash_sim_jlink.ps1`.

Do not pipe the flash script into `Select-Object -First N`. PowerShell stops the
upstream pipeline once the requested objects arrive, which kills `JLink.exe`
mid-program and returns 255.

## What it does

Five phases, repeating forever, so a capture started at any moment gets a
complete cycle and two cycles are a reproducibility check for free.

| Phase | Question | Method |
|---|---|---|
| A | Which register arithmetic puts `A6 C5 dnl dnh dtype` on the air? | 8 permutations of {bit-reversal} x {which end of `BASE0` is first on air} x {prefix first or last}, 5-byte address, **static** 10-byte body |
| B | Is the length byte really a length byte? | The predicted `S0LEN=1, LFLEN=8, CRCINC=1` against the documented `PCNF0=0, STATLEN=10` fallback |
| C | Can a 3-byte address see the channel ID? | `BALEN=2`, `STATLEN=12`, sweeping the packing axis that only exists once `BALEN < 4` |
| D | The pass criterion | 60 s tracking, then 60 s search, counting CRC-valid frames and checking every decoded field |
| E | The 30-minute bonus | Preamble-as-address: `BALEN=2`, `[55 A6 C5]`, `SKIPADDR=Skip`, `CRCINIT=0x233E` |

Phase A uses a static length deliberately: it makes the address question
independent of whether the length byte is a length byte at all, so exactly one
unknown is resolved at a time. Phase B then re-tests the winner with the real
length field.

> **"The length byte" above is history, and the answer is in.** Byte 3 is a
> control byte of six independent fields, and its low bits are not a length:
> `0x0A` reads 10 there and `0xA2` reads 2, both with an eight-byte payload. See
> `docs/spike-b-part2-results.md`. Phase B's `CRCINC=1, LFLEN=8` configuration
> works and is kept in the record because it did — but it is a **broadcast-only
> receiver**, and `PCNF0 = 0, STATLEN = 10` is the required form for receive and
> transmit alike. Read this file as the question Spike A asked, not as the
> answer.

Phase C is what makes the pass criterion non-circular. A 5-byte address match
proves the device number and device type were on the air, but it cannot *print*
them - those bytes are consumed by the hardware matcher and never reach RAM.
With a 3-byte address they land in the buffer and can be read out.

Phase C and E rank configurations on **decoded IDs, not CRC-valid frames**. A
3-byte address is short enough that noise triggers the matcher a few times a
second, and a variant whose only output is CRC errors has found the noise floor
rather than the transmitter. The printed RSSI is the giveaway: the real
transmitter sits ~70 dB above the false triggers on this bench.

## Reading the output

```
[cfg] <name> addr=<on-air bytes> BALEN=n BASE0=0x... PREFIX0=0x.. PCNF0=0x... PCNF1=0x...
  PKT crc=OK  rxmatch=0 rssi=-17dBm len=0x0A dev=14871 type=0x0B trans=5
      raw18 55 A6 C5 17 3A 0B 05 0A 10 E7 FF 4F 46 5A 62 00 65 46
      rxcrc=0x6546 sw_crc=0x6546 match  recompute_over_17=0x0000 zero
[res] <name> end=n crc_ok=n crc_err=n id_ok=n sw_crc_ok=n zero_ok=n rssi_avg=-17dBm
```

`raw18` is a **reconstruction**, and saying so matters. Only 10 to 13 of the 18
bytes are ever in RAM: the address is consumed by the hardware matcher and the
CRC lands in `RADIO->RXCRC`. The address bytes printed are the ones that were
programmed - the match is the evidence they were on the air - and the leading
`0x55` is the one genuine assumption in the line, because the demodulator does
not report what it locked on to.

`sw_crc` and `recompute_over_17` are the independent check. The software CRC is
computed over the reconstructed frame with no help from the radio, and the two
forms catch different mistakes: `sw_crc == rxcrc` says the polynomial, seed and
coverage window are right, and `recompute_over_17 == 0` says the same thing a
second way, via the property that appending a correct CCITT-FALSE CRC drives the
register to zero. A boot-time self-test runs both against the synthetic golden
vector in `docs/ant-radio-link.md` before the radio is touched at all, so a
broken CRC implementation cannot masquerade as a broken radio.

## Deliberate limits

- **Polling, not interrupts.** The `SHORTS` chain keeps the receiver armed with
  no software involvement between packets and the transmitter runs at 4 Hz, so
  the only thing the CPU must do is copy the buffer within 250 ms. An interrupt
  would add a concurrency question and answer none.
- **Receive only.** Transmit is a separate question and Spike A does not need
  it to answer this one.
- **One filter.** `RXADDRESSES = 1`. The eight-logical-address sweep the plan
  describes is core policy, not something a go/no-go has to demonstrate.
- **`radiant` proper links none of this.** The spike is a standalone Zephyr
  application outside the repo root `CMakeLists.txt`, so it can never end up in
  a shipped image by accident.

## Captures

The raw COM7 logs the results in `docs/spike-a-results.md` were read from live
in `archive/captures/radio/2026-08-09-nrf54l15-run{1,2}.log`, each with a header
explaining what it is. They moved there because they are what
`docs/preservation.md` calls the safest and highest-value artifact class - our
own recordings of our own hardware - and because a spike directory is a place
work passes through, not a place evidence lives.
