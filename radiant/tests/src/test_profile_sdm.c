/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include "profile_common.h"
#include "profile_sched.h"
#include "profile_sdm.h"

ZTEST(profile_sdm, test_the_channel_parameters_are_the_profiles_and_not_a_choice)
{
	/* Trap 1. Asserted as a value rather than trusted as a constant,
	 * because the failure mode of a wrong period is a channel that never
	 * opens with nothing anywhere naming the period as the reason - and
	 * 8070 (heart rate) and 8192 (FE-C) are both one digit away. */
	zassert_equal(PROFILE_SDM_PERIOD, 8134u, NULL);
	zassert_equal(PROFILE_SDM_DEVICE_TYPE, 0x7Cu, NULL);
	zassert_equal(PROFILE_SDM_TRANS_TYPE, 5u, NULL);

	/* Trap 3, as an assertion rather than as a comment: the summary page
	 * is 16 DECIMAL and the capabilities page is 22 DECIMAL, and the two
	 * hex values are each other's digits reversed. */
	zassert_equal(PROFILE_SDM_PAGE_SUMMARY, 0x10u, NULL);
	zassert_equal(PROFILE_SDM_PAGE_CAPABILITIES, 0x16u, NULL);
}

ZTEST(profile_sdm, test_status_byte_packs_four_two_bit_fields)
{
	uint8_t location;
	uint8_t battery;
	uint8_t health;
	uint8_t use_state;
	uint8_t status;

	status = profile_sdm_status(PROFILE_SDM_LOC_OTHER,
				    PROFILE_SDM_BATTERY_NEW,
				    PROFILE_SDM_HEALTH_OK,
				    PROFILE_SDM_USE_ACTIVE);
	/* location 2 in bits 7..6, everything else 0 but the use state. */
	zassert_equal(status, 0x81u, NULL);

	profile_sdm_status_split(status, &location, &battery, &health,
				 &use_state);
	zassert_equal(location, PROFILE_SDM_LOC_OTHER,
		      "a treadmill is not on anybody's shoe");
	zassert_equal(battery, PROFILE_SDM_BATTERY_NEW, NULL);
	zassert_equal(health, PROFILE_SDM_HEALTH_OK, NULL);
	zassert_equal(use_state, PROFILE_SDM_USE_ACTIVE, NULL);

	/* Each field is two bits and each is masked, so an out-of-range value
	 * cannot bleed into its neighbour - which is the failure that turns a
	 * health warning into a low battery. */
	status = profile_sdm_status(0xFFu, 0u, 0u, 0u);
	zassert_equal(status, 0xC0u, NULL);
}

ZTEST(profile_sdm, test_speed_splits_into_a_nibble_and_a_256th)
{
	uint8_t  int_mps;
	uint8_t  frac_256;

	/* 3.000 m/s: three whole metres and no fraction. */
	zassert_equal(profile_sdm_speed_split(3000u, &int_mps, &frac_256), 0,
		      NULL);
	zassert_equal(int_mps, 3u, NULL);
	zassert_equal(frac_256, 0u, NULL);

	/* 3.500 m/s: 0.5 x 256 = 128 exactly. */
	zassert_equal(profile_sdm_speed_split(3500u, &int_mps, &frac_256), 0,
		      NULL);
	zassert_equal(int_mps, 3u, NULL);
	zassert_equal(frac_256, 128u, NULL);

	/* The integer part is a NIBBLE. Anything above 15.996 m/s is refused
	 * rather than wrapped: a wrapped 16 m/s is a stopped treadmill on a
	 * receiver's screen. */
	zassert_equal(profile_sdm_speed_split(PROFILE_SDM_SPEED_MAX_MM_S + 1u,
					      &int_mps, &frac_256),
		      -EINVAL, NULL);

	/* Round-trip to within the field's own resolution (1/256 m/s is
	 * ~3.9 mm/s, so a millimetre-per-second input cannot survive exactly
	 * and demanding that it does would be testing the wrong thing). */
	for (uint32_t mm = 0; mm <= 6000u; mm += 137u) {
		uint32_t back;

		zassert_equal(profile_sdm_speed_split(mm, &int_mps, &frac_256),
			      0, NULL);
		back = profile_sdm_speed_mm_s(int_mps, frac_256);
		zassert_true(back <= mm && (mm - back) <= 4u,
			     "1/256 m/s is ~3.9 mm/s of resolution");
	}
}

