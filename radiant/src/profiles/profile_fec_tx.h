/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_fec_tx.h - ANT+ Fitness Equipment Control (FE-C), device type 0x11,
 * THE TRANSMIT HALF: the pages a treadmill sends, and the control pages a
 * controller sends it.
 *
 */

#ifndef RADIANT_PROFILE_FEC_TX_H_
#define RADIANT_PROFILE_FEC_TX_H_

#include <stdbool.h>
#include <stdint.h>

#include "profile_common.h"
#include "profile_fec.h"
#include "profile_sched.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Page numbers this file adds to profile_fec.h's two
 * ---------------------------------------------------------------------------
 * Decimal names are what the document uses and hex is what goes on the air;
 * both are spelled here because every FE-C conversation mixes them.
 */
#define PROFILE_FEC_PAGE_SETTINGS     0x11u /* page 17, General Settings */
#define PROFILE_FEC_PAGE_METABOLIC    0x12u /* page 18, General FE Metabolic */
#define PROFILE_FEC_PAGE_TREADMILL    0x13u /* page 19, Specific Treadmill Data */
#define PROFILE_FEC_PAGE_CAPABILITIES 0x36u /* page 54, FE Capabilities */
#define PROFILE_FEC_PAGE_CMD_STATUS   0x47u /* page 71, Command Status */

/* The control pages, controller -> equipment, as acknowledged data. */
#define PROFILE_FEC_PAGE_BASIC_RESISTANCE 0x30u /* page 48 */
#define PROFILE_FEC_PAGE_TARGET_POWER     0x31u /* page 49 */
#define PROFILE_FEC_PAGE_WIND_RESISTANCE  0x32u /* page 50 */
#define PROFILE_FEC_PAGE_TRACK_RESISTANCE 0x33u /* page 51 */
#define PROFILE_FEC_PAGE_USER_CONFIG      0x37u /* page 55 */
#define PROFILE_FEC_PAGE_REQUEST          0x46u /* page 70, Request Data Page */

/* ---------------------------------------------------------------------------
 * Grade and incline
 * ---------------------------------------------------------------------------
 */

/* Page 51 bytes 5-6. Grade% = raw x 0.01 - 200.00, so this is the raw value
 * that means 0.00 %. Named rather than spelled 20000 at each use, because
 * 20000 in a diff looks like a plausible number and 0.00 % does not. */
#define PROFILE_FEC_GRADE_ZERO_RAW 20000u
#define PROFILE_FEC_GRADE_INVALID  0xFFFFu

/* The representable span of page 51, in 0.01 % units: raw 0 and raw 40000. */
#define PROFILE_FEC_GRADE_MIN_CENTI_PCT (-20000)
#define PROFILE_FEC_GRADE_MAX_CENTI_PCT (20000)

/* Page 17 bytes 4-5, sint16 in 0.01 %, and NOT the same sentinel as grade's.
 * SS10.1.2.1: +-100.00 % is the stated reporting range. */
#define PROFILE_FEC_INCLINE_INVALID       ((int32_t)0x7FFF)
#define PROFILE_FEC_INCLINE_MIN_CENTI_PCT (-10000)
#define PROFILE_FEC_INCLINE_MAX_CENTI_PCT (10000)

/*
 * Page 51's raw grade -> 0.01 % units. PROFILE_FEC_GRADE_INVALID becomes 0,
 * which is the specified behaviour and not a convenience: SS8.8.4.1 says an
 * FE receiving the invalid value assumes flat ground. Returning an error and
 * leaving the treadmill at its last grade would be the FE deciding to keep
 * climbing because the controller stopped saying anything.
 *
 * Out-param rather than a return value only so the "was it the sentinel"
 * answer survives; *valid may be NULL when the caller does not care.
 */
int32_t profile_fec_grade_to_centi_pct(uint16_t raw, bool *valid);

