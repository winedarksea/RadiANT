/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_profile_env.c - ANT+ Environment 0x19: the two page tables and the
 * sample-bus adapter.
 *
 * Provenance: the vectors below are HAND-PLACED BIT PATTERNS built from Tables
 * 6-2, 6-3 and 6-4 of D00001502 Rev 1.0, not encoder output fed back through
 * the decoder. There is no encoder for this profile (see profile_env.c), so a
 * round-trip suite was never an option here - which is fortunate, because a
 * round-trip suite passes even with a split field read backwards on both
 * sides, and this page has TWO nibble-split fields packed in opposite
 * directions sharing one byte.
 *
 * What each group of tests is defending against:
 *
 *   - The two 24-hour fields swapped, or one packed with the other's shape.
 *     Both are temperatures in the same range, so the wrong answer is still a
 *     plausible one. The vector uses a NEGATIVE low and a POSITIVE high with
 *     no shared nibbles, so any mix-up changes the number.
 *   - Missing sign extension from bit 11. Correct in every warm room; wrong
 *     below freezing. Tested at both 12-bit extremes.
 *   - The two sentinels, which are different widths (0x800 and 0x8000) and
 *     which sign-extend to legal-looking large negatives if mistaken for data.
 *   - The degC -> K offset. It is exact, and 27315 at exp -2 is the assertion
 *     that says so.
 *   - u8 event-count wraparound, differenced at the wire width.
 *   - A page this profile does not define being declined rather than decoded.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include "profile_env.h"
#include "radiant_bridge.h"
#include "radiant_env_adapter.h"

/* ── The capture sink ──────────────────────────────────────────────────────
 * Sinks are linker-collected and global to the whole test binary, so this one
 * filters on its own source index and the suite resets the bus before every
 * test. Nothing else drains the bus while these tests run.
 */
#define ENV_SOURCE 3u
#define CAP_MAX    8

static struct radiant_sample cap_buf[CAP_MAX];
static uint32_t              cap_n;

static bool env_cap_want(const struct radiant_sample *s)
{
	return s->source == ENV_SOURCE;
}

static void env_cap_publish(const struct radiant_sample *s)
{
	if (cap_n < CAP_MAX) {
		cap_buf[cap_n++] = *s;
	}
}

RADIANT_SINK_DEFINE(test_env_cap_sink, env_cap_want, env_cap_publish, NULL);

static void env_test_reset(void *unused)
{
	ARG_UNUSED(unused);
	radiant_bridge_reset();
	memset(cap_buf, 0, sizeof(cap_buf));
	cap_n = 0u;
}

/* The one record with this field_id, or NULL. */
static const struct radiant_sample *captured(uint8_t field_id)
{
	uint32_t i;

	for (i = 0u; i < cap_n; i++) {
		if (cap_buf[i].field_id == field_id) {
			return &cap_buf[i];
		}
	}
	return NULL;
}

/* ── The vectors ───────────────────────────────────────────────────────────
 *
 * Page 0x01, byte by byte (Table 6-4):
 *
 *   [0] 0x01  page number
 *   [1] 0xFF  reserved
 *   [2] 0x2A  event count = 42
 *   [3] 0xCB  24-hour low LSB          )  0xFCB = 4043 = -53 -> -5.3 degC
 *   [4] 0xF8  bits 7:4 = low MSN = 0xF )
 *             bits 3:0 = high LSN = 0x8 )
 *   [5] 0x13  24-hour high MSB          )  0x138 = 312 -> +31.2 degC
 *   [6] 0x29  current temp LSB )  0x0929 = 2345 -> +23.45 degC
 *   [7] 0x09  current temp MSB )
 *
 * The two 24-hour fields deliberately have opposite signs and no digit in
 * common, so reading either with the other's packing produces a number nothing
 * in this file matches.
 */
static const uint8_t page1[8] = {
	0x01u, 0xFFu, 0x2Au, 0xCBu, 0xF8u, 0x13u, 0x29u, 0x09u
};

