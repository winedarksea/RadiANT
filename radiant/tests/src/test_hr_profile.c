/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work, against radiant_hr_profile.h/.c,
 * which transcribes docs/radiant-bridge.md sections 6.2-6.6.
 *
 * What earns a case: the quantiles interpolate inside a bin; forgetting
 * halves every bin and keeps their ratios; the blend converges from the prior
 * toward the observation at the stated weight; the clamps hold against a
 * strap that reads nonsense; eviction is least-recently-active; zero slots
 * (and an empty histogram) give exactly the prior's thresholds; one 240 bpm
 * artifact moves nothing; and the store is written only when the gating
 * allows.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <radiant/radiant_binding.h>
#include <radiant/radiant_hr_profile.h>

/* radiant/tests does not enable RADIANT_BRIDGE_RULES; the source falls back
 * to the same default. */
#ifndef CONFIG_RADIANT_BRIDGE_HR_PROFILES
#define CONFIG_RADIANT_BRIDGE_HR_PROFILES 8
#endif

static void add_n(struct radiant_hr_hist *h, uint32_t bpm, uint32_t n)
{
	for (uint32_t i = 0u; i < n; i++) {
		radiant_hr_hist_add(h, bpm);
	}
}

/* ---- fake store ---------------------------------------------------------- */

static struct {
	uint32_t saves;
	uint32_t loads;
	uint32_t erases;
	uint64_t last_uuid;
	bool     have;
	struct radiant_hr_profile_record rec;
	int      save_rc;
} fs;

static int fake_load(uint64_t uuid, struct radiant_hr_profile_record *rec)
{
	fs.loads++;
	if (!fs.have || fs.last_uuid != uuid) {
		return -ENOENT;
	}
	*rec = fs.rec;
	return 0;
}

static int fake_save(uint64_t uuid, const struct radiant_hr_profile_record *rec)
{
	fs.saves++;
	if (fs.save_rc != 0) {
		return fs.save_rc;
	}
	fs.last_uuid = uuid;
	fs.rec = *rec;
	fs.have = true;
	return 0;
}

static int fake_erase(uint64_t uuid)
{
	fs.erases++;
	if (fs.have && fs.last_uuid == uuid) {
		fs.have = false;
	}
	return 0;
}

static const struct radiant_hr_profile_store fake_store = {
	.load = fake_load,
	.save = fake_save,
	.erase = fake_erase,
};

static void hrp_reset(void *unused)
{
	ARG_UNUSED(unused);
	memset(&fs, 0, sizeof(fs));
	radiant_binding_init();
	radiant_hr_profile_set_store(NULL);
	radiant_hr_profile_reset();
}

/* ---- the estimator ------------------------------------------------------- */

ZTEST(radiant_hr_profile, test_quantiles_interpolate_inside_a_bin)
{
	struct radiant_hr_hist h = { 0 };

	zassert_equal(radiant_hr_hist_quantile_x10(&h, 500u), 0u, "empty = 0");

	/* 100 samples, all in the 140-144 bin: the quantiles spread across
	 * the bin's 5 bpm instead of all reading 140. */
	add_n(&h, 142u, 100u);
	zassert_equal(radiant_hr_hist_total(&h), 100u);
	zassert_equal(radiant_hr_hist_quantile_x10(&h, 10u), 1400u);   /* 140.05, truncated */
	zassert_equal(radiant_hr_hist_quantile_x10(&h, 500u), 1425u);       /* 142.5 */
	zassert_equal(radiant_hr_hist_quantile_x10(&h, 990u), 1449u);  /* 144.95 */

	/* Two bins: 50 at 60-64, 50 at 100-104. The median sits at the top of
	 * the lower one, p99 inside the upper one. */
	memset(&h, 0, sizeof(h));
	add_n(&h, 61u, 50u);
	add_n(&h, 101u, 50u);
	zassert_equal(radiant_hr_hist_quantile_x10(&h, 500u), 650u);
	zassert_equal(radiant_hr_hist_quantile_x10(&h, 990u), 1000u + 49u);
	zassert_equal(radiant_hr_hist_quantile_x10(&h, 10u), 601u);
}

