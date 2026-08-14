<!-- SPDX-License-Identifier: Apache-2.0 -->
# Spike B - `promisc`

Checked by: `spike_b_analyse.py`, which parses this program's output and exits
non-zero if the capture cannot be shown to be complete. The gate is

```powershell
C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe radiant\spike\promisc\spike_b_analyse.py `
    archive\captures\radio\2026-08-09-spike-b2-run{A,B,C}*.log --strict
```

with the results in `docs/spike-b-results.md` (part 1) and
`docs/spike-b-part2-results.md` (part 2). If the prose and the captures
disagree, the captures are right.

Provenance: clean-room. Written from `docs/ant-radio-link.md`,
`docs/spike-a-results.md`, this repository's own `radiant/spike/rx_raw`, the
public Nordic MDK headers and product specifications, and Zephyr's public
clock-control and IRQ APIs. The host script additionally uses the free *ANT
Message Protocol and Usage Rev 5.1* for the meaning of the messages it sends.
Nothing here derives from `sdk-ant`, from `libant.a`, or from an
ANT+ device profile document. See `docs/decisions/0002-clean-room-policy.md`.

## What it is for

The 18-byte ANT frame accounts for every byte, and **none of them is a message
type**. `docs/ant-radio-link.md` calls that the single most important unknown in
the public record, and states the likely resolution - that the byte rtl_433
calls "length" is really a control byte - as a guess.

This spike settles it. It is four programs:

| | |
|---|---|
| `src/main.c` | A promiscuous receiver. Puts the whole ANT body in RAM as opaque bytes and prints byte 3 of every frame from a known device, with microsecond timestamps and a ring-drop counter. |
| `appcore/` | The nRF5340 application-core companion. Two register writes that hand the network core its console pins, plus a heartbeat. Without it the capture runs on the network core and has nowhere to print. |
| `spike_b_drive.py` | A host script that makes a board transmit broadcast, then acknowledged data, then a burst, with every payload marked so a captured frame traces back to a line of its output. `--role slave` is the half that needed a third radio. |
| `spike_b_analyse.py` | Turns a capture into the control-byte table, and refuses to if `dr=` ever increments. |

**The answer is that byte 3 is a control byte, and it is six fields**: bit 7
"part of an acknowledged exchange", bit 6 "this is the acknowledgement", bit 5
"last packet", bit 4 a one-bit alternating burst sequence, bit 3 "this frame
opens the channel slot", bits 2:0 constant `010`. There is **no three-bit burst
sequence number** and acknowledged data is byte-for-byte a one-packet burst.

`docs/spike-b-results.md` is part 1 - the broadcast/acknowledged half, from two
radios. `docs/spike-b-part2-results.md` is part 2 - the burst half, the reply
frame and the turnaround, from three - and it corrects two of part 1's
inferences.

## Why it does not reuse Spike A's tracking configuration

Spike A's 5-byte-address configuration parses byte 3 with `LFLEN=8` - it has
already decided the byte is a length, so it can't report otherwise: a frame
whose byte 3 read `0x8A` would be taken as a 138-byte body, overrun, fail its
CRC, and look like interference.

So this uses the *search* configuration Spike A measured instead - `PCNF0 = 0`,
`BALEN = 2`, `STATLEN = 12` - which treats the body as opaque bytes and puts
byte 3 in RAM whatever it means. That is the one design decision in this
spike that had to be right in advance.

The consequence is a fixed body length. A frame carrying a *different* payload
length is still received - the header still reads correctly - but its CRC fails,
because the radio takes the wrong number of bytes off the air. `src/main.c`
prints those separately rather than discarding them, under the name
`longframe`; none occurred.

## Running it

Two boards for the part that works, three for the part that does not. See
*The rig that could not be built* in `docs/spike-b-results.md`.

**Capture: the nRF54L15 DK**, which flashes unattended over J-Link.

```powershell
. .\scripts\env.ps1 -NcsVersion v3.2.4
Push-Location C:\ncs\v3.2.4
west -z C:\ncs\v3.2.4\zephyr build -s C:\Users\Colin\ant_dongle\radiant\spike\promisc `
     -d C:\Users\Colin\ant_dongle\build\promisc_l15 -b nrf54l15dk/nrf54l15/cpuapp -p always
Pop-Location
.\scripts\flash_sim_jlink.ps1 -HexPath build\promisc_l15\merged.hex -JLinkExe <shim>
```

