/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The suite for radiant_core/src/radiant_radio_nrf_gate_mpsl.c.
 *
 * Provenance: original clean-room work, written against
 * radiant_core/src/radiant_radio_nrf_gate.h, the file under test, and nrfxlib's
 * mpsl_timeslot.h. Nothing here derives from sdk-ant, from libant.a, or from any
 * adopter-gated document.
 *
 * ---------------------------------------------------------------------------
 * THE ONE INVARIANT, AND WHY IT IS WORTH A WHOLE APPLICATION
 * ---------------------------------------------------------------------------
 *
 * EXACTLY ONE radiant_nrf_gate_on_grant_end() PER GRANTED TIMESLOT, ON EVERY
 * EXIT.
 *
 * Not "at least one": a second call hands back a radio the controller has
 * already picked up. Not "at most one": a missed one leaves the 802.15.4
 * driver's WRITE-ONCE SUBSCRIBE_RXEN swapped out for the rest of the boot, and
 * the failure is a receiver that stops and never resumes with nothing printed
 * anywhere. That is the one residual risk of the whole arbitrated backend that
 * fails PERMANENTLY and SILENTLY, which is exactly the class of fault a soak
 * cannot be trusted to find - a soak proves only that the run it took did not
 * hit it.
 *
 * So every test here ends in assert_invariant(), which asserts three things
 * together:
 *
 *   stats.grant_end_calls == stats.granted   one per granted timeslot,
 *   g.hw_held == false                       and the radio is actually back,
 *   the fake backend's own count agrees      through the real entry point.
 *
 * The scenarios are one per path that can end a grant, because the fault mode
 * is a path that skips the hand-back rather than a path that does it wrongly -
 * and a path that is never taken is a path with no evidence either way.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS CANNOT DO, STATED HERE RATHER THAN LEFT TO BE ASSUMED
 * ---------------------------------------------------------------------------
 *
 * It cannot produce a real BLOCKED arriving late out of mpsl_low_priority_
 * process() against a real SoftDevice Controller, and it cannot produce a real
 * OVERSTAYED. Those are the soak's, and the soak is scored on the same counters
 * this suite reads. Neither substitutes for the other.
 *
 * It also does not reach radiant_radio_nrf.c: that file is the whole 2800-line
 * backend and it defines the same eight HAL entry points fake_radio.c does, so
 * the two cannot be in one image. radiant_radio_disable() is therefore covered
 * here by the property it depends on - see
 * test_release_returns_the_radio_before_disable_can_write_the_mask - and its own
 * ~0 narrowing is checked by reading radiant_radio_nrf.c:2479, not by a fake
 * that would pass whatever that line said.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <mpsl_timeslot.h>
#include <nrf_errno.h>

#include "radiant_radio_nrf_gate.h"

#include "../../fake_radio.h"
#include "../fake_mpsl.h"
#include "../gate_probe.h"

/*
 * How long to let the work queue run.
 *
 * Every MPSL call this file makes is deferred - mpsl_timeslot_request() is not
 * callable from a timeslot or from the RADIO interrupt - so a request is placed
 * by a work item and not by gate_acquire(). A test that asserted straight after
 * the acquire would be asserting that the work had NOT run yet.
 */
#define WORK_MS 20

/*
 * The window every scenario is built on.
 *
 * The lead is half a second for one reason: gate_acquire() arms a backstop timer
 * at (t_from - now) + DEADLINE_SLACK_US, and a scenario that took longer than
 * that would have its state changed underneath it by a denial nothing asked
 * for. Half a second of lead is 520 ms of backstop against tests that are tens
 * of milliseconds long.
 */
#define WIN_LEAD_US 500000u
#define WIN_LEN_US  5000u

/*
 * What the gate should ask MPSL for, and where it should put the compare that
 * ends the grant. Pinned rather than derived, because deriving them here from
 * the same constants the file uses would make this assertion a tautology:
 *
 *   length = (t_to + follow_on + TAIL_MARGIN) - (t_from - HEAD_MARGIN)
 *            + END_MARGIN
 *          = (5000 + 0 + 250) + 250 + 2000 = 7500
 *   compare = length - END_MARGIN = 5500
 *
 * If either number changes, a margin changed, and the tail margin is the one
 * this file's header calls the most likely field failure of the whole design.
 */
#define WIN_REQ_LEN_US 7500u
#define WIN_CC_US      5500u

/* ---------------------------------------------------------------------------
 * The fixture
 * ---------------------------------------------------------------------------
 */

static struct gate_probe p;

