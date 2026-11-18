/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_rat_fold.h - classify one step of a free-running 32-bit counter.
 *
 * Provenance: clean-room. Written from this project's own source
 * (rat_fold() in radiant_radio_cc26xx.c) and from the read path of TI's RF
 * driver as shipped in hal_ti (RF_getCurrentTime(): with the RF core off it
 * returns the AON RTC scaled to 4 MHz plus one RTC tick).
 *
 * WHY. rat_fold() widens the CC26x2's 32-bit 4 MHz radio timer to 64 bits by
 * adding 2^32 whenever a read is below the previous one. That is right for a
 * genuine wrap and wrong for any other backward step: a read synthesised from
 * the RTC while the RF core is off is rounded UP by one RTC tick (about 122
 * RAT ticks), so the next real read after power-up can sit a little below it,
 * and the fold then jumps the clock forward by 2^32 ticks (about 1074 s).
 *
 * The two cases are told apart by size. Reads come at least once a minute
 * (the backend's keepalive), far below half the 1074 s wrap, so a genuine wrap
 * always shows as a backward step of more than 2^31 ticks (the counter went
 * almost all the way round), and a backward step of less than 2^31 is never a
 * wrap.
 *
 * TODAY THIS ONLY MEASURES. rat_fold() still adds 2^32 on every backward step;
 * under CONFIG_RADIANT_POWER_DIAG it calls the classifier below and counts
 * the steps it treated wrongly (radiant_power_diag.ti_fold_backsteps). A fold
 * that clamps instead is a separate, default-on correctness change.
 *
 * Pure functions over plain integers, so radiant/tests/src/test_power_diag.c
 * runs them on native_sim.
 */

#ifndef RADIANT_RAT_FOLD_H_
#define RADIANT_RAT_FOLD_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A backward step at least this large is a genuine wrap. */
#define RADIANT_RAT_WRAP_MIN_BACKSTEP 0x80000000u

enum radiant_rat_step {
	RADIANT_RAT_STEP_FORWARD = 0, /* now >= last: the ordinary case */
	RADIANT_RAT_STEP_WRAP,        /* the counter went round */
	RADIANT_RAT_STEP_BACK,        /* it went backwards a little */
};

static inline enum radiant_rat_step radiant_rat_step_classify(uint32_t last,
								uint32_t now)
{
	if (now >= last) {
		return RADIANT_RAT_STEP_FORWARD;
	}
	return ((uint32_t)(last - now) >= RADIANT_RAT_WRAP_MIN_BACKSTEP)
		       ? RADIANT_RAT_STEP_WRAP
		       : RADIANT_RAT_STEP_BACK;
}

/* How far a RADIANT_RAT_STEP_BACK step went backwards, in counter ticks. 0
 * for any other step. */
static inline uint32_t radiant_rat_backstep_ticks(uint32_t last, uint32_t now)
{
	return (radiant_rat_step_classify(last, now) == RADIANT_RAT_STEP_BACK)
		       ? (uint32_t)(last - now)
		       : 0u;
}

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_RAT_FOLD_H_ */
