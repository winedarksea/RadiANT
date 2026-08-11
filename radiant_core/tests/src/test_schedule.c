/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_schedule.c - the descriptor schedule block, and the guard it widens.
 *
 * Three things are being established here, and only the third needs the radio:
 *
 *   1. THE CODEC. The block's frames are pinned as hex and shared byte for byte
 *      with tools/test_ant_pages.py, because two implementations checked only
 *      against each other are not checked at all.
 *   2. THE IDLE COST, which is the phase's gate. A node that announces nothing
 *      must be indistinguishable from a node built before the block existed -
 *      the same frames, the same frame count, and the same guard at every miss
 *      count - and a node that announces an all-defaulted block must produce the
 *      same scheduler decisions as one that announces none. This suite asserts
 *      both, byte for byte and number for number. IT STANDS IN FOR, AND DOES NOT
 *      REPLACE, the bench `loss_exact` run of tools/ab_gates.toml: what is
 *      deterministic here is that nothing the scheduler reads has changed, which
 *      is the mechanism by which loss would have changed.
 *   3. THE RC-CLOCKED MASTER, on the mock's virtual clock. A master whose period
 *      is 300 ppm off nominal at a two-second heartbeat is 595 us out on every
 *      slot, against a 400 us ceiling: it is lost when it says nothing and held
 *      when it announces its accuracy, and both halves are asserted against the
 *      same air, the same channel calls and the same arithmetic. The bench
 *      version of this - a real RC oscillator, over temperature, against
 *      loss_exact - is deferred to a hardware session and is a different claim.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include <radiant_core/radiant_channel.h>
#include <radiant_core/radiant_frame.h>
#include <radiant_core/radiant_radio_hal.h>

#include "../fake_radio.h"

#include "profile_handoff.h"
#include "profile_schedule.h"
#include "profile_telemetry.h"

/* ---------------------------------------------------------------------------
 * The canonical block, pinned
 * ---------------------------------------------------------------------------
 */

/*
 * 500 us of listening every 2 s - the duty the plan names - announced by a node
 * with a 32 kHz crystal, at +4 dBm, on the 1 M PHY.
 *
 * The phase is 12000 counts, 366 ms, and it is not round on purpose: a phase
 * that happened to be a whole number of anything would hide a packer that had
 * quietly dropped its low bits.
 */
static const struct profile_schedule CANON_SCHED = {
	.dl_interval = 64u,   /* units of 1/32 s: 2 s */
	.dl_phase = 12000u,   /* counts of 1/32768 s from the carrying frame */
	.dl_dwell = 1u,       /* 500 us */
	.coding = PROFILE_SCHED_CODING_NONE,
	.tx_power_dbm = 4,
};

/* Bytes [2..7] of the schedule frame. Shared with tools/test_ant_pages.py. */
static const uint8_t CANON_SCHED_BODY[6] = {
	0x20u, 0x80u, 0x5Du, 0xC0u, 0x1Au, 0x00u,
};

/*
 * The whole descriptor set with the block in it: four frames, and the schedule
 * frame is index 2 - between the headers and the fields, so that the
 * authentication frame keeps the last slot section 6 promises it.
 */
static const uint8_t CANON_DESC[4][8] = {
	{ 0x00u, 0x03u, 0x11u, 0x2Bu, 0xF6u, 0x1Fu, 0x39u, 0x00u },
	{ 0x00u, 0x13u, 0x00u, 0x0Eu, 0x00u, 0x00u, 0x00u, 0x00u },
	{ 0x00u, 0x23u, 0x20u, 0x80u, 0x5Du, 0xC0u, 0x1Au, 0x00u },
	{ 0x00u, 0x33u, 0x01u, 0x10u, 0x5Cu, 0xFEu, 0x01u, 0x00u },
};

static void quiet_descriptor(struct profile_descriptor *d)
{
	const struct profile_schedule quiet = PROFILE_SCHED_INIT_DEFAULT;

	memset(d, 0, sizeof(*d));
	d->version = PROFILE_TLM_VERSION;
	d->schema_id = 0x2Bu;
	d->period = 8182u;
	d->rf_index = PROFILE_TLM_RF_INDEX_DEFAULT;
	d->schedule = quiet;

	d->n_fields = 1u;
	d->fields[0].id = 0x01u;
	d->fields[0].type = PROFILE_TLM_TYPE_TEMPERATURE;
	d->fields[0].is_signed = true;
	d->fields[0].width_code = 7u; /* 16 bits */
	d->fields[0].exponent = -2;
	d->fields[0].page = 0x01u;
	d->fields[0].bit_offset = 0u;
}

