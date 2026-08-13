/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work, written against
 * src/profiles/profile_handoff.h, radiant_core/include/radiant_core/radiant_channel.h,
 * radiant_core/include/radiant_core/radiant_search.h and
 * radiant_core/tests/fake_radio.h, with layout expectations from
 * docs/radiant-telemetry.md section 12. See docs/decisions/0002-clean-room-policy.md.
 *
 * Gate: a sync-handoff acquisition lands in the same channel state a sweep
 * would, without arming a search window. A real wildcard sweep and a handoff
 * are run against the same mock radio and compared field by field, so a
 * handoff that merely LOOKS tracked (right state, wrong guard, slot clock a
 * period out) cannot pass unnoticed. A second test turns the same comparison
 * into a discovery-latency measurement (virtual microseconds, not wall clock).
 *
 * CANON_* frame bytes are shared byte for byte with tools/test_ant_pages.py.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include "../fake_radio.h"

#include <radiant_core/radiant_channel.h>
#include <radiant_core/radiant_frame.h>
#include <radiant_core/radiant_radio_hal.h>
#include <radiant_core/radiant_search.h>

#include "profile_handoff.h"

#define TYPE_SLAVE  0x00u
#define TYPE_MASTER 0x10u

#define SWEEP_CH   1u
#define HANDOFF_CH 2u

/* 0xFF is slot 7 of set 31 - the LAST set an eight-filter sweep reaches, so the
 * sweep half of the A/B is its worst case rather than a lucky one. */
#define WORST_CASE_DEVNUM 0x01FFu
#define NODE_DEVTYPE      0x60u
#define NODE_TRANSTYPE    0x05u
#define NODE_PERIOD       8182u   /* counts; the 4 Hz ANT+ period */

static const uint8_t payload8[8] = {
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17
};

/* The canonical handoff, and the sixteen bytes it must become. Next slot 5000
 * counts after frame 1's t_sync, at the 4 Hz period, with a 32 kHz crystal
 * announced. */
static const struct profile_handoff CANON = {
	.device_number = 0xB1CEu,
	.period = 8182u,
	.phase = 5006u,
	.device_type = 0x60u,
	.trans_type = 0x05u,
	.clock_accuracy = PROFILE_HANDOFF_CLK_30PPM,
	.counter = 0x2Au,
};

static const uint8_t CANON_BYTES[PROFILE_HANDOFF_FRAMES *
				 PROFILE_HANDOFF_FRAME_LEN] = {
	0x12, 0x2A, 0x01, 0xB1, 0xCE, 0xC0, 0x0A, 0x00,
	0x12, 0x2A, 0x11, 0x1F, 0xF6, 0x9C, 0x76, 0x00,
};

/* ---------------------------------------------------------------------------
 * Search fixtures - the same shape test_search.c uses, because the sweep half
 * of the gate has to be a REAL sweep and not a stand-in for one.
 * ---------------------------------------------------------------------------
 */

static struct radiant_search g_s;
static struct radiant_search_window g_w;
static uint32_t g_op;
static uint32_t g_windows_run;
static uint32_t g_n_acquired;
static radiant_time_t g_t_acquired;

static const struct radiant_search_id_filter want_any = { 0u, 0u, 0u };

/* Mirrors radiant_api.c's adapter: hand the search result to the channel via
 * radiant_channel_on_acquired(), the only path there is - which is what makes
 * the comparison below meaningful. */
static void on_acquired(uint8_t channel, const struct radiant_search_result *r,
			void *user)
{
	ARG_UNUSED(user);
	radiant_channel_on_acquired(channel, &r->id, r->t_sync);
	g_t_acquired = radiant_radio_now();
	g_n_acquired++;
}

static void on_timeout(uint8_t channel, void *user)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(user);
}

static const struct radiant_search_cbs search_cbs = {
	.acquired = on_acquired,
	.timeout = on_timeout,
};

static void rx_cb(const struct radiant_rx_event *evt, void *user)
{
	ARG_UNUSED(user);
	radiant_search_on_rx(&g_s, evt);
}

static const struct radiant_radio_cbs radio_cbs = { .rx = rx_cb, .tx = NULL };

static void bring_up(void)
{
	struct radiant_search_cfg cfg;

	memset(&g_s, 0, sizeof(g_s));
	g_windows_run = 0u;
	g_n_acquired = 0u;
	g_t_acquired = 0u;

	radiant_search_cfg_default(&cfg);
	zassert_ok(radiant_search_init(&g_s, &cfg, &search_cbs, NULL));
	zassert_ok(radiant_radio_init(&radio_cbs, NULL));
	zassert_ok(radiant_radio_enable());
}

