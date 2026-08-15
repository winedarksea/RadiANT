/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RadiANT heart-rate node: a NODE, not a dongle, and the manufacturer
 * reference design. P8a of the multiprotocol plan - the first image where
 * radiant transmits as an ANT+ master with a real profile on top, rather than
 * a dongle bridge.
 *
 * P8b pairs this with a SoftDevice Controller advertising the SIG Heart
 * Rate Service while ANT+ 0x78 keeps broadcasting, testing coexistence on
 * a MASTER channel (a dongle, as a slave, can't stand in for this - a
 * master owns its slot phase). P9 adds the suppression rule on top.
 *
 * No USB/serial bridge: the dongle app is a transparent bridge with no
 * profile logic; this is the opposite shape. The log is the only host
 * interface.
 *
 * WHAT IS NOT IN THIS FILE ANY MORE. The heart rate used to be
 * CONFIG_STRAP_HR_BPM, read straight out of the beat work handler here. It is
 * now behind src/hrm_sensor.h's seam, with the simulator as one implementation
 * of it, because that constant was the single thing standing between this
 * application and being a reference design. Read hrm_sensor.h before changing
 * anything about how beats reach profile_hr; the context rules are stated
 * there and they are not obvious.
 *
 * See docs/hrm-reference-design.md for the porting guide and the production
 * checklist.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ant_radio.h"
#include "ant_wire.h"
#include "profile_hr.h"
#include "node_ident.h"
#include "hrm_sensor.h"

#if defined(CONFIG_HRM_BLE_HRS)
#include "hrm_ble.h"
#endif

LOG_MODULE_REGISTER(hrm, CONFIG_HRM_LOG_LEVEL);

/*
 * The public ANT+ network key, used by every ANT+ device on the air - not
 * the stack licence key (an sdk-ant concept radiant doesn't have).
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
 * The node's housekeeping cadence. It used to be the beat tick as well; now it
 * is only housekeeping - operating time, the source's optional poll, and the
 * suppression rule - because beats arrive when the source says they do. The
 * rename is the point: nothing on this tick decides when a heartbeat happened.
 */
#define NODE_TICK_MS 100u

static void node_tick_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(node_tick, node_tick_fn);

/* Defined below with the EVENT_TX handler, because that is where the reasoning
 * about who paces the stream lives; the suppression rule needs it earlier. */
static void load_next_page(void);

#if defined(CONFIG_HRM_SUPPRESS_ANT_WHEN_CONNECTED)
/*
 * The suppression rule lives here, not in hrm_ble.c: stopping an ANT+
 * channel is ANT+'s business, so the BLE module only reports the fact and
 * this decides what to do - keeping the dependency one-way. Taken on the
 * node tick rather than the connect callback, since that runs in
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
	bool want_open = !hrm_ble_connected();

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
#endif /* CONFIG_HRM_SUPPRESS_ANT_WHEN_CONNECTED */

/*
 * The seam's other half, declared in hrm_sensor.h and implemented here.
 *
 * RUNS ON THE SYSTEM WORKQUEUE, and that is the whole reason it exists as a
 * separate function rather than being done inside hrm_sensor_report_beat().
 * profile_hr has no lock and is already reached from the radiant callback path
 * (antr_on_message -> load_next_page -> profile_hr_next). Doing the profile
 * work here keeps that at two contexts however the source is driven, including
 * from an ISR. hrm_sensor.h's header states the rule in full.
 *
 * The three calls are together on purpose: the ANT+ accumulator pair, the ANT+
 * computed byte and the SIG notification all come from one beat, so a receiver
 * on either radio cannot see a rate the other does not.
 */
void hrm_node_deliver_beats(uint16_t beats, uint16_t event_time, uint8_t bpm)
{
	for (uint16_t i = 0; i < beats; i++) {
		/*
		 * Every beat, with the same instant when more than one is being
		 * accounted for. profile_hr_beat() increments the event count
		 * and stores the event time, so replaying a coalesced pair
		 * advances the count correctly and leaves the time at the
		 * genuine latest instant - it does not invent an interval that
		 * never happened. The count is the field that survives a lost
		 * packet, so that is the right way round.
		 */
		profile_hr_beat(&hr, event_time);
	}

	profile_hr_set_computed(&hr, bpm);

#if defined(CONFIG_HRM_BLE_HRS)
	/* One heart, two radios: the SIG service is notified from the
	 * same beat that advances the ANT+ accumulators, so the two
	 * sides can't disagree about the rate. */
	if (beats != 0u) {
		hrm_ble_notify_hr(bpm);
	}
#endif
}

