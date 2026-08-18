/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work, against radiant_liveness.h/.c, which
 * is itself a transcription of docs/radiant-bridge.md section 9.2's expiry
 * rule.
 *
 * Eight things earn their place here, and each of them is a way the module
 * could be wrong while looking right:
 *
 *   - It must not fire early. 3x the period, not 1x - a strap that misses two
 *     broadcasts in a row is normal on a 0.4 % loss floor.
 *   - It must fire ONCE. An un-edged implementation posts a STALE record every
 *     tick, which is a 1 Hz stream of traffic from a source that has stopped
 *     transmitting - the opposite of what the flag means.
 *   - A fresh sample must re-arm it, or a strap that comes back into range is
 *     never reported stale again.
 *   - A binding with no period must never expire. This is the case the header
 *     argues about at length: guessing here is worse than abstaining.
 *   - The posted record must carry the LAST VALUE, not a zeroed one. Section
 *     9.2's expiry is "this reading is old", and a sink renders the reading.
 *   - Unbinding must free the row, or the next device into that table slot
 *     inherits a stranger's last reading.
 *   - A field_id past the last slot must be refused and counted, not allowed
 *     to evict a tracked field.
 *   - A PAGE ROTATION MUST NOT READ AS A SENSOR GOING AWAY. This one is here
 *     because the module got it wrong on real hardware and no test noticed:
 *     the quiet that matters belongs to the source, and a field that only
 *     rides every fourth page is not evidence of anything.
 *
 * radiant_liveness_tick() takes its clock as an argument, so all of this runs
 * in a few microseconds of wall time and none of it needs a real timer.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include "radiant_binding.h"
#include "radiant_bridge.h"
#include "radiant_liveness.h"

#define CAP_MAX 16

static struct radiant_sample cap_buf[CAP_MAX];
static uint32_t          cap_n;

static bool live_cap_want(const struct radiant_sample *s)
{
	/* Only what the liveness module produced, so a test does not have to
	 * pick its own injected samples back out of the capture. */
	return (s->flags & RADIANT_SAMPLE_STALE) != 0u;
}

static void live_cap_publish(const struct radiant_sample *s)
{
	if (cap_n < CAP_MAX) {
		cap_buf[cap_n++] = *s;
	}
}

RADIANT_SINK_DEFINE(test_liveness_cap_sink, live_cap_want, live_cap_publish, NULL);

static void liveness_test_reset(void *unused)
{
	ARG_UNUSED(unused);
	radiant_bridge_reset();
	radiant_binding_init();
	radiant_liveness_reset();
	memset(cap_buf, 0, sizeof(cap_buf));
	cap_n = 0u;
}

#define US_PER_S ((uint64_t)1000000u)

/*
 * 8192 counts of 32768 Hz is 250 ms - the 4 Hz every ANT+ heart-rate profile
 * uses, and the value antr_channel_period_set()'s own contract quotes. So the
 * expiry under test is 750 ms, and every timestamp below is chosen against
 * that rather than against a round second.
 */
#define PERIOD_4HZ 8192u
#define EXPIRY_US  ((uint64_t)750000u)

/* Bind, and give the binding the period the way the real binder does: read off
 * the channel at bind time, not learned from the traffic. */
static uint32_t bound_source(uint16_t devnum, uint16_t period)
{
	uint32_t src = RADIANT_BINDING_NONE;

	zassert_equal(radiant_binding_bind(devnum, 0x78u, 1u, NULL, &src), 0, NULL);
	if (period != 0u) {
		zassert_equal(radiant_binding_set_period(src, period), 0, NULL);
	}
	return src;
}

static void post_hr(uint32_t source, uint8_t field_id, int64_t bpm, uint64_t t_us)
{
	struct radiant_sample s = {
		.source = source,
		.field_id = field_id,
		.field_type = RADIANT_FIELD_HEART_RATE,
		.flags = 0u,
		.exp = 0,
		.raw = bpm,
		.t_us = t_us,
	};

	radiant_bridge_post(&s);
	radiant_bridge_drain();
}

/* Run the sweep and deliver whatever it posted. The two steps are separate in
 * production too - tick() posts, the pump drains - so a test that folded them
 * would be testing a shape the firmware does not have. */
static uint32_t tick(uint64_t now_us)
{
	uint32_t n = radiant_liveness_tick(now_us);

	radiant_bridge_drain();
	return n;
}

