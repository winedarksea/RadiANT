/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_telemetry.h - device type 0x60, the RadiANT telemetry envelope.
 *
 * Provenance: docs/radiant-telemetry.md in full - section 3 (channel
 * parameters), section 4 (page map), section 5 (field kinds), section 6 (the
 * descriptor and the data pages), section 7 (the field-type vocabulary),
 * section 8 (sparse mode), section 9 (the reliable-command page layouts) and
 * section 11 (what is not in v1, and the rule that every reservation is
 * populated with zeros). That document is this project's own written
 * specification, authored in advance of any code. No adopter-gated ANT+ device
 * profile document was read for it, no sdk-ant source was consulted, and
 * nothing here derives from libant.a. See
 * docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * What this is
 * ---------------------------------------------------------------------------
 * The envelope, not a sensor. A node publishes typed values against a schema
 * it broadcasts itself, and a receiver that has never heard of the node
 * decodes them from that broadcast alone - no registry lookup, no back-channel,
 * no out-of-band knowledge beyond "device type 0x60 means read page 0x00 as a
 * descriptor". That last sentence is the gate this file exists to pass.
 *
 * Three things live here:
 *
 *   - struct profile_descriptor, and the encoder and decoder that move it to
 *     and from the frame set of page 0x00;
 *   - struct profile_desc_rx, the receiver-side accumulator that assembles a
 *     frame set out of frames arriving one per slot, in any order, with holes;
 *   - the data-page encode and decode, which are the bit packer plus the
 *     descriptor's offsets and nothing else.
 *
 * ---------------------------------------------------------------------------
 * What this deliberately does NOT do, and why
 * ---------------------------------------------------------------------------
 *   - NO SECURITY. v1 sets no transform bit, computes no MAC, and emits no
 *     descriptor authentication frame. Every reserved field is written as
 *     zero, per section 11: "Every reservation in this document is populated
 *     with zeros in v1." The encoder REFUSES a descriptor with a transform bit
 *     set rather than emitting one it cannot authenticate - the mandatory
 *     authentication frame of section 6 has no implementation yet, and a
 *     transform announced without it is worse than no transform.
 *   - NO COMMAND STATE MACHINE. The page 0x10 / 0x11 byte layouts are here
 *     because they are layouts and section 9 fixes them; the idempotency rule,
 *     the accept window, the tag computation and the failed-verification rate
 *     limit are not, because they need a key and they belong to the phase that
 *     turns the command path on. That phase has landed and they are in
 *     profile_command.h - so this remains a boundary rather than a gap, and the
 *     inline tag is still a caller-supplied value about whose derivation this
 *     file has no opinion. What DID change here is that the tag comes in two
 *     widths now, because the long-range PHY can carry the longer one; see the
 *     section-9 block near the end of this header.
 *   - NO SCHEDULE-BLOCK POLICY. The block itself is here now - frame 1 byte [3]
 *     bits 3..0 and the schedule frame, both in profile_schedule.h - but this
 *     file only moves it to and from the wire. What a receiver DOES with a
 *     downlink window is profile_command.h's, and this file still has no
 *     opinion about it beyond decoding it correctly.
 */

#ifndef RADIANT_PROFILE_TELEMETRY_H_
#define RADIANT_PROFILE_TELEMETRY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The schedule block: the clock-accuracy nibble of frame 1 and the schedule
 * frame's 48 bits. Its own header carries the argument for every field, and it
 * inherits the clock-accuracy ladder from profile_handoff.h rather than
 * restating it. */
#include "profile_schedule.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Channel parameters - docs/radiant-telemetry.md section 3
 * ---------------------------------------------------------------------------
 */

#define PROFILE_TLM_DEVICE_TYPE 0x60u
/* Fixed, because a searching receiver matches on it. Not a node choice. */
#define PROFILE_TLM_TRANS_TYPE  0x05u
#define PROFILE_TLM_RF_INDEX_DEFAULT 57u
#define PROFILE_TLM_RF_INDEX_MAX     124u

/* The envelope version. A receiver that does not implement it must REJECT the
 * node rather than guess: the version governs the frame layout itself. */
