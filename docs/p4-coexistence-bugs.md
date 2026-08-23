# P4: the four coexistence defects, and what fixed each

nRF54L15 DK, NCS v3.4.0, `apps/dongle_thread` with `thread.conf;bridge.conf` or
`thread.conf;matter.conf`, `-DANT_RADIO=core -DRADIANT_BACKEND=nrf`.

Four defects between RadiANT's MPSL-arbitrated radio path and the stacks it has
to share the radio with: bug 24 (a panic in `nrf_802154`), bugs 25 and 26 (the
gate got no air at all), and bug 27 (kernel calls from a zero-latency
interrupt). One section each.

---

## Bug 24: shared RADIO registers written with no timeslot held

### The symptom

The image panics 4-45 s after boot:

```
<err> os: ***** HARD FAULT *****   ARCH_EXCEPT with reason 4
<err> os: r14/lr: 0x000179cf   Faulting instruction address (r15/pc): 0x000365a4
<err> os: Fault during interrupt handling
```

`arm-zephyr-eabi-addr2line -i` against the image:

```
0x000365a4 -> nrf_802154_assert_handler   zephyr/modules/hal_nordic/nrf_802154/nrf_802154_assert_handler.c:24
0x000179cf -> rxframe_finish              nrfxlib/nrf_802154/driver/src/nrf_802154_trx.c:1718
0x000179cc -> rxframe_finish              nrfxlib/nrf_802154/driver/src/nrf_802154_trx.c:1715
```

Line 1715 is `wait_until_radio_is_disabled()`, and the only
`NRF_802154_ASSERT` reachable from `rxframe_finish` is that function's own
`NRF_802154_ASSERT(radio_is_disabled)` at `nrf_802154_trx.c:380`. Nothing
else in the chain (`rxframe_finish_disable_ppis`,
`..._disable_fem_activation`, `..._psdu_is_not_being_received`,
`..._disable_ints`, `nrf_802154_trx_ppi_for_ramp_up_propagation_delay_wait`)
contains an assert at all.

So the invariant that fails is: **at CRCOK (or CRCERROR) the RADIO was not in
the DISABLED state and did not get there within `MAX_RAMPDOWN_CYCLES`.**

### What it is NOT

Not the RADIO ADDRESS DPPI endpoint conflict. That conflict is real and the
per-grant endpoint swap in `radiant_radio_nrf.c` (`radio_endpoints_attach()`,
bug 20) is the fix for it, fully built. This fault survives with the swap
permanently inert - the seam probe in the failing run reports
`grant=0 prog=0/0`, i.e. `radiant_nrf_gate_on_grant()` was never entered and
no endpoint was ever swapped in - so the swap cannot be the cause. Not a
shortage of DPPI channels either, nor a PPI leak: allocation succeeds and
`radio_ep_contended`/`radio_ep_mine_mask` are irrelevant on this path.

### What it is

`nrf_802154` receives with

```c
#define SHORTS_RX (NRF_RADIO_SHORT_ADDRESS_RSSISTART_MASK |
                   NRF_RADIO_SHORT_PHYEND_DISABLE_MASK |
                   SHORT_ADDRESS_BCSTART)
```

`PHYEND_DISABLE` is the *only* thing that takes its radio down at the end of a
frame - `rxframe_finish()` then merely waits for a transition that the
hardware has already been told to make. Clear `RADIO->SHORTS` between ADDRESS
and PHYEND and the frame still completes, CRCOK still fires, and the handler
spins on a DISABLED that can never arrive.

Three functions in `radiant/src/radiant_radio_nrf.c` wrote shared RADIO
registers from ordinary thread / work-queue context, with no MPSL timeslot
held:

| function | write |
|---|---|
| `deliver_terminal()` | `nrf_radio_shorts_set(NRF_RADIO, 0)` (and an `INTENCLR00` write on the ED path) |
| `radiant_radio_abort()` | the same, plus `TASKS_STOP`, plus a forced `TASKS_DISABLE` + spin |
| `radiant_nrf_gate_on_grant_end()` | the same, plus `TASKS_DISABLE`, plus the endpoint restore |

A terminal is delivered for **every** operation, including a DENIED one, and
denials are delivered from the gate's work queue - no timeslot, radio owned by
`nrf_802154`. Under `thread.conf` the arbiter refused every real reservation
(bug 26), so the core ran a denial loop: `term` climbs at ~80/s. Each of those
~80 writes a second lands on the 802.15.4 driver's live receiver; one
eventually lands inside a frame.