static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	/*
	 * The gate keeps a session across tests - session_open is a file static
	 * and gate_init() returns early when it is set - so the shutdown is what
	 * makes each test a fresh program run. It also drains any work the
	 * previous test left queued, which would otherwise land on this test's
	 * counters.
	 */
	gate_shutdown();
	k_msleep(WORK_MS);

	fake_radio_bring_up();
	fake_mpsl_reset();
	fake_backend_reset();

	zassert_equal(RADIANT_RADIO_OK_RC, gate_init(), "gate_init failed");
	zassert_true(fake_mpsl_is_open(), "no session was opened");

	/*
	 * THE BOOTSTRAP TIMESLOT IS A GRANTED TIMESLOT AND IT COUNTS.
	 *
	 * gate_init() spends one minimum-length EARLIEST slot on nothing but its
	 * own start time - see bootstrap_anchor(). It carries no operation, so
	 * it is the shortest path to the hand-back there is, and it is the first
	 * thing the invariant has to hold for. Every test therefore starts with
	 * granted == grant_end_calls == 1.
	 */
	k_msleep(WORK_MS);
	zassert_equal(1u, fake_mpsl_requests(), "the bootstrap was not placed");
	zassert_equal(MPSL_TIMESLOT_REQ_TYPE_EARLIEST,
		      fake_mpsl_last_request()->request_type,
		      "the first request of a session must be EARLIEST");
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_START),
		      "the bootstrap slot was not handed straight back");

	gate_probe_read(&p);
	zassert_true(p.anchor_valid, "the bootstrap left no anchor");
	zassert_equal(1u, p.grant_end_calls, "the bootstrap skipped the hand-back");
	zassert_false(p.hw_held, "the bootstrap kept the radio");
}

static void after(void *fixture)
{
	ARG_UNUSED(fixture);
	gate_shutdown();
	k_msleep(WORK_MS);
}

ZTEST_SUITE(radiant_gate, NULL, NULL, before, after, NULL);

/* ---------------------------------------------------------------------------
 * The invariant, and the two helpers every scenario is built from
 * ---------------------------------------------------------------------------
 */

static void assert_invariant(void)
{
	gate_probe_read(&p);

	zassert_equal(p.granted, p.grant_end_calls,
		      "%u granted timeslots but %u on_grant_end() calls: the "
		      "802.15.4 driver's SUBSCRIBE_RXEN restore was skipped",
		      p.granted, p.grant_end_calls);
	zassert_false(p.hw_held,
		      "the radio is still on loan with no grant to hold it");
	zassert_equal(p.grant_end_calls, fake_backend()->on_grant_end,
		      "the gate counted %u hand-backs and the backend saw %u",
		      p.grant_end_calls, fake_backend()->on_grant_end);

	/* The three faults MPSL answers with an assert and nothing else. */
	zassert_equal(0u, fake_mpsl_viol_low_prio_action(),
		      "an action was returned from a low-priority signal");
	zassert_equal(0u, fake_mpsl_viol_end_when_ended(),
		      "ACTION_END was returned twice for one timeslot (bug 14)");
	zassert_equal(0u, fake_mpsl_viol_action_no_slot(),
		      "an action was returned outside a timeslot");
	zassert_equal(0u, p.overstayed + p.invalid_return + p.unknown_signal,
		      "the gate recorded a signal it should never see");
}

/* Place a window and take the grant. Returns with the grant live. */
static void start_grant(void)
{
	radiant_time_t now = radiant_radio_now();
	radiant_time_t t_from = now + WIN_LEAD_US;
	uint32_t before_req = fake_mpsl_requests();

	zassert_equal(GATE_PENDING,
		      gate_acquire(GATE_OP_RX, t_from, t_from + WIN_LEN_US, 0u,
				   RADIANT_GATE_PRIO_HIGH),
		      "the window was refused");
	k_msleep(WORK_MS);
	zassert_equal(before_req + 1u, fake_mpsl_requests(),
		      "the request never reached MPSL");
	zassert_equal(MPSL_TIMESLOT_REQ_TYPE_NORMAL,
		      fake_mpsl_last_request()->request_type,
		      "a window after the bootstrap must be a NORMAL request");

	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_START),
		      "the grant was ended at once");

	gate_probe_read(&p);
	zassert_true(p.hw_held, "SIGNAL_START did not take the radio");
	zassert_true(p.granted_flag, "SIGNAL_START did not mark the grant");
}

/* ---------------------------------------------------------------------------
 * 1. The ordinary end
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_gate, test_normal_grant_ends_once_on_timer0)
{
	start_grant();

	gate_probe_read(&p);
	zassert_equal(WIN_REQ_LEN_US, fake_mpsl_last_request()->params.normal.length_us,
		      "the requested length moved: a margin changed");
	zassert_equal(WIN_REQ_LEN_US, p.granted_len_us, NULL);
	zassert_equal(WIN_CC_US, fake_mpsl_timer0.cc[0],
		      "the compare that ends the timeslot is not END_MARGIN_US "
		      "before its end");
	zassert_true((fake_mpsl_timer0.inten & NRF_TIMER_INT_COMPARE0_MASK) != 0u,
		     "the compare was written but never enabled");

	/* The grant runs out. Not elastic - a tracked window's length is the
	 * core's - so there is no extension and the timeslot simply ends. */
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0),
		      "TIMER0 did not end the timeslot");

	gate_probe_read(&p);
	zassert_equal(2u, p.granted, NULL);
	zassert_equal(2u, p.grant_end_calls,
		      "expected one hand-back per granted timeslot");
	zassert_false(p.granted_flag, NULL);
	zassert_true((fake_mpsl_timer0.inten & NRF_TIMER_INT_COMPARE0_MASK) == 0u,
		     "the compare was left armed on a peripheral that is no "
		     "longer ours");
	assert_invariant();
}

