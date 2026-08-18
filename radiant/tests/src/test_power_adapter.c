/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Six things earn their place, and five of them are places where a plausible
 * implementation is wrong rather than absent:
 *
 *   - the page decoder's two sentinels are 0xFF and NOT 0, because 0 rpm and
 *     0 % are real readings (Rev 5.1 sections 8.2/8.3);
 *   - the update event count is differenced at its own u8 wire width, so a
 *     rollover is an ordinary delta and not a delta of -255;
 *   - RADIANT_FIELD_ENERGY is an INTEGRAL over real time, not the wire's
 *     "accumulated power" relabelled - 200 W held for 1 s is 200 J, and the
 *     number is checked in microjoules at exp -6 where the arithmetic is exact;
 *   - a coast contributes exactly nothing to that integral, which is the
 *     property "bike in use" leans on;
 *   - the first message establishes a baseline and integrates nothing, because
 *     there is no interval yet to integrate over;
 *   - and FE-C's 0xFFF instantaneous power invalidates the accumulator too, so
 *     an invalid message must publish nothing AND leave no gap for the next
 *     valid message to be charged for.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include "profile_fec.h"
#include "profile_power.h"
#include "radiant_bridge.h"
#include "radiant_power_adapter.h"
#include "radiant_rules.h"

#define PWR_CAP_MAX 32
#define PWR_SOURCE  0u

static struct radiant_sample pwr_cap[PWR_CAP_MAX];
static uint32_t              pwr_cap_n;

static bool pwr_cap_want(const struct radiant_sample *s)
{
	/* Everything the ADAPTER produced, and nothing radiant_rules.c derived
	 * from it - this suite is about the decode, not the rule. */
	return s->source == PWR_SOURCE &&
	       (s->flags & RADIANT_SAMPLE_DERIVED) == 0u;
}

static void pwr_cap_publish(const struct radiant_sample *s)
{
	if (pwr_cap_n < PWR_CAP_MAX) {
		pwr_cap[pwr_cap_n++] = *s;
	}
}

RADIANT_SINK_DEFINE(test_pwr_cap_sink, pwr_cap_want, pwr_cap_publish, NULL);

static struct radiant_power_adapter adapter;

static void pwr_test_reset(void *unused)
{
	ARG_UNUSED(unused);
	radiant_bridge_reset();
	radiant_rules_reset();
	radiant_power_adapter_init(&adapter);
	memset(pwr_cap, 0, sizeof(pwr_cap));
	pwr_cap_n = 0u;
}

/* The last captured sample with this field_id, or NULL. */
static const struct radiant_sample *cap_field(uint8_t field_id)
{
	int32_t i;

	for (i = (int32_t)pwr_cap_n - 1; i >= 0; i--) {
		if (pwr_cap[i].field_id == field_id) {
			return &pwr_cap[i];
		}
	}
	return NULL;
}

#define US_PER_S ((uint64_t)1000000u)

/*
 * Rev 5.1 Table 8-1, a power meter at 200 W:
 *   [0] 0x10  page 16, Standard Power Only
 *   [1] 0x05  update event count 5
 *   [2] 0x32  pedal power 50 %, differentiation bit clear
 *   [3] 0x5A  instantaneous cadence 90 rpm
 *   [4] 0xE8  accumulated power 0x03E8 = 1000 (a SUM OF WATT SAMPLES)
 *   [5] 0x03
 *   [6] 0xC8  instantaneous power 0x00C8 = 200 W
 *   [7] 0x00
 */
static const uint8_t power_std_200w[8] = {
	0x10u, 0x05u, 0x32u, 0x5Au, 0xE8u, 0x03u, 0xC8u, 0x00u
};

/* One page 0x10 with the caller's own event count, watts and cadence. */
static void power_body(uint8_t *out, uint8_t event_count, uint16_t inst_w)
{
	memcpy(out, power_std_200w, 8);
	out[1] = event_count;
	out[6] = (uint8_t)(inst_w & 0xFFu);
	out[7] = (uint8_t)((inst_w >> 8) & 0xFFu);
}

/* One FE-C page 0x19 (Rev 5.0 Table 8-25), IN_USE, cadence 85. */
static void fec_trainer_body(uint8_t *out, uint8_t event_count, uint16_t inst_w)
{
	out[0] = PROFILE_FEC_PAGE_TRAINER;
	out[1] = event_count;
	out[2] = 0x55u;
	out[3] = 0xE8u;
	out[4] = 0x03u;
	out[5] = (uint8_t)(inst_w & 0xFFu);
	out[6] = (uint8_t)((inst_w >> 8) & 0x0Fu);
	out[7] = 0x30u;
}

