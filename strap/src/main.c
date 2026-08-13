/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RadiANT heart-rate strap: a NODE, not a dongle. P8a of the multiprotocol
 * plan - the first image where radiant_core transmits as an ANT+ master
 * with a real profile on top, rather than a dongle bridge.
 *
 * P8b pairs this with a SoftDevice Controller advertising the SIG Heart
 * Rate Service while ANT+ 0x78 keeps broadcasting, testing coexistence on
 * a MASTER channel (a dongle, as a slave, can't stand in for this - a
 * master owns its slot phase). P9 adds the suppression rule on top.
 *
 * No USB/serial bridge: the dongle app is a transparent bridge with no
 * profile logic; this is the opposite shape. The log is the only host
 * interface.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ant_radio.h"
#include "ant_wire.h"
#include "profile_hr.h"
#include "node_ident.h"

#if defined(CONFIG_STRAP_BLE_HRS)
#include "strap_ble.h"
#endif

LOG_MODULE_REGISTER(strap, CONFIG_STRAP_LOG_LEVEL);

/*
 * The public ANT+ network key, used by every ANT+ device on the air - not
 * the stack licence key (an sdk-ant concept radiant_core doesn't have).
 * Confusing the two produces a node that runs and is heard by nothing.
 */
static const uint8_t ant_plus_key[8] = {
	0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45
};

/*
 * ANT+ Heart Rate Monitor, device type 0x78. The period is the profile's, not a
 * choice: 8070 counts of 32768 Hz is 4.06 Hz, and a receiver that has been told
 * 8070 will not find a strap transmitting at anything else.
 */
#define HR_DEVICE_TYPE   0x78u
#define HR_PERIOD_COUNTS 8070u
#define HR_RF_FREQ       57u
#define HR_CHANNEL       0u
#define HR_NETWORK       0u

static struct profile_hr hr;

/*
 * A beat generator rather than a bpm setting: profile_hr's page 0 carries
 * an event count and event time, the fields that survive a lost packet, so
 * only a real generator exercises those - a node that just set a computed
 * bpm would be testing the field that doesn't matter.
 */
#define BEAT_TICK_MS 100u

static uint16_t event_time_1024(void)
{
	/* 1/1024 s, wrapping at 16 bits (ANT+ common time base). ms * 1024 /
	 * 1000 rather than ms<<10/1000: the latter's ~2.4% error would show
	 * up in a receiver's bpm-from-interval calculation. */
	uint64_t ms = (uint64_t)k_uptime_get();

	return (uint16_t)((ms * 1024u) / 1000u);
}

static void beat_work_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(beat_work, beat_work_fn);

/* Defined below with the EVENT_TX handler, because that is where the reasoning
 * about who paces the stream lives; the suppression rule needs it earlier. */
static void load_next_page(void);

#if defined(CONFIG_STRAP_SUPPRESS_ANT_WHEN_CONNECTED)
/*
 * P9 suppression rule lives here, not in strap_ble.c: stopping an ANT+
 * channel is ANT+'s business, so the BLE module only reports the fact and
 * this decides what to do - keeping the dependency one-way. Taken on the
 * beat tick rather than the connect callback, since that runs in
 * controller context - the wrong place to be closing a radio channel.
 *
 * Defaults on because a connected phone already gets HR over BLE, so the
 * ANT+ broadcast is a duplicate - suppressing it means the contention this
 * project exists to arbitrate never arises on a strap. Turn it off to
 * stress the arbiter; that's the config the coexistence gate should run.
 */
static bool ant_open = true;

static void ant_suppression_update(void)
{
	bool want_open = !strap_ble_connected();

	if (want_open == ant_open) {
		return;
	}

	if (want_open) {
		/* Stage a payload before reopening, for the same reason the
		 * first open does: a master transmits the instant the channel
		 * opens. */
		load_next_page();
		if (antr_channel_open(HR_CHANNEL) == ANTW_RESPONSE_NO_ERROR) {
			ant_open = true;
			LOG_INF("ANT+ resumed (BLE disconnected)");
		}
	} else {
		if (antr_channel_close(HR_CHANNEL) == ANTW_RESPONSE_NO_ERROR) {
			ant_open = false;
			LOG_INF("ANT+ suppressed (BLE connected)");
		}
	}
}
#endif /* CONFIG_STRAP_SUPPRESS_ANT_WHEN_CONNECTED */

