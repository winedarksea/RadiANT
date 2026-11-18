/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work, written against
 * radiant/include/radiant/radiant_power_diag.h.
 *
 * The power proxy is read by a script that has only raw memory, and by a log
 * line that does its own subtraction. Three things can make it quietly wrong:
 *   - a delta taken across a counter wrap coming out as a huge number;
 *   - a duty cycle over 100 % (the on-time and the interval are sampled at
 *     different instants, so a little over is normal and must clamp);
 *   - a torn read: the seq protocol must leave seq even and advanced after
 *     every publish, and the reader's test must refuse odd or changed seqs.
 * The arithmetic is in every build; the last suite exists only where
 * CONFIG_RADIANT_POWER_DIAG compiles the real struct (radiant.unit.diag).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <radiant/radiant_power_diag.h>

#include "../../src/radiant_rat_fold.h"

ZTEST_SUITE(radiant_power_diag, NULL, NULL, NULL, NULL, NULL);

/* --- Layout: the offsets are what an SWD script decodes --- */

ZTEST(radiant_power_diag, test_layout_matches_documented_offsets)
{
	zassert_equal(sizeof(struct radiant_power_diag), RADIANT_POWER_DIAG_SIZE);
	zassert_equal(offsetof(struct radiant_power_diag, magic), 0x00u);
	zassert_equal(offsetof(struct radiant_power_diag, version), 0x04u);
	zassert_equal(offsetof(struct radiant_power_diag, size), 0x08u);
	zassert_equal(offsetof(struct radiant_power_diag, seq), 0x0Cu);
	zassert_equal(offsetof(struct radiant_power_diag, uptime_ms), 0x10u);
	zassert_equal(offsetof(struct radiant_power_diag, hfxo_req_us), 0x14u);
	zassert_equal(offsetof(struct radiant_power_diag, hfxo_requests), 0x18u);
	zassert_equal(offsetof(struct radiant_power_diag, hfxo_releases), 0x1Cu);
	zassert_equal(offsetof(struct radiant_power_diag, radio_tx_us), 0x20u);
	zassert_equal(offsetof(struct radiant_power_diag, radio_rx_us), 0x24u);
	zassert_equal(offsetof(struct radiant_power_diag, radio_tx_ops), 0x28u);
	zassert_equal(offsetof(struct radiant_power_diag, radio_rx_ops), 0x2Cu);
	zassert_equal(offsetof(struct radiant_power_diag, cpu_active_us), 0x30u);
	zassert_equal(offsetof(struct radiant_power_diag, cpu_load_permille), 0x34u);
	zassert_equal(offsetof(struct radiant_power_diag, idle_entries), 0x38u);
	zassert_equal(offsetof(struct radiant_power_diag, flags), 0x3Cu);
	zassert_equal(offsetof(struct radiant_power_diag, constlat_stat), 0x40u);
	zassert_equal(offsetof(struct radiant_power_diag, ti_rf_core_on_us), 0x44u);
	zassert_equal(offsetof(struct radiant_power_diag, ti_rf_powerups), 0x48u);
	zassert_equal(offsetof(struct radiant_power_diag, ti_rf_pu_dur_us), 0x4Cu);
	zassert_equal(offsetof(struct radiant_power_diag, ti_fs_errors), 0x50u);
	zassert_equal(offsetof(struct radiant_power_diag, ti_fold_backsteps), 0x54u);
}

/* --- Deltas of free-running u32 counters --- */

ZTEST(radiant_power_diag, test_delta_without_wrap)
{
	zassert_equal(radiant_power_diag_delta(1500u, 500u), 1000u);
	zassert_equal(radiant_power_diag_delta(7u, 7u), 0u);
}

ZTEST(radiant_power_diag, test_delta_across_wrap)
{
	/* 0xFFFFFFFB -> 5 is ten steps, not four billion. */
	zassert_equal(radiant_power_diag_delta(5u, 0xFFFFFFFBu), 10u);
	zassert_equal(radiant_power_diag_delta(0u, UINT32_MAX), 1u);
	/* A us counter that wrapped inside a 10 s log interval. */
	zassert_equal(radiant_power_diag_delta(9999000u - 4294967u,
					       UINT32_MAX - 4294966u),
		      9999000u);
}