ZTEST(radiant_liveness, test_does_not_expire_before_three_periods)
{
	uint32_t src = bound_source(1u, PERIOD_4HZ);

	post_hr(src, 0u, 70, 0u);

	zassert_equal(tick(EXPIRY_US - 1u), 0u,
		      "one microsecond under 3x the period: must not expire");
	zassert_equal(cap_n, 0u, NULL);

	/* Exactly at the threshold is still not past it. */
	zassert_equal(tick(EXPIRY_US), 0u,
		      "at 3x the period exactly: must not expire yet");
	zassert_equal(cap_n, 0u, NULL);
}

ZTEST(radiant_liveness, test_expires_once_and_carries_the_last_value)
{
	uint32_t src = bound_source(1u, PERIOD_4HZ);

	post_hr(src, 0u, 70, 0u);
	post_hr(src, 0u, 138, 200000u); /* the value that must survive */

	zassert_equal(tick(200000u + EXPIRY_US + 1u), 1u,
		      "past 3x the period: must expire");
	zassert_equal(cap_n, 1u, NULL);
	zassert_equal(cap_buf[0].source, src, NULL);
	zassert_equal(cap_buf[0].field_id, 0u, NULL);
	zassert_equal(cap_buf[0].field_type, RADIANT_FIELD_HEART_RATE, NULL);
	zassert_equal(cap_buf[0].raw, 138,
		      "the expiry must carry the last reading, not a zero");
	zassert_true((cap_buf[0].flags & RADIANT_SAMPLE_STALE) != 0u, NULL);
	/* Stamped when the expiry was decided, not when the value was heard -
	 * otherwise a sink ordering its writes by t_us puts the expiry before
	 * the reading it expires. */
	zassert_equal(cap_buf[0].t_us, 200000u + EXPIRY_US + 1u, NULL);

	/* Edge-triggered: still quiet, still stale, but nothing more on the
	 * bus. An un-edged implementation would emit one record per tick from
	 * a source that has stopped transmitting. */
	zassert_equal(tick(10u * US_PER_S), 0u, NULL);
	zassert_equal(tick(20u * US_PER_S), 0u, NULL);
	zassert_equal(cap_n, 1u, "exactly one STALE record per quiet period");
}

ZTEST(radiant_liveness, test_a_fresh_sample_rearms_the_edge)
{
	uint32_t src = bound_source(1u, PERIOD_4HZ);

	post_hr(src, 0u, 70, 0u);
	zassert_equal(tick(EXPIRY_US + 1u), 1u, NULL);
	zassert_equal(cap_n, 1u, NULL);

	/* Back in range. */
	post_hr(src, 0u, 72, 5u * US_PER_S);
	zassert_equal(tick(5u * US_PER_S + EXPIRY_US - 1u), 0u,
		      "freshly heard: must not still read stale");
	zassert_equal(cap_n, 1u, NULL);

	/* And gone again - which must be reported, not swallowed because it
	 * was reported once already. */
	zassert_equal(tick(5u * US_PER_S + EXPIRY_US + 1u), 1u, NULL);
	zassert_equal(cap_n, 2u, NULL);
	zassert_equal(cap_buf[1].raw, 72, NULL);
}

ZTEST(radiant_liveness, test_a_binding_with_no_period_never_expires)
{
	uint32_t src = bound_source(1u, 0u); /* deliberately never set */
	const struct radiant_liveness_stats *st;

	post_hr(src, 0u, 70, 0u);

	zassert_equal(tick(60u * US_PER_S), 0u,
		      "no period: no honest expiry exists, so none is invented");
	zassert_equal(cap_n, 0u, NULL);

	st = radiant_liveness_stats_get();
	zassert_true(st->no_period > 0u,
		     "abstaining must be counted, or it is indistinguishable "
		     "from working");
}

ZTEST(radiant_liveness, test_period_scales_the_threshold)
{
	/* 32768 counts is 1 s, so the expiry is 3 s - four times the 4 Hz
	 * case, which is the property that would be lost if the threshold were
	 * a constant with a period-shaped name. */
	uint32_t src = bound_source(1u, 32768u);

	post_hr(src, 0u, 70, 0u);
	zassert_equal(tick(3u * US_PER_S), 0u, "at 3 s exactly: not yet");
	zassert_equal(tick(3u * US_PER_S + 1u), 1u, "past 3 s: expired");
}

