/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_fec_tx.c - ANT+ Fitness Equipment Control, device type 0x11, the
 * transmit half. See profile_fec_tx.h for the provenance, the honesty block
 * about how strongly each layout below is cited, the grade/incline trap and
 * the SS10.1 interleave.
 *
 * No cryptography in this file and no call to radiant_sec. No radiant header
 * and no bridge header, direct or transitive - the same claim profile_fec.c,
 * profile_hr.c and profile_power.c make about themselves, and the include list
 * is the check.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "profile_common.h"
#include "profile_fec.h"
#include "profile_fec_tx.h"
#include "profile_sched.h"

/* Byte 7's two nibbles, assembled the way profile_fec.c's decode_fe_state()
 * takes them apart: bits 0-2 of the HIGH nibble are the state and bit 3 of it
 * is the lap toggle, i.e. bit 7 of the byte. Masking the state to three bits
 * here is what stops a caller's out-of-range state silently setting the lap
 * toggle - the exact aliasing the decoder side warns about, arriving from the
 * other direction. */
static uint8_t encode_fe_state(uint8_t caps_nibble, uint8_t state,
			       bool lap_toggle)
{
	uint8_t byte7 = (uint8_t)(caps_nibble & 0x0Fu);

	byte7 |= (uint8_t)((state & 0x07u) << 4);
	if (lap_toggle) {
		byte7 |= 0x80u;
	}
	return byte7;
}

static void split_fe_state(uint8_t byte7, uint8_t *state, bool *lap_toggle)
{
	*state = (uint8_t)((byte7 >> 4) & 0x07u);
	*lap_toggle = ((byte7 & 0x80u) != 0u);
}

