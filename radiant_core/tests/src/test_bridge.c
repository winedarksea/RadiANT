/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work, against radiant_bridge.h/.c and
 * radiant_hr_adapter.h/.c, themselves transcriptions of docs/radiant-bridge.md
 * sections 3-4. Nothing here derives from sdk-ant.
 *
 * Tests for the sample bus, the sink registry, and the ANT+ HR adapter.
 * Covers: the ring drops oldest and counts it; want()==false never reaches
 * publish(); the drop counter self-publishes once per change, never as a
 * heartbeat; the HR adapter's first message posts no accumulator delta; the
 * 1/1024 s conversion is exact across a u8/u16 wrap (difference in 1/1024 s
 * before converting, per section 3.2 - convert-then-difference truncates).
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include "radiant_bridge.h"
#include "radiant_hr_adapter.h"

/* Registered for the life of the test binary (STRUCT_SECTION_ITERABLE is a
 * link-time array, so a sink cannot be created/destroyed per test);
 * test_reset() clears its capture state instead. */

#define CAP_MAX 40 /* > RADIANT_BRIDGE_RING_CAP (32) + one diagnostics sample */

static struct radiant_sample cap_buf[CAP_MAX];
static uint32_t          cap_n;
static bool               cap_want_reject; /* when true, want() declines everything */

static bool cap_want(const struct radiant_sample *s)
{
	ARG_UNUSED(s);
	return !cap_want_reject;
}

static void cap_publish(const struct radiant_sample *s)
{
	if (cap_n < CAP_MAX) {
		cap_buf[cap_n++] = *s;
	}
}

RADIANT_SINK_DEFINE(test_cap_sink, cap_want, cap_publish, NULL);

/* A second sink that always declines, proving want()==false never reaches
 * publish(): if it did, this sink's own publish() being NULL would crash. */
static bool never_want(const struct radiant_sample *s)
{
	ARG_UNUSED(s);
	return false;
}

RADIANT_SINK_DEFINE(test_deaf_sink, never_want, NULL, NULL);

static void test_reset(void *unused)
{
	ARG_UNUSED(unused);
	radiant_bridge_reset();
	memset(cap_buf, 0, sizeof(cap_buf));
	cap_n = 0u;
	cap_want_reject = false;
}

/* ---------------------------------------------------------------------------
 * The bus
 * ---------------------------------------------------------------------------
 */

static struct radiant_sample sample(uint32_t source, uint8_t field_id,
				 uint8_t field_type, int64_t raw)
{
	struct radiant_sample s = {
		.source = source,
		.field_id = field_id,
		.field_type = field_type,
		.flags = 0u,
		.exp = 0,
		.raw = raw,
		.t_us = 0u,
	};
	return s;
}

ZTEST(radiant_bridge, test_post_and_drain_delivers_in_order)
{
	struct radiant_sample a = sample(1u, 0u, RADIANT_FIELD_TEMPERATURE, 100);
	struct radiant_sample b = sample(1u, 1u, RADIANT_FIELD_HUMIDITY, 50);

	radiant_bridge_post(&a);
	radiant_bridge_post(&b);

	zassert_equal(radiant_bridge_drain(), 2u, "two samples queued");
	zassert_equal(cap_n, 2u, "one capturing sink, two deliveries");
	zassert_equal(cap_buf[0].raw, 100, "FIFO: oldest first");
	zassert_equal(cap_buf[1].raw, 50, "FIFO: newest second");
	zassert_equal(radiant_bridge_stats_get()->posted, 2u, NULL);
	zassert_equal(radiant_bridge_stats_get()->dropped, 0u, NULL);
}

ZTEST(radiant_bridge, test_want_false_never_reaches_publish)
{
	/* test_deaf_sink's publish is NULL, so if want()==false were ignored this
	 * would crash rather than merely fail an assertion. */
	struct radiant_sample a = sample(1u, 0u, RADIANT_FIELD_TEMPERATURE, 1);

	radiant_bridge_post(&a);
	radiant_bridge_drain();

	cap_want_reject = true;
	radiant_bridge_post(&a);
	zassert_equal(radiant_bridge_drain(), 1u,
		     "drain() counts what left the ring, not what a sink accepted");
	zassert_equal(cap_n, 1u, "second sample: want() declined it, publish() never ran");
}

