/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_rd.h - ANT+ Running Dynamics, device type 0x1E.
 *
 * Provenance: the ANT+ Running Dynamics device profile, D00001679 Rev 1.1
 * (Garmin Canada, 2016-2018), obtained under this project's adopter login and
 * supplied by Colin on 2026-08-13. Layout tables 6-8 (page 0), 6-9 (page 1),
 * 6-10 (page 16), 6-11 (page 32) and 6-12 (page 74), the channel-parameter
 * tables 5-1 through 5-5, and the RF enumeration of table 5-4 are the source of
 * every constant and every bit position here. Field semantics are transcribed;
 * no prose is copied, per docs/decisions/0002-clean-room-policy.md, which
 * permits adopter-gated profile documents in src/profiles/ and nowhere else.
 * No sdk-ant source was consulted and nothing here derives from libant.a.
 *
 * The document is marked DEPRECATED and "provided AS IS with no support from
 * the ANT team" - which is the state of every ANT+ profile since the program
 * closed on 30 June 2025, and is why docs/device-profiles.md records the
 * revision this was written from rather than "the current revision".
 *
 * ---------------------------------------------------------------------------
 * THIS FILE CONTAINS NO CRYPTOGRAPHY AND CALLS radiant_sec NOWHERE
 * ---------------------------------------------------------------------------
 * Same claim profile_hr.h makes about itself, expressed the same way: the
 * include list reaches no radiant_core header, even transitively. Unlike heart
 * rate this profile has no compat client attached, because the RadiANT additive
 * pages are allocated in the 0x78 page space (profile_compat.h) and this device
 * type's page space is a different one - see "The page numbers do not collide"
 * below.
 *
 * ---------------------------------------------------------------------------
 * Three rules of this profile that are easy to get subtly wrong
 * ---------------------------------------------------------------------------
 *   THERE IS NO PAGE-CHANGE TOGGLE. Byte [0] is the whole page number, all
 *   eight bits of it. Heart rate 0x78 reserves bit 7 as the toggle and every
 *   page there appears under two byte values; nothing of the kind happens here,
 *   and a decoder that masked byte [0] with 0x7F out of habit would still work
 *   by accident on pages 0x00, 0x01, 0x10, 0x20 and 0x4A - all of which are
 *   below 0x80 - right up until the reserved range 33-63 is allocated.
 *
 *   EVERY METRIC IS A SPLIT FIXED-POINT FIELD, AND THE FRACTIONS ARE NOT ALL
 *   THE SAME WIDTH. Cadence carries 5 fractional bits worth 1/32 strides/min;
 *   vertical oscillation carries 2 worth 1/4 mm; stance time carries 2 worth
 *   1/4 %; ground contact balance and vertical ratio carry 5 worth 1/32 %.
 *   Ground contact time and step length carry none at all. This module
 *   therefore takes and returns every value ALREADY IN ITS WIRE SCALE - see
 *   struct profile_rd_metrics - so that a caller converting from float does it
 *   once, visibly, rather than four times with three different multipliers.
 *
 *   THE SPLIT FIELDS ARE NOT CONTIGUOUS AND NOT ALL LITTLE-ENDIAN-ISH. Ground
 *   contact time puts its THREE LOW BITS in byte [4] bits 5..7 and its eight
 *   high bits in byte [5]. Stance time's two fractional bits are split across a
 *   byte boundary, low bit first (byte [6] bit 7, then byte [7] bit 0). Ground
 *   contact balance's five fractional bits are split the same way, one bit in
 *   byte [1] bit 7 and four in byte [2] bits 0..3. Each of those is a place
 *   where a plausible reading produces a value that is wrong by a factor of
 *   two only sometimes.
 *
 * ---------------------------------------------------------------------------
 * The two channel configurations, which is the part that is not a page layout
 * ---------------------------------------------------------------------------
 * A standalone RD pod is one channel: device type 0x1E, RF 57 (2457 MHz),
 * period 4096 (8 Hz), transmission type 5.
 *
 * An HR-RD strap is TWO channels, and this is the fact that decides how a
 * receiver finds it. The strap's heart-rate channel is an ordinary ANT+ HRM
 * channel (device type 0x78, period 8070) and carries no running dynamics at
 * all. The running dynamics ride a SECOND channel, device type 0x1E again but
 * period 8070 and transmission type 1, on one of four permitted RF indices
 * rather than the ANT+ 57. Two things point at it:
 *
 *   - The display asks for it. Page 74 (0x4A) is sent as an acknowledged
 *     message on the HRM channel after pairing, naming the RF index, the
 *     device type and the period. A strap with no session leader opens the RD
 *     channel where it was told to.
 *   - The strap then advertises where it went, in the byte HRM page 0x04
 *     reserves as manufacturer-specific: byte [1] becomes an RF enumeration
 *     (PROFILE_RD_RF_ENUM_*), so a receiver that did NOT become session leader
 *     can still find the channel. That is why profile_hr.c writes
 *     PROFILE_COMMON_INVALID_U8 there and why this header exports
 *     profile_rd_rf_enum_index(): 0xFF is "not an RD strap, do not interpret",
 *     which is exactly what the RD document requires a receiver to assume of
 *     any value outside the enumeration.
 *
 * ---------------------------------------------------------------------------
 * The page numbers do not collide with the RadiANT compat pages
 * ---------------------------------------------------------------------------
 * Checked rather than assumed, because docs/device-profiles.md raised it as an
 * open question before this document was in hand. profile_compat.h allocates
 * 0x70..0x72 in the HEART RATE page space. This profile uses 0x00, 0x01, 0x10,
 * 0x20 and 0x4A in the RUNNING DYNAMICS page space, and its reserved ranges are
 * 2-15, 17-31 and 33-63 - so 0x70..0x72 is not reserved here either. The two
 * allocations are disjoint twice over: different device types, and no shared
 * number. Nothing in this file needs to defend against the compat pages, and
 * the compat layer is not attached to this profile at all.
 */

