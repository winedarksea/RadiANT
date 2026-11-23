/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_freq.c - adaptive frequency (RF-7).
 *
 * Provenance: docs/decisions/0012-adaptive-frequency.md and
 * docs/decisions/0005-extension-inside-ant-plus.md axis 5, both this project's
 * own documents. No ANT+ device profile document was read for
 * this file. See docs/decisions/0002-clean-room-policy.md.
 *
 * What this suite stands in for: the phase's bench gate ("loss ~ 0 on the
 * selected quiet channel, single copy per event, against ~0.4% on 57") needs
 * hardware that is unavailable and is deferred to the combined session. This
 * suite does NOT substitute for that measurement - no dB or loss figure
 * appears anywhere in this file. It checks the claims that are about
 * software, chiefly two that a bench would find last:
 *
 *   THE COUNTDOWN TERMINATES - the re-anchor moves the node's target to
 *   whatever it most recently announced. Applied one announcement too many,
 *   it moves the target past the move itself and the node never leaves,
 *   which looks like "adaptive frequency does not work" on a bench and like
 *   nothing at all in a code review.
 *
 *   NODE AND RECEIVER LAND ON THE SAME SLOT, including a receiver that
 *   joined mid-countdown or lost most of it - an agreement that fails
 *   silently: the two do not fail, they stop meeting.
 *
 * The scheduler half of the phase (an off-57 window never joining the merged
 * one) is asserted in test_sched.c next to the merge tests it is the
 * negative case for, rather than here where it would need the whole
 * mock-radio harness a second time.
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

#include <radiant/radiant_chanmap.h>
#include <radiant/radiant_frame.h>
#include <radiant/radiant_radio_hal.h>
#include <radiant/radiant_search.h>

#include <radiant/radiant_profile_freq.h>
#include <radiant/radiant_profile_telemetry.h>

/*
 * The canonical announcement, shared BYTE FOR BYTE with tools/test_ant_pages.py.
 * Two implementations checked only against each other are not checked at all,
 * and this page in particular tells a receiver where to point its radio.
 *
 * Target RF 26 (2426 MHz), countdown 8 units = 64 transmitted messages.
 */
static const uint8_t CANON_FREQ[RADIANT_PROFILE_FREQ_LEN] = {
	0x13u, 0x00u, 0x1Au, 0x08u, 0x00u, 0x00u, 0x00u, 0x00u,
};

/* Enough dwells to clear RADIANT_CHANMAP_MIN_DWELLS, at one figure. */
static void map_note(uint8_t rf, int8_t dbm)
{
	uint8_t i;

	for (i = 0u; i < (uint8_t)(RADIANT_CHANMAP_MIN_DWELLS + 1u); i++) {
		radiant_chanmap_note(rf, dbm, dbm, 16u);
	}
}

static void freq_before(void *f)
{
	ARG_UNUSED(f);
	radiant_chanmap_reset();
}

ZTEST_SUITE(radiant_profile_freq, NULL, NULL, freq_before, NULL, NULL);

/* ---------------------------------------------------------------------------
 * The candidate set
 * ---------------------------------------------------------------------------
 */

/*
 * The three defaults are BLE's advertising channels, and the assertion is the
 * megahertz rather than the indices: 2/26/80 is only meaningful as 2402, 2426
 * and 2480 MHz, which is where they sit relative to Wi-Fi 1, 6 and 11. An index
 * typo would still be three plausible small numbers.
 */
ZTEST(radiant_profile_freq, test_the_candidate_set_is_bles_advertising_placement)
{
	zassert_equal(2402u, radiant_rf_index_to_mhz(radiant_profile_freq_defaults[0]));
	zassert_equal(2426u, radiant_rf_index_to_mhz(radiant_profile_freq_defaults[1]));
	zassert_equal(2480u, radiant_rf_index_to_mhz(radiant_profile_freq_defaults[2]));

	/* Ascending, in range, and none of them is home - a candidate list
	 * containing the incumbent would make "move" and "stay" the same
	 * answer. */
	zassert_true(radiant_profile_freq_defaults[0] < radiant_profile_freq_defaults[1]);
	zassert_true(radiant_profile_freq_defaults[1] < radiant_profile_freq_defaults[2]);
	zassert_true(radiant_profile_freq_defaults[2] <= RADIANT_RF_INDEX_MAX);
	for (uint8_t i = 0u; i < RADIANT_PROFILE_FREQ_N_DEFAULTS; i++) {
		zassert_not_equal(RADIANT_PROFILE_FREQ_RF_HOME, radiant_profile_freq_defaults[i]);
	}

	/* ANT+ is 2457 MHz, which is the number the whole phase exists to
	 * leave. Stated here so the two are in one place. */
	zassert_equal(2457u, radiant_rf_index_to_mhz(RADIANT_PROFILE_FREQ_RF_HOME));
}

