/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work, written against radiant_transfer.h,
 * radiant_frame.h, fake_radio.h, and the measurements in
 * docs/spike-b-part2-results.md - every control byte, microsecond, and packet
 * count asserted below is quoted from that document or its capture logs.
 * Nothing here derives from sdk-ant, libant.a, or any ANT+
 * device profile document.
 *
 * Acknowledged data is how Zwift sets trainer resistance, so this is the path
 * whose failures look like "ERG mode does not work" months later. Three
 * silent failure modes, each with a dedicated test:
 *
 *   1. THE STALL. src/ant_radio.h obligations B1-B5: the bridge holds one
 *      24-byte block behind a semaphore, released only on NEXT_DATA_BLOCK,
 *      TX_COMPLETED, or TX_FAILED. Miss one release and the bridge's
 *      k_sem_take() waits its full 1000 ms per host packet, invisibly.
 *
 *   2. THE SEQUENCE BIT is ONE bit, alternating per ON-AIR PACKET (not a
 *      three-bit field in 7:5, as a partial capture once suggested). Caught
 *      by asserting packet 2 and packet 0 carry the SAME byte.
 *
 *   3. THE REPLY'S BIT 4 IS A COMPLEMENT, not an echo (165 adjacent CRC-valid
 *      pairs, no counterexamples: 82->D2, 92->C2, A2->F2, B2->E2).
 *
 * Every test ends idle and with zero recorded fake_radio.h contract
 * violations - together they catch a window left armed and a callback doing
 * something that would deadlock a real radio ISR.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include "../fake_radio.h"
#include <radiant_core/radiant_transfer.h>

/* ---------------------------------------------------------------------------
 * The channel under test
 *
 * Spike A's and Spike B's own sensor: #14871 (0x3A17), device type 0x0B
 * (bicycle power), transmission type 5, on ANT+ RF 57. Using the real one means
 * a byte comparison against a capture line needs no translation.
 * ---------------------------------------------------------------------------
 */
#define DEVNUM      0x3A17u
#define DEVNUM_LO   0x17u
#define DEVNUM_HI   0x3Au
#define DEVTYPE     0x0Bu
#define TRANSTYPE   5u

/* Body layout: [ttype][ctrl][d0..d7]. body[1] (the control byte) is the most
 * load-bearing offset in the file. */
#define BODY_TTYPE  0u
#define BODY_CTRL   1u
#define BODY_D0     2u
#define BODY_LEN    10u

/* An on-air frame as fake_radio stores it: address bytes then body bytes, no
 * preamble and no CRC. 5 + 10. */
#define AIR_LEN     15u

static struct radiant_transfer xfer;

/* ---------------------------------------------------------------------------
 * What the engine tells us
 * ---------------------------------------------------------------------------
 */

#define EVLOG_MAX 16

struct ev_rec {
	enum radiant_transfer_event ev;
	uint8_t                *block;
	uint8_t                 block_len;
	enum radiant_transfer_fail  fail;
	uint32_t                packets;
};

static struct ev_rec evlog[EVLOG_MAX];
static uint32_t      n_ev;        /* attempted, so an overflow is visible */

static uint8_t  rx_payload[RADIANT_TRANSFER_PKT_BYTES];
static uint8_t  rx_len;
static bool     rx_last;
static uint32_t n_rx_data;

/* The channel's broadcast buffer, which is what every acknowledgement ever
 * captured carried. */
static uint8_t bcast_payload[RADIANT_TRANSFER_PKT_BYTES] = {
	0x10u, 0xBDu, 0xFFu, 0x50u, 0xDEu, 0x11u, 0x64u, 0x00u
};
static bool bcast_available;

static void on_done(void *ctx, const struct radiant_transfer_result *r)
{
	ARG_UNUSED(ctx);

	if (n_ev < EVLOG_MAX) {
		evlog[n_ev].ev = r->ev;
		evlog[n_ev].block = r->block;
		evlog[n_ev].block_len = r->block_len;
		evlog[n_ev].fail = r->fail;
		evlog[n_ev].packets = r->packets;
	}
	n_ev++;
}

static void on_rx_data(void *ctx, const uint8_t *payload, uint8_t len, bool last)
{
	ARG_UNUSED(ctx);

	rx_len = len;
	rx_last = last;
	if (len <= sizeof(rx_payload)) {
		memcpy(rx_payload, payload, len);
	}
	n_rx_data++;
}

static bool on_broadcast_payload(void *ctx, uint8_t out[RADIANT_TRANSFER_PKT_BYTES])
{
	ARG_UNUSED(ctx);

	if (!bcast_available) {
		return false;
	}
	memcpy(out, bcast_payload, RADIANT_TRANSFER_PKT_BYTES);
	return true;
}

static const struct radiant_transfer_ops ops = {
	.done = on_done,
	.rx_data = on_rx_data,
	.broadcast_payload = on_broadcast_payload,
};

/* ---------------------------------------------------------------------------
 * Routing radio events, exactly as radiant_api.c does
 *
 * The engine posts to radiant_sched.c (the only caller of radiant_radio_tx/rx
 * in radiant_core), so the scheduler owns the HAL's one global callback pair
 * and this harness owns the scheduler's. An arm through the scheduler has no
 * HAL operation id, so the engine stores RADIANT_TRANSFER_OP_EXTERNAL and
 * trusts its caller's routing instead of filtering by op id - so routing here
 * must be exact and per channel, which is what the scheduler's callbacks
 * already carry.
 * ---------------------------------------------------------------------------
 */

#define CH_XFER  0u   /* &xfer, the slave under test */
#define CH_B     1u   /* engine_b, for the two tests that need a second one */

/* A second engine, for the tests that need one. Only &xfer exists by default. */
static struct radiant_transfer *engine_b;

static struct radiant_transfer *engine_for(uint8_t ch)
{
	if (ch == CH_XFER) {
		return &xfer;
	}
	if (ch == CH_B) {
		return engine_b;
	}
	return NULL;
}

static void sched_rx(uint8_t ch, uint8_t filter_index,
		     const struct radiant_rx_event *e, void *user)
{
	struct radiant_transfer *t = engine_for(ch);

	ARG_UNUSED(filter_index);
	ARG_UNUSED(user);
	if (t != NULL) {
		radiant_transfer_on_rx_event(t, e);
	}
}

static void sched_tx(uint8_t ch, const struct radiant_tx_event *e, void *user)
{
	struct radiant_transfer *t = engine_for(ch);

	ARG_UNUSED(user);
	if (t != NULL) {
		radiant_transfer_on_tx_event(t, e);
	}
}

/* api_feed_xfer_terminal() from radiant_api.c, reduced to the two states this
 * suite can be in: without it a reply window that closed empty never tells
 * the engine, and the missing-ack test waits forever for a NO_ACK. */
static void sched_done(uint8_t ch, enum radiant_sched_done why, void *user)
{
	struct radiant_transfer *t = engine_for(ch);
	enum radiant_radio_status st;

	ARG_UNUSED(user);
	if (t == NULL || radiant_sched_pending(ch)) {
		/* Re-posted from inside the transmit callback, which is the
		 * normal path: the new request is what the slot holds and this
		 * completion belongs to the old one. */
		return;
	}

	/* api_done_to_status() from radiant_api.c: a window that ran and closed
	 * empty is TIMEOUT, not OK - mapping DONE_OK to STATUS_OK would hand the
	 * engine a success event with no frame attached. */
	switch (why) {
	case RADIANT_SCHED_DONE_OK:      st = RADIANT_RADIO_STATUS_TIMEOUT; break;
	case RADIANT_SCHED_DONE_ABORTED: st = RADIANT_RADIO_STATUS_ABORTED; break;
	case RADIANT_SCHED_DONE_MISSED:
	default:                         st = RADIANT_RADIO_STATUS_FAILED;  break;
	}

	switch (radiant_transfer_state(t)) {
	case RADIANT_TRANSFER_STATE_WAIT_ACK: {
		struct radiant_rx_event e;

		memset(&e, 0, sizeof(e));
		e.op = RADIANT_TRANSFER_OP_EXTERNAL;
		e.status = st;
		e.t_sync = radiant_radio_now();
		radiant_transfer_on_rx_event(t, &e);
		break;
	}
	case RADIANT_TRANSFER_STATE_TX_DATA:
	case RADIANT_TRANSFER_STATE_TX_REPLY:
	case RADIANT_TRANSFER_STATE_ABORTING: {
		struct radiant_tx_event e;

		memset(&e, 0, sizeof(e));
		e.op = RADIANT_TRANSFER_OP_EXTERNAL;
		e.status = st;
		e.t_sync = radiant_radio_now();
		e.t_sync_exact = false;
		radiant_transfer_on_tx_event(t, &e);
		break;
	}
	default:
		break;
	}
}