/*
 * The same invariant across two grants in a row, which is the shape a running
 * dongle is in for hours. grant_end_calls TRACKING granted is the property the
 * soak is scored on, and a bug that loses one hand-back per N grants would pass
 * every single-grant test in this file.
 */
ZTEST(radiant_gate, test_the_hand_back_tracks_the_grants)
{
	int i;

	for (i = 0; i < 3; i++) {
		start_grant();
		zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
			      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
		/* The session goes idle between windows, which is where a
		 * timeslot MPSL ended by its own length expiry would be caught.
		 * Here there is nothing to catch. */
		zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
			      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_SESSION_IDLE),
			      NULL);
	}

	gate_probe_read(&p);
	zassert_equal(4u, p.granted, "expected the bootstrap plus three windows");
	zassert_equal(4u, p.grant_end_calls, NULL);
	zassert_equal(0u, p.idle_disarm,
		      "SESSION_IDLE had to give the radio back, so a normal end "
		      "did not");
	assert_invariant();
}

/*
 * An operation that completes synchronously inside its own grant - an arm that
 * turns out to be unreachable delivers its own terminal - so gate_release() runs
 * from INSIDE radiant_nrf_gate_on_grant(), before SIGNAL_START has returned.
 * This is the `if (g.release_wanted)` branch of START, and it is the only path
 * where the hand-back and the ACTION_END happen in the same callback.
 */
ZTEST(radiant_gate, test_release_from_inside_the_grant_callback_ends_once)
{
	fake_backend_release_in_grant(true);

	{
		radiant_time_t now = radiant_radio_now();
		radiant_time_t t_from = now + WIN_LEAD_US;

		zassert_equal(GATE_PENDING,
			      gate_acquire(GATE_OP_RX, t_from,
					   t_from + WIN_LEN_US, 0u,
					   RADIANT_GATE_PRIO_HIGH), NULL);
		k_msleep(WORK_MS);
		zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
			      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_START),
			      "a grant released inside its own callback must end "
			      "there");
	}

	gate_probe_read(&p);
	zassert_equal(2u, p.grant_end_calls, NULL);
	zassert_equal(1u, fake_backend()->on_grant, NULL);
	assert_invariant();
}

/* ---------------------------------------------------------------------------
 * 2. gate_release() whose compare is lost
 *
 * BUG 9's failure, and the reason A1 exists. gate_release() clears g.granted
 * EARLY and deliberately, then relies on a TIMER0 compare to reach the disarm.
 * Both backstops that would have caught a lost compare used to be gated on
 * g.granted, which gate_release() had just cleared - so a lost compare meant
 * NOTHING ever ran on_grant_end(). Both branches of the compare arithmetic are
 * driven here, and in each the assertion is that the radio came back WITHOUT the
 * compare ever firing.
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_gate, test_release_already_inside_the_end_margin_still_hands_back)
{
	start_grant();

	/*
	 * The counter is already past the point where a fresh compare could be
	 * placed ahead of it (cc_backstop is WIN_CC_US), so gate_release() takes
	 * the else-branch: the compare that is armed is the right one and moving
	 * it can only lose.
	 */
	fake_mpsl_timer0.counter = WIN_CC_US;
	fake_mpsl_timer0.counter_step = 0u;

	gate_release();

	zassert_equal(1u, fake_mpsl_timer0.captures,
		      "the else-branch was not the one taken");
	zassert_equal(WIN_CC_US, fake_mpsl_timer0.cc[0],
		      "the compare was moved after all");

	gate_probe_read(&p);
	zassert_equal(2u, p.grant_end_calls,
		      "gate_release() left the hand-back to a compare");
	zassert_equal(1u, p.release_disarm, NULL);
	zassert_false(p.hw_held, NULL);
	zassert_false(p.granted_flag, "g.granted must be cleared early");

	/*
	 * NOW LOSE THE COMPARE ENTIRELY. No TIMER0 signal ever arrives; the
	 * timeslot ends by expiry, which signals nothing this file handles, and
	 * the session goes idle. Before A1 this is where the receiver died: the
	 * backstop was gated on g.granted, which is false three lines up.
	 */
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_SESSION_IDLE), NULL);

	gate_probe_read(&p);
	zassert_equal(2u, p.grant_end_calls,
		      "the lost compare produced a second hand-back");
	zassert_equal(0u, p.idle_disarm,
		      "the backstop had to act, so gate_release() did not");
	assert_invariant();
}

