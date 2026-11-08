/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_telemetry.h - device type 0x60, the RadiANT telemetry envelope.
 *
 * Provenance: docs/radiant-telemetry.md in full - section 3 (channel
 * parameters), section 4 (page map), section 5 (field kinds), section 6 (the
 * descriptor and data pages), section 7 (field-type vocabulary), section 8
 * (sparse mode), section 9 (reliable-command page layouts) and section 11
 * (what is not in v1, and the rule that every reservation is zero). This
 * project's own written specification, authored in advance of any code. No
 * ANT+ device profile document was read for it, no sdk-ant
 * source was consulted, and nothing here derives from libant.a. See
 * docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * What this is
 * ---------------------------------------------------------------------------
 * The envelope, not a sensor. A node publishes typed values against a schema
 * it broadcasts itself, and a receiver that has never heard of the node
 * decodes them from that broadcast alone - no registry lookup, no
 * back-channel, no out-of-band knowledge beyond "device type 0x60 means read
 * page 0x00 as a descriptor".
 *
 * Three things live here: struct profile_descriptor with its encoder/decoder
 * for page 0x00's frame set; struct profile_desc_rx, the receiver-side
 * accumulator that assembles a frame set arriving one per slot, in any order,
 * with holes; and data-page encode/decode, which is the bit packer plus the
 * descriptor's offsets and nothing else.
 *
 * ---------------------------------------------------------------------------
 * What this deliberately does NOT do, and why
 * ---------------------------------------------------------------------------
 *   - NO SECURITY ON THE PLAIN PATH. profile_desc_encode() sets no transform
 *     bit, computes no MAC and emits no descriptor authentication frame;
 *     every reserved field is zero (section 11). A transform bit is refused
 *     there, because section 6 makes the authentication frame mandatory
 *     alongside one. The SEALED path - profile_desc_encode_auth() and the
 *     profile_desc_auth_*() receiver half, near the end of this file - is a
 *     separate set of functions, so a node that never seals a descriptor
 *     never references them and --gc-sections drops them.
 *   - NO COMMAND STATE MACHINE. The page 0x10/0x11 byte layouts are here
 *     since section 9 fixes them; the idempotency rule, accept window, tag
 *     computation and rate limit are in profile_command.h, which needs a key
 *     and per-node state this file doesn't have. The inline tag remains a
 *     caller-supplied value this file has no opinion about deriving. What DID
 *     change: the tag now comes in two widths, since the long-range PHY can
 *     carry the longer one - see the section-9 block near the end.
 *   - NO SCHEDULE-BLOCK POLICY. The block itself (frame 1 byte [3] bits 3..0
 *     and the schedule frame, both in profile_schedule.h) is here, but this
 *     file only moves it to and from the wire; what a receiver does with a
 *     downlink window is profile_command.h's.
 */

#ifndef RADIANT_PROFILE_TELEMETRY_H_
#define RADIANT_PROFILE_TELEMETRY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The schedule block: the clock-accuracy nibble of frame 1 and the schedule
 * frame's 48 bits. Its own header carries the argument for every field. */
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
 * 119/120, cycle length 121. NOT the 65 the generic ANT+ guidance claims: a
 * node sending common pages twice as often as required spends radio energy
 * for nothing.
 * ---------------------------------------------------------------------------
 */
#define PROFILE_TLM_CYCLE        121u
#define PROFILE_TLM_SLOT_PAGE_80 119u
#define PROFILE_TLM_SLOT_PAGE_81 120u

/* ---------------------------------------------------------------------------
 * Frame 0 flags, byte [7] - docs/radiant-telemetry.md section 6
 *
 * The forward-compatibility rule:
 *   bits 7..4 are TRANSFORM flags. They change how bytes must be interpreted,
 *   so a receiver seeing one it doesn't implement must not decode the node's
 *   data pages. FAIL CLOSED.
 *   bits 3..0 are INFORMATIONAL - describe the node, not the byte layout, so
 *   an unknown one is ignored. FAIL OPEN.
 *
 * profile_desc_may_decode_data() is that rule, the only place it should be
 * written.
 * ---------------------------------------------------------------------------
 */