ZTEST(radiant_bridge, test_ring_drops_oldest_and_counts)
{
	/* Fill the ring past capacity without draining, so the drop path runs
	 * before any sink sees anything - proving the ring itself enforces
	 * the bound rather than relying on a drain to keep up. */
	const uint32_t cap = 32u; /* RADIANT_BRIDGE_RING_CAP, mirrored: no
				   * public accessor, and the value is part of
				   * this test's contract with the module. */
	uint32_t   i;

	for (i = 0u; i < cap + 5u; i++) {
		struct radiant_sample s = sample(1u, 0u, RADIANT_FIELD_EVENT_COUNT,
						 (int64_t)i);
		radiant_bridge_post(&s);
	}

	zassert_equal(radiant_bridge_stats_get()->posted, cap + 5u, NULL);
	zassert_equal(radiant_bridge_stats_get()->dropped, 5u,
		     "five posts past capacity, five drops");

	radiant_bridge_drain();
	/* The five OLDEST (raw 0..4) were evicted; the ring now holds
	 * raw 5..(cap+4), oldest first. */
	zassert_equal(cap_buf[0].raw, 5, "oldest surviving record is the sixth posted");
	zassert_equal(cap_buf[cap - 1u].raw, (int64_t)(cap + 4u),
		     "newest surviving record is the last posted");
}

ZTEST(radiant_bridge, test_drop_counter_self_publishes_once_per_change)
{
	uint32_t i;
	bool     saw_diag = false;
	uint32_t diag_value = 0u;

	/* No drops yet: an idle-but-used bus must not manufacture a
	 * diagnostics sample. */
	struct radiant_sample s = sample(1u, 0u, RADIANT_FIELD_EVENT_COUNT, 1);

	radiant_bridge_post(&s);
	radiant_bridge_drain();
	for (i = 0u; i < cap_n; i++) {
		if (cap_buf[i].source == RADIANT_BRIDGE_DIAG_SOURCE) {
			saw_diag = true;
		}
	}
	zassert_false(saw_diag, "nothing dropped yet: no diagnostics sample expected");

	/* Force one drop, then drain: exactly one diagnostics sample, value 1. */
	cap_n = 0u;
	for (i = 0u; i < 33u; i++) { /* RADIANT_BRIDGE_RING_CAP + 1 */
		struct radiant_sample f = sample(1u, 0u, RADIANT_FIELD_EVENT_COUNT,
						 (int64_t)i);
		radiant_bridge_post(&f);
	}
	radiant_bridge_drain();

	saw_diag = false;
	for (i = 0u; i < cap_n; i++) {
		if (cap_buf[i].source == RADIANT_BRIDGE_DIAG_SOURCE) {
			zassert_false(saw_diag, "exactly one diagnostics sample per drain, not one per drop");
			saw_diag = true;
			diag_value = (uint32_t)cap_buf[i].raw;
		}
	}
	zassert_true(saw_diag, "one drop happened: a diagnostics sample must appear");
	zassert_equal(diag_value, 1u, "the diagnostics sample carries the running total");

	/* Drain again with nothing new dropped: no second diagnostics sample. */
	cap_n = 0u;
	radiant_bridge_post(&s);
	radiant_bridge_drain();
	for (i = 0u; i < cap_n; i++) {
		zassert_not_equal(cap_buf[i].source, RADIANT_BRIDGE_DIAG_SOURCE,
			       "drop count unchanged: no repeat diagnostics sample");
	}
}

/* ---------------------------------------------------------------------------
 * The ANT+ HR adapter
 * ---------------------------------------------------------------------------
 */

static void body(uint8_t out[8], uint8_t page, uint16_t event_time,
		 uint8_t beat_count, uint8_t computed_hr)
{
	out[0] = page;
	out[1] = 0u;
	out[2] = 0u;
	out[3] = 0u;
	out[4] = (uint8_t)(event_time & 0xFFu);
	out[5] = (uint8_t)(event_time >> 8);
	out[6] = beat_count;
	out[7] = computed_hr;
}

ZTEST(radiant_bridge, test_hr_adapter_first_message_posts_only_instantaneous)
{
	struct radiant_hr_adapter a;
	uint8_t              b[8];
	uint32_t             n;

	radiant_hr_adapter_init(&a);
	body(b, 0x00u, 1000u, 50u, 72u);

	n = radiant_hr_adapter_decode(&a, 7u, b, 12345u);
	zassert_equal(n, 1u, "first message: nothing to difference against yet");

	radiant_bridge_drain();
	zassert_equal(cap_n, 1u, NULL);
	zassert_equal(cap_buf[0].field_type, RADIANT_FIELD_HEART_RATE, NULL);
	zassert_equal(cap_buf[0].field_id, RADIANT_HR_FIELD_COMPUTED_BPM, NULL);
	zassert_equal(cap_buf[0].raw, 72, NULL);
	zassert_equal(cap_buf[0].source, 7u, "source is threaded through unchanged");
}

