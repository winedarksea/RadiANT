/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_telemetry.c - device type 0x60, the RadiANT telemetry envelope.
 *
 * Provenance: docs/radiant-telemetry.md sections 3, 4, 5, 6, 7, 8, 9 and 11,
 * which is this project's own written specification of the envelope, authored
 * before any code existed. Frame layouts, flag assignments, the width-code
 * table, the class-0x30 accumulate rule and the "every reservation is
 * populated with zeros" rule all come from there. No adopter-gated ANT+ device
 * profile document was read for this file, no sdk-ant source was consulted,
 * and nothing here derives from libant.a. See
 * docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * The validation is the interesting part
 * ---------------------------------------------------------------------------
 * An encoder that emits whatever it was handed is half a specification. The
 * checks below are the other half, and each one exists because the failure it
 * prevents is silent on air:
 *
 *   - A CLASS-0x30 TYPE WITHOUT THE ACCUMULATE BIT decodes as a plausible
 *     instantaneous reading that resets to zero every wrap. Nobody notices for
 *     hours.
 *   - TWO FIELDS OVERLAPPING IN ONE PAGE produces two wrong numbers with a
 *     valid CRC, which is exactly the collision docs/profile-registry.md says
 *     "there is no error to catch".
 *   - A SPARSE NODE WITH heartbeat_s == 0 cannot be told apart from a dead
 *     one, and "no data" is the one reading a telemetry system must never
 *     produce silently (section 8).
 *   - A NON-SPARSE NODE WITH period == 0 asks a tracking receiver to predict a
 *     window from a period that does not exist.
 *   - THE OFF-57 BIT DISAGREEING WITH byte [6] leaves the receiver to choose
 *     which of the node's two statements about its own frequency to believe.
 *
 * All five are refused at the encoder and rejected at the decoder. Rejecting
 * at both ends is not redundancy: the encoder protects the air from this node,
 * and the decoder protects this receiver from somebody else's.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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
		 * 0..39. The descriptor's explicit bit offsets are what make
		 * this a schema change rather than a format break. */
		return 40u;
	}
	return 48u;
}

bool profile_desc_may_decode_data(const struct profile_descriptor *d)
{
	if (d == NULL) {
		return false;
	}
	/* v1 implements no transform at all, so ANY transform bit is one this
	 * receiver does not implement. Fail closed. */
	return (d->flags & PROFILE_TLM_FLAG_TRANSFORM_MASK) == 0u;
}

uint8_t profile_desc_frame_count(const struct profile_descriptor *d)
{
	if (d == NULL) {
		return 0u;
	}
	/* Two header frames plus one per field. The descriptor authentication
	 * frame is mandatory whenever a transform bit is set and absent
	 * otherwise; v1 sets none, so it never appears and this stays 2 + n. */
	return (uint8_t)(2u + d->n_fields);
}