static const struct radiant_sched_cbs sched_cbs = {
	.rx = sched_rx,
	.tx = sched_tx,
	.done = sched_done,
};

/* Per radiant_transfer.h: a thread-context arm owes one radiant_sched_tick().
 * Posting is not committing - a test that inspected the arm log without
 * ticking first would see nothing and blame the engine. */
static void commit(void)
{
	(void)radiant_sched_tick();
}

/* ---------------------------------------------------------------------------
 * Fixture
 * ---------------------------------------------------------------------------
 */

static struct radiant_transfer_cfg cfg_slave;

static void make_cfg(struct radiant_transfer_cfg *c, bool slot_opener, uint8_t ch)
{
	memset(c, 0, sizeof(*c));
	c->id.device_number = DEVNUM;
	c->id.device_type = DEVTYPE;
	c->id.trans_type = TRANSTYPE;
	c->net_addr[0] = RADIANT_NET_ADDR_ANT_PLUS_0;
	c->net_addr[1] = RADIANT_NET_ADDR_ANT_PLUS_1;
	c->rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	c->power.dbm = 0;
	c->channel = ch;
	c->slot_opener = slot_opener;
	c->ops = &ops;
	c->ctx = NULL;
}

static void before(void *f)
{
	ARG_UNUSED(f);

	fake_radio_reset();
	/* Scheduler first, then the radio pointed at the scheduler's callbacks -
	 * the order radiant_core/tests/src/test_sched.c uses and radiant_api.c's
	 * bring-up follows. */
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_sched_init(&sched_cbs, NULL));
	zassert_equal(RADIANT_RADIO_OK_RC,
		      radiant_radio_init(radiant_sched_radio_cbs(), NULL));
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_enable());

	memset(evlog, 0, sizeof(evlog));
	n_ev = 0u;
	n_rx_data = 0u;
	rx_len = 0u;
	rx_last = false;
	bcast_available = true;
	engine_b = NULL;

	make_cfg(&cfg_slave, false, CH_XFER);
	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_init(&xfer, &cfg_slave));
}

ZTEST_SUITE(transfer, NULL, NULL, before, NULL, NULL);

/*
 * The end-of-test pair every suite in this tree runs. is_idle() catches an
 * engine that left an operation armed - which on hardware is a receiver burning
 * current forever - and the violation count catches a callback that did
 * something the HAL forbids and that only a real radio ISR would punish.
 */
static void end_of_test(void)
{
	zassert_true(fake_radio_is_idle(), "radio not idle: %s",
		     fake_radio_busy_reason());
	zassert_equal(0u, fake_radio_viol_count(), "contract violation: %s",
		      fake_radio_viol_name(fake_radio_viol(0)->code));
	zassert_true(n_ev <= EVLOG_MAX, "the event log overflowed");
}

/* ---------------------------------------------------------------------------
 * Driving one exchange against the mock
 * ---------------------------------------------------------------------------
 */

static const struct fake_radio_arm *last_arm_of(enum fake_radio_arm_kind kind)
{
	uint32_t n = fake_radio_arm_count();

	while (n > 0u) {
		const struct fake_radio_arm *a = fake_radio_arm(--n);

		if (a == NULL || a->kind != kind || a->rc != RADIANT_RADIO_OK_RC) {
			continue;
		}
		return a;
	}
	return NULL;
}

/* Every accepted transmit, oldest first - the on-air record the assertions
 * about the control-byte sequence read. */
static uint32_t collect_tx_bodies(uint8_t out[][RADIANT_RADIO_BODY_MAX], uint32_t cap)
{
	uint32_t n = fake_radio_arm_count();
	uint32_t found = 0u;
	uint32_t i;

	for (i = 0u; i < n && found < cap; i++) {
		const struct fake_radio_arm *a = fake_radio_arm(i);

		if (a == NULL || a->kind != FAKE_RADIO_ARM_TX ||
		    a->rc != RADIANT_RADIO_OK_RC) {
			continue;
		}
		memcpy(out[found], a->body, sizeof(out[found]));
		found++;
	}
	return found;
}

/* The peer's frame, hand-assembled rather than run through
 * radiant_frame_encode() (else the test would only assert the module agrees
 * with itself). Layout from docs/ant-radio-link.md, confirmed byte for byte
 * by Spike A: A6 C5 | dl dh | dtype | ttype | ctrl | d0..d7. */
static uint8_t build_peer_frame(uint8_t *out, uint8_t ctrl, const uint8_t *payload8)
{
	out[0] = RADIANT_NET_ADDR_ANT_PLUS_0;
	out[1] = RADIANT_NET_ADDR_ANT_PLUS_1;
	out[2] = DEVNUM_LO;
	out[3] = DEVNUM_HI;
	out[4] = DEVTYPE;
	out[5] = (uint8_t)TRANSTYPE;
	out[6] = ctrl;
	memcpy(&out[7], payload8, RADIANT_TRANSFER_PKT_BYTES);
	return AIR_LEN;
}

static const uint8_t peer_payload[RADIANT_TRANSFER_PKT_BYTES] = {
	0x10u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u, 0x88u
};

/* Put the acknowledgement of the packet just transmitted on the air, at
 * t_open + guard (the nominal window centre). The reply's control byte is
 * derived from what ACTUALLY went out (arm->body[1]), not what the test
 * expected, so this helper can't paper over a wrong encoding upstream. */
static void air_the_ack(void)
{
	const struct fake_radio_arm *tx = last_arm_of(FAKE_RADIO_ARM_TX);
	const struct fake_radio_arm *rx = last_arm_of(FAKE_RADIO_ARM_RX);
	uint8_t frame[AIR_LEN];
	uint8_t reply;

	zassert_not_null(tx, "no accepted transmit to acknowledge");
	zassert_not_null(rx, "no reply window was opened");

	reply = radiant_ctrl_reply_for(tx->body[BODY_CTRL]);
	zassert_not_equal(0u, reply,
			  "0x%02X has no measured acknowledgement", tx->body[BODY_CTRL]);

	(void)build_peer_frame(frame, reply, peer_payload);
	zassert_equal(RADIANT_RADIO_OK_RC,
		      fake_radio_air_frame(rx->t_open +
					   (radiant_time_t)RADIANT_TRANSFER_ACK_GUARD_US,
					   frame, AIR_LEN));
}

/* The host's blocks, in the order the bridge would hand them over. */
#define BLOCKS_MAX 4

static uint8_t  blocks[BLOCKS_MAX][RADIANT_TRANSFER_BLOCK_MAX];
static uint8_t  block_len[BLOCKS_MAX];
static uint8_t  block_seg[BLOCKS_MAX];
static uint32_t n_blocks;
static uint32_t next_block;
static uint32_t acks_to_air;

static void queue_block(uint32_t i, uint8_t len, uint8_t seg, uint8_t fill)
{
	uint8_t b;

	zassert_true(i < BLOCKS_MAX, "too many blocks");
	for (b = 0u; b < len; b++) {
		blocks[i][b] = (uint8_t)(fill + b);
	}
	block_len[i] = len;
	block_seg[i] = seg;
	if (i + 1u > n_blocks) {
		n_blocks = i + 1u;
	}
}

static void reset_blocks(void)
{
	memset(blocks, 0, sizeof(blocks));
	memset(block_len, 0, sizeof(block_len));
	memset(block_seg, 0, sizeof(block_seg));
	n_blocks = 0u;
	next_block = 0u;
	acks_to_air = 0xFFFFFFFFu;
}

static bool feed_next_block(void)
{
	int rc;

	if (next_block >= n_blocks) {
		return false;
	}
	rc = radiant_transfer_burst_block(&xfer, blocks[next_block],
				      block_len[next_block],
				      block_seg[next_block], RADIANT_TIME_NEVER);
	if (rc != RADIANT_TRANSFER_OK) {
		return false;
	}
	commit();
	next_block++;
	return true;
}

