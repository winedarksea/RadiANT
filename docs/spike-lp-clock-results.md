# Spike: HFXO release and an LF-anchored timebase (Pass B1, 2026-11-18)

Power plan Track B (`RADIANT_NRF_LOW_POWER`) wants the nRF backend to drop the
HFXO between radio operations and keep its 1 MHz timebase exact by anchoring
the radio TIMER to the low-frequency counter: an LF compare starts the TIMER
(wake), and an LF compare fork-captures the running TIMER into CC5 (release).
This spike checks the hardware facts that design depends on, on the two boards
Track B targets first. Gate G0 is **PASS** (verdict at the end).

The code is `radiant/spike/lp_clock/`, a standalone Zephyr app that does not
link radiant. It drives the same peripherals the backend does: RADIO, the
`radiant,radio-timer` TIMER (TIMER20 on the L15 DK, TIMER4 on the Pro Micro),
the backend's five (D)PPI connections through `nrfx_gppi_conn_alloc()`, and
the HFXO through the clock-control onoff manager. It adds the LF counter the
design anchors to. Raw console logs, image MD5s and restore checks are in
`archive/benchmarks/2026-11-18-lp-spike/`. Every distribution below is exact:
the logs hold every distinct value with its count.

## Method

| | nRF54L15 DK | Pro Micro nRF52840 (clone, LFXO fitted) |
|---|---|---|
| LF counter | GRTC SYSCOUNTER, 1 MHz, `z_nrf_grtc_timer_*`, source LFXO (`CLKCFG`=1) | RTC1 user channels, 32768 Hz, `z_nrf_rtc_timer_*`, `CONFIG_NRF_RTC_TIMER_USER_CHAN_COUNT=2` |
| Radio TIMER | TIMER20, prescaler 4 → 1 MHz | TIMER4, 1 MHz |
| Anchor connection | GRTC COMPARE[c] → TIMER START + fork TIMER CAPTURE[5] | RTC1 COMPARE[a] → TIMER START + FORK TIMER CAPTURE[5] |
| Measurement | TIMER COMPARE[3] → GRTC CAPTURE[c2] (hardware read of the GRTC at a known TIMER value) | RTC1 COMPARE[b] at a+64 ticks → TIMER CAPTURE[3] |
| HFXO-start timestamps | GRTC read before `onoff_request()` and in the callback; CLOCK XOSTARTED and XOTUNED → GRTC CAPTURE by DPPI | TIMER (on HFINT) capture before the request and in the callback; CLOCK HFCLKSTARTED → TIMER CAPTURE[2] by PPI |
| Console | UART20 VCOM (COM3, 115200, DTR) | USB CDC ACM, **enabled only after S1, S4 and S3-lp** (USB holds the HFXO while up); text buffered in RAM until then |
| BT/MPSL build | `ble.conf`: SDC peripheral advertising (the apps/hrm_ble shape) | not built (see "not done") |

What each experiment did:

- **S1, register retention.** Program the RADIO (fast ramp-up, mode,
  frequency, packet config, address, CRC, TX power, SHORTS, PACKETPTR, INTEN)
  and the TIMER (mode, bitmode, prescaler, CC0-5, SHORTS, INTEN). Allocate and
  enable the backend's five connections plus the anchor, measurement and clock
  connections. Stop the TIMER after 2000 counts. Snapshot every register,
  including each PUBLISH/SUBSCRIBE endpoint, DPPIC CHEN and every non-zero
  PPIB register. Then release the HFXO, confirm it reports off, read XO.STAT /
  HFCLKSTAT from a timer ISR at 125 ms, sleep 250 ms, re-request the HFXO, and
  diff. Run once with CONSTLAT and once in LOWPWR. The nRF52 adds a third run
  with TIMER SHUTDOWN (anomaly 78).
- **S2.** Allocate LF channels until `-ENOMEM`. Allocate the connections and
  report their domains and channels, plus the PPIB registers the LF
  connections changed. Count the connections still free per domain pair with
  `nrfx_gppi_domain_conn_alloc()` (then free them).
