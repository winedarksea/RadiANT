/* SPDX-License-Identifier: Apache-2.0 */
/*
 * self_channels.c - the slave channels this dongle drives itself, and the
 * pairing window that decides what may bind.
 *
 * THE ZWIFT MODEL (docs/radiant-bridge.md section 10.1b). Discovery is a
 * bounded pairing state, and a sensor once paired owns a channel PINNED to its
 * device number for as long as it stays paired:
 *
 *   IDLE      assigned, closed     window closed, nothing bound
 *   DISCOVER  wildcard search,     window open, nothing bound; the search
 *             rotating profiles    profile rotates on every search timeout
 *   PINNED    the bound device's   from the moment it binds until it is
 *             number/type/trans,   evicted - never unbound by a dropout, a
 *             infinite search      search timeout or a stranger
 *
 * A pinned channel that loses its sensor just keeps searching for it, which is
 * what Zwift does: its pairing survives the rider getting off the bike, and so
 * does ours. radiant revisits a named device's set with backoff
 * (RADIANT_SEARCH_REVISIT_FIRST), so the sensor is back within a second or two
 * of transmitting again. The cost is real and is stated in section 10.1b: while
 * a pinned sensor is absent, radiant's shared sweep keeps running.
 *
 * Binding is explicit opt-in (section 10.1 rule 1, section 5): a channel may
 * bind only while a button-press window is open. Outside it, an unpaired
 * channel does not search at all - which is also the radio rule, since section
 * 7 shows continuous promiscuous ingest is the one mode that cannot coexist
 * with Thread. When the window opens and every channel is pinned, the pinned
 * sensor that has been absent longest is EVICTED to make room (once per window
 * opening; a sensor being tracked is never evicted).
 *
 * Every decision is a pure function in radiant/src/bridge/radiant_self_policy.c
 * (tested in radiant/tests/src/test_self_policy.c); this file keeps the antr_*
 * calls.
 *
 * This starts on its own delayed thread rather than in
 * ant_dongle_post_radio_init(), because that weak hook already has one strong
 * override (src/main.c). The CONFIG_ANT_DONGLE_SELF_CHANNELS_DELAY_S delay is
 * a real precondition, not caution: ant_radio.h forbids any antr_* call
 * before antr_init() returns, and the default of 5 s is comfortably past it.
 * It polls channel status (antr_channel_status_get()/antr_channel_id_get())
 * rather than watching the RX tap, because the tap runs on the backend's own
 * event thread and calling back into antr_* from there is the re-entrancy
 * ant_radio_radiant.c's api_lock exists to prevent.
 *
 * An ANT slave's channel period must match its master's and its device type
 * must match or be wildcard 0, so a discovering channel cannot search two
 * profiles at once (8070 counts finds heart-rate straps and nothing else - a
 * power meter's slot is 8182 counts away). Hence a table of {device type,
 * period} rows. CONFIG_ANT_DONGLE_PROFILES_EXTRA decides how many rows there
 * are: off, one (heart rate, the only thing rx_tap.c then decodes); on, five.
 * There is one code path either way.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ant_radio.h"
#include "ant_serial_bridge.h"
#include "ant_wire.h"

#include "radiant_binding.h"
#include "radiant_bridge.h"
#include "radiant_self_policy.h"

#include "bridge_app.h"

LOG_MODULE_REGISTER(self_channels, LOG_LEVEL_INF);

#define SELF_N CONFIG_ANT_DONGLE_SELF_CHANNELS

BUILD_ASSERT(SELF_N <= ANT_BRIDGE_SELF_CHANNEL_TOP + 1,
	     "More self-driven channels were asked for than the ANT stack has "
	     "allocated.");

/*
 * The public ANT+ network key - the one every ANT+ device on the air uses, not
 * a stack licence key (an sdk-ant concept radiant does not have). The same
 * eight bytes apps/hrm_ble/src/main.c carries, and confusing the two produces
 * a receiver that runs and hears nothing.
 */
static const uint8_t ant_plus_key[8] = {
	0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45
};

#define SELF_NETWORK 0u
#define SELF_RF_FREQ 57u

/*
 * ANT's high-priority search timeout is expressed in 2.5 s ticks, and its
 * default is 10 ticks = 25 s (radiant/include/radiant/radiant_channel.h's
 * RADIANT_CHANNEL_SEARCH_TIMEOUT_DEFAULT, and the same figure ant_radio.h
 * documents for the antr_* contract). 255 is ANT's own "never" on sdk-ant and
 * radiant alike (radiant_channel.c treats either byte at 255 as infinite),
 * and it is what Zwift sets on every paired channel.
 */
#define SELF_SEARCH_TICKS_25_S    10u
#define SELF_SEARCH_TICKS_45_S    18u
#define SELF_SEARCH_TICKS_FOREVER 255u

/*
 * One row per {device type, channel period} pair a DISCOVERING channel can
 * acquire. A pinned channel takes neither from here: it searches for exactly
 * the bound device, at the period read off the channel when it bound.
 *
 * A PERIOD IS NOT A CHOICE AND MUST NOT BE ROUNDED. It is the profile's own
 * number, in counts of 1/32768 s, and a slave told anything else does not
 * merely acquire more slowly - it never acquires at all. Each row's period is
 * transcribed from the named constant in the comment beside it, which is the
 * primary-source-derived definition in radiant/src/profiles/. Those headers
 * are not on this file's include path with CONFIG_ANT_DONGLE_PROFILES_EXTRA
 * off (see CMakeLists.txt), so the numbers are written out, each naming the
 * constant it must agree with, and the agreement is a review obligation rather
 * than a compiler-enforced one.
 *
 * `name` exists so the search log line can be read by a human: "searching for
 * bicycle power" then "searching for fitness equipment" is the only thing that
 * separates "no sensors in range" from "searching for the wrong thing".
 */
