/* SPDX-License-Identifier: Apache-2.0 */
/*
 * CONFIG_RADIANT_NRF_LOW_POWER on the MPSL build (power plan pass B6): the
 * gate's half. The backend stops its radio TIMER between timeslots and starts
 * it from the GRTC at each SIGNAL_START, which is sound only because MPSL
 * guarantees the crystal there and only if the gate calls it at the right
 * moments. The contract this suite pins:
 *
 *   - every request asks for MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED
 *     (radiant_gate_hfclk, compiled in both gate scenarios: the guarantee
 *     matters to every MPSL build, not only this one);
 *   - every SIGNAL_START calls radiant_nrf_gate_lp_slot_start() once, before
 *     on_grant() programmes anything, with the requested start for a timed
 *     request and 0 for the bootstrap and the elastic class;
 *   - every ACTION_END calls radiant_nrf_gate_lp_slot_end(false) once, inside
 *     the timeslot, as the very last backend call - after the hand-back, every
 *     radiant_radio_abort() and the RADIO vector clear, since
 *     radio_disable_now() is a no-op on a stopped TIMER;
 *   - an extension does not end anything;
 *   - the low-priority hand-backs (BLOCKED, SESSION_IDLE, SESSION_CLOSED) and
 *     gate_shutdown() call it with backstop true, which stops a TIMER an
 *     expiry-ended timeslot left running and is a no-op otherwise.
 *
 * radiant.backend.lp_mpsl tests what the backend does with the calls.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <mpsl_timeslot.h>
#include <nrf_errno.h>

#include "radiant_radio_nrf_gate.h"

#include "../../fake_radio.h"
#include "../fake_mpsl.h"
#include "../fake_swi.h"
#include "../gate_probe.h"

#define WORK_MS  20
#define LEAD_US  500000u
#define WIN_US   5000u
#define CHUNK_US 40000u
#define RUN_OFF  3u

static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	gate_shutdown();
	k_msleep(WORK_MS);
	fake_radio_bring_up();
	fake_mpsl_reset();
	fake_backend_reset();
	fake_swi_reset();
	gate_probe_reset_elastic();
	zassert_equal(RADIANT_RADIO_OK_RC, gate_init(), NULL);
	k_msleep(WORK_MS);
}

static void after(void *fixture)
{
	ARG_UNUSED(fixture);
	gate_shutdown();
	k_msleep(WORK_MS);
	gate_probe_reset_elastic();
}

static void bootstrap(void)
{
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_START), NULL);
	k_msleep(WORK_MS);
}

static void place(uint8_t prio, uint32_t win_us)
{
	radiant_time_t from = radiant_radio_now() + LEAD_US;

	zassert_equal(GATE_PENDING,
		      gate_acquire(GATE_OP_RX, from, from + win_us, 0u, prio),
		      "the window was refused");
	k_msleep(WORK_MS);
}

/* ---- The HFCLK configuration: both gate scenarios ---- */

ZTEST_SUITE(radiant_gate_hfclk, NULL, NULL, before, after, NULL);

/*
 * Risk: a request placed with a weaker HFCLK configuration (DEFAULT, which
 * lets MPSL start the timeslot on HFINT): the radio loses bit sync in that
 * timeslot, and with the low-power option the backend's TIMER is started on
 * an unsettled clock with nothing to say so.
 */
ZTEST(radiant_gate_hfclk, test_every_request_is_xtal_guaranteed)
{
	const mpsl_timeslot_request_t *rq = fake_mpsl_last_request();

	zassert_equal(MPSL_TIMESLOT_REQ_TYPE_EARLIEST, rq->request_type,
		      "the bootstrap is EARLIEST");
	zassert_equal(MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED,
		      rq->params.earliest.hfclk, "bootstrap");
	bootstrap();
	place(RADIANT_GATE_PRIO_HIGH, WIN_US);
	rq = fake_mpsl_last_request();
	zassert_equal(MPSL_TIMESLOT_REQ_TYPE_NORMAL, rq->request_type, NULL);
	zassert_equal(MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED,
		      rq->params.normal.hfclk, "tracked window");
	/* Answer it (one request per session), then place the elastic class. */
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_START), NULL);
	gate_release();
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
	k_msleep(WORK_MS);
	place(RADIANT_GATE_PRIO_NORMAL, CHUNK_US);
	rq = fake_mpsl_last_request();
	zassert_equal(MPSL_TIMESLOT_REQ_TYPE_NORMAL, rq->request_type, NULL);
	zassert_equal(MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED,
		      rq->params.normal.hfclk, "elastic chunk");
}

#if defined(GATE_LP)

/* ---- The low-power hooks: radiant.gate.lp only ---- */

ZTEST_SUITE(radiant_gate_lp, NULL, NULL, before, after, NULL);

/* A grant whose on_grant() took the radio. */
static void take_grant(uint8_t prio, uint32_t win_us)
{
	place(prio, win_us);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_START), NULL);
	zassert_true(fake_backend()->on_grant > 0u, "on_grant() was not called");
}

