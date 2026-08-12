/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_command.c - response slots and the reliable-command state machine.
 *
 * Four things are being established, and only the last needs the radio.
 *
 *   1. THE TAG, AT BOTH WIDTHS. The width is the channel's and never the
 *      node's, the 16-bit tag is the 64-bit tag's prefix, and a forgery that is
 *      right in its first two bytes passes at the short width and fails at the
 *      long one. That last assertion is the whole of what the long-range PHY's
 *      length extension bought this page.
 *   2. SECTION 9'S IDEMPOTENCY RULE, as four numbered rules in four separate
 *      tests: verify first and change no state on failure, answer a duplicate
 *      without executing it, accept only inside 1..64, and adopt a sequence
 *      from nothing exactly once per epoch. Plus the backoff, costed in
 *      simulated microseconds rather than asserted to exist.
 *   3. THE PHASE, RECOMPUTED. RF-5a announced a downlink window and recorded
 *      that a scheduler-driven node had to leave the interval at zero because
 *      the phase is relative to the carrying frame. Two schedule frames sent at
 *      different instants must announce different phases that resolve to the
 *      SAME absolute window, and that is asserted directly against the page
 *      scheduler rather than against the arithmetic alone.
 *   4. THE SLOT ITSELF. A response slot is an ordinary bounded receive window -
 *      the arm the mock records is RADIANT_FRAME's RX arm, at the announced
 *      instant, for the announced dwell - and a node that announces no window
 *      arms exactly what it armed before this file existed.
 *
 * ---------------------------------------------------------------------------
 * WHAT THE LATENCY NUMBERS HERE ARE, AND WHAT THEY ARE NOT
 * ---------------------------------------------------------------------------
 * The phase gate asks for MEASURED command latency, host request to node
 * execution, against an FE-C channel as the control, with a bounded worst case,
 * a mean and a miss rate. That measurement needs a host driving a real dongle
 * against a real node, and it is deferred to the combined bench session.
 *
 * What is here is the deterministic equivalent, on the mock's virtual clock,
 * and it is labelled as such in every test name that carries it: latency in
 * SIMULATED microseconds, bounded by construction rather than observed. It
 * establishes that the mechanism has the shape the gate will measure - worst
 * case one interval, mean half an interval, and a missed slot costing exactly
 * one further interval and never a whole period. It does not establish what
 * that costs in the air.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include <radiant_core/radiant_frame.h>
#include <radiant_core/radiant_radio_hal.h>
#include <radiant_core/radiant_sched.h>
#include <radiant_core/radiant_sec.h>

#include "../fake_radio.h"

#include "profile_command.h"
#include "profile_handoff.h"
#include "profile_sched.h"
#include "profile_schedule.h"
#include "profile_telemetry.h"

/* ---------------------------------------------------------------------------
 * Fixture
 * ---------------------------------------------------------------------------
 */

#define CMD_CH         2u
#define NODE_DEVNUM    0xB1CEu
#define NODE_DEVTYPE   0x60u
#define NODE_TRANSTYPE 0x05u

/*
 * One second of downlink interval, at a 1 ms dwell, announced by a 30 ppm node.
 *
 * The interval is deliberately NOT the channel period: separating the two is
 * what "response slots, not a shorter period" means, and a test whose interval
 * happened to equal its period would prove nothing about the separation.
 */
#define DL_INTERVAL_UNITS 32u      /* units of 1/32 s: exactly 1 s */
#define DL_INTERVAL_US    1000000u /* what the counts convert to, exactly */
#define DL_DWELL_CODE     2u       /* 1 ms */
#define DL_DWELL_US       1000u

static const struct profile_schedule LIVE_SCHED = {
	.dl_interval = DL_INTERVAL_UNITS,
	.dl_phase = 0u, /* recomputed per frame; the announced value is never
			 * this one */
	.dl_dwell = DL_DWELL_CODE,
	.coding = PROFILE_SCHED_CODING_NONE,
	.tx_power_dbm = PROFILE_SCHED_TX_POWER_UNSTATED,
};

/* 30 ppm, the accuracy LIVE_SCHED's dwell is sized against. */
#define NODE_CLK_PPM 30u

static const uint8_t ROOT_KEY[16] = {
	0x00u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u,
	0x88u, 0x99u, 0xAAu, 0xBBu, 0xCCu, 0xDDu, 0xEEu, 0xFFu,
};

#define EPOCH_A 0x0000ABCDu
#define EPOCH_B 0x0000ABCEu

static struct radiant_sec_key g_root;
static struct radiant_rx_filter track_filter;

/* The actuator, and the counter that proves "at most once per sequence". */
static uint32_t g_exec_calls;
static uint8_t  g_exec_result;
static uint16_t g_exec_value;
static uint16_t g_last_arg;

static uint8_t test_exec(uint8_t cmd, uint8_t target, uint16_t arg,
			 uint16_t *value, void *user)
{
	ARG_UNUSED(cmd);
	ARG_UNUSED(target);
	ARG_UNUSED(user);

	g_exec_calls++;
	g_last_arg = arg;
	*value = g_exec_value;
	return g_exec_result;
}

static void node_init(struct profile_cmd_node *n, uint8_t tag_len, uint32_t epoch)
{
	struct profile_cmd_cfg cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.epoch = epoch;
	cfg.devnum = NODE_DEVNUM;
	cfg.k_root = &g_root;
	cfg.tag_len = tag_len;
	cfg.execute = test_exec;
	cfg.slot.ch = CMD_CH;
	cfg.slot.fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	cfg.slot.rf_index = PROFILE_TLM_RF_INDEX_DEFAULT;
	cfg.slot.filters = &track_filter;
	cfg.slot.n_filters = 1u;

	zassert_ok(profile_cmd_init(n, &cfg));
}

/* Build a signed page 0x10 the way a commanding receiver would. */
static int build_command(struct profile_cmd_node *signer, uint8_t seq, uint8_t cmd,
			 uint8_t target, uint16_t arg, uint8_t *body, size_t cap)
{
	struct profile_command c;
	uint8_t tag[PROFILE_TLM_CMD_TAG_LR];
	int rc;

	memset(&c, 0, sizeof(c));
	c.seq = seq;
	c.cmd = cmd;
	c.target = target;
	c.arg = arg;

	memset(tag, 0, sizeof(tag));
	rc = profile_command_encode_tag(&c, tag, signer->cfg.tag_len, body, cap);
	zassert_true(rc > 0);
	zassert_ok(profile_cmd_tag(signer, body, tag));
	return profile_command_encode_tag(&c, tag, signer->cfg.tag_len, body, cap);
}

