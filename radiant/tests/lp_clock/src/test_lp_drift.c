/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work, written against
 * radiant/src/radiant_radio_nrf_lp.h.
 *
 * 100 000 sleep/wake cycles of a 4 Hz master against a physical model of the
 * two crystals, to show that the low-power timebase's arithmetic does not
 * drift: the error at the end is the error after the first cycle.
 *
 * THE MODEL. Physical time is counted in "pu", 2^-20 us. The HF TIMER is the
 * physical reference (1 us = 2^20 pu exactly). The LF crystal ticks every
 * tick_pu, its nominal period plus a ppm offset. Each cycle:
 *
 *   awake    the TIMER, started (or, at boot, free-running) at
 *            seg_start_pu, reads seg_base_us + whole us since then. A wake
 *            starts it up to 1 us after its LF edge (anchor jitter).
 *   release  at an LF edge after the cycle's operation; the capture reads
 *            the TIMER up to 1 us after the edge (anchor jitter again).
 *   asleep   the LF conversion from the release anchor.
 *   wake     at the compare radiant_lp_wake_compare() chose for the next
 *            operation minus the wake lead.
 *
 * THE REFERENCE is the timebase locked to the LF crystal: from one anchor,
 * ref_q(k) = ref_q0 + (k - ref_k0) * num for every LF tick k. That is what the
 * design promises over short awake segments ("gaps run at the LF rate, error
 * per transition ~1 us, no accumulation"). A long awake segment (a 1 s linger
 * every 1000th cycle here) genuinely ran on the HF crystal, so its release
 * takes the TIMER and the reference re-locks there; that is the only place it
 * moves.
 *
 * Asserted over every cycle: asleep readings equal the reference EXACTLY
 * (q units); awake readings are within 3 us of the reference interpolated at
 * the HF rate from the wake edge (the wake's whole-us floor, the start jitter
 * and the TIMER's own floor, each under 1 us); a release moves the timebase by
 * at most RADIANT_LP_RELEASE_TOL_US; every handed-out value is monotonic; a
 * wake is never later than asked and less than one tick early; the sleep
 * decision says yes in every gap.
 *
 * Two negative controls show the simulation would see the failures it exists
 * for: dropping the wake's carried fraction, and anchoring every release on
 * the TIMER's capture. Both drift by milliseconds over the same run.
 *
 * The 24-bit RTC counter wraps ~50 times in a run and the counter values are
 * handed over with junk above the counter width, as an extended software
 * counter would; the GRTC run starts 100 000 ticks below its 52-bit wrap.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "radiant_radio_nrf_lp.h"

#define PU        (INT64_C(1) << 20)  /* pu per us */
#define CYCLES    100000u
#define PERIOD_US 246277u             /* ANT+ period 8070/32768 s, in us */
#define LEAD_US   1700u               /* ~HFXO start-up + arming margin */
#define KEEP_US   3000u

struct sim_cfg {
	uint32_t num;
	uint8_t  shift;
	uint8_t  width;
	int64_t  tick_pu;       /* true LF period, pu */
	uint64_t k_offset;      /* counter value at physical tick 0 */
	uint64_t junk;          /* bits above the width the caller passes */
	uint32_t min_lead;      /* compare lead, ticks */
	uint32_t tol_us;
	uint32_t linger_every;  /* 0 = never */
	bool     drop_fraction; /* negative control */
};

struct sim_result {
	uint64_t max_asleep_err_q;   /* |asleep - ref|, q units, whole run */
	uint64_t early_asleep_err_q; /* the same over the first 1000 cycles */
	int64_t  max_awake_err_pu;   /* |awake - ref|, pu */
	uint64_t max_back_us;        /* largest raw step back at a release */
	uint64_t max_fwd_us;         /* largest raw step forward at a release */
	uint32_t lf_releases;
	uint32_t timer_releases;
	uint32_t lingers;
	uint32_t late_wakes;
	uint32_t loose_wakes;        /* a whole tick or more earlier than needed */
	uint32_t refused_sleeps;
	uint32_t unsafe_compares;
	uint32_t bad_wake_rc;
	uint32_t mono_breaks;
};

static uint32_t rng;

static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static uint64_t rnd_upto(uint64_t n) /* uniform-ish in [0, n] */
{
	uint64_t r = ((uint64_t)rnd() << 32) | rnd();

	return r % (n + 1u);
}

static const struct sim_cfg *cfg;
static struct radiant_lp_tb tb;

/* Physical time of LF tick k (k counts from the start of the run). */
static int64_t lf_edge_pu(uint64_t k)
{
	return (int64_t)k * cfg->tick_pu;
}

/* The counter value the hardware would show at tick k. */
static uint64_t counter(uint64_t k)
{
	return ((cfg->k_offset + k) & tb.mask) | cfg->junk;
}

static uint64_t abs_diff(uint64_t a, uint64_t b)
{
	return a > b ? a - b : b - a;
}

static void run_sim(const struct sim_cfg *config, uint32_t seed,
		    struct sim_result *r)
{
	struct radiant_lp_clock c;
	uint64_t k_w = 0;          /* the awake segment's start edge */
	int64_t seg_start_pu = 0;  /* when its TIMER started counting */
	uint64_t seg_base_us = 0;  /* the fold base the backend loaded */
	uint64_t op_us = 50000u;   /* this segment's operation */
	uint64_t ref_k0 = 0, ref_q0 = 0;
	uint64_t last_out = 0;
	const uint64_t fracmask = (UINT64_C(1) << config->shift) - 1u;

	cfg = config;
	rng = seed;
	memset(r, 0, sizeof(*r));
	zassert_true(radiant_lp_tb_init(&tb, config->num, config->shift,
					config->width));
	radiant_lp_clock_init(&c);

	for (uint32_t i = 0; i < CYCLES; i++) {
		bool linger = config->linger_every != 0u &&
			      (i % config->linger_every) ==
			      config->linger_every - 1u;
		uint64_t tail_us = linger ? 1000000u : 500u + rnd_upto(3500u);
		/* A linger stays awake through the next four operations. */
		uint64_t next_op = op_us + (linger ? 5u : 1u) * PERIOD_US;
		uint64_t k_r, k_w2, k_s, cmp, c_us, now_us, raw, out;
		int64_t target_pu, p, ref_pu, err_pu;
		enum radiant_lp_wake_rc rc;
		bool took_lf;

		/* Release edge: the first tick at which the awake TIMER has
		 * passed the operation plus its tail.
		 */
		target_pu = seg_start_pu +
			    (int64_t)(op_us + tail_us - seg_base_us) * PU;
		k_r = (uint64_t)((target_pu + cfg->tick_pu - 1) / cfg->tick_pu);
		if (k_r <= k_w) {
			k_r = k_w + 1u;
		}

		/* An awake reading somewhere in the segment, against the
		 * reference interpolated at the HF rate from the wake edge.
		 * Not for the boot segment, which no wake started.
		 */
		p = seg_start_pu +
		    (int64_t)rnd_upto((uint64_t)(lf_edge_pu(k_r) - seg_start_pu));
		now_us = seg_base_us + (uint64_t)((p - seg_start_pu) / PU);
		if (i > 0u) {
			uint64_t ref_q_w = ref_q0 + (k_w - ref_k0) * cfg->num;

			ref_pu = (int64_t)(ref_q_w << (20u - cfg->shift)) +
				 (p - lf_edge_pu(k_w));
			err_pu = (int64_t)now_us * PU - ref_pu;
			if (err_pu < 0) {
				err_pu = -err_pu;
			}
			if (err_pu > r->max_awake_err_pu) {
				r->max_awake_err_pu = err_pu;
			}
		}
		out = radiant_lp_clock_now(&tb, &c, false, now_us, 0u);
		r->mono_breaks += out < last_out;
		last_out = out;

		/* The release: the capture lands up to 1 us after the edge. */
		c_us = seg_base_us +
		       (uint64_t)((lf_edge_pu(k_r) + (int64_t)rnd_upto(PU - 1) -
				   seg_start_pu) / PU);
		if (!radiant_lp_should_sleep(c_us, next_op, LEAD_US, KEEP_US,
					     0u)) {
			r->refused_sleeps++;
		}
		out = radiant_lp_clock_now(&tb, &c, false, c_us, 0u);
		r->mono_breaks += out < last_out;
		last_out = out;
		took_lf = radiant_lp_release(&tb, &c, counter(k_r), c_us,
					     config->tol_us, NULL);
		if (took_lf) {
			r->lf_releases++;
		} else {
			r->timer_releases++;
		}
		raw = radiant_lp_asleep_us(&tb, &c, counter(k_r));
		if (raw < c_us) {
			r->max_back_us = MAX(r->max_back_us, c_us - raw);
		} else {
			r->max_fwd_us = MAX(r->max_fwd_us, raw - c_us);
		}
		if (i == 0u || linger) {
			/* Boot, or a segment that ran on the HF crystal:
			 * the reference re-locks here.
			 */
			r->lingers += linger;
			ref_k0 = k_r;
			ref_q0 = c.anchor_q;
		}

		/* The wake compare for the next operation. */
		rc = radiant_lp_wake_compare(&tb, &c,
					     radiant_lp_wake_at(next_op, LEAD_US),
					     UINT64_MAX, &cmp);
		r->bad_wake_rc += rc != RADIANT_LP_WAKE_OK;
		if (!radiant_lp_compare_in_time(&tb, counter(k_r + 1u), cmp,
						config->min_lead)) {
			r->unsafe_compares++;
		}
		{
			uint64_t want_q = radiant_lp_wake_at(next_op, LEAD_US)
					  << cfg->shift;
			uint64_t got_q = radiant_lp_asleep_q(&tb, &c, cmp);

			r->late_wakes += got_q > want_q;
			r->loose_wakes += got_q + cfg->num <= want_q;
		}
		k_w2 = k_r + radiant_lp_ticks_since(&tb, counter(k_r), cmp);

		/* An asleep reading somewhere in the gap: exactly the LF
		 * reference.
		 */
		k_s = k_r + rnd_upto(k_w2 - k_r);
		{
			uint64_t got = radiant_lp_asleep_q(&tb, &c, counter(k_s));
			uint64_t want = ref_q0 + (k_s - ref_k0) * cfg->num;
			uint64_t e = abs_diff(got, want);

			r->max_asleep_err_q = MAX(r->max_asleep_err_q, e);
			if (i < 1000u) {
				r->early_asleep_err_q =
					MAX(r->early_asleep_err_q, e);
			}
		}
		out = radiant_lp_clock_now(&tb, &c, true, 0u, counter(k_s));
		r->mono_breaks += out < last_out;
		last_out = out;

		/* Wake: the compare starts the cleared TIMER up to 1 us late. */
		seg_base_us = radiant_lp_wake(&tb, &c, counter(k_w2), true);
		if (config->drop_fraction) {
			c.anchor_q &= ~fracmask;
			c.awake_frac = 0u;
		}
		k_w = k_w2;
		seg_start_pu = lf_edge_pu(k_w) + (int64_t)rnd_upto(PU - 1);
		op_us = next_op;
	}
}

static void print_result(const char *name, const struct sim_result *r)
{
	TC_PRINT("%s: asleep err %u q (first 1000 cycles %u q), awake err %u pu, "
		 "release step back %u us fwd %u us, releases lf %u timer %u\n",
		 name, (uint32_t)MIN(r->max_asleep_err_q, UINT32_MAX),
		 (uint32_t)MIN(r->early_asleep_err_q, UINT32_MAX),
		 (uint32_t)r->max_awake_err_pu,
		 (uint32_t)MIN(r->max_back_us, UINT32_MAX),
		 (uint32_t)MIN(r->max_fwd_us, UINT32_MAX),
		 r->lf_releases, r->timer_releases);
}

static void assert_clean(const struct sim_result *r, uint32_t lingers)
{
	zassert_equal(r->max_asleep_err_q, 0u,
		      "asleep reading left the LF reference by %u q",
		      (uint32_t)r->max_asleep_err_q);
	zassert_equal(r->early_asleep_err_q, 0u);
	zassert_true(r->max_awake_err_pu < 3 * PU,
		     "awake reading off by %d pu", (int)r->max_awake_err_pu);
	zassert_true(r->max_back_us <= RADIANT_LP_RELEASE_TOL_US,
		     "release stepped back %u us", (uint32_t)r->max_back_us);
	zassert_true(r->max_fwd_us <= RADIANT_LP_RELEASE_TOL_US,
		     "release stepped forward %u us", (uint32_t)r->max_fwd_us);
	zassert_equal(r->lingers, lingers);
	/* Boot and every linger take the TIMER; every other release the LF
	 * image of its edge.
	 */
	zassert_equal(r->timer_releases, 1u + lingers,
		      "%u releases fell back to the TIMER", r->timer_releases);
	zassert_equal(r->lf_releases, CYCLES - 1u - lingers);
	zassert_equal(r->late_wakes, 0u);
	zassert_equal(r->loose_wakes, 0u);
	zassert_equal(r->refused_sleeps, 0u);
	zassert_equal(r->unsafe_compares, 0u);
	zassert_equal(r->bad_wake_rc, 0u);
	zassert_equal(r->mono_breaks, 0u);
}

ZTEST_SUITE(radiant_lp_drift, NULL, NULL, NULL, NULL, NULL);

static struct sim_result res;

/* RTC, LF crystal +50 ppm: the nominal tick is 15625 << 11 = 32000000 pu, so
 * one ppm is 32 pu.
 */
ZTEST(radiant_lp_drift, test_rtc_lf_fast_no_drift)
{
	static const struct sim_cfg c = {
		.num = RADIANT_LP_RTC_NUM, .shift = RADIANT_LP_RTC_SHIFT,
		.width = RADIANT_LP_RTC_WIDTH,
		.tick_pu = 32000000 + 50 * 32,
		.k_offset = 0xFFFFFFu - 1000u,
		.junk = UINT64_C(0x5A) << 32,
		.min_lead = RADIANT_LP_RTC_MIN_LEAD_TICKS,
		.tol_us = RADIANT_LP_RELEASE_TOL_US,
		.linger_every = 1000u,
	};

	run_sim(&c, 0x1234567u, &res);
	print_result(__func__, &res);
	assert_clean(&res, CYCLES / 1000u);
}

ZTEST(radiant_lp_drift, test_rtc_lf_slow_no_drift)
{
	static const struct sim_cfg c = {
		.num = RADIANT_LP_RTC_NUM, .shift = RADIANT_LP_RTC_SHIFT,
		.width = RADIANT_LP_RTC_WIDTH,
		.tick_pu = 32000000 - 50 * 32,
		.k_offset = 12345u,
		.junk = 0u,
		.min_lead = RADIANT_LP_RTC_MIN_LEAD_TICKS,
		.tol_us = RADIANT_LP_RELEASE_TOL_US,
		.linger_every = 1000u,
	};

	run_sim(&c, 0xC0FFEEu, &res);
	print_result(__func__, &res);
	assert_clean(&res, CYCLES / 1000u);
}

/* GRTC, 1 MHz nominal (1048576 pu), +52 pu = +49.6 ppm. */
ZTEST(radiant_lp_drift, test_grtc_no_drift)
{
	static const struct sim_cfg c = {
		.num = RADIANT_LP_GRTC_NUM, .shift = RADIANT_LP_GRTC_SHIFT,
		.width = RADIANT_LP_GRTC_WIDTH,
		.tick_pu = 1048576 + 52,
		.k_offset = (UINT64_C(1) << 52) - 100000u,
		.junk = UINT64_C(0xA5) << 56,
		.min_lead = RADIANT_LP_GRTC_MIN_LEAD_TICKS,
		.tol_us = RADIANT_LP_RELEASE_TOL_US,
		.linger_every = 1000u,
	};

	run_sim(&c, 0xBADC0DEu, &res);
	print_result(__func__, &res);
	assert_clean(&res, CYCLES / 1000u);
}

/*
 * Negative control: a backend that rounds the wake to whole us and forgets
 * the rest loses ~0.5 us a cycle - ~50 ms over the run. The simulation must
 * see it, or the clean runs above prove nothing.
 */
ZTEST(radiant_lp_drift, test_control_dropping_the_fraction_drifts)
{
	static const struct sim_cfg c = {
		.num = RADIANT_LP_RTC_NUM, .shift = RADIANT_LP_RTC_SHIFT,
		.width = RADIANT_LP_RTC_WIDTH,
		.tick_pu = 32000000 + 50 * 32,
		.k_offset = 0u,
		.junk = 0u,
		.min_lead = RADIANT_LP_RTC_MIN_LEAD_TICKS,
		.tol_us = RADIANT_LP_RELEASE_TOL_US,
		.linger_every = 0u,
		.drop_fraction = true,
	};

	run_sim(&c, 0x1234567u, &res);
	print_result(__func__, &res);
	zassert_true(res.max_asleep_err_q > (UINT64_C(10000) << c.shift),
		     "only %u q of drift", (uint32_t)res.max_asleep_err_q);
	zassert_true(res.early_asleep_err_q < (UINT64_C(1000) << c.shift),
		     "the drift should build up, not start large");
}

/*
 * Negative control: anchoring every release on the TIMER's capture (tolerance
 * 0, so the LF image is never taken) keeps each capture's whole-us error, and
 * nothing ever takes it back out: milliseconds over the run.
 */
ZTEST(radiant_lp_drift, test_control_timer_anchored_releases_drift)
{
	static const struct sim_cfg c = {
		.num = RADIANT_LP_RTC_NUM, .shift = RADIANT_LP_RTC_SHIFT,
		.width = RADIANT_LP_RTC_WIDTH,
		.tick_pu = 32000000 + 50 * 32,
		.k_offset = 0u,
		.junk = 0u,
		.min_lead = RADIANT_LP_RTC_MIN_LEAD_TICKS,
		.tol_us = 0u,
		.linger_every = 0u,
	};

	run_sim(&c, 0x1234567u, &res);
	print_result(__func__, &res);
	zassert_true(res.timer_releases > CYCLES - CYCLES / 100u);
	zassert_true(res.max_asleep_err_q > (UINT64_C(10000) << c.shift),
		     "only %u q of drift", (uint32_t)res.max_asleep_err_q);
}
