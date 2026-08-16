# P4: kernel calls from a zero-latency interrupt

*2026-08-15, nRF54L15 DK (J-Link 1057737173), NCS v3.4.0.*

Follows `docs/p4-timeslot-placement-lead.md`, which is untouched by all of this.

**Status.** Root cause established and measured. Two fixes landed and verified:
a software-interrupt trampoline, and the system work-queue stack. One earlier
attempt was measured worse and reverted.

* **Bridge arm: fixed as far as 560 s of running can say.** Was faulting at
  199 s and at ~660 s; now 560 s with no fault, and the placement-lead result
  intact (`blocked=0`, `grant=13079`, `prog=13079/0`).
* **Matter arm: fixed as far as 12 min 40 s of running can say.** Was 13 lockups
  in 210 s; with the trampoline alone still 13 in 620 s; with the (a) split, one
  boot and zero lockups across 12 min 40 s of continuous uptime.

Three fixes, landed in this order because each one's measurement decided the
next: the trampoline (necessary, not sufficient), the work-queue stack
(a separate margin problem), and the (a) split - moving the core state machine
out of the zero-latency context, which is what actually stopped it.

## The symptom, and the one number that says what it is not

`apps/dongle_thread` with `thread.conf;matter.conf` reboots 13 times in 210 s
(`bench-logs/matter_boot1.log`), always after the self-channels open. Nothing is
printed at the reset.

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

## What it is

Raising the system work queue to 4096 did not change the rate but changed the
failure mode, and the new mode names the cause (`bench-logs/m_wqstack.log`):

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
fire once per reservation. Every bridge-arm dump names that list:

| log | frames |
|---|---|
| first sighting | `sys_dlist_remove` <- `remove_timeout` <- `z_abort_timeout` |
| `ts_zlifix.log` | `sys_dlist_insert` <- `z_add_timeout` |
| `ts_reverted.log` | `sys_dlist_remove` <- `remove_timeout` |

## The attempt that was reverted

The gate was changed to raise an atomic flag and let a gate-owned thread, polled
at 1 kHz, make the kernel calls. Measured worse on both arms:

| arm | change | result |
|---|---|---|
| bridge | as shipped | fault at 199-660 s |
| bridge | flag + 1 kHz poll thread | BUS FAULT at **25 s** |
| Matter | as shipped | 13 boots / 210 s |
| Matter | flag + 1 kHz poll thread | **29 boots / 240 s** |

Two lessons, both built into the fix that replaced it. Fixing the gate's own
calls without `radiant_event_wakeup()` leaves the frequent offender in place;
and a poll thread is the wrong deferral, because `k_msleep()` puts an entry on
the very timeout list being corrupted, a thousand times a second.

## Fix 1: the software-interrupt trampoline

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

## Fix 2: the system work-queue stack, separately

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

## Results

**Bridge arm** (`bench-logs/ts_swi.log`, 560 s, both fixes):

```
boots: 1 (the flash)   faults: 0
stacks: isr=888/4096 syswq=1016/4096
seam: grant=13079 nostage=0 prog=13079/0 | sigrad=480 sigt0=12695 isr=480 | ramp=13079
gate: acq=13080 placed=13081 granted=13080 blocked=0 ... near=0 long=0
      | den pend=0 anch=0 ... | bad over=0 inval=0 unk=0 inl=0 | lead=24056/23971
```

No fault in 560 s, where the same build previously faulted at 199 s. The
placement-lead work is intact. Say it as "no fault in 560 s", not "fixed" - the
pre-fix rate was one per few hundred seconds, so 560 s is about one expected
fault's worth of evidence, not ten.

**Matter arm** (`bench-logs/m_swi_soak.log`, 620 s): **still a reboot loop.**

```
boots: 14   lockups (reset cause 0x100): 13
first life 00:33:47 -> 00:38:15   = 4 min 28 s clean
then       00:38:15 -> 00:39:23   = five lockups in 68 s
```

