# Phase 0c: three MPSL facts (ADR 0018)

Measured 2026-11-10 on the nRF54L15 DK (J-Link `1057737173`), branch
`wip/adr18-0c`, spike code at `b1e76ad`, NCS v3.4.0. The spike is
`radiant/spike/mpsl_arb` (commands `a`, `b`, `c`; host driver
`run_facts.py`). Raw logs: [phase-0c-raw-logs.md](phase-0c-raw-logs.md).

The spike drives no radio. Every timeslot is granted, timed and handed back.
The second stack is the spike's own BLE advertiser (`ble.conf`, SoftDevice
Controller, 1.0-1.2 s interval). There is no 802.15.4 in the spike, so
nothing here says anything about Thread.

Both images hold the HFXO from boot (`hfxo_hold()`), as the gate does.
`.config` read back on both: `MPSL=y`, `MPSL_TIMESLOT_SESSION_COUNT=2`,
`MPSL_ASSERT_HANDLER=y`, `REBOOT=y`; BLE image also `BT=y`,
`BT_LL_SOFTDEVICE=y`, `BT_PERIPHERAL=y`. The BLE image's boot banner shows
`advertiser: 1.0-1.2 s`.

No MPSL assert, no OVERSTAYED and no INVALID_RETURN in any run.

## Verdicts

| Fact | No other stack | BLE advertiser | Verdict |
|---|---|---|---|
| F1: one timeslot extended past 100 ms | 80/80 grants reached 300 ms | 1 ms chain: 15/20 reached 300 ms in each of 2 runs; every refusal lands where the advertiser has an event | **PASS** |
| F2: two sessions in one client | open works; a request placed before session 1's grant covers it is granted 30/30, and session 1's extension is refused 0.2-0.4 ms before it; a request into air session 1 already holds is BLOCKED 30/30 | same shape; the few exceptions are BLE events | **Works, first come first served.** Priority does nothing. Session 1 is never cut |
| F3: ACTION_REQUEST at grant end, 50 us-3 ms gap | 0 blocked of 6000 (both HFCLK modes, 10 gaps each) | 0.3-1.0 % blocked, the same at every gap | **Short notice is not refused** |

## F1: extension past 100 ms

Session opened, EARLIEST 5 ms grant, NORMAL priority, XTAL_GUARANTEED.
`jit` runs grow it the gate's way: an MPSL_TIMER0 compare 2 ms before the
end asks for one more step. `b2b1` asks for the next step straight from
EXTEND_SUCCEEDED (the old spike pattern). Target 300 ms. 20 grants per row.

**No other stack** (`none_facts.log`):

| Run | Reached 300 ms | Ext. failed | Past 100 ms | 30 ms step OK | Drift over the hold |
|---|---|---|---|---|---|
| `jit1` (1 ms steps) | 20/20 | 0 | 20 | - | 1-2 us |
| `jit1_big40` (30 ms step at 40 ms) | 20/20 | 0 | 20 | 20/20 | 1-2 us |
| `jit1_big85` (30 ms step 85->115 ms) | 20/20 | 0 | 20 | 20/20 | 1-2 us |
| `b2b1` (back-to-back 1 ms) | 20/20 | 0 | 20 | - | 1-2 us |

Every grant held 298.0 ms of free-timer time (300 ms minus the 2 ms end
margin). MPSL_TIMER0 read 298 002-298 003 at the end, so it counts straight
through 100 ms with no reset or wrap.

**Drift** is the free-running TIMER21 elapsed minus MPSL_TIMER0 elapsed over
each hold: 0-2 us over 298 ms, in every run including BLE. Over each whole
row (about 6 s) the free timer and `k_uptime` agree within 0.2-0.7 ms, which
is `k_uptime_get_32()`'s 1 ms resolution.

**BLE advertiser**, two runs (`ble_facts.log`, `ble_facts_run2.log`):

