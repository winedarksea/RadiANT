/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_rd.h - ANT+ Running Dynamics, device type 0x1E.
 *
 * No cryptography here and no call to radiant_sec, matching profile_hr.h's
 * claim about itself. Unlike heart rate this profile has no compat client
 * attached: RadiANT's additive pages live in the 0x78 page space
 * (profile_compat.h) and this device type's page space is a different one -
 * see "The page numbers do not collide" below.
 *
 * ---------------------------------------------------------------------------
 * Three rules of this profile that are easy to get subtly wrong
 * ---------------------------------------------------------------------------
 *   NO PAGE-CHANGE TOGGLE. Byte [0] is the whole page number, all eight bits.
 *   Heart rate 0x78 reserves bit 7 as a toggle; this profile does not, and a
 *   decoder that masked byte [0] with 0x7F out of habit would work by
 *   accident on pages 0x00/0x01/0x10/0x20/0x4A (all below 0x80) until the
 *   reserved range 33-63 is allocated.
 *
 *   EVERY METRIC IS A SPLIT FIXED-POINT FIELD WITH DIFFERENT FRACTION WIDTHS.
 *   Cadence: 5 bits, 1/32 strides/min. Vertical oscillation: 2 bits, 1/4 mm.
 *   Stance time: 2 bits, 1/4%. Ground contact balance and vertical ratio: 5
 *   bits, 1/32%. Ground contact time and step length: none. This module takes
 *   and returns every value already in its wire scale (struct
 *   profile_rd_metrics), so a caller converting from float does it once,
 *   visibly.
 *
 *   THE SPLIT FIELDS ARE NOT CONTIGUOUS OR ALL LITTLE-ENDIAN-ISH. Ground
 *   contact time: three low bits in byte [4] bits 5..7, eight high bits in
 *   byte [5]. Stance time's two fractional bits split low-bit-first across a
 *   byte boundary (byte [6] bit 7, byte [7] bit 0); ground contact balance's
 *   five do the same (byte [1] bit 7, byte [2] bits 0..3). Each is a place
 *   where a plausible reading is wrong by a factor of two only sometimes.
 *
 * ---------------------------------------------------------------------------
 * The two channel configurations
 * ---------------------------------------------------------------------------
 * A standalone RD pod is one channel: device type 0x1E, RF 57 (2457 MHz),
 * period 4096 (8 Hz), transmission type 5.
 *
 * An HR-RD strap is TWO channels. The heart-rate channel (device type 0x78,
 * period 8070) carries no running dynamics. Running dynamics ride a second
 * channel, device type 0x1E again but period 8070 and transmission type 1, on
 * one of four permitted RF indices rather than ANT+ 57. Two things point at
 * it: the display's page 74 (0x4A), sent as acknowledged data on the HRM
 * channel after pairing, names the RF index/type/period the strap should open
 * the RD channel on; and the strap then advertises where it went in HRM page
 * 0x04's manufacturer-specific byte [1] as an RF enumeration
 * (PROFILE_RD_RF_ENUM_*), so a receiver that didn't become session leader can
 * still find it. 0xFF there (profile_hr.c writes PROFILE_COMMON_INVALID_U8)
 * means "not an RD strap, do not interpret" - profile_rd_rf_enum_index()
 * returns 0 for that and for every value outside the enumeration alike.
 *
 * ---------------------------------------------------------------------------
 * The page numbers do not collide with the RadiANT compat pages
 * ---------------------------------------------------------------------------
 * profile_compat.h allocates 0x70..0x72 in the HEART RATE page space; this
 * profile uses 0x00, 0x01, 0x10, 0x20 and 0x4A in the RUNNING DYNAMICS page
 * space (reserved ranges 2-15, 17-31, 33-63). Disjoint twice over - different
 * device types, no shared number - and the compat layer is not attached here.
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
 * The two channel periods, in counts of 1/32768 s. Unlike a page number, a
 * period has to match or the channel does not open at all.
 *
 *   4096   8 Hz      a standalone RD pod
 *   8070   ~4.06 Hz  the RD channel of an HR-RD strap, matching the rate of
 *                    the heart-rate channel it was opened from
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
 * The enumeration is NOT sorted, and that is not a transcription error: code 3
 * is 2475 MHz, code 4 is 2461 MHz (table 5-4 and page 0x04 agree on the
 * ordering). Code 0 means the RD channel is not open.
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
 * The invalid-value sentinels are ZERO for every measurement here, not the
 * 0xFF/0xFFFF the common pages use. A decoder reaching for the common
 * sentinel would read "no motion detected" as a real measurement.
 *
 * Step length and step count have NO invalid value: 0 mm is legitimate, and
 * step count is a rollover accumulator where 0 is one of its 128 values.
 */