struct self_profile {
	uint8_t     device_type;
	uint16_t    period_counts;
	uint8_t     search_ticks;
	const char *name;
};

static const struct self_profile self_profiles[] = {
	/* PROFILE_HR_PERIOD, radiant/src/profiles/profile_hr.h: 8070 counts is
	 * ~4.06 Hz, the ANT+ heart rate profile's standard rate. (The profile
	 * also defines 16140 and 32280 half/quarter rates; no strap on this
	 * bench transmits at either, and every extra row costs the whole table
	 * a search timeout per sweep, so they are deliberately absent.)
	 *
	 * With CONFIG_ANT_DONGLE_PROFILES_EXTRA off this is the ONLY row,
	 * because rx_tap.c then has only the heart-rate adapter: acquiring
	 * anything else would spend a channel and a binding on a device whose
	 * broadcasts are dropped. */
	{ 0x78u, 8070u, SELF_SEARCH_TICKS_25_S, "heart rate" },

#ifdef CONFIG_ANT_DONGLE_PROFILES_EXTRA
	/* PROFILE_POWER_PERIOD, radiant/src/profiles/profile_power.h: 8182
	 * counts, ~4.005 Hz. Note it is NOT 8192 - the Bicycle Power profile is
	 * one of the few that is not exactly 4 Hz, and a reader "tidying" this
	 * to match its neighbours would silently delete power-meter support. */
	{ 0x0Bu, 8182u, SELF_SEARCH_TICKS_25_S, "bicycle power" },

	/* PROFILE_FEC_PERIOD, radiant/src/profiles/profile_fec.h: 8192 counts,
	 * exactly 4 Hz, the one rate the Fitness Equipment profile defines.
	 * This is the indoor-trainer path - the device type Zwift's own trainer
	 * control speaks - and therefore the one that feeds "bike in use". */
	{ 0x11u, 8192u, SELF_SEARCH_TICKS_25_S, "fitness equipment" },

	/*
	 * Environment, device type 0x19. TWO ROWS, NOT ONE, AND THAT IS THE
	 * WHOLE POINT.
	 *
	 * PROFILE_ENV_PERIOD_4_HZ (8192) and PROFILE_ENV_PERIOD_0P5_HZ (65535),
	 * radiant/src/profiles/profile_env.h. The profile's section 5.1 permits
	 * BOTH, and page 0's transmission-info field is where a sensor declares
	 * which one it defaults to. The Garmin tempe - the ANT+ thermometer
	 * anyone actually owns - is the 0.5 Hz one.
	 *
	 * Reading page 0 to learn the rate and then retuning is the obvious
	 * design and it does not work: page 0 arrives on the channel, and there
	 * is no channel until the period already matches. A slave fixed at 8192
	 * never hears a tempe say it is a tempe. The only way to acquire an
	 * unknown-rate Environment sensor is to try both rates, which is
	 * exactly what a rotation over a table already does - so the 0.5 Hz
	 * variant is a row, and costs one more step of the sweep rather than
	 * any new mechanism.
	 *
	 * Both 0x19 rows get 45 s of search (PROFILE_ENV_SEARCH_TIMEOUT_S), not
	 * the ANT default 25 s; section 5.1 recommends that figure because at
	 * one transmission every two seconds a 25 s window is a thin sample of
	 * a slow master, and states it for the profile rather than one rate.
	 */
	{ 0x19u, 8192u,  SELF_SEARCH_TICKS_45_S, "environment (4 Hz)" },
	{ 0x19u, 65535u, SELF_SEARCH_TICKS_45_S, "environment (0.5 Hz)" },
#endif
};

#define SELF_PROFILE_N ARRAY_SIZE(self_profiles)

/*
 * Per-channel state. Written and read only by the polling thread below, so it
 * needs no synchronisation - and saying so here is cheaper than someone later
 * adding a mutex to be safe.
 */
struct self_chan {
	uint8_t  row;           /* discovery profile, an index into self_profiles */
	bool     owned;         /* we assigned it since the last host reset */
	bool     open_pinned;   /* the current open is configure_pinned_and_open()'s */
	bool     open_discover; /* the current open is a discovery search */
	uint32_t last_tracking_ms; /* the last poll that saw it TRACKING its bound sensor */
};

static struct self_chan self[SELF_N];

/*
 * Channels are claimed from the TOP of the allocation downward, and this is
 * not cosmetic. Every ANT host library allocates from channel 0 upward, so a
 * PC attached to a dongle that is also running self channels collides last
 * this way rather than first. There is no arbitration between the two - the
 * host's MESG_ASSIGN_CHANNEL for a channel this file has assigned will simply
 * be refused CHANNEL_IN_WRONG_STATE - which is a large part of why
 * CONFIG_ANT_DONGLE_SELF_CHANNELS defaults to 0. The CAPABILITIES reply
 * (ant_serial_bridge.c) tells the host about the channels below these only,
 * so a host that believes it never reaches them.
 */
static uint8_t self_channel_index(uint8_t i)
{
	return (uint8_t)(ANT_BRIDGE_SELF_CHANNEL_TOP - i);
}

/* Pairing window deadline in uptime ms; 0 means closed. */
static volatile int64_t window_until_ms;

