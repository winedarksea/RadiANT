/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RadiANT heart-rate strap: a NODE, not a dongle.
 *
 * P8a of the multiprotocol plan, and the plan is blunt about why it exists:
 * "there is no RadiANT node application, and this is new scope". Everything
 * needed for one was already written and none of it was ever linked into an
 * image. `src/profiles/*.c` is compiled only by radiant_core/tests/CMakeLists.txt
 * - the root application never lists it - so every profile in this project has,
 * until now, only ever run inside a unit test. `sim/` is not the missing piece
 * either: it is an *sdk-ant* application (CONFIG_ANT=y, evaluation licence key)
 * and shares nothing with radiant_core but the antenna.
 *
 * So this is the first image in the tree where radiant_core transmits as a
 * master with a real ANT+ profile on top of it.
 *
 * WHAT IT IS FOR, and it is not a demo. P8b puts a SoftDevice Controller beside
 * this advertising the SIG Heart Rate Service while this keeps broadcasting
 * ANT+ 0x78, which is the BLE half of the coexistence work. That test needs a
 * node whose ANT+ side is a MASTER - a dongle cannot stand in for it, because a
 * master owns its slot phase and a slave does not, and the whole question is
 * what happens to a transmit schedule when another stack wants the radio. P9
 * then adds the suppression rule on top.
 *
 * NO USB AND NO SERIAL BRIDGE, deliberately. The dongle application is a
 * transparent bridge with no profile logic of its own; this is the opposite
 * shape, and mixing them would produce a third thing that is neither. The only
 * host interface is the log.
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
 * THE ANT+ NETWORK KEY IS NOT THE STACK LICENCE KEY, and confusing the two
 * produces a node that runs perfectly and is heard by nothing. This is the
 * public ANT+ network key, which every ANT+ device on the air uses; the stack
 * licence is an sdk-ant concept that radiant_core does not have.
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
 * The beat generator. A real strap gets these from an electrode; this one
 * produces a plausible resting rhythm so that the accumulators advance and a
 * receiver's "sensor liveness" and "accumulator continuity" checks have
 * something true to check. It is deliberately a beat GENERATOR rather than a
 * bpm setting: profile_hr's page 0 carries an event count and an event time,
 * and those are what survive a lost packet, so a node that only ever set a
 * computed bpm would be exercising the field that does not matter.
 */
#define BEAT_TICK_MS 100u