/* Deliver a high-priority signal that ends the timeslot and check the
 * release came once, last, inside it. */
static void end_and_check(uint32_t signal, bool aborts, const char *what)
{
	const struct fake_backend_counts *b = fake_backend();
	const uint32_t ends = b->lp_end;
	const uint32_t aborts_before = fake_radio_abort_calls();

	zassert_true(b->lp_awake, "%s: the TIMER was not running", what);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END, fake_mpsl_signal(signal),
		      "%s did not end the timeslot", what);
	zassert_equal(ends + 1u, b->lp_end, "%s: slot_end not called once", what);
	zassert_equal(0u, b->lp_end_backstop, "%s: a funnel end is no backstop", what);
	zassert_equal(0u, b->lp_end_outside, "%s: slot_end outside the timeslot", what);
	zassert_false(b->lp_awake, "%s left the TIMER running", what);
	zassert_equal(b->lp_start, b->lp_releases,
		      "%s: STARTs %u, releases %u", what, b->lp_start, b->lp_releases);
	zassert_true(b->lp_end_seq > b->unpend_seq,
		     "%s stopped the TIMER before the RADIO vector clear", what);
	zassert_true(b->lp_end_seq > b->grant_end_seq,
		     "%s stopped the TIMER before the hand-back", what);
	zassert_equal(fake_radio_abort_calls(), b->lp_end_abort_calls,
		      "%s aborted after the TIMER stopped", what);
	if (aborts) {
		zassert_true(b->lp_end_abort_calls > aborts_before,
			     "%s did not abort", what);
	}
	zassert_equal(0u, b->lp_start_awake, "%s: a START found the TIMER running", what);
	zassert_equal(0u, fake_mpsl_viol_end_when_ended(), NULL);
	zassert_equal(0u, fake_mpsl_viol_action_no_slot(), NULL);
	zassert_equal(0u, fake_mpsl_viol_low_prio_action(), NULL);
	k_msleep(WORK_MS);
}

/*
 * Risk: the bootstrap timeslot, which never takes the radio, still has its
 * START and END bracket the TIMER; if either were skipped the pairing every
 * later timeslot relies on would be off by one from boot.
 */
ZTEST(radiant_gate_lp, test_bootstrap_is_bracketed_and_untimed)
{
	const struct fake_backend_counts *b = fake_backend();

	bootstrap();
	zassert_equal(1u, b->lp_start, NULL);
	zassert_equal(0u, b->lp_last_r_start, "the bootstrap has no requested start");
	zassert_equal(1u, b->lp_releases, NULL);
	zassert_false(b->lp_awake, NULL);
	zassert_equal(0u, b->on_grant, NULL);
}

/*
 * Risk: the TIMER started after on_grant() programmed the radio against it,
 * or with the wrong start for the cross-check.
 */
ZTEST(radiant_gate_lp, test_timed_start_wakes_before_grant)
{
	const struct fake_backend_counts *b = fake_backend();
	struct gate_probe p;

	bootstrap();
	place(RADIANT_GATE_PRIO_HIGH, WIN_US);
	gate_probe_read(&p);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_START), NULL);
	zassert_equal(2u, b->lp_start, NULL);
	zassert_true(b->lp_awake, NULL);
	zassert_not_equal(0u, b->lp_last_r_start, NULL);
	zassert_equal(p.r_start, b->lp_last_r_start,
		      "the cross-check gets the start the gate asked for");
	zassert_true(b->lp_start_seq < b->grant_seq,
		     "the TIMER started after on_grant() programmed against it");
	gate_release();
	end_and_check(MPSL_TIMESLOT_SIGNAL_TIMER0, false, "TIMER0 after a release");
}

/* Risk: the elastic class, whose placement is skewed and grown, is
 * cross-checked as if its start were a promise. */
ZTEST(radiant_gate_lp, test_elastic_start_is_untimed)
{
	const struct fake_backend_counts *b = fake_backend();

	bootstrap();
	place(RADIANT_GATE_PRIO_NORMAL, CHUNK_US);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_START), NULL);
	zassert_equal(0u, b->lp_last_r_start, NULL);
	zassert_true(b->lp_awake, NULL);
	gate_release();
	end_and_check(MPSL_TIMESLOT_SIGNAL_TIMER0, false, "elastic TIMER0 after a release");
}

/* Risk: one of the ACTION_END paths misses the funnel, so the TIMER keeps
 * running into the gap on whatever clock MPSL leaves. */