/* --- Duty cycle --- */

ZTEST(radiant_power_diag, test_duty_plain)
{
	zassert_equal(radiant_power_diag_duty_permille(500u, 1000u), 500u);
	zassert_equal(radiant_power_diag_duty_permille(0u, 1000u), 0u);
	zassert_equal(radiant_power_diag_duty_permille(10000000u, 10000000u), 1000u);
	/* 1.7 % of a 250 ms master period (rounds down). */
	zassert_equal(radiant_power_diag_duty_permille(4250u, 250000u), 17u);
}

ZTEST(radiant_power_diag, test_duty_clamps_at_1000)
{
	/* The on-time read a moment after the interval's end: over by a bit. */
	zassert_equal(radiant_power_diag_duty_permille(10000300u, 10000000u), 1000u);
	zassert_equal(radiant_power_diag_duty_permille(UINT32_MAX, 1u), 1000u);
}

ZTEST(radiant_power_diag, test_duty_no_overflow_at_full_scale)
{
	/* on_us * 1000 overflows 32 bits; the result must not. */
	zassert_equal(radiant_power_diag_duty_permille(UINT32_MAX, UINT32_MAX), 1000u);
	zassert_equal(radiant_power_diag_duty_permille(UINT32_MAX / 2u, UINT32_MAX), 499u);
}

ZTEST(radiant_power_diag, test_duty_empty_interval_reads_zero)
{
	zassert_equal(radiant_power_diag_duty_permille(123u, 0u), 0u);
}

ZTEST(radiant_power_diag, test_duty_of_a_wrapped_delta)
{
	/* HFXO held the whole 10 s although its counter wrapped inside it. */
	uint32_t then = UINT32_MAX - 2000000u;
	uint32_t now = then + 10000000u; /* wraps */

	zassert_true(now < then);
	zassert_equal(radiant_power_diag_duty_permille(
			      radiant_power_diag_delta(now, then), 10000000u),
		      1000u);
}

/* --- Rates, on-time, CPU active time --- */

ZTEST(radiant_power_diag, test_rate_x10)
{
	/* 52 wakes in 10 s is 5.2/s. */
	zassert_equal(radiant_power_diag_rate_x10(52u, 10000u), 52u);
	zassert_equal(radiant_power_diag_rate_x10(1u, 1000u), 10u);
	zassert_equal(radiant_power_diag_rate_x10(5u, 0u), 0u);
	zassert_equal(radiant_power_diag_rate_x10(UINT32_MAX, 1u), UINT32_MAX);
}

ZTEST(radiant_power_diag, test_on_us)
{
	zassert_equal(radiant_power_diag_on_us(1500u, 1000u), 500u);
	/* Ended before (or at) its open: never on. */
	zassert_equal(radiant_power_diag_on_us(1000u, 1000u), 0u);
	zassert_equal(radiant_power_diag_on_us(900u, 1000u), 0u);
	/* A 64-bit clock far past the open saturates rather than wrapping. */
	zassert_equal(radiant_power_diag_on_us(((uint64_t)1 << 40), 0u), UINT32_MAX);
}

ZTEST(radiant_power_diag, test_active_us)
{
	zassert_equal(radiant_power_diag_active_us(125u, 1000000u), 125000u);
	zassert_equal(radiant_power_diag_active_us(1000u, 1000000u), 1000000u);
	/* A load over 1000 cannot make active time exceed the interval. */
	zassert_equal(radiant_power_diag_active_us(1500u, 1000000u), 1000000u);
	zassert_equal(radiant_power_diag_active_us(1000u, UINT32_MAX), UINT32_MAX);
}

/* --- The HFXO request accumulator --- */