static void cmd_before(void *fixture)
{
	ARG_UNUSED(fixture);

	fake_radio_reset();

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_key_import(&g_root, ROOT_KEY, 128u));

	memset(&track_filter, 0, sizeof(track_filter));
	track_filter.addr[0] = RADIANT_NET_ADDR_ANT_PLUS_0;
	track_filter.addr[1] = RADIANT_NET_ADDR_ANT_PLUS_1;
	track_filter.addr[2] = (uint8_t)(NODE_DEVNUM & 0xFFu);
	track_filter.addr[3] = (uint8_t)(NODE_DEVNUM >> 8);
	track_filter.addr[4] = NODE_DEVTYPE;
	track_filter.addr_len = RADIANT_FRAME_ADDR_LEN_TRACKING;

	g_exec_calls = 0u;
	g_exec_result = PROFILE_TLM_RESULT_OK;
	g_exec_value = 0x1234u;
	g_last_arg = 0u;
}

ZTEST_SUITE(command, NULL, NULL, cmd_before, NULL, NULL);

/* ---------------------------------------------------------------------------
 * 1. The tag, at both widths
 * ---------------------------------------------------------------------------
 */

ZTEST(command, test_the_tag_width_is_the_channels_and_never_the_nodes)
{
	struct profile_schedule s = LIVE_SCHED;

	zassert_equal(PROFILE_TLM_CMD_TAG_LR,
		      profile_cmd_tag_bytes(RADIANT_FRAME_CFG_LR));
	zassert_equal(PROFILE_TLM_CMD_TAG_STD,
		      profile_cmd_tag_bytes(RADIANT_FRAME_CFG_TRACKING));
	zassert_equal(PROFILE_TLM_CMD_TAG_STD,
		      profile_cmd_tag_bytes(RADIANT_FRAME_CFG_SEARCH));

	/* A control channel is S=8 on the long-range configuration, and the two
	 * halves have to arrive together. */
	s.coding = PROFILE_CMD_CTRL_CODING;
	zassert_ok(profile_cmd_channel_check(&s, RADIANT_FRAME_CFG_LR));
	zassert_equal(-EINVAL,
		      profile_cmd_channel_check(&s, RADIANT_FRAME_CFG_TRACKING));

	s.coding = PROFILE_SCHED_CODING_NONE;
	zassert_ok(profile_cmd_channel_check(&s, RADIANT_FRAME_CFG_TRACKING));
	zassert_equal(-EINVAL, profile_cmd_channel_check(&s, RADIANT_FRAME_CFG_LR));

	/* A control channel with no window to be commanded in. */
	s.dl_interval = 0u;
	s.dl_phase = 0u;
	s.dl_dwell = 0u;
	zassert_equal(-ENOENT,
		      profile_cmd_channel_check(&s, RADIANT_FRAME_CFG_TRACKING));

	/* A width that is neither is a caller inventing a third format. */
	{
		struct profile_cmd_node n;
		struct profile_cmd_cfg cfg;

		memset(&cfg, 0, sizeof(cfg));
		cfg.tag_len = 4u;
		cfg.execute = test_exec;
		zassert_equal(-EINVAL, profile_cmd_init(&n, &cfg));
	}
}

ZTEST(command, test_the_short_tag_is_the_long_tags_prefix_and_the_rest_is_the_gain)
{
	struct profile_cmd_node short_n;
	struct profile_cmd_node long_n;
	uint8_t body_s[PROFILE_TLM_CMD_LEN_STD];
	uint8_t body_l[PROFILE_TLM_CMD_LEN_LR];
	uint8_t tag_s[PROFILE_TLM_CMD_TAG_LR];
	uint8_t tag_l[PROFILE_TLM_CMD_TAG_LR];
	int rc;

	node_init(&short_n, PROFILE_TLM_CMD_TAG_STD, EPOCH_A);
	node_init(&long_n, PROFILE_TLM_CMD_TAG_LR, EPOCH_A);

	rc = build_command(&short_n, 7u, PROFILE_TLM_CMD_SET_LEVEL, 0x01u, 0x0042u,
			   body_s, sizeof(body_s));
	zassert_equal((int)PROFILE_TLM_CMD_LEN_STD, rc);
	rc = build_command(&long_n, 7u, PROFILE_TLM_CMD_SET_LEVEL, 0x01u, 0x0042u,
			   body_l, sizeof(body_l));
	zassert_equal((int)PROFILE_TLM_CMD_LEN_LR, rc);

	/* Same six covered bytes either way: the tag grows off the end and the
	 * fields in front of it do not move. */
	zassert_mem_equal(body_s, body_l, PROFILE_TLM_CMD_COVERED);

	memset(tag_s, 0, sizeof(tag_s));
	memset(tag_l, 0, sizeof(tag_l));
	zassert_ok(profile_cmd_tag(&short_n, body_s, tag_s));
	zassert_ok(profile_cmd_tag(&long_n, body_l, tag_l));

	/* Truncation is a prefix, so the short tag is the long one's first two
	 * bytes. That is what makes the two widths one scheme. */
	zassert_mem_equal(tag_s, tag_l, PROFILE_TLM_CMD_TAG_STD);

	/*
	 * AND HERE IS THE POINT OF THE LONGER FRAME. A forgery that got the
	 * first two bytes right - which is 2^-16 of guesses - is accepted at the
	 * short width and refused at the long one. The six extra bytes are the
	 * difference between a rate-limit-mitigated actuator path and a
	 * defensible one.
	 */
	zassert_true(profile_cmd_tag_ok(&short_n, body_l,
					&body_l[PROFILE_TLM_CMD_COVERED]));
	body_l[PROFILE_TLM_CMD_COVERED + 2u] ^= 0x01u;
	zassert_true(profile_cmd_tag_ok(&short_n, body_l,
					&body_l[PROFILE_TLM_CMD_COVERED]),
		     "the short width cannot see past its own two bytes");
	zassert_false(profile_cmd_tag_ok(&long_n, body_l,
					 &body_l[PROFILE_TLM_CMD_COVERED]));
}

ZTEST(command, test_a_page_at_the_other_width_is_refused_rather_than_truncated)
{
	struct profile_cmd_node n;
	struct profile_cmd_node signer;
	uint8_t body[PROFILE_TLM_CMD_LEN_LR];
	uint8_t ack[PROFILE_TLM_CMD_LEN_LR];

	node_init(&n, PROFILE_TLM_CMD_TAG_STD, EPOCH_A);
	node_init(&signer, PROFILE_TLM_CMD_TAG_LR, EPOCH_A);
	(void)build_command(&signer, 1u, PROFILE_TLM_CMD_NOP, PROFILE_TLM_TARGET_NODE,
			    0u, body, sizeof(body));

	zassert_equal(-EINVAL,
		      profile_cmd_on_command(&n, body, PROFILE_TLM_CMD_LEN_LR,
					     1000u, ack, sizeof(ack)));
	zassert_equal(0u, g_exec_calls);
}

