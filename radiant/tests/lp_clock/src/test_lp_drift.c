/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work, written against
 * radiant/src/radiant_radio_nrf_lp.h.
 *
 * 100 000 sleep/wake cycles of a 4 Hz master against a physical model of the
 * crystals, driving every helper the backend will use in the order it will
 * use them.
 *
 * THE MODEL. Physical time is counted in "pu", 2^-20 us. The HFXO is the
 * physical reference (1 us = 2^20 pu exactly). HFINT, which clocks the TIMER
 * while the HFXO is starting, runs hfint_ppm off it (+-1.5 % here). The LF
 * crystal ticks every tick_pu, its nominal period plus a ppm offset: +-60 ppm
 * and -30 ppm on the RTC runs, -58.5 ppm on the GRTC run, covering the -30
 * (nRF54L15 DK) and -58.5 ppm (Pro Micro clone) HF-vs-LF steps spike B1
 * measured. Each cycle:
 *
 *   awake    the TIMER reads seg_base_us + whole us since it started; any
 *            part of that spent before the HFXO was stable counted at the
 *            HFINT rate.
 *   release  at an LF edge after the cycle's operation; the capture reads the
 *            TIMER up to 1 us after the edge (anchor jitter).
 *   asleep   the LF conversion from the release anchor.
 *   EDGE 1   radiant_lp_wake_plan()'s HFXO compare; the HFXO request is
 *            issued up to RADIANT_LP_HFXO_ISR_MARGIN_US - 1 later and the
 *            crystal is stable hfxo_us after that.
 *   EDGE 2   the TIMER compare: the cleared TIMER starts up to 1 us after it
 *            (anchor jitter) and radiant_lp_wake() takes it.
 *
 * The single-edge negative control starts the TIMER on EDGE 1 instead, as a
 * one-compare wake would, so it counts the HFXO start-up on HFINT.
 *
 * THE REFERENCE is the timebase locked to the LF crystal: from one anchor,
 * ref_q(k) = ref_q0 + (k - ref_k0) * num for every LF tick k. A long awake
 * segment (a 1 s linger every 1000th cycle here) genuinely ran on the HF
 * crystal, so its release takes the TIMER and the reference re-locks there.
 *
 * WHAT THE CLEAN RUNS PROVE, AND WHAT THEY DO NOT. Once a release takes the
 * LF image, the asleep reading equals the reference by construction - both
 * are the same multiply - so "zero asleep error" is not news about the
 * arithmetic. What is tested is everything around it: that under +-1 us
 * anchor jitter, +-60 ppm LF offset and an HFINT clock during start-up the
 * release takes the LF image every time it should (and the TIMER on every
 * linger), so the reference never has to move; that the 24-bit RTC wrap
 * (~50 times a run), junk above a raw counter's width, an extended counter
 * and the GRTC's 52-bit wrap are all handled; that the two wake edges are
 * never late, never a tick early, give the HFXO its whole lead and keep the
 * TIMER off HFINT; that should_sleep's budget admits every gap; that every
 * handed-out value is monotonic; and that awake readings stay within 3 us of
 * the reference interpolated at the HF rate from EDGE 2 (the wake's whole-us
 * floor, the start jitter and the TIMER's own floor, each under 1 us).
 *
 * The three negative controls are what make those clean runs mean something:
 * dropping the wake's carried fraction, anchoring every release on the TIMER,
 * and starting the TIMER on the HFXO request edge each drift by milliseconds
 * or more over the same run, and the simulation sees it.
 *
 * Missed compares are unit cases in test_lp_math.c
 * (test_missed_compare_is_refused_not_wrapped), not modelled here.
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
#define ARM_US    300u                /* the backend's arming lead */
#define HFXO_US   800u                /* nRF54L15 default (spike B1 S4) */
#define HFXO_LEAD (HFXO_US + RADIANT_LP_HFXO_ISR_MARGIN_US)
#define KEEP_US   3000u

struct sim_cfg {
	uint32_t num;
	uint8_t  shift;
	uint8_t  width;
	int64_t  tick_pu;       /* true LF period, pu */
	uint64_t k_offset;      /* counter value at physical tick 0 */
	uint64_t junk;          /* bits above a raw width the caller passes */
	uint32_t min_lead;      /* compare lead, ticks */
	uint32_t tol_us;
	int32_t  hfint_ppm;     /* HFINT's rate error */
	uint32_t linger_every;  /* 0 = never */
	bool     drop_fraction; /* negative control */
	bool     single_edge;   /* negative control: TIMER started on EDGE 1 */
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
	uint32_t late_wakes;         /* EDGE 2 after op - arm */
	uint32_t loose_wakes;        /* EDGE 2 a whole tick earlier than needed */
	uint32_t short_hfxo;         /* EDGE 1 to EDGE 2 under the HFXO lead */
	uint32_t hfint_segments;     /* awake TIMER that counted on HFINT */
	uint32_t refused_sleeps;
	uint32_t unsafe_compares;
	uint32_t bad_wake_rc;
	uint32_t mono_breaks;
	uint32_t range_errors;
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

/* The awake TIMER: when it started, its fold base, and how long (pu after
 * its start) it counted on HFINT.
 */
static int64_t seg_start_pu;
static uint64_t seg_base_us;
static int64_t seg_hfint_pu;

/* Physical time of LF tick k (k counts from the start of the run). */
static int64_t lf_edge_pu(uint64_t k)
{
	return (int64_t)k * cfg->tick_pu;
}

/* The counter value the caller would hand over at tick k. */
static uint64_t counter(uint64_t k)
{
	return ((cfg->k_offset + k) & tb.mask) | cfg->junk;
}

/* What the TIMER-domain fold reads at physical time p. */
static uint64_t timer_us(int64_t p)
{
	int64_t e = p - seg_start_pu;
	int64_t on_hfint = MIN(e, seg_hfint_pu);

	e += on_hfint * cfg->hfint_ppm / 1000000;
	return seg_base_us + (uint64_t)(e / PU);
}

static uint64_t abs_diff(uint64_t a, uint64_t b)
{
	return a > b ? a - b : b - a;
}

static void run_sim(const struct sim_cfg *config, uint32_t seed,
		    struct sim_result *r)
{
	struct radiant_lp_clock c;
	uint64_t k_w = 0;          /* the edge the awake TIMER started on */
	uint64_t op_us = 50000u;   /* this segment's operation */
	uint64_t ref_k0 = 0, ref_q0 = 0;
	uint64_t last_out = 0;
	uint64_t wake_lead, lf_overhead;
	const uint64_t fracmask = (UINT64_C(1) << config->shift) - 1u;

	cfg = config;
	rng = seed;
	memset(r, 0, sizeof(*r));
	zassert_true(radiant_lp_tb_init(&tb, config->num, config->shift,
					config->width));
	radiant_lp_clock_init(&c);
	wake_lead = radiant_lp_wake_lead_us(&tb, ARM_US, HFXO_LEAD);
	lf_overhead = radiant_lp_lf_overhead_us(&tb, config->min_lead);
	seg_start_pu = 0;
	seg_base_us = 0;
	seg_hfint_pu = 0;

	for (uint32_t i = 0; i < CYCLES; i++) {
		bool linger = config->linger_every != 0u &&
			      (i % config->linger_every) ==
			      config->linger_every - 1u;
		uint64_t tail_us = linger ? 1000000u : 500u + rnd_upto(3500u);
		/* A linger stays awake through the next four operations. */
		uint64_t next_op = op_us + (linger ? 5u : 1u) * PERIOD_US;
		struct radiant_lp_wake_plan plan;
		uint64_t k_r, k1, k2, k_s, c_us, now_us, raw, out;
		int64_t target_pu, p, ref_pu, err_pu, stable_pu;
		enum radiant_lp_wake_rc rc;
		bool took_lf;

		/* Release edge: the first tick at which the awake TIMER has
		 * passed the operation plus its tail (HFINT ignored here; it
		 * only moves the edge by a tick).
		 */
		target_pu = seg_start_pu +
			    (int64_t)(op_us + tail_us - seg_base_us) * PU;
		k_r = (uint64_t)((target_pu + cfg->tick_pu - 1) / cfg->tick_pu);
		if (k_r <= k_w) {
			k_r = k_w + 1u;
		}

		/* An awake reading somewhere in the segment, against the
		 * reference interpolated at the HF rate from the start edge.
		 * Not for the boot segment, which no wake started.
		 */
		p = seg_start_pu +
		    (int64_t)rnd_upto((uint64_t)(lf_edge_pu(k_r) - seg_start_pu));
		now_us = timer_us(p);
		if (i > 0u) {
			uint64_t ref_q_w = ref_q0 + (k_w - ref_k0) * cfg->num;

			ref_pu = (int64_t)(ref_q_w << (20u - cfg->shift)) +
				 (p - lf_edge_pu(k_w));
			err_pu = (int64_t)now_us * PU - ref_pu;
			if (err_pu < 0) {
				err_pu = -err_pu;
			}
			r->max_awake_err_pu = MAX(r->max_awake_err_pu, err_pu);
		}
		out = radiant_lp_clock_now(&tb, &c, false, now_us, 0u);
		r->mono_breaks += out < last_out;
		last_out = out;

		/* The release: the capture lands up to 1 us after the edge. */
		c_us = timer_us(lf_edge_pu(k_r) + (int64_t)rnd_upto(PU - 1));
		if (!radiant_lp_should_sleep(c_us, next_op, wake_lead,
					     lf_overhead, KEEP_US, 0u)) {
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

		/* Both wake edges for the next operation. */
		rc = radiant_lp_wake_plan(&tb, &c, next_op, ARM_US, HFXO_LEAD,
					  UINT64_MAX, &plan);
		r->bad_wake_rc += rc != RADIANT_LP_WAKE_OK;
		if (!radiant_lp_compare_in_time(&tb, counter(k_r), plan.hfxo_cmp,
						config->min_lead) ||
		    !radiant_lp_compare_in_time(&tb, plan.hfxo_cmp,
						plan.timer_cmp, 1u)) {
			r->unsafe_compares++;
		}
		{
			uint64_t want_q = (next_op - ARM_US) << cfg->shift;
			uint64_t q1 = radiant_lp_asleep_q(&tb, &c, plan.hfxo_cmp);
			uint64_t q2 = radiant_lp_asleep_q(&tb, &c, plan.timer_cmp);

			r->late_wakes += q2 > want_q;
			r->loose_wakes += q2 + cfg->num <= want_q;
			r->short_hfxo += q2 - q1 < ((uint64_t)HFXO_LEAD << cfg->shift);
		}
		k1 = k_r + radiant_lp_ticks_since(&tb, counter(k_r), plan.hfxo_cmp);
		k2 = k_r + radiant_lp_ticks_since(&tb, counter(k_r), plan.timer_cmp);

		/* An asleep reading somewhere in the gap. */
		k_s = k_r + rnd_upto(k2 - k_r);
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

		/* EDGE 1: the HFXO request goes out after the ISR's latency,
		 * and the crystal is stable its start-up after that.
		 */
		stable_pu = lf_edge_pu(k1) +
			    (int64_t)rnd_upto((RADIANT_LP_HFXO_ISR_MARGIN_US - 1u) *
					      (uint64_t)PU) +
			    (int64_t)HFXO_US * PU;

		/* EDGE 2 (or, in the control, EDGE 1 again) starts the cleared
		 * TIMER up to 1 us late.
		 */
		k_w = config->single_edge ? k1 : k2;
		seg_base_us = radiant_lp_wake(&tb, &c, counter(k_w), true);
		if (config->drop_fraction) {
			c.anchor_q &= ~fracmask;
			c.awake_frac = 0u;
		}
		seg_start_pu = lf_edge_pu(k_w) + (int64_t)rnd_upto(PU - 1);
		seg_hfint_pu = MAX(stable_pu - seg_start_pu, 0);
		r->hfint_segments += seg_hfint_pu > 0;
		op_us = next_op;
	}
	r->range_errors = c.range_errors;
}

static void print_result(const char *name, const struct sim_result *r)
{
	TC_PRINT("%s: asleep err %u q (first 1000 cycles %u q), awake err %u pu, "
		 "release step back %u us fwd %u us, releases lf %u timer %u\n",
		 name, (uint32_t)MIN(r->max_asleep_err_q, UINT32_MAX),
		 (uint32_t)MIN(r->early_asleep_err_q, UINT32_MAX),
		 (uint32_t)MIN(r->max_awake_err_pu, UINT32_MAX),
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
	zassert_equal(r->short_hfxo, 0u);
	zassert_equal(r->hfint_segments, 0u, "the TIMER counted on HFINT");
	zassert_equal(r->refused_sleeps, 0u);
	zassert_equal(r->unsafe_compares, 0u);
	zassert_equal(r->bad_wake_rc, 0u);
	zassert_equal(r->mono_breaks, 0u);
	zassert_equal(r->range_errors, 0u);
}

ZTEST_SUITE(radiant_lp_drift, NULL, NULL, NULL, NULL, NULL);

static struct sim_result res;

/* The RTC format; the nominal tick is 15625 << 11 = 32000000 pu, so one ppm
 * of LF offset is 32 pu.
 */
#define RTC_CFG(ppm)                                                          \
	.num = RADIANT_LP_RTC_NUM, .shift = RADIANT_LP_RTC_SHIFT,             \
	.tick_pu = 32000000 + (ppm) * 32,                                     \
	.min_lead = RADIANT_LP_RTC_MIN_LEAD_TICKS,                            \
	.hfint_ppm = 15000

ZTEST(radiant_lp_drift, test_rtc_lf_fast_no_drift)
{
	static const struct sim_cfg c = {
		RTC_CFG(60),
		.width = RADIANT_LP_RTC_WIDTH,
		.k_offset = 0xFFFFFFu - 1000u,
		.junk = UINT64_C(0x5A) << 32,
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
		RTC_CFG(-60),
		.width = RADIANT_LP_RTC_WIDTH,
		.k_offset = 12345u,
		.tol_us = RADIANT_LP_RELEASE_TOL_US,
		.linger_every = 1000u,
	};

	run_sim(&c, 0xC0FFEEu, &res);
	print_result(__func__, &res);
	assert_clean(&res, CYCLES / 1000u);
}

/* The same with an extended counter (z_nrf_rtc_timer_read()), HFINT slow. */
ZTEST(radiant_lp_drift, test_rtc_extended_no_drift)
{
	static const struct sim_cfg c = {
		.num = RADIANT_LP_RTC_NUM, .shift = RADIANT_LP_RTC_SHIFT,
		.tick_pu = 32000000 - 30 * 32,
		.min_lead = RADIANT_LP_RTC_MIN_LEAD_TICKS,
		.hfint_ppm = -15000,
		.width = RADIANT_LP_EXTENDED_WIDTH,
		.k_offset = UINT64_C(0x123456789AB),
		.tol_us = RADIANT_LP_RELEASE_TOL_US,
		.linger_every = 1000u,
	};

	run_sim(&c, 0x5EED5EEDu, &res);
	print_result(__func__, &res);
	assert_clean(&res, CYCLES / 1000u);
}

/* GRTC, 1 MHz nominal (1048576 pu), -61 pu = -58.2 ppm. */
ZTEST(radiant_lp_drift, test_grtc_no_drift)
{
	static const struct sim_cfg c = {
		.num = RADIANT_LP_GRTC_NUM, .shift = RADIANT_LP_GRTC_SHIFT,
		.width = RADIANT_LP_GRTC_WIDTH,
		.tick_pu = 1048576 - 61,
		.k_offset = (UINT64_C(1) << 52) - 100000u,
		.junk = UINT64_C(0xA5) << 56,
		.min_lead = RADIANT_LP_GRTC_MIN_LEAD_TICKS,
		.tol_us = RADIANT_LP_RELEASE_TOL_US,
		.hfint_ppm = 15000,
		.linger_every = 1000u,
	};

	run_sim(&c, 0xBADC0DEu, &res);
	print_result(__func__, &res);
	assert_clean(&res, CYCLES / 1000u);
}

/*
 * Negative control: a backend that rounds the wake to whole us and forgets
 * the rest loses ~0.5 us a cycle - ~50 ms over the run.
 */
ZTEST(radiant_lp_drift, test_control_dropping_the_fraction_drifts)
{
	static const struct sim_cfg c = {
		RTC_CFG(50),
		.width = RADIANT_LP_RTC_WIDTH,
		.tol_us = RADIANT_LP_RELEASE_TOL_US,
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
		RTC_CFG(50),
		.width = RADIANT_LP_RTC_WIDTH,
		.tol_us = 0u,
	};

	run_sim(&c, 0x1234567u, &res);
	print_result(__func__, &res);
	zassert_true(res.timer_releases > CYCLES - CYCLES / 100u);
	zassert_true(res.max_asleep_err_q > (UINT64_C(10000) << c.shift),
		     "only %u q of drift", (uint32_t)res.max_asleep_err_q);
}

/*
 * Negative control: one wake edge for both the HFXO request and the TIMER
 * start. The TIMER counts the ~850 us start-up on HFINT, 1.5 % fast: ~13 us
 * out at every release, beyond the tolerance, so the TIMER's reading is taken
 * every period and the error is kept - about a second over the run.
 */
ZTEST(radiant_lp_drift, test_control_single_edge_wake_drifts)
{
	static const struct sim_cfg c = {
		RTC_CFG(50),
		.width = RADIANT_LP_RTC_WIDTH,
		.tol_us = RADIANT_LP_RELEASE_TOL_US,
		.single_edge = true,
	};

	run_sim(&c, 0x1234567u, &res);
	print_result(__func__, &res);
	zassert_true(res.hfint_segments == CYCLES);
	zassert_true(res.timer_releases > CYCLES - CYCLES / 100u,
		     "only %u releases fell back", res.timer_releases);
	zassert_true(res.max_asleep_err_q > (UINT64_C(100000) << c.shift),
		     "only %u q of drift", (uint32_t)MIN(res.max_asleep_err_q,
							 UINT32_MAX));
}
