/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work, written against src/profiles/'s three
 * headers, radiant/include/radiant/radiant_sched.h,
 * radiant/include/radiant/radiant_frame.h and
 * radiant/tests/fake_radio.h, with layout expectations taken from
 * docs/radiant-telemetry.md. See docs/decisions/0002-clean-room-policy.md.
 *
 * Tests for the RadiANT telemetry envelope, device type 0x60. The suite is
 * built around one gate: a 0x60 node is discovered and its schema is decoded
 * from the descriptor alone, with no out-of-band knowledge. "No out-of-band
 * knowledge" is enforced in test_gate_*: the receiving half shares no
 * variable with the transmitting half except the eight bytes that crossed the
 * air - no struct, field list, page numbers or widths - only a device number,
 * device type and transmission type (what a searching receiver recovers from
 * a matched address), with everything else decoded from page 0x00.
 *
 * The frames really do cross a radio, via radiant_sched.c against
 * fake_radio.c, which also proves a page-rotation client needs no new
 * slot-insertion hook in radiant_sched.c: a master posts a transmit, posts
 * the next from the completion, and declines to post in a slot a sparse node
 * skips. No new scheduler concept appears anywhere in this file.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include <radiant/radiant_frame.h>
#include <radiant/radiant_sched.h>

#include "../fake_radio.h"

#include "profile_bits.h"
#include "profile_sched.h"
#include "profile_telemetry.h"

/* ---------------------------------------------------------------------------
 * The node under test
 * ---------------------------------------------------------------------------
 */

#define NODE_DEVNUM   0x51A7u
#define NODE_SCHEMA   0x2Bu
#define NODE_PERIOD   8182u   /* counts of 1/32768 s, ~4.005 Hz */
#define NODE_FIELDS   4u

/* Four fields over two data pages, chosen so that every packing case the
 * envelope allows appears at least once: byte-aligned and not, signed and
 * unsigned, accumulating and instantaneous, and one field that straddles a
 * byte boundary in both directions. */
static void build_node_descriptor(struct profile_descriptor *d)
{
	memset(d, 0, sizeof(*d));
	d->version = PROFILE_TLM_VERSION;
	d->schema_id = NODE_SCHEMA;
	d->period = NODE_PERIOD;
	d->rf_index = PROFILE_TLM_RF_INDEX_DEFAULT;
	d->flags = 0u;
	d->n_fields = NODE_FIELDS;

	/* Heart rate, 8 bits, page 1 at offset 0. The motivating case of
	 * section 1, and the one Matter has no cluster for. */
	d->fields[0].id = 1u;
	d->fields[0].type = PROFILE_TLM_TYPE_HEART_RATE;
	d->fields[0].width_code = 4u; /* 8 bits */
	d->fields[0].page = 1u;
	d->fields[0].bit_offset = 0u;

	/* Temperature in kelvin, 16 bits, exp -2. 29315 -> 293.15 K. */
	d->fields[1].id = 2u;
	d->fields[1].type = PROFILE_TLM_TYPE_TEMPERATURE;
	d->fields[1].width_code = 7u; /* 16 bits */
	d->fields[1].exponent = -2;
	d->fields[1].page = 1u;
	d->fields[1].bit_offset = 8u;

	/* Cumulative energy, 32 bits, accumulating - class 0x30, so the
	 * accumulate bit is not optional. */
	d->fields[2].id = 3u;
	d->fields[2].type = PROFILE_TLM_TYPE_ENERGY;
	d->fields[2].accumulate = true;
	d->fields[2].width_code = 10u; /* 32 bits */
	d->fields[2].page = 2u;
	d->fields[2].bit_offset = 0u;

	/* Active power, 12 bits, SIGNED and NOT byte-aligned: it starts at bit
	 * 32 and runs to bit 44, so a receiver that packed little-endian gets
	 * a plausible wrong number rather than an error. */
	d->fields[3].id = 4u;
	d->fields[3].type = PROFILE_TLM_TYPE_ACTIVE_POWER;
	d->fields[3].is_signed = true;
	d->fields[3].width_code = 6u; /* 12 bits */
	d->fields[3].page = 2u;
	d->fields[3].bit_offset = 32u;
}

/* ---------------------------------------------------------------------------
 * The bit packer
 * ---------------------------------------------------------------------------
 *
 * These vectors are shared byte for byte with tools/test_ant_pages.py.
 * MSB-first bit order (docs/radiant-telemetry.md section 6) exists because
 * little-endian bit order is a reliable source of two implementations that
 * each work alone but disagree.
 */

ZTEST(profiles, test_bit_packer_vectors)
{
	struct {
		uint16_t off;
		uint8_t  width;
		uint64_t value;
		uint8_t  expect[6];
	} const cases[] = {
		/* One bit at the very top: offset 0 is the MSB of byte [0]. */
		{ 0u, 1u, 1u, { 0x80, 0, 0, 0, 0, 0 } },
		/* ...and at the very bottom of a 48-bit area. */
		{ 47u, 1u, 1u, { 0, 0, 0, 0, 0, 0x01 } },
		/* A byte-aligned byte is just a byte. */
		{ 8u, 8u, 0xA5u, { 0, 0xA5, 0, 0, 0, 0 } },
		/* 12 bits at offset 4: low nibble of byte 0, then byte 1. */
		{ 4u, 12u, 0xABCu, { 0x0A, 0xBC, 0, 0, 0, 0 } },
		/* 12 bits at offset 32, the signed power field of the node
		 * above, carrying -1 in twelve bits. */
		{ 32u, 12u, 0xFFFu, { 0, 0, 0, 0, 0xFF, 0xF0 } },
		/* A 6-bit field starting at bit 7 - straddling, unaligned both
		 * ends, which is the case a shift-and-mask gets wrong. */
		{ 7u, 6u, 0x2Bu, { 0x01, 0x58, 0, 0, 0, 0 } },
		/* The whole area as one 48-bit value, MSB first. */
		{ 0u, 48u, 0x0123456789ABull,
		  { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB } },
		/* 32 bits at offset 0, the energy accumulator. */
		{ 0u, 32u, 0xDEADBEEFu, { 0xDE, 0xAD, 0xBE, 0xEF, 0, 0 } },
		/* 10 bits at offset 3. */
		{ 3u, 10u, 0x155u, { 0x0A, 0xA8, 0, 0, 0, 0 } },
	};
	size_t i;

	for (i = 0u; i < ARRAY_SIZE(cases); i++) {
		uint8_t area[6];
		uint64_t back = 0u;

		memset(area, 0, sizeof(area));
		zassert_ok(profile_bits_pack(area, 48u, cases[i].off,
					     cases[i].width, cases[i].value),
			   "case %u refused", (unsigned)i);
		zassert_mem_equal(area, cases[i].expect, sizeof(area),
				  "case %u packed wrong", (unsigned)i);

		zassert_ok(profile_bits_unpack(area, 48u, cases[i].off,
					       cases[i].width, &back));
		zassert_equal(back, cases[i].value, "case %u did not round trip",
			      (unsigned)i);
	}
}

ZTEST(profiles, test_bit_packer_refuses_what_does_not_fit)
{
	uint8_t area[6] = { 0 };
	uint64_t out;

	/* One bit past the end of the area. */
	zassert_equal(-ERANGE, profile_bits_pack(area, 48u, 41u, 8u, 0u));
	zassert_equal(-ERANGE, profile_bits_unpack(area, 48u, 41u, 8u, &out));
	/* With X_AUTH the area is 40 bits, and the same field is now illegal
	 * at an offset that was legal a moment ago. */
	zassert_equal(-ERANGE, profile_bits_pack(area, 40u, 33u, 8u, 0u));
	zassert_ok(profile_bits_pack(area, 40u, 32u, 8u, 0u));
	/* Zero width is not "no field", it is a malformed one. */
	zassert_equal(-EINVAL, profile_bits_pack(area, 48u, 0u, 0u, 0u));
}

