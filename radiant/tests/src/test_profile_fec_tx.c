/*
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include "profile_common.h"
#include "profile_fec.h"
#include "profile_fec_tx.h"
#include "profile_sched.h"

/* ── Grade and incline ──────────────────────────────────────────────────── */

ZTEST(profile_fec_tx, test_grade_bias_at_all_three_landmarks)
{
	bool valid = false;

	/* SS8.8.4.1: Grade% = raw x 0.01 - 200.00. The bottom of the range,
	 * flat, and the top. 0x4E20 is the one that matters in practice and
	 * the other two are what catch a sign error that happens to be zero at
	 * the middle. */
	zassert_equal(profile_fec_grade_to_centi_pct(0x0000u, &valid), -20000,
		      "raw 0 is -200.00 %%, not 0.00 %%");
	zassert_true(valid, NULL);

	zassert_equal(profile_fec_grade_to_centi_pct(0x4E20u, &valid), 0,
		      "0x4E20 is 20000 raw, which is exactly 0.00 %%");
	zassert_true(valid, NULL);

	zassert_equal(profile_fec_grade_to_centi_pct(0x9C40u, &valid), 20000,
		      NULL);
	zassert_true(valid, NULL);

	/* +3.00 %, the grade the bench procedure sends. */
	zassert_equal(profile_fec_grade_to_centi_pct(0x4F4Cu, &valid), 300,
		      NULL);
}

ZTEST(profile_fec_tx, test_grade_sentinel_is_flat_not_an_error)
{
	bool valid = true;

	/* SS8.8.4.1: an FE receiving 0xFFFF assumes flat ground. Reporting an
	 * error and holding the previous grade would leave a treadmill
	 * climbing because the controller went quiet. */
	zassert_equal(profile_fec_grade_to_centi_pct(PROFILE_FEC_GRADE_INVALID,
						     &valid),
		      0, NULL);
	zassert_false(valid, NULL);
}

ZTEST(profile_fec_tx, test_grade_round_trips_and_clamps)
{
	bool valid;

	zassert_equal(profile_fec_centi_pct_to_grade(0), 20000u, NULL);
	zassert_equal(profile_fec_centi_pct_to_grade(300), 0x4F4Cu, NULL);
	zassert_equal(profile_fec_centi_pct_to_grade(-20000), 0u, NULL);
	zassert_equal(profile_fec_centi_pct_to_grade(20000), 40000u, NULL);

	/* Outside the representable span the answer is the edge, not a wrap.
	 * A wrapped +250 % would come back as a steep descent. */
	zassert_equal(profile_fec_centi_pct_to_grade(25000), 40000u, NULL);
	zassert_equal(profile_fec_centi_pct_to_grade(-25000), 0u, NULL);

	zassert_equal(profile_fec_grade_to_centi_pct(
			      profile_fec_centi_pct_to_grade(-4200), &valid),
		      -4200, NULL);
	zassert_true(valid, NULL);
}

ZTEST(profile_fec_tx, test_incline_and_grade_use_different_sentinels)
{
	struct profile_fec_settings in;
	struct profile_fec_settings out;
	uint8_t                     body[8];

	memset(&in, 0, sizeof(in));
	in.incline_valid = false;
	in.state = PROFILE_FEC_STATE_READY;

	zassert_equal(profile_fec_encode_settings(&in, body), 0, NULL);
	/* 0x7FFF, the largest positive sint16 - NOT grade's 0xFFFF, which read
	 * as an incline is -0.01 % and would display as very nearly flat. */
	zassert_equal(body[4], 0xFFu, NULL);
	zassert_equal(body[5], 0x7Fu, NULL);

	zassert_equal(profile_fec_decode_settings(body, &out), 0, NULL);
	zassert_false(out.incline_valid, NULL);

	/* And the mirror: 0xFFFF in the incline field is a real -0.01 %, not
	 * "absent". This is the reading a grade habit gets wrong. */
	body[4] = 0xFFu;
	body[5] = 0xFFu;
	zassert_equal(profile_fec_decode_settings(body, &out), 0, NULL);
	zassert_true(out.incline_valid, NULL);
	zassert_equal(out.incline_centi_pct, -1, NULL);
}

