/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_ble_policy.c - radiant_ble_policy.c: when the advertiser is restarted,
 * how a failed restart backs off, the connection-parameter rewrite, the
 * per-unit name and the pairing window.
 *
 * The advertising cases replay the event sequences the Zephyr glue feeds the
 * policy (radiant_ble_adv.c): connected / disconnected / recycled from the
 * BT RX thread, a start result from the work item, the watchdog from the
 * application tick. The failure they exist for is the one apps/hrm_ble and
 * apps/treadmill shipped: a single restart from `disconnected`, no retry, and
 * a device undiscoverable until power-cycled.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <radiant/radiant_ble_adv.h>
#include <radiant/radiant_ble_conn.h>
#include <radiant/radiant_ble_pairing.h>

#define MIN_MS 100u
#define MAX_MS 5000u
#define WD_MS  5000u

static struct radiant_ble_adv_state st;

static void adv_before(void *f)
{
	ARG_UNUSED(f);
	radiant_ble_adv_state_init(&st, 1u, MIN_MS, MAX_MS, WD_MS);
}

ZTEST_SUITE(ble_adv, NULL, NULL, adv_before, NULL, NULL);

ZTEST(ble_adv, test_nothing_starts_until_advertising_is_wanted)
{
	zassert_false(radiant_ble_adv_may_start(&st));
	zassert_false(radiant_ble_adv_begin_attempt(&st));
	zassert_false(radiant_ble_adv_note_recycled(&st));
	zassert_false(radiant_ble_adv_watchdog_due(&st, 100000u));

	st.want = true;
	zassert_true(radiant_ble_adv_may_start(&st));
	zassert_true(radiant_ble_adv_begin_attempt(&st));
}

ZTEST(ble_adv, test_a_successful_start_needs_nothing_more)
{
	st.want = true;
	zassert_equal(0u, radiant_ble_adv_note_result(&st, 0));
	zassert_true(st.active);
	zassert_equal(0u, st.failures);
	zassert_equal(0u, st.backoff_ms);
}

/* A start of an advertiser that is already running is success, not a fault
 * to retry: the watchdog's probe relies on it. */
ZTEST(ble_adv, test_ealready_counts_as_success)
{
	st.want = true;
	zassert_equal(0u, radiant_ble_adv_note_result(&st, -EALREADY));
	zassert_true(st.active);
	zassert_equal(0u, st.backoff_ms);
}

/* 100 ms doubling to the 5 s cap, and it never stops retrying. */
ZTEST(ble_adv, test_failures_back_off_from_100_ms_to_a_5_s_cap)
{
	static const uint32_t want[] = { 100u, 200u, 400u, 800u, 1600u,
					 3200u, 5000u, 5000u, 5000u };

	st.want = true;
	for (size_t i = 0; i < ARRAY_SIZE(want); i++) {
		zassert_true(radiant_ble_adv_begin_attempt(&st));
		zassert_equal(want[i], radiant_ble_adv_note_result(&st, -ENOMEM),
			      "attempt %u", (unsigned)i);
		zassert_false(st.active);
	}
	zassert_equal(ARRAY_SIZE(want), st.failures);

	/* A hundred more failures later it is still retrying, at the cap. */
	for (int i = 0; i < 100; i++) {
		zassert_equal(MAX_MS, radiant_ble_adv_note_result(&st, -EIO));
	}

	/* And the first success ends the run and resets the ladder. */
	zassert_equal(0u, radiant_ble_adv_note_result(&st, 0));
	zassert_equal(0u, st.failures);
	zassert_equal(MIN_MS, radiant_ble_adv_note_result(&st, -ENOMEM));
}

ZTEST(ble_adv, test_backoff_step_is_pure_and_capped)
{
	zassert_equal(100u, radiant_ble_adv_next_backoff(0u, 100u, 5000u));
	zassert_equal(200u, radiant_ble_adv_next_backoff(100u, 100u, 5000u));
	zassert_equal(5000u, radiant_ble_adv_next_backoff(3200u, 100u, 5000u));
	zassert_equal(5000u, radiant_ble_adv_next_backoff(5000u, 100u, 5000u));
	/* No overflow from a huge previous value. */
	zassert_equal(5000u, radiant_ble_adv_next_backoff(0xF0000000u, 100u, 5000u));
}

/* Nobody wants advertising any more: a failure is not retried. */
ZTEST(ble_adv, test_a_failure_after_want_is_cleared_is_not_retried)
{
	st.want = false;
	zassert_equal(0u, radiant_ble_adv_note_result(&st, -ENOMEM));
	zassert_equal(0u, st.backoff_ms);
}