#define PROFILE_TLM_VERSION 1u

#define PROFILE_TLM_FRAME_LEN  8u
#define PROFILE_TLM_MAX_FIELDS 14u
/* Two header frames plus up to 14 field frames. Frame index is a nibble. */
#define PROFILE_TLM_MAX_FRAMES 16u

/* ---------------------------------------------------------------------------
 * Page map - docs/radiant-telemetry.md section 4
 * ---------------------------------------------------------------------------
 */

#define PROFILE_TLM_PAGE_DESCRIPTOR  0x00u
#define PROFILE_TLM_PAGE_DATA_FIRST  0x01u
#define PROFILE_TLM_PAGE_DATA_LAST   0x0Fu
#define PROFILE_TLM_PAGE_COMMAND     0x10u
#define PROFILE_TLM_PAGE_COMMAND_ACK 0x11u
#define PROFILE_TLM_PAGE_COMMON_80   0x50u
#define PROFILE_TLM_PAGE_COMMON_81   0x51u
#define PROFILE_TLM_PAGE_COMMON_82   0x52u

/* ---------------------------------------------------------------------------
 * The interleave - docs/radiant-telemetry.md section 6
 *
 * 119 / 120, cycle length 121. NOT the 65 the generic ANT+ guidance claims and
 * not the 64 tools/ant_pages.py once used: sdk-ant's certified bicycle power
 * profile interleaves page 80 at 119 and page 81 at 120, commented "Minimum:
 * Interleave every 121 messages". A node that sends common pages twice as
 * often as the profile requires is not simulating anything and spends radio
 * energy to do it.
 * ---------------------------------------------------------------------------
 */
#define PROFILE_TLM_CYCLE        121u
#define PROFILE_TLM_SLOT_PAGE_80 119u
#define PROFILE_TLM_SLOT_PAGE_81 120u

/* ---------------------------------------------------------------------------
 * Frame 0 flags, byte [7] - docs/radiant-telemetry.md section 6
 *
 * THE FORWARD-COMPATIBILITY RULE, and it is the most important line in the
 * envelope document:
 *
 *   bits 7..4 are TRANSFORM flags. They change how the bytes must be
 *   interpreted, so a receiver that sees one it does not implement MUST NOT
 *   decode the node's data pages. FAIL CLOSED.
 *
 *   bits 3..0 are INFORMATIONAL. They describe the node, not the byte layout,
 *   so an unknown one is ignored. FAIL OPEN.
 *
 * profile_desc_may_decode_data() is that rule, and it is the only place it
 * should ever be written.
 * ---------------------------------------------------------------------------
 */
#define PROFILE_TLM_FLAG_X_CONF   0x80u /* data pages are AES-128-CTR ciphertext */
#define PROFILE_TLM_FLAG_X_AUTH   0x40u /* trailing byte is a spread-MAC tag */
#define PROFILE_TLM_FLAG_RSVD_5   0x20u /* descriptor-set encryption; refused */
#define PROFILE_TLM_FLAG_RSVD_4   0x10u /* v2 TESLA delayed key disclosure */
/*
 * Bit 3 was X_PRIV, "the device number rotates per epoch", and it is now
 * reserved-must-be-zero. ADR 0006 rejected CONTINUOUS per-128-second rotation,
 * whose cost cliff is mid-session rotation; it did not reject rotation as such,
 * and its own cost table marks first-boot-only, explicit-user-action and
 * every-power-up rotation as free "because a rotation never happens while a
 * channel is open". Per-boot re-roll is identity Tier 2, an approved opt-in,
 * and it needs NO BIT HERE - a node that re-rolls at power-up is
 * indistinguishable on air from one provisioned with that number, which is the
 * point. Announcing a privacy posture in the clear is itself a leak, so this
 * bit stays reserved rather than being reused.
 */
#define PROFILE_TLM_FLAG_RSVD_3   0x08u
#define PROFILE_TLM_FLAG_SPARSE   0x04u /* transmits on change plus a heartbeat */
#define PROFILE_TLM_FLAG_OFF_RF57 0x02u /* byte [6] is authoritative */
#define PROFILE_TLM_FLAG_LR_PHY   0x01u /* long-range PHY in use */