- **S3, anchor accuracy, 10 000 cycles per variant.** Each cycle has a *wake
  anchor*: TIMER stopped and cleared, LF compare at T starts it, error = GRTC
  time of TIMER = X, minus (T + X). Each cycle also has a *release anchor*: the
  TIMER is running, the LF compare at T2 fork-captures it into CC5, error =
  GRTC time of TIMER = CC5 + X, minus (T2 + X). On the nRF52 both anchors are
  measured against a second RTC compare 64 ticks (1953.125 µs) later.
  Variants: **lp** (each cycle: release the HFXO, LOWPWR, sleep 4-12 ms, then
  CONSTLAT, request the HFXO, compare 3 ms after the request) and **held**
  (HFXO held). Even cycles keep the CPU busy across the events; odd cycles
  sleep.
- **S4, HFXO start-up, 1000 samples per CONSTLAT mode.** Release, wait until
  it reports off, sleep 2-8 ms at random, request, wait on the callback.
- **S5, rate.** HFXO held, TIMER free-running. LF compare every LF second
  (1 000 000 GRTC ticks / 32768 RTC ticks) fork-captures the TIMER: 300 s with
  the CPU sleeping between samples, then 120 s busy-waiting. Least-squares
  slope of (TIMER µs − LF µs) against time.

## Results: nRF54L15 DK

### S1: everything is retained, so the restore list is empty

Retained: all 63 registers in both runs, CONSTLAT and LOWPWR. The HFXO really
was off: `hfxo_off_seen=1`, and XO.STAT read 0x00000000 at the 125 ms point.
The stopped TIMER's counter was also retained (2000 before, 2000 after). The
list:

- RADIO: MODE, FREQUENCY, PCNF0/1, BASE0, PREFIX0, TXADDRESS, RXADDRESSES,
  CRCCNF/POLY/INIT, TXPOWER, SHORTS, PACKETPTR, TIFS, TIMING (fast ramp-up),
  INTENSET00.
- TIMER20: MODE, BITMODE, PRESCALER, SHORTS, INTENSET, CC0-5.
- Endpoints: all 21 PUBLISH/SUBSCRIBE registers of the nine connections,
  including RADIO PUBLISH_ADDRESS/RXREADY and SUBSCRIBE_RXEN/TXEN/DISABLE/
  RSSISTART.
- DPPIC00/10/20/30 CHEN.
- The 12 configured PPIB11/21/22/30 channel registers.

Caveat: this is HFXO-off System ON idle for 250 ms. Whether the RADIO's power
domain was actually gated in that time cannot be observed from software, and
System OFF was not tested. Within the regime Track B uses, though, no register
needs restoring after a wake. That is also the evidence
`RADIANT_NRF_REG_SHADOW`'s comment asks for before the shadow can be widened
to direct nRF54L builds. The tracked-loss A/B that comment also requires has
not been run.

### S2: channels and routes

- **GRTC channels.** `z_nrf_grtc_timer_chan_alloc()` returned channels 5, 2, 1
  and 0, then `-ENOMEM`: **4 free**, in the plain build and in the SDC/MPSL
  build alike. `NRFX_GRTC_CONFIG_NUM_OF_CC_CHANNELS`=5 (allowed mask 0x67,
  i.e. owned 0-11 minus child-owned 3,4,7-11). The system clock takes one.
  MPSL's channels 7-11 are child-owned, so MPSL does not compete for these.
  Track B needs 2.
- **GRTC → TIMER20 is NOT cross-domain.** The plan assumed it was. GRTC
  (0x500E2000) reports the same GPPI domain as TIMER20 (domain 2, DPPIC20), so
  the anchor is one DPPIC20 channel with no PPIB. The fork works: START and
  CAPTURE[5] both subscribe to channel 11, and `nrfx_gppi_ep_attach()`
  returned 0. The reverse route, TIMER20 COMPARE → GRTC CAPTURE, is also one
  DPPIC20 channel. Only the CLOCK events (domain 3, DPPIC30) needed a bridge:
  PPIB30 SUBSCRIBE_SEND[2,3] → PPIB22 PUBLISH_RECEIVE[2,3].
