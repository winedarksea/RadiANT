# P4: why RadiANT got no air beside OpenThread, and the number that fixed it

*2026-08-15, nRF54L15 DK (J-Link 1057737173), NCS v3.4.0,
`apps/dongle_thread` with `thread.conf;bridge.conf`,
`-DANT_RADIO=core -DRADIANT_BACKEND=nrf`.*

Package G's blocker, and the thing `docs/p4-radio-ownership-fault.md` left
open. Two defects, one of which was the whole story.

## The symptom

With the panic of bug 24 gone, the gate counters became readable and said the
gate was getting nothing at all (600 s, `bench-logs/dppi_ab_B2.log`):

```
gate: acq=57497 placed=57481 granted=11497 blocked=45984 ... den anch=11496
      ... inl=45984 ... len=100/100
seam: grant=0 nostage=0 prog=0/0 | sigrad=0 sigt0=0 isr=0
```

Read: every NORMAL reservation refused, and refused *inline* - the answer
delivered from inside `mpsl_timeslot_request()` before the call returned
(`inl == blocked`, exactly). Four refusals invalidate the anchor
(`BLOCKED_RUN_REANCHOR`), the next acquire denies with `den_no_anchor` and
re-bootstraps, and the bootstrap's EARLIEST request *is* granted - so all 11497
"grants" were 100 us anchor timeslots (`len=100/100`) and
`radiant_nrf_gate_on_grant()` had never run. No ANT operation had ever been
programmed into a real timeslot.

The asymmetry is the clue and it was there the whole time: **EARLIEST always
granted, NORMAL never**. That is not a busy radio. A radio with no room refuses
both.

## Bug 25: the elastic backoff never reached the arbiter

Found first, fixed first, and it is real - but it was not the blocker.

`gate_acquire()` computed `len` as the whole window plus margins, wrote it into
`req.params.normal.length_us`, and *then* clamped the elastic classes down to
`elastic_initial_us`:

```c
req.params.normal.length_us = (uint32_t)len;      /* 96700 for a scan chunk */
...
if (next_grant.extendable && len > elastic_initial_us) {
        len = elastic_initial_us;                  /* 2500 */
}
next_grant.granted_len_us = (uint32_t)len;
```

The clamp reached `next_grant.granted_len_us` - this file's own bookkeeping -
and never the request. So MPSL was asked for 96.7 ms of exclusive radio every
time, while ADR 0013's entire "ask small and grow" mechanism (halving
`elastic_initial_us` to `ELASTIC_FLOOR_US` on refusal, growing it back one
`EXTEND_STEP_US` per grant, then the `SIGNAL_ACTION_EXTEND` chain) moved a
number that was not in the request. The extension chain was unreachable code
beside any contending stack: a grant that large is never given, so
`SIGNAL_START` never arrives to grow it.

The gate's own header had this failure written down from bring-up - "a standing
demand for ~96% of the radio that MPSL simply refuses (one EARLIEST grant, then
every NORMAL request BLOCKED forever, chunks=0)" - which is precisely what the
bench was showing.

It was invisible to `radiant/tests/gate` because
`test_an_extension_does_not_hand_the_radio_back` asserts
`granted_len_us < want_len_us`: the two internal numbers, both correct. Nothing
asserted on `fake_mpsl_last_request()`. That assertion is now in the suite.

**Fix:** the length assignment moved below every clamp and now reads
`req.params.normal.length_us = next_grant.granted_len_us;`, so what is asked for
and what `SIGNAL_START` measures the grant against cannot disagree again.

**Measured effect on its own:** `req=2500` where it had been 96700, and
`blocked` unchanged at 100%.

```
gate: acq=17460 placed=17456 granted=3492 blocked=13964 ... inl=13964
      req=2500/+42237
seam: grant=0 prog=0/0
```

So: correct, necessary, and not the blocker. (`bench-logs/ts_fix1.log`, 180 s.)

## Bug 26: the placement lead, which is not grant latency

`PLACE_LEAD_US` was 2500, derived in `radiant/spike/mpsl_arb` from measured
EARLIEST **grant latency** (1692-1695 us, HFXO startup; 2760 us tail). Those are
different quantities:

* *grant latency* is how long MPSL takes to START a timeslot it has already
  decided to give;
* the *placement lead* is how much notice MPSL's SCHEDULER needs to fit a
  fixed-instant slot in beside another client that is already using the radio.

Beside `nrf_802154` in continuous receive - and an **unattached** OpenThread MED
is the worst case of that, since a detached MTD keeps its receiver up trying to
attach - the second quantity is an order of magnitude larger than the first.
2.5 ms of notice is refused on its face, which is why the refusal is inline
rather than deferred.

Instrumentation added to see it at all: `lead=%u/%u` in the gate dump, the last
and minimum `start - now` at placement. `dist` (distance from the anchor) could
not answer it - the anchor ages, so a distance of 42 ms said nothing about how
much notice MPSL was given. Measured lead was 2668 us, every request.

### The sweep

Same board, same image, same detached MED, only `PLACE_LEAD_US` changed:

| lead (us) | placed | granted | blocked | real grants (`seam grant=`) | window | log |
|---|---|---|---|---|---|---|
| 2500 | 17456 | 3492 | 13964 | **0** | 180 s | `ts_fix1.log` |
| 12000 | 10397 | 3465 | 6931 | 1734 | 120 s | `ts_lead12.log` |
| 24000 | 8565 | 8564 | **0** | 8563 | 300 s | `ts_lead24d.log` |

At 24 ms MPSL refuses nothing at all.

**Fix:** `CONFIG_RADIANT_GATE_MPSL_PLACE_LEAD_US` (default 24000) and
`CONFIG_RADIANT_GATE_MPSL_PLACE_MIN_US` (default 18000). A Kconfig symbol
rather than a constant because it is a property of the *other* stack's
schedule: a BLE-only image and an OpenThread image need different answers, and
the only way to find one is to sweep it against the stack in question. A
`BUILD_ASSERT` keeps `PLACE_MIN_US < PLACE_LEAD_US`.

The cost is latency, not air: `gate_min_arm_lead_us()` is
`PLACE_LEAD_US + HEAD_MARGIN_US`, so the core plans every window 24.25 ms
ahead. An ANT+ tracked window at 4 Hz has 250 ms of period to place it in, and
the acknowledged-data reply that arms 1.56 ms out is served from air already
held (`follow_on_us`), so it is unaffected.

### This was a regression, and the file records the moment it landed

Bug 22 removed `elastic_skew_us`, which had added 0-13 ms to `start` and `end`
for elastic requests. Removing it was right - the skew moved the *reservation*
without moving the *operation*, so the grant landed off the window it was
supposed to cover and `program_rx()` refused `ETIME`. But the skew's other
effect was to add up to 13 ms of placement lead, and that was the only thing
keeping the gate fed. The numbers in bug 22's own comment say so: with the skew,
`grant=1236`; without it, `grant=0`.

`build/p4med/dongle_thread/zephyr/.config` (the 2026-08-13/14 passing run,
0.10% loss contended) differs from today's build in exactly three symbols, all
of them the bridge (`RADIANT_BRIDGE`, `ANT_DONGLE_MQTT_BRIDGE_ID`,
`NET_TCP_WORKQ_STACK_SIZE`). Nothing in MPSL, 802.15.4 or radiant differs. It
was a source regression, not a configuration one.

The right repair is the one bug 22's comment said was "not this layer's to do"
- move the reservation and the operation together - and that is exactly what
`gate_min_arm_lead_us()` is for.

## Verification (300 s, default build, no `-D` overrides)

`bench-logs/ts_default.log`, `thread role -> detached` throughout:

```
gate: acq=8350 placed=8351 granted=8350 blocked=0 cancel=0 near=0 long=0
      | den pend=0 anch=0 dist=0 degen=0 nosess=0 owed=0
      | bad over=0 inval=0 unk=0 inl=0 | lead=24054/24033
seam: grant=8380 nostage=0 prog=8380/0 | sigrad=2318 sigt0=6748 isr=2318
      | term=8126 | ramp=8380/1
hi:   acq=1887 den=0 resv=0 pend=1887 blk=0 grant=1886
sw:   n=1886 wipe=0 ramp=1886 rx=266 | started=1886 | open lead=213us late=0
SWEEP ... track_us=230308/942 | end ok=939 abrt=6483 ... deny=0
```