#define PROFILE_TLM_FLAG_TRANSFORM_MASK 0xF0u
#define PROFILE_TLM_FLAG_INFO_MASK      0x0Fu

/* ---------------------------------------------------------------------------
 * The field-type vocabulary - docs/radiant-telemetry.md section 7
 *
 * Only the class boundaries and the handful of types this file reasons about
 * are named here. The full table lives in the document and, for a host, in
 * tools/ant_pages.py: a firmware node knows the two or three types it
 * publishes and has no use for a 40-entry table in flash.
 * ---------------------------------------------------------------------------
 */
#define PROFILE_TLM_CLASS_STATE_FIRST 0x00u /* boolean and state */
#define PROFILE_TLM_CLASS_STATE_LAST  0x0Fu
#define PROFILE_TLM_CLASS_INST_FIRST  0x10u /* instantaneous scalars */
#define PROFILE_TLM_CLASS_INST_LAST   0x2Fu
#define PROFILE_TLM_CLASS_ACC_FIRST   0x30u /* accumulating quantities */
#define PROFILE_TLM_CLASS_ACC_LAST    0x3Fu
#define PROFILE_TLM_CLASS_ENUM_FIRST  0x40u
#define PROFILE_TLM_CLASS_ENUM_LAST   0x4Fu
#define PROFILE_TLM_CLASS_OPAQUE      0x50u
#define PROFILE_TLM_CLASS_VENDOR_FIRST 0xF0u

/* The handful this project publishes itself; see the document for the rest. */
#define PROFILE_TLM_TYPE_TEMPERATURE 0x10u
#define PROFILE_TLM_TYPE_HEART_RATE  0x26u
#define PROFILE_TLM_TYPE_ACTIVE_POWER 0x1Cu
#define PROFILE_TLM_TYPE_ENERGY      0x30u
#define PROFILE_TLM_TYPE_REVOLUTIONS 0x35u
#define PROFILE_TLM_TYPE_EVENT_COUNT 0x36u

/*
 * True for a type in class 0x30-0x3F.
 *
 * "Everything in this class MUST carry the accumulate bit. That is what the
 * class is for, and a descriptor that clears the bit on one of these types is
 * malformed." The encoder enforces it and the decoder rejects it, which is
 * cheaper than every receiver discovering the same malformed node separately.
 */
bool profile_tlm_type_is_accumulating(uint8_t type);

/* ---------------------------------------------------------------------------
 * The MAC window and the sparse repeat count - frame 1 byte [3]
 * ---------------------------------------------------------------------------
 */

/*
 * W in {2, 4, 8} in v1. Encoding 0 means W=1 - an inline 16-bit tag - and is
 * reserved for the reliable-command page, refused on a data page. The window
 * set is not arbitrary: the spread MAC self-synchronises across packet loss
 * only because W divides both 256 and 65536, so a byte-counter wrap and a
 * 16-bit counter wrap both land on a window boundary. A future W of 3, 5 or 6
 * would break resynchronisation silently, one lost packet at a time.
 *
 * With no transform enabled the field is reserved and is written as zero.
 */
#define PROFILE_TLM_W_CODE_RESERVED 0u
#define PROFILE_TLM_W_CODE_2        1u
#define PROFILE_TLM_W_CODE_4        2u
#define PROFILE_TLM_W_CODE_8        3u

/* Sparse repeat count k: 1x, 2x, 3x, 5x. Default 3x - code 2. */
#define PROFILE_TLM_K_CODE_1 0u
#define PROFILE_TLM_K_CODE_2 1u
#define PROFILE_TLM_K_CODE_3 2u
#define PROFILE_TLM_K_CODE_5 3u

/* Repeats for a code, or 0 for a code out of range. */
uint8_t profile_tlm_repeat_for(uint8_t k_code);

/* ---------------------------------------------------------------------------
 * The descriptor
 * ---------------------------------------------------------------------------
 */

/*
 * One field descriptor - one of frames 2..N.
 *
 * `page` and `bit_offset` together are what make the layout a SCHEMA rather
 * than a format: turning X_AUTH on shrinks the field area from 48 bits to 40
 * and moves nothing, because the node re-publishes a descriptor with a new
 * schema id and every receiver picks it up within one interleave cycle.
 */
