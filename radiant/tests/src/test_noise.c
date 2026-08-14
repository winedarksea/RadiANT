/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work, written against
 * radiant/include/radiant/radiant_noise.h and
 * radiant/tests/fake_radio.h.
 *
 * The noise floor is a diagnostic - a bug here cannot lose a packet, but it
 * can be quietly wrong in a way that sends somebody looking in the wrong
 * place, which is worse than not having one. Two failure modes matter:
 *   - Measuring the wrong population: a window that received a packet has an
 *     RSSI, and it's the transmitter's, not the noise floor's.
 *   - A percentile that isn't one: it must be the 10th percentile of the
 *     samples, not of the bins.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <radiant/radiant_frame.h>
#include <radiant/radiant_noise.h>
#include "../fake_radio.h"

static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	fake_radio_reset();
	radiant_noise_reset();
}

ZTEST_SUITE(radiant_noise, NULL, NULL, before, NULL, NULL);

/* ---------------------------------------------------------------------------
 * The histogram
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_noise, test_percentiles_of_a_known_distribution)
{
	struct radiant_noise_report r;
	int i;

	/* A hundred samples spread evenly from -99 to -90 dBm, ten of each: the
	 * 10th percentile is the tenth sample (last -99), the 90th is the
	 * ninetieth (last -91) - a flat distribution makes both arithmetic. */
	for (i = 0; i < 10; i++) {
		int j;

		for (j = 0; j < 10; j++) {
			radiant_noise_note(57u, (int8_t)(-99 + i));
		}
	}

	zassert_true(radiant_noise_get(0u, &r), NULL);
	zassert_equal(57u, r.rf_index, NULL);
	zassert_equal(100u, r.samples, NULL);
	zassert_equal(-99, r.floor_dbm, "floor was %d", (int)r.floor_dbm);
	zassert_equal(-91, r.busy_dbm, "busy was %d", (int)r.busy_dbm);
	zassert_equal(-99, r.min_dbm, NULL);
	zassert_equal(-90, r.max_dbm, NULL);
	zassert_equal(0u, r.below_range, NULL);
	zassert_equal(0u, r.above_range, NULL);
}

ZTEST(radiant_noise, test_a_quiet_band_and_a_busy_one_are_told_apart)
{
	struct radiant_noise_report quiet;
	struct radiant_noise_report busy;
	int i;

	/* The measurement this feature exists to make: a quiet band sits within
	 * a couple dB of itself, while a bursty band has the same floor but a
	 * 90th percentile 30 dB above it. A mean would blur both into "about
	 * -93"/"about -85" and settle nothing. */
	for (i = 0; i < 100; i++) {
		radiant_noise_note(2u, (int8_t)(-95 + (i % 3)));
		radiant_noise_note(57u, (i % 5 == 0) ? (int8_t)-60
						    : (int8_t)(-95 + (i % 3)));
	}

	zassert_true(radiant_noise_get(0u, &quiet), NULL);
	zassert_true(radiant_noise_get(1u, &busy), NULL);
	zassert_equal(2u, quiet.rf_index, NULL);
	zassert_equal(57u, busy.rf_index, NULL);

	zassert_equal(quiet.floor_dbm, busy.floor_dbm,
		      "the same floor under both, since only the bursts differ");
	zassert_true(busy.busy_dbm - quiet.busy_dbm > 20,
		      "the busy band's 90th percentile is only %d dB above the "
		      "quiet one's", (int)(busy.busy_dbm - quiet.busy_dbm));
}

ZTEST(radiant_noise, test_samples_outside_the_range_are_counted_at_the_edges)
{
	struct radiant_noise_report r;
	int i;

	/* A distribution piled against a wall must say so: a percentile that
	 * quietly stopped moving because every sample lands in the last bin
	 * reads as stable and isn't. */
	for (i = 0; i < 30; i++) {
		radiant_noise_note(57u, (int8_t)-120);   /* below the range */
	}
	for (i = 0; i < 30; i++) {
		radiant_noise_note(57u, (int8_t)-10);    /* above it */
	}

	zassert_true(radiant_noise_get(0u, &r), NULL);
	zassert_equal(60u, r.samples, NULL);
	zassert_equal(30u, r.below_range, NULL);
	zassert_equal(30u, r.above_range, NULL);
	/* min and max are the REAL values, not the clamped ones - which is what
	 * says how far outside the range the population went. */
	zassert_equal(-120, r.min_dbm, NULL);
	zassert_equal(-10, r.max_dbm, NULL);
	zassert_equal(RADIANT_NOISE_DBM_MIN, r.floor_dbm, NULL);
	zassert_equal(RADIANT_NOISE_DBM_MAX, r.busy_dbm, NULL);
}