static void canon_descriptor(struct profile_descriptor *d)
{
	quiet_descriptor(d);
	d->clock_stated = true;
	d->clock_accuracy = PROFILE_HANDOFF_CLK_30PPM;
	d->has_schedule = true;
	d->schedule = CANON_SCHED;
}

/* ---------------------------------------------------------------------------
 * Fixture
 * ---------------------------------------------------------------------------
 */

#define SCHED_CH   3u
#define QUIET_CH   4u
#define TYPE_SLAVE 0x00u

#define NODE_DEVNUM    0xB1CEu
#define NODE_DEVTYPE   0x60u
#define NODE_TRANSTYPE 0x05u

/* 65000 counts, 1.98 s: the sparse heartbeat the clock-accuracy field was
 * written for, and short enough that a whole test fits in the mock's advance
 * budget. */
#define SLOW_PERIOD 65000u

static const uint8_t payload8[8] = { 0x00u, 0x11u, 0x22u, 0x33u,
				     0x44u, 0x55u, 0x66u, 0x77u };

static struct radiant_rx_filter track_filter;
static bool g_heard;
static radiant_time_t g_t_sync;

static void rx_cb(const struct radiant_rx_event *e, void *user)
{
	ARG_UNUSED(user);

	/* The three terminal statuses go to the channel, exactly as
	 * radiant_api.c's adapter does; anything else is a frame. */
	if (e->status == RADIANT_RADIO_STATUS_TIMEOUT ||
	    e->status == RADIANT_RADIO_STATUS_ABORTED ||
	    e->status == RADIANT_RADIO_STATUS_FAILED) {
		(void)radiant_channel_on_terminal(e->op, e->status,
						  radiant_radio_now());
		return;
	}

	g_heard = true;
	g_t_sync = e->t_sync;
}

static void tx_cb(const struct radiant_tx_event *e, void *user)
{
	ARG_UNUSED(user);
	(void)radiant_channel_on_terminal(e->op, e->status, radiant_radio_now());
}

static const struct radiant_radio_cbs radio_cbs = { .rx = rx_cb, .tx = tx_cb };

static void sched_before(void *fixture)
{
	ARG_UNUSED(fixture);

	fake_radio_reset();
	radiant_channel_init();

	memset(&track_filter, 0, sizeof(track_filter));
	track_filter.addr[0] = RADIANT_NET_ADDR_ANT_PLUS_0;
	track_filter.addr[1] = RADIANT_NET_ADDR_ANT_PLUS_1;
	track_filter.addr[2] = (uint8_t)(NODE_DEVNUM & 0xFFu);
	track_filter.addr[3] = (uint8_t)(NODE_DEVNUM >> 8);
	track_filter.addr[4] = NODE_DEVTYPE;
	track_filter.addr_len = RADIANT_FRAME_ADDR_LEN_TRACKING;
}

ZTEST_SUITE(schedule, NULL, NULL, sched_before, NULL, NULL);

/* ---------------------------------------------------------------------------
 * The vocabulary
 * ---------------------------------------------------------------------------
 */

ZTEST(schedule, test_the_clock_ladder_is_the_handoff_pages_and_not_a_second_one)
{
	uint8_t code;

	for (code = 0u; code < PROFILE_HANDOFF_CLK_COUNT; code++) {
		uint8_t nibble = profile_sched_clk_nibble(code, true);

		zassert_equal(PROFILE_SCHED_CLK_STATED | code, nibble,
			      "the nibble is the stated bit and the 5b code");
		zassert_equal(profile_handoff_clk_ppm(code),
			      profile_sched_clk_ppm(nibble),
			      "the descriptor invented a second ladder");
	}

	/*
	 * THE ONE DISTINCTION THIS BLOCK ADDS. The handoff page's code 0 is the
	 * worst case, because only a receiver that already knows something sends
	 * that page. A descriptor's zeros are what every node built before this
	 * block existed transmits, so with the stated bit clear they are silence
	 * and not a claim of 500 ppm.
	 */
	zassert_equal(500u, profile_handoff_clk_ppm(PROFILE_HANDOFF_CLK_UNKNOWN));
	zassert_equal(0u, profile_sched_clk_ppm(0x00u));
	zassert_equal(500u, profile_sched_clk_ppm(PROFILE_SCHED_CLK_STATED));
	zassert_equal(0u, profile_sched_clk_nibble(PROFILE_HANDOFF_CLK_COUNT, true));
	zassert_equal(0u, profile_sched_clk_nibble(PROFILE_HANDOFF_CLK_20PPM, false));
}

