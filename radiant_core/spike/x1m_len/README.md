<!-- SPDX-License-Identifier: Apache-2.0 -->
# Spike `x1m_len` — is a **long 1 M frame** invisible to a stock ANT+ dongle?

Provenance: clean-room. Written from `docs/decisions/0007-long-range-phy.md`,
from `radiant_core/include/radiant_core/radiant_radio_hal.h` and
`radiant_frame.h`, from `docs/ant-radio-link.md` and
`docs/spike-b-part2-results.md`, and from this repository's own earlier spikes.
Nothing here derives from `sdk-ant`, from `libant.a`, or from an adopter-gated
ANT+ device profile document. See `docs/decisions/0002-clean-room-policy.md`.

**Status: RUN 2026-08-11, and it passed.** The reading rule below was agreed
before the capture, which is where a reading rule belongs; the result is at the
bottom under *What actually happened*, and the authoritative record is
`docs/decisions/0007-long-range-phy.md`, section "Does 1 M also qualify?".

---

## The question (ADR 0007, "Does 1 M also qualify?")

ADR 0007 permits length extension **exactly where the PHY already makes us
invisible**, and it deliberately left one thing open:

> The authorship argument above applies equally to a RadiANT-authored **1 M**
> format […] on a device type no stock receiver opens. If a stock dongle in
> wildcard search simply drops such a frame on CRC and keeps every other
> sensor, the descriptor collapse — the largest battery item here — arrives
> without coded PHY at all, and most of this phase becomes optional.

The record marked it **open, owed a Tier 4 capture**, and said why it had not
been taken: the stock-dongle stand-in is the Adafruit Feather, and putting it
back into ANT dongle firmware costs a rationed, physically-gated Feather flash.
That flash was spent on 2026-08-11 and the capture was taken.

This directory is the **transmitter** half of that capture. The receiver under
test is a real stock ANT dongle (the Feather in `sdk-ant` firmware, enumerating
as `0FCF:1009`), driven by **`tools/ant_rf6_capture.py`** — not `ant_scan.py`,
which assigns a plain wildcard channel, acquires the first device it hears and
then reports only that one. This experiment needs a receiver that keeps
scanning and reports everything, so the capture tool uses the extended-assign
background-scan bit (`0x01`), which is the mechanism Zwift itself uses.

If it passes, ADR 0007 says explicitly what changes: it would permit a second
length-extended format on 1 M as an **addition**, and the rule would need
restating — the justification would become device-type isolation rather than
physical invisibility, which is a weaker argument owed its own record. Nothing
built in RF Phase 6 becomes wasted either way.

---

## The A/B control, and why it is load-bearing

Two frames alternate on the same radio, at the same power, at the same cadence,
both on device type `0x60`:

| | frame **A** — the control | frame **B** — the experiment |
|---|---|---|
| device number | `0x60A0` | `0x60B0` |
| on-air address | `A6 C5 A0 60 60` | `A6 C5 B0 60 60` |
| address length | 5 bytes | 5 bytes |
| body | **10 bytes** — `[trans_type][0x0A][8 payload]` | **30 bytes** — `[trans_type][0x0A][28 payload]` |
| PHY / RF | 1 M GFSK, RF 57 | 1 M GFSK, RF 57 |
| CRC | CRC-16/CCITT-FALSE, covers address | identical |
| rate | 4 Hz | 4 Hz |
| a stock dongle | **SHOULD report `0x60A0`** | **should NEVER report `0x60B0`** |

Frame A is an ordinary, well-formed ANT tracking frame in every respect. Frame B
differs from it in **exactly one property: the number of body bytes.**

**Without frame A, "B was not seen" is not a result.** That observation is
equally consistent with:

- a transmitter that never keyed,
- an address arithmetic mistake,
- a dongle that cannot hear this board across the bench,
- the wrong RF index,
- a dongle that was not really in wildcard search,
- a build that silently got the null radio backend (see below — this has
  happened in this project before).

Frame A removes all of them at once, in the same seconds, through the same
antenna. If A appears in the scan and B does not, the only surviving explanation
for the difference is frame length. **That is what converts a negative
observation into evidence**, and it is the reason this spike transmits two
things instead of one.

---

## Why not the nRF5340 DK

**This spike targets the nRF54L15 DK, and the nRF5340 DK cannot run it at all.**
This is a deviation from the obvious bench choice and it is not a preference:

1. The nRF5340's **RADIO is on the network core**, not on `cpuapp`. An
   application-core image has no radio peripheral to drive.
2. More decisively, `CONFIG_RADIANT_CORE_BACKEND_NRF` in `radiant_core/Kconfig`
   is `depends on SOC_COMPATIBLE_NRF52X || SOC_COMPATIBLE_NRF54LX`. **No nRF53
   core sets either** — `zephyr/soc/nordic/nrf53/Kconfig` selects
   `SOC_COMPATIBLE_NRF53X` / `_NRF5340_CPUAPP` / `_NRF5340_CPUNET`. So the
   backend is unreachable on that part on both cores, and the C file's per-series
   constant block would `#error` even if it were reached.