/*
 * THE SHIPPED BUG, replayed: connect, disconnect, and the restart attempted
 * before the connection object is free fails. The policy restarts from
 * `recycled`, and a failure there is retried rather than abandoned.
 */
ZTEST(ble_adv, test_restart_comes_from_recycled_not_disconnected)
{
	st.want = true;
	zassert_equal(0u, radiant_ble_adv_note_result(&st, 0));

	radiant_ble_adv_note_connected(&st, true);
	zassert_false(st.active, "a connection ends the connectable advertiser");
	zassert_false(radiant_ble_adv_may_start(&st),
		      "BT_MAX_CONN=1: no slot for an advertiser while connected");

	radiant_ble_adv_note_disconnected(&st);
	/* The glue does not attempt from `disconnected`; `recycled` says go. */
	zassert_true(radiant_ble_adv_note_recycled(&st));
	zassert_true(radiant_ble_adv_begin_attempt(&st));

	/* Even if that start fails, a retry is scheduled... */
	zassert_equal(MIN_MS, radiant_ble_adv_note_result(&st, -ENOMEM));
	/* ...and a second `recycled` does not stack a second attempt on it. */
	zassert_false(radiant_ble_adv_note_recycled(&st));
	/* The retry succeeds. */
	zassert_true(radiant_ble_adv_begin_attempt(&st));
	zassert_equal(0u, radiant_ble_adv_note_result(&st, 0));
	zassert_true(st.active);
}

/* A connection attempt that fails after CONNECT_IND also ends the advertiser
 * but holds no slot. */
ZTEST(ble_adv, test_a_failed_connection_frees_no_slot_it_never_took)
{
	st.want = true;
	(void)radiant_ble_adv_note_result(&st, 0);
	radiant_ble_adv_note_connected(&st, false);
	zassert_false(st.active);
	zassert_equal(0u, st.conns);
	zassert_true(radiant_ble_adv_may_start(&st));
}

/* A retry falling due after a connection took the slot is dropped, and that
 * drop must not leave the retry marked pending - which would block both
 * `recycled` and the watchdog for good. */
ZTEST(ble_adv, test_a_retry_that_falls_due_while_connected_does_not_wedge)
{
	st.want = true;
	zassert_equal(MIN_MS, radiant_ble_adv_note_result(&st, -ENOMEM));
	radiant_ble_adv_note_connected(&st, true);

	zassert_false(radiant_ble_adv_begin_attempt(&st));
	zassert_equal(0u, st.backoff_ms);

	radiant_ble_adv_note_disconnected(&st);
	zassert_true(radiant_ble_adv_note_recycled(&st));
}

ZTEST(ble_adv, test_disconnect_never_underflows)
{
	radiant_ble_adv_note_disconnected(&st);
	zassert_equal(0u, st.conns);
}

ZTEST(ble_adv, test_two_connection_slots_keep_advertising_possible)
{
	radiant_ble_adv_state_init(&st, 2u, MIN_MS, MAX_MS, WD_MS);
	st.want = true;
	radiant_ble_adv_note_connected(&st, true);
	zassert_true(radiant_ble_adv_may_start(&st));
	radiant_ble_adv_note_connected(&st, true);
	zassert_false(radiant_ble_adv_may_start(&st));
}

/* The watchdog: probes every period while wanted and a slot is free, never
 * while connected or while a retry is already pending, and survives the
 * 32-bit millisecond wrap. */
ZTEST(ble_adv, test_watchdog_probes_on_its_period_only_when_useful)
{
	st.want = true;
	zassert_false(radiant_ble_adv_watchdog_due(&st, WD_MS - 1u));
	zassert_true(radiant_ble_adv_watchdog_due(&st, WD_MS));
	zassert_false(radiant_ble_adv_watchdog_due(&st, WD_MS + 1u));
	zassert_true(radiant_ble_adv_watchdog_due(&st, 2u * WD_MS));

	radiant_ble_adv_note_connected(&st, true);
	zassert_false(radiant_ble_adv_watchdog_due(&st, 10u * WD_MS));
	radiant_ble_adv_note_disconnected(&st);
	zassert_true(radiant_ble_adv_watchdog_due(&st, 10u * WD_MS));

	(void)radiant_ble_adv_note_result(&st, -ENOMEM);
	zassert_false(radiant_ble_adv_watchdog_due(&st, 20u * WD_MS),
		      "a pending retry is already the probe");
	(void)radiant_ble_adv_note_result(&st, 0);

	st.last_probe_ms = 0xFFFFFFFFu - 1000u;
	zassert_false(radiant_ble_adv_watchdog_due(&st, 0xFFFFFFFFu));
	zassert_true(radiant_ble_adv_watchdog_due(&st, WD_MS - 1001u));
}