static int win_open(void)
{
	struct radiant_rx_req req;
	radiant_time_t earliest;
	int rc;

	earliest = radiant_radio_now() +
		   radiant_radio_caps_get()->min_arm_lead_us + 100u;

	rc = radiant_search_window(&g_s, earliest, &g_w);
	if (rc != RADIANT_SEARCH_OK) {
		return rc;
	}

	memset(&req, 0, sizeof(req));
	req.fmt = g_w.fmt;
	req.rf_index = g_w.rf_index;
	req.filters = g_w.filters;
	req.n_filters = g_w.n_filters;
	req.t_open = g_w.t_open;
	req.t_close = g_w.t_close;
	req.flags = g_w.flags;

	zassert_ok(radiant_radio_rx(&req, &g_op), "arming the search failed");
	radiant_search_armed(&g_s, g_op, req.t_open, req.t_close);
	return RADIANT_SEARCH_OK;
}

static void win_run(void)
{
	const struct fake_radio_arm *a = fake_radio_arm_for_op(g_op);

	zassert_not_null(a);
	fake_radio_advance_to(a->t_close + 1u);
	g_windows_run++;
	radiant_search_tick(&g_s, radiant_radio_now());
}

static uint32_t pump(uint32_t max_windows)
{
	uint32_t n = 0u;

	while (n < max_windows) {
		if (win_open() != RADIANT_SEARCH_OK) {
			break;
		}
		win_run();
		n++;
	}
	return n;
}

static void end_of_test(void)
{
	zassert_true(fake_radio_is_idle(), "%s", fake_radio_busy_reason());
	zassert_equal(0u, fake_radio_viol_count(), "%s",
		      fake_radio_viol_name(fake_radio_viol(0)->code));
}

static void handoff_before(void *fixture)
{
	ARG_UNUSED(fixture);
	fake_radio_reset();
	radiant_channel_init();
}

/* ---------------------------------------------------------------------------
 * The codec
 * ---------------------------------------------------------------------------
 */

ZTEST(handoff, test_the_frames_are_byte_for_byte_what_the_python_encoder_emits)
{
	uint8_t out[sizeof(CANON_BYTES)];

	zassert_ok(profile_handoff_encode(&CANON, out));
	zassert_mem_equal(out, CANON_BYTES, sizeof(CANON_BYTES),
			  "the two implementations have drifted");
}

ZTEST(handoff, test_page_number_counter_and_frame_index)
{
	uint8_t out[sizeof(CANON_BYTES)];
	uint8_t index;
	uint8_t count;
	uint8_t i;

	zassert_ok(profile_handoff_encode(&CANON, out));

	for (i = 0u; i < PROFILE_HANDOFF_FRAMES; i++) {
		const uint8_t *f = &out[i * PROFILE_HANDOFF_FRAME_LEN];

		zassert_equal(PROFILE_HANDOFF_PAGE, f[0]);
		/* Byte [1] is the COUNTER, not the frame index (that's byte [2]) -
		 * avoids a third exception to the section 4 counter invariant,
		 * which the security switches read a nonce out of. */
		zassert_equal(CANON.counter, f[1]);
		zassert_ok(profile_handoff_frame_index(f, &index, &count));
		zassert_equal(i, index);
		zassert_equal(PROFILE_HANDOFF_FRAMES, count);
	}
}

ZTEST(handoff, test_round_trip)
{
	uint8_t out[sizeof(CANON_BYTES)];
	struct profile_handoff got;

	zassert_ok(profile_handoff_encode(&CANON, out));
	zassert_ok(profile_handoff_decode(out, &got));

	zassert_equal(CANON.device_number, got.device_number);
	zassert_equal(CANON.device_type, got.device_type);
	zassert_equal(CANON.trans_type, got.trans_type);
	zassert_equal(CANON.period, got.period);
	zassert_equal(CANON.phase, got.phase);
	zassert_equal(CANON.clock_accuracy, got.clock_accuracy);
	zassert_equal(CANON.counter, got.counter);
}