ZTEST(profile_fec_tx, test_settings_incline_round_trip_and_range)
{
	struct profile_fec_settings in;
	struct profile_fec_settings out;
	uint8_t                     body[8];

	memset(&in, 0, sizeof(in));
	in.incline_valid = true;
	in.incline_centi_pct = 300; /* the +3.00 % of the bench procedure */
	in.cycle_length_valid = true;
	in.cycle_length_cm = 120u; /* 1.20 m stride */
	in.resistance_valid = false;
	in.state = PROFILE_FEC_STATE_IN_USE;
	in.lap_toggle = true;

	zassert_equal(profile_fec_encode_settings(&in, body), 0, NULL);
	zassert_equal(body[0], PROFILE_FEC_PAGE_SETTINGS, NULL);
	zassert_equal(body[4], 0x2Cu, NULL); /* 300 = 0x012C, little-endian */
	zassert_equal(body[5], 0x01u, NULL);

	zassert_equal(profile_fec_decode_settings(body, &out), 0, NULL);
	zassert_true(out.incline_valid, NULL);
	zassert_equal(out.incline_centi_pct, 300, NULL);
	zassert_equal(out.cycle_length_cm, 120u, NULL);
	zassert_false(out.resistance_valid, NULL);
	zassert_equal(out.state, PROFILE_FEC_STATE_IN_USE, NULL);
	zassert_true(out.lap_toggle, NULL);

	/* Negative incline, the descent case, and the one a two's complement
	 * mistake turns into +655 %. */
	in.incline_centi_pct = -250;
	zassert_equal(profile_fec_encode_settings(&in, body), 0, NULL);
	zassert_equal(profile_fec_decode_settings(body, &out), 0, NULL);
	zassert_equal(out.incline_centi_pct, -250, NULL);

	/* SS10.1.2.1's +-100.00 % is refused by the codec rather than
	 * truncated - a truncated incline is a number a head unit displays
	 * without complaint. */
	in.incline_centi_pct = 10001;
	zassert_equal(profile_fec_encode_settings(&in, body), -EINVAL, NULL);
}

/* ── Page 16, against the decoder that already exists ───────────────────── */

ZTEST(profile_fec_tx, test_general_page_round_trips_through_the_decoder)
{
	struct profile_fec_general in;
	struct profile_fec_general out;
	uint8_t                    body[8];

	memset(&in, 0, sizeof(in));
	in.equipment_type = PROFILE_FEC_TYPE_TREADMILL;
	in.elapsed_time_qs = 40u;  /* 10.0 s */
	in.distance_m = 100u;
	in.distance_valid = true;
	in.speed_mm_s = 3000u;     /* 3.000 m/s, a brisk run */
	in.speed_valid = true;
	in.speed_virtual = false;
	in.heart_rate_bpm = 150u;
	in.heart_rate_valid = true;
	in.hr_source = PROFILE_FEC_HR_SRC_ANTPLUS;
	in.state = PROFILE_FEC_STATE_IN_USE;
	in.lap_toggle = false;

	zassert_equal(profile_fec_encode_general(&in, body), 0, NULL);
	zassert_equal(body[0], PROFILE_FEC_PAGE_GENERAL, NULL);
	zassert_equal(body[1], PROFILE_FEC_TYPE_TREADMILL, NULL);
	zassert_equal(body[4], 0xB8u, NULL); /* 3000 = 0x0BB8 LE */
	zassert_equal(body[5], 0x0Bu, NULL);
	/* caps 0x5 = HR source 1 + distance enabled; state 3 in the high
	 * nibble, lap toggle clear. */
	zassert_equal(body[7], 0x35u, NULL);

	zassert_equal(profile_fec_decode_general(body, &out), 0, NULL);
	zassert_equal(out.equipment_type, in.equipment_type, NULL);
	zassert_equal(out.elapsed_time_qs, in.elapsed_time_qs, NULL);
	zassert_equal(out.distance_m, in.distance_m, NULL);
	zassert_true(out.distance_valid, NULL);
	zassert_equal(out.speed_mm_s, in.speed_mm_s, NULL);
	zassert_true(out.speed_valid, NULL);
	zassert_equal(out.heart_rate_bpm, in.heart_rate_bpm, NULL);
	zassert_equal(out.hr_source, PROFILE_FEC_HR_SRC_ANTPLUS, NULL);
	zassert_equal(out.state, PROFILE_FEC_STATE_IN_USE, NULL);
	zassert_false(out.lap_toggle, NULL);
}

ZTEST(profile_fec_tx, test_general_page_sentinels_come_from_the_flags)
{
	struct profile_fec_general in;
	struct profile_fec_general out;
	uint8_t                    body[8];

	memset(&in, 0, sizeof(in));
	in.equipment_type = PROFILE_FEC_TYPE_TREADMILL;
	in.distance_m = 255u;      /* a real 255 m */
	in.distance_valid = false; /* ... that the machine does not report */
	in.speed_mm_s = 1234u;
	in.speed_valid = false;
	in.heart_rate_bpm = 60u;
	in.heart_rate_valid = false;
	in.state = PROFILE_FEC_STATE_READY;

	zassert_equal(profile_fec_encode_general(&in, body), 0, NULL);
	zassert_equal(body[4], 0xFFu, NULL);
	zassert_equal(body[5], 0xFFu, NULL);
	zassert_equal(body[6], 0xFFu, NULL);
	/* SS8.5.2.3: distance has no invalid value, only an enable bit. Byte 3
	 * goes out unchanged; zeroing it here would be inventing a sentinel
	 * the document does not have. */
	zassert_equal(body[3], 255u, NULL);
	zassert_equal(body[7] & (1u << 2), 0u, "capabilities bit 2 must be clear");

	zassert_equal(profile_fec_decode_general(body, &out), 0, NULL);
	zassert_false(out.speed_valid, NULL);
	zassert_false(out.heart_rate_valid, NULL);
	zassert_false(out.distance_valid, NULL);
}