ZTEST(radiant_power_diag, test_hfxo_held_once_reads_uptime)
{
	/* Today's nRF backend: requested at init, never released. */
	struct radiant_power_diag_hfxo h = { 0 };

	radiant_power_diag_hfxo_note(&h, true, 2000u);
	zassert_equal(radiant_power_diag_hfxo_held(&h, 2000u), 0u);
	zassert_equal(radiant_power_diag_hfxo_held(&h, 10002000u), 10000000u);
	zassert_equal(h.requests, 1u);
	zassert_equal(h.releases, 0u);
	zassert_true(h.on);
}

ZTEST(radiant_power_diag, test_hfxo_release_and_rerequest)
{
	struct radiant_power_diag_hfxo h = { 0 };

	radiant_power_diag_hfxo_note(&h, true, 100u);
	radiant_power_diag_hfxo_note(&h, false, 1100u);
	/* Released: the total stops moving. */
	zassert_equal(radiant_power_diag_hfxo_held(&h, 5000u), 1000u);
	radiant_power_diag_hfxo_note(&h, true, 6000u);
	zassert_equal(radiant_power_diag_hfxo_held(&h, 6500u), 1500u);
	radiant_power_diag_hfxo_note(&h, false, 7000u);
	zassert_equal(radiant_power_diag_hfxo_held(&h, 9000u), 2000u);
	zassert_equal(h.requests, 2u);
	zassert_equal(h.releases, 2u);
	zassert_false(h.on);
}

ZTEST(radiant_power_diag, test_hfxo_repeated_calls_are_not_transitions)
{
	struct radiant_power_diag_hfxo h = { 0 };

	/* A release while not held is ignored entirely. */
	radiant_power_diag_hfxo_note(&h, false, 50u);
	zassert_equal(h.releases, 0u);

	radiant_power_diag_hfxo_note(&h, true, 100u);
	/* A second request must not restart the interval. */
	radiant_power_diag_hfxo_note(&h, true, 600u);
	zassert_equal(h.requests, 1u);
	zassert_equal(radiant_power_diag_hfxo_held(&h, 1100u), 1000u);
	radiant_power_diag_hfxo_note(&h, false, 1100u);
	radiant_power_diag_hfxo_note(&h, false, 1500u);
	zassert_equal(h.releases, 1u);
	zassert_equal(radiant_power_diag_hfxo_held(&h, 2000u), 1000u);
}

ZTEST(radiant_power_diag, test_hfxo_clock_behind_since_adds_nothing)
{
	struct radiant_power_diag_hfxo h = { 0 };

	radiant_power_diag_hfxo_note(&h, true, 1000u);
	zassert_equal(radiant_power_diag_hfxo_held(&h, 900u), 0u);
	radiant_power_diag_hfxo_note(&h, false, 900u);
	zassert_equal(radiant_power_diag_hfxo_held(&h, 900u), 0u);
}

/* --- The cc26xx radio-timer fold: wrap or back-step (ti_fold_backsteps) --- */

ZTEST(radiant_power_diag, test_rat_step_forward)
{
	zassert_equal(radiant_rat_step_classify(1000u, 1000u), RADIANT_RAT_STEP_FORWARD);
	zassert_equal(radiant_rat_step_classify(1000u, 1001u), RADIANT_RAT_STEP_FORWARD);
	zassert_equal(radiant_rat_step_classify(0u, 0xFFFFFFFFu), RADIANT_RAT_STEP_FORWARD);
	zassert_equal(radiant_rat_backstep_ticks(1000u, 1001u), 0u);
}

ZTEST(radiant_power_diag, test_rat_step_genuine_wrap)
{
	/* The keepalive samples once a minute (240 M ticks): a wrap seen at
	 * any sampling interval below half the 2^32 period. */
	zassert_equal(radiant_rat_step_classify(0xFFFFFF00u, 0x00000010u),
		      RADIANT_RAT_STEP_WRAP);
	zassert_equal(radiant_rat_step_classify(0xF0000000u, 0x0E4E1C00u),
		      RADIANT_RAT_STEP_WRAP);
	/* Exactly 2^31 behind is the boundary, and counts as a wrap. */
	zassert_equal(radiant_rat_step_classify(0x80000000u, 0x00000000u),
		      RADIANT_RAT_STEP_WRAP);
	zassert_equal(radiant_rat_backstep_ticks(0xFFFFFF00u, 0x10u), 0u);
}