/* ?????? The page 0x10 decoder ???????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????? */

ZTEST(radiant_power_adapter, test_power_page_fields)
{
	struct profile_power_std p;

	zassert_equal(profile_power_decode_std(power_std_200w, &p), 0, NULL);
	zassert_equal(p.event_count, 5u, NULL);
	zassert_equal(p.acc_power_w, 1000u, NULL);
	zassert_equal(p.inst_power_w, 200u, NULL);
	zassert_equal(p.cadence_rpm, 90u, NULL);
	zassert_true(p.cadence_valid, NULL);
	zassert_true(p.pedal_valid, NULL);
	zassert_equal(p.pedal_percent, 50u, NULL);
	zassert_false(p.pedal_right_known, NULL);
}

ZTEST(radiant_power_adapter, test_power_page_sentinels_are_ff_not_zero)
{
	uint8_t body[8];
	struct profile_power_std p;

	memcpy(body, power_std_200w, sizeof(body));
	body[2] = 0xFFu;
	body[3] = 0xFFu;
	zassert_equal(profile_power_decode_std(body, &p), 0, NULL);
	zassert_false(p.pedal_valid, NULL);
	zassert_false(p.cadence_valid, NULL);

	/* Zero is a real reading for both: a stopped rider at 0 rpm and a
	 * 0 % contribution from the instrumented pedal. */
	body[2] = 0x00u;
	body[3] = 0x00u;
	zassert_equal(profile_power_decode_std(body, &p), 0, NULL);
	zassert_true(p.cadence_valid, NULL);
	zassert_equal(p.cadence_rpm, 0u, NULL);
	zassert_true(p.pedal_valid, NULL);
	zassert_equal(p.pedal_percent, 0u, NULL);
}

ZTEST(radiant_power_adapter, test_pedal_differentiation_bit)
{
	uint8_t body[8];
	struct profile_power_std p;

	memcpy(body, power_std_200w, sizeof(body));
	/* Rev 5.1 section 8.2.1: bit 7 set means bits 0-6 are the RIGHT
	 * pedal's share. 0xB2 = 0x80 | 50. */
	body[2] = 0xB2u;

	zassert_equal(profile_power_decode_std(body, &p), 0, NULL);
	zassert_true(p.pedal_valid, NULL);
	zassert_true(p.pedal_right_known, NULL);
	zassert_equal(p.pedal_percent, 50u,
		      "the differentiation bit must not leak into the percent");
}

ZTEST(radiant_power_adapter, test_power_decoder_declines_other_pages)
{
	uint8_t body[8];
	struct profile_power_std p;

	memcpy(body, power_std_200w, sizeof(body));
	body[0] = PROFILE_POWER_PAGE_CRANK_TORQUE;
	zassert_equal(profile_power_decode_std(body, &p), -EINVAL,
		      "a torque page's bytes 4-7 are not watts");

	zassert_equal(profile_power_decode_std(NULL, &p), -EINVAL, NULL);
	zassert_equal(profile_power_decode_std(power_std_200w, NULL), -EINVAL,
		      NULL);
}

ZTEST(radiant_power_adapter, test_power_encode_decode_round_trip)
{
	uint8_t body[8];
	struct profile_power_std p;

	/* The encoder and the decoder were written from the same table by
	 * different hands; this is the check that they read it the same way,
	 * including the byte order of the two 16-bit fields. */
	zassert_equal(profile_power_encode_std(200u, 0xB4u, 0u, 60000u, 4095u,
					       body),
		      0, NULL);
	zassert_equal(profile_power_decode_std(body, &p), 0, NULL);
	zassert_equal(p.event_count, 200u, NULL);
	zassert_equal(p.acc_power_w, 60000u, NULL);
	zassert_equal(p.inst_power_w, 4095u, NULL);
	zassert_true(p.cadence_valid, NULL);
	zassert_equal(p.cadence_rpm, 0u, NULL);
	zassert_true(p.pedal_right_known, NULL);
	zassert_equal(p.pedal_percent, 52u, NULL); /* 0xB4 & 0x7F */
}

/* ?????? The adapter: bicycle power ????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????? */

