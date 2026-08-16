# P4: the 802.15.4 assert that was not a DPPI endpoint conflict

*2026-08-15, nRF54L15 DK (J-Link 1057737173), NCS v3.4.0.*

## The symptom

`apps/dongle_thread` built with `thread.conf;bridge.conf`
(`-DANT_RADIO=core -DRADIANT_BACKEND=nrf`) panics 4-45 s after boot:

```
[00:43:38.632,154] <err> os: ***** HARD FAULT *****
[00:43:38.632,163] <err> os:   Fault escalation (see below)
[00:43:38.632,168] <err> os: ARCH_EXCEPT with reason 4
[00:43:38.632,186] <err> os: r3/a4: 0x00000004 r12/ip: 0xaaaaaaaa r14/lr: 0x000179cf
[00:43:38.632,194] <err> os: Faulting instruction address (r15/pc): 0x000365a4
[00:43:38.632,215] <err> os: >>> ZEPHYR FATAL ERROR 4: Kernel panic on CPU 0
[00:43:38.632,219] <err> os: Fault during interrupt handling
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

## What it is NOT

It is not the RADIO ADDRESS DPPI endpoint conflict recorded in project
memory. That conflict is real and the per-grant endpoint swap in
`radiant_radio_nrf.c` (`radio_endpoints_attach()`, bug 20) is the fix for it;
it is fully built, not half-built. This fault survives with the swap
permanently inert - the seam probe in the failing run reports
`grant=0 prog=0/0`, i.e. `radiant_nrf_gate_on_grant()` was never entered and
no endpoint was ever swapped in - so the swap cannot be the cause.

It is also not a shortage of DPPI channels, nor a PPI leak: allocation
succeeds and `radio_ep_contended`/`radio_ep_mine_mask` are irrelevant on this
path.

## What it is (bug 24): shared RADIO registers written with no timeslot held

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
`nrf_802154`. Under `thread.conf` on this bench the arbiter refuses every real
reservation (see "still open" below), so the core runs a denial loop:
`term` climbs at ~80/s. Each of those ~80 writes a second lands on the
802.15.4 driver's live receiver; one eventually lands inside a frame.

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

## The fix

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

## Measurement (A/B/A on the same board, same session, 2026-08-15)

| arm | image | result |
|---|---|---|
| A (baseline) | guard absent | panic ~4 s into capture, `lr=0x179cf` |
| B | guard present | 420 s clean, `term=32336` (~32 000 previously-fatal writes exercised) |
| A' | guard forced off with `#define RADIO_IS_OURS() (true \|\| radio_on_loan)` | panic 26 s after boot, same `lr=0x179cf` |
| B' | guard restored | 600 s clean |

Logs: `bench-logs/dppi_repro1.log`, `dppi_fixed1.log`, `dppi_ab_A.log`,
`dppi_ab_B2.log`.

## RESOLVED, same day - see `docs/p4-timeslot-placement-lead.md`

**This section is kept because the counters below are the evidence that led to
the fix, not because anything in it is still open.** The blocker described here
was diagnosed and fixed within hours of this document being written: it was two
defects (bugs 25 and 26), of which the blocking one was `PLACE_LEAD_US` being
derived from *grant latency* (~1695 us, HFXO startup) when what the scheduler
actually needs is *placement lead* - how much notice it needs to fit a
fixed-instant slot in beside `nrf_802154` in continuous receive. The two differ
by an order of magnitude, which is exactly why the refusal was **inline**:
refused on its face rather than deferred.

Raising the lead to 24 000 us takes `blocked` to **0** and
`radiant_nrf_gate_on_grant()` from 0 entries to 8 380, and real ANT+ frames are
received. The regression is dated to bug 22's removal of `elastic_skew_us` -
correct in itself, but its side effect was 0-13 ms of extra placement lead and
that was the only thing feeding the gate.

The text below is the original "still open" write-up, preserved verbatim.

## Still open, and it blocks on-air verification separately (SUPERSEDED - see above)

With the panic gone the gate is visibly *not getting air* beside an
OpenThread MED. From the B run's own counters:

```
gate: acq=40430 placed=40421 granted=8085 blocked=32336 ... den anch=8084 ... inl=32336
seam: grant=0 nostage=0 prog=0/0 | sigrad=0 sigt0=0 isr=0
```

Every NORMAL reservation is refused, and answered *inline* from inside
`mpsl_timeslot_request()` (`inl=32336`). Four consecutive blocks invalidate
the anchor (`BLOCKED_RUN_REANCHOR`), the next acquire denies with
`den_no_anchor` and re-bootstraps, the bootstrap EARLIEST timeslot is granted,
and the cycle repeats - which is why `granted=8085` while
`radiant_nrf_gate_on_grant()` was entered zero times. RadiANT therefore gets
**no** real receive window in this build; every one of the 8085 grants was a
100 us anchor timeslot carrying no operation.

That is a separate defect from this one and is the next thing in the way of
Package G's on-air verification. It was previously masked: the board crashed
before anybody could read the counters.
