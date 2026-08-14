# Backends, transports and build targets

Checked by: [`scripts/build_all.ps1`](../scripts/build_all.ps1), which asserts
the link address and the compiled transport for every target in the matrix, and
gains a third assertion (`CONFIG_ANT_DONGLE_RADIO_<X>=y` in `.config`) and a
fourth (a build with `ANT_MODULE_DIR` pointing at a path that does not exist)
when the backend seam lands. Until then, everything under *Radio backends* and
*HAL contract* is design, not shipped code — the transport, build-target and
optional-feature material below is the shipping behaviour and is asserted.

**The MPSL gate exists, receives, and coexists with a running SoftDevice
Controller — and it fails its own cost gate by a factor of thirty.** Those are
three separate claims and only the first two are good news; see *`loss (exact)`,
measured at last* immediately below before quoting anything from this page.

**And it now coexists with Thread too, measured.** Beside an attached,
transmitting OpenThread node the ANT channel first would not acquire at all
(0 of 961 packets in both the MED and SED roles); five defects later it measures
**0.09 % loss in MED and 0.30 % in SED against 0.30 % for the same build with no
second stack** — a delta of −0.21 pp and 0.00 pp against a +0.5 pp bar, and
`[gates.coexistence]` PASSES in both roles. See
[`radiant-bridge.md` §7.4.1](radiant-bridge.md#741-first-measurement-2026-08-13--the-gate-fails)
and [§7.4.2](radiant-bridge.md#742-five-defects-in-series--and-the-gate-now-passes).
[`radiant_core/src/radiant_radio_nrf_gate.h`](../radiant_core/src/radiant_radio_nrf_gate.h)
and its direct implementation are compiled into every `-DRADIANT_BACKEND=nrf`
build; [`radiant_radio_nrf_gate_mpsl.c`](../radiant_core/src/radiant_radio_nrf_gate_mpsl.c)
is selected by `CONFIG_RADIANT_CORE_BACKEND_NRF_GATE_MPSL`, off by default.

### `loss (exact)`, measured at last — and the arbiter fails its own gate

There is now a driven master on this bench, so the A/B the gate below specifies
can actually be run. It does not need the `sim/` application and it does not need
a second DK: flash a **Feather nRF52840 with the ordinary dongle image** and
drive it with [`tools/ant_sim.py`](../tools/ant_sim.py). The firmware is a
transparent bridge and will be an ANT+ master when a host asks it to be, which is
what that tool exists for. Receiver is the nRF54L15 DK; both ends 45–60 s at
4 Hz, −16 dBm:

| build | `loss (exact)` |
|---|---|
| direct | **0.00 %** — 0 of 179 messages missing |
| MPSL gate, no second stack, *before* the fix below | **50.6 %** — 121 of 239 missing |
| MPSL gate, no second stack, **after** | **0.00 %** — 600 of 600 received over 149.5 s |

The plan's stated exit criterion is *abandon the combined build if the arbiter
alone costs more than 1.5 pp*. It was **+50 pp**; it is now **~0 pp**, and P3
passes its own gate. Arrival jitter came down with it, from 22.9 ms stddev to
3.7 ms, which is the direct build's figure. The direct column also establishes
that this bench has essentially no floor of its own, which is what makes the
others trustworthy.

**It is diagnosed, and the arbiter is not the one refusing.** MPSL grants every
single window it is asked for: `placed=94 granted=94 blocked=0`. The loss is
self-inflicted, and it took a new counter to see it, because with only `placed`
to read *a gate that is never called* and *a gate that is working perfectly* are
the same four numbers. Counting calls **in** as well as requests **out**:

```
gate: acq=184 in_grant=91/91 placed=94 granted=94 blocked=0 ...
```

184 acquires in 45 s is 4.1/s — exactly the master's rate, so the core is asking
for every slot. Of those, **91 arrive while a grant is still held and all 91 are
refused by us**, at the `return GATE_DENIED` in `gate_acquire()`'s
already-granted branch. Perfect alternation, hence exactly half.

The sequence is the arm/air seam again. A tracked window's packet arrives
mid-window; the receive callback immediately arms the next tracked slot 250 ms
out — that is the low-jitter path the HAL contract exists to permit. But the
current timeslot is still live (`gate_release()` runs from `deliver_terminal()`,
which has not happened yet), so `gate_acquire()` takes the "a grant is already
held" branch, asks whether a window a quarter of a second away fits inside a
5.5 ms grant, and refuses it. The core loses that slot; the next scheduler pump
re-posts and lands on the *following* one.

That branch exists for the acknowledged-data reply, which really does have to be
served out of the reserve it was granted with, and for that it is correct. What
is wrong is the else: a request that starts *after* the current grant ends is not
a reply that failed to fit, it is an ordinary future reservation, and it must be
**placed** rather than denied.

**Fixed.** The request is now staged in `next_grant` and committed by
`request_work_fn()` once `mpsl_timeslot_request()` has accepted it, because the
placement path writes `g.grant_end`, `g.granted_len_us` and `g.extendable` — all
of which describe the *next* grant and would retune the live one's end and
extension arithmetic if written straight through `g`. The acknowledged-data reply
keeps its synchronous refusal: at ~1.56 ms out it is inside `PLACE_MIN_US`, so it
is still turned away by the too-near test, which is the same answer by the road
that describes it. The counter stays, now reading "arms that had to be placed
rather than served from the reserve" — a real property of the traffic, and the
thing to watch if the reserve is ever resized.

**An earlier reading of this page said the cause was MPSL placement jitter
against an 800 µs window. That was wrong** and is corrected here: P0 measured the
anchor rule at 0–17 µs over 640 chained requests, and the radio's own timestamps
on received packets are 0.013 ms off the slot grid. Nothing about MPSL's
placement is loose. The 800 µs window is simply the direct build's 238 µs plus
`HEAD_MARGIN_US` and `TAIL_MARGIN_US`, and it is not implicated.

**The previous result on this page read "direct 30 / MPSL gate 30 / MPSL+BT 30
packets in 60 s" and is withdrawn as a yield comparison.** Thirty packets in
sixty seconds is 0.5/s against a 4 Hz power meter: it was a sensor of
opportunity, heard at whatever rate the room offered, and an equal number on both
sides of it means only that both builds heard the same weak thing. It concealed a
50 pp defect completely. The lesson is the one the gate config already encodes —
`loss (exact)`, counted against the transmitter's own event counter, is the only
number that means anything here.

The arbitration counters from that sitting are still worth keeping: 273
reservations placed, 272 granted, **0 blocked**, 2868 extensions, zero refusals,
scheduler `end ok=36 abrt=0 miss=0 fail=0 deny=0`. The arbiter does what it says;
what it costs is the table above.

One structural cost IS visible and is by design: a scan chunk is 94 200 µs under
the gate against 260 000 µs direct, because `caps.max_window_us` bounds a window
to what MPSL can grant, so the sweep needs about three times the chunks for the
same dwell.

**The sweep-rate cost has now been measured, and the number that matters is duty
rather than chunk count.** Sixty seconds searching for a device number that is
not on the air — a wildcard channel finds the sensor in the room within a second
or two and stops sweeping, so a wildcard run measures acquisition and not the
sweep:

| build | chunks/s | air time |
|---|---|---|
| direct | 3.78 | 97.7 % |
| MPSL gate, no second stack | 10.94 | 94.8 % |

The chunk rate nearly triples and means nothing on its own — the chunks are a
third the length. **The sweep spends 94.8 % of wall-clock on air against the
direct build's 97.7 %, a ratio of 0.970** against the 0.5 floor in
`[gates.coexistence] min_sweep_rate_ratio`. Arbitration costs the sweep about
3 % of its air with nothing else on the radio.

That is the arbiter's own cost. **The ratio with a second stack actually running
is not yet measurable, and the reason is an open bug rather than a missing
run.**

### The coexistence bug, found and fixed

`CONFIG_ANT_DONGLE_BLE_COEX_LOAD=y` starts a BLE advertiser beside ANT+ so that
the gate has a second stack that wants the radio. Built that way, the
SoftDevice Controller used to assert:

```
<err> bt_sdc_hci_driver: SoftDevice Controller ASSERT: 48, 1792
```

within about 4–18 ms of the first advertising event, **with no ANT+ channel ever
opened**, and reboot into doing it again. **The cause was a single line in
`radiant_radio_init()` and it is fixed**; see *What it actually was* below. The
elimination table is kept because every entry in it is a plausible half-day that
nobody should spend again, and because the shape of the mistake is worth having
written down: ten correct experiments, each one individually sound, all of them
looking at run time when the fault was at boot.

- **It is ours, not the platform's.** Zephyr's stock
  `samples/bluetooth/peripheral` advertises perfectly on this exact board and
  SDK. The reference build is the first move on any "does the controller work
  here" question, exactly as `nrf/samples/mpsl/timeslot` was for the timeslot
  API.
- **It is not starvation.** With the ANT+ channel never opened — one 100 µs
  bootstrap timeslot and nothing since — it still asserts. The elastic sweep
  giving way is not the missing piece.
- **It is not an ordering problem at init.** With the load deferred eight
  seconds (`CONFIG_ANT_DONGLE_BLE_COEX_LOAD_DELAY_S`), ANT+ sweeps normally for
  those eight seconds and the controller asserts 114 ms after it starts.
- **It is not the application.** `-DRADIANT_BACKEND=null` — same image, same
  MPSL, same controller, same advertiser, no radiant radio — runs clean. **This
  is the one clean configuration and every other difference is measured against
  it.**
- **It is not arbitration.** `CONFIG_RADIANT_CORE_GATE_MPSL_NO_SESSION=y` builds
  the arbitrated backend and never opens a timeslot session. It still asserts.

Beyond that, each of the following was removed individually from an asserting
build and **the assert survived every one**, so none of them is the cause. They
are listed because each is a plausible half-day, and because the switches that
tested them are kept:

| removed | switch |
|---|---|
| the permanent HFXO request | `RADIANT_CORE_NRF_NO_HFXO_HOLD` |
| all five (D)PPI allocations | `RADIANT_CORE_NRF_NO_GPPI` |
| enabling MPSL's own RADIO_0 NVIC line | `RADIANT_CORE_NRF_NO_RADIO0_IRQ` |
| radiant's TIMER moved off TIMER20 onto TIMER22 | devicetree overlay |
| the RADIO `PUBLISH`/`SUBSCRIBE` endpoints, detached at init | (reverted) |
| the RADIO interrupt mask left set between grants | fixed and kept |
| enabling RADIO line 1, which is not ours | fixed and kept |

**P0's "TIMER20 does not collide" is a statement about MPSL, not about the
SoftDevice Controller** — MPSL's `BUILD_ASSERT`s reserve TIMER10 on this part
and nothing else, and the controller ships as a binary that asserts nothing at
build time. That gap was real and worth closing; TIMER22 behaves identically, so
the choice of timer is not the problem either.

#### What it actually was

Two things broke the deadlock, and neither was another peripheral.

**The experiment that pointed the right way** was setting *all four* diagnostic
switches at once rather than one at a time. That build has no HFXO request, no
(D)PPI, no RADIO_0 `irq_enable()` and no timeslot session — and it still
asserted, twelve milliseconds into `bt_enable()`, before a single advertising
packet. Re-reading the failing captures with that in hand, 54 of the 55 asserts
in a 3000-line log had `chunks=0 end ok=0`: **no timeslot had ever been granted
and no radio operation had ever been programmed.** Everything the gate does at
run time was therefore already excluded by the evidence on hand, and had been
for some time. The single-variable cuts were each sound and all of them were
aimed at the wrong half of the program.

**The cause** is one line, and it is not what it looks like:

```c
IRQ_CONNECT(RADIANT_RADIO_IRQn, CONFIG_RADIANT_CORE_BACKEND_NRF_IRQ_PRIO,
            radio_isr, NULL, 0);
```

`IRQ_CONNECT()` is not only an ISR-table entry. On ARM it also expands to a
`z_arm_irq_priority_set()` call that runs *where it is written* — here, inside
`radiant_radio_init()`, well after MPSL's `SYS_INIT`. And `RADIANT_RADIO_IRQn`
is `RADIO_0_IRQn`, which is MPSL's: `mpsl_init.c` registers it with
`IRQ_DIRECT_CONNECT` under `IRQ_ZERO_LATENCY`, at a priority `BASEPRI` cannot
mask. Connecting the same line afterwards put it quietly back to an ordinary,
maskable one.

From that moment every `irq_lock()` in the system — ours, the kernel's,
anybody's — could hold off the SoftDevice Controller's radio interrupt. Nordic's
own account of this assert is that MPSL was *"prevented from doing a task on
time … by other same or higher priority threads or interrupts"*, and demoting
its interrupt out of zero-latency is the most thorough way there is to arrange
exactly that.

This is also why `RADIANT_CORE_NRF_NO_RADIO0_IRQ` did not find it: that switch
skips `irq_enable()`, and the damage was done by `IRQ_CONNECT()` — a different
line, doing a different thing, one screenful away. The elimination was
incomplete rather than wrong, which is the most expensive kind.

Under the gate this file now connects **neither** RADIO line. Nothing is lost:
inside a grant MPSL owns the vector and hands us RADIO events as
`MPSL_TIMESLOT_SIGNAL_RADIO`, which the gate forwards to the same handler;
outside a grant the radio is not ours and an interrupt from it is not ours to
take. The NVIC line itself is still *enabled* — `irq_enable()` is
`NVIC_EnableIRQ()` and touches no priority — because with
`CONFIG_MPSL_DYNAMIC_INTERRUPTS` off MPSL uses the static `IRQ_DIRECT_CONNECT`
path, which never enables anything, and `mpsl.rst` makes the timeslot user
responsible for precisely that. Dropping line 1's connection closes a second
hole for free: the controller's own RADIO_1 interrupts now reach the spurious
handler instead of reaching `radio_isr()` and having the events it was waiting
for consumed out from under it.

Measured after the fix: **35 s with the advertiser running, 0 asserts, 0
reboots**, where every previous attempt rebooted about every 9 s. A gate-only
control built from the same tree still receives at the direct build's rate
(`DATA=20` in 40 s), so neither this nor the teardown below cost anything.

#### What the reference implementations changed

Reviewing against two published ESB-beside-BLE timeslot samples —
[`too1/ncs-esb-ble-mpsl-demo`](https://github.com/too1/ncs-esb-ble-mpsl-demo) and
[`inductivekickback/ncs_ble_esb_demo`](https://github.com/inductivekickback/ncs_ble_esb_demo)
— found one contract this file was not keeping. Both end **every** timeslot by
putting the peripheral all the way back to `DISABLED`, not merely by masking its
interrupt:

```c
irq_disable(RADIO_IRQn);
NRF_RADIO->SHORTS = 0;
NRF_RADIO->EVENTS_DISABLED = 0;
NRF_RADIO->TASKS_DISABLE = 1;
while (NRF_RADIO->EVENTS_DISABLED == 0);
```

`radiant_nrf_gate_on_grant_end()` now does the same. `SHORTS` is the part that
mattered: `deliver_terminal()` clears it, but that function runs on every path
that ends a grant *including the ones where no terminal was owed* — a grant that
ran out under an armed receive, an `EXTEND_FAILED`, a session closing. On those
paths a short is a hardware edge from an event to a task that does not care whose
packet raised the event, so our `END`→`DISABLE` short left set is the
controller's next advertising packet disabling the controller's own radio. It
was not what produced assert 48/1792 — that had already been fixed — but it is a
real leak on a real path and it is closed. Neither register is shared, which is
what makes this different from the `INTENCLR00` trap above.

Nordic's own
[`nrf_802154` multiprotocol note](https://github.com/nrfconnect/sdk-nrfxlib/blob/main/nrf_802154/doc/multiprotocol_support.rst)
adds only the priority rule — *"Bluetooth Low Energy has always priority over
802.15.4"* — and says nothing about peripheral hand-back, which is worth knowing
so nobody goes looking there for it.

#### What is still open: the search phase gets blocked

With the assert gone, the arbitration that was underneath it is finally visible,
and it splits cleanly in two.

**Tracking works.** ANT+ tracking a real sensor beside a 100 ms advertiser:
`placed=36 granted=35 blocked=0`, windows about 5.5 ms. Zero refusals.

**Searching does not.** The sweep's chunks are ~94 ms — see the structural cost
above — and a 94 ms window cannot be fitted around a 100 ms advertising interval.
Measured: `placed=744 granted=2 blocked=742`, after which the sweep gives up
(`abrt=1`) and stops placing at all.

Three things were tried. All three are kept, because two of them are right for
the contended case even though neither was the cause, and the elimination is
worth more than the code:

| tried | effect, from `placed=570 granted=2 blocked=568` |
|---|---|
| back the elastic first bite off on refusal, 10 ms → 3.5 ms, grow back on grant | none — `granted=2 blocked=408` |
| skew a refused elastic request's phase, so a ~94 ms chunk cadence cannot lock onto a 100 ms advertiser | none — `granted=2 blocked=93` |
| **abandon the anchor after 4 consecutive refusals and re-bootstrap with EARLIEST** | **`granted=4 blocked=8`** |

The third is the one that mattered, and the reason is the anchor rule.
`distance_us` is measured from the *previous timeslot's start*, and the anchor
deliberately does not move when a request is refused — which is correct, it is
what stops a run of denials walking the schedule, and it has an end. If nothing
is ever granted the anchor ages without bound, so every later request asks MPSL
to place a slot further into the future; by the end of a two-minute run the
distance was over a minute. A fresh anchor costs one minimum-length timeslot, and
EARLIEST is the one request type a busy neighbour cannot starve.

**It is still not enough, and what remains is in the core rather than the gate.**
After re-anchoring the sweep gets about a third of what it asks for — and then
stops asking: `acq=13` frozen, `deny=12`, `abrt=1`. The scheduler abandons the
search after a dozen denials. Under an arbiter a denial during acquisition is
ordinary weather rather than a reason to give up, so what needs changing is the
search's response to `RADIANT_RADIO_EDENIED`: persist and keep re-posting.

**The persistence half of that is now done**, and the OpenThread sitting of
2026-08-13 shows it working: the search no longer gives up, placing 22 796 scan
chunks across a 257 s contended run instead of freezing at a dozen. It did not
help on its own — not one of those chunks completed, and the channel delivered
0 of 961 packets in both roles. Five defects were behind that and are all fixed;
both contended arms now pass the gate. Full numbers and the five defects:
[`radiant-bridge.md` §7.4.1](radiant-bridge.md#741-first-measurement-2026-08-13--the-gate-fails)
and [§7.4.2](radiant-bridge.md#742-five-defects-in-series--and-the-gate-now-passes).

**The denial livelock named above is still real and still unfixed.**
`DONE_DENIED` credits zero dwell, so a chunk that is always denied leaves
`dwell_remaining` unchanged and `radiant_sched_rechunk()` re-arms the identical
chunk forever — the sweep never leaves the set. That needs a bounded-denial
escape whatever the denials turn out to be caused by.

Tracking is unaffected by all of this: a channel that is already locked keeps its
packets beside a live advertiser. Acquisition is the whole of the problem.

So the +0.5 pp coexistence bar below and the arbiter's own cost are **now
results, and the result is a pass in both Thread roles**; the sweep-rate ratio
remains a threshold
with no run behind it, because no arm of that sitting completed a whole sweep
set — the control arm found its sensor and went to tracking before finishing
one, and the contended arms completed no chunk at all. The quantity is not
measurable on a one-sensor rig, so `min_sweep_rate_ratio` has nothing to read.
They live in [`tools/ab_gates.toml`](../tools/ab_gates.toml) as
`[gates.coexistence]`, which is written and evaluated by `ant_ab.py` and is now
`enabled = true` — measured on the nRF54L15 DK, which is the line that moves
this page's coexistence claims from *treat as narrative* to a named gate. It is
`required = false` while the root cause is open, so it reports rather than
blocks.

---

## Radio backends: `sdk_ant` | `core` | `stub`

Today the radio is Nordic's prebuilt `libant.a`, from a private,
non-redistributable repository, and `CMakeLists.txt` hard-fails without its headers — so losing
access to that repository means nothing builds, not even the radio-stub build.
The seam below is what removes that, and it mirrors the transport pattern in
[`src/ant_transport.h`](../src/ant_transport.h) exactly, because that pattern
already works and is already asserted in CI.

```
src/ant_serial_bridge.c        unchanged logic; speaks only ANTW_*/antr_*
        |
   src/ant_radio.h             OUR ~50-function contract  (antr_*)
   src/ant_wire.h              OUR protocol constants     (ANTW_*)
        |
   +----+-----------------+----------------------+
   |                      |                      |
ant_radio_sdk_ant.c   ant_radio_stub.c    radiant_core/   (clean-room stack)
(thin forwarders +    (the no-op radio)         |
 BUILD_ASSERTs)                        radiant_radio_hal.h
                                                |
                                      +---------+---------+
                                   nRF52/54L            EFR32 (later)
```

| Backend | What it is | Why it exists |
|---|---|---|
| `sdk_ant` | ~50 one-line forwarders onto `libant.a`, plus a `BUILD_ASSERT` block comparing every `ANTW_*` constant against its `MESG_*` counterpart | The reference half of every A/B, and the shipping radio until Tier 3 passes. See [`sdk-ant-contract.md`](sdk-ant-contract.md) |
| `core` | The clean-room rebuild in `radiant_core/` | The point of the exercise: builds with zero sdk-ant present, and is a superset — 32 channels, background scan, the RadiANT extensions |
| `stub` | A no-op radio, [`src/ant_radio_stub.c`](../src/ant_radio_stub.c) — the rename of the old `src/ant_stub.c` | The cheapest proof the seam holds. Builds in seconds, and is the only configuration today that runs with no sdk-ant at all |

**The prefixes are load-bearing, not cosmetic.** sdk-ant's error macros are
computed expressions (`NRF_ANT_ERROR_OFFSET + INVALID_MESSAGE`), not literals,
so identical macro *names* with non-identical token sequences are a hard
redefinition error — `ant_wire.h` and `ant_parameters.h` could never coexist in
one translation unit. `ANTW_`/`antr_` is what lets the shim include both and
`BUILD_ASSERT` them against each other. It also makes the bridge textually free
of Garmin's API names, which matters for the clean-room narrative.

**The shim is self-checking, for free.** If sdk-ant changes a signature the
file stops compiling; if a constant drifts the assert fires. Both checks exist
only where sdk-ant is present — exactly where they can be checked. That is why
keeping the sdk-ant backend is worth more than a fallback would be.

### CMake decides, Kconfig mirrors

`list(APPEND ZEPHYR_EXTRA_MODULES ...)` runs *before* `find_package(Zephyr)`,
therefore before Kconfig — so a Kconfig symbol cannot decide whether sdk-ant's
Kconfig gets sourced. The choice has to be made one level up:

- an `ANT_RADIO` cache variable (`sdk_ant` | `core` | `stub`), defaulting to
  `sdk_ant` when `ANT_MODULE_DIR` resolves and `core` otherwise, with
  `ANT_MODULE_DIR` defaulting to `$ENV{SDK_ANT_DIR}`;
- CMake writes a generated `.conf` fragment setting `CONFIG_ANT_DONGLE_RADIO_*`,
  so `.config` records the backend and CI can assert it;
- with `core` or `stub` the module is never added and `CONFIG_ANT` never
  exists, so there is no `CONFIG_ANT=n` dance to get wrong.

Independence is then proved rather than asserted: `build_all.ps1` builds the
`core` and `stub` targets with `-DANT_MODULE_DIR=` pointing at a path that does
not exist. Success is the proof, and it costs nothing.

#### Under sysbuild, neither spelling of `-DANT_MODULE_DIR=` reaches the image

This one is not obvious and it silently destroys the proof above, so it is
written down rather than rediscovered. With sysbuild in the picture, **both
`-DANT_MODULE_DIR=` and `-Dant_dongle_ANT_MODULE_DIR=` land in the *sysbuild*
cache and neither arrives as a cache variable in the image build.** Sysbuild
exports its cache to `<build>/<image>_sysbuild_cache.txt`, and the image loads
that file onto a `sysbuild_cache` target whose properties only `zephyr_get()`
reads. `zephyr_get()` does not exist until `find_package(Zephyr)` — which is
*after* the module decision has to be made, because
`list(APPEND ZEPHYR_EXTRA_MODULES ...)` runs before it. So the one mechanism
that would deliver the value is unavailable at the only moment it matters.

`CMakeLists.txt` therefore parses `<image>_sysbuild_cache.txt` directly, ahead
of `find_package(Zephyr)`, applying `zephyr_get()`'s own precedence rules so
the answer is the same one Zephyr would have given later. Without that, a build
told to look at a nonexistent `ANT_MODULE_DIR` would quietly fall back to the
real sdk-ant checkout, link `libant.a`, **pass**, and prove nothing at all —
the worst kind of green, because the assertion it defeats is the one the whole
seam exists to make.

---

## Inside `radiant_core`: one nRF backend, two gates

**This section used to describe two backends and it now describes one backend
with a seam in it.** The change is recorded rather than edited away, because the
reason for it is the more useful half.

The original plan was `radiant_radio_nrf.c` and `radiant_radio_nrf_mpsl.c` as
two implementations of `radiant_radio_hal.h`, selected by Kconfig. Writing the
second one made it clear that they would not have been two implementations: the
2800 lines that turn a `struct radiant_rx_req` into RADIO registers, a TIMER
compare and two (D)PPI connections are *identical* under both, and exactly one
thing differs — whether the radio is ours whenever we want it.

And the refactor that would have split them **cannot be verified by this
project's own gate.** `radiant_radio_hal.h` states at length that a `t_sync`
calibration error moves RX yield "by a few tenths of a percent, at the same
order as the bench's characterised ~0.4 % collision floor, which is precisely
the range in which a real regression is easiest to mistake for noise". An A/B
run cannot detect the most likely failure of splitting that file.

So the file is not split. Who owns the RADIO is a small internal interface —
[`radiant_core/src/radiant_radio_nrf_gate.h`](../radiant_core/src/radiant_radio_nrf_gate.h) —
with two implementations:

| Gate | Answers "may I have the air?" | For |
|---|---|---|
| `radiant_radio_nrf_gate_direct.c` | always yes, immediately | **The dongle.** ~30 lines of constants; the compiler folds the branch and the emitted code is what it was |
| `radiant_radio_nrf_gate_mpsl.c` | a timeslot session on the public `mpsl_timeslot_*` API | **Combo builds** — ANT beside BLE, or beside Thread/Matter |

Selected by `CONFIG_RADIANT_CORE_BACKEND_NRF_GATE_MPSL`, off by default.

**Direct is the default and landed first**, for a technical reason rather than a
preference: developing the link layer on timeslots from day one means debugging
two unknowns at once, and a missed slot is indistinguishable between "my
scheduler is wrong" and "the grant didn't arrive". Prove the core against
peripherals you fully control, *then* put it under timeslots — at which point
any regression is unambiguously the timeslot layer.

`BUILD_ASSERT(!IS_ENABLED(CONFIG_BT))` is now conditional on the gate rather
than absolute, and the point of it is unchanged: on the direct gate, choosing
the wrong configuration for a build that also wants Bluetooth is loud at compile
time rather than silent on the air. That is the only failure mode that matters
here — two stacks each believing they own RADIO do not announce themselves, they
just lose packets.

Cost of the direct gate: HFCLK management is ours (~20 LOC, nearly free on a USB
build since USBD already holds HFXO) and one bench day for nRF52840 RADIO
anomalies. **Under the MPSL gate that line inverts**: MPSL owns the HFXO,
because every timeslot request asks for `XTAL_GUARANTEED` and MPSL turns the
crystal on and off at the timeslot's edges. What it costs instead is
`CONFIG_MPSL_HFCLK_LATENCY` — the DT `hfxo/startup-time-us`, 1650 µs on nRF54L —
built into how far ahead a request has to be placed.

Cost of the MPSL gate: a closed Nordic binary *under that gate only* — the
direct build stays free of it — and accepting that **we cannot outrank the other
stack.** `timeslot.rst` says applications should always request
`PRIORITY_NORMAL` and that other MPSL users hold priority levels above anything
an application may request; the two application-visible levels rank us against
*ourselves*. The product priority is delivered by shaping the other stack's
demand instead, and ADR 0013 records how. On EFR32 the equivalent is RAIL's own
multiprotocol command scheduling rather than MPSL; the abstraction holds because
no MPSL type or symbol appears above the gate.

### BLE coexistence is a v1 requirement, not a someday

ANT+BLE dual-broadcast is what every modern fitness sensor does, and sdk-ant's
own integration notes say ANT concurrent with anything *other than* Bluetooth
LE is untested — ANT+BLE is **the** tested combination, which is what the
S332/S340 SoftDevices exist for.

Note the asymmetry, because it decides the ordering: **the dongle never needs
BLE; a sensor does.** Nothing about a USB dongle wants a second radio protocol.
Anything built on `radiant_core` as a *node* — `sim/`, a CdA sensor, a telemetry
node — is where coexistence is the whole point. That is why the timeslot gate is
a later phase than the dongle, and why the dongle is not held up waiting for it.

**The asymmetry has since acquired an exception**, and it is worth stating
because it is what makes the arbiter a dongle problem after all: a combined USB
+ Thread/Matter dongle serves Zwift over USB *and* drives a fan over Matter. The
dongle still does not want BLE — it wants 802.15.4 — but it wants the same
arbiter, and it wants it while a permanent background scanner is running. See
ADR 0013.

**Coexistence gate:** a build with `CONFIG_BT=y` and a minimal BLE peripheral
advertising continuously, with `ant_verify.py` showing `loss (exact)` no worse
than +0.5 pp against the same board running ANT alone. That single number is
the whole acceptance test, and it is cheap to run. See
[`testing.md`](testing.md) for how to read it.

**That number now has somewhere to live.** It had none: `tools/ab_gates.toml`
had no coexistence entry at all, and its `[gates.loss_exact]` requires
`max_delta_pp = 0.2` — a *tighter* bar than the +0.5 pp stated here, which reads
as though this paragraph had been superseded. It has not; the two measure
different things. `loss_exact` compares two **backends** with nothing else on the
radio; coexistence compares the **same** backend with the second stack on
against off. Both exist in `ab_gates.toml` now, as `[gates.loss_exact]` and
`[gates.coexistence]`, and both must pass on a combined build.

Three further things belong to that block rather than to this paragraph, and
they are stated there: the arbiter's own cost with no second stack attached
(+1.5 pp is the **exit criterion** for the combined build, not a threshold to
tune), the sweep-rate ratio that quantifies what "the sweep is the elastic
consumer" cost, and `rig.second_stack`, without which a BLE-on run could be
compared against a Thread-on run and produce a figure for neither.

**`CONFIG_NET_L2_OPENTHREAD` must be as loud as `CONFIG_BT` on the direct
gate**, and for the identical reason: the 802.15.4 driver takes the RADIO
through MPSL, so a build with Thread on and the direct gate selected has two
owners and loses packets silently. `radiant_core/Kconfig`'s `depends on !BT ||
RADIANT_CORE_BACKEND_NRF_GATE_MPSL` is the pattern; the OpenThread half lands
with the Thread branch, because until then there is nothing in-tree that can
turn it on.

## HAL contract

The contract is [`radiant_core/include/radiant_core/radiant_radio_hal.h`](../radiant_core/include/radiant_core/radiant_radio_hal.h),
and that file is the normative text — where it and this section disagree, the
header wins. What follows is the part of it you need before choosing a backend
or writing one.

`radiant_core` is a link layer. It decides *what* goes on the air and *when*. A
backend decides *how*: which peripheral, which registers, which DMA, which
interrupt. The header is the whole of the boundary between them, and it is
written so that a second backend on a completely different vendor's radio is an
addition rather than a redesign. Two are planned on nRF (direct-peripheral and
MPSL-timeslot) and one on EFR32/RAIL; a fourth, `radiant_core/tests/fake_radio.c`,
is what lets six core modules be developed in parallel with no hardware.

### Six rules

They are not style preferences; each one is a specific portability failure that
has been designed out.

1. **No register semantics.** Nothing in the header names a field of any radio
   peripheral. An operation is "put this frame on the air with its address
   ending at absolute time T" or "be able to hear a matching address between T0
   and T1", and a completion callback says what happened. If a backend concept
   cannot be phrased that way it does not belong there.
2. **Absolute 64-bit microsecond timestamps everywhere.** Not ticks, not
   relative delays, not a per-backend resolution. RAIL is natively
   microsecond-based, the nRF backends run a 1 MHz timer, and the core's
   scheduler is easier to reason about and to unit-test in absolute time.
   Relative delays would push wrap and latency handling into the core, which is
   exactly where portability bugs hide.
3. **Frequency is an index 0–124 meaning 2400 + N MHz, never hertz.** That is
   how ANT itself expresses it, how RAIL's generated channel configuration
   expresses it, and it removes an entire class of unit mistakes.
4. **Power is dBm, with a raw escape hatch.** dBm is portable and is what the
   link budget is reasoned in; the raw field exists because every part has a
   small set of settings that do not land on integer dBm, and a bench sweep
   needs to reach them without a HAL change.
5. **PHY is a compile-time backend property, selected — not configured — at run
   time.** RAIL's PHY comes out of a generated blob and its parameters cannot be
   set at run time; a backend therefore advertises the PHYs it was built with
   and rejects any other. Adding the long-range coded axis
   ([ADR 0007](decisions/0007-long-range-phy.md)) grew that list and nothing
   else — which is the claim this rule was written to make, now tested rather
   than predicted.
6. **Radio configuration is per-operation, never global state.** Every arm call
   carries its own `struct radiant_pkt_format`. The reason is concrete rather than
   aesthetic: tracking/TX and wildcard search need genuinely different packet
   configurations. Search matches a 3-byte on-air address `[A6 C5 devnum_lo]`,
   so its body is four bytes longer and its address two bytes shorter than
   tracking's. **A HAL with one global configuration would have been wrong from
   its first line, and it would have been discovered on the bench rather than
   here.**

   This used to be argued from a length field: three bytes ahead of byte 3, and
   the nRF's fixed `S0|LENGTH|S1` layout cannot place a length field there. That
   is true of the nRF and it is not the reason, because **there is no length
   field**. Byte 3 is a control byte of six independent flags whose low bits
   read 10 on a broadcast and 2 on an in-slot frame, both carrying eight payload
   bytes (`docs/spike-b-part2-results.md`). Both ANT configurations are
   static-length; they differ in address length and body length, and that is the
   whole of it. A backend that derives its length handling from the
   `LFLEN=8 / CRCINC=1` form inherits a **broadcast-only receiver** — see the
   required-form box in `docs/ant-radio-link.md`.

### The capability query

`struct radiant_radio_caps` is what makes portability structural instead of
conditional. Core policy reads these; core policy never tests for a backend by
name, and there is no `#ifdef` on a part number anywhere above the HAL.

The **CC13x2/CC26x2** column is the first one filled in from a backend that
exists and has run on hardware rather than from a datasheet
([`radiant_radio_cc26xx.c`](../radiant_core/src/radiant_radio_cc26xx.c)).
Measured against the nRF backend in one A/B/A sitting on 2026-08-13: **3.74 %
packet loss against 0.14 %**, and **better** slot timing (0.0093 ms off-grid by
the radio's own clock against 0.0120 ms). See
[ADR 0014](decisions/0014-second-vendor-port-what-it-cost.md).

| Field | What it says | nRF | CC13x2 / CC26x2 | EFR32 / RAIL |
|---|---|---|---|---|
| `max_filters` | Addresses matchable in one receive window. Sets the wildcard-sweep length | **8** (one base plus eight prefixes) | **2** (`syncWord0`, `syncWord1`, and nothing else) | **2** (two runtime sync words) |
| `max_addr_groups` | Distinct values of `addr[0 .. addr_len-2]` matchable in one window. Sets how many *tracked* channels can share a merged window | **2** (BASE0 and BASE1) | **2** — but for the opposite reason: each sync word is fully independent, so this *equals* `max_filters` and the constraint never binds | **2** (two independent sync words) |
| `min_filter_hamming_bits` | Minimum bit distance between two simultaneously-armed hardware addresses before the matcher can separate them | 0 (a comparator; no constraint) | **4** — measured, and the field exists because of this part. See below | 0 (unmeasured) |
| `filter_wildcard_dev` | Can one filter match *any* device number? | false | false | false |
| `addr_len_hw_max` | Longest address the hardware matcher itself handles. Informational — a shorter hardware match is completed in software, at the cost of more spurious wakeups and more receive current | 5 | **3** — the silicon reaches 4 (`nSwBits` is 8..32) but `nSwBits` lives in the setup command that `RF_open()` consumes and cannot be changed under a live handle, so the backend fixes it at the 3 bytes search and tracking share | 4 |
| `max_body_len` | Largest body (bytes between address and CRC), either direction | — | — | — |
| `phys[]`, `n_phys`, `phy_switch_us` | PHYs this build supports, most-preferred first, and what switching between two of them costs the scheduler | switch is free | one PHY; a switch would be a fresh `RF_open` | reloads a generated configuration |
| `ramp_up_us`, `rx_to_tx_us`, `tx_to_rx_us`, `min_arm_lead_us` | The four timing budgets: transmitter ramp-up, both turnarounds, and the minimum lead an arm call needs before it fails `RADIANT_RADIO_ETIME` rather than running late | measured, antenna-referenced | **seeded, not measured** — a P4 bench item; `min_arm_lead_us` is 600 with a separate 150 µs hard floor, split after a scheduler that subtracts the advertised figure and calls immediately produced `ETIME` on every window. Under `CONFIG_RADIANT_CORE_BACKEND_CC26XX_COEX`, `min_arm_lead_us` grows to ~1414 µs (a phy-switch lead) — see "TI coexistence" below | measured, antenna-referenced |
| `min_arm_lead_in_grant_us`, `max_window_us` | The arbitrated pair: lead once a grant is already held, and a fairness bound on window length | 0 / measured under the MPSL gate | **0 / 0 outside coex** ("this backend OWNS the radio" — the two lead figures coincide, nothing bounds a window). Under coex: **600 / ~20 ms** — see "TI coexistence" below | — |
| `time_resolution_ns` | How much of the last digit of a timestamp to believe | 1000 | **250** — the RAT is 4 MHz, measured at 4 000 244 ticks/s | 1000 |
| `has_sync_timestamp` | Is `t_sync` a hardware capture of the address event, or an inference? | true | true — `bAppendTimestamp`, and it is good: consecutive captures came out as exact multiples of the transmitter's period with 1–18 µs of residual | — |
| `has_rssi` | Is `rssi_dbm` populated? | true | true | — |
| `crc_in_hw` | Is the CRC computed by hardware? | true | **false** — the sync-word matcher eats the address bytes before the CRC engine sees them, and ANT's CRC covers them. Software CRC instead, which is what keeps `crc_rx` available and `radiant_crc_repair.c` usable | conditional — see below |
| `tx_power_min_dbm`, `tx_power_max_dbm` | Inclusive dBm range, for clamping and for the bench sweep's bounds | — | **−20 … +5** — real, as of the transmit-power table landing: `apply_power()` + `radiant_cc26xx_txp[]` (provenance: Zephyr's in-tree `ieee802154_cc13xx_cc26xx.c`, not SmartRF Studio), called from the TX arm path. Used to read `+5 … +5` — one operating point, `txPower` fixed at `RF_open()` time, the arm call never reading `req->power` — until `tools/ant_sens.py` tried to use this radio as a ladder's instrument and found nothing moved; see [ADR 0014](decisions/0014-second-vendor-port-what-it-cost.md) | — |

**`min_filter_hamming_bits` is new, and it is new because this part made the
core deaf.** The nRF matches addresses with a comparator; the CC26x2 matches
them with a *correlator*, which scores a sliding window against a template and
fires above a threshold. Give it two templates one bit apart and every arriving
frame scores nearly equally against both — and the part resolves that by firing
on neither. Measured against a transmitter at −47 dBm sending a known
4.0049 frames per second, with one sync word set to its address and the other a
controlled distance away, out of ~30 frames each point could have received:

| distance | 1 bit | 2 bits | 4 bits | 8 bits | single word |
|---|---|---|---|---|---|
| frames | **0** | 3 | 18 | 20 | 21–24 |

`radiant_search.c` put `devnum_lo` `2k` and `2k+1` in set `k` — consecutive
integers, one bit apart, for all 128 sets. Free on the nRF, total deafness
here, and it produced no error at any layer: the window armed, the receiver
ran, the command reported an ordinary timeout, and the scheduler re-armed
forever. The core now spreads each set's pair to `k` and `k ^ 0xFF` when a
backend asks for distance, which is 8 bits apart, is an involution so the 128
sets still partition all 256 values exactly once, and keeps "which set holds
device X" arithmetic rather than a table.

The same defect has an extreme: a *tracked* window has one filter, and the
backend originally aliased `syncWord1 = syncWord0` on the reasoning that the
same word cannot match anything the window did not ask for. That is reasoning
about a comparator. Two identical templates are zero bits apart, and it cost
454 `EVENT_RX_FAIL` against one packet received.

`max_filters` is the one to read twice. There is no wildcard field in
`struct radiant_rx_filter`, because no planned backend can express "any device
number": the shortest on-air address the nRF RADIO will match is 3 bytes, so
the third matched byte is unavoidably `devnum_lo`. Wildcard search is therefore
**core-level policy driven by the capability query** — at `max_filters == 8`,
`radiant_search.c` enumerates eight concrete addresses per window and sweeps
**32 sets** to cover all 256 values of that byte; at `max_filters == 2` the same
policy code produces **128 sets**, or picks a different strategy, with no HAL
change. Keeping the sweep in the core is also what keeps it *shared*: one sweep
serves every channel in SEARCHING at once, where a per-backend sweep would make
eight simultaneous searches take ~64 s instead of ~8 s.

**`max_addr_groups` is the one that was missing, and its absence was a live
bug.** The nRF's eight logical addresses are not eight independent addresses:
logical 0 is `BASE0 + AP0` and logical 1..7 are `BASE1 + AP1..AP7`, so a window
expresses at most **two distinct bases**. That falls out exactly right for
search and exactly wrong for tracking:

| Format | Address | Group | Prefix | Fits |
|---|---|---|---|---|
| Search, 3-byte | `[A6 C5 devnum_lo]` | `[A6 C5]`, shared by every filter | `devnum_lo` | **8** |
| Tracking, 5-byte | `[A6 C5 dnl dnh dtype]` | `[A6 C5 dnl dnh]` — carries the device number | `dtype` | **2** |

So two tracked channels share a window only if they have the same device
number, which is to say never. Until this field existed, `radiant_sched.c` read
`max_filters` for both cases, built merged tracking windows of eight, and the
backend refused every one with `RADIANT_RADIO_ENOTSUP` — a refusal that was then
charged to whichever channel happened to be leading the merge. On a bench that
reads as one healthy sensor failing at random.

`radiant_core/tests/fake_radio.c` models the constraint too, and that is the
part that keeps it fixed: the preset used to advertise eight filters with no
base model at all, which is precisely why CI could not see any of this. **A mock
more capable than the hardware certifies the bug it was written to catch.**

ADR 0005's "32 sensors do not cost 32 windows" survives, with a corrected
number: sixteen windows rather than four. Merging still halves the cost; it does
not divide it by eight.

### TI coexistence

Selected by `CONFIG_RADIANT_CORE_BACKEND_CC26XX_COEX`, off by default. See
[ADR 0015](decisions/0015-cc26xx-coexistence-design.md) for the full design
and why it does not look like the nRF gate above — the short version is that
this part has no timeslot API to sit behind; coexistence is entirely a matter
of which RF-driver scheduling function posts a command
(`RF_postCmd`/`RF_scheduleCmd`) and what `RFCC26XX_schedulerPolicy`'s two
hooks decide when two clients' requests collide.

Two Kconfig symbols, not one: `..._MULTI_PATCH` swaps the RF core's resident
CPE patch to `rf_patch_cpe_multi_protocol` (mandatory once a second client
exists — the patch is fixed for the whole power cycle, see the ADR), and
`..._COEX` (which selects the patch symbol) additionally links
`radiant_radio_cc26xx_arb.c` and switches every arm call to
`RF_scheduleCmd`. Kept separate so the patch's own PHY cost is a build arm on
its own — see `docs/testing.md`'s four-arm table.

The second RF-core client is Zephyr's 802.15.4 driver (there is no BLE stack
for this part in this NCS version, and no DMM either — see the ADR). Unlike
the nRF side, that driver does not survive being preempted: it never re-posts
its background receive command, so the fourth build arm needs an app-local
fork of it (under `coex154/`, not yet written) before it can be measured
under real contention. Until then, `..._COEX` with no second client attached
is a real, buildable, three-arm measurement (the floor, the patch alone, the
scheduler alone) and nothing more.

### `t_sync` — the subtlest thing in the header

**Definition.** `t_sync` is the absolute time, on the HAL's microsecond
timebase, at which the *last bit of the on-air address was at the antenna*. Not
when the interrupt fired, not the end of the packet, not when a callback ran.
Every backend applies its own correction for demodulator group delay, filter
latency and event-capture offset, so that all of them report the same instant
for the same physical event.

**Why that instant.** It is the earliest point at which the frame is
identified, it is equally definable on transmit and on receive, and it is
independent of payload length and of PHY. Anchoring on the end of the packet
would make the reference move whenever a frame's length changed — which is
precisely what the extension axes do. Transmit is symmetric: a request names
the `t_sync` the frame should have and the backend works backwards through its
own ramp-up, preamble and address airtime, so a master's period is exact by
construction instead of silently carrying one backend's ramp-up time.

**Calibration is a measurement, not a datasheet number.** The correction
constant is measured per backend with a **wired two-board trigger**: one board
pulses a GPIO from its own address-sent event, the other captures that pulse on
the same timer it timestamps `t_sync` with, and the difference minus the known
cable and air delay is the constant. Re-measured for every new part and every
PHY, and each backend records its measured value and the date next to it.

**The failure mode, which is silent — this is why the contract is written
down.** The channel state machine predicts the next master transmission from
previous `t_sync` values and centres the next RX window on the prediction. A
*constant* `t_sync` error **cancels out of the period estimate**, so the drift
PLL still locks and nothing looks wrong — but it shifts every predicted window
by that amount, eating the guard on one side and enlarging it on the other. The
window keeps catching packets until offset plus jitter reaches the edge, at
which point yield falls. There is no error code, no CRC failure, no log line:
swapping backends simply changes RX yield **by a few tenths of a percent — the
same order as the bench's characterised ~0.4 % collision floor**, which is
precisely the range in which a real regression is easiest to mistake for noise.

**The other consumer.** Extended message lib config `0xE0` reports channel ID,
RSSI and an RX timestamp to the host. That timestamp MUST derive from `t_sync`
and MUST NOT come from a host-side or thread-context clock. It is what makes
the timing figure read 0.009 ms on the radio clock rather than the ~2.6 ms a
host clock sees through USB jitter, and it is what the A/B `timing` gate in
[`testing.md`](testing.md) is read against. A backend with
`has_sync_timestamp == false` must still fill `t_sync` best-effort and MUST
clear `t_sync_exact`, so the `0xE0` timestamp's advertised accuracy degrades
instead of a worse number being reported quietly as if it were the good one.

### Other conventions worth knowing before you write a backend

- **Time never wraps for the core.** `radiant_time_t` is 64-bit absolute
  microseconds — 584,000 years — so the core may subtract two timestamps
  freely. A backend whose counter is narrower must extend it, and this is not
  hypothetical: **`RAIL_Time_t` is 32-bit microseconds and wraps every
  ~71.6 minutes**, so the EFR32 backend owns a wrap-extension counter. Putting
  that duty in the backend is the point of the rule — a dongle left plugged in
  over a weekend must not acquire a scheduling bug at minute 72.
- **CRC is expressed as a value, not a register**: width 16, poly `0x1021`
  (normal form — the implicit x^16 term is *not* part of the value, and a
  backend whose register wants `0x11021` translates it itself), init `0xFFFF`,
  no reflection, no final XOR, covering the on-air address bytes as well as the
  body.
- **Callbacks run in radio interrupt context**, at the highest priority the
  backend uses — not a work queue, not a thread. That is deliberate and will
  not change: re-arming the next operation from inside the completion callback
  is the low-jitter path and the only one that reliably meets an
  acknowledged-data turnaround. A callback may arm, abort, read the clock and
  signal an ISR-safe object; it must not block, must not retain `event->body`
  past the return, and must not do work proportional to anything.
- **One operation at a time**, with every accepted arm call producing exactly
  one terminal event, and an operation id on every event so a late event from a
  cancelled operation is recognisable rather than merely surprising.

### The EFR32 CRC finding

RAIL's CRC engine starts *after* the sync word, but ANT's CRC covers the
network address. That looks like an immediate disqualification for hardware CRC
on EFR32, and it is not, because CCITT-FALSE chains: running the CRC over a
**constant** 2-byte prefix `A6 C5` from init `0xFFFF` yields `0x233E`, so a
backend that sets `CRCINIT = 0x233E` and lets hardware CRC the remaining 13
bytes produces ANT's exact CRC over all 15. K3 derived this; it is reproduced
here because it decides whether an EFR32 backend pays a per-frame software CRC.

```
CRC-16/CCITT-FALSE(A6 C5)                     = 0x233E
CRC(init 0xFFFF, [A6 C5] + 13 bytes)          = CRC(init 0x233E, those 13 bytes)
```

**It only holds while the covered prefix is constant.** A 4-byte sync word
containing the device number varies per channel, so nothing can be folded and
the CRC must be computed in software — identical semantics, more CPU per frame,
and no hardware suppression of noise-triggered matches. That is exactly what
`caps.crc_in_hw` reports, and why it is a capability rather than an assumption.

---

## EFR32: accommodated in the shape, not built

The six rules above are what make a Silicon Labs EFR32 backend a later addition
rather than a redesign, and three of them were written with RAIL specifically in
view: the 32-bit `RAIL_Time_t` wrap, the two-sync-word `max_filters` limit that
turns the 32-set sweep into 128 without touching the core, and the CRC folding
that decides whether hardware CRC is usable at all.

**Three of those six were since tested for real, on different silicon.** The
CC13x2/CC26x2 backend is built, runs, and reaches all three: its timer is a
32-bit RAT that wraps every 1073 s, it has exactly two sync words and does
sweep 128 sets, and its CRC engine cannot express ANT's so the CRC is folded
into software. All three held without a HAL change. What did *not* hold was a
constraint none of the six anticipated — how far apart two simultaneously
armed addresses have to be — and
[ADR 0014](decisions/0014-second-vendor-port-what-it-cost.md) records what the
whole exercise cost. Anyone starting the EFR32 backend should read it first:
RAIL matches sync words with a correlator too, and
`caps.min_filter_hamming_bits` is currently 0 in the RAIL column because it is
*unmeasured*, not because it is known to be unconstrained.

**It is not built in the first pass, and the reason it is not chosen yet is a
number that has not been measured.** Silicon Labs' ~-105 dBm sensitivity figure
for the xG24 is quoted for **BLE 1M** — 1 Mbps at 250 kHz deviation. ANT is
1 Mbps at ~170 kHz deviation, a different RX filter bandwidth, so the number
does not transfer. Spike C derives the achievable figure for an ANT-compatible
PHY from the Radio Configurator and confirms it on the bench, against the
nRF52840, on the same attenuated link used for the Phase 4 baseline — same
method, same transmitter, two receivers. The result, a dBm or
attenuation-equivalent figure for the ANT PHY on both parts, is written into
this document. Committing to EFR32 on a sensitivity argument before that is
exactly the kind of assumption that costs months if the real gain is 6 dB
rather than 10.

Nothing in `radiant_core` waits on it.

## Backends that were rejected

| Candidate | Why not |
|---|---|
| **nRF24L01+** | Not selected — though the frame mapping onto ShockBurst is what validates the whole design (the nRF24AP2 was an ANT MCU bonded to an nRF24L01+ core), and the HAL would not preclude one |
| **SX1280** (2.4 GHz LoRa) | It *can* do ANT's PHY, but its CRC engine covers payload only while ANT's CRC includes the network address, so it needs software CRC — and it lands near −93 dBm, worse than the nRF52840 already in hand, while the CSS long-range mode that makes LoRa interesting is unusable for ANT. A two-radio gateway (ANT on 2.4 GHz, LoRa sub-GHz backhaul) is an *application* of the telemetry envelope, not a backend |
| **nRF5340** | Dropped from v1. Its RADIO is on the network core, so sdk-ant needs a whole RPC subsystem for it — which is why `src/ant_serial_bridge.c` already branches on `CONFIG_ANT_NP_HOST`. A separate project of comparable size that buys nothing the nRF52840 dongle does not have. It stays valuable as a *debug* board; see [`testing.md`](testing.md) |
| **A 2 Mbps mode** | Saves ~2 µA and costs ~3 dB of sensitivity. Wrong trade for every stated goal |

---

## Transports

The bridge does not know what carries its bytes. It needs a byte sink, a byte
source, and a way to signal that it has drained its input —
[ant_transport.h](../src/ant_transport.h) — and exactly one of three
implementations is compiled in, chosen by `CONFIG_ANT_DONGLE_TRANSPORT_*`.

| Transport | Where it applies | Status |
|---|---|---|
| `USB_LEGACY` | nRF52/nRF53 — anything with a Nordic USBD peripheral | The shipping build. Verified against Zwift |
| `USB_NEXT` | Same, plus every nRF54 part that has USB at all | Verified on a Feather against a real host; DWC2 itself untested |
| `UART` | Parts with no USB device peripheral, e.g. nRF54L15 | Builds and links; not yet run on hardware |

The default follows the devicetree rather than the board name: a
`nordic,nrf-usbd` node selects the legacy stack, an `snps,dwc2` node selects the
new one, and neither means the part has no USB device peripheral and the ANT
serial protocol goes out a UART instead. CI asserts the resulting choice per
board, because a defaulted-from-hardware decision fails silently — a renamed
node would just quietly build something else.

**The new stack is not an upgrade, it is a requirement.** Every nRF54 part with
USB carries a DesignWare DWC2 controller, and Zephyr's only DWC2 driver is
[udc_dwc2.c](https://github.com/zephyrproject-rtos/zephyr/blob/main/drivers/usb/udc/udc_dwc2.c),
which belongs to the new stack. There is no `usb_dc_dwc2.c`. So on those parts
the legacy stack is not older-but-available, it is absent.

That is also why nRF52840 has not been moved over. The legacy build is the one
with hours on it against Zwift, and the new stack buys nothing there — the same
controller, the same descriptors, the same endpoints. What it buys is that
`usb_ant_class_next.c` can be tested on hardware that exists, ahead of hardware
that does not, and that the two can be measured against each other:

```powershell
west -z C:\ncs\v3.2.4\zephyr build -s C:\Users\Colin\ant_dongle `
  -d C:\Users\Colin\ant_dongle\build\feather_next `
  -b adafruit_feather_nrf52840/nrf52840/uf2 -p always -- "-DEXTRA_CONF_FILE=next.conf"
```

That build enumerates as the same `0FCF:1009` with the same 16-character serial
and passes `ant_probe.py` and `ant_scan.py` identically to the legacy one. One
Kconfig warning is expected and harmless — `USB_NRFX_ATTACHED_EVENT_DELAY` lives
inside `if USB_DEVICE_DRIVER` and so does not exist in a new-stack build;
[next.conf](../next.conf) explains why it needs no replacement.

One respect in which the impersonation stops being exact: DWC2 is a high-speed
controller, so an nRF54LM20A dongle enumerates at 480 Mbit/s with 512-byte bulk
endpoints where a retail stick is full speed with 64. The frames are identical
and libusb hides the packet size, but it is a visible difference.

### Which stack is faster

[`tools/ant_bench.py`](../tools/ant_bench.py) times `request capabilities` round
trips — the cheapest message with a guaranteed reply and no side effects, so
what it measures is the USB path and the bridge, not the radio. Four runs of
500 requests each, same Feather, same host, same session:

| | Legacy | New (USBD) |
|---|---|---|
| Latency mean | **0.381 ms** | 0.436 ms |
| Latency p50 | **0.361 ms** | 0.417 ms |
| Latency p90 | **0.449 ms** | 0.479 ms |
| Latency p99 | 0.796 ms | **0.787 ms** |
| Worst case seen | 2.137 ms | **1.274 ms** |
| Throughput, depth 8 | **3334 msg/s** | 2423 msg/s |
| Timeouts | 0 | 0 |
| Flash | **115 960 B** | 125 720 B |
| RAM | 42 880 B | **41 280 B** |

Run-to-run spread was under 2% on the legacy build and under 6% on the new one,
so the gaps in the central figures are real: the new stack is about 15% slower
per round trip and about 27% lower in sustained throughput, and costs ~9.8 KB
more flash while saving ~1.6 KB of RAM. Its only repeatable advantage is a
tighter worst case; p99 is a tie.

**None of this matters for the workload.** Eight ANT+ channels at 4 Hz is
32 messages a second, and the slower of the two stacks does 2400 — a margin of
75×. Both are latency-irrelevant against ANT's 250 ms message period. So the
numbers are not an argument for switching, and not much of an argument against
one either; they exist so the choice is not made on a guess.

What would eventually force the move is that the legacy stack is deprecated —
every build of it prints `Deprecated symbol USB_DEVICE_STACK is enabled` — and
one Zephyr release will remove it. Until then, shipping images stay on the
stack with hours on it against a real application, because being 27% faster at
something with 75× headroom is worth less than being known-good.

### The serial number the host sees

`CONFIG_USB_DEVICE_SN` is not the serial a host sees, despite looking like it.
Zephyr's `usb_update_sn_string_descriptor()` replaces it at runtime with the
HWINFO device ID — the nRF52840's 8-byte FICR DEVICEID — so every dongle has
always had a per-unit serial. What the literal controls is how much of it
survives: Zephyr keeps the low `sizeof(SN)/2` bytes and then copies only
`strlen(SN)` characters of the hex. At the old 7-character `"ANT0001"` that
truncated a 16-character ID to `183A618`, and logged a length-mismatch warning
on every boot. It is now a 16-character placeholder, which is the width that
matches DEVICEID exactly.

That per-unit serial is what `--serial` selects between when two boards
enumerate as the same `0FCF:1009`, which is every two-board bench run in
[`testing.md`](testing.md).

---

## Build targets

The base build, the toolchain pairing and the flashing step are in the
[README](../README.md#building-from-source); this section is the per-target
detail — the board-specific reasoning that is worth more than the command
lines it surrounds. Every one of these is built by
[`scripts/build_all.ps1`](../scripts/build_all.ps1) in one pass.

### nRF52840 Dongle build

Same source, different board target, and a DFU package instead of a UF2:

```powershell
west -z C:\ncs\v3.2.4\zephyr build -s C:\Users\Colin\ant_dongle `
  -d C:\Users\Colin\ant_dongle\build\dongle `
  -b nrf52840dongle/nrf52840 -p always

.\scripts\package_dfu.ps1     # -> dist\ant_dongle_nrf52840dongle.zip
```

The package is unsigned, which is correct here: the dongle ships a
signature-less bootloader, and that is why it takes firmware with no debugger
and no cable. `package_dfu.ps1` refuses an image that does not start at
`0x1000` — a Feather build starts at `0x26000` and would otherwise package
happily into a zip that installs cleanly and then does nothing.

[`boards/nrf52840dongle_nrf52840.conf`](../boards/nrf52840dongle_nrf52840.conf)
settles the two differences: no UF2 output, and `CONFIG_USE_DT_CODE_PARTITION`
left *off* so the offset comes out `0x1000` rather than the `slot0_partition`
the board's devicetree names for MCUboot, which we do not use.

### Pro Micro nRF52840 build

The board's own `/uf2` variant already sets `CONFIG_BUILD_OUTPUT_UF2` and takes
its offset from `nrf52840_partition_uf2_sdv6.dtsi`, the same `0x26000` layout
the Feather uses — so it shares
[`pm_static_nrf52840_uf2_sdv6.yml`](../pm_static_nrf52840_uf2_sdv6.yml) and needs
no map of its own. Build two images:

```powershell
west -z C:\ncs\v3.2.4\zephyr build -s C:\Users\Colin\ant_dongle `
  -d C:\Users\Colin\ant_dongle\build\promicro `
  -b promicro_nrf52840/nrf52840/uf2 -p always

west -z C:\ncs\v3.2.4\zephyr build -s C:\Users\Colin\ant_dongle `
  -d C:\Users\Colin\ant_dongle\build\promicro_synth `
  -b promicro_nrf52840/nrf52840/uf2 -p always -- "-DEXTRA_CONF_FILE=synth.conf"
```

The default uses the 32.768 kHz crystal. `synth.conf` derives that clock from
the 32 MHz crystal instead, for boards that do not have one — see the file for
what it costs and why it is not the default.

Two images rather than one because there is no way to detect this at build
time and no good way to fail at run time: without the crystal the LFXO simply
never starts, `ant_init()` never returns, and USB never comes up. The board
looks dead. Shipping both makes that a one-file retry instead of a diagnosis.

### nRF54 builds

Two nRF54 targets exist. Neither is a product yet — cheap nRF54 dongles are not
on sale — but both build against the real `lib/nrf54l/libant.a`, which is
shipped in sdk-ant and does apply here: `SOC_NRF54LM20A` and `SOC_NRF54L15`
both select `SOC_SERIES_NRF54LX`, and that is what `ANT_LIB_DIR` keys on.

```powershell
west -z C:\ncs\v3.2.4\zephyr build -s C:\Users\Colin\ant_dongle `
  -d C:\Users\Colin\ant_dongle\build\l15 -b nrf54l15dk/nrf54l15/cpuapp -p always

west -z C:\ncs\v3.2.4\zephyr build -s C:\Users\Colin\ant_dongle `
  -d C:\Users\Colin\ant_dongle\build\lm20 -b nrf54lm20dk/nrf54lm20a/cpuapp -p always
```

**nRF54L15 has no USB device peripheral at all.** This is not a DK leaving a
connector unpopulated — there is no `usbd` node and no `usbhs` node in
`nrf54l_05_10_15.dtsi`, because there is nothing on the die. The L05 and L10 are
the same. So that target cannot be a dongle, and it is built with the UART
transport instead: `uart30` is the DK's other VCOM, so the ANT byte stream and
the log leave the board as two separate COM ports over the one debugger cable.
Point the tools at it with `--port`:

```powershell
python tools\ant_probe.py --port COM8
python tools\ant_scan.py  --port COM8 --seconds 30
```

That target is hardware-verified: reset returns the startup message,
capabilities reports the same `080800b23200fd8d0f` the USB builds do, and a
wildcard channel hears real ANT+ sensors. It proves the half the nRF52840
boards cannot — that ANT, MPSL and the clock work on nRF54L silicon.

**nRF54LM20A is the one nRF54L part that does have USB**, as `usbhs@5a000`,
`compatible = "nordic,nrf-usbhs-nrf54l", "snps,dwc2"`, wired up by the DK as
`zephyr_udc0`. That target is a real dongle build and uses the new USB stack
because DWC2 leaves no alternative. Build-only so far — no hardware here.

Flashing either DK needs SEGGER J-Link installed. Without it `nrfutil device
list` still finds the board over its board-controller interface, and the
`JLINK` mass-storage volume will accept a copied hex and then reject it with
`FAIL.TXT: The currently active SWD interface does not support MSD drag and
drop` — which reads like a broken image rather than a missing tool.

### nRF54L needs a real 32.768 kHz crystal

On nRF52840, a board that omits the crystal can still be a dongle: build with
[synth.conf](../synth.conf) and the low-frequency clock is derived from the 32 MHz
crystal the radio already requires. **That fallback does not work on nRF54L,
and the way it fails is the problem.**

Measured on an nRF54L15 DK against a live power meter, alternating builds in
one sitting:

| Build | Broadcasts heard in 30 s |
|---|---|
| XTAL | 14 |
| SYNTH | **0** |
| XTAL | 14 |

Nothing reports a fault. `LFSYNT` is a real value in the nRF54L register map,
the clock starts, `ant_init()` returns 0, and every channel command is
acknowledged — the radio simply never receives anything. A dongle built this
way looks perfectly healthy to a host and is deaf.

The mechanism is not established, and on nRF52840 the same configuration works
and stays supported. But because the failure is silent, `src/main.c` carries a
`BUILD_ASSERT` that refuses the nRF54L + SYNTH combination outright rather than
warning about it.

The practical consequence is a purchasing criterion: **an nRF54 board without a
32.768 kHz crystal cannot be made into an ANT dongle in software.** On nRF52840
a missing crystal at least announces itself, because the board never enumerates
at all. On nRF54L it would enumerate, pass `ant_probe.py`, and hear nothing.

### Stub build

`stub.conf` compiles `src/ant_radio_stub.c` in place of the radio and turns on the MS
OS descriptors, so the USB half builds and enumerates against whatever NCS you
have installed. Useful for working on the USB class, or for USB debugging on a
board sdk-ant does not target:

```powershell
. .\scripts\env.ps1 -NcsVersion v3.4.0
Push-Location C:\ncs\v3.4.0
west -z C:\ncs\v3.4.0\zephyr build -s C:\Users\Colin\ant_dongle `
  -d C:\Users\Colin\ant_dongle\build\stub `
  -b adafruit_feather_nrf52840/nrf52840/uf2 -p always -- "-DEXTRA_CONF_FILE=stub.conf"
Pop-Location
```

**v3.4.0 is deliberate here and only here.** The stub links no `libant.a`, so it
is the one build that does not care which NCS it gets. Every target above it is
an sdk-ant build and must use **NCS v3.2.4, toolchain bundle `fd21892d0f`** —
which is what `build/release`'s `CMakeCache.txt` records the shipping images
being built with — because sdk-ant v2.1.0's Kconfig keys `ANT_LIB_DIR` off
`CONFIG_SOC_SERIES_NRF52X`, and Zephyr 4.4 in v3.4.0 renamed that symbol to
`CONFIG_SOC_SERIES_NRF52`, so the link goes looking for a `libant.a` that is not
there.

---

## What each backend can do

Capacity is a backend property, and the two rows below are the reason the
rebuild is a superset rather than a clone. Both have to be sized into
`radiant_channel.c`, `radiant_sched.c` and the event queue from the first line —
retrofitting a channel-count assumption through a scheduler is far more
expensive than starting at 32.

| | ANT+ / `libant.a` | `radiant_core` |
|---|---|---|
| Simultaneous channels | 8 configured (`CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED`), **15 maximum** (`MAX_ANT_CHANNELS`) | **32** |
| Background scan mode | advertised **off**, not implemented | **on** |

32 is the serial protocol's natural ceiling — the burst header uses the low
5 bits for the channel number. At 4 Hz with ~400 µs windows, 8 channels is 4.8 %
radio duty and 32 is ~19 %, still comfortable, and merging the RX windows of
channels that share RF 57 means 32 tracked sensors cost 16 windows rather than
32 — two per window on nRF, which is `max_addr_groups` above and not
`max_filters`.
RAM cost is ~32 × 72 B = 2.3 KB, still under `libant.a`'s footprint.

**sdk-ant's ceiling is 15, not 8** — 8 is what this build configures,
`MAX_ANT_CHANNELS` is what the stack allows. That matters for how the 32-channel
target is *tested*: `libant.a` cannot be raised to 32 by any configuration, so
there is no sdk-ant reference run to diff against. The 32-channel and
background-scan gates in [`testing.md`](testing.md) are therefore **absolute,
not relative** — a threshold the rebuild has to meet on its own terms rather
than "no worse than sdk-ant". Same for background scan, which sdk-ant
advertises off and does not implement. Everything else in the A/B is relative,
which is why these two are called out.

**Reachable two ways, as of 2026-08-10.** `radiant_core` implements background
scan at the module level through `RADIANT_SEARCH_MODE_SCAN`, and there are two
routes in from the wire. The first is ext-assign — the
`API_EXT_ASSIGN_BACKGROUND_SCAN` bit on `antr_channel_assign()`, which is what
`radiant_api.c` reads to choose `RADIANT_SEARCH_MODE_SCAN` over
`RADIANT_SEARCH_MODE_ACQUIRE` for one channel at a time. The second is
`MESG_OPEN_RX_SCAN_MODE` (`0x5B`) itself, now handled in
`src/ant_serial_bridge.c`: it refuses with `ANTW_CLOSE_ALL_CHANNELS` unless
every channel is closed, then takes channel 0 through the same ext-assign path
in one call instead of three.

This was a real gap until it was closed, not a hypothetical one:
`archive/host-api/ant_dll_exports.json` records `ANT_OpenRxScanMode` as
`"zwift_uses": true` — `ZwiftApp.exe` resolves it — and the capabilities reply
`radiant_core` sends advertises the bit *on*, unlike `sdk_ant`/`stub`, which
report it off. A host that reads that advertisement and calls
`ANT_OpenRxScanMode` on this backend would have gotten `INVALID_MESSAGE` for a
capability it was just told existed — the exact trap
[`docs/gotchas.md`](gotchas.md) warns about, just not caught by
`tools/ant_features.py`, whose coverage check only walks
`ADVANCED_OPTIONS_3`; this bit lives in `ADVANCED_OPTIONS_2`. The conformance
transcript still records `MESG_OPEN_RX_SCAN_MODE` as excluded, deliberately —
see `tools/ant_conformance.py`'s `EXCLUDED` list — because opening a real
scanning channel puts the radio on air and breaks the transcript's
determinism, independent of whether the message is implemented.

The optional features below are the other axis: what exists past the messages a
fitness app sends. The answer is a backend property too — sdk-ant can do
encryption and cannot do event buffering; `radiant_core` v1 does neither, which is
where its −38 % flash estimate comes from.

## Optional features

Past the messages a fitness app sends, ANT carries a set of optional features —
the ones Dynastream's [nRF51 and ANTUSB-m tech
bulletin](https://www.thisisant.com/developer/resources/tech-bulletin/new-nrf51-and-antusb-m-features)
introduced. Which are worth bridging is decided by what a host can actually
call, and that is answerable rather than arguable: on Windows every ANT
application reaches the stick through `ANT_DLL.dll`, so its export table bounds
what any of them can ask for, and the subset Zwift resolves is plain strings in
`ZwiftApp.exe`.

The **In `ANT_DLL`** column is derived from
[`archive/host-api/ant_dll_exports.json`](../archive/host-api/ant_dll_exports.json),
which records every export with its ordinal, message id, and whether sdk-ant
implements it, the bridge bridges it and Zwift resolves it.
[`tools/ant_features.py`](../tools/ant_features.py) reads that JSON rather than
hardcoding the column, so the table and the tool cannot drift apart — if you
edit a cell here, edit the JSON, not the cell.

| Feature | Message | In `ANT_DLL` | In sdk-ant | Bridged |
|---|---|---|---|---|
| Advanced burst | `0x78` config, `0x72` data | `ANT_ConfigureAdvancedBurst` | yes | **yes** |
| Selective data updates | `0x7A`, `0x7B` | `ANT_ConfigSelectiveDataUpdate` | yes | **yes** |
| Event filter | `0x79` | `ANT_ConfigEventFilter` | yes | **yes** |
| Fast channel initiation | ext assign `0x10` | `ANT_AssignChannelExt` | yes | **yes**, passed through |
| Async transmission channel | ext assign `0x20` | `ANT_AssignChannelExt` | yes | **yes**, passed through |
| Single channel encryption | `0x7D`–`0x7F` | **nothing** | yes | reads always; writes behind a Kconfig |
| Event buffer | `0x74` | `ANT_ConfigEventBuffer` | **nothing** | no — and not advertised |
| High duty search | `0x77` | `ANT_ConfigHighDutySearch` | **nothing** | no |
| NVM user space | `ANT_NVM_*` | `ANT_ConfigUserNVM` | **nothing** | no |

**Zwift calls none of them.** Of the ~40 ANT functions it resolves, not one is
on this list, so nothing here is on the path that matters for the shipping use
case. What decided the three that are implemented is that a host *could* reach
them and the stack can do them — advanced burst in particular, because ANT-FS
file transfers use it, and because `MESG_ADV_BURST_DATA_ID` was already accepted
while the message that switches advanced burst on was not, which is a dongle
that takes 24-byte packets and can never send one.

Encryption is the case that needed a switch. `ant_crypto_channel_enable()` and
friends exist in sdk-ant, so the three writes are perfectly implementable — and
they are implemented, behind `CONFIG_ANT_DONGLE_ENCRYPTION`, off by default:

```powershell
west ... -- "-DEXTRA_CONF_FILE=encryption.conf"
```

Off, because `ANT_DLL.dll` exports no encryption call at all, so no host on this
platform can send `0x7D`–`0x7F`. That makes the cost one-sided. The messages
cannot help any application that exists, and what they *can* do is put a channel
into AES-CTR mode, which changes what the radio does per message on the path a
fitness app depends on — note that `ant_crypto_channel_enable()`'s own
documentation requires advanced burst to be on first, so entering that mode
changes the burst configuration too. A shipping image should not carry the
ability to be put somewhere nothing asked for; a build that wants to experiment
can have it for one flag and 1.6 KB of flash.

The read side is unconditional. Those are getters and cannot change what the
radio does. One thing they *can* do is mislead: they were misframed until
`tools/ant_features.py` compared a reply against what had been written.

**Compiling the writes in is not enough to make encryption work, and that is the
real argument for the default.** sdk-ant's `CONFIG_ANT_ENCRYPTED_CHANNELS`
defaults to `0`, so `ant_init()` passes `ucNumberOfEncryptedChannels = 0` to
`ant_stack_config()` and — see the `#if CONFIG_ANT_ENCRYPTED_CHANNELS > 0` in
`init/ant_init.c` — never registers the `fpRANDGet` and `fpECBEncrypt`
callbacks at all. With the writes bridged and that left at zero, measured on the
Feather: setting the crypto ID and the custom user data both succeed, and
`MESG_SET_ENCRYPT_KEY` fails with `INVALID_PARAMETER_PROVIDED` (51), because the
valid key index range is `[0, num encrypted channels - 1]` and that range is
empty.

So on-by-default would ship a feature that answers two of its messages and
refuses the one that matters — the advertised-but-partial trap from the gotchas,
moved one layer in. Raising `CONFIG_ANT_ENCRYPTED_CHANNELS` is what would make
it real, and that is not a free switch either: encrypted channel state is
allocated from the same `m_ant_stack_buffer` the eight ordinary channels live
in (`ANT_ENABLE_GET_REQUIRED_SPACE`), and an encrypted channel is larger than a
plain one. It relays out the memory of the stack a fitness app is using, to
enable something no application on this platform can ask for.

`ENCRYPTION_INFO_SET_RNG_SEED` is refused even with the writes compiled in.
sdk-ant calls it "platform specific" and defines no size for it anywhere, and
the stack does not take its randomness from the host in any case —
`ant_stack_funcs_register()` hands it an `fpRANDGet` callback at init. Refusing
beats handing the library a pointer of a length nobody documents.

The last three cannot be done here at any price: sdk-ant's `ant_interface.h`
exposes no API for event buffering, high duty search or user NVM. `ant_np.c`
handles `0x77` only in the nRF5340 network-processor passthrough, and its
implementation is commented out even there.

**The capability bits are left alone.** High duty search and encryption are
advertised in `ant_capabilities_get()`'s advanced options 3 byte and are not
bridged, which is a mismatch — but clearing those bits would make this dongle
report something a real ANT USB-m does not, and hosts do read those bytes to
decide what they are talking to. Reporting an ANTUSB-m's capabilities and
declining the message is closer to the device being impersonated than reporting
capabilities no ANTUSB-m has ever reported. `tools/ant_features.py` knows which
mismatches are deliberate and fails on any that are not:

```sh
python tools/ant_features.py
```

It walks every optional bit, probes the message behind it with an "off"
configuration, and round-trips each set/get pair — which is the only check that
catches a payload offset wrong by one, the failure the encryption and SDU
replies had.