ZTEST(profiles, test_width_codes_are_the_documented_table)
{
	static const uint8_t expect[13] = {
		1u, 2u, 4u, 6u, 8u, 10u, 12u, 16u, 20u, 24u, 32u, 40u, 48u
	};
	uint8_t code;

	for (code = 0u; code < 13u; code++) {
		zassert_equal((int)expect[code], profile_bits_width(code),
			      "width code %u", code);
		zassert_equal((int)code, profile_bits_width_code(expect[code]));
	}
	/* 13..15 are reserved, and are rejected rather than clamped: a
	 * receiver that guesses a width decodes every field after it at the
	 * wrong offset. */
	zassert_equal(-EINVAL, profile_bits_width(13u));
	zassert_equal(-EINVAL, profile_bits_width(15u));
	zassert_equal(-EINVAL, profile_bits_width_code(64u));
}

ZTEST(profiles, test_sign_extension_happens_in_the_fields_own_width)
{
	zassert_equal(-1, profile_bits_sign_extend(0xFFFu, 12u));
	zassert_equal(-2048, profile_bits_sign_extend(0x800u, 12u));
	zassert_equal(2047, profile_bits_sign_extend(0x7FFu, 12u));
	zassert_equal(-1, profile_bits_sign_extend(0xFFu, 8u));
	zassert_equal(127, profile_bits_sign_extend(0x7Fu, 8u));
	/* A one-bit signed field is a strange thing to declare, but it is
	 * legal, and -1/0 is what two's complement says it means. */
	zassert_equal(-1, profile_bits_sign_extend(1u, 1u));
}

/* ---------------------------------------------------------------------------
 * The descriptor
 * ---------------------------------------------------------------------------
 */

ZTEST(profiles, test_descriptor_round_trip_and_frame_layout)
{
	struct profile_descriptor d, back;
	uint8_t frames[PROFILE_TLM_MAX_FRAMES * PROFILE_TLM_FRAME_LEN];
	int n;
	uint8_t i;

	build_node_descriptor(&d);
	n = profile_desc_encode(&d, frames, PROFILE_TLM_MAX_FRAMES);
	zassert_equal(2 + (int)NODE_FIELDS, n, "frame count is 2 + field count");

	/* Every frame claims page 0x00 and carries (index << 4) | (count - 1)
	 * in byte [1] - a frame index, one of the two documented exceptions to
	 * the counter invariant of section 4. */
	for (i = 0u; i < (uint8_t)n; i++) {
		const uint8_t *f = &frames[(size_t)i * PROFILE_TLM_FRAME_LEN];

		zassert_equal(PROFILE_TLM_PAGE_DESCRIPTOR, f[0]);
		zassert_equal((uint8_t)((i << 4) | (n - 1)), f[1],
			      "frame %u index byte", i);
	}

	/* Frame 0, byte by byte. */
	zassert_equal((uint8_t)((PROFILE_TLM_VERSION << 4) | NODE_FIELDS),
		      frames[2], "version and field count share byte [2]");
	zassert_equal(NODE_SCHEMA, frames[3]);
	zassert_equal((uint8_t)(NODE_PERIOD & 0xFFu), frames[4],
		      "period is little-endian");
	zassert_equal((uint8_t)(NODE_PERIOD >> 8), frames[5]);
	zassert_equal(PROFILE_TLM_RF_INDEX_DEFAULT, frames[6]);
	zassert_equal(0u, frames[7], "no flag is set on a plain v1 node");

	zassert_ok(profile_desc_decode(frames, (uint8_t)n, &back));
	zassert_equal(d.version, back.version);
	zassert_equal(d.schema_id, back.schema_id);
	zassert_equal(d.period, back.period);
	zassert_equal(d.rf_index, back.rf_index);
	zassert_equal(d.flags, back.flags);
	zassert_equal(d.epoch, back.epoch);
	zassert_equal(d.n_fields, back.n_fields);
	for (i = 0u; i < d.n_fields; i++) {
		zassert_equal(d.fields[i].id, back.fields[i].id, "field %u id", i);
		zassert_equal(d.fields[i].type, back.fields[i].type);
		zassert_equal(d.fields[i].accumulate, back.fields[i].accumulate);
		zassert_equal(d.fields[i].is_signed, back.fields[i].is_signed);
		zassert_equal(d.fields[i].width_code, back.fields[i].width_code);
		zassert_equal(d.fields[i].exponent, back.fields[i].exponent);
		zassert_equal(d.fields[i].page, back.fields[i].page);
		zassert_equal(d.fields[i].bit_offset, back.fields[i].bit_offset);
	}
}

/*
 * docs/radiant-telemetry.md section 11: "Every reservation in this document is
 * populated with zeros in v1." Asserted positively rather than trusted,
 * because a reserved bit that quietly carries something is a format break the
 * day the phase that owns it arrives.
 */
ZTEST(profiles, test_every_reserved_field_is_zero)
{
	struct profile_descriptor d;
	uint8_t frames[PROFILE_TLM_MAX_FRAMES * PROFILE_TLM_FRAME_LEN];
	const uint8_t *f1;
	int n;
	uint8_t i;

	build_node_descriptor(&d);
	n = profile_desc_encode(&d, frames, PROFILE_TLM_MAX_FRAMES);
	zassert_true(n > 0);

	/* Frame 0 byte [7]: no transform bit, and INFO bit 3 - the withdrawn
	 * X_PRIV - is reserved-must-be-zero. */
	zassert_equal(0u, frames[7] & PROFILE_TLM_FLAG_TRANSFORM_MASK);
	zassert_equal(0u, frames[7] & PROFILE_TLM_FLAG_RSVD_3);

	f1 = &frames[PROFILE_TLM_FRAME_LEN];
	/* Not sparse, so the heartbeat byte is zero. */
	zassert_equal(0u, f1[2]);
	/* Byte [3]: W is reserved with no transform, k is meaningless off
	 * sparse, and bits 3..0 are the informational-flag extension space a
	 * later phase's schedule block claims. All zero. */
	zassert_equal(0u, f1[3]);
	/* Epoch: zero on a node with no transform and no command page. */
	zassert_equal(0u, f1[4] | f1[5] | f1[6] | f1[7]);

	/* Field frames: encoding byte bits 1..0 reserved, must be 0. */
	for (i = 0u; i < d.n_fields; i++) {
		const uint8_t *f = &frames[(size_t)(2u + i) * PROFILE_TLM_FRAME_LEN];

		zassert_equal(0u, f[4] & 0x03u, "field %u encoding reserved bits",
			      i);
	}
}

