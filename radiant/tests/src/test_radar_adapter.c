/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_radar_adapter.c - Bike Radar 0x28 onto the sample bus: the nearest
 * target is computed (targets are not sorted), threat-0 targets are never
 * decoded further, and target information goes unavailable on the 2 s
 * timeout, page 87, Shutdown Requested/Forced and the clear-targets bit.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include <radiant/radiant_binding.h>
#include <radiant/radiant_bridge.h>
#include <radiant/radiant_profile_radar.h>
#include <radiant/radiant_radar_adapter.h>

#define SRC     0x2828u
#define CAP_MAX 32

static struct radiant_sample cap[CAP_MAX];
static uint32_t              cap_n;

static bool cap_want(const struct radiant_sample *s)
{
	return s->source == SRC;
}

static void cap_publish(const struct radiant_sample *s)
{
	if (cap_n < CAP_MAX) {
		cap[cap_n++] = *s;
	}
}

RADIANT_SINK_DEFINE(test_radar_sink, cap_want, cap_publish, NULL);

/* The LAST captured sample for a field, or NULL. */
static const struct radiant_sample *find(uint8_t field_id)
{
	const struct radiant_sample *hit = NULL;

	for (uint32_t i = 0u; i < cap_n; i++) {
		if (cap[i].field_id == field_id) {
			hit = &cap[i];
		}
	}
	return hit;
}

#define REQUIRE(_s, _f)                                                       \
	do {                                                                  \
		(_s) = find(_f);                                              \
		zassert_not_null((_s), "field %u not posted", (_f));          \
		if ((_s) == NULL) {                                           \
			return;                                               \
		}                                                             \
	} while (0)

static struct radiant_radar_adapter ad;

static void reset(void *unused)
{
	ARG_UNUSED(unused);
	radiant_bridge_reset();
	radiant_binding_init();
	memset(cap, 0, sizeof(cap));
	cap_n = 0u;
	radiant_radar_adapter_init(&ad);
}

static void drain(void)
{
	cap_n = 0u;
	(void)radiant_bridge_drain();
}

ZTEST_SUITE(radiant_radar_adapter, NULL, NULL, reset, NULL, NULL);

static void targets(uint8_t page, const struct radiant_profile_radar_target t[4],
		    uint8_t *body)
{
	zassert_equal(radiant_profile_radar_encode_targets(page, t, body), 0);
}

ZTEST(radiant_radar_adapter, test_unavailable_until_first_target_page)
{
	const struct radiant_sample *s;

	radiant_radar_adapter_tick(&ad, SRC, 1000u);
	drain();
	REQUIRE(s, RADIANT_RADAR_FIELD_AVAILABLE);
	zassert_equal(s->raw, 0);
}

ZTEST(radiant_radar_adapter, test_nearest_target_is_computed_not_assumed)
{
	struct radiant_profile_radar_target t[4];
	uint8_t body[8];
	const struct radiant_sample *s;

	memset(t, 0, sizeof(t));
	/* Slot 1 is FARTHER than slot 2: the radar need not sort (Rev 2.1). */
	t[0] = (struct radiant_profile_radar_target){true, 1u, 0u, 30u, 2u};
	t[1] = (struct radiant_profile_radar_target){true, 2u, 2u, 8u, 5u};
	targets(0x30u, t, body);
	radiant_radar_adapter_decode(&ad, SRC, body, 1000000u);
	drain();

	REQUIRE(s, RADIANT_RADAR_FIELD_AVAILABLE);
	zassert_equal(s->raw, 1);
	REQUIRE(s, RADIANT_RADAR_FIELD_TARGET_COUNT);
	zassert_equal(s->raw, 2);
	REQUIRE(s, RADIANT_RADAR_FIELD_THREAT);
	zassert_equal(s->raw, 2);
	REQUIRE(s, RADIANT_RADAR_FIELD_NEAREST_RANGE);
	zassert_equal(s->raw, 25000, "8 x 3.125 m in mm");
	zassert_equal(s->exp, -3);
	zassert_equal(s->field_type, RADIANT_FIELD_LENGTH);
	REQUIRE(s, RADIANT_RADAR_FIELD_NEAREST_SPEED);
	zassert_equal(s->raw, 15200);
	REQUIRE(s, RADIANT_RADAR_FIELD_NEAREST_SIDE);
	zassert_equal(s->raw, RADIANT_PROFILE_RADAR_SIDE_LEFT);
}

ZTEST(radiant_radar_adapter, test_idle_page_posts_no_nearest_target)
{
	static const uint8_t idle[8] = {0x30, 0, 0, 0, 0, 0, 0, 0};
	const struct radiant_sample *s;

	radiant_radar_adapter_decode(&ad, SRC, idle, 1000u);
	drain();
	REQUIRE(s, RADIANT_RADAR_FIELD_TARGET_COUNT);
	zassert_equal(s->raw, 0);
	zassert_is_null(find(RADIANT_RADAR_FIELD_NEAREST_RANGE),
			"no target means no range, not a zero range");
}

