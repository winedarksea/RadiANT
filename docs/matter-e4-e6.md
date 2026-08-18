# Packages E4-E6: the Matter plane goes live

E1 retired the build risk, E3 vendored Nordic's bridge core and E7 plumbed it in.
After all three, the image still bridged nothing: `nm` on E3's ELF found zero
`Nrf::Bridge*` symbols, because nothing in the application called any of them and
`--gc-sections` is entitled to discard an archive nobody references.

E4-E6 are what call it.

| Package | What it adds |
|---|---|
| **E4** | `AntDataProvider` - one per (sensor, field), with `uuid -> endpoint_id` persisted through `Nrf::BridgeStorageManager` |
| **E5** | An occupancy bridged device type, serving all three attributes from the shadow |
| **E6** | `ant_matter_start()`, the thread it spawns, and the shadow/flush path that carries values from the ANT pump to the CHIP thread |

## Measured, on the CI row's own recipe

Built as `.github/workflows/release.yml`'s "dongle_thread: Matter (CHIP + bridge
core)" row builds it - `nrf54l15dk/nrf54l15/cpuapp`, `-DANT_RADIO=core
-DRADIANT_BACKEND=nrf`, `EXTRA_CONF_FILE=thread.conf;matter.conf`,
`SB_EXTRA_CONF_FILE=matter_sysbuild.conf`.

```
FLASH:  804756 B  /  1524 KB   51.57%
RAM:    242428 B  /   256 KB   92.48%
```

RAM is the number that matters, and it is the number `docs/matter-ram-budget.md`
was written to protect. That document left 33 612 B free after cutting
`CHIP_NFC_ONBOARDING_PAYLOAD` and sizing `HEAP_MEM_POOL_SIZE` at 8 192 B.
E4-E6 spend 13 896 B of it and leave **19 716 B**. The budget held; nothing in
it had to be re-cut, which is the outcome that document's method was aiming at.

### Read back, not read off the log

Every check below is against a generated artefact. The build log is not evidence
for any of them - trap 1 exists precisely because sysbuild rewrites `CONFIG_CHIP`
after the application's fragments are merged and says nothing while doing it.

From the generated `.config`:

```
CONFIG_CHIP=y                             CONFIG_ANT_DONGLE_MATTER=y
CONFIG_BT=y                               CONFIG_ANT_DONGLE_MATTER_MAP=y
CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL=y    CONFIG_ANT_DONGLE_PROFILES_EXTRA=y
CONFIG_MPSL_TIMESLOT_SESSION_COUNT=3      CONFIG_RADIANT_BACKEND_NULL   absent
```

From `nm` on the final ELF:

```
radiant_radio_init          KEPT     ant_matter_start             KEPT
radiant_sched_tick          KEPT     ant_matter_note_sample       KEPT
radiant_matter_row          KEPT     ant_matter_binding_changed   KEPT
radiant_matter_convert      KEPT     radiant_matter_sink          KEPT
radiant_matter_endpoint_for KEPT     radiant_matter_glue_sink     KEPT

chip:: symbols            3599       Nrf bridge-core symbols        71
```

**3 599 and 71 are the headline.** E1 measured zero `chip::` symbols and E3
measured zero `Nrf::Bridge*` symbols, and in both cases the build was green. This
is the first ELF in which the Matter plane is reachable rather than merely
linkable, and it is the only one of the three where those two counts mean
anything at all.

`k_malloc` is absent from the ELF and `k_queue_alloc_*` with it, so the 8 192 B
kernel heap `docs/matter-ram-budget.md` cut down to is still not on any path
E4-E6 added. That check is re-run here rather than inherited because new code is
exactly what could have newly reached it.

## The fourth sink, and the CI row that did not know about it

`src/matter/ant/ant_matter_sink.c` registers `radiant_matter_glue_sink`, a
second `RADIANT_SINK_DEFINE()` beside the one in `radiant/src/bridge/radiant_matter.c`.
It exists because that first sink cannot see two things the Matter plane needs:

- **bindings** - `radiant_matter.c` passes `NULL` for `binding_changed`, since a
  type map has no use for a device number; the glue needs one for the stable key
  it persists an endpoint id against.
- **staleness** - `RADIANT_SAMPLE_STALE` arrives on fields the type map declines
  (heart rate first, on a strap), so a `want()` filtered to mapped types would
  never observe it.

The Matter arm therefore links **four** sinks, and `bridge_pump.c` logs
`bridge: 4 sinks` at boot:

```
radiant_liveness_sink   radiant_matter_sink
radiant_rules_sink      radiant_matter_glue_sink
```

The CI row's `assert_sink_names` listed three and diffs them as an **exact set**,
not a subset - so this would have failed CI on the first push. The row has been
corrected to the set of four. Two earlier predictions of this count are now
superseded: `docs/matter-e3-vendoring.md` said 2 from the symbol table before
this file existed, and the row itself said 3 from the non-Matter arm.

## Threading: what runs where

Three threads touch this path and the boundaries between them are the design.

```
ANT pump (prio 7)          CHIP event loop (K_PRIO_PREEMPT(1))
  |                          |
  writes shadow              reads shadow, calls the data model
  under k_mutex              under no lock of ours
  |                          ^
  +-- 1 Hz delayable work ---+  PlatformMgr().ScheduleWork(), single-in-flight