`radiant_nrf_gate_on_grant_end()` adds a second, rarer instance of the same
mistake: the gate's `g.hw_held` is set at `SIGNAL_START` for *every* granted
timeslot, including the anchor-establishing bootstrap - which returns
`ACTION_END` without ever calling `radiant_nrf_gate_on_grant()` and therefore
without enabling an interrupt, attaching an endpoint or programming a
register. The bootstrap timeslot then "handed back" a radio it had never
borrowed, about once a second.

Note that a Thread peer is not required. `irq_handler_crcerror()` also calls
`rxframe_finish()`, so ambient RF that the 802.15.4 demodulator locks onto is
enough. Both failing runs here had `thread role -> detached`.

### The fix

One predicate, `RADIO_IS_OURS()`, and a `radio_on_loan` flag:

* set in `radiant_nrf_gate_on_grant()` immediately before the first shared
  write (`nrf_radio_int_enable`), i.e. *after* the two early returns that
  leave without touching the peripheral;
* cleared on the last line of `radiant_nrf_gate_on_grant_end()`;
* a compile-time `true` when `CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL` is off, so
  the direct backend is bit-for-bit unchanged.

Every shared-RADIO write listed above is now guarded on it, and
`radiant_nrf_gate_on_grant_end()` returns early for a timeslot that never took
the peripheral. Our own DPPIC channel enables/disables (`nrfx_gppi_conn_*`)
are unguarded - those are ours from any context.

This is deliberately *not* the same question as the gate's `g.hw_held`, and
the gate is untouched: the "exactly one `on_grant_end()` per granted timeslot"
invariant the soak is scored on still holds, only the register writes became
conditional.

### Measurement (A/B/A, same board, same session)

| arm | image | result |
|---|---|---|
| A (baseline) | guard absent | panic ~4 s into capture, `lr=0x179cf` |
| B | guard present | 420 s clean, `term=32336` (~32 000 previously-fatal writes exercised) |
| A' | guard forced off with `#define RADIO_IS_OURS() (true \|\| radio_on_loan)` | panic 26 s after boot, same `lr=0x179cf` |
| B' | guard restored | 600 s clean |

---

## Bugs 25 and 26: why RadiANT got no air beside OpenThread

### The symptom

With the panic of bug 24 gone the gate counters became readable, and said the
gate was getting nothing at all (600 s):

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

The asymmetry is the clue: **EARLIEST always granted, NORMAL never**. That is
not a busy radio. A radio with no room refuses both.

### Bug 25: the elastic backoff never reached the arbiter

Real, and not the blocker.

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

It was invisible to `radiant/tests/gate` because
`test_an_extension_does_not_hand_the_radio_back` asserts
`granted_len_us < want_len_us`: the two internal numbers, both correct. Nothing
asserted on `fake_mpsl_last_request()`. That assertion is now in the suite.

**Fix:** the length assignment moved below every clamp and now reads
`req.params.normal.length_us = next_grant.granted_len_us;`, so what is asked for
and what `SIGNAL_START` measures the grant against cannot disagree again.

**Measured effect on its own** (180 s): `req=2500` where it had been 96700, and
`blocked` unchanged at 100%.

```
gate: acq=17460 placed=17456 granted=3492 blocked=13964 ... inl=13964
      req=2500/+42237
seam: grant=0 prog=0/0
```

### Bug 26: the placement lead, which is not grant latency

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

The sweep, same board, same image, same detached MED, only `PLACE_LEAD_US`
changed:

| lead (us) | placed | granted | blocked | real grants (`seam grant=`) | window |
|---|---|---|---|---|---|
| 2500 | 17456 | 3492 | 13964 | **0** | 180 s |
| 12000 | 10397 | 3465 | 6931 | 1734 | 120 s |
| 24000 | 8565 | 8564 | **0** | 8563 | 300 s |

At 24 ms MPSL refuses nothing at all: 24 000 us takes `blocked` to **0** and
`radiant_nrf_gate_on_grant()` from 0 entries to 8 380, and real ANT+ frames are
received.

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