#ifndef RADIANT_PROFILE_RD_H_
#define RADIANT_PROFILE_RD_H_

#include <stdbool.h>
#include <stdint.h>

#include "profile_common.h"
#include "profile_sched.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROFILE_RD_DEVICE_TYPE 0x1Eu

/*
 * The two channel periods, in counts of 1/32768 s. Unlike a page number a
 * period is not something a receiver can skip: it has to match or the channel
 * does not open at all.
 *
 *   4096   8 Hz      a standalone RD pod
 *   8070   ~4.06 Hz  the RD channel of an HR-RD strap, which matches the rate
 *                    of the heart-rate channel it was opened from
 */
#define PROFILE_RD_PERIOD       4096u
#define PROFILE_RD_PERIOD_HR_RD 8070u

/* Transmission type. The spec fixes the LOW nibble and leaves the high nibble
 * to the optional 20-bit device-number extension, so these are the values a
 * sensor that does not extend its device number transmits. */
#define PROFILE_RD_TRANS_TYPE       0x05u
#define PROFILE_RD_TRANS_TYPE_HR_RD 0x01u

/* RF index for a standalone pod: 57, the ANT+ public frequency, spelled as an
 * offset from 2400 MHz exactly as ANT_PLUS_RF_FREQ is in tools/ant_pages.py. */
#define PROFILE_RD_RF_FREQ 57u

/*
 * The four RF indices an HR-RD strap's RD channel may use, and the enumeration
 * that names them on the wire.
 *
 * THE ENUMERATION IS NOT SORTED AND THAT IS NOT A TRANSCRIPTION ERROR: code 3
 * is 2475 MHz and code 4 is 2461 MHz. The document's own table 5-4 and its
 * restatement under page 0x04 agree with each other on that ordering, which is
 * why it is written out here as a table rather than computed. Code 0 means the
 * RD channel is not open.
 */
#define PROFILE_RD_RF_ENUM_INVALID 0u
#define PROFILE_RD_RF_ENUM_2403    1u
#define PROFILE_RD_RF_ENUM_2439    2u
#define PROFILE_RD_RF_ENUM_2475    3u
#define PROFILE_RD_RF_ENUM_2461    4u

#define PROFILE_RD_RF_2403 3u
#define PROFILE_RD_RF_2439 39u
#define PROFILE_RD_RF_2461 61u
#define PROFILE_RD_RF_2475 75u

/* The page numbers. 0x00 and 0x01 are the main pages; 0x10 and 0x20 come from
 * the display on the back channel; 0x4A rides the HRM channel, not this one. */