/* ---------------------------------------------------------------------------
 * 2. Section 9's idempotency rule
 * ---------------------------------------------------------------------------
 */

ZTEST(command, test_a_command_executes_once_however_many_times_it_arrives)
{
	struct profile_cmd_node n;
	struct profile_command_ack a;
	uint8_t body[PROFILE_TLM_CMD_LEN_STD];
	uint8_t ack[PROFILE_TLM_CMD_LEN_LR];
	uint8_t tag[PROFILE_TLM_CMD_TAG_LR];
	int rc;

	node_init(&n, PROFILE_TLM_CMD_TAG_STD, EPOCH_A);
	(void)build_command(&n, 42u, PROFILE_TLM_CMD_SET_BOOL, 0x02u, 1u, body,
			    sizeof(body));

	rc = profile_cmd_on_command(&n, body, sizeof(body), 1000u, ack, sizeof(ack));
	zassert_equal((int)PROFILE_TLM_CMD_LEN_STD, rc);
	zassert_equal(1u, g_exec_calls);
	zassert_true(profile_command_ack_decode_tag(ack, (uint8_t)rc, &a, tag,
						    sizeof(tag)) > 0);
	zassert_equal(PROFILE_TLM_RESULT_OK, a.result);
	zassert_equal(42u, a.seq);
	zassert_equal(PROFILE_TLM_CMD_SET_BOOL, a.cmd);
	zassert_equal(0x1234u, a.value);

	/* Twice more. The effect runs once; the answer comes back every time,
	 * which is what makes a retry free. */
	rc = profile_cmd_on_command(&n, body, sizeof(body), 2000u, ack, sizeof(ack));
	zassert_equal((int)PROFILE_TLM_CMD_LEN_STD, rc);
	zassert_true(profile_command_ack_decode_tag(ack, (uint8_t)rc, &a, tag,
						    sizeof(tag)) > 0);
	zassert_equal(PROFILE_TLM_RESULT_ALREADY, a.result);
	zassert_equal(0x1234u, a.value, "a repeat carries the stored value");

	rc = profile_cmd_on_command(&n, body, sizeof(body), 3000u, ack, sizeof(ack));
	zassert_equal((int)PROFILE_TLM_CMD_LEN_STD, rc);

	zassert_equal(1u, g_exec_calls, "the effect ran exactly once");
	zassert_equal(1u, profile_cmd_stats(&n)->executed);
	zassert_equal(2u, profile_cmd_stats(&n)->repeats);

	/* The acknowledgement is authenticated too, and its tag covers the
	 * result - so the repeat's tag differs from the original's. */
	zassert_true(profile_cmd_tag_ok(&n, ack, &ack[PROFILE_TLM_CMD_COVERED]));
}

ZTEST(command, test_a_rejection_repeats_as_a_rejection_and_never_as_already)
{
	struct profile_cmd_node n;
	struct profile_command_ack a;
	uint8_t body[PROFILE_TLM_CMD_LEN_STD];
	uint8_t ack[PROFILE_TLM_CMD_LEN_LR];
	uint8_t tag[PROFILE_TLM_CMD_TAG_LR];
	int rc;

	g_exec_result = PROFILE_TLM_RESULT_BAD_ARG;
	node_init(&n, PROFILE_TLM_CMD_TAG_STD, EPOCH_A);
	(void)build_command(&n, 9u, PROFILE_TLM_CMD_SET_LEVEL, 0x01u, 0xFFFFu, body,
			    sizeof(body));

	rc = profile_cmd_on_command(&n, body, sizeof(body), 1000u, ack, sizeof(ack));
	zassert_true(rc > 0);
	zassert_true(profile_command_ack_decode_tag(ack, (uint8_t)rc, &a, tag,
						    sizeof(tag)) > 0);
	zassert_equal(PROFILE_TLM_RESULT_BAD_ARG, a.result);

	rc = profile_cmd_on_command(&n, body, sizeof(body), 2000u, ack, sizeof(ack));
	zassert_true(rc > 0);
	zassert_true(profile_command_ack_decode_tag(ack, (uint8_t)rc, &a, tag,
						    sizeof(tag)) > 0);
	zassert_equal(PROFILE_TLM_RESULT_BAD_ARG, a.result,
		      "a refused command is still refused, never 'already done'");
	zassert_equal(0u, profile_cmd_stats(&n)->executed);
	zassert_equal(1u, profile_cmd_stats(&n)->rejected);
}

ZTEST(command, test_a_bad_tag_changes_no_state_including_last_seq)
{
	struct profile_cmd_node n;
	struct profile_command_ack a;
	uint8_t body[PROFILE_TLM_CMD_LEN_STD];
	uint8_t ack[PROFILE_TLM_CMD_LEN_LR];
	uint8_t tag[PROFILE_TLM_CMD_TAG_LR];
	int rc;

	node_init(&n, PROFILE_TLM_CMD_TAG_STD, EPOCH_A);
	(void)build_command(&n, 200u, PROFILE_TLM_CMD_SET_BOOL, 0x01u, 1u, body,
			    sizeof(body));
	body[PROFILE_TLM_CMD_COVERED] ^= 0x80u; /* a wrong tag */

	rc = profile_cmd_on_command(&n, body, sizeof(body), 1000u, ack, sizeof(ack));
	zassert_true(rc > 0, "the first failure is answered, once");
	zassert_true(profile_command_ack_decode_tag(ack, (uint8_t)rc, &a, tag,
						    sizeof(tag)) > 0);
	zassert_equal(PROFILE_TLM_RESULT_BAD_TAG, a.result);

	zassert_equal(0u, g_exec_calls);
	zassert_false(n.seq_known, "a bad tag adopted nothing");
	zassert_equal(0u, profile_cmd_stats(&n)->adopted);

	/* Rule 4 still available: the first command that PASSES the tag check
	 * is the one whose sequence is adopted. */
	(void)build_command(&n, 7u, PROFILE_TLM_CMD_SET_BOOL, 0x01u, 1u, body,
			    sizeof(body));
	rc = profile_cmd_on_command(&n, body, sizeof(body),
				    1000u + (2u * PROFILE_CMD_BACKOFF_BASE_US),
				    ack, sizeof(ack));
	zassert_true(rc > 0);
	zassert_equal(1u, g_exec_calls);
	zassert_equal(1u, profile_cmd_stats(&n)->adopted);
	zassert_equal(7u, n.last_seq);
}