/* The inverse, clamped into the representable span. Used by page 71's echo,
 * which must carry back the grade the controller sent. */
uint16_t profile_fec_centi_pct_to_grade(int32_t centi_pct);

/* ---------------------------------------------------------------------------
 * Page 54's capability bits - byte 7. Bit 2 is the one that matters here.
 * ---------------------------------------------------------------------------
 */
#define PROFILE_FEC_CAP_BASIC_RESISTANCE (1u << 0)
#define PROFILE_FEC_CAP_TARGET_POWER     (1u << 1)
/* SIMULATION MODE IS WHAT UNLOCKS PAGE 51. A controller reads page 54, sees
 * this bit clear, and never sends a track-resistance command - so an
 * implementation that honours page 51 without declaring this is an
 * implementation nobody will ever exercise. Declaring it without honouring
 * page 51 is the same mistake pointing the other way. */
#define PROFILE_FEC_CAP_SIMULATION       (1u << 2)

/* ---------------------------------------------------------------------------
 * Page 71's command status codes - byte 3
 * ---------------------------------------------------------------------------
 * EVERY acknowledged control page owes one of these, with the sequence number
 * the command carried. A controller that gets no page 71 reports the equipment
 * as broken rather than as ignoring it, which is a worse failure than either.
 */
#define PROFILE_FEC_CMD_STATUS_PASS          0u
#define PROFILE_FEC_CMD_STATUS_FAIL          1u
#define PROFILE_FEC_CMD_STATUS_NOT_SUPPORTED 2u
#define PROFILE_FEC_CMD_STATUS_REJECTED      3u
#define PROFILE_FEC_CMD_STATUS_PENDING       4u
#define PROFILE_FEC_CMD_STATUS_UNINITIALISED 0xFFu

/* Byte 1 of page 71 when no command has ever been received, and byte 2 when
 * there is no sequence number to report. */
#define PROFILE_FEC_CMD_NONE      0xFFu
#define PROFILE_FEC_CMD_NO_SEQ    0xFFu

/* ---------------------------------------------------------------------------
 * The encoders. Pure, out-param, 0 or -EINVAL - the same contract every other
 * page codec in this directory has, and each writes exactly eight bytes.
 * Reserved bytes are written (0xFF), never skipped, so an uninitialised body
 * cannot put stack contents on the air.
 * ---------------------------------------------------------------------------
 */

/*
 * Page 16 (0x10) General FE Data - Table 8-7, and the exact inverse of
 * profile_fec_decode_general().
 *
 * It takes profile_fec.h's DECODER struct on purpose, rather than a mirrored
 * transmit struct. The `_valid` members then mean "emit the real value" on the
 * way out and "the real value was emitted" on the way in, so a round-trip is
 * the identity and test_profile_fec_tx.c can assert exactly that. A second
 * struct would have made the round trip a field-by-field translation, which is
 * the place a scaling error hides.
 *
 * The three sentinels are written HERE from the flags, not by the caller:
 *   speed_valid == false      -> bytes 4-5 = 0xFFFF
 *   heart_rate_valid == false -> byte 6 = 0xFF
 *   distance_valid            -> capabilities BIT 2, and byte 3 goes out
 *                                unchanged either way. Distance has no invalid
 *                                value (SS8.5.2.3); zeroing byte 3 when the
 *                                bit is clear would be inventing one.
 */
int profile_fec_encode_general(const struct profile_fec_general *in,
			       uint8_t *out);

/*
 * Page 17 (0x11) General Settings - the incline REPORT, offset-recorded.
 *
 *   [0]    page number, 0x11
 *   [1..2] reserved, 0xFF
 *   [3]    cycle length, 0.01 m. On a treadmill this is STRIDE LENGTH; on a
 *          bike it is crank length, and the field is the same byte.
 *          0xFF is the not-supported sentinel.
 *   [4..5] incline, sint16 LE, 0.01 %, 0x7FFF invalid, +-100.00 % range
 *   [6]    resistance level, 0.5 % units, 0xFF invalid
 *   [7]    bits 0-3 capabilities, bits 4-7 FE state (Table 8-10)
 */