/* ---------------------------------------------------------------------------
 * Selection
 * ---------------------------------------------------------------------------
 */

/*
 * A quiet candidate is not evidence that here is loud. Without a measurement of
 * the incumbent there is no margin to clear, and a node that moved anyway would
 * be moving on half a comparison - which on a bench looks like adaptive
 * frequency choosing at random.
 */
ZTEST(radiant_profile_freq, test_selection_refuses_without_evidence_for_where_we_are)
{
	struct radiant_profile_freq_evidence ev[] = {
		{ RADIANT_PROFILE_FREQ_RF_HOME, false, 0, 0 },
		{ RADIANT_PROFILE_FREQ_RF_MID, true, -100, -104 },
	};
	uint8_t out = 0xFFu;

	zassert_equal(-ENODATA, radiant_profile_freq_select(RADIANT_PROFILE_FREQ_RF_HOME, ev,
						    ARRAY_SIZE(ev), &out));
	zassert_equal(0xFFu, out);
}

ZTEST(radiant_profile_freq, test_selection_picks_the_quietest_candidate_past_the_margin)
{
	struct radiant_profile_freq_evidence ev[] = {
		{ RADIANT_PROFILE_FREQ_RF_HOME, true, -70, -100 },
		{ RADIANT_PROFILE_FREQ_RF_LOW, true, -84, -102 },
		{ RADIANT_PROFILE_FREQ_RF_MID, true, -96, -104 },
		{ RADIANT_PROFILE_FREQ_RF_HIGH, true, -88, -103 },
	};
	uint8_t out = 0u;

	zassert_ok(radiant_profile_freq_select(RADIANT_PROFILE_FREQ_RF_HOME, ev, ARRAY_SIZE(ev),
				       &out));
	zassert_equal(RADIANT_PROFILE_FREQ_RF_MID, out);
}

/*
 * The anti-churn rule. Two indices within a few dB of each other trade places as
 * the room changes, and a node that re-announced every time would be hopping
 * slowly rather than adapting - which is the thing this phase is defined against
 * rather than a tuning preference.
 */
ZTEST(radiant_profile_freq, test_a_candidate_inside_the_margin_is_not_worth_moving_to)
{
	struct radiant_profile_freq_evidence ev[] = {
		{ RADIANT_PROFILE_FREQ_RF_HOME, true, -90, -100 },
		/* Five dB quieter, one short of the margin. */
		{ RADIANT_PROFILE_FREQ_RF_MID, true, -95, -104 },
	};
	uint8_t out = 0xFFu;

	zassert_equal(-EALREADY, radiant_profile_freq_select(RADIANT_PROFILE_FREQ_RF_HOME, ev,
						     ARRAY_SIZE(ev), &out));

	/* One more dB and it is exactly the margin, which qualifies: the rule is
	 * "at least this much quieter", and a boundary that meant one thing in
	 * the comment and another in the code is the reason this case is a test
	 * rather than an inspection. */
	ev[1].busy_dbm = (int8_t)(ev[0].busy_dbm - RADIANT_PROFILE_FREQ_MARGIN_DB);
	zassert_ok(radiant_profile_freq_select(RADIANT_PROFILE_FREQ_RF_HOME, ev, ARRAY_SIZE(ev),
				       &out));
	zassert_equal(RADIANT_PROFILE_FREQ_RF_MID, out);
}

/*
 * Two receivers with the same evidence must recommend the same index, ties
 * included. This is the rule that lets selection be "node-side if it can
 * measure, receiver-side if not" without being two rules: the map is the
 * argument and the ranking is one function.
 */
