/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Phase 5: the ANT+ heart-rate slave. See treadmill_hr_rx.h for the
 * acquisition rule and for what "strongest wins" honestly means here.
 *
 * The decode is deliberately NOT radiant/src/bridge/radiant_hr_adapter.c.
 * That adapter's job is to turn a strap's accumulators into bridge samples for
 * a dongle - it posts to the sample bus, keys on a binding index and carries
 * the whole liveness contract. This node wants ONE BYTE, the sensor's own
 * computed bpm, and pulling the bridge in to get it would put the sample bus,
 * the binding table and the sink registry on a treadmill that has no use for
 * any of them. The byte's position is the same fact in both files and it is
 * profile_hr.h's, cited here rather than re-derived.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ant_radio.h"
#include "ant_wire.h"
#include "profile_hr.h"
#include "treadmill_hr_rx.h"
#include "treadmill_state.h"

LOG_MODULE_DECLARE(treadmill, CONFIG_TREADMILL_LOG_LEVEL);

#define HR_PERIOD    PROFILE_HR_PERIOD  /* 8070, the profile's standard rate */
#define HR_RF_FREQ   57u
#define HR_NETWORK   0u

/*
 * Search timeout in 2.5 s units. 12 is 30 s: long enough that a strap being
 * put on is found, short enough that the proximity floor relaxes within a
 * couple of minutes in a quiet room.
 *
 * NOT ZERO AND NOT 0xFF. Zero disables the search outright and 0xFF is an
 * infinite search - which on this node would be a sweep that never stops,
 * i.e. exactly the thing the header says starves the arbiter.
 */
#define HR_SEARCH_TIMEOUT_2P5S 12u

/*
 * The proximity floor, in ANT's 1..10 bins (10 is nearest, 0 disables). Starts
 * at 6 - about arm's length on the straps this project has measured - and
 * relaxes one bin per search timeout down to 1, then to 0.
 *
 * IT NEVER TIGHTENS AGAIN WHILE TRACKING, which is the hold-down: a strap that
 * drops a few packets does not restart the search, and one that goes away for
 * good costs a full timeout before anything moves. Both halves are what stop a
 * marginal second strap flapping the display.
 */
#define PROX_START 6u
#define PROX_FLOOR 0u

/* Extended-message flag bits, from the same table tools/ant_wire.py records.
 * The fields appear in descending flag order, which is why the RSSI offset is
 * computed rather than constant. */
#define EXT_FLAG_CHANNEL_ID   0x80u
#define EXT_FLAG_RSSI         0x40u
#define EXT_FLAG_RX_TIMESTAMP 0x20u

#define EXT_LEN_CHANNEL_ID   4u
#define EXT_LEN_RSSI         3u
#define EXT_LEN_RX_TIMESTAMP 2u

static uint8_t  hr_channel = 0xFFu;
static uint8_t  prox_threshold = PROX_START;
static uint16_t tracked_device;
static int8_t   tracked_rssi_dbm;
static uint8_t  last_bpm;

/* How many consecutive messages the tracked strap has reported no reading.
 * PROFILE_HR_INVALID_BPM is 0 on this profile - NOT 0xFF, which is a real
 * 255 bpm - so "no reading" is a value the strap sends deliberately while it
 * has no skin contact, and one of them is not a reason to drop anything. */
#define NO_READING_LIMIT 20u
static uint8_t no_reading_run;

static void deliver_work_fn(struct k_work *w);
static K_WORK_DEFINE(deliver_work, deliver_work_fn);
static atomic_t pending_bpm = ATOMIC_INIT(TREADMILL_HR_NONE);

static void deliver_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	/* The one hop to the workqueue, for the reason treadmill_source.h's
	 * rule 1 gives: the radiant callback path must not touch a profile
	 * module that the workqueue is also in. */
	treadmill_node_heart_rate((uint8_t)atomic_get(&pending_bpm));
}