ZTEST(profiles, test_the_encoder_refuses_a_malformed_schema)
{
	struct profile_descriptor d;
	uint8_t frames[PROFILE_TLM_MAX_FRAMES * PROFILE_TLM_FRAME_LEN];

	/* A class-0x30 type without the accumulate bit. Section 7: "a
	 * descriptor that clears the bit on one of these types is
	 * malformed." */
	build_node_descriptor(&d);
	d.fields[2].accumulate = false;
	zassert_equal(-EINVAL, profile_desc_encode(&d, frames,
						   PROFILE_TLM_MAX_FRAMES));

	/* Two fields overlapping in one page: two wrong numbers with a valid
	 * CRC, and no error anywhere to catch it. */
	build_node_descriptor(&d);
	d.fields[1].bit_offset = 4u; /* 16 bits from 4 collides with 8 from 0 */
	zassert_equal(-EINVAL, profile_desc_encode(&d, frames,
						   PROFILE_TLM_MAX_FRAMES));

	/* A field running off the end of the field area. */
	build_node_descriptor(&d);
	d.fields[2].bit_offset = 20u; /* 32 bits from 20 needs 52 */
	zassert_equal(-ERANGE, profile_desc_encode(&d, frames,
						   PROFILE_TLM_MAX_FRAMES));

	/* Two fields sharing an id: the envelope's topic name, ambiguous. */
	build_node_descriptor(&d);
	d.fields[1].id = d.fields[0].id;
	zassert_equal(-EINVAL, profile_desc_encode(&d, frames,
						   PROFILE_TLM_MAX_FRAMES));

	/* The node's two statements about its own frequency disagreeing. */
	build_node_descriptor(&d);
	d.rf_index = 80u; /* off 57, but the informational bit is clear */
	zassert_equal(-EINVAL, profile_desc_encode(&d, frames,
						   PROFILE_TLM_MAX_FRAMES));
	d.flags |= PROFILE_TLM_FLAG_OFF_RF57;
	zassert_true(profile_desc_encode(&d, frames, PROFILE_TLM_MAX_FRAMES) > 0,
		     "agreeing about RF 80 is fine");

	/* A sparse node with no heartbeat cannot be told from a dead one. */
	build_node_descriptor(&d);
	d.flags |= PROFILE_TLM_FLAG_SPARSE;
	d.heartbeat_s = 0u;
	zassert_equal(-EINVAL, profile_desc_encode(&d, frames,
						   PROFILE_TLM_MAX_FRAMES));

	/* Descriptor INFO bit 3: reserved-must-be-zero. ADR 0006 keeps it
	 * reserved rather than reclaiming it, so revisiting rotation stays a
	 * format-compatible change - and announcing a privacy posture in the
	 * clear is itself a leak. */
	build_node_descriptor(&d);
	d.flags |= PROFILE_TLM_FLAG_RSVD_3;
	zassert_equal(-EINVAL, profile_desc_encode(&d, frames,
						   PROFILE_TLM_MAX_FRAMES));
}

/*
 * v1 announces no transform, because the descriptor authentication frame
 * section 6 makes mandatory alongside one has no implementation yet.
 * Refusing to encode is the honest response: a transform announced without
 * the frame that authenticates the schema gets an attacker wrong readings
 * out of correctly authenticated packets.
 */
ZTEST(profiles, test_v1_refuses_to_announce_a_transform)
{
	struct profile_descriptor d;
	uint8_t frames[PROFILE_TLM_MAX_FRAMES * PROFILE_TLM_FRAME_LEN];

	build_node_descriptor(&d);
	d.flags |= PROFILE_TLM_FLAG_X_AUTH;
	d.w_code = PROFILE_TLM_W_CODE_4;
	d.epoch = 7u;
	zassert_equal(-ENOTSUP, profile_desc_encode(&d, frames,
						    PROFILE_TLM_MAX_FRAMES));

	/* And a v1 receiver that HEARS one decodes the descriptor - it has
	 * still learned the node's identity, period and RF index - but must
	 * not turn its data pages into numbers. Fail closed means "do not
	 * decode", not "do not listen". */
	d.flags = PROFILE_TLM_FLAG_X_CONF;
	d.w_code = 0u;
	zassert_false(profile_desc_may_decode_data(&d));
	d.flags = 0u;
	zassert_true(profile_desc_may_decode_data(&d));
}

ZTEST(profiles, test_a_receiver_rejects_a_version_it_does_not_implement)
{
	struct profile_descriptor d, back;
	uint8_t frames[PROFILE_TLM_MAX_FRAMES * PROFILE_TLM_FRAME_LEN];
	int n;

	build_node_descriptor(&d);
	n = profile_desc_encode(&d, frames, PROFILE_TLM_MAX_FRAMES);
	zassert_true(n > 0);

	/* Version 2 in the high nibble of frame 0 byte [2]. The version
	 * governs the frame layout itself, so a receiver that guessed would be
	 * reading fields at offsets that may not exist. */
	frames[2] = (uint8_t)(0x20u | NODE_FIELDS);
	zassert_equal(-ENOTSUP, profile_desc_decode(frames, (uint8_t)n, &back));

	/* Field count 15 is reserved for an extended descriptor: a different
	 * frame set, not fifteen fields. */
	frames[2] = (uint8_t)((PROFILE_TLM_VERSION << 4) | 0x0Fu);
	zassert_equal(-ENOTSUP, profile_desc_decode(frames, (uint8_t)n, &back));
}

/* ---------------------------------------------------------------------------
 * Receiver-side assembly
 * ---------------------------------------------------------------------------
 */

ZTEST(profiles, test_the_set_assembles_out_of_order_and_through_holes)
{
	struct profile_descriptor d, back;
	struct profile_desc_rx rx;
	uint8_t frames[PROFILE_TLM_MAX_FRAMES * PROFILE_TLM_FRAME_LEN];
	int n;
	int8_t i;

	build_node_descriptor(&d);
	n = profile_desc_encode(&d, frames, PROFILE_TLM_MAX_FRAMES);
	profile_desc_rx_init(&rx);

	/* Backwards, which is the same problem as "joined mid-set and wrapped
	 * round" without needing two cycles to express. */
	for (i = (int8_t)(n - 1); i >= 0; i--) {
		int rc = profile_desc_rx_feed(&rx,
			&frames[(size_t)i * PROFILE_TLM_FRAME_LEN]);

		zassert_true(rc >= 0, "frame %d refused", i);
		zassert_equal(i == 0 ? 1 : 0, rc,
			      "the set completes exactly when the last hole "
			      "fills, not before");
	}
	zassert_ok(profile_desc_rx_take(&rx, &back));
	zassert_equal(NODE_SCHEMA, back.schema_id);
	zassert_equal(NODE_FIELDS, back.n_fields);

	/* A data page and a common page are not descriptor frames and must not
	 * disturb an assembly in progress. */
	profile_desc_rx_init(&rx);
	zassert_equal(0, profile_desc_rx_feed(&rx, frames));
	{
		uint8_t data[8] = { 0x01u, 0x07u, 0, 0, 0, 0, 0, 0 };
		uint8_t p80[8] = { 0x50u, 0xFFu, 0xFFu, 1u, 0xFFu, 0, 1u, 0 };

		zassert_equal(0, profile_desc_rx_feed(&rx, data));
		zassert_equal(0, profile_desc_rx_feed(&rx, p80));
	}
	zassert_false(profile_desc_rx_complete(&rx));
	zassert_equal(-EAGAIN, profile_desc_rx_take(&rx, &back));
}

/*
 * The schema id's whole job: a receiver notices a change after reading a
 * single frame instead of the whole set. Anything already assembled
 * describes the previous schema, and decoding against it would put every
 * field at the wrong offset.
 */