ZTEST(command, test_the_accept_window_is_one_to_sixty_four_and_wraps)
{
	struct profile_command_ack a;
	uint8_t body[PROFILE_TLM_CMD_LEN_STD];
	uint8_t ack[PROFILE_TLM_CMD_LEN_LR];
	uint8_t tag[PROFILE_TLM_CMD_TAG_LR];
	int rc;
	uint8_t i;

	/* delta 64 in, delta 65 out, delta 255 (one backwards) out, and a
	 * delta that crosses the wrap treated like any other. */
	static const struct {
		uint8_t first;
		uint8_t second;
		bool    accept;
	} cases[] = {
		{ 10u, 74u, true },  /* delta 64 - the edge, inside */
		{ 10u, 75u, false }, /* delta 65 - the edge, outside */
		{ 10u, 11u, true },  /* delta 1 */
		{ 10u, 9u, false },  /* delta 255: backwards */
		{ 250u, 5u, true },  /* delta 11, across the wrap */
		{ 250u, 200u, false }, /* delta 206 */
	};

	for (i = 0u; i < ARRAY_SIZE(cases); i++) {
		struct profile_cmd_node n;

		g_exec_calls = 0u;
		node_init(&n, PROFILE_TLM_CMD_TAG_STD, EPOCH_A);

		(void)build_command(&n, cases[i].first, PROFILE_TLM_CMD_NOP,
				    PROFILE_TLM_TARGET_NODE, 0u, body,
				    sizeof(body));
		zassert_true(profile_cmd_on_command(&n, body, sizeof(body), 1000u,
						    ack, sizeof(ack)) > 0);
		zassert_equal(1u, g_exec_calls);

		(void)build_command(&n, cases[i].second, PROFILE_TLM_CMD_NOP,
				    PROFILE_TLM_TARGET_NODE, 0u, body,
				    sizeof(body));
		rc = profile_cmd_on_command(&n, body, sizeof(body), 2000u, ack,
					    sizeof(ack));
		zassert_true(rc > 0);
		zassert_true(profile_command_ack_decode_tag(ack, (uint8_t)rc, &a,
							    tag, sizeof(tag)) > 0);

		if (cases[i].accept) {
			zassert_equal(PROFILE_TLM_RESULT_OK, a.result,
				      "case %u: %u -> %u should be inside", i,
				      cases[i].first, cases[i].second);
			zassert_equal(2u, g_exec_calls);
		} else {
			zassert_equal(PROFILE_TLM_RESULT_BAD_SEQ, a.result,
				      "case %u: %u -> %u should be outside", i,
				      cases[i].first, cases[i].second);
			zassert_equal(1u, g_exec_calls);
		}
	}
}

ZTEST(command, test_failed_verifications_back_off_and_a_success_clears_the_ladder)
{
	struct profile_cmd_node n;
	uint8_t body[PROFILE_TLM_CMD_LEN_STD];
	uint8_t ack[PROFILE_TLM_CMD_LEN_LR];
	radiant_time_t t = 1000000u;
	radiant_time_t blocked_1;
	radiant_time_t blocked_2;
	radiant_time_t blocked_3;

	node_init(&n, PROFILE_TLM_CMD_TAG_STD, EPOCH_A);
	(void)build_command(&n, 5u, PROFILE_TLM_CMD_SET_BOOL, 0x01u, 1u, body,
			    sizeof(body));
	body[PROFILE_TLM_CMD_COVERED] ^= 0xFFu;

	/* Failure 1: answered once, and the ladder starts at the base. */
	zassert_true(profile_cmd_on_command(&n, body, sizeof(body), t, ack,
					    sizeof(ack)) > 0);
	blocked_1 = n.blocked_until;
	zassert_equal(t + PROFILE_CMD_BACKOFF_BASE_US, blocked_1);

	/* A second attempt inside the block is dropped WITHOUT verifying it,
	 * which is the cost the throttle exists to save. */
	zassert_equal(0, profile_cmd_on_command(&n, body, sizeof(body), t + 1u, ack,
						sizeof(ack)));
	zassert_equal(1u, profile_cmd_stats(&n)->throttled);
	zassert_equal(1u, profile_cmd_stats(&n)->bad_tag);

	/* Past the block: verified, failed, and the interval has doubled. */
	t = blocked_1;
	zassert_equal(0, profile_cmd_on_command(&n, body, sizeof(body), t, ack,
						sizeof(ack)));
	blocked_2 = n.blocked_until;
	zassert_equal(t + (2u * PROFILE_CMD_BACKOFF_BASE_US), blocked_2);
	zassert_equal(2u, profile_cmd_stats(&n)->bad_tag);

	t = blocked_2;
	zassert_equal(0, profile_cmd_on_command(&n, body, sizeof(body), t, ack,
						sizeof(ack)));
	blocked_3 = n.blocked_until;
	zassert_equal(t + (4u * PROFILE_CMD_BACKOFF_BASE_US), blocked_3);

	/* A good command clears the ladder, so a legitimate receiver that was
	 * merely unlucky never climbs it. */
	(void)build_command(&n, 5u, PROFILE_TLM_CMD_SET_BOOL, 0x01u, 1u, body,
			    sizeof(body));
	zassert_true(profile_cmd_on_command(&n, body, sizeof(body), blocked_3, ack,
					    sizeof(ack)) > 0);
	zassert_equal(0u, n.fails);
	zassert_equal(1u, g_exec_calls);
	zassert_equal(3u, profile_cmd_stats(&n)->bad_tag);
}

