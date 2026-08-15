/* SPDX-License-Identifier: Apache-2.0 */
/*
 * self_channels.c - wildcard slave channels this dongle drives itself, and the
 * pairing window that decides what may bind.
 *
 * ---------------------------------------------------------------------------
 * WHY THERE IS A BUTTON AND NOT AN AUTO-BIND
 * ---------------------------------------------------------------------------
 *
 * A rebroadcast bridge that only works while a PC is plugged in is not the
 * product docs/radiant-bridge.md section 1 describes ("one QR scan, nothing
 * installed"), so the dongle has to open its own channels. The obvious next
 * step - bind whatever those channels acquire - is the one thing the design
 * refuses. Section 10.1 rule 1 and section 5 both say binding is explicit
 * opt-in, and section 10.1 says why the rule is stated as architecture rather
 * than as a privacy footnote: it is ALSO the radio rule, because promiscuous
 * ingest means continuous scan and section 7 shows continuous scan is the one
 * mode that cannot coexist with Thread. Auto-binding every strap in range to
 * make a demo work would violate a rule the document says cannot be
 * retrofitted.
 *
 * So: a button press opens a window. Inside it, a self channel that has
 * acquired a device may bind. Outside it, that channel keeps tracking and its
 * broadcasts are dropped by rx_tap.c - which is the honest behaviour, not a
 * degraded one.
 *
 * A DEVICE ALREADY ACQUIRED WHEN THE BUTTON IS PRESSED ALSO BINDS, and that is
 * deliberate rather than sloppy. The alternative - only devices acquired
 * strictly after the press - would mean a user has to press the button, then
 * power-cycle the strap, and get the order right; the press is the explicit
 * act either way, and the window is what bounds it.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS RUNS ON A DELAYED THREAD AND NOT ON THE POST-RADIO HOOK
 * ---------------------------------------------------------------------------
 *
 * ant_dongle_post_radio_init() is the correct place - it is defined to run
 * after antr_init() has returned and before the transport - but this
 * application's single override of it already belongs to the coexistence
 * loads (src/main.c), and a weak hook has exactly one strong definition. So
 * this starts on its own thread after CONFIG_ANT_DONGLE_SELF_CHANNELS_DELAY_S
 * seconds instead, which is the same delayed-start idiom thread_coex_load.c
 * and ble_coex_load.c already use, for the same reason they use it (a stack
 * that starts at millisecond zero makes "it never worked" and "it stopped when
 * X started" the same observation).
 *
 * The delay is a real precondition, not caution: ant_radio.h forbids any
 * antr_* call before antr_init() returns, and the boot sequence reaches
 * antr_init() within the UF2 settle delay plus a few milliseconds. The default
 * of 5 s is comfortably past it. If src/main.c's hook is ever reopened, moving
 * these calls into it is strictly better and this comment is the note saying
 * so.
 *
 * ---------------------------------------------------------------------------
 * WHY IT POLLS CHANNEL STATUS RATHER THAN WATCHING THE RX TAP
 * ---------------------------------------------------------------------------
 *
 * Identity resolution needs antr_channel_id_get() and the period needs
 * antr_channel_period_get(). The RX tap runs inside antr_on_message(), on the
 * backend's own event thread, and calling back into an antr_* function from
 * there is the re-entrancy the backend's own comments (ant_radio_radiant.c
 * around the api_lock) exist to prevent. Polling from this thread costs one
 * status read per channel per second and needs no such favour.
 *
 * ---------------------------------------------------------------------------
 * WHY THERE IS A TABLE OF SEARCH PROFILES AND WHY IT ROTATES
 * ---------------------------------------------------------------------------
 *
 * An ANT slave's channel period must MATCH its master's, and its device type
 * must match or be the wildcard 0. Those two facts together mean a slave
 * cannot search for two profiles at once: 8070 counts finds heart-rate straps
 * and finds nothing else, because a power meter's slot is 8182 counts away and
 * the receive window is simply never open when it transmits. This file used to
 * hard-wire 8070/0x78, so every decoder rx_tap.c ever grows is unreachable on
 * hardware no matter how well it is tested in ztest - the acquisition layer
 * would never hand it a packet.
 *
 * The fully-wildcard alternative (device type 0) does NOT solve this. It
 * widens the ID match and leaves the period fixed, so it acquires whichever
 * sensors happen to share the one period that was compiled in, and does it
 * indiscriminately. That is worse on both axes at once.
 *
 * So: a table, one row per {device type, period} pair this bridge can decode,
 * and a channel searches one row at a time. Two things spread the cost:
 *
 *   ACROSS CHANNELS. Channel i starts on row (i mod N), so a two-channel
 *   build searches two different profiles simultaneously rather than running
 *   two identical searches beside each other.
 *
 *   ACROSS TIME. When a channel's high-priority search times out, it advances
 *   one row before reopening. N channels therefore sweep the whole table,
 *   and a console shows HR -> power -> FE-C -> env cycling by name.
 *
 * THE LOG LINE NAMING THE PROFILE IS THE POINT, not decoration. Without it,
 * "no sensors in range" and "searching for the wrong thing" produce the
 * identical console - a silent, permanently-idle bridge - and that exact
 * ambiguity is what the ANTW_STATUS_ASSIGNED_CHANNEL case below already
 * exists to avoid for a different cause.
 *
 * NONE OF THIS TOUCHES THE OPT-IN RULE. docs/radiant-bridge.md section 10.1
 * rule 1 governs what may BIND, not what a channel may search for; rotating
 * the search profile changes which sensors can be acquired and changes
 * nothing at all about which may be bound. try_bind()'s window_open() test is
 * deliberately untouched by this, and a rotation that ever grew a bind path
 * of its own would be the promiscuous ingest the rule forbids.
 *
 * ---------------------------------------------------------------------------
 * AND WHY THE TABLE IS BEHIND CONFIG_ANT_DONGLE_PROFILES_EXTRA
 * ---------------------------------------------------------------------------
 *
 * With that symbol n, this file is the file it was before packages A-C: both
 * self channels search device type 0x78 at period 8070, and NO search timeout
 * is set at all, so every channel inherits the ANT default. With it y, the
 * five-row rotation above.
 *
 * The reason is not doubt about the rotation - it is that CONFIG_RADIANT_BRIDGE
 * is also set by bridge.conf, which carries one of the five coexistence
 * baselines in docs/radiant-bridge.md section 7.4. Those numbers are
 * measurements OF THE RADIO, and this file is the file that decides what the
 * radio does: one fixed 4.06 Hz search versus a rotation across four different
 * channel periods and five device types is not the same duty cycle, not the
 * same retune rate and not the same thing to compare a Thread throughput
 * number against. docs/matter-e2-regression.md caught the rotation arriving on
 * that arm unasked, alongside +2 564 B of flash. Kconfig's help for the symbol
 * is the full argument.
 *
 * THE SHAPE OF THE GUARD IS DELIBERATE AND IS NOT "a one-row table". A single
 * row plus a rotation that happens to be modulo 1 would be tidier to read and
 * would NOT restore the old image: it still emits self_profiles[], self_row[]
 * and configure_and_open(), still carries the per-row search-timeout call that
 * the original never made, and still carries the profile-naming log strings.
 * The requirement here is byte-identity with the pre-A-C image, not
 * approximate equivalence, so the off path is the ORIGINAL code verbatim and
 * the two paths are kept adjacent and small: one #ifdef around the table and
 * the open/retune helpers, and two more inside self_thread_fn() at the exact
 * two places the old and new behaviours differ. Everything else in this file -
 * the button, the window, try_bind(), the poll loop - is shared and has one
 * copy.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ant_radio.h"
#include "ant_wire.h"

#include "radiant_binding.h"
#include "radiant_bridge.h"

#include "bridge_app.h"

LOG_MODULE_REGISTER(self_channels, LOG_LEVEL_INF);

#ifndef CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED
#define CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED 8
#endif

#define SELF_N CONFIG_ANT_DONGLE_SELF_CHANNELS

BUILD_ASSERT(SELF_N <= CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED,
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

#ifdef CONFIG_ANT_DONGLE_PROFILES_EXTRA

/*
 * ANT's high-priority search timeout is expressed in 2.5 s ticks, and its
 * default is 10 ticks = 25 s (radiant/include/radiant/radiant_channel.h's
 * RADIANT_CHANNEL_SEARCH_TIMEOUT_DEFAULT, and the same figure ant_radio.h
 * documents for the antr_* contract). This file never used to set it at all,
 * so every row inherited that default - which is correct for a 4 Hz profile
 * and NOT long enough for the 0.5 Hz Environment master; see the table.
 */
