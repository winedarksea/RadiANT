/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_telemetry.c - device type 0x60, the RadiANT telemetry envelope.
 *
 * Provenance: docs/radiant-telemetry.md sections 3-9 and 11, this project's
 * own written specification of the envelope, authored before any code
 * existed. Frame layouts, flag assignments, the width-code table, the
 * class-0x30 accumulate rule and the "every reservation is populated with
 * zeros" rule all come from there. No ANT+ device profile
 * document was read for this file, no sdk-ant source was consulted, and
 * nothing here derives from libant.a. See docs/decisions/0002-clean-room-policy.md.
 *
 * The validation is the interesting part: an encoder that emits whatever it
 * was handed is half a specification. Each check below exists because the
 * failure it prevents is silent on air:
 *   - a class-0x30 type without the accumulate bit decodes as a plausible
 *     instantaneous reading that resets to zero every wrap;
 *   - two fields overlapping in one page produces two wrong numbers with a
 *     valid CRC;
 *   - a sparse node with heartbeat_s == 0 can't be told apart from a dead one;
 *   - a non-sparse node with period == 0 asks a tracking receiver to predict
 *     a window from a period that doesn't exist;
 *   - the off-57 bit disagreeing with byte [6] leaves the receiver to choose
 *     which of the node's two statements about its own frequency to believe.
 *
 * All five are refused at the encoder and rejected at the decoder: the
 * encoder protects the air from this node, the decoder protects this
 * receiver from somebody else's.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* For RADIANT_FRAME_HDR_LEN_LR, the long-range header the duty bound has to
 * account for. The collapse packs payload bytes; the bound is on the whole
 * body. */
#include <radiant_core/radiant_frame.h>

#include "profile_bits.h"
#include "profile_telemetry.h"

/* ---------------------------------------------------------------------------
 * Small shared facts
 * ---------------------------------------------------------------------------
 */

bool profile_tlm_type_is_accumulating(uint8_t type)
{
	return type >= PROFILE_TLM_CLASS_ACC_FIRST &&
	       type <= PROFILE_TLM_CLASS_ACC_LAST;
}

uint8_t profile_tlm_repeat_for(uint8_t k_code)
{
	switch (k_code) {
	case PROFILE_TLM_K_CODE_1:
		return 1u;
	case PROFILE_TLM_K_CODE_2:
		return 2u;
	case PROFILE_TLM_K_CODE_3:
		return 3u;
	case PROFILE_TLM_K_CODE_5:
		return 5u;
	default:
		return 0u;
	}
}

uint16_t profile_desc_field_area_bits(const struct profile_descriptor *d)
{
	if (d != NULL && (d->flags & PROFILE_TLM_FLAG_X_AUTH) != 0u) {
		/* Byte [7] is the spread-MAC tag byte; the field area is bits
		 * 0..39. */
		return 40u;
	}
	return 48u;
}

uint16_t profile_desc_clock_ppm(const struct profile_descriptor *d)
{
	if (d == NULL || !d->clock_stated) {
		return 0u;
	}
	/* One ladder: the sync-handoff page's. */
	return profile_handoff_clk_ppm(d->clock_accuracy);
}

bool profile_desc_may_decode_data(const struct profile_descriptor *d)
{
	if (d == NULL) {
		return false;
	}
	/* v1 implements no transform, so any transform bit is unimplemented.
	 * Fail closed. */
	return (d->flags & PROFILE_TLM_FLAG_TRANSFORM_MASK) == 0u;
}

uint8_t profile_desc_frame_count(const struct profile_descriptor *d)
{
	if (d == NULL) {
		return 0u;
	}
	/* Two header frames, the schedule frame if the node sends one, then
	 * one per field. The authentication frame is mandatory when a
	 * transform bit is set; v1 sets none, so it never appears. */
	return (uint8_t)(2u + (d->has_schedule ? 1u : 0u) + d->n_fields);
}

int profile_desc_schedule_index(const struct profile_descriptor *d)
{
	if (d == NULL || !d->has_schedule) {
		return -ENOENT;
	}
	return 2;
}

int profile_desc_data_pages(const struct profile_descriptor *d, uint8_t *pages,
			    uint8_t cap)
{
	uint8_t n = 0u;
	uint8_t page;

	if (d == NULL || pages == NULL) {
		return -EINVAL;
	}

	/* Ascending, by scanning the small legal range rather than sorting -
	 * deterministic, so a receiver's rotation prediction and a test's
	 * expectation are the same list. */
	for (page = PROFILE_TLM_PAGE_DATA_FIRST;
	     page <= PROFILE_TLM_PAGE_DATA_LAST; page++) {
		uint8_t i;
		bool used = false;

		for (i = 0u; i < d->n_fields; i++) {
			if (d->fields[i].page == page) {
				used = true;
				break;
			}
		}
		if (!used) {
			continue;
		}
		if (n >= cap) {
			return -ENOSPC;
		}
		pages[n++] = page;
	}

	return (int)n;
}