ZTEST(profiles, test_a_new_schema_id_abandons_the_partial_set)
{
	struct profile_descriptor d, back;
	struct profile_desc_rx rx;
	uint8_t old_frames[PROFILE_TLM_MAX_FRAMES * PROFILE_TLM_FRAME_LEN];
	uint8_t new_frames[PROFILE_TLM_MAX_FRAMES * PROFILE_TLM_FRAME_LEN];
	int n;
	uint8_t i;

	build_node_descriptor(&d);
	n = profile_desc_encode(&d, old_frames, PROFILE_TLM_MAX_FRAMES);

	d.schema_id = (uint8_t)(NODE_SCHEMA + 1u);
	d.fields[0].bit_offset = 16u; /* the change the new id announces */
	d.fields[1].bit_offset = 24u;
	zassert_equal(n, profile_desc_encode(&d, new_frames,
					     PROFILE_TLM_MAX_FRAMES));

	profile_desc_rx_init(&rx);
	/* Half the old set... */
	for (i = 0u; i < (uint8_t)(n - 1); i++) {
		zassert_equal(0, profile_desc_rx_feed(&rx,
			&old_frames[(size_t)i * PROFILE_TLM_FRAME_LEN]));
	}
	/* ...then frame 0 of the new one. */
	zassert_equal(0, profile_desc_rx_feed(&rx, new_frames));
	zassert_false(profile_desc_rx_complete(&rx),
		      "the old frames must not count towards the new set");
	zassert_equal(1u, rx.resets);

	for (i = 1u; i < (uint8_t)n; i++) {
		(void)profile_desc_rx_feed(&rx,
			&new_frames[(size_t)i * PROFILE_TLM_FRAME_LEN]);
	}
	zassert_ok(profile_desc_rx_take(&rx, &back));
	zassert_equal((uint8_t)(NODE_SCHEMA + 1u), back.schema_id);
	zassert_equal(16u, back.fields[0].bit_offset);
}

/* ---------------------------------------------------------------------------
 * Data pages
 * ---------------------------------------------------------------------------
 */

ZTEST(profiles, test_data_pages_round_trip_including_the_signed_field)
{
	struct profile_descriptor d;
	int64_t in[NODE_FIELDS] = { 0 };
	int64_t out[NODE_FIELDS] = { 0 };
	uint8_t body[8];
	uint16_t present = 0u;

	build_node_descriptor(&d);

	in[0] = 62;            /* bpm */
	in[1] = 29315;         /* 293.15 K at exp -2 */
	in[2] = 0xFFFFFFF0ull; /* energy, near its wrap */
	in[3] = -250;          /* watts, signed and unaligned */

	zassert_equal(2, profile_data_encode(&d, 1u, 0x11u, in, NODE_FIELDS,
					     body));
	zassert_equal(1u, body[0], "byte [0] is the page number");
	zassert_equal(0x11u, body[1], "byte [1] is the event counter");
	zassert_equal(62u, body[2]);
	zassert_equal(2, profile_data_decode(&d, body, out, NODE_FIELDS, &present));
	zassert_equal(0x03u, present, "page 1 carries fields 0 and 1");
	zassert_equal(62, out[0]);
	zassert_equal(29315, out[1]);

	zassert_equal(2, profile_data_encode(&d, 2u, 0x12u, in, NODE_FIELDS,
					     body));
	zassert_equal(2, profile_data_decode(&d, body, out, NODE_FIELDS, &present));
	zassert_equal(0x0Cu, present, "page 2 carries fields 2 and 3");
	zassert_equal((int64_t)0xFFFFFFF0ull, out[2]);
	zassert_equal(-250, out[3], "a signed field is sign-extended in its "
				    "own width, not widened before packing");

	/* Bits the schema does not claim are zero. Page 2 uses bits 0..43, so
	 * the last four bits of byte [7] are the node's to leave alone - and
	 * it leaves them zero, because a later schema may claim them and a
	 * receiver mid-change would read a stale value as a number. */
	zassert_equal(0u, body[7] & 0x0Fu);
}

ZTEST(profiles, test_an_accumulating_field_wraps_rather_than_saturating)
{
	struct profile_descriptor d;
	int64_t in[NODE_FIELDS] = { 0 };
	int64_t out[NODE_FIELDS] = { 0 };
	uint8_t body[8];

	build_node_descriptor(&d);

	/* Section 5: the transmitted value is a running total in the field's
	 * declared width and it is MEANT to wrap. Handing the encoder a wider
	 * counter must truncate, not refuse - otherwise every caller has to
	 * own the masking, and therefore the wrap semantics. */
	in[2] = 0x1000000FFull; /* 2^32 + 255 */
	zassert_true(profile_data_encode(&d, 2u, 0u, in, NODE_FIELDS, body) > 0);
	zassert_equal(2, profile_data_decode(&d, body, out, NODE_FIELDS, NULL));
	zassert_equal(255, out[2], "the accumulator wrapped in 32 bits");
}

/* ---------------------------------------------------------------------------
 * The interleave
 * ---------------------------------------------------------------------------
 */

static struct profile_descriptor sched_desc;
static int64_t sched_values[NODE_FIELDS];

static int cb_data_page(uint8_t page, uint8_t counter, uint8_t *body, void *user)
{
	ARG_UNUSED(user);
	return profile_data_encode(&sched_desc, page, counter, sched_values,
				   sched_desc.n_fields, body) < 0 ? -EINVAL : 0;
}

static int cb_common_80(uint8_t *body, void *user)
{
	/* Manufacturer 0x00FF, the development id an unregistered device is
	 * supposed to use rather than borrowing somebody else's. */
	static const uint8_t p80[8] = {
		0x50u, 0xFFu, 0xFFu, 0x01u, 0xFFu, 0x00u, 0x01u, 0x00u
	};

	ARG_UNUSED(user);
	memcpy(body, p80, sizeof(p80));
	return 0;
}

static int cb_common_81(uint8_t *body, void *user)
{
	/* serial = 0xFFFFFFFF, the "not supplied" sentinel. A 32-bit globally
	 * unique serial in the clear every 30 seconds is strictly more
	 * identifying than the 16-bit device number and is unaffected by any
	 * re-roll, so this is what a node with a privacy posture must send. */
	static const uint8_t p81[8] = {
		0x51u, 0xFFu, 0xFFu, 0x01u, 0xFFu, 0xFFu, 0xFFu, 0xFFu
	};

	ARG_UNUSED(user);
	memcpy(body, p81, sizeof(p81));
	return 0;
}

static int cb_common_82(uint8_t *body, void *user)
{
	static const uint8_t p82[8] = {
		0x52u, 0xFFu, 0x00u, 0x00u, 0x00u, 0x00u, 0x80u, 0x83u
	};

	ARG_UNUSED(user);
	memcpy(body, p82, sizeof(p82));
	return 0;
}

static void sched_cfg_init(struct profile_sched_cfg *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->desc = &sched_desc;
	cfg->data_page = cb_data_page;
	cfg->common_80 = cb_common_80;
	cfg->common_81 = cb_common_81;
}