ZTEST(schedule, test_the_coding_vocabulary_phase_six_inherits)
{
	/* One rate implemented, one defined and refused, the rest reserved. The
	 * kbps figures are what lets a consumer budget a window, which is why
	 * "yes" alone was never enough in the registry's LR PHY column. */
	zassert_equal(1000u, profile_sched_coding_kbps(PROFILE_SCHED_CODING_NONE));
	zassert_equal(125u, profile_sched_coding_kbps(PROFILE_SCHED_CODING_S8));
	zassert_equal(500u, profile_sched_coding_kbps(PROFILE_SCHED_CODING_S2));
	zassert_equal(0u, profile_sched_coding_kbps(7u));

	zassert_true(profile_sched_coding_implemented(PROFILE_SCHED_CODING_NONE));
	zassert_true(profile_sched_coding_implemented(PROFILE_SCHED_CODING_S8));
	zassert_false(profile_sched_coding_implemented(PROFILE_SCHED_CODING_S2));
	zassert_false(profile_sched_coding_implemented(7u));
}

ZTEST(schedule, test_the_dwell_ladder)
{
	zassert_equal(250u, profile_sched_dwell_us(0u));
	zassert_equal(500u, profile_sched_dwell_us(1u));
	zassert_equal(32000u, profile_sched_dwell_us(PROFILE_SCHED_DWELL_CODE_MAX));
	zassert_equal(0u, profile_sched_dwell_us(PROFILE_SCHED_DWELL_CODE_MAX + 1u));
}

/* ---------------------------------------------------------------------------
 * The block's 48 bits
 * ---------------------------------------------------------------------------
 */

ZTEST(schedule, test_the_block_is_byte_for_byte_what_the_python_encoder_emits)
{
	uint8_t body[6];

	zassert_ok(profile_sched_pack(&CANON_SCHED, false, 30u, body));
	zassert_mem_equal(body, CANON_SCHED_BODY, sizeof(body),
			  "the two implementations have drifted");
}

ZTEST(schedule, test_an_all_defaulted_block_is_forty_eight_zero_bits)
{
	const struct profile_schedule quiet = PROFILE_SCHED_INIT_DEFAULT;
	static const uint8_t zeros[6] = { 0u, 0u, 0u, 0u, 0u, 0u };
	struct profile_schedule got;
	uint8_t body[6];

	zassert_ok(profile_sched_pack(&quiet, false, 0u, body));
	zassert_mem_equal(body, zeros, sizeof(body),
			  "a block that announces nothing put something on air");

	/* And it decodes back to announcing nothing, rather than to 0 dBm -
	 * which is why the power field is biased on the wire. */
	zassert_ok(profile_sched_unpack(body, false, 0u, &got));
	zassert_equal(0u, got.dl_interval);
	zassert_equal(PROFILE_SCHED_TX_POWER_UNSTATED, got.tx_power_dbm);
	zassert_equal(PROFILE_SCHED_CODING_NONE, got.coding);
}

ZTEST(schedule, test_round_trip)
{
	struct profile_schedule got;
	uint8_t body[6];

	zassert_ok(profile_sched_pack(&CANON_SCHED, false, 30u, body));
	zassert_ok(profile_sched_unpack(body, false, 30u, &got));

	zassert_equal(CANON_SCHED.dl_interval, got.dl_interval);
	zassert_equal(CANON_SCHED.dl_phase, got.dl_phase);
	zassert_equal(CANON_SCHED.dl_dwell, got.dl_dwell);
	zassert_equal(CANON_SCHED.coding, got.coding);
	zassert_equal(CANON_SCHED.tx_power_dbm, got.tx_power_dbm);
}

ZTEST(schedule, test_the_reservation_is_refused_rather_than_ignored)
{
	struct profile_schedule got;
	uint8_t body[6];

	zassert_ok(profile_sched_pack(&CANON_SCHED, false, 30u, body));
	body[5] |= 0x01u; /* the last of the six reserved bits */
	zassert_equal(-EPROTO, profile_sched_unpack(body, false, 30u, &got));
}

ZTEST(schedule, test_the_phy_bit_and_the_coding_rate_must_agree)
{
	struct profile_schedule s = CANON_SCHED;
	uint8_t body[6];

	/* Long range with no coding rate tells a consumer nothing about how long
	 * a frame takes. */
	zassert_equal(-EINVAL, profile_sched_pack(&s, true, 30u, body));

	/* A coding rate on a 1 M channel is a node describing a PHY it is not
	 * using. */
	s.coding = PROFILE_SCHED_CODING_S8;
	zassert_equal(-EINVAL, profile_sched_pack(&s, false, 30u, body));

	/* Agreed, and S=8 is the rate this project has committed to. */
	zassert_ok(profile_sched_pack(&s, true, 30u, body));

	/* S=2 is defined so that adding it later is a value and not a format
	 * break, and refused so that nothing announces a rate no code
	 * transmits. */
	s.coding = PROFILE_SCHED_CODING_S2;
	zassert_equal(-ENOTSUP, profile_sched_pack(&s, true, 30u, body));
}

