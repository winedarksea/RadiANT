/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_handoff.c - page 0x12, the sync handoff.
 *
 * Provenance: docs/radiant-telemetry.md section 12 (the page layout, the
 * two-frame arithmetic, the relative-phase rule, the clock-accuracy ladder and
 * the no-epoch constraint) and section 4 (the two positional invariants that
 * fix bytes [0], [1] and [7]). That document is this project's own written
 * specification. No adopter-gated ANT+ device profile document was read for
 * this file, no sdk-ant source was consulted, and nothing here derives from
 * libant.a. See docs/decisions/0002-clean-room-policy.md.
 *
 * The wire layout, once, so neither this file nor its Python twin has to infer
 * it:
 *
 *   [0] 0x12                        page number
 *   [1] handoff counter             the section 4 counter invariant
 *   [2] (index << 4) | (count - 1)  frame index within the set
 *   [3..6] 32-bit field area, MSB-first via profile_bits
 *   [7] tag space; zero when no transform is in use
 *
 *   frame 0, WHO      bits  0..15  device number
 *                     bits 16..22  device type (7 bits, no pairing bit)
 *                     bits 23..30  transmission type
 *                     bit      31  reserved, must be 0
 *
 *   frame 1, WHEN     bits  0..15  channel period, 1/32768 s counts
 *                     bits 16..28  slot phase, period/8192 units, measured
 *                                  from the t_sync of THIS frame
 *                     bits 29..31  clock accuracy code
 *
 * Every one of those 64 bits is assigned, and that is the enforcement of the
 * no-epoch rule rather than a coincidence: there is no space a four-byte epoch
 * could quietly occupy, so adding one would need a third frame and a visible
 * format change rather than an edit.
 */

#include <errno.h>
#include <string.h>

#include "profile_bits.h"
#include "profile_handoff.h"

/* Field-area bit offsets. Named rather than inlined because the Python mirror
 * names the same numbers and a silent disagreement between the two is the one
 * failure this whole file/twin arrangement exists to catch. */
#define WHO_DEVNUM_OFF   0u
#define WHO_DEVNUM_W    16u
#define WHO_DTYPE_OFF   16u
#define WHO_DTYPE_W      7u
#define WHO_TTYPE_OFF   23u
#define WHO_TTYPE_W      8u
#define WHO_RSVD_OFF    31u
#define WHO_RSVD_W       1u

#define WHEN_PERIOD_OFF  0u
#define WHEN_PERIOD_W   16u
#define WHEN_PHASE_OFF  16u
#define WHEN_PHASE_W    13u
#define WHEN_CLK_OFF    29u
#define WHEN_CLK_W       3u

#define HEAD_LEN 3u  /* bytes [0..2] */
#define AREA_OFF 3u  /* the field area starts at byte [3] */
#define TAG_OFF  7u  /* byte [7], tag space */

/* Every bit of both field areas is accounted for. If a later phase wants a
 * field here it has to take one from this list, and this assert is where it
 * finds that out. */
_Static_assert(WHO_DEVNUM_W + WHO_DTYPE_W + WHO_TTYPE_W + WHO_RSVD_W ==
		       PROFILE_HANDOFF_AREA_BITS,
	       "frame 0's field area is not exactly accounted for");
_Static_assert(WHEN_PERIOD_W + WHEN_PHASE_W + WHEN_CLK_W ==
		       PROFILE_HANDOFF_AREA_BITS,
	       "frame 1's field area is not exactly accounted for");
_Static_assert(PROFILE_HANDOFF_PHASE_MAX < (1u << WHEN_PHASE_W),
	       "the phase field cannot hold the phase range");

static const uint16_t clk_ppm[PROFILE_HANDOFF_CLK_COUNT] = {
	500u, 250u, 150u, 100u, 75u, 50u, 30u, 20u,
};

uint16_t profile_handoff_clk_ppm(uint8_t code)
{
	if (code >= PROFILE_HANDOFF_CLK_COUNT) {
		return 0u;
	}
	return clk_ppm[code];
}

static bool handoff_valid(const struct profile_handoff *h)
{
	if (h == NULL) {
		return false;
	}
	/*
	 * A zero period is "asynchronous" in descriptor frame 0, and an
	 * asynchronous node has no slot to hand over. Refusing here rather than
	 * encoding a phase against a period of zero is the difference between a
	 * receiver that declines a handoff and one that opens a window at an
	 * instant computed by dividing by zero.
	 */
	if (h->period == 0u) {
		return false;
	}
	/* The MSB of the on-air device type field is the pairing bit, which is
	 * a property of a search rather than of the node, so it is not handed
	 * over and 0x80..0xFF is not a device type. */
	if (h->device_type == 0u ||
	    h->device_type > RADIANT_DEVICE_TYPE_MASK) {
		return false;
	}
	if (h->phase > PROFILE_HANDOFF_PHASE_MAX) {
		return false;
	}
	if (h->clock_accuracy >= PROFILE_HANDOFF_CLK_COUNT) {
		return false;
	}
	return true;
}