ZTEST(handoff, test_the_two_frames_are_order_insensitive)
{
	uint8_t out[sizeof(CANON_BYTES)];
	uint8_t swapped[sizeof(CANON_BYTES)];
	struct profile_handoff got;

	zassert_ok(profile_handoff_encode(&CANON, out));
	memcpy(&swapped[0], &out[PROFILE_HANDOFF_FRAME_LEN],
	       PROFILE_HANDOFF_FRAME_LEN);
	memcpy(&swapped[PROFILE_HANDOFF_FRAME_LEN], &out[0],
	       PROFILE_HANDOFF_FRAME_LEN);

	/* Frame 0 is timeless and frame 1 is self-dating, so a receiver that
	 * hears them backwards or a rotation apart loses nothing but the wait. */
	zassert_ok(profile_handoff_decode(swapped, &got));
	zassert_equal(CANON.device_number, got.device_number);
	zassert_equal(CANON.phase, got.phase);
}

ZTEST(handoff, test_two_frames_of_different_handoffs_are_refused)
{
	struct profile_handoff other = CANON;
	uint8_t a[sizeof(CANON_BYTES)];
	uint8_t b[sizeof(CANON_BYTES)];
	uint8_t mixed[sizeof(CANON_BYTES)];
	struct profile_handoff got;

	other.counter = 0x2Bu;
	other.device_number = 0x0001u;

	zassert_ok(profile_handoff_encode(&CANON, a));
	zassert_ok(profile_handoff_encode(&other, b));
	memcpy(&mixed[0], &a[0], PROFILE_HANDOFF_FRAME_LEN);
	memcpy(&mixed[PROFILE_HANDOFF_FRAME_LEN], &b[PROFILE_HANDOFF_FRAME_LEN],
	       PROFILE_HANDOFF_FRAME_LEN);

	/* Two counters are two handoffs, and merging them would describe a
	 * channel neither sender meant - well-formed, and pointing nowhere. */
	zassert_equal(-EBADMSG, profile_handoff_decode(mixed, &got));
}

ZTEST(handoff, test_an_incomplete_set_is_refused_rather_than_half_applied)
{
	uint8_t out[sizeof(CANON_BYTES)];
	uint8_t doubled[sizeof(CANON_BYTES)];
	struct profile_handoff got;

	zassert_ok(profile_handoff_encode(&CANON, out));
	memcpy(&doubled[0], &out[0], PROFILE_HANDOFF_FRAME_LEN);
	memcpy(&doubled[PROFILE_HANDOFF_FRAME_LEN], &out[0],
	       PROFILE_HANDOFF_FRAME_LEN);

	zassert_equal(-EBADMSG, profile_handoff_decode(doubled, &got));
}

ZTEST(handoff, test_every_reserved_field_is_zero_and_is_asserted_not_assumed)
{
	uint8_t out[sizeof(CANON_BYTES)];
	struct profile_handoff got;
	uint8_t i;

	zassert_ok(profile_handoff_encode(&CANON, out));

	for (i = 0u; i < PROFILE_HANDOFF_FRAMES; i++) {
		/* Byte [7] is tag space, not padding: section 4 puts a RadiANT
		 * page's authentication tag at the end and nowhere else, so no
		 * field may ever be placed there. */
		zassert_equal(0u, out[i * PROFILE_HANDOFF_FRAME_LEN + 7u]);
	}
	/* Frame 0's field area ends in one reserved bit. */
	zassert_equal(0u, out[6] & 0x01u);

	out[6] |= 0x01u;
	zassert_equal(-EPROTO, profile_handoff_decode(out, &got));

	zassert_ok(profile_handoff_encode(&CANON, out));
	out[PROFILE_HANDOFF_FRAME_LEN + 7u] = 0xFFu;
	zassert_equal(-EPROTO, profile_handoff_decode(out, &got));
}

ZTEST(handoff, test_the_page_has_no_room_for_an_epoch)
{
	/* No room for an epoch: for a hostless node the epoch is the boot
	 * counter, and broadcasting it in the clear would fingerprint the
	 * device and defeat per-boot device-number rotation. All 64 field bits
	 * are already assigned; _Static_asserts in profile_handoff.c catch any
	 * attempt to add one. */
	const uint32_t assigned = 16u + 7u + 8u + 1u   /* who  */
				  + 16u + 13u + 3u;    /* when */

	zassert_equal(2u * PROFILE_HANDOFF_AREA_BITS, assigned,
		      "a field area with slack in it is a field area an epoch "
		      "can be added to");
	/* And the area genuinely ends where the tag space begins: four bytes,
	 * not five. */
	zassert_equal(32u, PROFILE_HANDOFF_AREA_BITS);
	zassert_equal(8u, PROFILE_HANDOFF_FRAME_LEN);
}

