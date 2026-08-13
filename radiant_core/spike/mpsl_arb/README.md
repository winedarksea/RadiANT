# P0 — the MPSL timeslot arbiter, measured

Throwaway. It includes no `radiant_core`, touches no HAL and drives no RADIO.
Its whole purpose is that P3's margins, deadline timer and status set are
derived from numbers rather than guessed at, and it is expected to be deleted
once they are.

Board: **nRF54L15 DK only**, NCS **v3.4.0**. Both are load-bearing —
`MPSL_TIMESLOT_START_JITTER_US` is 0 on this part because it schedules off GRTC
and 1 elsewhere, and `MPSL_TIMESLOT_CONTEXT_SIZE` is 96 bytes in v3.4.0 against
48 in v3.2.4. `scripts\build_all.ps1` defaults to v3.2.4 for the sdk-ant release
images; this is not one of those and is pinned explicitly.

## Building

```powershell
. .\scripts\env.ps1 -NcsVersion v3.4.0
Push-Location C:\ncs\v3.4.0

# baseline: MPSL, no second stack
west -z C:\ncs\v3.4.0\zephyr build `
     -s C:\Users\Colin\ant_dongle\radiant_core\spike\mpsl_arb `
     -d C:\Users\Colin\ant_dongle\build\spike_mpsl_arb `
     -b nrf54l15dk/nrf54l15/cpuapp -p always --no-sysbuild

# contended: the SoftDevice Controller advertising beside us
west -z C:\ncs\v3.4.0\zephyr build `
     -s C:\Users\Colin\ant_dongle\radiant_core\spike\mpsl_arb `
     -d C:\Users\Colin\ant_dongle\build\spike_mpsl_arb_ble `
     -b nrf54l15dk/nrf54l15/cpuapp -p always --no-sysbuild `
     -- "-DEXTRA_CONF_FILE=ble.conf"