#define PROFILE_RD_INVALID_CADENCE      0u
#define PROFILE_RD_INVALID_VERTICAL_OSC 0u
#define PROFILE_RD_INVALID_GCT          0u
#define PROFILE_RD_INVALID_STANCE       0u
#define PROFILE_RD_INVALID_BALANCE      0u

/* Percentage fields reserve 101..127, so a sensor clamping a computed
 * percentage must clamp to 100, not the field maximum. */
#define PROFILE_RD_PERCENT_RESERVED_MIN 101u

/* Session leader. 0 means "no leader assigned"; an HR-RD strap always reports
 * 0xFFFF, since session leadership is negotiated on its HRM channel with page
 * 74 instead. */
#define PROFILE_RD_LEADER_NONE  0x0000u
#define PROFILE_RD_LEADER_HR_RD 0xFFFFu

/* Page 16's two sentinels, which are NOT the same width: the integer part is a
 * 4-bit field whose invalid value is 0x0F, and the fraction is an 8-bit field
 * whose invalid value is 0xFF. */
#define PROFILE_RD_SPEED_INVALID_INT  0x0Fu
#define PROFILE_RD_SPEED_INVALID_FRAC 0xFFu

/* The wire scales, one per fractional field, so a caller converting from
 * physical units names the multiplier from here. */
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
 * One sensor's running dynamics, every value in its wire scale rather than SI
 * or float: four fractional widths in one profile is four chances to apply
 * the wrong multiplier, so the conversion is the caller's to do once.
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

	/* True for the RD channel of an HR-RD strap, false for a standalone
	 * pod. Follows: session-leader reports 0xFFFF rather than a negotiated
	 * id, the bidirectional-support bit is unused, and page 0x20 is not
	 * part of this channel's conversation. Period and transmission type
	 * are the caller's to set - nothing here opens a channel. */
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
 * The page encoders. Pure and public: tools/test_ant_pages.py compares per
 * page, so a failure names the page and field, not a stream offset. Each
 * writes eight bytes and returns 0, or -EINVAL.
 * ---------------------------------------------------------------------------
 */

/* Page 0x00. Values above their field width are REJECTED with -EINVAL rather
 * than masked: a masked cadence is a plausible number no receiver can tell
 * was truncated. */
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
 * The page decoders. One decoder used by both the bridge adapter and the
 * tests is one implementation of each split; two would be two.
 * ---------------------------------------------------------------------------
 */

/* Fills `m` from a page 0x00 body and, if `bidirectional` is non-NULL, reports
 * the bidirectional-support bit. -EINVAL if byte [0] is not 0x00. Fields the
 * sensor marked invalid arrive as 0, the same sentinel the encoder takes, so
 * a decode/encode round trip of an all-invalid page is exact. */
int profile_rd_decode_a(const uint8_t *body, struct profile_rd_metrics *m,
			bool *bidirectional);

/* Fills the page 0x01 half of `m` and, if non-NULL, `session_leader`. The
 * struct's page 0x00 members are left alone rather than zeroed, so a caller
 * may decode pages 0x00 and 0x01 into one struct in either order. -EINVAL if
 * byte [0] is not 0x01. */
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
 * not an RD strap". Returned for PROFILE_RD_RF_ENUM_INVALID and for every
 * value outside the enumeration alike, so "do not interpret" is the default
 * rather than something a caller has to remember.
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
 * Returns 0 if accepted, -EBUSY if a different display already holds it (a
 * sensor with a leader does not change leader on request), -EINVAL on an id
 * of 0 or an HR-RD strap (leader negotiated via page 74 instead).
 */
int profile_rd_claim_leader(struct profile_rd *rd, uint16_t leader_id);

/* The display's speed from a page 0x10 on the back channel. */
int profile_rd_apply_speed(struct profile_rd *rd, const uint8_t *body);

/*
 * Fill `body` with the next message and say what it was.
 *
 * The rotation is pages 0x00 and 0x01 alternating - the document's "~2 Hz
 * each" at 8 Hz. Common pages 80/81 arrive on profile_sched.c's own
 * 119/120-of-121 cadence, inside this profile's "at least once every 260
 * messages".
 */
enum profile_slot_kind profile_rd_next(struct profile_rd *rd, uint8_t *body);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_RD_H_ */