#define P1_EVENT_COUNT   42
#define P1_LOW_DECI      (-53)
#define P1_HIGH_DECI     312
#define P1_CURRENT_CENTI 2345

/*
 * Page 0x00, byte by byte (Tables 6-2 and 6-3):
 *
 *   [0] 0x00  page number
 *   [1] 0xFF  reserved      (BOTH bytes 1 and 2 are reserved on this page,
 *   [2] 0xFF  reserved       unlike page 1 where only byte 1 is)
 *   [3] 0x18  transmission info = 0b00011000:
 *               bits 1:0 = 0b00 -> default transmission rate 0.5 Hz
 *               bits 3:2 = 0b10 -> UTC time supported and set
 *               bits 5:4 = 0b01 -> local time supported, not set
 *               bits 7:6 = 0b00 -> reserved
 *   [4..7] 0x00000003 LE -> pages 0 and 1 supported, the only legal value a
 *          temperature sensor can report in this revision (section 6.3.2).
 */
static const uint8_t page0[8] = {
	0x00u, 0xFFu, 0xFFu, 0x18u, 0x03u, 0x00u, 0x00u, 0x00u
};

/* A page 0x01 body with the three data fields replaced. `low12` and `high12`
 * are the RAW twelve-bit codes as they sit on the wire, not temperatures -
 * the point of these tests is the packing and the sign extension, so the test
 * states the bit pattern and the decoder is what turns it into a number. */
static void build_page1(uint8_t *body, uint8_t event_count, uint16_t low12,
			uint16_t high12, uint16_t cur16)
{
	body[0] = PROFILE_ENV_PAGE_TEMPERATURE;
	body[1] = 0xFFu;
	body[2] = event_count;
	body[3] = (uint8_t)(low12 & 0xFFu);
	body[4] = (uint8_t)((uint8_t)(((low12 >> 8) & 0x0Fu) << 4) |
			    (uint8_t)(high12 & 0x0Fu));
	body[5] = (uint8_t)((high12 >> 4) & 0xFFu);
	body[6] = (uint8_t)(cur16 & 0xFFu);
	body[7] = (uint8_t)((cur16 >> 8) & 0xFFu);
}

/* ── Page 0x01 ─────────────────────────────────────────────────────────── */

/*
 * The builder above is a second statement of the packing, so it is pinned
 * against the hand-placed vector before any test relies on it. If the two ever
 * disagree, this is the test that says so rather than a silent agreement
 * between two copies of the same mistake.
 */
ZTEST(profile_env, test_page1_builder_matches_the_hand_placed_vector)
{
	uint8_t body[8];

	build_page1(body, P1_EVENT_COUNT, 0x0FCBu, 0x0138u, 0x0929u);
	zassert_mem_equal(body, page1, sizeof(page1),
			  "the test's own packing disagrees with Table 6-4");
}

ZTEST(profile_env, test_page1_decode)
{
	struct profile_env_temperature t;

	zassert_equal(profile_env_decode_temperature(page1, &t), 0);

	zassert_equal(t.event_count, P1_EVENT_COUNT);

	zassert_true(t.low_24h_valid);
	zassert_true(t.high_24h_valid);
	zassert_true(t.current_valid);

	/* Low is byte [3] plus byte [4]'s HIGH nibble; high is byte [4]'s LOW
	 * nibble plus byte [5]. Swapping the two shapes gives 0x8FC and 0x13F,
	 * neither of which is either of these. */
	zassert_equal(t.low_24h_deci, P1_LOW_DECI, "24-hour low, 0.1 degC");
	zassert_equal(t.high_24h_deci, P1_HIGH_DECI, "24-hour high, 0.1 degC");

	/* And the current reading is a different SCALE as well as a different
	 * width: 0.01 degC, not 0.1. */
	zassert_equal(t.current_centi, P1_CURRENT_CENTI,
		      "current temperature, 0.01 degC");
}