ZTEST(radiant_power_diag, test_rat_step_small_backstep)
{
	/* An RTC-synthesised read while the RF core is off is rounded up one
	 * 32768 Hz tick, about 122 radio-timer ticks; the next real read can
	 * land that far behind it. */
	zassert_equal(radiant_rat_step_classify(5000122u, 5000000u),
		      RADIANT_RAT_STEP_BACK);
	zassert_equal(radiant_rat_backstep_ticks(5000122u, 5000000u), 122u);
	zassert_equal(radiant_rat_step_classify(1u, 0u), RADIANT_RAT_STEP_BACK);
	zassert_equal(radiant_rat_backstep_ticks(1u, 0u), 1u);
	/* One tick short of the boundary is still a back-step, and a back-step
	 * across the top of the counter is still small. */
	zassert_equal(radiant_rat_step_classify(0x80000000u, 0x00000001u),
		      RADIANT_RAT_STEP_BACK);
	zassert_equal(radiant_rat_backstep_ticks(0x80000000u, 0x00000001u),
		      0x7FFFFFFFu);
	zassert_equal(radiant_rat_step_classify(0xFFFFFFFFu, 0xFFFFFF85u),
		      RADIANT_RAT_STEP_BACK);
}

/* --- The seq protocol --- */

ZTEST(radiant_power_diag, test_publish_leaves_seq_even_and_advanced)
{
	struct radiant_power_diag dst;
	struct radiant_power_diag src;

	memset(&dst, 0, sizeof(dst));
	memset(&src, 0, sizeof(src));
	src.magic = RADIANT_POWER_DIAG_MAGIC;
	src.version = RADIANT_POWER_DIAG_VERSION;
	src.size = RADIANT_POWER_DIAG_SIZE;
	src.seq = 0xDEADu; /* ignored */
	src.uptime_ms = 1234u;
	src.radio_tx_us = 77u;
	src.ti_fold_backsteps = RADIANT_POWER_DIAG_NOT_MEASURED;

	radiant_power_diag_publish(&dst, &src);
	zassert_equal(dst.seq, 2u, "seq %u", dst.seq);
	zassert_equal(dst.magic, RADIANT_POWER_DIAG_MAGIC);
	zassert_equal(dst.size, RADIANT_POWER_DIAG_SIZE);
	zassert_equal(dst.uptime_ms, 1234u);
	zassert_equal(dst.radio_tx_us, 77u);
	/* The last field: the copy reached the end of the struct. */
	zassert_equal(dst.ti_fold_backsteps, RADIANT_POWER_DIAG_NOT_MEASURED);

	src.uptime_ms = 2234u;
	radiant_power_diag_publish(&dst, &src);
	zassert_equal(dst.seq, 4u);
	zassert_equal(dst.uptime_ms, 2234u);
}

ZTEST(radiant_power_diag, test_publish_recovers_from_an_odd_seq)
{
	struct radiant_power_diag dst;
	struct radiant_power_diag src;

	memset(&dst, 0, sizeof(dst));
	memset(&src, 0, sizeof(src));
	/* As if a writer had stopped half-way: the next publish ends even. */
	dst.seq = 5u;
	radiant_power_diag_publish(&dst, &src);
	zassert_equal(dst.seq, 6u);
	/* And across the u32 wrap: even, and different from where it was. */
	dst.seq = UINT32_MAX - 1u;
	radiant_power_diag_publish(&dst, &src);
	zassert_equal(dst.seq, 0u);
}