int profile_desc_data_pages(const struct profile_descriptor *d, uint8_t *pages,
			    uint8_t cap)
{
	uint8_t n = 0u;
	uint8_t page;

	if (d == NULL || pages == NULL) {
		return -EINVAL;
	}

	/* Ascending, by scanning the small legal range rather than sorting: 15
	 * page numbers is fewer comparisons than a sort and the result is
	 * deterministic, which is what makes a receiver's rotation prediction
	 * and a test's expectation the same list. */
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

	/* Section 7: everything in class 0x30-0x3F MUST carry the accumulate
	 * bit. That is what the class is for. */
	if (profile_tlm_type_is_accumulating(f->type) && !f->accumulate) {
		return -EINVAL;
	}

	/* No two fields may overlap, and no two may share an id: an id is the
	 * envelope's topic name and a duplicate makes the schema ambiguous. */
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
		/* 15 is reserved for an extended descriptor, so a field count
		 * that would encode as 15 is not merely too big - it names a
		 * layout this version does not have. */
		return -EINVAL;
	}
	if (d->rf_index > PROFILE_TLM_RF_INDEX_MAX) {
		return -EINVAL;
	}
	if ((d->flags & PROFILE_TLM_FLAG_RSVD_3) != 0u) {
		/* Reserved-must-be-zero. Was X_PRIV; see the header. */
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
		/* Asynchronous - period 0x0000 - has no slot discipline at all
		 * and requires a continuous-scan receiver. A periodic node
		 * asking for it is asking a tracking receiver to predict a
		 * window from a period that does not exist. */
		if (d->period == 0u) {
			return -EINVAL;
		}
	}

	/* Section 11: every reservation is populated with zeros in v1. With no
	 * transform enabled the MAC window is one of them. */
	if ((d->flags & PROFILE_TLM_FLAG_X_AUTH) == 0u) {
		if (d->w_code != PROFILE_TLM_W_CODE_RESERVED) {
			return -EINVAL;
		}
	} else if (d->w_code == PROFILE_TLM_W_CODE_RESERVED ||
		   d->w_code > PROFILE_TLM_W_CODE_8) {
		/* W=1 is the reliable-command page's inline tag and is refused
		 * on a data page. */
		return -EINVAL;
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
	uint8_t i;
	uint8_t *f;
	int rc;

	if (d == NULL || frames == NULL) {
		return -EINVAL;
	}

	/*
	 * The set is refused rather than emitted when a transform is announced.
	 *
	 * CHECKED BEFORE THE REST OF THE VALIDATION, deliberately. X_AUTH
	 * shrinks the field area from 48 bits to 40, so a schema that is
	 * perfectly well formed today reports -ERANGE the moment the bit is
	 * set - a true statement about the fields, and a completely misleading
	 * answer to what was actually asked. The reason this build will not
	 * emit the set has nothing to do with where the fields sit.
	 * Section 6 makes the descriptor authentication frame MANDATORY
	 * whenever any transform bit is set, and it exists for a reason worth
	 * repeating: the descriptor carries the epoch, the window W, the
	 * transform flags and every field's offset and scale, so an attacker
	 * who forges one descriptor frame gets wrong readings out of correctly
	 * authenticated packets. The tag verifies and the numbers are still a
	 * lie. This build cannot produce that frame, so it does not get to
	 * announce a transform.
	 */
	if ((d->flags & PROFILE_TLM_FLAG_TRANSFORM_MASK) != 0u) {
		return -ENOTSUP;
	}

	rc = check_descriptor(d);
	if (rc != 0) {
		return rc;
	}

	count = profile_desc_frame_count(d);
	if (count > PROFILE_TLM_MAX_FRAMES) {
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
		 * documented exceptions to the counter invariant of section 4,
		 * and it never was a counter. */
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
	 * from the first shipped node, because a receiver with no real-time
	 * clock has to get the epoch from somewhere and the only place it can
	 * come from is a page a node already sends. Adding it later is a format
	 * break for every deployed node. */
	f = &frames[PROFILE_TLM_FRAME_LEN];
	f[2] = d->heartbeat_s;
	f[3] = (uint8_t)(((uint32_t)d->w_code << 6) |
			 ((uint32_t)d->k_code << 4));
	/* bits 3..0 stay zero: the informational-flag extension space, which
	 * the schedule block of a later phase fills. */
	f[4] = (uint8_t)(d->epoch & 0xFFu);
	f[5] = (uint8_t)((d->epoch >> 8) & 0xFFu);
	f[6] = (uint8_t)((d->epoch >> 16) & 0xFFu);
	f[7] = (uint8_t)((d->epoch >> 24) & 0xFFu);

	/* Frames 2..N - one per field. */
	for (i = 0u; i < d->n_fields; i++) {
		const struct profile_field *fd = &d->fields[i];

		f = &frames[(size_t)(2u + i) * PROFILE_TLM_FRAME_LEN];
		f[2] = fd->id;
		f[3] = fd->type;
		f[4] = (uint8_t)((fd->accumulate ? 0x80u : 0u) |
				 (fd->is_signed ? 0x40u : 0u) |
				 ((uint32_t)fd->width_code << 2));
		/* bits 1..0 reserved, must be 0 - and they are, because the
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
	struct profile_descriptor d;
	const uint8_t *f;
	uint8_t i;

	if (frames == NULL || out == NULL) {
		return -EINVAL;
	}
	if (n_frames < 2u || n_frames > PROFILE_TLM_MAX_FRAMES) {
		return -EPROTO;
	}

	memset(&d, 0, sizeof(d));

	/* Every frame must claim page 0x00 and must agree about the index and
	 * the count. A set assembled from two different broadcasts is the one
	 * way a receiver produces a schema nobody transmitted. */
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
	 * rather than guess, because the version governs the frame layout
	 * itself - including the meaning of every byte read above. */
	if (d.version != PROFILE_TLM_VERSION) {
		return -ENOTSUP;
	}
	if (d.n_fields == 0x0Fu) {
		/* Reserved for an extended descriptor this version has no
		 * layout for. Not "no fields" - a different frame set. */
		return -ENOTSUP;
	}
	if ((uint8_t)(2u + d.n_fields) != n_frames) {
		return -EPROTO;
	}

	f = &frames[PROFILE_TLM_FRAME_LEN];
	d.heartbeat_s = f[2];
	d.w_code = (uint8_t)((f[3] >> 6) & 0x03u);
	d.k_code = (uint8_t)((f[3] >> 4) & 0x03u);
	if ((f[3] & 0x0Fu) != 0u) {
		/* Reserved, must be 0 in v1 - and unlike an unknown
		 * informational FLAG, these bits have no defined meaning at
		 * all, so a receiver that ignored them would be inventing one.
		 * The phase that fills them changes the envelope version or
		 * uses frame 0's informational rule; either way, not this. */
		return -EPROTO;
	}
	d.epoch = (uint32_t)f[4] | ((uint32_t)f[5] << 8) |
		  ((uint32_t)f[6] << 16) | ((uint32_t)f[7] << 24);

	for (i = 0u; i < d.n_fields; i++) {
		struct profile_field *fd = &d.fields[i];

		f = &frames[(size_t)(2u + i) * PROFILE_TLM_FRAME_LEN];
		fd->id = f[2];
		fd->type = f[3];
		fd->accumulate = (f[4] & 0x80u) != 0u;
		fd->is_signed = (f[4] & 0x40u) != 0u;
		fd->width_code = (uint8_t)((f[4] >> 2) & 0x0Fu);
		if ((f[4] & 0x03u) != 0u) {
			return -EPROTO; /* reserved, must be 0 */
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
		/* The node changed the size of its set. Everything held is
		 * from the old one. */
		rx_restart(rx, count);
	}
	rx->count = count;

	if (index == 0u) {
		uint8_t schema_id = body[3];

		if (rx->schema_id_seen && schema_id != rx->schema_id) {
			/* This is what the schema id is FOR: a receiver
			 * notices a change after reading a single frame
			 * instead of the whole set. Anything already
			 * assembled describes the previous schema and would
			 * decode this node's pages at the wrong offsets. */
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

	/* Zeroed first, so a byte the schema does not claim is zero rather
	 * than whatever the last page left there. A later schema may claim it,
	 * and a receiver mid-change would read the stale value as a number. */
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
		/* A transform bit this build does not implement. Loudly, not
		 * silently: a receiver that returned zeros here would be
		 * reporting plaintext-looking numbers off a ciphertext page. */
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
	/* The trailing bytes are tag space. No RadiANT page may place an
	 * authentication tag anywhere but at the end. */
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
