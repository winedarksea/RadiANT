/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_rd.c - ANT+ Running Dynamics, device type 0x1E.
 *
 * Provenance: the ANT+ Running Dynamics device profile, D00001679 Rev 1.1,
 * tables 5-1 through 5-5 and 6-8 through 6-12, obtained under this project's
 * adopter login and supplied by Colin on 2026-08-13. Layout tables and field
 * semantics only; no prose copied. ANT+ device profiles are implementable in
 * src/profiles/ under docs/decisions/0002-clean-room-policy.md's graded
 * posture, as amended by fact 5 of docs/decisions/0008. No sdk-ant source was
 * consulted and nothing here derives from libant.a.
 *
 * There is no cryptography in this file and no call to radiant_sec anywhere in
 * it. See the header.
 *
 * ---------------------------------------------------------------------------
 * Every split field is packed and unpacked in ONE place
 * ---------------------------------------------------------------------------
 * The eight helpers below are the whole of the bit arithmetic in this file, and
 * every encoder and decoder goes through them. That is not indirection for its
 * own sake: the ground-contact-time split (3 low bits, then 8 high bits) and
 * the two low-bit-first fraction splits are each written once and read by both
 * directions, so an encoder and a decoder cannot disagree about a field the way
 * two independently-written shift expressions eventually do.
 *
 * profile_bits.c is deliberately NOT used. It packs MSB-first into the RadiANT
 * envelope's field area, which is a different convention from this document's
 * numbered bit positions within a byte, and borrowing it here would mean two
 * bit orders in one file with nothing but a comment separating them.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "profile_common.h"
#include "profile_rd.h"
#include "profile_sched.h"

/* ── The splits ─────────────────────────────────────────────────────────────
 *
 * "lo bits at `lo_off` of byte a, hi bits at bit 0 of byte b" is the shape of
 * every multi-byte field on this profile. Naming it once makes each call site
 * read as the table row it came from.
 */

/* An integer whose low 8 bits are in `lsb` and whose high bits are the low
 * `hi_width` bits of `hi`, starting at bit `hi_off`. Vertical oscillation and
 * step length are both this shape. */
static uint16_t split_lo8(uint8_t lsb, uint8_t hi, uint8_t hi_off,
			  uint8_t hi_width)
{
	uint16_t mask = (uint16_t)((1u << hi_width) - 1u);

	return (uint16_t)(lsb | (uint16_t)(((hi >> hi_off) & mask) << 8));
}

/* Ground contact time, which is the other way round: the THREE LOW bits live in
 * byte [4] bits 5..7 and the eight high bits are all of byte [5]. A field whose
 * low bits are the ones sharing a byte is unusual enough on this profile to be
 * worth its own function rather than a parameter to the one above. */
static uint16_t split_lo3(uint8_t lo, uint8_t lo_off, uint8_t msb)
{
	return (uint16_t)(((lo >> lo_off) & 0x07u) | ((uint16_t)msb << 3));
}

/* A fraction split low-bit-first: bit 0 of the fraction at `lo_off` of byte
 * `lo`, the remaining `hi_width` bits at bit `hi_off` of byte `hi`. Stance time
 * (1+1) and ground contact balance (1+4) are both this, and getting the
 * direction backwards is the "wrong by a factor of two only sometimes" bug the
 * header warns about. */
static uint8_t split_frac(uint8_t lo, uint8_t lo_off, uint8_t hi,
			  uint8_t hi_off, uint8_t hi_width)
{
	uint8_t mask = (uint8_t)((1u << hi_width) - 1u);

	return (uint8_t)(((lo >> lo_off) & 0x01u) |
			 (uint8_t)((((hi >> hi_off) & mask)) << 1));
}

/* ── Range checks ───────────────────────────────────────────────────────────
 *
 * A percentage field is 7 bits wide and 101..127 is reserved, so "fits the
 * field" and "is a legal value" are two different questions and this profile is
 * one of the few that says so explicitly. Both are refused rather than
 * truncated: a masked percentage is a plausible number no receiver can tell
 * was wrong.
 */
static bool percent_ok(uint16_t scaled, uint8_t per_percent)
{
	return (scaled / per_percent) < PROFILE_RD_PERCENT_RESERVED_MIN;
}