#define PROFILE_RD_PAGE_A              0x00u
#define PROFILE_RD_PAGE_B              0x01u
#define PROFILE_RD_PAGE_SPEED          0x10u
#define PROFILE_RD_PAGE_LEADER_REQUEST 0x20u
#define PROFILE_RD_PAGE_OPEN_CHANNEL   0x4Au

/*
 * The invalid-value sentinels, which on this profile are ZERO for every
 * measurement and not the 0xFF/0xFFFF the common pages use. A decoder that
 * reached for the common sentinel would read "no motion detected" as a real
 * measurement on every field below.
 *
 * Step length and step count have NO invalid value: 0 mm is a legitimate
 * reading and the step count is a rollover accumulator where 0 is one of the
 * 128 values it visits.
 */
#define PROFILE_RD_INVALID_CADENCE      0u
#define PROFILE_RD_INVALID_VERTICAL_OSC 0u
#define PROFILE_RD_INVALID_GCT          0u
#define PROFILE_RD_INVALID_STANCE       0u
#define PROFILE_RD_INVALID_BALANCE      0u

/* Percentage fields reserve 101..127 rather than leaving them undefined, so a
 * sensor clamping a computed percentage must clamp to 100 and not to the field
 * maximum. */
#define PROFILE_RD_PERCENT_RESERVED_MIN 101u

/* Session leader. 0 means "no leader assigned"; an HR-RD strap always reports
 * 0xFFFF, because session leadership is negotiated on its HRM channel with page
 * 74 instead of in this field. */
#define PROFILE_RD_LEADER_NONE  0x0000u
#define PROFILE_RD_LEADER_HR_RD 0xFFFFu

/* Page 16's two sentinels, which are NOT the same width: the integer part is a
 * 4-bit field whose invalid value is 0x0F, and the fraction is an 8-bit field
 * whose invalid value is 0xFF. */
#define PROFILE_RD_SPEED_INVALID_INT  0x0Fu
#define PROFILE_RD_SPEED_INVALID_FRAC 0xFFu

/* The wire scales, one per fractional field, so a caller converting from
 * physical units names the multiplier from here rather than writing 32 or 4 and
 * hoping it picked the right one. */
#define PROFILE_RD_CADENCE_PER_STRIDE_MIN 32u /* 1/32 strides/min */
#define PROFILE_RD_VERT_OSC_PER_MM        4u  /* 1/4 mm */
#define PROFILE_RD_STANCE_PER_PERCENT     4u  /* 1/4 % */
#define PROFILE_RD_BALANCE_PER_PERCENT    32u /* 1/32 % */
#define PROFILE_RD_VERT_RATIO_PER_PERCENT 32u /* 1/32 % */
#define PROFILE_RD_SPEED_PER_MPS          256u /* 1/256 m/s */

/* Field maxima, in the wire scale above. Each is the width of the packed field
 * and not a physical limit: 13 bits of quarter-millimetres is 2047.75 mm. */
#define PROFILE_RD_CADENCE_MAX   8191u /* 13 bits: 8 integer + 5 fractional */
#define PROFILE_RD_VERT_OSC_MAX  8191u /* 13 bits: 11 integer + 2 fractional */
#define PROFILE_RD_GCT_MAX       2047u /* 11 bits, whole milliseconds */
#define PROFILE_RD_STANCE_MAX    511u  /* 9 bits: 7 integer + 2 fractional */
#define PROFILE_RD_BALANCE_MAX   4095u /* 12 bits: 7 integer + 5 fractional */
#define PROFILE_RD_STEP_LEN_MAX  8191u /* 13 bits, whole millimetres */
#define PROFILE_RD_STEP_COUNT_MAX 127u /* 7 bits, and it is MEANT to roll over */
#define PROFILE_RD_SPEED_MAX     4095u /* 12 bits: 4 integer + 8 fractional */

/*
 * One sensor's running dynamics, every value in its wire scale.
 *
 * Wire scale rather than SI or float, for the reason the header comment gives:
 * four fractional widths in one profile is four chances to apply the wrong
 * multiplier, and the conversion is the caller's to do once. A node holding
 * millimetres converts to quarter-millimetres where it fills this struct, and
 * that line is the only place the number 4 appears in its source.
 */
struct profile_rd_metrics {
	uint16_t cadence_32;      /* strides/min * 32; 0 = invalid/no motion */
	uint16_t vert_osc_quarter_mm; /* mm * 4; 0 = invalid/no motion */
	uint16_t gct_ms;          /* whole ms; 0 = invalid/no motion */
	uint16_t stance_quarter;  /* % * 4; 0 = invalid/no motion */
	uint8_t  step_count;      /* rollover accumulator, 7 bits */