static void head(uint8_t *frame, uint8_t index, uint8_t counter)
{
	frame[0] = PROFILE_HANDOFF_PAGE;
	frame[1] = counter;
	frame[2] = (uint8_t)((index << 4) | (PROFILE_HANDOFF_FRAMES - 1u));
}

int profile_handoff_encode(const struct profile_handoff *h, uint8_t *out)
{
	uint8_t *who;
	uint8_t *when;

	if (out == NULL || !handoff_valid(h)) {
		return -EINVAL;
	}

	memset(out, 0, PROFILE_HANDOFF_FRAMES * PROFILE_HANDOFF_FRAME_LEN);

	head(&out[0], 0u, h->counter);
	head(&out[PROFILE_HANDOFF_FRAME_LEN], 1u, h->counter);

	who = &out[AREA_OFF];
	when = &out[PROFILE_HANDOFF_FRAME_LEN + AREA_OFF];

	(void)profile_bits_pack(who, PROFILE_HANDOFF_AREA_BITS, WHO_DEVNUM_OFF,
				WHO_DEVNUM_W, h->device_number);
	(void)profile_bits_pack(who, PROFILE_HANDOFF_AREA_BITS, WHO_DTYPE_OFF,
				WHO_DTYPE_W, h->device_type);
	(void)profile_bits_pack(who, PROFILE_HANDOFF_AREA_BITS, WHO_TTYPE_OFF,
				WHO_TTYPE_W, h->trans_type);
	/* WHO_RSVD stays zero; the memset above is what writes it, and the
	 * decoder asserts it rather than ignoring it. */

	(void)profile_bits_pack(when, PROFILE_HANDOFF_AREA_BITS,
				WHEN_PERIOD_OFF, WHEN_PERIOD_W, h->period);
	(void)profile_bits_pack(when, PROFILE_HANDOFF_AREA_BITS,
				WHEN_PHASE_OFF, WHEN_PHASE_W, h->phase);
	(void)profile_bits_pack(when, PROFILE_HANDOFF_AREA_BITS, WHEN_CLK_OFF,
				WHEN_CLK_W, h->clock_accuracy);

	/* Byte [7] of both frames is left zero by the memset. It is tag space,
	 * not padding: section 4 puts a RadiANT page's authentication tag at
	 * the end and nowhere else, so no field may be placed there. */
	return 0;
}

int profile_handoff_frame_index(const uint8_t *frame, uint8_t *index,
				uint8_t *count)
{
	uint8_t i;
	uint8_t n;

	if (frame == NULL || frame[0] != PROFILE_HANDOFF_PAGE) {
		return -EINVAL;
	}

	i = (uint8_t)((frame[2] >> 4) & 0x0Fu);
	n = (uint8_t)((frame[2] & 0x0Fu) + 1u);
	if (n != PROFILE_HANDOFF_FRAMES || i >= n) {
		return -EINVAL;
	}

	if (index != NULL) {
		*index = i;
	}
	if (count != NULL) {
		*count = n;
	}
	return 0;
}

static int decode_pair(const uint8_t *who_frame, const uint8_t *when_frame,
		       struct profile_handoff *out)
{
	uint64_t value = 0u;
	const uint8_t *who = &who_frame[AREA_OFF];
	const uint8_t *when = &when_frame[AREA_OFF];

	if (who_frame[1] != when_frame[1]) {
		/* Two counters are two handoffs. Merging them would describe a
		 * channel neither sender meant, and the result would look
		 * entirely well-formed. */
		return -EBADMSG;
	}

	(void)profile_bits_unpack(who, PROFILE_HANDOFF_AREA_BITS, WHO_RSVD_OFF,
				  WHO_RSVD_W, &value);
	if (value != 0u) {
		return -EPROTO;
	}
	if (who_frame[TAG_OFF] != 0u || when_frame[TAG_OFF] != 0u) {
		/* v1 sets no transform, so tag space carries no tag and must be
		 * zero. Asserted rather than assumed, per section 11. */
		return -EPROTO;
	}

	memset(out, 0, sizeof(*out));
	out->counter = who_frame[1];

	(void)profile_bits_unpack(who, PROFILE_HANDOFF_AREA_BITS,
				  WHO_DEVNUM_OFF, WHO_DEVNUM_W, &value);
	out->device_number = (uint16_t)value;
	(void)profile_bits_unpack(who, PROFILE_HANDOFF_AREA_BITS, WHO_DTYPE_OFF,
				  WHO_DTYPE_W, &value);
	out->device_type = (uint8_t)value;
	(void)profile_bits_unpack(who, PROFILE_HANDOFF_AREA_BITS, WHO_TTYPE_OFF,
				  WHO_TTYPE_W, &value);
	out->trans_type = (uint8_t)value;