ZTEST(schedule, test_a_rate_this_build_cannot_use_still_decodes)
{
	struct profile_schedule s = CANON_SCHED;
	struct profile_schedule got;
	uint8_t body[6];

	s.coding = PROFILE_SCHED_CODING_S8;
	zassert_ok(profile_sched_pack(&s, true, 30u, body));

	/* Hand-edit the coding field to S=2 - what a node from a later build
	 * would send. The receiver decodes the node and declines the channel;
	 * it does not reject the node, because the rate describes the node and
	 * not the layout of these bytes. */
	body[3] = (uint8_t)((body[3] & 0xFEu) | 0x00u);
	body[4] = (uint8_t)((body[4] & 0x3Fu) | 0x80u);
	zassert_ok(profile_sched_unpack(body, true, 30u, &got));
	zassert_equal(PROFILE_SCHED_CODING_S2, got.coding);
	zassert_false(profile_sched_coding_implemented(got.coding));
}

ZTEST(schedule, test_the_dwell_must_cover_the_clock_error_it_announces)
{
	struct profile_schedule s = CANON_SCHED;
	uint8_t body[6];

	/* Point at a window most of an interval away - 60000 counts, 1.83 s -
	 * which is where the two announcements start to constrain each other. */
	s.dl_phase = 60000u;

	/* 500 us every 2 s is the plan's example duty, and a crystal node can
	 * offer it: 80 ppm relative over 1.83 s is 146 us, and twice that
	 * fits. */
	zassert_ok(profile_sched_pack(&s, false, 30u, body));

	/*
	 * The same announcement from an RC node is a window the commanding
	 * receiver cannot hit - 550 ppm over 1.83 s is 1007 us either side -
	 * and a downlink that silently never works is worse than one that was
	 * refused at the encoder.
	 */
	zassert_equal(-EINVAL, profile_sched_pack(&s, false, 500u, body));

	/* It can have the window; it has to open it wider. 2 ms is still short
	 * of twice the drift, and 4 ms is not. */
	s.dl_dwell = 3u; /* 2 ms */
	zassert_equal(-EINVAL, profile_sched_pack(&s, false, 500u, body));
	s.dl_dwell = 4u; /* 4 ms */
	zassert_ok(profile_sched_pack(&s, false, 500u, body));
}

ZTEST(schedule, test_tx_power_is_biased_so_that_zero_can_mean_silence)
{
	struct profile_schedule s = CANON_SCHED;
	struct profile_schedule got;
	uint8_t body[6];

	s.tx_power_dbm = 0;
	zassert_ok(profile_sched_pack(&s, false, 30u, body));
	zassert_ok(profile_sched_unpack(body, false, 30u, &got));
	zassert_equal(0, got.tx_power_dbm, "0 dBm decoded as silence");

	s.tx_power_dbm = PROFILE_SCHED_TX_POWER_UNSTATED;
	zassert_ok(profile_sched_pack(&s, false, 30u, body));
	zassert_ok(profile_sched_unpack(body, false, 30u, &got));
	zassert_equal(PROFILE_SCHED_TX_POWER_UNSTATED, got.tx_power_dbm);

	s.tx_power_dbm = 40; /* outside the range any of this runs at */
	zassert_equal(-EINVAL, profile_sched_pack(&s, false, 30u, body));

	/* Biased byte 228 is 128 once unbiased, which lands on the sentinel if
	 * a receiver narrows before it checks. */
	zassert_ok(profile_sched_pack(&CANON_SCHED, false, 30u, body));
	body[4] = (uint8_t)((body[4] & 0xC0u) | (228u >> 2));
	body[5] = (uint8_t)((body[5] & 0x3Fu) | ((228u & 0x03u) << 6));
	zassert_equal(-EINVAL, profile_sched_unpack(body, false, 30u, &got));
}

ZTEST(schedule, test_a_window_the_node_does_not_open_carries_no_phase)
{
	struct profile_schedule s = PROFILE_SCHED_INIT_DEFAULT;
	uint8_t body[6];

	s.dl_phase = 1u;
	zassert_equal(-EINVAL, profile_sched_pack(&s, false, 0u, body));

	s.dl_phase = 0u;
	s.dl_dwell = 2u;
	zassert_equal(-EINVAL, profile_sched_pack(&s, false, 0u, body));

	/* A phase at or past the interval is a window in the next cycle
	 * described as one in this one. */
	s = CANON_SCHED;
	s.dl_interval = 32u; /* 1 s, which is 32768 counts */
	s.dl_phase = 32768u;
	zassert_equal(-EINVAL, profile_sched_pack(&s, false, 30u, body),
		      "a phase at the interval boundary was accepted");
	s.dl_phase = 32767u;
	zassert_ok(profile_sched_pack(&s, false, 30u, body));
}