Bug 26 was a regression. Bug 22 removed `elastic_skew_us`, which had added
0-13 ms to `start` and `end` for elastic requests. Removing it was right - the
skew moved the *reservation* without moving the *operation*, so the grant
landed off the window it was supposed to cover and `program_rx()` refused
`ETIME`. But its other effect was up to 13 ms of placement lead, and that was
the only thing keeping the gate fed: with the skew, `grant=1236`; without it,
`grant=0`. The right repair is the one bug 22's comment said was "not this
layer's to do" - move the reservation and the operation together - and that is
what `gate_min_arm_lead_us()` is for.

### Verification (300 s, default build, no `-D` overrides)

`thread role -> detached` throughout:

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
*to* zero, and no MPSL assert fired (`over=0 inval=0`). On air, one of the two
wildcard 0x78 slaves `self_channels` opens acquired and held a real ANT+
heart-rate master for the remaining five minutes of the run, beside the
detached MED - `sw: rx=266` frames in tracked windows, `track_us=230308/942`.
Deliberately **no loss figure**: this room is 5-8% and unstable, the master is
not one this bench controls, and the bar was "packets arrive at all".
`radiant/tests/gate`: 23/23 on hardware, including the new assertion.

### Left open by bugs 25/26: every extension is refused

`ext=0/6472` - the elastic chain asks and is turned down every single time, so
a scan chunk gets only its initial bite (`req=10000` against `want=96884`) and
ends as a partial (`abrt=6483` against `end ok=939`). Search dwell is therefore
a fraction of what the sweep plans for. This is the arbiter yielding exactly
where ADR 0013 says it should, so it may be correct behaviour beside a detached
MED - but it has never been measured against an *attached* one, and
`EXTEND_FAILED` was documented as an unexercised branch until this work.

---

## Bug 27: kernel calls from a zero-latency interrupt

### The symptom, and the one number that says what it is not

`apps/dongle_thread` with `thread.conf;matter.conf` reboots 13 times in 210 s,
always after the self-channels open. Nothing is printed at the reset.

```
<inf> ant_dongle: reset cause 0x00000100: settling 1000 ms for the bootloader
```

`0x100` is `RESET_CPU_LOCKUP`. The decisive companion fact is in the same
image's `.config`: `# CONFIG_RESET_ON_FATAL_ERROR is not set`. **Any fault
Zephyr detects in this image halts and prints; it does not reboot.** The board
neither halts nor prints. Zephyr's fault path never ran, so this is a genuine
CPU lockup - a fault whose own exception entry faults - not a reported crash.
That distinction is what kept `CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=1120` as the
second hypothesis rather than the first: with `CONFIG_BUILTIN_STACK_GUARD=y` on
this M33, a thread-stack overflow raises a UsageFault, is handled on MSP, prints
and halts.

The bridge arm showed the same root cause as a fault rather than a lockup: a
BUS FAULT at `sys_dlist_remove` <- `remove_timeout` <- `z_abort_timeout`
(`zephyr/kernel/timeout.c:49`, `:156`) - a NULL in the kernel's timeout list,
i.e. reached from a `k_timer_stop()`.

### What it is

Raising the system work queue to 4096 did not change the rate but changed the
failure mode, and the new mode names the cause:

```
ASSERTION FAIL @ spinlock.h:132
***** HARD FAULT *****  Fault escalation (see below)
>>> ZEPHYR FATAL ERROR 4: Kernel panic on CPU 0
Fault during interrupt handling
lr=0x00036213 -> z_work_submit_to_queue   zephyr/kernel/work.c:382
```

`spinlock.h:132` is `__ASSERT(z_spin_lock_valid(l), "Invalid spinlock %p", l)` -
the work queue's own lock, taken recursively on one CPU.

**Mechanism.** `nrf/subsys/mpsl/init/mpsl_init.c:187` connects MPSL's
TIMER0/RADIO/RTC0 vectors with `IRQ_ZERO_LATENCY` when
`CONFIG_ZERO_LATENCY_IRQS=y` (both arms). A zero-latency interrupt runs above
the priority `irq_lock()` and every Zephyr spinlock mask, so kernel data
structures are not protected against it, and any kernel API called from a
timeslot signal callback can land inside another context's critical section.
Nordic's own sample makes no kernel call from its callback
(`nrf/samples/mpsl/timeslot/src/main.c`).

radiant made them in two places, and the second is the one that mattered:

```
SIGNAL_RADIO (ZLI) -> radiant_nrf_gate_on_radio_irq() -> deliver_terminal()
  -> radiant_event_post_rx() -> radiant_event_wakeup()   radiant_event.c:404
  -> k_sem_give(&api_event_sem)            apps/common/ant_radio_radiant.c
```