ZTEST(command, test_a_command_captured_in_a_previous_epoch_never_verifies)
{
	struct profile_cmd_node yesterday;
	struct profile_cmd_node today;
	uint8_t body[PROFILE_TLM_CMD_LEN_STD];
	uint8_t ack[PROFILE_TLM_CMD_LEN_LR];
	struct profile_command_ack a;
	uint8_t tag[PROFILE_TLM_CMD_TAG_LR];
	int rc;

	node_init(&yesterday, PROFILE_TLM_CMD_TAG_STD, EPOCH_A);
	node_init(&today, PROFILE_TLM_CMD_TAG_STD, EPOCH_B);

	(void)build_command(&yesterday, 3u, PROFILE_TLM_CMD_SET_BOOL, 0x01u, 1u,
			    body, sizeof(body));

	rc = profile_cmd_on_command(&today, body, sizeof(body), 1000u, ack,
				    sizeof(ack));
	zassert_true(rc > 0);
	zassert_true(profile_command_ack_decode_tag(ack, (uint8_t)rc, &a, tag,
						    sizeof(tag)) > 0);
	zassert_equal(PROFILE_TLM_RESULT_BAD_TAG, a.result,
		      "this is what the epoch's four bytes buy a command-only node");
	zassert_equal(0u, g_exec_calls);
	zassert_false(today.seq_known);

	/* And moving epoch forgets the sequence, for the same reason. */
	zassert_true(profile_cmd_on_command(&yesterday, body, sizeof(body), 1000u,
					    ack, sizeof(ack)) > 0);
	zassert_true(yesterday.seq_known);
	zassert_ok(profile_cmd_set_epoch(&yesterday, EPOCH_B));
	zassert_false(yesterday.seq_known);
}

/* ---------------------------------------------------------------------------
 * 3. The phase, recomputed - RF-5a's forward reference, closed
 * ---------------------------------------------------------------------------
 */

ZTEST(command, test_rephasing_touches_the_phase_and_nothing_else)
{
	struct profile_schedule s = LIVE_SCHED;
	struct profile_schedule back;
	uint8_t body[6];
	uint8_t quiet[6];
	const struct profile_schedule none = PROFILE_SCHED_INIT_DEFAULT;

	s.dl_phase = 100u;
	zassert_ok(profile_sched_pack(&s, false, NODE_CLK_PPM, body));

	zassert_ok(profile_sched_rephase(body, 20000u));
	zassert_ok(profile_sched_unpack(body, false, NODE_CLK_PPM, &back));
	zassert_equal(20000u, back.dl_phase);
	zassert_equal(s.dl_interval, back.dl_interval);
	zassert_equal(s.dl_dwell, back.dl_dwell);
	zassert_equal(s.coding, back.coding);
	zassert_equal(s.tx_power_dbm, back.tx_power_dbm);

	/* A phase at or past the interval is the next window described as this
	 * one, and is refused at exactly the boundary. */
	zassert_equal(-EINVAL, profile_sched_rephase(body, 32768u));
	zassert_ok(profile_sched_rephase(body, 32767u));

	/* A block with no window has no phase to write. */
	zassert_ok(profile_sched_pack(&none, false, 0u, quiet));
	zassert_equal(-ENOENT, profile_sched_rephase(quiet, 0u));
	zassert_equal(-EINVAL, profile_sched_rephase(NULL, 0u));
}

ZTEST(command, test_two_frames_sent_at_different_times_announce_the_same_window)
{
	struct profile_schedule a = LIVE_SCHED;
	struct profile_schedule b = LIVE_SCHED;
	const radiant_time_t anchor = 5000000u;
	const radiant_time_t t1 = 4000123u;
	const radiant_time_t t2 = t1 + 250000u; /* not a multiple of the interval */
	int32_t p1;
	int32_t p2;
	radiant_time_t w1;
	radiant_time_t w2;

	p1 = profile_sched_phase_for(&LIVE_SCHED, anchor, t1);
	p2 = profile_sched_phase_for(&LIVE_SCHED, anchor, t2);
	zassert_true(p1 >= 0);
	zassert_true(p2 >= 0);
	zassert_not_equal(p1, p2, "a retransmitted phase is the bug being fixed");

	a.dl_phase = (uint16_t)p1;
	b.dl_phase = (uint16_t)p2;
	w1 = profile_sched_listen_at(&a, t1, 0u);
	w2 = profile_sched_listen_at(&b, t2, 0u);

	/* One instant, described twice. The tolerance is the 1/32768 s
	 * quantisation of the phase field and nothing else. */
	zassert_true((w1 > w2 ? w1 - w2 : w2 - w1) <= 31u,
		     "w1=%llu w2=%llu", (unsigned long long)w1,
		     (unsigned long long)w2);
	zassert_true((w1 > anchor ? w1 - anchor : anchor - w1) <= 31u);

	/* An anchor already in the past is reduced forward, not refused. */
	zassert_true(profile_sched_phase_for(&LIVE_SCHED, 1000u, t1) >= 0);
	/* A block with no window has no phase. */
	{
		const struct profile_schedule none = PROFILE_SCHED_INIT_DEFAULT;

		zassert_equal(-1, profile_sched_phase_for(&none, anchor, t1));
	}
}

/* ---------------------------------------------------------------------------
 * The page scheduler carrying a live window
 * ---------------------------------------------------------------------------
 */

static struct profile_descriptor g_desc;

/* How far `w` sits from the nearest point of the grid anchor + n * interval.
 * The phase field quantises at 1/32768 s, so anything under 31 us is exact. */
static uint64_t grid_error(radiant_time_t w, radiant_time_t anchor)
{
	uint64_t off = (w >= anchor) ? ((uint64_t)(w - anchor) % DL_INTERVAL_US)
				     : ((uint64_t)(anchor - w) % DL_INTERVAL_US);

	return (off < (DL_INTERVAL_US - off)) ? off : (DL_INTERVAL_US - off);
}

static int cb_data_page(uint8_t page, uint8_t counter, uint8_t *body, void *user)
{
	ARG_UNUSED(user);

	memset(body, 0, PROFILE_TLM_FRAME_LEN);
	body[0] = page;
	body[1] = counter;
	return 0;
}

static void live_descriptor(struct profile_descriptor *d)
{
	memset(d, 0, sizeof(*d));
	d->version = PROFILE_TLM_VERSION;
	d->schema_id = 0x2Bu;
	d->period = 8182u;
	d->rf_index = PROFILE_TLM_RF_INDEX_DEFAULT;
	d->clock_stated = true;
	d->clock_accuracy = PROFILE_HANDOFF_CLK_30PPM;
	d->has_schedule = true;
	d->schedule = LIVE_SCHED;

	d->n_fields = 1u;
	d->fields[0].id = 0x01u;
	d->fields[0].type = PROFILE_TLM_TYPE_TEMPERATURE;
	d->fields[0].is_signed = true;
	d->fields[0].width_code = 7u;
	d->fields[0].exponent = -2;
	d->fields[0].page = 0x01u;
	d->fields[0].bit_offset = 0u;
}