ZTEST(schedule, test_the_listen_window_is_relative_to_the_frame_that_carried_it)
{
	const struct profile_schedule quiet = PROFILE_SCHED_INIT_DEFAULT;
	radiant_time_t t_carrier = 1000000u;
	radiant_time_t first;
	radiant_time_t second;

	first = profile_sched_listen_at(&CANON_SCHED, t_carrier, 0u);
	second = profile_sched_listen_at(&CANON_SCHED, t_carrier, 1u);

	/* 12000 counts is 366210 us, and 64/32 s is 2000000 us exactly. */
	zassert_equal(t_carrier + 366210u, first);
	zassert_equal(first + 2000000u, second);

	/* Nothing announced is not "now": it is nothing. */
	zassert_equal(RADIANT_TIME_NEVER, profile_sched_listen_at(&quiet, t_carrier, 0u));
}

/* ---------------------------------------------------------------------------
 * The descriptor
 * ---------------------------------------------------------------------------
 */

ZTEST(schedule, test_the_descriptor_set_is_byte_for_byte_what_python_emits)
{
	struct profile_descriptor d;
	uint8_t frames[PROFILE_TLM_MAX_FRAMES * PROFILE_TLM_FRAME_LEN];

	canon_descriptor(&d);
	zassert_equal(4, profile_desc_encode(&d, frames, PROFILE_TLM_MAX_FRAMES));
	zassert_mem_equal(frames, CANON_DESC, sizeof(CANON_DESC),
			  "the two implementations have drifted");
	zassert_equal(2, profile_desc_schedule_index(&d));
}

ZTEST(schedule, test_a_node_that_announces_nothing_is_the_node_it_was_before)
{
	struct profile_descriptor d;
	uint8_t with[PROFILE_TLM_MAX_FRAMES * PROFILE_TLM_FRAME_LEN];
	uint8_t without[PROFILE_TLM_MAX_FRAMES * PROFILE_TLM_FRAME_LEN];
	int n_without;
	int n_with;

	/*
	 * THE IDLE-COST A/B, ON THE WIRE. The block is compiled in either way;
	 * the only difference is whether the node fills it. A node that does not
	 * must produce the same frames, in the same number, with frame 1's
	 * nibble still zero - which is the state every node built before this
	 * phase transmits, so "compatible" and "identical" are the same claim.
	 */
	quiet_descriptor(&d);
	n_without = profile_desc_encode(&d, without, PROFILE_TLM_MAX_FRAMES);
	zassert_equal(3, n_without, "a quiet node grew a frame");
	zassert_equal(0u, without[PROFILE_TLM_FRAME_LEN + 3] & 0x0Fu,
		      "a quiet node put something in the extension nibble");
	zassert_equal(-ENOENT, profile_desc_schedule_index(&d));

	/* The field frames sit at index 2 exactly as they did. */
	zassert_equal(0x01u, without[2u * PROFILE_TLM_FRAME_LEN + 2]);

	/* Announcing only a clock accuracy costs a nibble and NOT a frame,
	 * which is the whole reason it lives in frame 1: a sparse asset tag on
	 * an RC oscillator announces it without lengthening its descriptor. */
	d.clock_stated = true;
	d.clock_accuracy = PROFILE_HANDOFF_CLK_UNKNOWN;
	n_with = profile_desc_encode(&d, with, PROFILE_TLM_MAX_FRAMES);
	zassert_equal(n_without, n_with);
	with[PROFILE_TLM_FRAME_LEN + 3] &= 0xF0u;
	zassert_mem_equal(with, without, (size_t)n_with * PROFILE_TLM_FRAME_LEN,
			  "announcing a clock changed something other than the "
			  "nibble it is announced in");
}