#define PROFILE_TLM_FLAG_X_CONF   0x80u /* data pages are AES-128-CTR ciphertext */
#define PROFILE_TLM_FLAG_X_AUTH   0x40u /* trailing byte is a spread-MAC tag */
#define PROFILE_TLM_FLAG_RSVD_5   0x20u /* descriptor-set encryption; refused */
#define PROFILE_TLM_FLAG_RSVD_4   0x10u /* v2 TESLA delayed key disclosure */
/*
 * Bit 3 was X_PRIV, "the device number rotates per epoch", now
 * reserved-must-be-zero. ADR 0006 rejected continuous per-128-second
 * rotation (its cost cliff is mid-session rotation), not rotation as such;
 * per-boot re-roll is identity Tier 2, an approved opt-in, and needs no bit
 * here - a node that re-rolls at power-up is indistinguishable on air from
 * one provisioned with that number. Announcing a privacy posture in the
 * clear is itself a leak, so this bit stays reserved.
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
 * are named here; the full table lives in the document and tools/ant_pages.py.
 * A firmware node has no use for a 40-entry table in flash.
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

/* True for a type in class 0x30-0x3F. Everything in this class must carry the
 * accumulate bit; the encoder enforces it and the decoder rejects it, cheaper
 * than every receiver discovering the same malformed node separately. */
bool profile_tlm_type_is_accumulating(uint8_t type);

/* ---------------------------------------------------------------------------
 * The MAC window and the sparse repeat count - frame 1 byte [3]
 * ---------------------------------------------------------------------------
 */

/*
 * W in {2, 4, 8} in v1. Encoding 0 means W=1 (an inline 16-bit tag), reserved
 * for the reliable-command page and refused on a data page. The window set
 * is not arbitrary: the spread MAC self-synchronises across packet loss only
 * because W divides both 256 and 65536, so a future W of 3, 5 or 6 would
 * break resynchronisation silently, one lost packet at a time.
 *
 * With no transform enabled the field is reserved and written as zero.
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
 * `page` and `bit_offset` together make the layout a schema rather than a
 * format: turning X_AUTH on shrinks the field area from 48 bits to 40 and
 * moves nothing, since the node re-publishes with a new schema id.
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
	/* What frame 1 carries. Zero with no transform and no command page.
	 * With a transform it is the epoch only when that epoch is coarse real
	 * time, zero otherwise (TODO item 4); the sealed encoder writes it from
	 * struct profile_desc_auth, and a caller leaves this zero or equal to
	 * the real epoch. */
	uint32_t epoch;

	/*
	 * The schedule block - frame 1 byte [3] bits 3..0, plus one frame.
	 *
	 * Both halves default to silence: `clock_stated` false and
	 * `has_schedule` false encode to exactly the bytes emitted before the
	 * block existed, so a node announcing nothing is indistinguishable
	 * from a pre-block one.
	 */
	bool     clock_stated;   /* the field below is a ladder code at all */
	uint8_t  clock_accuracy; /* enum profile_handoff_clk; the 5b ladder */
	bool     has_schedule;   /* a schedule frame is in the set, at index 2 */
	struct profile_schedule schedule;

	uint8_t  n_fields;
	struct profile_field fields[PROFILE_TLM_MAX_FIELDS];

	/*
	 * Receiver-side only, never encoded: true when the set this came from
	 * carried a descriptor authentication frame that verified. Only
	 * profile_desc_rx_take_auth() sets it; every other path leaves it
	 * false. profile_desc_may_decode_data() reads it.
	 */
	bool     authenticated;
};

/* Bits of field area a data page has: 48, or 40 when X_AUTH claims byte [7]. */
uint16_t profile_desc_field_area_bits(const struct profile_descriptor *d);

/*
 * The clock-accuracy ceiling this descriptor announces, in ppm, or 0 for a
 * node announcing nothing - the value that leaves radiant_channel_guard_us()
 * unchanged.
 *
 * A receiver hands this to radiant_channel_clock_accuracy_set();
 * profile_sched_apply_clock() is the one-call version that does both.
 */
uint16_t profile_desc_clock_ppm(const struct profile_descriptor *d);