ZTEST(radiant_gate, test_release_whose_compare_is_overtaken_restores_the_backstop)
{
	start_grant();

	/*
	 * The race BUG 9 is: the counter moves between writing CC0 and reading
	 * it back, so the compare is already in the past and the ONLY signal
	 * that would have ended this timeslot is gone. counter_step is what
	 * makes that reproducible rather than a coin toss - the second capture
	 * reads 200 against a compare placed at 130.
	 */
	fake_mpsl_timer0.counter = 100u;
	fake_mpsl_timer0.counter_step = 100u;

	gate_release();

	zassert_equal(2u, fake_mpsl_timer0.captures,
		      "the compare was not re-read after being written");
	zassert_equal(WIN_CC_US, fake_mpsl_timer0.cc[0],
		      "the backstop compare was left dangling in the past: late "
		      "is survivable, absent is not");
	zassert_true((fake_mpsl_timer0.inten & NRF_TIMER_INT_COMPARE0_MASK) != 0u,
		     NULL);

	gate_probe_read(&p);
	zassert_equal(2u, p.grant_end_calls,
		      "the hand-back was left riding on the lost compare");
	assert_invariant();

	/* And the backstop compare, when it does fire, ends the timeslot without
	 * handing the radio back a second time. */
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
	gate_probe_read(&p);
	zassert_equal(2u, p.grant_end_calls, NULL);
	assert_invariant();
}

/* ---------------------------------------------------------------------------
 * 3. EXTEND_FAILED - the yield point, self-labelled UNEXERCISED
 * ---------------------------------------------------------------------------
 */

static const struct radiant_pkt_format fmt_track = {
	.phy = RADIANT_PHY_1M_GFSK,
	.addr_len = 5u,
	.len_mode = RADIANT_LEN_FROM_BODY,
	.body_len = 0u,
	.len_offset = 1u,
	.len_bias = 0,
	.max_body_len = 26u,
	/* CRC-16/CCITT-FALSE, which is what the HAL requires a well-formed arm
	 * call to carry. The mock does not compute it; nothing in this suite
	 * depends on its value, only on the arm being accepted. */
	.crc = {
		.width_bits = 16u,
		.poly = 0x1021u,
		.init = 0xFFFFu,
		.xor_out = 0u,
		.reflect_in = false,
		.reflect_out = false,
		.cover_addr = true,
	},
};

static const struct radiant_rx_filter filt_track = {
	.addr = { 0xA6u, 0xC5u, 0x17u, 0x3Au, 0x0Bu },
	.addr_len = 5u,
};

/* An operation the HAL believes is armed, so that radiant_radio_abort() has
 * something to abort and its terminal is visible in the mock's counters. */
static void arm_an_operation(void)
{
	struct radiant_rx_req req;
	uint32_t op = 0u;

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_track;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = radiant_radio_now() + 10000u;
	req.t_close = req.t_open + WIN_LEN_US;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op),
		      "the mock refused the operation this test aborts");
}

ZTEST(radiant_gate, test_extend_failed_ends_once_and_denies_the_orphan)
{
	uint32_t aborted_before;

	start_grant();
	arm_an_operation();
	aborted_before = fake_radio_stats()->ev_aborted;

	/*
	 * The arbiter refused to grow the grant. It arrives with the grant still
	 * LIVE, which is what lets the window be closed cleanly and reported as
	 * a partial rather than as a denial.
	 */
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED),
		      "EXTEND_FAILED did not end the grant");

	gate_probe_read(&p);
	zassert_equal(1u, p.extends_failed, NULL);
	zassert_equal(2u, p.grant_end_calls,
		      "EXTEND_FAILED skipped the hand-back");
	zassert_equal(aborted_before + 1u, fake_radio_stats()->ev_aborted,
		      "the armed operation was not aborted, so the peripheral "
		      "was handed back still running");

	/*
	 * AND THE ORPHAN GETS ITS DENIAL, which this path used to omit on the
	 * reasoning that the abort above already delivers the terminal. It does -
	 * for an operation that is ARMED. A STAGED one has no terminal to abort
	 * and nothing is coming for it, which is the wedge end_housekeeping()
	 * exists to prevent: the core's single operation slot held for ever.
	 */
	k_msleep(WORK_MS);
	zassert_equal(1u, fake_backend()->on_denied,
		      "no denial was submitted for the staged operation");
	zassert_true(fake_backend()->finish_failed > 0u, NULL);
	gate_probe_read(&p);
	zassert_equal(1u, p.sent_after_grant,
		      "the denial did not come down the after-grant road");
	assert_invariant();
}

/* ---------------------------------------------------------------------------
 * 4. BLOCKED / CANCELLED arriving with the radio still on loan
 *
 * These two mean NO TIMESLOT STARTED, and the anchor rule depends on it - but
 * nothing enforced it, and the cost of being wrong is not a lost packet. A2
 * turned the assumption into a check with a counter behind it; if late_disarm is
 * ever non-zero on the bench it names a real fault that today presents only as a
 * dead receiver hours later.
 * ---------------------------------------------------------------------------
 */