Against 13 boots in 210 s shipped, that is roughly a threefold drop in rate and
a longest-clean-life of 4 min 28 s against ~15 s - a real improvement, well
outside noise, and **not the bar**. The trampoline is necessary and is not
sufficient.

`radiant/tests/gate`: 24/24 on hardware, including
`test_signal_callback_defers_through_the_trampoline`, which asserts the routing
(the fake trampoline counts pends per producer). The link carries the other half
of the check: the gate no longer references `k_work_submit()` at all, so
reaching for the kernel from the callback again is a visible edit.

## What is still wrong in the Matter arm

### The bisection: it IS radiant's arbitrated path, and nothing else

`CONFIG_RADIANT_GATE_MPSL_NO_SESSION=y` is the gate's own documented bisection
aid: the gate builds and boots, the RADIO is initialised, the HFXO request is
taken - but no timeslot session is ever opened, so MPSL never delivers a signal
and **nothing of radiant's ever runs in the zero-latency context**. Everything
else in the arm is untouched: CHIP, the SoftDevice Controller, OpenThread, the
same RAM.

`bench-logs/m_nosession.log`, same board, same recipe plus that one symbol:

```
boots: 1        lockups (reset cause 0x100): 0
alive 00:04:26 -> 00:14:18 continuously = 9 min 52 s, self-channels rotating
```

Against 13 lockups in 620 s with the session open. So the lockups are caused by
radiant's arbitrated radio path running in a zero-latency interrupt. **The
SoftDevice Controller on its own, RAM pressure and CHIP are all exonerated as
sufficient causes** - they are present and quiet in this run.

### But the ring is not the whole of it, and it is not SPSC

Two things checked before spending anything on a lock-free ring, and both came
back against it.

**There is more than one producer.** `radiant_event_post_channel_event()` is
called from `api_sched_done()` (`apps/common/ant_radio_radiant.c:2398`), and
that callback is reached from two different contexts:

* `deliver_terminal()` in the RADIO signal callback - the zero-latency one;
* the gate's `denied_work_fn()` -> `radiant_nrf_gate_on_denied()` ->
  `deliver_terminal()` - the system work queue, a thread.

So the ring is multi-producer, single-consumer. A textbook SPSC ring is not
correct here; making it safe would need a CAS-reserved slot with per-slot
sequence numbers (Vyukov-style), which is a different and much less obvious
piece of work than "atomic head/tail".

**And the ring is only the visible part.** `api_sched_done()` does not merely
post an event - it runs the core's state machine: `radiant_channel_on_slot()`,
`radiant_sched` re-arming, `api_ch[]`, `api_xfer[]`, `api_stats`. Every one of
those is shared with the thread-context API surface, which guards itself with
`k_mutex_lock(&api_lock)` - and **a mutex cannot exclude a zero-latency
interrupt any more than `irq_lock()` can**. `radiant_event_crit_enter()` being
`irq_lock()` is the same fault in a second place, not a separate one.

A lock-free ring would therefore fix the ring and leave the scheduler and
channel state exactly as exposed as they are now.

### Fix 3: the (a) split - the core leaves the zero-latency context

Decided on the evidence above and built. `core_cb_rx()` / `_tx()` / `_ed()` in
`radiant_radio_nrf.c` are the new seam, and all six `radiant_op.cbs->` sites go
through them.

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
`k_is_in_isr()`: the trampoline's own handler is an ISR too.

That also collapses the multi-producer problem. The queue has exactly one
producer (the MPSL signal callback, which cannot preempt itself - MPSL delivers
START/RADIO/TIMER0 from one priority) and one consumer (the trampoline handler,
strictly below it). Genuinely SPSC, so a plain ring with a producer-owned head
and a consumer-owned tail is correct.

**The body is copied.** `struct radiant_rx_event::body` is documented
"valid only for the duration of this callback", and the callback now happens
later, so the entry carries its own 32-byte copy and the pointer is repointed at
dispatch.