/*
 * The forward-compatibility rule, in one function.
 *
 *   - bit 5 or bit 4 set (reserved transforms, implemented by nothing): false;
 *   - X_CONF or X_AUTH set: true only when `authenticated` - the descriptor
 *     came through profile_desc_rx_take_auth() and its authentication frame
 *     verified;
 *   - no transform bit: true.
 *
 * True here is about the SCHEMA, not the packets. With a transform on, the
 * data bytes handed to profile_data_decode() must have come through
 * radiant_sec's receive path - decrypted, and carrying a verdict - and gating
 * each packet on that verdict (docs/device-profiles.md 5.1's rule for 0x61:
 * an unverified reading is no reading) is still the caller's job.
 *
 * A receiver getting false has still learned the node's identity, period, RF
 * index and schema - fail closed means "do not decode", not "do not listen".
 */
bool profile_desc_may_decode_data(const struct profile_descriptor *d);

/*
 * How many frames the set has: 2 + a schedule frame if there is one +
 * n_fields + the authentication frame when any transform bit is set.
 *
 * The schedule frame is fixed at index 2, before the fields, so the
 * authentication frame keeps the last slot section 6 promises it, and a
 * receiver joining mid-rotation gets the node's timing before its schema.
 * With a transform on, the field budget is therefore 13, or 12 with a
 * schedule frame.
 */
uint8_t profile_desc_frame_count(const struct profile_descriptor *d);

/* The index of the schedule frame in the set, or -ENOENT when the node sends
 * none. Always 2 when present: the authentication frame takes the LAST
 * slot, so it never displaces the schedule frame. */
int profile_desc_schedule_index(const struct profile_descriptor *d);

/*
 * Distinct data pages the schema uses, ascending, deduplicated - the node's
 * page rotation, derived from the schema rather than configured alongside it.
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
 *   -ENOTSUP  a transform bit is set: this function never emits the
 *             authentication frame, so it never announces a transform.
 *             profile_desc_encode_auth() is the sealed path.
 *   -ENOSPC   cap_frames too small
 *
 * Every reserved bit in the output is zero, per section 11.
 */
int profile_desc_encode(const struct profile_descriptor *d, uint8_t *frames,
			uint8_t cap_frames);

/*
 * Decode a complete, in-order frame set: given nothing but n_frames * 8
 * bytes heard on the air, produce the schema.
 *
 * Returns 0, or -EPROTO for a malformed set, or -ENOTSUP for a version this
 * build does not implement. A transform bit is NOT an error here - the
 * descriptor still decodes WITHOUT A KEY, so a receiver learns identity,
 * period and RF; the last frame is then the authentication frame, which this
 * function skips, and profile_desc_may_decode_data() refuses the data pages
 * because `authenticated` stays false. X_AUTH with the sparse flag is
 * -EPROTO: no encoder emits it.
 */
int profile_desc_decode(const uint8_t *frames, uint8_t n_frames,
			struct profile_descriptor *out);

/* ---------------------------------------------------------------------------
 * The descriptor authentication frame - docs/radiant-telemetry.md section 6,
 * docs/radiant-security.md 5.3
 *
 * The last frame of a set whose frame 0 announces X_CONF or X_AUTH:
 *
 *   K_auth = KDF(K_root, "auth", epoch, base_devnum)  (the spread MAC's key)
 *   msg    = nonce_block(epoch, devnum, counter = schema_id, dom = 0x03)
 *            || bytes [2..7] of frames 0..n-2, as on air
 *   tag    = CMAC(K_auth, msg)[0..5]  ->  the last frame's bytes [2..7]
 *
 * `epoch` is the REAL epoch even when frame 1 shows zero; that is what lets
 * a keyed receiver recover the epoch by trial verification
 * (profile_desc_auth_find_epoch()) while a hostless node stops broadcasting
 * its boot counter (TODO item 4). `devnum` is the on-air device number, as
 * in the nonce; `base_devnum` is the provisioned one, as in the KDF - the
 * same split radiant_sec.c's derive_keys() and nonce use. Domain 0x03 keeps
 * this MAC apart from the spread MAC, which uses the same K_auth.
 *
 * Every function here needs CONFIG_RADIANT_SEC for the MAC and returns
 * -ENOTSUP without it. Only a sealed node or a keyed receiver references
 * them; a standard device links none of this.
 *
 * THE LOCK. The software AES keeps one key schedule, so every call here must
 * run under the same lock as every other radiant_sec call (api_lock in the
 * applications) - the hazard of TODO item 8.
 * ---------------------------------------------------------------------------
 */

struct radiant_sec_key;

#define PROFILE_TLM_AUTH_TAG_LEN 6u

