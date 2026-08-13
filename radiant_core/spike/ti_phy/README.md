<!-- SPDX-License-Identifier: Apache-2.0 -->
# Spike TI-PHY — `ti_phy`

**Status: run on hardware 2026-08-13, and it PASSES.** A LAUNCHXL-CC26X2R1
received byte-exact ANT+ broadcasts from a Feather driven by `tools/ant_sim.py`
and transmitted a frame a second, independent ANT stick decoded. The numbers
are in *What it measured* below.

The three findings under *What the settings database already answered* were
derived from TI's shipped data before any hardware ran and are still marked as
such; the hardware run did not contradict them.

Provenance: clean-room. Written from `docs/ant-radio-link.md`, from TI's public
CC13x2/CC26x2 Technical Reference Manual and `driverlib` headers, from the
public SimpleLink RF driver API, and from SmartRF Studio's settings database as
shipped in the SimpleLink CC13xx/CC26xx SDK
(`source/ti/devices/radioconfig/.meta/config/cc2652r_prop_pg21/`). Nothing here
derives from `sdk-ant`, from `libant.a`, or from an adopter-gated ANT+ device
profile document. See
[`docs/decisions/0002-clean-room-policy.md`](../../../docs/decisions/0002-clean-room-policy.md).

## What it is for

The TI port rests on one claim nobody here has tested: **that TI's RF core can
be configured for ANT's PHY — 1 Mbps 2-GFSK at ~170 kHz deviation — and
demodulate a byte-exact ANT frame.** It is the whole of P1 and it gates
everything after it. If this program cannot receive a frame, the backend is not
written and the port stops, which is the cheapest place for it to stop.

It is the sixth spike in this directory and keeps their shape: bare, no
`radiant_core`, no HAL, just the peripheral and a console. It is written to
**fail loudly rather than succeed quietly** — every phase prints what it
programmed and what came back, including the phases that work.

## What the settings database already answered, before any hardware

These are derived from data TI ships, not measured on this bench. They are
worth having up front because two of them are already caps-table facts and one
of them is the reason phase 2 is a sweep.

**The stock settings stop at 250 kbps, and that is a gap in TI's catalogue
rather than a limit of the part.** SmartRF ships exactly three 2.4 GHz
proprietary PHYs for this device:

| id | PHY |
|---|---|
| `tc900` | 100 kbps, 50 kHz deviation, 2-GFSK, 350 kHz RX BW |
| `tc901` | 250 kbps, 125 kHz deviation, 2-GFSK, 530 kHz RX BW |
| `tc902` | 250 kbps, 62.5 kHz deviation, MSK, 530 kHz RX BW |

But `symbolRate.rateWord` is 21 bits, so 1 Mbps is nowhere near a ceiling, and
the RX filter bandwidth table runs to 1883.7 kHz. ANT at 1 Mbps with 170 kHz
deviation wants about 2 × (170 + 500) = **1340 kHz** by Carson's rule, and the
table has 1243.2 (code 97) and 1567.2 (code 98) either side of it. There is no
code at 1340, which is exactly why phase 2 sweeps rather than chooses.

**`caps.max_filters` is 2.** `CMD_PROP_RX_ADV` has `syncWord0` and `syncWord1`
and nothing else. That is RAIL's number, not the nRF's eight, and it is the
number that turns the wildcard sweep from 32 sets into 128 — see *What this
means for P3* below.

**`caps.addr_len_hw_max` is 4, and ANT's tracking address is 5.**
`formatConf.nSwBits` is documented 8..32, so this part cannot match a tracked
channel's full on-air address in hardware; a backend must match four bytes and
finish the device-type byte in software. The nRF matches all five. This was
knowable from a header and it is a real caps difference.

## The geometry, and why it is the search one

ANT's frame after the preamble is 17 bytes, of which the CRC covers the first
15. This spike splits them the way a **search** window does:

```
sync word   3 bytes   A6 C5 devnum_lo
body       12 bytes   devnum_hi dtype ttype control d0..d7
CRC         2 bytes
```

rather than the way a tracking window does (5-byte address, 10-byte body), for
two reasons.

**Inherited, from Spike B:** a receiver that misparses a body byte cannot
report what that byte is. The search geometry puts every interesting byte in
RAM, and the channel ID is read out of RAM rather than assumed — which is what
makes a pass non-circular.

**New:** it is inside the 4-byte hardware limit above with a byte to spare, so
it answers the PHY question without also answering the address-matcher
question. One unknown at a time.

## Two decisions that would produce silence if wrong

**`bMsbFirst = 1`.** This is *not* what a hasty reading of
`docs/ant-radio-link.md`'s *Fact two* produces. That fact says address bytes
must be bit-reversed into the nRF's `BASE`/`PREFIX` registers — because the nRF
serialises its address field LSBit-first while serialising the payload
MSBit-first under `ENDIAN=Big`. The invariant is the **on-air** order, and on
air the whole ANT frame is MSBit-first. A radio that serialises sync word and
payload the same way needs no reversal anywhere. Getting this backwards
produces *silence*, not a weak link — which is exactly what Spike A measured on
the nRF across its eight permutations.

**`preamConf`: one preamble byte, `preamMode = 2`.** ANT's preamble is one
byte, chosen so the alternating pattern runs continuously into the first bit of
the address; `preamMode = 2` ("same first bit in preamble and sync word")
reproduces the nRF's rule exactly. SmartRF's own settings use four preamble
bytes and `preamMode = 0`, which is right for their PHYs and wrong for this
one. Note that on transmit this is the **first time this project chooses the
preamble byte** rather than inheriting it: `docs/ant-radio-link.md` records
that the byte itself has never been directly observed, because the nRF derives
it from the first address bit and does not report what it locked onto.

## Running it

Two boards. One transmits, one receives — a board cannot hear itself.

**Transmitter: the Feather**, already flashed with the ordinary dongle image
and enumerating as `0FCF:1009`, driven as an ANT+ master from the host. That
costs **no Feather flash**:

```powershell
& C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe tools\ant_sim.py --serial 6183 --seconds 720 -q
```

Confirm the device number, device type and transmission type independently with
`tools/ant_scan.py` **on the other stick** and set `SPIKE_DEVNUM` /
`SPIKE_DTYPE` / `SPIKE_TTYPE` in `src/main.c` to match. **A spike that matches
its own assumption has proved nothing** — and this is not theoretical here:
`ant_sim.py` derives the device number from the transmitting stick's own
identity rather than from the command line, so the first draft's `14871` was
fiction and the real number on this bench is **52233**.

```powershell
& C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe tools\ant_scan.py --serial 772A --seconds 20
#   found: #52233 - power meter
```

**Receiver: the LaunchXL.**

```powershell
. .\scripts\env.ps1 -NcsVersion v3.4.0
.\scripts\fetch_hal_ti.ps1
Push-Location C:\ncs\v3.4.0
west -z C:\ncs\v3.4.0\zephyr build `
     -s C:\Users\Colin\ant_dongle\radiant_core\spike\ti_phy `
     -d C:\Users\Colin\ant_dongle\build\spike_ti_phy `
     -b cc26x2r1_launchxl -p always `
     -- -DEXTRA_ZEPHYR_MODULES=C:/ncs/v3.4.0/modules/hal/ti
Pop-Location
.\scripts\flash_ti.ps1 -HexPath build\spike_ti_phy\ti_phy\zephyr\zephyr.hex
```

Output is on the **XDS110 backchannel COM port at 115200** — COM14 on this
bench. Note that is the *spike's* arrangement and the **opposite** of the
dongle image's, which reserves that port for the ANT byte stream at 57600 and
sends its log to `uart1` on BoosterPack pins. A spike has no ANT serial
protocol to carry, so the one reachable port should carry the thing the spike
exists to produce.