static void beat_work_fn(struct k_work *w)
{
	static uint32_t elapsed_ms;
	static uint32_t next_beat_ms;

	uint32_t bpm = CONFIG_STRAP_HR_BPM;
	uint32_t interval_ms = (bpm != 0u) ? (60000u / bpm) : 1000u;

	ARG_UNUSED(w);

	elapsed_ms += BEAT_TICK_MS;
	if (elapsed_ms >= next_beat_ms) {
		/*
		 * Advance the schedule; don't re-anchor it to the tick.
		 * `next_beat_ms = elapsed_ms + interval_ms` looks equivalent
		 * but isn't: elapsed_ms is quantised to BEAT_TICK_MS, so
		 * re-anchoring every beat compounds rounding instead of
		 * cancelling it. Measured effect at 72 bpm (833 ms interval):
		 * a 60 s BLE connection saw 67 HR notifications, not 72 -
		 * exactly the event-count/interval disagreement the profile
		 * tests exist to catch. Accumulating the exact interval keeps
		 * the average right; individual beats still land on a tick.
		 */
		next_beat_ms += interval_ms;
		if (next_beat_ms <= elapsed_ms) {
			/* Recovery, not steady state: a bpm change or missed
			 * ticks can leave the schedule far behind; re-anchor
			 * here, once, rather than burst-fire catch-up beats. */
			next_beat_ms = elapsed_ms + interval_ms;
		}
		profile_hr_beat(&hr, event_time_1024());
		profile_hr_set_computed(&hr, (uint8_t)bpm);
#if defined(CONFIG_STRAP_BLE_HRS)
		/* One heart, two radios: the SIG service is notified from the
		 * same beat that advances the ANT+ accumulators, so the two
		 * sides can't disagree about the rate. */
		strap_ble_notify_hr((uint8_t)bpm);
#endif
	}
	profile_hr_set_operating_time(&hr, elapsed_ms / 1000u);

#if defined(CONFIG_STRAP_SUPPRESS_ANT_WHEN_CONNECTED)
	ant_suppression_update();
#endif

	k_work_schedule(&beat_work, K_MSEC(BEAT_TICK_MS));
}

/*
 * Load the next page, paced by EVENT_TX rather than a wall clock: the stack
 * raises this exactly when a payload just went on air, which is when the
 * next one should be staged. A timer-driven load would drift against the
 * channel period and, under an arbiter, stage for slots never granted.
 */
static void load_next_page(void)
{
	uint8_t body[8];
	enum profile_slot_kind kind;

	kind = profile_hr_next(&hr, (uint64_t)k_uptime_get() * 1000u, body);
	if (kind == PROFILE_SLOT_IDLE) {
		/* A sparse node's silent slot. The radio stays off and the
		 * channel period keeps running; nothing to stage. */
		return;
	}

	(void)antr_broadcast_message_tx(HR_CHANNEL, sizeof(body), body);
}

/*
 * The backend's only way up. The dongle implements this by forwarding to USB;
 * a node implements it by being the application the messages were for.
 */
void antr_on_message(const struct antr_msg *msg)
{
	if (msg == NULL || msg->data == NULL) {
		return;
	}

	if (msg->id == ANTW_MESG_RESPONSE_EVENT_ID &&
	    msg->len >= ANTW_MESG_RESPONSE_EVENT_SIZE) {
		/* data[0] channel, data[1] the message id being responded to
		 * (0 for an event), data[2] the code. */
		if (msg->data[0] == HR_CHANNEL && msg->data[2] == ANTW_EVENT_TX) {
			load_next_page();
		}
	}
}