static void late_low_priority_signal(uint32_t signal)
{
	start_grant();

	/* The state that is supposed to be impossible. */
	gate_probe_read(&p);
	zassert_true(p.hw_held, NULL);

	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(signal),
		      "an action was returned from a low-priority signal");

	gate_probe_read(&p);
	zassert_equal(1u, p.late_disarm,
		      "the stale loan was not noticed");
	zassert_equal(2u, p.grant_end_calls,
		      "the radio was not given back");
	zassert_false(p.granted_flag, NULL);
	assert_invariant();
}

ZTEST(radiant_gate, test_blocked_with_a_stale_loan_returns_the_radio)
{
	late_low_priority_signal(MPSL_TIMESLOT_SIGNAL_BLOCKED);
	gate_probe_read(&p);
	zassert_equal(1u, p.blocked, NULL);
}

ZTEST(radiant_gate, test_cancelled_with_a_stale_loan_returns_the_radio)
{
	late_low_priority_signal(MPSL_TIMESLOT_SIGNAL_CANCELLED);
	gate_probe_read(&p);
	zassert_equal(1u, p.cancelled, NULL);
}

/*
 * And the ordinary case, which must NOT count as late: a request refused before
 * any timeslot started. late_disarm has to stay zero here or it is useless as a
 * bench alarm.
 */
ZTEST(radiant_gate, test_an_ordinary_block_is_not_a_late_disarm)
{
	radiant_time_t now = radiant_radio_now();
	radiant_time_t t_from = now + WIN_LEAD_US;

	zassert_equal(GATE_PENDING,
		      gate_acquire(GATE_OP_RX, t_from, t_from + WIN_LEN_US, 0u,
				   RADIANT_GATE_PRIO_HIGH), NULL);
	k_msleep(WORK_MS);

	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_BLOCKED), NULL);

	gate_probe_read(&p);
	zassert_equal(1u, p.blocked, NULL);
	zassert_equal(0u, p.late_disarm,
		      "a block with no timeslot behind it was counted as a "
		      "fault");
	zassert_equal(1u, p.grant_end_calls, "only the bootstrap was granted");

	/* The staged operation is orphaned and must be told. */
	k_msleep(WORK_MS);
	zassert_equal(1u, fake_backend()->on_denied, NULL);
	gate_probe_read(&p);
	zassert_equal(1u, p.sent_blocked, NULL);
	assert_invariant();
}

/* ---------------------------------------------------------------------------
 * 5. SESSION_IDLE and SESSION_CLOSED delivered mid-grant
 *
 * The designed backstop for a timeslot MPSL ended by its own length expiry -
 * which signals nothing this file handles - so idle_disarm is allowed to be
 * non-zero in a healthy run, which is exactly why it is counted apart from
 * late_disarm.
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_gate, test_session_idle_mid_grant_returns_the_radio)
{
	start_grant();

	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_SESSION_IDLE), NULL);

	gate_probe_read(&p);
	zassert_equal(1u, p.idle_disarm, NULL);
	zassert_equal(0u, p.late_disarm,
		      "the designed backstop was counted as a fault");
	zassert_equal(2u, p.grant_end_calls, NULL);
	zassert_false(p.granted_flag, NULL);
	assert_invariant();
}

ZTEST(radiant_gate, test_session_closed_mid_grant_returns_the_radio)
{
	start_grant();

	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_SESSION_CLOSED), NULL);

	gate_probe_read(&p);
	zassert_equal(1u, p.idle_disarm, NULL);
	zassert_equal(2u, p.grant_end_calls, NULL);
	zassert_false(p.anchor_valid,
		      "a closed session's anchor is meaningless");
	assert_invariant();
}

/* ---------------------------------------------------------------------------
 * 6. Teardown from above: gate_shutdown() and the ordering
 *    radiant_radio_disable() depends on
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_gate, test_shutdown_mid_grant_returns_the_radio_once)
{
	start_grant();

	/* The session must not be closed with the 802.15.4 driver's
	 * SUBSCRIBE_RXEN still swapped out. */
	gate_shutdown();

	gate_probe_read(&p);
	zassert_equal(2u, p.grant_end_calls, NULL);
	zassert_equal(1u, p.idle_disarm, NULL);
	zassert_false(p.hw_held, NULL);
	zassert_equal(1u, fake_mpsl_closes(), NULL);
	zassert_false(fake_mpsl_is_open(), NULL);

	/* Idempotent: the second one must not hand back a radio the controller
	 * has already picked up. */
	gate_shutdown();
	gate_probe_read(&p);
	zassert_equal(2u, p.grant_end_calls, NULL);
	assert_invariant();
}