ZTEST(radiant_profile_freq, test_selection_is_deterministic_under_a_tie)
{
	struct radiant_profile_freq_evidence a[] = {
		{ RADIANT_PROFILE_FREQ_RF_HOME, true, -70, -100 },
		{ RADIANT_PROFILE_FREQ_RF_HIGH, true, -96, -104 },
		{ RADIANT_PROFILE_FREQ_RF_MID, true, -96, -104 },
	};
	/* The same three, in the other order. */
	struct radiant_profile_freq_evidence b[] = {
		{ RADIANT_PROFILE_FREQ_RF_MID, true, -96, -104 },
		{ RADIANT_PROFILE_FREQ_RF_HIGH, true, -96, -104 },
		{ RADIANT_PROFILE_FREQ_RF_HOME, true, -70, -100 },
	};
	uint8_t out_a = 0u;
	uint8_t out_b = 0u;

	zassert_ok(radiant_profile_freq_select(RADIANT_PROFILE_FREQ_RF_HOME, a, ARRAY_SIZE(a),
				       &out_a));
	zassert_ok(radiant_profile_freq_select(RADIANT_PROFILE_FREQ_RF_HOME, b, ARRAY_SIZE(b),
				       &out_b));
	zassert_equal(out_a, out_b);
	zassert_equal(RADIANT_PROFILE_FREQ_RF_MID, out_a);
}

/*
 * The same function reading the real map, which is the node-side path. It also
 * pins the map's own vocabulary into this phase: `busy_dbm` is the maximum over
 * per-dwell means, and a selection built on the floor instead would rank by the
 * quietest microsecond.
 */
ZTEST(radiant_profile_freq, test_selection_from_the_channel_quality_map)
{
	uint8_t out = 0u;

	/* Nothing measured at all. */
	zassert_equal(-ENODATA,
		      radiant_profile_freq_select_from_map(RADIANT_PROFILE_FREQ_RF_HOME, NULL,
						   0u, &out));

	/* Home is loud; one candidate is far quieter, one is not. */
	map_note(RADIANT_PROFILE_FREQ_RF_HOME, -60);
	map_note(RADIANT_PROFILE_FREQ_RF_LOW, -62);
	map_note(RADIANT_PROFILE_FREQ_RF_MID, -95);
	map_note(RADIANT_PROFILE_FREQ_RF_HIGH, -64);

	zassert_ok(radiant_profile_freq_select_from_map(RADIANT_PROFILE_FREQ_RF_HOME, NULL, 0u,
						&out));
	zassert_equal(RADIANT_PROFILE_FREQ_RF_MID, out);

	/* An index with fewer than RADIANT_CHANMAP_MIN_DWELLS behind it is NOT
	 * MEASURED rather than quiet, and must not be chosen on the strength of
	 * one dwell's luck. */
	radiant_chanmap_reset();
	map_note(RADIANT_PROFILE_FREQ_RF_HOME, -60);
	radiant_chanmap_note(RADIANT_PROFILE_FREQ_RF_MID, -110, -110, 16u);
	zassert_equal(-ENODATA,
		      radiant_profile_freq_select_from_map(RADIANT_PROFILE_FREQ_RF_HOME, NULL,
						   0u, &out));
}

/* ---------------------------------------------------------------------------
 * The page
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_profile_freq, test_the_announcement_round_trips_through_the_canon_bytes)
{
	uint8_t body[RADIANT_PROFILE_FREQ_LEN];
	uint8_t target = 0u;
	uint8_t countdown = 0u;

	zassert_ok(radiant_profile_freq_encode(RADIANT_PROFILE_FREQ_RF_MID, 8u, body));
	zassert_mem_equal(body, CANON_FREQ, sizeof(CANON_FREQ));

	zassert_ok(radiant_profile_freq_decode(CANON_FREQ, sizeof(CANON_FREQ), &target,
				       &countdown));
	zassert_equal(RADIANT_PROFILE_FREQ_RF_MID, target);
	zassert_equal(8u, countdown);

	/* K = 64 messages is what eight units means, and the two constants have
	 * to keep agreeing or a receiver retunes eight times too early. */
	zassert_equal(RADIANT_PROFILE_FREQ_K_DEFAULT,
		      (uint16_t)(countdown * RADIANT_PROFILE_FREQ_UNIT));
}

/*
 * Fail CLOSED on a reserved byte, which is the opposite of what a descriptor
 * information flag does and is right here: these bytes say where to point a
 * radio, so a field this build does not understand is not something to ignore.
 */