static bool window_open(void)
{
	int64_t until = window_until_ms;

	return until != 0 && k_uptime_get() < until;
}

void self_channels_open_pairing_window(void)
{
	window_until_ms = k_uptime_get() +
			  (int64_t)CONFIG_ANT_DONGLE_PAIRING_WINDOW_S * 1000;
	LOG_INF("pairing window open for %d s",
		CONFIG_ANT_DONGLE_PAIRING_WINDOW_S);
}

/* -- The button ---------------------------------------------------- */

#if DT_NODE_HAS_STATUS(DT_ALIAS(sw0), okay)
#include <zephyr/drivers/gpio.h>

static const struct gpio_dt_spec pair_button =
	GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback pair_cb;

static void pair_pressed(const struct device *dev, struct gpio_callback *cb,
			 uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	/* ISR context. Only a timestamp is written; every decision that
	 * follows is taken on the polling thread below, where an antr_* call
	 * is legal. */
	window_until_ms = k_uptime_get() +
			  (int64_t)CONFIG_ANT_DONGLE_PAIRING_WINDOW_S * 1000;
}

static void button_init(void)
{
	int rc;

	if (!gpio_is_ready_dt(&pair_button)) {
		LOG_ERR("pairing button not ready - NOTHING CAN EVER BIND on "
			"this build");
		return;
	}

	rc = gpio_pin_configure_dt(&pair_button, GPIO_INPUT);
	if (rc == 0) {
		rc = gpio_pin_interrupt_configure_dt(&pair_button,
						     GPIO_INT_EDGE_TO_ACTIVE);
	}
	if (rc != 0) {
		LOG_ERR("pairing button setup failed: %d - NOTHING CAN EVER "
			"BIND on this build", rc);
		return;
	}

	gpio_init_callback(&pair_cb, pair_pressed, BIT(pair_button.pin));
	(void)gpio_add_callback(pair_button.port, &pair_cb);
	LOG_INF("pairing button ready");
}
#else
static void button_init(void)
{
	/*
	 * No sw0 alias on this board. Say so loudly rather than falling back
	 * to binding automatically: the fallback would be exactly the
	 * promiscuous ingest section 10.1 rule 1 forbids, arrived at by a
	 * board file rather than by a decision.
	 */
	LOG_WRN("no sw0 alias on this board: there is no way to open a pairing "
		"window, so no device will ever bind and the self channels will "
		"stay idle.");
}
#endif

/* -- The channels -------------------------------------------------- */

/* Returns the stack's answer and logs nothing: what a refusal means depends on
 * why the caller was assigning - see log_assign_refusal(). */
static antr_err_t assign_one(uint8_t i)
{
	antr_err_t rc;

	rc = antr_channel_assign(self_channel_index(i), ANTW_CHANNEL_TYPE_SLAVE,
				 SELF_NETWORK, 0u);
	if (rc != 0u) {
		return rc;
	}
	self[i].owned = true;
	self[i].open_pinned = false;
	self[i].open_discover = false;
	return 0u;
}

/*
 * CHANNEL_IN_WRONG_STATE on an assign is a channel somebody else already
 * assigned, and after a host reset that somebody is the host: its ASSIGN
 * landed between this poll reading the channel unassigned and assigning it.
 * That is the reset race self_key_poll()'s caller leaves as designed, and the
 * channel is the host's from then on - the next poll sees it assigned but not
 * owned and takes the RELEASE path for whatever was bound to it. Saying
 * NOTHING CAN EVER BIND there was false. Any other refusal still means exactly
 * that.
 */
static void log_assign_refusal(uint8_t ch, antr_err_t rc)
{
	if (rc == (antr_err_t)ANTW_CHANNEL_IN_WRONG_STATE) {
		LOG_INF("self channel %u: the host assigned it first; it is the "
			"host's until its next reset", ch);
	} else {
		LOG_ERR("channel %u assign: %u - NOTHING CAN EVER BIND on this "
			"channel", ch, rc);
	}
}

/*
 * The most device IDs one channel's exclusion list can hold, on either backend:
 * the radiant backend's API_ID_LIST_MAX (apps/common/ant_radio_radiant.c,
 * "what a retail stick accepts") and sdk-ant's ant_id_list_config() list size
 * (0-4, include/ant_interface.h). One more is ANTW_INVALID_LIST_ID.
 */
#define SELF_ID_LIST_MAX 4u

/*
 * Give a discovering channel an EXCLUSION list of the devices bound to the
 * other self channels, so its wildcard search cannot acquire them.
 *
 * Why it is needed, from the bench: after an eviction, the freed channel's
 * search kept acquiring the sensor PINNED on the other self channel - a
 * device the radio has cached and hears every period, so the search steered
 * straight to it - and try_bind() refused it ("not binding it twice"),
 * rotate_and_reopen() tore the channel down and rebuilt it, and the same
 * thing happened again a second later, for as long as the window stayed open.
 * It never reached the new sensor the button was pressed for. With the list,
 * the search skips the pinned devices and the duplicate check in try_bind()
 * is a backstop for a device that binds elsewhere after this search opened.
 *
 * Backend-agnostic by construction: the list is the ANT API's own
 * (antr_id_list_add()/_config(), ant_radio.h), native on sdk-ant, and on
 * radiant consulted at steer time, so the sweep does not chase an excluded
 * device, and again at offer time, when an acquisition is handed to the
 * channel (api_id_list_permits(), ant_radio_radiant.c).
 *
 * The list outlives a close and an unassign, so it is ALWAYS reconfigured
 * here, with size 0 when nothing is bound elsewhere: an exclusion written on
 * an earlier open would otherwise still apply. Nothing here is fatal. A
 * refused call leaves a search that may find a pinned device again, which
 * try_bind() catches; and a partly written list is enabled only for the
 * entries that were actually written.
 *
 * The channel must be assigned and closed.
 */