**Overflow is counted, not hoped about.** Eight entries; a completion that finds
no room increments `cb_dropped`, which is published through
`radiant_nrf_win_diag` and printed as `cbdrop=dropped/high-water`. A dropped
completion is a window the core never sees finish - a channel that quietly stops
rather than a crash - so it needed a number rather than a comment.

**A trap this found in the header:** `gate_in_signal()` was first declared inside
the `CONFIG_RADIANT_SWEEP_DEBUG` block. The bridge arm has that on and compiled;
the Matter arm does not and failed on an implicit declaration. Had `-Werror` not
caught it, it would have been an int-returning guess and a completion queue
bypassed exactly where it was needed most. The declaration is now outside.

### Results of the (a) split

**Matter arm** (`bench-logs/m_asplit.log`, the `matter_final` recipe unchanged):

```
alive 00:52:03 -> 01:04:43 continuously = 12 min 40 s
boots: 1 (the flash)     lockups (reset cause 0x100): 0
self-channels rotating normally throughout
```

Against 13 lockups in 620 s with the trampoline alone, and 13 in 210 s shipped.

**Bridge arm** (`bench-logs/ts_asplit.log` 600 s, `ts_cbdrop.log` a further
420 s): 0 boots, 0 faults in either.

```
seam: grant=17709 nostage=0 prog=17709/0 | sigrad=660 isr=660 | ramp=17709
gate: placed=17711 granted=17710 blocked=0 near=0 long=0 dead=0 | bad over=0 inval=0
seam: grant=12778 nostage=0 prog=12778/0 cbdrop=0/1 | ramp=12778
```

**The latency it cost, measured.** The counters that would show a completion
arriving too late for the core to re-arm in time, before (trampoline only,
`ts_swi.log`) and after (`ts_asplit.log` / `ts_cbdrop.log`):

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

**Frames.** Like for like, and stated as a count rather than a rate: `sw rx=2`
in 560 s before the split, `rx=3` in 600 s after, `rx=2` in the further 420 s.
All three are low for the same reason - the ANT+ master that was in the room for
the earlier `rx=266` run is no longer transmitting - and the immediately
preceding build measured the same, so this is the room and not the split.
Frames still arrive in tracked windows; that is the whole claim.

### On the degeneration shape

4 min 28 s clean, then five lockups in 68 s, looks like something accumulating.
It is weaker evidence than it appears: lives were 268, 8, 37, 15, 8, 96, 18, 12,
19, 8 s. Under a constant-rate (exponential, mean ~48 s) model the chance of one
sample exceeding 268 s is about 0.4%, so ~5% across 13 samples - unusual, not
decisive. The long life is also the confounded one: it is the first after a
flash, with settings storage in a different state from every later boot. Not
worth weighing much either way.

### RAM

94.14% on the Matter arm, from 92.48% before this work: +4342 B, of which
+2976 B is the deliberate `SYSTEM_WORKQUEUE_STACK_SIZE` 1120 -> 4096 and the
trampoline's own state is about 50 B. It still fits, but
`docs/matter-ram-budget.md`'s headroom is thinner and the work-queue stack is
where it went - knowingly, and reversible if that trade needs revisiting.

## Files

* `radiant/include/radiant/radiant_swi.h`, `radiant/src/radiant_swi.c` - the
  trampoline.
* `radiant/src/radiant_radio_nrf_gate_mpsl.c` - the BUG 27 note, the gate's
  routing, `deadline_apply()`, the `stacks:` probe.
* `apps/common/ant_radio_radiant.c` - `radiant_event_wakeup()`, the one that
  mattered.
* `apps/dongle_thread/thread.conf` - the work-queue stack, with the reason.
* `radiant/tests/gate/fake_swi.[ch]`, `src/test_gate.c` - the routing test.
* Logs: `matter_boot1.log`, `m_wqstack.log`, `m_zlifix.log` (the reverted poll
  thread), `ts_zlifix.log`, `ts_reverted.log`, `m_swi_soak.log`, `ts_swi.log`.