/* Fire the first data packet and let the peer acknowledge it, leaving the
 * engine part-way through a multi-packet block. */
static void run_one_packet_exchange(void)
{
	zassert_true(fake_radio_step(), "nothing was due; no packet was armed");
	zassert_equal(RADIANT_TRANSFER_STATE_WAIT_ACK, radiant_transfer_state(&xfer),
		      "the transmit callback did not open a reply window");
	air_the_ack();
	zassert_true(fake_radio_step(), "the acknowledgement never arrived");
}

/* Run the exchange to completion on the virtual clock. Nothing sleeps or
 * polls: every event fires inside fake_radio_step(); the loop only decides
 * what the peer and host do between two of them. */
static void drive(void)
{
	uint32_t acked_tx = 0u;
	uint32_t guard;

	for (guard = 0u; guard < 400u; guard++) {
		enum radiant_transfer_state st = radiant_transfer_state(&xfer);

		if (st == RADIANT_TRANSFER_STATE_IDLE) {
			return;
		}
		if (st == RADIANT_TRANSFER_STATE_WAIT_BLOCK) {
			if (!feed_next_block()) {
				/* The host ran dry. Leave the radio idle rather
				 * than the suite's end-of-test check failing for
				 * a reason the test did not intend. */
				(void)radiant_transfer_abort(&xfer);
				return;
			}
			continue;
		}
		/* One acknowledgement per data packet. Keyed off arms_tx (monotonic,
		 * counts what the peer would have heard) rather than
		 * radiant_transfer_op(), which is now always OP_EXTERNAL since arms
		 * go through the scheduler. */
		if (st == RADIANT_TRANSFER_STATE_WAIT_ACK &&
		    fake_radio_stats()->arms_tx != acked_tx) {
			acked_tx = fake_radio_stats()->arms_tx;
			if (acks_to_air > 0u) {
				acks_to_air--;
				air_the_ack();
			}
		}
		if (!fake_radio_step()) {
			return;
		}
	}
	zassert_unreachable("the exchange did not converge in 400 steps");
}

/* ---------------------------------------------------------------------------
 * The encoding
 * ---------------------------------------------------------------------------
 */

/* Acknowledged data IS a one-packet burst, and 0xAA is burst-last: sequence 0,
 * last packet, opening the slot. There is no RADIANT_MSG_ACKNOWLEDGED in
 * radiant_frame.h, and its absence is the finding. */
ZTEST(transfer, test_acknowledged_data_is_a_one_packet_burst)
{
	uint8_t opener = 0u;
	uint8_t in_slot = 0u;

	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_ctrl(0u, true, true, &opener));
	zassert_equal(RADIANT_CTRL_ACK_DATA_OPEN, opener,
		      "a one-packet slot-opening transfer must be 0xAA, got 0x%02X",
		      opener);

	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_ctrl(0u, true, false, &in_slot));
	zassert_equal(RADIANT_CTRL_BURST_LAST_SEQ0, in_slot,
		      "a one-packet in-slot transfer must be 0xA2, got 0x%02X",
		      in_slot);

	/* The two differ in exactly one bit, bit 3 (measured on run C: same
	 * board/stack/script/payload, only the channel role changed). */
	zassert_equal((uint8_t)(opener ^ in_slot), RADIANT_CTRL_SLOT,
		      "the slot-opening and in-slot forms must differ in bit 3 only");

	/* Both decode as burst-last, which is what a receiver dispatching on the
	 * control byte sees - it cannot tell acknowledged data from a one-packet
	 * burst, and must not pretend to. */
	zassert_equal(RADIANT_MSG_BURST_LAST, radiant_frame_msg_type(opener));
	zassert_equal(RADIANT_MSG_BURST_LAST, radiant_frame_msg_type(in_slot));
	zassert_true(radiant_ctrl_is_last(opener));
	zassert_equal(0u, radiant_ctrl_seq(opener), "0xAA is sequence 0");
	zassert_true(radiant_ctrl_is_slot_opener(opener));
	zassert_false(radiant_ctrl_is_slot_opener(in_slot));

	end_of_test();
}

/* The sequence is one bit, alternating per on-air packet. Measured over
 * bursts of 1, 2, 3, 6, 9, 17, 27, and 51 packets with the sniffer's
 * ring-drop counter at zero. */
ZTEST(transfer, test_the_sequence_is_one_bit_and_alternates_per_packet)
{
	uint32_t i;
	uint8_t c0 = 0u;
	uint8_t c2 = 0u;

	for (i = 0u; i < 51u; i++) {
		uint8_t mid = 0u;
		uint8_t last = 0u;

		zassert_equal(RADIANT_TRANSFER_OK,
			      radiant_transfer_ctrl(i, false, false, &mid));
		zassert_equal(RADIANT_TRANSFER_OK,
			      radiant_transfer_ctrl(i, true, false, &last));

		if ((i & 1u) == 0u) {
			zassert_equal(RADIANT_CTRL_BURST_SEQ0, mid,
				      "packet %u: expected 0x82, got 0x%02X",
				      (unsigned)i, mid);
			zassert_equal(RADIANT_CTRL_BURST_LAST_SEQ0, last,
				      "packet %u last: expected 0xA2, got 0x%02X",
				      (unsigned)i, last);
		} else {
			zassert_equal(RADIANT_CTRL_BURST_SEQ1, mid,
				      "packet %u: expected 0x92, got 0x%02X",
				      (unsigned)i, mid);
			zassert_equal(RADIANT_CTRL_BURST_LAST_SEQ1, last,
				      "packet %u last: expected 0xB2, got 0x%02X",
				      (unsigned)i, last);
		}
		zassert_equal((uint8_t)(i & 1u), radiant_ctrl_seq(mid));

		if (i == 0u) {
			c0 = mid;
		}
		if (i == 2u) {
			c2 = mid;
		}
	}

	zassert_equal(c0, c2,
		      "packet 2 differs from packet 0 (0x%02X vs 0x%02X): that is "
		      "a three-bit sequence field, and there is not one",
		      c2, c0);

	/* Only packet 0 of a slot-opening transfer carries bit 3. */
	{
		uint8_t p0 = 0u;
		uint8_t p1 = 0u;

		zassert_equal(RADIANT_TRANSFER_OK,
			      radiant_transfer_ctrl(0u, false, true, &p0));
		zassert_equal(RADIANT_TRANSFER_OK,
			      radiant_transfer_ctrl(1u, false, true, &p1));
		zassert_equal(RADIANT_CTRL_BURST_OPEN_SEQ0, p0,
			      "a master's first burst packet must be 0x8A");
		zassert_equal(RADIANT_CTRL_BURST_SEQ1, p1,
			      "packets 1..N do not open the slot");
	}

	end_of_test();
}

/* The reply relation on all four measured pairs: 165 adjacent CRC-valid
 * data/acknowledgement pairs across runs 0, A, and B, no counterexamples in
 * the 171 data packets those runs carried. */