static void exclude_pinned(uint8_t ch)
{
	static bool overflow_logged;
	uint8_t n = 0u;
	uint8_t j;
	antr_err_t rc;

	for (j = 0u; j < SELF_N; j++) {
		uint8_t other = self_channel_index(j);
		uint32_t source;
		const struct radiant_binding *b;
		uint8_t id[4];

		if (other == ch) {
			continue;
		}
		source = ant_bridge_channel_source(other);
		if (source == RADIANT_BINDING_NONE) {
			continue;
		}
		b = radiant_binding_get(source);
		if (b == NULL) {
			continue;
		}
		if (n >= SELF_ID_LIST_MAX) {
			/* Only with six or more self channels (Kconfig allows
			 * eight). The ones left out can still be found, and
			 * try_bind() turns them away. */
			if (!overflow_logged) {
				overflow_logged = true;
				LOG_WRN("more than %u devices pinned on other self "
					"channels: the discovery exclusion list holds "
					"%u, the rest are caught at bind time",
					SELF_ID_LIST_MAX, SELF_ID_LIST_MAX);
			}
			break;
		}

		/* Wire order: [devnum_lo, devnum_hi, devtype, trans_type]. */
		id[0] = (uint8_t)(b->devnum & 0xFFu);
		id[1] = (uint8_t)(b->devnum >> 8);
		id[2] = b->devtype;
		id[3] = b->trans_type;
		rc = antr_id_list_add(ch, id, n);
		if (rc != 0u) {
			LOG_WRN("channel %u exclusion list add %u: %u", ch, n,
				rc);
			break;
		}
		n++;
	}

	rc = antr_id_list_config(ch, n, 1u);
	if (rc != 0u) {
		LOG_WRN("channel %u exclusion list (%u entries): %u - a pinned "
			"device may be found again and rotated away", ch, n, rc);
	}
}

/*
 * Point an ASSIGNED, CLOSED channel at one discovery row and put it on air.
 *
 * All four configuration calls are legal in exactly this state and in no
 * other: ant_radio.h says antr_channel_id_set(), antr_channel_period_set() and
 * antr_channel_radio_freq_set() answer ANTW_CHANNEL_IN_WRONG_STATE when the
 * channel is "unassigned or open", and the radiant backend implements that as
 * require_assigned_closed() (radiant/src/radiant_channel.c). A channel whose
 * search has just timed out is precisely assigned-and-closed - the backend
 * raises RX_SEARCH_TIMEOUT and then closes itself, in that order - so this
 * needs NO unassign/reassign to retune.
 */
static int configure_and_open(uint8_t ch, const struct self_profile *p)
{
	antr_err_t rc;

	/* All three ID fields wildcard except the device type. Fully wildcard
	 * (0) is not the more general option it looks like: it widens the ID
	 * match while leaving the period fixed, so it acquires
	 * indiscriminately AND still only within one profile's rate. */
	rc = antr_channel_id_set(ch, 0u, p->device_type, 0u);
	if (rc != 0u) {
		LOG_WRN("channel %u id: %u", ch, rc);
		return -EIO;
	}

	rc = antr_channel_period_set(ch, p->period_counts);
	if (rc != 0u) {
		LOG_WRN("channel %u period: %u", ch, rc);
		return -EIO;
	}

	rc = antr_channel_radio_freq_set(ch, SELF_RF_FREQ);
	if (rc != 0u) {
		LOG_WRN("channel %u freq: %u", ch, rc);
		return -EIO;
	}

	/* Deliberately not fatal. A backend that refuses this leaves the ANT
	 * default 25 s in place, which is right for three rows of four and
	 * merely thin for the 0.5 Hz one - a worse search, not a broken
	 * channel. */
	rc = antr_channel_search_timeout_set(ch, p->search_ticks);
	if (rc != 0u) {
		LOG_WRN("channel %u search timeout: %u (leaving the default)",
			ch, rc);
	}

	/* Reached only with the channel assigned and closed - every set above
	 * refuses anything else, and rotate_and_reopen()'s first try on a
	 * still-OPEN channel leaves at antr_channel_id_set() - which is the
	 * one state the list calls accept. */
	exclude_pinned(ch);

	rc = antr_channel_open(ch);
	if (rc != 0u) {
		LOG_WRN("channel %u open: %u", ch, rc);
		return -EIO;
	}

	LOG_INF("self channel %u searching for %s (devtype 0x%02x, period %u, "
		"wildcard device number)", ch, p->name, p->device_type,
		p->period_counts);
	return 0;
}

static void discover_open(uint8_t i)
{
	if (configure_and_open(self_channel_index(i), &self_profiles[self[i].row]) == 0) {
		self[i].open_discover = true;
		self[i].open_pinned = false;
	}
}

/*
 * Discovery timed out, or is holding a device another channel already
 * receives. Advance one row and reopen, so that N channels sweep the whole
 * table over time instead of retrying one profile forever.
 *
 * The fallback is the reason for the extra calls. configure_and_open() is
 * correct for the assigned-and-closed state that a timed-out search leaves
 * behind - but a channel rotated away from a duplicate is still OPEN, and the
 * backend runs on its own thread, so a refusal here means the channel is not
 * where we thought it was. Rather than open it with a half-applied
 * configuration (a channel searching for the new device type at the old period
 * would find nothing, forever, silently), tear it down and rebuild: close,
 * unassign - which ant_radio.h requires a closed channel for, hence the order
 * - and reassign. Both leading calls may fail; each is a no-op when the
 * channel is already past that state.
 */