ZTEST(radiant_bridge, test_hr_adapter_second_message_posts_all_three)
{
	struct radiant_hr_adapter a;
	uint8_t              b[8];

	radiant_hr_adapter_init(&a);
	body(b, 0x00u, 1000u, 50u, 72u);
	radiant_hr_adapter_decode(&a, 1u, b, 0u);
	radiant_bridge_drain();
	cap_n = 0u;

	/* One beat, 1024/1024 s later - exactly one second. */
	body(b, 0x00u, 2024u, 51u, 73u);
	radiant_hr_adapter_decode(&a, 1u, b, 1000000u);
	radiant_bridge_drain();

	zassert_equal(cap_n, 3u, "second message: instantaneous + two accumulators");

	zassert_equal(cap_buf[0].field_id, RADIANT_HR_FIELD_COMPUTED_BPM, NULL);
	zassert_equal(cap_buf[0].raw, 73, NULL);

	zassert_equal(cap_buf[1].field_id, RADIANT_HR_FIELD_BEAT_COUNT, NULL);
	zassert_equal(cap_buf[1].field_type, RADIANT_FIELD_EVENT_COUNT, NULL);
	zassert_true((cap_buf[1].flags & RADIANT_SAMPLE_ACCUMULATING) != 0u, NULL);
	zassert_equal(cap_buf[1].raw, 1, "one beat since the first message");

	zassert_equal(cap_buf[2].field_id, RADIANT_HR_FIELD_BEAT_TIME_MS, NULL);
	zassert_equal(cap_buf[2].field_type, RADIANT_FIELD_DURATION, NULL);
	zassert_equal(cap_buf[2].exp, -3, "milliseconds");
	zassert_equal(cap_buf[2].raw, 1000, "1024/1024 s = exactly 1000 ms");
}

ZTEST(radiant_bridge, test_hr_adapter_beat_count_wraps_at_u8)
{
	struct radiant_hr_adapter a;
	uint8_t              b[8];

	radiant_hr_adapter_init(&a);
	body(b, 0x00u, 0u, 254u, 72u);
	radiant_hr_adapter_decode(&a, 1u, b, 0u);
	radiant_bridge_drain();
	cap_n = 0u;

	/* 254 -> 255 -> 0 -> 1 -> 2 is four beats, not a negative or
	 * wrapped-nonsense delta. */
	body(b, 0x00u, 100u, 2u, 72u);
	radiant_hr_adapter_decode(&a, 1u, b, 1000u);
	radiant_bridge_drain();

	zassert_equal(cap_buf[1].raw, 4, "254 -> 2 across a u8 wrap is four beats");
}

ZTEST(radiant_bridge, test_hr_adapter_event_time_conversion_exact_across_wrap)
{
	struct radiant_hr_adapter a;
	uint8_t              b[8];
	int                   i;

	/*
	 * Sixty-five one-beat steps of 1000 (1/1024 s), crossing the u16
	 * event-time wrap (65536) partway through. Converting-then-differencing
	 * (the wrong construction) would drift; differencing in 1/1024 s and
	 * converting once per publish cannot drift beyond the final rounding.
	 * Exact total: 65000/1024 s = 63476.5625 ms -> truncates to 63476.
	 */
	radiant_hr_adapter_init(&a);
	body(b, 0x00u, 0u, 0u, 72u);
	radiant_hr_adapter_decode(&a, 1u, b, 0u);
	radiant_bridge_drain();

	for (i = 1; i <= 65; i++) {
		uint16_t et = (uint16_t)((uint32_t)(i * 1000) % 65536u);

		cap_n = 0u;
		body(b, 0x00u, et, (uint8_t)i, 72u);
		radiant_hr_adapter_decode(&a, 1u, b, (uint64_t)i * 1000000u);
		radiant_bridge_drain();
	}

	/* The LAST publish carries the exact running total. */
	zassert_equal(cap_buf[2].field_id, RADIANT_HR_FIELD_BEAT_TIME_MS, NULL);
	zassert_equal(cap_buf[2].raw, 63476,
		     "65000/1024 s truncated to ms, exact after a u16 wrap");
}

static void *bridge_setup(void)
{
	return NULL;
}

ZTEST_SUITE(radiant_bridge, NULL, bridge_setup, test_reset, NULL, NULL);