ZTEST(profiles, test_the_interleave_is_119_120_over_121)
{
	struct profile_sched ps;
	struct profile_sched_cfg cfg;
	uint8_t body[8];
	uint32_t slot;
	uint32_t cycle;
	uint32_t data_pages = 0u;

	build_node_descriptor(&sched_desc);
	sched_cfg_init(&cfg);
	zassert_ok(profile_sched_init(&ps, &cfg));

	/* Burn the power-up burst so the assertions below run over settled
	 * cycles rather than over the first one. A node transmits its schema
	 * before anything else on purpose - a receiver already listening
	 * should not wait for message 0 of the second cycle. */
	for (slot = 0u; slot < PROFILE_TLM_CYCLE; slot++) {
		(void)profile_sched_next(&ps, body);
	}

	for (cycle = 0u; cycle < 3u; cycle++) {
		for (slot = 0u; slot < PROFILE_TLM_CYCLE; slot++) {
			enum profile_slot_kind kind =
				profile_sched_next(&ps, body);

			if (slot <= 5u) {
				/* 2 + 4 fields = 6 frames, consecutive, at
				 * messages 0..5. Spreading them one per cycle
				 * would make a mid-stream join take six
				 * cycles - over a minute at 4 Hz, over an
				 * hour at 0.25 Hz. */
				zassert_equal(PROFILE_SLOT_DESCRIPTOR, kind,
					      "cycle %u slot %u", cycle, slot);
				zassert_equal(PROFILE_TLM_PAGE_DESCRIPTOR,
					      body[0]);
				zassert_equal((uint8_t)((slot << 4) | 5u),
					      body[1],
					      "descriptor frames arrive in "
					      "index order");
			} else if (slot == PROFILE_TLM_SLOT_PAGE_80) {
				zassert_equal(PROFILE_SLOT_COMMON_80, kind);
				zassert_equal(0x50u, body[0]);
			} else if (slot == PROFILE_TLM_SLOT_PAGE_81) {
				zassert_equal(PROFILE_SLOT_COMMON_81, kind);
				zassert_equal(0x51u, body[0]);
			} else {
				zassert_equal(PROFILE_SLOT_DATA, kind,
					      "cycle %u slot %u", cycle, slot);
				data_pages++;
			}
		}
	}

	/* 121 - 6 descriptor - 2 common = 113 data pages per cycle. */
	zassert_equal(3u * 113u, data_pages);
}

ZTEST(profiles, test_the_rotation_alternates_and_the_counter_counts_only_data)
{
	struct profile_sched ps;
	struct profile_sched_cfg cfg;
	uint8_t body[8];
	uint32_t slot;
	uint32_t n_data = 0u;
	uint8_t expect_page = 1u;
	uint8_t expect_counter = 0u;

	build_node_descriptor(&sched_desc);
	sched_cfg_init(&cfg);
	zassert_ok(profile_sched_init(&ps, &cfg));

	for (slot = 0u; slot < 3u * PROFILE_TLM_CYCLE; slot++) {
		enum profile_slot_kind kind = profile_sched_next(&ps, body);

		if (kind != PROFILE_SLOT_DATA) {
			continue;
		}
		zassert_equal(expect_page, body[0],
			      "the rotation runs the schema's pages ascending, "
			      "slot %u", slot);
		zassert_equal(expect_counter, body[1],
			      "the counter counts data pages only, slot %u",
			      slot);
		expect_page = (uint8_t)(expect_page == 1u ? 2u : 1u);
		expect_counter++; /* uint8_t: wraps at 256, like the wire */
		n_data++;
	}

	/* 339 data pages over three cycles, which is past the counter's wrap -
	 * so the assertion above checked the wrap for real rather than
	 * checking arithmetic that never reached it. */
	zassert_equal(3u * 113u, n_data);
	zassert_true(n_data > 256u, "the run was too short to cross the wrap");
}

/*
 * The client seam: a client profile family gets first refusal on the slots
 * the rotation would fill with a data page. It is never offered message 119,
 * 120 or a descriptor frame - those three are the cadence rule, and a client
 * that could displace them would be forking the rotation, which one engine
 * exists to prevent.
 */
static uint32_t client_offers;
static uint32_t client_claims;

static bool client_claim_every_third(uint32_t m, uint8_t *body, void *user)
{
	ARG_UNUSED(user);
	client_offers++;
	if ((m % 3u) != 0u) {
		return false;
	}
	client_claims++;
	memset(body, 0, 8);
	body[0] = 0xF0u; /* a vendor-private page: never registered, never
			  * bridged, never assumed to mean anything */
	body[1] = (uint8_t)m;
	return true;
}

ZTEST(profiles, test_the_client_seam_takes_slots_but_never_the_cadence)
{
	struct profile_sched ps;
	struct profile_sched_cfg cfg;
	struct profile_sched_client client = {
		.claim = client_claim_every_third,
		.user = NULL,
	};
	uint8_t body[8];
	uint32_t slot;
	uint32_t claimed = 0u;

	build_node_descriptor(&sched_desc);
	sched_cfg_init(&cfg);
	zassert_ok(profile_sched_init(&ps, &cfg));
	zassert_ok(profile_sched_set_client(&ps, &client));

	client_offers = 0u;
	client_claims = 0u;

	for (slot = 0u; slot < PROFILE_TLM_CYCLE; slot++) {
		enum profile_slot_kind kind = profile_sched_next(&ps, body);

		if (slot <= 5u) {
			zassert_equal(PROFILE_SLOT_DESCRIPTOR, kind,
				      "slot %u went to the client", slot);
		} else if (slot == PROFILE_TLM_SLOT_PAGE_80) {
			zassert_equal(PROFILE_SLOT_COMMON_80, kind);
		} else if (slot == PROFILE_TLM_SLOT_PAGE_81) {
			zassert_equal(PROFILE_SLOT_COMMON_81, kind);
		} else if (kind == PROFILE_SLOT_CLIENT) {
			zassert_equal(0xF0u, body[0]);
			zassert_equal((uint8_t)slot, body[1]);
			claimed++;
		} else {
			zassert_equal(PROFILE_SLOT_DATA, kind);
		}
	}

	/* Offered exactly the 113 data slots, and no others. */
	zassert_equal(113u, client_offers);
	zassert_equal(client_claims, claimed);
	zassert_true(claimed > 30u, "the client took a real share of the "
				    "rotation");

	/* Removing the client hands every slot straight back. */
	zassert_ok(profile_sched_set_client(&ps, NULL));
	client_offers = 0u;
	for (slot = 0u; slot < PROFILE_TLM_CYCLE; slot++) {
		(void)profile_sched_next(&ps, body);
	}
	zassert_equal(0u, client_offers);
}

/*
 * A client profile family with no descriptor of its own - an ANT+
 * compatibility device type, the actual first caller. The interleave must
 * hold with the descriptor slots simply absent, or that case would force a
 * fork.
 */
ZTEST(profiles, test_a_client_with_no_descriptor_still_gets_the_interleave)
{
	struct profile_sched ps;
	struct profile_sched_cfg cfg;
	struct profile_sched_client client = {
		.claim = client_claim_every_third,
		.user = NULL,
	};
	uint8_t body[8];
	uint32_t slot;

	memset(&cfg, 0, sizeof(cfg));
	cfg.desc = NULL; /* no schema; the client builds every data body */
	cfg.common_80 = cb_common_80;
	cfg.common_81 = cb_common_81;

	zassert_ok(profile_sched_init(&ps, &cfg));
	zassert_ok(profile_sched_set_client(&ps, &client));
	client_offers = 0u;

	for (slot = 0u; slot < PROFILE_TLM_CYCLE; slot++) {
		enum profile_slot_kind kind = profile_sched_next(&ps, body);

		if (slot == PROFILE_TLM_SLOT_PAGE_80) {
			zassert_equal(PROFILE_SLOT_COMMON_80, kind);
		} else if (slot == PROFILE_TLM_SLOT_PAGE_81) {
			zassert_equal(PROFILE_SLOT_COMMON_81, kind);
		} else if ((slot % 3u) == 0u) {
			zassert_equal(PROFILE_SLOT_CLIENT, kind, "slot %u",
				      slot);
		} else {
			/* No descriptor and no data_page builder, so an
			 * unclaimed slot is idle rather than invented. */
			zassert_equal(PROFILE_SLOT_IDLE, kind, "slot %u", slot);
		}
	}
	zassert_equal(119u, client_offers, "every slot but 119 and 120");
}