| Run | Run 1: reached 300 ms / ext. failed | Run 2 | Refused at (total ms) |
|---|---|---|---|
| `jit1` | 15/20, 5 | 15/20, 5 | 82-89, 166; run 2 from 79 |
| `jit1_big40` | 15/20, 5 (30 ms step 20/20) | 11 of 15 visible reached 300, 4 failed (see note) | 83-193 |
| `jit1_big85` | 15/20, 5 (30 ms step 16 OK, 4 refused) | 15/20, 5 (15 OK, 4 refused) | 81-115 |
| `b2b1` | 4/20, 16 | 3/20, 17 | 9-88 |

Every BLE refusal is EXTEND_FAILED with the grant still live. It ends
cleanly; nothing overstays. A 300 ms hold against a 1.0-1.2 s advertiser
overlaps an advertising event about a quarter of the time, and 5 of 20
is that. Refusal points are spread over the hold, and one is at 166 ms and
one at 193 ms, so it is not a 100 ms rule that only shows up under load.

`b2b1` looks worse but is one event hit repeatedly. Its whole 300 ms is
asked for in under 0.5 ms (`held` 30-425 us at refusal), so each grant
fails at the next advertising event, ends at once, and the next grant
starts about 7 ms later. The refusal point then walks 7 ms nearer per grant
(79, 72, 65 ... 9 ms) until the event has passed. It counts the same event
up to 11 times. It is not a lower success rate per event.

Note: in run 2 the console lost bytes. 4 of 20 `jit1_big40` grant lines and
its summary line are gone, and one line is cut. The table counts the 15
complete lines. Run 1 has the full row.

**What it means.** MPSL's 100 ms ceiling is on the *requested* timeslot
length, not on the extended total. One grant can be held for a whole ANT+
period. The arbiter still refuses an extension exactly where the other
stack has the air, so the elastic model of ADR 0013 holds past 100 ms too.

## F2: two sessions in one client

Both sessions open at once (`session_open` rc 0 for both, ids 0 and 1, every
run). The header's "only one session can be open at a time" is not true in
v3.4.0 with `SESSION_COUNT=2`.

Session 2 is bootstrapped with one EARLIEST (1 ms, ended at once). Then per
iteration: session 1 gets an EARLIEST 5 ms NORMAL grant and grows by 1 ms
steps toward 90 ms. Session 2 asks for a 2 ms slot at session 1's start +
40 ms, with about 40 ms of notice.

- **A:** session 2's request is placed about 15 us after session 1 starts,
  when session 1's grant ends at 5 ms. Nothing covers 40 ms yet.
- **B:** session 1 first asks for one 60 ms step (to 65 ms). Session 2 is
  placed only after that step is granted, so 40 ms is already held.
- **A_step200:** as A with 200 us steps, to see how close session 1 may come.

**No other stack** (`none_facts.log`), 30 iterations each:

| Run | S2 prio | S2 granted | S2 blocked | S1 cut | S1 end | Handover (S2 start - S1 END) |
|---|---|---|---|---|---|---|
| A_high | HIGH | 30 | 0 | 0 | ext. refused at the step to 40.0 ms, 30/30 | 3.00 ms (2996-3015 us) |
| A_normal | NORMAL | 30 | 0 | 0 | same, 30/30 | 3.00 ms (2994-3015 us) |
| B_high | HIGH | 0 | 30 | 0 | reached its 90 ms cap, 30/30 | - |
| B_normal | NORMAL | 0 | 30 | 0 | same, 30/30 | - |
| A_high_step200 | HIGH | 30 | 0 | 0 | granted to 39.6 ms (29) or 39.8 ms (1) | 2.39 ms (2215-2398 us) |

Session 2's start error against its asked-for instant: 0-17 us.

In B, session 2's BLOCKED arrives about 28 us after the request, not near
the slot. It is refused at placement.

