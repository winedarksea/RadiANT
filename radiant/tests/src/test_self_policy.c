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

#include <radiant/radiant_self_policy.h>

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
	zassert_true(fresh);
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

	zassert_equal(RADIANT_SELF_KEY_APPLY, key_poll(&st, 1u, 0u, false, false, &fresh));
	zassert_true(fresh);
	zassert_equal(RADIANT_SELF_KEY_NONE, key_poll(&st, 1u, 0u, false, false, &fresh));
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
	zassert_true(fresh);
}

ZTEST(self_policy, test_a_host_foreign_key_is_never_overwritten)
{
	struct radiant_self_key_state st;
	bool fresh;

	radiant_self_key_init(&st);
	(void)key_poll(&st, 0u, 0u, false, false, &fresh);

	zassert_equal(RADIANT_SELF_KEY_HOST_FOREIGN,
		      key_poll(&st, 1u, 1u, true, false, &fresh));
	zassert_equal(RADIANT_SELF_KEY_NONE, key_poll(&st, 1u, 1u, true, false, &fresh));
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
	zassert_false(fresh);
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

/* ---------------------------------------------------------------------------
 * A channel's next step (the Zwift model)
 * ---------------------------------------------------------------------------
 */

/* An owned channel in the given radio state; everything else false. */
static struct radiant_self_chan_in chan(enum radiant_self_radio radio)
{
	struct radiant_self_chan_in in = {
		.radio = radio,
		.owned = true,
	};

	return in;
}

ZTEST(self_policy, test_an_unassigned_channel_is_reestablished)
{
	struct radiant_self_chan_in in = chan(RADIANT_SELF_RADIO_UNASSIGNED);

	in.owned = false;
	in.bound = true;
	zassert_equal(RADIANT_SELF_REESTABLISH, radiant_self_decide(&in));
}

ZTEST(self_policy, test_a_channel_the_host_took_is_never_touched)
{
	struct radiant_self_chan_in in = chan(RADIANT_SELF_RADIO_TRACKING);

	in.owned = false;
	in.window_open = true;
	zassert_equal(RADIANT_SELF_NONE, radiant_self_decide(&in));
	in.bound = true;
	zassert_equal(RADIANT_SELF_RELEASE, radiant_self_decide(&in));
}

/* NEW-O6-1: a timeout or a dropout must never unbind a paired sensor. */
ZTEST(self_policy, test_a_pinned_channel_stays_pinned_through_any_dropout)
{
	static const enum radiant_self_radio open_states[] = {
		RADIANT_SELF_RADIO_SEARCHING, RADIANT_SELF_RADIO_TRACKING,
	};

	for (uint8_t w = 0u; w < 2u; w++) {
		for (uint8_t k = 0u; k < ARRAY_SIZE(open_states); k++) {
			struct radiant_self_chan_in in = chan(open_states[k]);

			in.bound = true;
			in.open_pinned = true;
			in.identity_matches = true;
			in.window_open = (w != 0u);
			zassert_equal(RADIANT_SELF_NONE, radiant_self_decide(&in),
				      "radio %d window %u", (int)open_states[k], w);
		}
	}
}

ZTEST(self_policy, test_a_closed_bound_channel_is_reopened_pinned)
{
	struct radiant_self_chan_in in = chan(RADIANT_SELF_RADIO_ASSIGNED);

	in.bound = true;
	zassert_equal(RADIANT_SELF_PIN_REOPEN, radiant_self_decide(&in));
	in.open_pinned = true;
	zassert_equal(RADIANT_SELF_PIN_REOPEN, radiant_self_decide(&in));
	in.window_open = true;
	in.open_discover = true;
	zassert_equal(RADIANT_SELF_PIN_REOPEN, radiant_self_decide(&in));
}

/* Pin by closing: on sdk-ant the close is asynchronous and the channel can
 * still read TRACKING a poll later. */
ZTEST(self_policy, test_a_bound_channel_still_on_its_discovery_search_is_closed_to_pin)
{
	struct radiant_self_chan_in in = chan(RADIANT_SELF_RADIO_TRACKING);

	in.bound = true;
	in.open_discover = true;
	in.identity_matches = true;
	in.window_open = true;
	zassert_equal(RADIANT_SELF_PIN_REOPEN, radiant_self_decide(&in));
}

ZTEST(self_policy, test_a_stranger_on_a_pinned_channel_is_repinned_not_unbound)
{
	struct radiant_self_chan_in in = chan(RADIANT_SELF_RADIO_TRACKING);

	in.bound = true;
	in.open_pinned = true;
	in.identity_matches = false;
	zassert_equal(RADIANT_SELF_PIN_REOPEN, radiant_self_decide(&in));
}

ZTEST(self_policy, test_a_closed_window_means_idle)
{
	struct radiant_self_chan_in in;

	in = chan(RADIANT_SELF_RADIO_ASSIGNED);
	zassert_equal(RADIANT_SELF_NONE, radiant_self_decide(&in));
	in.open_discover = true;
	zassert_equal(RADIANT_SELF_NONE, radiant_self_decide(&in));

	in = chan(RADIANT_SELF_RADIO_SEARCHING);
	in.open_discover = true;
	zassert_equal(RADIANT_SELF_CLOSE_TO_IDLE, radiant_self_decide(&in));

	in = chan(RADIANT_SELF_RADIO_TRACKING);
	in.open_discover = true;
	zassert_equal(RADIANT_SELF_CLOSE_TO_IDLE, radiant_self_decide(&in));
}

ZTEST(self_policy, test_an_open_window_discovers_and_binds)
{
	struct radiant_self_chan_in in;

	in = chan(RADIANT_SELF_RADIO_ASSIGNED);
	in.window_open = true;
	zassert_equal(RADIANT_SELF_DISCOVER_OPEN, radiant_self_decide(&in));

	in.open_discover = true;
	zassert_equal(RADIANT_SELF_ROTATE, radiant_self_decide(&in));

	in = chan(RADIANT_SELF_RADIO_SEARCHING);
	in.window_open = true;
	in.open_discover = true;
	zassert_equal(RADIANT_SELF_NONE, radiant_self_decide(&in));

	in.radio = RADIANT_SELF_RADIO_TRACKING;
	zassert_equal(RADIANT_SELF_BIND, radiant_self_decide(&in));
}

/* An eviction's close that has not landed: the search still open is for the
 * evicted sensor, and it must not bind it straight back. */
ZTEST(self_policy, test_an_evicted_channel_still_open_pinned_is_closed_first)
{
	struct radiant_self_chan_in in = chan(RADIANT_SELF_RADIO_TRACKING);

	in.open_pinned = true;
	in.window_open = true;
	zassert_equal(RADIANT_SELF_CLOSE_TO_IDLE, radiant_self_decide(&in));
}

/* ---------------------------------------------------------------------------
 * Eviction
 * ---------------------------------------------------------------------------
 */

ZTEST(self_policy, test_eviction_picks_the_longest_absent_sensor)
{
	struct radiant_self_evict_in c[3] = {
		{ .bound = true, .tracking = false, .last_tracking_ms = 50000u },
		{ .bound = true, .tracking = false, .last_tracking_ms = 10000u },
		{ .bound = true, .tracking = true,  .last_tracking_ms = 99000u },
	};

	zassert_equal(1u, radiant_self_pick_evict(c, 3u, true, false, 100000u));
}

ZTEST(self_policy, test_a_tracking_sensor_is_never_evicted)
{
	struct radiant_self_evict_in c[2] = {
		{ .bound = true, .tracking = true, .last_tracking_ms = 0u },
		{ .bound = true, .tracking = true, .last_tracking_ms = 0u },
	};

	zassert_equal(RADIANT_SELF_EVICT_NONE,
		      radiant_self_pick_evict(c, 2u, true, false, 100000u));
}

ZTEST(self_policy, test_no_eviction_without_need_or_window_or_twice)
{
	struct radiant_self_evict_in c[2] = {
		{ .bound = true,  .tracking = false, .last_tracking_ms = 0u },
		{ .bound = false, .tracking = false, .last_tracking_ms = 0u },
	};

	zassert_equal(RADIANT_SELF_EVICT_NONE,
		      radiant_self_pick_evict(c, 2u, true, false, 100000u));

	c[1].bound = true;
	zassert_equal(RADIANT_SELF_EVICT_NONE,
		      radiant_self_pick_evict(c, 2u, false, false, 100000u));
	zassert_equal(RADIANT_SELF_EVICT_NONE,
		      radiant_self_pick_evict(c, 2u, true, true, 100000u));
	zassert_equal(0u, radiant_self_pick_evict(c, 2u, true, false, 100000u));
}

ZTEST(self_policy, test_eviction_orders_across_an_uptime_wrap)
{
	struct radiant_self_evict_in c[2] = {
		{ .bound = true, .tracking = false, .last_tracking_ms = 0xFFFFF000u },
		{ .bound = true, .tracking = false, .last_tracking_ms = 0x00000800u },
	};

	zassert_equal(0u, radiant_self_pick_evict(c, 2u, true, false, 0x00001000u));
}

/* ---------------------------------------------------------------------------
 * Forced eviction (the long press)
 * ---------------------------------------------------------------------------
 */

ZTEST(self_policy, test_forced_eviction_takes_the_newest_bind_even_tracking)
{
	struct radiant_self_evict_in c[3] = {
		{ .bound = true, .tracking = true, .owned = true, .bind_seq = 4u },
		{ .bound = true, .tracking = true, .owned = true, .bind_seq = 9u },
		{ .bound = true, .tracking = false, .owned = true, .bind_seq = 2u },
	};

	/* The ordinary pick refuses a tracking sensor; this one does not. */
	zassert_equal(1u, radiant_self_pick_evict_forced(c, 3u));
}

ZTEST(self_policy, test_forced_eviction_never_takes_a_host_channel)
{
	struct radiant_self_evict_in c[2] = {
		/* The host took this number after a reset: bound as far as the
		 * caller's map says, with the newest sequence, and not ours. */
		{ .bound = true, .tracking = true, .owned = false, .bind_seq = 7u },
		{ .bound = true, .tracking = true, .owned = true,  .bind_seq = 3u },
	};

	zassert_equal(1u, radiant_self_pick_evict_forced(c, 2u));

	c[1].owned = false;
	zassert_equal(RADIANT_SELF_EVICT_NONE, radiant_self_pick_evict_forced(c, 2u));
}

ZTEST(self_policy, test_forced_eviction_picks_nothing_when_nothing_is_bound)
{
	struct radiant_self_evict_in c[2] = {
		{ .bound = false, .owned = true, .bind_seq = 0u },
		/* A bound flag with no recorded bind is not one of ours to
		 * order: nothing to pick. */
		{ .bound = true, .owned = true, .bind_seq = 0u },
	};

	zassert_equal(RADIANT_SELF_EVICT_NONE, radiant_self_pick_evict_forced(c, 2u));
	zassert_equal(RADIANT_SELF_EVICT_NONE, radiant_self_pick_evict_forced(c, 0u));
	zassert_equal(RADIANT_SELF_EVICT_NONE, radiant_self_pick_evict_forced(NULL, 2u));
}

ZTEST(self_policy, test_repeated_forced_evictions_walk_back_one_bind_at_a_time)
{
	struct radiant_self_evict_in c[3] = {
		{ .bound = true, .tracking = true, .owned = true, .bind_seq = 1u },
		{ .bound = true, .tracking = true, .owned = true, .bind_seq = 3u },
		{ .bound = true, .tracking = true, .owned = true, .bind_seq = 2u },
	};
	uint8_t pick;

	/* What the caller does with each pick: release_binding() clears both. */
	pick = radiant_self_pick_evict_forced(c, 3u);
	zassert_equal(1u, pick);
	c[pick].bound = false;
	c[pick].bind_seq = 0u;

	pick = radiant_self_pick_evict_forced(c, 3u);
	zassert_equal(2u, pick);
	c[pick].bound = false;
	c[pick].bind_seq = 0u;

	pick = radiant_self_pick_evict_forced(c, 3u);
	zassert_equal(0u, pick);
	c[pick].bound = false;
	c[pick].bind_seq = 0u;

	zassert_equal(RADIANT_SELF_EVICT_NONE, radiant_self_pick_evict_forced(c, 3u));
}

ZTEST(self_policy, test_forced_eviction_ignores_the_ordinary_picks_inputs)
{
	/* No window and every channel tracking: the ordinary pick says NONE.
	 * The forced pick takes no window and no absence time - the hold is
	 * the request. */
	struct radiant_self_evict_in c[2] = {
		{ .bound = true, .tracking = true, .last_tracking_ms = 0u,
		  .owned = true, .bind_seq = 5u },
		{ .bound = true, .tracking = true, .last_tracking_ms = 0u,
		  .owned = true, .bind_seq = 6u },
	};

	zassert_equal(RADIANT_SELF_EVICT_NONE,
		      radiant_self_pick_evict(c, 2u, false, false, 100000u));
	zassert_equal(1u, radiant_self_pick_evict_forced(c, 2u));
}