ZTEST(schedule, test_the_set_round_trips_and_the_shape_is_derived)
{
	struct profile_descriptor d;
	struct profile_descriptor got;
	uint8_t frames[PROFILE_TLM_MAX_FRAMES * PROFILE_TLM_FRAME_LEN];

	canon_descriptor(&d);
	zassert_equal(4, profile_desc_encode(&d, frames, PROFILE_TLM_MAX_FRAMES));
	zassert_ok(profile_desc_decode(frames, 4u, &got));

	zassert_true(got.clock_stated);
	zassert_equal(PROFILE_HANDOFF_CLK_30PPM, got.clock_accuracy);
	zassert_equal(30u, profile_desc_clock_ppm(&got));
	zassert_true(got.has_schedule);
	zassert_equal(CANON_SCHED.dl_interval, got.schedule.dl_interval);
	zassert_equal(CANON_SCHED.dl_phase, got.schedule.dl_phase);
	zassert_equal(CANON_SCHED.dl_dwell, got.schedule.dl_dwell);
	zassert_equal(CANON_SCHED.tx_power_dbm, got.schedule.tx_power_dbm);
	zassert_equal(1u, got.n_fields);
	zassert_equal(0x01u, got.fields[0].id);
	zassert_equal(-2, got.fields[0].exponent);

	/*
	 * No presence bit was spent on the schedule frame: the count byte and
	 * the field count already say whether it is there. A set that claims one
	 * more frame than either shape allows is refused rather than guessed
	 * at, because guessing produces a schema nobody transmitted.
	 */
	frames[1] = (uint8_t)((frames[1] & 0xF0u) | 4u); /* claim 5 frames */
	zassert_equal(-EPROTO, profile_desc_decode(frames, 4u, &got));
}

ZTEST(schedule, test_a_ladder_code_with_nothing_stating_it_is_refused)
{
	struct profile_descriptor d;
	uint8_t frames[PROFILE_TLM_MAX_FRAMES * PROFILE_TLM_FRAME_LEN];

	quiet_descriptor(&d);
	d.clock_accuracy = PROFILE_HANDOFF_CLK_50PPM;
	zassert_equal(-EINVAL,
		      profile_desc_encode(&d, frames, PROFILE_TLM_MAX_FRAMES),
		      "the caller and the wire would have disagreed about what "
		      "this node announced");
}

/* ---------------------------------------------------------------------------
 * The guard
 * ---------------------------------------------------------------------------
 */

static void track_at(uint8_t ch, uint16_t period, radiant_time_t t0)
{
	struct radiant_channel_id id = { NODE_DEVNUM, NODE_DEVTYPE,
					 NODE_TRANSTYPE };

	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_assign(ch, TYPE_SLAVE, 0u, 0u));
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_id_set(ch, NODE_DEVNUM, NODE_DEVTYPE,
					     NODE_TRANSTYPE));
	zassert_equal(RADIANT_CH_OK, radiant_channel_period_set(ch, period));
	zassert_equal(RADIANT_CH_OK, radiant_channel_open(ch, 0u, t0));
	radiant_channel_on_acquired(ch, &id, t0);
}

/* Eight clean slots, exactly on the prediction, which is what locks the
 * estimator and takes the guard off its ceiling. */
static void lock_estimator(uint8_t ch)
{
	uint32_t i;

	for (i = 0u; i < RADIANT_CHANNEL_GUARD_LOCK_SLOTS; i++) {
		radiant_channel_on_slot(ch, radiant_channel_next_slot(ch));
	}
}

ZTEST(schedule, test_an_announcement_only_ever_widens)
{
	static const uint8_t ladder[] = {
		PROFILE_HANDOFF_CLK_20PPM,  PROFILE_HANDOFF_CLK_30PPM,
		PROFILE_HANDOFF_CLK_50PPM,  PROFILE_HANDOFF_CLK_75PPM,
		PROFILE_HANDOFF_CLK_100PPM, PROFILE_HANDOFF_CLK_150PPM,
		PROFILE_HANDOFF_CLK_250PPM, PROFILE_HANDOFF_CLK_UNKNOWN,
	};
	uint32_t baseline;
	uint32_t previous;
	uint32_t i;

	zassert_ok(radiant_radio_init(&radio_cbs, NULL));
	zassert_ok(radiant_radio_enable());

	track_at(SCHED_CH, SLOW_PERIOD, radiant_radio_now());
	lock_estimator(SCHED_CH);

	baseline = radiant_channel_guard_us(SCHED_CH);
	previous = baseline;

	for (i = 0u; i < ARRAY_SIZE(ladder); i++) {
		uint32_t guard;

		zassert_equal(profile_handoff_clk_ppm(ladder[i]),
			      profile_sched_apply_clock(
				      SCHED_CH,
				      profile_sched_clk_nibble(ladder[i], true)));
		guard = radiant_channel_guard_us(SCHED_CH);

		zassert_true(guard >= baseline,
			     "announcing %u ppm narrowed the guard from %u to %u",
			     profile_handoff_clk_ppm(ladder[i]),
			     (unsigned int)baseline, (unsigned int)guard);
		zassert_true(guard >= previous,
			     "the guard is not monotonic in the announcement");
		previous = guard;
	}

	/* The worst rung has to be genuinely wider, or the field is decorative:
	 * 550 ppm over a 1.98 s period is 1.09 ms, which no compiled-in ceiling
	 * of 400 us can cover. */
	zassert_true(previous > RADIANT_CHANNEL_GUARD_MAX_US,
		     "500 ppm at a 2 s period still fits in the old ceiling");

	/* And withdrawing the announcement puts it back exactly. */
	zassert_equal(0u, profile_sched_apply_clock(SCHED_CH, 0x00u));
	zassert_equal(baseline, radiant_channel_guard_us(SCHED_CH));

	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_close(SCHED_CH, radiant_radio_now()));
}