static int metrics_ok(const struct profile_rd_metrics *m)
{
	if (m->cadence_32 > PROFILE_RD_CADENCE_MAX ||
	    m->vert_osc_quarter_mm > PROFILE_RD_VERT_OSC_MAX ||
	    m->gct_ms > PROFILE_RD_GCT_MAX ||
	    m->stance_quarter > PROFILE_RD_STANCE_MAX ||
	    m->balance_32 > PROFILE_RD_BALANCE_MAX ||
	    m->vert_ratio_32 > PROFILE_RD_BALANCE_MAX ||
	    m->step_length_mm > PROFILE_RD_STEP_LEN_MAX ||
	    m->step_count > PROFILE_RD_STEP_COUNT_MAX) {
		return -EINVAL;
	}
	if (!percent_ok(m->stance_quarter, PROFILE_RD_STANCE_PER_PERCENT) ||
	    !percent_ok(m->balance_32, PROFILE_RD_BALANCE_PER_PERCENT) ||
	    !percent_ok(m->vert_ratio_32, PROFILE_RD_VERT_RATIO_PER_PERCENT)) {
		return -EINVAL;
	}
	return 0;
}

/* ── Page 0x00, Running Dynamics A ──────────────────────────────────────── */

int profile_rd_encode_a(const struct profile_rd_metrics *m, bool bidirectional,
			uint8_t *out)
{
	uint16_t vo_mm;
	uint8_t  vo_frac;
	uint8_t  stance_frac;
	int      rc;

	if (m == NULL || out == NULL) {
		return -EINVAL;
	}
	rc = metrics_ok(m);
	if (rc != 0) {
		return rc;
	}

	vo_mm = (uint16_t)(m->vert_osc_quarter_mm / PROFILE_RD_VERT_OSC_PER_MM);
	vo_frac = (uint8_t)(m->vert_osc_quarter_mm % PROFILE_RD_VERT_OSC_PER_MM);
	stance_frac = (uint8_t)(m->stance_quarter % PROFILE_RD_STANCE_PER_PERCENT);

	out[0] = PROFILE_RD_PAGE_A;
	out[1] = (uint8_t)(m->cadence_32 / PROFILE_RD_CADENCE_PER_STRIDE_MIN);
	/* Bit 7 is reserved and set to 0 - the one reserved field on this
	 * profile that is not 0xFF-filled, because it shares a byte. */
	out[2] = (uint8_t)((m->cadence_32 % PROFILE_RD_CADENCE_PER_STRIDE_MIN) |
			   (m->walking ? (1u << 5) : 0u) |
			   (bidirectional ? (1u << 6) : 0u));
	out[3] = (uint8_t)(vo_mm & 0xFFu);
	out[4] = (uint8_t)(((vo_mm >> 8) & 0x07u) | (uint8_t)(vo_frac << 3) |
			   (uint8_t)((m->gct_ms & 0x07u) << 5));
	out[5] = (uint8_t)((m->gct_ms >> 3) & 0xFFu);
	out[6] = (uint8_t)((m->stance_quarter / PROFILE_RD_STANCE_PER_PERCENT) |
			   (uint8_t)((stance_frac & 0x01u) << 7));
	out[7] = (uint8_t)(((stance_frac >> 1) & 0x01u) |
			   (uint8_t)((m->step_count & 0x7Fu) << 1));
	return 0;
}