ZTEST(profile_sdm, test_default_page_layout_and_round_trip)
{
	struct profile_sdm_default in;
	struct profile_sdm_default out;
	uint8_t                    body[8];

	memset(&in, 0, sizeof(in));
	in.time_frac_200 = 100u; /* 0.5 s */
	in.time_s = 42u;
	in.distance_m = 200u;
	in.distance_frac_16 = 8u; /* 0.5 m */
	in.speed_int_mps = 3u;
	in.speed_frac_256 = 128u;
	in.strides = 77u;
	in.latency_32 = 4u; /* 0.125 s */

	zassert_equal(profile_sdm_encode_default(&in, body), 0, NULL);
	zassert_equal(body[0], PROFILE_SDM_PAGE_DEFAULT, NULL);
	zassert_equal(body[1], 100u, NULL);
	zassert_equal(body[2], 42u, NULL);
	zassert_equal(body[3], 200u, NULL);
	/* Byte 4 is TWO fields: distance fraction high, speed integer low.
	 * 8 << 4 | 3 = 0x83, and a swap here reads 0.1875 m and 8 m/s. */
	zassert_equal(body[4], 0x83u, NULL);
	zassert_equal(body[5], 128u, NULL);
	zassert_equal(body[6], 77u, NULL);
	zassert_equal(body[7], 4u, NULL);

	zassert_equal(profile_sdm_decode_default(body, &out), 0, NULL);
	zassert_mem_equal(&out, &in, sizeof(out), NULL);

	/* Neither nibble may overflow into the other. */
	in.distance_frac_16 = 16u;
	zassert_equal(profile_sdm_encode_default(&in, body), -EINVAL, NULL);
	in.distance_frac_16 = 8u;
	in.speed_int_mps = 16u;
	zassert_equal(profile_sdm_encode_default(&in, body), -EINVAL, NULL);
}

ZTEST(profile_sdm, test_reserved_bytes_are_zero_not_all_ones)
{
	struct profile_sdm_supplementary in;
	uint8_t                          body[8];

	memset(&in, 0, sizeof(in));
	in.cadence_strides_min = 85u;
	in.speed_int_mps = 3u;
	in.status = profile_sdm_status(PROFILE_SDM_LOC_OTHER,
				       PROFILE_SDM_BATTERY_NEW,
				       PROFILE_SDM_HEALTH_OK,
				       PROFILE_SDM_USE_ACTIVE);

	zassert_equal(profile_sdm_encode_supplementary(PROFILE_SDM_PAGE_BASE,
						       &in, body),
		      0, NULL);
	/* TRAP 2, and it is the opposite of every other profile in this tree:
	 * an unused field here is 0x00, and validity is out of band in page
	 * 22. 0xFF in any of these bytes would be a real reading. */
	zassert_equal(body[1], 0x00u, NULL);
	zassert_equal(body[2], 0x00u, NULL);
	zassert_equal(body[6], 0x00u,
		      "page 2 has no calories byte, and 'no calories' is zero "
		      "here rather than a sentinel");
}