/*
 * radiant_radio_disable() (radiant_radio_nrf.c:2424) reaches gate_release()
 * through radiant_radio_abort() and THEN writes the shared RADIO interrupt mask.
 * The whole of A4's ordering claim is that the radio is already back by the time
 * that write happens - on both of gate_release()'s branches, including the
 * nothing-held early return, because the session stays open across a disable and
 * MPSL still owns the register.
 *
 * The mask write itself is in radiant_radio_nrf.c, which cannot be in this image
 * (it defines the same HAL entry points fake_radio.c does). What IS testable
 * here is the precondition it rests on, and it is the half that was left to
 * chance.
 */
ZTEST(radiant_gate, test_release_returns_the_radio_before_disable_can_write_the_mask)
{
	start_grant();

	gate_release();
	gate_probe_read(&p);
	zassert_false(p.hw_held,
		      "gate_release() returned with the radio still on loan, so "
		      "radiant_radio_disable() would write INTENCLR00 while the "
		      "802.15.4 driver's SUBSCRIBE_RXEN is still swapped out");
	zassert_equal(2u, p.grant_end_calls, NULL);

	/* The nothing-held early return, which is where the abort path arrives
	 * when there was no grant at all. */
	gate_release();
	gate_probe_read(&p);
	zassert_false(p.hw_held, NULL);
	zassert_equal(2u, p.grant_end_calls, NULL);
	assert_invariant();
}

/* ---------------------------------------------------------------------------
 * 7. Double release
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_gate, test_release_after_a_normal_terminal_does_not_end_twice)
{
	start_grant();

	/* The ordinary terminal: the core is finished, so the air goes back. */
	gate_release();
	gate_probe_read(&p);
	zassert_equal(2u, p.grant_end_calls, NULL);

	/* The compare it armed arrives and ends the timeslot. */
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);

	/*
	 * And then the abort path calls gate_release() without knowing - which
	 * is the documented way it is called, and lands on the branch where
	 * g.granted is already false and the radio is already back.
	 */
	gate_release();

	gate_probe_read(&p);
	zassert_equal(2u, p.grant_end_calls,
		      "the radio was handed back twice for one timeslot");
	assert_invariant();
}

/* ---------------------------------------------------------------------------
 * 8. ACTION_END returned twice - BUG 14
 *
 * TIMER0 and RADIO race at the end of a window and either can win; whichever
 * loses used to end an already-ended timeslot, and MPSL asserts on the second
 * action. One guard at the single return statement, not per branch, because the
 * branch that is about to be wrong is whichever one loses a race it cannot see.
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_gate, test_a_second_end_is_refused_by_the_ended_guard)
{
	start_grant();
	gate_release();

	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
	gate_probe_read(&p);
	zassert_true(p.ended, NULL);

	/*
	 * The loser of the race arrives. It still has to be HANDLED - the event
	 * needs consuming or the operation never gets its terminal - but it must
	 * not be ANSWERED.
	 */
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0),
		      "a second ACTION_END was returned for one timeslot");
	gate_probe_read(&p);
	zassert_equal(2u, p.grant_end_calls, NULL);
	assert_invariant();
}

/*
 * The RADIO signal is the other half of the same race: MPSL owns the vector
 * inside a grant and delivers the interrupt as a signal, so the handler is
 * re-entered by hand and the grant given back from there rather than from a
 * TIMER0 signal thirty microseconds later.
 */
ZTEST(radiant_gate, test_radio_signal_completing_the_operation_ends_once)
{
	start_grant();
	fake_backend_release_in_radio_irq(true);

	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_RADIO),
		      "the completed operation did not end its grant");
	zassert_equal(1u, fake_backend()->on_radio_irq, NULL);

	gate_probe_read(&p);
	zassert_equal(2u, p.grant_end_calls, NULL);

	/* And the TIMER0 compare that was still armed loses the race. */
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
	gate_probe_read(&p);
	zassert_equal(2u, p.grant_end_calls, NULL);
	assert_invariant();
}

/*
 * A fresh timeslot makes ACTION_END legal again, and it has to: the guard is
 * per grant and not per session. Without this, the guard that fixes bug 14 would
 * wedge the gate after its first window instead - which is the same symptom by
 * the opposite road.
 */
ZTEST(radiant_gate, test_the_ended_guard_is_cleared_by_the_next_grant)
{
	start_grant();
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_SESSION_IDLE), NULL);

	start_grant();
	gate_probe_read(&p);
	zassert_false(p.ended, "the second grant inherited the first's guard");
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0),
		      "the second grant could not be ended at all");

	gate_probe_read(&p);
	zassert_equal(3u, p.granted, NULL);
	zassert_equal(3u, p.grant_end_calls, NULL);
	assert_invariant();
}