static int ant_start(uint16_t devnum)
{
	antr_err_t rc;

	rc = antr_network_address_set(HR_NETWORK, ant_plus_key);
	if (rc != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("network key: %d", (int)rc);
		return -EIO;
	}

	rc = antr_channel_assign(HR_CHANNEL, ANTW_CHANNEL_TYPE_MASTER,
				 HR_NETWORK, 0u);
	if (rc != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("assign: %d", (int)rc);
		return -EIO;
	}

	/* Transmission type 5, matching sim/ and tools/ant_sim.py - part of
	 * the channel id a receiver matches on. */
	rc = antr_channel_id_set(HR_CHANNEL, devnum, HR_DEVICE_TYPE, 5u);
	if (rc != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("channel id: %d", (int)rc);
		return -EIO;
	}

	rc = antr_channel_period_set(HR_CHANNEL, HR_PERIOD_COUNTS);
	if (rc != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("period: %d", (int)rc);
		return -EIO;
	}

	rc = antr_channel_radio_freq_set(HR_CHANNEL, HR_RF_FREQ);
	if (rc != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("rf freq: %d", (int)rc);
		return -EIO;
	}

	/*
	 * Stage the first page before opening, not after: a master transmits
	 * the instant the channel opens, so the first slot is gone before any
	 * EVENT_TX could stage it - otherwise the first frame goes out empty.
	 */
	load_next_page();

	rc = antr_channel_open(HR_CHANNEL);
	if (rc != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("open: %d", (int)rc);
		return -EIO;
	}

	return 0;
}

int main(void)
{
	struct profile_hr_cfg cfg;
	uint16_t devnum = CONFIG_STRAP_DEVICE_NUMBER;
	int err;

	LOG_INF("RadiANT strap: ANT+ 0x78 heart rate");

	if (antr_init() != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("antr_init failed");
		return 0;
	}

	/*
	 * Device number comes from the identity record when provisioned (see
	 * docs/decisions/0009-hostless-node-identity.md); falls back to
	 * Kconfig so a freshly-flashed DK works on a bench without a
	 * provisioning step.
	 */
	if (node_ident_boot() == 0 && node_ident_is_provisioned()) {
		uint16_t d;

		if (node_ident_devnum(&d) == 0 && d != 0u) {
			devnum = d;
			LOG_INF("device number %u from the identity record", d);
		}
	} else {
		LOG_INF("device number %u from Kconfig (node not provisioned)",
			devnum);
	}

	/*
	 * cfg.id is the common-page identity (0x50/0x51), not the ANT channel
	 * id set in ant_start() - conflating them produces a strap that's
	 * found and then misidentifies itself.
	 */
	memset(&cfg, 0, sizeof(cfg));
	cfg.id.hw_revision = 1u;
	cfg.id.manufacturer_id = CONFIG_STRAP_MANUFACTURER_ID;
	cfg.id.model_number = 1u;
	cfg.id.sw_revision_main = 1u;
	cfg.id.sw_revision_supplemental = PROFILE_COMMON_INVALID_U8;
	cfg.id.serial_number = devnum;
	cfg.manufacturer_id_8 = CONFIG_STRAP_MANUFACTURER_ID;
	cfg.serial_upper16 = devnum;
	cfg.model_number_8 = 1u;
	cfg.hw_version = 1u;
	cfg.sw_version = 1u;
	cfg.common_82_every = 0u; /* no battery page rotation on a DK */

	err = profile_hr_init(&hr, &cfg);
	if (err != 0) {
		LOG_ERR("profile_hr_init: %d", err);
		return 0;
	}

	err = ant_start(devnum);
	if (err != 0) {
		return 0;
	}

	k_work_schedule(&beat_work, K_MSEC(BEAT_TICK_MS));
	LOG_INF("transmitting: device #%u, type 0x%02x, %u bpm",
		devnum, HR_DEVICE_TYPE, (unsigned)CONFIG_STRAP_HR_BPM);

#if defined(CONFIG_STRAP_BLE_HRS)
	/* P8b, started last: bringing the controller up after the ANT+
	 * channel means a failure to advertise can't be mistaken for a
	 * failure to transmit. */
	(void)strap_ble_start();
#endif

	return 0;
}