	uint16_t balance_32;      /* % * 32, left foot's share; 0 = invalid */
	uint16_t vert_ratio_32;   /* % * 32 */
	uint16_t step_length_mm;  /* whole mm */

	bool walking;             /* false = running */
	bool upside_down;         /* module orientation; triggers a display warning */
};

/* Everything about the node that does not change per step. */
struct profile_rd_cfg {
	/* Common pages 80 and 81, and page 82 when the node has a battery. */
	struct profile_common_id id;

	/*
	 * True for the RD channel of an HR-RD strap, false for a standalone
	 * pod. Three things follow from it and no others: the session-leader
	 * field reports 0xFFFF rather than a negotiated id, the bidirectional-
	 * support bit is not used (the document says the bit "is only used for
	 * Running Dynamics"), and page 0x20 is not part of this channel's
	 * conversation. The period and transmission type are the CALLER's to
	 * set on the channel, not this module's - nothing here opens a channel.
	 */
	bool hr_rd;

	/* Page 82 every N data slots; 0 means never. */
	uint16_t common_82_every;
	struct profile_common_battery battery;
};

struct profile_rd {
	struct profile_rd_cfg cfg;
	struct profile_sched  sched;

	struct profile_rd_metrics m;

	/* 0 until a display claims leadership with page 0x20, and forced to
	 * PROFILE_RD_LEADER_HR_RD for an HR-RD strap. */
	uint16_t session_leader;

	/* The display's speed, as page 0x10 last reported it, in 1/256 m/s.
	 * PROFILE_RD_SPEED_INVALID means nothing has been heard. */
	uint16_t leader_speed_256;
	bool     have_leader_speed;

	uint8_t rotation[2];
};

/* ---------------------------------------------------------------------------
 * The page encoders
 *
 * Pure, and public, for the reason profile_hr.h gives: tools/test_ant_pages.py
 * compares per page, so a failure names the page and the field rather than the
 * byte offset of a stream. Each writes eight bytes and returns 0, or -EINVAL.
 * ---------------------------------------------------------------------------
 */

/* Page 0x00. Values above their field width are REJECTED with -EINVAL rather
 * than masked: a masked cadence is a plausible number and a receiver has no way
 * to tell it was truncated, where a sensor that refuses to encode is a bug its
 * own author finds. */
int profile_rd_encode_a(const struct profile_rd_metrics *m, bool bidirectional,
			uint8_t *out);

/* Page 0x01. `session_leader` is the id to report; PROFILE_RD_LEADER_HR_RD on
 * an HR-RD strap and PROFILE_RD_LEADER_NONE when no display has claimed it. */
int profile_rd_encode_b(const struct profile_rd_metrics *m,
			uint16_t session_leader, uint8_t *out);

/* Page 0x10, built by the DISPLAY and sent on the back channel. `speed_256` is
 * 1/256 m/s; pass PROFILE_RD_SPEED_INVALID to report no speed. */
#define PROFILE_RD_SPEED_INVALID 0xFFFFu
int profile_rd_encode_speed(uint16_t speed_256, uint8_t *out);

/* Page 0x20, built by the DISPLAY, sent as an acknowledged message to claim
 * session leadership. -EINVAL on PROFILE_RD_LEADER_NONE, which the document
 * requires a manufacturer to verify is never transmitted. */
int profile_rd_encode_leader_request(uint16_t leader_id, uint8_t *out);

/*
 * Page 0x4A, built by the DISPLAY and sent on the HEART-RATE channel, not this
 * one. `rf_freq` is the RF index itself (3, 39, 61 or 75 - not the enumeration
 * that HRM page 0x04 carries), and `period` must be PROFILE_RD_PERIOD_HR_RD.
 * -EINVAL for any other frequency or period, because a strap that opened its RD
 * channel somewhere the display is not listening is indistinguishable from a
 * strap with no running dynamics at all.
 */
int profile_rd_encode_open_channel(uint32_t leader_id_24, uint8_t rf_freq,
				   uint16_t period, uint8_t *out);

/* ---------------------------------------------------------------------------
 * The page decoders
 *
 * A dongle is a receiver first, and every field above is split across a byte
 * boundary at least once. One decoder used by both the bridge adapter and the
 * tests is one implementation of each split; two would be two.
 * ---------------------------------------------------------------------------
 */