int profile_rd_decode_a(const uint8_t *body, struct profile_rd_metrics *m,
			bool *bidirectional)
{
	uint16_t vo_mm;

	if (body == NULL || m == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_RD_PAGE_A) {
		return -EINVAL;
	}

	m->cadence_32 = (uint16_t)((uint16_t)body[1] *
				   PROFILE_RD_CADENCE_PER_STRIDE_MIN +
				   (body[2] & 0x1Fu));
	m->walking = ((body[2] >> 5) & 0x01u) != 0u;
	if (bidirectional != NULL) {
		*bidirectional = ((body[2] >> 6) & 0x01u) != 0u;
	}

	vo_mm = split_lo8(body[3], body[4], 0u, 3u);
	m->vert_osc_quarter_mm = (uint16_t)(vo_mm * PROFILE_RD_VERT_OSC_PER_MM +
					    ((body[4] >> 3) & 0x03u));
	m->gct_ms = split_lo3(body[4], 5u, body[5]);
	m->stance_quarter = (uint16_t)((uint16_t)(body[6] & 0x7Fu) *
				       PROFILE_RD_STANCE_PER_PERCENT +
				       split_frac(body[6], 7u, body[7], 0u, 1u));
	m->step_count = (uint8_t)((body[7] >> 1) & 0x7Fu);

	/*
	 * A sensor reporting "no motion" sets the INTEGER part to zero, and the
	 * fractional bits beside it are then not a quarter of anything. Zeroing
	 * the whole scaled value here is what makes PROFILE_RD_INVALID_* a
	 * single comparison for the caller rather than a shift and a mask.
	 */
	if (body[1] == 0u) {
		m->cadence_32 = PROFILE_RD_INVALID_CADENCE;
	}
	if (vo_mm == 0u) {
		m->vert_osc_quarter_mm = PROFILE_RD_INVALID_VERTICAL_OSC;
	}
	if ((body[6] & 0x7Fu) == 0u) {
		m->stance_quarter = PROFILE_RD_INVALID_STANCE;
	}
	return 0;
}

/* ── Page 0x01, Running Dynamics B ──────────────────────────────────────── */

int profile_rd_encode_b(const struct profile_rd_metrics *m,
			uint16_t session_leader, uint8_t *out)
{
	uint8_t bal_frac;
	uint8_t vr_int;
	int     rc;

	if (m == NULL || out == NULL) {
		return -EINVAL;
	}
	rc = metrics_ok(m);
	if (rc != 0) {
		return rc;
	}

	bal_frac = (uint8_t)(m->balance_32 % PROFILE_RD_BALANCE_PER_PERCENT);
	vr_int = (uint8_t)(m->vert_ratio_32 / PROFILE_RD_VERT_RATIO_PER_PERCENT);

	out[0] = PROFILE_RD_PAGE_B;
	out[1] = (uint8_t)((m->balance_32 / PROFILE_RD_BALANCE_PER_PERCENT) |
			   (uint8_t)((bal_frac & 0x01u) << 7));
	out[2] = (uint8_t)(((bal_frac >> 1) & 0x0Fu) |
			   (uint8_t)((vr_int & 0x0Fu) << 4));
	out[3] = (uint8_t)(((vr_int >> 4) & 0x07u) |
			   (uint8_t)((m->vert_ratio_32 %
				      PROFILE_RD_VERT_RATIO_PER_PERCENT) << 3));
	out[4] = (uint8_t)(m->step_length_mm & 0xFFu);
	/* Bits 6..7 are reserved and set to 0b11 here, where page 0's reserved
	 * bit is set to 0. Two reserved conventions in one profile, one line
	 * apart in the two tables, and neither is a typo in this file. */
	out[5] = (uint8_t)(((m->step_length_mm >> 8) & 0x1Fu) |
			   (m->upside_down ? (1u << 5) : 0u) | 0xC0u);
	out[6] = (uint8_t)(session_leader & 0xFFu);
	out[7] = (uint8_t)((session_leader >> 8) & 0xFFu);
	return 0;
}

int profile_rd_decode_b(const uint8_t *body, struct profile_rd_metrics *m,
			uint16_t *session_leader)
{
	uint8_t vr_int;

	if (body == NULL || m == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_RD_PAGE_B) {
		return -EINVAL;
	}

	m->balance_32 = (uint16_t)((uint16_t)(body[1] & 0x7Fu) *
				   PROFILE_RD_BALANCE_PER_PERCENT +
				   split_frac(body[1], 7u, body[2], 0u, 4u));
	vr_int = (uint8_t)(((body[2] >> 4) & 0x0Fu) |
			   (uint8_t)((body[3] & 0x07u) << 4));
	m->vert_ratio_32 = (uint16_t)((uint16_t)vr_int *
				      PROFILE_RD_VERT_RATIO_PER_PERCENT +
				      ((body[3] >> 3) & 0x1Fu));
	m->step_length_mm = split_lo8(body[4], body[5], 0u, 5u);
	m->upside_down = ((body[5] >> 5) & 0x01u) != 0u;

	if ((body[1] & 0x7Fu) == 0u) {
		m->balance_32 = PROFILE_RD_INVALID_BALANCE;
	}
	if (session_leader != NULL) {
		*session_leader = (uint16_t)(body[6] | ((uint16_t)body[7] << 8));
	}
	return 0;
}

