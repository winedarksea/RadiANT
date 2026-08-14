/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_schedule.c - the descriptor schedule block.
 *
 * Provenance: docs/radiant-telemetry.md sections 6 and 12, this project's own
 * written specification, authored in advance of any code. No ANT+
 * device profile document was read for this file, no sdk-ant source was
 * consulted, and nothing here derives from libant.a. See
 * docs/decisions/0002-clean-room-policy.md. The argument for every field is in
 * profile_schedule.h.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <radiant/radiant_frame.h>

#include "profile_bits.h"
#include "profile_schedule.h"

/* The whole body is assigned; adding a field means taking bits from the
 * reservation in this sum rather than discovering an overlap on air. */
_Static_assert(PROFILE_SCHED_DWELL_W + PROFILE_SCHED_INTERVAL_W +
		       PROFILE_SCHED_PHASE_W + PROFILE_SCHED_CODING_W +
		       PROFILE_SCHED_POWER_W + PROFILE_SCHED_RSVD_W ==
	       PROFILE_SCHED_AREA_BITS,
	       "the schedule block's fields no longer tile its 48 bits");
_Static_assert(PROFILE_SCHED_RSVD_OFF + PROFILE_SCHED_RSVD_W ==
	       PROFILE_SCHED_AREA_BITS,
	       "the reservation is not at the end of the block");
_Static_assert(PROFILE_SCHED_CODING_COUNT <= (1u << PROFILE_SCHED_CODING_W),
	       "the coding-rate vocabulary outgrew its field");

/* 1e6 / 32768 exactly, as a ratio, so counts convert to microseconds without
 * floating point or the rounding error a us-per-count constant would
 * accumulate over a 128 s interval. */
#define US_NUM 15625u
#define US_DEN 512u

static const uint32_t dwell_us[PROFILE_SCHED_DWELL_CODE_MAX + 1u] = {
	250u, 500u, 1000u, 2000u, 4000u, 8000u, 16000u, 32000u,
};

uint32_t profile_sched_dwell_us(uint8_t dwell_code)
{
	if (dwell_code > PROFILE_SCHED_DWELL_CODE_MAX) {
		return 0u;
	}
	return dwell_us[dwell_code];
}

uint16_t profile_sched_coding_kbps(uint8_t coding)
{
	switch (coding) {
	case PROFILE_SCHED_CODING_NONE:
		return 1000u;
	case PROFILE_SCHED_CODING_S8:
		return 125u;
	case PROFILE_SCHED_CODING_S2:
		return 500u;
	default:
		return 0u;
	}
}

bool profile_sched_coding_implemented(uint8_t coding)
{
	/* S=8 is not built yet either (the long-range phase builds it), but is
	 * the rate this project has committed to. S=2 and the reserved codes
	 * are refused - nothing transmits them without a further decision. */
	return coding == PROFILE_SCHED_CODING_NONE ||
	       coding == PROFILE_SCHED_CODING_S8;
}

/* ---------------------------------------------------------------------------
 * The duty bound
 * ---------------------------------------------------------------------------
 */

/* Defined below, next to the ratio it uses. Forward-declared rather than
 * moved so that this section reads in the order the header introduces it. */
static uint64_t counts_to_us(uint64_t counts);

/*
 * Which frame configuration a coding rate describes - the one place the
 * profile layer and the frame layer meet, as a function rather than an
 * assumption spread over two files. Uncoded is the ANT tracking geometry;
 * S=8 is the long-range one. S=2 maps to nothing (nothing builds it) rather
 * than quietly budgeting it as S=8, which would silently under-report duty.
 */
static int coding_cfg(uint8_t coding)
{
	switch (coding) {
	case PROFILE_SCHED_CODING_NONE:
		return (int)RADIANT_FRAME_CFG_TRACKING;
	case PROFILE_SCHED_CODING_S8:
		return (int)RADIANT_FRAME_CFG_LR;
	default:
		return -1;
	}
}

uint32_t profile_sched_frame_us(uint8_t coding, uint8_t body_len)
{
	int cfg = coding_cfg(coding);
	int hdr;

	if (cfg < 0) {
		return 0u;
	}
	hdr = radiant_frame_hdr_len((enum radiant_frame_cfg)cfg);
	if (hdr < 0 || (int)body_len < hdr) {
		return 0u;
	}

	/* radiant_frame_airtime_us() is stated in payload rather than body,
	 * since that is what a caller choosing a frame actually varies. */
	return radiant_frame_airtime_us((enum radiant_frame_cfg)cfg,
					(uint8_t)((int)body_len - hdr));
}