ZTEST(ble_adv, test_watchdog_period_zero_is_off)
{
	radiant_ble_adv_state_init(&st, 1u, MIN_MS, MAX_MS, 0u);
	st.want = true;
	zassert_false(radiant_ble_adv_watchdog_due(&st, 1000000u));
}

ZTEST(ble_adv, test_init_sanitises_its_arguments)
{
	radiant_ble_adv_state_init(&st, 0u, 0u, 0u, 0u);
	zassert_equal(1u, st.max_conns);
	zassert_true(st.min_ms >= 1u);
	zassert_true(st.max_ms >= st.min_ms);
}

/* ── Per-unit name ───────────────────────────────────────────────────────── */

ZTEST_SUITE(ble_name, NULL, NULL, NULL, NULL, NULL);

ZTEST(ble_name, test_name_with_devnum)
{
	char out[32];

	zassert_equal(16, radiant_ble_name_with_devnum(out, sizeof(out),
						      "RadiANT HR", 51235u));
	zassert_str_equal("RadiANT HR 51235", out);
	zassert_equal(12, radiant_ble_name_with_devnum(out, sizeof(out),
						      "RadiANT HR", 7u));
	zassert_str_equal("RadiANT HR 7", out);
}

/* The suffix never exceeds the budget the applications' BUILD_ASSERTs
 * reserve for it. */
ZTEST(ble_name, test_suffix_fits_the_reserved_budget)
{
	char out[64];
	int  n = radiant_ble_name_with_devnum(out, sizeof(out), "X", 65535u);

	zassert_equal(1 + (int)RADIANT_BLE_NAME_SUFFIX_MAX, n);
}

ZTEST(ble_name, test_a_name_that_does_not_fit_falls_back_to_the_base)
{
	char out[12];

	zassert_equal(-1, radiant_ble_name_with_devnum(out, sizeof(out),
						      "RadiANT HR", 51235u));
	zassert_str_equal("RadiANT HR", out);
	zassert_equal(-1, radiant_ble_name_with_devnum(out, 4u, "RadiANT HR", 1u));
	zassert_str_equal("", out);
	zassert_equal(-1, radiant_ble_name_with_devnum(NULL, 4u, "x", 1u));
	zassert_equal(-1, radiant_ble_name_with_devnum(out, sizeof(out), NULL, 1u));
}

/* ── Connection parameters ───────────────────────────────────────────────── */

ZTEST_SUITE(ble_conn, NULL, NULL, NULL, NULL, NULL);

#define POL_INT 800u
#define POL_TO  400u

ZTEST(ble_conn, test_a_compliant_valid_request_is_left_alone)
{
	struct radiant_ble_conn_param p = { 800u, 800u, 0u, 400u };

	zassert_false(radiant_ble_conn_param_apply(&p, POL_INT, POL_TO));
	zassert_equal(800u, p.interval_min);
	zassert_equal(400u, p.timeout);
}

/* A phone's fast request is rewritten to policy, not rejected: a reject
 * leaves the link at the fast interval it connected at. */
ZTEST(ble_conn, test_a_fast_request_is_rewritten_to_policy)
{
	struct radiant_ble_conn_param p = { 24u, 40u, 0u, 42u };

	zassert_true(radiant_ble_conn_param_apply(&p, POL_INT, POL_TO));
	zassert_equal(POL_INT, p.interval_min);
	zassert_equal(POL_INT, p.interval_max);
	zassert_equal(0u, p.latency);
	zassert_equal(POL_TO, p.timeout);
	zassert_true(radiant_ble_conn_param_valid(&p));
}

ZTEST(ble_conn, test_latency_is_forced_to_zero_and_timeout_never_lowered)
{
	struct radiant_ble_conn_param p = { 800u, 800u, 4u, 3200u };

	zassert_true(radiant_ble_conn_param_apply(&p, POL_INT, POL_TO));
	zassert_equal(0u, p.latency);
	zassert_equal(3200u, p.timeout);
}