ZTEST(radiant_power_diag, test_reader_refuses_torn_reads)
{
	zassert_true(radiant_power_diag_seq_consistent(2u, 2u));
	zassert_true(radiant_power_diag_seq_consistent(0u, 0u));
	/* A write in progress when the reader started. */
	zassert_false(radiant_power_diag_seq_consistent(3u, 3u));
	/* A write landed between the reader's two reads of seq. */
	zassert_false(radiant_power_diag_seq_consistent(2u, 4u));
	zassert_false(radiant_power_diag_seq_consistent(2u, 3u));
}

#if defined(CONFIG_RADIANT_POWER_DIAG)
/* --- The real struct (radiant.unit.diag only) --- */

/* Read radiant_power_diag the way an SWD script must. */
static struct radiant_power_diag read_published(void)
{
	struct radiant_power_diag out;
	volatile const struct radiant_power_diag *p = &radiant_power_diag;
	uint32_t s1;
	uint32_t s2;

	do {
		s1 = p->seq;
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		memcpy(&out, (const void *)p, sizeof(out));
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		s2 = p->seq;
	} while (!radiant_power_diag_seq_consistent(s1, s2));
	return out;
}

ZTEST(radiant_power_diag, test_global_header_and_sentinels)
{
	struct radiant_power_diag d = read_published();

	zassert_equal(d.magic, RADIANT_POWER_DIAG_MAGIC);
	zassert_equal(d.version, RADIANT_POWER_DIAG_VERSION);
	zassert_equal(d.size, RADIANT_POWER_DIAG_SIZE);
	/* The ti_* fields are cc26xx only, and this image's radio is the fake. */
	zassert_equal(d.flags & RADIANT_POWER_DIAG_F_TI, 0u);
	zassert_equal(d.ti_rf_core_on_us, RADIANT_POWER_DIAG_NOT_MEASURED);
	zassert_equal(d.ti_rf_powerups, RADIANT_POWER_DIAG_NOT_MEASURED);
	zassert_equal(d.ti_rf_pu_dur_us, RADIANT_POWER_DIAG_NOT_MEASURED);
	zassert_equal(d.ti_fs_errors, RADIANT_POWER_DIAG_NOT_MEASURED);
	zassert_equal(d.ti_fold_backsteps, RADIANT_POWER_DIAG_NOT_MEASURED);
	zassert_equal(d.hfxo_req_us, RADIANT_POWER_DIAG_NOT_MEASURED);
	zassert_equal(d.flags & RADIANT_POWER_DIAG_F_HFXO_MEASURED, 0u);
	zassert_equal(d.constlat_stat, RADIANT_POWER_DIAG_NOT_MEASURED);
}

ZTEST(radiant_power_diag, test_snapshot_publishes_radio_counters)
{
	struct radiant_power_diag before;
	struct radiant_power_diag after;

	/* Let at least one snapshot land, so `before` is a published one. */
	k_sleep(K_MSEC(RADIANT_POWER_DIAG_SNAPSHOT_MS + 100u));
	before = read_published();
	zassert_true(before.seq != 0u && (before.seq & 1u) == 0u,
		     "seq %u", before.seq);

	radiant_power_diag_radio(true, 123u);
	radiant_power_diag_radio(false, 4567u);
	radiant_power_diag_radio(false, 0u);
	k_sleep(K_MSEC(RADIANT_POWER_DIAG_SNAPSHOT_MS + 100u));
	after = read_published();

	zassert_true(radiant_power_diag_delta(after.seq, before.seq) >= 2u);
	zassert_true(radiant_power_diag_delta(after.uptime_ms, before.uptime_ms) >=
		     RADIANT_POWER_DIAG_SNAPSHOT_MS);
	zassert_true(radiant_power_diag_delta(after.radio_tx_us, before.radio_tx_us) >= 123u);
	zassert_true(radiant_power_diag_delta(after.radio_tx_ops, before.radio_tx_ops) >= 1u);
	zassert_true(radiant_power_diag_delta(after.radio_rx_us, before.radio_rx_us) >= 4567u);
	zassert_true(radiant_power_diag_delta(after.radio_rx_ops, before.radio_rx_ops) >= 2u);
}
#endif /* CONFIG_RADIANT_POWER_DIAG */