/* ---------------------------------------------------------------------------
 * Sparse mode
 * ---------------------------------------------------------------------------
 */

static void build_sparse_descriptor(struct profile_descriptor *d, uint8_t hb_s)
{
	build_node_descriptor(d);
	d->flags |= PROFILE_TLM_FLAG_SPARSE;
	d->heartbeat_s = hb_s;
	d->k_code = PROFILE_TLM_K_CODE_3; /* the documented default, 3x */
}

ZTEST(profiles, test_a_sparse_node_is_silent_between_heartbeats)
{
	struct profile_sched ps;
	struct profile_sched_cfg cfg;
	uint8_t body[8];
	uint32_t slot;
	uint32_t descriptor_frames = 0u;
	uint32_t transmissions = 0u;
	uint32_t hb_slots;

	/* 30 s heartbeat at 8182 counts is 120 slots between bursts. The 121
	 * interleave is useless to a node like this, which is exactly why
	 * section 8 replaces it rather than tuning it. */
	build_sparse_descriptor(&sched_desc, 30u);
	sched_cfg_init(&cfg);
	cfg.common_80 = NULL; /* section 8 gives a sparse node no 121 cycle to
			       * hang a common page on */
	cfg.common_81 = NULL;
	zassert_ok(profile_sched_init(&ps, &cfg));

	hb_slots = (30u * 32768u) / NODE_PERIOD;
	zassert_equal(120u, hb_slots);

	for (slot = 0u; slot < 3u * hb_slots; slot++) {
		enum profile_slot_kind kind = profile_sched_next(&ps, body);

		if (kind == PROFILE_SLOT_IDLE) {
			continue;
		}
		transmissions++;
		zassert_equal(PROFILE_SLOT_DESCRIPTOR, kind,
			      "a quiet sparse node transmits nothing but its "
			      "heartbeat, slot %u", slot);
		descriptor_frames++;
	}

	/* Three heartbeats, six frames each. Every heartbeat carries the WHOLE
	 * set, because a receiver that joined mid-stream has no other way to
	 * get a schema before the next one. */
	zassert_equal(18u, descriptor_frames);
	zassert_equal(18u, transmissions);
	/* 360 slots, 18 transmissions: the node was silent 95% of the time
	 * while keeping its period configured, which is the whole of the
	 * energy argument. */
}

ZTEST(profiles, test_a_sparse_event_is_repeated_k_times)
{
	struct profile_sched ps;
	struct profile_sched_cfg cfg;
	uint8_t body[8];
	uint32_t slot;
	uint32_t data_pages = 0u;
	uint8_t counters[8];

	build_sparse_descriptor(&sched_desc, 60u);
	sched_cfg_init(&cfg);
	cfg.common_80 = NULL;
	cfg.common_81 = NULL;
	zassert_ok(profile_sched_init(&ps, &cfg));

	/* Clear the power-up heartbeat burst. */
	for (slot = 0u; slot < 16u; slot++) {
		(void)profile_sched_next(&ps, body);
	}

	zassert_ok(profile_sched_post_event(&ps, 1u));
	for (slot = 0u; slot < 10u; slot++) {
		enum profile_slot_kind kind = profile_sched_next(&ps, body);

		if (kind == PROFILE_SLOT_IDLE) {
			continue;
		}
		zassert_equal(PROFILE_SLOT_DATA, kind);
		zassert_equal(1u, body[0]);
		counters[data_pages] = body[1];
		data_pages++;
	}

	/* k = 3, one per slot, spaced by roughly one channel period - because
	 * there is no retransmission and a scanning receiver may be mid-dwell
	 * elsewhere. */
	zassert_equal(3u, data_pages);
	/* Distinct counters, which is what lets the receiver deduplicate the
	 * repeats. The same counter detects loss in periodic mode: one
	 * mechanism, two jobs. */
	zassert_equal((uint8_t)(counters[0] + 1u), counters[1]);
	zassert_equal((uint8_t)(counters[1] + 1u), counters[2]);

	/* Command 0x06, "send the descriptor set now" - how a receiver that
	 * just joined gets a schema without waiting for the next heartbeat. */
	profile_sched_request_descriptor(&ps);
	for (slot = 0u; slot < 6u; slot++) {
		zassert_equal(PROFILE_SLOT_DESCRIPTOR,
			      profile_sched_next(&ps, body), "slot %u", slot);
	}
	zassert_equal(PROFILE_SLOT_IDLE, profile_sched_next(&ps, body));
}

/*
 * The asset tag: no fields at all, a heartbeat, and page 82 - suppressed by
 * section 6's privacy rule. It is the envelope with everything turned off,
 * and the cheapest exercise of the sparse path there is.
 *
 * The privacy rule matters most here: a stable 16-bit device number plus
 * page 81's 32-bit serial every 30 s is a tracking beacon, and page 82's
 * operating-time counter is monotone, surviving an identity change to
 * fingerprint a battery swap - worse for a tag, whose entire payload is an
 * identity, than for a strap.
 */
ZTEST(profiles, test_the_asset_tag_is_the_envelope_with_everything_off)
{
	struct profile_sched ps;
	struct profile_sched_cfg cfg;
	struct profile_descriptor back;
	struct profile_desc_rx rx;
	uint8_t body[8];
	uint32_t slot;
	uint32_t p82 = 0u;
	uint32_t frames = 0u;

	memset(&sched_desc, 0, sizeof(sched_desc));
	sched_desc.version = PROFILE_TLM_VERSION;
	sched_desc.schema_id = 0x01u;
	sched_desc.period = NODE_PERIOD;
	sched_desc.rf_index = PROFILE_TLM_RF_INDEX_DEFAULT;
	sched_desc.flags = PROFILE_TLM_FLAG_SPARSE;
	sched_desc.heartbeat_s = 30u;
	sched_desc.k_code = PROFILE_TLM_K_CODE_3;
	sched_desc.n_fields = 0u;

	memset(&cfg, 0, sizeof(cfg));
	cfg.desc = &sched_desc;
	cfg.common_82 = cb_common_82; /* configured, and suppressed below */

	zassert_ok(profile_sched_init(&ps, &cfg));
	profile_desc_rx_init(&rx);

	for (slot = 0u; slot < 240u; slot++) {
		enum profile_slot_kind kind = profile_sched_next(&ps, body);

		if (kind == PROFILE_SLOT_COMMON_82) {
			p82++;
		} else if (kind == PROFILE_SLOT_DESCRIPTOR) {
			frames++;
			(void)profile_desc_rx_feed(&rx, body);
		} else {
			zassert_equal(PROFILE_SLOT_IDLE, kind,
				      "a tag has no fields and therefore no "
				      "data pages, slot %u", slot);
		}
	}

	/* Two heartbeats in 240 slots, two frames each - a zero-field
	 * descriptor is the two mandatory header frames and nothing more. */
	zassert_equal(4u, frames);
	zassert_equal(2u, p82);

	/* The set still decodes, from the descriptor alone, to a node that
	 * says everything a receiver needs and publishes nothing. */
	zassert_ok(profile_desc_rx_take(&rx, &back));
	zassert_equal(0u, back.n_fields);
	zassert_equal(30u, back.heartbeat_s);
	zassert_true((back.flags & PROFILE_TLM_FLAG_SPARSE) != 0u);

	/* Now the privacy rule: page 82 suppressed. Nothing but the heartbeat
	 * remains on the air. */
	cfg.common_82 = NULL;
	zassert_ok(profile_sched_init(&ps, &cfg));
	p82 = 0u;
	frames = 0u;
	for (slot = 0u; slot < 240u; slot++) {
		enum profile_slot_kind kind = profile_sched_next(&ps, body);

		if (kind == PROFILE_SLOT_DESCRIPTOR) {
			frames++;
		} else {
			zassert_equal(PROFILE_SLOT_IDLE, kind);
		}
	}
	zassert_equal(4u, frames);
	zassert_equal(0u, p82);
}