struct profile_fec_settings {
	uint8_t cycle_length_cm;  /* 0.01 m; PROFILE_COMMON_INVALID_U8 = absent */
	bool    cycle_length_valid;

	int32_t incline_centi_pct; /* -10000..+10000 */
	bool    incline_valid;     /* false emits 0x7FFF */

	uint8_t resistance_half_pct; /* 0.5 % units */
	bool    resistance_valid;

	uint8_t capabilities; /* low nibble, passed through */
	uint8_t state;        /* PROFILE_FEC_STATE_* */
	bool    lap_toggle;
};

int profile_fec_encode_settings(const struct profile_fec_settings *in,
				uint8_t *out);
int profile_fec_decode_settings(const uint8_t *body,
				struct profile_fec_settings *out);

/*
 * Page 19 (0x13) Specific Treadmill Data - offset-recorded.
 *
 *   [0]    page number, 0x13
 *   [1..3] reserved, 0xFF
 *   [4]    instantaneous cadence, STRIDES per minute, 0xFF invalid
 *   [5]    negative vertical distance, 0.1 m, u8 accumulator - the MAGNITUDE
 *          of descent, so it is unsigned and it counts UP while going down
 *   [6]    positive vertical distance, 0.1 m, u8 accumulator
 *   [7]    bits 0-1 capabilities, bits 4-7 FE state
 *
 * STRIDES, NOT STEPS, AND THE FACTOR IS TWO. One stride is two footfalls. BLE
 * RSC's cadence is steps per minute and every real client expects steps there,
 * so rsc_cadence = this x 2 - see treadmill_rsc_cadence_from_strides() in
 * apps/treadmill. One conversion helper and one test, or exactly one of the
 * three outputs this project emits will be wrong.
 */
#define PROFILE_FEC_TREADMILL_CAP_NEG_VERTICAL (1u << 0)
#define PROFILE_FEC_TREADMILL_CAP_POS_VERTICAL (1u << 1)

struct profile_fec_treadmill {
	uint8_t cadence_strides_min; /* 0xFF invalid */
	bool    cadence_valid;

	uint8_t neg_vertical_dm; /* 0.1 m accumulator, wraps, and is meant to */
	uint8_t pos_vertical_dm;
	uint8_t capabilities; /* PROFILE_FEC_TREADMILL_CAP_* */

	uint8_t state;
	bool    lap_toggle;
};

int profile_fec_encode_treadmill(const struct profile_fec_treadmill *in,
				 uint8_t *out);
int profile_fec_decode_treadmill(const uint8_t *body,
				 struct profile_fec_treadmill *out);

/*
 * Page 18 (0x12) General FE Metabolic - optional, offset-recorded.
 *
 *   [0]    page number, 0x12
 *   [1]    reserved, 0xFF
 *   [2..3] instantaneous METs, u16 LE, 0.01, 0xFFFF invalid
 *   [4..5] caloric burn rate, u16 LE, 0.1 kcal/h, 0xFFFF invalid
 *   [6]    accumulated calories, u8 accumulator, 1 kcal
 *   [7]    bit 0 calories-enabled, bits 4-7 FE state
 */
#define PROFILE_FEC_METABOLIC_CAP_CALORIES (1u << 0)

struct profile_fec_metabolic {
	uint16_t mets_centi;      /* 0.01 METs */
	bool     mets_valid;
	uint16_t burn_rate_deci;  /* 0.1 kcal/h */
	bool     burn_rate_valid;
	uint8_t  calories;        /* kcal accumulator */
	uint8_t  capabilities;
	uint8_t  state;
	bool     lap_toggle;
};

int profile_fec_encode_metabolic(const struct profile_fec_metabolic *in,
				 uint8_t *out);