/* ---------------------------------------------------------------------------
 * The elastic chain, because it is the one path that RETURNS FROM TIMER0
 * WITHOUT ENDING THE GRANT - and a hand-back on that return would be a radio
 * given away in the middle of a window it is still using.
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_gate, test_an_extension_does_not_hand_the_radio_back)
{
	radiant_time_t now = radiant_radio_now();
	radiant_time_t t_from = now + WIN_LEAD_US;

	/* GATE_OP_RX at PRIO_NORMAL is the elastic class: a scan chunk asks
	 * small and grows. */
	zassert_equal(GATE_PENDING,
		      gate_acquire(GATE_OP_RX, t_from, t_from + 40000u, 0u,
				   RADIANT_GATE_PRIO_NORMAL), NULL);
	k_msleep(WORK_MS);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_START), NULL);

	gate_probe_read(&p);
	zassert_true(p.granted_len_us < p.want_len_us,
		     "the elastic class asked for the whole thing up front");

	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0),
		      "the chain did not try to grow");
	gate_probe_read(&p);
	zassert_equal(1u, p.grant_end_calls,
		      "an extension attempt handed the radio back mid-window");
	zassert_true(p.hw_held, NULL);

	{
		uint32_t before_len = p.granted_len_us;

		zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
			      fake_mpsl_signal(
				      MPSL_TIMESLOT_SIGNAL_EXTEND_SUCCEEDED),
			      NULL);
		gate_probe_read(&p);
		zassert_equal(before_len + fake_mpsl_last_extend_us(),
			      p.granted_len_us, NULL);
		/*
		 * BUG 8: the compare must stay END_MARGIN_US ahead of the new
		 * end, initial grant and extended grant alike. It used
		 * TAIL_MARGIN_US on exactly one of the two paths, and the moment
		 * a window stopped extending it had 250 us to get ACTION_END
		 * back - which is the overstay the whole margin exists to
		 * prevent.
		 */
		zassert_equal(p.granted_len_us - 2000u, fake_mpsl_timer0.cc[0],
			      "the extended grant's compare is not "
			      "END_MARGIN_US before its end");
		zassert_equal(1u, p.grant_end_calls, NULL);
	}

	/* And the end of the chain still ends exactly once. */
	gate_release();
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
	gate_probe_read(&p);
	zassert_equal(2u, p.grant_end_calls, NULL);
	assert_invariant();
}

/* ---------------------------------------------------------------------------
 * BUG 15 - THE ANSWER THAT ARRIVES INSIDE THE CALL
 *
 * The only failure this suite has that is PERMANENT AND TOTAL rather than
 * permanent and silent. g.mpsl_owes means "MPSL has a request of ours and has
 * not answered it", and nothing this side of the API can clear it - a placed
 * request cannot be withdrawn. So a g.mpsl_owes that is true when nothing is
 * outstanding refuses every gate_acquire() for the rest of the boot with
 * den_owed, and the ANT radio is dead until the board is reset.
 *
 * The measured signature on the bench is arithmetic that cannot happen:
 *
 *   placed=3 granted=2 blocked=1 ... owed=2403 ... owes=1
 *
 * every placed request answered, and the flag still set. It got that way
 * because the flag was raised AFTER the request it describes, and the answer
 * arrived in between.
 *
 * These two tests are the two halves of it: the flag must survive an answer
 * that beats it, and the gate must go on working afterwards.
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_gate, test_a_block_answered_inside_the_request_does_not_wedge)
{
	radiant_time_t now = radiant_radio_now();
	radiant_time_t t_from = now + WIN_LEAD_US;

	fake_mpsl_answer_inline(MPSL_TIMESLOT_SIGNAL_BLOCKED, 1u);

	zassert_equal(GATE_PENDING,
		      gate_acquire(GATE_OP_RX, t_from, t_from + WIN_LEN_US, 0u,
				   RADIANT_GATE_PRIO_HIGH),
		      "the window was refused before it was ever placed");
	k_msleep(WORK_MS);

	gate_probe_read(&p);
	zassert_equal(2u, p.placed, "the request never reached MPSL");
	zassert_equal(1u, p.blocked, "the inline answer was not delivered");
	zassert_equal(1u, p.answered_inline,
		      "the gate did not notice that it had been answered from "
		      "inside mpsl_timeslot_request()");
	/* THE ASSERTION THE WHOLE FILE IS FOR. */
	zassert_false(p.mpsl_owes,
		      "every placed request has been answered and the gate "
		      "still believes MPSL owes it one: every future acquire "
		      "is now den_owed and the radio is dead (bug 15)");
	zassert_false(p.pending, NULL);

	/* And the orphaned operation was still told, by the ordinary road. */
	zassert_equal(1u, fake_backend()->on_denied, NULL);
	gate_probe_read(&p);
	zassert_equal(1u, p.sent_blocked, NULL);
	assert_invariant();
}

/*
 * THE RECOVERY, WHICH IS THE HALF THAT WAS ACTUALLY LOST. A refusal is an
 * ordinary event; what made this a wedge rather than a hiccup is that the NEXT
 * window could never be placed. So the test is not "owes is false", it is "the
 * gate takes the next window and gets a grant".
 */