ZTEST(profile_fec_tx, test_fe_state_nibble_inside_a_nibble)
{
	struct profile_fec_general in;
	struct profile_fec_general out;
	uint8_t                    body[8];

	memset(&in, 0, sizeof(in));
	in.equipment_type = PROFILE_FEC_TYPE_TREADMILL;
	in.state = PROFILE_FEC_STATE_IN_USE;
	in.lap_toggle = true;
	in.hr_source = PROFILE_FEC_HR_SRC_INVALID;

	zassert_equal(profile_fec_encode_general(&in, body), 0, NULL);
	/* Table 8-10: bits 0-2 of the HIGH nibble are the state, bit 3 of it
	 * is the lap toggle. IN_USE with the toggle set is 0xB in the high
	 * nibble, never 0x3 with the toggle written somewhere else. */
	zassert_equal((body[7] >> 4) & 0x0Fu, 0x0Bu, NULL);

	zassert_equal(profile_fec_decode_general(body, &out), 0, NULL);
	zassert_equal(out.state, PROFILE_FEC_STATE_IN_USE,
		      "the lap toggle must not leak into the state");
	zassert_true(out.lap_toggle, NULL);

	/* A caller handing in a state that does not fit three bits must not
	 * set the lap toggle as a side effect. */
	in.state = 0x0Fu;
	in.lap_toggle = false;
	zassert_equal(profile_fec_encode_general(&in, body), 0, NULL);
	zassert_equal(body[7] & 0x80u, 0u, NULL);
}

/* ── Pages 19, 54 and 71 ────────────────────────────────────────────────── */

ZTEST(profile_fec_tx, test_treadmill_page_round_trip)
{
	struct profile_fec_treadmill in;
	struct profile_fec_treadmill out;
	uint8_t                      body[8];

	memset(&in, 0, sizeof(in));
	in.cadence_strides_min = 85u; /* 170 steps per minute */
	in.cadence_valid = true;
	in.neg_vertical_dm = 12u;
	in.pos_vertical_dm = 47u;
	in.capabilities = PROFILE_FEC_TREADMILL_CAP_POS_VERTICAL;
	in.state = PROFILE_FEC_STATE_IN_USE;

	zassert_equal(profile_fec_encode_treadmill(&in, body), 0, NULL);
	zassert_equal(body[0], PROFILE_FEC_PAGE_TREADMILL, NULL);
	zassert_equal(body[1], 0xFFu, NULL);
	zassert_equal(body[2], 0xFFu, NULL);
	zassert_equal(body[3], 0xFFu, NULL);
	zassert_equal(body[4], 85u, NULL);

	zassert_equal(profile_fec_decode_treadmill(body, &out), 0, NULL);
	zassert_equal(out.cadence_strides_min, 85u, NULL);
	zassert_true(out.cadence_valid, NULL);
	zassert_equal(out.neg_vertical_dm, 12u, NULL);
	zassert_equal(out.pos_vertical_dm, 47u, NULL);
	zassert_equal(out.capabilities, PROFILE_FEC_TREADMILL_CAP_POS_VERTICAL,
		      NULL);
	zassert_equal(out.state, PROFILE_FEC_STATE_IN_USE, NULL);

	/* 0xFF cadence is the sentinel; 0 is a real standing rider. */
	in.cadence_valid = false;
	zassert_equal(profile_fec_encode_treadmill(&in, body), 0, NULL);
	zassert_equal(body[4], 0xFFu, NULL);
	in.cadence_valid = true;
	in.cadence_strides_min = 0u;
	zassert_equal(profile_fec_encode_treadmill(&in, body), 0, NULL);
	zassert_equal(profile_fec_decode_treadmill(body, &out), 0, NULL);
	zassert_true(out.cadence_valid, NULL);
	zassert_equal(out.cadence_strides_min, 0u, NULL);
}

ZTEST(profile_fec_tx, test_capabilities_declares_simulation_mode)
{
	struct profile_fec_capabilities in;
	struct profile_fec_capabilities out;
	uint8_t                         body[8];

	memset(&in, 0, sizeof(in));
	in.capabilities = PROFILE_FEC_CAP_SIMULATION;
	in.max_resistance_valid = false;

	zassert_equal(profile_fec_encode_capabilities(&in, body), 0, NULL);
	zassert_equal(body[0], PROFILE_FEC_PAGE_CAPABILITIES, NULL);
	zassert_equal(body[5], 0xFFu, NULL);
	zassert_equal(body[6], 0xFFu, NULL);
	/* Bit 2, and only bit 2. This is what unlocks page 51 - a controller
	 * that reads this byte with the bit clear never sends a track
	 * resistance command at all. */
	zassert_equal(body[7], 0x04u, NULL);

	zassert_equal(profile_fec_decode_capabilities(body, &out), 0, NULL);
	zassert_false(out.max_resistance_valid, NULL);
	zassert_true((out.capabilities & PROFILE_FEC_CAP_SIMULATION) != 0u,
		     NULL);
}