struct profile_field {
	uint8_t id;         /* node-chosen, stable; the MQTT topic of section 1 */
	uint8_t type;       /* the vocabulary of section 7 */
	bool    accumulate; /* running total in the field's width; MEANT to wrap */
	bool    is_signed;
	uint8_t width_code; /* 0..12; see profile_bits_width() */
	int8_t  exponent;   /* value = raw * 10^exp, in the type's canonical unit */
	uint8_t page;       /* 0x01..0x0F */
	uint8_t bit_offset; /* 0..47 within that page's field area */
};

struct profile_descriptor {
	uint8_t  version;   /* PROFILE_TLM_VERSION */
	uint8_t  schema_id; /* changes whenever any field descriptor changes */
	/* Counts of 1/32768 s. 0x0000 means asynchronous, which is legal only
	 * for a sparse node - there is no such thing as an aperiodic ANT
	 * master that a tracking receiver can follow. */
	uint16_t period;
	uint8_t  rf_index;  /* 0..124, meaning 2400 + N MHz */
	uint8_t  flags;

	/* Frame 1. */
	uint8_t  heartbeat_s;   /* sparse only; zero is INVALID for a sparse node */
	uint8_t  w_code;        /* MAC window; zero while no transform is enabled */
	uint8_t  k_code;        /* sparse repeat count */
	uint32_t epoch;         /* zero only with no transform and no command page */

	/*
	 * The schedule block - frame 1 byte [3] bits 3..0, plus one frame.
	 *
	 * BOTH HALVES DEFAULT TO SILENCE, and that is the compatibility
	 * argument rather than a convenience: `clock_stated` false and
	 * `has_schedule` false encode to exactly the bytes this encoder emitted
	 * before the block existed, frame for frame, so a node that announces
	 * nothing is not merely compatible with a pre-block node - it is
	 * indistinguishable from one.
	 */
	bool     clock_stated;   /* the field below is a ladder code at all */
	uint8_t  clock_accuracy; /* enum profile_handoff_clk; the 5b ladder */
	bool     has_schedule;   /* a schedule frame is in the set, at index 2 */
	struct profile_schedule schedule;

	uint8_t  n_fields;
	struct profile_field fields[PROFILE_TLM_MAX_FIELDS];
};

/* Bits of field area a data page has: 48, or 40 when X_AUTH claims byte [7]. */
uint16_t profile_desc_field_area_bits(const struct profile_descriptor *d);

/*
 * The clock-accuracy ceiling this descriptor announces, in ppm, or 0 for a node
 * that announces nothing - which is every node built before the block existed,
 * and is the value that leaves radiant_channel_guard_us() exactly as it was.
 *
 * This is what a receiver hands to radiant_channel_clock_accuracy_set(), and
 * profile_sched_apply_clock() is the one-call version that does both.
 */
uint16_t profile_desc_clock_ppm(const struct profile_descriptor *d);

/*
 * The forward-compatibility rule, in one function.
 *
 * False when any transform bit is set, because v1 implements none of them.
 * A receiver that gets false here has still learned the node's identity, its
 * period, its RF index and its schema - it simply must not turn its data pages
 * into numbers. FAIL CLOSED means "do not decode", not "do not listen".
 */
bool profile_desc_may_decode_data(const struct profile_descriptor *d);

/*
 * How many frames the set has: 2 + a schedule frame if there is one + n_fields.
 * There is no authentication frame in v1 because there is no transform in v1;
 * when there is one, it takes the last slot and this grows by one.
 *
 * THE ORDER IS FIXED AND THE SCHEDULE FRAME IS AT INDEX 2, before the fields
 * rather than after them, so that the authentication frame keeps the last slot
 * section 6 promises it - and so that a receiver joining mid-rotation has the
 * node's timing before it has the node's schema, which is the order in which it
 * needs them.
 */
uint8_t profile_desc_frame_count(const struct profile_descriptor *d);

/* The index of the schedule frame in the set, or -ENOENT when the node sends
 * none. A constant today; a function because the authentication frame will
 * make the set's shape depend on the flags. */