ZTEST(handoff, test_the_clock_accuracy_ladder_is_the_documented_one)
{
	static const uint16_t ppm[] = { 500u, 250u, 150u, 100u,
					75u, 50u, 30u, 20u };
	uint8_t code;

	for (code = 0u; code < PROFILE_HANDOFF_CLK_COUNT; code++) {
		uint8_t out[sizeof(CANON_BYTES)];
		struct profile_handoff h = CANON;
		struct profile_handoff got;

		zassert_equal(ppm[code], profile_handoff_clk_ppm(code));

		h.clock_accuracy = code;
		zassert_ok(profile_handoff_encode(&h, out));
		zassert_ok(profile_handoff_decode(out, &got));
		zassert_equal(code, got.clock_accuracy);
	}

	/* Code 0 is "not stated", and on no evidence the worst case is the only
	 * safe answer. A consumer must round outward from these. */
	zassert_equal(500u, profile_handoff_clk_ppm(PROFILE_HANDOFF_CLK_UNKNOWN));
	zassert_equal(0u, profile_handoff_clk_ppm(PROFILE_HANDOFF_CLK_COUNT));
}

ZTEST(handoff, test_phase_resolution_stays_inside_the_guard_at_every_period)
{
	/* Phase is a period/8192 fraction rather than a 1/32768 s count: a
	 * round trip must stay under RADIANT_CHANNEL_GUARD_MAX_US at every
	 * period the field can express, which a fixed-resolution count could
	 * not do at both a 2 s period and 4 Hz. */
	static const uint16_t periods[] = { 273u, 1024u, 8182u, 16384u,
					    32768u, 65535u };
	size_t i;

	for (i = 0u; i < ARRAY_SIZE(periods); i++) {
		uint16_t period = periods[i];
		uint32_t phases[4];
		size_t j;

		phases[0] = 0u;
		phases[1] = 1u;
		phases[2] = period / 3u;
		phases[3] = (uint32_t)period - 1u;

		for (j = 0u; j < ARRAY_SIZE(phases); j++) {
			struct profile_handoff h = CANON;
			uint32_t back;
			uint32_t err_us;

			h.period = period;
			h.phase = profile_handoff_phase_encode(phases[j], period);
			back = profile_handoff_phase_counts(&h);

			err_us = (uint32_t)radiant_channel_counts_to_us(
				back > phases[j] ? back - phases[j]
						 : phases[j] - back);
			zassert_true(err_us < RADIANT_CHANNEL_GUARD_MAX_US,
				     "period %u phase %u: %u us of error",
				     period, phases[j], err_us);
		}
	}
}

ZTEST(handoff, test_a_phase_across_a_slot_boundary_is_reduced_not_refused)
{
	/* It is a phase. A caller one period out is not wrong, it is one period
	 * out, and refusing would push modular arithmetic to every caller. */
	zassert_equal(profile_handoff_phase_encode(5000u, 8182u),
		      profile_handoff_phase_encode(8182u + 5000u, 8182u));
	/* An asynchronous node has no slot to hand over. */
	zassert_equal(0u, profile_handoff_phase_encode(5000u, 0u));
}

ZTEST(handoff, test_an_unopenable_channel_is_refused_at_the_encoder)
{
	struct profile_handoff h;
	uint8_t out[sizeof(CANON_BYTES)];

	h = CANON;
	h.period = 0u;
	zassert_equal(-EINVAL, profile_handoff_encode(&h, out));

	/* The MSB of the on-air device type field is the pairing bit, which is
	 * a property of a search rather than of the node, so it is not handed
	 * over and 0x80..0xFF is not a device type. */
	h = CANON;
	h.device_type = 0xE0u;
	zassert_equal(-EINVAL, profile_handoff_encode(&h, out));

	h = CANON;
	h.device_type = 0u;
	zassert_equal(-EINVAL, profile_handoff_encode(&h, out));

	h = CANON;
	h.clock_accuracy = PROFILE_HANDOFF_CLK_COUNT;
	zassert_equal(-EINVAL, profile_handoff_encode(&h, out));
}

