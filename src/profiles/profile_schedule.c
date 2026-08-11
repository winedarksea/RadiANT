/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_schedule.c - the descriptor schedule block.
 *
 * Provenance: docs/radiant-telemetry.md section 6 and section 12, this
 * project's own written specification, authored in advance of any code. No
 * adopter-gated ANT+ device profile document was read for this file, no
 * sdk-ant source was consulted, and nothing here derives from libant.a. See
 * docs/decisions/0002-clean-room-policy.md. The argument for every field is in
 * profile_schedule.h.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "profile_bits.h"
#include "profile_schedule.h"

/* The whole body is assigned, and this is the arithmetic that says so. Adding a
 * field means taking bits from the reservation in this sum, in this file,
 * rather than discovering an overlap on air. */
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
 * floating point and without the rounding error that a us-per-count constant
 * would accumulate over a 128 s interval. */
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
	/* S=8 is not built yet either - the long-range phase builds it - but it
	 * is the rate this project has committed to, and a build that gains the
	 * PHY gains it here by gaining the PHY. S=2 and the reserved codes are
	 * refused because nothing will ever transmit them from this tree
	 * without a further decision. */
	return coding == PROFILE_SCHED_CODING_NONE ||
	       coding == PROFILE_SCHED_CODING_S8;
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
		/* Nothing stated. NOT "500 ppm" and not "20 ppm": every node
		 * built before this block existed transmits these bits as
		 * zeros, so reading a claim out of them would put a claim on
		 * every node in the field retroactively. */
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

	/* Frame 0's long-range bit and this field are two statements about one
	 * PHY, so they agree or the block is refused. */
	if (lr_phy != (s->coding != PROFILE_SCHED_CODING_NONE)) {
		return -EINVAL;
	}
	if (!profile_sched_coding_implemented(s->coding)) {
		return -ENOTSUP;
	}

	if (s->dl_interval == 0u) {
		/* No window: section 11's rule, applied to two fields that
		 * would otherwise describe a window nobody opens. */
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

	/*
	 * The dwell must cover the clock error accumulated between the frame
	 * that announces the window and the window itself, in BOTH directions -
	 * the commanding receiver aims at the middle and can be early or late -
	 * so twice the drift, not once.
	 *
	 * This is the field that makes the block internally honest. A 500 ppm
	 * node pointing two seconds ahead has drifted a millisecond by the time
	 * it opens, so the plan's example duty of 500 us every 2 s is a window
	 * only a crystal node can offer. Announcing it anyway would produce a
	 * downlink that never works and reports nothing.
	 */
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
		/* Reserved-must-be-zero, and unlike an unknown informational
		 * flag these bits have no defined meaning at all - a receiver
		 * that ignored them would be inventing one. */
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

		/* Range-checked BEFORE the narrowing cast, not after. Biased
		 * byte 228 minus 100 is 128, which lands on the sentinel when
		 * squeezed into an int8 - so a receiver that cast first would
		 * read a nonsense announcement as no announcement at all. */
		if (dbm < PROFILE_SCHED_TX_POWER_MIN ||
		    dbm > PROFILE_SCHED_TX_POWER_MAX) {
			return -EINVAL;
		}
		s.tx_power_dbm = (int8_t)dbm;
	}

	/*
	 * A coding rate this build cannot use is NOT a decode error, and that
	 * asymmetry with the encoder is the forward-compatibility rule of
	 * section 6 applied to a vocabulary field: the node is describing
	 * itself, not the layout of these bytes, so a receiver decodes it and
	 * then declines the channel. Everything else is checked.
	 */
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