int profile_desc_schedule_index(const struct profile_descriptor *d);

/*
 * Distinct data pages the schema uses, ascending, deduplicated. This is the
 * node's page rotation, derived from the schema rather than configured
 * alongside it - two places to state which pages exist is one place and one
 * drift.
 *
 * Returns the count written, or -ENOSPC.
 */
int profile_desc_data_pages(const struct profile_descriptor *d, uint8_t *pages,
			    uint8_t cap);

/*
 * Encode the whole descriptor set. `frames` receives frame_count * 8 bytes.
 *
 * Returns the number of frames, or:
 *   -EINVAL   malformed - see the validation list in profile_telemetry.c
 *   -ENOTSUP  a transform bit is set, and v1 cannot authenticate the set
 *   -ENOSPC   cap_frames too small
 *
 * Every reserved bit in the output is zero, per section 11.
 */
int profile_desc_encode(const struct profile_descriptor *d, uint8_t *frames,
			uint8_t cap_frames);

/*
 * Decode a complete, in-order frame set. The inverse of the above, and the
 * function the gate turns on: given nothing but n_frames * 8 bytes heard on
 * the air, produce the schema.
 *
 * Returns 0, or -EPROTO for a malformed set, or -ENOTSUP for a version this
 * build does not implement. A transform bit is NOT an error here - the
 * descriptor still decodes, and profile_desc_may_decode_data() is what refuses
 * the data pages.
 */
int profile_desc_decode(const uint8_t *frames, uint8_t n_frames,
			struct profile_descriptor *out);

/* ---------------------------------------------------------------------------
 * Receiver-side assembly
 *
 * A receiver does not get a frame set; it gets frames, one per slot, starting
 * wherever it happened to join. Byte [1] carries (index << 4) | (count - 1),
 * which is enough to assemble the set with no ordering assumption and no
 * timer - and to notice, from a single frame, that the count changed under it.
 * ---------------------------------------------------------------------------
 */
struct profile_desc_rx {
	uint8_t  frames[PROFILE_TLM_MAX_FRAMES][PROFILE_TLM_FRAME_LEN];
	uint16_t have;  /* bit i set: frame i present */
	uint8_t  count; /* frames in the set; 0 until the first frame arrives */
	uint8_t  schema_id;      /* frame 0's, once seen */
	bool     schema_id_seen;
	uint32_t frames_fed;
	uint32_t resets;         /* sets abandoned because the node changed */
};

void profile_desc_rx_init(struct profile_desc_rx *rx);

/*
 * Feed one 8-byte payload. Ignores anything that is not page 0x00.
 *
 * Restarts the accumulation when the frame count changes, or when frame 0
 * arrives carrying a schema id different from the one being assembled - which
 * is exactly the cache-invalidation the schema id exists for, and the reason a
 * receiver can notice a change after reading a single frame instead of the
 * whole set.
 *
 * Returns 1 when the set is now complete, 0 when it is not, -EPROTO for a
 * frame that cannot belong to any set.
 */
int profile_desc_rx_feed(struct profile_desc_rx *rx, const uint8_t *body);

bool profile_desc_rx_complete(const struct profile_desc_rx *rx);

/* Decode the assembled set. -EAGAIN while it is still incomplete. */
int profile_desc_rx_take(const struct profile_desc_rx *rx,
			 struct profile_descriptor *out);