ZTEST(handoff, test_a_new_counter_abandons_a_partial_set)
{
	struct profile_handoff other = CANON;
	struct profile_handoff got;
	struct profile_handoff_rx rx;
	uint8_t a[sizeof(CANON_BYTES)];
	uint8_t b[sizeof(CANON_BYTES)];

	other.counter = 0x2Bu;
	other.device_number = 0x0042u;

	zassert_ok(profile_handoff_encode(&CANON, a));
	zassert_ok(profile_handoff_encode(&other, b));

	memset(&rx, 0, sizeof(rx));

	/* Frame 0 of the first handoff, then frame 1 of the second. Merging
	 * them would assemble a perfectly well-formed page describing a channel
	 * that never existed, whose only symptom is a search timeout minutes
	 * later - so the partial set is dropped instead. */
	zassert_equal(0, profile_handoff_rx_frame(&rx, &a[0], &got));
	zassert_equal(0, profile_handoff_rx_frame(
				&rx, &b[PROFILE_HANDOFF_FRAME_LEN], &got));

	/* And the second handoff completes on its own terms. */
	zassert_equal(1, profile_handoff_rx_frame(&rx, &b[0], &got));
	zassert_equal(0x0042u, got.device_number);
	zassert_equal(0x2Bu, got.counter);
}

/* ---------------------------------------------------------------------------
 * The gate: one sweep, one handoff, one comparison
 * ---------------------------------------------------------------------------
 */

/* Put the node on the air and acquire it the slow way. Returns the number of
 * search windows the sweep needed. */
static uint32_t sweep_acquire(void)
{
	uint8_t frame[16];
	uint8_t len;

	len = fake_radio_build_ant_frame(frame, WORST_CASE_DEVNUM,
					 NODE_DEVTYPE, NODE_TRANSTYPE,
					 payload8);
	(void)fake_radio_air_master(radiant_radio_now() + 1000u,
				    FAKE_RADIO_ANT_PERIOD_US, 200u, frame, len);

	/* A wildcard slave: device number 0 is what a receiver that has never
	 * heard this node has to start from, and it is exactly what the handoff
	 * makes unnecessary. */
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_assign(SWEEP_CH, TYPE_SLAVE, 0u, 0u));
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_id_set(SWEEP_CH, 0u, 0u, 0u));
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_period_set(SWEEP_CH, NODE_PERIOD));
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_open(SWEEP_CH, 0u, radiant_radio_now()));

	zassert_ok(radiant_search_begin(&g_s, SWEEP_CH,
					RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
					radiant_radio_now(),
					RADIANT_SEARCH_TIMEOUT_NONE));

	(void)pump(radiant_search_sets(&g_s) + 1u);

	zassert_equal(1u, g_n_acquired, "the sweep did not acquire");
	zassert_equal(RADIANT_CH_STATE_TRACKING,
		      radiant_channel_state_get(SWEEP_CH));
	return g_windows_run;
}