static void rotate_and_reopen(uint8_t i)
{
	uint8_t ch = self_channel_index(i);

	self[i].row = (uint8_t)((self[i].row + 1u) % SELF_PROFILE_N);
	if (configure_and_open(ch, &self_profiles[self[i].row]) == 0) {
		self[i].open_discover = true;
		self[i].open_pinned = false;
		return;
	}

	LOG_WRN("self channel %u would not retune in place - reassigning", ch);
	(void)antr_channel_close(ch);
	(void)antr_channel_unassign(ch);
	{
		antr_err_t rc = assign_one(i);

		if (rc == 0u) {
			discover_open(i);
		} else {
			log_assign_refusal(ch, rc);
		}
	}
}

/*
 * Put an ASSIGNED, CLOSED, bound channel on air searching for its sensor and
 * nothing else: the binding's device number, device type AND transmission
 * type, the period the channel had when it bound, and an infinite search.
 *
 * trans_type is pinned too, which is one place this differs from Zwift (it
 * leaves the transmission type wildcard 0). Its upper nibble extends the
 * device number, and a wildcard there would let tracking_bound_device()'s
 * trans comparison flap between two sensors that differ only in it.
 */
static int configure_pinned_and_open(uint8_t i, const struct radiant_binding *b)
{
	uint8_t ch = self_channel_index(i);
	uint16_t period = b->period;
	antr_err_t rc;

	if (period == 0u) {
		/* The read at bind time failed. The row it was found on is the
		 * best guess left; liveness already says it has no expiry. */
		period = self_profiles[self[i].row].period_counts;
	}

	rc = antr_channel_id_set(ch, b->devnum, b->devtype, b->trans_type);
	if (rc == 0u) {
		rc = antr_channel_period_set(ch, period);
	}
	if (rc == 0u) {
		rc = antr_channel_radio_freq_set(ch, SELF_RF_FREQ);
	}
	if (rc != 0u) {
		LOG_WRN("channel %u could not be pinned: %u", ch, rc);
		return -EIO;
	}
	rc = antr_channel_search_timeout_set(ch, SELF_SEARCH_TICKS_FOREVER);
	if (rc != 0u) {
		/* Survivable: a pinned channel whose search times out comes
		 * back here on the next poll and is reopened pinned. */
		LOG_WRN("channel %u infinite search refused: %u", ch, rc);
	}
	/* The channel names its device exactly, so it wants no list - and the
	 * exclusion list exclude_pinned() gave it while it was discovering
	 * outlives the close, so it is switched off here rather than left to
	 * apply to a pinned search. Not fatal: an exclusion list only ever
	 * names devices bound to OTHER channels, never this one's. */
	rc = antr_id_list_config(ch, 0u, 0u);
	if (rc != 0u) {
		LOG_WRN("channel %u id list off: %u", ch, rc);
	}
	rc = antr_channel_open(ch);
	if (rc != 0u) {
		LOG_WRN("channel %u open: %u", ch, rc);
		return -EIO;
	}

	self[i].open_pinned = true;
	self[i].open_discover = false;
	LOG_INF("self channel %u pinned to device %u (type 0x%02x, trans %u, "
		"period %u)", ch, b->devnum, b->devtype, b->trans_type, period);
	return 0;
}

/* Close for a reason this file chose. The next poll sees the channel ASSIGNED
 * and decides from there - which is also what makes an asynchronous close
 * (sdk-ant) work the same as a synchronous one (radiant). */
static void close_one(uint8_t i)
{
	(void)antr_channel_close(self_channel_index(i));
	self[i].open_pinned = false;
	self[i].open_discover = false;
}

/* The binding behind channel i is released: the tap stops attributing the
 * channel's traffic, the row is freed (so a ninth distinct pairing is not
 * "binding table full"), and every sink is told the source is gone. */
static void release_binding(uint8_t i)
{
	uint8_t ch = self_channel_index(i);
	uint32_t source = ant_bridge_channel_source(ch);

	ant_bridge_channel_unbind(ch);
	if (source != RADIANT_BINDING_NONE) {
		(void)radiant_binding_unbind(source);
		radiant_bridge_binding_changed(source, NULL);
	}
}

/*
 * A tracking discovery channel with no binding, inside the pairing window.
 * Everything that makes binding legal happens here: the identity read, the
 * one-device-one-channel check, the bind, the period read-back, and the map
 * entry the tap consults. Called only when the policy said BIND, which it says
 * only with the window open - section 10.1 rule 1 in code.
 *
 * The identity comes from antr_channel_id_get(), never from the table row
 * the channel was searching on. A wildcard search resolves to a real device
 * type, and it is the resolved one that reaches radiant_binding_bind() and
 * therefore rx_tap.c's devtype dispatch - so a binding records what the
 * sensor IS, not what was being looked for.
 *
 * Returns false if the channel is holding a device another channel already
 * receives, and must be rotated away by the caller; true in every other case
 * (bound, or nothing to do yet).
 */