int profile_fec_decode_metabolic(const uint8_t *body,
				 struct profile_fec_metabolic *out);

/*
 * Page 54 (0x36) FE Capabilities - offset-recorded.
 *
 *   [0]    page number, 0x36
 *   [1..4] reserved, 0xFF
 *   [5..6] maximum resistance, u16 LE, 1 N, 0xFFFF invalid
 *   [7]    capabilities bit field, PROFILE_FEC_CAP_*
 */
struct profile_fec_capabilities {
	uint16_t max_resistance_n;
	bool     max_resistance_valid;
	uint8_t  capabilities; /* PROFILE_FEC_CAP_* */
};

int profile_fec_encode_capabilities(const struct profile_fec_capabilities *in,
				    uint8_t *out);
int profile_fec_decode_capabilities(const uint8_t *body,
				    struct profile_fec_capabilities *out);

/*
 * Page 71 (0x47) Command Status - offset-recorded.
 *
 *   [0]    page number, 0x47
 *   [1]    last received command id (the command PAGE number), 0xFF = none
 *   [2]    sequence number of that command, 0xFF = none
 *   [3]    command status, PROFILE_FEC_CMD_STATUS_*
 *   [4..7] response data, four bytes
 *
 * THE FOUR RESPONSE BYTES ARE THE COMMAND PAGE'S OWN BYTES [4..7], POSITION
 * FOR POSITION. That is why page 51's grade, which sits at bytes 5-6 of the
 * command, comes back at bytes 5-6 of the status - the echo is not re-packed
 * and a controller reads it with the same field offsets it wrote.
 */
struct profile_fec_cmd_status {
	uint8_t last_command; /* the command page number, or PROFILE_FEC_CMD_NONE */
	uint8_t sequence;
	uint8_t status;
	uint8_t data[4]; /* bytes [4..7] of the command being answered */
};

int profile_fec_encode_cmd_status(const struct profile_fec_cmd_status *in,
				  uint8_t *out);
int profile_fec_decode_cmd_status(const uint8_t *body,
				  struct profile_fec_cmd_status *out);

/* ---------------------------------------------------------------------------
 * The control pages, controller -> equipment. Decoders only: this project is
 * the equipment, and a controller is a different product.
 * ---------------------------------------------------------------------------
 */

/* Page 48 (0x30) Basic Resistance: [7] total resistance, 0.5 % units. */
struct profile_fec_cmd_basic_resistance {
	uint8_t resistance_half_pct;
};

int profile_fec_decode_basic_resistance(
	const uint8_t *body, struct profile_fec_cmd_basic_resistance *out);

/* Page 49 (0x31) Target Power: [6..7] target power, u16 LE, 0.25 W. */
struct profile_fec_cmd_target_power {
	uint16_t power_quarter_w;
};

int profile_fec_decode_target_power(const uint8_t *body,
				    struct profile_fec_cmd_target_power *out);

/*
 * Page 50 (0x32) Wind Resistance.
 *
 *   [5] wind resistance coefficient, 0.01 kg/m, 0xFF = use default
 *   [6] wind speed, km/h, BIASED BY +127 (so 127 on the wire is 0 km/h),
 *       0xFF = use default
 *   [7] drafting factor, 0.01, 0xFF = use default
 *
 * A TREADMILL HAS NOTHING TO DO WITH ANY OF IT, and that is not a reason to
 * refuse the page: SS8.8 makes an unsupported-but-well-formed command an
 * ACKNOWLEDGED command with a page 71 saying so, not a silence. Decoded so the
 * application can answer it correctly and then discard it.
 */
struct profile_fec_cmd_wind_resistance {
	uint8_t coefficient_centi; /* 0.01 kg/m */
	bool    coefficient_valid;
	int16_t wind_speed_kph;    /* already un-biased */
	bool    wind_speed_valid;
	uint8_t drafting_centi;    /* 0.01 */
	bool    drafting_valid;
};