/* ---------------------------------------------------------------------------
 * Validation
 * ---------------------------------------------------------------------------
 */

static int check_field(const struct profile_descriptor *d, uint8_t index)
{
	const struct profile_field *f = &d->fields[index];
	uint16_t area = profile_desc_field_area_bits(d);
	int width;
	uint8_t j;

	if (f->page < PROFILE_TLM_PAGE_DATA_FIRST ||
	    f->page > PROFILE_TLM_PAGE_DATA_LAST) {
		return -EINVAL;
	}

	width = profile_bits_width(f->width_code);
	if (width < 0) {
		return -EINVAL;
	}
	if ((uint32_t)f->bit_offset + (uint32_t)width > (uint32_t)area) {
		return -ERANGE;
	}

	/* Section 7: everything in class 0x30-0x3F must carry the accumulate
	 * bit. */
	if (profile_tlm_type_is_accumulating(f->type) && !f->accumulate) {
		return -EINVAL;
	}

	/* No two fields may overlap or share an id - an id is the envelope's
	 * topic name, and a duplicate makes the schema ambiguous. */
	for (j = 0u; j < index; j++) {
		const struct profile_field *g = &d->fields[j];
		int gwidth = profile_bits_width(g->width_code);

		if (g->id == f->id) {
			return -EINVAL;
		}
		if (g->page != f->page || gwidth < 0) {
			continue;
		}
		if (f->bit_offset < (uint16_t)(g->bit_offset + gwidth) &&
		    g->bit_offset < (uint16_t)(f->bit_offset + width)) {
			return -EINVAL;
		}
	}

	return 0;
}