static void publish_bpm(uint8_t bpm)
{
	if (bpm == last_bpm) {
		return;
	}
	last_bpm = bpm;
	atomic_set(&pending_bpm, (atomic_val_t)bpm);
	k_work_submit(&deliver_work);
}

uint16_t treadmill_hr_rx_device(void)
{
	return tracked_device;
}

static int configure_channel(uint8_t channel)
{
	antr_err_t rc;

	rc = antr_channel_assign(channel, ANTW_CHANNEL_TYPE_SLAVE, HR_NETWORK,
				 0u);
	if (rc != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("hr ch%u assign: %d", channel, (int)rc);
		return -EIO;
	}

	/* WILDCARD device number and WILDCARD transmission type: 0 in either
	 * field means "any". The device TYPE is not wildcarded - a slave that
	 * matched any type would acquire the treadmill's own FE-C master. */
	rc = antr_channel_id_set(channel, 0u, PROFILE_HR_DEVICE_TYPE, 0u);
	if (rc != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("hr ch%u channel id: %d", channel, (int)rc);
		return -EIO;
	}

	rc = antr_channel_period_set(channel, HR_PERIOD);
	if (rc != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("hr ch%u period: %d", channel, (int)rc);
		return -EIO;
	}

	rc = antr_channel_radio_freq_set(channel, HR_RF_FREQ);
	if (rc != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("hr ch%u rf freq: %d", channel, (int)rc);
		return -EIO;
	}

	(void)antr_channel_search_timeout_set(channel, HR_SEARCH_TIMEOUT_2P5S);

	/* LOW priority search, and it is the whole coexistence decision in one
	 * call: a low-priority search yields to the two masters' transmit
	 * slots rather than competing with them. A high-priority search on a
	 * node that also transmits is how a sweep starves an arbiter. */
	(void)antr_search_channel_priority_set(channel, 0u);

	/* The proximity floor - see the header. A backend that does not
	 * implement it refuses here, which costs the "nearest strap" property
	 * and nothing else, so it is not fatal. */
	if (antr_prox_search_set(channel, prox_threshold, 0u) !=
	    ANTW_RESPONSE_NO_ERROR) {
		LOG_WRN("hr: no proximity search on this backend - the first "
			"strap heard will be acquired, near or far");
	}

	/* Channel id and RSSI on every received message. The channel id is
	 * what names the strap in the log and the RSSI is what the hold-down
	 * is measured in; without them this module still works and simply
	 * cannot say which strap it has. */
	(void)antr_lib_config_set(ANTW_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID |
				  ANTW_LIB_CONFIG_MESG_OUT_INC_RSSI);

	return 0;
}

int treadmill_hr_rx_start(uint8_t channel)
{
	antr_err_t rc;
	int        err;

	hr_channel = channel;
	err = configure_channel(channel);
	if (err != 0) {
		return err;
	}

	rc = antr_channel_open(channel);
	if (rc != ANTW_RESPONSE_NO_ERROR) {
		LOG_ERR("hr ch%u open: %d", channel, (int)rc);
		return -EIO;
	}

	LOG_INF("heart rate: searching (0x78, wildcard, proximity bin %u)",
		prox_threshold);
	return 0;
}

/*
 * Relax the floor by one bin and search again. Called only on a search
 * TIMEOUT, never on a lost packet, which is the hold-down: nothing here reacts
 * to a strap dropping a message.
 */
static void relax_and_research(void)
{
	if (prox_threshold > PROX_FLOOR) {
		prox_threshold--;
	}
	tracked_device = 0u;
	publish_bpm(TREADMILL_HR_NONE);

	(void)antr_channel_close(hr_channel);
	(void)antr_prox_search_set(hr_channel, prox_threshold, 0u);
	if (antr_channel_open(hr_channel) == ANTW_RESPONSE_NO_ERROR) {
		LOG_INF("heart rate: search timed out, floor now bin %u",
			prox_threshold);
	}
}