/* Compliant by every policy test but invalid by Zephyr's bound (4 * 400 is
 * not > 1600): accepting it verbatim would be silently neg-replied. */
ZTEST(ble_conn, test_a_compliant_but_invalid_request_is_rewritten)
{
	struct radiant_ble_conn_param p = { 800u, 1600u, 0u, 400u };

	zassert_false(radiant_ble_conn_param_valid(&p));
	zassert_true(radiant_ble_conn_param_apply(&p, POL_INT, POL_TO));
	zassert_equal(POL_INT, p.interval_max);
	zassert_true(radiant_ble_conn_param_valid(&p));
}

/* Every accepted triple is valid, over a sweep of peer requests. */
ZTEST(ble_conn, test_every_outcome_is_valid)
{
	static const uint16_t ints[] = { 6u, 24u, 400u, 800u, 1600u, 3200u };
	static const uint16_t lats[] = { 0u, 1u, 4u, 499u };
	static const uint16_t tos[] = { 10u, 42u, 200u, 400u, 3200u };

	for (size_t a = 0; a < ARRAY_SIZE(ints); a++) {
		for (size_t b = a; b < ARRAY_SIZE(ints); b++) {
			for (size_t l = 0; l < ARRAY_SIZE(lats); l++) {
				for (size_t t = 0; t < ARRAY_SIZE(tos); t++) {
					struct radiant_ble_conn_param p = {
						ints[a], ints[b], lats[l], tos[t]
					};

					(void)radiant_ble_conn_param_apply(&p, POL_INT, POL_TO);
					zassert_true(radiant_ble_conn_param_valid(&p),
						     "%u-%u/%u/%u", ints[a], ints[b],
						     lats[l], tos[t]);
					zassert_true(p.interval_min >= POL_INT);
					zassert_equal(0u, p.latency);
				}
			}
		}
	}
}

/* ── Pairing window ──────────────────────────────────────────────────────── */

ZTEST_SUITE(ble_pairing, NULL, NULL, NULL, NULL, NULL);

ZTEST(ble_pairing, test_closed_window_allows_nothing)
{
	zassert_false(radiant_ble_pairing_window_allows(0u, 0u, 0u));
	zassert_false(radiant_ble_pairing_window_allows(1000u, 0u, 1000u));
}

ZTEST(ble_pairing, test_window_is_half_open_and_expires)
{
	zassert_true(radiant_ble_pairing_window_allows(1000u, 60000u, 1000u));
	zassert_true(radiant_ble_pairing_window_allows(1000u, 60000u, 60999u));
	zassert_false(radiant_ble_pairing_window_allows(1000u, 60000u, 61000u));
	/* Before it opened (a clock read racing the open) is outside. */
	zassert_false(radiant_ble_pairing_window_allows(1000u, 60000u, 999u));
}

/* The review finding: on a 32-bit millisecond clock an expired window opened
 * just before 2^32 ms (49.7 days) reads as open again 2^32 ms later. With
 * 64-bit uptime it stays shut. */
ZTEST(ble_pairing, test_window_does_not_reopen_after_49_7_days)
{
	const uint64_t opened = 0xFFFFFFFFull - 10000u;

	zassert_true(radiant_ble_pairing_window_allows(opened, 60000u, opened + 15000u),
		     "spanning the 32-bit wrap, still inside");
	zassert_false(radiant_ble_pairing_window_allows(opened, 60000u, opened + 60000u));
	zassert_false(radiant_ble_pairing_window_allows(1000u, 60000u,
							1000u + 0x100000000ull),
		      "2^32 ms later the window must not be open again");
	zassert_false(radiant_ble_pairing_window_allows(1000u, 60000u,
							2000u + 0x100000000ull));
}

/* ── Bond LRU ────────────────────────────────────────────────────────────── */

ZTEST_SUITE(ble_lru, NULL, NULL, NULL, NULL, NULL);

static const uint8_t A[RADIANT_BLE_ADDR_BYTES] = { 0, 1, 1, 1, 1, 1, 1 };
static const uint8_t B[RADIANT_BLE_ADDR_BYTES] = { 0, 2, 2, 2, 2, 2, 2 };
static const uint8_t C[RADIANT_BLE_ADDR_BYTES] = { 1, 3, 3, 3, 3, 3, 3 };
static const uint8_t D[RADIANT_BLE_ADDR_BYTES] = { 1, 4, 4, 4, 4, 4, 4 };

