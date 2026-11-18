# What holds interrupts off on the TI (owner item b, 2026-11-16)

Follow-up to `../README.md`: the UART overruns needed the ANT UART ISR held off for more than ~170 µs. This finds
what does it, and judges whether it matters for radio timing.

## Result

**One site: the radiant scheduler's `pass()`, called from the RF RX callback.** `pass()` takes
`radiant_event_crit_enter()` (an irq_lock: BASEPRI) for its whole body, on purpose:

> radiant_sched.c `pass()`: "Interrupts stay off for the whole pass: the in_pass check-and-set and the table reads
> must not interleave with an ISR."

- **Where every long hold-off returned to:** PC `0x131ae`, inside `radiant_event_crit_exit` (the instruction
  after `msr BASEPRI`). The stacked LR was `0x8b51`, the return address of `hal_rx()`'s call to `pass()`
  (radiant_sched.c:2478; `pass()` tail-calls `crit_exit`). The interrupted context was IPSR 43 = `INT_SWEV0`,
  the TI SwiP software interrupt that runs the RF driver callbacks.
- **Nothing else contributed.** Every hold-off of 50 µs or more, in every run, came from this one site. The
  table held 16 sites and only one was ever used. Other priority-0 ISRs (RTC/sys-clock, UART) and RF-driver
  `HwiP_disable` sections never reached 50 µs.
- **The span is page 8 `pass_max_us`.** Page 8 already describes it as "the pass's interrupts-off span". The
  probe's per-run maximum tracks it: 184 / 225 / 368 µs against page 8 181 / 229 / 492 µs. The probe samples
  at ~1 kHz, so it undersamples the rare worst case.

### Measured spans by tracked-channel count

120 s per run, C2 masters plus trainer, TI on the probe image:

| | 1 tracked | 4 tracked | 9 tracked | 9 tracked + 4.5 s scan reopen |
|---|---|---|---|---|
| probe max hold-off (µs) | **184.5** | **225.4** | **368.1** | **302.8** |
| probe samples ≥ 100 µs / ≥ 170 µs / ≥ 250 µs (of ~120 400) | 41 / 8 / 0 | 243 / 105 / 0 | 819 / 505 / 169 | 1228 / 655 / 109 |
| page 8 `pass_max_us` (since boot) | 181 | 229 | 492 | 492 |
| `pass_over_100us` / `sched_dones` (this run) | 492 / 491 | 1955 / 1979 | 4384 / 4462 | 8357 / 7986 |
| page 9 max (µs, since boot): sweep / decide / arm call / done callbacks | 17 / 85 / 85 / 86 | 31 / 119 / 85 / 127 | 128 / 172 / 85 / 143 | 128 / 178 / 85 / 143 |
| page 9 `pass_to_arm_max_us` / `arm_slack_min_us` | 110 / 367 | 154 / 123 | 420 / 26 | 420 / **19** |
| page 4 `min_lead_us` | 520 | 276 | 180 | 173 |
| page 7 `trk_open_late` (this run) / max late µs (since boot) | 0 / 0 | 0 / 0 | 15 / 203 | 26 / **458** |
| page 0 `sched_missed` / `slots_missed` (this run) | 0 / 19 | 0 / 103 | 5 / 477 | 4 / 467 |
| page 9 `early_etime` | 0 | 0 | 0 | 0 |

- **Almost every pass takes more than 100 µs on the TI,** even with one channel (`pass_over_100us` ≈
  `sched_dones`).
- **The pass grows with the channel count:** the sweep goes from 17 to 128 µs and the decision from 85 to
  178 µs.
- **Two parts are flat:**
  - The arm call is a flat 85 µs. That is `RF_scheduleCmd` in the TI RF driver, running inside the masked pass.
  - The done callbacks reach 86–143 µs. They are the channel state machines plus event posting to the host
    queue.
- Idle (no channels): max 34 µs (`raw/t0_idle`).

## Does it matter for radio timing?

**The masking itself, no; the pass length, marginally, and only at 9 channels.**

- **Masking does not delay any radio work.** `pass()` runs in the SwiP software interrupt at the lowest
  priority. The RF driver's hardware interrupts (CPE, RAT) are at that same lowest priority, so they wait for
  the pass whether or not interrupts are masked. Radio commands start from RAT hardware triggers, which masking
  cannot delay.
- **What masking does block:** the priority-0 ISRs, the UART and the sys-clock RTC. The UART half is fixed by
  the FIFO (4 ms of tolerance against a 492 µs worst case, ~8x margin). The RTC half only delays `k_timer`
  expiries by up to ~0.5 ms, and nothing timing-critical on the radio runs on a `k_timer` here: the RF driver
  inactivity timer is off (`nInactivityTimeout = UINT32_MAX`).
- **The pass length is the real radio-timing quantity,** and it is already on page 8 and page 9:
  - At 9 channels, `pass_to_arm` reaches 420 µs and the worst arm slack falls to 19–26 µs.
  - Tracked windows open late 15–26 times in 120 s (~0.3 % of dones), by up to 458 µs.
  - `sched_missed` is 4–5 per 120 s; `early_etime` stays 0.
  - At 1 and 4 channels slack stays above 120 µs and nothing opens late.
  - So the TI scheduler is near its edge at the C2 load: a few late opens and misses per two minutes. That is
    real but small next to the measured RF loss.