ZTEST(schedule, test_an_all_defaulted_block_changes_no_scheduler_decision)
{
	struct profile_descriptor d;
	uint8_t frames[PROFILE_TLM_MAX_FRAMES * PROFILE_TLM_FRAME_LEN];
	struct profile_descriptor got;
	radiant_time_t t0;
	uint32_t miss;

	zassert_ok(radiant_radio_init(&radio_cbs, NULL));
	zassert_ok(radiant_radio_enable());

	/*
	 * THE GATE, IN THE FORM THIS BENCH CAN ASSERT. Two channels on the same
	 * master: one told about an all-defaulted schedule block that really was
	 * decoded off real frames, one told nothing at all. Every number the
	 * scheduler reads out of a channel must match at every miss count, both
	 * before the estimator locks and after, because those numbers are the
	 * whole mechanism by which loss could have changed.
	 */
	quiet_descriptor(&d);
	d.has_schedule = true;
	d.schedule = (struct profile_schedule)PROFILE_SCHED_INIT_DEFAULT;
	zassert_equal(4, profile_desc_encode(&d, frames, PROFILE_TLM_MAX_FRAMES));
	zassert_ok(profile_desc_decode(frames, 4u, &got));
	zassert_true(got.has_schedule);
	zassert_equal(0u, profile_desc_clock_ppm(&got));

	t0 = radiant_radio_now();
	track_at(SCHED_CH, SLOW_PERIOD, t0);
	track_at(QUIET_CH, SLOW_PERIOD, t0);

	/* The block, applied. A defaulted one announces no clock, so this is a
	 * write of zero - and the assertion is that a write of zero is the same
	 * as no write at all. */
	zassert_equal(0u, profile_sched_apply_clock(SCHED_CH,
						    profile_sched_clk_nibble(
							    got.clock_accuracy,
							    got.clock_stated)));

	zassert_equal(radiant_channel_guard_us(QUIET_CH),
		      radiant_channel_guard_us(SCHED_CH),
		      "the unlocked guard moved");
	zassert_equal(radiant_channel_next_slot(QUIET_CH),
		      radiant_channel_next_slot(SCHED_CH));

	lock_estimator(SCHED_CH);
	lock_estimator(QUIET_CH);

	for (miss = 0u; miss < RADIANT_CHANNEL_RX_FAIL_TO_SEARCH - 1u; miss++) {
		zassert_equal(radiant_channel_guard_us(QUIET_CH),
			      radiant_channel_guard_us(SCHED_CH),
			      "the guard moved at miss %u", miss);
		zassert_equal(radiant_channel_next_slot(QUIET_CH),
			      radiant_channel_next_slot(SCHED_CH),
			      "the slot clock moved at miss %u", miss);
		zassert_equal(radiant_channel_residual_us(QUIET_CH),
			      radiant_channel_residual_us(SCHED_CH));

		(void)radiant_channel_on_slot_missed(SCHED_CH,
						     radiant_radio_now());
		(void)radiant_channel_on_slot_missed(QUIET_CH,
						     radiant_radio_now());
	}

	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_close(SCHED_CH, radiant_radio_now()));
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_close(QUIET_CH, radiant_radio_now()));
}

/* ---------------------------------------------------------------------------
 * The RC-clocked master
 * ---------------------------------------------------------------------------
 */

/*
 * One tracked slot, the way radiant_api.c does it: predict, size the window
 * from radiant_channel_guard_us(), arm it, let the mock decide whether the
 * master's frame is inside it, and tell the channel which way it went.
 *
 * The mock is the judge and this function is not: nothing here compares the
 * master's t_sync with the window it just armed.
 */