ZTEST(profile_env, test_page1_rejects_the_wrong_page_number)
{
	struct profile_env_temperature t;
	uint8_t body[8];

	memcpy(body, page1, sizeof(body));

	/* Byte [0] is the WHOLE page number - there is no page-change toggle
	 * on this profile, so 0x81 is not "page 1 with a toggle set". */
	body[0] = 0x81u;
	zassert_equal(profile_env_decode_temperature(body, &t), -EINVAL);

	body[0] = PROFILE_ENV_PAGE_CAPABILITIES;
	zassert_equal(profile_env_decode_temperature(body, &t), -EINVAL);
}

/* ── The twelve-bit signed fields ──────────────────────────────────────── */

ZTEST(profile_env, test_24h_fields_at_both_signed_extremes)
{
	struct profile_env_temperature t;
	uint8_t body[8];

	/* 0x7FF is the largest positive code: +204.7 degC. */
	build_page1(body, 0u, 0x07FFu, 0x07FFu, 0u);
	zassert_equal(profile_env_decode_temperature(body, &t), 0);
	zassert_true(t.low_24h_valid);
	zassert_true(t.high_24h_valid);
	zassert_equal(t.low_24h_deci, 2047, "0x7FF is +204.7 degC");
	zassert_equal(t.high_24h_deci, 2047, "0x7FF is +204.7 degC");

	/* 0x801 is the most negative LEGAL code: -204.7 degC. Without sign
	 * extension from bit 11 it reads as +2049, which is a temperature and
	 * therefore not obviously wrong to anything downstream. */
	build_page1(body, 0u, 0x0801u, 0x0801u, 0u);
	zassert_equal(profile_env_decode_temperature(body, &t), 0);
	zassert_true(t.low_24h_valid);
	zassert_true(t.high_24h_valid);
	zassert_equal(t.low_24h_deci, -2047, "0x801 is -204.7 degC");
	zassert_equal(t.high_24h_deci, -2047, "0x801 is -204.7 degC");

	/* 0xFFF is -1, the smallest negative step, and the case a decoder that
	 * masks to 12 bits without extending gets most spectacularly wrong. */
	build_page1(body, 0u, 0x0FFFu, 0x0FFFu, 0u);
	zassert_equal(profile_env_decode_temperature(body, &t), 0);
	zassert_equal(t.low_24h_deci, -1, "0xFFF is -0.1 degC");
	zassert_equal(t.high_24h_deci, -1, "0xFFF is -0.1 degC");
}

ZTEST(profile_env, test_24h_invalid_sentinel_is_not_a_temperature)
{
	struct profile_env_temperature t;
	uint8_t body[8];

	/* 0x800 is "invalid" (section 6.4), NOT -204.8 degC - which is why
	 * section 6.4's stated range stops at -204.7 rather than -204.8. Each
	 * field carries its own sentinel independently, so one invalid must
	 * not invalidate the other. */
	build_page1(body, 0u, PROFILE_ENV_INVALID_12, 0x0138u, 0u);
	zassert_equal(profile_env_decode_temperature(body, &t), 0);
	zassert_false(t.low_24h_valid, "0x800 in the low field is invalid");
	zassert_true(t.high_24h_valid, "the high field is untouched by it");
	zassert_equal(t.high_24h_deci, P1_HIGH_DECI);

	build_page1(body, 0u, 0x0FCBu, PROFILE_ENV_INVALID_12, 0u);
	zassert_equal(profile_env_decode_temperature(body, &t), 0);
	zassert_true(t.low_24h_valid);
	zassert_equal(t.low_24h_deci, P1_LOW_DECI);
	zassert_false(t.high_24h_valid, "0x800 in the high field is invalid");
}