/* How far forward a receiver searches for an epoch frame 1 does not carry.
 * Forward only: see profile_desc_auth_find_epoch(). */
#define PROFILE_TLM_AUTH_EPOCH_SPAN 16u

struct profile_desc_auth {
	const struct radiant_sec_key *k_root;
	uint16_t devnum;        /* on air - the nonce's */
	uint16_t base_devnum;   /* provisioned - the KDF's */
	uint32_t epoch;         /* the real epoch */
	/* The 0xF3 "coarse real time" flag: frame 1 carries the epoch only
	 * when this is set, zero otherwise. Ignored by the receiver. */
	bool     epoch_is_time;
};

/*
 * The six tag bytes over a set's first n_frames - 1 frames, as encoded.
 * Frame 0 byte [3] is the schema id the nonce carries. The key is derived on
 * every call and destroyed before returning, like profile_cmd_tag().
 *
 * Returns 0, -EINVAL, -EACCES (no root key), -EIO, or -ENOTSUP without
 * CONFIG_RADIANT_SEC. Never 0 with an untouched buffer.
 */
int profile_desc_auth_tag(const struct profile_desc_auth *a,
			  const uint8_t *frames, uint8_t n_frames,
			  uint8_t tag[PROFILE_TLM_AUTH_TAG_LEN]);

/*
 * The sealed encoder: the plain set plus the authentication frame.
 *
 * Refuses, beyond everything profile_desc_encode() refuses:
 *   -ENOTSUP  reserved transform bit 5 or 4;
 *   -EINVAL   no transform bit at all (use profile_desc_encode());
 *   -EINVAL   X_AUTH with the sparse flag - the spread tag lags one window,
 *             so a sparse burst never carries its own tag (TODO item 7);
 *   -EINVAL   d->epoch neither 0 nor a->epoch - the two would silently
 *             disagree about what frame 1 says;
 *   -EINVAL   more than 16 frames: 13 fields, or 12 with a schedule frame.
 *
 * Frame 1 bytes [4..7] are a->epoch when a->epoch_is_time, zero otherwise.
 * Returns the frame count. On a MAC failure the frames are zeroed and the
 * error returned (-EACCES, -EIO, -ENOTSUP).
 */
int profile_desc_encode_auth(const struct profile_descriptor *d,
			     const struct profile_desc_auth *a, uint8_t *frames,
			     uint8_t cap_frames);

/*
 * Verify a complete set's authentication frame under a->epoch. Constant-time
 * compare. Returns 0, -EBADMSG for a tag that does not match, or the
 * profile_desc_auth_tag() errors.
 */
int profile_desc_auth_verify(const uint8_t *frames, uint8_t n_frames,
			     const struct profile_desc_auth *a);

/*
 * Find the epoch a sealed set was MAC'd under. `a_template` supplies the key
 * and both device numbers; its epoch is ignored.
 *
 *   - frame 1 carries a non-zero epoch: verify under exactly that one, and
 *     refuse it below `from_epoch` - the replay guard;
 *   - frame 1 carries zero: try from_epoch .. from_epoch + span, in order.
 *
 * NEVER BACKWARDS. That is what stops a captured old descriptor being
 * replayed against a receiver that already knows a newer epoch. It does NOT
 * stop a replay against a receiver that knows no epoch at all (from_epoch 0
 * finds whatever old epoch the recording was made under) - docs/
 * radiant-security.md 7.6.
 *
 * A trial is one KDF and one CMAC over at most 106 bytes (the 16-byte nonce
 * block and fifteen 6-byte bodies, seven AES blocks), so the worst case is
 * span + 1 of each. Returns 0 with *out set, -EBADMSG when nothing in
 * range verifies (or the frame-1 epoch is below from_epoch), or the
 * profile_desc_auth_tag() errors.
 */
int profile_desc_auth_find_epoch(const uint8_t *frames, uint8_t n_frames,
				 const struct profile_desc_auth *a_template,
				 uint32_t from_epoch, uint32_t span,
				 uint32_t *out);

/* ---------------------------------------------------------------------------
 * Receiver-side assembly
 *
 * A receiver gets frames, one per slot, starting wherever it happened to
 * join. Byte [1] carries (index << 4) | (count - 1), enough to assemble the
 * set with no ordering assumption and no timer.
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
 * carries a schema id different from the one being assembled - the
 * cache-invalidation the schema id exists for.
 *
 * Returns 1 when the set is now complete, 0 when it is not, -EPROTO for a
 * frame that cannot belong to any set.
 */
