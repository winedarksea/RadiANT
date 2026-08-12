# 0013 — The sweep is the elastic consumer; tracked slots are inviolate

Date: 2026-08-12
Status: accepted

## Context

`radiant_core` owns the RADIO peripheral outright today. Two products need it
not to: a combined USB + Thread/Matter dongle
(`docs/radiant-bridge.md`), and a strap advertising the SIG Heart Rate Service
while broadcasting ANT+ `0x78` (`docs/backends.md` §, "a v1 requirement, not a
someday"). Both mean sharing the radio with a second protocol stack through
MPSL's timeslot API, which makes the question "what gives way when two stacks
want the air at the same instant?" a design decision rather than an
implementation detail.

**`docs/radiant-bridge.md` §7.2 already answered it, and the answer does not
survive contact with Zwift.** §7.2 observes that a wildcard sweep is ~100 %
radio duty, concludes that it *"cannot coexist with Thread"*, and resolves the
conflict by declaring that *"scanning is a bounded pairing state, not a running
state"* — enforced by having the sink layer refuse to arm a sweep while a Thread
session is up.

The USBPcap capture analysed on this project shows that premise is false.
**Zwift tears down and rebuilds a background-scan channel every 4–5 s, cycling
networks 0/1/2, for the entire session.** A combined dongle therefore has a
permanent scanner in it from the moment the ride starts. Under §7.2's rule
Matter would be dead for the whole ride, which is the same as not shipping it.

Two further facts constrain the answer, both established while designing the
arbiter and neither of them optional:

- **We cannot outrank the other stack.** `nrfxlib/mpsl/doc/timeslot.rst`
  states that applications should always request `PRIORITY_NORMAL`, and that
  other MPSL users — the SoftDevice Controller — hold priority levels above
  anything an application may request. `nrf_802154`'s multiprotocol
  documentation says the same from the other side: *"Bluetooth Low Energy has
  always priority over 802.15.4 protocols."* The two application-visible levels
  rank us against *ourselves*.
- **ANT has no retry.** `radiant_transfer.h` records that the retransmit path
  is deliberately unwired. On the USB dongle a delayed packet is a rider losing
  responsiveness mid-race, and a dropped tracked slot is loss that no layer
  above recovers.

## Decision

**§7.2 is amended rather than implemented.** The rule that a sweep and a second
stack cannot coexist is replaced by:

1. **A tracked slot is inviolate against our own work.** Tracked RX, master TX,
   and the acknowledged-data reply that may follow either of them are reserved
   as a unit (`follow_on_us`) and are never shortened, never displaced and never
   traded for another stack's convenience by anything `radiant_core` decides.
2. **The second stack's slice comes out of the search sweep.** The sweep is the
   elastic consumer: it fills the gaps, it yields when the arbiter says so, and
   it takes the whole cost of coexistence.
3. **Elasticity is discovered, not configured.** A scan chunk is granted a short
   timeslot and then extended repeatedly; MPSL grants an extension only if it
   has nothing else scheduled in the extended region, so the yield point is the
   arbiter's own answer rather than a Kconfig constant guessing at one. A fixed
   slice yields when it need not and overruns when it must not.
4. **Denial and elasticity are two different signals, and must stay two.**
   Being refused air is `RADIANT_RADIO_EDENIED` / `RADIANT_RADIO_STATUS_DENIED`
   (this ADR's companion mechanism). Running a granted window shorter than
   requested is a bound on the request — `caps.max_window_us` — applied where
   the scheduler can see it. Neither may be expressed as the other.

**The priority is delivered by shaping the other stack's demand, not by
outranking it**, because outranking it is not available. The levers are a long
BLE advertising interval, a long BLE connection interval (the in-tree
`esb_ptx_ble` sample runs at 1 s for exactly this reason), a sleepy Thread role,
802.15.4 `TERM_NONE` and late coex request modes, and ANT+ suppression while a
BLE connection is live. All five were chosen for other reasons already; this
ADR makes them the mechanism.

## The two dead ends this fences

Both were measured on this bench, and both currently survive only in a code
comment (`radiant_api.c`, `api_sched_done()`) and a test name. They are recorded
here because both are things a future reader will re-propose, and because the
shape they share is the shape any third attempt will also have: **they made a
policy decision with a fact missing, and shortened the plan instead of supplying
the fact.**

1. **Do not bound the sweep's request in `api_post_search_window()` against the
   earliest tracked slot.** It looks equivalent to letting the scheduler bound
   the chunk and is not: it shortens the dwell even when the scheduler already
   knew about the tracked window and had bounded the chunk correctly. Measured:
   2–3 sensors found instead of 5, and no set-9 or set-30 device found in 90 s.
   `test_the_scan_keeps_finding_devices_while_another_channel_tracks` fails on
   it — a device on the air for twelve seconds went unreported.
2. **Do not chunk the search dwell.** Seven tests in `test_search.c` fail if it
   returns, and they are right.

**A third, added by this design and closed before it was built:** do not have
the MPSL front end silently shorten a granted window. `arm_rx_window()` stores
`s.armed_end = close` and notifies the search layer of the bounds it *asked*
for; `radiant_search_on_done()` then credits `s->t_close` whenever the window
ran to its close. So a 250 ms grant quietly run for 20 ms credits 250 ms of
dwell for 20 ms of listening — the set advances after 8 % of its dwell and
"certain within one sweep" becomes false with no counter moving. That is the
same class of defect as the two above, reached from the opposite direction:
they under-listened by shortening the plan, this over-credits by shortening the
grant behind the plan's back. `caps.max_window_us` is the fix, because it keeps
the scheduler's model of the world true.

## Consequences

- **The sweep gets slower on a combined build, and that is the intended cost.**
  It must be quantified rather than assumed: sets/s from
  `CONFIG_RADIANT_CORE_SWEEP_DEBUG` with and without the second stack, in P3.
- **`[gates.acquisition]`'s `max_absolute_s = 5.0` is now reachable by a
  deliberate design choice rather than only by a bug.** Re-acquisition of a
  known device is steered by the 16-entry, 60 s seen cache and should stay fast,
  but it is measured in P3. If it breaches, the fix is the slice size — never
  the gate.
- **A denied chunk must still count as "ran" for the purpose of keeping the
  sweep's request in the scheduler's slot**, even though it credits zero dwell.
  Excluding it sends every denial round the rate-limited pump, which is the same
  round trip that measured 12.8 s per sweep against 8.3 s — now paid hundreds of
  times per sweep instead of never.
- **A tracked channel that is denied must not report `EVENT_RX_FAIL`**, and must
  not count towards `RX_FAIL_TO_SEARCH`. A denial is evidence about us, not
  about the sensor. It is still charged to the window guard, because a period
  really did elapse with no fresh sync.
- **The combined build is not the default.** A Thread-free dongle build remains
  the safe Zwift default, and the exit criterion is written down: if the
  arbiter's own cost exceeds +1.5 pp of `loss (exact)` with no second stack
  attached, the combined build is abandoned in favour of the two-box handoff of
  `docs/radiant-bridge.md` §7.3.

## Status of the evidence

The Zwift scanner cadence is from a USBPcap capture of a real session and is the
load-bearing fact of this ADR — so P3's discovery gate is driven by a scripted
host replaying that open/close/network-rotation cadence, not by a static device
set. **A gate that never reproduces the permanent scanner never tests the
premise this decision rests on.**