ZTEST(radiant_gate, test_the_gate_still_places_after_an_inline_block)
{
	radiant_time_t now = radiant_radio_now();
	radiant_time_t t_from = now + WIN_LEAD_US;

	fake_mpsl_answer_inline(MPSL_TIMESLOT_SIGNAL_BLOCKED, 1u);
	zassert_equal(GATE_PENDING,
		      gate_acquire(GATE_OP_RX, t_from, t_from + WIN_LEN_US, 0u,
				   RADIANT_GATE_PRIO_HIGH), NULL);
	k_msleep(WORK_MS);

	gate_probe_read(&p);
	zassert_equal(0u, p.den_owed, NULL);

	/* The next window, placed and granted the ordinary way. start_grant()
	 * asserts every step of that for us. */
	start_grant();
	gate_probe_read(&p);
	zassert_equal(0u, p.den_owed,
		      "the acquire after an inline refusal was refused because "
		      "MPSL was still thought to owe an answer (bug 15)");
	zassert_equal(2u, p.granted, NULL);

	gate_release();
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
	assert_invariant();
}

/*
 * CANCELLED ARRIVING THE SAME WAY. It is the other refusal that clears the
 * flag, it is delivered in the same low-priority context, and a fix that only
 * covered BLOCKED would leave the identical wedge one signal along.
 *
 * SESSION_IDLE is deliberately NOT tested this way, and that is a statement
 * about the model rather than about the coverage: IDLE means "no request is
 * outstanding", so MPSL cannot say it about the request currently being placed.
 * Injecting it there produces a legitimately-owed flag - the gate re-places on
 * IDLE, and that second request really is unanswered - which would be a test
 * asserting that correct behaviour is a bug.
 */
ZTEST(radiant_gate, test_a_cancel_answered_inside_the_request_does_not_wedge)
{
	radiant_time_t now = radiant_radio_now();
	radiant_time_t t_from = now + WIN_LEAD_US;

	fake_mpsl_answer_inline(MPSL_TIMESLOT_SIGNAL_CANCELLED, 1u);
	zassert_equal(GATE_PENDING,
		      gate_acquire(GATE_OP_RX, t_from, t_from + WIN_LEN_US, 0u,
				   RADIANT_GATE_PRIO_HIGH), NULL);
	k_msleep(WORK_MS);

	gate_probe_read(&p);
	zassert_equal(1u, p.cancelled, NULL);
	zassert_equal(1u, p.answered_inline, NULL);
	zassert_false(p.mpsl_owes, "bug 15, by way of CANCELLED");
	zassert_equal(0u, p.den_owed, NULL);
	assert_invariant();
}

/*
 * AND THE ORDINARY ORDER IS UNCHANGED, which is what says the fix is an
 * ordering correction and not a new behaviour. An answer that arrives after the
 * call returns must still clear the flag, and answered_inline must stay zero -
 * otherwise the counter that is about to be read off a bench log means nothing.
 */
ZTEST(radiant_gate, test_an_answer_after_the_call_still_clears_the_flag)
{
	radiant_time_t now = radiant_radio_now();
	radiant_time_t t_from = now + WIN_LEAD_US;

	zassert_equal(GATE_PENDING,
		      gate_acquire(GATE_OP_RX, t_from, t_from + WIN_LEN_US, 0u,
				   RADIANT_GATE_PRIO_HIGH), NULL);
	k_msleep(WORK_MS);

	gate_probe_read(&p);
	zassert_true(p.mpsl_owes, "the placed request is not being tracked");
	zassert_equal(0u, p.answered_inline, NULL);

	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_BLOCKED), NULL);
	gate_probe_read(&p);
	zassert_false(p.mpsl_owes, NULL);
	zassert_equal(0u, p.answered_inline, NULL);
	assert_invariant();
}

/*
 * A REQUEST THAT WAS NOT PLACED OWES NOTHING, and the pre-arm must be undone
 * for it. -NRF_EAGAIN is the common one: it is a "not yet", the request is kept
 * for SESSION_IDLE to place, and leaving the flag raised here would refuse the
 * very acquire that is meant to retry.
 */
ZTEST(radiant_gate, test_an_unplaced_request_leaves_nothing_owed)
{
	radiant_time_t now = radiant_radio_now();
	radiant_time_t t_from = now + WIN_LEAD_US;

	fake_mpsl_force_request_rc(-NRF_EAGAIN, 1u);
	zassert_equal(GATE_PENDING,
		      gate_acquire(GATE_OP_RX, t_from, t_from + WIN_LEN_US, 0u,
				   RADIANT_GATE_PRIO_HIGH), NULL);
	k_msleep(WORK_MS);

	gate_probe_read(&p);
	zassert_equal(1u, p.refused_eagain, NULL);
	zassert_equal(1u, p.placed, "only the bootstrap was placed");
	zassert_false(p.mpsl_owes,
		      "a request MPSL never took is being waited on");
	zassert_true(p.pending, "the -EAGAIN request was not kept");
	assert_invariant();
}