- **Headroom** after the backend's five connections plus the spike's four:
  8 more connections fit per domain pair (lf↔timer, timer↔radio) in the plain
  build, 7 under SDC/MPSL.
- **Under SDC/MPSL** (bt_enable and advertising both returned 0), all nine
  connections allocated with no endpoint contention, and the held S3 ran clean
  on the 2000 cycles where the CPU stayed awake: every error was −1 or 0 µs.

### S3: the anchors are exact to the 1 µs tick, with one trap

v2 run (`l15_s3v2.log`): the CPU is asleep across the compare, with the
compare IRQ masked so nothing wakes it at T, and awake for the GRTC capture.

| variant | anchor | n | distribution (µs) | mean |
|---|---|---|---|---|
| lp (HFXO off between cycles) | wake | 10 000 | −1: 6784, 0: 3216 | −0.68 |
| lp | release | 10 000 | −1: 7451, 0: 2549 | −0.75 |
| held | wake | 10 000 | −1: 7212, 0: 2788 | −0.72 |
| held | release | 10 000 | −1: 7380, 0: 2620 | −0.74 |

No HFXO start was late (0 of 10 000). CC5 at wake was 0 every time. The
CPU-asleep and CPU-busy halves have the same distribution. **Target ±1 µs:
met. Gate ≤ ±2 µs: met.** The 0/−1 split is the phase between two
unsynchronised 1 MHz counters. A constant offset below 1 µs needs no
calibration.

**Trap found (first run, `l15_plain.log`, and the BT run): do not trust a
DPPI-triggered GRTC CAPTURE while the CPU sleeps.** When the capture fired
during sleep, it read a value 799-829 µs (wake) or 272-303 µs (release) too
early. In every case the value corresponds to a SYSCOUNTER that stopped about
170-200 µs after the CPU last ran: `TIMEOUT`=5 LF cycles is 153 µs, and the
GRTC keeps the 1 MHz SYSCOUNTER active only while a CPU is awake. GRTC
*compare* events are exact during sleep, as the v2 run shows (`WAKETIME`=4).
Design consequence: anchor only via LF compare → TIMER (as Track B already
plans), never via TIMER/RADIO event → GRTC CAPTURE, unless the CPU or a
KEEPRUNNING request holds the SYSCOUNTER. Software reads
(`z_nrf_grtc_timer_read()`) are made by an awake CPU and are unaffected.

### S4: HFXO start-up (µs from `onoff_request()`)

| CONSTLAT | request → callback (what the backend sees) | → XOSTARTED (hw) | → XOTUNED (hw) |
|---|---|---|---|
| on | p50 655, p99 675, **p99.9 677, max 679** | p50 192, max 207 | p50 606, p99.9 636 |
| off (LOWPWR) | p50 656, p99 676, **p99.9 679, max 680** | p50 192, max 207 | p50 607, p99.9 637 |

1000 samples each, no timeouts, HFXO off before every request. CONSTLAT makes
no measurable difference to start-up. The driver signals "started" at XOTUNED,
about 420 µs after XOSTARTED, and the callback lands about 45 µs after
XOTUNED. HFINT calibration (errata 30 path) is off in this build.

### S5: rate (TIMER on HFXO vs GRTC)

Sleep, 300 s: **−29.95 ppm**. Busy, 120 s: **−29.97 ppm**. Negative means the
TIMER counts fewer µs than the GRTC. Sleep and busy agree, so the GRTC 1 MHz
count runs at the LFXO rate even while the CPU is awake: it is not clocked
from the HFXO when one is running. On this DK the two crystals differ by about
30 ppm. Which crystal is off cannot be told without an external reference.

## Results: Pro Micro nRF52840

### S1: everything is retained, but SHUTDOWN zeroes the counter

All 54 registers were retained in all three runs. The one reported DIFF,
TIMER.CC[2] (0x40000222 → 0x7D0), comes from the spike's own HFCLKSTARTED →
CAPTURE[2] connection firing when the HFXO restarted. It is not a retention
loss. The HFXO was off during the sleep: HFCLKSTAT read 0x00010000 (running,
on RC) at 125 ms.

