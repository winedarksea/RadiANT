# Integration guide: ANT+ heart rate reception in ~30 lines

Checked by: [`../../tests/import_smoke/`](../../tests/import_smoke/), which
is exactly this walkthrough as a buildable application. CI builds it against
an isolated copy of `radiant/` extracted with `git archive`, so this guide
cannot silently reach a path the module does not actually expose - if it
did, that build would fail rather than this document quietly going stale.

This is the whole downstream story: what a project that wants to receive an
ANT+ heart-rate broadcast, and nothing else, has to write. It stops short of
a working receiver on purpose - see the note at the end - but every line
here is real, linked, and the same shape a working port uses.

## 1. Bring the module in

Before `find_package(Zephyr)`, so `ZEPHYR_EXTRA_MODULES` is set in time for
Kconfig to source `radiant/Kconfig`:

```cmake
list(APPEND ZEPHYR_EXTRA_MODULES path/to/radiant)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(my_hr_receiver)

target_sources(app PRIVATE src/main.c)

# The sample bus and the HR decoder are library material the module ships
# but does not compile for you - see radiant/README.md's "What is not
# compiled for you". Reach into the module's own source tree for them.
target_sources(app PRIVATE
  path/to/radiant/src/bridge/radiant_bridge.c
  path/to/radiant/src/bridge/radiant_hr_adapter.c
)
target_include_directories(app PRIVATE path/to/radiant/src/bridge)

# RADIANT_SINK_DEFINE (step 4 below) needs this or the sink list's start/end
# symbols never exist - a link error, but only once something dispatches to
# it, so it is easy to miss until step 6 actually runs.
zephyr_linker_sources(SECTIONS path/to/radiant/src/bridge/radiant_bridge_sections.ld)
```

`prj.conf` needs one line: `CONFIG_RADIANT=y`. Its default backend,
`CONFIG_RADIANT_BACKEND_NULL`, has a real lifecycle and clock and refuses
every radio arm - correct for this walkthrough, since the point here is the
API shape, not a real receive. A real product sets
`CONFIG_RADIANT_BACKEND_NRF=y` (or `_CC26XX`) and reads
[`../../docs/backends.md`](../../docs/backends.md) for what that backend
needs from the devicetree.

## 2. The two hooks the module needs from you

`radiant/include/radiant/radiant_event.h` declares two functions and calls
them; nothing in the module defines them, so a build that forgets one fails
at link time rather than shipping a silent data race:

```c
unsigned int radiant_event_crit_enter(void) { return irq_lock(); }
void radiant_event_crit_exit(unsigned int key) { irq_unlock(key); }
void radiant_event_wakeup(void) { /* wake your drain thread here */ }
```

A real port wakes a work queue or gives a semaphore here; this walkthrough's
own [`main.c`](../../tests/import_smoke/src/main.c) just polls, so its
`radiant_event_wakeup()` is empty.

## 3. Receive a message

`radiant/include/radiant/radiant_msg.h` declares the one function the
module calls for every message it decides you should see -
`radiant_on_message()` - implemented outside the module, resolved at link
time, no registration call:

```c
void radiant_on_message(const struct radiant_msg *msg)
{
    if ((msg->id != RADIANT_WIRE_MESG_BROADCAST_DATA_ID &&
         msg->id != RADIANT_WIRE_MESG_ACKNOWLEDGED_DATA_ID) ||
        msg->len != 9u) {
        return;                              /* not an 8-byte channel-scoped data message */
    }
    uint8_t channel = msg->data[0];
    radiant_hr_adapter_decode(&hr_adapter, channel, &msg->data[1], now_us());
}
```

`radiant_hr_adapter_decode()` (`radiant/src/bridge/radiant_hr_adapter.h`)
reads the four bytes every ANT+ HR page carries unchanged - event time, beat
count, computed heart rate - and posts up to three records onto the sample
bus via `radiant_bridge_post()`. It does not care which HR page arrived,
because those four bytes are the same on all of them.

## 4. Register a sink

A sink is a registration, not a case in a switch - `RADIANT_SINK_DEFINE`
links every translation unit's definition into one array with no central
list to edit, the same shape Zephyr's own `DEVICE_DEFINE` uses:

```c
static bool want_hr(const struct radiant_sample *s)
{
    return s->field_type == RADIANT_FIELD_HEART_RATE;
}

static void publish_hr(const struct radiant_sample *s)
{
    printk("HR source=%u bpm=%lld\n", s->source, s->raw);
}

RADIANT_SINK_DEFINE(hr_sink, want_hr, publish_hr, NULL);
```

`want()` is a first refusal - `NULL` means "wants everything" - and
`publish()` runs in thread context from `radiant_bridge_drain()`, so it may
block: write to a socket, a flash log, an MQTT client, whatever your product
actually wants heart rate to go. See
[`../../docs/radiant-bridge.md`](../../docs/radiant-bridge.md) for the full
sink/binding/rule design if you want more than one source feeding one sink.

## 5. Open a channel

`radiant/include/radiant/radiant_channel.h` mirrors the ANT serial
protocol's own four channel calls almost one to one:

```c
radiant_channel_init();
radiant_event_init();

radiant_channel_assign(0 /* channel */, 0x00 /* slave */, 0 /* network */, 0);
radiant_channel_id_set(0, 0 /* wildcard device number */, 0x78 /* HR */, 0);
radiant_channel_open(0, 0 /* offset */, (radiant_time_t)0 /* now */);
```

Type `0x00` and device number `0` (wildcard) ask for exactly what an
unconfigured ANT+ receiver asks for: any HR sensor, on any of the network's
usual channels. `radiant_channel.c` calls no HAL function - it only updates
channel state - so this much links and runs correctly even with no radio
behind it at all, which is what makes it safe to demonstrate without one.

## 6. Drive it

```c
while (1) {
    radiant_event_drain(0);      /* deliver anything queued, oldest first */
    radiant_bridge_drain();      /* dispatch anything the adapter posted */
    k_sleep(K_SECONDS(1));
}
```

## What this does not do

This walkthrough never calls `radiant_sched_init()` or `radiant_radio_init()`
- the two calls that actually arm a radio and let the scheduler drive it.
Without them, `radiant_channel_open()` above reaches `SEARCHING` and stays
there forever: correct behaviour for an unarmed channel, and proof that the
API sequence above is real and linkable, but not a receiver that will ever
hear a heart-rate strap.

Wiring a real receiver needs `struct radiant_sched_cbs` and the radio
callbacks `radiant_sched_radio_cbs()` returns, and is the antr_* adapter's
whole job -
[`../../apps/common/ant_radio_radiant.c`](../../apps/common/ant_radio_radiant.c)
is the reference implementation, ~2500 lines because an ANT-serial dongle's
bridge protocol needs all of it. A product that is not an ANT-serial dongle
needs much less: read that file for the full sequence
(`radiant_channel_init()` → `radiant_event_init()` → `radiant_sched_init()`
→ `radiant_radio_init()` → `radiant_search_init()`, in that order, each step
preconditioning the next) and keep only the parts your product's channel
plan actually uses.