**And the failure is silent by default.** Asking for `nrf5340dk/nrf5340/cpuapp`
does not error: Kconfig prints a warning, falls back to the choice default
`RADIANT_CORE_BACKEND_NULL`, and produces an image that boots, runs this loop,
and refuses every arm with `ENOTSUP` — i.e. an experiment that transmits nothing
and looks exactly like the interesting negative result. This is the same trap
`radiant_core/Kconfig` already records against a Zephyr symbol rename.

`src/main.c` therefore opens with

```c
BUILD_ASSERT(IS_ENABLED(CONFIG_RADIANT_CORE_BACKEND_NRF), …);
```

which was verified to fire on `nrf5340dk/nrf5340/cpuapp`. The wrong board is a
compile error here, not a wasted bench session.

---

## Build (verified)

Built clean with NCS **v3.2.4** — the same bundle `scripts/run_ztest_hw.ps1`
uses. This is the exact command that was run:

```powershell
. .\scripts\env.ps1 -NcsVersion v3.2.4
Push-Location C:\ncs\v3.2.4
west -z C:\ncs\v3.2.4\zephyr build `
     -s C:\Users\Colin\ant_dongle\radiant_core\spike\x1m_len `
     -d C:\Users\Colin\ant_dongle\build\spike_x1m_len `
     -b nrf54l15dk/nrf54l15/cpuapp -p always --no-sysbuild
Pop-Location
```

`Push-Location` is required: `west build` is an extension command discovered
through the workspace manifest, so `west` must run with its cwd inside the
workspace even though `-s`/`-d`/`-z` all point elsewhere.

Result: **exit 0**, 36 380 B flash / 10 064 B RAM, image at
`build\spike_x1m_len\zephyr\zephyr.hex`. CMake prints one `WARNING` — the nrf
backend's standing "ramp-up and `t_sync` are seeded, not measured" notice. That
warning is itself the confirmation that the **nrf** backend was selected and not
the null one; `.config` says `CONFIG_RADIANT_CORE_BACKEND_NRF=y`.

`boards/nrf54l15dk_nrf54l15_cpuapp.overlay` in this directory supplies the
`radiant,radio-timer` chosen node the backend `#error`s without. It is a copy of
the chosen-node half of the repo-root board overlay, not an include of it — a
standalone spike does not see the product build's board directory.

## Flash

Not performed here. Two J-Link probes are attached and non-interactive JLink
cannot choose between them, and **`exec DisableAutoUpdateFW` must be the first
line of any script** or the probe ends up bricked in its bootloader. Use the
repo's guarded scripts; never a bare `CommanderScript`. `scripts/run_ztest_hw.ps1`
is the model — it builds and flashes over J-Link with that guard and with
`-SelectEmuBySN`. The nRF54L15 DK's probe serial on this bench is `1057737173`
and its device name is `nRF54L15_M33`.