ZTEST(radiant_liveness, test_unbinding_frees_the_row)
{
	uint32_t src = bound_source(1u, PERIOD_4HZ);
	uint32_t rebound;

	post_hr(src, 0u, 138, 0u);
	zassert_equal(radiant_binding_unbind(src), 0, NULL);

	/* No binding, so nothing to expire against and nothing posted. */
	zassert_equal(tick(60u * US_PER_S), 0u, NULL);
	zassert_equal(cap_n, 0u, NULL);

	/* A different device lands in the same table slot. It must not inherit
	 * the previous occupant's 138 bpm. */
	rebound = bound_source(2u, PERIOD_4HZ);
	zassert_equal(rebound, src, "same slot, which is what makes this a test");
	zassert_equal(tick(120u * US_PER_S), 0u,
		      "a fresh binding that has never published has nothing to "
		      "expire");
	zassert_equal(cap_n, 0u, NULL);
}

ZTEST(radiant_liveness, test_an_extra_field_is_refused_not_evicted)
{
	uint32_t src = bound_source(1u, PERIOD_4HZ);
	const struct radiant_liveness_stats *st;
	uint8_t f;

	for (f = 0u; f < (uint8_t)RADIANT_LIVENESS_FIELDS_PER_SOURCE; f++) {
		post_hr(src, f, 100 + f, 0u);
	}
	post_hr(src, (uint8_t)RADIANT_LIVENESS_FIELDS_PER_SOURCE, 200, 0u);

	st = radiant_liveness_stats_get();
	zassert_equal(st->tracked, RADIANT_LIVENESS_FIELDS_PER_SOURCE, NULL);
	zassert_equal(st->no_slot, 1u, "the field past the last slot is refused and counted");

	/* The four that were accepted are all still watched - the refusal must
	 * not have cost one of them its slot. */
	zassert_equal(tick(EXPIRY_US + 1u), RADIANT_LIVENESS_FIELDS_PER_SOURCE,
		      NULL);
	zassert_equal(cap_n, RADIANT_LIVENESS_FIELDS_PER_SOURCE, NULL);
}

ZTEST(radiant_liveness, test_a_page_rotation_is_not_a_sensor_going_away)
{
	/*
	 * THE REGRESSION. An FE-C trainer carries speed and FE state on page 16,
	 * power and event count on page 25, energy on 26 and equipment type on
	 * 54, so on a 4 Hz channel any ONE field arrives roughly once a second
	 * while the expiry is 750 ms. Measured on a real Wahoo #52233 before the
	 * fix: 165 STALE records in 150 s from a trainer sitting on the bench
	 * transmitting perfectly, each field re-armed by its own next page and
	 * expiring again before the one after it - and because mqtt_sink.c
	 * drives the per-binding availability topic from STALE, the whole device
	 * flapped offline and back once a second in Home Assistant.
	 *
	 * Two fields, strictly alternating at 1 s, for 10 s. Every individual
	 * field is silent for 2 s at a stretch, far past the 750 ms threshold.
	 * Nothing may expire: the source is demonstrably on air the whole time.
	 */
	uint32_t src = bound_source(1u, PERIOD_4HZ);
	uint64_t t;

	for (t = 0u; t <= 10u * US_PER_S; t += US_PER_S) {
		post_hr(src, (uint8_t)((t / US_PER_S) % 2u), 70, t);
		zassert_equal(tick(t + (US_PER_S / 2u)), 0u,
			      "a rotating page is still traffic: the sensor has "
			      "not gone anywhere");
	}
	zassert_equal(cap_n, 0u, "not one STALE record while it is transmitting");

	/*
	 * And the guarantee the module exists for is unchanged: when the whole
	 * source really does stop, BOTH fields are reported, on the same 3x
	 * period the header promises - not just the one that happened to be
	 * carried by the last page received.
	 */
	zassert_equal(tick(10u * US_PER_S + EXPIRY_US + 1u), 2u,
		      "every tracked field of a source that went quiet");
	zassert_equal(cap_n, 2u, NULL);
}

ZTEST(radiant_liveness, test_its_own_output_does_not_rearm_it)
{
	/*
	 * The STALE record goes back round the bus and reaches this module's
	 * own want(). If it were accepted as a fresh arrival the edge would be
	 * re-armed by the edge, and the source would produce one STALE record
	 * every 3x period forever. Two ticks well past a second expiry window
	 * prove it does not.
	 */
	uint32_t src = bound_source(1u, PERIOD_4HZ);

	post_hr(src, 0u, 70, 0u);
	zassert_equal(tick(EXPIRY_US + 1u), 1u, NULL);
	zassert_equal(tick(2u * (EXPIRY_US + 1u)), 0u, NULL);
	zassert_equal(tick(4u * (EXPIRY_US + 1u)), 0u, NULL);
	zassert_equal(cap_n, 1u, NULL);
}

static void *liveness_setup(void)
{
	return NULL;
}

ZTEST_SUITE(radiant_liveness, NULL, liveness_setup, liveness_test_reset, NULL, NULL);