ZTEST(profile_sdm, test_pages_2_and_3_differ_in_exactly_one_byte)
{
	struct profile_sdm_supplementary in;
	struct profile_sdm_supplementary out;
	uint8_t                          base[8];
	uint8_t                          cal[8];

	memset(&in, 0, sizeof(in));
	in.cadence_strides_min = 85u;
	in.cadence_frac_16 = 8u;
	in.speed_int_mps = 3u;
	in.speed_frac_256 = 64u;
	in.calories = 123u;
	in.status = 0x81u;

	zassert_equal(profile_sdm_encode_supplementary(PROFILE_SDM_PAGE_BASE,
						       &in, base),
		      0, NULL);
	zassert_equal(profile_sdm_encode_supplementary(
			      PROFILE_SDM_PAGE_CALORIES, &in, cal),
		      0, NULL);

	zassert_equal(base[0], PROFILE_SDM_PAGE_BASE, NULL);
	zassert_equal(cal[0], PROFILE_SDM_PAGE_CALORIES, NULL);
	zassert_equal(base[6], 0x00u, NULL);
	zassert_equal(cal[6], 123u, NULL);
	/* Everything between the page number and the calories byte is
	 * identical, which is what makes one template correct. */
	zassert_mem_equal(&base[1], &cal[1], 5u, NULL);
	zassert_equal(base[7], cal[7], NULL);

	zassert_equal(profile_sdm_decode_supplementary(cal, &out), 0, NULL);
	zassert_equal(out.cadence_strides_min, 85u, NULL);
	zassert_equal(out.cadence_frac_16, 8u, NULL);
	zassert_equal(out.calories, 123u, NULL);

	/* Page 2 carries no calories, so the decoder reports zero rather than
	 * whatever the reserved byte happened to hold. */
	zassert_equal(profile_sdm_decode_supplementary(base, &out), 0, NULL);
	zassert_equal(out.calories, 0u, NULL);

	zassert_equal(profile_sdm_encode_supplementary(PROFILE_SDM_PAGE_DEFAULT,
						       &in, base),
		      -EINVAL, "the template is only pages 2 and 3");
}

ZTEST(profile_sdm, test_summary_page_is_wider_than_page_1s_accumulators)
{
	struct profile_sdm_summary in;
	struct profile_sdm_summary out;
	uint8_t                    body[8];

	memset(&in, 0, sizeof(in));
	in.strides = 0x123456u;      /* 24 bits, LE */
	in.distance_256 = 0x89ABCDEFu; /* 1/256 m, 32 bits, LE */

	zassert_equal(profile_sdm_encode_summary(&in, body), 0, NULL);
	zassert_equal(body[0], PROFILE_SDM_PAGE_SUMMARY, NULL);
	zassert_equal(body[1], 0x56u, NULL);
	zassert_equal(body[2], 0x34u, NULL);
	zassert_equal(body[3], 0x12u, NULL);
	zassert_equal(body[4], 0xEFu, NULL);
	zassert_equal(body[7], 0x89u, NULL);

	zassert_equal(profile_sdm_decode_summary(body, &out), 0, NULL);
	zassert_equal(out.strides, 0x123456u, NULL);
	zassert_equal(out.distance_256, 0x89ABCDEFu, NULL);

	/* 24 bits means 24 bits: truncating would restart a session total
	 * mid-run, which is worse than refusing. */
	in.strides = 0x1000000u;
	zassert_equal(profile_sdm_encode_summary(&in, body), -EINVAL, NULL);
}

ZTEST(profile_sdm, test_capabilities_page_is_the_validity_convention)
{
	struct profile_sdm_capabilities in;
	struct profile_sdm_capabilities out;
	uint8_t                         body[8];

	in.flags = PROFILE_SDM_CAP_TIME | PROFILE_SDM_CAP_DISTANCE |
		   PROFILE_SDM_CAP_SPEED | PROFILE_SDM_CAP_CADENCE;

	zassert_equal(profile_sdm_encode_capabilities(&in, body), 0, NULL);
	zassert_equal(body[0], PROFILE_SDM_PAGE_CAPABILITIES, NULL);
	zassert_equal(body[1], 0x17u, NULL);
	for (size_t i = 2; i < 8u; i++) {
		zassert_equal(body[i], 0x00u,
			      "reserved is zero on this profile, not 0xFF");
	}

	zassert_equal(profile_sdm_decode_capabilities(body, &out), 0, NULL);
	zassert_equal(out.flags, in.flags, NULL);
}

/* ── The accumulators, where three denominators meet ────────────────────── */