/* ── Page 0x10, Session Leader Speed Metrics ────────────────────────────── */

int profile_rd_encode_speed(uint16_t speed_256, uint8_t *out)
{
	uint8_t whole;
	uint8_t frac;

	if (out == NULL) {
		return -EINVAL;
	}

	if (speed_256 == PROFILE_RD_SPEED_INVALID) {
		whole = PROFILE_RD_SPEED_INVALID_INT;
		frac = PROFILE_RD_SPEED_INVALID_FRAC;
	} else {
		/* 0x0F is the integer field's sentinel, so 15 m/s is not
		 * expressible and the range stops at 14.996 m/s. */
		if (speed_256 >= (PROFILE_RD_SPEED_INVALID_INT * 256u)) {
			return -EINVAL;
		}
		whole = (uint8_t)(speed_256 / PROFILE_RD_SPEED_PER_MPS);
		frac = (uint8_t)(speed_256 % PROFILE_RD_SPEED_PER_MPS);
	}

	out[0] = PROFILE_RD_PAGE_SPEED;
	out[1] = (uint8_t)((whole & 0x0Fu) | (uint8_t)((frac & 0x0Fu) << 4));
	out[2] = (uint8_t)(((frac >> 4) & 0x0Fu) | 0xF0u);
	memset(&out[3], PROFILE_COMMON_INVALID_U8, 5);
	return 0;
}

int profile_rd_decode_speed(const uint8_t *body, uint16_t *speed_256)
{
	uint8_t whole;
	uint8_t frac;

	if (body == NULL || speed_256 == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_RD_PAGE_SPEED) {
		return -EINVAL;
	}

	whole = (uint8_t)(body[1] & 0x0Fu);
	frac = (uint8_t)(((body[1] >> 4) & 0x0Fu) |
			 (uint8_t)((body[2] & 0x0Fu) << 4));

	/* Either sentinel alone invalidates the reading: they are different
	 * values in different widths and the document gives each its own. */
	if (whole == PROFILE_RD_SPEED_INVALID_INT ||
	    frac == PROFILE_RD_SPEED_INVALID_FRAC) {
		*speed_256 = PROFILE_RD_SPEED_INVALID;
	} else {
		*speed_256 = (uint16_t)((uint16_t)whole *
					PROFILE_RD_SPEED_PER_MPS + frac);
	}
	return 0;
}

/* ── Page 0x20, Session Leader Request ──────────────────────────────────── */

int profile_rd_encode_leader_request(uint16_t leader_id, uint8_t *out)
{
	if (out == NULL || leader_id == PROFILE_RD_LEADER_NONE) {
		return -EINVAL;
	}

	out[0] = PROFILE_RD_PAGE_LEADER_REQUEST;
	out[1] = (uint8_t)(leader_id & 0xFFu);
	out[2] = (uint8_t)((leader_id >> 8) & 0xFFu);
	memset(&out[3], PROFILE_COMMON_INVALID_U8, 5);
	return 0;
}

int profile_rd_decode_leader_request(const uint8_t *body, uint16_t *leader_id)
{
	if (body == NULL || leader_id == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_RD_PAGE_LEADER_REQUEST) {
		return -EINVAL;
	}
	*leader_id = (uint16_t)(body[1] | ((uint16_t)body[2] << 8));
	return 0;
}

/* ── Page 0x4A, Open Channel Command ────────────────────────────────────── */