ZTEST(radiant_profile_freq, test_a_malformed_announcement_is_refused_rather_than_guessed)
{
	uint8_t body[RADIANT_PROFILE_FREQ_LEN];
	uint8_t target;
	uint8_t countdown;

	for (uint8_t i = 4u; i < RADIANT_PROFILE_FREQ_LEN; i++) {
		memcpy(body, CANON_FREQ, sizeof(body));
		body[i] = 0x01u;
		zassert_equal(-EPROTO,
			      radiant_profile_freq_decode(body, sizeof(body), &target,
						  &countdown),
			      "reserved [%u]", i);
	}

	/* A zero countdown names no message at all. */
	memcpy(body, CANON_FREQ, sizeof(body));
	body[3] = 0u;
	zassert_equal(-EPROTO, radiant_profile_freq_decode(body, sizeof(body), &target,
						   &countdown));

	/* An index the radio cannot tune to. */
	memcpy(body, CANON_FREQ, sizeof(body));
	body[2] = (uint8_t)(RADIANT_RF_INDEX_MAX + 1u);
	zassert_equal(-EPROTO, radiant_profile_freq_decode(body, sizeof(body), &target,
						   &countdown));

	/* Some other page is not an error - it is not this page. */
	memcpy(body, CANON_FREQ, sizeof(body));
	body[0] = RADIANT_PROFILE_TLM_PAGE_DESCRIPTOR;
	zassert_equal(-ENOENT, radiant_profile_freq_decode(body, sizeof(body), &target,
						   &countdown));

	zassert_equal(-EINVAL, radiant_profile_freq_encode(
				       (uint8_t)(RADIANT_RF_INDEX_MAX + 1u), 8u,
				       body));
	zassert_equal(-EINVAL, radiant_profile_freq_encode(RADIANT_PROFILE_FREQ_RF_MID, 0u, body));
}

/* ---------------------------------------------------------------------------
 * The move
 * ---------------------------------------------------------------------------
 */

/* Run the node for `n` messages, feeding every announcement it builds to `rx`
 * and every silent slot to `rx` as well. Returns the message index at which the
 * node moved, or 0 if it did not. */
static uint32_t run(struct radiant_profile_freq *pf, struct radiant_profile_freq_rx *rx,
		    uint32_t n, uint64_t t0_us, uint32_t rx_from,
		    uint32_t *rx_due_at)
{
	uint8_t body[RADIANT_PROFILE_FREQ_LEN];
	uint32_t moved_at = 0u;
	uint32_t i;

	for (i = 0u; i < n; i++) {
		bool claimed = radiant_profile_freq_claim(0u, body, pf);

		if (rx != NULL && i >= rx_from) {
			(void)radiant_profile_freq_rx_slot(rx, claimed ? body : NULL,
						   RADIANT_PROFILE_FREQ_LEN);
			if (rx_due_at != NULL && *rx_due_at == 0u &&
			    radiant_profile_freq_rx_due(rx)) {
				*rx_due_at = i + 1u;
			}
		}

		if (radiant_profile_freq_sent(pf, t0_us + (uint64_t)i * 250000u) &&
		    moved_at == 0u) {
			moved_at = i + 1u;
		}
	}
	return moved_at;
}

/*
 * A node that never moves is byte for byte the node it was before this file
 * existed: the seam is offered every slot and claims nothing.
 */
ZTEST(radiant_profile_freq, test_a_node_that_never_moves_puts_nothing_extra_on_the_air)
{
	struct radiant_profile_freq_cfg cfg = { 0 };
	struct radiant_profile_freq pf;
	uint8_t body[RADIANT_PROFILE_FREQ_LEN];

	zassert_ok(radiant_profile_freq_init(&pf, &cfg));
	zassert_equal(RADIANT_PROFILE_FREQ_RF_HOME, radiant_profile_freq_rf_index(&pf));
	zassert_false(radiant_profile_freq_off_home(&pf));

	for (uint32_t i = 0u; i < 500u; i++) {
		memset(body, 0xAA, sizeof(body));
		zassert_false(radiant_profile_freq_claim(0u, body, &pf), "slot %u", i);
		(void)radiant_profile_freq_sent(&pf, (uint64_t)i * 250000u);
	}
	zassert_equal(0u, pf.announcements);
	zassert_equal(0u, pf.moves);
	zassert_equal(RADIANT_PROFILE_FREQ_RF_HOME, radiant_profile_freq_rf_index(&pf));
}

/* The regression that matters: the re-anchor moves the node's target to
 * whatever it just said. Applied to the last slot before the move, it would
 * push the move one unit further out, and the next announcement would do it
 * again - the node announcing forever and never leaving. */