/* Fills `m` from a page 0x00 body and, if `bidirectional` is non-NULL, reports
 * the bidirectional-support bit. -EINVAL if byte [0] is not 0x00. Fields the
 * sensor marked invalid arrive as 0, which is the same sentinel the encoder
 * takes - so a decode/encode round trip of an all-invalid page is exact. */
int profile_rd_decode_a(const uint8_t *body, struct profile_rd_metrics *m,
			bool *bidirectional);

/* Fills the page 0x01 half of `m` and, if non-NULL, `session_leader`. The
 * struct's page 0x00 members are left ALONE rather than zeroed, so a caller may
 * decode a page 0x00 and a page 0x01 into one struct in either order and hold
 * a complete picture of the sensor. -EINVAL if byte [0] is not 0x01. */
int profile_rd_decode_b(const uint8_t *body, struct profile_rd_metrics *m,
			uint16_t *session_leader);

/* *speed_256 is PROFILE_RD_SPEED_INVALID when either half of the field carries
 * its own sentinel - the two halves have different ones and either alone
 * invalidates the value. */
int profile_rd_decode_speed(const uint8_t *body, uint16_t *speed_256);

int profile_rd_decode_leader_request(const uint8_t *body, uint16_t *leader_id);

int profile_rd_decode_open_channel(const uint8_t *body, uint32_t *leader_id_24,
				   uint8_t *rf_freq, uint16_t *period);

/* ---------------------------------------------------------------------------
 * The HR-RD linkage
 * ---------------------------------------------------------------------------
 */

/*
 * The RF index an HRM page 0x04 byte [1] enumeration names, or 0 for "this is
 * not an RD strap".
 *
 * 0 is returned for PROFILE_RD_RF_ENUM_INVALID and for every value outside the
 * enumeration, which are two different facts that a receiver must act on
 * identically: the document is explicit that byte [1] is manufacturer-specific
 * on a strap without running dynamics, so an unrecognised value must NOT be
 * interpreted. Returning 0 for both is what makes "do not interpret" the
 * default rather than something a caller has to remember.
 */
uint8_t profile_rd_rf_enum_index(uint8_t rf_enum);

/* The inverse, for a strap filling in its own page 0x04: the enumeration code
 * for an RF index, or PROFILE_RD_RF_ENUM_INVALID if the index is not one of the
 * four permitted ones. */
uint8_t profile_rd_rf_index_enum(uint8_t rf_freq);

/* ---------------------------------------------------------------------------
 * The master
 * ---------------------------------------------------------------------------
 */

int profile_rd_init(struct profile_rd *rd, const struct profile_rd_cfg *cfg);

/* Replace the metrics the next page will report. Copied, not referenced: a body
 * is built at the instant it goes out. */
void profile_rd_set_metrics(struct profile_rd *rd,
			    const struct profile_rd_metrics *m);

/* Advance the step-count accumulator by `steps`, wrapping at 128 - which is
 * what the 7-bit field does and what a display differencing two readings
 * expects. */
void profile_rd_add_steps(struct profile_rd *rd, uint8_t steps);

/*
 * A display claimed session leadership with page 0x20.
 *
 * Returns 0 if this node accepted it, -EBUSY if a different display already
 * holds it (the document's rule: a sensor with a leader does not change leader
 * on request), and -EINVAL on an id of 0 or on an HR-RD strap, whose leader is
 * negotiated with page 74 on the heart-rate channel instead.
 */
int profile_rd_claim_leader(struct profile_rd *rd, uint16_t leader_id);

/* The display's speed from a page 0x10 on the back channel. */
int profile_rd_apply_speed(struct profile_rd *rd, const uint8_t *body);

/*
 * Fill `body` with the next message and say what it was.
 *
 * The rotation is pages 0x00 and 0x01 alternating, which is the document's
 * "~2 Hz each" at the 8 Hz channel period and its "interleave page 1 with page
 * 0" on an HR-RD strap - one rotation satisfies both because both say the same
 * thing. Common pages 80 and 81 arrive on profile_sched.c's own 119/120-of-121
 * cadence, which is inside this profile's "at least once every 260 messages".
 */
enum profile_slot_kind profile_rd_next(struct profile_rd *rd, uint8_t *body);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_RD_H_ */