The API event thread waits on that semaphore **with a timeout**
(`k_sem_take(&api_event_sem, K_MSEC(RADIANT_API_HOUSEKEEP_MS))`), so giving it
runs `z_abort_timeout()` on the kernel timeout list from a context that cannot
hold its lock - and it fires once per received packet, where the gate's calls
fire once per reservation. Every bridge-arm dump names that list
(`sys_dlist_remove` <- `remove_timeout`, `sys_dlist_insert` <- `z_add_timeout`).

An earlier attempt - the gate raising an atomic flag for a gate-owned thread
polled at 1 kHz to pick up - measured worse on both arms and was reverted. Two
lessons, both built into the fix that replaced it. Fixing the gate's own calls
without `radiant_event_wakeup()` leaves the frequent offender in place; and a
poll thread is the wrong deferral, because `k_msleep()` puts an entry on the
very timeout list being corrupted, a thousand times a second.

Three fixes landed, in this order because each one's measurement decided the
next.

### Fix 1: the software-interrupt trampoline

`radiant/include/radiant/radiant_swi.h`, `radiant/src/radiant_swi.c`. The
zero-latency side does an atomic OR and one `NVIC_SetPendingIRQ()` - no kernel
object, no allocation, nothing that can take a lock. The NVIC runs the handler
at an ordinary Zephyr priority, where `irq_lock()` does mask it, and the kernel
calls happen there.

**The line.** `CONFIG_RADIANT_SWI_IRQN` defaults to 29 (SWI01) on nRF54L.
Neighbours checked rather than assumed: MPSL holds SWI00 (28,
`CONFIG_MPSL_LOW_PRIO_IRQN`) and `nrf_802154` holds EGU10 (135,
`NRF_802154_EGU_INSTANCE_NO`). A `BUILD_ASSERT` in `radiant_swi.c` compares
against MPSL's symbol - MPSL connects dynamically and so would not collide at
build time - and everything using Zephyr's `IRQ_CONNECT()` collides in the
generated ISR table, which is why the trampoline connects statically.

**No dropped wakeups.** `radiant_swi_pend()` ORs the bits in *then* pends the
line; the handler clears the whole word in one atomic before dispatching. A ZLI
landing before the clear is handled by the run in progress and costs one extra
empty handler entry; one landing after leaves both the bits and the NVIC pending
flag set, so the handler runs again.

Routed through it: `radiant_event_wakeup()` first, then the gate's three work
submissions and its deadline timer (arm and stop, via a last-writer-wins
request word - the backstop has never fired on this bench, `dead=0`).

**A bug this found in itself:** `gate_init()` registered on every call, so a
`gate_shutdown()`/`gate_init()` cycle - which is also how every test in
`radiant/tests/gate` starts - exhausted the four-entry table on the fourth pass
(`gate: radiant_swi_register: -12`). Registration is now once per boot.

**Result, bridge arm** (560 s, with fix 2): 1 boot, 0 faults, where the same
build previously faulted at 199 s, and the placement-lead work intact
(`grant=13079 prog=13079/0`, `blocked=0`, `bad over=0 inval=0 unk=0 inl=0`,
`lead=24056/23971`, `stacks: isr=888/4096 syswq=1016/4096`). Say it as "no
fault in 560 s", not "fixed" - the pre-fix rate was one per few hundred
seconds, so 560 s is about one expected fault's worth of evidence, not ten.

**Result, Matter arm** (620 s): **still a reboot loop** - 14 boots, 13 lockups,
first life 4 min 28 s clean then five lockups in 68 s. Against 13 boots in
210 s shipped that is roughly a threefold drop in rate and a longest-clean-life
of 4 min 28 s against ~15 s - a real improvement, well outside noise, and
**not the bar**. The trampoline is necessary and is not sufficient.

`radiant/tests/gate`: 24/24 on hardware, including
`test_signal_callback_defers_through_the_trampoline` (the fake trampoline
counts pends per producer). The link carries the other half: the gate no longer
references `k_work_submit()` at all, so reaching for the kernel from the
callback again is a visible edit.

### Fix 2: the system work-queue stack, separately

Measured, not precautionary: peak 1016 bytes of 1120, **91%**, read from the
`CONFIG_INIT_STACKS` fill by a `stacks:` probe added to `gate_dump_fn()`.