ZTEST(command, test_the_page_scheduler_rephases_the_schedule_frame_it_hands_out)
{
	struct profile_sched ps;
	struct profile_sched_cfg cfg;
	struct profile_cmd_node n;
	struct profile_sched_downlink dl;
	struct profile_schedule heard_1;
	struct profile_schedule heard_2;
	uint8_t body[PROFILE_TLM_FRAME_LEN];
	radiant_time_t t = 4000123u;
	radiant_time_t carrier_1 = 0u;
	radiant_time_t carrier_2 = 0u;
	radiant_time_t w1;
	radiant_time_t w2;
	uint32_t seen = 0u;
	uint32_t slot;
	int idx;

	live_descriptor(&g_desc);
	node_init(&n, PROFILE_TLM_CMD_TAG_STD, EPOCH_A);
	zassert_ok(profile_cmd_window_set(&n, &LIVE_SCHED, 5000000u, false,
					  NODE_CLK_PPM));

	memset(&cfg, 0, sizeof(cfg));
	cfg.desc = &g_desc;
	cfg.data_page = cb_data_page;
	zassert_ok(profile_sched_init(&ps, &cfg));

	idx = profile_desc_schedule_index(&g_desc);
	zassert_equal(2, idx);

	profile_cmd_downlink(&n, &dl);
	zassert_ok(profile_sched_set_downlink(&ps, &dl));

	/*
	 * Two whole cycles, so the schedule frame goes out twice at two
	 * different instants. The slot spacing is not the downlink interval, so
	 * the two announcements cannot be the same by accident.
	 */
	for (slot = 0u; slot < (2u * PROFILE_TLM_CYCLE) + 1u; slot++) {
		enum profile_slot_kind kind;
		uint32_t m_before = profile_sched_m(&ps);

		kind = profile_sched_next_at(&ps, body, t);
		if (kind == PROFILE_SLOT_DESCRIPTOR && m_before % PROFILE_TLM_CYCLE ==
							       (uint32_t)idx) {
			if (seen == 0u) {
				carrier_1 = t;
				zassert_ok(profile_sched_unpack(&body[2], false,
								NODE_CLK_PPM,
								&heard_1));
			} else if (seen == 1u) {
				carrier_2 = t;
				zassert_ok(profile_sched_unpack(&body[2], false,
								NODE_CLK_PPM,
								&heard_2));
			}
			seen++;
		}
		t += 250000u; /* a 4 Hz slot grid */
	}

	zassert_true(seen >= 2u, "the schedule frame went out twice");
	zassert_not_equal(heard_1.dl_phase, heard_2.dl_phase,
			  "a scheduler-driven node no longer repeats a stale phase");

	w1 = profile_sched_listen_at(&heard_1, carrier_1, 0u);
	w2 = profile_sched_listen_at(&heard_2, carrier_2, 0u);

	/*
	 * The two frames are a whole cycle apart, so they point at two DIFFERENT
	 * windows - which is correct and is the property being asserted. Both
	 * must land on the node's grid, anchor + n * interval. A node that
	 * retransmitted a stale phase would put the second one anywhere.
	 */
	zassert_true(grid_error(w1, 5000000u) <= 31u, "w1=%llu off grid",
		     (unsigned long long)w1);
	zassert_true(grid_error(w2, 5000000u) <= 31u, "w2=%llu off grid",
		     (unsigned long long)w2);
	zassert_not_equal(w1, w2, "a cycle apart is a different window");

	/* Every other field of the block survived the re-phase untouched. */
	zassert_equal(LIVE_SCHED.dl_interval, heard_2.dl_interval);
	zassert_equal(LIVE_SCHED.dl_dwell, heard_2.dl_dwell);
	zassert_equal(LIVE_SCHED.coding, heard_2.coding);
}

ZTEST(command, test_a_node_that_announces_no_window_is_the_node_it_was_before)
{
	struct profile_sched with_hook;
	struct profile_sched plain;
	struct profile_sched_cfg cfg;
	struct profile_cmd_node n;
	struct profile_sched_downlink dl;
	uint8_t a[PROFILE_TLM_FRAME_LEN];
	uint8_t b[PROFILE_TLM_FRAME_LEN];
	uint32_t slot;

	/* No schedule frame at all. */
	live_descriptor(&g_desc);
	g_desc.has_schedule = false;
	g_desc.schedule = (struct profile_schedule)PROFILE_SCHED_INIT_DEFAULT;

	memset(&cfg, 0, sizeof(cfg));
	cfg.desc = &g_desc;
	cfg.data_page = cb_data_page;
	zassert_ok(profile_sched_init(&with_hook, &cfg));
	zassert_ok(profile_sched_init(&plain, &cfg));

	node_init(&n, PROFILE_TLM_CMD_TAG_STD, EPOCH_A);
	profile_cmd_downlink(&n, &dl);

	/* A hook on a node with nothing to re-phase is refused at start-up
	 * rather than silently doing nothing forever. */
	zassert_equal(-ENOENT, profile_sched_set_downlink(&with_hook, &dl));

	/* And with no window configured, nothing is armed and nothing is
	 * announced. */
	zassert_equal(-ENOENT, profile_cmd_arm_listen(&n, 1000u));
	zassert_equal(RADIANT_TIME_NEVER, profile_cmd_next_window(&n, 1000u));
	zassert_equal(-1, profile_cmd_sched_phase(1000u, &n));

	/* Byte for byte, slot for slot, told the time and not told it. */
	for (slot = 0u; slot < (2u * PROFILE_TLM_CYCLE); slot++) {
		enum profile_slot_kind ka;
		enum profile_slot_kind kb;

		memset(a, 0xA5u, sizeof(a));
		memset(b, 0x5Au, sizeof(b));
		ka = profile_sched_next_at(&with_hook, a, 4000123u + slot * 250000u);
		kb = profile_sched_next(&plain, b);
		zassert_equal(ka, kb, "slot %u", slot);
		if (ka != PROFILE_SLOT_IDLE) {
			zassert_mem_equal(a, b, sizeof(a), "slot %u", slot);
		}
	}
}

/* ---------------------------------------------------------------------------
 * 4. The slot itself
 * ---------------------------------------------------------------------------
 */

static uint32_t g_rx_events;
static uint8_t g_rx_payload[PROFILE_TLM_FRAME_LEN];
static radiant_time_t g_rx_t_sync;
static bool g_rx_have;

static void on_rx(uint8_t ch, uint8_t filter_index,
		  const struct radiant_rx_event *evt, void *user)
{
	ARG_UNUSED(ch);
	ARG_UNUSED(filter_index);
	ARG_UNUSED(user);

	if (evt->status != RADIANT_RADIO_STATUS_OK || evt->body == NULL) {
		return;
	}
	g_rx_events++;
	g_rx_t_sync = evt->t_sync;
	if (evt->body_len >= (RADIANT_FRAME_HDR_LEN_TRACKING + PROFILE_TLM_FRAME_LEN)) {
		memcpy(g_rx_payload, &evt->body[RADIANT_FRAME_HDR_LEN_TRACKING],
		       PROFILE_TLM_FRAME_LEN);
		g_rx_have = true;
	}
}