	(void)profile_bits_unpack(when, PROFILE_HANDOFF_AREA_BITS,
				  WHEN_PERIOD_OFF, WHEN_PERIOD_W, &value);
	out->period = (uint16_t)value;
	(void)profile_bits_unpack(when, PROFILE_HANDOFF_AREA_BITS,
				  WHEN_PHASE_OFF, WHEN_PHASE_W, &value);
	out->phase = (uint16_t)value;
	(void)profile_bits_unpack(when, PROFILE_HANDOFF_AREA_BITS, WHEN_CLK_OFF,
				  WHEN_CLK_W, &value);
	out->clock_accuracy = (uint8_t)value;

	if (!handoff_valid(out)) {
		return -EINVAL;
	}
	return 0;
}

int profile_handoff_decode(const uint8_t *frames, struct profile_handoff *out)
{
	const uint8_t *by_index[PROFILE_HANDOFF_FRAMES] = { NULL, NULL };
	uint8_t slot;

	if (frames == NULL || out == NULL) {
		return -EINVAL;
	}

	for (slot = 0u; slot < PROFILE_HANDOFF_FRAMES; slot++) {
		const uint8_t *frame = &frames[slot * PROFILE_HANDOFF_FRAME_LEN];
		uint8_t index;
		int err = profile_handoff_frame_index(frame, &index, NULL);

		if (err != 0) {
			return err;
		}
		if (by_index[index] != NULL) {
			return -EBADMSG;
		}
		by_index[index] = frame;
	}

	for (slot = 0u; slot < PROFILE_HANDOFF_FRAMES; slot++) {
		if (by_index[slot] == NULL) {
			return -EBADMSG;
		}
	}

	return decode_pair(by_index[0], by_index[1], out);
}

int profile_handoff_rx_frame(struct profile_handoff_rx *rx,
			     const uint8_t *frame, struct profile_handoff *out)
{
	uint8_t index;
	int err;

	if (rx == NULL || frame == NULL || out == NULL) {
		return -EINVAL;
	}

	err = profile_handoff_frame_index(frame, &index, NULL);
	if (err != 0) {
		return err;
	}

	/*
	 * A new counter abandons a partial set rather than merging with it. The
	 * alternative fails silently: two halves of two handoffs assemble into
	 * a perfectly well-formed page describing a channel that never existed,
	 * and the receiver's only symptom is a search timeout minutes later.
	 */
	if (!rx->started || frame[1] != rx->counter) {
		rx->have = 0u;
		rx->counter = frame[1];
		rx->started = true;
	}

	memcpy(rx->frames[index], frame, PROFILE_HANDOFF_FRAME_LEN);
	rx->have |= (uint8_t)(1u << index);

	if (rx->have != (uint8_t)((1u << PROFILE_HANDOFF_FRAMES) - 1u)) {
		return 0;
	}

	err = decode_pair(rx->frames[0], rx->frames[1], out);
	if (err != 0) {
		rx->have = 0u;
		rx->started = false;
		return err;
	}
	return 1;
}

uint16_t profile_handoff_phase_encode(uint32_t phase_counts, uint16_t period)
{
	uint32_t code;

	if (period == 0u) {
		return 0u;
	}

	/* Reduced rather than refused: it is a phase, and a caller that
	 * measured across a slot boundary is one period out, not wrong. */
	phase_counts %= (uint32_t)period;

	code = (phase_counts * PROFILE_HANDOFF_PHASE_DEN + (period / 2u)) /
	       (uint32_t)period;
	if (code > PROFILE_HANDOFF_PHASE_MAX) {
		code = PROFILE_HANDOFF_PHASE_MAX;
	}
	return (uint16_t)code;
}

uint32_t profile_handoff_phase_counts(const struct profile_handoff *h)
{
	if (h == NULL || h->period == 0u) {
		return 0u;
	}
	return ((uint32_t)h->phase * (uint32_t)h->period +
		PROFILE_HANDOFF_PHASE_DEN / 2u) / PROFILE_HANDOFF_PHASE_DEN;
}

radiant_time_t profile_handoff_next_slot(const struct profile_handoff *h,
					 radiant_time_t t_carrier)
{
	return t_carrier +
	       radiant_channel_counts_to_us(profile_handoff_phase_counts(h));
}

uint32_t profile_handoff_us_to_counts(radiant_time_t us)
{
	/* counts = us * 32768 / 1000000 = us * 512 / 15625, to nearest. The
	 * exact inverse of radiant_channel_counts_to_us(), written the same way
	 * so the pair round-trips within half a count rather than drifting. */
	return (uint32_t)(((us * 512u) + 7812u) / 15625u);
}