int profile_sched_duty_check(uint8_t coding, uint16_t period_counts,
			     uint8_t body_len)
{
	uint32_t frame_us;
	uint64_t period_us;

	frame_us = profile_sched_frame_us(coding, body_len);
	if (frame_us == 0u) {
		/* A rate nothing implements, or a body the matching format
		 * cannot carry - a frame that will never exist, not a duty
		 * question. */
		return -EINVAL;
	}

	if (period_counts == 0u) {
		/* Asynchronous: no period to take a quarter of. */
		return 0;
	}

	period_us = counts_to_us((uint64_t)period_counts);

	/* frame * DEN > period * NUM, rather than frame/period > NUM/DEN:
	 * integer division would round a frame at 25.4% down to 25% and pass
	 * it. Both sides fit in 64 bits by a wide margin. */
	if ((uint64_t)frame_us * PROFILE_SCHED_DUTY_DEN >
	    period_us * PROFILE_SCHED_DUTY_NUM) {
		return -EINVAL;
	}

	return 0;
}

uint32_t profile_sched_interval_counts(const struct profile_schedule *s)
{
	if (s == NULL || s->dl_interval == 0u) {
		return 0u;
	}
	return (uint32_t)s->dl_interval * PROFILE_SCHED_INTERVAL_COUNTS;
}

static uint64_t counts_to_us(uint64_t counts)
{
	return (counts * US_NUM) / US_DEN;
}

/* ---------------------------------------------------------------------------
 * The clock nibble
 * ---------------------------------------------------------------------------
 */

uint16_t profile_sched_clk_ppm(uint8_t nibble)
{
	if ((nibble & PROFILE_SCHED_CLK_STATED) == 0u) {
		/* Nothing stated - not "500 ppm" or "20 ppm": every node built
		 * before this block existed transmits these bits as zeros. */
		return 0u;
	}
	return profile_handoff_clk_ppm((uint8_t)(nibble & PROFILE_SCHED_CLK_MASK));
}

uint8_t profile_sched_clk_nibble(uint8_t code, bool stated)
{
	if (!stated || code >= PROFILE_HANDOFF_CLK_COUNT) {
		return 0u;
	}
	return (uint8_t)(PROFILE_SCHED_CLK_STATED |
			 (code & PROFILE_SCHED_CLK_MASK));
}

uint16_t profile_sched_apply_clock(uint8_t channel, uint8_t nibble)
{
	uint16_t ppm = profile_sched_clk_ppm(nibble);

	radiant_channel_clock_accuracy_set(channel, ppm);
	return radiant_channel_clock_accuracy_get(channel);
}

/* ---------------------------------------------------------------------------
 * Validation, pack, unpack
 * ---------------------------------------------------------------------------
 */

int profile_sched_check(const struct profile_schedule *s, bool lr_phy,
			uint16_t clk_ppm)
{
	uint64_t phase_us;
	uint64_t drift_us;

	if (s == NULL) {
		return -EINVAL;
	}
	if (s->dl_interval > PROFILE_SCHED_INTERVAL_MAX) {
		return -EINVAL;
	}
	if (s->dl_dwell > PROFILE_SCHED_DWELL_CODE_MAX) {
		return -EINVAL;
	}
	if (s->coding >= (1u << PROFILE_SCHED_CODING_W)) {
		return -EINVAL;
	}
	if (s->tx_power_dbm != PROFILE_SCHED_TX_POWER_UNSTATED &&
	    (s->tx_power_dbm < PROFILE_SCHED_TX_POWER_MIN ||
	     s->tx_power_dbm > PROFILE_SCHED_TX_POWER_MAX)) {
		return -EINVAL;
	}

	/* Frame 0's long-range bit and this field must agree - two statements
	 * about one PHY. */
	if (lr_phy != (s->coding != PROFILE_SCHED_CODING_NONE)) {
		return -EINVAL;
	}
	if (!profile_sched_coding_implemented(s->coding)) {
		return -ENOTSUP;
	}

	if (s->dl_interval == 0u) {
		/* No window: section 11's rule, applied to two fields that
		 * would otherwise describe one nobody opens. */
		if (s->dl_phase != 0u || s->dl_dwell != 0u) {
			return -EINVAL;
		}
		return 0;
	}

	/* A phase at or past the interval is a window in the next cycle
	 * described as one in this one. */
	if ((uint32_t)s->dl_phase >= profile_sched_interval_counts(s)) {
		return -EINVAL;
	}

	/* The dwell must cover the clock error accumulated between the
	 * announcing frame and the window itself, in BOTH directions (the
	 * receiver aims at the middle and can be early or late) - so twice
	 * the drift. A 500 ppm node pointing two seconds ahead has drifted a
	 * millisecond by the time it opens, so announcing a window too narrow
	 * for that drift would produce a downlink that never works. */
	if (clk_ppm != 0u) {
		phase_us = counts_to_us(s->dl_phase);
		drift_us = (phase_us *
			    ((uint64_t)clk_ppm + RADIANT_CHANNEL_CLOCK_PPM_ANT)) /
			   1000000u;
		if ((uint64_t)profile_sched_dwell_us(s->dl_dwell) <
		    (2u * drift_us)) {
			return -EINVAL;
		}
	}

	return 0;
}