ZTEST(profile_fec_tx, test_command_status_echoes_the_command_bytes_in_place)
{
	struct profile_fec_cmd_status in;
	struct profile_fec_cmd_status out;
	uint8_t                       command[8];
	uint8_t                       body[8];

	/* A page 51 asking for +3.00 %: raw 20300 = 0x4F4C, at bytes 5-6. */
	memset(command, 0xFFu, sizeof(command));
	command[0] = PROFILE_FEC_PAGE_TRACK_RESISTANCE;
	command[5] = 0x4Cu;
	command[6] = 0x4Fu;
	command[7] = 0xFFu;

	memset(&in, 0, sizeof(in));
	in.last_command = PROFILE_FEC_PAGE_TRACK_RESISTANCE;
	in.sequence = 7u;
	in.status = PROFILE_FEC_CMD_STATUS_PASS;
	memcpy(in.data, &command[4], sizeof(in.data));

	zassert_equal(profile_fec_encode_cmd_status(&in, body), 0, NULL);
	zassert_equal(body[0], PROFILE_FEC_PAGE_CMD_STATUS, NULL);
	zassert_equal(body[1], PROFILE_FEC_PAGE_TRACK_RESISTANCE, NULL);
	zassert_equal(body[2], 7u, NULL);
	zassert_equal(body[3], PROFILE_FEC_CMD_STATUS_PASS, NULL);
	/* THE ECHO KEEPS THE COMMAND'S OWN BYTE POSITIONS. A controller reads
	 * the grade back at bytes 5-6, exactly where it wrote it; re-packing
	 * it to bytes 4-5 here is the mistake this case exists to catch. */
	zassert_equal(body[5], 0x4Cu, NULL);
	zassert_equal(body[6], 0x4Fu, NULL);

	zassert_equal(profile_fec_decode_cmd_status(body, &out), 0, NULL);
	zassert_equal(out.sequence, 7u, NULL);
	zassert_equal(out.status, PROFILE_FEC_CMD_STATUS_PASS, NULL);
	zassert_mem_equal(out.data, in.data, sizeof(out.data), NULL);
}

/* ── The control pages ──────────────────────────────────────────────────── */

ZTEST(profile_fec_tx, test_track_resistance_decodes_grade_and_keeps_byte_7)
{
	struct profile_fec_cmd_track_resistance out;
	uint8_t                                 body[8];

	memset(body, 0xFFu, sizeof(body));
	body[0] = PROFILE_FEC_PAGE_TRACK_RESISTANCE;
	body[5] = 0x4Cu; /* 20300 -> +3.00 % */
	body[6] = 0x4Fu;
	body[7] = 0x41u; /* a rolling resistance a treadmill must ignore */

	zassert_equal(profile_fec_decode_track_resistance(body, &out), 0, NULL);
	zassert_true(out.grade_valid, NULL);
	zassert_equal(out.grade_centi_pct, 300, NULL);
	zassert_equal(out.rolling_resistance, 0x41u,
		      "byte 7 is decoded and must simply not be acted on");
	zassert_true(out.rolling_resistance_valid, NULL);

	/* The flat-ground sentinel. */
	body[5] = 0xFFu;
	body[6] = 0xFFu;
	zassert_equal(profile_fec_decode_track_resistance(body, &out), 0, NULL);
	zassert_false(out.grade_valid, NULL);
	zassert_equal(out.grade_centi_pct, 0, NULL);
}

ZTEST(profile_fec_tx, test_wind_resistance_speed_is_biased_by_127)
{
	struct profile_fec_cmd_wind_resistance out;
	uint8_t                                body[8];

	memset(body, 0xFFu, sizeof(body));
	body[0] = PROFILE_FEC_PAGE_WIND_RESISTANCE;
	body[5] = 0x33u;
	body[6] = 127u; /* a still day, not 127 km/h */
	body[7] = 0x64u;

	zassert_equal(profile_fec_decode_wind_resistance(body, &out), 0, NULL);
	zassert_equal(out.wind_speed_kph, 0, NULL);
	zassert_true(out.wind_speed_valid, NULL);
	zassert_equal(out.coefficient_centi, 0x33u, NULL);
	zassert_equal(out.drafting_centi, 0x64u, NULL);

	body[6] = 0u; /* the strongest headwind the field can express */
	zassert_equal(profile_fec_decode_wind_resistance(body, &out), 0, NULL);
	zassert_equal(out.wind_speed_kph, -127, NULL);
}