ZTEST(transfer, test_the_reply_relation_on_the_four_measured_pairs)
{
	static const struct {
		uint8_t data;
		uint8_t reply;
	} pairs[] = {
		{ RADIANT_CTRL_BURST_SEQ0,      RADIANT_CTRL_ACK_SEQ1 },      /* 82 -> D2 */
		{ RADIANT_CTRL_BURST_SEQ1,      RADIANT_CTRL_ACK_SEQ0 },      /* 92 -> C2 */
		{ RADIANT_CTRL_BURST_LAST_SEQ0, RADIANT_CTRL_ACK_LAST_SEQ1 }, /* A2 -> F2 */
		{ RADIANT_CTRL_BURST_LAST_SEQ1, RADIANT_CTRL_ACK_LAST_SEQ0 }, /* B2 -> E2 */
	};
	size_t i;
	size_t j;

	for (i = 0u; i < ARRAY_SIZE(pairs); i++) {
		uint8_t d = pairs[i].data;
		uint8_t a = radiant_transfer_reply_ctrl(d);

		zassert_equal(pairs[i].reply, a,
			      "0x%02X should be acknowledged by 0x%02X, got 0x%02X",
			      d, pairs[i].reply, a);

		zassert_true(radiant_ctrl_is_ack(a), "bit 6 must be set on a reply");
		zassert_false(radiant_ctrl_is_ack(d), "bit 6 must be clear on data");
		zassert_equal(radiant_ctrl_is_last(d), radiant_ctrl_is_last(a),
			      "bit 5 is echoed, not changed");
		zassert_not_equal(radiant_ctrl_seq(d), radiant_ctrl_seq(a),
				  "bit 4 is COMPLEMENTED, not echoed: 0x%02X -> 0x%02X",
				  d, a);
		zassert_true(radiant_transfer_ack_matches(d, a));
		zassert_true(radiant_ctrl_observed(a),
			     "0x%02X has never been on the air", a);
	}

	/* Nothing else acknowledges anything else. In particular the predicate
	 * must reject the echo reading, which is the plausible wrong answer. */
	for (i = 0u; i < ARRAY_SIZE(pairs); i++) {
		for (j = 0u; j < ARRAY_SIZE(pairs); j++) {
			if (i == j) {
				continue;
			}
			zassert_false(radiant_transfer_ack_matches(pairs[i].data,
							       pairs[j].reply),
				      "0x%02X must not accept 0x%02X",
				      pairs[i].data, pairs[j].reply);
		}
	}

	/* A slot opener has no measured acknowledgement; guessing bit 3 here is
	 * the gap this refusal keeps open and visible (gap 2 in
	 * radiant_transfer.h). */
	zassert_equal(0u, radiant_transfer_reply_ctrl(RADIANT_CTRL_BURST_OPEN_SEQ0));
	zassert_equal(0u, radiant_transfer_reply_ctrl(RADIANT_CTRL_ACK_DATA_OPEN));
	zassert_equal(0u, radiant_transfer_reply_ctrl(RADIANT_CTRL_BROADCAST));

	end_of_test();
}

/* ---------------------------------------------------------------------------
 * Buffer ownership - obligations B1 through B5
 * ---------------------------------------------------------------------------
 */

/* The stall bug, directly: three 8-byte blocks, two producing exactly one
 * NEXT_BLOCK, the END block producing exactly one TX_COMPLETED. Every
 * accepted block appears in exactly one event. */
ZTEST(transfer, test_one_release_per_accepted_block)
{
	const struct radiant_transfer_stats *st;
	uint8_t bodies[6][RADIANT_RADIO_BODY_MAX];
	uint32_t n_bodies;
	uint32_t i;

	reset_blocks();
	queue_block(0u, 8u, RADIANT_TRANSFER_SEG_START, 0x10u);
	queue_block(1u, 8u, RADIANT_TRANSFER_SEG_CONTINUE, 0x20u);
	queue_block(2u, 8u, RADIANT_TRANSFER_SEG_END, 0x30u);

	zassert_true(feed_next_block(), "the first block was refused");
	drive();

	zassert_equal(3u, n_ev, "expected 3 events, got %u", (unsigned)n_ev);
	zassert_equal(RADIANT_TRANSFER_EV_NEXT_BLOCK, evlog[0].ev);
	zassert_equal_ptr(blocks[0], evlog[0].block,
			  "block 0 was not the one released");
	zassert_equal(RADIANT_TRANSFER_EV_NEXT_BLOCK, evlog[1].ev);
	zassert_equal_ptr(blocks[1], evlog[1].block);
	zassert_equal(RADIANT_TRANSFER_EV_TX_COMPLETED, evlog[2].ev,
		      "the END block must be released by TX_COMPLETED (B4)");
	zassert_equal_ptr(blocks[2], evlog[2].block);
	zassert_equal(RADIANT_TRANSFER_FAIL_NONE, evlog[2].fail);
	zassert_equal(3u, evlog[2].packets);

	st = radiant_transfer_stats(&xfer);
	zassert_equal(3u, st->blocks_accepted);
	zassert_equal(3u, st->blocks_released,
		      "released %u blocks for %u accepted: the bridge's semaphore "
		      "is now out of step and every host packet costs 1000 ms",
		      (unsigned)st->blocks_released,
		      (unsigned)st->blocks_accepted);
	zassert_equal(3u, st->packets_sent);
	zassert_equal(1u, st->transfers_ok);
	zassert_equal(0u, st->transfers_failed);

	/* And what actually went on the air: 82, 92, A2. Packet 2 carries the
	 * same byte as packet 0 in bits 7:5. */
	n_bodies = collect_tx_bodies(bodies, 6u);
	zassert_equal(3u, n_bodies, "expected 3 transmits, saw %u",
		      (unsigned)n_bodies);
	zassert_equal(RADIANT_CTRL_BURST_SEQ0, bodies[0][BODY_CTRL]);
	zassert_equal(RADIANT_CTRL_BURST_SEQ1, bodies[1][BODY_CTRL]);
	zassert_equal(RADIANT_CTRL_BURST_LAST_SEQ0, bodies[2][BODY_CTRL]);
	for (i = 0u; i < 3u; i++) {
		zassert_equal((uint8_t)TRANSTYPE, bodies[i][BODY_TTYPE]);
		zassert_mem_equal(&bodies[i][BODY_D0], blocks[i],
				  RADIANT_TRANSFER_PKT_BYTES,
				  "packet %u carried the wrong block", (unsigned)i);
	}

	end_of_test();
}

/* A 24-byte advanced-burst block is three on-air packets, and the sequence bit
 * alternates across the PACKETS, not the blocks - the air always carries
 * eight-byte packets. One block still produces exactly one release. */
ZTEST(transfer, test_an_advanced_burst_block_fragments_into_three_packets)
{
	const struct radiant_transfer_stats *st;
	uint8_t bodies[4][RADIANT_RADIO_BODY_MAX];
	uint32_t n_bodies;

	reset_blocks();
	queue_block(0u, 24u,
		    RADIANT_TRANSFER_SEG_START | RADIANT_TRANSFER_SEG_END, 0x40u);

	zassert_equal(3u, radiant_transfer_packets_in_block(24u));
	zassert_equal(2u, radiant_transfer_packets_in_block(16u));
	zassert_equal(1u, radiant_transfer_packets_in_block(8u));
	zassert_equal(0u, radiant_transfer_packets_in_block(12u),
		      "12 is not a block size any ANT stack fragments cleanly");

	zassert_true(feed_next_block());
	drive();

	n_bodies = collect_tx_bodies(bodies, 4u);
	zassert_equal(3u, n_bodies, "a 24-byte block is three packets, saw %u",
		      (unsigned)n_bodies);
	zassert_equal(RADIANT_CTRL_BURST_SEQ0, bodies[0][BODY_CTRL]);
	zassert_equal(RADIANT_CTRL_BURST_SEQ1, bodies[1][BODY_CTRL]);
	zassert_equal(RADIANT_CTRL_BURST_LAST_SEQ0, bodies[2][BODY_CTRL],
		      "only the third packet of the END block is burst-last");
	zassert_mem_equal(&bodies[0][BODY_D0], &blocks[0][0], 8u);
	zassert_mem_equal(&bodies[1][BODY_D0], &blocks[0][8], 8u);
	zassert_mem_equal(&bodies[2][BODY_D0], &blocks[0][16], 8u);

	zassert_equal(1u, n_ev);
	zassert_equal(RADIANT_TRANSFER_EV_TX_COMPLETED, evlog[0].ev);
	zassert_equal_ptr(blocks[0], evlog[0].block);
	zassert_equal(3u, evlog[0].packets);

	st = radiant_transfer_stats(&xfer);
	zassert_equal(1u, st->blocks_accepted);
	zassert_equal(1u, st->blocks_released);

	end_of_test();
}

/* B2 and B5: a rejected block is not owned, not touched, and produces no
 * event. Releasing one never accepted is worse than not releasing one that
 * was: it hands back a semaphore for a different block, and the next host
 * packet overwrites a buffer the radio is transmitting from. */
