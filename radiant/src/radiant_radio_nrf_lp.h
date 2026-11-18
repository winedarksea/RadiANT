/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_nrf_lp.h - the nRF low-power timebase's arithmetic, as pure
 * functions (CONFIG_RADIANT_NRF_LOW_POWER, power plan Track B).
 *
 * Provenance: clean-room. Written from radiant_radio_nrf.c's own clock
 * (radiant_radio_now(), clock_fold()), the nRF52840/nRF5340 RTC and nRF54L15
 * GRTC counter widths in the nrfx MDK headers, Zephyr's nrf_rtc_timer and
 * nrf_grtc_timer drivers, and the nordic,nrf-grtc devicetree binding.
 * Nothing derives from sdk-ant or any ANT+ document.
 *
 * WHAT THIS IS FOR. Today the backend keeps the HFXO requested forever and
 * reads time from a free-running 1 MHz TIMER. Track B stops that TIMER and
 * releases the HFXO in long idle gaps, keeping time on the low-frequency
 * counter the system timer already runs (RTC1 at 32768 Hz on the nRF52840 and
 * the nRF5340 network core, the GRTC SYSCOUNTER at 1 MHz on the nRF54L15).
 * The timebase becomes piecewise:
 *
 *   awake   the TIMER, exactly as today (the backend's own 64-bit fold);
 *   asleep  now = max(hwm, anchor + lf_us(ticks - anchor_ticks)).
 *
 * The two meet at LF edges. RELEASE: an LF compare fires TIMER CAPTURE by
 * (D)PPI, so the TIMER-domain time of one known LF tick is captured, and the
 * TIMER stops. WAKE: an LF compare starts a cleared TIMER by (D)PPI, so the
 * awake segment's TIMER counts from a known LF tick. Everything in this file
 * is the bookkeeping between those two edges and the decisions around them;
 * none of it touches a register, so radiant/tests/lp_clock runs it on
 * native_sim. Nothing in the tree includes this header unless
 * CONFIG_RADIANT_NRF_LOW_POWER is set (a header included by a default build
 * can renumber compiler-generated symbols and fail the zero-cost check).
 *
 * UNITS. Every time is a uint64_t in microseconds of the radio timebase, the
 * same scale as radiant_time_t. LF counter values are raw counter values,
 * reduced modulo 2^width (radiant_lp_tb.width); the caller may pass a wider
 * (software-extended) value, it is masked. Sub-microsecond quantities are "q
 * units": 2^-shift microseconds, where 1 LF tick = num q units exactly.
 *
 *   32768 Hz (RTC):  1 tick = 1e6 / 32768 us = 15625 / 512 us
 *                    num = 15625, shift = 9, so q units are 1/512 us.
 *   1 MHz (GRTC):    1 tick = 1 us: num = 1, shift = 0, q units are us.
 *
 * WHY THE FRACTION IS CARRIED. 15625/512 us is 30.517578125 us. Rounding each
 * conversion to whole microseconds loses up to 1 us per conversion, and a
 * battery node converts at every wake: four times a second that is a 0.5 us
 * average loss per period, a timebase running ~2 ppm slow from arithmetic
 * alone, and an error that grows without bound. The anchor is therefore held
 * in q units and every conversion is one multiply from the anchor (never a
 * sum of rounded pieces); the fraction left over at a wake is carried into
 * the awake segment (radiant_lp_clock.awake_frac) and comes back at the next
 * release. The arithmetic itself then never accumulates error: the sum of any
 * number of piecewise conversions equals one conversion of the total
 * (radiant_lp_conv proves the identity; radiant_lp_clock relies on it).
 *
 * ROUNDING DIRECTIONS, ALL OF THEM:
 *   - LF ticks to us (radiant_lp_asleep_us, radiant_lp_conv_step,
 *     radiant_lp_wake): toward zero (floor). An asleep reading is never ahead
 *     of the exact LF image; it is behind it by less than 1 us.
 *   - us to the wake compare tick (radiant_lp_wake_compare): toward EARLIER.
 *     The compare is the last LF tick whose exact time is at or before the
 *     requested wake time, so a wake is early by less than one tick
 *     (30.5 us on the RTC, 1 us on the GRTC) and never late.
 *   - release (radiant_lp_release): exact in q units, no rounding; which of
 *     the two exact candidates it takes is the subject of its comment.
 *   - radiant_lp_should_sleep: strictly greater than lead + keep-on; equal
 *     stays awake.
 *
 * COUNTER WIDTH AND WRAP. Differences of counter values are taken modulo
 * 2^width, so they are right across a wrap as long as the true distance is
 * below 2^width ticks: 512 s for the RTC's 24 bits, ~142 years for the
 * GRTC's 52. Forward distances are only trusted up to half the range
 * (radiant_lp_tb_half()), which is also the furthest radiant_lp_wake_compare
 * will place a wake; a longer sleep is a clamped wake that reconsiders.
 *
 * CONCURRENCY. None here. These are pure functions over caller-owned structs;
 * the backend serialises (and makes ZLI-safe) every call that writes one.
 */

#ifndef RADIANT_RADIO_NRF_LP_H_
#define RADIANT_RADIO_NRF_LP_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Counter formats. The RTC values are exact for a 32768 Hz LF clock
 * (CONFIG_CLOCK_CONTROL_NRF_K32SRC_FREQUENCY, RTC PRESCALER 0, as Zephyr's
 * nrf_rtc_timer runs RTC1) and the 24-bit COUNTER of the nRF52840 and nRF5340
 * RTC (RTC_COUNTER_COUNTER_Msk). The GRTC values are the nRF54L15 SYSCOUNTER:
 * 1 MHz (the nordic,nrf-grtc binding's clock-frequency default, which
 * nrf54l's SYS_CLOCK_HW_CYCLES_PER_SEC takes) and 32 + 20 = 52 bits
 * (GRTC_SYSCOUNTER_SYSCOUNTERL/H_VALUE_Msk). The backend should
 * BUILD_ASSERT the frequency it assumes against those symbols.
 */
#define RADIANT_LP_RTC_NUM    15625u
#define RADIANT_LP_RTC_SHIFT  9u
#define RADIANT_LP_RTC_WIDTH  24u
#define RADIANT_LP_GRTC_NUM   1u
#define RADIANT_LP_GRTC_SHIFT 0u
#define RADIANT_LP_GRTC_WIDTH 52u

/*
 * Least lead, in ticks, a compare needs over the counter to be sure to fire.
 * The nRF52 RTC may not fire a compare written as COUNTER or COUNTER + 1
 * (product specification, RTC "COMPARE" section); Zephyr's nrf_rtc_timer
 * keeps MIN_CYCLES_FROM_NOW = 3, and so does this. The GRTC needs a latch
 * time instead, which nrf_grtc_timer bounds at MAX_CC_LATCH_WAIT_TIME_US = 77
 * SYSCOUNTER ticks; one LF period on top of it is the safe figure. Spike B1
 * measures both; these are starting values, not measurements.
 */
#define RADIANT_LP_RTC_MIN_LEAD_TICKS  3u
#define RADIANT_LP_GRTC_MIN_LEAD_TICKS (77u + 31u)

/*
 * How far the LF image of a release edge may sit from the TIMER's capture of
 * it, in us, and still be taken (radiant_lp_release). The budget: 1 us for
 * the capture's whole-us quantisation, 1 us of anchor jitter (the (D)PPI
 * start and capture latencies; spike B1's S3 target is +-1 us) and the
 * HF-vs-LF crystal difference over a short awake segment (+-60 ppm over
 * 6 ms is 0.4 us): 2.4 us, so 3. It is also the most a release can move the
 * timebase. S3's measurement may revise it.
 */
#define RADIANT_LP_RELEASE_TOL_US 3u

/* radiant_lp_should_sleep(): no operation is pending. */
#define RADIANT_LP_NO_OP UINT64_MAX

/* ------------------------------------------------------------------------ */
/* The counter format                                                        */
/* ------------------------------------------------------------------------ */

struct radiant_lp_tb {
	uint64_t mask;   /* 2^width - 1 */
	uint32_t num;    /* q units per tick */
	uint8_t  shift;  /* q units per us = 2^shift */
	uint8_t  width;  /* counter bits */
};

/*
 * Fill *tb. False (and *tb untouched) for a format this arithmetic cannot
 * carry exactly in 64 bits: num 0, shift over 31, width outside 2..63, or
 * width + bitlength(num) over 63, which is what keeps (any in-range tick
 * distance) * num below 2^63. The RTC and GRTC formats above use 38 and 53
 * bits of that budget.
 */
static inline bool radiant_lp_tb_init(struct radiant_lp_tb *tb, uint32_t num,
				      uint8_t shift, uint8_t width)
{
	uint8_t num_bits = 0;

	for (uint32_t n = num; n != 0u; n >>= 1) {
		num_bits++;
	}
	if (num == 0u || shift > 31u || width < 2u || width > 63u ||
	    (unsigned int)width + num_bits > 63u) {
		return false;
	}
	tb->mask = (UINT64_C(1) << width) - 1u;
	tb->num = num;
	tb->shift = shift;
	tb->width = width;
	return true;
}

/* The furthest forward distance, in ticks, that is trusted: half the range. */
static inline uint64_t radiant_lp_tb_half(const struct radiant_lp_tb *tb)
{
	return tb->mask >> 1;
}

/* Ticks from `from` to `to`, modulo 2^width (right across a wrap). */
static inline uint64_t radiant_lp_ticks_since(const struct radiant_lp_tb *tb,
					      uint64_t from, uint64_t to)
{
	return (to - from) & tb->mask;
}

/*
 * True once the counter has reached `target`: `now` is at or past it by less
 * than half the range. The backend's "has the wake (or release) edge already
 * gone by" test, e.g. to see that an anchor compare passed while the TIMER
 * did not start.
 */
static inline bool radiant_lp_tick_reached(const struct radiant_lp_tb *tb,
					   uint64_t now, uint64_t target)
{
	return radiant_lp_ticks_since(tb, target, now) <= radiant_lp_tb_half(tb);
}

/*
 * True if a compare written now, at counter value `now`, at `compare`, is
 * certain to fire: it is at least min_lead ticks ahead and not more than half
 * the range ahead (further reads as the past). False is a late anchor: the
 * edge may never come, so the caller must not stop or start anything on it.
 */
static inline bool radiant_lp_compare_in_time(const struct radiant_lp_tb *tb,
					      uint64_t now, uint64_t compare,
					      uint32_t min_lead)
{
	uint64_t ahead = radiant_lp_ticks_since(tb, now, compare);

	return ahead >= min_lead && ahead <= radiant_lp_tb_half(tb);
}

/* Ticks to us, toward zero, with no carry: (ticks * num) >> shift. */
static inline uint64_t radiant_lp_ticks_to_us(const struct radiant_lp_tb *tb,
					      uint64_t ticks)
{
	return (ticks * tb->num) >> tb->shift;
}

/* ------------------------------------------------------------------------ */
/* Conversion with the fraction carried                                      */
/* ------------------------------------------------------------------------ */

/* The sub-microsecond remainder of conversions so far, in q units (< 2^shift). */
struct radiant_lp_conv {
	uint32_t frac;
};

/*
 * Convert a tick distance to whole us, toward zero, adding in the remainder
 * the previous conversions left and keeping the new one. For any split of a
 * distance into pieces, starting from frac = 0, the sum of the pieces' results
 * equals radiant_lp_ticks_to_us() of the whole distance, and frac ends equal
 * to the whole distance's remainder. ticks must be within the format's range.
 */
static inline uint64_t radiant_lp_conv_step(const struct radiant_lp_tb *tb,
					    struct radiant_lp_conv *cv,
					    uint64_t ticks)
{
	uint64_t q = ticks * tb->num + cv->frac;

	cv->frac = (uint32_t)(q & ((UINT64_C(1) << tb->shift) - 1u));
	return q >> tb->shift;
}

/* ------------------------------------------------------------------------ */
/* The piecewise timebase                                                    */
/* ------------------------------------------------------------------------ */

/*
 * One anchor: the exact timebase value (q units) of one LF tick. After a
 * release it is the release edge and asleep readings convert from it. After a
 * wake it is the wake edge, and it is what the next release measures the
 * awake segment against.
 */
struct radiant_lp_clock {
	uint64_t anchor_q;      /* timebase at anchor_ticks, q units */
	uint64_t anchor_ticks;  /* LF counter at the anchor, masked */
	uint64_t hwm_us;        /* highest value radiant_lp_mono() returned */
	uint32_t awake_frac;    /* awake segment's sub-us offset, q units: the
				 * TIMER reads whole us from wake_us, the true
				 * time is that plus this */
	bool     timer_at_anchor; /* the awake segment's TIMER was started, from
				   * zero, by the anchor's LF edge */
};

/* Start in the awake state on a TIMER that has free-run since boot. */
static inline void radiant_lp_clock_init(struct radiant_lp_clock *c)
{
	c->anchor_q = 0u;
	c->anchor_ticks = 0u;
	c->hwm_us = 0u;
	c->awake_frac = 0u;
	c->timer_at_anchor = false;
}

/* Exact asleep timebase at LF counter `ticks`, q units. */
static inline uint64_t radiant_lp_asleep_q(const struct radiant_lp_tb *tb,
					   const struct radiant_lp_clock *c,
					   uint64_t ticks)
{
	return c->anchor_q +
	       radiant_lp_ticks_since(tb, c->anchor_ticks, ticks) * tb->num;
}

/*
 * Asleep timebase at LF counter `ticks`, us, toward zero. No monotonic clamp:
 * see radiant_lp_mono(). `ticks` must be less than 2^width past the anchor;
 * the backend wakes long before (radiant_lp_wake_compare clamps at half).
 */
static inline uint64_t radiant_lp_asleep_us(const struct radiant_lp_tb *tb,
					    const struct radiant_lp_clock *c,
					    uint64_t ticks)
{
	return radiant_lp_asleep_q(tb, c, ticks) >> tb->shift;
}

/*
 * The monotonic clamp: max(hwm, v), raising hwm to it. Every value the
 * timebase hands out passes through this, awake and asleep, because a release
 * may move the timebase back by up to RADIANT_LP_RELEASE_TOL_US (the LF image
 * of the edge sitting just below the TIMER's reading of it); time then stands
 * still until it catches up, rather than running backwards.
 */
static inline uint64_t radiant_lp_mono(struct radiant_lp_clock *c, uint64_t v)
{
	if (v < c->hwm_us) {
		return c->hwm_us;
	}
	c->hwm_us = v;
	return v;
}

/*
 * The piecewise reading: awake, the TIMER-domain us the backend's fold
 * produced; asleep, the LF conversion of `ticks`. Either way clamped.
 */
static inline uint64_t radiant_lp_clock_now(const struct radiant_lp_tb *tb,
					    struct radiant_lp_clock *c,
					    bool asleep, uint64_t timer_us,
					    uint64_t ticks)
{
	return radiant_lp_mono(c, asleep ? radiant_lp_asleep_us(tb, c, ticks)
					 : timer_us);
}

/*
 * RELEASE at LF edge `ticks`, whose TIMER-domain time the (D)PPI capture read
 * as `timer_us` (the backend's 64-bit fold of the captured CC value). Becomes
 * the asleep anchor. Returns true if it took the LF image, false if the
 * TIMER's; *lf_minus_timer_q (may be NULL) gets their signed difference in q
 * units, for a stat.
 *
 * There are two exact candidates for the anchor:
 *
 *   x, the TIMER's reading: timer_us in us plus the awake segment's carried
 *      fraction. Continuous with every awake reading, but the capture is a
 *      whole-us read of a continuous process, so it is up to 1 us off the
 *      true time - and an error made at a release is never undone, so taking
 *      x at every release random-walks (and, if the TIMER's prescaler phase
 *      restarts with it, drifts by ~0.5 us a cycle).
 *   y, the LF image: the wake anchor plus the exact conversion of the LF
 *      ticks from the wake edge to this one. Only defined if this awake
 *      segment's TIMER started at the wake edge (timer_at_anchor). Exact in
 *      the LF domain, so taking y at every release leaves the arithmetic
 *      locked to the LF crystal with no accumulation at all.
 *
 * y is taken when it exists and lies within tol_us of x. Over a short awake
 * segment (a master slot: ~5 ms) the two crystals agree to well under 1 us, so
 * y is always taken and the long-term timebase is the LF clock. Over a long
 * one (a 1 s linger, a search) the HF crystal has genuinely run apart from the
 * LF one by tens of us; the awake readings in that segment were HF time, so x
 * is taken and the step stays zero. Either way the release moves the timebase
 * by at most tol_us from the last awake reading.
 */
static inline bool radiant_lp_release(const struct radiant_lp_tb *tb,
				      struct radiant_lp_clock *c,
				      uint64_t ticks, uint64_t timer_us,
				      uint32_t tol_us, int64_t *lf_minus_timer_q)
{
	uint64_t x = (timer_us << tb->shift) + c->awake_frac;
	bool take_lf = false;
	int64_t diff = 0;

	if (c->timer_at_anchor) {
		uint64_t y = radiant_lp_asleep_q(tb, c, ticks);
		uint64_t tol_q = (uint64_t)tol_us << tb->shift;

		diff = (int64_t)(y - x);
		take_lf = (y >= x) ? (y - x <= tol_q) : (x - y <= tol_q);
		if (take_lf) {
			x = y;
		}
	}
	if (lf_minus_timer_q != NULL) {
		*lf_minus_timer_q = diff;
	}
	c->anchor_q = x;
	c->anchor_ticks = ticks & tb->mask;
	c->awake_frac = 0u;
	c->timer_at_anchor = false;
	return take_lf;
}

/*
 * WAKE at LF edge `ticks`. Returns the us the backend loads as its TIMER
 * fold's base (with the fold's `last` = 0, the TIMER having been cleared):
 * the asleep reading at that edge, toward zero, so the awake readings
 * continue the asleep ones without a step. The fraction below it is carried
 * in awake_frac.
 *
 * timer_started_at_edge: true when the (D)PPI connection started the cleared
 * TIMER on this very edge (the normal path). False when the backend had to
 * start it in software after reading `ticks` (a late anchor's recovery); the
 * awake segment then has no exact relation to the edge, the next release
 * takes the TIMER's reading, and the fraction is not carried either.
 */
static inline uint64_t radiant_lp_wake(const struct radiant_lp_tb *tb,
				       struct radiant_lp_clock *c,
				       uint64_t ticks, bool timer_started_at_edge)
{
	uint64_t q = radiant_lp_asleep_q(tb, c, ticks);

	c->anchor_q = q;
	c->anchor_ticks = ticks & tb->mask;
	c->timer_at_anchor = timer_started_at_edge;
	c->awake_frac = timer_started_at_edge ?
			(uint32_t)(q & ((UINT64_C(1) << tb->shift) - 1u)) : 0u;
	return q >> tb->shift;
}

/* ------------------------------------------------------------------------ */
/* Decisions                                                                 */
/* ------------------------------------------------------------------------ */

/*
 * Linger: a deadline before which the backend does not sleep, pushed out to
 * now + linger_us (never pulled in). The backend extends it on enable and on
 * every PRIO_NORMAL acquire (CONFIG_RADIANT_NRF_LP_LINGER_MS).
 */
static inline uint64_t radiant_lp_linger(uint64_t linger_until_us,
					 uint64_t now_us, uint64_t linger_us)
{
	uint64_t until = now_us + linger_us;

	return until > linger_until_us ? until : linger_until_us;
}

/*
 * Sleep now? Only when not lingering (now >= linger_until) and the next
 * operation starts STRICTLY more than wake_lead_us + keep_on_us from now -
 * wake_lead to have the HFXO and the TIMER back before it, keep_on so that a
 * gap barely long enough is not spent stopping and restarting the crystal.
 * No pending operation (RADIANT_LP_NO_OP) sleeps; an operation at or before
 * now does not.
 */
static inline bool radiant_lp_should_sleep(uint64_t now_us,
					   uint64_t next_op_start_us,
					   uint32_t wake_lead_us,
					   uint32_t keep_on_us,
					   uint64_t linger_until_us)
{
	if (now_us < linger_until_us) {
		return false;
	}
	if (next_op_start_us == RADIANT_LP_NO_OP) {
		return true;
	}
	if (next_op_start_us <= now_us) {
		return false;
	}
	return next_op_start_us - now_us >
	       (uint64_t)wake_lead_us + (uint64_t)keep_on_us;
}

/* When to wake for an operation at op_start_us: wake_lead_us before it (0 floor). */
static inline uint64_t radiant_lp_wake_at(uint64_t op_start_us,
					  uint32_t wake_lead_us)
{
	return op_start_us > wake_lead_us ? op_start_us - wake_lead_us : 0u;
}

enum radiant_lp_wake_rc {
	RADIANT_LP_WAKE_OK = 0,   /* *compare is the wake edge */
	RADIANT_LP_WAKE_CLAMPED,  /* too far: *compare is max_ticks out; wake
				   * there and decide again */
	RADIANT_LP_WAKE_PAST,     /* wake_at is before the anchor: do not sleep */
};

/*
 * The LF compare value for a wake at wake_at_us while asleep on *c: the LAST
 * tick whose exact time is at or before wake_at_us (rounded toward earlier,
 * never later - a wake is early by less than one tick). max_ticks caps the
 * distance from the anchor and is itself capped at half the range. Whether the
 * compare is still far enough ahead of the counter to fire is a separate
 * question: radiant_lp_compare_in_time().
 *
 * One 64-by-32-bit division (a library call on Cortex-M, a few hundred cycles
 * at most), once per sleep decision.
 */
static inline enum radiant_lp_wake_rc
radiant_lp_wake_compare(const struct radiant_lp_tb *tb,
			const struct radiant_lp_clock *c, uint64_t wake_at_us,
			uint64_t max_ticks, uint64_t *compare)
{
	uint64_t half = radiant_lp_tb_half(tb);
	uint64_t ticks;
	enum radiant_lp_wake_rc rc = RADIANT_LP_WAKE_OK;

	if (max_ticks > half) {
		max_ticks = half;
	}
	if (wake_at_us > (UINT64_MAX >> tb->shift)) {
		/* Not representable in q units: beyond any reachable anchor. */
		ticks = max_ticks;
		rc = RADIANT_LP_WAKE_CLAMPED;
	} else {
		uint64_t wake_q = wake_at_us << tb->shift;

		if (wake_q < c->anchor_q) {
			*compare = c->anchor_ticks;
			return RADIANT_LP_WAKE_PAST;
		}
		ticks = (wake_q - c->anchor_q) / tb->num;
		if (ticks > max_ticks) {
			ticks = max_ticks;
			rc = RADIANT_LP_WAKE_CLAMPED;
		}
	}
	*compare = (c->anchor_ticks + ticks) & tb->mask;
	return rc;
}

/*
 * How late, in us, a wake that has the HFXO and the TIMER back at now_us is
 * for an operation at op_start_us that needs need_us of arming lead (the
 * backend's min_arm_lead): 0 if in time. Non-zero is the late-anchor case,
 * which the backend reports through its existing FAILED path and counts.
 */
static inline uint64_t radiant_lp_late_by_us(uint64_t now_us,
					     uint64_t op_start_us,
					     uint32_t need_us)
{
	uint64_t ready = now_us + need_us;

	return ready > op_start_us ? ready - op_start_us : 0u;
}

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_RADIO_NRF_LP_H_ */