ZTEST(radiant_power_adapter, test_first_call_posts_watts_only)
{
	uint8_t body[8];
	uint32_t n;

	power_body(body, 5u, 200u);
	n = radiant_power_adapter_decode(&adapter, PWR_SOURCE, body, 0u);
	radiant_bridge_drain();

	zassert_equal(n, 1u, "no baseline yet: watts, and nothing to difference");
	zassert_not_null(cap_field(RADIANT_POWER_FIELD_INST_POWER), NULL);
	zassert_is_null(cap_field(RADIANT_POWER_FIELD_EVENT_COUNT), NULL);
	zassert_is_null(cap_field(RADIANT_POWER_FIELD_ENERGY), NULL);
}

ZTEST(radiant_power_adapter, test_instantaneous_watts_are_exp_zero)
{
	uint8_t body[8];
	const struct radiant_sample *s;

	power_body(body, 5u, 250u);
	radiant_power_adapter_decode(&adapter, PWR_SOURCE, body, 0u);
	radiant_bridge_drain();

	s = cap_field(RADIANT_POWER_FIELD_INST_POWER);
	zassert_not_null(s, NULL);
	zassert_equal(s->field_type, RADIANT_FIELD_ACTIVE_POWER, NULL);
	zassert_equal(s->exp, 0, "watts are the vocabulary's own unit");
	zassert_equal(s->raw, 250, NULL);
	zassert_equal(s->flags, 0u, "instantaneous: not an accumulator");
}

ZTEST(radiant_power_adapter, test_energy_is_the_time_integral)
{
	uint8_t body[8];
	const struct radiant_sample *s;
	uint32_t n;

	/* 200 W held for exactly one second is 200 J. */
	power_body(body, 5u, 200u);
	radiant_power_adapter_decode(&adapter, PWR_SOURCE, body, 0u);
	radiant_bridge_drain();

	power_body(body, 6u, 200u);
	n = radiant_power_adapter_decode(&adapter, PWR_SOURCE, body,
					 1u * US_PER_S);
	radiant_bridge_drain();
	zassert_equal(n, 3u, "watts, event count, energy");

	s = cap_field(RADIANT_POWER_FIELD_ENERGY);
	zassert_not_null(s, NULL);
	zassert_equal(s->field_type, RADIANT_FIELD_ENERGY, NULL);
	zassert_equal(s->flags, RADIANT_SAMPLE_ACCUMULATING, NULL);
	zassert_equal(s->exp, -6, "microjoules, so nothing rounds in the adapter");
	zassert_equal(s->raw, INT64_C(200000000), "200 W x 1 s = 200 J = 2e8 uJ");

	/* And it must NOT be the wire's accumulated-power field, which reads
	 * 1000 in this vector and would be a plausible wrong answer. */
	zassert_not_equal(s->raw, INT64_C(1000), NULL);
}

ZTEST(radiant_power_adapter, test_energy_integral_exceeds_32_bits)
{
	uint8_t body[8];
	const struct radiant_sample *s;

	/* 500 W for 4 s is 2e9 uJ, past INT32_MAX. The accumulator is a
	 * uint64_t and the sample's raw is an int64_t; a 32-bit intermediate
	 * anywhere in the chain shows up here and nowhere else. */
	power_body(body, 0u, 500u);
	radiant_power_adapter_decode(&adapter, PWR_SOURCE, body, 0u);
	radiant_bridge_drain();

	power_body(body, 1u, 500u);
	radiant_power_adapter_decode(&adapter, PWR_SOURCE, body,
				     4u * US_PER_S);
	radiant_bridge_drain();

	s = cap_field(RADIANT_POWER_FIELD_ENERGY);
	zassert_not_null(s, NULL);
	zassert_equal(s->raw, INT64_C(2000000000), NULL);
}

ZTEST(radiant_power_adapter, test_a_coast_does_not_advance_the_accumulator)
{
	uint8_t body[8];
	const struct radiant_sample *s;

	power_body(body, 0u, 200u);
	radiant_power_adapter_decode(&adapter, PWR_SOURCE, body, 0u);
	radiant_bridge_drain();

	power_body(body, 1u, 200u);
	radiant_power_adapter_decode(&adapter, PWR_SOURCE, body,
				     1u * US_PER_S);
	radiant_bridge_drain();

	/* Rider stops pedalling. The event count keeps advancing (the sensor
	 * still updates), the integral must not. */
	pwr_cap_n = 0u;
	power_body(body, 2u, 0u);
	radiant_power_adapter_decode(&adapter, PWR_SOURCE, body,
				     2u * US_PER_S);
	radiant_bridge_drain();

	s = cap_field(RADIANT_POWER_FIELD_ENERGY);
	zassert_not_null(s, NULL);
	zassert_equal(s->raw, INT64_C(200000000),
		      "0 W over any interval adds exactly nothing");

	s = cap_field(RADIANT_POWER_FIELD_EVENT_COUNT);
	zassert_not_null(s, NULL);
	zassert_equal(s->raw, INT64_C(2), "the sensor is still updating");
}