ZTEST(profile_sdm, test_accumulators_use_the_right_denominator_each)
{
	struct profile_sdm     sdm;
	struct profile_sdm_cfg cfg;
	uint8_t                body[8];
	struct profile_sdm_default page1;

	memset(&cfg, 0, sizeof(cfg));
	cfg.location = PROFILE_SDM_LOC_OTHER;
	zassert_equal(profile_sdm_init(&sdm, &cfg), 0, NULL);

	/* 42.500 s, 1234.500 m, 999 strides. */
	profile_sdm_set_accumulators(&sdm, 42500u, 1234500u, 999u, 4u);

	/* The very first slot of the cycle is page 1. */
	zassert_equal(profile_sdm_next(&sdm, body), PROFILE_SLOT_DATA, NULL);
	zassert_equal(profile_sdm_decode_default(body, &page1), 0, NULL);

	zassert_equal(page1.time_s, 42u, NULL);
	zassert_equal(page1.time_frac_200, 100u,
		      "time is 1/200 s: 0.5 s is 100, not 128 and not 8");
	zassert_equal(page1.distance_m, 1234u % 256u,
		      "the page 1 distance byte is meant to wrap at 256 m");
	zassert_equal(page1.distance_frac_16, 8u,
		      "distance is 1/16 m: 0.5 m is 8, not 100 and not 128");
	zassert_equal(page1.strides, 999u % 256u, NULL);
	zassert_equal(page1.latency_32, 4u, NULL);
}

/* ── The interleave ─────────────────────────────────────────────────────── */

#define SDM_RUN_MESSAGES 264u

ZTEST(profile_sdm, test_interleave_is_1_1_x_x_with_a_consecutive_common_pair)
{
	struct profile_sdm     sdm;
	struct profile_sdm_cfg cfg;
	uint8_t                pages[SDM_RUN_MESSAGES];
	uint8_t                body[8];
	uint32_t               n_80 = 0u;
	uint32_t               n_81 = 0u;
	uint32_t               pairs = 0u;
	uint32_t               n_page2 = 0u;
	uint32_t               n_page3 = 0u;

	memset(&cfg, 0, sizeof(cfg));
	cfg.id.manufacturer_id = 255u;
	cfg.location = PROFILE_SDM_LOC_OTHER;
	cfg.caps.flags = PROFILE_SDM_CAP_SPEED | PROFILE_SDM_CAP_DISTANCE;
	zassert_equal(profile_sdm_init(&sdm, &cfg), 0, NULL);

	for (uint32_t i = 0; i < SDM_RUN_MESSAGES; i++) {
		enum profile_slot_kind kind = profile_sdm_next(&sdm, body);

		zassert_not_equal(kind, PROFILE_SLOT_IDLE, NULL);
		pages[i] = body[0];

		if (body[0] == PROFILE_COMMON_PAGE_80) {
			zassert_equal(kind, PROFILE_SLOT_COMMON_80, NULL);
			n_80++;
		} else if (body[0] == PROFILE_COMMON_PAGE_81) {
			zassert_equal(kind, PROFILE_SLOT_COMMON_81, NULL);
			n_81++;
			if (i > 0u && pages[i - 1u] == PROFILE_COMMON_PAGE_80) {
				pairs++;
			}
		} else if (body[0] == PROFILE_SDM_PAGE_BASE) {
			n_page2++;
		} else if (body[0] == PROFILE_SDM_PAGE_CALORIES) {
			n_page3++;
		}
	}

	/* 1, 1, X, X - the first two messages of every four-message group are
	 * page 1, which is what carries the stride and distance accumulators a
	 * receiver differences. */
	for (uint32_t i = 0; i < PROFILE_SDM_DATA_MESSAGES; i++) {
		if ((i % PROFILE_SDM_GROUP_SLOTS) < 2u) {
			zassert_equal(pages[i], PROFILE_SDM_PAGE_DEFAULT,
				      "message %u of the cycle must be page 1",
				      i);
		} else {
			zassert_true(pages[i] == PROFILE_SDM_PAGE_BASE ||
					     pages[i] ==
						     PROFILE_SDM_PAGE_CALORIES,
				     "the X slots carry page 2 or page 3");
		}
	}

	/* The pair lands on messages 64 and 65 of a 66-message cycle. */
	zassert_equal(pages[PROFILE_SDM_DATA_MESSAGES], PROFILE_COMMON_PAGE_80,
		      NULL);
	zassert_equal(pages[PROFILE_SDM_DATA_MESSAGES + 1u],
		      PROFILE_COMMON_PAGE_81, NULL);
	zassert_equal(n_80, n_81, NULL);
	zassert_equal(pairs, n_81, "the two must be CONSECUTIVE, not merely "
				  "both present");
	zassert_equal(n_80, SDM_RUN_MESSAGES / PROFILE_SDM_CYCLE, NULL);

	/* Pages 2 and 3 share the X slots evenly, group by group. */
	zassert_true(n_page2 > 0u && n_page3 > 0u, NULL);
	zassert_true((n_page2 > n_page3 ? n_page2 - n_page3
					: n_page3 - n_page2) <= 4u,
		     "the alternation must not starve either page");

	/* And the phase survives the pair: message 66 starts a fresh group at
	 * slot 0, not two slots into one. This is the assertion that fails if
	 * the two-message pair is allowed to shift a four-message grid. */
	zassert_equal(pages[PROFILE_SDM_CYCLE], PROFILE_SDM_PAGE_DEFAULT, NULL);
	zassert_equal(pages[PROFILE_SDM_CYCLE + 1u], PROFILE_SDM_PAGE_DEFAULT,
		      NULL);
}