static bool try_bind(uint8_t ch)
{
	uint16_t devnum = 0u;
	uint8_t  devtype = 0u;
	uint8_t  trans = 0u;
	uint16_t period = 0u;
	uint32_t source = RADIANT_BINDING_NONE;
	uint32_t existing;
	int      rc;

	if (antr_channel_id_get(ch, &devnum, &devtype, &trans) != 0u) {
		return true;
	}
	if (devnum == 0u) {
		/* Tracking but the wildcard has not resolved yet. */
		return true;
	}

	/*
	 * ONE DEVICE, ONE CHANNEL. Measured on this bench: the Wahoo #52233 was
	 * acquired on channel 6, and 47 s later a rotating channel 7 acquired
	 * THE SAME DEVICE and bound it again as the same source 0.
	 *
	 * radiant_binding_bind() de-duplicates - it returns the existing source
	 * rather than a second row - so the damage is all on this side of it: a
	 * self channel spent on a device already pinned elsewhere is one that
	 * never searches again; ant_bridge_channel_bind() RESETS EVERY ADAPTER
	 * for the source, so a re-bind zeroes the sensor's accumulators
	 * mid-session; and radiant_bridge_binding_changed() re-arms every sink's
	 * per-binding state.
	 *
	 * With pinning this would be the common case: a pinned sensor that
	 * dropped out is still being searched for on its own channel, and a
	 * discovering channel can find it first - and after an eviction the
	 * freed channel found the other channel's pinned sensor every second,
	 * forever. exclude_pinned() now keeps a discovering search off every
	 * device bound elsewhere WHEN IT OPENS, so this is the backstop: a
	 * device that bound on another channel after this search opened, one
	 * past the list's four entries, or a backend that refused the list.
	 * The pinned channel gets it back; this one moves on (false).
	 */
	existing = radiant_binding_find(devnum, devtype, trans);
	if (existing != RADIANT_BINDING_NONE) {
		uint8_t i;

		for (i = 0u; i < SELF_N; i++) {
			uint8_t other = self_channel_index(i);

			if (other == ch) {
				continue;
			}
			if (ant_bridge_channel_source(other) == existing) {
				LOG_INF("channel %u is tracking device %u, which "
					"channel %u is pinned to - not binding it "
					"twice, rotating instead",
					ch, devnum, other);
				return false;
			}
		}
	}

	rc = radiant_binding_bind(devnum, devtype, trans, NULL, &source);
	if (rc != 0) {
		LOG_ERR("binding table full: device %u not bound (%d)", devnum,
			rc);
		return true;
	}

	/*
	 * The period comes off the channel, not from the table row this channel
	 * happens to be searching on: the row is a statement of what was ASKED
	 * FOR rather than of what the channel settled on. This is the number
	 * radiant_liveness.c multiplies by 3 to decide the sensor has gone away,
	 * and the one the pinned channel is reopened at. A read that fails leaves
	 * period 0, which the liveness table treats as "no honest expiry" and
	 * counts - see radiant_liveness.h.
	 */
	if (antr_channel_period_get(ch, &period) == 0u) {
		(void)radiant_binding_set_period(source, period);
	} else {
		LOG_WRN("channel %u period read failed: source %u will never "
			"be marked stale", ch, source);
	}

	(void)ant_bridge_channel_bind(ch, source);
	radiant_bridge_binding_changed(source, radiant_binding_get(source));

	LOG_INF("bound device %u (type 0x%02x, trans %u) on channel %u as "
		"source %u, period %u", devnum, devtype, trans, ch, source,
		period);
	/* -Werror=return-type on this file (CMakeLists.txt): this return was
	 * once missing and the built code returned a stale register, rotating
	 * a just-bound channel away. */
	return true;
}

/* Is a bound channel tracking the device it is bound to? On radiant a pinned
 * id cannot acquire anything else, so this mainly guards sdk-ant and the
 * moment between a bind and its re-pin. Unresolved id = no evidence. */
static bool tracking_bound_device(uint8_t ch)
{
	const struct radiant_binding *b =
		radiant_binding_get(ant_bridge_channel_source(ch));
	uint16_t devnum = 0u;
	uint8_t  devtype = 0u;
	uint8_t  trans = 0u;

	if (b == NULL) {
		return false;
	}
	if (antr_channel_id_get(ch, &devnum, &devtype, &trans) != 0u ||
	    devnum == 0u) {
		return true;
	}
	return devnum == b->devnum && devtype == b->devtype &&
	       trans == b->trans_type;
}

/*
 * The self network's key, against what the host has done to it. Called at boot
 * and on every poll; radiant_self_key_decide() makes it a no-op except once per
 * host reset (or boot) and once per host key write.
 *
 * Why this exists: a host SYSTEM_RESET - Zwift sends one at startup - wipes
 * every channel and every key, ours included, and the poll below used to find
 * its channels UNASSIGNED and do nothing about it. The bridge then received
 * nothing until the next reboot.
 *
 * Returns true when this call is the first to see a reset, i.e. every self
 * channel was wiped since the last poll.
 */
static struct radiant_self_key_state key_state;