int profile_desc_rx_feed(struct profile_desc_rx *rx, const uint8_t *body);

bool profile_desc_rx_complete(const struct profile_desc_rx *rx);

/* Decode the assembled set. -EAGAIN while it is still incomplete. Never sets
 * `authenticated`; profile_desc_rx_take_auth() is the verified version. */
int profile_desc_rx_take(const struct profile_desc_rx *rx,
			 struct profile_descriptor *out);

/*
 * The verified take: decode, then, for a set announcing X_CONF or X_AUTH,
 * recover its epoch with profile_desc_auth_find_epoch() and set
 * out->authenticated.
 *
 * Returns 0 with *epoch set for a verified set; 0 with *epoch untouched and
 * `authenticated` false for a set with no transform (nothing to verify, and
 * profile_desc_may_decode_data() needs nothing); -EBADMSG when the
 * authentication frame does not verify - and `out` still holds the
 * unauthenticated schema, so the receiver keeps the node's identity while
 * profile_desc_may_decode_data() refuses its data. Decode errors as
 * profile_desc_rx_take().
 */
int profile_desc_rx_take_auth(const struct profile_desc_rx *rx,
			      const struct profile_desc_auth *a_template,
			      uint32_t from_epoch, struct profile_descriptor *out,
			      uint32_t *epoch);

/* ---------------------------------------------------------------------------
 * The descriptor-set collapse - ADR 0007
 *
 * The largest battery item the long-range PHY buys is the length extension it
 * permits, not the PHY itself.
 *
 * A descriptor set is one 8-byte frame per header, per schedule block and per
 * field, and every frame is a separate transmission - a separate HFXO start
 * and ramp-up. On a coin cell those dominate whether the frame is 8 bytes or
 * 40, so N frames cost N wakes. It also cuts mid-stream join: a receiver that
 * hears one long frame has the whole set instead of one N-th of it.
 *
 * Nothing about the 8-byte frames changes. Each descriptor frame already
 * carries its own page number and (index << 4) | (count - 1) byte, so a set
 * of them is self-describing when concatenated: a receiver splits a long
 * payload into 8-byte chunks and feeds each to profile_desc_rx_feed(), the
 * same function a 1 M receiver has always used.
 *
 * How complete the collapse is: one long-range payload is at most
 * RADIANT_FRAME_PAYLOAD_LR_MAX = 36 bytes, so four 8-byte frames fit per
 * transmission:
 *
 *     asset tag, no fields          2 frames  -> 1 wake   (was 2)
 *     2 fields                      4 frames  -> 1 wake   (was 4)
 *     2 fields + schedule block     5 frames  -> 2 wakes  (was 5)
 *     8 fields                     10 frames  -> 3 wakes  (was 10)
 *     14 fields + schedule block   17 frames  -> refused, as it always was
 *
 * So the collapse is total up to four frames and partial beyond it - ten
 * wakes becomes three, not one, for an eight-field sparse node. Ten becoming
 * one would need a ~76-byte body, ~5.4 ms of airtime at S=8, failing the 25%
 * duty bound at any period under 22 ms.
 * ---------------------------------------------------------------------------
 */

/* Whole 8-byte descriptor frames that fit in one payload of `payload_max`
 * bytes. Never partial: a split frame would need reassembling across
 * transmissions, duplicating the accumulator that already exists one level
 * up. */
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
 * ADR 0007's duty bound enforced for the descriptor path: the longest
 * payload this set will emit, at the schedule block's rate, against frame
 * 0's period. -EINVAL when it does not fit.
 *
 * Separate from profile_desc_long_payload() so the refusal cannot be skipped
 * by a caller wanting only one frame - it is called by the payload accessor
 * too, but being public lets a caller find out before building anything.
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
 * The receiver's half: split one long payload into 8-byte descriptor frames
 * and feed each to profile_desc_rx_feed().
 *
 * Returns 1 when the set is complete, 0 when it is not, or -EPROTO for a
 * payload that is not a whole number of frames or whose frames don't belong
 * to one set. A receiver needs no notion of "this node collapsed its
 * descriptor" - it feeds what it heard and the accumulator behaves
 * identically either way.
 */