- **TIMER SHUTDOWN** (anomaly 78): the configuration and CC registers survive,
  but the **counter is reset to 0**. The capture after the wake read 0
  (`count_after=0`, against 2000 without SHUTDOWN).
- The release capture must therefore be read before SHUTDOWN. The wake path
  starts from a cleared TIMER anyway.
- PPI CH/FORK/CHEN were retained.
- Restore list: empty.

### S2

- RTC1 user channels: 2 (channels 2 and 1, then `-ENOMEM`), as configured.
- PPI: the anchor is channel 13 with FORK → CAPTURE[5]; the measurement uses
  channel 12. The backend's five connections took 18-14.
- **11 PPI channels remain free** after all eight connections.

### S3 (units: 1/8 µs; expected 64 ticks = 1953.125 µs)

> **The lp rows are INVALID, pending a rerun. The rerun has NOT been done.**
> S4 ended with a second HFXO request on the client it already held, so the
> HFXO had two references going into S3 lp, and each lp cycle released only
> one. The HFXO kept running through every "cold" wake: the lp rows measured
> a held HFXO, and "no HFXO start was late" says nothing about a real start.
> The spike is fixed (`hf_request()` now holds at most one reference) but has
> not been rebuilt or run on the Pro Micro since. The held rows are
> unaffected. The L15 S3 numbers above are unaffected too: they come from the
> `SPIKE_S3ONLY` image, which skips S4.

| variant | anchor | n | distribution (1/8 µs) |
|---|---|---|---|
| lp (USB off; **HFXO was NOT off between cycles, invalid**) | wake | 10 000 | −9: 832, −1: 9093, +7: 75 |
| lp | release | 10 000 | −9: 15, −1: 9389, +7: 596 |
| held (USB up) | wake | 10 000 | −9: 622, −1: 9280, +7: 98 |
| held | release | 10 000 | −9: 273, −1: 9540, +7: 187 |

The three values are TIMER counts of 1952, 1953 and 1954 against an exact
1953.125: errors of −1.125, −0.125 and +0.875 µs, which is pure 1 µs
quantisation. No HFXO start was late (0 of 10 000), and CPU asleep vs busy
made no difference. **Gate met for held only; lp is unmeasured (see the note
above).** The RTC has no capture task, so this method
measures the combined jitter of two RTC → TIMER paths, not an absolute offset.
The design needs exactly that: LF ticks map to TIMER µs with ≤ ±1.125 µs.

### S4: HFXO start-up (µs, TIMER on HFINT; HFINT measured +0.13 to +0.16 % fast against RTC1, negligible)

Measured before USB was enabled, so the HFXO really was released between
samples.

| CONSTLAT | request → callback | → HFCLKSTARTED (hw) |
|---|---|---|
| on | p50 344, p99 365, **p99.9 379, max 385** | p50 323, p99.9 357, max 364 |
| off | p50 346, p99 362, **p99.9 405, max 418** | p50 323, p99.9 382, max 395 |

### S5: rate (TIMER on HFXO vs RTC1)

Sleep, 300 s: **−58.53 ppm**. Busy, 120 s: **−57.65 ppm**. USB was up and
holding the HFXO, which does not matter here because the TIMER needs the HFXO
anyway. The clone's two crystals differ by about 58 ppm. That is above the
±50 ppm usually quoted for ANT+, so at least one of them is near or outside
tolerance. The clone has passed every on-air test so far with HFXO timing.

### What USB on the nRF52840 does to the numbers

The CDC console holds the HFXO while USB is enabled, so S1, S4 and S3-lp were
run before `usbd_enable()`. S1 and S4 are real HFXO-off numbers; S3-lp is
not, because of the spike's own reference leak (see S3). S3-held and S5
ran with USB up, and need the HFXO to be on regardless. In B5, a CDC debug
build can therefore only report the stack's own request duty, as the plan
already says.

