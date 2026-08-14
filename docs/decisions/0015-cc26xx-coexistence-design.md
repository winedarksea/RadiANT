# 0015 — CC26xx coexistence: why not a portable gate, and what stands in for it

Date: 2026-08-13
Status: accepted (all four arms build, including the second-client arm; nothing
has been run against an 802.15.4 peer - see "Status" below)

## Context

ADR 0014 records the CC26xx port receiving and transmitting real ANT+ through
the unmodified core, with sensitivity and `ack_data` left with no verdict and
coexistence left entirely unaddressed - `rf_mode.rfMode =
RF_MODE_PROPRIETARY_2_4` with a comment deferring the question. This ADR
records what closing that question actually took, and why it does not look
like the nRF side's answer (`radiant_radio_nrf_gate.h`, ADR 0013).

Two facts, established during planning, that shape everything below:

**There is no BLE stack for CC13x2/CC26x2 in NCS v3.4.0.**
`drivers/bluetooth/hci/cc13xx_cc26xx*` was removed upstream; the surviving
`CONFIG_BLE_CC13XX_CC26XX` is a hidden power-config flag with one consumer.
DMM is not vendored either - `hal_ti` is a stripped subset (no `ti/boards`,
no DMM, no TI-RTOS), so `simplelink-dmm-examples` is architectural reference
only, not code to import. The one real second RF client in-tree is Zephyr's
802.15.4 driver, the same load the nRF side already uses in `coex154/`.
**Decision: 802.15.4 is the second stack**, not BLE.

**On the default RF scheduler policy, arbitration is not weak - it is
impossible.** `RF_howToSchedule` step 1 calls `RF_verifyGap`, which refuses
to insert anything ahead of a `prevCmd` whose `endType == RF_EndNotSpecified`.
The 802.15.4 driver's background `CMD_IEEE_RX` is posted via `RF_postCmd`,
which hardcodes exactly that shape. Step 2 re-runs the same test down the
pending queue and fails identically; step 3 needs `allowDelay`, which our own
commands must not set (see below). So `RF_defaultSubmitPolicy` rejects
**every** ANT window for as long as 802.15.4 is receiving - which, with no
end trigger on its background RX, is always. A custom
`submitHook`/`executeHook` pair is not a tuning knob here; without it there
is no arbitration at all.

## Decision: not a portable gate header