static bool self_key_poll(void)
{
	struct ant_bridge_host_state hs;
	struct radiant_self_key_in in;
	bool new_generation = false;

	ant_serial_bridge_host_state(SELF_NETWORK, &hs);
	in.reset_gen = hs.reset_gen;
	in.key_writes = hs.key_writes;
	in.key_written = hs.key_written;
	in.key_is_ours = hs.key_written &&
			 memcmp(hs.key, ant_plus_key, sizeof(ant_plus_key)) == 0;

	switch (radiant_self_key_decide(&key_state, &in, &new_generation)) {
	case RADIANT_SELF_KEY_APPLY:
		if (antr_network_address_set(SELF_NETWORK, ant_plus_key) != 0u) {
			LOG_ERR("ANT+ network key rejected - self channels will "
				"hear nothing");
			break;
		}
		/* The host may have written its own key between the snapshot
		 * and the write above. The host owns the stack, so put its key
		 * back; the next poll reports it if it is not the ANT+ one. */
		ant_serial_bridge_host_state(SELF_NETWORK, &hs);
		if (hs.key_writes != in.key_writes && hs.key_written) {
			(void)antr_network_address_set(SELF_NETWORK, hs.key);
		}
		break;
	case RADIANT_SELF_KEY_HOST_FOREIGN:
		LOG_WRN("the host set its own key on network %u: self channels "
			"hear nothing until it sets the ANT+ key or resets",
			SELF_NETWORK);
		break;
	default:
		break;
	}
	return new_generation;
}

static enum radiant_self_radio radio_of(uint8_t status)
{
	switch (status & ANTW_STATUS_CHANNEL_STATE_MASK) {
	case ANTW_STATUS_ASSIGNED_CHANNEL:
		return RADIANT_SELF_RADIO_ASSIGNED;
	case ANTW_STATUS_SEARCHING_CHANNEL:
		return RADIANT_SELF_RADIO_SEARCHING;
	case ANTW_STATUS_TRACKING_CHANNEL:
		return RADIANT_SELF_RADIO_TRACKING;
	default:
		return RADIANT_SELF_RADIO_UNASSIGNED;
	}
}

static void act(uint8_t i, enum radiant_self_action a, bool booting)
{
	uint8_t ch = self_channel_index(i);

	switch (a) {
	case RADIANT_SELF_REESTABLISH: {
		antr_err_t rc = assign_one(i);

		if (rc != 0u) {
			log_assign_refusal(ch, rc);
		} else if (!booting) {
			LOG_INF("self channel %u re-established after host reset",
				ch);
		}
		break;
	}
	case RADIANT_SELF_RELEASE:
		LOG_WRN("self channel %u was taken by the host after its reset - "
			"releasing the sensor bound to it", ch);
		release_binding(i);
		break;
	case RADIANT_SELF_PIN_REOPEN: {
		const struct radiant_binding *b =
			radiant_binding_get(ant_bridge_channel_source(ch));
		uint8_t status = 0u;

		if (antr_channel_status_get(ch, &status) == 0u &&
		    radio_of(status) == RADIANT_SELF_RADIO_ASSIGNED) {
			if (b != NULL) {
				(void)configure_pinned_and_open(i, b);
			}
		} else {
			close_one(i);
		}
		break;
	}
	case RADIANT_SELF_BIND:
		if (!try_bind(ch)) {
			rotate_and_reopen(i);
		} else if (ant_bridge_channel_source(ch) != RADIANT_BINDING_NONE) {
			/* Bound: close, and the next poll reopens it pinned. */
			self[i].last_tracking_ms = k_uptime_get_32();
			close_one(i);
		}
		break;
	case RADIANT_SELF_DISCOVER_OPEN:
		discover_open(i);
		break;
	case RADIANT_SELF_ROTATE:
		if (self[i].open_discover) {
			LOG_INF("self channel %u search timed out on %s - rotating",
				ch, self_profiles[self[i].row].name);
		}
		rotate_and_reopen(i);
		break;
	case RADIANT_SELF_CLOSE_TO_IDLE:
		close_one(i);
		break;
	default:
		break;
	}
}