#define SELF_SEARCH_TICKS_25_S 10u
#define SELF_SEARCH_TICKS_45_S 18u

/*
 * One row per {device type, channel period} pair this bridge can acquire.
 *
 * A PERIOD IS NOT A CHOICE AND MUST NOT BE ROUNDED. It is the profile's own
 * number, in counts of 1/32768 s, and a slave told anything else does not
 * merely acquire more slowly - it never acquires at all. Each row's period is
 * transcribed from the named constant in the comment beside it, which is the
 * primary-source-derived definition in radiant/src/profiles/. Those headers
 * CANNOT BE INCLUDED FROM HERE: apps/dongle_thread/CMakeLists.txt puts only
 * radiant/src/bridge and this application's own src/ on the include path
 * (radiant/CMakeLists.txt exports radiant/include and nothing under src/), and
 * adding a directory to that list is a build-plumbing change this file is not
 * the place for. So the numbers are written out, each naming the constant it
 * must agree with, and the agreement is a review obligation rather than a
 * compiler-enforced one. If radiant/src/profiles ever reaches this app's
 * include path, replace the literals with the constants and delete this note.
 *
 * The period also reaches struct radiant_binding::period and therefore
 * radiant_liveness.c's 3x expiry - but NOT from here. try_bind() reads it back
 * off the channel with antr_channel_period_get(), which is what makes it a
 * fact about the channel rather than a constant that was true when it was
 * typed, and is why the 0.5 Hz row below is purely an ACQUISITION problem and
 * not also a liveness one.
 *
 * `name` exists so the search log line can be read by a human. See the header.
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
	 * a search timeout per sweep, so they are deliberately absent.) */
	{ 0x78u, 8070u, SELF_SEARCH_TICKS_25_S, "heart rate" },

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
	 * The 0.5 Hz row gets 45 s of search (PROFILE_ENV_SEARCH_TIMEOUT_S),
	 * not the ANT default 25 s, and section 5.1 recommends that figure for
	 * precisely this reason: at one transmission every two seconds a 25 s
	 * window is a thin sample of a slow master. Both 0x19 rows carry it,
	 * because the recommendation is stated for the profile rather than for
	 * one of its rates.
	 *
	 * Liveness is NOT a second problem here even though the two rows differ
	 * eightfold in expiry - see the note above about antr_channel_period_get().
	 */
	{ 0x19u, 8192u,  SELF_SEARCH_TICKS_45_S, "environment (4 Hz)" },
	{ 0x19u, 65535u, SELF_SEARCH_TICKS_45_S, "environment (0.5 Hz)" },
};