int profile_rd_encode_open_channel(uint32_t leader_id_24, uint8_t rf_freq,
				   uint16_t period, uint8_t *out)
{
	if (out == NULL) {
		return -EINVAL;
	}
	if (leader_id_24 > 0xFFFFFFu) {
		return -EINVAL;
	}
	if (profile_rd_rf_index_enum(rf_freq) == PROFILE_RD_RF_ENUM_INVALID) {
		return -EINVAL;
	}
	if (period != PROFILE_RD_PERIOD_HR_RD) {
		return -EINVAL;
	}

	out[0] = PROFILE_RD_PAGE_OPEN_CHANNEL;
	out[1] = (uint8_t)(leader_id_24 & 0xFFu);
	out[2] = (uint8_t)((leader_id_24 >> 8) & 0xFFu);
	out[3] = (uint8_t)((leader_id_24 >> 16) & 0xFFu);
	out[4] = PROFILE_RD_DEVICE_TYPE;
	out[5] = rf_freq;
	out[6] = (uint8_t)(period & 0xFFu);
	out[7] = (uint8_t)((period >> 8) & 0xFFu);
	return 0;
}

int profile_rd_decode_open_channel(const uint8_t *body, uint32_t *leader_id_24,
				   uint8_t *rf_freq, uint16_t *period)
{
	if (body == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_RD_PAGE_OPEN_CHANNEL) {
		return -EINVAL;
	}
	/* Byte [4] is the device type and the document fixes it at 30. A page
	 * naming any other device type is not an open-channel command for this
	 * profile, whatever else it may be. */
	if (body[4] != PROFILE_RD_DEVICE_TYPE) {
		return -EINVAL;
	}

	if (leader_id_24 != NULL) {
		*leader_id_24 = (uint32_t)body[1] |
				((uint32_t)body[2] << 8) |
				((uint32_t)body[3] << 16);
	}
	if (rf_freq != NULL) {
		*rf_freq = body[5];
	}
	if (period != NULL) {
		*period = (uint16_t)(body[6] | ((uint16_t)body[7] << 8));
	}
	return 0;
}

/* ── The HR-RD linkage ──────────────────────────────────────────────────── */

uint8_t profile_rd_rf_enum_index(uint8_t rf_enum)
{
	switch (rf_enum) {
	case PROFILE_RD_RF_ENUM_2403:
		return PROFILE_RD_RF_2403;
	case PROFILE_RD_RF_ENUM_2439:
		return PROFILE_RD_RF_2439;
	case PROFILE_RD_RF_ENUM_2475:
		return PROFILE_RD_RF_2475;
	case PROFILE_RD_RF_ENUM_2461:
		return PROFILE_RD_RF_2461;
	default:
		/* PROFILE_RD_RF_ENUM_INVALID, and every value outside the
		 * enumeration - which on a strap without running dynamics is
		 * manufacturer-specific data that must not be interpreted. */
		return 0u;
	}
}

uint8_t profile_rd_rf_index_enum(uint8_t rf_freq)
{
	switch (rf_freq) {
	case PROFILE_RD_RF_2403:
		return PROFILE_RD_RF_ENUM_2403;
	case PROFILE_RD_RF_2439:
		return PROFILE_RD_RF_ENUM_2439;
	case PROFILE_RD_RF_2475:
		return PROFILE_RD_RF_ENUM_2475;
	case PROFILE_RD_RF_2461:
		return PROFILE_RD_RF_ENUM_2461;
	default:
		return PROFILE_RD_RF_ENUM_INVALID;
	}
}

/* ── The master ─────────────────────────────────────────────────────────── */

static int rd_data_page(uint8_t page, uint8_t counter, uint8_t *body,
			void *user)
{
	struct profile_rd *rd = (struct profile_rd *)user;

	/* This device type carries no event counter of the envelope's kind; its
	 * one accumulator is the 7-bit step count inside page 0x00. */
	(void)counter;

	if (rd == NULL) {
		return -EINVAL;
	}

	if (page == PROFILE_RD_PAGE_B) {
		return profile_rd_encode_b(&rd->m, rd->session_leader, body);
	}
	/* The bidirectional-support bit says this pod can be given a session
	 * leader and be told the display's speed. An HR-RD strap does not use
	 * the bit at all, so it reports 0 there. */
	return profile_rd_encode_a(&rd->m, !rd->cfg.hr_rd, body);
}