**MPSL's own guard** between session 1's last granted end and session 2's
start is 0.2-0.4 ms (A_step200: 39.6 ms granted, 40.0 ms start, 29 of 30).
The rest of the 2.4-3.0 ms handover is this spike's 2 ms end margin (the
gate's `END_MARGIN_EXTEND_US`) plus step granularity. A design that ends
session 1 closer to its granted end gets the gap down toward 0.4 ms.

**BLE advertiser**, two runs:

| Run | Run 1: granted / blocked / skipped | Run 2 |
|---|---|---|
| A_high | 29 / 1 / 0 | 29 / 1 / 0 |
| A_normal | 30 / 0 / 0 | 30 / 0 / 0 |
| B_high | 0 / 24 / 6 | 0 / 26 / 4 |
| B_normal | 0 / 26 / 4 | 0 / 25 / 5 |
| A_high_step200 | 30 / 0 / 0 | 30 / 0 / 0 |

"Skipped" means session 1's 60 ms step was itself refused (an advertising
event inside it), so session 2 was not placed. The two A_high blocks are
the advertiser, not session 1: session 2 was BLOCKED at placement (about
26 us after the request), and session 1 then extended past 36-40 ms and was
refused one step later, where the BLE event was. In A_normal the outliers
are session 1 refused early (at 6-21 ms) by a BLE event, with session 2
still granted on time.

**What it means.** Our two sessions are not ranked against each other. MPSL
schedules whichever claim on the air arrives first. HIGH versus NORMAL made
no difference in 300 comparisons. A held grant is never cut. A tracked
request placed while the scan grant ends short of it is granted every time
and stops the scan's extension 0.2-0.4 ms before the tracked start.

## F3: ACTION_REQUEST at a grant's end

One session, 5 ms NORMAL grants. A compare 0.75 ms before the end returns
ACTION_REQUEST for the next 5 ms grant, `distance_us` = MPSL_TIMER0 now +
gap, so the next start is `gap` after the close. On BLOCKED the thread
re-requests at a comfortable distance, and the next grant tries again. 300
short-notice attempts per row.

**No other stack:** every row 300/300 granted, 0 blocked, for gaps 50, 100,
200, 300, 500, 750, 1000, 1500, 2000 and 3000 us, with both
`XTAL_GUARANTEED` and `NO_GUARANTEE`. 6000 attempts, 0 blocked. Start error
0-41 us, mean 1-2 us.

**BLE advertiser**, blocked of 300 per row:

| Gap (us) | Run 1 xtal | Run 1 noguar | Run 2 xtal | Run 2 noguar |
|---|---|---|---|---|
| 50 | 1 | 1 | 1 | 1 |
| 100 | 2 | 1 | 2 | 2 |
| 200 | 1 | 1 | 1 | 1 |
| 300 | 2 | 1 | 2 | 2 |
| 500 | 1 | 1 | 1 | 1 |
| 750 | 2 | 2 | 2 | 2 |
| 1000 | 1 | 1 | 2 | 2 |
| 1500 | 1 | 2 | 1 | 1 |
| 2000 | 2 | 2 | 1 | 2 |
| 3000 | 3 | 3 | 2 | 3 |

That is 0.3-1.0 %, flat across gaps. Each row lasts about 1.7 s, so it sees
one or two advertising events, and each one blocks a grant. The rate is the
advertiser's duty, not the notice. Every BLOCKED arrives at the request
instant (it is early by the gap minus about 31 us). So the refusal is known
at once, not after the slot would have started.

**What it means.** With nothing else on the air, MPSL takes a request with
50 us of notice. The 24 ms `PLACE_LEAD_US` is not something MPSL needs for
our own chaining. It was swept for 802.15.4 (bug 26). This spike cannot say
whether Thread refuses short notice; bug 26 saw about 80 % refused at 2.5 ms
notice there.

## Pruning, as the plan defines it

- **F1 passes, so (a) becomes a full fix and its investigation moves first.**
  One grant can span a whole 246 ms period. The ADR's "about 21 % deaf,
  drifting" arithmetic for (a) came from the 100 ms ceiling, which does not
  exist. With the period held as one grant, the bands at the tracked slot go
  to the two in-grant handoffs, and the mid-period boundary goes away.