ZTEST(radiant_hr_profile, test_out_of_range_readings_are_dropped)
{
	struct radiant_hr_hist h = { 0 };

	radiant_hr_hist_add(&h, 0u);
	radiant_hr_hist_add(&h, 29u);
	radiant_hr_hist_add(&h, 220u);
	radiant_hr_hist_add(&h, 240u);
	zassert_equal(radiant_hr_hist_total(&h), 0u);

	radiant_hr_hist_add(&h, 30u);
	radiant_hr_hist_add(&h, 219u);
	zassert_equal(h.bin[0], 1u);
	zassert_equal(h.bin[RADIANT_HR_HIST_BINS - 1u], 1u);
}

ZTEST(radiant_hr_profile, test_a_saturating_bin_halves_every_bin)
{
	struct radiant_hr_hist h = { 0 };

	add_n(&h, 60u, 1000u);
	add_n(&h, 150u, UINT16_MAX - 1u);
	zassert_equal(h.bin[(150u - 30u) / 5u], UINT16_MAX - 1u);
	zassert_equal(h.bin[(60u - 30u) / 5u], 1000u);

	/* The sample that reaches the ceiling halves everything. */
	radiant_hr_hist_add(&h, 150u);
	zassert_equal(h.bin[(150u - 30u) / 5u], UINT16_MAX / 2u);
	zassert_equal(h.bin[(60u - 30u) / 5u], 500u, "ratios survive the halving");

	/* And the bin never wraps, however long it runs. */
	add_n(&h, 150u, 200000u);
	zassert_true(h.bin[(150u - 30u) / 5u] < UINT16_MAX);
	zassert_true(h.bin[(150u - 30u) / 5u] > 0u);
}

ZTEST(radiant_hr_profile, test_an_empty_histogram_is_exactly_the_prior)
{
	struct radiant_hr_thresholds a;
	struct radiant_hr_thresholds b;
	struct radiant_hr_hist       h = { 0 };

	/* Section 6.2's worked numbers. */
	radiant_hr_thresholds_compute(NULL, 180u, 65u, 1800u, &a);
	zassert_equal(a.hr_max, 180u);
	zassert_equal(a.hr_rest, 65u);
	zassert_equal(a.rest_below, 82u);
	zassert_equal(a.zone2_at, 134u);
	zassert_equal(a.zone3_at, 146u);

	/* n = 0 through the same path: identical, field for field. */
	radiant_hr_thresholds_compute(&h, 180u, 65u, 1800u, &b);
	zassert_mem_equal(&a, &b, sizeof(a));

	/* And the Kconfig default is that same prior. */
	radiant_hr_thresholds_default(NULL, &b);
	zassert_mem_equal(&a, &b, sizeof(a));

	/* A single-user home's own numbers: 190 / 50 -> HRR 140. */
	radiant_hr_thresholds_compute(NULL, 190u, 50u, 1800u, &a);
	zassert_equal(a.rest_below, 71u);  /* 50 + 21 */
	zassert_equal(a.zone2_at, 134u);   /* 50 + 84 */
	zassert_equal(a.zone3_at, 148u);   /* 50 + 98 */
}

ZTEST(radiant_hr_profile, test_the_blend_converges_at_the_stated_weight)
{
	struct radiant_hr_hist       h = { 0 };
	struct radiant_hr_thresholds t;

	/* A rider whose session spans 60-64 and 190-194 bpm, half each. With
	 * N = N0 the estimate is the midpoint of prior and observation. */
	add_n(&h, 62u, 900u);
	add_n(&h, 192u, 900u);
	radiant_hr_thresholds_compute(&h, 180u, 65u, 1800u, &t);
	/* obs_max = p99 = 190 + 0.98*5 = 194.9 (194.9 in tenths: 1949);
	 * (1800 + 1949) / 2 = 1874.5, rounded to 187.5, reported as 188. */
	zassert_equal(t.hr_max, 188u);
	/* obs_rest = p01 = 60 + 0.02*5 = 60.1; (65 + 60.1) / 2 = 62.55 */
	zassert_equal(t.hr_rest, 63u);

	/* Ten times the weight: nearly the observation. */
	memset(&h, 0, sizeof(h));
	add_n(&h, 62u, 9000u);
	add_n(&h, 192u, 9000u);
	radiant_hr_thresholds_compute(&h, 180u, 65u, 1800u, &t);
	zassert_true(t.hr_max >= 193u && t.hr_max <= 195u, "hr_max %u", t.hr_max);
	zassert_true(t.hr_rest >= 60u && t.hr_rest <= 61u, "hr_rest %u", t.hr_rest);

	/* Monotonic in N: more wear never moves the estimate back. */
	{
		uint8_t prev = 180u;

		memset(&h, 0, sizeof(h));
		for (uint32_t k = 0u; k < 20u; k++) {
			add_n(&h, 202u, 600u);
			radiant_hr_thresholds_compute(&h, 180u, 65u, 1800u, &t);
			zassert_true(t.hr_max >= prev);
			prev = t.hr_max;
		}
		zassert_true(prev >= 200u);
	}
}