ZTEST(profile_sdm, test_on_request_takes_the_second_x_slot_only)
{
	struct profile_sdm     sdm;
	struct profile_sdm_cfg cfg;
	uint8_t                body[8];
	uint32_t               seen = 0u;
	uint32_t               page1_seen = 0u;

	memset(&cfg, 0, sizeof(cfg));
	cfg.location = PROFILE_SDM_LOC_OTHER;
	cfg.caps.flags = PROFILE_SDM_CAP_SPEED;
	zassert_equal(profile_sdm_init(&sdm, &cfg), 0, NULL);

	zassert_equal(profile_sdm_request(&sdm, PROFILE_SDM_PAGE_CAPABILITIES,
					  2u),
		      0, NULL);
	zassert_equal(profile_sdm_request(&sdm, PROFILE_SDM_PAGE_DEFAULT, 1u),
		      -ENOTSUP,
		      "page 1 is in the rotation; asking for it is answered by "
		      "waiting");

	for (uint32_t i = 0; i < 16u; i++) {
		(void)profile_sdm_next(&sdm, body);
		if (body[0] == PROFILE_SDM_PAGE_CAPABILITIES) {
			seen++;
			zassert_equal(i % PROFILE_SDM_GROUP_SLOTS, 3u,
				      "an on-request page takes the SECOND X "
				      "slot, never a page 1 slot");
		} else if (body[0] == PROFILE_SDM_PAGE_DEFAULT) {
			page1_seen++;
		}
	}

	zassert_equal(seen, 2u, NULL);
	zassert_equal(page1_seen, 8u,
		      "four groups of two page 1s, undisturbed");
}

/*
 * The golden vectors, which are the same bytes
 * tools/vectors/treadmill-pages.antcap holds and tools/test_compat_capture.py
 * asserts from the Python side. Hand computed from the scalings profile_sdm.h
 * states, so a change that breaks one implementation and not the other is a
 * disagreement between the two rather than a rewrite of both at once.
 */