ZTEST(profile_fec_tx, test_the_other_control_pages)
{
	struct profile_fec_cmd_basic_resistance basic;
	struct profile_fec_cmd_target_power     power;
	struct profile_fec_cmd_user_config      user;
	struct profile_fec_cmd_request          request;
	uint8_t                                 body[8];

	memset(body, 0xFFu, sizeof(body));
	body[0] = PROFILE_FEC_PAGE_BASIC_RESISTANCE;
	body[7] = 40u; /* 20.0 % in 0.5 % units */
	zassert_equal(profile_fec_decode_basic_resistance(body, &basic), 0,
		      NULL);
	zassert_equal(basic.resistance_half_pct, 40u, NULL);

	memset(body, 0xFFu, sizeof(body));
	body[0] = PROFILE_FEC_PAGE_TARGET_POWER;
	body[6] = 0x20u; /* 800 quarter-watts = 200 W */
	body[7] = 0x03u;
	zassert_equal(profile_fec_decode_target_power(body, &power), 0, NULL);
	zassert_equal(power.power_quarter_w, 800u, NULL);

	memset(body, 0xFFu, sizeof(body));
	body[0] = PROFILE_FEC_PAGE_USER_CONFIG;
	body[1] = 0x70u; /* 7500 x 0.01 kg = 75.00 kg */
	body[2] = 0x1Du;
	zassert_equal(profile_fec_decode_user_config(body, &user), 0, NULL);
	zassert_true(user.user_weight_valid, NULL);
	zassert_equal(user.user_weight_10g, 7536u, NULL);

	memset(body, 0xFFu, sizeof(body));
	body[0] = PROFILE_FEC_PAGE_REQUEST;
	body[5] = 0x02u; /* transmit twice, not acknowledged */
	body[6] = PROFILE_FEC_PAGE_CAPABILITIES;
	body[7] = PROFILE_FEC_REQUEST_CMD_DATA_PAGE;
	zassert_equal(profile_fec_decode_request(body, &request), 0, NULL);
	zassert_false(request.acknowledged, NULL);
	zassert_equal(request.tx_count, 2u, NULL);
	zassert_equal(request.page, PROFILE_FEC_PAGE_CAPABILITIES, NULL);

	body[5] = 0x80u; /* the ack bit, with a count of zero under it */
	zassert_equal(profile_fec_decode_request(body, &request), 0, NULL);
	zassert_true(request.acknowledged, NULL);
	zassert_equal(request.tx_count, 0u, NULL);
}

ZTEST(profile_fec_tx, test_decoders_decline_the_wrong_page_and_nulls)
{
	struct profile_fec_settings   settings;
	struct profile_fec_treadmill  treadmill;
	struct profile_fec_cmd_status status;
	uint8_t                       body[8];

	memset(body, 0u, sizeof(body));
	body[0] = PROFILE_FEC_PAGE_TREADMILL;
	zassert_equal(profile_fec_decode_settings(body, &settings), -EINVAL,
		      NULL);
	body[0] = PROFILE_FEC_PAGE_SETTINGS;
	zassert_equal(profile_fec_decode_treadmill(body, &treadmill), -EINVAL,
		      NULL);
	zassert_equal(profile_fec_decode_cmd_status(body, &status), -EINVAL,
		      NULL);

	zassert_equal(profile_fec_encode_general(NULL, body), -EINVAL, NULL);
	zassert_equal(profile_fec_decode_settings(NULL, &settings), -EINVAL,
		      NULL);
	zassert_equal(profile_fec_decode_treadmill(body, NULL), -EINVAL, NULL);
}

/* ── The interleave, which is the whole point of the master ─────────────── */

/*
 * SS10.1's rules are stated as "at least once every N messages", which is a
 * property of the WORST gap over a long run and not of any one cycle. 264
 * messages is a little over four common-page cycles (65 each), which is the
 * shortest run in which every rule has been exercised more than once.
 */
#define RUN_MESSAGES 264u

struct run_result {
	uint8_t  page[RUN_MESSAGES];
	uint32_t common_80_at[8];
	uint32_t n_common_80;
	uint32_t n_common_81;
	uint32_t consecutive_pairs;
};

static void run_master(struct profile_fec_tx *tx, struct run_result *r)
{
	uint8_t body[8];
	uint8_t previous = 0u;

	memset(r, 0, sizeof(*r));

	for (uint32_t i = 0; i < RUN_MESSAGES; i++) {
		enum profile_slot_kind kind = profile_fec_tx_next(tx, body);

		zassert_not_equal(kind, PROFILE_SLOT_IDLE,
				  "a master must fill every slot it is given");
		r->page[i] = body[0];

		if (body[0] == PROFILE_COMMON_PAGE_80) {
			zassert_equal(kind, PROFILE_SLOT_COMMON_80,
				      "the client-claimed pair must be "
				      "reported as a common slot, not as "
				      "PROFILE_SLOT_CLIENT");
			if (r->n_common_80 < ARRAY_SIZE(r->common_80_at)) {
				r->common_80_at[r->n_common_80] = i;
			}
			r->n_common_80++;
		} else if (body[0] == PROFILE_COMMON_PAGE_81) {
			zassert_equal(kind, PROFILE_SLOT_COMMON_81, NULL);
			r->n_common_81++;
			if (previous == PROFILE_COMMON_PAGE_80) {
				r->consecutive_pairs++;
			}
		}
		previous = body[0];
	}
}