ZTEST(radiant_profile_freq, test_the_countdown_terminates_rather_than_walking_away)
{
	struct radiant_profile_freq_cfg cfg = { 0 };
	struct radiant_profile_freq pf;
	uint32_t moved_at;

	zassert_ok(radiant_profile_freq_init(&pf, &cfg));
	zassert_ok(radiant_profile_freq_begin(&pf, RADIANT_PROFILE_FREQ_RF_MID, 0u));
	zassert_true(radiant_profile_freq_announcing(&pf));
	zassert_equal(RADIANT_PROFILE_FREQ_RF_MID, radiant_profile_freq_target(&pf));

	moved_at = run(&pf, NULL, 4u * RADIANT_PROFILE_FREQ_K_DEFAULT, 0u, 0u, NULL);

	zassert_not_equal(0u, moved_at);
	/* Within one unit of K: the first announcement re-anchors by at most
	 * RADIANT_PROFILE_FREQ_UNIT - 1 messages and nothing after it moves the target
	 * further. */
	zassert_true(moved_at >= RADIANT_PROFILE_FREQ_K_DEFAULT, "early at %u", moved_at);
	zassert_true(moved_at <= RADIANT_PROFILE_FREQ_K_DEFAULT + RADIANT_PROFILE_FREQ_UNIT,
		     "late at %u", moved_at);

	zassert_equal(RADIANT_PROFILE_FREQ_RF_MID, radiant_profile_freq_rf_index(&pf));
	zassert_true(radiant_profile_freq_off_home(&pf));
	zassert_false(radiant_profile_freq_announcing(&pf));
	zassert_equal(1u, pf.moves);

	/* Eight announcements at K = 64, one every eight messages, and the last
	 * slot before the move is silent. */
	zassert_true(pf.announcements >= 2u);
}

/*
 * The property the countdown exists for: both ends act on the same slot, and the
 * receiver never learns anything except from the air.
 */
ZTEST(radiant_profile_freq, test_node_and_receiver_move_on_the_same_slot)
{
	struct radiant_profile_freq_cfg cfg = { 0 };
	struct radiant_profile_freq pf;
	struct radiant_profile_freq_rx rx;
	uint32_t due_at = 0u;
	uint32_t moved_at;
	uint8_t target = 0u;

	zassert_ok(radiant_profile_freq_init(&pf, &cfg));
	radiant_profile_freq_rx_init(&rx);
	zassert_ok(radiant_profile_freq_begin(&pf, RADIANT_PROFILE_FREQ_RF_HIGH, 0u));

	moved_at = run(&pf, &rx, 4u * RADIANT_PROFILE_FREQ_K_DEFAULT, 0u, 0u, &due_at);

	zassert_not_equal(0u, moved_at);
	zassert_not_equal(0u, due_at);
	zassert_equal(moved_at, due_at, "node %u rx %u", moved_at, due_at);

	zassert_ok(radiant_profile_freq_rx_target(&rx, &target));
	zassert_equal(RADIANT_PROFILE_FREQ_RF_HIGH, target);
	zassert_equal(radiant_profile_freq_rf_index(&pf), target);
}

/* A receiver that arrives part way through reads the remaining count out of the
 * frame and lands on the same slot as one that heard the first copy. */
ZTEST(radiant_profile_freq, test_a_receiver_joining_mid_countdown_lands_on_the_same_slot)
{
	struct radiant_profile_freq_cfg cfg = { 0 };
	struct radiant_profile_freq pf;
	struct radiant_profile_freq_rx late;
	uint32_t due_at = 0u;
	uint32_t moved_at;

	zassert_ok(radiant_profile_freq_init(&pf, &cfg));
	radiant_profile_freq_rx_init(&late);
	zassert_ok(radiant_profile_freq_begin(&pf, RADIANT_PROFILE_FREQ_RF_LOW, 0u));

	/* Joins after five of the eight announcements have gone. */
	moved_at = run(&pf, &late, 4u * RADIANT_PROFILE_FREQ_K_DEFAULT, 0u, 41u, &due_at);

	zassert_not_equal(0u, moved_at);
	zassert_equal(moved_at, due_at, "rx %u node %u", due_at, moved_at);
	zassert_true(late.heard >= 1u);
}

/* A receiver counts SLOTS, not messages: the node's countdown is in
 * transmitted messages, and a receiver counting only what it heard would
 * fall behind by its own loss rate - least reliable exactly where it's
 * most needed. */