static void self_thread_fn(void *a, void *b, void *c)
{
	struct radiant_self_evict_in evict[SELF_N];
	enum radiant_self_radio radio[SELF_N];
	int64_t last_window = 0;
	bool opened;
	bool booting = true;
#if defined(CONFIG_ANT_DONGLE_PAIRING_WINDOW_BENCH_REPEAT_S) && \
	CONFIG_ANT_DONGLE_PAIRING_WINDOW_BENCH_REPEAT_S > 0
	int64_t last_bench_open = k_uptime_get();
#endif
	uint8_t i;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	LOG_INF("self channels: %d, from channel %u downward", SELF_N,
		self_channel_index(0));

	button_init();

#if defined(CONFIG_ANT_DONGLE_PAIRING_WINDOW_AT_BOOT)
	/*
	 * A BENCH INSTRUMENT, NOT A FEATURE: off by default, selected by no
	 * shipping image, and it says on the console exactly what it did so a
	 * log can never be mistaken for one from a normal build.
	 *
	 * It exists because the liveness STALE path is the one thing in the
	 * bridge that cannot be verified without a binding, a binding needs the
	 * pairing window, and the window needs a finger on sw0. That is correct
	 * for the product - section 10.1 rule 1 makes binding explicit opt-in
	 * and says so cannot be retrofitted - and it makes the single most
	 * important bridge test unreachable from an automated bench.
	 *
	 * This does NOT weaken the rule it works around. The window still
	 * opens once, still expires after CONFIG_ANT_DONGLE_PAIRING_WINDOW_S,
	 * and still binds only devices acquired inside it; what changes is who
	 * opened it.
	 */
	LOG_WRN("BENCH BUILD: opening the pairing window at boot without a "
		"button press. No shipping image does this - see "
		"CONFIG_ANT_DONGLE_PAIRING_WINDOW_AT_BOOT.");
	self_channels_open_pairing_window();
#endif

	/* Channel i starts on row (i mod N of the table), so a two-channel
	 * build discovers two profiles at once rather than running two
	 * identical searches side by side. */
	for (i = 0u; i < SELF_N; i++) {
		self[i].row = (uint8_t)(i % SELF_PROFILE_N);
	}

	/* Boot is the first generation: the key is applied here unless a host
	 * already set one in the delay before this thread started. The
	 * channels themselves are assigned by the first pass of the loop - a
	 * boot is a reset this file has not yet re-established from. */
	radiant_self_key_init(&key_state);
	(void)self_key_poll();

	for (;;) {
		bool win;
		uint32_t now;
		uint8_t pick;

		if (self_key_poll()) {
			/* A host reset wiped every channel. Whatever the host
			 * assigns from here is its own. */
			for (i = 0u; i < SELF_N; i++) {
				self[i].owned = false;
				self[i].open_pinned = false;
				self[i].open_discover = false;
			}
		}

#if defined(CONFIG_ANT_DONGLE_PAIRING_WINDOW_BENCH_REPEAT_S) && \
	CONFIG_ANT_DONGLE_PAIRING_WINDOW_BENCH_REPEAT_S > 0
		/* The bench instrument's second half: see the Kconfig help. */
		if (k_uptime_get() - last_bench_open >=
		    (int64_t)CONFIG_ANT_DONGLE_PAIRING_WINDOW_BENCH_REPEAT_S * 1000) {
			last_bench_open = k_uptime_get();
			LOG_WRN("BENCH BUILD: reopening the pairing window (every "
				"%d s, CONFIG_ANT_DONGLE_PAIRING_WINDOW_BENCH_REPEAT_S)",
				CONFIG_ANT_DONGLE_PAIRING_WINDOW_BENCH_REPEAT_S);
			self_channels_open_pairing_window();
		}
#endif

		now = k_uptime_get_32();
		win = window_open();
		/* Eviction is decided on the poll that first sees a window open,
		 * and only then: a press of the button is a request for one free
		 * channel now, not a licence to evict any sensor that blips while
		 * the window happens to be open. */
		opened = false;
		if (window_until_ms != last_window) {
			last_window = window_until_ms;
			opened = win;
		}

		for (i = 0u; i < SELF_N; i++) {
			uint8_t status = 0u;
			uint8_t ch = self_channel_index(i);
			bool bound = ant_bridge_channel_source(ch) != RADIANT_BINDING_NONE;

			radio[i] = (antr_channel_status_get(ch, &status) == 0u)
					   ? radio_of(status)
					   : RADIANT_SELF_RADIO_UNASSIGNED;
			if (bound && self[i].owned &&
			    radio[i] == RADIANT_SELF_RADIO_TRACKING) {
				self[i].last_tracking_ms = now;
			}
			/* A channel that is not ours is neither free nor
			 * evictable: present it as tracking and bound. */
			evict[i].bound = bound || !self[i].owned;
			evict[i].tracking = !self[i].owned ||
					    radio[i] == RADIANT_SELF_RADIO_TRACKING;
			evict[i].last_tracking_ms = self[i].last_tracking_ms;
		}

		pick = opened ? radiant_self_pick_evict(evict, SELF_N, win, false, now)
			      : RADIANT_SELF_EVICT_NONE;
		if (opened && pick == RADIANT_SELF_EVICT_NONE) {
			bool full = true;

			for (i = 0u; i < SELF_N; i++) {
				full = full && evict[i].bound;
			}
			if (full) {
				/* The one refusal a user would otherwise read as
				 * "the button does nothing". */
				LOG_INF("pairing window open, but every self channel "
					"is tracking a paired sensor - nothing to evict, "
					"so nothing new can pair");
			}
		}
		if (pick != RADIANT_SELF_EVICT_NONE) {
			const struct radiant_binding *eb = radiant_binding_get(
				ant_bridge_channel_source(self_channel_index(pick)));

			LOG_INF("evicting device %u from self channel %u (absent "
				"%u s) to make room for a new pairing",
				eb != NULL ? eb->devnum : 0u, self_channel_index(pick),
				(now - self[pick].last_tracking_ms) / 1000u);
			release_binding(pick);
			close_one(pick);
		}

		for (i = 0u; i < SELF_N; i++) {
			uint8_t ch = self_channel_index(i);
			struct radiant_self_chan_in in;

			if (i == pick) {
				continue; /* acted on above; re-read next poll */
			}
			in.radio = radio[i];
			in.owned = self[i].owned;
			in.bound = ant_bridge_channel_source(ch) != RADIANT_BINDING_NONE;
			in.open_pinned = self[i].open_pinned;
			in.open_discover = self[i].open_discover;
			in.window_open = win;
			in.identity_matches = !(in.bound && in.owned &&
						radio[i] == RADIANT_SELF_RADIO_TRACKING) ||
					      tracking_bound_device(ch);
			if (in.bound && in.owned && radio[i] == RADIANT_SELF_RADIO_TRACKING &&
			    !in.identity_matches) {
				LOG_WRN("channel %u is tracking a device other than the "
					"one pinned to it - re-pinning", ch);
			}
			act(i, radiant_self_decide(&in), booting);
		}

		booting = false;
		k_sleep(K_SECONDS(1));
	}
}

#define SELF_STACK_SIZE 1536

/*
 * Priority below the ANT host thread for the same reason bridge_pump.c's is:
 * nothing here is time-critical, and a once-a-second status poll must never be
 * in a position to delay a receive window.
 */
K_THREAD_DEFINE(self_channels_tid, SELF_STACK_SIZE, self_thread_fn, NULL, NULL,
		NULL, ANTR_HOST_THREAD_PRIORITY + 2, 0,
		CONFIG_ANT_DONGLE_SELF_CHANNELS_DELAY_S * 1000);