static int check_descriptor(const struct profile_descriptor *d)
{
	bool sparse;
	uint8_t i;
	int rc;

	if (d == NULL) {
		return -EINVAL;
	}
	if (d->version != PROFILE_TLM_VERSION) {
		return -ENOTSUP;
	}
	if (d->n_fields > PROFILE_TLM_MAX_FIELDS) {
		/* 15 is reserved for an extended descriptor - a count that
		 * would encode as 15 names a layout this version doesn't have. */
		return -EINVAL;
	}
	if (d->rf_index > PROFILE_TLM_RF_INDEX_MAX) {
		return -EINVAL;
	}
	if ((d->flags & PROFILE_TLM_FLAG_RSVD_3) != 0u) {
		/* Reserved-must-be-zero; was X_PRIV, see the header. */
		return -EINVAL;
	}

	/* The node's two statements about its own frequency must agree. */
	if (((d->flags & PROFILE_TLM_FLAG_OFF_RF57) != 0u) !=
	    (d->rf_index != PROFILE_TLM_RF_INDEX_DEFAULT)) {
		return -EINVAL;
	}

	sparse = (d->flags & PROFILE_TLM_FLAG_SPARSE) != 0u;
	if (sparse) {
		/* Zero is invalid for a sparse node: without a heartbeat a
		 * receiver cannot tell a quiet node from a dead one. */
		if (d->heartbeat_s == 0u) {
			return -EINVAL;
		}
		if (profile_tlm_repeat_for(d->k_code) == 0u) {
			return -EINVAL;
		}
	} else {
		if (d->heartbeat_s != 0u) {
			return -EINVAL;
		}
		if (d->k_code != 0u) {
			return -EINVAL;
		}
		/* Asynchronous (period 0x0000) has no slot discipline and
		 * requires a continuous-scan receiver; a periodic node asking
		 * for it is asking to predict a window that doesn't exist. */
		if (d->period == 0u) {
			return -EINVAL;
		}
	}

	/* Section 11: every reservation is zero in v1. With no transform
	 * enabled the MAC window is one of them. */
	if ((d->flags & PROFILE_TLM_FLAG_X_AUTH) == 0u) {
		if (d->w_code != PROFILE_TLM_W_CODE_RESERVED) {
			return -EINVAL;
		}
	} else if (d->w_code == PROFILE_TLM_W_CODE_RESERVED ||
		   d->w_code > PROFILE_TLM_W_CODE_8) {
		/* W=1 is the reliable-command page's inline tag, refused on a
		 * data page. */
		return -EINVAL;
	}

	/* The schedule block: checked only when announced. Every caller here
	 * zeroes a descriptor before filling it, and a zeroed struct is a
	 * legal-looking announcement of 0 dBm, not silence - refusing on
	 * bytes nobody meant to send would break every existing caller. */
	if (d->clock_stated) {
		if (d->clock_accuracy >= PROFILE_HANDOFF_CLK_COUNT) {
			return -EINVAL;
		}
	} else if (d->clock_accuracy != 0u) {
		/* A ladder code with nothing stating it goes on the air as
		 * three zero bits: caller and wire would disagree about what
		 * this node announced. */
		return -EINVAL;
	}

	if (d->has_schedule) {
		rc = profile_sched_check(&d->schedule,
					 (d->flags & PROFILE_TLM_FLAG_LR_PHY) != 0u,
					 profile_desc_clock_ppm(d));
		if (rc != 0) {
			return rc;
		}
	}

	for (i = 0u; i < d->n_fields; i++) {
		rc = check_field(d, i);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

/* ---------------------------------------------------------------------------
 * Descriptor encode
 * ---------------------------------------------------------------------------
 */

int profile_desc_encode(const struct profile_descriptor *d, uint8_t *frames,
			uint8_t cap_frames)
{
	uint8_t count;
	uint8_t base;
	uint8_t i;
	uint8_t *f;
	int rc;

	if (d == NULL || frames == NULL) {
		return -EINVAL;
	}

	/*
	 * The set is refused rather than emitted when a transform is
	 * announced, and checked BEFORE the rest of validation: X_AUTH
	 * shrinks the field area from 48 bits to 40, so a well-formed schema
	 * would otherwise report a misleading -ERANGE unrelated to where the
	 * fields actually sit. Section 6 makes the authentication frame
	 * mandatory whenever a transform bit is set - without it, an attacker
	 * who forges one descriptor frame gets wrong readings out of
	 * correctly authenticated packets. This build cannot produce that
	 * frame, so it does not get to announce a transform.
	 */
	if ((d->flags & PROFILE_TLM_FLAG_TRANSFORM_MASK) != 0u) {
		return -ENOTSUP;
	}

	rc = check_descriptor(d);
	if (rc != 0) {
		return rc;
	}

	count = profile_desc_frame_count(d);
	base = (uint8_t)(count - d->n_fields); /* 2, or 3 with a schedule frame */
	if (count > PROFILE_TLM_MAX_FRAMES) {
		/* A schedule frame costs a field: 14 fields plus one is 17
		 * frames, and the index is a nibble. */
		return -EINVAL;
	}
	if (cap_frames < count) {
		return -ENOSPC;
	}

	memset(frames, 0, (size_t)count * PROFILE_TLM_FRAME_LEN);

	for (i = 0u; i < count; i++) {
		f = &frames[(size_t)i * PROFILE_TLM_FRAME_LEN];
		f[0] = PROFILE_TLM_PAGE_DESCRIPTOR;
		/* Byte [1] is a FRAME INDEX, not a counter - one of the two
		 * documented exceptions to section 4's counter invariant. */
		f[1] = (uint8_t)(((uint32_t)i << 4) | (uint32_t)(count - 1u));
	}

	/* Frame 0 - identity and timing. */
	f = &frames[0];
	f[2] = (uint8_t)(((uint32_t)d->version << 4) | (uint32_t)d->n_fields);
	f[3] = d->schema_id;
	f[4] = (uint8_t)(d->period & 0xFFu);
	f[5] = (uint8_t)((d->period >> 8) & 0xFFu);
	f[6] = d->rf_index;
	f[7] = d->flags;

	/* Frame 1 - mode and the reserved security space. Four bytes of epoch
	 * so a receiver with no real-time clock can get it from a page the
	 * node already sends; adding it later would be a format break. */
	f = &frames[PROFILE_TLM_FRAME_LEN];
	f[2] = d->heartbeat_s;
	/* Bits 3..0 are the informational-flag extension space, filled by the
	 * schedule block's clock-accuracy nibble. A node stating nothing
	 * writes zeros, same as every node built before the block existed. */
	f[3] = (uint8_t)(((uint32_t)d->w_code << 6) |
			 ((uint32_t)d->k_code << 4) |
			 profile_sched_clk_nibble(d->clock_accuracy,
						  d->clock_stated));
	f[4] = (uint8_t)(d->epoch & 0xFFu);
	f[5] = (uint8_t)((d->epoch >> 8) & 0xFFu);
	f[6] = (uint8_t)((d->epoch >> 16) & 0xFFu);
	f[7] = (uint8_t)((d->epoch >> 24) & 0xFFu);

	/* Frame 2 - the schedule frame, when there is one. */
	if (d->has_schedule) {
		f = &frames[2u * PROFILE_TLM_FRAME_LEN];
		rc = profile_sched_pack(&d->schedule,
					(d->flags & PROFILE_TLM_FLAG_LR_PHY) != 0u,
					profile_desc_clock_ppm(d), &f[2]);
		if (rc != 0) {
			/* Unreachable given the check above, but checked
			 * anyway rather than emitting whatever the packer
			 * left behind. */
			return rc;
		}
	}

	/* The field frames, after the schedule frame if there is one. */
	for (i = 0u; i < d->n_fields; i++) {
		const struct profile_field *fd = &d->fields[i];

		f = &frames[(size_t)(base + i) * PROFILE_TLM_FRAME_LEN];
		f[2] = fd->id;
		f[3] = fd->type;
		f[4] = (uint8_t)((fd->accumulate ? 0x80u : 0u) |
				 (fd->is_signed ? 0x40u : 0u) |
				 ((uint32_t)fd->width_code << 2));
		/* bits 1..0 reserved, must be 0 - and are, since the
		 * expression above never sets them. */
		f[5] = (uint8_t)fd->exponent;
		f[6] = fd->page;
		f[7] = fd->bit_offset;
	}

	return (int)count;
}

/* ---------------------------------------------------------------------------
 * Descriptor decode
 * ---------------------------------------------------------------------------
 */

int profile_desc_decode(const uint8_t *frames, uint8_t n_frames,
			struct profile_descriptor *out)
{
	const struct profile_schedule quiet = PROFILE_SCHED_INIT_DEFAULT;
	struct profile_descriptor d;
	const uint8_t *f;
	uint8_t base;
	uint8_t i;

	if (frames == NULL || out == NULL) {
		return -EINVAL;
	}
	if (n_frames < 2u || n_frames > PROFILE_TLM_MAX_FRAMES) {
		return -EPROTO;
	}

	memset(&d, 0, sizeof(d));
	/* Not the memset's zeros: a zeroed block announces 0 dBm, and a node
	 * that sent no block announced nothing. */
	d.schedule = quiet;

	/* Every frame must claim page 0x00 and agree on index and count - a
	 * set assembled from two different broadcasts is the one way a
	 * receiver produces a schema nobody transmitted. */
	for (i = 0u; i < n_frames; i++) {
		f = &frames[(size_t)i * PROFILE_TLM_FRAME_LEN];
		if (f[0] != PROFILE_TLM_PAGE_DESCRIPTOR) {
			return -EPROTO;
		}
		if ((f[1] >> 4) != i) {
			return -EPROTO;
		}
		if ((uint8_t)((f[1] & 0x0Fu) + 1u) != n_frames) {
			return -EPROTO;
		}
	}

	f = &frames[0];
	d.version = (uint8_t)(f[2] >> 4);
	d.n_fields = (uint8_t)(f[2] & 0x0Fu);
	d.schema_id = f[3];
	d.period = (uint16_t)((uint16_t)f[4] | ((uint16_t)f[5] << 8));
	d.rf_index = f[6];
	d.flags = f[7];

	/* A receiver that does not implement the version MUST REJECT THE NODE
	 * rather than guess: the version governs the frame layout, including
	 * every byte read above. */
	if (d.version != PROFILE_TLM_VERSION) {
		return -ENOTSUP;
	}
	if (d.n_fields == 0x0Fu) {
		/* Reserved for an extended descriptor this version has no
		 * layout for - not "no fields", a different frame set. */
		return -ENOTSUP;
	}

	/* The shape of the set, derived rather than announced: two header
	 * frames plus one per field, with one extra (the schedule frame) at
	 * index 2. No presence bit was spent on this, since the count byte
	 * and field count already say it. */
	base = (uint8_t)(2u + d.n_fields);
	if (n_frames == base) {
		d.has_schedule = false;
		base = 2u;
	} else if (n_frames == (uint8_t)(base + 1u)) {
		d.has_schedule = true;
		base = 3u;
	} else {
		return -EPROTO;
	}

	f = &frames[PROFILE_TLM_FRAME_LEN];
	d.heartbeat_s = f[2];
	d.w_code = (uint8_t)((f[3] >> 6) & 0x03u);
	d.k_code = (uint8_t)((f[3] >> 4) & 0x03u);
	/* Bits 3..0 are the informational-flag extension space of section 6,
	 * filled by the schedule block's clock-accuracy nibble. Informational:
	 * they describe the node, not the byte layout, so the fail-open half
	 * of the forward-compatibility rule applies and a receiver that
	 * doesn't care about the clock ignores them. */
	d.clock_stated = (f[3] & PROFILE_SCHED_CLK_STATED) != 0u;
	d.clock_accuracy = d.clock_stated
				   ? (uint8_t)(f[3] & PROFILE_SCHED_CLK_MASK)
				   : 0u;
	d.epoch = (uint32_t)f[4] | ((uint32_t)f[5] << 8) |
		  ((uint32_t)f[6] << 16) | ((uint32_t)f[7] << 24);

	if (d.has_schedule) {
		f = &frames[2u * PROFILE_TLM_FRAME_LEN];
		if (profile_sched_unpack(&f[2],
					 (d.flags & PROFILE_TLM_FLAG_LR_PHY) != 0u,
					 profile_desc_clock_ppm(&d),
					 &d.schedule) != 0) {
			return -EPROTO;
		}
	}

	for (i = 0u; i < d.n_fields; i++) {
		struct profile_field *fd = &d.fields[i];

		f = &frames[(size_t)(base + i) * PROFILE_TLM_FRAME_LEN];
		fd->id = f[2];
		fd->type = f[3];
		fd->accumulate = (f[4] & 0x80u) != 0u;
		fd->is_signed = (f[4] & 0x40u) != 0u;
		fd->width_code = (uint8_t)((f[4] >> 2) & 0x0Fu);
		if ((f[4] & 0x03u) != 0u) {
			return -EPROTO; /* bits 1..0 reserved, must be 0 */
		}
		fd->exponent = (int8_t)f[5];
		fd->page = f[6];
		fd->bit_offset = f[7];
	}

	if (check_descriptor(&d) != 0) {
		return -EPROTO;
	}

	*out = d;
	return 0;
}

/* ---------------------------------------------------------------------------
 * Receiver-side assembly
 * ---------------------------------------------------------------------------
 */

void profile_desc_rx_init(struct profile_desc_rx *rx)
{
	if (rx != NULL) {
		memset(rx, 0, sizeof(*rx));
	}
}

static void rx_restart(struct profile_desc_rx *rx, uint8_t count)
{
	rx->have = 0u;
	rx->count = count;
	rx->schema_id_seen = false;
	rx->resets++;
}

int profile_desc_rx_feed(struct profile_desc_rx *rx, const uint8_t *body)
{
	uint8_t index;
	uint8_t count;

	if (rx == NULL || body == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_TLM_PAGE_DESCRIPTOR) {
		return 0; /* not ours; a receiver sees data and common pages too */
	}

	index = (uint8_t)(body[1] >> 4);
	count = (uint8_t)((body[1] & 0x0Fu) + 1u);

	if (count < 2u || index >= count) {
		/* A frame that cannot belong to any set: fewer than the two
		 * mandatory header frames, or an index past the end. */
		return -EPROTO;
	}

	rx->frames_fed++;

	if (rx->count != 0u && rx->count != count) {
		/* The node changed the size of its set; everything held is
		 * from the old one. */
		rx_restart(rx, count);
	}
	rx->count = count;

	if (index == 0u) {
		uint8_t schema_id = body[3];

		if (rx->schema_id_seen && schema_id != rx->schema_id) {
			/* This is what the schema id is for: notice a change
			 * after one frame instead of the whole set. Anything
			 * already assembled describes the previous schema. */
			rx_restart(rx, count);
		}
		rx->schema_id = schema_id;
		rx->schema_id_seen = true;
	}

	memcpy(rx->frames[index], body, PROFILE_TLM_FRAME_LEN);
	rx->have |= (uint16_t)((uint16_t)1u << index);

	return profile_desc_rx_complete(rx) ? 1 : 0;
}

bool profile_desc_rx_complete(const struct profile_desc_rx *rx)
{
	uint16_t want;

	if (rx == NULL || rx->count == 0u) {
		return false;
	}
	want = (uint16_t)(((uint32_t)1u << rx->count) - 1u);
	return (rx->have & want) == want;
}

int profile_desc_rx_take(const struct profile_desc_rx *rx,
			 struct profile_descriptor *out)
{
	uint8_t flat[PROFILE_TLM_MAX_FRAMES * PROFILE_TLM_FRAME_LEN];
	uint8_t i;

	if (rx == NULL || out == NULL) {
		return -EINVAL;
	}
	if (!profile_desc_rx_complete(rx)) {
		return -EAGAIN;
	}

	for (i = 0u; i < rx->count; i++) {
		memcpy(&flat[(size_t)i * PROFILE_TLM_FRAME_LEN], rx->frames[i],
		       PROFILE_TLM_FRAME_LEN);
	}

	return profile_desc_decode(flat, rx->count, out);
}

/* ---------------------------------------------------------------------------
 * Data pages
 * ---------------------------------------------------------------------------
 */

int profile_data_encode(const struct profile_descriptor *d, uint8_t page,
			uint8_t counter, const int64_t *values, uint8_t n_values,
			uint8_t *body)
{
	uint16_t area = profile_desc_field_area_bits(d);
	uint8_t packed = 0u;
	uint8_t i;

	if (d == NULL || body == NULL) {
		return -EINVAL;
	}
	if (n_values != d->n_fields) {
		return -EINVAL;
	}
	if (n_values != 0u && values == NULL) {
		return -EINVAL;
	}
	if (page < PROFILE_TLM_PAGE_DATA_FIRST ||
	    page > PROFILE_TLM_PAGE_DATA_LAST) {
		return -EINVAL;
	}

	/* Zeroed first, so a byte the schema doesn't claim is zero rather than
	 * whatever the last page left there. */
	memset(body, 0, PROFILE_TLM_FRAME_LEN);
	body[0] = page;
	body[1] = counter;

	for (i = 0u; i < d->n_fields; i++) {
		const struct profile_field *f = &d->fields[i];
		int width;
		int rc;

		if (f->page != page) {
			continue;
		}
		width = profile_bits_width(f->width_code);
		if (width < 0) {
			return -EINVAL;
		}
		rc = profile_bits_pack(&body[2], area, f->bit_offset,
				       (uint8_t)width, (uint64_t)values[i]);
		if (rc != 0) {
			return rc;
		}
		packed++;
	}

	return (int)packed;
}

int profile_data_decode(const struct profile_descriptor *d, const uint8_t *body,
			int64_t *values, uint8_t n_values, uint16_t *present)
{
	uint16_t area;
	uint8_t page;
	uint8_t decoded = 0u;
	uint8_t i;

	if (d == NULL || body == NULL) {
		return -EINVAL;
	}
	if (n_values != d->n_fields) {
		return -EINVAL;
	}
	if (n_values != 0u && values == NULL) {
		return -EINVAL;
	}
	if (!profile_desc_may_decode_data(d)) {
		/* A transform bit this build doesn't implement. Loudly, not
		 * silently - zeros here would look like plaintext off a
		 * ciphertext page. */
		return -EACCES;
	}

	page = body[0];
	if (page < PROFILE_TLM_PAGE_DATA_FIRST ||
	    page > PROFILE_TLM_PAGE_DATA_LAST) {
		return -EINVAL;
	}

	area = profile_desc_field_area_bits(d);
	if (present != NULL) {
		*present = 0u;
	}

	for (i = 0u; i < d->n_fields; i++) {
		const struct profile_field *f = &d->fields[i];
		uint64_t raw;
		int width;
		int rc;

		if (f->page != page) {
			continue;
		}
		width = profile_bits_width(f->width_code);
		if (width < 0) {
			return -EINVAL;
		}
		rc = profile_bits_unpack(&body[2], area, f->bit_offset,
					 (uint8_t)width, &raw);
		if (rc != 0) {
			return rc;
		}
		values[i] = f->is_signed
				    ? profile_bits_sign_extend(raw, (uint8_t)width)
				    : (int64_t)raw;
		if (present != NULL) {
			*present |= (uint16_t)((uint16_t)1u << i);
		}
		decoded++;
	}

	return (int)decoded;
}

/* ---------------------------------------------------------------------------
 * The reliable-command pages - layout only, see the header
 * ---------------------------------------------------------------------------
 */

int profile_command_encode(const struct profile_command *c, uint8_t *body)
{
	if (c == NULL || body == NULL) {
		return -EINVAL;
	}
	body[0] = PROFILE_TLM_PAGE_COMMAND;
	body[1] = c->seq; /* the counter invariant: byte [1] is a counter */
	body[2] = c->cmd;
	body[3] = c->target;
	body[4] = (uint8_t)(c->arg & 0xFFu);
	body[5] = (uint8_t)((c->arg >> 8) & 0xFFu);
	/* Trailing bytes are tag space; no RadiANT page places a tag anywhere
	 * but the end. */
	body[6] = (uint8_t)(c->tag & 0xFFu);
	body[7] = (uint8_t)((c->tag >> 8) & 0xFFu);
	return 0;
}

int profile_command_decode(const uint8_t *body, struct profile_command *out)
{
	if (body == NULL || out == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_TLM_PAGE_COMMAND) {
		return -EPROTO;
	}
	out->seq = body[1];
	out->cmd = body[2];
	out->target = body[3];
	out->arg = (uint16_t)((uint16_t)body[4] | ((uint16_t)body[5] << 8));
	out->tag = (uint16_t)((uint16_t)body[6] | ((uint16_t)body[7] << 8));
	return 0;
}

int profile_command_ack_encode(const struct profile_command_ack *a, uint8_t *body)
{
	if (a == NULL || body == NULL) {
		return -EINVAL;
	}
	body[0] = PROFILE_TLM_PAGE_COMMAND_ACK;
	body[1] = a->seq;
	body[2] = a->result;
	body[3] = a->cmd;
	body[4] = (uint8_t)(a->value & 0xFFu);
	body[5] = (uint8_t)((a->value >> 8) & 0xFFu);
	body[6] = (uint8_t)(a->tag & 0xFFu);
	body[7] = (uint8_t)((a->tag >> 8) & 0xFFu);
	return 0;
}

int profile_command_ack_decode(const uint8_t *body, struct profile_command_ack *out)
{
	if (body == NULL || out == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_TLM_PAGE_COMMAND_ACK) {
		return -EPROTO;
	}
	out->seq = body[1];
	out->result = body[2];
	out->cmd = body[3];
	out->value = (uint16_t)((uint16_t)body[4] | ((uint16_t)body[5] << 8));
	out->tag = (uint16_t)((uint16_t)body[6] | ((uint16_t)body[7] << 8));
	return 0;
}

/* ---------------------------------------------------------------------------
 * The same two pages at either tag width: the six covered bytes are written
 * by the pair above, then the tag is appended, so there's one statement of
 * where the sequence number and argument live rather than a second copy of
 * the layout for the long form.
 * ---------------------------------------------------------------------------
 */

static bool cmd_tag_len_ok(uint8_t tag_len)
{
	return tag_len == PROFILE_TLM_CMD_TAG_STD ||
	       tag_len == PROFILE_TLM_CMD_TAG_LR;
}

int profile_command_encode_tag(const struct profile_command *c,
			       const uint8_t *tag, uint8_t tag_len,
			       uint8_t *body, size_t cap)
{
	uint8_t std[PROFILE_TLM_CMD_LEN_STD];
	int rc;

	if (tag == NULL || body == NULL || !cmd_tag_len_ok(tag_len)) {
		return -EINVAL;
	}
	if (cap < (size_t)PROFILE_TLM_CMD_COVERED + tag_len) {
		return -EINVAL;
	}

	rc = profile_command_encode(c, std);
	if (rc != 0) {
		return rc;
	}
	memcpy(body, std, PROFILE_TLM_CMD_COVERED);
	memcpy(&body[PROFILE_TLM_CMD_COVERED], tag, tag_len);
	return (int)(PROFILE_TLM_CMD_COVERED + tag_len);
}

int profile_command_decode_tag(const uint8_t *body, uint8_t len,
			       struct profile_command *out, uint8_t *tag,
			       uint8_t tag_cap)
{
	uint8_t tag_len;

	if (body == NULL || out == NULL || tag == NULL) {
		return -EINVAL;
	}
	if (len < PROFILE_TLM_CMD_COVERED) {
		return -EINVAL;
	}
	tag_len = (uint8_t)(len - PROFILE_TLM_CMD_COVERED);
	if (!cmd_tag_len_ok(tag_len) || tag_cap < tag_len) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_TLM_PAGE_COMMAND) {
		return -EPROTO;
	}

	out->seq = body[1];
	out->cmd = body[2];
	out->target = body[3];
	out->arg = (uint16_t)((uint16_t)body[4] | ((uint16_t)body[5] << 8));
	memcpy(tag, &body[PROFILE_TLM_CMD_COVERED], tag_len);
	/* The low 16 bits, so a standard-width caller reads the struct and a
	 * long-width caller reads the bytes. */
	out->tag = (uint16_t)((uint16_t)tag[0] | ((uint16_t)tag[1] << 8));
	return (int)tag_len;
}

int profile_command_ack_encode_tag(const struct profile_command_ack *a,
				   const uint8_t *tag, uint8_t tag_len,
				   uint8_t *body, size_t cap)
{
	uint8_t std[PROFILE_TLM_CMD_LEN_STD];
	int rc;

	if (tag == NULL || body == NULL || !cmd_tag_len_ok(tag_len)) {
		return -EINVAL;
	}
	if (cap < (size_t)PROFILE_TLM_CMD_COVERED + tag_len) {
		return -EINVAL;
	}

	rc = profile_command_ack_encode(a, std);
	if (rc != 0) {
		return rc;
	}
	memcpy(body, std, PROFILE_TLM_CMD_COVERED);
	memcpy(&body[PROFILE_TLM_CMD_COVERED], tag, tag_len);
	return (int)(PROFILE_TLM_CMD_COVERED + tag_len);
}

int profile_command_ack_decode_tag(const uint8_t *body, uint8_t len,
				   struct profile_command_ack *out,
				   uint8_t *tag, uint8_t tag_cap)
{
	uint8_t tag_len;

	if (body == NULL || out == NULL || tag == NULL) {
		return -EINVAL;
	}
	if (len < PROFILE_TLM_CMD_COVERED) {
		return -EINVAL;
	}
	tag_len = (uint8_t)(len - PROFILE_TLM_CMD_COVERED);
	if (!cmd_tag_len_ok(tag_len) || tag_cap < tag_len) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_TLM_PAGE_COMMAND_ACK) {
		return -EPROTO;
	}

	out->seq = body[1];
	out->result = body[2];
	out->cmd = body[3];
	out->value = (uint16_t)((uint16_t)body[4] | ((uint16_t)body[5] << 8));
	memcpy(tag, &body[PROFILE_TLM_CMD_COVERED], tag_len);
	out->tag = (uint16_t)((uint16_t)tag[0] | ((uint16_t)tag[1] << 8));
	return (int)tag_len;
}

