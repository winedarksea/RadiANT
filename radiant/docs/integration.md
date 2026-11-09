# Integration guide

There are two ways to use RadiANT. Almost everyone wants the first.

- **The `antr_*` API.** RadiANT runs itself. You configure channels and receive
  messages. This is what every application in this repository uses.
- **The low-level API.** You write the runtime that connects the scheduler, search
  and event queue yourself. Only worth it if `antr_*` cannot do what you need.

## The `antr_*` API

This is [`samples/hr_receiver`](../samples/hr_receiver/), which CI builds. Follow
the [README](../README.md) to add the module first.

`prj.conf`:

```
CONFIG_RADIANT=y
CONFIG_RADIANT_ANTR_API=y
```

### Open a channel

Every call returns `ANTW_RESPONSE_NO_ERROR` (0) or an ANT response code.

```c
#include <radiant/ant_radio.h>

static const uint8_t ant_plus_key[8] = {
	0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45
};

int main(void)
{
	antr_init();
	antr_network_address_set(0, ant_plus_key);

	antr_channel_assign(0, ANTW_CHANNEL_TYPE_SLAVE, 0, 0);
	antr_channel_id_set(0, 0, 0x78, 0);        /* any heart-rate sensor */
	antr_channel_period_set(0, 8070);          /* 4.06 Hz */
	antr_channel_radio_freq_set(0, 57);        /* 2457 MHz */
	antr_channel_search_timeout_set(0, 0xFF);  /* search until found */
	antr_channel_open(0);
	return 0;
}
```

`antr_init()` starts RadiANT's own thread, so `main()` can return.

### Receive

Define `antr_on_message()` once in your application. It is resolved at link time,
with no registration call, and it runs on RadiANT's event thread, so it must not
block.

```c
void antr_on_message(const struct antr_msg *msg)
{
	if (msg->id == ANTW_MESG_BROADCAST_DATA_ID && msg->len >= 9) {
		uint8_t channel = msg->data[0];
		const uint8_t *page = &msg->data[1];

		printk("ch %u: %u bpm
", channel, page[7]);
	}
}
```

`data[0]` is the channel and the eight payload bytes follow. Byte 7 of every
heart-rate page is the computed heart rate. Events arrive the same way, with `id`
set to `ANTW_MESG_RESPONSE_EVENT_ID` and the event code in `data[2]`.

### Transmit

Assign the channel as `ANTW_CHANNEL_TYPE_MASTER`, give it your own device number,
open it, and supply each next page when `ANTW_EVENT_TX` arrives. See
[`samples/hr_transmitter`](../samples/hr_transmitter/).

### Decode profiles

For anything beyond a heart-rate byte, set `CONFIG_RADIANT_PROFILES=y` to get the
page encoders and decoders, or `CONFIG_RADIANT_BRIDGE=y` to get decoded samples
delivered to sinks you register. [`../../docs/radiant-bridge.md`](../../docs/radiant-bridge.md)
describes the bridge.

The full API is [`include/radiant/ant_radio.h`](../include/radiant/ant_radio.h).

## The low-level API

Checked by [`../../tests/import_smoke/`](../../tests/import_smoke/), which is this
walkthrough as a buildable application. CI builds it against an isolated copy of
`radiant/`.

This path stops short of a working receiver: see the note at the end. Every line
here is real and linked.

### 1. Bring the module in

Before `find_package(Zephyr)`:

```cmake
list(APPEND ZEPHYR_EXTRA_MODULES path/to/radiant)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(my_hr_receiver)

target_sources(app PRIVATE src/main.c)
```

`prj.conf`:

```
CONFIG_RADIANT=y
CONFIG_RADIANT_BRIDGE=y
```

Leave `CONFIG_RADIANT_ANTR_API` off: it defines the hooks in step 2.

### 2. The two hooks the module needs from you

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

### 3. Receive a message

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

### 4. Register a sink

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

### 5. Open a channel

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

### 6. Drive it

```c
while (1) {
    radiant_event_drain(0);      /* deliver anything queued, oldest first */
    radiant_bridge_drain();      /* dispatch anything the adapter posted */
    k_sleep(K_SECONDS(1));
}
```

### What this does not do

This walkthrough never calls `radiant_sched_init()` or `radiant_radio_init()`
- the two calls that actually arm a radio and let the scheduler drive it.
Without them, `radiant_channel_open()` above reaches `SEARCHING` and stays
there forever: correct behaviour for an unarmed channel, and proof that the
API sequence above is real and linkable, but not a receiver that will ever
hear a heart-rate strap.

Wiring a real receiver needs `struct radiant_sched_cbs`, the radio callbacks
`radiant_sched_radio_cbs()` returns, and a thread driving it all. That is what
[`src/antr/ant_radio_radiant.c`](../src/antr/ant_radio_radiant.c) does, and
`antr_init()` shows the order: `radiant_channel_init()`, `radiant_event_init()`,
`radiant_sched_init()`, `radiant_radio_init()`, `radiant_search_init()`, with each
step preconditioning the next.