Output is on **COM7** at 115200. The J-Link notes in
`radiant/spike/rx_raw/README.md` apply unchanged and all of them still matter:
two probes are attached so `-SelectEmuBySN` is mandatory,
`exec DisableAutoUpdateFW` must be the first line, and the flash script must
never be piped into `Select-Object -First N`.

**Transmit: the Feather**, already flashed with the shipping dongle firmware, so
this costs **no Feather flash**:

```powershell
& C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe radiant\spike\promisc\spike_b_drive.py `
    --role master --serial 6183 --broadcast-secs 20 --acks 4 --burst-plan 1,12
```

`--role slave` is the other half and is what the three-radio rig needs: it
tracks a master and sends acknowledged data and a burst *back*, which is the
direction Zwift uses to set trainer resistance and the only one that yields a
reply turnaround.

### The three-radio rig, which is what part 2 actually used

| Role | Board | Image |
|---|---|---|
| Master | nRF54L15 DK | `sim/`, device 14871 / type `0x0B` / transmission type 5, period 8182 |
| Slave | Feather nRF52840 | the shipping dongle firmware, driven by `spike_b_drive.py --role slave` |
| Sniffer | nRF5340 DK, **both cores** | `promisc` on `cpunet`, `promisc/appcore` on `cpuapp` |

```powershell
. .\scripts\env.ps1 -NcsVersion v3.2.4
Push-Location C:\ncs\v3.2.4
west -z C:\ncs\v3.2.4\zephyr build -s <repo>\radiant\spike\promisc `
     -d <repo>\build\promisc_net2 -b nrf5340dk/nrf5340/cpunet -p always
west -z C:\ncs\v3.2.4\zephyr build -s <repo>\radiant\spike\promisc\appcore `
     -d <repo>\build\promisc_appcore2 -b nrf5340dk/nrf5340/cpuapp -p always
Pop-Location

.\scripts\flash_sim_jlink.ps1 -HexPath build\promisc_net2\merged.hex `
    -JLinkExe <shim -SelectEmuBySN 1050006310> -Device nRF5340_xxAA_NET
.\scripts\flash_sim_jlink.ps1 -HexPath build\promisc_appcore2\merged.hex `
    -JLinkExe <same shim> -Device nRF5340_xxAA_APP
```

Then the capture is on the network core's VCOM - **COM10** on this bench - and
the application core's heartbeat is on **COM9**. Having both is the point: if
COM10 is silent, COM9 says whether the board is alive at all, which is the
question part 1 could not answer and spent a day on.

Two things stand between a network-core image and its console and both are the
application core's job: `RESET.NETWORK.FORCEOFF` holds the network core until an
application-core image clears it (`CONFIG_SOC_NRF53_CPUNET_ENABLE=y`), and every
GPIO belongs to the application core until it writes
`PIN_CNF[n].MCUSEL = NetworkMCU`. `appcore/` does both, and grants **only** the
two console pins - unlike `nrf/samples/nrf5340/empty_app_core`, which grants all
of them and then powers off application-core RAM, taking the second opinion away
with it.

`net_rtt.conf` is the fallback if the VCOM stays silent: RTT lives in
network-core RAM and needs no pin from anybody, so it separates "the capture
program is not running" from "the pins were never granted". It builds clean
(`build/promisc_net_rtt`, 22,988 B flash) and has never been flashed, because
the VCOM route worked first time once `appcore/` existed.

```powershell
west ... -b nrf5340dk/nrf5340/cpunet -- -DEXTRA_CONF_FILE=net_rtt.conf
JLinkRTTLogger -Device nRF5340_xxAA_NET -If SWD -Speed 4000 `
               -SelectEmuBySN 1050006310 0 <outfile>
```

## Reading the output