/* ---------------------------------------------------------------------------
 * The descriptor-set collapse - ADR 0007
 *
 * Everything here is concatenation and division: the frames packed are
 * byte-for-byte the frames a 1 M node emits one at a time, so there's no
 * second encoding or wire format to get wrong. See the header for what the
 * collapse buys and how complete it is.
 * ---------------------------------------------------------------------------
 */

uint8_t profile_desc_frames_per_body(uint8_t payload_max)
{
	return (uint8_t)(payload_max / PROFILE_TLM_FRAME_LEN);
}

int profile_desc_long_count(const struct profile_descriptor *d,
			    uint8_t payload_max)
{
	uint8_t per;
	uint8_t count;

	if (d == NULL) {
		return -EINVAL;
	}
	per = profile_desc_frames_per_body(payload_max);
	if (per == 0u) {
		/* A payload that can't hold one whole descriptor frame:
		 * refused rather than falling back to 1 M, since a silent
		 * fallback would make transmission shape depend on a number
		 * nobody looked at. */
		return -EINVAL;
	}

	count = profile_desc_frame_count(d);
	if (count == 0u || count > PROFILE_TLM_MAX_FRAMES) {
		return -EINVAL;
	}

	return (int)(((uint32_t)count + per - 1u) / per);
}

int profile_desc_long_check(const struct profile_descriptor *d,
			    uint8_t payload_max)
{
	uint8_t per;
	uint8_t count;
	uint8_t longest;
	int     n;

	n = profile_desc_long_count(d, payload_max);
	if (n < 0) {
		return n;
	}

	/*
	 * A collapsed set is only meaningful on the coded PHY, and the
	 * descriptor has to say so: a node claiming 1 M and emitting a
	 * 32-byte body would put a frame on RF 57 in the ANT+ network address
	 * that no ANT receiver can parse and every ANT receiver will try to.
	 */
	if ((d->flags & PROFILE_TLM_FLAG_LR_PHY) == 0u) {
		return -EINVAL;
	}
	if (!d->has_schedule) {
		/* The LR bit and coding rate must agree, and the rate lives in
		 * the schedule block - an LR node without one has announced a
		 * PHY with no way to say how long its frames take. */
		return -EINVAL;
	}

	per = profile_desc_frames_per_body(payload_max);
	count = profile_desc_frame_count(d);

	/* The longest body this set will emit - a full one whenever there's
	 * more than one transmission, the remainder otherwise. Bounding on
	 * the longest rather than the average: a node must afford every
	 * frame it sends, not its mean frame. */
	longest = (count >= per) ? per : count;
	longest = (uint8_t)(longest * PROFILE_TLM_FRAME_LEN);

	return profile_sched_duty_check(d->schedule.coding, d->period,
					(uint8_t)(longest +
						  RADIANT_FRAME_HDR_LEN_LR));
}