static bool tracked_slot(uint8_t ch, radiant_time_t t_true)
{
	struct radiant_rx_req req;
	const struct fake_radio_arm *arm;
	radiant_time_t predicted;
	uint32_t guard;
	uint32_t op = 0u;
	uint8_t frame[16];
	uint8_t len;

	predicted = radiant_channel_next_slot(ch);
	if (predicted == RADIANT_TIME_NEVER) {
		return false;
	}
	guard = radiant_channel_guard_us(ch);

	len = fake_radio_build_ant_frame(frame, NODE_DEVNUM, NODE_DEVTYPE,
					 NODE_TRANSTYPE, payload8);
	(void)fake_radio_air_frame(t_true, frame, len);

	memset(&req, 0, sizeof(req));
	req.fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &track_filter;
	req.n_filters = 1u;
	req.t_open = predicted - (radiant_time_t)guard;
	req.t_close = predicted + (radiant_time_t)guard;

	g_heard = false;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));
	radiant_channel_bind_op(ch, op);

	arm = fake_radio_arm_for_op(op);
	zassert_not_null(arm);
	fake_radio_advance_to(arm->t_close + 1u);

	if (g_heard) {
		radiant_channel_on_slot(ch, g_t_sync);
		return true;
	}
	(void)radiant_channel_on_slot_missed(ch, radiant_radio_now());
	return false;
}

/*
 * An RC-clocked master: nominal period on the wire, 300 ppm fast in reality,
 * which at this period is 595 us of disagreement PER SLOT and does not
 * accumulate, because the channel re-anchors on every frame it hears.
 *
 * Returns the number of slots heard out of `slots`.
 */
static uint32_t rc_master(uint8_t ch, uint16_t announced_ppm, uint32_t slots)
{
	radiant_time_t period;
	radiant_time_t true_period;
	radiant_time_t t0;
	uint32_t heard = 0u;
	uint32_t i;

	t0 = radiant_radio_now() + 10000u;
	track_at(ch, SLOW_PERIOD, t0);
	radiant_channel_clock_accuracy_set(ch, announced_ppm);

	period = radiant_channel_counts_to_us(SLOW_PERIOD);
	true_period = period + (period * 300u) / 1000000u;

	for (i = 1u; i <= slots; i++) {
		if (radiant_channel_state_get(ch) != RADIANT_CH_STATE_TRACKING) {
			break;
		}
		if (tracked_slot(ch, t0 + (radiant_time_t)i * true_period)) {
			heard++;
		}
	}
	return heard;
}

ZTEST(schedule, test_an_rc_clocked_master_is_lost_when_it_says_nothing)
{
	uint32_t heard;

	zassert_ok(radiant_radio_init(&radio_cbs, NULL));
	zassert_ok(radiant_radio_enable());

	heard = rc_master(SCHED_CH, 0u, RADIANT_CHANNEL_RX_FAIL_TO_SEARCH + 2u);

	/*
	 * Not one slot, and the failure is exactly the silent one the guard's
	 * header describes: 595 us of error against a 400 us window, no error
	 * code anywhere, and an estimator that can never lock because it is
	 * never handed a clean slot to average.
	 */
	zassert_equal(0u, heard, "the old ceiling covered 300 ppm after all");
	zassert_not_equal(RADIANT_CH_STATE_TRACKING,
			  radiant_channel_state_get(SCHED_CH),
			  "eight empty windows did not give up the channel");
	zassert_equal(0u, radiant_channel_residual_us(SCHED_CH));
}

ZTEST(schedule, test_an_rc_clocked_master_is_held_when_it_announces_itself)
{
	uint32_t slots = RADIANT_CHANNEL_RX_FAIL_TO_SEARCH + 6u;
	uint32_t heard;

	zassert_ok(radiant_radio_init(&radio_cbs, NULL));
	zassert_ok(radiant_radio_enable());

	/* What the descriptor's ladder code 0 means, arriving through the same
	 * call the descriptor would use. */
	heard = rc_master(SCHED_CH,
			  profile_handoff_clk_ppm(PROFILE_HANDOFF_CLK_UNKNOWN),
			  slots);

	zassert_equal(slots, heard, "%u of %u slots were lost",
		      (unsigned int)(slots - heard), (unsigned int)slots);
	zassert_equal(RADIANT_CH_STATE_TRACKING,
		      radiant_channel_state_get(SCHED_CH));

	/*
	 * And the estimator did lock, on a residual that is the master's real
	 * per-period error rather than a fiction - which is the second half of
	 * the fix: the window is wide because the master said so, and it stays
	 * wide because the measurement agrees.
	 */
	zassert_true(radiant_channel_residual_us(SCHED_CH) > 400u,
		     "the residual should be the real 595 us, not the old bound");
	zassert_true(radiant_channel_guard_us(SCHED_CH) >
			     RADIANT_CHANNEL_GUARD_MAX_US,
		     "the guard fell back inside the compiled-in ceiling");

	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_close(SCHED_CH, radiant_radio_now()));
}