ZTEST(handoff, test_a_handoff_lands_in_the_same_channel_state_a_sweep_does)
{
	struct profile_handoff h;
	struct radiant_channel_id swept_id;
	struct radiant_channel_id handed_id;
	uint8_t frames[PROFILE_HANDOFF_FRAMES * PROFILE_HANDOFF_FRAME_LEN];
	struct profile_handoff received;
	radiant_time_t t_carrier;
	radiant_time_t next_swept;
	radiant_time_t next_handed;
	radiant_time_t period_us;
	radiant_time_t off;
	uint8_t swept_status;
	uint8_t handed_status;

	bring_up();
	(void)sweep_acquire();

	next_swept = radiant_channel_next_slot(SWEEP_CH);

	/* The receiver already tracking builds a handoff from what it knows and
	 * sends it as sixteen bytes; nothing else passes between the two
	 * channels, so the comparison below tests the format, not this test. */
	t_carrier = radiant_radio_now();
	zassert_ok(profile_handoff_from_channel(SWEEP_CH, t_carrier, 0x2Au,
						PROFILE_HANDOFF_CLK_30PPM, &h));
	zassert_ok(profile_handoff_encode(&h, frames));
	zassert_ok(profile_handoff_decode(frames, &received));

	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_assign(HANDOFF_CH, TYPE_SLAVE, 0u, 0u));
	zassert_equal(RADIANT_CH_OK,
		      profile_handoff_apply(HANDOFF_CH, &received, t_carrier,
					    radiant_radio_now()));

	/* ---- field by field ---- */

	zassert_equal(RADIANT_CH_STATE_TRACKING,
		      radiant_channel_state_get(HANDOFF_CH));
	zassert_equal(radiant_channel_state_get(SWEEP_CH),
		      radiant_channel_state_get(HANDOFF_CH));

	/*
	 * The ACQUIRED id, which is what a host reads back. The two channels'
	 * CONFIGURED ids differ on purpose - the swept one was opened on a
	 * wildcard and the handed one was told the number - and that difference
	 * is the feature, not a discrepancy.
	 */
	zassert_equal(RADIANT_CH_OK, radiant_channel_id_get(SWEEP_CH, &swept_id));
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_id_get(HANDOFF_CH, &handed_id));
	zassert_equal(swept_id.device_number, handed_id.device_number);
	zassert_equal(swept_id.device_type, handed_id.device_type);
	zassert_equal(swept_id.trans_type, handed_id.trans_type);
	zassert_equal(WORST_CASE_DEVNUM, handed_id.device_number);

	{
		uint16_t swept_period;
		uint16_t handed_period;

		zassert_equal(RADIANT_CH_OK,
			      radiant_channel_period_get(SWEEP_CH, &swept_period));
		zassert_equal(RADIANT_CH_OK,
			      radiant_channel_period_get(HANDOFF_CH,
							 &handed_period));
		zassert_equal(swept_period, handed_period);
		period_us = radiant_channel_counts_to_us(handed_period);
	}

	/* A handed-off channel has measured nothing about this master's clock,
	 * so it must get the widest guard - same as a freshly swept one.
	 * Narrowing on the sender's estimate is the one thing the estimator
	 * must never do. */
	zassert_equal(RADIANT_CHANNEL_GUARD_MAX_US,
		      radiant_channel_guard_us(HANDOFF_CH));
	zassert_equal(radiant_channel_guard_us(SWEEP_CH),
		      radiant_channel_guard_us(HANDOFF_CH));
	zassert_equal(radiant_channel_residual_us(SWEEP_CH),
		      radiant_channel_residual_us(HANDOFF_CH));

	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_status_get(SWEEP_CH, &swept_status));
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_status_get(HANDOFF_CH, &handed_status));
	zassert_equal(swept_status, handed_status);

	zassert_true(radiant_channel_is_open(HANDOFF_CH));
	zassert_false(radiant_channel_is_master(HANDOFF_CH));
	zassert_equal(RADIANT_TIME_NEVER,
		      radiant_channel_search_deadline(HANDOFF_CH));
	zassert_equal(radiant_channel_search_deadline(SWEEP_CH),
		      radiant_channel_search_deadline(HANDOFF_CH));

	/* Slot clock compared modulo the period: a slot one period later is the
	 * same schedule. The residual is the phase's period/8192 quantisation
	 * and must fit inside the narrowest guard the estimator ever chooses. */
	next_handed = radiant_channel_next_slot(HANDOFF_CH);
	off = (next_handed > next_swept) ? (next_handed - next_swept)
					 : (next_swept - next_handed);
	off %= period_us;
	if (off > period_us / 2u) {
		off = period_us - off;
	}
	zassert_true(off < RADIANT_CHANNEL_GUARD_MIN_US,
		     "the handed-off slot clock is %u us from the swept one",
		     (uint32_t)off);

	/* One period, not one sweep. */
	zassert_true(next_handed > radiant_radio_now());
	zassert_true(next_handed - radiant_radio_now() <= period_us,
		     "a handoff must put the first slot within one period");

	end_of_test();
}