#define SELF_PROFILE_N ARRAY_SIZE(self_profiles)

/*
 * Which row each self channel is currently searching. Written only by the
 * polling thread below, which is also the only reader, so it needs no
 * synchronisation - and saying so here is cheaper than someone later adding a
 * mutex to be safe.
 */
static uint8_t self_row[SELF_N];

#else /* !CONFIG_ANT_DONGLE_PROFILES_EXTRA */

/*
 * THE ORIGINAL, VERBATIM. Everything from here to the matching #endif is the
 * pre-packages-A-C file, comments included, and it is copied rather than
 * paraphrased on purpose: the point of this arm is that bridge.conf's image
 * does not move by one byte, and a rewritten comment is fine but a rewritten
 * log string is not (it lands in .rodata and shows up in a section diff).
 *
 * 8070 counts of 32768 Hz is 4.06 Hz - the ANT+ heart rate profile's period,
 * not a choice. A slave told anything else will not find a strap. This is also
 * the number that reaches struct radiant_binding::period and therefore
 * radiant_liveness.c's 3x expiry, which is why it is read back off the channel
 * with antr_channel_period_get() at bind time rather than written down twice.
 */
#define SELF_PERIOD_COUNTS 8070u

/*
 * The device type the wildcard is narrowed to. Fully wildcard (0) would
 * acquire any ANT+ sensor in range, and rx_tap.c has one adapter, so the extra
 * acquisitions would consume channels and bindings to produce nothing. 0x78 is
 * what this bridge can actually decode.
 *
 * "rx_tap.c has one adapter" is true precisely BECAUSE
 * CONFIG_ANT_DONGLE_PROFILES_EXTRA is n here: that same symbol compiles the
 * other three adapters away in rx_tap.c. The two files agree by construction
 * rather than by anybody remembering to keep them in step.
 */
#define SELF_DEVICE_TYPE 0x78u

#endif /* CONFIG_ANT_DONGLE_PROFILES_EXTRA */