/* ---------------------------------------------------------------------------
 * The descriptor-set collapse - ADR 0007
 *
 * THE LARGEST BATTERY ITEM THE LONG-RANGE PHY BUYS, and it is not the PHY: it
 * is the length extension the PHY permits.
 *
 * A descriptor set is one 8-byte frame per header, per schedule block and per
 * field, and every one of those frames is a separate transmission - which means
 * a separate high-frequency crystal start and a separate ramp-up. On a coin
 * cell those dominate: the HFXO start and the ramp are paid whether the frame
 * that follows is 8 bytes or 40, so N frames cost N wakes and one frame costs
 * one. It also cuts mid-stream join, because a receiver that hears one long
 * frame has the whole set instead of one N-th of it.
 *
 * NOTHING ABOUT THE 8-BYTE FRAMES CHANGES, AND THAT IS THE DESIGN. Each
 * descriptor frame already carries its own page number and its own
 * (index << 4) | (count - 1) byte, so a set of them is SELF-DESCRIBING when
 * concatenated: a receiver splits a long payload into 8-byte chunks and feeds
 * each one to profile_desc_rx_feed(), which is the same function, with the same
 * ordering rules and the same schema-id invalidation, that a receiver on 1 M
 * has always used. The codec is untouched, the wire format of each frame is
 * untouched, and a node can emit the same descriptor either way with no second
 * encoder to keep in step.
 *
 * HOW COMPLETE THE COLLAPSE IS, STATED IN ARITHMETIC RATHER THAN IN HOPE. One
 * long-range payload is at most RADIANT_FRAME_PAYLOAD_LR_MAX = 36 bytes, and
 * the chunks are whole 8-byte frames, so FOUR frames fit per transmission:
 *
 *     asset tag, no fields          2 frames  -> 1 wake   (was 2)
 *     2 fields                      4 frames  -> 1 wake   (was 4)
 *     2 fields + schedule block     5 frames  -> 2 wakes  (was 5)
 *     8 fields                     10 frames  -> 3 wakes  (was 10)
 *     14 fields + schedule block   17 frames  -> refused, as it always was
 *
 * So the collapse is TOTAL up to four frames and PARTIAL beyond it, and the
 * plan's "a sparse node with eight fields wakes ten times per heartbeat; one
 * frame is one wake" is right about the mechanism and optimistic about the
 * count - ten becomes three, not one. Ten becoming one would need a ~76-byte
 * body, which at S=8 is ~5.4 ms of airtime and fails the 25 % duty bound at any
 * period under 22 ms. The honest version of the claim is a 70 % cut in wakes for
 * the eight-field case and a complete one for the sparse node the envelope was
 * written for, which is the node that needed it most.
 * ---------------------------------------------------------------------------
 */

/* Whole 8-byte descriptor frames that fit in one payload of `payload_max`
 * bytes. Never partial: a split frame would have to be reassembled across
 * transmissions, which is the accumulator that already exists one level up and
 * would then exist twice. */
uint8_t profile_desc_frames_per_body(uint8_t payload_max);

/*
 * How many long-range transmissions this descriptor set needs, or a negative
 * errno if the set itself is malformed. 1 is the collapse working completely.
 */
int profile_desc_long_count(const struct profile_descriptor *d,
			    uint8_t payload_max);

/*
 * Refuse a collapsed set the node cannot afford.
 *
 * This is where ADR 0007's duty bound is enforced for the descriptor path: the
 * longest payload this set will emit, at the rate the schedule block announces,
 * against the period frame 0 announces. -EINVAL when it does not fit.
 *
 * SEPARATE FROM profile_desc_long_payload() SO THAT THE REFUSAL CANNOT BE
 * SKIPPED BY A CALLER THAT ONLY WANTS ONE FRAME. It is called by the payload
 * accessor as well; having it public is what lets a caller find out before it
 * has built anything.
 */
int profile_desc_long_check(const struct profile_descriptor *d,
			    uint8_t payload_max);

/*
 * Fill in the payload of long-range transmission `index`, given the set as
 * profile_desc_encode() produced it.
 *
 * Returns the number of payload bytes written - always a multiple of 8, and
 * short only on the last index - or a negative errno.
 */
int profile_desc_long_payload(const struct profile_descriptor *d,
			      const uint8_t *frames, uint8_t n_frames,
			      uint8_t payload_max, uint8_t index, uint8_t *out,
			      size_t out_len);

/*
 * The receiver's half: split one long payload into 8-byte descriptor frames and
 * feed each to profile_desc_rx_feed().
 *
 * Returns 1 when the set is complete, 0 when it is not, or -EPROTO for a
 * payload that is not a whole number of frames or whose frames do not belong to
 * one set. A receiver needs no notion of "this node collapsed its descriptor" -
 * it feeds what it heard and the accumulator behaves identically either way,
 * which is what makes a node's choice to collapse invisible above this line.
 */
