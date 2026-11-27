/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * One pass of the sensor worker. See hrm_sensor_pass.h for why the order is
 * what it is.
 */

#include "hrm_sensor_pass.h"

void hrm_pass_init(struct hrm_pass_state *s)
{
	s->rate_seq = 0u;
	s->losses = 0u;
	s->contact = true;
}

void hrm_pass_run(struct hrm_pass_state *s, uint32_t rate_word, uint32_t losses,
		  const struct hrm_pass_ops *ops)
{
	bool level;

	/* 1. A new loss first: this pass's beats go in without contact. */
	if (losses != s->losses) {
		s->losses = losses;
		s->contact = false;
		ops->contact(ops->ctx, false);
	}

	/* 2. The newest rate, once per pass in which any report arrived. */
	if ((rate_word >> 8) != s->rate_seq) {
		s->rate_seq = rate_word >> 8;
		ops->rate(ops->ctx, (uint8_t)rate_word);
	}

	/* 3. Every beat, oldest first. */
	ops->drain(ops->ctx);

	/* 4. Contact as it is NOW: a regain after the drain, so beats drained
	 * while the pass believed contact lost stay without it; and a loss
	 * the counter has not shown yet, at once (the next pass then sees the
	 * counter move and delivers it again, which is harmless). */
	level = ops->level(ops->ctx);
	if (level != s->contact) {
		s->contact = level;
		ops->contact(ops->ctx, level);
	}
}