/*
 * Channels are claimed from the TOP of the allocation downward, and this is
 * not cosmetic. Every ANT host library allocates from channel 0 upward, so a
 * PC attached to a dongle that is also running self channels collides last
 * this way rather than first. There is no arbitration between the two - the
 * host's MESG_ASSIGN_CHANNEL for a channel this file has open will simply be
 * refused CHANNEL_IN_WRONG_STATE - which is a large part of why
 * CONFIG_ANT_DONGLE_SELF_CHANNELS defaults to 0.
 */
static uint8_t self_channel_index(uint8_t i)
{
	return (uint8_t)(CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED - 1 - i);
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

/* ── The button ────────────────────────────────────────────────────────────── */

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
		"window, so no device will ever bind. Self channels will track "
		"and their traffic will be dropped.");
}
#endif

/* ── The channels ──────────────────────────────────────────────────────────── */

#ifdef CONFIG_ANT_DONGLE_PROFILES_EXTRA

/*
 * Point an ASSIGNED, CLOSED channel at one table row and put it on air.
 *
 * All four configuration calls are legal in exactly this state and in no
 * other: ant_radio.h says antr_channel_id_set(), antr_channel_period_set() and
 * antr_channel_radio_freq_set() answer ANTW_CHANNEL_IN_WRONG_STATE when the
 * channel is "unassigned or open", and the radiant backend implements that as
 * require_assigned_closed() (radiant/src/radiant_channel.c). A channel whose
 * search has just timed out is precisely assigned-and-closed - the backend
 * raises RX_SEARCH_TIMEOUT and then closes itself, in that order - so this
 * needs NO unassign/reassign to retune, and the ANT default of "reconfigure
 * only what changed" is honoured.
 *
 * The search timeout is set per row, not once: it is the only per-profile
 * parameter that is a policy rather than a fact about the master, and the 0.5
 * Hz Environment row genuinely needs a different one.
 */
static int configure_and_open(uint8_t ch, const struct self_profile *p)
{
	antr_err_t rc;

	/* All three ID fields wildcard except the device type. Fully wildcard
	 * (0) is not the more general option it looks like - see the header:
	 * it widens the ID match while leaving the period fixed, so it acquires
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
	 * channel, so it must not cost us the whole rotation. */
	rc = antr_channel_search_timeout_set(ch, p->search_ticks);
	if (rc != 0u) {
		LOG_WRN("channel %u search timeout: %u (leaving the default)",
			ch, rc);
	}

	rc = antr_channel_open(ch);
	if (rc != 0u) {
		LOG_WRN("channel %u open: %u", ch, rc);
		return -EIO;
	}

	/*
	 * NAMED, not just numbered. A console reading "searching for bicycle
	 * power" then "searching for fitness equipment" is the only thing that
	 * separates "no sensors in range" from "searching for the wrong
	 * thing"; the two look identical otherwise, and this file's other
	 * comments are careful about exactly that class of ambiguity.
	 */
	LOG_INF("self channel %u searching for %s (devtype 0x%02x, period %u, "
		"wildcard device number)", ch, p->name, p->device_type,
		p->period_counts);
	return 0;
}

/* Create the channel and point it at a row. Used at boot, and again as the
 * recovery path in rotate_and_reopen() below. */
static int open_one(uint8_t ch, const struct self_profile *p)
{
	antr_err_t rc;

	rc = antr_channel_assign(ch, ANTW_CHANNEL_TYPE_SLAVE, SELF_NETWORK, 0u);
	if (rc != 0u) {
		LOG_ERR("channel %u assign: %u - NOTHING CAN EVER BIND on this "
			"channel", ch, rc);
		return -EIO;
	}

	if (configure_and_open(ch, p) != 0) {
		LOG_ERR("channel %u could not be configured for %s - NOTHING "
			"CAN EVER BIND on this channel", ch, p->name);
		return -EIO;
	}

	return 0;
}

/*
 * A search timed out. Advance this channel one row and reopen, so that N
 * channels sweep the whole table over time instead of one channel retrying one
 * profile forever - which is what the old code did, and which is
 * indistinguishable on the console from a room with no sensors in it.
 *
 * The fallback is the reason for the extra calls. configure_and_open() is
 * correct for the assigned-and-closed state and that is the state a timed-out
 * search leaves behind - but the state was observed a moment ago through
 * antr_channel_status_get() and the backend runs on its own thread, so a
 * refusal here means the channel is not where we thought it was. Rather than
 * open it with a half-applied configuration (a channel searching for the new
 * device type at the old period would find nothing, forever, silently), tear
 * it down and rebuild: close it if it is open at all, unassign - which
 * ant_radio.h requires a closed channel for, hence the order - and reassign
 * from scratch. Both leading calls are allowed to fail; each is a no-op when
 * the channel is already past that state.
 */