Flashing this board is not as simple as it looks; `scripts/flash_ti.ps1`'s
header records four routes tried and the three that are dead.

## Two things `hal_ti` will not do for you

Both are link errors, both are loud, and both read as "hal_ti is broken" rather
than as what they are.

**`driverlib/rfc.c` and the RF patches are gated on other drivers' Kconfig
symbols.** `hal_ti`'s own `CMakeLists.txt` compiles `RFCC26X2_multiMode.c` —
the RF driver — unconditionally, and then compiles the `NOROM_RFC*` functions
that driver cannot link without only under `CONFIG_IEEE802154_CC13XX_CC26XX`,
`..._SUB_GHZ` or `CONFIG_BLE_CC13XX_CC26XX`. `rf_patch_cpe_prop.c` is compiled
by nothing at all. This spike's `CMakeLists.txt` adds both files itself rather
than patching the module, so the checkout stays a pristine copy of the pinned
revision.

**`PowerCC26X2_config` is missing, and Zephyr's version of it is worse than
missing.** It lives in `soc/ti/simplelink/cc13x2_cc26x2/power.c`, which that
SoC compiles only under `CONFIG_PM`, `CONFIG_PM_DEVICE` or `CONFIG_POWEROFF` —
so the first instinct is to switch one on, and this spike originally switched
on `CONFIG_POWEROFF` (rather than `PM`, which additionally lets the idle thread
enter standby and power down the debug interface).

**That was wrong, and the hardware said so on the first run.** The version
Zephyr compiles for anything that is not one of its three in-tree radio drivers
sets `.calibrateFxn = NULL`, under a comment reading "disable oscillator
calibration functionality for now" — and `PowerCC26X2.c:1357` dereferences that
pointer with no NULL check, the first time anything requests XOSC_HF, which is
the first `RF_open()`:

```
***** USAGE FAULT *****
Illegal use of the EPSR
Faulting instruction address (r15/pc): 0x00000000
r14/lr: 0x0000237d   -> switchXOSCHF, PowerCC26X2.c:1357
```

**An earlier draft of this file predicted the defect and got its severity
wrong**, and that is left on the record rather than quietly corrected: it said
an out-of-tree radio user "silently gets oscillator calibration disabled…
costs accuracy, not function, so nothing fails". It is not accuracy. Outside
Zephyr's three radio drivers the very first radio operation branches to address
zero. The general lesson survives — Zephyr's SoC layer here is written for its
own in-tree drivers and quietly degrades for anyone else — but the specific
"nothing fails" was a guess dressed as a finding.

So `CONFIG_POWEROFF` is now **off**, `power.c` is not compiled, and
[`src/ti_power_shim.c`](src/ti_power_shim.c) supplies the three symbols the RF
driver needs with `calibrateFxn` pointing at `PowerCC26XX_calibrate`. **P2
inherits this**: `radiant_radio_cc26xx.c` needs the same three symbols by the
same route, and it is not spike scaffolding.

## What a pass and a failure look like

A **pass** is the sweep summary showing a non-zero `crcOk` column at some
`rxBw`, and the channel ID printed from RAM matching `ant_scan.py`'s
independent ground truth. The program then transmits 40 frames, and
`ant_scan.py` pointed at the Feather should report the same channel ID back.

A **failure** is `NOTHING HEARD, ON ANY SETTING`, and the program prints the
five things to check in the order they should be checked, because they all fail
identically. The last of them — the MCE/RFE — is the one that would make this a
genuine no-go rather than a configuration error.

## What it measured

Bench of 2026-08-13, LAUNCHXL-CC26X2R1 receiving, Feather at roughly one metre.

**The PHY works, and the RX filter bandwidth matters more than the AA filter.**