#define PROFILE_FEC_WIND_SPEED_BIAS 127

int profile_fec_decode_wind_resistance(
	const uint8_t *body, struct profile_fec_cmd_wind_resistance *out);

/*
 * Page 51 (0x33) Track Resistance - THE INCLINE COMMAND.
 *
 *   [1..4] reserved
 *   [5..6] grade, u16 LE, Grade% = raw x 0.01 - 200.00, 0xFFFF = assume flat
 *   [7]    coefficient of rolling resistance, 5e-5 units, 0xFF = use default
 *
 * Byte 7 IS DECODED AND MUST NOT BE ACTED ON by a treadmill (SS8.8.4.2, p.59).
 * It is here so that page 71's echo can carry the bytes back unchanged and so
 * that a reader looking for it finds this sentence instead of assuming it was
 * forgotten.
 */
struct profile_fec_cmd_track_resistance {
	int32_t grade_centi_pct; /* 0 when the controller sent the sentinel */
	bool    grade_valid;     /* false = 0xFFFF was sent, treat as flat */
	uint8_t rolling_resistance; /* 5e-5 units; a treadmill ignores this */
	bool    rolling_resistance_valid;
};

int profile_fec_decode_track_resistance(
	const uint8_t *body, struct profile_fec_cmd_track_resistance *out);

/*
 * Page 55 (0x37) User Configuration.
 *
 *   [1..2] user weight, u16 LE, 0.01 kg, 0xFFFF invalid
 *
 * Bytes [3..7] carry bicycle wheel diameter, bicycle weight and gear ratio in
 * a sub-byte packing that was not recorded with the rest of this page. They
 * are NOT decoded - see the honesty block at the top of this header. A
 * treadmill wants the user's mass (for page 18's calorie estimate and nothing
 * else) and has no wheel.
 */
struct profile_fec_cmd_user_config {
	uint16_t user_weight_10g; /* 0.01 kg */
	bool     user_weight_valid;
};

int profile_fec_decode_user_config(const uint8_t *body,
				   struct profile_fec_cmd_user_config *out);

/*
 * Page 70 (0x46) Request Data Page.
 *
 *   [5] bit 7 = answer with an acknowledged message; bits 0-6 = how many times
 *       to transmit the requested page
 *   [6] the requested page number
 *   [7] command type: 1 request data page, 2 request ANT-FS session,
 *       3 request data page from slave, 4 data page set
 */
#define PROFILE_FEC_REQUEST_CMD_DATA_PAGE       1u
#define PROFILE_FEC_REQUEST_CMD_ANTFS_SESSION   2u
#define PROFILE_FEC_REQUEST_CMD_DATA_PAGE_SLAVE 3u
#define PROFILE_FEC_REQUEST_CMD_DATA_PAGE_SET   4u

struct profile_fec_cmd_request {
	uint8_t page;         /* the page being asked for */
	uint8_t tx_count;     /* how many times to send it; 0x80 on the wire is ack */
	bool    acknowledged; /* byte 5 bit 7 */
	uint8_t command_type;
};

int profile_fec_decode_request(const uint8_t *body,
			       struct profile_fec_cmd_request *out);

