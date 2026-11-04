/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original work. Written against radiant/src/bridge/
 * radiant_self_policy.h. Nothing here derives from sdk-ant, libant.a, or any
 * ANT+ device profile document.
 *
 * The decisions apps/dongle_thread/src/self_channels.c takes once a second,
 * reached here because that file cannot be: radiant/tests links radiant and
 * nothing under apps/.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/ztest.h>

#include "radiant_self_policy.h"

ZTEST_SUITE(self_policy, NULL, NULL, NULL, NULL, NULL);

/* ---------------------------------------------------------------------------
 * The network key after a host reset
 * ---------------------------------------------------------------------------
 */

static enum radiant_self_key_action key_poll(struct radiant_self_key_state *st,
					     uint32_t reset_gen, uint32_t key_writes,
					     bool written, bool ours, bool *fresh)
{
	struct radiant_self_key_in in = {
		.reset_gen = reset_gen,
		.key_writes = key_writes,
		.key_written = written,
		.key_is_ours = ours,
	};

	return radiant_self_key_decide(st, &in, fresh);
}

ZTEST(self_policy, test_boot_applies_the_key_once)
{
	struct radiant_self_key_state st;
	bool fresh;

	radiant_self_key_init(&st);
	zassert_equal(RADIANT_SELF_KEY_APPLY, key_poll(&st, 0u, 0u, false, false, &fresh));
	zassert_true(fresh, "boot is a generation");
	zassert_equal(RADIANT_SELF_KEY_NONE, key_poll(&st, 0u, 0u, false, false, &fresh));
	zassert_false(fresh);
}

/* Zwift: SYSTEM_RESET at startup, then (a moment later) the ANT+ key. */
ZTEST(self_policy, test_a_host_reset_with_no_key_reapplies_ours_once)
{
	struct radiant_self_key_state st;
	bool fresh;

	radiant_self_key_init(&st);
	(void)key_poll(&st, 0u, 0u, false, false, &fresh);

	zassert_equal(RADIANT_SELF_KEY_APPLY, key_poll(&st, 1u, 0u, false, false, &fresh),
		      "the reset wiped our key and the host wrote none");
	zassert_true(fresh, "the caller must learn every channel was wiped");
	zassert_equal(RADIANT_SELF_KEY_NONE, key_poll(&st, 1u, 0u, false, false, &fresh),
		      "one action per generation");
	zassert_false(fresh);

	/* The host then writes the ANT+ key over ours: nothing to say. */
	zassert_equal(RADIANT_SELF_KEY_NONE, key_poll(&st, 1u, 1u, true, true, &fresh));
}

ZTEST(self_policy, test_a_host_that_wrote_the_ant_plus_key_is_left_alone)
{
	struct radiant_self_key_state st;
	bool fresh;

	radiant_self_key_init(&st);
	(void)key_poll(&st, 0u, 0u, false, false, &fresh);
	zassert_equal(RADIANT_SELF_KEY_NONE, key_poll(&st, 1u, 1u, true, true, &fresh));
	zassert_true(fresh, "the channels still need re-establishing");
}

ZTEST(self_policy, test_a_host_foreign_key_is_never_overwritten)
{
	struct radiant_self_key_state st;
	bool fresh;

	radiant_self_key_init(&st);
	(void)key_poll(&st, 0u, 0u, false, false, &fresh);

	zassert_equal(RADIANT_SELF_KEY_HOST_FOREIGN,
		      key_poll(&st, 1u, 1u, true, false, &fresh),
		      "the host's own key must be reported, not replaced");
	zassert_equal(RADIANT_SELF_KEY_NONE, key_poll(&st, 1u, 1u, true, false, &fresh),
		      "said once, not once a second");
}

ZTEST(self_policy, test_a_foreign_key_written_after_ours_wins_and_is_reported)
{
	struct radiant_self_key_state st;
	bool fresh;

	radiant_self_key_init(&st);
	(void)key_poll(&st, 0u, 0u, false, false, &fresh);
	zassert_equal(RADIANT_SELF_KEY_APPLY, key_poll(&st, 1u, 0u, false, false, &fresh));

	zassert_equal(RADIANT_SELF_KEY_HOST_FOREIGN,
		      key_poll(&st, 1u, 1u, true, false, &fresh));
	zassert_false(fresh, "a key write is not a reset");
	zassert_equal(RADIANT_SELF_KEY_NONE, key_poll(&st, 1u, 1u, true, false, &fresh));
}

ZTEST(self_policy, test_a_host_key_before_the_thread_starts_is_respected_at_boot)
{
	struct radiant_self_key_state st;
	bool fresh;

	radiant_self_key_init(&st);
	zassert_equal(RADIANT_SELF_KEY_HOST_FOREIGN,
		      key_poll(&st, 0u, 1u, true, false, &fresh));
	zassert_true(fresh);
}