```

**The pump thread never takes the CHIP stack lock.** `LockChipStack()` from a
priority-7 thread lets it block the priority-1 thread servicing BLE
commissioning and OpenThread - which is the exact coupling `src/bridge_pump.c`
exists to prevent, approached from the other side. So the pump writes a static
shadow row under an ordinary `k_mutex`, that mutex is never held across a CHIP
call, and no CHIP API is called from any thread but CHIP's own.

The flush is armed by a delayable work item rather than a `k_timer` (whose
handler runs in ISR context, where a queue put is not something to do casually)
and rather than a thread (which would cost a stack for microseconds of work).
`atomic_cas` keeps a single flush in flight, so a slow CHIP thread coalesces
rather than queueing.

`main()` does not move. `ant_matter_start()` is called from
`ant_dongle_post_radio_init()`, **returns immediately**, and does nothing but
create a thread - because that hook runs before `usb_ant_class_init()`, and this
repository has already paid once for a pre-transport stall being indistinguishable
from a dead board (`thread.conf:60-75`: 31.04 s in `net_config_init()`, read as a
hung board by a human and by `ant_verify.py` in the same session).

## Deadbands

`radiant_matter.h` gains `deadband` and `heartbeat_s` per row, with sentinels
`RADIANT_MATTER_DEADBAND_EVERY` (-1) and `_ON_CHANGE` (0). A zeroed row therefore
gets on-change suppression, which is the one suppression that can never lose
information.

These are an **efficiency** measure and not a conformance one. Min/max interval
enforcement belongs to the Matter server's `ReadHandler`; writing an attribute at
4 Hz is legal. It is merely wasteful - each write walks the subscription list and
bumps `DataVersion` on the CHIP thread, which sits above the ANT host thread - and
a controller subscribing with `MinIntervalFloor = 0` would put 4 Hz per attribute
straight into the coexistence budget of `docs/radiant-bridge.md` section 7. The
threshold is compared **after** conversion, in the cluster's own units, so a
0.5 degC temperature deadband is `50`; comparing before conversion would make it
depend on whichever exponent a decoder happened to pick.

The heartbeat is not optional: a deadband without one means a sensor whose reading
never changes stops writing forever, and "unchanged" then becomes
indistinguishable from "gone" to anything reading timestamps rather than
`Reachable`.

## Tests

`radiant_matter` is 28 cases; the full `radiant/tests` application is **723
passing, 0 failing**, run on real hardware (nRF5340 DK) via
`scripts/run_ztest_hw.ps1`.

## What this does NOT establish

Everything above is a build, a symbol table and a host-independent test suite.
**None of it is on-air.** No ANT+ sensor has been decoded into a Matter attribute
on a live radio, and no controller has commissioned this image.

Package G is where that would happen, and at the time of writing it is blocked by
a fault in `nrf_802154` (`rxframe_finish` reaching `nrf_802154_assert_handler`)
that reproduces identically on a pristine baseline at commit `244857e`. It is
pre-existing and unrelated to anything in packages A-E - but it is between this
work and any claim that the bridge functions. Nothing in this document should be
read as evidence that it does.