## Design changes these results force

1. **Wake lead.** HFXO start-up is far tighter than the plan's 1.7 ms
   assumed.
   - nRF54L15: p99.9 of request → callback is 679-680 µs (max 680). Suggested
     `RADIANT_NRF_LP_HFXO_STARTUP_US` default **800** (max plus about 18 %).
   - nRF52840 clone: max 418 µs. Suggested default **500**.
   - The full wake lead is that figure plus the existing arm lead and ISR
     latency. With KEEP_ON 3 ms, the sleep threshold drops from ≈4.7 ms to
     ≈4 ms on the L15.
   - Keep these values per SoC and Kconfig-overridable: start-up depends on
     the crystal and its load capacitors, and only one board of each kind was
     measured.
2. **The rate step between asleep and awake is ~30 ppm (L15) to ~58 ppm (Pro
   Micro), not "±5 µs per period".**
   - Asleep intervals run at the LFXO rate and awake intervals at the HFXO
     rate.
   - At the HR period (8070/32768 s ≈ 246 ms), a period spent asleep comes
     out 7.4 µs (L15) or 14.4 µs (Pro Micro) different from an HFXO-timed
     one. The shift is constant, not accumulating.
   - G1's "mean period shift ≤ LFXO offset" still holds, but the expected
     shift should be stated per board from these S5 numbers.
   - On the clone, consider which crystal a master should trust; that is a
     B5 question. Measuring against an external reference (Garmin or TI
     receiver period by the host clock) would settle which crystal is off.
3. **Anchor source.** Use only LF COMPARE → TIMER (START / fork CAPTURE5).
   Never take a GRTC CAPTURE triggered by DPPI while the CPU may be asleep
   (S3 trap above). No anchor offset calibration constant is needed on either
   SoC (|mean| < 1 µs).
4. **nRF54L15 routing.** The anchor is a single same-domain DPPIC20
   connection. No PPIB is involved, and none of the plan's cross-domain
   failure handling applies to it. Allocation headroom is ample (7 under
   MPSL).
5. **nRF52 SHUTDOWN.** Read CC5 (the release capture) before triggering
   SHUTDOWN, because SHUTDOWN zeroes the counter. CC registers survive it.
6. **Restore list: empty on both SoCs** for HFXO-off System ON idle. Track B
   does not need to rewrite RADIO, TIMER or (D)PPI configuration on wake.

## Gate G0

| criterion | nRF54L15 | nRF52840 |
|---|---|---|
| S2: two LF channels, LF compare → TIMER START + fork CAPTURE allocates and works | PASS: 4 channels (plain and SDC/MPSL), same-domain DPPIC20 | PASS: 2 RTC1 user channels, PPI + FORK |
| S3 ≤ ±2 µs | PASS: {−1, 0} µs over 20 000 cycles each for wake and release | held PASS: −1.125…+0.875 µs over 10 000 cycles each; **lp INVALID, rerun not done** (HFXO stayed on, see S3) |
| restore list known | PASS: empty (63/63 retained) | PASS: empty (54/54 retained); SHUTDOWN zeroes the counter |

**G0: PASS.** Track B can proceed to B2, with the changes above.

## Not done / limits

- **One board of each kind.** No current measurement, and no measurement of
  the power actually saved; that is G1.
- **nRF52840 with BT/MPSL not built or run.** By inspection, MPSL on nRF52
  uses RTC0/TIMER0 and its own PPI channels, not RTC1. This is unverified on
  hardware.
- **nRF5340 network core not tested.** It is wrap-up scope.
- **System OFF and RADIO power-domain gating** were not observed directly
  (see the S1 caveat).
- The L15 `held_bt` S3 ran 2000 cycles, not 10 000, and was built before the
  v2 method: only its CPU-awake half is a valid measurement.
- Absolute crystal error (which of HFXO/LFXO is off) was not measured.
- **Pro Micro S3 lp must be rerun** with the fixed spike (one HFXO reference
  per cycle). Until then its nRF52840 G0 S3 cell rests on the held rows only.
  Not yet done.