ZTEST(transfer, test_a_rejected_block_is_never_released)
{
	const struct radiant_transfer_stats *st;
	uint8_t stray[RADIANT_TRANSFER_BLOCK_MAX];

	reset_blocks();
	memset(stray, 0xEEu, sizeof(stray));
	queue_block(0u, 8u, RADIANT_TRANSFER_SEG_START, 0x50u);

	/* A length no ANT stack fragments into eight-byte packets. */
	zassert_equal(RADIANT_TRANSFER_EINVAL,
		      radiant_transfer_burst_block(&xfer, stray, 12u,
					       RADIANT_TRANSFER_SEG_START,
					       RADIANT_TIME_NEVER));
	/* A continuation with no transfer open. */
	zassert_equal(RADIANT_TRANSFER_ESEQ,
		      radiant_transfer_burst_block(&xfer, stray, 8u,
					       RADIANT_TRANSFER_SEG_CONTINUE,
					       RADIANT_TIME_NEVER));
	/* An undefined segment bit. */
	zassert_equal(RADIANT_TRANSFER_EINVAL,
		      radiant_transfer_burst_block(&xfer, stray, 8u, 0x04u,
					       RADIANT_TIME_NEVER));
	zassert_equal(0u, n_ev, "a rejected block raised %u events", (unsigned)n_ev);
	zassert_equal(0u, radiant_transfer_stats(&xfer)->blocks_accepted);

	/* And mid-packet, which is the ANTW_TRANSFER_IN_PROGRESS the bridge
	 * already knows how to answer with. */
	zassert_true(feed_next_block());
	zassert_equal(RADIANT_TRANSFER_STATE_TX_DATA, radiant_transfer_state(&xfer));
	zassert_equal(RADIANT_TRANSFER_EBUSY,
		      radiant_transfer_burst_block(&xfer, stray, 8u,
					       RADIANT_TRANSFER_SEG_CONTINUE,
					       RADIANT_TIME_NEVER));
	/* And a second START is not a restart either - the engine is mid-packet
	 * and the answer is the same one the host already knows how to wait on. */
	zassert_equal(RADIANT_TRANSFER_EBUSY,
		      radiant_transfer_burst_block(&xfer, stray, 8u,
					       RADIANT_TRANSFER_SEG_START,
					       RADIANT_TIME_NEVER));
	zassert_equal(0u, n_ev);

	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_abort(&xfer));

	st = radiant_transfer_stats(&xfer);
	zassert_equal(1u, st->blocks_accepted, "only the good block was accepted");
	zassert_equal(1u, st->blocks_released);
	zassert_equal(1u, n_ev);
	zassert_equal_ptr(blocks[0], evlog[0].block,
		      "the released block must be the accepted one, never the "
		      "rejected one");

	end_of_test();
}

/* A missing acknowledgement fails the transfer once, releases the block in
 * hand, and does NOT retry. Untested item 5 of the spike (no measured
 * behaviour exists); this pins the decision so a retry, if added, is
 * deliberate. */
ZTEST(transfer, test_a_missing_acknowledgement_fails_once_and_releases_the_block)
{
	const struct radiant_transfer_stats *st;
	uint32_t arms_before;

	reset_blocks();
	queue_block(0u, 8u, RADIANT_TRANSFER_SEG_START, 0x60u);
	queue_block(1u, 8u, RADIANT_TRANSFER_SEG_END, 0x70u);

	/* Acknowledge the first packet and then go quiet. */
	acks_to_air = 1u;

	zassert_true(feed_next_block());
	drive();

	zassert_equal(2u, n_ev, "expected NEXT_BLOCK then TX_FAILED, got %u events",
		      (unsigned)n_ev);
	zassert_equal(RADIANT_TRANSFER_EV_NEXT_BLOCK, evlog[0].ev);
	zassert_equal_ptr(blocks[0], evlog[0].block);
	zassert_equal(RADIANT_TRANSFER_EV_TX_FAILED, evlog[1].ev);
	zassert_equal(RADIANT_TRANSFER_FAIL_NO_ACK, evlog[1].fail);
	zassert_equal_ptr(blocks[1], evlog[1].block,
		      "the block in hand must come back with TX_FAILED (B1)");
	zassert_equal(1u, evlog[1].packets);

	st = radiant_transfer_stats(&xfer);
	zassert_equal(2u, st->blocks_accepted);
	zassert_equal(2u, st->blocks_released);
	zassert_equal(1u, st->transfers_failed);

	/* No retransmission of the unacknowledged packet. */
	arms_before = fake_radio_arm_count();
	fake_radio_advance(50000u);
	zassert_equal(arms_before, fake_radio_arm_count(),
		      "the sender retried an unacknowledged packet; that behaviour "
		      "has never been measured");

	end_of_test();
}

/* A burst aborted mid-flight, and the late terminal event that follows it. A
 * cancelled operation's terminal event still arrives (per HAL contract), so
 * the engine ends on that event, not the abort call, and must recognise a
 * second, genuinely late one as somebody else's. */
ZTEST(transfer, test_a_burst_aborted_midflight_and_its_late_terminal_event)
{
	const struct radiant_transfer_stats *st;
	uint32_t dead_op;

	reset_blocks();
	queue_block(0u, 16u, RADIANT_TRANSFER_SEG_START, 0x80u);
	queue_block(1u, 8u, RADIANT_TRANSFER_SEG_END, 0x90u);

	zassert_true(feed_next_block());
	/* One packet out, its acknowledgement in, the second packet of the same
	 * 16-byte block now armed. */
	run_one_packet_exchange();
	zassert_equal(RADIANT_TRANSFER_STATE_TX_DATA, radiant_transfer_state(&xfer));

	dead_op = radiant_transfer_op(&xfer);
	zassert_not_equal(0u, dead_op);

	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_abort(&xfer));

	zassert_equal(RADIANT_TRANSFER_STATE_IDLE, radiant_transfer_state(&xfer),
		      "the abort's terminal event should already have arrived");
	zassert_equal(1u, n_ev, "abort must raise exactly one event, got %u",
		      (unsigned)n_ev);
	zassert_equal(RADIANT_TRANSFER_EV_TX_FAILED, evlog[0].ev);
	zassert_equal(RADIANT_TRANSFER_FAIL_ABORTED, evlog[0].fail);
	zassert_equal_ptr(blocks[0], evlog[0].block,
		      "the block in hand must come back exactly once");

	st = radiant_transfer_stats(&xfer);
	zassert_equal(1u, st->blocks_accepted);
	zassert_equal(1u, st->blocks_released);

	/* The events already in the pipeline when the abort ran, handed to the
	 * engine directly rather than via fake_radio's injection helpers - an
	 * injected late event would now be absorbed by radiant_sched.c, one
	 * layer below the thing under test. What's under test is the engine's
	 * own last line of defence: t->op is 0 (retired), so an event arriving
	 * anyway must be counted and dropped, not turned into a second release.
	 */
	{
		struct radiant_tx_event late_tx;
		struct radiant_rx_event late_rx;
		uint8_t frame[AIR_LEN];

		memset(&late_tx, 0, sizeof(late_tx));
		late_tx.op = dead_op;
		late_tx.status = RADIANT_RADIO_STATUS_ABORTED;
		late_tx.t_sync = radiant_radio_now();
		radiant_transfer_on_tx_event(&xfer, &late_tx);

		memset(&late_rx, 0, sizeof(late_rx));
		late_rx.op = dead_op;
		late_rx.status = RADIANT_RADIO_STATUS_OK;
		late_rx.t_sync = radiant_radio_now();
		/* The HAL hands a backend's body, which is the frame with its
		 * address bytes already consumed by the matcher. */
		(void)build_peer_frame(frame, RADIANT_CTRL_ACK_SEQ1, peer_payload);
		late_rx.body = &frame[RADIANT_NET_ADDR_LEN];
		late_rx.body_len = (uint8_t)(AIR_LEN - RADIANT_NET_ADDR_LEN);
		radiant_transfer_on_rx_event(&xfer, &late_rx);
	}

	zassert_equal(1u, n_ev, "a late event produced a second transfer event");
	zassert_true(radiant_transfer_stats(&xfer)->late_events >= 2u,
		     "late events must be counted, not silently dropped");
	zassert_equal(1u, radiant_transfer_stats(&xfer)->blocks_released);

	end_of_test();
}