int profile_sched_pack(const struct profile_schedule *s, bool lr_phy,
		       uint16_t clk_ppm, uint8_t *body)
{
	int rc;

	if (body == NULL) {
		return -EINVAL;
	}
	rc = profile_sched_check(s, lr_phy, clk_ppm);
	if (rc != 0) {
		return rc;
	}

	memset(body, 0, PROFILE_SCHED_AREA_BITS / 8u);

	(void)profile_bits_pack(body, PROFILE_SCHED_AREA_BITS,
				PROFILE_SCHED_DWELL_OFF, PROFILE_SCHED_DWELL_W,
				s->dl_dwell);
	(void)profile_bits_pack(body, PROFILE_SCHED_AREA_BITS,
				PROFILE_SCHED_INTERVAL_OFF,
				PROFILE_SCHED_INTERVAL_W, s->dl_interval);
	(void)profile_bits_pack(body, PROFILE_SCHED_AREA_BITS,
				PROFILE_SCHED_PHASE_OFF, PROFILE_SCHED_PHASE_W,
				s->dl_phase);
	(void)profile_bits_pack(body, PROFILE_SCHED_AREA_BITS,
				PROFILE_SCHED_CODING_OFF, PROFILE_SCHED_CODING_W,
				s->coding);
	if (s->tx_power_dbm != PROFILE_SCHED_TX_POWER_UNSTATED) {
		(void)profile_bits_pack(
			body, PROFILE_SCHED_AREA_BITS, PROFILE_SCHED_POWER_OFF,
			PROFILE_SCHED_POWER_W,
			(uint64_t)(int64_t)((int)s->tx_power_dbm +
					    PROFILE_SCHED_POWER_BIAS));
	}

	/* The reservation stays zero from the memset, per section 11. */
	return 0;
}

int profile_sched_unpack(const uint8_t *body, bool lr_phy, uint16_t clk_ppm,
			 struct profile_schedule *out)
{
	struct profile_schedule s;
	uint64_t value;
	int rc;

	if (body == NULL || out == NULL) {
		return -EINVAL;
	}

	memset(&s, 0, sizeof(s));

	(void)profile_bits_unpack(body, PROFILE_SCHED_AREA_BITS,
				  PROFILE_SCHED_RSVD_OFF, PROFILE_SCHED_RSVD_W,
				  &value);
	if (value != 0u) {
		/* Reserved-must-be-zero; unlike an unknown informational flag,
		 * these bits have no defined meaning a receiver could assume. */
		return -EPROTO;
	}

	(void)profile_bits_unpack(body, PROFILE_SCHED_AREA_BITS,
				  PROFILE_SCHED_DWELL_OFF, PROFILE_SCHED_DWELL_W,
				  &value);
	s.dl_dwell = (uint8_t)value;
	(void)profile_bits_unpack(body, PROFILE_SCHED_AREA_BITS,
				  PROFILE_SCHED_INTERVAL_OFF,
				  PROFILE_SCHED_INTERVAL_W, &value);
	s.dl_interval = (uint16_t)value;
	(void)profile_bits_unpack(body, PROFILE_SCHED_AREA_BITS,
				  PROFILE_SCHED_PHASE_OFF, PROFILE_SCHED_PHASE_W,
				  &value);
	s.dl_phase = (uint16_t)value;
	(void)profile_bits_unpack(body, PROFILE_SCHED_AREA_BITS,
				  PROFILE_SCHED_CODING_OFF,
				  PROFILE_SCHED_CODING_W, &value);
	s.coding = (uint8_t)value;
	(void)profile_bits_unpack(body, PROFILE_SCHED_AREA_BITS,
				  PROFILE_SCHED_POWER_OFF, PROFILE_SCHED_POWER_W,
				  &value);
	if (value == 0u) {
		s.tx_power_dbm = PROFILE_SCHED_TX_POWER_UNSTATED;
	} else {
		int dbm = (int)value - PROFILE_SCHED_POWER_BIAS;

		/* Range-checked before the narrowing cast: biased byte 228
		 * minus 100 is 128, which lands on the sentinel if squeezed
		 * into an int8 first, misreading nonsense as "no announcement". */
		if (dbm < PROFILE_SCHED_TX_POWER_MIN ||
		    dbm > PROFILE_SCHED_TX_POWER_MAX) {
			return -EINVAL;
		}
		s.tx_power_dbm = (int8_t)dbm;
	}