ZTEST(radiant_noise, test_too_few_samples_is_no_answer_rather_than_a_bad_one)
{
	struct radiant_noise_report r;
	uint32_t i;

	for (i = 0u; i < RADIANT_NOISE_MIN_SAMPLES - 1u; i++) {
		radiant_noise_note(57u, (int8_t)-93);
		zassert_false(radiant_noise_get(0u, &r),
			      "reported a percentile from %u sample(s)", i + 1u);
	}
	radiant_noise_note(57u, (int8_t)-93);
	zassert_true(radiant_noise_get(0u, &r), NULL);
	zassert_equal(-93, r.floor_dbm, NULL);
}

ZTEST(radiant_noise, test_each_frequency_gets_its_own_histogram)
{
	struct radiant_noise_report r;
	uint8_t slot;
	int i;
	bool seen_2 = false;
	bool seen_57 = false;

	/* The whole use of the number is comparing one frequency against
	 * another. A shared histogram would average 2402 MHz and 2457 MHz into
	 * a single figure that describes neither. */
	for (i = 0; i < 40; i++) {
		radiant_noise_note(2u, (int8_t)-100);
		radiant_noise_note(57u, (int8_t)-80);
	}

	for (slot = 0u; slot < RADIANT_NOISE_SLOTS; slot++) {
		if (!radiant_noise_get(slot, &r)) {
			continue;
		}
		if (r.rf_index == 2u) {
			seen_2 = true;
			zassert_equal(-100, r.floor_dbm, NULL);
		} else if (r.rf_index == 57u) {
			seen_57 = true;
			zassert_equal(-80, r.floor_dbm, NULL);
		}
	}
	zassert_true(seen_2 && seen_57, NULL);
}

ZTEST(radiant_noise, test_a_fifth_frequency_is_dropped_and_counted)
{
	struct radiant_noise_report r;
	uint8_t rf;
	int i;

	/* Dropped rather than evicted, and counted rather than silent: evicting
	 * would let a frequency visited once destroy the record of the one the
	 * dongle lives on, with no sign it happened. */
	for (rf = 0u; rf < (uint8_t)RADIANT_NOISE_SLOTS; rf++) {
		for (i = 0; i < 40; i++) {
			radiant_noise_note(rf, (int8_t)-90);
		}
	}
	zassert_equal(0u, radiant_noise_unslotted(), NULL);

	for (i = 0; i < 40; i++) {
		radiant_noise_note(99u, (int8_t)-50);
	}
	zassert_equal(40u, radiant_noise_unslotted(), NULL);

	/* And the four that were there are untouched. */
	for (rf = 0u; rf < (uint8_t)RADIANT_NOISE_SLOTS; rf++) {
		zassert_true(radiant_noise_get(rf, &r), NULL);
		zassert_equal(rf, r.rf_index, NULL);
		zassert_equal(40u, r.samples, NULL);
		zassert_equal(-90, r.floor_dbm, NULL);
	}
}

ZTEST(radiant_noise, test_clearing_keeps_the_frequency_and_drops_the_distribution)
{
	struct radiant_noise_report r;
	int i;

	for (i = 0; i < 40; i++) {
		radiant_noise_note(57u, (int8_t)-90);
	}
	zassert_true(radiant_noise_get(0u, &r), NULL);

	radiant_noise_clear(0u);
	zassert_false(radiant_noise_get(0u, &r),
		      "a cleared slot still reported a distribution");

	/* The frequency has to survive: a slot that gave up its rf_index would
	 * be reallocated to whichever frequency spoke next, and consecutive log
	 * lines would appear to be about the same band without being so. */
	for (i = 0; i < 40; i++) {
		radiant_noise_note(2u, (int8_t)-70);
	}
	zassert_true(radiant_noise_get(1u, &r),
		     "the new frequency did not take a fresh slot");
	zassert_equal(2u, r.rf_index, NULL);

	for (i = 0; i < 40; i++) {
		radiant_noise_note(57u, (int8_t)-70);
	}
	zassert_true(radiant_noise_get(0u, &r), NULL);
	zassert_equal(57u, r.rf_index, "the cleared slot lost its frequency");
	zassert_equal(-70, r.floor_dbm, "the cleared slot kept its old samples");
}