/* The worst gap between successive occurrences of `page`, counting from before
 * the run started - so a page that never appears, or appears late, fails. */
static uint32_t worst_gap(const struct run_result *r, uint8_t page)
{
	uint32_t last = 0u;
	uint32_t worst = 0u;
	bool     seen = false;

	for (uint32_t i = 0; i < RUN_MESSAGES; i++) {
		if (r->page[i] != page) {
			continue;
		}
		if (seen && (i - last) > worst) {
			worst = i - last;
		}
		if (!seen && (i + 1u) > worst) {
			worst = i + 1u; /* how long the first one took */
		}
		seen = true;
		last = i;
	}
	return seen ? worst : RUN_MESSAGES;
}

static void init_master(struct profile_fec_tx *tx, bool metabolic)
{
	struct profile_fec_tx_cfg cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.id.hw_revision = 1u;
	cfg.id.manufacturer_id = 255u; /* the ANT+ development id */
	cfg.id.model_number = 1u;
	cfg.id.sw_revision_main = 1u;
	cfg.id.sw_revision_supplemental = PROFILE_COMMON_INVALID_U8;
	cfg.id.serial_number = PROFILE_COMMON_INVALID_U32;
	cfg.equipment_type = PROFILE_FEC_TYPE_TREADMILL;
	cfg.caps.capabilities = PROFILE_FEC_CAP_SIMULATION;
	cfg.metabolic = metabolic;

	zassert_equal(profile_fec_tx_init(tx, &cfg), 0, NULL);
}

ZTEST(profile_fec_tx, test_interleave_meets_every_one_of_section_10_1s_rules)
{
	struct profile_fec_tx tx;
	struct run_result     r;

	init_master(&tx, false);
	run_master(&tx, &r);

	zassert_true(worst_gap(&r, PROFILE_FEC_PAGE_GENERAL) <=
			     PROFILE_FEC_TX_MAX_GAP_16,
		     "page 16 must appear at least once every 5 messages");
	zassert_true(worst_gap(&r, PROFILE_FEC_PAGE_TREADMILL) <=
			     PROFILE_FEC_TX_MAX_GAP_19,
		     "the equipment-specific page must appear at least once "
		     "every 5 messages");
	zassert_true(worst_gap(&r, PROFILE_FEC_PAGE_SETTINGS) <=
			     PROFILE_FEC_TX_MAX_GAP_SETTINGS,
		     "page 17 must appear at least once every 20 messages");

	/* Page 16 twice consecutively, which is the other half of the first
	 * rule and the half a "once every 5th" implementation would miss. */
	{
		bool found_pair = false;

		for (uint32_t i = 1; i < RUN_MESSAGES; i++) {
			if (r.page[i] == PROFILE_FEC_PAGE_GENERAL &&
			    r.page[i - 1u] == PROFILE_FEC_PAGE_GENERAL) {
				found_pair = true;
				break;
			}
		}
		zassert_true(found_pair, NULL);
	}
}

ZTEST(profile_fec_tx, test_common_pages_are_consecutive_and_inside_66)
{
	struct profile_fec_tx tx;
	struct run_result     r;

	init_master(&tx, false);
	run_master(&tx, &r);

	zassert_true(r.n_common_80 >= 4u, "264 messages is four cycles of 65");
	zassert_equal(r.n_common_80, r.n_common_81,
		      "80 and 81 go out as a pair or not at all");
	zassert_equal(r.consecutive_pairs, r.n_common_81,
		      "SS10.1 requires the two to be CONSECUTIVE background "
		      "pages, not merely both present");

	for (uint32_t i = 1; i < r.n_common_80 &&
			     i < ARRAY_SIZE(r.common_80_at); i++) {
		uint32_t gap = r.common_80_at[i] - r.common_80_at[i - 1u];

		zassert_equal(gap, PROFILE_FEC_TX_COMMON_INTERVAL, NULL);
		zassert_true(gap <= 66u,
			     "the pair is due at least every 66 messages, and "
			     "profile_sched.c's own 121 is the wrong cadence "
			     "for this profile");
	}
}

ZTEST(profile_fec_tx, test_metabolic_page_shares_the_settings_budget)
{
	struct profile_fec_tx tx;
	struct run_result     r;

	init_master(&tx, true);
	run_master(&tx, &r);

	/* With page 18 enabled the two alternate in one slot, so each is due
	 * half as often - and both must still fit inside 20 messages. */
	zassert_true(worst_gap(&r, PROFILE_FEC_PAGE_SETTINGS) <=
			     PROFILE_FEC_TX_MAX_GAP_SETTINGS,
		     NULL);
	zassert_true(worst_gap(&r, PROFILE_FEC_PAGE_METABOLIC) <=
			     PROFILE_FEC_TX_MAX_GAP_SETTINGS,
		     NULL);

	/* And the two 5-message rules are unaffected by the extra page. */
	zassert_true(worst_gap(&r, PROFILE_FEC_PAGE_GENERAL) <=
			     PROFILE_FEC_TX_MAX_GAP_16, NULL);
	zassert_true(worst_gap(&r, PROFILE_FEC_PAGE_TREADMILL) <=
			     PROFILE_FEC_TX_MAX_GAP_19, NULL);
}

