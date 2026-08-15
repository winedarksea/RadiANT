/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work against src/profiles/profile_fec.h/.c,
 * which transcribes D000001231 "ANT+ Device Profile - Fitness Equipment"
 * Rev 5.0. Every vector below is hand-computed from a named table in that
 * document, and the comment on each says which - a vector produced by running
 * the decoder would only prove the decoder agrees with itself.
 *
 * What earns a case here is the four traps profile_fec.h names, because each
 * has a plausible wrong reading that produces a legal-looking number:
 * the state nibble inside a nibble (Table 8-10), the 0xFFF instantaneous power
 * that also condemns the accumulator (Table 8-25), distance being gated by an
 * enable bit rather than a sentinel (section 8.5.2.3), and virtual speed being
 * a flag on a valid reading rather than an invalid one (section 8.5.2.6.2).
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include "profile_fec.h"

/*
 * Table 8-7, a trainer mid-workout:
 *   [0] 0x10  page 16
 *   [1] 0x19  equipment type 25 = trainer/stationary bike (Table 8-8)
 *   [2] 0x28  elapsed time 40 quarter-seconds = 10.0 s
 *   [3] 0x64  distance 100 m
 *   [4] 0x88  speed 0x1388 = 5000 x 0.001 m/s = 5.000 m/s
 *   [5] 0x13
 *   [6] 0x50  heart rate 80 bpm
 *   [7] 0x35  caps nibble 0x5 = HR source 1 (ANT+) + distance enabled (bit 2),
 *             virtual-speed bit clear; state nibble 0x3 = IN_USE, lap toggle 0
 */
static const uint8_t fec_general_in_use[8] = {
	0x10u, 0x19u, 0x28u, 0x64u, 0x88u, 0x13u, 0x50u, 0x35u
};

/*
 * Table 8-25, the same trainer at 200 W:
 *   [0] 0x19  page 25
 *   [1] 0x07  update event count 7
 *   [2] 0x55  cadence 85 rpm
 *   [3] 0xE8  accumulated power 0x03E8 = 1000 W (a SUM OF WATT SAMPLES)
 *   [4] 0x03
 *   [5] 0xC8  instantaneous power low byte
 *   [6] 0x00  bits 0-3 instantaneous power high nibble -> 0x0C8 = 200 W;
 *             bits 4-7 trainer status 0 = nothing needs calibration
 *   [7] 0x30  flags nibble 0x0; state nibble 0x3 = IN_USE, lap toggle 0
 */
static const uint8_t fec_trainer_200w[8] = {
	0x19u, 0x07u, 0x55u, 0xE8u, 0x03u, 0xC8u, 0x00u, 0x30u
};

ZTEST(profile_fec, test_general_page_fields)
{
	struct profile_fec_general g;

	zassert_equal(profile_fec_decode_general(fec_general_in_use, &g), 0,
		      NULL);

	zassert_equal(g.equipment_type, PROFILE_FEC_TYPE_TRAINER, NULL);
	zassert_equal(g.elapsed_time_qs, 40u, "0.25 s units, not seconds");
	zassert_equal(g.distance_m, 100u, NULL);
	zassert_true(g.distance_valid, "capabilities bit 2 is set");
	zassert_equal(g.speed_mm_s, 5000u, NULL);
	zassert_true(g.speed_valid, NULL);
	zassert_false(g.speed_virtual, NULL);
	zassert_equal(g.heart_rate_bpm, 80u, NULL);
	zassert_true(g.heart_rate_valid, NULL);
	zassert_equal(g.hr_source, PROFILE_FEC_HR_SRC_ANTPLUS, NULL);
	zassert_equal(g.state, PROFILE_FEC_STATE_IN_USE, NULL);
	zassert_false(g.lap_toggle, NULL);
}

ZTEST(profile_fec, test_equipment_type_reserved_bits_are_masked)
{
	uint8_t body[8];
	struct profile_fec_general g;

	memcpy(body, fec_general_in_use, sizeof(body));
	/* Table 8-8: bits 5-7 are "Do Not Interpret". Setting all three must
	 * not change the reported type - reading the whole byte would give
	 * 0xF9 = 249 and match no defined equipment. */
	body[1] = (uint8_t)(0x19u | 0xE0u);

	zassert_equal(profile_fec_decode_general(body, &g), 0, NULL);
	zassert_equal(g.equipment_type, PROFILE_FEC_TYPE_TRAINER, NULL);
}