/* ---------------------------------------------------------------------------
 * Defensive invariants, cross-checked against sdk-ant's own burst handler
 *
 * sdk-ant resets its handler on every unexpected state rather than continuing
 * (docs/sdk-ant-comparison.md item 5). None of the three below copies
 * sdk-ant's code or wire behaviour - the source is D00000652's sequencing
 * rules, restated as a property of this engine - but the fault shape is the
 * same, and the engine's answer is written out so a future change has
 * something to break loudly against.
 * ---------------------------------------------------------------------------
 */

/* Out-of-order sequencing: a SEG_START where a CONTINUE was expected. sdk-ant
 * resets and answers TRANSFER_SEQUENCE_NUMBER_ERROR; this engine needs no
 * reset since radiant_transfer_submit() refuses the bad request before
 * touching any state. */
ZTEST(transfer, test_a_restart_mid_block_wait_is_a_sequence_error)
{
	uint32_t ev_before;

	reset_blocks();
	queue_block(0u, 8u, RADIANT_TRANSFER_SEG_START, 0xA0u);

	zassert_true(feed_next_block());
	run_one_packet_exchange();
	zassert_equal(RADIANT_TRANSFER_STATE_WAIT_BLOCK, radiant_transfer_state(&xfer),
		      "one packet in an 8-byte block must finish the block");

	/* Snapshot, not zero: reaching WAIT_BLOCK is itself an event
	 * (NEXT_BLOCK on the block's last-packet ack), so n_ev is 1 here.
	 * The claim under test is about the refusal, not the count. */
	ev_before = n_ev;

	zassert_equal(RADIANT_TRANSFER_ESEQ,
		      radiant_transfer_burst_block(&xfer, blocks[1], 8u,
						RADIANT_TRANSFER_SEG_START,
						RADIANT_TIME_NEVER),
		      "a SEG_START while WAIT_BLOCK must be refused as ESEQ");
	zassert_equal(RADIANT_TRANSFER_STATE_WAIT_BLOCK, radiant_transfer_state(&xfer),
		      "a refused restart must not disturb the block already in flight");
	zassert_equal(ev_before, n_ev, "the refusal itself raises no event");
	zassert_equal(1u, radiant_transfer_stats(&xfer)->blocks_accepted);

	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_abort(&xfer));
	end_of_test();
}

/* An unrecognised message mid-transfer: a frame that decodes cleanly but
 * isn't the acknowledgement expected. sdk-ant resets; this engine fails the
 * transfer once (B1: the block in hand still comes back) and returns to
 * IDLE. */
ZTEST(transfer, test_a_reply_that_is_not_an_acknowledgement_fails_like_sdk_ants_reset)
{
	const struct fake_radio_arm *tx;
	const struct fake_radio_arm *rx;
	uint8_t frame[AIR_LEN];

	reset_blocks();
	queue_block(0u, 8u, RADIANT_TRANSFER_SEG_START | RADIANT_TRANSFER_SEG_END, 0xB0u);
	zassert_true(feed_next_block());
	zassert_true(fake_radio_step(), "no packet was armed");
	zassert_equal(RADIANT_TRANSFER_STATE_WAIT_ACK, radiant_transfer_state(&xfer));

	tx = last_arm_of(FAKE_RADIO_ARM_TX);
	rx = last_arm_of(FAKE_RADIO_ARM_RX);
	zassert_not_null(tx, "no accepted transmit to reply to");
	zassert_not_null(rx, "no reply window was opened");

	/* The peer's own DATA control byte, echoed back instead of the ACK
	 * byte this window is waiting for - decodable, and wrong. */
	(void)build_peer_frame(frame, tx->body[BODY_CTRL], peer_payload);
	zassert_equal(RADIANT_RADIO_OK_RC,
		      fake_radio_air_frame(rx->t_open +
					   (radiant_time_t)RADIANT_TRANSFER_ACK_GUARD_US,
					   frame, AIR_LEN));
	zassert_true(fake_radio_step(), "the mismatched frame never arrived");

	zassert_equal(RADIANT_TRANSFER_STATE_IDLE, radiant_transfer_state(&xfer),
		      "a mismatched reply must fail the transfer, not leave it armed");
	zassert_equal(1u, n_ev);
	zassert_equal(RADIANT_TRANSFER_EV_TX_FAILED, evlog[0].ev);
	zassert_equal(RADIANT_TRANSFER_FAIL_BAD_ACK, evlog[0].fail);
	zassert_equal_ptr(blocks[0], evlog[0].block,
			  "the block in hand must still come back (B1)");

	end_of_test();
}

/* A stack event arriving in IDLE. sdk-ant resets harmlessly; this engine gets
 * the same property by construction: reset_transfer() clears t->op to 0, and
 * both event entry points check t->op != 0, so an event with nothing armed is
 * counted as late and dropped. */
ZTEST(transfer, test_an_event_with_nothing_armed_is_counted_and_dropped)
{
	struct radiant_rx_event rx_ev;
	struct radiant_tx_event tx_ev;

	zassert_equal(RADIANT_TRANSFER_STATE_IDLE, radiant_transfer_state(&xfer));
	zassert_equal(0u, radiant_transfer_op(&xfer));

	memset(&rx_ev, 0, sizeof(rx_ev));
	rx_ev.op = 12345u;
	rx_ev.status = RADIANT_RADIO_STATUS_OK;
	radiant_transfer_on_rx_event(&xfer, &rx_ev);

	memset(&tx_ev, 0, sizeof(tx_ev));
	tx_ev.op = 12345u;
	tx_ev.status = RADIANT_RADIO_STATUS_OK;
	radiant_transfer_on_tx_event(&xfer, &tx_ev);

	zassert_equal(RADIANT_TRANSFER_STATE_IDLE, radiant_transfer_state(&xfer));
	zassert_equal(0u, n_ev, "an event with nothing armed must not raise anything");
	zassert_equal(2u, radiant_transfer_stats(&xfer)->late_events);

	end_of_test();
}

/* ---------------------------------------------------------------------------
 * The receive path - the tightest deadline in the link layer
 * ---------------------------------------------------------------------------
 */

/* A received data packet is acknowledged within the measured budget
 * (data->ack was 1567 us n=33 and 1559 us n=128, range 1515..1599). The
 * engine must arm the ack frame before handing the payload up - time spent
 * in the caller comes out of the budget, with no error code to catch it. */
ZTEST(transfer, test_the_receive_path_meets_the_turnaround_deadline)
{
	struct radiant_frame in;
	struct radiant_ctrl_fields fields;
	const struct fake_radio_arm *arm;
	radiant_time_t t_sync;
	uint64_t turnaround;

	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_ctrl_fields(0u, false, false, &fields));
	zassert_equal(RADIANT_FRAME_OK,
		      radiant_frame_make(&in, &cfg_slave.id, &fields, peer_payload,
				     RADIANT_TRANSFER_PKT_BYTES));
	zassert_equal(RADIANT_CTRL_BURST_SEQ0, in.ctrl_byte);

	t_sync = radiant_radio_now() + 10000u;

	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_on_data(&xfer, &in, t_sync));
	zassert_equal(RADIANT_TRANSFER_STATE_TX_REPLY, radiant_transfer_state(&xfer));
	commit();

	arm = last_arm_of(FAKE_RADIO_ARM_TX);
	zassert_not_null(arm, "the acknowledgement was never armed");

	turnaround = (uint64_t)(arm->t_sync_at - t_sync);
	zassert_equal((uint64_t)RADIANT_TRANSFER_REPLY_US, turnaround,
		      "reply at +%llu us; the measured interval is 1559-1567",
		      (unsigned long long)turnaround);
	zassert_true(turnaround <= 1599u,
		     "the reply misses the widest interval ever measured");

	/* Bit 6 set, bit 5 echoed, bit 4 complemented: 0x82 -> 0xD2. */
	zassert_equal(RADIANT_CTRL_ACK_SEQ1, arm->body[BODY_CTRL],
		      "acknowledged 0x82 with 0x%02X", arm->body[BODY_CTRL]);
	zassert_equal((uint8_t)TRANSTYPE, arm->body[BODY_TTYPE]);
	/* And it carries the broadcast buffer, which is what every
	 * acknowledgement ever captured carried. */
	zassert_mem_equal(&arm->body[BODY_D0], bcast_payload,
			  RADIANT_TRANSFER_PKT_BYTES,
			  "an acknowledgement must carry the broadcast buffer");
	zassert_equal(BODY_LEN, arm->body_len);

	/* The payload went up, after the transmit was armed and not before. */
	zassert_equal(1u, n_rx_data);
	zassert_equal(RADIANT_TRANSFER_PKT_BYTES, rx_len);
	zassert_false(rx_last, "0x82 is not the last packet of a transfer");
	zassert_mem_equal(rx_payload, peer_payload, RADIANT_TRANSFER_PKT_BYTES);

	fake_radio_advance_to(arm->t_sync_at + 1u);
	zassert_equal(RADIANT_TRANSFER_STATE_IDLE, radiant_transfer_state(&xfer));
	zassert_equal(1u, radiant_transfer_stats(&xfer)->acks_sent);

	end_of_test();
}

