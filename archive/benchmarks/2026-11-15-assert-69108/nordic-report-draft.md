# MPSL assert 69:108 from a timeslot user: cause and fix (draft for DevZone 128550)

Status: draft, not posted. For DevZone thread 128550 (same `69, 108` reported
from the ESB timeslot code beside BLE) or a private ticket.

## Summary

`MPSL ASSERT: 69, 108` fires when a RADIO interrupt is taken after a timeslot
has ended and no other radio user holds the radio. A timeslot user that
returns `MPSL_TIMESLOT_SIGNAL_ACTION_END` while the RADIO IRQ is pending in the
NVIC triggers it. Clearing the application's RADIO interrupt enables before
returning END is not enough: the NVIC pending bit stays latched.

Clearing the pending bit as the last radio action before returning END removes
the assert. Nordic's own 802.15.4 driver already does this at its hand-over
(`nrf_802154_trx.c`: `nrf_radio_reset()` followed by
`nrf_802154_irq_clear_pending()`). The timeslot API documentation does not
mention it, and the ESB timeslot code in thread 128550 may have the same gap.

## Setup

- nRF54L15 DK, nRF Connect SDK v3.4.0, `libmpsl` soft-float for nRF54L.
- Application: a proprietary 2.4 GHz protocol using two MPSL timeslot sessions,
  beside OpenThread (nRF 802.15.4 radio driver) as an attached MTD child.
- Workload: one session receives in 10 ms timeslots back to back (a scan); the
  other holds a short periodic receive slot.
- The application programs TIMER0 CC0 to end each timeslot 750 us before its
  end and returns `ACTION_END` from `MPSL_TIMESLOT_SIGNAL_TIMER0`.
- The receiver is still running when that compare fires. The handler clears its
  RADIO `INTENSET` bits (END, DISABLED), clears SHORTS, disables the radio and
  returns END.

## What happens

The RADIO END or DISABLED event of the running receiver can fire while the
TIMER0 signal handler is executing. TIMER10 and RADIO_0 have the same priority
on nRF54L15, so the RADIO_0 interrupt only becomes pending. The handler then
clears `INTENSET00`, which drops the interrupt line but not the NVIC pending
state. After `ACTION_END` returns and MPSL closes the timeslot, the pending
RADIO_0 interrupt is taken by `MPSL_IRQ_RADIO_Handler` with no timeslot open,
and MPSL asserts.

## Evidence

State captured in the assert handler, identical in three runs of the unfixed
image (asserts at 102 s, 185 s and 224 s after boot):

- `IPSR = 154`: RADIO_0 (IRQ 138) is the executing interrupt. The return
  address passed to the assert handler and four further return addresses on the
  stack are inside `libmpsl`; the preempted context was the idle thread.
- 18-19 us since our handler returned `ACTION_END` from `SIGNAL_TIMER0`.
  TIMER0 count 9293, CC0 9250, timeslot length 10000 us.
- No `MPSL_TIMESLOT_SIGNAL_RADIO` was delivered to the application between the
  END and the assert.
- RADIO `STATE = 0` (disabled), `SHORTS = 0`, every RADIO DPPI
  subscribe/publish register 0. TIMER0 `INTENSET = 0`.
- No `mpsl_timeslot_request()` or session call in flight (last one returned
  25 ms earlier).
- HFXO running and tuned: `XO.STAT = 0x10001`, `EVENTS_XOTUNED = 1`,
  `EVENTS_XOTUNEERROR = 0`, `EVENTS_XOTUNEFAILED = 0`. So this is not one of
  the "HFXO failure" asserts listed in `mpsl_asserts.h` (`67, 108` is listed;
  `69, 108` is not).

With the fix, the application counts how often the RADIO_0 pending bit was set
at the moment it cleared it. Three 30 minute runs of the fixed image under the
same workload:

| run | timeslots ended by END | pending bit found set | asserts |
|---|---|---|---|
| 1 | 65 554 | 14 | 0 |
| 2 | 65 561 | 18 | 0 |
| 3 | 65 567 | 11 | 0 |

That is one pending bit per roughly 2 to 3 minutes, which matches the unfixed
image's time to assert (1.5 to 6.3 minutes over eleven runs). The unfixed image
asserted in a control run in the same sitting.

The assert does not need the second protocol: the same application without the
802.15.4 driver asserted once in earlier testing.

## The fix on the application side

```c
/* In the timeslot signal callback, on every path that returns
 * MPSL_TIMESLOT_SIGNAL_ACTION_END from a high-priority signal, after the
 * RADIO interrupt enables are cleared and the radio is disabled: */
NVIC_ClearPendingIRQ(RADIO_0_IRQn);   /* RADIO_IRQn on nRF52/nRF53 */
```

It is done only inside the timeslot, where the RADIO and its interrupt belong
to the application.

## Questions for Nordic

1. Is `69, 108` "RADIO interrupt with no radio owner / no timeslot open"? Can
   it be added to `mpsl_asserts.h`?
2. Should `mpsl_timeslot.h` state that the application must leave no RADIO (or
   TIMER0) interrupt pending in the NVIC when it returns `ACTION_END`, or
   should MPSL clear the pending RADIO interrupt itself when it closes a
   timeslot?
3. Does the ESB timeslot integration clear the pending RADIO interrupt
   before it ends its timeslot? The report in this thread shows the same
   assert on nRF54L10 and nRF52840.
4. The symmetric case: a TIMER0 compare that fires while a
   `SIGNAL_RADIO` handler returns `ACTION_END` leaves the TIMER0 interrupt
   pending. Does MPSL tolerate that?