static void node_tick_fn(struct k_work *w)
{
	static uint32_t elapsed_ms;

	ARG_UNUSED(w);

	elapsed_ms += NODE_TICK_MS;
	profile_hr_set_operating_time(&hr, elapsed_ms / 1000u);

	/* Contact detection, AGC, battery - the slow, level-valued things a
	 * source may want a thread context for. Never beats; see the seam. */
	hrm_sensor_poll();

#if defined(CONFIG_HRM_SUPPRESS_ANT_WHEN_CONNECTED)
	ant_suppression_update();
#endif

	k_work_schedule(&node_tick, K_MSEC(NODE_TICK_MS));
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

	/* Transmission type 5, matching apps/sim and tools/ant_sim.py - part of
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
	uint16_t devnum = CONFIG_HRM_DEVICE_NUMBER;
	int err;

	LOG_INF("RadiANT heart-rate node: ANT+ 0x78");

	if (antr_init() != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("antr_init failed");
		return 0;
	}

	/*
	 * Device number comes from the identity record when provisioned (see
	 * docs/decisions/0009-hostless-node-identity.md); falls back to
	 * Kconfig so a freshly-flashed DK works on a bench without a
	 * provisioning step.
	 *
	 * NOTHING IN apps/ CALLS node_ident_provision() TODAY, so on every
	 * board in this tree the fallback is the only branch that runs. That is
	 * a real gap, not a nuance: production checklist items 3 and 4 in
	 * docs/hrm-reference-design.md are unachievable until it is closed, and
	 * closing it is a manufacturing-process decision with its own ADR
	 * rather than something to bolt on here.
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
	 *
	 * THE FOUR `1`s BELOW ARE PLACEHOLDERS AND A HEAD UNIT DISPLAYS THEM.
	 * model_number, hw_revision and the two sw_revision fields are what
	 * pages 0x50/0x51 carry to a watch's device-information screen, so a
	 * shipped product must set them from something real. Production
	 * checklist item 2.
	 */
	memset(&cfg, 0, sizeof(cfg));
	cfg.id.hw_revision = 1u;
	cfg.id.manufacturer_id = CONFIG_HRM_MANUFACTURER_ID;
	cfg.id.model_number = 1u;
	cfg.id.sw_revision_main = 1u;
	cfg.id.sw_revision_supplemental = PROFILE_COMMON_INVALID_U8;
	cfg.id.serial_number = devnum;
	cfg.manufacturer_id_8 = CONFIG_HRM_MANUFACTURER_ID;
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

	/*
	 * Between profile_hr_init() and ant_start(), which is the only correct
	 * window: the profile must exist before a beat can be delivered into
	 * it, and the source must be running before the channel opens so the
	 * first staged page can already carry a rate.
	 *
	 * The name is logged rather than the option, so that a manufacturer
	 * reads THEIR driver's name at boot - or the reference simulator's, and
	 * there is no third answer. A -ENODEV here is the deliberate
	 * CONFIG_HRM_SENSOR_CUSTOM failure; the node keeps running and
	 * transmits PROFILE_HR_INVALID_BPM, which is visible from the receiving
	 * end in a way that a silent simulator fallback would not be.
	 */
	err = hrm_sensor_start();
	if (err != 0) {
		LOG_ERR("heart-rate source did not start: %d", err);
	} else {
		LOG_INF("heart-rate source: %s", hrm_sensor_name());
	}

	err = ant_start(devnum);
	if (err != 0) {
		return 0;
	}

	k_work_schedule(&node_tick, K_MSEC(NODE_TICK_MS));
	LOG_INF("transmitting: device #%u, type 0x%02x", devnum, HR_DEVICE_TYPE);

#if defined(CONFIG_HRM_BLE_HRS)
	/* P8b, started last: bringing the controller up after the ANT+
	 * channel means a failure to advertise can't be mistaken for a
	 * failure to transmit. */
	(void)hrm_ble_start();
#endif

	return 0;
}