int profile_desc_rx_feed_long(struct profile_desc_rx *rx, const uint8_t *payload,
			      uint8_t payload_len);

/* ---------------------------------------------------------------------------
 * Data pages 0x01..0x0F
 *
 *   [0]    page number
 *   [1]    event counter, +1 per TRANSMITTED data page, wraps at 256
 *   [2..7] field area, 48 bits, bit offset 0 = MSB of byte [2]
 *
 * The counter counts transmissions, not application writes: a master
 * retransmits its current body every slot whether or not the application
 * supplied a new one, and a counter that advanced per write would repeat
 * across retransmissions. Under a secured channel a repeated counter within
 * one (epoch, device number) is keystream reuse, which is why this is
 * mandatory in v1 even though nothing here is secured - adding it later would
 * renumber every field offset in every deployed schema.
 * ---------------------------------------------------------------------------
 */

/*
 * Pack every field of `page` into `body`.
 *
 * `values` is parallel to d->fields[] and n_values must equal d->n_fields;
 * entries for fields on other pages are ignored. Bytes the schema does not
 * claim are zero - a node must not leave stale bytes in a field area, because
 * a later schema may claim them and a receiver mid-change would read them.
 *
 * Returns the number of fields packed, or -EINVAL / -ERANGE.
 */
int profile_data_encode(const struct profile_descriptor *d, uint8_t page,
			uint8_t counter, const int64_t *values, uint8_t n_values,
			uint8_t *body);

/*
 * Unpack every field the descriptor places on this body's page.
 *
 * Signed fields are sign-extended; unsigned ones are not. `present` receives a
 * bitmask over d->fields[] of the entries written, so a caller can tell "this
 * page does not carry that field" from "it carries zero".
 *
 * Returns the number of fields decoded, -EINVAL, or -EACCES when a transform
 * bit forbids decoding (profile_desc_may_decode_data()). -EACCES rather than a
 * silent zero, because failing closed silently is how a receiver ends up
 * reporting plaintext-looking numbers off a ciphertext page.
 */
int profile_data_decode(const struct profile_descriptor *d, const uint8_t *body,
			int64_t *values, uint8_t n_values, uint16_t *present);

/* ---------------------------------------------------------------------------
 * The reliable-command pages - docs/radiant-telemetry.md section 9
 *
 * STILL LAYOUT ONLY, and that is now a boundary rather than a gap. The
 * idempotency rule, the accept window, the tag derivation and the exponential
 * backoff on failed verifications are implemented - they are in
 * profile_command.h, which is where they went because they need a key, a clock
 * and per-node state, and this file has none of the three. What is here is the
 * byte order and the two widths it comes in.
 *
 * WHAT THE RESPONSE-SLOT PHASE ADDED HERE IS ONE THING: the tag is no longer
 * always two bytes. The long-range PHY carries a RadiANT-authored length field,
 * so an LR control channel can put a 64-bit tag on the air where an eight-byte
 * ANT frame can only fit 16 bits - and section 9's "two bytes is the limit" was
 * written when no mechanism for a longer frame existed. It does now, on exactly
 * one PHY, so the width is a function of the frame configuration and never a
 * node's preference. profile_cmd_tag_bytes() is that function; this file only
 * moves the bytes.
 * ---------------------------------------------------------------------------
 */

/*
 * The two tag widths, and the page lengths they produce.
 *
 * The covered length is the same six bytes either way - page number, sequence,
 * command, target, argument - so the tag grows off the end and the fields in
 * front of it do not move. A receiver decoding a 14-byte page 0x10 with an
 * eight-byte reader gets the right command and the wrong tag rather than
 * garbage, which is the failure mode worth having.
 */
#define PROFILE_TLM_CMD_COVERED  6u  /* body[0..5]: what a tag is computed over */
#define PROFILE_TLM_CMD_TAG_STD  2u  /* an 8-byte ANT frame; section 9's limit */
#define PROFILE_TLM_CMD_TAG_LR   8u  /* an LR frame; the length extension's use */
#define PROFILE_TLM_CMD_LEN_STD  (PROFILE_TLM_CMD_COVERED + PROFILE_TLM_CMD_TAG_STD)
#define PROFILE_TLM_CMD_LEN_LR   (PROFILE_TLM_CMD_COVERED + PROFILE_TLM_CMD_TAG_LR)