static void on_tx(uint8_t ch, const struct radiant_tx_event *evt, void *user)
{
	ARG_UNUSED(ch);
	ARG_UNUSED(evt);
	ARG_UNUSED(user);
}

static void on_done(uint8_t ch, enum radiant_sched_done why, void *user)
{
	ARG_UNUSED(ch);
	ARG_UNUSED(why);
	ARG_UNUSED(user);
}

static const struct radiant_sched_cbs slot_cbs = {
	.rx = on_rx,
	.tx = on_tx,
	.done = on_done,
};

ZTEST(command, test_a_response_slot_is_an_ordinary_bounded_receive_window)
{
	struct profile_cmd_node n;
	const struct fake_radio_arm *arm;
	radiant_time_t now;
	radiant_time_t open_at;

	node_init(&n, PROFILE_TLM_CMD_TAG_STD, EPOCH_A);
	zassert_ok(radiant_sched_init(&slot_cbs, NULL));
	zassert_ok(radiant_radio_init(radiant_sched_radio_cbs(), NULL));
	zassert_ok(radiant_radio_enable());

	now = radiant_radio_now() + 10000u;
	zassert_ok(profile_cmd_window_set(&n, &LIVE_SCHED, now + 400000u, false,
					  NODE_CLK_PPM));

	/* A node that expects nothing arms nothing, and that is not an error -
	 * it is the whole of the energy saving. */
	zassert_equal(-EAGAIN, profile_cmd_arm_listen(&n, now));
	zassert_equal(0u, profile_cmd_stats(&n)->windows);

	profile_cmd_expect(&n, 1u);
	open_at = profile_cmd_next_window(&n, now);
	zassert_not_equal(RADIANT_TIME_NEVER, open_at);
	zassert_true((open_at > now + 400000u ? open_at - (now + 400000u)
					      : (now + 400000u) - open_at) <= 31u);

	zassert_ok(profile_cmd_arm_listen(&n, now));
	zassert_ok(radiant_sched_tick());
	zassert_equal(1u, profile_cmd_stats(&n)->windows);
	zassert_equal(0u, n.expect_left, "an arm spends one expectation");

	/* THE CLAIM THIS TEST EXISTS FOR: what reached the radio is an RX arm,
	 * bounded, at the announced instant, for the announced dwell. No new
	 * slot kind, no energy-detect arm, no continuous window. */
	arm = fake_radio_last_arm();
	zassert_not_null(arm);
	zassert_equal(FAKE_RADIO_ARM_RX, arm->kind);
	zassert_equal(open_at, arm->t_open);
	zassert_equal(open_at + DL_DWELL_US, arm->t_close);

	fake_radio_advance_to(arm->t_close + 1000u);
	(void)radiant_sched_cancel(CMD_CH);
	radiant_sched_reset();
	zassert_true(fake_radio_is_idle(), "%s", fake_radio_busy_reason());
	zassert_equal(0u, fake_radio_viol_count());
}

ZTEST(command, test_a_command_lands_in_the_slot_the_node_armed)
{
	struct profile_cmd_node n;
	uint8_t body[PROFILE_TLM_CMD_LEN_STD];
	uint8_t ack[PROFILE_TLM_CMD_LEN_LR];
	uint8_t frame[FAKE_RADIO_AIR_FRAME_MAX];
	uint8_t frame_len;
	radiant_time_t now;
	radiant_time_t open_at;
	radiant_time_t t_request;
	int rc;

	node_init(&n, PROFILE_TLM_CMD_TAG_STD, EPOCH_A);
	zassert_ok(radiant_sched_init(&slot_cbs, NULL));
	zassert_ok(radiant_radio_init(radiant_sched_radio_cbs(), NULL));
	zassert_ok(radiant_radio_enable());

	g_rx_events = 0u;
	g_rx_have = false;

	now = radiant_radio_now() + 10000u;
	zassert_ok(profile_cmd_window_set(&n, &LIVE_SCHED, now + 700000u, false,
					  NODE_CLK_PPM));
	profile_cmd_expect(&n, 1u);

	/* The host asks for the command here. Everything after this is the
	 * latency being measured. */
	t_request = now;

	zassert_ok(profile_cmd_arm_listen(&n, now));
	zassert_ok(radiant_sched_tick());
	open_at = n.armed_open;

	(void)build_command(&n, 11u, PROFILE_TLM_CMD_SET_LEVEL, 0x01u, 0x0055u,
			    body, sizeof(body));
	frame_len = fake_radio_build_ant_frame(frame, NODE_DEVNUM, NODE_DEVTYPE,
					       NODE_TRANSTYPE, body);
	/* The commanding receiver aims at the middle of the announced dwell,
	 * which is what the dwell-versus-drift rule assumes it does. */
	zassert_ok(fake_radio_air_frame(open_at + (DL_DWELL_US / 2u), frame,
					frame_len));

	fake_radio_advance_to(open_at + DL_DWELL_US + 1000u);

	zassert_equal(1u, g_rx_events, "the frame landed inside the armed window");
	zassert_true(g_rx_have);
	zassert_mem_equal(g_rx_payload, body, PROFILE_TLM_CMD_LEN_STD);
	zassert_true(g_rx_t_sync >= open_at && g_rx_t_sync <= open_at + DL_DWELL_US);

	rc = profile_cmd_on_command(&n, g_rx_payload, PROFILE_TLM_CMD_LEN_STD,
				    g_rx_t_sync, ack, sizeof(ack));
	zassert_true(rc > 0);
	zassert_equal(1u, g_exec_calls);
	zassert_equal(0x0055u, g_last_arg);
	zassert_equal(1u, profile_cmd_stats(&n)->window_hits);

	/* SIMULATED latency, host request to node execution. Bounded by one
	 * interval by construction; see the file comment on what this is not. */
	zassert_true(g_rx_t_sync - t_request < DL_INTERVAL_US + DL_DWELL_US,
		     "simulated latency %llu us exceeded one interval",
		     (unsigned long long)(g_rx_t_sync - t_request));

	(void)radiant_sched_cancel(CMD_CH);
	radiant_sched_reset();
	zassert_true(fake_radio_is_idle(), "%s", fake_radio_busy_reason());
	zassert_equal(0u, fake_radio_viol_count());
}

/* ---------------------------------------------------------------------------
 * The gate's arithmetic, in simulated time
 * ---------------------------------------------------------------------------
 */