`radiant/Kconfig` already carries `default 4096 if RADIANT_BACKEND_NRF_GATE_MPSL`
and it can never apply: `nrf/subsys/net/openthread/Kconfig.defconfig:105` says
`default 1120`, and Kconfig takes the first definition whose condition holds in
parse order - OpenThread's is parsed first. On exactly the arm where the gate is
compiled. An assignment in a `.conf` is not a default and does not lose that
race, so it is now `CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=4096` in
`apps/dongle_thread/thread.conf`, with the reason written beside it. After:
`syswq=1016/4096`.

This did not fix the reboot loop and was never expected to; it is a separate
margin problem that the investigation exposed.

### The bisection: it IS radiant's arbitrated path, and nothing else

`CONFIG_RADIANT_GATE_MPSL_NO_SESSION=y` is the gate's own documented bisection
aid: the gate builds and boots, the RADIO is initialised, the HFXO request is
taken - but no timeslot session is ever opened, so **nothing of radiant's ever
runs in the zero-latency context** while CHIP, the SoftDevice Controller,
OpenThread and the same RAM are all untouched. Same board, same recipe plus
that one symbol: 1 boot, 0 lockups, alive continuously for 9 min 52 s with
self-channels rotating, against 13 lockups in 620 s with the session open. So
**the SoftDevice Controller on its own, RAM pressure and CHIP are all
exonerated as sufficient causes** - they are present and quiet in that run.

### Why a lock-free event ring would not have been enough

`radiant_event_post_channel_event()` is called from `api_sched_done()`, which is
reached both from `deliver_terminal()` in the RADIO signal callback (the
zero-latency one) and from the gate's `denied_work_fn()` (a thread) - so the
ring is multi-producer, and a textbook SPSC ring is not correct here. More
importantly the ring is only the visible part: `api_sched_done()` runs the
core's state machine (`radiant_channel_on_slot()`, `radiant_sched` re-arming,
`api_ch[]`, `api_xfer[]`, `api_stats`), all shared with the thread-context API
surface, which guards itself with `k_mutex_lock(&api_lock)` - and **a mutex
cannot exclude a zero-latency interrupt any more than `irq_lock()` can**.
`radiant_event_crit_enter()` being `irq_lock()` is the same fault in a second
place, not a separate one.

### Fix 3: the (a) split - the core leaves the zero-latency context

`core_cb_rx()` / `_tx()` / `_ed()` in `radiant_radio_nrf.c` are the new seam,
and all six `radiant_op.cbs->` sites go through them.

**What stays in the signal callback:** everything that touches the peripheral or
the grant - the (D)PPI disables, `RADIO->SHORTS`, the interrupt mask,
`radio_disable_now()`, and `gate_release()`, which must be prompt or a tracked
window holds its follow-on reservation for a reply that is not coming. The event
is also fully *built* there, so every register read (`rssi_sample_dbm()`,
`RXCRC`, the captured `t_sync`) still happens inside the window it describes.

**What moves:** the callback into the core, and with it `api_sched_done()`,
`radiant_channel_on_slot()`, the scheduler re-arm, the ring post and the wakeup.

**What makes the existing locking correct again:** the queue is used *only* when
`gate_in_signal()` is true. Every other context - the gate's work queue
delivering a denial, the trampoline dispatching - calls the core directly, which
is legal there. So `api_sched_done()` is now only ever entered at ordinary
priority, and `k_mutex_lock(&api_lock)` means something instead of being
decorative. That is the larger prize, and it is why `gate_in_signal()` is not
`k_is_in_isr()`: the trampoline's own handler is an ISR too. It also collapses
the multi-producer problem - one producer (the MPSL signal callback, which
cannot preempt itself, since MPSL delivers START/RADIO/TIMER0 from one
priority) and one consumer (the trampoline handler, strictly below it), so a
plain ring with a producer-owned head and a consumer-owned tail is correct.

**The body is copied.** `struct radiant_rx_event::body` is documented
"valid only for the duration of this callback", and the callback now happens
later, so the entry carries its own 32-byte copy and the pointer is repointed at
dispatch.

**Overflow was counted, not hoped about.** Eight entries; a completion that
found no room incremented `cb_dropped`, published through
`radiant_nrf_win_diag` as `cbdrop=dropped/high-water`. A dropped completion is
a window the core never sees finish - a channel that quietly stops rather than
a crash. The counter and its publisher were retired with the SWEEP_DEBUG
instrument once P3/P4 concluded; the overflow behaviour itself is unchanged.