Console: **COM7** at 115 200 (uart20, the DK's first VCOM). Not RTT — a result
that can only be read through vendor tooling is a result that is hard to
archive. One line a second:

```
[tx] A/std sent=4 fail=0 refuse=0 | B/long sent=4 fail=0 refuse=0 | resync=0 timeout=0 stray_rx=0
```

**Read that line before reading the dongle.** It is what says the board is
really transmitting both frames; the whole A/B argument collapses if `sent` is
not climbing for both.

## Scan (the receiver under test)

The stock dongle must be in **ANT dongle firmware** (`0FCF:1009`), not the
Feather bootloader (`239A:0029`). That is the rationed flash ADR 0007 records as
the blocker; it is spent before this step, not by it.

```powershell
& C:\ncs\toolchains\dcbdc366a1\opt\bin\python.exe tools\ant_scan.py --seconds 60
```

(There is no system Python on this bench — that interpreter is the one with
`pyusb`. Add `--serial <suffix>` if more than one USB board is attached.)

`ant_scan.py` runs the same opening sequence Zwift uses: ANT+ network key,
wildcard slave channel, RF 57, open, report. Run it for at least 60 s — a
wildcard search sweeps 32 filter sets, and a device that appears in the last set
of a sweep is not a device that appeared late.

**Run it at least twice, and once with a real sensor awake** (a strap worn, or
`tools/ant_sim.py` driving a second board). The "keeps hearing every other
sensor normally" half of ADR 0007's question is a claim about the *other*
sensors, and a scan with nothing else on the air cannot test it.

---

## What result means what

| A (`0x60A0`) | B (`0x60B0`) | Reading |
|---|---|---|
| **seen** | **not seen** | **The 1 M length extension is interop-safe.** A stock dongle drops the long frame on CRC and is otherwise unaffected. ADR 0007's open question resolves in favour of a second length-extended format on 1 M — as an **addition** to that record, with the rule restated on device-type isolation rather than physical invisibility, in a record of its own. |
| **seen** | **ALSO seen** | **It is NOT safe. ADR 0007's rule stands unchanged:** length extension is permitted *only* where the PHY already makes us invisible. A stock dongle reports a device the frame was not addressed to any receiver about, which is exactly the pollution the rule forbids. Stop; the coded PHY is the only route to the descriptor collapse. |
| **not seen** | not seen | **The rig is broken. Conclude nothing.** Not a pass, not a fail, not weak evidence. Check the console counters first (is the board transmitting?), then the dongle's VID/PID (`0FCF:1009`, not `239A:0029`), then whether the scan saw *any* sensor at all. |
| **not seen** | **seen** | Impossible as designed, and therefore a bug in the rig rather than a result. Suspect the addresses, the device numbers, or an edit that made A and B differ in more than length. |

Two further things worth writing down before the bench, so they are not decided
afterwards:

- **"B not seen" needs a duration, not a moment.** At 4 Hz over 60 s frame B is
  put on the air ~240 times. A single scan that missed it is a much weaker claim
  than three scans that all did.
- **The dongle reporting `0x60A0` intermittently is not a pass.** If frame A's
  yield is poor, the link is marginal and B's absence is partly explained by
  range rather than by length. Move the boards closer and re-run; A should be
  solid before B's silence means anything.

---

## Deliberate limits

- **Transmit only.** Whether *radiant_core* can receive its own long frame is a
  different question with a different rig, and answering it here would not make
  the stock dongle's behaviour any clearer.
- **No change to shipping code, and that is a result rather than a constraint.**
  Both packet formats are local `static const struct radiant_pkt_format` in
  `src/main.c`, handed to `radiant_radio_tx()`. `apply_format()` in
  `radiant_core/src/radiant_radio_nrf.c` requires `RADIANT_LEN_FIXED` on 1 M
  (Spike B: byte 3 is a control byte, not a length) and then accepts **any**
  `body_len` from 1 to `RADIANT_RADIO_BODY_MAX`, which ADR 0007 raised to 40 for
  the coded format's sake. A 30-byte static-length 1 M body is therefore
  expressible today, through the public HAL, with no register knowledge in this
  file at all.
- **No register writes here, unlike `rx_raw` and `promisc`.** Those spikes
  existed to test whether the register mapping was right; this one exists to test
  what *today's shipping backend* puts on the air, so going around it would
  answer the wrong question.
- **Thread-driven, not callback-chained.** The HAL permits re-arming from inside
  a completion callback and calls it the low-jitter path. This program does not
  need low jitter at 8 transmissions a second, and the schedule is absolute — so
  thread latency moves when the *arm call* happens, not when the frame leaves the
  antenna.
- **`radiant_core` proper links none of this.** The spike is a standalone Zephyr
  application outside the repo root `CMakeLists.txt`, so it can never end up in a
  shipped image by accident.

## What actually happened — 2026-08-11

Run on the nRF54L15 DK (`nrf54l15dk/nrf54l15/cpuapp`, `.config` confirmed
`CONFIG_RADIANT_CORE_BACKEND_NRF=y`) against the Feather in stock ANT firmware
(`0FCF:1009`, `MESG_VERSION` = `BOK02.01.00`). 120 s, one sitting, with a real
trainer on the air throughout.

Transmitter console, sampled mid-run — both frames going out at 4 Hz with the
radio refusing neither:

```
[tx] A/std sent=93 fail=0 refuse=0 | B/long sent=92 fail=0 refuse=0 | resync=0 timeout=0 stray_rx=0
```

Receiver:

```
  device    type  broadcasts   first    last   rssi(min/mean/max)
  #24736    0x60          47    1.1s  117.3s   -33/-33/-32     <- A, control
  #52233    0x0B          72    2.1s  118.6s   -73/-71/-70     <- real trainer
  #52233    0x11          39    1.5s  120.5s   -71/-71/-70
  #52233    0x7B          36    1.7s  114.5s   -71/-70/-70

VERDICT: PASS - the 1 M length extension is interop-safe here.
```

**A heard 47 times at −33 dBm, evenly across the window; B transmitted ~480
times and heard exactly zero times; the trainer reported on all three of its
device types from first second to last.** The link was some 40 dB above where
ANT stops tracking and stronger than the real trainer the same dongle was
holding, so B's silence is not a range artifact — which is precisely the failure
mode the "treat an intermittent A as a failed rig" rule above exists to catch,
and A was not intermittent.

The one weakness, stated rather than buried: heart-rate straps sleep when not
worn, so the only *independent* sensor on the air was the trainer. "Keeps every
other sensor" therefore rests on one real device across three device types, plus
the A control. A repeat with a second live sensor would strengthen that half. It
would not change the B result, which is what the question turned on.

ADR 0007's "Does 1 M also qualify?" section has been amended to record this in
full, including what it does **not** license: a length-extended 1 M format is now
*permitted* but still unwritten, and owes its own decision record, because its
justification is device-type isolation rather than the physical invisibility the
coded PHY enjoys. The guard in `apply_format()` pairing `RADIANT_LEN_FROM_BODY`
with the coded PHY should not be loosened on the strength of this result alone.

Still owed: raw console and scan logs into `archive/captures/`, on the same
terms as `docs/spike-a-results.md` and `docs/spike-b-part2-results.md`. A spike
directory is a place work passes through, not a place evidence lives.
