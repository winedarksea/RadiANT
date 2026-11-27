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
 * anything about how beats reach radiant_profile_hr; the context rules are stated
 * there and they are not obvious.
 *
 * See docs/hrm-reference-design.md for the porting guide and the production
 * checklist.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <radiant/radiant_ant.h>
#include <radiant/radiant_wire.h>
#include <radiant/radiant_profile_hr.h>
#include <radiant/radiant_node_ident.h>
#include <radiant/radiant_product.h>
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

static struct radiant_profile_hr hr;

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
 * Off by default (Kconfig): the strap keeps ANT+ on air while a phone is
 * connected, so a watch and a phone are served at once. Turned on
 * (ble_suppress.conf), the ANT+ broadcast stops because a connected phone
 * already gets HR over BLE, which saves air but drops any ANT+ listener.
 */
static bool ant_open = true;

static void ant_suppression_update(void)
{
	bool want_open = !hrm_ble_connected();

	if (want_open == ant_open) {
		return;
	}

	if (want_open) {
		/* No pre-open stage: radiant_ant_broadcast_message_tx() refuses a
		 * closed channel (NOT_OPENED, by design) and the refused call
		 * still consumed a page. open() posts the first slot from
		 * whatever the buffer holds - the last page, after a resume -
		 * and EVENT_TX after that slot starts the real stream. */
		if (radiant_ant_channel_open(HR_CHANNEL) == RADIANT_WIRE_RESPONSE_NO_ERROR) {
			ant_open = true;
			LOG_INF("ANT+ resumed (BLE disconnected)");
		}
	} else {
		if (radiant_ant_channel_close(HR_CHANNEL) == RADIANT_WIRE_RESPONSE_NO_ERROR) {
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
 * radiant_profile_hr has no lock and is already reached from the radiant callback path
 * (radiant_ant_on_message -> load_next_page -> radiant_profile_hr_next). Doing the profile
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
		 * accounted for. radiant_profile_hr_beat() increments the event count
		 * and stores the event time, so replaying a coalesced pair
		 * advances the count correctly and leaves the time at the
		 * genuine latest instant - it does not invent an interval that
		 * never happened. The count is the field that survives a lost
		 * packet, so that is the right way round.
		 */
		radiant_profile_hr_beat(&hr, event_time);
	}

	radiant_profile_hr_set_computed(&hr, bpm);

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
	ARG_UNUSED(w);

	/* Uptime, not a count of wake-ups: a sum of NODE_TICK_MS drifted slow
	 * by every late wake-up and wrapped a uint32 of ms at 49.7 days. */
	radiant_profile_hr_set_operating_time(&hr, (uint32_t)(k_uptime_get() / 1000));

	/* Contact detection, AGC, battery - the slow, level-valued things a
	 * source may want a thread context for. Never beats; see the seam. */
	hrm_sensor_poll();

#if defined(CONFIG_HRM_SUPPRESS_ANT_WHEN_CONNECTED)
	ant_suppression_update();
#endif

#if defined(CONFIG_HRM_BLE_HRS)
	/* The advertising watchdog (radiant_ble_adv.h): cheap unless a probe
	 * is due, and the backstop for advertising ending unseen. */
	hrm_ble_tick();
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
	enum radiant_profile_slot_kind kind;

	/* This runs on the (preemptible) event thread; the source publishes
	 * from the cooperative system workqueue, which can preempt it mid-
	 * encode and hand this page half old and half new fields. Lock for
	 * the encode only - not the send, which takes a mutex. */
	k_sched_lock();
	kind = radiant_profile_hr_next(&hr, (uint64_t)k_uptime_get() * 1000u, body);
	k_sched_unlock();
	if (kind == RADIANT_PROFILE_SLOT_IDLE) {
		/* A sparse node's silent slot. The radio stays off and the
		 * channel period keeps running; nothing to stage. */
		return;
	}

	(void)radiant_ant_broadcast_message_tx(HR_CHANNEL, sizeof(body), body);
}

/*
 * The backend's only way up. The dongle implements this by forwarding to USB;
 * a node implements it by being the application the messages were for.
 */
void radiant_ant_on_message(const struct radiant_ant_msg *msg)
{
	if (msg == NULL || msg->data == NULL) {
		return;
	}

	if (msg->id == RADIANT_WIRE_MESG_RESPONSE_EVENT_ID &&
	    msg->len >= RADIANT_WIRE_MESG_RESPONSE_EVENT_SIZE) {
		/* data[0] channel, data[1] the message id being responded to
		 * (0 for an event), data[2] the code. */
		if (msg->data[0] == HR_CHANNEL && msg->data[2] == RADIANT_WIRE_EVENT_TX) {
			load_next_page();
		}
	}
}

static int ant_start(uint16_t devnum)
{
	radiant_ant_err_t rc;

	rc = radiant_ant_network_address_set(HR_NETWORK, ant_plus_key);
	if (rc != RADIANT_WIRE_RESPONSE_NO_ERROR) {
		LOG_ERR("network key: %d", (int)rc);
		return -EIO;
	}

	rc = radiant_ant_channel_assign(HR_CHANNEL, RADIANT_WIRE_CHANNEL_TYPE_MASTER,
				 HR_NETWORK, 0u);
	if (rc != RADIANT_WIRE_RESPONSE_NO_ERROR) {
		LOG_ERR("assign: %d", (int)rc);
		return -EIO;
	}

	/* Transmission type 5, matching apps/sim and tools/ant_sim.py - part of
	 * the channel id a receiver matches on. */
	rc = radiant_ant_channel_id_set(HR_CHANNEL, devnum, HR_DEVICE_TYPE, 5u);
	if (rc != RADIANT_WIRE_RESPONSE_NO_ERROR) {
		LOG_ERR("channel id: %d", (int)rc);
		return -EIO;
	}

	rc = radiant_ant_channel_period_set(HR_CHANNEL, HR_PERIOD_COUNTS);
	if (rc != RADIANT_WIRE_RESPONSE_NO_ERROR) {
		LOG_ERR("period: %d", (int)rc);
		return -EIO;
	}

	rc = radiant_ant_channel_radio_freq_set(HR_CHANNEL, HR_RF_FREQ);
	if (rc != RADIANT_WIRE_RESPONSE_NO_ERROR) {
		LOG_ERR("rf freq: %d", (int)rc);
		return -EIO;
	}

	/*
	 * No pre-open stage. It was here to stop the first frame going out
	 * empty, and it never could: radiant_ant_broadcast_message_tx() refuses a
	 * channel that is not open yet (NOT_OPENED, by design), so the call
	 * only consumed a page for nothing. open() posts the first slot from
	 * whatever the buffer holds (zeros at boot - HR's computed 0 reads as
	 * "invalid" to a receiver), and EVENT_TX after that slot starts the
	 * real stream.
	 */
	rc = radiant_ant_channel_open(HR_CHANNEL);
	if (rc != RADIANT_WIRE_RESPONSE_NO_ERROR) {
		LOG_ERR("open: %d", (int)rc);
		return -EIO;
	}

	return 0;
}

int main(void)
{
	struct radiant_profile_hr_cfg cfg;
	const struct radiant_product *product = radiant_product_get();
	struct radiant_product_unit unit;
	uint16_t devnum;
	int err;

	LOG_INF("RadiANT heart-rate node: ANT+ 0x78");

	if (radiant_ant_init() != RADIANT_WIRE_RESPONSE_NO_ERROR) {
		LOG_ERR("radiant_ant_init failed");
		return 0;
	}

	/*
	 * Device number and serial from the identity record (ADR 0009). On the
	 * first boot of a part with no record, radiant_node_ident_boot()
	 * generates K_dev from the on-chip TRNG and stores it first (ADR 0020,
	 * CONFIG_RADIANT_NODE_IDENT_SELF_PROVISION), so every unit flashed from
	 * one build gets its own number with no factory step.
	 *
	 * radiant_product_unit_boot() owns the failure branch. A development
	 * build falls back to CONFIG_HRM_DEVICE_NUMBER with no transforms,
	 * logged at error level on every boot because a strap on that branch
	 * shares its identity with every other unit built from the same image.
	 * A strict build (CONFIG_RADIANT_PRODUCT_STRICT) does not return until
	 * the identity boots - no channel is open yet, so that is staying off
	 * the air rather than transmitting on the shared number.
	 */
	(void)radiant_product_unit_boot(CONFIG_HRM_DEVICE_NUMBER, &unit);
	devnum = unit.devnum;

	/*
	 * cfg.id is the common-page identity (0x50/0x51), not the ANT channel
	 * id set in ant_start() - conflating them produces a strap that's
	 * found and then misidentifies itself. It comes from radiant_product
	 * (CONFIG_RADIANT_PRODUCT_*), the same record the BLE DIS strings come
	 * from, and the unit's 32-bit serial.
	 *
	 * Page 0x02 is the legacy manufacturer page with narrower fields: the
	 * low bytes of the manufacturer id and model number, and the UPPER 16
	 * bits of the serial - whose lower 16 are the device number, which
	 * radiant_node_ident_serial32() guarantees, so pages 0x02 and 0x51
	 * name the same serial.
	 */
	memset(&cfg, 0, sizeof(cfg));
	(void)radiant_product_fill_common_id(product, unit.serial, &cfg.id);
	cfg.manufacturer_id_8 = (uint8_t)(product->manufacturer_id & 0xFFu);
	cfg.serial_upper16 = (uint16_t)(unit.serial >> 16);
	cfg.model_number_8 = (uint8_t)(product->model_number & 0xFFu);
	cfg.hw_version = product->hw_rev;
	cfg.sw_version = product->sw_rev_main;
	cfg.common_82_every = 0u; /* no battery page rotation on a DK */

	err = radiant_profile_hr_init(&hr, &cfg);
	if (err != 0) {
		LOG_ERR("radiant_profile_hr_init: %d", err);
		return 0;
	}

	/*
	 * Between radiant_profile_hr_init() and ant_start(), which is the only correct
	 * window: the profile must exist before a beat can be delivered into
	 * it, and the source must be running before the channel opens so the
	 * first staged page can already carry a rate.
	 *
	 * The name is logged rather than the option, so that a manufacturer
	 * reads THEIR driver's name at boot - or the reference simulator's, and
	 * there is no third answer. A -ENODEV here is the deliberate
	 * CONFIG_HRM_SENSOR_CUSTOM failure; the node keeps running and
	 * transmits RADIANT_PROFILE_HR_INVALID_BPM, which is visible from the receiving
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
	(void)hrm_ble_start(devnum, unit.serial);
#endif

	return 0;
}