**A trap this found in the header:** `gate_in_signal()` was first declared inside
the `CONFIG_RADIANT_SWEEP_DEBUG` block. The bridge arm has that on and compiled;
the Matter arm does not and failed on an implicit declaration. Had `-Werror` not
caught it, it would have been an int-returning guess and a completion queue
bypassed exactly where it was needed most. The declaration is now outside.

### Results of the (a) split

**Matter arm:** alive 12 min 40 s continuously, 1 boot (the flash), 0 lockups
(reset cause 0x100), self-channels rotating normally throughout. Against 13
lockups in 620 s with the trampoline alone, and 13 in 210 s shipped.

**Bridge arm** (600 s, plus a further 420 s): 0 boots, 0 faults in either.

```
seam: grant=17709 nostage=0 prog=17709/0 | sigrad=660 isr=660 | ramp=17709
gate: placed=17711 granted=17710 blocked=0 near=0 long=0 dead=0 | bad over=0 inval=0
seam: grant=12778 nostage=0 prog=12778/0 cbdrop=0/1 | ramp=12778
```

**The latency it cost, measured.** The counters that would show a completion
arriving too late for the core to re-arm in time, before (trampoline only) and
after:

| counter | meaning of non-zero | before | after |
|---|---|---|---|
| `near` | an arm refused for being too near to place | 0 | 0 |
| `prog_fail` | the window was programmed late (ETIME) | 0 | 0 |
| `nostage` | a grant arrived with nothing staged | 0 | 0 |
| `late` (`sw_open_late`) | window opened >1 ms into its grant | 0 | 0 |
| `dead` | the backstop deadline beat the grant | 0 | 0 |
| `open lead` / max | grant start to window open, us | 203 / 234 | 218 / 260 |
| `cbdrop` | completions the queue had no room for | - | 0, high-water 1 of 8 |

So: no arm was refused, no window was programmed late, no completion was
dropped, and the queue never held more than **one** entry. The only number that
moved is the window's open lead, by ~15-26 us against a ~246 ms channel period.

**Frames.** As counts, not rates: `sw rx=2` in 560 s before the split, `rx=3`
in 600 s after, `rx=2` in the further 420 s. All three are low because the ANT+
master present for the earlier `rx=266` run is no longer transmitting, and the
immediately preceding build measured the same - the room, not the split.

The trampoline-only run's shape (4 min 28 s clean, then five lockups in 68 s)
looks like something accumulating and is weaker evidence than it appears: lives
were 268, 8, 37, 15, 8, 96, 18, 12, 19, 8 s, and under a constant-rate
(exponential, mean ~48 s) model one sample exceeding 268 s has ~5% probability
across 13 - unusual, not decisive. The long life is also the first after a
flash, with settings storage in a different state from every later boot.

**RAM:** 94.14% on the Matter arm, from 92.48% before this work: +4342 B, of
which +2976 B is the deliberate `SYSTEM_WORKQUEUE_STACK_SIZE` 1120 -> 4096 and
the trampoline's own state about 50 B. It still fits, but
`docs/matter-bringup.md`'s headroom is thinner and the work-queue stack is
where it went - knowingly, and reversible.

---

## Files

* `radiant/src/radiant_radio_nrf.c` - bug 24's `RADIO_IS_OURS()` guard; the
  (a) split's `core_cb_rx()` / `_tx()` / `_ed()` seam.
* `radiant/src/radiant_radio_nrf_gate_mpsl.c` - bugs 25, 26 and 27: the
  `lead=`/`req=` probes, the trampoline routing, `deadline_apply()`, `stacks:`.
* `radiant/Kconfig` - `CONFIG_RADIANT_GATE_MPSL_PLACE_LEAD_US` /
  `..._PLACE_MIN_US` and the sweep table.
* `radiant/include/radiant/radiant_swi.h`, `radiant/src/radiant_swi.c`.
* `apps/common/ant_radio_radiant.c` - `radiant_event_wakeup()`.
* `apps/dongle_thread/thread.conf` - the work-queue stack, with the reason.
* `radiant/tests/gate/src/test_gate.c` (the `fake_mpsl_last_request()`
  assertion bug 25 walked past) and `radiant/tests/gate/fake_swi.c`.