ZTEST(ble_lru, test_the_least_recently_used_unconnected_bond_is_evicted)
{
	struct radiant_ble_lru_entry t[3] = { 0 };
	uint32_t next = 0u;
	uint8_t bonds[3][RADIANT_BLE_ADDR_BYTES];
	bool connected[3] = { false, false, false };

	memcpy(bonds[0], A, sizeof(A));
	memcpy(bonds[1], B, sizeof(B));
	memcpy(bonds[2], C, sizeof(C));

	radiant_ble_lru_touch(t, 3, A, &next);
	radiant_ble_lru_touch(t, 3, B, &next);
	radiant_ble_lru_touch(t, 3, C, &next);
	zassert_equal(0, radiant_ble_lru_pick_victim(t, 3, bonds, connected, 3));

	/* A used again: B is now the least recent. */
	radiant_ble_lru_touch(t, 3, A, &next);
	zassert_equal(1, radiant_ble_lru_pick_victim(t, 3, bonds, connected, 3));
}

/* A connected peer is never evicted, even when it is the least recent. */
ZTEST(ble_lru, test_a_connected_peer_is_never_evicted)
{
	struct radiant_ble_lru_entry t[3] = { 0 };
	uint32_t next = 0u;
	uint8_t bonds[3][RADIANT_BLE_ADDR_BYTES];
	bool connected[3] = { true, false, false };

	memcpy(bonds[0], A, sizeof(A));
	memcpy(bonds[1], B, sizeof(B));
	memcpy(bonds[2], C, sizeof(C));
	radiant_ble_lru_touch(t, 3, A, &next);
	radiant_ble_lru_touch(t, 3, B, &next);
	radiant_ble_lru_touch(t, 3, C, &next);

	zassert_equal(1, radiant_ble_lru_pick_victim(t, 3, bonds, connected, 3));

	bool all[3] = { true, true, true };

	zassert_equal(-1, radiant_ble_lru_pick_victim(t, 3, bonds, all, 3));
	zassert_equal(-1, radiant_ble_lru_pick_victim(t, 3, bonds, connected, 0));
}

/* A bond the LRU has never seen (bonded before this record existed, or the
 * record was lost) counts as least recent of all. */
ZTEST(ble_lru, test_an_unrecorded_bond_goes_first)
{
	struct radiant_ble_lru_entry t[3] = { 0 };
	uint32_t next = 0u;
	uint8_t bonds[3][RADIANT_BLE_ADDR_BYTES];
	bool connected[3] = { false, false, false };

	memcpy(bonds[0], A, sizeof(A));
	memcpy(bonds[1], B, sizeof(B));
	memcpy(bonds[2], C, sizeof(C));
	radiant_ble_lru_touch(t, 3, A, &next);
	radiant_ble_lru_touch(t, 3, C, &next);

	zassert_equal(1, radiant_ble_lru_pick_victim(t, 3, bonds, connected, 3));
	zassert_equal(0, radiant_ble_lru_pick_victim(NULL, 0, bonds, connected, 3),
		      "no record at all: the first bond");
}

/* A full table replaces its least recent entry; forget frees a slot. */
ZTEST(ble_lru, test_touch_replaces_the_oldest_and_forget_frees)
{
	struct radiant_ble_lru_entry t[2] = { 0 };
	uint32_t next = 0u;

	radiant_ble_lru_touch(t, 2, A, &next);
	radiant_ble_lru_touch(t, 2, B, &next);
	radiant_ble_lru_touch(t, 2, C, &next); /* replaces A */
	zassert_mem_equal(C, t[0].addr, sizeof(C));
	zassert_mem_equal(B, t[1].addr, sizeof(B));

	radiant_ble_lru_forget(t, 2, B);
	zassert_equal(0u, t[1].seq);
	radiant_ble_lru_touch(t, 2, D, &next);
	zassert_mem_equal(D, t[1].addr, sizeof(D));
	zassert_true(t[1].seq > t[0].seq);
}

/* The counter saturating halves the table rather than wrapping: order kept. */
ZTEST(ble_lru, test_counter_exhaustion_keeps_the_order)
{
	struct radiant_ble_lru_entry t[3] = { 0 };
	uint32_t next = 0xFFFFFFFFu;
	uint8_t bonds[3][RADIANT_BLE_ADDR_BYTES];
	bool connected[3] = { false, false, false };

	memcpy(t[0].addr, A, sizeof(A));
	t[0].seq = 0xFFFFFFF0u;
	memcpy(t[1].addr, B, sizeof(B));
	t[1].seq = 0xFFFFFFFFu;
	memcpy(bonds[0], A, sizeof(A));
	memcpy(bonds[1], B, sizeof(B));
	memcpy(bonds[2], C, sizeof(C));

	radiant_ble_lru_touch(t, 3, C, &next);
	zassert_true(next < 0x90000000u);
	zassert_true(t[0].seq <= t[1].seq);
	zassert_true(t[1].seq < t[2].seq, "C is the newest");
	zassert_equal(0, radiant_ble_lru_pick_victim(t, 3, bonds, connected, 3));
}