ZTEST(radiant_radar_adapter, test_post_on_change_only)
{
	static const uint8_t idle[8] = {0x30, 0, 0, 0, 0, 0, 0, 0};

	radiant_radar_adapter_decode(&ad, SRC, idle, 1000u);
	drain();
	radiant_radar_adapter_decode(&ad, SRC, idle, 126000u);
	drain();
	zassert_equal(cap_n, 0u, "an unchanged page posts nothing");
}

ZTEST(radiant_radar_adapter, test_two_second_timeout_is_time_based)
{
	static const uint8_t idle[8] = {0x30, 0, 0, 0, 0, 0, 0, 0};
	const struct radiant_sample *s;

	radiant_radar_adapter_decode(&ad, SRC, idle, 1000000u);
	drain();
	radiant_radar_adapter_tick(&ad, SRC, 2900000u);
	drain();
	zassert_is_null(find(RADIANT_RADAR_FIELD_AVAILABLE), "1.9 s: still current");
	radiant_radar_adapter_tick(&ad, SRC, 3000000u);
	drain();
	REQUIRE(s, RADIANT_RADAR_FIELD_AVAILABLE);
	zassert_equal(s->raw, 0);
	radiant_radar_adapter_tick(&ad, SRC, 3500000u);
	drain();
	zassert_equal(cap_n, 0u, "posted once, not every tick");
}

ZTEST(radiant_radar_adapter, test_error_page_clears_regardless_of_content)
{
	static const uint8_t idle[8] = {0x30, 0, 0, 0, 0, 0, 0, 0};
	/* Page 87 with garbage beyond the page number still clears (6.4.2). */
	static const uint8_t err[8] = {0x57, 0x00, 0x30, 0x01, 1, 2, 3, 4};
	const struct radiant_sample *s;

	radiant_radar_adapter_decode(&ad, SRC, idle, 1000u);
	drain();
	radiant_radar_adapter_decode(&ad, SRC, err, 2000u);
	drain();
	REQUIRE(s, RADIANT_RADAR_FIELD_AVAILABLE);
	zassert_equal(s->raw, 0);
	REQUIRE(s, RADIANT_RADAR_FIELD_ERROR_CODE);
	zassert_equal(s->raw, RADIANT_PROFILE_RADAR_ERR_UNIT_SKEW);
}

ZTEST(radiant_radar_adapter, test_status_states_and_clear_bit)
{
	static const uint8_t idle[8] = {0x30, 0, 0, 0, 0, 0, 0, 0};
	/* On air, RTL515 C7: state 0, clear-targets requested (bit 0 = 0). */
	static const uint8_t varia_clear[8] = {0x01, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF,
					       0xFF, 0xFE};
	/* On air, RTL515 C2: Shutdown Aborted, no clear. */
	static const uint8_t varia_aborted[8] = {0x01, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF,
						 0xFF, 0xFF};
	const struct radiant_sample *s;

	radiant_radar_adapter_decode(&ad, SRC, idle, 1000u);
	drain();
	radiant_radar_adapter_decode(&ad, SRC, varia_aborted, 2000u);
	drain();
	REQUIRE(s, RADIANT_RADAR_FIELD_DEVICE_STATE);
	zassert_equal(s->raw, RADIANT_PROFILE_RADAR_STATE_SHUTDOWN_ABORTED);
	zassert_is_null(find(RADIANT_RADAR_FIELD_AVAILABLE),
			"aborted does not clear targets");

	radiant_radar_adapter_decode(&ad, SRC, varia_clear, 3000u);
	drain();
	REQUIRE(s, RADIANT_RADAR_FIELD_AVAILABLE);
	zassert_equal(s->raw, 0);
}

ZTEST(radiant_radar_adapter, test_page_49_half_and_its_expiry)
{
	struct radiant_profile_radar_target a[4], b[4];
	uint8_t pa[8], pb[8];
	const struct radiant_sample *s;

	for (int i = 0; i < 4; i++) {
		a[i] = (struct radiant_profile_radar_target){true, 1u, 0u, (uint8_t)(20 + i), 1u};
	}
	memset(b, 0, sizeof(b));
	b[0] = (struct radiant_profile_radar_target){true, 1u, 1u, 5u, 1u};
	targets(0x30u, a, pa);
	targets(0x31u, b, pb);
	radiant_radar_adapter_decode(&ad, SRC, pa, 1000000u);
	radiant_radar_adapter_decode(&ad, SRC, pb, 1125000u);
	drain();
	REQUIRE(s, RADIANT_RADAR_FIELD_TARGET_COUNT);
	zassert_equal(s->raw, 5);
	REQUIRE(s, RADIANT_RADAR_FIELD_NEAREST_RANGE);
	zassert_equal(s->raw, 15625, "target 5 is the nearest");

	/* Page 48 only, after a gap: the stale page-49 half is dropped. */
	radiant_radar_adapter_decode(&ad, SRC, pa, 1600000u);
	drain();
	REQUIRE(s, RADIANT_RADAR_FIELD_TARGET_COUNT);
	zassert_equal(s->raw, 4);
}