| `rxBw` | kHz | `aaFilter` | frames | CRC ok |
|---|---|---|---|---|
| 97 | 1243.2 | 0xB | 12 | 11 |
| 97 | 1243.2 | 0xF | 4 | 4 |
| **98** | **1567.2** | **0xB** | **40** | **40** |
| 98 | 1567.2 | 0xF | 16 | 14 |
| 96 | 1092.5 | 0xB | 20 | 17 |
| 96 | 1092.5 | 0x5 | 12 | 10 |

**`rxBw = 98`, `aaFilter = 0xB` is the setting P2 should start from**: 40 of 40
CRC-valid, `nRxNok = 0`, and RSSI around −67 dBm where the narrower filters sat
ten to fifteen dB lower on the same link in the same minute. The sweep points
are not simultaneous and the ordering is confounded with drift, so read the
table as "98 is clearly good and 96 is clearly worse", not as a calibrated
comparison.

Carson's rule wanted ~1340 kHz and the table has no code there; the part
prefers the wider neighbour, which is the answer the pre-hardware note said it
could not predict.

**The channel ID came out of RAM, not out of an assumption.** Every good frame
decoded to `#52233`, with the three device types `ant_sim.py` was transmitting
(`0x0B` power, `0x11` fitness equipment, `0x7B` speed) — matching what a second
ANT stick independently reported. Software CRC over the 15 covered bytes agreed
with the two CRC bytes on the wire.

**Transmit works too**, and was witnessed rather than assumed. The spike sends
`#52233 / 0x0B / ttype 5`; the simulator's power meter is `ttype 165`. A scanner
on the second stick, patched to print transmission type, saw both at once:

```
found: #52233 / 0x0B / ttype 5   - power meter     <- the CC26x2
found: #52233 / 0x0B / ttype 165 - power meter     <- the Feather
found: #52233 / 0x7B / ttype 1   - speed           <- the Feather
```

An earlier attempt at this — killing `ant_sim.py` and scanning afterwards —
proved nothing, and the reason is worth writing down: **an ANT master keeps
broadcasting after the host process dies**, because the channel is open in the
stick, not in the program. Silence has to be arranged by closing the channel,
not by killing the process.

**The RAT.**

* 4 000 244 ticks in 1 000 ms — **4 MHz**, 250 ns resolution, as documented.
* Across five `RF_close()`/`RF_open()` cycles it advanced 200 337 850 ticks in
  the ~50 s those cycles took: **the RAT free-runs across RF-core power
  cycles**. The backend does not have to re-establish an epoch on every open.
* 32 bits at 4 MHz wraps every **1073 s ≈ 17.9 min**, so the 64-bit fold the
  HAL's `radiant_time_t` requires is P2's problem and cannot be skipped.

## What this spike does not measure

Named so their absence is a decision. Ramp-up, RX→TX turnaround, minimum arm
lead and the `t_sync` calibration constant are all P2/P3 work: they need the
absolute-start-time path (`TRIG_ABSTIME`) and the wired two-board trigger, and
none of them is a go/no-go. The RAT's rate and its behaviour across an RF-core
power cycle **are** measured here, because the sweep power-cycles the core six
times anyway and the 64-bit fold the backend owes the core depends on the
answer.

## What this means for P3, if it passes

`caps.max_filters == 2` is a **product regression on discovery time and it is
known in advance.** `radiant_search.c`'s policy enumerates `max_filters`
concrete addresses per window and sweeps enough sets to cover all 256 values of
`devnum_lo`: at 8 filters that is 32 sets and ~8.3 s worst case, and at 2
filters it is **128 sets**, which breaks
`[gates.acquisition] max_absolute_s = 5.0` by construction.

The plan's instruction on that is explicit and is repeated here so it is not
quietly disregarded: **record the measured discovery time, state the
regression, and decide separately whether the sweep needs a different strategy
at low filter counts. Do not tune the gate to fit the result.**