ZTEST(profile_fec, test_lap_toggle_is_bit_3_of_the_state_nibble)
{
	uint8_t body[8];
	struct profile_fec_general g;

	memcpy(body, fec_general_in_use, sizeof(body));
	/* Table 8-10: within byte 7's high nibble, bits 0-2 are the state and
	 * bit 3 is the lap toggle. 0xB5's high nibble is 0b1011: still IN_USE
	 * (3), with the toggle set. A decoder taking the whole nibble as the
	 * state would read 11, which is reserved. */
	body[7] = 0xB5u;

	zassert_equal(profile_fec_decode_general(body, &g), 0, NULL);
	zassert_equal(g.state, PROFILE_FEC_STATE_IN_USE,
		      "the lap toggle must not leak into the state");
	zassert_true(g.lap_toggle, NULL);
}

ZTEST(profile_fec, test_general_page_instantaneous_sentinels)
{
	uint8_t body[8];
	struct profile_fec_general g;

	memcpy(body, fec_general_in_use, sizeof(body));
	body[4] = 0xFFu; /* Table 8-7: 0xFFFF speed is invalid */
	body[5] = 0xFFu;
	body[6] = 0xFFu; /* 0xFF heart rate is invalid */

	zassert_equal(profile_fec_decode_general(body, &g), 0, NULL);
	zassert_false(g.speed_valid, NULL);
	zassert_false(g.heart_rate_valid, NULL);
	/* Still a legal distance and a legal elapsed time - the sentinels are
	 * per field, and 255 is not one of them anywhere on this page. */
	zassert_true(g.distance_valid, NULL);
	zassert_equal(g.distance_m, 100u, NULL);
}

ZTEST(profile_fec, test_distance_is_gated_by_a_bit_not_a_sentinel)
{
	uint8_t body[8];
	struct profile_fec_general g;

	memcpy(body, fec_general_in_use, sizeof(body));
	body[3] = 0xFFu;             /* 255 m: a real distance, per 8.5.2.3 */
	body[7] = 0x31u;             /* caps 0x1: distance-enabled bit CLEAR */

	zassert_equal(profile_fec_decode_general(body, &g), 0, NULL);
	zassert_false(g.distance_valid,
		      "capabilities bit 2 clear means byte 3 is meaningless");

	/* And the mirror image: 255 with the bit set is 255 metres, not
	 * "absent". This is the reading a sentinel habit gets wrong. */
	body[7] = 0x35u;
	zassert_equal(profile_fec_decode_general(body, &g), 0, NULL);
	zassert_true(g.distance_valid, NULL);
	zassert_equal(g.distance_m, 255u, NULL);
}

ZTEST(profile_fec, test_virtual_speed_is_a_flag_on_a_valid_reading)
{
	uint8_t body[8];
	struct profile_fec_general g;

	memcpy(body, fec_general_in_use, sizeof(body));
	body[7] = 0x3Du; /* caps 0xD = 0x5 | bit 3 (virtual speed) */

	zassert_equal(profile_fec_decode_general(body, &g), 0, NULL);
	zassert_true(g.speed_valid, "virtual speed is not invalid speed");
	zassert_true(g.speed_virtual, NULL);
	zassert_equal(g.speed_mm_s, 5000u, NULL);
}

ZTEST(profile_fec, test_trainer_page_fields)
{
	struct profile_fec_trainer t;

	zassert_equal(profile_fec_decode_trainer(fec_trainer_200w, &t), 0, NULL);

	zassert_equal(t.event_count, 7u, NULL);
	zassert_equal(t.cadence_rpm, 85u, NULL);
	zassert_true(t.cadence_valid, NULL);
	zassert_equal(t.acc_power_w, 1000u, "bytes 3-4 LE, one earlier than 0x0B's");
	zassert_equal(t.inst_power_w, 200u, "byte 5 + byte 6's LOW nibble");
	zassert_true(t.power_valid, NULL);
	zassert_equal(t.trainer_status, 0u, NULL);
	zassert_equal(t.flags, PROFILE_FEC_TARGET_POWER_OK, NULL);
	zassert_equal(t.state, PROFILE_FEC_STATE_IN_USE, NULL);
	zassert_false(t.lap_toggle, NULL);
}