ZTEST(radiant_power_adapter, test_event_count_wraps_at_its_own_u8_width)
{
	uint8_t body[8];
	const struct radiant_sample *s;

	power_body(body, 250u, 100u);
	radiant_power_adapter_decode(&adapter, PWR_SOURCE, body, 0u);
	radiant_bridge_drain();

	/* 250 -> 3 is nine events across the rollover. Widening before
	 * subtracting gives -247, and a rule testing "did it advance" would
	 * read a pedalling rider as stopped once every 256 events. */
	power_body(body, 3u, 100u);
	radiant_power_adapter_decode(&adapter, PWR_SOURCE, body,
				     1u * US_PER_S);
	radiant_bridge_drain();

	s = cap_field(RADIANT_POWER_FIELD_EVENT_COUNT);
	zassert_not_null(s, NULL);
	zassert_equal(s->field_type, RADIANT_FIELD_EVENT_COUNT, NULL);
	zassert_equal(s->flags, RADIANT_SAMPLE_ACCUMULATING, NULL);
	zassert_equal(s->exp, 0, NULL);
	zassert_equal(s->raw, INT64_C(9), NULL);

	/* And the total keeps climbing rather than restarting. */
	power_body(body, 5u, 100u);
	radiant_power_adapter_decode(&adapter, PWR_SOURCE, body,
				     2u * US_PER_S);
	radiant_bridge_drain();
	s = cap_field(RADIANT_POWER_FIELD_EVENT_COUNT);
	zassert_equal(s->raw, INT64_C(11), "a running total, not a per-message delta");
}

ZTEST(radiant_power_adapter, test_adapter_ignores_pages_it_does_not_decode)
{
	uint8_t body[8];
	uint32_t n;

	memcpy(body, power_std_200w, sizeof(body));
	body[0] = PROFILE_POWER_PAGE_WHEEL_TORQUE;
	n = radiant_power_adapter_decode(&adapter, PWR_SOURCE, body, 0u);
	radiant_bridge_drain();
	zassert_equal(n, 0u, NULL);
	zassert_equal(pwr_cap_n, 0u, "a torque page must post nothing at all");

	/* And it must not have consumed the baseline, either: the next real
	 * page 0x10 is still the first one this instance has seen. */
	power_body(body, 5u, 200u);
	n = radiant_power_adapter_decode(&adapter, PWR_SOURCE, body,
					 1u * US_PER_S);
	zassert_equal(n, 1u, NULL);
}

ZTEST(radiant_power_adapter, test_null_arguments_post_nothing)
{
	uint8_t body[8];

	power_body(body, 5u, 200u);
	zassert_equal(radiant_power_adapter_decode(NULL, PWR_SOURCE, body, 0u),
		      0u, NULL);
	zassert_equal(radiant_power_adapter_decode(&adapter, PWR_SOURCE, NULL,
						   0u),
		      0u, NULL);
	zassert_equal(radiant_power_adapter_decode_fec(&adapter, PWR_SOURCE,
						       NULL, 0u),
		      0u, NULL);
	radiant_bridge_drain();
	zassert_equal(pwr_cap_n, 0u, NULL);
}

/* ?????? The adapter: FE-C ???????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????? */