static void rotate_and_reopen(uint8_t i)
{
	uint8_t ch = self_channel_index(i);
	const struct self_profile *p;

	self_row[i] = (uint8_t)((self_row[i] + 1u) % SELF_PROFILE_N);
	p = &self_profiles[self_row[i]];

	if (configure_and_open(ch, p) == 0) {
		return;
	}

	LOG_WRN("self channel %u would not retune in place - reassigning", ch);
	(void)antr_channel_close(ch);
	(void)antr_channel_unassign(ch);
	(void)open_one(ch, p);
}

#else /* !CONFIG_ANT_DONGLE_PROFILES_EXTRA */

/* THE ORIGINAL open_one(), VERBATIM - see the header's note on the shape of
 * the guard. One device type, one period, no search-timeout call (so the ANT
 * default of 25 s applies, which is what the pre-A-C image did), and no
 * rotation, so there is no configure_and_open()/rotate_and_reopen() pair
 * either. */
static int open_one(uint8_t ch)
{
	antr_err_t rc;

	rc = antr_channel_assign(ch, ANTW_CHANNEL_TYPE_SLAVE, SELF_NETWORK, 0u);
	if (rc != 0u) {
		LOG_ERR("channel %u assign: %u", ch, rc);
		return -EIO;
	}

	/* All three wildcards except the device type - see SELF_DEVICE_TYPE. */
	rc = antr_channel_id_set(ch, 0u, SELF_DEVICE_TYPE, 0u);
	if (rc != 0u) {
		LOG_ERR("channel %u id: %u", ch, rc);
		return -EIO;
	}

	rc = antr_channel_period_set(ch, SELF_PERIOD_COUNTS);
	if (rc != 0u) {
		LOG_ERR("channel %u period: %u", ch, rc);
		return -EIO;
	}

	rc = antr_channel_radio_freq_set(ch, SELF_RF_FREQ);
	if (rc != 0u) {
		LOG_ERR("channel %u freq: %u", ch, rc);
		return -EIO;
	}

	rc = antr_channel_open(ch);
	if (rc != 0u) {
		LOG_ERR("channel %u open: %u", ch, rc);
		return -EIO;
	}

	LOG_INF("self channel %u searching (devtype 0x%02x, wildcard device "
		"number)", ch, SELF_DEVICE_TYPE);
	return 0;
}

#endif /* CONFIG_ANT_DONGLE_PROFILES_EXTRA */

/*
 * A tracking channel with no binding. Called once per second per channel.
 * Everything that makes this legal happens here: the window test, the identity
 * read, the bind, the period read-back, and the map entry the tap consults.
 *
 * UNCHANGED BY THE SEARCH-PROFILE ROTATION, and both halves of that are
 * load-bearing:
 *
 *   The window test below is the whole of docs/radiant-bridge.md section 10.1
 *   rule 1 in code. Rotation decides what a channel may ACQUIRE; this decides
 *   what may BIND, and the two must not be allowed to grow into each other.
 *
 *   The identity comes from antr_channel_id_get(), never from the table row
 *   the channel was searching on. A wildcard search resolves to a real device
 *   type, and it is the resolved one that reaches radiant_binding_bind() and
 *   therefore rx_tap.c's devtype dispatch - so a binding records what the
 *   sensor IS, not what was being looked for. That was already true and stays
 *   true; with a table it merely stops being trivially true.
 */