	/* A coding rate this build cannot use is NOT a decode error - the
	 * forward-compatibility rule of section 6: the node describes itself,
	 * not the layout of these bytes, so a receiver decodes and then
	 * declines the channel. Everything else is checked. */
	rc = profile_sched_check(&s, lr_phy, clk_ppm);
	if (rc == -ENOTSUP) {
		rc = 0;
	}
	if (rc != 0) {
		return rc;
	}

	*out = s;
	return 0;
}

radiant_time_t profile_sched_listen_at(const struct profile_schedule *s,
				       radiant_time_t t_carrier, uint32_t k)
{
	uint64_t counts;

	if (s == NULL || s->dl_interval == 0u) {
		return RADIANT_TIME_NEVER;
	}

	counts = (uint64_t)s->dl_phase +
		 ((uint64_t)k * (uint64_t)profile_sched_interval_counts(s));

	return t_carrier + (radiant_time_t)counts_to_us(counts);
}

/* ---------------------------------------------------------------------------
 * The node side - the response-slot phase
 * ---------------------------------------------------------------------------
 */

/* Microseconds back into 1/32768 s counts, rounded to nearest rather than
 * truncated: truncation would bias every announced phase early by up to
 * 30 us, always in the same direction. */
static uint32_t us_to_counts(uint64_t us)
{
	return (uint32_t)(((us * US_DEN) + (US_NUM / 2u)) / US_NUM);
}

int32_t profile_sched_phase_for(const struct profile_schedule *s,
				radiant_time_t t_anchor, radiant_time_t t_carrier)
{
	uint32_t interval_counts;
	uint64_t interval_us;
	uint64_t delta_us;
	uint32_t counts;

	if (s == NULL) {
		return -1;
	}
	interval_counts = profile_sched_interval_counts(s);
	if (interval_counts == 0u) {
		return -1;
	}
	interval_us = counts_to_us((uint64_t)interval_counts);
	if (interval_us == 0u) {
		return -1;
	}

	/* Reduce the anchor onto the interval grid relative to the carrier, in
	 * unsigned arithmetic: the two operands can sit either side of each
	 * other, so the subtraction is done once in each order and the branch
	 * picks the one that did not wrap. */
	if (t_anchor >= t_carrier) {
		delta_us = (uint64_t)(t_anchor - t_carrier) % interval_us;
	} else {
		uint64_t behind = (uint64_t)(t_carrier - t_anchor) % interval_us;

		delta_us = (behind == 0u) ? 0u : (interval_us - behind);
	}

	counts = us_to_counts(delta_us);
	if (counts >= interval_counts) {
		/* The rounding above can push a delta within half a count of a
		 * full interval onto the interval, an illegal phase. Clamp
		 * DOWN to the last representable one; wrapping to phase 0
		 * instead would announce a window up to 128 s early against a
		 * dwell measured in microseconds - the receiver would miss
		 * every time. Clamping down is at most 30.5 us early, inside
		 * the smallest dwell in the ladder. */
		counts = interval_counts - 1u;
	}

	return (int32_t)counts;
}

int profile_sched_rephase(uint8_t *body, uint16_t phase)
{
	struct profile_schedule s;
	uint64_t value;

	if (body == NULL) {
		return -EINVAL;
	}

	(void)profile_bits_unpack(body, PROFILE_SCHED_AREA_BITS,
				  PROFILE_SCHED_INTERVAL_OFF,
				  PROFILE_SCHED_INTERVAL_W, &value);
	if (value == 0u) {
		return -ENOENT;
	}

	memset(&s, 0, sizeof(s));
	s.dl_interval = (uint16_t)value;
	if ((uint32_t)phase >= profile_sched_interval_counts(&s)) {
		return -EINVAL;
	}

	(void)profile_bits_pack(body, PROFILE_SCHED_AREA_BITS,
				PROFILE_SCHED_PHASE_OFF, PROFILE_SCHED_PHASE_W,
				phase);
	return 0;
}