ZTEST(radiant_hr_profile, test_clamps_keep_the_zones_ordered)
{
	struct radiant_hr_hist       h = { 0 };
	struct radiant_hr_thresholds t;

	/* A strap stuck reading 35 bpm for days: p99 would be ~40. */
	add_n(&h, 35u, 60000u);
	radiant_hr_thresholds_compute(&h, 180u, 65u, 1800u, &t);
	zassert_equal(t.hr_max, RADIANT_HR_MAX_FLOOR);
	zassert_true(t.hr_rest >= RADIANT_HR_REST_FLOOR);
	zassert_true(t.hr_max - t.hr_rest >= 40u, "HRR >= 40");
	zassert_true(t.rest_below < t.zone2_at);
	zassert_true(t.zone2_at < t.zone3_at);

	/* Cadence-lock near the top of the range for days: p01 ~215. */
	memset(&h, 0, sizeof(h));
	add_n(&h, 217u, 60000u);
	radiant_hr_thresholds_compute(&h, 180u, 65u, 1800u, &t);
	zassert_equal(t.hr_rest, RADIANT_HR_REST_CEIL);
	zassert_true(t.hr_max <= RADIANT_HR_MAX_CEIL);
	zassert_true(t.hr_max - t.hr_rest >= 40u, "HRR >= 40");
	zassert_true(t.rest_below < t.zone2_at);
	zassert_true(t.zone2_at < t.zone3_at);
}

ZTEST(radiant_hr_profile, test_a_240_bpm_artifact_moves_nothing)
{
	struct radiant_hr_hist       h = { 0 };
	struct radiant_hr_thresholds clean;
	struct radiant_hr_thresholds dirty;

	add_n(&h, 70u, 1000u);
	add_n(&h, 150u, 1000u);
	radiant_hr_thresholds_compute(&h, 180u, 65u, 1800u, &clean);

	/* Section 6.3: "One 240 bpm artifact destroys a running maximum
	 * permanently; a p99 does not notice it." Out of range, so dropped
	 * outright - and a handful of in-range spikes stay under p99. */
	add_n(&h, 240u, 5u);
	add_n(&h, 218u, 5u);
	radiant_hr_thresholds_compute(&h, 180u, 65u, 1800u, &dirty);
	zassert_equal(clean.hr_max, dirty.hr_max);
	zassert_equal(clean.zone3_at, dirty.zone3_at);
	zassert_equal(clean.rest_below, dirty.rest_below);
}

/* ---- the slot table ------------------------------------------------------ */

ZTEST(radiant_hr_profile, test_eviction_is_least_recently_active)
{
	uint32_t cap = CONFIG_RADIANT_BRIDGE_HR_PROFILES;

	zassert_true(cap >= 2u, "the suite needs at least two slots");

	for (uint32_t i = 0u; i < cap; i++) {
		zassert_true(radiant_hr_profile_attach(1000u + i));
		radiant_hr_profile_add(1000u + i, 100u);
	}
	/* Touch the oldest, so the second-oldest is now least recent. */
	(void)radiant_hr_profile_session_end(1000u);

	zassert_true(radiant_hr_profile_attach(5000u));
	zassert_not_null(radiant_hr_profile_hist(1000u), "recently active: kept");
	zassert_is_null(radiant_hr_profile_hist(1001u), "least recent: evicted");
	zassert_not_null(radiant_hr_profile_hist(5000u));
	zassert_equal(radiant_hr_hist_total(radiant_hr_profile_hist(5000u)), 0u);

	/* The evicted binding is not an error: it reads the prior. */
	{
		struct radiant_hr_thresholds a;
		struct radiant_hr_thresholds b;

		radiant_hr_profile_thresholds(1001u, &a);
		radiant_hr_thresholds_default(NULL, &b);
		zassert_mem_equal(&a, &b, sizeof(a));
	}
}