ZTEST(radiant_gate_lp, test_every_end_path_releases_last)
{
	bootstrap();
	take_grant(RADIANT_GATE_PRIO_HIGH, WIN_US);
	end_and_check(MPSL_TIMESLOT_SIGNAL_TIMER0, false, "the TIMER0 backstop");

	take_grant(RADIANT_GATE_PRIO_HIGH, WIN_US);
	fake_backend_release_in_radio_irq(true);
	end_and_check(MPSL_TIMESLOT_SIGNAL_RADIO, false, "the RADIO signal");
	fake_backend_release_in_radio_irq(false);

	place(RADIANT_GATE_PRIO_HIGH, WIN_US);
	fake_backend_release_in_grant(true);
	{
		/* START ends its own timeslot: its START is the wake. */
		const struct fake_backend_counts *b = fake_backend();
		const uint32_t ends = b->lp_end;

		zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
			      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_START), NULL);
		zassert_equal(ends + 1u, b->lp_end, NULL);
		zassert_false(b->lp_awake, NULL);
		zassert_true(b->lp_end_seq > b->unpend_seq, NULL);
		zassert_equal(b->lp_start, b->lp_releases, NULL);
		k_msleep(WORK_MS);
	}
	fake_backend_release_in_grant(false);

	/* An orphan: the operation forgotten before its START. */
	place(RADIANT_GATE_PRIO_HIGH, WIN_US);
	gate_forget_pending();
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_START), NULL);
	zassert_false(fake_backend()->lp_awake, "the orphan's TIMER left running");
	zassert_equal(fake_backend()->lp_start, fake_backend()->lp_releases, NULL);
	k_msleep(WORK_MS);
}

/*
 * Risk: an extension (the elastic class grows a step at a time) is taken as an
 * end and stops the TIMER under a live scan; and EXTEND_FAILED, which aborts,
 * stops it before the abort's radio_disable_now() has run.
 */
ZTEST(radiant_gate_lp, test_extension_keeps_awake_and_failure_releases_after_abort)
{
	const struct fake_backend_counts *b = fake_backend();

	bootstrap();
	take_grant(RADIANT_GATE_PRIO_NORMAL, CHUNK_US);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
	zassert_true(b->lp_awake, "an extension is not an end");
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_EXTEND_SUCCEEDED), NULL);
	zassert_true(b->lp_awake, NULL);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
	end_and_check(MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED, true, "EXTEND_FAILED");

	/* A run of refusals turns the skip on: TIMER0 then ends without asking,
	 * and aborts. */
	for (uint32_t i = 1u; i < RUN_OFF; i++) {
		take_grant(RADIANT_GATE_PRIO_NORMAL, CHUNK_US);
		zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND,
			      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
		end_and_check(MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED, true, "EXTEND_FAILED");
	}
	take_grant(RADIANT_GATE_PRIO_NORMAL, CHUNK_US);
	end_and_check(MPSL_TIMESLOT_SIGNAL_TIMER0, true, "the ext-skip TIMER0");
}

/* The low-priority hand-backs and gate_shutdown(), with the timeslot's own
 * ACTION_END never returned (an expiry-ended timeslot). */
static void backstop_releases(uint32_t signal, const char *what)
{
	const struct fake_backend_counts *b = fake_backend();

	bootstrap();
	take_grant(RADIANT_GATE_PRIO_HIGH, WIN_US);
	if (signal == UINT32_MAX) {
		gate_shutdown();
	} else {
		zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
			      fake_mpsl_signal(signal), NULL);
	}
	zassert_true(b->lp_end_backstop > 0u, "%s: no backstop call", what);
	zassert_equal(1u, b->lp_backstop_releases,
		      "%s: the left-running TIMER was not stopped", what);
	zassert_false(b->lp_awake, NULL);
	zassert_equal(b->lp_start, b->lp_releases, NULL);
	k_msleep(WORK_MS);
}

/*
 * Risk: a timeslot that ended by expiry, past the ACTION_END funnel, leaves
 * the TIMER running until the next START, timing the gap on a clock MPSL may
 * have dropped. Each backstop path stops it.
 */
ZTEST(radiant_gate_lp, test_blocked_backstop_releases)
{
	backstop_releases(MPSL_TIMESLOT_SIGNAL_BLOCKED, "BLOCKED");
}

ZTEST(radiant_gate_lp, test_session_idle_backstop_releases)
{
	backstop_releases(MPSL_TIMESLOT_SIGNAL_SESSION_IDLE, "SESSION_IDLE");
}

ZTEST(radiant_gate_lp, test_session_closed_backstop_releases)
{
	backstop_releases(MPSL_TIMESLOT_SIGNAL_SESSION_CLOSED, "SESSION_CLOSED");
}

ZTEST(radiant_gate_lp, test_shutdown_backstop_releases)
{
	backstop_releases(UINT32_MAX, "gate_shutdown()");
}

/* Risk: the backstop after a normal END (SESSION_IDLE follows nearly every
 * timeslot) is counted as a missed release. */
ZTEST(radiant_gate_lp, test_idle_after_a_normal_end_releases_nothing)
{
	const struct fake_backend_counts *b = fake_backend();

	bootstrap();
	take_grant(RADIANT_GATE_PRIO_HIGH, WIN_US);
	gate_release();
	end_and_check(MPSL_TIMESLOT_SIGNAL_TIMER0, false, "TIMER0 after a release");
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_SESSION_IDLE), NULL);
	zassert_true(b->lp_end_backstop > 0u, NULL);
	zassert_equal(0u, b->lp_backstop_releases, NULL);
}

#endif /* GATE_LP */
