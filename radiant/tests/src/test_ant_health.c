/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The 0xF6 reply's optional coverage field.
 *
 * ant_health.c is the host's copy of the reply and lives in apps/common, so it
 * is pulled in here by relative include with the health symbol forced on and
 * the sweep's getter replaced. The thing under test is fill(): the body
 * length, the field, and above all that saturating this field does not set
 * the flag in [1] - one long-running counter would otherwise mark every other
 * field on the board as a floor, and that is the bug that kept the idle sweep
 * field out of this reply.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#if defined(CONFIG_RADIANT_SEARCH_COVERAGE)

#define CONFIG_ANT_DONGLE_HEALTH_COUNTERS 1

#define radiant_search_sets_uncovered_get test_sets_uncovered_get
static uint32_t test_uncovered;
uint32_t test_sets_uncovered_get(void);

/* The file under test, built into this translation unit so its statics are
 * this suite's own and the getter above is the one it calls. */
#include "../../../apps/common/ant_health.c"

uint32_t test_sets_uncovered_get(void)
{
	return test_uncovered;
}

static void health_before(void *f)
{
	ARG_UNUSED(f);
	test_uncovered = 0u;
	atomic_set(&saturated, 0);
}

ZTEST_SUITE(ant_health_coverage, NULL, NULL, health_before, NULL, NULL);

ZTEST(ant_health_coverage, test_a_coverage_build_answers_32_bytes)
{
	uint8_t out[38];

	test_uncovered = 0x1234u;

	zassert_equal(32u, ant_health_fill(out, sizeof(out)));
	zassert_equal(1u, out[0], "the structure version stays 1");
	zassert_equal(0x34u, out[30]);
	zassert_equal(0x12u, out[31]);
}

ZTEST(ant_health_coverage, test_saturating_the_coverage_field_does_not_set_the_flag)
{
	uint8_t out[38];

	test_uncovered = 70000u;

	zassert_equal(32u, ant_health_fill(out, sizeof(out)));
	zassert_equal(0xFFu, out[30]);
	zassert_equal(0xFFu, out[31], "the field clamps at 65535");
	zassert_equal(0u, out[1] & 0x01u,
		      "a long-running field set the flag every other field reads");
}

/* cap has to fit this build's body, and an undersized cap is refused rather
 * than a truncated reply being written. */
ZTEST(ant_health_coverage, test_a_buffer_too_small_for_this_build_is_refused)
{
	uint8_t out[38];

	zassert_equal(0u, ant_health_fill(out, 31u),
		      "31 bytes cannot hold a coverage build's body");
	zassert_equal(32u, ant_health_fill(out, 32u));
}

#endif /* CONFIG_RADIANT_SEARCH_COVERAGE */