ZTEST(radiant_hr_profile, test_store_writes_are_gated)
{
	const uint64_t uuid = 0xA11CEu;

	radiant_hr_profile_set_store(&fake_store);
	zassert_true(radiant_hr_profile_attach(uuid));
	zassert_equal(fs.loads, 1u, "a new slot is loaded from the store");

	/* Under five minutes of new samples: no write. */
	for (uint32_t i = 0u; i < RADIANT_HR_PROFILE_MIN_SAVE_SAMPLES - 1u; i++) {
		radiant_hr_profile_add(uuid, 120u);
	}
	zassert_false(radiant_hr_profile_session_end(uuid));
	zassert_equal(fs.saves, 0u);

	/* At five minutes: one write, of version 1 and the histogram. */
	radiant_hr_profile_add(uuid, 120u);
	zassert_true(radiant_hr_profile_session_end(uuid));
	zassert_equal(fs.saves, 1u);
	zassert_equal(fs.rec.version, RADIANT_HR_PROFILE_RECORD_VERSION);
	zassert_equal(radiant_hr_hist_total(&fs.rec.hist),
		      RADIANT_HR_PROFILE_MIN_SAVE_SAMPLES);

	/* A spoofed strap toggling worn: session ends with nothing new do
	 * not write... */
	for (uint32_t i = 0u; i < 50u; i++) {
		zassert_false(radiant_hr_profile_session_end(uuid));
	}
	zassert_equal(fs.saves, 1u);

	/* ...and even with real samples, the per-boot cap stops it. */
	for (uint32_t k = 0u; k < 2u * RADIANT_HR_PROFILE_SAVES_PER_BOOT; k++) {
		for (uint32_t i = 0u; i < RADIANT_HR_PROFILE_MIN_SAVE_SAMPLES; i++) {
			radiant_hr_profile_add(uuid, 120u);
		}
		(void)radiant_hr_profile_session_end(uuid);
	}
	zassert_equal(fs.saves, RADIANT_HR_PROFILE_SAVES_PER_BOOT);
}

ZTEST(radiant_hr_profile, test_a_reboot_reloads_and_forget_erases)
{
	const uint64_t uuid = 0xB0B;

	radiant_hr_profile_set_store(&fake_store);
	zassert_true(radiant_hr_profile_attach(uuid));
	for (uint32_t i = 0u; i < RADIANT_HR_PROFILE_MIN_SAVE_SAMPLES; i++) {
		radiant_hr_profile_add(uuid, 90u);
	}
	zassert_true(radiant_hr_profile_session_end(uuid));

	/* "Reboot": RAM gone, store kept. */
	radiant_hr_profile_reset();
	zassert_is_null(radiant_hr_profile_hist(uuid));
	zassert_true(radiant_hr_profile_attach(uuid));
	zassert_equal(radiant_hr_hist_total(radiant_hr_profile_hist(uuid)),
		      RADIANT_HR_PROFILE_MIN_SAVE_SAMPLES, "restored from the store");

	/* User eviction: the record is erased at once, the slot is dropped
	 * before the next lookup. */
	radiant_hr_profile_forget(uuid);
	zassert_equal(fs.erases, 1u);
	zassert_false(fs.have);
	zassert_is_null(radiant_hr_profile_hist(uuid));
}

ZTEST(radiant_hr_profile, test_a_failing_store_is_not_retried_forever)
{
	const uint64_t uuid = 0xFA11;

	radiant_hr_profile_set_store(&fake_store);
	fs.save_rc = -EIO;
	zassert_true(radiant_hr_profile_attach(uuid));
	for (uint32_t k = 0u; k < 2u * RADIANT_HR_PROFILE_SAVES_PER_BOOT; k++) {
		for (uint32_t i = 0u; i < RADIANT_HR_PROFILE_MIN_SAVE_SAMPLES; i++) {
			radiant_hr_profile_add(uuid, 120u);
		}
		zassert_false(radiant_hr_profile_session_end(uuid));
	}
	zassert_equal(fs.saves, RADIANT_HR_PROFILE_SAVES_PER_BOOT);
}

ZTEST_SUITE(radiant_hr_profile, NULL, NULL, hrp_reset, NULL, NULL);