/* ---------------------------------------------------------------------------
 * The master
 * ---------------------------------------------------------------------------
 *
 * THE INTERLEAVE IS A SPEC REQUIREMENT AND NOT A PREFERENCE, AND IT IS NOT THE
 * 119/120/121 CADENCE profile_sched.c IMPLEMENTS. SS10.1, p.68:
 *
 *   - page 16 twice consecutively every 4 messages, OR once every 5th
 *   - the equipment-specific page (19 for a treadmill) at least once every 5
 *   - pages 17 and 18 at least once every 20
 *   - common pages 80 and 81 as TWO CONSECUTIVE background pages every 66
 *
 * 66 is not 121, so the common-page pair CANNOT come out of profile_sched.c's
 * built-in cadence, whose 119/120-of-121 placement is hard-coded to
 * PROFILE_TLM_CYCLE. This profile therefore leaves cfg.common_80 and
 * cfg.common_81 NULL and claims those two slots through the CLIENT SEAM
 * instead, which is what the seam is for ("a client family with no descriptor
 * configures cfg.desc = NULL and gets the interleave with the descriptor slots
 * simply absent" - profile_sched.h). profile_fec_tx_next() then reports them
 * back as PROFILE_SLOT_COMMON_80/81 rather than PROFILE_SLOT_CLIENT, so a
 * caller counting common pages counts the right thing.
 *
 * THE SCHEDULE, as a five-message group. Group g, slot i in 0..4:
 *
 *   i = 0, 1   page 16                (twice consecutively, every group)
 *   i = 2, 3   page 19                (twice, so one survives a substitution)
 *   i = 4      page 17, or page 18 when metabolic is enabled - alternating
 *
 *   every 13th group (65 messages, which is <= 66) slots 3 and 4 become
 *   pages 80 and 81 instead - consecutive, as required.
 *
 * Worst-case gaps, which is the only way to check a rule stated as "at least
 * once every N": page 16 appears at least once in every 4 consecutive
 * messages; page 19 at least once in every 5; page 17 at least once in every
 * 15 (10 in the steady state, plus one displaced group); page 18 the same when
 * enabled. test_profile_fec_tx.c asserts all four over 264 messages rather
 * than trusting this paragraph.
 */

/* The group and cycle constants, named so the test can assert against the
 * same numbers the encoder uses rather than against copies of them. */
#define PROFILE_FEC_TX_GROUP_SLOTS      5u
#define PROFILE_FEC_TX_COMMON_GROUP     13u /* every 13th group = every 65 messages */
#define PROFILE_FEC_TX_COMMON_INTERVAL  (PROFILE_FEC_TX_GROUP_SLOTS * \
					 PROFILE_FEC_TX_COMMON_GROUP)

/* SS10.1's four numbers, as the assertions test_profile_fec_tx.c makes. */
#define PROFILE_FEC_TX_MAX_GAP_16       5u
#define PROFILE_FEC_TX_MAX_GAP_19       5u
#define PROFILE_FEC_TX_MAX_GAP_SETTINGS 20u

struct profile_fec_tx_cfg {
	/*
	 * Common pages 80 and 81.
	 *
	 * PAGE 82 IS DELIBERATELY ABSENT FROM THIS MASTER. Every other profile
	 * in this directory offers it behind a `common_82_every` cadence, and
	 * this one does not, for a reason worth writing down rather than
	 * rediscovering: the interleave above is a five-message group whose
	 * every slot is spoken for by SS10.1, and page 82 has nowhere to ride
	 * except the settings slot, where it would compete with pages 17 and 18
	 * for the 20-message budget those two already fill. A mains-powered
	 * treadmill has no battery to report, so the field it would fill is
	 * PROFILE_COMMON_INVALID_* anyway. A battery-powered FE that wants it
	 * should widen the group rather than borrow a slot.
	 */
	struct profile_common_id id;

	/* Table 8-8's equipment type. 19 is a treadmill; the encoder does not
	 * insist on it, because the schedule above is the treadmill-specific
	 * part and a rower using this master would only need a different
	 * equipment-specific page. */
	uint8_t equipment_type;

	/* Page 54, answered on request and never broadcast unasked. */
	struct profile_fec_capabilities caps;

	/* Emit page 18 in the settings slot, alternating with page 17. A
	 * machine that cannot estimate calories should leave this false rather
	 * than send 0xFFFF twice a minute. */
	bool metabolic;
};

struct profile_fec_tx {
	struct profile_fec_tx_cfg cfg;
	struct profile_sched      sched;