/* ---------------------------------------------------------------------------
 * THE GATE
 *
 * A 0x60 node is discovered and its schema is decoded from the descriptor
 * alone, over a radio, with no out-of-band knowledge.
 * ---------------------------------------------------------------------------
 */

#define GATE_SLOTS 32u

/* The transmitting half. */
static uint8_t gate_air[GATE_SLOTS][8];
static uint32_t gate_n_air;

/* What the node actually packed, so the receiving half's decode is checked
 * against the truth rather than itself. Only the transmitting half writes
 * this; the receiving half reads it only in the final assertions. */
static struct {
	uint8_t page;
	uint8_t counter;
	int64_t values[NODE_FIELDS];
} gate_sent[GATE_SLOTS];
static uint32_t gate_n_sent;

static int gate_data_page(uint8_t page, uint8_t counter, uint8_t *body,
			  void *user)
{
	ARG_UNUSED(user);

	/* A little physics, so no two pages are identical and a receiver that
	 * decoded a stale body would be caught. */
	sched_values[0] = 55 + (counter % 45u);
	sched_values[1] = 29000 + counter;
	sched_values[2] = (sched_values[2] + 7 + counter) & 0xFFFFFFFFll;
	sched_values[3] = -1 * (int64_t)(counter % 400u);

	if (profile_data_encode(&sched_desc, page, counter, sched_values,
				sched_desc.n_fields, body) < 0) {
		return -EINVAL;
	}

	if (gate_n_sent < GATE_SLOTS) {
		gate_sent[gate_n_sent].page = page;
		gate_sent[gate_n_sent].counter = counter;
		memcpy(gate_sent[gate_n_sent].values, sched_values,
		       sizeof(sched_values));
		gate_n_sent++;
	}
	return 0;
}

/* The receiving half. It knows only a device number, device type and
 * transmission type - what a searching receiver recovers from a matched
 * address. */
static struct profile_desc_rx heard_rx;
static struct profile_descriptor heard_desc;
static bool heard_have_schema;
static struct {
	uint8_t body[8];
} heard_data[GATE_SLOTS];
static uint32_t heard_n_data;
static uint32_t heard_n_frames;

static void gate_on_rx(uint8_t ch, uint8_t filter_index,
		       const struct radiant_rx_event *evt, void *user)
{
	const uint8_t *payload;

	ARG_UNUSED(ch);
	ARG_UNUSED(filter_index);
	ARG_UNUSED(user);

	if (evt->status != RADIANT_RADIO_STATUS_OK || evt->body_len < 12u) {
		return;
	}

	/* The search format's address is [A6 C5 dl], so the body carries
	 * [dh][dt][tt][ctrl] before the eight payload bytes - the recovery a
	 * receiver that has just found this node has to do. */
	payload = &evt->body[4];
	heard_n_frames++;

	if (payload[0] == PROFILE_TLM_PAGE_DESCRIPTOR) {
		(void)profile_desc_rx_feed(&heard_rx, payload);
		return;
	}
	if (payload[0] >= PROFILE_TLM_PAGE_DATA_FIRST &&
	    payload[0] <= PROFILE_TLM_PAGE_DATA_LAST &&
	    heard_n_data < GATE_SLOTS) {
		memcpy(heard_data[heard_n_data].body, payload, 8);
		heard_n_data++;
	}
}

static void gate_on_tx(uint8_t ch, const struct radiant_tx_event *evt,
		       void *user)
{
	ARG_UNUSED(ch);
	ARG_UNUSED(evt);
	ARG_UNUSED(user);
}

static const struct radiant_sched_cbs gate_cbs = {
	.rx = gate_on_rx,
	.tx = gate_on_tx,
	.armed = NULL,
	.done = NULL,
};