int profile_desc_long_payload(const struct profile_descriptor *d,
			      const uint8_t *frames, uint8_t n_frames,
			      uint8_t payload_max, uint8_t index, uint8_t *out,
			      size_t out_len)
{
	uint8_t per;
	uint8_t first;
	uint8_t n;
	int     bodies;
	int     rc;

	if (d == NULL || frames == NULL || out == NULL) {
		return -EINVAL;
	}
	if (n_frames == 0u || n_frames > PROFILE_TLM_MAX_FRAMES) {
		return -EINVAL;
	}
	/* The caller's frame count and the descriptor's must agree, or the
	 * bytes packed aren't this descriptor's set. */
	if (n_frames != profile_desc_frame_count(d)) {
		return -EINVAL;
	}

	rc = profile_desc_long_check(d, payload_max);
	if (rc != 0) {
		return rc;
	}

	bodies = profile_desc_long_count(d, payload_max);
	if (bodies < 0) {
		return bodies;
	}
	if ((int)index >= bodies) {
		return -EINVAL;
	}

	per = profile_desc_frames_per_body(payload_max);
	first = (uint8_t)(index * per);
	n = (uint8_t)(n_frames - first);
	if (n > per) {
		n = per;
	}

	if (out_len < (size_t)n * PROFILE_TLM_FRAME_LEN) {
		return -ENOSPC;
	}

	memcpy(out, &frames[(size_t)first * PROFILE_TLM_FRAME_LEN],
	       (size_t)n * PROFILE_TLM_FRAME_LEN);

	return (int)((uint32_t)n * PROFILE_TLM_FRAME_LEN);
}

int profile_desc_rx_feed_long(struct profile_desc_rx *rx, const uint8_t *payload,
			      uint8_t payload_len)
{
	uint8_t i;
	int     rc = 0;

	if (rx == NULL || payload == NULL) {
		return -EINVAL;
	}
	/* A whole number of frames or nothing. The packer never emits a
	 * partial frame, so a payload not a multiple of eight didn't come
	 * from one - feeding the first floor(n/8) frames would accept
	 * exactly the corrupted cases while looking like robustness. */
	if (payload_len == 0u ||
	    (payload_len % PROFILE_TLM_FRAME_LEN) != 0u) {
		return -EPROTO;
	}

	for (i = 0u; i < payload_len / PROFILE_TLM_FRAME_LEN; i++) {
		rc = profile_desc_rx_feed(
			rx, &payload[(size_t)i * PROFILE_TLM_FRAME_LEN]);
		if (rc < 0) {
			return rc;
		}
	}

	/* The last chunk's verdict is the set's verdict: feeding is ordered
	 * and completeness is monotonic within one payload. */
	return rc;
}