Against the `grant=0 prog=0/0 sigrad=0 isr=0` this started from: every counter
the fix is scored on moved off zero, `blocked` and the API-level `deny` went
*to* zero, and no MPSL assert fired (`over=0 inval=0`).

**On-air ANT+ reception, observed:** `self_channels` opens two wildcard 0x78
(heart rate) slaves. Channel 7 keeps timing out and reopening every 26 s
throughout. Channel 6 opened at `00:40:08` and never timed out again for the
remaining five minutes of the run - it acquired and held a real ANT+ heart-rate
master on air, beside the detached MED. `sw: rx=266` frames caught in tracked
windows; `track_us=230308/942`.

Deliberately **no loss figure**: project memory puts this room at 5-8% and
unstable, the master is not one this bench started or controls, and the bar
here was "packets arrive at all".

`radiant/tests/gate`: 23/23 on hardware, including the new assertion.

## Two things this did NOT fix, both now visible for the first time

**1. Every extension is refused.** `ext=0/6472` - the elastic chain asks and is
turned down every single time, so a scan chunk gets only its initial bite
(`req=10000` against `want=96884`) and ends as a partial (`abrt=6483` against
`end ok=939`). Search dwell is therefore a fraction of what the sweep plans for.
This is the arbiter yielding exactly where ADR 0013 says it should, so it may be
correct behaviour beside a detached MED - but it has never been measured against
an *attached* one, and `EXTEND_FAILED` was documented as an unexercised branch
until today.

**2. A bus fault, seen once, not reproduced.** At `PLACE_LEAD_US=24000`, about
100 s into the first run (`bench-logs/ts_lead24.log`):

```
***** BUS FAULT ***** Precise data bus error, BFAR Address: 0x0
pc=0x00021a26  lr=0x0003a2c5
```

`addr2line -i`:

```
0x00021a26 -> sys_dlist_remove   zephyr/include/zephyr/sys/dlist.h:532
              remove_timeout     zephyr/kernel/timeout.c:49
0x0003a2c5 -> z_abort_timeout    zephyr/kernel/timeout.c:156
```

A NULL in the kernel's timeout list, reached from `z_abort_timeout` - i.e. from
a `k_timer_stop()`. **Hypothesis, not a conclusion:** `CONFIG_ZERO_LATENCY_IRQS=y`
in this image and `nrf/subsys/mpsl/init/mpsl_init.c:187-188` connects
MPSL's RADIO/TIMER0/RTC0 vectors with `IRQ_ZERO_LATENCY`, so `on_signal()` for
`SIGNAL_START`/`RADIO`/`TIMER0` runs in a zero-latency ISR, where `irq_lock()`
does not mask the interrupt and kernel data structures are therefore not
protected. The gate calls `k_timer_stop(&deadline)` and `k_work_submit()` from
those callbacks. That would corrupt exactly this list.

Against the hypothesis: those same calls ran clean for 540 s afterwards
(240 s + 300 s, plus this run's own first 100 s), and `k_work_submit()` from the
same context runs thousands of times per run. One sample is one sample.

It is **not** newly introduced by this fix - the calls have always been there -
but it is newly *exercised*, because until today `SIGNAL_START` only ever ran
for bootstrap anchors, which arm no deadline timer. Deliberately left alone: a
speculative change to the ZLI paths would have muddied a result that is now
clean, and this needs its own bisection.

## Files

* `radiant/src/radiant_radio_nrf_gate_mpsl.c` - bugs 25 and 26, the
  `lead=`/`req=` probes.
* `radiant/Kconfig` - the two new symbols and the sweep table.
* `radiant/tests/gate/src/test_gate.c` - the assertion against
  `fake_mpsl_last_request()` that bug 25 walked past.
* Logs: `bench-logs/ts_fix1.log`, `ts_lead12.log`, `ts_lead24.log`,
  `ts_lead24b.log`, `ts_lead24d.log`, `ts_default.log`.