ZTEST(profiles, test_gate_a_0x60_node_is_discovered_and_decoded_from_the_air)
{
	struct profile_sched ps;
	struct profile_sched_cfg cfg;
	struct radiant_channel_id id = {
		.device_number = NODE_DEVNUM,
		.device_type = PROFILE_TLM_DEVICE_TYPE,
		.trans_type = PROFILE_TLM_TRANS_TYPE,
	};
	struct radiant_rx_filter search_filter;
	static uint8_t tx_body[10];
	uint8_t tx_addr[RADIANT_RADIO_ADDR_MAX];
	radiant_time_t t;
	uint32_t slot;
	uint32_t i;
	uint32_t descriptor_frames = 0u;
	uint32_t common_pages = 0u;

	/* ------------------------------------------------------------------
	 * The node
	 * ------------------------------------------------------------------
	 */
	build_node_descriptor(&sched_desc);
	memset(sched_values, 0, sizeof(sched_values));
	sched_cfg_init(&cfg);
	cfg.data_page = gate_data_page;
	zassert_ok(profile_sched_init(&ps, &cfg));

	/* Join mid-stream: spin the node to message 100 of its cycle, so the
	 * 32 slots that reach the air straddle a cycle boundary, carrying
	 * common pages, the descriptor set and data pages on both sides - what
	 * a receiver switching on at an arbitrary moment actually sees. The
	 * spin transmits nothing, so counters are zeroed after it. */
	{
		uint8_t scratch[8];

		while (profile_sched_m(&ps) != 100u) {
			(void)profile_sched_next(&ps, scratch);
		}
	}
	gate_n_air = 0u;
	gate_n_sent = 0u;

	/* Phase 1: transmit, through radiant_sched.c, against the mock radio.
	 * No new scheduler API: a page-rotation master posts a transmit and
	 * posts the next one when the last has gone out, radiant_sched.h's
	 * documented path. */
	fake_radio_reset();
	zassert_ok(radiant_sched_init(&gate_cbs, NULL));
	zassert_ok(radiant_radio_init(radiant_sched_radio_cbs(), NULL));
	zassert_ok(radiant_radio_enable());

	zassert_equal(5, radiant_frame_addr(RADIANT_FRAME_CFG_TRACKING,
					    radiant_net_addr_ant_plus, &id,
					    tx_addr, sizeof(tx_addr)));

	t = radiant_radio_now() + 20000u;
	for (slot = 0u; slot < GATE_SLOTS; slot++) {
		struct radiant_sched_tx tx;
		uint8_t payload[8];
		enum profile_slot_kind kind;

		kind = profile_sched_next(&ps, payload);
		if (kind == PROFILE_SLOT_DESCRIPTOR) {
			descriptor_frames++;
		} else if (kind == PROFILE_SLOT_COMMON_80 ||
			   kind == PROFILE_SLOT_COMMON_81) {
			common_pages++;
		} else if (kind == PROFILE_SLOT_IDLE) {
			/* Sparse mode's decline-to-transmit, expressed as not
			 * posting. This node is periodic so it never happens;
			 * the branch is here because "nothing" is the whole
			 * of what a sparse node needs from the scheduler. */
			t += FAKE_RADIO_ANT_PERIOD_US;
			continue;
		}

		memcpy(gate_air[gate_n_air], payload, 8);
		gate_n_air++;

		tx_body[0] = PROFILE_TLM_TRANS_TYPE;
		tx_body[1] = RADIANT_CTRL_BROADCAST;
		memcpy(&tx_body[2], payload, 8);

		memset(&tx, 0, sizeof(tx));
		tx.fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
		tx.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
		memcpy(tx.addr, tx_addr, sizeof(tx.addr));
		tx.addr_len = 5u;
		tx.body = tx_body;
		tx.body_len = (uint8_t)sizeof(tx_body);
		tx.t_sync_at = t;

		zassert_ok(radiant_sched_request_tx(0u, &tx),
			   "slot %u refused", slot);
		zassert_ok(radiant_sched_tick());
		fake_radio_advance_to(t + 1000u);

		t += FAKE_RADIO_ANT_PERIOD_US;
	}

	zassert_equal(GATE_SLOTS, gate_n_air);
	zassert_equal(6u, descriptor_frames,
		      "the window straddles a cycle boundary and carries the "
		      "whole descriptor set");
	zassert_equal(2u, common_pages);
	zassert_equal(GATE_SLOTS, fake_radio_stats()->ev_tx,
		      "every slot reached the air");
	zassert_equal(0u, fake_radio_stats()->arms_rejected);

	radiant_sched_reset();

	/* Phase 2: discover. A receiver that has never heard of this node
	 * opens a continuous search window and is handed eight bytes at a
	 * time; everything below comes out of those bytes. */
	fake_radio_reset();
	profile_desc_rx_init(&heard_rx);
	heard_have_schema = false;
	heard_n_data = 0u;
	heard_n_frames = 0u;

	zassert_ok(radiant_sched_init(&gate_cbs, NULL));
	zassert_ok(radiant_radio_init(radiant_sched_radio_cbs(), NULL));
	zassert_ok(radiant_radio_enable());

	memset(&search_filter, 0, sizeof(search_filter));
	zassert_equal(3, radiant_frame_addr(RADIANT_FRAME_CFG_SEARCH,
					    radiant_net_addr_ant_plus, &id,
					    search_filter.addr,
					    sizeof(search_filter.addr)));
	search_filter.addr_len = 3u;

	t = radiant_radio_now() + 50000u;
	for (i = 0u; i < gate_n_air; i++) {
		uint8_t frame[FAKE_RADIO_AIR_FRAME_MAX];
		uint8_t n;

		n = fake_radio_build_ant_frame(frame, NODE_DEVNUM,
					       PROFILE_TLM_DEVICE_TYPE,
					       PROFILE_TLM_TRANS_TYPE,
					       gate_air[i]);
		zassert_equal(15u, n);
		zassert_ok(fake_radio_air_frame(
			t + (radiant_time_t)i * FAKE_RADIO_ANT_PERIOD_US,
			frame, n));
	}

	{
		struct radiant_sched_rx rx;

		memset(&rx, 0, sizeof(rx));
		rx.fmt = radiant_frame_format(RADIANT_FRAME_CFG_SEARCH);
		rx.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
		rx.filters = &search_filter;
		rx.n_filters = 1u;
		rx.t_open = t - 1000u;
		rx.t_close = RADIANT_TIME_NEVER; /* background scan */
		zassert_ok(radiant_sched_request_rx(0u, &rx));
		zassert_ok(radiant_sched_tick());

		fake_radio_advance_to(t + (radiant_time_t)gate_n_air *
						  FAKE_RADIO_ANT_PERIOD_US +
				      10000u);
		zassert_ok(radiant_sched_cancel(0u));
		radiant_sched_reset();
	}

	zassert_equal(gate_n_air, heard_n_frames,
		      "every frame that went out was heard: %u of %u",
		      heard_n_frames, gate_n_air);

	/* ------------------------------------------------------------------
	 * The gate itself.
	 * ------------------------------------------------------------------
	 */
	zassert_true(profile_desc_rx_complete(&heard_rx),
		     "the descriptor set never assembled");
	zassert_ok(profile_desc_rx_take(&heard_rx, &heard_desc),
		   "the descriptor set did not decode");
	heard_have_schema = true;

	/* Everything below was learned from page 0x00 and nothing else. */
	zassert_equal(PROFILE_TLM_VERSION, heard_desc.version);
	zassert_equal(NODE_SCHEMA, heard_desc.schema_id);
	zassert_equal(NODE_PERIOD, heard_desc.period,
		      "the receiver now knows how often to open a window");
	zassert_equal(PROFILE_TLM_RF_INDEX_DEFAULT, heard_desc.rf_index);
	zassert_equal(0u, heard_desc.flags);
	zassert_true(profile_desc_may_decode_data(&heard_desc));
	zassert_equal(NODE_FIELDS, heard_desc.n_fields);

	zassert_equal(PROFILE_TLM_TYPE_HEART_RATE, heard_desc.fields[0].type);
	zassert_equal(8, profile_bits_width(heard_desc.fields[0].width_code));
	zassert_equal(PROFILE_TLM_TYPE_TEMPERATURE, heard_desc.fields[1].type);
	zassert_equal(-2, heard_desc.fields[1].exponent);
	zassert_equal(PROFILE_TLM_TYPE_ENERGY, heard_desc.fields[2].type);
	zassert_true(heard_desc.fields[2].accumulate,
		     "an accumulating class type must announce itself as one");
	zassert_equal(PROFILE_TLM_TYPE_ACTIVE_POWER, heard_desc.fields[3].type);
	zassert_true(heard_desc.fields[3].is_signed);

	/* And the data pages decode against that schema, to the numbers the
	 * node packed - checked against gate_sent[] rather than a re-encode,
	 * since a self-consistently wrong schema would still pass otherwise. */
	zassert_true(heard_n_data >= 20u, "only %u data pages heard",
		     heard_n_data);
	for (i = 0u; i < heard_n_data; i++) {
		int64_t got[NODE_FIELDS] = { 0 };
		uint16_t present = 0u;
		int n;
		uint8_t f;

		n = profile_data_decode(&heard_desc, heard_data[i].body, got,
					heard_desc.n_fields, &present);
		zassert_equal(2, n, "data page %u decoded %d fields", i, n);
		zassert_equal(gate_sent[i].page, heard_data[i].body[0],
			      "page %u", i);
		zassert_equal(gate_sent[i].counter, heard_data[i].body[1],
			      "counter of data page %u", i);

		for (f = 0u; f < NODE_FIELDS; f++) {
			if ((present & (1u << f)) == 0u) {
				continue;
			}
			zassert_equal(gate_sent[i].values[f], got[f],
				      "data page %u field %u: %lld != %lld", i,
				      f, (long long)got[f],
				      (long long)gate_sent[i].values[f]);
		}
	}

	/* The habits every suite in here ends with. */
	zassert_true(fake_radio_is_idle(), "%s", fake_radio_busy_reason());
	zassert_equal(0u, fake_radio_viol_count(), "%s",
		      fake_radio_viol_name(fake_radio_viol(0)->code));
	zassert_true(heard_have_schema);
}

/* ---------------------------------------------------------------------------
 * Suite
 * ---------------------------------------------------------------------------
 */

static void profiles_before(void *fixture)
{
	ARG_UNUSED(fixture);
	fake_radio_reset();
	client_offers = 0u;
	client_claims = 0u;
}

ZTEST_SUITE(profiles, NULL, NULL, profiles_before, NULL, NULL);