ZTEST(radiant_profile_freq, test_a_receiver_that_loses_most_of_the_countdown_is_still_on_time)
{
	struct radiant_profile_freq_cfg cfg = { 0 };
	struct radiant_profile_freq pf;
	struct radiant_profile_freq_rx rx;
	uint8_t body[RADIANT_PROFILE_FREQ_LEN];
	uint32_t due_at = 0u;
	uint32_t moved_at = 0u;
	uint32_t heard = 0u;

	zassert_ok(radiant_profile_freq_init(&pf, &cfg));
	radiant_profile_freq_rx_init(&rx);
	zassert_ok(radiant_profile_freq_begin(&pf, RADIANT_PROFILE_FREQ_RF_MID, 0u));

	for (uint32_t i = 0u; i < 4u * RADIANT_PROFILE_FREQ_K_DEFAULT; i++) {
		bool claimed = radiant_profile_freq_claim(0u, body, &pf);

		/* Every announcement after the first is dropped on the floor.
		 * The slot still happened, and the receiver still counts it. */
		if (claimed && heard == 0u) {
			(void)radiant_profile_freq_rx_slot(&rx, body, RADIANT_PROFILE_FREQ_LEN);
			heard++;
		} else {
			(void)radiant_profile_freq_rx_slot(&rx, NULL, 0u);
		}
		if (due_at == 0u && radiant_profile_freq_rx_due(&rx)) {
			due_at = i + 1u;
		}
		if (radiant_profile_freq_sent(&pf, (uint64_t)i * 250000u) &&
		    moved_at == 0u) {
			moved_at = i + 1u;
		}
	}

	zassert_equal(1u, rx.heard);
	zassert_not_equal(0u, moved_at);
	zassert_equal(moved_at, due_at, "rx %u node %u", due_at, moved_at);
}

/*
 * Minutes-scale, as a refusal rather than as advice. Rechoosing faster than a
 * receiver can follow is fast per-event hopping arrived at by accident, and that
 * is declined with arithmetic rather than left to a caller.
 */
ZTEST(radiant_profile_freq, test_a_second_move_inside_the_rate_limit_is_refused)
{
	struct radiant_profile_freq_cfg cfg = { 0 };
	struct radiant_profile_freq pf;
	uint64_t t;

	zassert_ok(radiant_profile_freq_init(&pf, &cfg));
	zassert_ok(radiant_profile_freq_begin(&pf, RADIANT_PROFILE_FREQ_RF_MID, 0u));

	/* A move already in flight is refused, and separately counted from a
	 * rate-limited one: they are different things to a caller. */
	zassert_equal(-EBUSY, radiant_profile_freq_begin(&pf, RADIANT_PROFILE_FREQ_RF_LOW, 0u));
	zassert_equal(1u, pf.refused_busy);

	(void)run(&pf, NULL, 4u * RADIANT_PROFILE_FREQ_K_DEFAULT, 0u, 0u, NULL);
	zassert_equal(1u, pf.moves);

	/* The floor runs from the move, not the request that started it or the
	 * last message sent: charging the countdown against the next move
	 * would let a long K buy the next move sooner - backwards. Reading the
	 * instant from the module rather than recomputing it tests the rule,
	 * not the test's own arithmetic. */
	t = pf.last_move_us;
	zassert_true(t < (uint64_t)(4u * RADIANT_PROFILE_FREQ_K_DEFAULT - 1u) * 250000u);

	zassert_equal(-EAGAIN,
		      radiant_profile_freq_begin(&pf, RADIANT_PROFILE_FREQ_RF_LOW, t + 1u));
	zassert_equal(1u, pf.refused_rate);

	zassert_equal(-EAGAIN,
		      radiant_profile_freq_begin(&pf, RADIANT_PROFILE_FREQ_RF_LOW,
					 t + (uint64_t)RADIANT_PROFILE_FREQ_MIN_MOVE_S *
						     1000000u - 1u));

	zassert_ok(radiant_profile_freq_begin(&pf, RADIANT_PROFILE_FREQ_RF_LOW,
				      t + (uint64_t)RADIANT_PROFILE_FREQ_MIN_MOVE_S *
						  1000000u));
	zassert_equal(2u, pf.refused_rate);
}

/*
 * A sparse node's message count and its receiver's slot count are unrelated, so
 * there is no countdown either end could agree on. Refused with a counter rather
 * than served with something that half works.
 */