ZTEST(profile_env, test_current_invalid_sentinel_is_a_different_width)
{
	struct profile_env_temperature t;
	uint8_t body[8];

	/* The current field's sentinel is 0x8000, not 0x800. A decoder reusing
	 * the 12-bit sentinel here would treat 0x0800 (+20.48 degC) as missing
	 * and 0x8000 (-327.68 degC) as a reading. Both directions are checked. */
	build_page1(body, 0u, 0x0000u, 0x0000u, PROFILE_ENV_INVALID_16);
	zassert_equal(profile_env_decode_temperature(body, &t), 0);
	zassert_false(t.current_valid, "0x8000 is invalid");

	build_page1(body, 0u, 0x0000u, 0x0000u, 0x0800u);
	zassert_equal(profile_env_decode_temperature(body, &t), 0);
	zassert_true(t.current_valid, "0x0800 is +20.48 degC, a real reading");
	zassert_equal(t.current_centi, 2048);

	/* The most negative LEGAL current reading, one count above the
	 * sentinel: 0x8001 = -327.67 degC. */
	build_page1(body, 0u, 0x0000u, 0x0000u, 0x8001u);
	zassert_equal(profile_env_decode_temperature(body, &t), 0);
	zassert_true(t.current_valid);
	zassert_equal(t.current_centi, -32767, "0x8001 is -327.67 degC");
}

/* ── Page 0x00 ─────────────────────────────────────────────────────────── */

ZTEST(profile_env, test_page0_capabilities)
{
	struct profile_env_capabilities c;

	zassert_equal(profile_env_decode_capabilities(page0, &c), 0);

	/* The rate bits are the LOW pair even though Table 6-3 lists them
	 * last, and 0b00 is the SLOW rate - it would be easy to assume the
	 * zero value meant the profile's more common 4 Hz. It does not. */
	zassert_equal(c.default_rate, PROFILE_ENV_RATE_0P5_HZ,
		      "bits 1:0 = 0b00 is 0.5 Hz");
	zassert_equal(c.utc_time, PROFILE_ENV_TIME_SUPPORTED_SET);
	zassert_equal(c.local_time, PROFILE_ENV_TIME_SUPPORTED_UNSET);

	zassert_equal(c.supported_pages,
		      PROFILE_ENV_SUPPORTED_PAGE_BIT(PROFILE_ENV_PAGE_CAPABILITIES) |
		      PROFILE_ENV_SUPPORTED_PAGE_BIT(PROFILE_ENV_PAGE_TEMPERATURE),
		      "bit position is the page number, u32 little-endian");
}

ZTEST(profile_env, test_page0_the_other_rate_and_the_reserved_ones)
{
	struct profile_env_capabilities c;
	uint8_t body[8];

	memcpy(body, page0, sizeof(body));

	body[3] = 0x01u; /* bits 1:0 = 0b01 */
	zassert_equal(profile_env_decode_capabilities(body, &c), 0);
	zassert_equal(c.default_rate, PROFILE_ENV_RATE_4_HZ);

	/* 0b10 and 0b11 are Reserved and are passed through raw rather than
	 * folded into either real rate. */
	body[3] = 0x03u;
	zassert_equal(profile_env_decode_capabilities(body, &c), 0);
	zassert_equal(c.default_rate, 3u, "a reserved rate stays visible");
}

/*
 * The 0.5 Hz option is trap 18: radiant_liveness.c's threshold is period x 3,
 * so the two rates differ eightfold in how long a sensor may be quiet before
 * it is called stale. This asserts the mapping the binding's period comes
 * from, and that a reserved rate abstains rather than guessing.
 */
ZTEST(profile_env, test_period_counts_for_each_declared_rate)
{
	zassert_equal(profile_env_period_counts(PROFILE_ENV_RATE_0P5_HZ),
		      PROFILE_ENV_PERIOD_0P5_HZ, "65535 counts is 0.5 Hz");
	zassert_equal(profile_env_period_counts(PROFILE_ENV_RATE_4_HZ),
		      PROFILE_ENV_PERIOD_4_HZ, "8192 counts is 4 Hz");

	/* 0 is what struct radiant_binding::period already means by "never
	 * set", and what radiant_liveness.c already abstains on. */
	zassert_equal(profile_env_period_counts(2u), 0u);
	zassert_equal(profile_env_period_counts(3u), 0u);
}

