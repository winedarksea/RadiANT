<!-- SPDX-License-Identifier: Apache-2.0 -->
# Spike `phy_ladder` — the instrument for ADR 0007's owed sensitivity gate

Provenance: clean-room. Written from `docs/decisions/0007-long-range-phy.md`
(the gate, and the "Two limitations" section the third mode closes), from
`radiant_core/include/radiant_core/radiant_radio_hal.h` and
`radiant_frame.h`, from `tools/ab_gates.toml`'s header (the
`loss (exact)` rule), and from this repository's own spikes
`radiant_core/spike/x1m_len` and `radiant_core/spike/rx_raw`. Nothing here
derives from `sdk-ant`, from `libant.a`, or from an ANT+ device
profile document. See `docs/decisions/0002-clean-room-policy.md`.

**Status: BUILT for both boards, NOT FLASHED, NOT RUN.** The bench radio was in
use by another measurement when this was written and a second transmitter would
have corrupted it. Nothing below is a result.

---

## The question

`docs/decisions/0007-long-range-phy.md`, *Verification status*, lists what a
green test suite does **not** cover, and the first line of that table is the
gate this directory exists to make measurable:

> **The gate: ≥ 6 dB improvement in the 5 %-loss point for S=8 vs 1 M**
> (`tools/ant_sens.py`) — *Needs the sensitivity rig and a transmitter-power
> ladder*

Nothing has measured it. The record says so in as many words, and says a green
suite is not a met gate.

### Why this is firmware and not `tools/ant_sens.py`

`ant_sens.py` already runs exactly the right measurement — walk the master's
transmit power down until the receiver misses 5 % — but it drives boards over
the ANT serial protocol, and **no ANT serial message selects a PHY.** So the
ladder has to live where PHY selection lives: in an image that calls
`radiant_radio_tx()` with a `struct radiant_pkt_format`.

### The secondary objective, and why it is nearly free

ADR 0007 also records, under *Two limitations*, that the 2026-08-11 `x1m_len`
capture proved **harmlessness, not usability**:

> A frame that was malformed for some reason unrelated to its length would
> produce a byte-for-byte identical result. […] it does not answer "can a
> RadiANT receiver read a long 1 M frame", which has never been tested at all.
> […] **The ADR owed for a length-extended 1 M format must not be written until
> that has been done.**

`phy 1m-long` is that receiver. One run of it against the `x1m_len` transmitter
closes the gap.

---

## Method

**One application, two roles, selected at run time over the console.** Not two
applications and not a build-time switch: a rung of a ladder is one power
setting, and a twelve-rung ladder must not cost twelve flashes — one of the two
radios on this bench is the Feather, whose flashes are rationed and physically
gated (`docs/decisions/0007-long-range-phy.md`, and the `x1m_len` record).

### Console commands