static int rd_common_80(uint8_t *body, void *user)
{
	struct profile_rd *rd = (struct profile_rd *)user;

	if (rd == NULL) {
		return -EINVAL;
	}
	return profile_common_80(&rd->cfg.id, body);
}

static int rd_common_81(uint8_t *body, void *user)
{
	struct profile_rd *rd = (struct profile_rd *)user;

	if (rd == NULL) {
		return -EINVAL;
	}
	return profile_common_81(&rd->cfg.id, body);
}

static int rd_common_82(uint8_t *body, void *user)
{
	struct profile_rd *rd = (struct profile_rd *)user;

	if (rd == NULL) {
		return -EINVAL;
	}
	return profile_common_82(&rd->cfg.battery, body);
}

int profile_rd_init(struct profile_rd *rd, const struct profile_rd_cfg *cfg)
{
	struct profile_sched_cfg sched_cfg;

	if (rd == NULL || cfg == NULL) {
		return -EINVAL;
	}

	memset(rd, 0, sizeof(*rd));
	rd->cfg = *cfg;
	rd->rotation[0] = PROFILE_RD_PAGE_A;
	rd->rotation[1] = PROFILE_RD_PAGE_B;
	rd->session_leader = cfg->hr_rd ? PROFILE_RD_LEADER_HR_RD
					: PROFILE_RD_LEADER_NONE;
	rd->leader_speed_256 = PROFILE_RD_SPEED_INVALID;

	memset(&sched_cfg, 0, sizeof(sched_cfg));
	sched_cfg.desc = NULL; /* an ANT+ compatibility type announces no
				* schema: its pages are fixed by somebody
				* else's document */
	sched_cfg.pages = rd->rotation;
	sched_cfg.n_pages = (uint8_t)sizeof(rd->rotation);
	sched_cfg.data_page = rd_data_page;
	sched_cfg.common_80 = rd_common_80;
	sched_cfg.common_81 = rd_common_81;
	sched_cfg.common_82 = (cfg->common_82_every != 0u) ? rd_common_82 : NULL;
	sched_cfg.common_82_every = cfg->common_82_every;
	sched_cfg.user = rd;

	return profile_sched_init(&rd->sched, &sched_cfg);
}

void profile_rd_set_metrics(struct profile_rd *rd,
			    const struct profile_rd_metrics *m)
{
	if (rd != NULL && m != NULL) {
		rd->m = *m;
	}
}

void profile_rd_add_steps(struct profile_rd *rd, uint8_t steps)
{
	if (rd != NULL) {
		/* Wraps at 128, which is what a 7-bit rollover field does and
		 * what a display differencing two readings in that width
		 * expects. */
		rd->m.step_count = (uint8_t)((rd->m.step_count + steps) & 0x7Fu);
	}
}

int profile_rd_claim_leader(struct profile_rd *rd, uint16_t leader_id)
{
	if (rd == NULL || leader_id == PROFILE_RD_LEADER_NONE) {
		return -EINVAL;
	}
	if (rd->cfg.hr_rd) {
		/* An HR-RD strap's leader is settled by page 74 on the
		 * heart-rate channel; page 0x20 is not part of this
		 * conversation and its leader field stays 0xFFFF. */
		return -EINVAL;
	}
	if (rd->session_leader != PROFILE_RD_LEADER_NONE &&
	    rd->session_leader != leader_id) {
		return -EBUSY;
	}
	rd->session_leader = leader_id;
	return 0;
}

int profile_rd_apply_speed(struct profile_rd *rd, const uint8_t *body)
{
	uint16_t speed;
	int      rc;

	if (rd == NULL) {
		return -EINVAL;
	}
	rc = profile_rd_decode_speed(body, &speed);
	if (rc != 0) {
		return rc;
	}
	rd->leader_speed_256 = speed;
	rd->have_leader_speed = (speed != PROFILE_RD_SPEED_INVALID);
	return 0;
}

enum profile_slot_kind profile_rd_next(struct profile_rd *rd, uint8_t *body)
{
	if (rd == NULL || body == NULL) {
		return PROFILE_SLOT_IDLE;
	}
	return profile_sched_next(&rd->sched, body);
}