ZTEST(profile_env, test_page0_rejects_the_wrong_page_number)
{
	struct profile_env_capabilities c;
	uint8_t body[8];

	memcpy(body, page0, sizeof(body));
	body[0] = PROFILE_ENV_PAGE_TEMPERATURE;
	zassert_equal(profile_env_decode_capabilities(body, &c), -EINVAL);
}

/* ── The adapter ───────────────────────────────────────────────────────── */

ZTEST(profile_env, test_adapter_posts_temperature_in_kelvin_and_nothing_else)
{
	struct radiant_env_adapter a;
	const struct radiant_sample *s;

	radiant_env_adapter_init(&a);

	/* First page 0x01: temperature only. The event count has nothing to
	 * difference against yet, exactly as radiant_hr_adapter_decode()'s
	 * first call behaves. */
	zassert_equal(radiant_env_adapter_decode(&a, ENV_SOURCE, page1, 1000u), 1u);
	zassert_equal(radiant_bridge_drain(), 1u);
	zassert_equal(cap_n, 1u);

	s = captured(RADIANT_ENV_FIELD_TEMPERATURE);
	zassert_not_null(s);
	zassert_equal(s->field_type, RADIANT_FIELD_TEMPERATURE);
	zassert_equal(s->exp, -2);
	zassert_equal(s->flags, 0u, "an instantaneous reading, not accumulating");
	zassert_equal(s->t_us, 1000u);
	zassert_equal(s->raw, P1_CURRENT_CENTI + RADIANT_ENV_KELVIN_OFFSET_CENTI,
		      "23.45 degC is 296.60 K");

	/* THE 24-HOUR STATISTICS ARE NOT ON THE BUS. They are decoded and kept
	 * - a caller can read them - but publishing them would mint two more
	 * Matter Temperature Sensor endpoints per sensor that read like live
	 * values. This assertion is the decision, not an accident of the
	 * vector. */
	zassert_is_null(captured(RADIANT_ENV_FIELD_LOW_24H));
	zassert_is_null(captured(RADIANT_ENV_FIELD_HIGH_24H));
	zassert_true(a.have_temperature);
	zassert_equal(a.t.low_24h_deci, P1_LOW_DECI, "decoded, just not posted");
	zassert_equal(a.t.high_24h_deci, P1_HIGH_DECI, "decoded, just not posted");
}

ZTEST(profile_env, test_adapter_kelvin_offset_is_exact_at_zero_and_below)
{
	struct radiant_env_adapter a;
	const struct radiant_sample *s;
	uint8_t body[8];

	radiant_env_adapter_init(&a);

	/* 0.00 degC. The offset is a whole number of hundredths, so this is
	 * 27315 at exp -2 with nothing rounded away - and it is also why an
	 * invalid reading must never be published as a zero, since a zero here
	 * is a perfectly ordinary winter temperature. */
	build_page1(body, 1u, 0x0000u, 0x0000u, 0x0000u);
	zassert_equal(radiant_env_adapter_decode(&a, ENV_SOURCE, body, 1000u), 1u);
	zassert_equal(radiant_bridge_drain(), 1u);
	s = captured(RADIANT_ENV_FIELD_TEMPERATURE);
	zassert_not_null(s);
	zassert_equal(s->raw, 27315, "0.00 degC is exactly 273.15 K");
	zassert_equal(s->exp, -2);

	/* -40.00 degC = -4000 centi = 0xF060 on the wire. 233.15 K. This is
	 * the one that fails if the sixteen-bit field was read unsigned. */
	cap_n = 0u;
	build_page1(body, 2u, 0x0000u, 0x0000u, 0xF060u);
	zassert_equal(radiant_env_adapter_decode(&a, ENV_SOURCE, body, 2000u), 2u);
	zassert_equal(radiant_bridge_drain(), 2u);
	s = captured(RADIANT_ENV_FIELD_TEMPERATURE);
	zassert_not_null(s);
	zassert_equal(s->raw, 23315, "-40.00 degC is exactly 233.15 K");
}

