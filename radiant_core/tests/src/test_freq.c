/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_freq.c - adaptive frequency (RF-7).
 *
 * Provenance: docs/decisions/0012-adaptive-frequency.md and
 * docs/decisions/0005-extension-inside-ant-plus.md axis 5, both this project's
 * own documents. No adopter-gated ANT+ device profile document was read for this
 * file. See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * What this suite is standing in for
 * ---------------------------------------------------------------------------
 * The phase's bench gate is "loss ~ 0 on the selected quiet channel with a
 * single copy per event, against ~0.4 % on 57, in one sitting", and it cannot be
 * run: the Feather that stands in for the stock dongle is in its UF2 bootloader
 * and is reserved for the combined session. THIS SUITE DOES NOT SUBSTITUTE FOR
 * THAT MEASUREMENT AND MUST NOT BE READ AS ONE. No dB figure and no loss figure
 * appears anywhere in this file.
 *
 * What it does check is every claim the mechanism makes that is a claim about
 * software, and two of them are the ones a bench would find last:
 *
 *   THE COUNTDOWN TERMINATES. The re-anchor moves the node's target to whatever
 *   it most recently announced, which is what makes every receiver land on one
 *   message. Applied one announcement too many it moves the target past the move
 *   itself, and then again, and the node never leaves - a bug that looks exactly
 *   like "adaptive frequency does not work" on a bench and exactly like nothing
 *   at all in a code review.
 *
 *   NODE AND RECEIVER LAND ON THE SAME SLOT, including a receiver that joined
 *   part way through the countdown and one that lost most of it. That agreement
 *   is silent when it breaks: the two do not fail, they stop meeting.
 *
 * The scheduler half of the phase - an off-57 window never joining the merged
 * one - is asserted in test_sched.c, next to the merge tests it is the negative
 * case for, rather than here where it would need the whole mock-radio harness a
 * second time.
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

#include <radiant_core/radiant_chanmap.h>
#include <radiant_core/radiant_frame.h>
#include <radiant_core/radiant_radio_hal.h>
#include <radiant_core/radiant_search.h>

#include "profile_freq.h"
#include "profile_telemetry.h"

/*
 * The canonical announcement, shared BYTE FOR BYTE with tools/test_ant_pages.py.
 * Two implementations checked only against each other are not checked at all,
 * and this page in particular tells a receiver where to point its radio.
 *
 * Target RF 26 (2426 MHz), countdown 8 units = 64 transmitted messages.
 */