/* A one-packet transfer received - acknowledged data, or a one-packet burst,
 * and the receiver cannot tell which. 0xA2 -> 0xF2, and `last` is true. */
ZTEST(transfer, test_received_acknowledged_data_is_answered_with_transfer_ack)
{
	struct radiant_frame in;
	struct radiant_ctrl_fields fields;
	const struct fake_radio_arm *arm;

	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_ctrl_fields(0u, true, false, &fields));
	zassert_equal(RADIANT_FRAME_OK,
		      radiant_frame_make(&in, &cfg_slave.id, &fields, peer_payload,
				     RADIANT_TRANSFER_PKT_BYTES));
	zassert_equal(RADIANT_CTRL_BURST_LAST_SEQ0, in.ctrl_byte);

	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_on_data(&xfer, &in, radiant_radio_now() + 10000u));
	commit();

	arm = last_arm_of(FAKE_RADIO_ARM_TX);
	zassert_not_null(arm);
	/* 0xA2 -> 0xF2 (RADIANT_CTRL_ACK_LAST_SEQ1, not _SEQ0): bit 4 is the
	 * sequence bit and the reply complements it, so the ack of a
	 * sequence-0 final packet is necessarily the sequence-1 constant. */
	zassert_equal(RADIANT_CTRL_ACK_LAST_SEQ1, arm->body[BODY_CTRL],
		      "0xA2 is acknowledged by 0xF2 - bit 5 echoed, bit 4 "
		      "flipped - got 0x%02X", arm->body[BODY_CTRL]);
	zassert_true(rx_last, "bit 5 is what tells the layer above the transfer "
			      "is complete");

	fake_radio_advance_to(arm->t_sync_at + 1u);
	end_of_test();
}

/* A slot-opening data packet is not acknowledged - no acknowledgement of one
 * has ever been captured. radiant_ctrl_reply_for() returns 0 for 0x8A/0xAA
 * and this module refuses rather than guessing bit 3. */
ZTEST(transfer, test_a_slot_opening_packet_has_no_measured_acknowledgement)
{
	struct radiant_frame in;
	struct radiant_ctrl_fields fields;

	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_ctrl_fields(0u, false, true, &fields));
	zassert_equal(RADIANT_FRAME_OK,
		      radiant_frame_make(&in, &cfg_slave.id, &fields, peer_payload,
				     RADIANT_TRANSFER_PKT_BYTES));
	zassert_equal(RADIANT_CTRL_BURST_OPEN_SEQ0, in.ctrl_byte);

	zassert_equal(RADIANT_TRANSFER_ENOTSUP,
		      radiant_transfer_on_data(&xfer, &in, radiant_radio_now() + 10000u));
	zassert_equal(1u, radiant_transfer_stats(&xfer)->unackable_openers);
	zassert_equal(RADIANT_TRANSFER_STATE_IDLE, radiant_transfer_state(&xfer));
	/* Tick first: posting is not committing, so asserting arms_tx == 0
	 * without giving the scheduler a chance to arm would pass even if the
	 * engine HAD posted a reply. */
	commit();
	zassert_equal(0u, fake_radio_stats()->arms_tx,
		      "nothing may go on the air for an unmeasured reply");

	/* And 0xAA, the acknowledged-data form of the same thing. */
	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_ctrl_fields(0u, true, true, &fields));
	zassert_equal(RADIANT_FRAME_OK,
		      radiant_frame_make(&in, &cfg_slave.id, &fields, peer_payload,
				     RADIANT_TRANSFER_PKT_BYTES));
	zassert_equal(RADIANT_CTRL_ACK_DATA_OPEN, in.ctrl_byte);
	zassert_equal(RADIANT_TRANSFER_ENOTSUP,
		      radiant_transfer_on_data(&xfer, &in, radiant_radio_now() + 10000u));
	zassert_equal(2u, radiant_transfer_stats(&xfer)->unackable_openers);

	end_of_test();
}

/* Whether an acknowledgement may carry something other than the broadcast
 * buffer is untested item 4, so a channel with nothing to broadcast is not
 * answered with a substitute. */
ZTEST(transfer, test_a_channel_with_no_broadcast_buffer_is_not_answered)
{
	struct radiant_frame in;
	struct radiant_ctrl_fields fields;

	bcast_available = false;

	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_ctrl_fields(0u, false, false, &fields));
	zassert_equal(RADIANT_FRAME_OK,
		      radiant_frame_make(&in, &cfg_slave.id, &fields, peer_payload,
				     RADIANT_TRANSFER_PKT_BYTES));

	zassert_equal(RADIANT_TRANSFER_ESTATE,
		      radiant_transfer_on_data(&xfer, &in, radiant_radio_now() + 10000u));
	zassert_equal(1u, radiant_transfer_stats(&xfer)->no_broadcast);
	commit();
	zassert_equal(0u, fake_radio_stats()->arms_tx);

	end_of_test();
}

/* The receiver's retransmission limit: run 0 caught a master resending an
 * identical ack frame 20 times at 3143 us before abandoning (21 attempts,
 * measured n=2 on one stack, hence the named constant). */
ZTEST(transfer, test_the_acknowledgement_retransmission_limit)
{
	struct radiant_frame in;
	struct radiant_ctrl_fields fields;
	const struct fake_radio_arm *arm;
	uint32_t i;

	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_ctrl_fields(0u, false, false, &fields));
	zassert_equal(RADIANT_FRAME_OK,
		      radiant_frame_make(&in, &cfg_slave.id, &fields, peer_payload,
				     RADIANT_TRANSFER_PKT_BYTES));
	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_on_data(&xfer, &in, radiant_radio_now() + 10000u));
	commit();

	arm = last_arm_of(FAKE_RADIO_ARM_TX);
	zassert_not_null(arm);
	fake_radio_advance_to(arm->t_sync_at + 1u);

	/* Attempt 1 has been made; twenty repeats remain. */
	for (i = 1u; i < RADIANT_TRANSFER_REPLY_ATTEMPTS_MAX; i++) {
		radiant_time_t at = radiant_radio_now() +
				(radiant_time_t)RADIANT_TRANSFER_REPLY_RETRY_US;

		zassert_equal(RADIANT_TRANSFER_OK,
			      radiant_transfer_reply_retransmit(&xfer, at),
			      "repeat %u was refused", (unsigned)i);
		commit();
		arm = last_arm_of(FAKE_RADIO_ARM_TX);
		zassert_not_null(arm);
		zassert_equal(RADIANT_CTRL_ACK_SEQ1, arm->body[BODY_CTRL],
			      "a repeat must be the identical frame");
		fake_radio_advance_to(arm->t_sync_at + 1u);
	}

	zassert_equal(RADIANT_TRANSFER_ESTATE,
		      radiant_transfer_reply_retransmit(&xfer,
						    radiant_radio_now() + 5000u),
		      "the 22nd attempt must be refused; 21 is what was measured");
	zassert_equal(RADIANT_TRANSFER_REPLY_ATTEMPTS_MAX - 1u,
		      radiant_transfer_stats(&xfer)->ack_retransmits);

	end_of_test();
}

/* A saved acknowledgement goes stale; the attempt counter alone can't say so.
 * TX_REPLY returns to IDLE without going through finish(), so the saved frame
 * stays loaded for the channel's life, and without a staleness check
 * radiant_transfer_reply_retransmit() would authorise repeating a
 * well-formed but hour-old acknowledgement just as readily as a fresh one. */