ZTEST(handoff, test_a_handoff_acquires_faster_than_a_sweep_on_the_same_rig)
{
	struct profile_handoff h;
	struct profile_handoff received;
	uint8_t frames[PROFILE_HANDOFF_FRAMES * PROFILE_HANDOFF_FRAME_LEN];
	radiant_time_t t_begin;
	radiant_time_t t_first_slot_swept;
	radiant_time_t t_carrier;
	radiant_time_t t_first_slot_handed;
	radiant_time_t sweep_us;
	radiant_time_t handoff_us;
	uint32_t windows_swept;
	uint32_t arms_after_sweep;
	uint32_t arms_after_handoff;
	uint32_t windows_handed;

	bring_up();

	/* Both halves answer "how long from wanting this node to hearing it"
	 * on one rig, in one run, against one master. The sweep half is a real
	 * wildcard search walking address sets. */
	t_begin = radiant_radio_now();
	windows_swept = sweep_acquire();
	arms_after_sweep = fake_radio_stats()->arms_rx;
	t_first_slot_swept = radiant_channel_next_slot(SWEEP_CH);
	sweep_us = t_first_slot_swept - t_begin;

	/* The handoff half. The datum is assumed to be in hand, which is the
	 * plan's own case: persisted across a reboot, or handed over by a
	 * receiver that is already tracking - as here. */
	t_carrier = radiant_radio_now();
	zassert_ok(profile_handoff_from_channel(SWEEP_CH, t_carrier, 1u,
						PROFILE_HANDOFF_CLK_30PPM, &h));
	zassert_ok(profile_handoff_encode(&h, frames));
	zassert_ok(profile_handoff_decode(frames, &received));

	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_assign(HANDOFF_CH, TYPE_SLAVE, 0u, 0u));
	zassert_equal(RADIANT_CH_OK,
		      profile_handoff_apply(HANDOFF_CH, &received, t_carrier,
					    t_carrier));

	arms_after_handoff = fake_radio_stats()->arms_rx;
	windows_handed = arms_after_handoff - arms_after_sweep;
	t_first_slot_handed = radiant_channel_next_slot(HANDOFF_CH);
	handoff_us = t_first_slot_handed - t_carrier;

	TC_PRINT("sweep:   %u search windows, %u us to the first slot\n",
		 windows_swept, (uint32_t)sweep_us);
	TC_PRINT("handoff: %u search windows, %u us to the first slot\n",
		 windows_handed, (uint32_t)handoff_us);
	TC_PRINT("handoff is %u times faster and arms %u fewer windows\n",
		 (uint32_t)(sweep_us / (handoff_us ? handoff_us : 1u)),
		 windows_swept - windows_handed);

	/* THE SWEEP IS SKIPPED, NOT SHORTENED. Zero windows, and no virtual
	 * microsecond passed between asking and tracking. */
	zassert_equal(0u, windows_handed,
		      "a handoff must arm no search window at all");
	zassert_equal(t_carrier, radiant_radio_now(),
		      "a handoff must not have to wait for anything");
	zassert_true(windows_swept > 1u,
		     "the sweep half of this comparison did no sweeping");

	/* The gate itself. */
	zassert_true(handoff_us < sweep_us,
		     "handoff %u us is not faster than sweep %u us",
		     (uint32_t)handoff_us, (uint32_t)sweep_us);
	zassert_true(handoff_us <= radiant_channel_counts_to_us(NODE_PERIOD),
		     "a handoff acquires within one period; this took %u us",
		     (uint32_t)handoff_us);

	end_of_test();
}

ZTEST(handoff, test_only_a_tracking_channel_may_be_handed_over)
{
	struct profile_handoff h;

	bring_up();

	/* Unassigned. */
	zassert_equal(-ENOTCONN,
		      profile_handoff_from_channel(HANDOFF_CH,
						   radiant_radio_now(), 0u,
						   PROFILE_HANDOFF_CLK_UNKNOWN,
						   &h));

	/* Assigned and searching: no slot phase exists to give away yet. */
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_assign(SWEEP_CH, TYPE_SLAVE, 0u, 0u));
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_id_set(SWEEP_CH, 0u, 0u, 0u));
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_period_set(SWEEP_CH, NODE_PERIOD));
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_open(SWEEP_CH, 0u, radiant_radio_now()));
	zassert_equal(RADIANT_CH_STATE_SEARCHING,
		      radiant_channel_state_get(SWEEP_CH));
	zassert_equal(-ENOTCONN,
		      profile_handoff_from_channel(SWEEP_CH,
						   radiant_radio_now(), 0u,
						   PROFILE_HANDOFF_CLK_UNKNOWN,
						   &h));

	end_of_test();
}

ZTEST(handoff, test_a_handoff_is_refused_on_a_master)
{
	bring_up();

	/* A handoff describes a node to LISTEN to. Applying it to a master
	 * would silently reconfigure a transmitter, and the node it was meant
	 * for would still be unfound. */
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_assign(HANDOFF_CH, TYPE_MASTER, 0u, 0u));
	zassert_equal(RADIANT_CH_ERR_INVALID_PARAM,
		      profile_handoff_apply(HANDOFF_CH, &CANON,
					    radiant_radio_now(),
					    radiant_radio_now()));

	end_of_test();
}

ZTEST_SUITE(handoff, NULL, NULL, handoff_before, NULL, NULL);