static void put_le16(uint8_t *out, uint16_t value)
{
	out[0] = (uint8_t)(value & 0xFFu);
	out[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static uint16_t get_le16(const uint8_t *body)
{
	return (uint16_t)body[0] | (uint16_t)((uint16_t)body[1] << 8);
}

/* ── Grade and incline ──────────────────────────────────────────────────── */

int32_t profile_fec_grade_to_centi_pct(uint16_t raw, bool *valid)
{
	if (raw == PROFILE_FEC_GRADE_INVALID) {
		if (valid != NULL) {
			*valid = false;
		}
		/* SS8.8.4.1: an FE that receives the invalid value assumes flat
		 * ground. Returning the last grade instead would leave a
		 * treadmill climbing because the controller went quiet. */
		return 0;
	}
	if (valid != NULL) {
		*valid = true;
	}
	/* The bias, and the one line in this file where a missing cast would
	 * matter: raw is unsigned and the result is routinely negative. */
	return (int32_t)raw - (int32_t)PROFILE_FEC_GRADE_ZERO_RAW;
}

uint16_t profile_fec_centi_pct_to_grade(int32_t centi_pct)
{
	if (centi_pct < PROFILE_FEC_GRADE_MIN_CENTI_PCT) {
		centi_pct = PROFILE_FEC_GRADE_MIN_CENTI_PCT;
	} else if (centi_pct > PROFILE_FEC_GRADE_MAX_CENTI_PCT) {
		centi_pct = PROFILE_FEC_GRADE_MAX_CENTI_PCT;
	}
	return (uint16_t)(centi_pct + (int32_t)PROFILE_FEC_GRADE_ZERO_RAW);
}

/* ── Page 16 (0x10) General FE Data ─────────────────────────────────────── */

int profile_fec_encode_general(const struct profile_fec_general *in,
			       uint8_t *out)
{
	uint8_t caps;

	if (in == NULL || out == NULL) {
		return -EINVAL;
	}

	out[0] = PROFILE_FEC_PAGE_GENERAL;
	/* Table 8-8: bits 5-7 are "Do Not Interpret" and go out clear, which
	 * is what the decoder masks off anyway. */
	out[1] = (uint8_t)(in->equipment_type & PROFILE_FEC_TYPE_MASK);
	out[2] = in->elapsed_time_qs;
	/* Byte 3 goes out unchanged whether or not the enable bit is set:
	 * distance has no invalid value (SS8.5.2.3), so zeroing it here would
	 * be inventing one. */
	out[3] = in->distance_m;

	put_le16(&out[4], in->speed_valid ? in->speed_mm_s
					  : PROFILE_FEC_INVALID_SPEED);
	out[6] = in->heart_rate_valid ? in->heart_rate_bpm
				      : PROFILE_FEC_INVALID_HR;

	caps = (uint8_t)(in->hr_source & 0x03u);
	if (in->distance_valid) {
		caps |= (uint8_t)(1u << 2);
	}
	if (in->speed_virtual) {
		caps |= (uint8_t)(1u << 3);
	}
	out[7] = encode_fe_state(caps, in->state, in->lap_toggle);

	return 0;
}

/* ── Page 17 (0x11) General Settings ────────────────────────────────────── */

int profile_fec_encode_settings(const struct profile_fec_settings *in,
				uint8_t *out)
{
	int32_t incline;

	if (in == NULL || out == NULL) {
		return -EINVAL;
	}
	if (in->incline_valid &&
	    (in->incline_centi_pct < PROFILE_FEC_INCLINE_MIN_CENTI_PCT ||
	     in->incline_centi_pct > PROFILE_FEC_INCLINE_MAX_CENTI_PCT)) {
		/* SS10.1.2.1 gives +-100.00 % as the field's range. Refusing is
		 * better than truncating: a truncated incline is a number a
		 * head unit will display without complaint. */
		return -EINVAL;
	}

	out[0] = PROFILE_FEC_PAGE_SETTINGS;
	out[1] = PROFILE_COMMON_INVALID_U8;
	out[2] = PROFILE_COMMON_INVALID_U8;
	out[3] = in->cycle_length_valid ? in->cycle_length_cm
				       : PROFILE_COMMON_INVALID_U8;

	incline = in->incline_valid ? in->incline_centi_pct
				    : PROFILE_FEC_INCLINE_INVALID;
	/* Two's complement into a u16, which is what the wire carries. The
	 * cast chain is explicit because a negative int32 narrowed straight to
	 * uint8_t is implementation-defined ground nobody should stand on. */
	put_le16(&out[4], (uint16_t)(int16_t)incline);

	out[6] = in->resistance_valid ? in->resistance_half_pct
				      : PROFILE_COMMON_INVALID_U8;
	out[7] = encode_fe_state(in->capabilities, in->state, in->lap_toggle);

	return 0;
}

int profile_fec_decode_settings(const uint8_t *body,
				struct profile_fec_settings *out)
{
	int16_t incline;

	if (body == NULL || out == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_FEC_PAGE_SETTINGS) {
		return -EINVAL;
	}

	out->cycle_length_cm = body[3];
	out->cycle_length_valid = (body[3] != PROFILE_COMMON_INVALID_U8);

	incline = (int16_t)get_le16(&body[4]);
	out->incline_valid = ((int32_t)incline != PROFILE_FEC_INCLINE_INVALID);
	out->incline_centi_pct = out->incline_valid ? (int32_t)incline : 0;

	out->resistance_half_pct = body[6];
	out->resistance_valid = (body[6] != PROFILE_COMMON_INVALID_U8);

	out->capabilities = (uint8_t)(body[7] & 0x0Fu);
	split_fe_state(body[7], &out->state, &out->lap_toggle);

	return 0;
}

/* ── Page 19 (0x13) Specific Treadmill Data ─────────────────────────────── */

int profile_fec_encode_treadmill(const struct profile_fec_treadmill *in,
				 uint8_t *out)
{
	if (in == NULL || out == NULL) {
		return -EINVAL;
	}

	out[0] = PROFILE_FEC_PAGE_TREADMILL;
	out[1] = PROFILE_COMMON_INVALID_U8;
	out[2] = PROFILE_COMMON_INVALID_U8;
	out[3] = PROFILE_COMMON_INVALID_U8;
	out[4] = in->cadence_valid ? in->cadence_strides_min
				   : PROFILE_FEC_INVALID_CADENCE;
	/* Both vertical distances are accumulators in 0.1 m and both are
	 * unsigned: the negative one carries the MAGNITUDE of descent, so it
	 * counts up while the belt is going down. */
	out[5] = in->neg_vertical_dm;
	out[6] = in->pos_vertical_dm;
	out[7] = encode_fe_state((uint8_t)(in->capabilities & 0x03u), in->state,
				 in->lap_toggle);

	return 0;
}

int profile_fec_decode_treadmill(const uint8_t *body,
				 struct profile_fec_treadmill *out)
{
	if (body == NULL || out == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_FEC_PAGE_TREADMILL) {
		return -EINVAL;
	}

	out->cadence_strides_min = body[4];
	out->cadence_valid = (body[4] != PROFILE_FEC_INVALID_CADENCE);
	out->neg_vertical_dm = body[5];
	out->pos_vertical_dm = body[6];
	out->capabilities = (uint8_t)(body[7] & 0x03u);
	split_fe_state(body[7], &out->state, &out->lap_toggle);

	return 0;
}

/* ── Page 18 (0x12) General FE Metabolic ────────────────────────────────── */

int profile_fec_encode_metabolic(const struct profile_fec_metabolic *in,
				 uint8_t *out)
{
	if (in == NULL || out == NULL) {
		return -EINVAL;
	}

	out[0] = PROFILE_FEC_PAGE_METABOLIC;
	out[1] = PROFILE_COMMON_INVALID_U8;
	put_le16(&out[2], in->mets_valid ? in->mets_centi
					 : PROFILE_COMMON_INVALID_U16);
	put_le16(&out[4], in->burn_rate_valid ? in->burn_rate_deci
					      : PROFILE_COMMON_INVALID_U16);
	out[6] = in->calories;
	out[7] = encode_fe_state((uint8_t)(in->capabilities & 0x0Fu), in->state,
				 in->lap_toggle);

	return 0;
}

int profile_fec_decode_metabolic(const uint8_t *body,
				 struct profile_fec_metabolic *out)
{
	if (body == NULL || out == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_FEC_PAGE_METABOLIC) {
		return -EINVAL;
	}

	out->mets_centi = get_le16(&body[2]);
	out->mets_valid = (out->mets_centi != PROFILE_COMMON_INVALID_U16);
	out->burn_rate_deci = get_le16(&body[4]);
	out->burn_rate_valid =
		(out->burn_rate_deci != PROFILE_COMMON_INVALID_U16);
	out->calories = body[6];
	out->capabilities = (uint8_t)(body[7] & 0x0Fu);
	split_fe_state(body[7], &out->state, &out->lap_toggle);

	return 0;
}

/* ── Page 54 (0x36) FE Capabilities ─────────────────────────────────────── */

int profile_fec_encode_capabilities(const struct profile_fec_capabilities *in,
				    uint8_t *out)
{
	if (in == NULL || out == NULL) {
		return -EINVAL;
	}

	out[0] = PROFILE_FEC_PAGE_CAPABILITIES;
	out[1] = PROFILE_COMMON_INVALID_U8;
	out[2] = PROFILE_COMMON_INVALID_U8;
	out[3] = PROFILE_COMMON_INVALID_U8;
	out[4] = PROFILE_COMMON_INVALID_U8;
	put_le16(&out[5], in->max_resistance_valid
				  ? in->max_resistance_n
				  : PROFILE_COMMON_INVALID_U16);
	out[7] = (uint8_t)(in->capabilities & 0x0Fu);

	return 0;
}

int profile_fec_decode_capabilities(const uint8_t *body,
				    struct profile_fec_capabilities *out)
{
	if (body == NULL || out == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_FEC_PAGE_CAPABILITIES) {
		return -EINVAL;
	}

	out->max_resistance_n = get_le16(&body[5]);
	out->max_resistance_valid =
		(out->max_resistance_n != PROFILE_COMMON_INVALID_U16);
	out->capabilities = (uint8_t)(body[7] & 0x0Fu);

	return 0;
}

/* ── Page 71 (0x47) Command Status ──────────────────────────────────────── */

int profile_fec_encode_cmd_status(const struct profile_fec_cmd_status *in,
				  uint8_t *out)
{
	if (in == NULL || out == NULL) {
		return -EINVAL;
	}

	out[0] = PROFILE_FEC_PAGE_CMD_STATUS;
	out[1] = in->last_command;
	out[2] = in->sequence;
	out[3] = in->status;
	/* Bytes [4..7] are the command's own [4..7], position for position -
	 * see the header. Re-packing them here is how page 51's grade ends up
	 * one byte out from where the controller reads it. */
	memcpy(&out[4], in->data, sizeof(in->data));

	return 0;
}

int profile_fec_decode_cmd_status(const uint8_t *body,
				  struct profile_fec_cmd_status *out)
{
	if (body == NULL || out == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_FEC_PAGE_CMD_STATUS) {
		return -EINVAL;
	}

	out->last_command = body[1];
	out->sequence = body[2];
	out->status = body[3];
	memcpy(out->data, &body[4], sizeof(out->data));

	return 0;
}

/* ── The control pages, controller -> equipment ─────────────────────────── */

int profile_fec_decode_basic_resistance(
	const uint8_t *body, struct profile_fec_cmd_basic_resistance *out)
{
	if (body == NULL || out == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_FEC_PAGE_BASIC_RESISTANCE) {
		return -EINVAL;
	}

	out->resistance_half_pct = body[7];
	return 0;
}

int profile_fec_decode_target_power(const uint8_t *body,
				    struct profile_fec_cmd_target_power *out)
{
	if (body == NULL || out == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_FEC_PAGE_TARGET_POWER) {
		return -EINVAL;
	}

	out->power_quarter_w = get_le16(&body[6]);
	return 0;
}

int profile_fec_decode_wind_resistance(
	const uint8_t *body, struct profile_fec_cmd_wind_resistance *out)
{
	if (body == NULL || out == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_FEC_PAGE_WIND_RESISTANCE) {
		return -EINVAL;
	}

	out->coefficient_centi = body[5];
	out->coefficient_valid = (body[5] != PROFILE_COMMON_INVALID_U8);

	/* Byte 6 is biased by +127, so 127 on the wire is a still day. The
	 * bias is removed HERE and the struct carries a plain signed km/h,
	 * because a biased number stored in a signed field is the shape that
	 * gets un-biased twice. */
	out->wind_speed_valid = (body[6] != PROFILE_COMMON_INVALID_U8);
	out->wind_speed_kph =
		(int16_t)((int16_t)body[6] - PROFILE_FEC_WIND_SPEED_BIAS);

	out->drafting_centi = body[7];
	out->drafting_valid = (body[7] != PROFILE_COMMON_INVALID_U8);

	return 0;
}

int profile_fec_decode_track_resistance(
	const uint8_t *body, struct profile_fec_cmd_track_resistance *out)
{
	if (body == NULL || out == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_FEC_PAGE_TRACK_RESISTANCE) {
		return -EINVAL;
	}

	out->grade_centi_pct =
		profile_fec_grade_to_centi_pct(get_le16(&body[5]),
					       &out->grade_valid);

	/* Decoded and NOT to be acted on by a treadmill (SS8.8.4.2, p.59).
	 * Present so the page 71 echo can carry it back unchanged and so that
	 * its absence is not read as an oversight. */
	out->rolling_resistance = body[7];
	out->rolling_resistance_valid = (body[7] != PROFILE_COMMON_INVALID_U8);

	return 0;
}

int profile_fec_decode_user_config(const uint8_t *body,
				   struct profile_fec_cmd_user_config *out)
{
	if (body == NULL || out == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_FEC_PAGE_USER_CONFIG) {
		return -EINVAL;
	}

	out->user_weight_10g = get_le16(&body[1]);
	out->user_weight_valid =
		(out->user_weight_10g != PROFILE_COMMON_INVALID_U16);

	/* Bytes [3..7] are bicycle fields in a packing this file does not
	 * claim to know. See the header. */
	return 0;
}

int profile_fec_decode_request(const uint8_t *body,
			       struct profile_fec_cmd_request *out)
{
	if (body == NULL || out == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_FEC_PAGE_REQUEST) {
		return -EINVAL;
	}

	out->acknowledged = ((body[5] & 0x80u) != 0u);
	out->tx_count = (uint8_t)(body[5] & 0x7Fu);
	out->page = body[6];
	out->command_type = body[7];

	return 0;
}

/* ---------------------------------------------------------------------------
 * The master
 * ---------------------------------------------------------------------------
 */

static int fec_common_80(struct profile_fec_tx *tx, uint8_t *body)
{
	return profile_common_80(&tx->cfg.id, body);
}

static int fec_common_81(struct profile_fec_tx *tx, uint8_t *body)
{
	return profile_common_81(&tx->cfg.id, body);
}

/* True when this group is the one whose slots 3 and 4 carry the common pair.
 * Every 13th group is every 65 messages, which is inside SS10.1's 66. */
static bool fec_common_group(const struct profile_fec_tx *tx)
{
	return (tx->group % PROFILE_FEC_TX_COMMON_GROUP) ==
	       (PROFILE_FEC_TX_COMMON_GROUP - 1u);
}

/*
 * The client seam, and the ONLY thing it is used for: the common-page pair.
 *
 * profile_sched.c places pages 80 and 81 at messages 119 and 120 of a
 * 121-message cycle, which is the certified cadence for the device types that
 * plan owns. FE-C's is 66, hard-coded nowhere in that engine and not
 * expressible through it, so this profile leaves cfg.common_80/81 NULL and
 * takes the two slots itself. See profile_fec_tx.h's interleave block.
 *
 * `m` is the engine's own message index and is deliberately unused: the
 * schedule is keyed on this profile's five-message group, which the engine
 * knows nothing about.
 */
static bool fec_claim(uint32_t m, uint8_t *body, void *user)
{
	struct profile_fec_tx *tx = (struct profile_fec_tx *)user;

	(void)m;

	if (tx == NULL || !fec_common_group(tx)) {
		return false;
	}

	if (tx->group_slot == 3u) {
		return fec_common_80(tx, body) == 0;
	}
	if (tx->group_slot == 4u) {
		return fec_common_81(tx, body) == 0;
	}
	return false;
}

/* One page, by number, for the on-request path and for the rotation. */
static int fec_encode_page(struct profile_fec_tx *tx, uint8_t page,
			   uint8_t *body)
{
	switch (page) {
	case PROFILE_FEC_PAGE_GENERAL:
		return profile_fec_encode_general(&tx->general, body);
	case PROFILE_FEC_PAGE_SETTINGS:
		return profile_fec_encode_settings(&tx->settings, body);
	case PROFILE_FEC_PAGE_METABOLIC:
		return profile_fec_encode_metabolic(&tx->metabolic, body);
	case PROFILE_FEC_PAGE_TREADMILL:
		return profile_fec_encode_treadmill(&tx->treadmill, body);
	case PROFILE_FEC_PAGE_CAPABILITIES:
		return profile_fec_encode_capabilities(&tx->cfg.caps, body);
	case PROFILE_FEC_PAGE_CMD_STATUS:
		return profile_fec_encode_cmd_status(&tx->status, body);
	case PROFILE_COMMON_PAGE_80:
		return fec_common_80(tx, body);
	case PROFILE_COMMON_PAGE_81:
		return fec_common_81(tx, body);
	default:
		return -EINVAL;
	}
}

/*
 * One data slot.
 *
 * `page` (what the engine's rotation offered) is ignored, exactly as
 * profile_hr.c ignores it and for the same reason: this profile's rotation is
 * not round-robin over a page list, it is SS10.1's five-message group, and the
 * engine has no way to express that. The rotation array exists only because
 * profile_sched_init() refuses a descriptor-less family with no pages at all.
 *
 * THE PRE-EMPTION RULE, WHICH IS WHERE THE INTERLEAVE WOULD OTHERWISE BREAK.
 * Page 71 and an on-request page take SLOT 3 ONLY, never 0, 1, 2 or 4:
 *   - slots 0 and 1 are page 16 and losing one breaks "twice consecutively";
 *   - slot 2 is the page 19 that survives when slot 3 is taken, so page 19's
 *     "at least once every 5 messages" holds no matter what pre-empts;
 *   - slot 4 is the pages 17/18 budget, which is 20 messages and already
 *     spends 10 of them.
 * The cost is latency: a command answered just after slot 3 waits up to nine
 * messages (~2.25 s at 4 Hz) for the next one, and in a common group slot 3 is
 * the page 80. That is a deliberate trade of promptness for an interleave a
 * head unit will not complain about.
 */
static int fec_data_page(uint8_t page, uint8_t counter, uint8_t *body,
			 void *user)
{
	struct profile_fec_tx *tx = (struct profile_fec_tx *)user;

	(void)page;
	/* FE-C pages carry no envelope event counter; page 25's is the
	 * trainer's own and a treadmill does not send that page. */
	(void)counter;

	if (tx == NULL) {
		return -EINVAL;
	}

	if (tx->group_slot == 3u) {
		if (tx->status_pending) {
			tx->status_pending = false;
			return fec_encode_page(tx, PROFILE_FEC_PAGE_CMD_STATUS,
					       body);
		}
		if (tx->on_request_left != 0u) {
			int rc = fec_encode_page(tx, tx->on_request_page, body);

			if (rc == 0) {
				tx->on_request_left--;
			}
			return rc;
		}
	}

	switch (tx->group_slot) {
	case 0u:
	case 1u:
		return fec_encode_page(tx, PROFILE_FEC_PAGE_GENERAL, body);
	case 2u:
	case 3u:
		return fec_encode_page(tx, PROFILE_FEC_PAGE_TREADMILL, body);
	default:
		break;
	}

	/* Slot 4: the settings budget. The alternation advances only when a
	 * settings page actually goes out, so a displaced slot delays the
	 * sequence rather than skipping a page - which is what keeps page 17's
	 * 20-message budget met when a common group intervenes. */
	if (tx->cfg.metabolic && tx->settings_slot_is_metabolic) {
		int rc = fec_encode_page(tx, PROFILE_FEC_PAGE_METABOLIC, body);

		if (rc == 0) {
			tx->settings_slot_is_metabolic = false;
		}
		return rc;
	}

	{
		int rc = fec_encode_page(tx, PROFILE_FEC_PAGE_SETTINGS, body);

		if (rc == 0) {
			tx->settings_slot_is_metabolic = tx->cfg.metabolic;
		}
		return rc;
	}
}

int profile_fec_tx_init(struct profile_fec_tx *tx,
			const struct profile_fec_tx_cfg *cfg)
{
	struct profile_sched_cfg    sched_cfg;
	struct profile_sched_client client;
	int                         rc;

	if (tx == NULL || cfg == NULL) {
		return -EINVAL;
	}

	memset(tx, 0, sizeof(*tx));
	tx->cfg = *cfg;
	tx->rotation[0] = PROFILE_FEC_PAGE_GENERAL;

	/* A master that has answered nothing yet still owes a well-formed page
	 * 71 if one is requested, so the status starts at the document's
	 * "uninitialised" rather than at a zeroed "pass". */
	tx->status.last_command = PROFILE_FEC_CMD_NONE;
	tx->status.sequence = PROFILE_FEC_CMD_NO_SEQ;
	tx->status.status = PROFILE_FEC_CMD_STATUS_UNINITIALISED;
	memset(tx->status.data, PROFILE_COMMON_INVALID_U8,
	       sizeof(tx->status.data));

	/* The three pages the node has not filled in yet must still encode to
	 * something legal, so every field starts at its own invalid value
	 * rather than at zero - a zeroed page 16 is a treadmill reporting
	 * 0.000 m/s and 0 bpm as measurements. */
	tx->general.equipment_type = cfg->equipment_type;
	tx->general.state = PROFILE_FEC_STATE_READY;
	tx->settings.state = PROFILE_FEC_STATE_READY;
	tx->treadmill.state = PROFILE_FEC_STATE_READY;
	tx->metabolic.state = PROFILE_FEC_STATE_READY;

	memset(&sched_cfg, 0, sizeof(sched_cfg));
	sched_cfg.desc = NULL; /* an ANT+ compatibility type announces no schema */
	sched_cfg.pages = tx->rotation;
	sched_cfg.n_pages = (uint8_t)sizeof(tx->rotation);
	sched_cfg.data_page = fec_data_page;
	/* NULL on purpose - see fec_claim(). The engine's 119/120-of-121
	 * placement is the wrong cadence for this profile and there is no way
	 * to retune it, so the pair rides the client seam instead. */
	sched_cfg.common_80 = NULL;
	sched_cfg.common_81 = NULL;
	sched_cfg.common_82 = NULL;
	sched_cfg.user = tx;

	rc = profile_sched_init(&tx->sched, &sched_cfg);
	if (rc != 0) {
		return rc;
	}

	client.claim = fec_claim;
	client.user = tx;
	return profile_sched_set_client(&tx->sched, &client);
}

void profile_fec_tx_set_general(struct profile_fec_tx *tx,
				const struct profile_fec_general *g)
{
	if (tx != NULL && g != NULL) {
		tx->general = *g;
	}
}

/*
 * The incline is CLAMPED here and REFUSED by the encoder, and the asymmetry is
 * deliberate. profile_fec_encode_settings() is a page codec and an
 * out-of-range field is a caller bug worth an -EINVAL. This setter feeds a
 * running master, where an -EINVAL two layers down turns into a slot the
 * scheduler declines - and because profile_fec_tx_next() advances the group
 * only on a message that actually went out, a permanently failing slot 4 would
 * wedge the whole five-message group on that slot and stop the node
 * transmitting anything at all. Clamping is the failure a receiver can see.
 */
void profile_fec_tx_set_settings(struct profile_fec_tx *tx,
				 const struct profile_fec_settings *s)
{
	if (tx == NULL || s == NULL) {
		return;
	}

	tx->settings = *s;
	if (tx->settings.incline_centi_pct < PROFILE_FEC_INCLINE_MIN_CENTI_PCT) {
		tx->settings.incline_centi_pct = PROFILE_FEC_INCLINE_MIN_CENTI_PCT;
	} else if (tx->settings.incline_centi_pct >
		   PROFILE_FEC_INCLINE_MAX_CENTI_PCT) {
		tx->settings.incline_centi_pct = PROFILE_FEC_INCLINE_MAX_CENTI_PCT;
	}
}

void profile_fec_tx_set_treadmill(struct profile_fec_tx *tx,
				  const struct profile_fec_treadmill *t)
{
	if (tx != NULL && t != NULL) {
		tx->treadmill = *t;
	}
}

void profile_fec_tx_set_metabolic(struct profile_fec_tx *tx,
				  const struct profile_fec_metabolic *m)
{
	if (tx != NULL && m != NULL) {
		tx->metabolic = *m;
	}
}

void profile_fec_tx_command_answered(struct profile_fec_tx *tx,
				     uint8_t command_page, uint8_t sequence,
				     uint8_t status, const uint8_t *data4)
{
	if (tx == NULL) {
		return;
	}

	tx->status.last_command = command_page;
	tx->status.sequence = sequence;
	tx->status.status = status;
	if (data4 != NULL) {
		memcpy(tx->status.data, data4, sizeof(tx->status.data));
	} else {
		memset(tx->status.data, PROFILE_COMMON_INVALID_U8,
		       sizeof(tx->status.data));
	}
	tx->status_pending = true;
}

int profile_fec_tx_request(struct profile_fec_tx *tx, uint8_t page,
			   uint8_t count)
{
	if (tx == NULL) {
		return -EINVAL;
	}

	switch (page) {
	case PROFILE_FEC_PAGE_CAPABILITIES:
	case PROFILE_FEC_PAGE_CMD_STATUS:
	case PROFILE_COMMON_PAGE_80:
	case PROFILE_COMMON_PAGE_81:
		break;
	default:
		/* Everything else is already in the rotation and asking for it
		 * is answered by waiting. Saying so is better than pretending
		 * to schedule it: SS8.8 wants a page 71 with
		 * NOT_SUPPORTED, and that is the caller's to send. */
		return -ENOTSUP;
	}

	tx->on_request_page = page;
	tx->on_request_left = (count == 0u) ? 1u : count;
	return 0;
}

enum profile_slot_kind profile_fec_tx_next(struct profile_fec_tx *tx,
					   uint8_t *body)
{
	enum profile_slot_kind kind;

	if (tx == NULL || body == NULL) {
		return PROFILE_SLOT_IDLE;
	}

	kind = profile_sched_next(&tx->sched, body);

	/* The common pair comes back as a CLIENT slot because that is the seam
	 * it rode. Translating here rather than leaving it to the caller keeps
	 * "how many common pages went out" answerable by anybody holding the
	 * return value, which is what a coexistence run counts. */
	if (kind == PROFILE_SLOT_CLIENT) {
		kind = (body[0] == PROFILE_COMMON_PAGE_80)
			       ? PROFILE_SLOT_COMMON_80
			       : PROFILE_SLOT_COMMON_81;
	}

	if (kind != PROFILE_SLOT_IDLE) {
		tx->messages++;
		tx->group_slot++;
		if (tx->group_slot >= PROFILE_FEC_TX_GROUP_SLOTS) {
			tx->group_slot = 0u;
			tx->group++;
		}
	}

	return kind;
}

uint32_t profile_fec_tx_messages(const struct profile_fec_tx *tx)
{
	return (tx == NULL) ? 0u : tx->messages;
}