ZTEST(transfer, test_an_ancient_acknowledgement_is_not_retransmitted)
{
	struct radiant_frame in;
	struct radiant_ctrl_fields fields;
	const struct fake_radio_arm *arm;

	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_ctrl_fields(0u, false, false, &fields));
	zassert_equal(RADIANT_FRAME_OK,
		      radiant_frame_make(&in, &cfg_slave.id, &fields, peer_payload,
				     RADIANT_TRANSFER_PKT_BYTES));
	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_on_data(&xfer, &in, radiant_radio_now() + 10000u));
	commit();

	arm = last_arm_of(FAKE_RADIO_ARM_TX);
	zassert_not_null(arm);
	fake_radio_advance_to(arm->t_sync_at + 1u);

	/* One retry interval later is the measured cadence and must be allowed,
	 * so the staleness check cannot be mistaken for a stricter limit. */
	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_reply_retransmit(
			      &xfer, radiant_radio_now() +
					     (radiant_time_t)RADIANT_TRANSFER_REPLY_RETRY_US));
	commit();
	arm = last_arm_of(FAKE_RADIO_ARM_TX);
	zassert_not_null(arm);
	fake_radio_advance_to(arm->t_sync_at + 1u);

	zassert_equal(RADIANT_TRANSFER_ESTATE,
		      radiant_transfer_reply_retransmit(
			      &xfer, radiant_radio_now() +
					     (radiant_time_t)RADIANT_TRANSFER_REPLY_VALID_US + 1u),
		      "a repeat a whole validity window late must be refused");
	zassert_equal(1u, radiant_transfer_stats(&xfer)->stale_replies);

	/* Dropped, not merely refused: a refusal that left the frame loaded
	 * would be answered by the next caller that asked in time. */
	zassert_equal(RADIANT_TRANSFER_ESTATE,
		      radiant_transfer_reply_retransmit(
			      &xfer, radiant_radio_now() +
					     (radiant_time_t)RADIANT_TRANSFER_REPLY_RETRY_US),
		      "the saved acknowledgement must be gone, not merely late");
	zassert_equal(1u, radiant_transfer_stats(&xfer)->stale_replies,
		      "the second refusal is 'nothing saved', not a second stale reply");

	end_of_test();
}

/* ---------------------------------------------------------------------------
 * Scheduling and capability
 * ---------------------------------------------------------------------------
 */

/* The first packet goes out at the instant the scheduler asked for. A slave
 * answering its master asks for master_t_sync + 2190 us (n=7 at 2186 us, n=12
 * at 2191 us) - a distinct population from the 1.55 ms in-burst turnaround,
 * hence the separate constants. */
ZTEST(transfer, test_the_first_packet_lands_where_the_scheduler_asked)
{
	const struct fake_radio_arm *arm;
	radiant_time_t master_t_sync = radiant_radio_now() + 100000u;
	uint8_t payload[RADIANT_TRANSFER_PKT_BYTES];

	memset(payload, 0xA5u, sizeof(payload));

	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_ack_data(&xfer, payload,
					    RADIANT_TRANSFER_PKT_BYTES,
					    master_t_sync +
					    (radiant_time_t)RADIANT_TRANSFER_SLOT_REPLY_US));
	commit();

	arm = last_arm_of(FAKE_RADIO_ARM_TX);
	zassert_not_null(arm);
	zassert_equal(master_t_sync + (radiant_time_t)RADIANT_TRANSFER_SLOT_REPLY_US,
		      arm->t_sync_at);
	/* In-slot acknowledged data is 0xA2 - one packet, sequence 0, last. */
	zassert_equal(RADIANT_CTRL_BURST_LAST_SEQ0, arm->body[BODY_CTRL]);
	zassert_mem_equal(&arm->body[BODY_D0], payload, RADIANT_TRANSFER_PKT_BYTES);

	/* Copied, not owned: no block pointer ever comes back for acknowledged
	 * data, because antr_acknowledge_message_tx() copies its payload. */
	reset_blocks();
	acks_to_air = 1u;
	drive();

	zassert_equal(1u, n_ev);
	zassert_equal(RADIANT_TRANSFER_EV_TX_COMPLETED, evlog[0].ev);
	zassert_is_null(evlog[0].block,
			"acknowledged data transfers no buffer ownership");
	zassert_equal(1u, evlog[0].packets);
	zassert_equal(0u, radiant_transfer_stats(&xfer)->blocks_released);

	end_of_test();
}

/* A backend too slow to acknowledge is refused at configuration time: the
 * reply window needs 1310 us of arm lead and the reply frame 1560 us. A
 * backend that can't meet the first is loudly RADIANT_TRANSFER_ENOTSUP here
 * rather than "ERG mode does not work" months later. */
ZTEST(transfer, test_a_backend_that_cannot_turn_a_frame_round_is_refused)
{
	struct radiant_transfer slow;
	struct radiant_transfer_cfg cfg;

	make_cfg(&cfg, false, CH_B);

	/* The RAIL preset - 400 us of arm lead, 200 us of turnaround - still
	 * meets it, which is the point of checking rather than assuming. */
	fake_radio_caps_preset_rail();
	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_init(&slow, &cfg));

	fake_radio_caps_mut()->min_arm_lead_us =
		(uint16_t)(RADIANT_TRANSFER_REPLY_US - RADIANT_TRANSFER_ACK_GUARD_US + 1u);
	zassert_equal(RADIANT_TRANSFER_ENOTSUP, radiant_transfer_init(&slow, &cfg));

	fake_radio_caps_preset_nrf();
	fake_radio_caps_mut()->rx_to_tx_us = (uint16_t)(RADIANT_TRANSFER_REPLY_US + 1u);
	zassert_equal(RADIANT_TRANSFER_ENOTSUP, radiant_transfer_init(&slow, &cfg));

	fake_radio_caps_preset_nrf();
	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_init(&slow, &cfg));

	end_of_test();
}

/* A master's transfer opens the slot on packet 0 and not afterwards. The
 * acknowledgement of a slot opener is unmeasured, so this asserts the encoding
 * only - it does not drive an exchange, because there would be no measured
 * reply to feed it. */
ZTEST(transfer, test_a_master_opens_the_slot_on_the_first_packet_only)
{
	struct radiant_transfer master;
	struct radiant_transfer_cfg cfg;
	const struct fake_radio_arm *arm;
	uint8_t payload[RADIANT_TRANSFER_PKT_BYTES];

	make_cfg(&cfg, true, CH_B);
	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_init(&master, &cfg));
	engine_b = &master;

	memset(payload, 0x5Au, sizeof(payload));
	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_ack_data(&master, payload,
					    RADIANT_TRANSFER_PKT_BYTES,
					    RADIANT_TIME_NEVER));
	commit();

	arm = last_arm_of(FAKE_RADIO_ARM_TX);
	zassert_not_null(arm);
	zassert_equal(RADIANT_CTRL_ACK_DATA_OPEN, arm->body[BODY_CTRL],
		      "a master's acknowledged data is 0xAA, not 0xA2");

	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_abort(&master));
	zassert_true(radiant_transfer_is_idle(&master));
	/* The engine on the other channel was not disturbed. Now asserted
	 * directly (channel 0 neither acted on channel 1's transfer nor
	 * released a block it never held) rather than via late_events, since the
	 * engine no longer has an op id to filter by. */
	zassert_true(radiant_transfer_is_idle(&xfer),
		     "channel 0's engine acted on channel 1's transfer");
	zassert_equal(0u, radiant_transfer_stats(&xfer)->blocks_released);
	zassert_equal(0u, radiant_transfer_stats(&xfer)->packets_sent,
		      "channel 0's engine transmitted for channel 1's transfer");
	/* Exactly one event: the master's own abort. Both engines share `ops`,
	 * so n_ev counts the pair - hence asserting what the event was, not
	 * just its absence. */
	zassert_equal(1u, n_ev, "expected the master's abort alone, got %u events",
		      (unsigned)n_ev);
	zassert_equal(RADIANT_TRANSFER_EV_TX_FAILED, evlog[0].ev);
	zassert_equal(RADIANT_TRANSFER_FAIL_ABORTED, evlog[0].fail);
	zassert_is_null(evlog[0].block,
			"acknowledged data transfers no buffer ownership");

	engine_b = NULL;
	end_of_test();
}