ZTEST(radiant_power_adapter, test_fec_general_page_posts_type_state_and_speed)
{
	/* Table 8-7: trainer, 10 s elapsed, 100 m, 5.000 m/s, 80 bpm, caps
	 * 0x5 (ANT+ HR source, distance enabled), state IN_USE. */
	static const uint8_t body[8] = {
		0x10u, 0x19u, 0x28u, 0x64u, 0x88u, 0x13u, 0x50u, 0x35u
	};
	const struct radiant_sample *s;
	uint32_t n;

	n = radiant_power_adapter_decode_fec(&adapter, PWR_SOURCE, body, 0u);
	radiant_bridge_drain();
	zassert_equal(n, 3u, "equipment type, state and speed");

	/* Byte 1 is 0x19 = 25, PROFILE_FEC_TYPE_TREADMILL's neighbour
	 * "trainer". Decoded since the codec landed and posted since
	 * radiant_naming.c needed it: it is what names the endpoint
	 * "Indoor Bike 51234 In Use" instead of a hex string. */
	s = cap_field(RADIANT_POWER_FIELD_FEC_TYPE);
	zassert_not_null(s, NULL);
	zassert_equal(s->field_type, RADIANT_FIELD_ENUM_GENERIC, NULL);
	zassert_equal(s->raw, 25, NULL);
	zassert_equal(s->flags, RADIANT_SAMPLE_ROTATED,
		      "an identity, but still a decoded fact - and it rides "
		      "FE-C page 16, so it is ROTATED");

	s = cap_field(RADIANT_POWER_FIELD_FEC_STATE);
	zassert_not_null(s, NULL);
	zassert_equal(s->field_type, RADIANT_FIELD_ENUM_GENERIC, NULL);
	zassert_equal(s->raw, (int64_t)PROFILE_FEC_STATE_IN_USE, NULL);
	zassert_equal(s->flags, RADIANT_SAMPLE_ROTATED,
		      "a decoded measurement, not a rule's derived boolean - and "
		      "ROTATED, because page 16 is one page of a rotation");

	s = cap_field(RADIANT_POWER_FIELD_FEC_SPEED);
	zassert_not_null(s, NULL);
	zassert_equal(s->field_type, RADIANT_FIELD_SPEED, NULL);
	zassert_equal(s->exp, -3, "0.001 m/s IS the raw at exp -3");
	zassert_equal(s->raw, INT64_C(5000), NULL);

	/* The general page carries no power and must not start the clock -
	 * the next trainer page is still this instance's first. */
	zassert_false(adapter.have_prev, NULL);
}

ZTEST(radiant_power_adapter, test_fec_trainer_page_integrates_the_same_way)
{
	uint8_t body[8];
	const struct radiant_sample *s;
	uint32_t n;

	fec_trainer_body(body, 7u, 200u);
	n = radiant_power_adapter_decode_fec(&adapter, PWR_SOURCE, body, 0u);
	radiant_bridge_drain();
	zassert_equal(n, 1u, "baseline call: watts only");

	fec_trainer_body(body, 8u, 200u);
	n = radiant_power_adapter_decode_fec(&adapter, PWR_SOURCE, body,
					     1u * US_PER_S);
	radiant_bridge_drain();
	zassert_equal(n, 3u, NULL);

	s = cap_field(RADIANT_POWER_FIELD_ENERGY);
	zassert_not_null(s, NULL);
	zassert_equal(s->exp, -6, NULL);
	zassert_equal(s->raw, INT64_C(200000000), NULL);

	/* Same field_ids as the bicycle-power path: one series per quantity
	 * per source, whichever envelope it arrived in. */
	s = cap_field(RADIANT_POWER_FIELD_EVENT_COUNT);
	zassert_not_null(s, NULL);
	zassert_equal(s->raw, INT64_C(1), NULL);
}

ZTEST(radiant_power_adapter, test_only_the_interleaved_pages_are_marked_rotated)
{
	/*
	 * The discriminator, and the whole reason RADIANT_SAMPLE_ROTATED is a
	 * parameter of post_power() rather than a property of the file.
	 *
	 * The SAME three series - watts, event count, energy - are ROTATED off
	 * FE-C page 25 and are NOT off bicycle-power page 0x10, because page
	 * 0x10 is that profile's every-message page while 25 is one page of an
	 * interleaved rotation. A consumer computing a per-entity timeout from
	 * the channel period is right in the second case and wrong by about two
	 * orders of magnitude in the first; on a real Wahoo #52233 every FE-C
	 * entity was published with "expire_after":1 and would have been greyed
	 * out in Home Assistant between the trainer's own pages.
	 */
	uint8_t body[8];
	const struct radiant_sample *s;

	fec_trainer_body(body, 7u, 200u);
	(void)radiant_power_adapter_decode_fec(&adapter, PWR_SOURCE, body, 0u);
	fec_trainer_body(body, 8u, 200u);
	(void)radiant_power_adapter_decode_fec(&adapter, PWR_SOURCE, body,
					       1u * US_PER_S);
	radiant_bridge_drain();

	s = cap_field(RADIANT_POWER_FIELD_INST_POWER);
	zassert_not_null(s, NULL);
	zassert_true((s->flags & RADIANT_SAMPLE_ROTATED) != 0u,
		     "FE-C page 25 is one page of a rotation");
	s = cap_field(RADIANT_POWER_FIELD_ENERGY);
	zassert_not_null(s, NULL);
	zassert_equal(s->flags,
		      RADIANT_SAMPLE_ACCUMULATING | RADIANT_SAMPLE_ROTATED,
		      "an accumulator AND rotated - the two are independent");

	/* Now the bicycle-power profile, same adapter, same three field ids. */
	pwr_test_reset(NULL);
	power_body(body, 7u, 200u);
	(void)radiant_power_adapter_decode(&adapter, PWR_SOURCE, body, 0u);
	power_body(body, 8u, 200u);
	(void)radiant_power_adapter_decode(&adapter, PWR_SOURCE, body,
					   1u * US_PER_S);
	radiant_bridge_drain();

	s = cap_field(RADIANT_POWER_FIELD_INST_POWER);
	zassert_not_null(s, NULL);
	zassert_equal(s->flags, 0u,
		      "page 0x10 is on every message: NOT rotated, and its "
		      "expiry from the channel period is correct");
	s = cap_field(RADIANT_POWER_FIELD_ENERGY);
	zassert_not_null(s, NULL);
	zassert_equal(s->flags, RADIANT_SAMPLE_ACCUMULATING,
		      "accumulating, and nothing else");
}