ZTEST(profile_fec_tx, test_command_status_preempts_without_breaking_the_rules)
{
	struct profile_fec_tx tx;
	struct run_result     r;
	uint8_t               echo[4] = { 0xFFu, 0x4Cu, 0x4Fu, 0xFFu };
	uint8_t               body[8];
	bool                  saw_status = false;

	init_master(&tx, false);

	/* A controller sending a track-resistance command every few messages,
	 * which is the worst realistic case for the interleave. */
	memset(&r, 0, sizeof(r));
	for (uint32_t i = 0; i < RUN_MESSAGES; i++) {
		if ((i % 17u) == 0u) {
			profile_fec_tx_command_answered(
				&tx, PROFILE_FEC_PAGE_TRACK_RESISTANCE,
				(uint8_t)i, PROFILE_FEC_CMD_STATUS_PASS, echo);
		}
		zassert_not_equal(profile_fec_tx_next(&tx, body),
				  PROFILE_SLOT_IDLE, NULL);
		r.page[i] = body[0];
		if (body[0] == PROFILE_FEC_PAGE_CMD_STATUS) {
			saw_status = true;
			zassert_equal(body[5], 0x4Cu, NULL);
			zassert_equal(body[6], 0x4Fu, NULL);
		}
	}

	zassert_true(saw_status, "every answered command owes a page 71");
	zassert_true(worst_gap(&r, PROFILE_FEC_PAGE_GENERAL) <=
			     PROFILE_FEC_TX_MAX_GAP_16,
		     "page 71 must never displace a page 16 slot");
	zassert_true(worst_gap(&r, PROFILE_FEC_PAGE_TREADMILL) <=
			     PROFILE_FEC_TX_MAX_GAP_19,
		     "page 71 must never take the page 19 that keeps the "
		     "5-message rule");
}

ZTEST(profile_fec_tx, test_on_request_delivers_page_54_and_refuses_the_rest)
{
	struct profile_fec_tx tx;
	uint8_t               body[8];
	uint32_t              seen = 0u;

	init_master(&tx, false);

	zassert_equal(profile_fec_tx_request(&tx, PROFILE_FEC_PAGE_CAPABILITIES,
					     2u),
		      0, NULL);
	/* Pages already in the rotation are answered by waiting, and saying so
	 * lets the caller send the page 71 the document wants instead of
	 * silently scheduling a duplicate. */
	zassert_equal(profile_fec_tx_request(&tx, PROFILE_FEC_PAGE_TREADMILL,
					     1u),
		      -ENOTSUP, NULL);

	for (uint32_t i = 0; i < 40u; i++) {
		(void)profile_fec_tx_next(&tx, body);
		if (body[0] == PROFILE_FEC_PAGE_CAPABILITIES) {
			seen++;
			zassert_equal(body[7], PROFILE_FEC_CAP_SIMULATION, NULL);
		}
	}
	zassert_equal(seen, 2u, "requested twice, delivered twice");
}

/* ---------------------------------------------------------------------------
 * The golden vectors, which are the same bytes tools/vectors/treadmill-pages.antcap
 * holds and tools/test_compat_capture.py asserts from the Python side.
 * ---------------------------------------------------------------------------
 *
 * Every other case in this file proves the encoder agrees with the decoder
 * beside it, which is a claim about two functions written together. These are
 * hand computed from the scalings profile_fec_tx.h states, committed to a file,
 * and asserted from BOTH implementations - so a change that breaks one and not
 * the other is a disagreement between C and Python rather than a rewrite of
 * both at once.
 *
 * If these ever have to change, change the .antcap and the Python case in the
 * same commit and say in the message which reading of the document moved.
 */

static const uint8_t golden_page_16[8] = {
	0x10u, 0x13u, 0x28u, 0x64u, 0xB8u, 0x0Bu, 0x96u, 0x35u
};
static const uint8_t golden_page_17[8] = {
	0x11u, 0xFFu, 0xFFu, 0x78u, 0x2Cu, 0x01u, 0xFFu, 0x30u
};
static const uint8_t golden_page_19[8] = {
	0x13u, 0xFFu, 0xFFu, 0xFFu, 0x55u, 0x0Cu, 0x2Fu, 0x33u
};
static const uint8_t golden_page_54[8] = {
	0x36u, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x04u
};
static const uint8_t golden_page_51[8] = {
	0x33u, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x4Cu, 0x4Fu, 0xFFu
};
static const uint8_t golden_page_71[8] = {
	0x47u, 0x33u, 0x07u, 0x00u, 0xFFu, 0x4Cu, 0x4Fu, 0xFFu
};