static void try_bind(uint8_t ch)
{
	uint16_t devnum = 0u;
	uint8_t  devtype = 0u;
	uint8_t  trans = 0u;
	uint16_t period = 0u;
	uint32_t source = RADIANT_BINDING_NONE;
	int      rc;

	if (antr_channel_id_get(ch, &devnum, &devtype, &trans) != 0u) {
		return;
	}
	if (devnum == 0u) {
		/* Tracking but the wildcard has not resolved yet. */
		return;
	}

	if (!window_open()) {
		/* The state the design asks for, and it is worth one line at
		 * DEBUG rather than silence: a user watching a console wants
		 * to know the dongle heard the strap and declined it. Not INF,
		 * because it repeats once a second for as long as the strap is
		 * on the air. */
		LOG_DBG("channel %u tracking device %u but no pairing window is "
			"open: not binding", ch, devnum);
		return;
	}

	rc = radiant_binding_bind(devnum, devtype, trans, NULL, &source);
	if (rc != 0) {
		LOG_ERR("binding table full: device %u not bound (%d)", devnum,
			rc);
		return;
	}

	/*
	 * The period comes off the channel, not from the table row this channel
	 * happens to be searching on, and the difference matters: with a
	 * rotating search profile there is no single compile-time period any
	 * more, and the row is a statement of what was ASKED FOR rather than of
	 * what the channel settled on. This is the number radiant_liveness.c
	 * multiplies by 3 to decide the sensor has gone away, and reading it
	 * back is what makes that number a fact about the channel rather than
	 * a constant that was true when it was typed. A read that fails leaves
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
}

static void self_thread_fn(void *a, void *b, void *c)
{
	uint8_t i;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	LOG_INF("self channels: %d, opening from channel %u downward", SELF_N,
		self_channel_index(0));

	button_init();

#if defined(CONFIG_ANT_DONGLE_PAIRING_WINDOW_AT_BOOT)
	/*
	 * A BENCH INSTRUMENT, NOT A FEATURE, and the same shape as
	 * ANT_DONGLE_THREAD_COEX_LOAD: off by default, selected by no shipping
	 * image, and it says on the console exactly what it did so a log can
	 * never be mistaken for one from a normal build.
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
	 * opened it. Promiscuous binding - the thing section 10.1 forbids -
	 * would be a window that never closes, and this is not that.
	 */
	LOG_WRN("BENCH BUILD: opening the pairing window at boot without a "
		"button press. No shipping image does this - see "
		"CONFIG_ANT_DONGLE_PAIRING_WINDOW_AT_BOOT.");
	self_channels_open_pairing_window();
#endif

	if (antr_network_address_set(SELF_NETWORK, ant_plus_key) != 0u) {
		LOG_ERR("ANT+ network key rejected - self channels will hear "
			"nothing");
		return;
	}

	/*
	 * Channel i starts on row (i mod N of the table), so a two-channel
	 * build searches TWO profiles at once rather than running two identical
	 * heart-rate searches beside each other. With one channel this is just
	 * row 0 and the rotation does all the work; with as many channels as
	 * rows, every profile is covered continuously and nothing ever rotates.
	 *
	 * With CONFIG_ANT_DONGLE_PROFILES_EXTRA off there is no table and no
	 * row, so this is the original single-argument open_one() loop.
	 */
	for (i = 0u; i < SELF_N; i++) {
#ifdef CONFIG_ANT_DONGLE_PROFILES_EXTRA
		self_row[i] = (uint8_t)(i % SELF_PROFILE_N);
		(void)open_one(self_channel_index(i),
			       &self_profiles[self_row[i]]);
#else
		(void)open_one(self_channel_index(i));
#endif
	}

	for (;;) {
		k_sleep(K_SECONDS(1));

		for (i = 0u; i < SELF_N; i++) {
			uint8_t ch = self_channel_index(i);
			uint8_t status = 0u;

			if (antr_channel_status_get(ch, &status) != 0u) {
				continue;
			}

			switch (status & ANTW_STATUS_CHANNEL_STATE_MASK) {
			case ANTW_STATUS_TRACKING_CHANNEL:
				if (ant_bridge_channel_source(ch) ==
				    RADIANT_BINDING_NONE) {
					try_bind(ch);
				}
				break;
			case ANTW_STATUS_ASSIGNED_CHANNEL:
				/* Search timed out and the channel closed
				 * itself. Re-open rather than leaving a
				 * configured-but-idle channel: a bridge whose
				 * self channels quietly stop searching after
				 * the first timeout looks identical to one
				 * with no sensors in range.
				 *
				 * And reopen on the NEXT profile, not the same
				 * one. Retrying one row forever is the same
				 * indistinguishable-from-empty-room failure one
				 * level up: the channel is visibly busy and
				 * still cannot reach three of the four device
				 * types this bridge decodes. */
				ant_bridge_channel_unbind(ch);
#ifdef CONFIG_ANT_DONGLE_PROFILES_EXTRA
				LOG_INF("self channel %u search timed out on %s "
					"- rotating", ch,
					self_profiles[self_row[i]].name);
				rotate_and_reopen(i);
#else
				/* No table, so "the next profile" is the same
				 * profile: the original plain reopen, verbatim. */
				LOG_INF("self channel %u closed - reopening",
					ch);
				if (antr_channel_open(ch) != 0u) {
					LOG_WRN("self channel %u reopen failed",
						ch);
				}
#endif
				break;
			default:
				break;
			}
		}
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