static uint16_t event_time_1024(void)
{
	/* 1/1024 s, wrapping at 16 bits, which is the ANT+ common time base.
	 * k_uptime_get() is milliseconds, so this is ms * 1024 / 1000 reduced
	 * to avoid the divide: the profile only needs a consistent clock, and
	 * the ~2.4 % error a naive ms<<10/1000 would introduce is not
	 * acceptable to a receiver computing bpm from the interval. */
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
 * P9 â€” the suppression rule, and it lives HERE rather than in strap_ble.c on
 * purpose.
 *
 * Stopping an ANT+ channel is the ANT+ side's business. The BLE module reports
 * a fact and this decides what to do about it, so the dependency runs one way
 * and a controller callback never reaches into radiant_core. That also means
 * the transition is taken on the beat tick - a thread, at a predictable
 * instant - rather than from inside the connect callback, which is the
 * controller's own context and the last place to be closing a radio channel.
 *
 * WHAT IT BUYS is not airtime, it is the removal of the hard case. A connected
 * phone is already receiving this heart rate over the SIG service, so the ANT+
 * broadcast is a duplicate - and with it stopped, the contention the whole
 * multiprotocol design exists to arbitrate never arises on a strap at all. That
 * is why the Kconfig defaults on. Turning it OFF is what stresses the arbiter,
 * and it is the configuration the coexistence gate should be run in.
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
		 * ADVANCE THE SCHEDULE, DO NOT RE-ANCHOR IT TO THE TICK.
		 *
		 * `next_beat_ms = elapsed_ms + interval_ms` looks equivalent and
		 * is not: elapsed_ms is quantised to BEAT_TICK_MS, so each beat
		 * re-anchors to the tick that noticed it and the rounding error
		 * compounds instead of cancelling. At 72 bpm the interval is
		 * 833 ms, every beat lands on the next 100 ms boundary, and the
		 * strap beats every 900 ms - 66.7 bpm - while page 0's computed
		 * field goes on claiming 72.
		 *
		 * That was MEASURED, not reasoned about: a host BLE central held
		 * a connection for 60 s (scripts\ble_central.ps1) and counted 67
		 * Heart Rate Measurement notifications, one per beat.
		 *
		 * It matters more than a cosmetic 7 %: this file's own comment
		 * above says the event count and event time are the fields that
		 * survive a lost packet, so a receiver computes bpm from the
		 * interval and reads 67 while the computed field says 72. The
		 * two disagreeing is exactly the sensor bug the profile tests
		 * exist to catch. Accumulating the exact interval keeps the
		 * average right - individual beats still land on a tick, but the
		 * error cancels rather than accrues.
		 */
		next_beat_ms += interval_ms;
		if (next_beat_ms <= elapsed_ms) {
			/* Recovery, not steady state: a bpm change (or a missed
			 * run of ticks) can leave the schedule far behind, and
			 * catching up one interval per tick would fire a burst
			 * of false beats. Re-anchoring is right HERE and wrong
			 * above. */
			next_beat_ms = elapsed_ms + interval_ms;
		}
		profile_hr_beat(&hr, event_time_1024());
		profile_hr_set_computed(&hr, (uint8_t)bpm);
#if defined(CONFIG_STRAP_BLE_HRS)
		/* ONE HEART, TWO RADIOS. The SIG service is notified from the
		 * same beat that advances the ANT+ accumulators, so the two
		 * sides cannot disagree about the rate - which is the first
		 * thing anybody checks when a strap serves a phone and a watch
		 * at once, and the first thing that goes wrong if each side
		 * keeps its own timebase. */
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
 * Load the next page.
 *
 * PACED BY EVENT_TX, NEVER BY A WALL CLOCK, which is the same discipline
 * tools/ant_sim.py describes for driving a dongle from a host: the stack raises
 * this event at the moment it has put a payload on the air, which is exactly
 * when the next one should be staged. A node that instead loaded on a timer
 * would drift against its own channel period and, under an arbiter, would stage
 * payloads for slots that were never granted.
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

	/*
	 * Transmission type 5, matching sim/ and tools/ant_sim.py. It is part
	 * of the channel id a receiver matches on, so a strap that picked its
	 * own number would be invisible to every tool in this repository.
	 */
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
	 * STAGE THE FIRST PAGE BEFORE OPENING, not after. A master starts
	 * transmitting the instant the channel opens, and the first slot is
	 * gone before any EVENT_TX can be delivered for it - so a node that
	 * waited for the event would put one empty message on the air, which a
	 * receiver reads as a page 0 with a zero event count and an
	 * accumulator that then jumps.
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
	 * THE DEVICE NUMBER COMES FROM THE IDENTITY RECORD WHEN THERE IS ONE.
	 * docs/decisions/0009-hostless-node-identity.md makes the device number
	 * a property of the node's provisioned identity rather than of its
	 * firmware image, and src/node/ already implements the tiers and the
	 * power-up rule. Falling back to the Kconfig when the node has never
	 * been provisioned keeps a freshly-flashed DK usable on a bench without
	 * a provisioning step, which is what this application is mostly for.
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
	 * cfg.id IS THE COMMON-PAGE IDENTITY (pages 0x50/0x51), NOT the ANT
	 * channel id. The channel id - device number, device type, transmission
	 * type - is what a receiver matches on and was set in ant_start(); this
	 * is the manufacturer and serial a receiver reads out of the common
	 * pages once it is already listening. Two different identities, and
	 * conflating them produces a strap that is found and then describes
	 * itself as something else.
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
	/*
	 * P8b. Started last and deliberately: the ANT+ channel is the thing
	 * under test, and bringing the controller up after it means a failure
	 * to advertise cannot be mistaken for a failure to transmit.
	 */
	(void)strap_ble_start();
#endif

	return 0;
}