static const uint8_t golden_sdm_page_1[8] = {
	0x01u, 0x64u, 0x2Au, 0xC8u, 0x83u, 0x80u, 0x4Du, 0x04u
};
static const uint8_t golden_sdm_page_2[8] = {
	0x02u, 0x00u, 0x00u, 0x55u, 0x83u, 0x80u, 0x00u, 0x81u
};
static const uint8_t golden_sdm_page_3[8] = {
	0x03u, 0x00u, 0x00u, 0x55u, 0x83u, 0x80u, 0x7Bu, 0x81u
};
static const uint8_t golden_sdm_page_16[8] = {
	0x10u, 0x56u, 0x34u, 0x12u, 0xEFu, 0xCDu, 0xABu, 0x89u
};
static const uint8_t golden_sdm_page_22[8] = {
	0x16u, 0x17u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u
};

ZTEST(profile_sdm, test_golden_vectors)
{
	struct profile_sdm_default       d;
	struct profile_sdm_supplementary s;
	struct profile_sdm_summary       sum;
	struct profile_sdm_capabilities  caps;
	uint8_t                          body[8];

	memset(&d, 0, sizeof(d));
	d.time_frac_200 = 100u;
	d.time_s = 42u;
	d.distance_m = 200u;
	d.distance_frac_16 = 8u;
	d.speed_int_mps = 3u;
	d.speed_frac_256 = 128u;
	d.strides = 77u;
	d.latency_32 = 4u;
	zassert_equal(profile_sdm_encode_default(&d, body), 0, NULL);
	zassert_mem_equal(body, golden_sdm_page_1, sizeof(body), NULL);

	memset(&s, 0, sizeof(s));
	s.cadence_strides_min = 85u;
	s.cadence_frac_16 = 8u;
	s.speed_int_mps = 3u;
	s.speed_frac_256 = 128u;
	s.calories = 123u;
	s.status = profile_sdm_status(PROFILE_SDM_LOC_OTHER,
				      PROFILE_SDM_BATTERY_NEW,
				      PROFILE_SDM_HEALTH_OK,
				      PROFILE_SDM_USE_ACTIVE);
	zassert_equal(profile_sdm_encode_supplementary(PROFILE_SDM_PAGE_BASE, &s,
						       body),
		      0, NULL);
	zassert_mem_equal(body, golden_sdm_page_2, sizeof(body), NULL);
	zassert_equal(profile_sdm_encode_supplementary(
			      PROFILE_SDM_PAGE_CALORIES, &s, body),
		      0, NULL);
	zassert_mem_equal(body, golden_sdm_page_3, sizeof(body), NULL);

	sum.strides = 0x123456u;
	sum.distance_256 = 0x89ABCDEFu;
	zassert_equal(profile_sdm_encode_summary(&sum, body), 0, NULL);
	zassert_mem_equal(body, golden_sdm_page_16, sizeof(body), NULL);

	caps.flags = PROFILE_SDM_CAP_TIME | PROFILE_SDM_CAP_DISTANCE |
		     PROFILE_SDM_CAP_SPEED | PROFILE_SDM_CAP_CADENCE;
	zassert_equal(profile_sdm_encode_capabilities(&caps, body), 0, NULL);
	zassert_mem_equal(body, golden_sdm_page_22, sizeof(body), NULL);
}

ZTEST(profile_sdm, test_master_declines_null_arguments)
{
	struct profile_sdm     sdm;
	struct profile_sdm_cfg cfg;
	uint8_t                body[8];

	memset(&cfg, 0, sizeof(cfg));
	zassert_equal(profile_sdm_init(NULL, &cfg), -EINVAL, NULL);
	zassert_equal(profile_sdm_init(&sdm, NULL), -EINVAL, NULL);
	zassert_equal(profile_sdm_init(&sdm, &cfg), 0, NULL);
	zassert_equal(profile_sdm_next(NULL, body), PROFILE_SLOT_IDLE, NULL);
	zassert_equal(profile_sdm_next(&sdm, NULL), PROFILE_SLOT_IDLE, NULL);
	zassert_equal(profile_sdm_messages(NULL), 0u, NULL);
	zassert_equal(profile_sdm_encode_default(NULL, body), -EINVAL, NULL);
	zassert_equal(profile_sdm_decode_summary(NULL, NULL), -EINVAL, NULL);
}

ZTEST_SUITE(profile_sdm, NULL, NULL, NULL, NULL, NULL);