ZTEST(radiant_noise, test_bad_arguments)
{
	struct radiant_noise_report r;

	zassert_false(radiant_noise_get(RADIANT_NOISE_SLOTS, &r), NULL);
	zassert_false(radiant_noise_get(0u, NULL), NULL);
	radiant_noise_clear(RADIANT_NOISE_SLOTS);   /* must not fault */
	zassert_false(radiant_noise_get(0u, &r), NULL);
}

/* ---------------------------------------------------------------------------
 * Which windows contribute, decided by the backend
 * ---------------------------------------------------------------------------
 */

static uint32_t n_terminal;
static bool     last_has_noise;
static int8_t   last_noise_dbm;

static void noise_rx_cb(const struct radiant_rx_event *evt, void *user)
{
	ARG_UNUSED(user);

	if (evt->status != RADIANT_RADIO_STATUS_TIMEOUT) {
		return;
	}
	n_terminal++;
	last_has_noise = evt->has_noise;
	last_noise_dbm = evt->noise_dbm;
}

static void noise_tx_cb(const struct radiant_tx_event *evt, void *user)
{
	ARG_UNUSED(evt);
	ARG_UNUSED(user);
}

static const struct radiant_radio_cbs noise_cbs = { noise_rx_cb, noise_tx_cb };

static const uint8_t noise_addr[RADIANT_FRAME_ADDR_LEN_TRACKING] = {
	0xA6, 0xC5, 0x17, 0x3A, 0x0B,
};

/* Run one window. `with_frame` puts a good frame in it, which is the case that
 * must contribute nothing. */
static void run_window(bool with_frame)
{
	struct radiant_rx_filter filter;
	struct radiant_rx_req    req;
	uint32_t             op = 0u;
	radiant_time_t           t0;

	n_terminal = 0u;
	last_has_noise = false;
	last_noise_dbm = 0;

	t0 = radiant_radio_now();

	memset(&filter, 0, sizeof(filter));
	memcpy(filter.addr, noise_addr, sizeof(noise_addr));
	filter.addr_len = RADIANT_FRAME_ADDR_LEN_TRACKING;

	memset(&req, 0, sizeof(req));
	req.fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &filter;
	req.n_filters = 1u;
	req.t_open = t0 + 1000u;
	req.t_close = t0 + 200000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op), NULL);

	if (with_frame) {
		struct fake_radio_air air;
		uint8_t body[10] = { 0x05, 0x0A, 0x10, 0, 0, 0, 0, 0, 0, 0 };

		fake_radio_air_init(&air);
		air.t_sync = t0 + 2000u;
		memcpy(air.bytes, noise_addr, sizeof(noise_addr));
		memcpy(air.bytes + sizeof(noise_addr), body, sizeof(body));
		air.len = (uint8_t)(sizeof(noise_addr) + sizeof(body));
		zassert_equal(RADIANT_RADIO_OK_RC, fake_radio_air_add(&air), NULL);
	}

	fake_radio_advance_to(t0 + 300000u);
	zassert_equal(1u, n_terminal, "expected exactly one terminal event");
}

ZTEST(radiant_noise, test_an_empty_window_reports_the_floor)
{
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_init(&noise_cbs, NULL),
		      NULL);
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_enable(), NULL);
	fake_radio_set_noise(-97);

	run_window(false);

	zassert_true(last_has_noise,
		     "a window that heard nothing reported no noise sample, "
		     "which is the entire population this measures");
	zassert_equal(-97, last_noise_dbm, NULL);
	zassert_equal(0u, fake_radio_viol_count(), NULL);
}

ZTEST(radiant_noise, test_a_window_that_received_a_packet_contributes_nothing)
{
	/* A window that received something has an RSSI, and it's the
	 * transmitter's. Counting it as noise would make every busy channel
	 * look noisy - plausibly so, since a nearby ANT+ master really is
	 * 40 dB above the floor. */
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_init(&noise_cbs, NULL),
		      NULL);
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_enable(), NULL);
	fake_radio_set_noise(-97);

	run_window(true);

	zassert_false(last_has_noise,
		      "a window that received a frame contributed a noise "
		      "sample; that sample is the transmitter's level");
	zassert_equal(0u, fake_radio_viol_count(), NULL);
}

ZTEST(radiant_noise, test_a_backend_that_cannot_measure_says_nothing)
{
	/* No fake_radio_set_noise(): a backend with nothing to say. The core
	 * must then have no samples at all rather than a histogram full of
	 * zeroes, which would read as an impossibly loud band. */
	struct radiant_noise_report r;

	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_init(&noise_cbs, NULL),
		      NULL);
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_enable(), NULL);

	run_window(false);

	zassert_false(last_has_noise, NULL);
	zassert_false(radiant_noise_get(0u, &r), NULL);
}