ZTEST(ble_lru, test_record_round_trips)
{
	struct radiant_ble_lru_entry t[4] = { 0 };
	struct radiant_ble_lru_entry back[4];
	uint32_t next = 0u;
	uint32_t hi = 0u;
	uint8_t buf[1 + 4 * RADIANT_BLE_LRU_ENTRY_BYTES];
	int len;

	radiant_ble_lru_touch(t, 4, A, &next);
	radiant_ble_lru_touch(t, 4, B, &next);
	radiant_ble_lru_touch(t, 4, A, &next);

	len = radiant_ble_lru_encode(t, 4, buf, sizeof(buf));
	zassert_equal(1 + 2 * (int)RADIANT_BLE_LRU_ENTRY_BYTES, len);
	zassert_equal(RADIANT_BLE_LRU_FORMAT_V1, buf[0]);

	zassert_equal(2, radiant_ble_lru_decode(buf, (size_t)len, back, 4, &hi));
	zassert_equal(3u, hi);
	zassert_mem_equal(A, back[0].addr, sizeof(A));
	zassert_equal(3u, back[0].seq);
	zassert_equal(2u, back[1].seq);
	zassert_equal(0u, back[2].seq);

	/* Too small an output, a wrong format byte, a torn length. */
	zassert_equal(-1, radiant_ble_lru_encode(t, 4, buf, 5u));
	buf[0] = 2u;
	zassert_equal(-1, radiant_ble_lru_decode(buf, (size_t)len, back, 4, &hi));
	buf[0] = RADIANT_BLE_LRU_FORMAT_V1;
	zassert_equal(-1, radiant_ble_lru_decode(buf, (size_t)len - 1u, back, 4, &hi));
	/* A record from a larger bond store keeps what fits. */
	zassert_equal(1, radiant_ble_lru_decode(buf, (size_t)len, back, 1, &hi));
}

/* The review finding: on a first pairing the entry must end up under the
 * identity address. An entry made under the RPA moves to the identity, and
 * merges with an existing identity entry keeping the more recent use. */
ZTEST(ble_lru, test_rekey_moves_an_rpa_entry_to_the_identity)
{
	static const uint8_t RPA[RADIANT_BLE_ADDR_BYTES] = { 1, 0x4A, 9, 9, 9, 9, 9 };
	struct radiant_ble_lru_entry t[3] = { 0 };
	uint32_t next = 0u;
	uint8_t bonds[2][RADIANT_BLE_ADDR_BYTES];
	bool connected[2] = { false, false };

	radiant_ble_lru_touch(t, 3, A, &next);  /* old bond */
	radiant_ble_lru_touch(t, 3, RPA, &next); /* new phone, before resolution */

	zassert_true(radiant_ble_lru_rekey(t, 3, RPA, D));
	memcpy(bonds[0], A, sizeof(A));
	memcpy(bonds[1], D, sizeof(D));
	/* The new phone's bond is the most recent; the old one goes. */
	zassert_equal(0, radiant_ble_lru_pick_victim(t, 3, bonds, connected, 2));

	/* Nothing to move, or moving onto itself: no change. */
	zassert_false(radiant_ble_lru_rekey(t, 3, RPA, D));
	zassert_false(radiant_ble_lru_rekey(t, 3, D, D));

	/* Merge: both present, the more recent seq survives under `to`. */
	radiant_ble_lru_touch(t, 3, RPA, &next);
	uint32_t newest = next;

	zassert_true(radiant_ble_lru_rekey(t, 3, RPA, A));
	int live = 0;

	for (int i = 0; i < 3; i++) {
		if (t[i].seq != 0u) {
			live++;
			zassert_true(memcmp(t[i].addr, RPA, sizeof(RPA)) != 0);
			if (memcmp(t[i].addr, A, sizeof(A)) == 0) {
				zassert_equal(newest, t[i].seq);
			}
		}
	}
	zassert_equal(2, live);
}
