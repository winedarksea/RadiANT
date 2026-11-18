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
 * The two meet at LF edges, three per sleep:
 *
 *   RELEASE   an LF compare fires TIMER CAPTURE by (D)PPI, so the
 *             TIMER-domain time of one known LF tick is captured; then the
 *             TIMER stops and the HFXO is released.
 *   EDGE 1    (HFXO) an LF compare interrupt requests the HFXO back, early
 *             enough for its start-up (CONFIG_RADIANT_NRF_LP_HFXO_STARTUP_US)
 *             plus the interrupt's own latency. The TIMER stays stopped.
 *   EDGE 2    (TIMER) once the HFXO is certain to be stable, an LF compare
 *             starts the cleared TIMER by (D)PPI, so the awake segment counts
 *             from a known LF tick on the crystal. This is the edge
 *             radiant_lp_wake() takes.
 *
 * WHY TWO WAKE EDGES. Requesting the HFXO does not stop the HF clock: until
 * the crystal is up the TIMER runs on HFINT, which is only ~1 % accurate. A
 * TIMER started on the HFXO request edge therefore gains or loses ~16 us
 * across a 1650 us start-up. The next release then sees the TIMER's reading
 * that far from the LF image, beyond any sane tolerance, takes the TIMER's
 * (radiant_lp_release), and the error is kept: every period, so ~65 ppm of
 * drift at 4 Hz - the same failure as anchoring every release on the TIMER.
 * Starting the TIMER on a second edge, after start-up, keeps HFINT out of the
 * timebase altogether. radiant/tests/lp_clock's drift simulation models
 * HFINT and shows both outcomes.
 *
 * Everything in this file is the bookkeeping between those edges and the
 * decisions around them; none of it touches a register, so
 * radiant/tests/lp_clock runs it on native_sim. Nothing in the tree includes
 * this header unless CONFIG_RADIANT_NRF_LOW_POWER is set (a header included
 * by a default build can renumber compiler-generated symbols and fail the
 * zero-cost check).
 *
 * UNITS. Every time is a uint64_t in microseconds of the radio timebase, the
 * same scale as radiant_time_t. Sub-microsecond quantities are "q units":
 * 2^-shift microseconds, where 1 LF tick = num q units exactly.
 *
 *   32768 Hz (RTC):  1 tick = 1e6 / 32768 us = 15625 / 512 us
 *                    num = 15625, shift = 9, so q units are 1/512 us.
 *   1 MHz (GRTC):    1 tick = 1 us: num = 1, shift = 0, q units are us.
 *
 * COUNTER VALUES: EXTENDED OR RAW. A format is either
 *   - EXTENDED (width RADIANT_LP_EXTENDED_WIDTH): the caller passes the
 *     software-extended 64-bit count, z_nrf_rtc_timer_read() or
 *     z_nrf_grtc_timer_read(), which never wraps in practice. Nothing is
 *     masked, a compare comes back as the absolute 64-bit target those
 *     drivers' _set() calls take, and a distance that is too long or runs
 *     backwards (a missed compare, a reading taken before the anchor) is
 *     detectable. THE RECOMMENDED MODE.
 *   - RAW (width = the hardware counter's bits, 24 for the RTC, 52 for the
 *     GRTC): values are reduced modulo 2^width and distances are right
 *     across a wrap only while the true distance is under half the range
 *     (256 s on the RTC). A longer one - an RTC compare that was missed and
 *     so fires one counter period later - is indistinguishable from a short
 *     one. Kept for callers that only have the raw register.
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
 *   - us to a compare tick (radiant_lp_wake_compare, radiant_lp_wake_plan):
 *     toward EARLIER. The compare is the last LF tick whose exact time is at
 *     or before the requested time, so an edge is early by less than one
 *     tick (30.5 us on the RTC, 1 us on the GRTC) and never late.
 *   - lead budgets (radiant_lp_wake_lead_us, radiant_lp_lf_overhead_us):
 *     toward MORE time, whole ticks rounded up to whole us.
 *   - release (radiant_lp_release): exact in q units, no rounding; which of
 *     the two exact candidates it takes is the subject of its comment.
 *   - radiant_lp_should_sleep: strictly greater than the whole budget; equal
 *     stays awake.
 *
 * CONCURRENCY. None here, and NOT lock-free. These are pure functions over
 * caller-owned structs holding 64-bit fields; on a 32-bit core a 64-bit field
 * is two stores, and a torn high-water mark or anchor reads up to 2^32 us
 * (71 min) wrong. Every call that reads or writes a struct radiant_lp_clock -
 * radiant_lp_mono() and radiant_lp_clock_now() included, since they raise
 * hwm_us - runs under the backend's CLOCK_LOCK, the same critical section as
 * its TIMER fold (PRIMASK on an MPSL build, so ZLI-safe there).
 *
 * FOR PASS B3 (not done here): BUILD_ASSERT that the system timer has the two
 * user channels this needs (CONFIG_NRF_RTC_TIMER_USER_CHAN_COUNT >= 2 on RTC1;
 * free GRTC channels for z_nrf_grtc_timer_chan_alloc() on the nRF54L), choose
 * the RTC or GRTC format by which system timer is enabled (CONFIG_NRF_RTC_TIMER
 * / CONFIG_NRF_GRTC_TIMER) rather than by SoC, and BUILD_ASSERT the frequency
 * the chosen format assumes (CONFIG_CLOCK_CONTROL_NRF_K32SRC_FREQUENCY ==
 * 32768; the GRTC node's clock-frequency == 1000000).
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
 * (GRTC_SYSCOUNTER_SYSCOUNTERL/H_VALUE_Msk). Either can be used EXTENDED
 * instead, by passing RADIANT_LP_EXTENDED_WIDTH as the width.
 */
#define RADIANT_LP_RTC_NUM         15625u
#define RADIANT_LP_RTC_SHIFT       9u
#define RADIANT_LP_RTC_WIDTH       24u
#define RADIANT_LP_GRTC_NUM        1u
#define RADIANT_LP_GRTC_SHIFT      0u
#define RADIANT_LP_GRTC_WIDTH      52u
#define RADIANT_LP_EXTENDED_WIDTH  64u

/*
 * Least lead, in ticks, a compare needs over the counter to be sure to fire.
 * The nRF52 RTC compare is an equality match, and one written as COUNTER or
 * COUNTER + 1 may not fire (product specification, RTC "COMPARE" section);
 * Zephyr's nrf_rtc_timer keeps MIN_CYCLES_FROM_NOW = 3, and so does this. The
 * GRTC needs a latch time instead, which nrf_grtc_timer bounds at
 * MAX_CC_LATCH_WAIT_TIME_US = 77 SYSCOUNTER ticks; one LF period on top of it
 * is the safe figure. Spike B1 measures both; these are starting values, not
 * measurements.
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
 * timebase. It assumes the TIMER ran on the HFXO for the whole segment
 * (EDGE 2 above); S3's measurement may revise it.
 */
#define RADIANT_LP_RELEASE_TOL_US 3u

/*
 * Time from EDGE 1's compare event to the HFXO request actually issued: the
 * compare interrupt's latency, in us, on top of the crystal's start-up. A
 * starting value; B3 counts late wakes, which is how a too-small one shows.
 */
#define RADIANT_LP_HFXO_ISR_MARGIN_US 100u

/* radiant_lp_should_sleep(): no operation is pending. */
#define RADIANT_LP_NO_OP UINT64_MAX

/* ------------------------------------------------------------------------ */
/* The counter format                                                        */
/* ------------------------------------------------------------------------ */

struct radiant_lp_tb {
	uint64_t mask;   /* 2^width - 1; all ones when extended */
	uint64_t range;  /* the longest trusted distance, ticks */
	uint32_t num;    /* q units per tick */
	uint8_t  shift;  /* q units per us = 2^shift */
	uint8_t  width;  /* counter bits; RADIANT_LP_EXTENDED_WIDTH = extended */
};

/*
 * Fill *tb. False (and *tb untouched) for a format this arithmetic cannot
 * carry exactly in 64 bits: num 0, shift over 31, or a raw width outside
 * 2..63 or with width + bitlength(num) over 63 (which keeps any in-range
 * distance * num below 2^63). The trusted distance is half the range for a
 * raw counter, and for an extended one the largest whose product with num
 * stays below 2^63 (2^49 ticks, ~544 years, on the RTC format).
 */
static inline bool radiant_lp_tb_init(struct radiant_lp_tb *tb, uint32_t num,
				      uint8_t shift, uint8_t width)
{
	uint8_t num_bits = 0;

	for (uint32_t n = num; n != 0u; n >>= 1) {
		num_bits++;
	}
	if (num == 0u || shift > 31u) {
		return false;
	}
	if (width == RADIANT_LP_EXTENDED_WIDTH) {
		tb->mask = UINT64_MAX;
		tb->range = (UINT64_C(1) << (63u - num_bits)) - 1u;
	} else {
		if (width < 2u || width > 63u ||
		    (unsigned int)width + num_bits > 63u) {
			return false;
		}
		tb->mask = (UINT64_C(1) << width) - 1u;
		tb->range = tb->mask >> 1;
	}
	tb->num = num;
	tb->shift = shift;
	tb->width = width;
	return true;
}

/* The longest forward distance, in ticks, that is trusted. */
static inline uint64_t radiant_lp_tb_half(const struct radiant_lp_tb *tb)
{
	return tb->range;
}

/* One tick in whole us, rounded up: 31 on the RTC, 1 on the GRTC. */
static inline uint32_t radiant_lp_tick_us_ceil(const struct radiant_lp_tb *tb)
{
	return (uint32_t)((tb->num + (UINT32_C(1) << tb->shift) - 1u) >>
			  tb->shift);
}

/* Ticks from `from` to `to`, modulo 2^width (raw) or plain (extended). */
static inline uint64_t radiant_lp_ticks_since(const struct radiant_lp_tb *tb,
					      uint64_t from, uint64_t to)
{
	return (to - from) & tb->mask;
}

/*
 * True once the counter has reached `target`: `now` is at or past it by no
 * more than the trusted distance. The backend's "has this edge already gone
 * by" test - e.g. EDGE 2's compare passed but the TIMER is not running, which
 * is a missed compare (a late anchor).
 */
static inline bool radiant_lp_tick_reached(const struct radiant_lp_tb *tb,
					   uint64_t now, uint64_t target)
{
	return radiant_lp_ticks_since(tb, target, now) <= radiant_lp_tb_half(tb);
}

/*
 * True if a compare at `compare` is certain to fire, the counter reading
 * `now`: at least min_lead ticks ahead and not more than the trusted distance
 * ahead (further reads as the past). False is a late anchor: the edge may
 * never come (on the RTC, an equality compare that the counter has already
 * passed fires one counter period - 512 s - later), so nothing may be
 * stopped or started on it.
 *
 * `now` MUST BE READ AFTER THE COMPARE IS WRITTEN, as Zephyr's nrf_rtc_timer
 * does: a value read before the write can be preempted for any length of time
 * before the write lands, and then says nothing about whether the counter got
 * there first.
 */
static inline bool radiant_lp_compare_in_time(const struct radiant_lp_tb *tb,
					      uint64_t now, uint64_t compare,
					      uint32_t min_lead)
{
	uint64_t ahead = radiant_lp_ticks_since(tb, now, compare);

	return ahead >= min_lead && ahead <= radiant_lp_tb_half(tb);
}

/*
 * The release edge for a sleep decided with the counter at `now`: min_lead
 * ticks on, the earliest compare that is certain to fire.
 */
static inline uint64_t radiant_lp_release_edge(const struct radiant_lp_tb *tb,
					       uint64_t now, uint32_t min_lead)
{
	return (now + min_lead) & tb->mask;
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
 * wake it is EDGE 2, and it is what the next release measures the awake
 * segment against.
 */
struct radiant_lp_clock {
	uint64_t anchor_q;      /* timebase at anchor_ticks, q units */
	uint64_t anchor_ticks;  /* LF counter at the anchor (masked if raw) */
	uint64_t hwm_us;        /* highest value radiant_lp_mono() returned */
	uint32_t awake_frac;    /* awake segment's sub-us offset, q units: the
				 * TIMER reads whole us from wake_us, the true
				 * time is that plus this */
	uint32_t range_errors;  /* asleep readings refused as out of range */
	bool     timer_at_anchor; /* the awake TIMER's relation to the anchor
				   * edge is exact: it was started from zero by
				   * that edge, or captured on it */
};

/* Start in the awake state on a TIMER that has free-run since boot. */
static inline void radiant_lp_clock_init(struct radiant_lp_clock *c)
{
	c->anchor_q = 0u;
	c->anchor_ticks = 0u;
	c->hwm_us = 0u;
	c->awake_frac = 0u;
	c->range_errors = 0u;
	c->timer_at_anchor = false;
}

/*
 * Exact asleep timebase at LF counter `ticks`, q units. Unchecked: `ticks`
 * must be within the trusted distance past the anchor (radiant_lp_asleep_ok).
 */
static inline uint64_t radiant_lp_asleep_q(const struct radiant_lp_tb *tb,
					   const struct radiant_lp_clock *c,
					   uint64_t ticks)
{
	return c->anchor_q +
	       radiant_lp_ticks_since(tb, c->anchor_ticks, ticks) * tb->num;
}

/*
 * True if `ticks` is a reading the asleep conversion can trust: at or after
 * the anchor by no more than the trusted distance. False means the timebase
 * cannot say what time it is from this reading - with an extended counter, a
 * reading from before the anchor or a sleep that overran by centuries; with a
 * raw one, a reading past half the range, which also catches a missed RTC
 * compare once it is more than 256 s old.
 */
static inline bool radiant_lp_asleep_ok(const struct radiant_lp_tb *tb,
					const struct radiant_lp_clock *c,
					uint64_t ticks)
{
	return radiant_lp_ticks_since(tb, c->anchor_ticks, ticks) <=
	       radiant_lp_tb_half(tb);
}

/* Asleep timebase at LF counter `ticks`, us, toward zero. Unchecked. */
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
 * still until it catches up, rather than running backwards. Writes hwm_us:
 * under CLOCK_LOCK (see CONCURRENCY above).
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
 * produced; asleep, the LF conversion of `ticks`. Either way clamped. An
 * asleep reading that fails radiant_lp_asleep_ok() is not converted (a wrapped
 * conversion would be garbage, or would freeze the clamp for a counter
 * period): it counts in range_errors and returns the high-water mark, and the
 * backend treats a non-zero range_errors as a lost timebase. Under CLOCK_LOCK.
 */
static inline uint64_t radiant_lp_clock_now(const struct radiant_lp_tb *tb,
					    struct radiant_lp_clock *c,
					    bool asleep, uint64_t timer_us,
					    uint64_t ticks)
{
	if (!asleep) {
		return radiant_lp_mono(c, timer_us);
	}
	if (!radiant_lp_asleep_ok(tb, c, ticks)) {
		c->range_errors++;
		return c->hwm_us;
	}
	return radiant_lp_mono(c, radiant_lp_asleep_us(tb, c, ticks));
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
 *   y, the LF image: the anchor (EDGE 2, or a re-anchor capture) plus the
 *      exact conversion of the LF ticks from it to this edge. Only defined if
 *      the TIMER's relation to that anchor is exact (timer_at_anchor). Exact
 *      in the LF domain, so taking y at every release leaves the arithmetic
 *      locked to the LF crystal with no accumulation at all.
 *
 * y is taken when it exists and lies within tol_us of x. Over a short awake
 * segment (a master slot: ~5 ms) on the HFXO the two crystals agree to well
 * under 1 us, so y is always taken and the long-term timebase is the LF clock.
 * Over a long one (a 1 s linger, a search) the HF crystal has genuinely run
 * apart from the LF one by tens of us; the awake readings in that segment
 * were HF time, so x is taken and the step stays zero. Either way the release
 * moves the timebase by at most tol_us from the last awake reading.
 *
 * The tolerance assumes the TIMER ran on the HFXO throughout. A TIMER that
 * ran on HFINT for part of the segment (started on the HFXO request edge)
 * is tens of us out, x is taken every period, and the timebase drifts: hence
 * EDGE 2.
 */
static inline bool radiant_lp_release(const struct radiant_lp_tb *tb,
				      struct radiant_lp_clock *c,
				      uint64_t ticks, uint64_t timer_us,
				      uint32_t tol_us, int64_t *lf_minus_timer_q)
{
	uint64_t x = (timer_us << tb->shift) + c->awake_frac;
	bool take_lf = false;
	int64_t diff = 0;

	if (c->timer_at_anchor && radiant_lp_asleep_ok(tb, c, ticks)) {
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
 * RE-ANCHOR while awake, without stopping anything: a release that does not
 * sleep. `ticks` is an LF edge on which (D)PPI captured the running TIMER as
 * `timer_us`. The anchor becomes that edge at the TIMER's reading of it, and
 * timer_at_anchor becomes true, so the next release can take the LF image
 * again.
 *
 * What it is for: a wake whose TIMER was started in software
 * (radiant_lp_wake(..., false), a late anchor's recovery) has no exact
 * relation to any LF edge. Its awake readings are off the LF image by the
 * software start's delay - less than one LF tick plus the start's interrupt
 * latency (~31 us + latency on the RTC, ~1 us + latency on the GRTC). That
 * offset is taken into the timebase ONCE, at the next release, and does not
 * grow: the wake after it is an exact (D)PPI start again. Re-anchoring inside
 * the segment changes nothing about that offset (the TIMER's reading is what
 * the awake readings already said) but restores the exact relation for the
 * rest of the segment, so a long software-started segment's release is
 * judged against the LF image rather than taken on trust. Also usable on the
 * boot TIMER, which has free-run since before any LF edge was known.
 */
static inline void radiant_lp_reanchor(const struct radiant_lp_tb *tb,
				       struct radiant_lp_clock *c,
				       uint64_t ticks, uint64_t timer_us)
{
	c->anchor_q = (timer_us << tb->shift) + c->awake_frac;
	c->anchor_ticks = ticks & tb->mask;
	c->timer_at_anchor = true;
	/* awake_frac is unchanged: it is the TIMER domain's offset, and the
	 * TIMER keeps running from the same base.
	 */
}

/*
 * WAKE at EDGE 2 (`ticks`). Returns the us the backend loads as its TIMER
 * fold's base (with the fold's `last` = 0, the TIMER having been cleared):
 * the asleep reading at that edge, toward zero, so the awake readings
 * continue the asleep ones without a step. The fraction below it is carried
 * in awake_frac.
 *
 * timer_started_at_edge: true when the (D)PPI connection started the cleared
 * TIMER on this very edge, on a stable HFXO (the normal path). False when the
 * backend had to start it in software after reading `ticks` (a late anchor's
 * recovery); see radiant_lp_reanchor() for the bound that leaves and how to
 * restore the exact relation.
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

/*
 * REBASE while asleep, with no HFXO and no TIMER: move the anchor forward to
 * LF edge `ticks`, at its exact LF image. Nothing about the timebase changes;
 * only the distance future readings convert over gets short again. For a
 * clamped wake (RADIANT_LP_WAKE_CLAMPED: the operation is beyond the trusted
 * distance, or there is none - RADIANT_LP_NO_OP) the backend arms only a
 * compare at the clamped tick, rebases there, and decides again.
 */
static inline void radiant_lp_rebase(const struct radiant_lp_tb *tb,
				     struct radiant_lp_clock *c, uint64_t ticks)
{
	c->anchor_q = radiant_lp_asleep_q(tb, c, ticks);
	c->anchor_ticks = ticks & tb->mask;
	c->awake_frac = 0u;
	c->timer_at_anchor = false;
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
 * The whole wake lead, in us, an operation needs before it: arm_lead_us (the
 * TIMER running and the HFXO stable that long before the operation - the
 * backend's own arming lead), one tick of EDGE 2's rounding toward earlier,
 * hfxo_lead_us (start-up plus RADIANT_LP_HFXO_ISR_MARGIN_US, from EDGE 1 to
 * EDGE 2), and one tick of EDGE 1's rounding. Ticks round up.
 */
static inline uint64_t radiant_lp_wake_lead_us(const struct radiant_lp_tb *tb,
					       uint32_t arm_lead_us,
					       uint32_t hfxo_lead_us)
{
	return (uint64_t)arm_lead_us + hfxo_lead_us +
	       2u * (uint64_t)radiant_lp_tick_us_ceil(tb);
}

/*
 * What going to sleep costs in LF ticks before the wake lead starts, in us:
 * the release edge is min_lead ticks after the decision (plus up to a tick,
 * the counter having been read mid-tick), and EDGE 1 at least min_lead after
 * the release edge. 2 * min_lead + 1 ticks, rounded up: 214 us on the RTC
 * (min_lead 3), 217 on the GRTC (min_lead 108). Interrupt latency on top of
 * that is the keep-on margin's to cover.
 */
static inline uint64_t radiant_lp_lf_overhead_us(const struct radiant_lp_tb *tb,
						 uint32_t min_lead)
{
	uint64_t q = (2u * (uint64_t)min_lead + 1u) * tb->num;

	return (q + (UINT64_C(1) << tb->shift) - 1u) >> tb->shift;
}

/*
 * Sleep now? Only when not lingering (now >= linger_until) and the next
 * operation starts STRICTLY more than
 *     wake_lead_us (radiant_lp_wake_lead_us)
 *   + lf_overhead_us (radiant_lp_lf_overhead_us)
 *   + keep_on_us (CONFIG_RADIANT_NRF_LP_KEEP_ON_US)
 * from now: the two wake edges' budget, the release edge's, and a margin so
 * that a gap barely long enough is not spent stopping and restarting the
 * crystal. No pending operation (RADIANT_LP_NO_OP) sleeps; an operation at or
 * before now does not.
 *
 * This is the decision BEFORE anything is committed. Once the release edge is
 * armed, a wake plan that comes back RADIANT_LP_WAKE_PAST, or an edge that
 * fails radiant_lp_compare_in_time(), does not mean "do not sleep" - the
 * TIMER may already be stopping - it is a late anchor: request the HFXO and
 * start the TIMER now, and let the operation fail late (and be counted) if
 * it must.
 */
static inline bool radiant_lp_should_sleep(uint64_t now_us,
					   uint64_t next_op_start_us,
					   uint64_t wake_lead_us,
					   uint64_t lf_overhead_us,
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
	       wake_lead_us + lf_overhead_us + (uint64_t)keep_on_us;
}

/* `t` minus `lead`, floored at 0. */
static inline uint64_t radiant_lp_wake_at(uint64_t t, uint64_t lead)
{
	return t > lead ? t - lead : 0u;
}

enum radiant_lp_wake_rc {
	RADIANT_LP_WAKE_OK = 0,   /* the compare(s) are the wake edge(s) */
	RADIANT_LP_WAKE_CLAMPED,  /* too far: the compare is max_ticks out; wake
				   * there with no HFXO, radiant_lp_rebase(),
				   * decide again */
	RADIANT_LP_WAKE_PAST,     /* the time is before the anchor (or leaves no
				   * room for both edges): see should_sleep on
				   * what that means after a commit */
};

/*
 * The LF compare value for an edge at `at_us` while asleep on *c: the LAST
 * tick whose exact time is at or before at_us (rounded toward earlier, never
 * later - an edge is early by less than one tick). max_ticks caps the
 * distance from the anchor and is itself capped at the trusted distance.
 * Whether the compare is still far enough ahead of the counter to fire is a
 * separate question: radiant_lp_compare_in_time().
 *
 * One 64-by-32-bit division (a library call on Cortex-M, a few hundred cycles
 * at most).
 */
static inline enum radiant_lp_wake_rc
radiant_lp_wake_compare(const struct radiant_lp_tb *tb,
			const struct radiant_lp_clock *c, uint64_t at_us,
			uint64_t max_ticks, uint64_t *compare)
{
	uint64_t half = radiant_lp_tb_half(tb);
	uint64_t ticks;
	enum radiant_lp_wake_rc rc = RADIANT_LP_WAKE_OK;

	if (max_ticks > half) {
		max_ticks = half;
	}
	if (at_us > (UINT64_MAX >> tb->shift)) {
		/* Not representable in q units: beyond any reachable anchor. */
		ticks = max_ticks;
		rc = RADIANT_LP_WAKE_CLAMPED;
	} else {
		uint64_t at_q = at_us << tb->shift;

		if (at_q < c->anchor_q) {
			*compare = c->anchor_ticks;
			return RADIANT_LP_WAKE_PAST;
		}
		ticks = (at_q - c->anchor_q) / tb->num;
		if (ticks > max_ticks) {
			ticks = max_ticks;
			rc = RADIANT_LP_WAKE_CLAMPED;
		}
	}
	*compare = (c->anchor_ticks + ticks) & tb->mask;
	return rc;
}

/* Both wake edges for one operation. */
struct radiant_lp_wake_plan {
	uint64_t hfxo_cmp;   /* EDGE 1: the interrupt requests the HFXO */
	uint64_t timer_cmp;  /* EDGE 2: (D)PPI starts the cleared TIMER; the
			      * edge radiant_lp_wake() takes */
};

/*
 * Plan the wake for an operation at op_start_us, asleep on *c:
 *
 *   EDGE 2 is the last tick at or before op_start_us - arm_lead_us;
 *   EDGE 1 is the last tick at or before EDGE 2's exact time - hfxo_lead_us,
 *          and strictly before EDGE 2,
 *
 * so the HFXO has at least hfxo_lead_us (start-up + request latency) before
 * the TIMER starts, and the TIMER and HFXO at least arm_lead_us before the
 * operation. In all, the wake begins less than radiant_lp_wake_lead_us() before
 * the operation.
 *
 * RADIANT_LP_WAKE_CLAMPED: the operation is beyond max_ticks (or the trusted
 * distance) - both fields hold the clamped tick; wake there without the HFXO,
 * radiant_lp_rebase(), plan again. RADIANT_LP_WAKE_PAST: EDGE 1 would fall
 * before the anchor. Each compare must still pass radiant_lp_compare_in_time()
 * when written.
 */
static inline enum radiant_lp_wake_rc
radiant_lp_wake_plan(const struct radiant_lp_tb *tb,
		     const struct radiant_lp_clock *c, uint64_t op_start_us,
		     uint32_t arm_lead_us, uint32_t hfxo_lead_us,
		     uint64_t max_ticks, struct radiant_lp_wake_plan *plan)
{
	uint64_t t2, d2, q2, lead_q, d1;
	enum radiant_lp_wake_rc rc;

	rc = radiant_lp_wake_compare(tb, c,
				     radiant_lp_wake_at(op_start_us, arm_lead_us),
				     max_ticks, &t2);
	plan->timer_cmp = t2;
	plan->hfxo_cmp = t2;
	if (rc != RADIANT_LP_WAKE_OK) {
		return rc;
	}
	d2 = radiant_lp_ticks_since(tb, c->anchor_ticks, t2);
	q2 = c->anchor_q + d2 * tb->num;
	lead_q = (uint64_t)hfxo_lead_us << tb->shift;
	if (d2 == 0u || q2 < c->anchor_q + lead_q) {
		return RADIANT_LP_WAKE_PAST;
	}
	d1 = (q2 - lead_q - c->anchor_q) / tb->num;
	if (d1 >= d2) {
		d1 = d2 - 1u;
	}
	plan->hfxo_cmp = (c->anchor_ticks + d1) & tb->mask;
	return RADIANT_LP_WAKE_OK;
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