#define PROFILE_TLM_TARGET_NODE 0xFFu /* target field id meaning "the node" */

/* Result codes, section 9. */
#define PROFILE_TLM_RESULT_OK          0x00u
#define PROFILE_TLM_RESULT_ALREADY     0x01u
#define PROFILE_TLM_RESULT_BAD_SEQ     0x02u
#define PROFILE_TLM_RESULT_BAD_TAG     0x03u
#define PROFILE_TLM_RESULT_UNKNOWN_CMD 0x04u
#define PROFILE_TLM_RESULT_BAD_ARG     0x05u
#define PROFILE_TLM_RESULT_BUSY        0x06u

/* Command ids, section 9. */
#define PROFILE_TLM_CMD_NOP         0x00u
#define PROFILE_TLM_CMD_SET_BOOL    0x01u
#define PROFILE_TLM_CMD_SET_LEVEL   0x02u
#define PROFILE_TLM_CMD_SET_SETPOINT 0x03u
#define PROFILE_TLM_CMD_SET_MODE    0x04u
#define PROFILE_TLM_CMD_IDENTIFY    0x05u
#define PROFILE_TLM_CMD_SEND_DESC   0x06u
#define PROFILE_TLM_CMD_REPORT_SCHEMA 0x07u

struct profile_command {
	uint8_t  seq;
	uint8_t  cmd;
	uint8_t  target; /* field id, or PROFILE_TLM_TARGET_NODE */
	uint16_t arg;    /* scaled by the target field's exponent */
	uint16_t tag;    /* caller-supplied; this file derives nothing */
};

struct profile_command_ack {
	uint8_t  seq;
	uint8_t  result;
	uint8_t  cmd;   /* echoed */
	uint16_t value; /* resulting value of the target field */
	uint16_t tag;
};

/*
 * The eight-byte forms, unchanged. `tag` is the u16 of the struct, little-
 * endian in body[6..7], and these are exactly the functions the envelope phase
 * shipped - byte for byte, so every existing vector still passes.
 */
int profile_command_encode(const struct profile_command *c, uint8_t *body);
int profile_command_decode(const uint8_t *body, struct profile_command *out);
int profile_command_ack_encode(const struct profile_command_ack *a, uint8_t *body);
int profile_command_ack_decode(const uint8_t *body, struct profile_command_ack *out);

/*
 * The width-carrying forms. `tag_len` is PROFILE_TLM_CMD_TAG_STD or
 * PROFILE_TLM_CMD_TAG_LR and nothing else - a width that is neither is a caller
 * inventing a third format, which is the thing a vocabulary field exists to
 * prevent.
 *
 * The tag is passed as BYTES rather than as an integer, and that is deliberate:
 * a 64-bit tag is a truncated MAC, truncation is a byte operation, and routing
 * it through a uint64_t would put an endianness decision between the MAC and
 * the wire where there is no reason for one. The struct's `tag` field is left
 * alone by these functions for the same reason; on the decode side it receives
 * the low 16 bits so that a caller which only wants the standard width does not
 * have to reassemble them.
 *
 * Encode returns the number of body bytes written, or -EINVAL. Decode returns
 * the tag length it found, or -EINVAL / -EPROTO for the wrong page number.
 */
int profile_command_encode_tag(const struct profile_command *c,
			       const uint8_t *tag, uint8_t tag_len,
			       uint8_t *body, size_t cap);
int profile_command_decode_tag(const uint8_t *body, uint8_t len,
			       struct profile_command *out, uint8_t *tag,
			       uint8_t tag_cap);
int profile_command_ack_encode_tag(const struct profile_command_ack *a,
				   const uint8_t *tag, uint8_t tag_len,
				   uint8_t *body, size_t cap);
int profile_command_ack_decode_tag(const uint8_t *body, uint8_t len,
				   struct profile_command_ack *out,
				   uint8_t *tag, uint8_t tag_cap);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_TELEMETRY_H_ */