ZTEST(radiant_profile_freq, test_a_sparse_node_is_refused_rather_than_half_served)
{
	struct radiant_profile_freq_cfg cfg = { .sparse = true };
	struct radiant_profile_freq pf;

	zassert_ok(radiant_profile_freq_init(&pf, &cfg));
	zassert_equal(-ENOTSUP, radiant_profile_freq_begin(&pf, RADIANT_PROFILE_FREQ_RF_MID, 0u));
	zassert_equal(1u, pf.refused_sparse);
	zassert_false(radiant_profile_freq_announcing(&pf));

	/* And asking it to move where it already is is not a refusal at all. */
	zassert_equal(-EALREADY,
		      radiant_profile_freq_begin(&pf, RADIANT_PROFILE_FREQ_RF_HOME, 0u));
	zassert_equal(1u, pf.refused_sparse);
}

ZTEST(radiant_profile_freq, test_a_k_the_countdown_field_cannot_express_is_refused)
{
	struct radiant_profile_freq_cfg cfg = { 0 };
	struct radiant_profile_freq pf;

	cfg.k = RADIANT_PROFILE_FREQ_UNIT + 1u;
	zassert_equal(-EINVAL, radiant_profile_freq_init(&pf, &cfg));

	cfg.k = (uint16_t)(RADIANT_PROFILE_FREQ_K_MAX + RADIANT_PROFILE_FREQ_UNIT);
	zassert_equal(-EINVAL, radiant_profile_freq_init(&pf, &cfg));

	cfg.k = RADIANT_PROFILE_FREQ_K_MAX;
	zassert_ok(radiant_profile_freq_init(&pf, &cfg));
}

/* ---------------------------------------------------------------------------
 * The descriptor, and the retry list
 * ---------------------------------------------------------------------------
 */

/*
 * After the move the node's two statements about its own frequency agree, which
 * radiant_profile_desc_encode() enforces and would otherwise refuse the whole set for.
 * Round-tripped through the wire rather than inspected in the struct, because
 * byte [6] and flag bit 1 are what a receiver actually reads.
 */
ZTEST(radiant_profile_freq, test_the_descriptor_after_a_move_announces_index_and_flag)
{
	struct radiant_profile_descriptor d;
	struct radiant_profile_descriptor back;
	uint8_t frames[RADIANT_PROFILE_TLM_MAX_FRAMES * RADIANT_PROFILE_TLM_FRAME_LEN];
	int n;

	memset(&d, 0, sizeof(d));
	d.version = RADIANT_PROFILE_TLM_VERSION;
	d.schema_id = 1u;
	d.period = 8192u;
	d.rf_index = RADIANT_PROFILE_TLM_RF_INDEX_DEFAULT;

	/* Before: home, and the flag clear. */
	n = radiant_profile_desc_encode(&d, frames, RADIANT_PROFILE_TLM_MAX_FRAMES);
	zassert_true(n > 0, "home %d", n);
	zassert_ok(radiant_profile_desc_decode(frames, (uint8_t)n, &back));
	zassert_equal(RADIANT_PROFILE_TLM_RF_INDEX_DEFAULT, back.rf_index);
	zassert_equal(0u, back.flags & RADIANT_PROFILE_TLM_FLAG_OFF_RF57);

	/* After. */
	zassert_ok(radiant_profile_freq_apply(&d, RADIANT_PROFILE_FREQ_RF_MID));
	n = radiant_profile_desc_encode(&d, frames, RADIANT_PROFILE_TLM_MAX_FRAMES);
	zassert_true(n > 0, "off-57 %d", n);
	zassert_ok(radiant_profile_desc_decode(frames, (uint8_t)n, &back));
	zassert_equal(RADIANT_PROFILE_FREQ_RF_MID, back.rf_index);
	zassert_not_equal(0u, back.flags & RADIANT_PROFILE_TLM_FLAG_OFF_RF57);

	/* And back again, which is the case a caller writes by hand and gets
	 * wrong: setting the index without clearing the flag. */
	zassert_ok(radiant_profile_freq_apply(&d, RADIANT_PROFILE_TLM_RF_INDEX_DEFAULT));
	n = radiant_profile_desc_encode(&d, frames, RADIANT_PROFILE_TLM_MAX_FRAMES);
	zassert_true(n > 0, "home %d", n);
	zassert_ok(radiant_profile_desc_decode(frames, (uint8_t)n, &back));
	zassert_equal(0u, back.flags & RADIANT_PROFILE_TLM_FLAG_OFF_RF57);

	zassert_equal(-EINVAL, radiant_profile_freq_apply(&d, (uint8_t)(
						  RADIANT_PROFILE_TLM_RF_INDEX_MAX + 1u)));
}