```
[cfg] BALEN=2 BASE0=BASE1=0xA3650000 PREFIX0=0x22CC4488 PREFIX1=0x11EEE8AA
[cfg] RXADDRESSES=0xFF PCNF0=0x00000000 PCNF1=0x01020C1C CRCCNF=0x00000002
[cfg] on-air prefixes (slot:byte): 0:11 1:22 2:33 3:44 4:55 5:17* 6:77 7:88
P n=1914 dr=0 q=1 t=0419780355 dt=   +2194 OK m=5 rssi=-34 dn=14871 ty=0B tt=5 B3=82 pl=C0005AA500112233 crc=B467 air=111
S t=... ours=1735 foreign=0 longframe=2 oddtype=0 crc_ok=1735 crc_err=182 addr=1917 end=1917 dropped=0  B3: 0A=1672 82=3 ...
```

- `B3` is the field under investigation. The `S` line's histogram of it is the
  result in one line, and `spike_b_analyse.py` decodes it into fields.
- `dr` is the number of frames the ring has lost, as it stood when this record
  was made, and it is the only thing that makes a *sequence* result
  trustworthy - a frame dropped in the middle of a burst reads as an encoding
  that skips a value. `spike_b_analyse.py --strict` fails a run in which it ever
  increments. `q` is ring occupancy at enqueue, so headroom is reported rather
  than assumed; it peaked at 88 of 512 on the busiest capture.
- `n` counts every END event including noise through the eight matchers, so gaps
  in the printed `n` are expected and are **not** the completeness check. It is
  there to catch two captures accidentally concatenated.
- A `!` after `ty` means the device type or transmission type was not the
  expected pair. A frame is now judged "ours" on its device number alone,
  because requiring the type bytes as well would have silently discarded a reply
  frame whose layout is the thing under test.
- `pl` byte 0 says which host message produced the frame - `B0` broadcast, `A0`
  acknowledged, `C0` burst - and byte 1 which one of them. That is what makes
  the mapping from host action to on-air frame an identity rather than a timing
  argument.
- `m` is `RADIO->RXMATCH`, and it is not decoration: eight logical addresses are
  armed with the real `devnum_lo` at slot 5 and seven decoys, so `m=5` on every
  frame is a live test of two things `docs/ant-radio-link.md` still marks
  `[inferred]` - that eight filters can share one window, and that `RXMATCH`
  recovers `devnum_lo`.
- `dt` is the gap to this device's previous frame, and is where the master's
  slot period and jitter are read from.
- `air` is ADDRESS-to-END. It runs about 15 us short of the 112 us the frame
  takes on the air because the ADDRESS interrupt wakes an idle CPU and the END
  interrupt does not. Differences between `dt` values are unaffected, because
  both ends of a `dt` carry the same wake-up cost; absolute `t` values are not,
  and nothing is read from them.
- CRC-failing frames are counted, not printed, unless their channel ID still
  reads correctly - the `longframe` case. With three matched address bytes and
  eight filters the matcher fires on noise of order once a second on this bench,
  and printing that would bury the signal.

## Deliberate limits

- **Interrupt-driven, unlike Spike A.** A burst arrives faster than a 115200
  console can drain it, so the ISR writes fixed-size records into a ring and
  `main()` prints from it. `dropped` in the `S` line is what says whether the
  ring ever lost one; it was zero in every run.
- **Software timestamps, not (D)PPI.** Interrupt entry adds a small offset. Every
  figure this spike reports is a difference between two timestamps, where that
  offset cancels, and the one place it does not cancel - `air` - is called out
  above rather than quietly reported.
- **Receive only.** This spike never transmits. That is why it needs a third
  radio: making the capture board reply to the master would mean knowing what an
  ANT reply frame looks like, which was the question, not the method. Part 2
  answered it with three radios instead.
- **One fixed body length.** See above. No frame with a payload other than eight
  bytes has ever appeared on this bench, in either part, so `STATLEN = 12` has
  never been the wrong choice - and that is itself a result rather than luck.
- **`radiant` proper links none of this.** A standalone Zephyr application
  outside the repo root `CMakeLists.txt`, so it cannot reach a shipped image.

## Captures

`archive/captures/radio/2026-08-09-spike-b-nrf54l15-run{1..6}*.log` (part 1) and
`archive/captures/radio/2026-08-09-spike-b2-run{0,A,B,C}*.log` (part 2), each
with a header saying what the host was asked to do while it was recording.
Run 0 is a failure kept on purpose: it is the only capture of what a receiver
does when a burst stops mid-transfer.