ZTEST(profile_fec_tx, test_golden_vectors)
{
	struct profile_fec_general      g;
	struct profile_fec_settings     s;
	struct profile_fec_treadmill    t;
	struct profile_fec_capabilities c;
	struct profile_fec_cmd_status   st;
	uint8_t                         body[8];

	memset(&g, 0, sizeof(g));
	g.equipment_type = PROFILE_FEC_TYPE_TREADMILL;
	g.elapsed_time_qs = 40u;
	g.distance_m = 100u;
	g.distance_valid = true;
	g.speed_mm_s = 3000u;
	g.speed_valid = true;
	g.heart_rate_bpm = 150u;
	g.heart_rate_valid = true;
	g.hr_source = PROFILE_FEC_HR_SRC_ANTPLUS;
	g.state = PROFILE_FEC_STATE_IN_USE;
	zassert_equal(profile_fec_encode_general(&g, body), 0, NULL);
	zassert_mem_equal(body, golden_page_16, sizeof(body), NULL);

	memset(&s, 0, sizeof(s));
	s.cycle_length_cm = 120u;
	s.cycle_length_valid = true;
	s.incline_centi_pct = 300;
	s.incline_valid = true;
	s.resistance_valid = false;
	s.state = PROFILE_FEC_STATE_IN_USE;
	zassert_equal(profile_fec_encode_settings(&s, body), 0, NULL);
	zassert_mem_equal(body, golden_page_17, sizeof(body), NULL);

	memset(&t, 0, sizeof(t));
	t.cadence_strides_min = 85u;
	t.cadence_valid = true;
	t.neg_vertical_dm = 12u;
	t.pos_vertical_dm = 47u;
	t.capabilities = PROFILE_FEC_TREADMILL_CAP_NEG_VERTICAL |
			 PROFILE_FEC_TREADMILL_CAP_POS_VERTICAL;
	t.state = PROFILE_FEC_STATE_IN_USE;
	zassert_equal(profile_fec_encode_treadmill(&t, body), 0, NULL);
	zassert_mem_equal(body, golden_page_19, sizeof(body), NULL);

	memset(&c, 0, sizeof(c));
	c.capabilities = PROFILE_FEC_CAP_SIMULATION;
	c.max_resistance_valid = false;
	zassert_equal(profile_fec_encode_capabilities(&c, body), 0, NULL);
	zassert_mem_equal(body, golden_page_54, sizeof(body), NULL);

	memset(&st, 0, sizeof(st));
	st.last_command = PROFILE_FEC_PAGE_TRACK_RESISTANCE;
	st.sequence = 7u;
	st.status = PROFILE_FEC_CMD_STATUS_PASS;
	memcpy(st.data, &golden_page_51[4], sizeof(st.data));
	zassert_equal(profile_fec_encode_cmd_status(&st, body), 0, NULL);
	zassert_mem_equal(body, golden_page_71, sizeof(body), NULL);
}

ZTEST(profile_fec_tx, test_the_reported_incline_matches_the_commanded_grade)
{
	struct profile_fec_cmd_track_resistance cmd;
	struct profile_fec_settings             s;

	/*
	 * SS8.8.4: "If data page 17 is used by the controllable FE, the incline
	 * reported should match the grade received in this command page."
	 *
	 * Asserted from the WIRE BYTES of the two golden pages rather than from
	 * a variable that happens to hold both, because the two encodings are
	 * different - grade is an unsigned u16 biased by -200.00 %, incline is
	 * a plain sint16 - and a test that compared one variable to itself
	 * would pass with either encoding wrong.
	 */
	zassert_equal(profile_fec_decode_track_resistance(golden_page_51, &cmd),
		      0, NULL);
	zassert_equal(profile_fec_decode_settings(golden_page_17, &s), 0, NULL);

	zassert_true(cmd.grade_valid, NULL);
	zassert_true(s.incline_valid, NULL);
	zassert_equal(cmd.grade_centi_pct, s.incline_centi_pct,
		      "the reported incline must equal the commanded grade");
	zassert_equal(cmd.grade_centi_pct, 300, NULL);

	/* And the bytes are NOT the same, which is the whole trap: 0x4F4C in
	 * the command against 0x012C in the report. */
	zassert_not_equal(golden_page_51[5], golden_page_17[4], NULL);
	zassert_not_equal(golden_page_51[6], golden_page_17[5], NULL);
}

ZTEST(profile_fec_tx, test_master_declines_null_arguments)
{
	struct profile_fec_tx tx;
	uint8_t               body[8];

	zassert_equal(profile_fec_tx_init(NULL, NULL), -EINVAL, NULL);
	init_master(&tx, false);
	zassert_equal(profile_fec_tx_next(NULL, body), PROFILE_SLOT_IDLE, NULL);
	zassert_equal(profile_fec_tx_next(&tx, NULL), PROFILE_SLOT_IDLE, NULL);
	zassert_equal(profile_fec_tx_messages(NULL), 0u, NULL);
}

ZTEST_SUITE(profile_fec_tx, NULL, NULL, NULL, NULL, NULL);