- **F2 does not show the tracked request blocked or cut in the (d2) layout,
  so (d2) is not dropped.** In layout A, which is (d2)'s design (scan grows
  just in time, tracked request placed with full notice), the tracked request
  was granted 60/60 with no stack and 59/60 and 59/60 under BLE (each miss was
  a BLE event). It *is* blocked in layout B, 30/30, when the scan's grant
  already covers the tracked instant. So (d2) carries one rule: the scan must
  not extend over a tracked start before that start's request is placed. A
  big bridging extension, which is (a)'s tool, breaks (d2); the two do not
  combine naively. Priority cannot rescue it.
- **F3 is not mostly BLOCKED with no stack (0 of 6000), so (d1) is not
  dropped.** Its open question moves to Thread.

Suggested order for Phase 1: (e), then (a) (now a full fix), then (d1) and
(d2), as the plan says when F1 passes. (e) still sets the lead the others
assume and is R1's lever, and F3 says the lead is set by the other stack
alone.

## Confidence, and what would change each verdict

- **F1: high** that MPSL accepts past 100 ms with no stack (80/80, drift
  1-2 us, clean refusals under BLE). **What would change it:** an MPSL
  version where extending past 100 ms asserts or fails; Thread refusing
  long holds much more often than BLE does (not measured here); or a real
  radio inside the grant behaving differently from an idle one (the spike
  never touches RADIO). Also open: whether a grant reserved far ahead (like
  `b2b1`) makes the SoftDevice drop advertising events scheduled later.
  The spike does not count the advertiser's own losses. The gate's
  just-in-time chain reserves only 2 ms ahead, so it is not exposed.
- **F2: high** for the shape (300 comparisons per image, no exceptions not
  explained by BLE). **Medium** on the 0.2-0.4 ms guard, which is measured to
  200 us steps only. **What would change it:** a tracked slot that must be
  placed while a bridging extension already covers it (layout B), or Thread
  behaving differently.
- **F3: high** with no stack and with BLE. **Not measured under Thread.**
  **What would change (d1):** Thread blocking short-notice chains at bug 26's
  rate. That is Phase 1's (d1) measurement.

## Deviations and things not done

- Extra runs beyond the plan: a 30 ms step that straddles 100 ms (85 -> 115),
  the back-to-back chain, NORMAL-priority controls for session 2, a 200 us
  step variant of F2 layout A, F3 gaps of 50 and 100 us, and F3 with
  `NO_GUARANTEE`. Each answers an edge of the same question.
- F2 layout B (session 2 into air already held) is not in the plan's text.
  It was added because it is the case a bridging extension creates.
- An earlier image (`img1_*` logs, same code without the 200 us variant and
  the 50/100 us gaps) gave the same F1/F2/F3 numbers with no stack. It is
  kept, not used in the tables.
- The assert handler now reboots after printing instead of halting, so an
  unattended run would survive one. It never fired.
- The session count in the spike's `prj.conf` went from 1 to 2.
- Console byte loss: a few lines in `ble_facts_run2.log` and one line in each
  of `img1_none_F2.log` and `img1_none_F3.log` are cut. No table depends on a
  cut line except where noted.
- Zero-cost: no file outside `radiant/spike/mpsl_arb` and `docs/` changed
  against `be39a7d`, and no build includes the spike (it is referenced only in
  comments in `radiant/Kconfig`). `check_zero_cost.ps1` was not run: on a host
  short of commit memory, building 16 images to compare files no image
  compiles was not worth it.
- Not done: Thread, the other stacks, and anything with the radio on. The
  plan puts those in Phase 1.

## Unexplained

- Why the console drops bytes now and then (seen on both images, at 115200
  with DTR, through the J-Link OB VCOM). It costs log lines, not results.