- **Tracked loss at 9 channels:**
  - RX_FAIL per channel is 7–74 per 120 s for masters, about 1.5–15 %. The worst channels are the 5340 net-core
    masters, which have the known 8 dB TX ceiling.
  - The trainer reads 167. At 26 late opens per run, late opening could account for at most a few tenths of a
    percent of that loss.

**Pro Micro → TI TX loss (8–13 %, `../../2026-11-16-b4-pair/gate_extra/`):** not explained by this.
- The receive path showed no buffer overflow: page 4 `rx_buf_full` 0, page 8 `drain_max_frames` 1.
- RF core reception is in hardware and masking cannot stall it.
- Late opens are ~0.3 % of windows, far below 8–13 %.
- Not chased further, as briefed.

## Fix options (none applied; your call)

- **Do nothing for the host link.** The FIFO commit already covers it with ~8x margin. Page 8 `pass_max_us`
  remains the watch number for interrupt hold-off, and 0xF6 `uart_rx_overruns` now shows any return of the
  symptom.
- **Shorten the masked pass. This is scheduler core, shared with nRF, so it is not contained:**
  - Run the done callbacks and event posting after `crit_exit`, through a deferred list. They are up to 143 µs.
  - Move `RF_scheduleCmd` (85 µs flat) out of the masked region. That needs the pass to commit its decision
    under the lock and arm outside it, which changes the scheduler's re-entrancy design.
  - Cut sweep/decide cost at high channel counts (128 + 178 µs at 9).
  - Each of these attacks the 19–26 µs slack too, which is the radio-relevant margin.
- **No vendor-code change is indicated.** No `HwiP_disable` section in the TI RF driver, SwiP, HwiP or ClockP
  showed up at 50 µs or more.

## Method

- **Probe** (`images/lat_probe.c`, ad hoc, not committed; full diff `images/ti_lat_final.patch`):
  - GPT0A runs a periodic 48 MHz down-counter with a 997 µs period, interrupting at Zephyr priority 0, the
    UART's level.
  - A naked stub hands the hardware-stacked frame to C. The ISR records:
    - lateness, i.e. counter value past timeout, a 20.8 ns resolution;
    - a histogram;
    - min and max;
    - a 16-entry table of interrupted PC, LR and IPSR for every sample of 50 µs or more.
  - Readout is an ad hoc MESG_REQUEST `[page, 0xFE]` in the bridge.
  - Probe cost is ~1 kHz × ~1 µs. Baseline entry latency is 0.75 µs.
- **What a sample shows:**
  - For an irq_lock span, the recorded PC is the first instruction after the unmask.
  - For an equal-priority ISR it would be whatever that ISR had preempted. Neither case appeared at 50 µs or
    more except the one site.
  - Worst-case lateness equals the longest masked or equal-priority span that a sample happened to land in.
- **Image:** `images/ti_lat.hex` md5 `19e66ffa0fc29614ad862545edb7a324` (5429cf6 + probe, `ti_health`+`ti_diag`),
  with its ELF for addr2line.
- **Scripts:** `lat_run.py` (the run), `summarise.py` → `raw/summary.json`.
  - Page 0/4/7/8/9/11 "this run" values are after minus before.
  - Maxima and minima are since boot and so cumulative across runs t1 → t9r, in that order.
- **Runs:**
  - `raw/t0_idle`: no channels, 20 s.
  - `raw/t1`, `t4`, `t9`: tracked only.
  - `raw/t9r`: 9 tracked + 4.5 s reopen.
  - `raw/aborted_t1_appclosed`: a first t1 cut off when the session closed. Its samples went into the probe's
    since-boot counters before t1 reset them.
- **No logic analyser was used.** None is on this bench, and the GPT probe gives the same answer plus the code
  site.

## Bench and final state

- **Flashing:** only the TI was flashed: the probe image, then at the end the reviewed diag image
  `../images/ti_diag_5429cf6.hex` (md5 `d3731c7dd5d128b53edbca362c984b8e`, `flash_final.log`, probe PASS).
- **0xF6 on the final image:** answers with `uart_rx_overruns: 0`, which is measured. Coverage is absent
  (sentinel). This is the first hardware read of the new field.
- **Worktree:** C:\wt_tistall is back to clean 5429cf6. The ad hoc probe edits are reverted and saved in
  `images/`.
- **Masters:** reset before each run by `lat_run.py`. The L15 kept its Zwift MPSL image.

## Status (2026-11-16, later)

- **Accepted:** the coordinator accepted this result. Nothing from the probe is committed; page 8
  `pass_max_us` stays the measure of the hold-off.
- **Follow-ups:** filed in `TODO.md` on `fix/ti-cmd-stall` (commit 944ac38), in the section "Open after the
  2026-11-16 benches":
  - shorten the masked `pass()` (done callbacks and event posting after the unlock; `RF_scheduleCmd` out of
    the lock);
  - the unexplained Pro Micro → TI link loss.
- **Watch numbers:** if overruns ever return, 0xF6 `uart_rx_overruns` shows them. Each is a receiver-overrun
  event: at least one lost host byte, and at most one counted per ISR pass.