ZTEST(profile_fec, test_trainer_power_uses_twelve_bits_across_two_bytes)
{
	uint8_t body[8];
	struct profile_fec_trainer t;

	memcpy(body, fec_trainer_200w, sizeof(body));
	/* 4094 W, the field's maximum legal value per Table 8-25 (0xFFF is the
	 * sentinel and 0xFFE = 4094 is the top real reading). 4094 = 0xFFE, so
	 * byte 5 = 0xFE and byte 6's low nibble = 0xF. Byte 6's HIGH nibble
	 * carries the trainer status and must survive untouched. */
	body[5] = 0xFEu;
	body[6] = 0x3Fu; /* status 0x3 = power cal + resistance cal required */

	zassert_equal(profile_fec_decode_trainer(body, &t), 0, NULL);
	zassert_equal(t.inst_power_w, 4094u, NULL);
	zassert_true(t.power_valid, "4094 is not the sentinel; 4095 is");
	zassert_equal(t.trainer_status,
		      PROFILE_FEC_TRAINER_STATUS_POWER_CAL |
			      PROFILE_FEC_TRAINER_STATUS_RESISTANCE_CAL,
		      NULL);
}

ZTEST(profile_fec, test_invalid_instantaneous_power_condemns_the_accumulator)
{
	uint8_t body[8];
	struct profile_fec_trainer t;

	memcpy(body, fec_trainer_200w, sizeof(body));
	/* Table 8-25: "0xFFF indicates BOTH the instantaneous and accumulated
	 * power fields are invalid". Bytes 3-4 still read 1000 on the wire and
	 * a caller must not believe them. */
	body[5] = 0xFFu;
	body[6] = 0x0Fu;

	zassert_equal(profile_fec_decode_trainer(body, &t), 0, NULL);
	zassert_false(t.power_valid, NULL);
	zassert_equal(t.acc_power_w, 1000u,
		      "the raw value is still reported; power_valid is the gate");
}

ZTEST(profile_fec, test_trainer_cadence_sentinel)
{
	uint8_t body[8];
	struct profile_fec_trainer t;

	memcpy(body, fec_trainer_200w, sizeof(body));
	body[2] = 0xFFu; /* section 8.6.7.2: cannot measure cadence */

	zassert_equal(profile_fec_decode_trainer(body, &t), 0, NULL);
	zassert_false(t.cadence_valid, NULL);

	/* Zero is NOT the sentinel - a stopped rider is a real 0 rpm. */
	body[2] = 0x00u;
	zassert_equal(profile_fec_decode_trainer(body, &t), 0, NULL);
	zassert_true(t.cadence_valid, NULL);
	zassert_equal(t.cadence_rpm, 0u, NULL);
}

ZTEST(profile_fec, test_each_decoder_declines_the_other_page)
{
	struct profile_fec_general g;
	struct profile_fec_trainer t;

	/* The pages carry different quantities at every offset, so decoding
	 * one as the other would report a cadence as a distance. */
	zassert_equal(profile_fec_decode_general(fec_trainer_200w, &g), -EINVAL,
		      NULL);
	zassert_equal(profile_fec_decode_trainer(fec_general_in_use, &t),
		      -EINVAL, NULL);
}

ZTEST(profile_fec, test_null_arguments_are_declined)
{
	struct profile_fec_general g;
	struct profile_fec_trainer t;

	zassert_equal(profile_fec_decode_general(NULL, &g), -EINVAL, NULL);
	zassert_equal(profile_fec_decode_general(fec_general_in_use, NULL),
		      -EINVAL, NULL);
	zassert_equal(profile_fec_decode_trainer(NULL, &t), -EINVAL, NULL);
	zassert_equal(profile_fec_decode_trainer(fec_trainer_200w, NULL),
		      -EINVAL, NULL);
}

ZTEST_SUITE(profile_fec, NULL, NULL, NULL, NULL, NULL);