ZTEST(radiant_power_adapter, test_fec_invalid_power_posts_nothing_but_keeps_time)
{
	uint8_t body[8];
	const struct radiant_sample *s;
	uint32_t n;

	fec_trainer_body(body, 0u, 200u);
	radiant_power_adapter_decode_fec(&adapter, PWR_SOURCE, body, 0u);
	radiant_bridge_drain();

	/* Table 8-25's 0xFFF: both power fields invalid. Nothing publishable. */
	pwr_cap_n = 0u;
	fec_trainer_body(body, 1u, PROFILE_FEC_INVALID_INST_POWER);
	n = radiant_power_adapter_decode_fec(&adapter, PWR_SOURCE, body,
					     10u * US_PER_S);
	radiant_bridge_drain();
	zassert_equal(n, 0u, NULL);
	zassert_equal(pwr_cap_n, 0u, NULL);

	/*
	 * Now a valid 200 W message one second later. The integral must cover
	 * that one second only. If the ten-second unknown stretch had been
	 * left on the clock, this would read 2200 J instead of 200 J - a rider
	 * who stopped for a while billed for their restart wattage the whole
	 * time they were stopped.
	 */
	fec_trainer_body(body, 2u, 200u);
	radiant_power_adapter_decode_fec(&adapter, PWR_SOURCE, body,
					 11u * US_PER_S);
	radiant_bridge_drain();

	s = cap_field(RADIANT_POWER_FIELD_ENERGY);
	zassert_not_null(s, NULL);
	zassert_equal(s->raw, INT64_C(200000000),
		      "the unknown stretch is charged nothing, not restart watts");

	/* The two events that happened during it are not lost, though. */
	s = cap_field(RADIANT_POWER_FIELD_EVENT_COUNT);
	zassert_not_null(s, NULL);
	zassert_equal(s->raw, INT64_C(2), NULL);
}

ZTEST(radiant_power_adapter, test_fec_ignores_pages_it_does_not_decode)
{
	uint8_t body[8];
	uint32_t n;

	/* Page 26 (0x1A), Specific Trainer Torque Data - a real page a real
	 * trainer sends, deliberately not decoded (see profile_fec.h). */
	memset(body, 0, sizeof(body));
	body[0] = 0x1Au;
	n = radiant_power_adapter_decode_fec(&adapter, PWR_SOURCE, body, 0u);
	radiant_bridge_drain();
	zassert_equal(n, 0u, NULL);
	zassert_equal(pwr_cap_n, 0u, NULL);
}

ZTEST(radiant_power_adapter, test_field_ids_are_inside_the_profile_block)
{
	/* radiant_bridge.h reserves 0x20-0x3F for the common-page adapter,
	 * which runs on this same source. A collision would silently merge two
	 * different series in every sink's history. */
	zassert_true(RADIANT_POWER_FIELD_FEC_HR < RADIANT_FIELD_ID_COMMON_BASE,
		     "the highest id this adapter reserves is still a profile id");
	zassert_equal(RADIANT_FIELD_ID_PROFILE_MAX + 1u,
		      RADIANT_FIELD_ID_COMMON_BASE,
		      "the two blocks must abut with no unowned gap");
}

ZTEST_SUITE(radiant_power_adapter, NULL, NULL, pwr_test_reset, NULL, NULL);