/* Pull the channel id and RSSI out of an extended message, if they are there.
 * The fields follow the eight payload bytes in DESCENDING FLAG ORDER, so a
 * fixed RSSI offset is wrong whenever the channel id is absent - which is
 * exactly the shape of bug that reads a device number's low byte as a signal
 * strength. */
static void parse_ext(const uint8_t *data, uint8_t len, uint16_t *device,
		      int8_t *rssi_dbm)
{
	uint8_t offset = 9u; /* channel + eight payload bytes */
	uint8_t flags;

	if (len <= offset) {
		return;
	}
	flags = data[offset++];

	if ((flags & EXT_FLAG_CHANNEL_ID) != 0u) {
		if ((uint8_t)(offset + EXT_LEN_CHANNEL_ID) > len) {
			return;
		}
		*device = (uint16_t)data[offset] |
			  ((uint16_t)data[offset + 1u] << 8);
		offset += EXT_LEN_CHANNEL_ID;
	}
	if ((flags & EXT_FLAG_RSSI) != 0u) {
		if ((uint8_t)(offset + EXT_LEN_RSSI) > len) {
			return;
		}
		/* [0] measurement type, [1] the value in dBm, [2] threshold
		 * configuration. The value is signed and routinely negative. */
		*rssi_dbm = (int8_t)data[offset + 1u];
	}
}

bool treadmill_hr_rx_message(const struct antr_msg *msg)
{
	const uint8_t *payload;
	uint8_t        bpm;

	if (msg == NULL || msg->data == NULL || hr_channel == 0xFFu) {
		return false;
	}

	if (msg->id == ANTW_MESG_RESPONSE_EVENT_ID &&
	    msg->len >= ANTW_MESG_RESPONSE_EVENT_SIZE &&
	    msg->data[0] == hr_channel) {
		if (msg->data[2] == ANTW_EVENT_RX_SEARCH_TIMEOUT) {
			relax_and_research();
		}
		return true;
	}

	if ((msg->id != ANTW_MESG_BROADCAST_DATA_ID &&
	     msg->id != ANTW_MESG_EXT_BROADCAST_DATA_ID) ||
	    msg->len < 9u || msg->data[0] != hr_channel) {
		return false;
	}

	payload = &msg->data[1];

	{
		static uint16_t announced;

		parse_ext(msg->data, msg->len, &tracked_device,
			  &tracked_rssi_dbm);
		if (tracked_device != announced) {
			announced = tracked_device;
			/* Named once, on acquisition, with the signal strength
			 * the proximity floor was judged against - which is the
			 * number a bench operator needs when deciding whether
			 * PROX_START is right for their straps. */
			LOG_INF("heart rate: tracking strap #%u at %d dBm "
				"(proximity bin %u)",
				tracked_device, (int)tracked_rssi_dbm,
				prox_threshold);
		}
	}

	/*
	 * Byte [7] of every page of device type 0x78 is the sensor's own
	 * COMPUTED heart rate - profile_hr.h's "bytes [4..7] are the same
	 * heartbeat event time, beat count and computed heart rate on EVERY
	 * page", which is why nothing here has to know or care which page
	 * arrived, or mask byte [0]'s page-change toggle.
	 *
	 * 0 IS "NO READING" ON THIS PROFILE AND 0xFF IS A REAL 255 BPM. That
	 * is the opposite of the common-page convention and it is
	 * PROFILE_HR_INVALID_BPM's whole reason for existing.
	 */
	bpm = payload[7];

	if (bpm == PROFILE_HR_INVALID_BPM) {
		/* A strap with no skin contact says this deliberately, and one
		 * message of it is not a reason to drop a reading. A run of
		 * them is. */
		if (no_reading_run < NO_READING_LIMIT) {
			no_reading_run++;
			return true;
		}
		publish_bpm(TREADMILL_HR_NONE);
		return true;
	}

	no_reading_run = 0u;
	publish_bpm(bpm);
	return true;
}
