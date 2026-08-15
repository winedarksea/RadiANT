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

/*
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
 */
#define SELF_DEVICE_TYPE 0x78u

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

/*
 * A tracking channel with no binding. Called once per second per channel.
 * Everything that makes this legal happens here: the window test, the identity
 * read, the bind, the period read-back, and the map entry the tap consults.
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
	 * The period comes off the channel, not from SELF_PERIOD_COUNTS above,
	 * and the difference matters: this is the number radiant_liveness.c
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

	for (i = 0u; i < SELF_N; i++) {
		(void)open_one(self_channel_index(i));
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
				 * with no sensors in range. */
				ant_bridge_channel_unbind(ch);
				LOG_INF("self channel %u closed - reopening",
					ch);
				if (antr_channel_open(ch) != 0u) {
					LOG_WRN("self channel %u reopen failed",
						ch);
				}
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