ZTEST(command, test_simulated_latency_is_bounded_by_one_interval_and_averages_half)
{
	struct profile_cmd_node n;
	const radiant_time_t anchor = 5000000u;
	uint64_t total = 0u;
	uint64_t worst = 0u;
	uint32_t i;
	const uint32_t k = 257u; /* prime, so the samples do not align with the
				  * interval's factors */

	node_init(&n, PROFILE_TLM_CMD_TAG_STD, EPOCH_A);
	zassert_ok(profile_cmd_window_set(&n, &LIVE_SCHED, anchor, false,
					  NODE_CLK_PPM));

	for (i = 0u; i < k; i++) {
		radiant_time_t t_req = anchor + 3000000u +
				       (radiant_time_t)(((uint64_t)i *
							 DL_INTERVAL_US) / k);
		radiant_time_t open_at = profile_cmd_next_window(&n, t_req);
		uint64_t wait;

		zassert_not_equal(RADIANT_TIME_NEVER, open_at);
		zassert_true(open_at >= t_req, "a window is never in the past");
		wait = (uint64_t)(open_at - t_req);

		total += wait;
		if (wait > worst) {
			worst = wait;
		}
	}

	/*
	 * THE BOUNDED WORST CASE. A request arriving just after a window waits
	 * almost exactly one interval and never more. This is the number the
	 * bench gate will measure against an FE-C channel, whose equivalent is
	 * its channel period; here it is asserted rather than observed.
	 */
	zassert_true(worst < DL_INTERVAL_US, "simulated worst case %llu us",
		     (unsigned long long)worst);
	zassert_true(worst > (DL_INTERVAL_US - (DL_INTERVAL_US / 100u)),
		     "the worst case should approach a full interval");

	/* THE MEAN. Uniform request arrivals over the interval give half of it,
	 * to within the phase field's quantisation. */
	{
		uint64_t mean = total / k;
		uint64_t want = DL_INTERVAL_US / 2u;
		uint64_t slack = want / 50u; /* 2 % */

		zassert_true((mean > want ? mean - want : want - mean) < slack,
			     "simulated mean %llu us against %llu us",
			     (unsigned long long)mean, (unsigned long long)want);
	}
}

ZTEST(command, test_a_missed_response_slot_costs_exactly_one_interval_and_no_more)
{
	struct profile_cmd_node n;
	const radiant_time_t anchor = 5000000u;
	const radiant_time_t t_req = anchor + 3000000u + 137u;
	radiant_time_t first;
	uint32_t misses;

	node_init(&n, PROFILE_TLM_CMD_TAG_STD, EPOCH_A);
	zassert_ok(profile_cmd_window_set(&n, &LIVE_SCHED, anchor, false,
					  NODE_CLK_PPM));

	first = profile_cmd_next_window(&n, t_req);
	zassert_not_equal(RADIANT_TIME_NEVER, first);

	/*
	 * A miss is a window that opened and caught nothing. The next attempt
	 * lands on the next window, so the cost of m misses is m intervals -
	 * NOT m channel periods, which is what it would be without a response
	 * slot and is the whole reason this phase exists.
	 */
	for (misses = 0u; misses <= 3u; misses++) {
		radiant_time_t landed =
			profile_cmd_next_window(&n, first + 1u +
						       (radiant_time_t)((uint64_t)misses *
									DL_INTERVAL_US));
		uint64_t latency = (uint64_t)(landed - t_req);
		uint64_t want = (uint64_t)(first - t_req) +
				((uint64_t)(misses + 1u) * DL_INTERVAL_US);

		/* The tolerance is two phase quantisations, 1/32768 s each: one
		 * for the announcement that produced `first` and one for the
		 * announcement that produced this window. It is not slack for
		 * the arithmetic - a miss costing a whole extra interval would
		 * be a thousand times this. */
		zassert_true((latency > want ? latency - want : want - latency) <= 61u,
			     "misses=%u simulated latency %llu us against %llu us",
			     misses, (unsigned long long)latency,
			     (unsigned long long)want);
		zassert_true(grid_error(landed, anchor) <= 31u,
			     "misses=%u landed off the node's grid", misses);
	}

	/* And a retry is free at the protocol layer as well as the slot layer:
	 * a duplicate that finally arrives executes nothing twice. */
	{
		uint8_t body[PROFILE_TLM_CMD_LEN_STD];
		uint8_t ack[PROFILE_TLM_CMD_LEN_LR];

		(void)build_command(&n, 77u, PROFILE_TLM_CMD_SET_BOOL, 0x01u, 1u,
				    body, sizeof(body));
		zassert_true(profile_cmd_on_command(&n, body, sizeof(body), t_req,
						    ack, sizeof(ack)) > 0);
		zassert_true(profile_cmd_on_command(&n, body, sizeof(body),
						    t_req + DL_INTERVAL_US, ack,
						    sizeof(ack)) > 0);
		zassert_true(profile_cmd_on_command(&n, body, sizeof(body),
						    t_req + (2u * DL_INTERVAL_US),
						    ack, sizeof(ack)) > 0);
		zassert_equal(1u, g_exec_calls);
		zassert_equal(2u, profile_cmd_stats(&n)->repeats);
	}
}

ZTEST(command, test_a_received_command_buys_the_windows_a_retry_needs)
{
	struct profile_cmd_node n;
	uint8_t body[PROFILE_TLM_CMD_LEN_STD];
	uint8_t ack[PROFILE_TLM_CMD_LEN_LR];

	node_init(&n, PROFILE_TLM_CMD_TAG_STD, EPOCH_A);
	zassert_ok(profile_cmd_window_set(&n, &LIVE_SCHED, 5000000u, false,
					  NODE_CLK_PPM));

	zassert_equal(0u, n.expect_left);
	(void)build_command(&n, 1u, PROFILE_TLM_CMD_NOP, PROFILE_TLM_TARGET_NODE, 0u,
			    body, sizeof(body));
	zassert_true(profile_cmd_on_command(&n, body, sizeof(body), 1000u, ack,
					    sizeof(ack)) > 0);
	zassert_equal(PROFILE_CMD_RETRY_WINDOWS, n.expect_left,
		      "a command opens the window its own retry will need");

	/* And so does a duplicate, or a retry of the retry would find the node
	 * deaf. */
	zassert_true(profile_cmd_on_command(&n, body, sizeof(body), 2000u, ack,
					    sizeof(ack)) > 0);
	zassert_equal(2u * PROFILE_CMD_RETRY_WINDOWS, n.expect_left);
}