/*
 * The bounded retry list. It is what makes an off-57 node re-acquirable without
 * touching the sweep, and the assertion that matters is that it is SHORT and
 * ends at home - a list that grew would be a frequency sweep with a different
 * name, which is the thing ADR 0007 refused for a second PHY.
 */
ZTEST(radiant_profile_freq, test_the_retry_list_is_bounded_and_ends_at_home)
{
	struct radiant_profile_freq_rx rx;
	uint8_t order[RADIANT_PROFILE_FREQ_REACQUIRE_MAX];
	uint8_t n;

	radiant_profile_freq_rx_init(&rx);

	/* A receiver that never heard anything: the defaults, then home. */
	n = radiant_profile_freq_reacquire_order(&rx, order, sizeof(order));
	zassert_equal(RADIANT_PROFILE_FREQ_N_DEFAULTS + 1u, n);
	zassert_equal(RADIANT_PROFILE_FREQ_RF_HOME, order[n - 1u]);

	/* One that was told where the node went tries there first. */
	(void)radiant_profile_freq_rx_slot(&rx, CANON_FREQ, sizeof(CANON_FREQ));
	n = radiant_profile_freq_reacquire_order(&rx, order, sizeof(order));
	zassert_equal(RADIANT_PROFILE_FREQ_RF_MID, order[0]);
	zassert_true(n <= RADIANT_PROFILE_FREQ_REACQUIRE_MAX);
	zassert_equal(RADIANT_PROFILE_FREQ_RF_HOME, order[n - 1u]);

	/* Deduplicated: RF 26 is both the announced target and a default, and
	 * an entry listed twice is a window spent twice. */
	for (uint8_t i = 0u; i < n; i++) {
		for (uint8_t j = (uint8_t)(i + 1u); j < n; j++) {
			zassert_not_equal(order[i], order[j], "RF %u", order[i]);
		}
	}
}

/* ---------------------------------------------------------------------------
 * Discovery never moves
 * ---------------------------------------------------------------------------
 */

/*
 * The sweep is 1 M, RF 57, the three-byte search format, and nothing in this
 * phase can reach it. ADR 0007 asserted the same property for a second PHY;
 * this is the frequency axis making the same promise, and it is asserted
 * against a real search instance rather than by reading the source.
 *
 * The stronger half of the claim is structural and is not testable here: there
 * is no call from radiant_profile_freq.c into radiant_search.c, and no candidate list
 * the sweep consults. What this test defends is that the default has not
 * quietly become configurable from somewhere else.
 */
ZTEST(radiant_profile_freq, test_discovery_stays_on_rf_57_however_far_a_node_moves)
{
	static struct radiant_search s;
	struct radiant_search_cfg cfg;
	struct radiant_search_id_filter want;
	struct radiant_search_window w;
	struct radiant_profile_freq_cfg fcfg = { 0 };
	struct radiant_profile_freq pf;

	radiant_search_cfg_default(&cfg);
	zassert_equal(RADIANT_RF_INDEX_ANT_PLUS, cfg.rf_index);

	zassert_ok(radiant_profile_freq_init(&pf, &fcfg));
	zassert_ok(radiant_profile_freq_begin(&pf, RADIANT_PROFILE_FREQ_RF_HIGH, 0u));
	(void)run(&pf, NULL, 4u * RADIANT_PROFILE_FREQ_K_DEFAULT, 0u, 0u, NULL);
	zassert_equal(RADIANT_PROFILE_FREQ_RF_HIGH, radiant_profile_freq_rf_index(&pf));

	fake_radio_reset();
	zassert_ok(radiant_search_init(&s, &cfg, NULL, NULL));
	memset(&want, 0, sizeof(want)); /* ANT's wildcard: zero is "any" */
	zassert_ok(radiant_search_begin(&s, 0u, RADIANT_SEARCH_MODE_ACQUIRE,
					&want, radiant_radio_now(),
					RADIANT_SEARCH_TIMEOUT_NONE));

	memset(&w, 0, sizeof(w));
	zassert_ok(radiant_search_window(&s, radiant_radio_now() + 1000u, &w));
	zassert_equal(RADIANT_RF_INDEX_ANT_PLUS, w.rf_index, "RF %u", w.rf_index);
	zassert_equal(radiant_frame_format(RADIANT_FRAME_CFG_SEARCH), w.fmt);

	(void)radiant_search_end(&s, 0u);
}