| command | effect |
|---|---|
| `role tx` / `role rx` | pick the end. Refused while running. |
| `phy 1m` | `radiant_frame_format(RADIANT_FRAME_CFG_TRACKING)` — the comparison's baseline |
| `phy s8` | `radiant_frame_format(RADIANT_FRAME_CFG_LR)` — the comparison's subject |
| `phy 1m-long` | **RX only** — `x1m_len`'s frame B. The secondary objective. |
| `power <dbm>` | transmit power. Refused while running (see below). |
| `start` | reset counters and go. On TX this also **resets the sequence to 0**. |
| `stop` | abort, then print `stats` |
| `stats` | print the counters |
| `help` | reprint the banner (the Feather's USB console misses the boot one) |

There is no `rate` command and no way to change the cadence. Both PHYs run at
**20 Hz**, deliberately: the gate is a comparison, and collision exposure on a
bench with a characterised ~0.4 % floor scales with how often this program is on
the air. Same rate, same exposure, same floor on both arms. 20 Hz also sizes a
rung — 1,000 packets in 50 s resolves a 5 % point to about ±0.7 % (1σ
binomial), comfortably finer than the 6 dB the gate asks about.

### Counting loss without a clock

`tools/ab_gates.toml`'s header states the rule this project already learned the
hard way:

> Read `loss (exact)`, never the wall-clock line. The wall-clock figure divides
> elapsed time by the nominal period and assumes the transmitter's crystal is
> exact; `loss (exact)` counts what is missing from the transmitter's own event
> counter, with no clock involved.

So the transmitter puts an incrementing **32-bit sequence number** in the
payload and the receiver counts gaps in it:

```
expected = high_seq - first_seq + 1
received = frames that decoded and carried this program's marker
loss     = (expected - received) / expected
```

`stats` prints `first`, `high`, `expected`, `received` and `loss`, and prints
**no rate, no packets-per-second and no elapsed time**. There is nothing else in
the output the gate may be read from.

The transmitter's sequence advances even when an arm was **refused**. That is
deliberate: a frame the transmitter never keyed is a frame the receiver never
had a chance at, and it must appear in the gap count — otherwise a transmitter
defect would be invisible on the only line the gate is read from. The
`refused`/`failed`/`timeout` counters on the TX console are how an operator
tells that case from a link one, which is why **both consoles must be read**.

### The frame formats are the shipping ones

`radiant_frame_format()` supplies both formats under comparison, and
`radiant_frame_encode()` / `radiant_frame_lr_body()` compose both bodies. A
ladder run against a locally-declared format would measure a format nothing
ships, and the number would have to be re-measured the first time the shipping
one was used.

The one local format is `phy 1m-long`'s, and it is local because the thing it
must be byte-identical to is also local — `fmt_long` in
`radiant_core/spike/x1m_len/src/main.c`. That mode checks **all thirty body
bytes** against what `x1m_len` builds, not merely that something arrived: "a
frame arrived" is also what a matcher firing on noise produces, and this is the
one mode whose frame carries no sequence number to cross-check against.

### Channel

| | 1 M (`phy 1m`) | S=8 (`phy s8`) | `phy 1m-long` |
|---|---|---|---|
| RF index | 57 (2457 MHz) | 57 | 57 |
| device number | `0x60C0` | `0x60C0` | `0x60B0` (`x1m_len`'s frame B) |
| device type | `0x60` | `0x60` | `0x60` |
| on-air address | `A6 C5 C0 60 60` (5 B) | `A6 C5 C0 60` (4 B) | `A6 C5 B0 60 60` (5 B) |
| body | 10 B | 12 B | 30 B |
| payload | `[seq32 LE][0x01][A5 5A C3]` | `[seq32 LE][0x08][A5 5A C3]` | none — filler ramp |

`0x60C0` is deliberately neither of `x1m_len`'s numbers (`0x60A0`, `0x60B0`).
Both spikes may be on the bench at once — `phy 1m-long` exists precisely so that
they are — and a ladder whose 1 M receiver also matched `x1m_len`'s control
frame would be counting somebody else's packets into its own loss figure. The
three marker bytes are the second line of that defence: a frame that matched the
address but is not ours is counted as `alien` and never reaches the sequence
accounting.

---

## Why not the nRF5340 DK

**This spike targets the nRF54L15 DK and the Adafruit Feather nRF52840. The
nRF5340 DK cannot run it at all**, for the reasons `x1m_len/README.md` already
records: the nRF5340's RADIO is on the network core, and
`CONFIG_RADIANT_CORE_BACKEND_NRF` is `depends on SOC_COMPATIBLE_NRF52X ||
SOC_COMPATIBLE_NRF54LX`, which no nRF53 core sets.

**And the failure is silent.** Kconfig falls back to
`RADIANT_CORE_BACKEND_NULL`, and the image boots, accepts every console command
and refuses every arm with `ENOTSUP`. On a *ladder* that is worse than on
`x1m_len`: the null backend's receiver hears nothing, manufacturing **100 %
loss at every rung** — indistinguishable from a rig with too much attenuation.

`src/main.c` therefore opens with the same guard `x1m_len` does:

```c
BUILD_ASSERT(IS_ENABLED(CONFIG_RADIANT_CORE_BACKEND_NRF), …);
```

---

## Build (verified 2026-08-11, NCS v3.2.4, exit 0 for both boards)

```powershell
. .\scripts\env.ps1 -NcsVersion v3.2.4
Push-Location C:\ncs\v3.2.4

west -z C:\ncs\v3.2.4\zephyr build `
     -s C:\Users\Colin\ant_dongle\radiant_core\spike\phy_ladder `
     -d C:\Users\Colin\ant_dongle\build\spike_phy_ladder_54l15 `
     -b nrf54l15dk/nrf54l15/cpuapp -p always --no-sysbuild

west -z C:\ncs\v3.2.4\zephyr build `
     -s C:\Users\Colin\ant_dongle\radiant_core\spike\phy_ladder `
     -d C:\Users\Colin\ant_dongle\build\spike_phy_ladder_feather `
     -b adafruit_feather_nrf52840/nrf52840/uf2 -p always --no-sysbuild

Pop-Location
```

`Push-Location` is required: `west build` is an extension command discovered
through the workspace manifest, so `west` must run with its cwd inside the
workspace even though `-s`/`-d`/`-z` all point elsewhere.

| board | flash | RAM | artefact |
|---|---|---|---|
| `nrf54l15dk/nrf54l15/cpuapp` | 43 708 B (2.99 %) | 12 440 B (6.46 %) | `build\spike_phy_ladder_54l15\zephyr\zephyr.hex` |
| `adafruit_feather_nrf52840/nrf52840/uf2` | 59 484 B (7.33 %) | 21 952 B (8.37 %) | `build\spike_phy_ladder_feather\zephyr\zephyr.uf2`, start `0x26000` |

Both `.config` files say `CONFIG_RADIANT_CORE_BACKEND_NRF=y` — **check this
before believing any run.** CMake prints one `WARNING` in each build: the nrf
backend's standing "ramp-up and `t_sync` are seeded, not measured" notice. That
warning is itself the confirmation that the **nrf** backend was selected and not
the null one.

### The two consoles

**nRF54L15 DK — UART VCOM**, 115 200, the board's default `uart20` (COM7 on this
bench). `boards/nrf54l15dk_nrf54l15_cpuapp.overlay` in this directory supplies
the `radiant,radio-timer` chosen node the backend `#error`s without; it is a
copy of the chosen-node half of the repo-root board overlay, not an include of
it, because a standalone spike does not see the product build's board directory.

**Feather — USB CDC ACM.** This board has no debugger and no usable UART on this
bench, and this program's console carries its *input* as well as its output, so
RTT (what the product build uses) would make every rung depend on vendor tooling
being attached. Getting the console off the Feather is not a convenience: the
Feather is the only other radiant-capable radio here, so **one end of every S=8
link must be it**, and a spike that cannot report from the Feather cannot
measure the gate at all.

Almost nothing was needed for that. The `adafruit_feather_nrf52840/nrf52840/uf2`
board target already includes `boards/common/usb/cdc_acm_serial.dtsi` (which
creates `board_cdc_acm_uart` and points `zephyr,console` at it) and sources
`Kconfig.cdc_acm_serial.defconfig` (which defaults `USB_DEVICE_STACK_NEXT`,
`CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` and `CDC_ACM_SERIAL_ENABLE_AT_BOOT` to `y`,
so `usbd_init()`/`usbd_enable()` run from `SYS_INIT` with no application code).
The *product* build turns all of that off — see the repo-root
`boards/adafruit_feather_nrf52840_nrf52840_uf2.conf` and `.overlay` — because
the product's USB is a vendor class impersonating an ANT dongle. This spike is
the opposite case, so `boards/adafruit_feather_nrf52840_nrf52840_uf2.conf` here
sets `CONFIG_BOARD_SERIAL_BACKEND_CDC_ACM=y` explicitly rather than relying on
its absence, and `CONFIG_USE_DT_CODE_PARTITION=y` so the image links at the
`0x26000` the UF2 bootloader expects.

It enumerates as a **generic CDC ACM port, VID `2FE3` PID `0004`** (Zephyr's
defaults) — *not* `0FCF:1009`. Any COM terminal at any baud will do; the rate is
ignored.

## Flash

> ### ⚠ BLOCKING DEFECT, found on first contact with hardware 2026-08-11
>
> **The Feather build's USB console does not work. Do not flash this image to a
> Feather again until it is fixed** — recovering the board costs a physical
> double-tap of RESET, which is the rationed resource this whole spike is
> supposed to economise.
>
> Observed, with the image built by the command below flashed to the Feather:
>
> - the CDC ACM port **enumerates** (`VID 2FE3 / PID 0004`, appeared as `COM4`),
>   so USB comes up and the application is running at least that far;
> - the port **emits nothing at all** — 0 bytes over a 6 s listen with DTR and
>   RTS asserted, so not even the banner;
> - **writes to it time out** (`SerialTimeoutException`), i.e. the device is not
>   draining the bulk-OUT endpoint. Without `write_timeout` set, the host blocks
>   forever — that is what it looks like first.
>
> **The same image on the nRF54L15 DK is completely healthy** and prints its
> banner and `help` on the UART VCOM (COM7) immediately. So the spike's logic,
> its command parser and its build are fine; what fails is specifically the
> Feather's USB-console binding.
>
> **The board cannot be recovered from software.** A 1200-baud touch on the CDC
> port — the Arduino/Adafruit reset convention — does **not** work here; Zephyr
> does not implement it, and the port simply stays up. `FTHR840BOOT` never
> appears. Only a physical double-tap of RESET recovers the board.
>
> This is the exact failure `hardware-bench` warns about: the Feather has no
> debugger, so anything that fails at or before the console is bound is
> invisible on it. **Whatever the fix is, prove it on a board with a debugger
> first** — or add a heartbeat that is observable without the console (an LED),
> so "did it boot" and "is the console bound" stop being the same question.
>
> Suspects, in the order worth checking: the app polling the console before the
> USB stack has finished configuring; a missing wait on DTR where the CDC
> device only starts servicing endpoints once the host raises it; and console
> output being routed to a `chosen` node that is not the enumerated CDC
> instance. The generated DTS was confirmed to say
> `zephyr,console = &board_cdc_acm_uart`, so the third is the least likely and
> the first two are not distinguishable from the outside.

**Not performed by the author of this spike, deliberately: `west flash` / JLink
was not run at all** while it was being written. The bench radio was in use by
another measurement and a second transmitter would have corrupted it.

When it is done: two J-Link probes are attached and non-interactive JLink cannot
choose between them, and **`exec DisableAutoUpdateFW` must be the first line of
any script** or the probe ends up bricked in its bootloader. Use the repo's
guarded scripts; never a bare `CommanderScript`. `scripts/run_ztest_hw.ps1` is
the model. The Feather takes the `.uf2` by drag-and-drop onto `FTHR840BOOT`
(double-tap reset), which costs no J-Link at all — but it *does* cost a Feather
flash, and the ANT dongle firmware has to be put back afterwards.

---

## How to read a result

### Running one rung

At **every** rung, in this order:

1. **TX board:** `stop`
2. **TX board:** `power <dbm>`
3. **TX board:** `start`  ← the sequence resets to 0 here
4. **RX board:** `start`  ← counters reset here
5. wait for `expected` to pass ~1000 (about a minute at 20 Hz)
6. **RX board:** `stats`, and **TX board:** `stats`

**The order matters and is not a style preference.** `start` on the TX resets
the sequence; a receiver already running when that happens sees the sequence go
backwards and every number after it is nonsense. That is not silently repaired —
it is counted, and `stats` prints a `*** VOID ***` line if it ever happened.

`power` is refused while running, for the same class of reason: a rung whose
power changed halfway is a loss figure averaged over two powers, and nothing on
the console afterwards would show that it had happened.

### Reading the gate

Walk the power down on 1 M until loss crosses 5 %, then repeat with `phy s8` on
**both** boards, **in the same sitting, without moving anything.**
`docs/testing.md`'s A/B/A rule applies with full force here: the run-to-run
spread on this bench is comparable to the effect being measured, and a 1 M
number taken today against an S=8 number taken last week measures the room.

> **gate: (5 %-loss power on 1 M) − (5 %-loss power on S=8) ≥ 6 dB**

Interpolate between the two rungs that straddle 5 %; do not report the nearest
rung as the crossing.

If 5 % is not reached at `-40 dBm` — the backend's floor — the rig has too
little path loss. **Add attenuation or separate the boards; do not report a
result from a ladder that never crossed.** The gate is a *difference* of two
crossings, so the absolute level does not matter as long as both arms see the
same rig.

### `phy 1m-long`

Put the `x1m_len` spike on the other board (it transmits frames A and B at 4 Hz
unconditionally), select `role rx` / `phy 1m-long` here, and `start`.

> **readable if `exact > 0` and `wrong_bytes == 0`.**

`stats` prints no loss figure in this mode and that is deliberate: frame B
carries no sequence number, so there is nothing to count gaps in, and a
percentage here would be a wall-clock number in disguise. The claim ADR 0007 is
owed is binary — a `radiant_core` receiver read the frame, byte for byte, or it
did not.

### What makes a run VOID

Any one of these and the numbers are not a result:

- **`*** VOID: the sequence went BACKWARDS ***` on the RX console.** The
  transmitter restarted mid-run. Redo the rung in the documented order.
- **`.config` does not say `CONFIG_RADIANT_CORE_BACKEND_NRF=y`.** The null
  backend reports 100 % loss at every rung and looks like an attenuation
  problem.
- **The TX console shows `refused` or `failed` climbing.** Those frames never
  reached the air; the loss figure includes them, but it is then measuring the
  transmitter, not the link. A single `ARM REFUSED rc=-2` (`ENOTSUP`) on
  `phy s8` means this board's backend has no coded PHY and the comparison
  cannot be made here at all.
- **`alien` or `decode_fail` climbing on the RX console.** Something else is on
  this address, or the two ends disagree about the format. Either way the
  `received` count is not counting what it claims to.
- **The two arms were not measured in one sitting, on the same rig, without
  anything being moved.** Not a weak result — not a result.
- **`rearm_fail` non-zero and climbing.** The receiver's window chain broke and
  there is dead air the loss figure is charging to the link.
- **A rung with fewer than a few hundred `expected`.** At 200 packets a 5 %
  point is ±1.5 % (1σ), which is wider than the rung spacing a 6 dB claim needs.

---

## Deliberate limits, and what will need watching on first contact

- **The receive duty cycle is not 100 %.** Windows are 1 s long and are re-armed
  from the terminal event — the low-jitter path `radiant_radio_hal.h` describes,
  chosen here because a gap between windows is loss this *instrument* invents
  rather than loss the link caused. The residual gap is the backend's arm path,
  order 10⁻⁴ of the run and far below the 5 % point, but **it is not zero** and
  it biases every measurement in the same direction. It cancels out of the
  *difference* of two crossings, which is what the gate is.
- **nRF52840 errata 191 is not applied by `radiant_core`.** The nRF52840's RADIO
  has a documented erratum affecting the coded (Long Range) PHY, and Zephyr's
  own BLE controller works around it with a register write on every mode change
  (`radio_nrf52840.h`, `hal_radio_phy_mode_get()`). `radiant_radio_nrf.c`'s
  `apply_format()` does not, and this spike does not either — going around the
  shipping backend is exactly what `x1m_len` refused to do, and a spike that
  quietly applied a workaround the product lacks would report a sensitivity the
  product does not have. **Every S=8 arm on this bench has the Feather at one
  end**, so this is expected to cost real dB on the S=8 side and to make any
  measured improvement a *lower bound*. If the gate comes out at, say, 4 dB, the
  first thing to try is the same run with the S=8 arms swapped, and the second
  is the erratum. This is a shipping-code question, recorded here rather than
  fixed here.
- **`t_sync` is seeded, not measured, on both PHYs** — and the coded constant
  (−30 µs) is three times the 1 M one and no better established (ADR 0007). A
  constant `t_sync` error does not affect this measurement, because the receive
  windows are a second long and the error is tens of microseconds; it is listed
  so nobody reaches for it as an explanation of a result it cannot produce.
- **Transmit only walks `power.dbm`.** The HAL's `use_raw` escape hatch is not
  exposed. A backend clamps rather than fails, so `power -60` silently becomes
  −40 and produces a duplicate rung; the console says so when the value is
  outside `caps.tx_power_min_dbm .. tx_power_max_dbm`.
- **No change to shipping code.** Nothing under `radiant_core/src/`,
  `radiant_core/include/`, `src/` or any Kconfig was touched; this is a new
  directory only. Both packet formats under comparison come from
  `radiant_frame_format()` and the only local format is the one that mirrors
  another spike's local one.
- **`radiant_core` proper links none of this.** The spike is a standalone Zephyr
  application outside the repo root `CMakeLists.txt`, so it can never end up in
  a shipped image by accident.
- **A spike directory is a place work passes through.** When this is run, the
  raw console logs from both boards belong in `archive/captures/`, on the same
  terms as `docs/spike-a-results.md` and `docs/spike-b-part2-results.md`, and
  the result belongs in `docs/decisions/0007-long-range-phy.md`'s
  *Verification status* table — not here.