static const uint8_t CANON_FREQ[PROFILE_FREQ_LEN] = {
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

ZTEST_SUITE(profile_freq, NULL, NULL, freq_before, NULL, NULL);

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
ZTEST(profile_freq, test_the_candidate_set_is_bles_advertising_placement)
{
	zassert_equal(2402u, radiant_rf_index_to_mhz(profile_freq_defaults[0]));
	zassert_equal(2426u, radiant_rf_index_to_mhz(profile_freq_defaults[1]));
	zassert_equal(2480u, radiant_rf_index_to_mhz(profile_freq_defaults[2]));

	/* Ascending, in range, and none of them is home - a candidate list
	 * containing the incumbent would make "move" and "stay" the same
	 * answer. */
	zassert_true(profile_freq_defaults[0] < profile_freq_defaults[1]);
	zassert_true(profile_freq_defaults[1] < profile_freq_defaults[2]);
	zassert_true(profile_freq_defaults[2] <= RADIANT_RF_INDEX_MAX);
	for (uint8_t i = 0u; i < PROFILE_FREQ_N_DEFAULTS; i++) {
		zassert_not_equal(PROFILE_FREQ_RF_HOME, profile_freq_defaults[i]);
	}

	/* ANT+ is 2457 MHz, which is the number the whole phase exists to
	 * leave. Stated here so the two are in one place. */
	zassert_equal(2457u, radiant_rf_index_to_mhz(PROFILE_FREQ_RF_HOME));
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
ZTEST(profile_freq, test_selection_refuses_without_evidence_for_where_we_are)
{
	struct profile_freq_evidence ev[] = {
		{ PROFILE_FREQ_RF_HOME, false, 0, 0 },
		{ PROFILE_FREQ_RF_MID, true, -100, -104 },
	};
	uint8_t out = 0xFFu;

	zassert_equal(-ENODATA, profile_freq_select(PROFILE_FREQ_RF_HOME, ev,
						    ARRAY_SIZE(ev), &out));
	zassert_equal(0xFFu, out, "a refused selection wrote an answer anyway");
}

ZTEST(profile_freq, test_selection_picks_the_quietest_candidate_past_the_margin)
{
	struct profile_freq_evidence ev[] = {
		{ PROFILE_FREQ_RF_HOME, true, -70, -100 },
		{ PROFILE_FREQ_RF_LOW, true, -84, -102 },
		{ PROFILE_FREQ_RF_MID, true, -96, -104 },
		{ PROFILE_FREQ_RF_HIGH, true, -88, -103 },
	};
	uint8_t out = 0u;

	zassert_ok(profile_freq_select(PROFILE_FREQ_RF_HOME, ev, ARRAY_SIZE(ev),
				       &out));
	zassert_equal(PROFILE_FREQ_RF_MID, out,
		      "the quietest candidate was not chosen");
}

/*
 * The anti-churn rule. Two indices within a few dB of each other trade places as
 * the room changes, and a node that re-announced every time would be hopping
 * slowly rather than adapting - which is the thing this phase is defined against
 * rather than a tuning preference.
 */
ZTEST(profile_freq, test_a_candidate_inside_the_margin_is_not_worth_moving_to)
{
	struct profile_freq_evidence ev[] = {
		{ PROFILE_FREQ_RF_HOME, true, -90, -100 },
		/* Five dB quieter, one short of the margin. */
		{ PROFILE_FREQ_RF_MID, true, -95, -104 },
	};
	uint8_t out = 0xFFu;

	zassert_equal(-EALREADY, profile_freq_select(PROFILE_FREQ_RF_HOME, ev,
						     ARRAY_SIZE(ev), &out));

	/* One more dB and it is exactly the margin, which qualifies: the rule is
	 * "at least this much quieter", and a boundary that meant one thing in
	 * the comment and another in the code is the reason this case is a test
	 * rather than an inspection. */
	ev[1].busy_dbm = (int8_t)(ev[0].busy_dbm - PROFILE_FREQ_MARGIN_DB);
	zassert_ok(profile_freq_select(PROFILE_FREQ_RF_HOME, ev, ARRAY_SIZE(ev),
				       &out));
	zassert_equal(PROFILE_FREQ_RF_MID, out);
}

/*
 * Two receivers with the same evidence must recommend the same index, ties
 * included. This is the rule that lets selection be "node-side if it can
 * measure, receiver-side if not" without being two rules: the map is the
 * argument and the ranking is one function.
 */
ZTEST(profile_freq, test_selection_is_deterministic_under_a_tie)
{
	struct profile_freq_evidence a[] = {
		{ PROFILE_FREQ_RF_HOME, true, -70, -100 },
		{ PROFILE_FREQ_RF_HIGH, true, -96, -104 },
		{ PROFILE_FREQ_RF_MID, true, -96, -104 },
	};
	/* The same three, in the other order. */
	struct profile_freq_evidence b[] = {
		{ PROFILE_FREQ_RF_MID, true, -96, -104 },
		{ PROFILE_FREQ_RF_HIGH, true, -96, -104 },
		{ PROFILE_FREQ_RF_HOME, true, -70, -100 },
	};
	uint8_t out_a = 0u;
	uint8_t out_b = 0u;

	zassert_ok(profile_freq_select(PROFILE_FREQ_RF_HOME, a, ARRAY_SIZE(a),
				       &out_a));
	zassert_ok(profile_freq_select(PROFILE_FREQ_RF_HOME, b, ARRAY_SIZE(b),
				       &out_b));
	zassert_equal(out_a, out_b,
		      "the same evidence in a different order chose differently");
	zassert_equal(PROFILE_FREQ_RF_MID, out_a,
		      "the tie-break is not the lower index");
}

/*
 * The same function reading the real map, which is the node-side path. It also
 * pins the map's own vocabulary into this phase: `busy_dbm` is the maximum over
 * per-dwell means, and a selection built on the floor instead would rank by the
 * quietest microsecond.
 */
ZTEST(profile_freq, test_selection_from_the_channel_quality_map)
{
	uint8_t out = 0u;

	/* Nothing measured at all. */
	zassert_equal(-ENODATA,
		      profile_freq_select_from_map(PROFILE_FREQ_RF_HOME, NULL,
						   0u, &out));

	/* Home is loud; one candidate is far quieter, one is not. */
	map_note(PROFILE_FREQ_RF_HOME, -60);
	map_note(PROFILE_FREQ_RF_LOW, -62);
	map_note(PROFILE_FREQ_RF_MID, -95);
	map_note(PROFILE_FREQ_RF_HIGH, -64);

	zassert_ok(profile_freq_select_from_map(PROFILE_FREQ_RF_HOME, NULL, 0u,
						&out));
	zassert_equal(PROFILE_FREQ_RF_MID, out);

	/* An index with fewer than RADIANT_CHANMAP_MIN_DWELLS behind it is NOT
	 * MEASURED rather than quiet, and must not be chosen on the strength of
	 * one dwell's luck. */
	radiant_chanmap_reset();
	map_note(PROFILE_FREQ_RF_HOME, -60);
	radiant_chanmap_note(PROFILE_FREQ_RF_MID, -110, -110, 16u);
	zassert_equal(-ENODATA,
		      profile_freq_select_from_map(PROFILE_FREQ_RF_HOME, NULL,
						   0u, &out));
}

/* ---------------------------------------------------------------------------
 * The page
 * ---------------------------------------------------------------------------
 */

ZTEST(profile_freq, test_the_announcement_round_trips_through_the_canon_bytes)
{
	uint8_t body[PROFILE_FREQ_LEN];
	uint8_t target = 0u;
	uint8_t countdown = 0u;

	zassert_ok(profile_freq_encode(PROFILE_FREQ_RF_MID, 8u, body));
	zassert_mem_equal(body, CANON_FREQ, sizeof(CANON_FREQ),
			  "the announcement's bytes moved");

	zassert_ok(profile_freq_decode(CANON_FREQ, sizeof(CANON_FREQ), &target,
				       &countdown));
	zassert_equal(PROFILE_FREQ_RF_MID, target);
	zassert_equal(8u, countdown);

	/* K = 64 messages is what eight units means, and the two constants have
	 * to keep agreeing or a receiver retunes eight times too early. */
	zassert_equal(PROFILE_FREQ_K_DEFAULT,
		      (uint16_t)(countdown * PROFILE_FREQ_UNIT));
}

/*
 * Fail CLOSED on a reserved byte, which is the opposite of what a descriptor
 * information flag does and is right here: these bytes say where to point a
 * radio, so a field this build does not understand is not something to ignore.
 */
ZTEST(profile_freq, test_a_malformed_announcement_is_refused_rather_than_guessed)
{
	uint8_t body[PROFILE_FREQ_LEN];
	uint8_t target;
	uint8_t countdown;

	for (uint8_t i = 4u; i < PROFILE_FREQ_LEN; i++) {
		memcpy(body, CANON_FREQ, sizeof(body));
		body[i] = 0x01u;
		zassert_equal(-EPROTO,
			      profile_freq_decode(body, sizeof(body), &target,
						  &countdown),
			      "reserved byte [%u] was ignored", i);
	}

	/* A zero countdown names no message at all. */
	memcpy(body, CANON_FREQ, sizeof(body));
	body[3] = 0u;
	zassert_equal(-EPROTO, profile_freq_decode(body, sizeof(body), &target,
						   &countdown));

	/* An index the radio cannot tune to. */
	memcpy(body, CANON_FREQ, sizeof(body));
	body[2] = (uint8_t)(RADIANT_RF_INDEX_MAX + 1u);
	zassert_equal(-EPROTO, profile_freq_decode(body, sizeof(body), &target,
						   &countdown));

	/* Some other page is not an error - it is not this page. */
	memcpy(body, CANON_FREQ, sizeof(body));
	body[0] = PROFILE_TLM_PAGE_DESCRIPTOR;
	zassert_equal(-ENOENT, profile_freq_decode(body, sizeof(body), &target,
						   &countdown));

	zassert_equal(-EINVAL, profile_freq_encode(
				       (uint8_t)(RADIANT_RF_INDEX_MAX + 1u), 8u,
				       body));
	zassert_equal(-EINVAL, profile_freq_encode(PROFILE_FREQ_RF_MID, 0u, body));
}

/* ---------------------------------------------------------------------------
 * The move
 * ---------------------------------------------------------------------------
 */

/* Run the node for `n` messages, feeding every announcement it builds to `rx`
 * and every silent slot to `rx` as well. Returns the message index at which the
 * node moved, or 0 if it did not. */
static uint32_t run(struct profile_freq *pf, struct profile_freq_rx *rx,
		    uint32_t n, uint64_t t0_us, uint32_t rx_from,
		    uint32_t *rx_due_at)
{
	uint8_t body[PROFILE_FREQ_LEN];
	uint32_t moved_at = 0u;
	uint32_t i;

	for (i = 0u; i < n; i++) {
		bool claimed = profile_freq_claim(0u, body, pf);

		if (rx != NULL && i >= rx_from) {
			(void)profile_freq_rx_slot(rx, claimed ? body : NULL,
						   PROFILE_FREQ_LEN);
			if (rx_due_at != NULL && *rx_due_at == 0u &&
			    profile_freq_rx_due(rx)) {
				*rx_due_at = i + 1u;
			}
		}

		if (profile_freq_sent(pf, t0_us + (uint64_t)i * 250000u) &&
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
ZTEST(profile_freq, test_a_node_that_never_moves_puts_nothing_extra_on_the_air)
{
	struct profile_freq_cfg cfg = { 0 };
	struct profile_freq pf;
	uint8_t body[PROFILE_FREQ_LEN];

	zassert_ok(profile_freq_init(&pf, &cfg));
	zassert_equal(PROFILE_FREQ_RF_HOME, profile_freq_rf_index(&pf));
	zassert_false(profile_freq_off_home(&pf));

	for (uint32_t i = 0u; i < 500u; i++) {
		memset(body, 0xAA, sizeof(body));
		zassert_false(profile_freq_claim(0u, body, &pf),
			      "a quiet node claimed slot %u", i);
		(void)profile_freq_sent(&pf, (uint64_t)i * 250000u);
	}
	zassert_equal(0u, pf.announcements);
	zassert_equal(0u, pf.moves);
	zassert_equal(PROFILE_FREQ_RF_HOME, profile_freq_rf_index(&pf));
}

/*
 * THE REGRESSION THAT MATTERS. The re-anchor moves the node's target to whatever
 * it just said; applied to the last slot before the move it would push the move
 * one unit further out, and the next announcement would do it again. The node
 * would announce forever and never leave - which reads on a bench as "adaptive
 * frequency does not work" with nothing in any log.
 */
ZTEST(profile_freq, test_the_countdown_terminates_rather_than_walking_away)
{
	struct profile_freq_cfg cfg = { 0 };
	struct profile_freq pf;
	uint32_t moved_at;

	zassert_ok(profile_freq_init(&pf, &cfg));
	zassert_ok(profile_freq_begin(&pf, PROFILE_FREQ_RF_MID, 0u));
	zassert_true(profile_freq_announcing(&pf));
	zassert_equal(PROFILE_FREQ_RF_MID, profile_freq_target(&pf));

	moved_at = run(&pf, NULL, 4u * PROFILE_FREQ_K_DEFAULT, 0u, 0u, NULL);

	zassert_not_equal(0u, moved_at, "the node never moved");
	/* Within one unit of K: the first announcement re-anchors by at most
	 * PROFILE_FREQ_UNIT - 1 messages and nothing after it moves the target
	 * further. */
	zassert_true(moved_at >= PROFILE_FREQ_K_DEFAULT,
		     "the node moved early, at message %u", moved_at);
	zassert_true(moved_at <= PROFILE_FREQ_K_DEFAULT + PROFILE_FREQ_UNIT,
		     "the countdown walked the move out to message %u", moved_at);

	zassert_equal(PROFILE_FREQ_RF_MID, profile_freq_rf_index(&pf));
	zassert_true(profile_freq_off_home(&pf));
	zassert_false(profile_freq_announcing(&pf));
	zassert_equal(1u, pf.moves);

	/* Eight announcements at K = 64, one every eight messages, and the last
	 * slot before the move is silent. */
	zassert_true(pf.announcements >= 2u,
		     "one announcement is not a countdown a receiver can join");
}

/*
 * The property the countdown exists for: both ends act on the same slot, and the
 * receiver never learns anything except from the air.
 */
ZTEST(profile_freq, test_node_and_receiver_move_on_the_same_slot)
{
	struct profile_freq_cfg cfg = { 0 };
	struct profile_freq pf;
	struct profile_freq_rx rx;
	uint32_t due_at = 0u;
	uint32_t moved_at;
	uint8_t target = 0u;

	zassert_ok(profile_freq_init(&pf, &cfg));
	profile_freq_rx_init(&rx);
	zassert_ok(profile_freq_begin(&pf, PROFILE_FREQ_RF_HIGH, 0u));

	moved_at = run(&pf, &rx, 4u * PROFILE_FREQ_K_DEFAULT, 0u, 0u, &due_at);

	zassert_not_equal(0u, moved_at);
	zassert_not_equal(0u, due_at, "the receiver never became due");
	zassert_equal(moved_at, due_at,
		      "the node moved on message %u and the receiver on slot %u",
		      moved_at, due_at);

	zassert_ok(profile_freq_rx_target(&rx, &target));
	zassert_equal(PROFILE_FREQ_RF_HIGH, target);
	zassert_equal(profile_freq_rf_index(&pf), target,
		      "the receiver retuned somewhere the node is not");
}

/* A receiver that arrives part way through reads the remaining count out of the
 * frame and lands on the same slot as one that heard the first copy. */
ZTEST(profile_freq, test_a_receiver_joining_mid_countdown_lands_on_the_same_slot)
{
	struct profile_freq_cfg cfg = { 0 };
	struct profile_freq pf;
	struct profile_freq_rx late;
	uint32_t due_at = 0u;
	uint32_t moved_at;

	zassert_ok(profile_freq_init(&pf, &cfg));
	profile_freq_rx_init(&late);
	zassert_ok(profile_freq_begin(&pf, PROFILE_FREQ_RF_LOW, 0u));

	/* Joins after five of the eight announcements have gone. */
	moved_at = run(&pf, &late, 4u * PROFILE_FREQ_K_DEFAULT, 0u, 41u, &due_at);

	zassert_not_equal(0u, moved_at);
	zassert_equal(moved_at, due_at,
		      "a late joiner retuned on slot %u, not %u", due_at,
		      moved_at);
	zassert_true(late.heard >= 1u, "the late joiner heard nothing");
}

/*
 * A receiver counts SLOTS, not messages, and this is the test that says why. The
 * node's countdown is in transmitted messages; a receiver that counted only what
 * it heard would fall behind by its own loss rate - which is the ~0.4 % this
 * whole phase exists to remove, so the feature would be least reliable exactly
 * where it is most needed.
 */
ZTEST(profile_freq, test_a_receiver_that_loses_most_of_the_countdown_is_still_on_time)
{
	struct profile_freq_cfg cfg = { 0 };
	struct profile_freq pf;
	struct profile_freq_rx rx;
	uint8_t body[PROFILE_FREQ_LEN];
	uint32_t due_at = 0u;
	uint32_t moved_at = 0u;
	uint32_t heard = 0u;

	zassert_ok(profile_freq_init(&pf, &cfg));
	profile_freq_rx_init(&rx);
	zassert_ok(profile_freq_begin(&pf, PROFILE_FREQ_RF_MID, 0u));

	for (uint32_t i = 0u; i < 4u * PROFILE_FREQ_K_DEFAULT; i++) {
		bool claimed = profile_freq_claim(0u, body, &pf);

		/* Every announcement after the first is dropped on the floor.
		 * The slot still happened, and the receiver still counts it. */
		if (claimed && heard == 0u) {
			(void)profile_freq_rx_slot(&rx, body, PROFILE_FREQ_LEN);
			heard++;
		} else {
			(void)profile_freq_rx_slot(&rx, NULL, 0u);
		}
		if (due_at == 0u && profile_freq_rx_due(&rx)) {
			due_at = i + 1u;
		}
		if (profile_freq_sent(&pf, (uint64_t)i * 250000u) &&
		    moved_at == 0u) {
			moved_at = i + 1u;
		}
	}

	zassert_equal(1u, rx.heard, "the test did not actually drop anything");
	zassert_not_equal(0u, moved_at);
	zassert_equal(moved_at, due_at,
		      "a receiver holding only the first copy retuned on slot "
		      "%u, not %u",
		      due_at, moved_at);
}

/*
 * Minutes-scale, as a refusal rather than as advice. Rechoosing faster than a
 * receiver can follow is fast per-event hopping arrived at by accident, and that
 * is declined with arithmetic rather than left to a caller.
 */
ZTEST(profile_freq, test_a_second_move_inside_the_rate_limit_is_refused)
{
	struct profile_freq_cfg cfg = { 0 };
	struct profile_freq pf;
	uint64_t t;

	zassert_ok(profile_freq_init(&pf, &cfg));
	zassert_ok(profile_freq_begin(&pf, PROFILE_FREQ_RF_MID, 0u));

	/* A move already in flight is refused, and separately counted from a
	 * rate-limited one: they are different things to a caller. */
	zassert_equal(-EBUSY, profile_freq_begin(&pf, PROFILE_FREQ_RF_LOW, 0u));
	zassert_equal(1u, pf.refused_busy);

	(void)run(&pf, NULL, 4u * PROFILE_FREQ_K_DEFAULT, 0u, 0u, NULL);
	zassert_equal(1u, pf.moves);

	/*
	 * THE FLOOR RUNS FROM THE MOVE, NOT FROM THE REQUEST THAT STARTED IT AND
	 * NOT FROM THE LAST MESSAGE SENT. The countdown is time a receiver has
	 * already spent following this move, so charging it against the next one
	 * would let a long K buy the next move sooner - backwards. run() sent
	 * messages long past the move, and reading the instant from the module
	 * rather than recomputing it here is what makes this a test of the rule
	 * instead of a test of the test's arithmetic.
	 */
	t = pf.last_move_us;
	zassert_true(t < (uint64_t)(4u * PROFILE_FREQ_K_DEFAULT - 1u) * 250000u,
		     "the floor was charged from the last message, not the move");

	zassert_equal(-EAGAIN,
		      profile_freq_begin(&pf, PROFILE_FREQ_RF_LOW, t + 1u));
	zassert_equal(1u, pf.refused_rate);

	zassert_equal(-EAGAIN,
		      profile_freq_begin(&pf, PROFILE_FREQ_RF_LOW,
					 t + (uint64_t)PROFILE_FREQ_MIN_MOVE_S *
						     1000000u - 1u));

	zassert_ok(profile_freq_begin(&pf, PROFILE_FREQ_RF_LOW,
				      t + (uint64_t)PROFILE_FREQ_MIN_MOVE_S *
						  1000000u));
	zassert_equal(2u, pf.refused_rate);
}

/*
 * A sparse node's message count and its receiver's slot count are unrelated, so
 * there is no countdown either end could agree on. Refused with a counter rather
 * than served with something that half works.
 */
ZTEST(profile_freq, test_a_sparse_node_is_refused_rather_than_half_served)
{
	struct profile_freq_cfg cfg = { .sparse = true };
	struct profile_freq pf;

	zassert_ok(profile_freq_init(&pf, &cfg));
	zassert_equal(-ENOTSUP, profile_freq_begin(&pf, PROFILE_FREQ_RF_MID, 0u));
	zassert_equal(1u, pf.refused_sparse);
	zassert_false(profile_freq_announcing(&pf));

	/* And asking it to move where it already is is not a refusal at all. */
	zassert_equal(-EALREADY,
		      profile_freq_begin(&pf, PROFILE_FREQ_RF_HOME, 0u));
	zassert_equal(1u, pf.refused_sparse);
}

ZTEST(profile_freq, test_a_k_the_countdown_field_cannot_express_is_refused)
{
	struct profile_freq_cfg cfg = { 0 };
	struct profile_freq pf;

	cfg.k = PROFILE_FREQ_UNIT + 1u;
	zassert_equal(-EINVAL, profile_freq_init(&pf, &cfg));

	cfg.k = (uint16_t)(PROFILE_FREQ_K_MAX + PROFILE_FREQ_UNIT);
	zassert_equal(-EINVAL, profile_freq_init(&pf, &cfg));

	cfg.k = PROFILE_FREQ_K_MAX;
	zassert_ok(profile_freq_init(&pf, &cfg));
}

/* ---------------------------------------------------------------------------
 * The descriptor, and the retry list
 * ---------------------------------------------------------------------------
 */

/*
 * After the move the node's two statements about its own frequency agree, which
 * profile_desc_encode() enforces and would otherwise refuse the whole set for.
 * Round-tripped through the wire rather than inspected in the struct, because
 * byte [6] and flag bit 1 are what a receiver actually reads.
 */
ZTEST(profile_freq, test_the_descriptor_after_a_move_announces_index_and_flag)
{
	struct profile_descriptor d;
	struct profile_descriptor back;
	uint8_t frames[PROFILE_TLM_MAX_FRAMES * PROFILE_TLM_FRAME_LEN];
	int n;

	memset(&d, 0, sizeof(d));
	d.version = PROFILE_TLM_VERSION;
	d.schema_id = 1u;
	d.period = 8192u;
	d.rf_index = PROFILE_TLM_RF_INDEX_DEFAULT;

	/* Before: home, and the flag clear. */
	n = profile_desc_encode(&d, frames, PROFILE_TLM_MAX_FRAMES);
	zassert_true(n > 0, "a home descriptor was refused: %d", n);
	zassert_ok(profile_desc_decode(frames, (uint8_t)n, &back));
	zassert_equal(PROFILE_TLM_RF_INDEX_DEFAULT, back.rf_index);
	zassert_equal(0u, back.flags & PROFILE_TLM_FLAG_OFF_RF57);

	/* After. */
	zassert_ok(profile_freq_apply(&d, PROFILE_FREQ_RF_MID));
	n = profile_desc_encode(&d, frames, PROFILE_TLM_MAX_FRAMES);
	zassert_true(n > 0, "an off-57 descriptor was refused: %d", n);
	zassert_ok(profile_desc_decode(frames, (uint8_t)n, &back));
	zassert_equal(PROFILE_FREQ_RF_MID, back.rf_index);
	zassert_not_equal(0u, back.flags & PROFILE_TLM_FLAG_OFF_RF57,
			  "the off-57 flag did not follow the index");

	/* And back again, which is the case a caller writes by hand and gets
	 * wrong: setting the index without clearing the flag. */
	zassert_ok(profile_freq_apply(&d, PROFILE_TLM_RF_INDEX_DEFAULT));
	n = profile_desc_encode(&d, frames, PROFILE_TLM_MAX_FRAMES);
	zassert_true(n > 0, "the return to home was refused: %d", n);
	zassert_ok(profile_desc_decode(frames, (uint8_t)n, &back));
	zassert_equal(0u, back.flags & PROFILE_TLM_FLAG_OFF_RF57);

	zassert_equal(-EINVAL, profile_freq_apply(&d, (uint8_t)(
						  PROFILE_TLM_RF_INDEX_MAX + 1u)));
}

/*
 * The bounded retry list. It is what makes an off-57 node re-acquirable without
 * touching the sweep, and the assertion that matters is that it is SHORT and
 * ends at home - a list that grew would be a frequency sweep with a different
 * name, which is the thing ADR 0007 refused for a second PHY.
 */
ZTEST(profile_freq, test_the_retry_list_is_bounded_and_ends_at_home)
{
	struct profile_freq_rx rx;
	uint8_t order[PROFILE_FREQ_REACQUIRE_MAX];
	uint8_t n;

	profile_freq_rx_init(&rx);

	/* A receiver that never heard anything: the defaults, then home. */
	n = profile_freq_reacquire_order(&rx, order, sizeof(order));
	zassert_equal(PROFILE_FREQ_N_DEFAULTS + 1u, n);
	zassert_equal(PROFILE_FREQ_RF_HOME, order[n - 1u],
		      "the retry list does not end at RF 57");

	/* One that was told where the node went tries there first. */
	(void)profile_freq_rx_slot(&rx, CANON_FREQ, sizeof(CANON_FREQ));
	n = profile_freq_reacquire_order(&rx, order, sizeof(order));
	zassert_equal(PROFILE_FREQ_RF_MID, order[0],
		      "the announced target is not tried first");
	zassert_true(n <= PROFILE_FREQ_REACQUIRE_MAX);
	zassert_equal(PROFILE_FREQ_RF_HOME, order[n - 1u]);

	/* Deduplicated: RF 26 is both the announced target and a default, and
	 * an entry listed twice is a window spent twice. */
	for (uint8_t i = 0u; i < n; i++) {
		for (uint8_t j = (uint8_t)(i + 1u); j < n; j++) {
			zassert_not_equal(order[i], order[j],
					  "RF %u appears twice", order[i]);
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
 * is no call from profile_freq.c into radiant_search.c, and no candidate list
 * the sweep consults. What this test defends is that the default has not
 * quietly become configurable from somewhere else.
 */
ZTEST(profile_freq, test_discovery_stays_on_rf_57_however_far_a_node_moves)
{
	static struct radiant_search s;
	struct radiant_search_cfg cfg;
	struct radiant_search_id_filter want;
	struct radiant_search_window w;
	struct profile_freq_cfg fcfg = { 0 };
	struct profile_freq pf;

	radiant_search_cfg_default(&cfg);
	zassert_equal(RADIANT_RF_INDEX_ANT_PLUS, cfg.rf_index,
		      "the search default left RF 57");

	zassert_ok(profile_freq_init(&pf, &fcfg));
	zassert_ok(profile_freq_begin(&pf, PROFILE_FREQ_RF_HIGH, 0u));
	(void)run(&pf, NULL, 4u * PROFILE_FREQ_K_DEFAULT, 0u, 0u, NULL);
	zassert_equal(PROFILE_FREQ_RF_HIGH, profile_freq_rf_index(&pf),
		      "the node did not actually move");

	fake_radio_reset();
	zassert_ok(radiant_search_init(&s, &cfg, NULL, NULL));
	memset(&want, 0, sizeof(want)); /* ANT's wildcard: zero is "any" */
	zassert_ok(radiant_search_begin(&s, 0u, RADIANT_SEARCH_MODE_ACQUIRE,
					&want, radiant_radio_now(),
					RADIANT_SEARCH_TIMEOUT_NONE));

	memset(&w, 0, sizeof(w));
	zassert_ok(radiant_search_window(&s, radiant_radio_now() + 1000u, &w));
	zassert_equal(RADIANT_RF_INDEX_ANT_PLUS, w.rf_index,
		      "a search window was armed on RF %u", w.rf_index);
	zassert_equal(radiant_frame_format(RADIANT_FRAME_CFG_SEARCH), w.fmt,
		      "the sweep is no longer the three-byte search format");

	(void)radiant_search_end(&s, 0u);
}