Pop-Location
```

Flash with the repo's guarded J-Link path and `-SelectEmuBySN` — never a bare
CommanderScript. Console is the DK's default UART VCOM (uart20); one-character
commands, `?` for the list.

## Runs

| Key | What it measures | Which P3 decision it feeds |
|---|---|---|
| `e` | `EARLIEST` grant latency, 100 samples | whether the bootstrap and recovery requests can be placed as late as the design assumes |
| `3` | `NORMAL` chain at 32/s for 20 s — eight tracked ANT+ sensors | the BLOCKED rate the denial signal has to absorb |
| `9` | `NORMAL` chain at ~90/s — those plus a sweep | the same under the sweep's load |
| `x` | extension chain, 50 iterations | whether "the sweep is the elastic consumer" is implementable as `ACTION_EXTEND` at all |
| `t` | free-running TIMER continuity across timeslot edges | whether `radiant_core`'s 1 MHz absolute timebase survives under MPSL |
| `p` | toggle `PRIORITY_NORMAL` ↔ `HIGH` | finding 1: expected to change little, and that is the result |
| `l` | cycle slot length 1000 / 5000 / 20000 µs | |

Run each of `e`, `3`, `9`, `x` on **both** images and record the pair. A grant
latency quoted without the other stack's duty cycle beside it is not a
measurement of anything.

Two figures deserve naming, because P3 has no substitute for either:

- **`blocked_lateness`** — how late `SIGNAL_BLOCKED` arrives relative to the
  start that was requested. This sizes the front end's own deadline timer, which
  is a *required* component: between the arm and the signal, the core's single
  operation slot holds an operation that will never happen, and every other
  channel's window expires behind it as MISSED — the exact failure the denial
  signal exists to prevent.
- **`anchor_error`** — `|actual start − predicted start|` on a chained request,
  where the prediction is `anchor + distance` and the anchor does **not** move
  when a request is BLOCKED. `START_JITTER_US` is 0 on this part, so anything
  beyond a few microseconds here is a schedule that walks.

## Results — nRF54L15 DK, NCS v3.4.0, 2026-08-12

Baseline = MPSL only. Contended = the SoftDevice Controller advertising
connectably at 1.0–1.2 s. Same board, same sitting, one flash apart.

| Measurement | No second stack | BLE advertiser |
|---|---|---|
| `EARLIEST` grant latency (n=100) | 1692 / 1692 / 1695 µs | 1693 / 1704 / **2760** µs |
| `NORMAL` chain @ 32/s, 20 s | 641 starts, **0 blocked** | 635 starts, **7 blocked — 1.1 %** |
| `NORMAL` chain @ 90/s, 20 s | 1801 starts, **0 blocked** | 1775 starts, **10 blocked — 0.56 %** |
| `BLOCKED` timing vs the requested start | — | **0 late, 17 early by 11–31 ms** |
| Extension chain (n=50) | 90 ms every time, **0 refused** | 90 ms every time, **0 refused** (4450 extensions) |
| Anchor error \|actual − predicted\| | 0 / 0 / 15 µs | 0 / 1 / 17 µs |
| Free TIMER vs kernel, 321 timeslots | **−1 ms / 10 s (−99 ppm)** | — |

min / mean / max. Flash and RAM are in the table further down.

### What each one settles

**Grant latency is not a lottery — it is the crystal.** 1692–1695 µs with a
3 µs spread over 100 samples, sitting exactly on nRF54L's 1650 µs HFXO
startup plus change. A BLE advertiser leaves the median untouched and adds a
tail: one sample in a hundred at 2760 µs. So finding 2 of the plan holds for
the right reason — the exposure is a **tail**, not a shifted mean, and a
`min_arm_lead_us` big enough to cover it would have to be 2.8 ms, well past the
1310 µs ceiling `radiant_burst.c` enforces. Arbitration uncertainty has to reach
the core as denial. It cannot be absorbed as lead.

**The anchor rule is exact and cheap.** 0–17 µs error on a chain of 640
requests, consistent with `MPSL_TIMESLOT_START_JITTER_US = 0` on this part, and
it does **not accumulate** over 20 s. Computing every distance against the last
granted start — and not moving the anchor when a request is blocked — is
correct arithmetic rather than an approximation.

**The extension chain works, and nothing refused one.** 4450 successful
extensions with the advertiser running and zero `EXTEND_FAILED`. So "the sweep
is the elastic consumer" (ADR 0013) is implementable as designed. **What this
does NOT yet show is the yield**: a 1 s advertiser simply never wanted the air
during a 90 ms window. The refusal path is what P4 has to reproduce against
OpenThread, and until it does, `EXTEND_FAILED` is an untested branch.

**About 1 % of requests blocked, at both loads.** Eight tracked ANT+ sensors is
32 requests/s (1.1 % refused); those plus a sweep is ~90/s (0.56 %). That is the
rate the denial mechanism has to absorb, and it is small — seven denied slots in
20 s, each of which must reach `radiant_channel.c` as a widened guard rather
than as a miss. Well inside `RADIANT_CHANNEL_DENY_TO_SEARCH` (16 consecutive),
which is the bound that would have fired if the arbiter were broken.

### ⚠ THE DEADLINE TIMER IS NOT REQUIRED, AND THE PLAN SAYS IT IS

The plan's finding is that denial *"arrives late, and from a cooperative
thread… It is too late to be useful on its own"*, concluding the front end
needs its own deadline timer as a **required component** — the single most
complex and least testable part of P3.

**Measured, that is false on this part.** Of seventeen `BLOCKED` signals across
both loads, **not one arrived after the start it referred to.** Every one
arrived *early*, by 11–31 ms at 32/s and by 11078–11081 µs at 90/s — and that
second figure is almost exactly one `distance_us` (11111), which gives the
mechanism away:

> **MPSL decides a `NORMAL` request when it is PLACED, not when the requested
> start arrives.** The plan already says this about grants — *"a NORMAL request
> is placed in advance by the MPSL scheduler, making the reservation
> deterministic rather than a latency lottery"* — and the same is true of
> refusals. `SIGNAL_BLOCKED` runs from `mpsl_low_priority_process()`, so it is
> late *relative to the decision*; it is not late relative to the **slot**.

So the `BLOCKED` signal is timely enough to synthesise the DENIED terminal
before the window's own `t_open`, which is exactly what the core needs, and the
front end does not need to predict the denial with a timer of its own.

P3 keeps a deadline timer anyway, as a **backstop rather than the primary
path**: it is a few lines of `k_timer`, the cost of being wrong about this is a
wedged operation slot, and the measurement above is 17 samples against one
advertiser rather than a guarantee. What changes is that it is no longer
load-bearing, and P3 counts how often it fires — under this measurement, never.

### ⚠ Two numbers that are the instrument, not the arbiter

Both were believed for a while, and both are recorded because the way they
looked plausible is the useful part.

**The contended block rate read 53 % and is 0.56 %.** Two separate bugs in this
spike's *recovery* loop, found one after the other, and the second is the more
interesting:

1. `SESSION_IDLE` fires for reasons other than a broken chain, so the recovery
   thread woke while the chain was healthy, issued a `NORMAL` request into a
   session that was not IDLE, took `-NRF_EAGAIN`, and ended the run. 100 grants
   in 20 s instead of 640 reads as catastrophic contention and was a stopped
   chain. Fixed with an explicit `chain_broken` flag.
2. The recovery then aimed at `anchor + distance × (blocks + 1)`, which is the
   obvious reading of the anchor rule and works at 32/s and spirals at 90/s.
   **At an 11.1 ms distance a thread-context recovery costs more than one
   distance**, so by the time the request was placed its instant had gone; it
   was refused, and the next attempt was one distance further behind. That
   produced `blocked_lateness` samples running to 13 s and a block rate of
   5283 per 10 000. Fixed by computing the skip from the clock — however many
   whole distances from the anchor it takes to clear `now` plus a margin above
   the measured grant latency.

**That second one is a result rather than a defect.** It is the same arithmetic
P3's front end has to get right, and it says the recovery margin must exceed the
grant latency or a busy arbiter and a slow recovery are indistinguishable. A
tracked ANT+ slot at 249.7 ms has ample headroom; a burst turnaround at 1.55 ms
has none, which is why the arbitrated backend refuses those synchronously
instead of re-requesting.

**"The free-running TIMER was DISTURBED" was the RC oscillator.** It first read
10118 ms against the kernel's 10076 ms over ten seconds — 0.42 % fast — and the
same 0.4 % was sitting in the anchor error (132–172 µs at a 31.25 ms distance).
Not a timeslot effect at all: this spike never requested the crystal, and MPSL
turns HFXO on and off at the edges of every `XTAL_GUARANTEED` timeslot, so
between grants the TIMER's PCLK was the internal RC. With one
`clock_control_on(HF)` the drift is −99 ppm and the anchor error collapses to
0–15 µs.

> **That is a requirement on P3, not a quirk of the spike.** `radiant_core`'s
> nrf backend already holds the HFXO from init, and the arbitrated gate **must
> keep doing so** — Zephyr's clock control refcounts, so our request holds the
> crystal across MPSL's cycling. Drop it and the 1 MHz absolute timebase, which
> is the clock `t_sync` is captured on and the whole scheduler plans in, gains a
> 0.4 % rate error between grants: about a millisecond of slot-placement error
> per 250 ms ANT+ period, silently, with every counter reading zero.

### Still to measure — and what P3 was allowed to be written without

**`blocked_lateness` is measured and the answer is "there isn't any"**: 17
samples, all early. That is what unblocked P3, and it demoted the deadline timer
from a required component to a backstop.

Two things remain, and neither gates P3's *code* — both gate its **acceptance**,
which is P3's A/B and P4:

- **The whole `EXTEND_FAILED` path is untested.** 4450 extensions, zero refused:
  a 1 s advertiser simply never wanted the air during a 90 ms window. The
  refusal is the mechanism ADR 0013's "the sweep is the elastic consumer" rests
  on, so until something reproduces it, that branch is written and unexercised.
  It needs OpenThread, not BLE — P4.
- **Everything against 802.15.4** rather than BLE. The switching-time and
  priority arguments differ on that side, and §7.3a's frame-loss arithmetic is
  measured by P3.5 against a stub 15.4 receiver.

## Settled before the DK was free

Two of P0's questions are answered by the headers and by a build, and both are
recorded here so they are not re-asked at the bench.

### The TIMER audit: TIMER20 does not collide, TIMER10 is claimed three ways

The plan flagged a possible collision between MPSL's `MPSL_TIMER0`, the
802.15.4 driver's own instance, and `radiant,radio-timer = &timer20` in the
product board overlay. From the shipping headers:

| Claimant | Instance on nRF54L | Source |
|---|---|---|
| MPSL | `NRF_TIMER10` | `mpsl_hwres.h` — `MPSL_TIMER0` under `LUMOS_XXAA` |
| `nrf_802154` | 10 | `nrf_802154_peripherals_nrf54l.h` — `NRF_802154_TIMER_INSTANCE_NO` |
| `radiant_core` | `timer20` | `boards/nrf54l15dk_nrf54l15_cpuapp.overlay` |

`nrf/subsys/mpsl/init/mpsl_init.c` asserts
`!DT_NODE_HAS_STATUS_OKAY()` for **timer0, timer10 and timer020 only**. TIMER20
is not reserved on any part. **The product overlay does not have to move**, and
the three-way claim is on TIMER10, which the 802.15.4 driver takes through
MPSL's arbitration rather than beside it.

Worth having settled early: had it collided, the fix would have been to move the
shipping backend's timer to `timer21`–`timer24`, which is a change to a board
file every bench result to date was produced against.

### Flash and RAM with a stub second stack

Built, not estimated — nRF54L15, `-Os`, v3.4.0:

| Image | FLASH | RAM |
|---|---|---|
| spike, MPSL only | 45 244 B | 12 376 B |
| spike, + SoftDevice Controller peripheral | 123 948 B | 28 368 B |
| **BLE second stack costs** | **+78.7 kB** | **+16.0 kB** |
| `ant_dongle` `-DANT_RADIO=core -DRADIANT_BACKEND=nrf` | 73 528 B | 41 996 B |

So a combined RadiANT + BLE image lands around **150 kB flash / 58 kB RAM** on a
part with 1524 kB / 256 kB. **BLE coexistence has no memory problem.**

Thread/Matter is the one that does, and this does not measure it: the Matter
reference figures for nRF54L15 are 649 kB ROM / 160 kB RAM, and the reference
layouts want ≥1.5 MB *external* flash for DFU, which the dongle form factor does
not have. That number decides whether the nRF54LM20 conversation has to happen,
and it needs an OpenThread build rather than this one.

## Two ways this program can assert, both deliberate

1. **`SIGNAL_OVERSTAYED`.** Closing a timeslot late is a crash, not a lost
   window. Every timeslot here ends from its own `SIGNAL_START` or from an
   `EXTEND_FAILED` having done no work, so it should be unreachable — and if it
   is reached anyway, P3's tail margin is larger than anything this spike would
   have suggested. The assert handler prints the counters.
2. **An action returned from a low-priority signal.** `BLOCKED`, `CANCELLED` and
   `SESSION_IDLE` run in `mpsl_low_priority_process()` and returning anything but
   `ACTION_NONE` from one asserts inside MPSL. Recovery here is a semaphore and a
   thread for that reason, which is the same reason P3's recovery cannot come
   from the signal either.