ZTEST(profile_env, test_adapter_does_not_publish_an_invalid_reading)
{
	struct radiant_env_adapter a;
	uint8_t body[8];

	radiant_env_adapter_init(&a);

	/* Only the event count is publishable, and not on the first page. */
	build_page1(body, 5u, 0x0FCBu, 0x0138u, PROFILE_ENV_INVALID_16);
	zassert_equal(radiant_env_adapter_decode(&a, ENV_SOURCE, body, 1000u), 0u);

	build_page1(body, 6u, 0x0FCBu, 0x0138u, PROFILE_ENV_INVALID_16);
	zassert_equal(radiant_env_adapter_decode(&a, ENV_SOURCE, body, 2000u), 1u);
	zassert_equal(radiant_bridge_drain(), 1u);
	zassert_equal(cap_n, 1u);
	zassert_is_null(captured(RADIANT_ENV_FIELD_TEMPERATURE),
			"273.15 K is not what 'no reading' means");
	zassert_not_null(captured(RADIANT_ENV_FIELD_EVENT_COUNT));
}

ZTEST(profile_env, test_adapter_event_count_wraps_in_eight_bits)
{
	struct radiant_env_adapter a;
	const struct radiant_sample *s;
	uint8_t body[8];

	radiant_env_adapter_init(&a);

	build_page1(body, 0xFEu, 0x0000u, 0x0000u, 0x0000u);
	zassert_equal(radiant_env_adapter_decode(&a, ENV_SOURCE, body, 1000u), 1u);

	cap_n = 0u;
	(void)radiant_bridge_drain();

	/* 0xFE -> 0x03 across the u8 rollover is five events, not 261 and not
	 * a negative. Differencing at the wire width makes that automatic;
	 * widening first is the bug. */
	build_page1(body, 0x03u, 0x0000u, 0x0000u, 0x0000u);
	zassert_equal(radiant_env_adapter_decode(&a, ENV_SOURCE, body, 2000u), 2u);
	zassert_equal(radiant_bridge_drain(), 2u);

	s = captured(RADIANT_ENV_FIELD_EVENT_COUNT);
	zassert_not_null(s);
	zassert_equal(s->field_type, RADIANT_FIELD_EVENT_COUNT);
	zassert_equal(s->flags, RADIANT_SAMPLE_ACCUMULATING);
	zassert_equal(s->exp, 0);
	zassert_equal(s->raw, 5, "254 -> 3 is five events");
	zassert_equal(a.prev_event_count, 0x03u);
}

ZTEST(profile_env, test_adapter_absorbs_page0_and_declines_everything_else)
{
	struct radiant_env_adapter a;
	uint8_t body[8];

	radiant_env_adapter_init(&a);

	/* Page 0 is a description of the sensor, not a reading: absorbed into
	 * the adapter's state, nothing posted. */
	zassert_equal(radiant_env_adapter_decode(&a, ENV_SOURCE, page0, 1000u), 0u);
	zassert_true(a.have_caps);
	zassert_equal(a.caps.default_rate, PROFILE_ENV_RATE_0P5_HZ);

	/* A page in the reserved 2..63 range (section 6.5). Declining is a
	 * normal answer; decoding it as page 1 would publish garbage. */
	memcpy(body, page1, sizeof(body));
	body[0] = 0x02u;
	zassert_equal(radiant_env_adapter_decode(&a, ENV_SOURCE, body, 2000u), 0u);

	/* And a common data page, which is legal on this channel and belongs
	 * to a different adapter entirely. */
	body[0] = 0x54u;
	zassert_equal(radiant_env_adapter_decode(&a, ENV_SOURCE, body, 3000u), 0u);

	zassert_equal(radiant_bridge_drain(), 0u);
	zassert_equal(cap_n, 0u);
}

ZTEST_SUITE(profile_env, NULL, NULL, env_test_reset, NULL, NULL);