int profile_handoff_from_channel(uint8_t channel, radiant_time_t t_carrier,
				 uint8_t counter, uint8_t clock_accuracy,
				 struct profile_handoff *out)
{
	struct radiant_channel_id id;
	radiant_time_t t_next;
	uint16_t period;
	int64_t delta_us;
	int64_t period_us;

	if (out == NULL || clock_accuracy >= PROFILE_HANDOFF_CLK_COUNT) {
		return -EINVAL;
	}
	if (radiant_channel_state_get(channel) != RADIANT_CH_STATE_TRACKING) {
		/* Nothing else in this file can recover from this: a searching
		 * channel has no phase, and a merely configured one knows only
		 * what a host typed into it. */
		return -ENOTCONN;
	}
	if (radiant_channel_id_get(channel, &id) != RADIANT_CH_OK ||
	    radiant_channel_period_get(channel, &period) != RADIANT_CH_OK) {
		return -EINVAL;
	}

	t_next = radiant_channel_next_slot(channel);
	if (t_next == RADIANT_TIME_NEVER || period == 0u) {
		return -ENOTCONN;
	}

	memset(out, 0, sizeof(*out));
	out->device_number = id.device_number;
	out->device_type = (uint8_t)(id.device_type & RADIANT_DEVICE_TYPE_MASK);
	out->trans_type = id.trans_type;
	out->period = period;
	out->clock_accuracy = clock_accuracy;
	out->counter = counter;

	/*
	 * The node's slots are at t_next + k*period for every integer k, so the
	 * phase is a modular quantity and t_carrier may legitimately fall on
	 * either side of the predicted slot - a scheduler, not this function,
	 * decides when the frame goes out. Signed, because the unsigned
	 * difference of two instants in the wrong order is not a small negative
	 * number, it is an enormous positive one.
	 */
	period_us = (int64_t)radiant_channel_counts_to_us(period);
	delta_us = (int64_t)t_next - (int64_t)t_carrier;
	delta_us %= period_us;
	if (delta_us < 0) {
		delta_us += period_us;
	}

	out->phase = profile_handoff_phase_encode(
		profile_handoff_us_to_counts((radiant_time_t)delta_us), period);

	if (!handoff_valid(out)) {
		return -EINVAL;
	}
	return 0;
}

radiant_channel_err_t profile_handoff_apply(uint8_t channel,
					    const struct profile_handoff *h,
					    radiant_time_t t_carrier,
					    radiant_time_t now)
{
	struct radiant_channel_id id;
	radiant_channel_err_t err;
	radiant_time_t t_next;
	radiant_time_t period;

	if (!handoff_valid(h)) {
		return RADIANT_CH_ERR_INVALID_PARAM;
	}
	if (radiant_channel_is_master(channel)) {
		/* A handoff describes a node to listen to. Applying it to a
		 * master would silently reconfigure a transmitter. */
		return RADIANT_CH_ERR_INVALID_PARAM;
	}

	err = radiant_channel_id_set(channel, h->device_number, h->device_type,
				     h->trans_type);
	if (err != RADIANT_CH_OK) {
		return err;
	}
	err = radiant_channel_period_set(channel, h->period);
	if (err != RADIANT_CH_OK) {
		return err;
	}

	/*
	 * Open with no offset, then acquire in the same breath. The open is not
	 * ceremony: it is what takes the channel from ASSIGNED to SEARCHING,
	 * and radiant_channel_on_acquired() accepts nothing else. Going through
	 * that one entry point rather than adding a second path into TRACKING
	 * is the whole reason a handed-off channel is in the same state a swept
	 * one is - including a guard estimator that has been RESET, because
	 * nothing has been measured about this master's clock yet and the first
	 * window after acquisition is the one least able to afford a guess.
	 *
	 * No window is armed in between: radiant_channel.c calls no HAL entry
	 * point, and the scheduler only sees a channel when it ticks.
	 */
	err = radiant_channel_open(channel, 0u, now);
	if (err != RADIANT_CH_OK) {
		return err;
	}

	/*
	 * on_acquired() sets t_next = t_sync + period, because it is told about
	 * a slot that was HEARD. Nothing was heard here, so the synthesised
	 * t_sync is one period before the slot the handoff points at - which is
	 * exactly the slot a sweep would have acquired on had it been listening
	 * one period earlier. The arithmetic is the same on both sides because
	 * the period was set above, so t_next lands on the handoff's instant
	 * exactly rather than within a rounding of it.
	 */
	t_next = profile_handoff_next_slot(h, t_carrier);
	period = radiant_channel_counts_to_us(h->period);

	id.device_number = h->device_number;
	id.device_type = h->device_type;
	id.trans_type = h->trans_type;
	radiant_channel_on_acquired(channel, &id, t_next - period);

	return RADIANT_CH_OK;
}