int profile_desc_rx_feed_long(struct profile_desc_rx *rx, const uint8_t *payload,
			      uint8_t payload_len);

/* ---------------------------------------------------------------------------
 * Data pages 0x01..0x0F
 *
 *   [0]    page number
 *   [1]    event counter: the slot index within the epoch, low 8 bits
 *   [2..7] field area, 48 bits, bit offset 0 = MSB of byte [2]
 *
 * The counter is the SLOT index (docs/radiant-telemetry.md section 6): +1
 * every slot, transmitted or not, descriptor and common slots included -
 * not per application write (a master retransmits its body every slot, and
 * a counter repeated within one (epoch, device number) is keystream reuse
 * under a secured channel) and not per data page (the secured receiver
 * counts time, one per slot). Mandatory in v1 even unsecured.
 * ---------------------------------------------------------------------------
 */

/*
 * Pack every field of `page` into `body`.
 *
 * `values` is parallel to d->fields[] and n_values must equal d->n_fields;
 * entries for fields on other pages are ignored. Bytes the schema doesn't
 * claim are zero - a later schema may claim them, and a receiver mid-change
 * would read stale bytes as a number.
 *
 * Returns the number of fields packed, or -EINVAL / -ERANGE.
 */
int profile_data_encode(const struct profile_descriptor *d, uint8_t page,
			uint8_t counter, const int64_t *values, uint8_t n_values,
			uint8_t *body);

/*
 * Unpack every field the descriptor places on this body's page.
 *
 * Signed fields are sign-extended; unsigned ones are not. `present` receives
 * a bitmask over d->fields[] of the entries written, so a caller can tell
 * "this page does not carry that field" from "it carries zero".
 *
 * Returns the number of fields decoded, -EINVAL, or -EACCES when a transform
 * bit forbids decoding (profile_desc_may_decode_data()) - not a silent zero,
 * which would report plaintext-looking numbers off a ciphertext page.
 */
int profile_data_decode(const struct profile_descriptor *d, const uint8_t *body,
			int64_t *values, uint8_t n_values, uint16_t *present);

/* ---------------------------------------------------------------------------
 * The reliable-command pages - docs/radiant-telemetry.md section 9
 *
 * Still layout only, a boundary rather than a gap: idempotency, accept
 * window, tag derivation and backoff on failed verifications are in
 * profile_command.h, which needs a key, a clock and per-node state this file
 * doesn't have. What is here is the byte order and the two widths.
 *
 * What the response-slot phase added: the tag is no longer always two bytes.
 * The long-range PHY carries a RadiANT-authored length field, so an LR
 * control channel can put a 64-bit tag on the air where an eight-byte ANT
 * frame fits only 16 bits. The width is a function of the frame
 * configuration, never a node's preference; profile_cmd_tag_bytes() is that
 * function, and this file only moves the bytes.
 * ---------------------------------------------------------------------------
 */

/*
 * The two tag widths, and the page lengths they produce.
 *
 * The covered length is the same six bytes either way - page number,
 * sequence, command, target, argument - so the tag grows off the end and the
 * fields in front don't move. A receiver decoding a 14-byte page 0x10 with
 * an eight-byte reader gets the right command and the wrong tag, not
 * garbage.
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
 * The eight-byte forms, unchanged. `tag` is the u16 of the struct,
 * little-endian in body[6..7] - byte for byte the functions the envelope
 * phase shipped, so every existing vector still passes.
 */
int profile_command_encode(const struct profile_command *c, uint8_t *body);
int profile_command_decode(const uint8_t *body, struct profile_command *out);
int profile_command_ack_encode(const struct profile_command_ack *a, uint8_t *body);
int profile_command_ack_decode(const uint8_t *body, struct profile_command_ack *out);

/*
 * The width-carrying forms. `tag_len` is PROFILE_TLM_CMD_TAG_STD or
 * PROFILE_TLM_CMD_TAG_LR and nothing else.
 *
 * The tag is passed as bytes rather than an integer: a 64-bit tag is a
 * truncated MAC, truncation is a byte operation, and a uint64_t would put an
 * unnecessary endianness decision between the MAC and the wire. The struct's
 * `tag` field is left alone by these functions; on decode it receives the
 * low 16 bits so a standard-width caller doesn't have to reassemble them.
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