`radiant_radio_nrf_gate.h` exists because on nRF the *programming* must wait
for the grant - `MODE`/`FREQUENCY`/`PCNF`/`PACKETPTR` are the other stack's
live registers, and an armed (D)PPI fires `TASKS_RXEN` regardless of
ownership. Neither hazard exists here: `rx_cmd`/`tx_cmd` are RAM structures a
separate processor reads at dispatch, so filling them early is invisible to
the other client - there is nothing to stage. `GATE_PENDING` is likewise
uninhabited: `RF_scheduleCmd` runs the submit hook inline under
`HwiP_disable()` and returns its verdict synchronously, so the gate collapses
to a two-valued synchronous answer. `radiant_radio_nrf_gate.h`'s own header
anticipated this ("its equivalent is RF-driver command scheduling, not a
timeslot API") - honouring that, coexistence here is:

- **A compile-time variant inside the backend**
  (`CONFIG_RADIANT_BACKEND_CC26XX_COEX`): one `post_op()` helper in
  `radiant_radio_cc26xx.c` that is `RF_postCmd` or `RF_scheduleCmd`. The
  non-coex path must compile to the same machine code as today - the same
  argument `radiant_radio_nrf_gate.h` makes for `gate_direct.c`, and for the
  same reason: a t_sync error moves yield by about the size of the bench's
  ~0.4 % collision floor, too small for an A/B to prove a refactor safe on
  its own.
- **A separate file, `radiant_radio_cc26xx_arb.c`**, holding
  `RFCC26XX_schedulerPolicy`. This is a genuine seam, but it faces the other
  way: it is code the RF driver calls, above BOTH clients, so it must not
  include `radiant_radio_hal.h`. Its only interface to the backend is our own
  `RF_Handle` (`radiant_cc26xx_client()`) and an `activityInfo` tag encoding
  priority (`RADIANT_CC26XX_PRIO_NORMAL`/`_HIGH`,
  `radiant_radio_cc26xx_arb.h`).

## Two Kconfig symbols, not one

`RADIANT_BACKEND_CC26XX_MULTI_PATCH` (swaps `rf_patch_cpe_prop` for
`rf_patch_cpe_multi_protocol`) and `RADIANT_BACKEND_CC26XX_COEX` (the
hooks, `select`ing the patch symbol above) are deliberately separate, even
though a real 802.15.4 build always needs both. `RF_applyRfCorePatch` applies
`cpePatchFxn` only at `RF_PHY_BOOTUP_MODE` and never re-applies it on a
client switch (`RFCCpePatchReset()` is an empty stub in `driverlib/rfc.c`),
so whichever patch is resident at power-up is what BOTH clients run under for
the whole power cycle - and Zephyr's 802.15.4 driver opens with
`rf_patch_cpe_multi_protocol`, so matching it is mandatory, not tidy.

But every PHY constant in `radiant_radio_cc26xx.c` was measured under
`rf_patch_cpe_prop`, so the patch swap is itself a real, separately
measurable cost - and it has to be measurable *without* the scheduler hooks
also changing, or a PHY regression and a scheduling regression would be
indistinguishable in one number. Hence four build arms, not three
(`docs/testing.md`):

| arm | conf fragment | isolates |
|---|---|---|
| 1 | none | the floor (today) |
| 2 | `ti_patch.conf` | the CPE patch's PHY cost alone |
| 3 | `ti_gate.conf` | the scheduler's cost, patch held constant |
| 4 | `ti_coex.conf` + `ti_coex.overlay` | the neighbour's cost |

`arbiter_only_max_delta_pp` is measured **arm 3 against arm 2**, never
against arm 1 - measuring against arm 1 would charge the arbiter for the
patch swap, and the patch swap could plausibly be the larger number.

## The backend changes, and why each one is load-bearing

All in `radiant_radio_cc26xx.c`, all exercised by `post_op()` and the arm
paths:

- **`allowDelay = RF_AllowDelayNone`.** With `allowDelay` set,
  `RF_howToSchedule` step 3 returns `Tail` and appends our window behind
  whatever is running - on a shared core that can be an infinite background
  RX. The absolute start trigger is then long past by the time the queue
  reaches it, `pastTrig = 1` opens it at an arbitrary instant, every frame
  fails the `t_open`/`t_close` test, and the terminal is an ordinary
  `STATUS_TIMEOUT`. That is the same silent-forever failure shape as ADR
  0014's `min_filter_hamming_bits` bug: no error anywhere, a scheduler that
  calmly re-arms, a channel that is deaf.
- **RX's scheduler `endTime` reserves `follow_on_us`** on top of the
  hardware end trigger (`RX_END_SLOP_US` already there). This is what makes
  `caps.min_arm_lead_in_grant_us = 600` (not the non-coex value, 0) an honest
  number: any tracked frame can become a transmit `follow_on_us` later on a
  payload byte that does not exist yet when the request is built, so the
  reservation has to be unconditional, and the phy-switch lead the caller
  pays is then only ever paid *between* clients, never inside our own
  reserved span. `radiant_burst.c` treats `min_arm_lead_in_grant_us == 0` as
  "same as `min_arm_lead_us`" and requires `lead + 250 <= 1560` for ERG mode;
  with 0 the coex build's `min_arm_lead_us` (1414) blows that budget and
  `radiant_transfer_init()` returns `ENOTSUP` silently - ERG mode off on the
  entire TI coex build with no error anywhere else.
- **TX gets a scheduler `endTime`** (`TX_TAIL_SLOP_US`) for the identical
  reason: `rfc_CMD_PROP_TX_ADV_t` has no `endTrigger` field at all, and
  `RF_verifyGap` refuses to place any `RF_EndNotSpecified` command anywhere
  but the tail - the same trap as `allowDelay`, reached from the transmit
  side.
- **Negative handle -> `RADIANT_RADIO_EDENIED`, never `EIO`.** Under
  arbitration a refusal is routine; `EIO` sends `radiant_sched.c` to its
  default case (`slot_clear()` + `DONE_FAILED`), killing a healthy tracked
  channel over an ordinary, expected denial. `EDENIED` is the signal
  `radiant_radio_hal.h` defines for exactly this synchronous refusal, and on
  this part it is the only denial signal available (there is no separate
  "pending" state to poll, per the gate-header discussion above).
- **`RF_EventCmdPreempted` maps to `STATUS_DENIED`, tested before
  `Cancelled`/`Aborted`** (`RF_abortCmd` sets both Cancelled and Preempted, so
  order picks which fact wins). Folding it into `ABORTED` would make search
  accounting read "credit what was actually listened to" on a window that
  heard nothing - optimistic enough that a coexistence loss gate could never
  fail.
- **`caps` becomes non-`const` only under coex** (a preprocessor-selected
  qualifier, not a duplicated literal), patched once in `radiant_radio_enable()`
  after `RF_open()` succeeds: `min_arm_lead_us` grows by a phy-switch lead,
  `min_arm_lead_in_grant_us` becomes 600, and `max_window_us` gets a fairness
  bound (~20 ms) instead of 0 - a 100 ms sweep chunk is a 100 ms outage for
  the other client once one exists, and the comment that used to read
  "unbounded: nothing is arbitrating" would otherwise become a lie.

## The scheduler policy itself

`radiant_radio_cc26xx_arb.c`'s submit hook: foreign commands are delegated
verbatim to `RF_defaultSubmitPolicy` - never "improved". Our own commands are
checked for a wrap-safe overlap against everything foreign (background,
foreground, and the pend queue), where a command with `endType ==
RF_EndNotSpecified` (802.15.4's background RX) is treated as active from its
own start onward forever, since it has no meaningful end. No overlap:
delegate to the default policy, which inserts in time order. Overlap at
`PRIO_NORMAL` (sweep, scan, ED - the elastic consumer, ADR 0013): refuse.
Overlap at `PRIO_HIGH` (tracked slot, transmit): head-insert and let the
execute hook preempt. The execute hook is equally required and easy to
miss - `RF_defaultExecutionPolicy` returns `RF_ExecuteActionNone`
unconditionally, so head-of-queue plus the default execute hook would mean
waiting behind an infinite RX forever; only `AbortOngoing` from the execute
hook actually displaces a running command.

Priority is derived from window duration alone (>= 10 ms is elastic), the
same rule and the same threshold `radiant_radio_nrf_gate_mpsl.c`'s call sites
use, for the same reason: rule 1 (a HAL backend may not know anything
ANT-specific) does not forbid looking at window *length*, which is not ANT
knowledge.

## Status

**Landed:** the two Kconfig symbols, `radiant_radio_cc26xx_arb.c`, the
`post_op()` split, every backend change above, the CMake PM-guard rekey (see
`docs/testing.md`), `ti_patch.conf`/`ti_gate.conf` (arms 2 and 3), and - since
2026-08-14 - the forked 802.15.4 driver and `ti_coex.conf`/`ti_coex.overlay`
(arm 4). **All four arms compile and link**, and arm 1's image is byte-for-byte
the same size it was before any of this landed (68248 B), which is the check
that matters for the "non-coex path must not change" claim above.

### The fourth arm, and the fork under it

Zephyr's 802.15.4 driver has no re-post path for `cmd_ieee_rx` -
`cmd_ieee_rx_callback()` never resubmits, and the driver never subscribes to
`RF_ClientEventRadioFree`. So the first preemption ends 802.15.4 reception
**permanently**: a naive coexistence run would report "ANT healthy, the
neighbour silently dead, EXIT=0" - exactly the failure class ADR 0013's own
gates were written to prevent, and with no nRF analogue (both the SoftDevice
Controller and `nrf_802154` are built to be denied; this driver is not).

`radiant/coex154_ti/ieee802154_cc13xx_cc26xx.c` is a verbatim vendored
copy of that driver with four marked changes: the provenance comment, `CONFIG_*`
fallback shims for the sub-symbols that vanish with the upstream menuconfig, the
re-post branch itself, and the `rx_active` flag that branch needs. The flag is
the part that is easy to miss: `ieee802154_cc13xx_cc26xx_stop()` flushes with
`RF_CMDHANDLE_FLUSH_ALL`, which delivers the *same* Aborted/Cancelled events a
preemption does, so without it `stop()` would be undone by its own callback and
`do_set_channel()` - which stops and then re-posts - would queue two background
receives.

**The build-system problem that deferred this turned out to have a clean
answer, and it is the opposite of the one that was feared.** The worry was
double-registration: both files register `NET_DEVICE` against the same
`DT_DRV_COMPAT` node, and Zephyr compiles the original unconditionally under
`CONFIG_IEEE802154_CC13XX_CC26XX` with no seam to exclude one file. But that
symbol is a plain `menuconfig bool` off a devicetree node, so `ti_coex.conf`
simply sets it to **n** and turns ours on instead - exactly one driver
compiles, one device is registered, the devicetree node is untouched. No
Zephyr patch, no compatible-string renaming, no re-parenting. Turning it off
is *required* rather than merely tidy for a second reason nobody predicted:
`radiant/CMakeLists.txt` compiles `driverlib/rfc.c` itself and hal_ti
compiles it under that same symbol, so leaving it `y` is a duplicate-symbol
link error.

What did bite, and is worth recognising on sight, is a devicetree problem
wearing a C compiler's clothes: Zephyr's own `cc26x2r1_launchxl.dts` **disables**
the `ieee802154` node and deletes the `zephyr,ieee802154` chosen property, so
the first build failed on `error: '__device_dts_ord_53' undeclared` from
`DEVICE_DT_INST_GET(0)` inside the vendored driver. Hence `ti_coex.overlay`,
which must be passed alongside the conf fragment.

**What arm 4 is verified to be, and what it is not.** It builds, it links, the
fork's object and `radiant_cc26xx_submit` are both in the image, and its
`.config` asserts positively (`BACKEND_CC26XX_COEX=y`, `MULTI_PATCH=y` by
selection, `COEX154_CC26XX_FORK=y`, upstream driver absent). **Nothing in it has
run against an 802.15.4 peer, because this bench has none.** The re-post branch
has therefore never executed. That asymmetry is the thing to hold onto: the
ANT-side number this arm produces would be honest either way, since the
neighbour's endless background receive genuinely contends for the core whether
or not anyone answers it - but the neighbour's own survival across a preemption,
which is the entire reason the fork exists, cannot be observed here. A run
reporting only an ANT loss figure has measured half of what the arm is for.

Still unwritten, and deliberately: the `radiant/spike/ti_coex` measurement
rig the original plan calls for. It needs the same peer.

**R1 is FALSIFIED - the multi-protocol patch does not degrade this PHY.**
`radiant/spike/ti_phy` was built twice (a `SPIKE_TI_PHY_MULTI_PATCH`
CMake option added for this, off by default - see the spike's own
CMakeLists.txt) and run back to back against the same paced transmitter
(`tools/ant_sim.py` on an nRF54L15 DK, device 14871, 4.0049 Hz):

| build | best point | nRxNok (every point) | notes |
|---|---|---|---|
| `rf_patch_cpe_prop` (today) | 90 % (54/60 frames) | 0 | one point dropped to 4/60 entries on an `nRxBufFull` overflow - a spike ring-buffer artefact, not signal |
| `rf_patch_cpe_multi_protocol` (D1) | 90 % (54/60 frames) | 0 | no dropped point; if anything more consistent across the sweep |

Identical best score, zero `nRxNok` in every point of both runs. The patch
swap costs nothing measurable on this PHY. `RADIANT_BACKEND_CC26XX_
MULTI_PATCH` can be relied on without re-measuring every constant in
`radiant_radio_cc26xx.c` - the risk table's "falsified first" item is closed.

## What this fences for the next reader

Do not fold the patch symbol and the hook symbol into one - arm 2 needs to
exist independently of arm 3, or a PHY regression and a scheduler regression
become indistinguishable. Do not "improve" the neighbour's scheduling from
inside `radiant_cc26xx_submit()` - the only sanctioned change to how the
802.15.4 driver behaves is the re-post fix in the coex154/ fork, and that is
a survival fix, not a scheduling one. Do not trust a coexistence number that
was not run against a paced transmitter (`ant_sim.py`), for the reason ADR
0014's own measurement-discipline section already gives: a gate scored
against "frames it already detected" reads clean on a PHY dropping half of
everything.