	/* The live page contents. The node writes these through the setters
	 * below and this module never reads a clock or a sensor. */
	struct profile_fec_general    general;
	struct profile_fec_settings   settings;
	struct profile_fec_treadmill  treadmill;
	struct profile_fec_metabolic  metabolic;
	struct profile_fec_cmd_status status;

	/* Transmitted messages, and the position within the five-message
	 * group. Advanced once per emitted message by profile_fec_tx_next(),
	 * never by the engine - a slot the engine declined is not a message. */
	uint32_t messages;
	uint32_t group;
	uint8_t  group_slot;

	/* Alternates the settings slot between pages 17 and 18. */
	bool settings_slot_is_metabolic;

	/* A page 70 asked for something; deliver it in the next data slot,
	 * `on_request_left` times. */
	uint8_t on_request_page;
	uint8_t on_request_left;

	/* Set when a control page has been answered and page 71 has not gone
	 * out yet. SS8.8: the status page follows the command promptly, so it
	 * pre-empts the rotation exactly once. */
	bool status_pending;

	uint8_t rotation[1];
};

int profile_fec_tx_init(struct profile_fec_tx *tx,
			const struct profile_fec_tx_cfg *cfg);

/* ---------------------------------------------------------------------------
 * What the node tells this module. All plain stores: no locks, and reached
 * only from the system workqueue and the radio callback, exactly as
 * profile_hr is (see apps/hrm_ble/src/hrm_sensor.h's context rules).
 * ---------------------------------------------------------------------------
 */

/* Page 16's fields. The struct is copied, so a caller may reuse its own. */
void profile_fec_tx_set_general(struct profile_fec_tx *tx,
				const struct profile_fec_general *g);
void profile_fec_tx_set_settings(struct profile_fec_tx *tx,
				 const struct profile_fec_settings *s);
void profile_fec_tx_set_treadmill(struct profile_fec_tx *tx,
				  const struct profile_fec_treadmill *t);
void profile_fec_tx_set_metabolic(struct profile_fec_tx *tx,
				  const struct profile_fec_metabolic *m);

/*
 * Record the answer to a control page and arm the page 71 that owes it.
 *
 * `command_page` is the page number of the command being answered,
 * `sequence` the sequence number it carried, `status` a
 * PROFILE_FEC_CMD_STATUS_* value and `data4` the command's own bytes [4..7],
 * which go back out unchanged. `data4` may be NULL, which sends four 0xFF.
 *
 * The status page pre-empts the next data slot once. It does not displace a
 * common page or a page-16 slot, so the interleave assertions above still hold
 * while commands are arriving.
 */
void profile_fec_tx_command_answered(struct profile_fec_tx *tx,
				     uint8_t command_page, uint8_t sequence,
				     uint8_t status, const uint8_t *data4);

/*
 * A page 70 arrived. Deliver `page` in the next `count` data slots.
 *
 * Returns 0, or -ENOTSUP for a page this master cannot produce on request -
 * which the caller should answer with a page 71 carrying
 * PROFILE_FEC_CMD_STATUS_NOT_SUPPORTED rather than by staying silent.
 * Supported: 54, 71, 80 and 81.
 */
int profile_fec_tx_request(struct profile_fec_tx *tx, uint8_t page,
			   uint8_t count);

/*
 * Fill `body` with the next message and say what it was.
 *
 * PROFILE_SLOT_COMMON_80 and PROFILE_SLOT_COMMON_81 are returned for the
 * client-claimed common pair, not PROFILE_SLOT_CLIENT - see the interleave
 * block above for why the pair comes through the seam at all.
 */
enum profile_slot_kind profile_fec_tx_next(struct profile_fec_tx *tx,
					   uint8_t *body);

/* The message index the next call will serve, for a test that wants to assert
 * a cadence without counting its own calls. */
uint32_t profile_fec_tx_messages(const struct profile_fec_tx *tx);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_FEC_TX_H_ */
