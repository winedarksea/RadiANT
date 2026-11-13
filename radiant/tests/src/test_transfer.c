/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work, written against radiant_transfer.h,
 * radiant_frame.h, fake_radio.h, and the measurements in
 * docs/spike-b-results.md - every control byte, microsecond, and packet
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
#include <radiant/radiant_transfer.h>

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
	uint8_t                *staged_block;
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
		evlog[n_ev].staged_block = r->staged_block;
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

/* The inbound-burst ends: rx_failed is the owner's EVENT_TRANSFER_RX_FAILED,
 * inbound_ended its cue to post tracked work again. */
static uint32_t n_rx_failed;
static uint32_t n_inbound_ended;
static enum radiant_transfer_state state_at_inbound_ended;

static void on_rx_failed(void *ctx)
{
	ARG_UNUSED(ctx);
	n_rx_failed++;
}

static void on_inbound_ended(void *ctx)
{
	ARG_UNUSED(ctx);
	n_inbound_ended++;
	state_at_inbound_ended = radiant_transfer_state(&xfer);
}

static const struct radiant_transfer_ops ops = {
	.done = on_done,
	.rx_data = on_rx_data,
	.broadcast_payload = on_broadcast_payload,
	.rx_failed = on_rx_failed,
	.inbound_ended = on_inbound_ended,
};

/* ---------------------------------------------------------------------------
 * Routing radio events, exactly as radiant_api.c does
 *
 * The engine posts to radiant_sched.c (the only caller of radiant_radio_tx/rx
 * in radiant), so the scheduler owns the HAL's one global callback pair
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
	case RADIANT_TRANSFER_STATE_WAIT_ACK:
	case RADIANT_TRANSFER_STATE_WAIT_NEXT: {
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
	 * the order radiant/tests/src/test_sched.c uses and radiant_api.c's
	 * bring-up follows. */
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_sched_init(&sched_cbs, NULL));
	zassert_equal(RADIANT_RADIO_OK_RC,
		      radiant_radio_init(radiant_sched_radio_cbs(), NULL));
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_enable());

	memset(evlog, 0, sizeof(evlog));
	n_ev = 0u;
	n_rx_data = 0u;
	n_rx_failed = 0u;
	n_inbound_ended = 0u;
	state_at_inbound_ended = RADIANT_TRANSFER_STATE_ABORTING;
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
#define BLOCKS_MAX 8

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

/*
 * THE CLOSED LOOP, and the one this suite did not have: everything this engine
 * can PUT ON THE AIR, the same engine must be able to ANSWER. Both halves are
 * ours, so a data byte the acknowledger has no reply for is not a coverage gap,
 * it is a transfer that can never complete.
 *
 * It could never complete, and did not: with cfg.slot_opener set (which
 * ant_radio_radiant.c does for every master - a trainer, a treadmill, any
 * dongle channel opened as a master) packet 0 of an acknowledged transfer is
 * 0xAA, radiant_ctrl_reply_for(0xAA) returned 0, radiant_transfer_on_data()
 * refused with ENOTSUP, and the originator sat out its reply window and
 * reported FAIL_NO_ACK. Nothing in the tree failed; the transfer just never
 * worked, on hardware, for as long as the code existed.
 *
 * Written as an exhaustive sweep of the originator's own input space rather
 * than as four literal pairs, so a future encoding change cannot add a
 * seventh reachable byte without this failing.
 */
ZTEST(transfer, test_every_control_byte_we_can_originate_can_be_acknowledged)
{
	static const bool bools[] = { false, true };
	size_t   si, li;
	uint32_t index;
	unsigned seen = 0u;

	for (si = 0u; si < ARRAY_SIZE(bools); si++) {
		for (li = 0u; li < ARRAY_SIZE(bools); li++) {
			/* 0..3 covers both sequence phases twice; the slot bit
			 * only ever appears at index 0, so this reaches every
			 * distinct byte the encoder can produce. */
			for (index = 0u; index < 4u; index++) {
				uint8_t ctrl = 0u;
				uint8_t reply;

				zassert_equal(RADIANT_TRANSFER_OK,
					      radiant_transfer_ctrl(index, bools[li],
								bools[si], &ctrl),
					      "opener=%d last=%d index=%u would not encode",
					      (int)bools[si], (int)bools[li],
					      (unsigned)index);

				reply = radiant_transfer_reply_ctrl(ctrl);
				zassert_not_equal(0u, reply,
						  "0x%02X (opener=%d last=%d index=%u) "
						  "is originated but cannot be "
						  "acknowledged - the transfer can "
						  "never complete",
						  ctrl, (int)bools[si],
						  (int)bools[li], (unsigned)index);

				/* And the originator must accept what the
				 * acknowledger would send back, or the ack
				 * arrives and is thrown away as FAIL_BAD_ACK. */
				zassert_true(radiant_transfer_ack_matches(ctrl, reply),
					     "0x%02X -> 0x%02X is generated by the "
					     "acknowledger and rejected by the "
					     "originator",
					     ctrl, reply);

				/* Whatever goes out has to be a byte measured on
				 * the air, both directions. */
				zassert_true(radiant_ctrl_observed(ctrl), NULL);
				zassert_true(radiant_ctrl_observed(reply), NULL);
				seen++;
			}
		}
	}

	zassert_equal(16u, seen, NULL);
	end_of_test();
}

/*
 * The inference itself, pinned: 0x8A -> 0xD2 and 0xAA -> 0xF2, bit 3 CLEAR.
 * Separate from the sweep above so that if a capture ever settles bit 3 the
 * other way, exactly one test fails and it is the one holding the claim.
 */
ZTEST(transfer, test_a_slot_openers_acknowledgement_clears_bit_3)
{
	zassert_equal(RADIANT_CTRL_ACK_SEQ1,
		      radiant_transfer_reply_ctrl(RADIANT_CTRL_BURST_OPEN_SEQ0),
		      "0x8A must be answered 0xD2");
	zassert_equal(RADIANT_CTRL_ACK_LAST_SEQ1,
		      radiant_transfer_reply_ctrl(RADIANT_CTRL_ACK_DATA_OPEN),
		      "0xAA must be answered 0xF2");

	/* The same replies as the in-slot twins - that IS the inference. */
	zassert_equal(radiant_transfer_reply_ctrl(RADIANT_CTRL_BURST_SEQ0),
		      radiant_transfer_reply_ctrl(RADIANT_CTRL_BURST_OPEN_SEQ0), NULL);
	zassert_equal(radiant_transfer_reply_ctrl(RADIANT_CTRL_BURST_LAST_SEQ0),
		      radiant_transfer_reply_ctrl(RADIANT_CTRL_ACK_DATA_OPEN), NULL);

	/* No acknowledgement anywhere carries bit 3. */
	zassert_false(radiant_ctrl_is_slot_opener(
			      radiant_transfer_reply_ctrl(RADIANT_CTRL_ACK_DATA_OPEN)),
		      NULL);

	/* A byte with no mapping still gets none: the inference is two values
	 * wide, not "clear bit 3 and hope". 0x1A is low-bits-legal and was only
	 * ever seen as a CRC-failed 0x0A. */
	zassert_equal(0u, radiant_transfer_reply_ctrl(0x1Au), NULL);
	zassert_equal(0u, radiant_transfer_reply_ctrl(RADIANT_CTRL_BROADCAST),
		      "a broadcast is not acknowledged");

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

	/* A slot opener's acknowledgement is inferred, not measured - bit 3
	 * clear, so the same reply as its in-slot twin. Held on its own in
	 * test_a_slot_openers_acknowledgement_clears_bit_3; asserted here only
	 * as "there IS one", because the four measured pairs above are what
	 * this test is about. */
	zassert_not_equal(0u,
			  radiant_transfer_reply_ctrl(RADIANT_CTRL_BURST_OPEN_SEQ0));
	zassert_not_equal(0u, radiant_transfer_reply_ctrl(RADIANT_CTRL_ACK_DATA_OPEN));
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

	/* And mid-packet with a block already staged, which is the
	 * ANTW_TRANSFER_IN_PROGRESS the bridge already knows how to answer
	 * with. (Since 2026-11-12 the FIRST mid-packet continuation is staged,
	 * not refused - see test_staging_refuses_a_second_block_and_anything_
	 * after_end - so one is staged here before the stray is tried.) */
	queue_block(1u, 8u, RADIANT_TRANSFER_SEG_CONTINUE, 0x60u);
	zassert_true(feed_next_block());
	zassert_equal(RADIANT_TRANSFER_STATE_TX_DATA, radiant_transfer_state(&xfer));
	zassert_true(feed_next_block(), "the first continuation was not staged");
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
	zassert_equal(2u, st->blocks_accepted,
		      "only the good block and the staged one were accepted");
	zassert_equal(2u, st->blocks_released);
	zassert_equal(1u, n_ev);
	zassert_equal_ptr(blocks[0], evlog[0].block,
		      "the released block must be the accepted one, never the "
		      "rejected one");
	zassert_equal_ptr(blocks[1], evlog[0].staged_block);

	end_of_test();
}

/* Every accepted transmit's arm record, oldest first. */
static uint32_t collect_tx_arms(const struct fake_radio_arm **out, uint32_t cap)
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
		out[found++] = a;
	}
	return found;
}

/*
 * Rev 5.1 sec 5.4.3 / 9.5.5.3: a burst packet whose acknowledgement goes
 * missing is retried, up to five times, then EVENT_TRANSFER_TX_FAILED. Six
 * misses of packet 1: the same packet goes out six times with the same control
 * byte, 3110 us apart, the transfer fails once with NO_ACK, and the block in
 * hand comes back exactly once. (Before 2026-11-11 this test pinned "fails at
 * the first miss, never retried".)
 */
ZTEST(transfer, test_six_missed_acknowledgements_fail_once_and_release_the_block)
{
	const struct radiant_transfer_stats *st;
	const struct fake_radio_arm *tx[12];
	uint32_t n_tx;
	uint32_t i;
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
	zassert_equal(RADIANT_TRANSFER_TX_RETRIES_MAX, st->retries);

	/* Packet 0 once, then packet 1 six times: same ctrl, same payload,
	 * one packet time apart. */
	n_tx = collect_tx_arms(tx, ARRAY_SIZE(tx));
	zassert_equal(1u + 1u + RADIANT_TRANSFER_TX_RETRIES_MAX, n_tx,
		      "expected 7 transmits, got %u", (unsigned)n_tx);
	for (i = 2u; i < n_tx; i++) {
		zassert_equal(tx[1]->body[BODY_CTRL], tx[i]->body[BODY_CTRL],
			      "retry %u moved the control byte", (unsigned)(i - 1u));
		zassert_mem_equal(&tx[1]->body[BODY_D0], &tx[i]->body[BODY_D0],
				  RADIANT_TRANSFER_PKT_BYTES);
		zassert_equal(tx[i - 1u]->t_sync_at +
				      (radiant_time_t)RADIANT_TRANSFER_PACKET_US,
			      tx[i]->t_sync_at,
			      "retry %u is not one packet time after the attempt "
			      "it repeats", (unsigned)(i - 1u));
	}

	/* And nothing after the failure. */
	arms_before = fake_radio_arm_count();
	fake_radio_advance(50000u);
	zassert_equal(arms_before, fake_radio_arm_count(),
		      "the sender kept transmitting after TX_FAILED");

	end_of_test();
}

/* One missed acknowledgement: the retry goes out with the same control byte
 * 3110 us after the miss, it is acknowledged, and the transfer completes. */
ZTEST(transfer, test_one_missed_acknowledgement_is_retried_and_the_transfer_completes)
{
	const struct fake_radio_arm *tx[8];
	const struct fake_radio_arm *rx;
	uint32_t n_tx;

	reset_blocks();
	queue_block(0u, 16u, RADIANT_TRANSFER_SEG_START | RADIANT_TRANSFER_SEG_END,
		    0x30u);
	zassert_true(feed_next_block());

	/* Packet 0 acknowledged. */
	run_one_packet_exchange();
	/* Packet 1: transmit, then let its window close empty. */
	zassert_true(fake_radio_step());
	zassert_equal(RADIANT_TRANSFER_STATE_WAIT_ACK, radiant_transfer_state(&xfer));
	zassert_true(fake_radio_step(), "the empty window never closed");
	zassert_equal(RADIANT_TRANSFER_STATE_TX_DATA, radiant_transfer_state(&xfer),
		      "a missed acknowledgement on packet 1 was not retried");
	zassert_equal(0u, n_ev, "the transfer ended at the first miss");

	/* The retry transmits and is acknowledged. */
	zassert_true(fake_radio_step());
	rx = last_arm_of(FAKE_RADIO_ARM_RX);
	zassert_not_null(rx);
	air_the_ack();
	zassert_true(fake_radio_step());

	zassert_equal(1u, n_ev);
	zassert_equal(RADIANT_TRANSFER_EV_TX_COMPLETED, evlog[0].ev);
	zassert_equal(2u, evlog[0].packets);
	zassert_equal(1u, radiant_transfer_stats(&xfer)->retries);

	n_tx = collect_tx_arms(tx, ARRAY_SIZE(tx));
	zassert_equal(3u, n_tx);
	zassert_equal(tx[1]->body[BODY_CTRL], tx[2]->body[BODY_CTRL],
		      "the retry moved the sequence bit");
	zassert_equal(tx[1]->t_sync_at + (radiant_time_t)RADIANT_TRANSFER_PACKET_US,
		      tx[2]->t_sync_at);

	end_of_test();
}

/* The retry is armed from the empty acknowledgement window's terminal, which
 * a backend with a receive close hold delivers late. Here no fresh arm can
 * reach the retry instant from that callback (a 1400 us lead against 1300 us
 * of notice); only the in-grant lead inside the window's reserve can. Before
 * the window reserved air the retry was reported MISSED and the burst failed:
 * the CC26x2 bench's every failure (2026-11-12). */
ZTEST(transfer, test_a_retry_is_armed_inside_the_ack_windows_reserve)
{
	const struct fake_radio_arm *tx[8];
	uint32_t n_tx;

	reset_blocks();
	queue_block(0u, 16u, RADIANT_TRANSFER_SEG_START | RADIANT_TRANSFER_SEG_END,
		    0x31u);
	zassert_true(feed_next_block());
	run_one_packet_exchange();

	fake_radio_caps_mut()->min_arm_lead_us = 1400u;
	fake_radio_caps_mut()->min_arm_lead_in_grant_us = 200u;

	zassert_true(fake_radio_step());
	zassert_equal(RADIANT_TRANSFER_STATE_WAIT_ACK, radiant_transfer_state(&xfer));
	zassert_true(fake_radio_step(), "the empty window never closed");
	zassert_equal(RADIANT_TRANSFER_STATE_TX_DATA, radiant_transfer_state(&xfer),
		      "the retry was not armed");
	zassert_equal(0u, n_ev, "the transfer ended at the first miss");

	zassert_true(fake_radio_step(), "the retry never transmitted");
	air_the_ack();
	zassert_true(fake_radio_step());

	zassert_equal(1u, n_ev);
	zassert_equal(RADIANT_TRANSFER_EV_TX_COMPLETED, evlog[0].ev);
	n_tx = collect_tx_arms(tx, ARRAY_SIZE(tx));
	zassert_equal(3u, n_tx);
	zassert_equal(tx[1]->t_sync_at + (radiant_time_t)RADIANT_TRANSFER_PACKET_US,
		      tx[2]->t_sync_at, "the retry left its instant");

	end_of_test();
}

/* A single-packet burst has no retries (Rev 5.1 sec 5.4.3), and neither does
 * acknowledged data, which is one. */
ZTEST(transfer, test_a_single_packet_transfer_is_never_retried)
{
	uint8_t payload[RADIANT_TRANSFER_PKT_BYTES];
	const struct fake_radio_arm *tx[4];

	reset_blocks();
	queue_block(0u, 8u, RADIANT_TRANSFER_SEG_START | RADIANT_TRANSFER_SEG_END,
		    0x40u);
	acks_to_air = 0u;
	zassert_true(feed_next_block());
	drive();
	zassert_equal(1u, n_ev);
	zassert_equal(RADIANT_TRANSFER_FAIL_NO_ACK, evlog[0].fail);
	zassert_equal(1u, collect_tx_arms(tx, ARRAY_SIZE(tx)),
		      "a one-packet burst was retried");

	memset(payload, 0x5Au, sizeof(payload));
	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_ack_data(&xfer, payload,
					    RADIANT_TRANSFER_PKT_BYTES,
					    RADIANT_TIME_NEVER));
	commit();
	drive();
	zassert_equal(2u, n_ev);
	zassert_equal(RADIANT_TRANSFER_FAIL_NO_ACK, evlog[1].fail);
	zassert_equal(2u, collect_tx_arms(tx, ARRAY_SIZE(tx)),
		      "acknowledged data was retried");
	zassert_equal(0u, radiant_transfer_stats(&xfer)->retries);

	end_of_test();
}

/* A slave's packet 0 is retried on the master's next reply window, which the
 * owner projects (ops->retry_t_sync), not 3110 us after the miss: the master
 * only listens at its own slot + 2190 us. Without the hook it is not retried
 * at all. */
static radiant_time_t retry_slot_at;
static uint32_t       retry_slot_asked;

static radiant_time_t on_retry_t_sync(void *ctx)
{
	ARG_UNUSED(ctx);
	retry_slot_asked++;
	return retry_slot_at;
}

static const struct radiant_transfer_ops ops_with_retry_slot = {
	.done = on_done,
	.rx_data = on_rx_data,
	.broadcast_payload = on_broadcast_payload,
	.retry_t_sync = on_retry_t_sync,
};

ZTEST(transfer, test_a_slaves_packet_0_is_retried_on_the_next_slot)
{
	struct radiant_transfer_cfg c;
	const struct fake_radio_arm *tx[4];
	radiant_time_t first;

	make_cfg(&c, false, CH_XFER);
	c.ops = &ops_with_retry_slot;
	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_init(&xfer, &c));
	retry_slot_asked = 0u;

	reset_blocks();
	queue_block(0u, 16u, RADIANT_TRANSFER_SEG_START | RADIANT_TRANSFER_SEG_END,
		    0x50u);
	zassert_true(feed_next_block());

	zassert_true(fake_radio_step(), "packet 0 never transmitted");
	first = last_arm_of(FAKE_RADIO_ARM_TX)->t_sync_at;
	/* One 4 Hz period later, the master's next reply window. */
	retry_slot_at = first + 249696u;
	zassert_true(fake_radio_step(), "the empty window never closed");
	zassert_equal(1u, retry_slot_asked);
	zassert_equal(RADIANT_TRANSFER_STATE_TX_DATA, radiant_transfer_state(&xfer));

	zassert_true(fake_radio_step());
	zassert_equal(2u, collect_tx_arms(tx, ARRAY_SIZE(tx)));
	zassert_equal(retry_slot_at, tx[1]->t_sync_at,
		      "a slave's packet 0 retry must wait for the master's next "
		      "reply window");
	zassert_equal(tx[0]->body[BODY_CTRL], tx[1]->body[BODY_CTRL]);

	/* Let it run out. The next miss asks for the same instant, now in the
	 * past; however the scheduler refuses it (at arm time, NO_ACK, or as a
	 * missed slot, RADIO), the transfer ends exactly once. */
	acks_to_air = 0u;
	drive();
	zassert_equal(1u, n_ev);
	zassert_equal(RADIANT_TRANSFER_EV_TX_FAILED, evlog[0].ev);
	end_of_test();
}

/*
 * A master's packet 0 (0x8A) is answered on the slave's SLOT-reply turnaround,
 * not the burst one: a Garmin USB-m answered at +2209 and +2218 us
 * (archive/captures/radio/2026-11-11-master-burst-garmin-ack.log). The window
 * used to close at +1810, so no master-originated burst ever completed on air.
 * The next packet is placed from the acknowledgement actually heard.
 */
ZTEST(transfer, test_a_masters_packet_0_is_acknowledged_on_the_slot_turnaround)
{
	struct radiant_transfer_cfg c;
	const struct fake_radio_arm *tx[4];
	const struct fake_radio_arm *rx;
	uint8_t frame[AIR_LEN];
	radiant_time_t t0;

	make_cfg(&c, true, CH_XFER);
	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_init(&xfer, &c));
	reset_blocks();
	queue_block(0u, 16u, RADIANT_TRANSFER_SEG_START | RADIANT_TRANSFER_SEG_END,
		    0x20u);
	zassert_true(feed_next_block());

	zassert_true(fake_radio_step());
	t0 = last_arm_of(FAKE_RADIO_ARM_TX)->t_sync_at;
	zassert_equal(RADIANT_CTRL_BURST_OPEN_SEQ0,
		      last_arm_of(FAKE_RADIO_ARM_TX)->body[BODY_CTRL]);
	rx = last_arm_of(FAKE_RADIO_ARM_RX);
	zassert_true(rx->t_close >= t0 + 2209u,
		     "the window closes at +%u us, before a real slave answers",
		     (unsigned)(rx->t_close - t0));

	(void)build_peer_frame(frame, radiant_ctrl_reply_for(RADIANT_CTRL_BURST_OPEN_SEQ0),
			       peer_payload);
	zassert_equal(RADIANT_RADIO_OK_RC,
		      fake_radio_air_frame(t0 + 2209u, frame, AIR_LEN));
	zassert_true(fake_radio_step());
	zassert_equal(RADIANT_TRANSFER_STATE_TX_DATA, radiant_transfer_state(&xfer),
		      "the +2209 us acknowledgement was not accepted");
	zassert_true(fake_radio_step());
	zassert_equal(2u, collect_tx_arms(tx, ARRAY_SIZE(tx)));
	zassert_equal(t0 + 2209u + (radiant_time_t)RADIANT_TRANSFER_NEXT_PACKET_US,
		      tx[1]->t_sync_at,
		      "packet 1 must follow the acknowledgement heard by 1550 us");

	air_the_ack();
	zassert_true(fake_radio_step());
	zassert_equal(1u, n_ev);
	zassert_equal(RADIANT_TRANSFER_EV_TX_COMPLETED, evlog[0].ev);
	end_of_test();
}

/* A master's packet 0 that hears nothing is retried where its packet 1 would
 * have gone: slot turnaround + 1550 us. Later repeats meet a receiver already
 * in the burst, so they step by the burst packet time. */
ZTEST(transfer, test_a_masters_packet_0_retry_follows_the_slot_turnaround)
{
	struct radiant_transfer_cfg c;
	const struct fake_radio_arm *tx[4];
	const struct fake_radio_arm *rx;

	make_cfg(&c, true, CH_XFER);
	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_init(&xfer, &c));
	reset_blocks();
	queue_block(0u, 16u, RADIANT_TRANSFER_SEG_START | RADIANT_TRANSFER_SEG_END,
		    0x20u);
	zassert_true(feed_next_block());

	zassert_true(fake_radio_step());   /* packet 0 */
	/* The first attempt's window spans the slot turnaround... */
	rx = last_arm_of(FAKE_RADIO_ARM_RX);
	zassert_equal(last_arm_of(FAKE_RADIO_ARM_TX)->t_sync_at +
			      (radiant_time_t)RADIANT_TRANSFER_OPENER_REPLY_US +
			      (radiant_time_t)RADIANT_TRANSFER_ACK_GUARD_US,
		      rx->t_close);
	zassert_true(fake_radio_step());   /* its window, empty */
	zassert_true(fake_radio_step());   /* retry 1 */
	/* ...a repeat's is the burst one: every repeated 0x8A on air was
	 * answered at +1534..+1584 (2026-11-11 A/B, A1 and A2). */
	rx = last_arm_of(FAKE_RADIO_ARM_RX);
	zassert_equal(last_arm_of(FAKE_RADIO_ARM_TX)->t_sync_at +
			      (radiant_time_t)RADIANT_TRANSFER_REPLY_US +
			      (radiant_time_t)RADIANT_TRANSFER_ACK_GUARD_US,
		      rx->t_close,
		      "a repeated opener still holds the slot-turnaround window");
	zassert_true(fake_radio_step());   /* its window, empty */
	zassert_true(fake_radio_step());   /* retry 2 */
	zassert_equal(3u, collect_tx_arms(tx, ARRAY_SIZE(tx)));
	/* And the air held after each transmit follows its window. */
	zassert_equal((uint32_t)(RADIANT_TRANSFER_OPENER_REPLY_US -
				 RADIANT_TRANSFER_REPLY_US),
		      (uint32_t)(tx[0]->follow_on_us - tx[1]->follow_on_us),
		      "first attempt reserves %u us, the retry %u us",
		      (unsigned)tx[0]->follow_on_us, (unsigned)tx[1]->follow_on_us);
	zassert_equal(tx[1]->follow_on_us, tx[2]->follow_on_us);
	zassert_equal(tx[0]->t_sync_at +
			      (radiant_time_t)RADIANT_TRANSFER_OPENER_REPLY_US +
			      (radiant_time_t)RADIANT_TRANSFER_NEXT_PACKET_US,
		      tx[1]->t_sync_at);
	zassert_equal(tx[1]->t_sync_at + (radiant_time_t)RADIANT_TRANSFER_PACKET_US,
		      tx[2]->t_sync_at);
	zassert_equal(tx[0]->body[BODY_CTRL], tx[2]->body[BODY_CTRL]);

	acks_to_air = 0u;
	drive();
	zassert_equal(1u, n_ev);
	zassert_equal(RADIANT_TRANSFER_FAIL_NO_ACK, evlog[0].fail);
	end_of_test();
}

/* A retried opener answered on the burst turnaround, as the Garmin answers
 * one, completes, and packet 1 follows that acknowledgement by 1550 us. */
ZTEST(transfer, test_a_masters_retried_packet_0_is_acknowledged_on_the_burst_turnaround)
{
	struct radiant_transfer_cfg c;
	const struct fake_radio_arm *tx[4];
	const struct fake_radio_arm *rx;

	make_cfg(&c, true, CH_XFER);
	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_init(&xfer, &c));
	reset_blocks();
	queue_block(0u, 16u, RADIANT_TRANSFER_SEG_START | RADIANT_TRANSFER_SEG_END,
		    0x20u);
	zassert_true(feed_next_block());

	zassert_true(fake_radio_step());   /* packet 0 */
	zassert_true(fake_radio_step());   /* its window, empty */
	zassert_true(fake_radio_step());   /* retry 1 */
	rx = last_arm_of(FAKE_RADIO_ARM_RX);
	air_the_ack();                      /* at t_open + guard = +1560 */
	zassert_true(fake_radio_step());
	zassert_equal(RADIANT_TRANSFER_STATE_TX_DATA, radiant_transfer_state(&xfer),
		      "the retried opener's acknowledgement was not accepted");
	zassert_true(fake_radio_step());   /* packet 1 */
	zassert_equal(3u, collect_tx_arms(tx, ARRAY_SIZE(tx)));
	zassert_equal(rx->t_open + (radiant_time_t)RADIANT_TRANSFER_ACK_GUARD_US +
			      (radiant_time_t)RADIANT_TRANSFER_NEXT_PACKET_US,
		      tx[2]->t_sync_at);

	air_the_ack();
	zassert_true(fake_radio_step());
	zassert_equal(1u, n_ev);
	zassert_equal(RADIANT_TRANSFER_EV_TX_COMPLETED, evlog[0].ev);
	zassert_equal(1u, radiant_transfer_stats(&xfer)->retries);
	end_of_test();
}

ZTEST(transfer, test_a_slaves_packet_0_is_not_retried_without_a_slot_to_aim_at)
{
	const struct fake_radio_arm *tx[4];

	/* The default ops have no retry_t_sync. */
	reset_blocks();
	queue_block(0u, 16u, RADIANT_TRANSFER_SEG_START | RADIANT_TRANSFER_SEG_END,
		    0x50u);
	acks_to_air = 0u;
	zassert_true(feed_next_block());
	drive();
	zassert_equal(1u, n_ev);
	zassert_equal(RADIANT_TRANSFER_FAIL_NO_ACK, evlog[0].fail);
	zassert_equal(1u, collect_tx_arms(tx, ARRAY_SIZE(tx)));

	end_of_test();
}

/* ---------------------------------------------------------------------------
 * Stale acknowledgements: the receiver repeating its answer to the previous
 * packet because it missed this one. Captured 2026-11-11 from a Garmin USB-m:
 * 0x82 answered by 0xC2, 0x92 by 0xD2 (archive/benchmarks/
 * 2026-11-11-burst-retry-ab/README.md, "A2's two failures").
 * ---------------------------------------------------------------------------
 */

/* The stale answer to data byte `d`: a non-final acknowledgement whose
 * sequence bit still names `d`'s packet. */
static uint8_t stale_ack_for(uint8_t d)
{
	struct radiant_ctrl_fields f;

	zassert_equal(0, radiant_ctrl_decode(d, &f));
	f.ack = true;
	f.last = false;
	f.slot_opener = false;
	return radiant_ctrl_encode(&f);
}

/* Air `ctrl` into the reply window that is open now. */
static void air_reply_ctrl(uint8_t ctrl)
{
	const struct fake_radio_arm *rx = last_arm_of(FAKE_RADIO_ARM_RX);
	uint8_t frame[AIR_LEN];

	zassert_not_null(rx, "no reply window was opened");
	(void)build_peer_frame(frame, ctrl, peer_payload);
	zassert_equal(RADIANT_RADIO_OK_RC,
		      fake_radio_air_frame(rx->t_open +
					   (radiant_time_t)RADIANT_TRANSFER_ACK_GUARD_US,
					   frame, AIR_LEN));
}

ZTEST(transfer, test_the_stale_acknowledgement_predicate)
{
	/* The two captured cases. */
	zassert_true(radiant_transfer_ack_is_stale(0x82u, 0xC2u));
	zassert_true(radiant_transfer_ack_is_stale(0x92u, 0xD2u));
	/* A last packet answered with the previous, non-final ack. */
	zassert_true(radiant_transfer_ack_is_stale(0xA2u, 0xC2u));
	zassert_true(radiant_transfer_ack_is_stale(0xB2u, 0xD2u));

	/* The real acknowledgements are not stale, and nothing is both. */
	zassert_false(radiant_transfer_ack_is_stale(0x82u, 0xD2u));
	zassert_false(radiant_transfer_ack_is_stale(0x92u, 0xC2u));
	zassert_false(radiant_transfer_ack_is_stale(0xA2u, 0xF2u));
	zassert_false(radiant_transfer_ack_is_stale(0xB2u, 0xE2u));
	/* A FINAL acknowledgement with the same sequence bit is not a repeat
	 * of anything this sender could have been answered for. */
	zassert_false(radiant_transfer_ack_is_stale(0xA2u, 0xE2u));
	zassert_false(radiant_transfer_ack_is_stale(0xB2u, 0xF2u));
	/* Data echoed, a broadcast, an ack answered to an ack. */
	zassert_false(radiant_transfer_ack_is_stale(0x82u, 0x82u));
	zassert_false(radiant_transfer_ack_is_stale(0x82u, RADIANT_CTRL_BROADCAST));
	zassert_false(radiant_transfer_ack_is_stale(0xC2u, 0xC2u));
	/* Illegal low bits on either side. */
	zassert_false(radiant_transfer_ack_is_stale(0x83u, 0xC2u));
	zassert_false(radiant_transfer_ack_is_stale(0x82u, 0xC3u));
}

/* One stale acknowledgement: the packet goes out again, same control byte, one
 * packet time later, and the transfer completes. */
ZTEST(transfer, test_a_stale_acknowledgement_is_retried_and_the_transfer_completes)
{
	const struct fake_radio_arm *tx[8];
	const struct radiant_transfer_stats *st;
	uint32_t n_tx;

	reset_blocks();
	queue_block(0u, 16u, RADIANT_TRANSFER_SEG_START | RADIANT_TRANSFER_SEG_END,
		    0x30u);
	zassert_true(feed_next_block());

	run_one_packet_exchange();            /* packet 0 acknowledged */
	zassert_true(fake_radio_step());       /* packet 1 transmits */
	zassert_equal(RADIANT_TRANSFER_STATE_WAIT_ACK, radiant_transfer_state(&xfer));
	air_reply_ctrl(stale_ack_for(last_arm_of(FAKE_RADIO_ARM_TX)->body[BODY_CTRL]));
	zassert_true(fake_radio_step(), "the stale acknowledgement never arrived");
	zassert_equal(RADIANT_TRANSFER_STATE_TX_DATA, radiant_transfer_state(&xfer),
		      "a stale acknowledgement was not retried");
	zassert_equal(0u, n_ev, "the transfer ended on a stale acknowledgement");

	zassert_true(fake_radio_step());       /* the retry transmits */
	air_the_ack();
	zassert_true(fake_radio_step());

	zassert_equal(1u, n_ev);
	zassert_equal(RADIANT_TRANSFER_EV_TX_COMPLETED, evlog[0].ev);
	zassert_equal(2u, evlog[0].packets);
	st = radiant_transfer_stats(&xfer);
	zassert_equal(1u, st->stale_acks);
	zassert_equal(1u, st->retries);

	n_tx = collect_tx_arms(tx, ARRAY_SIZE(tx));
	zassert_equal(3u, n_tx);
	zassert_equal(tx[1]->body[BODY_CTRL], tx[2]->body[BODY_CTRL],
		      "the retry moved the sequence bit");
	zassert_equal(tx[1]->t_sync_at + (radiant_time_t)RADIANT_TRANSFER_PACKET_US,
		      tx[2]->t_sync_at);

	end_of_test();
}

/* Stale every time: the first attempt and five retries, then one TX_FAILED as
 * BAD_ACK (the cause the last answer gave), and the block back exactly once. */
ZTEST(transfer, test_six_stale_acknowledgements_fail_once_as_bad_ack)
{
	const struct radiant_transfer_stats *st;
	const struct fake_radio_arm *tx[12];
	uint32_t i;
	uint32_t arms_before;

	reset_blocks();
	queue_block(0u, 16u, RADIANT_TRANSFER_SEG_START | RADIANT_TRANSFER_SEG_END,
		    0x30u);
	zassert_true(feed_next_block());
	run_one_packet_exchange();

	for (i = 0u; i <= RADIANT_TRANSFER_TX_RETRIES_MAX; i++) {
		zassert_true(fake_radio_step(), "attempt %u never transmitted",
			     (unsigned)i);
		zassert_equal(RADIANT_TRANSFER_STATE_WAIT_ACK,
			      radiant_transfer_state(&xfer));
		air_reply_ctrl(stale_ack_for(
			last_arm_of(FAKE_RADIO_ARM_TX)->body[BODY_CTRL]));
		zassert_true(fake_radio_step());
	}

	zassert_equal(RADIANT_TRANSFER_STATE_IDLE, radiant_transfer_state(&xfer));
	zassert_equal(1u, n_ev);
	zassert_equal(RADIANT_TRANSFER_EV_TX_FAILED, evlog[0].ev);
	zassert_equal(RADIANT_TRANSFER_FAIL_BAD_ACK, evlog[0].fail);
	zassert_equal_ptr(blocks[0], evlog[0].block);

	st = radiant_transfer_stats(&xfer);
	zassert_equal(1u, st->blocks_released);
	zassert_equal(RADIANT_TRANSFER_TX_RETRIES_MAX, st->retries);
	zassert_equal(RADIANT_TRANSFER_TX_RETRIES_MAX + 1u, st->stale_acks);
	zassert_equal(1u + 1u + RADIANT_TRANSFER_TX_RETRIES_MAX,
		      collect_tx_arms(tx, ARRAY_SIZE(tx)));

	arms_before = fake_radio_arm_count();
	fake_radio_advance(50000u);
	zassert_equal(arms_before, fake_radio_arm_count(),
		      "the sender kept transmitting after TX_FAILED");
	end_of_test();
}

/* A transfer that may not retry - a one-packet burst, acknowledged data - ends
 * on a stale answer as BAD_ACK, with nothing sent again. */
ZTEST(transfer, test_a_stale_acknowledgement_of_a_single_packet_is_bad_ack)
{
	const struct radiant_transfer_stats *st;
	uint8_t payload[RADIANT_TRANSFER_PKT_BYTES];
	const struct fake_radio_arm *tx[4];

	reset_blocks();
	queue_block(0u, 8u, RADIANT_TRANSFER_SEG_START | RADIANT_TRANSFER_SEG_END,
		    0x40u);
	zassert_true(feed_next_block());
	zassert_true(fake_radio_step());
	air_reply_ctrl(stale_ack_for(last_arm_of(FAKE_RADIO_ARM_TX)->body[BODY_CTRL]));
	zassert_true(fake_radio_step());
	zassert_equal(1u, n_ev);
	zassert_equal(RADIANT_TRANSFER_FAIL_BAD_ACK, evlog[0].fail);
	zassert_equal(1u, collect_tx_arms(tx, ARRAY_SIZE(tx)),
		      "a one-packet burst was retried");

	memset(payload, 0x5Au, sizeof(payload));
	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_ack_data(&xfer, payload,
					    RADIANT_TRANSFER_PKT_BYTES,
					    RADIANT_TIME_NEVER));
	commit();
	zassert_true(fake_radio_step());
	air_reply_ctrl(stale_ack_for(last_arm_of(FAKE_RADIO_ARM_TX)->body[BODY_CTRL]));
	zassert_true(fake_radio_step());
	zassert_equal(2u, n_ev);
	zassert_equal(RADIANT_TRANSFER_FAIL_BAD_ACK, evlog[1].fail);
	zassert_equal(2u, collect_tx_arms(tx, ARRAY_SIZE(tx)),
		      "acknowledged data was retried");

	st = radiant_transfer_stats(&xfer);
	zassert_equal(0u, st->retries);
	zassert_equal(2u, st->stale_acks);
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
	/* 0x82 is not LAST, so it opened an inbound burst: the window for the
	 * next packet follows the acknowledgement. */
	zassert_equal(RADIANT_TRANSFER_STATE_WAIT_NEXT, radiant_transfer_state(&xfer));
	zassert_equal(1u, radiant_transfer_stats(&xfer)->acks_sent);

	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_abort(&xfer));
	zassert_ok(radiant_sched_cancel(CH_XFER));
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
	zassert_equal(0u, arm->follow_on_us,
		      "an acknowledgement opens no reply window, so it must not "
		      "hold an arbiter's air past its own frame");

	fake_radio_advance_to(arm->t_sync_at + 1u);
	end_of_test();
}

#if defined(CONFIG_RADIANT_ACK_FAULTS)
/* ---------------------------------------------------------------------------
 * The reply_plan seam (CONFIG_RADIANT_ACK_FAULTS), the trainer simulator's
 * lever on the acknowledger. Every other case in this file still runs in the
 * build that has it on, with no op installed, and that is the "a NULL op is
 * the unhooked path" half of the claim.
 * ---------------------------------------------------------------------------
 */

static struct radiant_reply_plan plan_next;
static uint32_t                  n_plan_calls;
static radiant_time_t            plan_t_sync_seen;

static void on_reply_plan(void *ctx, const struct radiant_frame *f,
			  radiant_time_t t_sync, struct radiant_reply_plan *out)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(f);

	n_plan_calls++;
	plan_t_sync_seen = t_sync;
	*out = plan_next;
}

static const struct radiant_transfer_ops ops_planned = {
	.done = on_done,
	.rx_data = on_rx_data,
	.broadcast_payload = on_broadcast_payload,
	.reply_plan = on_reply_plan,
};

static void use_planned_ops(enum radiant_reply_action action, int32_t offset_us)
{
	struct radiant_transfer_cfg c;

	plan_next.action = action;
	plan_next.offset_us = offset_us;
	n_plan_calls = 0u;
	plan_t_sync_seen = 0u;

	make_cfg(&c, false, CH_XFER);
	c.ops = &ops_planned;
	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_init(&xfer, &c));
}

static void make_peer_data(struct radiant_frame *in)
{
	struct radiant_ctrl_fields fields;

	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_ctrl_fields(0u, true, false, &fields));
	zassert_equal(RADIANT_FRAME_OK,
		      radiant_frame_make(in, &cfg_slave.id, &fields, peer_payload,
					 RADIANT_TRANSFER_PKT_BYTES));
}

static uint64_t planned_turnaround(int32_t offset_us)
{
	struct radiant_frame in;
	const struct fake_radio_arm *arm;
	radiant_time_t t_sync = radiant_radio_now() + 10000u;
	uint64_t turnaround;

	use_planned_ops(RADIANT_REPLY_ACK, offset_us);
	make_peer_data(&in);

	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_on_data(&xfer, &in, t_sync));
	zassert_equal(1u, n_plan_calls, "the plan is asked exactly once");
	zassert_equal(t_sync, plan_t_sync_seen, "and is told the data's t_sync");
	commit();

	arm = last_arm_of(FAKE_RADIO_ARM_TX);
	zassert_not_null(arm, "a planned ACK must be armed");
	turnaround = (uint64_t)(arm->t_sync_at - t_sync);
	zassert_equal(RADIANT_CTRL_ACK_LAST_SEQ1, arm->body[BODY_CTRL],
		      "a moved ACK is still the right ACK");
	zassert_equal(1u, n_rx_data, "and the command still goes up");

	fake_radio_advance_to(arm->t_sync_at + 1u);
	zassert_equal(1u, radiant_transfer_stats(&xfer)->acks_sent);
	zassert_equal(0u, radiant_transfer_stats(&xfer)->reply_plan_too_early);
	return turnaround;
}

ZTEST(transfer, test_reply_plan_offset_moves_the_ack_by_exactly_that_much)
{
	zassert_equal((uint64_t)RADIANT_TRANSFER_REPLY_US + 300u,
		      planned_turnaround(300), NULL);
	end_of_test();
}

ZTEST(transfer, test_reply_plan_negative_offset_moves_the_ack_earlier)
{
	zassert_equal((uint64_t)RADIANT_TRANSFER_REPLY_US - 200u,
		      planned_turnaround(-200), NULL);
	end_of_test();
}

ZTEST(transfer, test_reply_plan_ack_with_no_offset_is_the_unhooked_path)
{
	zassert_equal((uint64_t)RADIANT_TRANSFER_REPLY_US, planned_turnaround(0),
		      NULL);
	end_of_test();
}

ZTEST(transfer, test_reply_plan_mute_delivers_the_command_and_arms_nothing)
{
	struct radiant_frame in;

	use_planned_ops(RADIANT_REPLY_MUTE, 0);
	make_peer_data(&in);

	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_on_data(&xfer, &in, radiant_radio_now() + 10000u));
	commit();

	zassert_is_null(last_arm_of(FAKE_RADIO_ARM_TX),
			"MUTE is 'applied, ACK lost': nothing may be armed");
	zassert_equal(RADIANT_TRANSFER_STATE_IDLE, radiant_transfer_state(&xfer));
	zassert_equal(1u, n_rx_data, "the command was applied");
	zassert_true(rx_last, NULL);
	zassert_mem_equal(rx_payload, peer_payload, RADIANT_TRANSFER_PKT_BYTES);
	zassert_equal(0u, radiant_transfer_stats(&xfer)->acks_sent);
	end_of_test();
}

ZTEST(transfer, test_reply_plan_deaf_delivers_nothing)
{
	struct radiant_frame in;

	use_planned_ops(RADIANT_REPLY_DEAF, 0);
	make_peer_data(&in);

	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_on_data(&xfer, &in, radiant_radio_now() + 10000u));
	commit();

	zassert_is_null(last_arm_of(FAKE_RADIO_ARM_TX), NULL);
	zassert_equal(0u, n_rx_data, "DEAF is 'never heard'");
	zassert_equal(RADIANT_TRANSFER_STATE_IDLE, radiant_transfer_state(&xfer));
	end_of_test();
}

ZTEST(transfer, test_reply_plan_too_early_is_counted_never_sent_early)
{
	struct radiant_frame in;
	radiant_time_t t_sync;

	/* A data packet whose unhooked reply instant is NOW: with any arm lead
	 * at all, an offset of -100 us lands where the radio cannot go. */
	fake_radio_advance_to(radiant_radio_now() + 20000u);
	t_sync = radiant_radio_now() - (radiant_time_t)RADIANT_TRANSFER_REPLY_US;

	use_planned_ops(RADIANT_REPLY_ACK, -100);
	make_peer_data(&in);

	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_on_data(&xfer, &in, t_sync));
	commit();

	zassert_is_null(last_arm_of(FAKE_RADIO_ARM_TX),
			"an unreachable instant must not become an early ACK");
	zassert_equal(1u, radiant_transfer_stats(&xfer)->reply_plan_too_early);
	zassert_equal(1u, n_rx_data,
		      "the command was heard; only the ACK could not go out");
	zassert_equal(RADIANT_TRANSFER_STATE_IDLE, radiant_transfer_state(&xfer));
	end_of_test();
}

ZTEST(transfer, test_reply_plan_is_not_asked_about_a_refused_packet)
{
	struct radiant_frame in;

	use_planned_ops(RADIANT_REPLY_MUTE, 0);
	make_peer_data(&in);
	bcast_available = false;

	zassert_equal(RADIANT_TRANSFER_ESTATE,
		      radiant_transfer_on_data(&xfer, &in, radiant_radio_now() + 10000u));
	zassert_equal(0u, n_plan_calls,
		      "a refusal must stay a refusal, not become a planned MUTE");
	zassert_equal(0u, n_rx_data, NULL);
	end_of_test();
}
#endif /* CONFIG_RADIANT_ACK_FAULTS */

/*
 * A SLOT-OPENING DATA PACKET IS ACKNOWLEDGED, and this test is the inverse of
 * the one that stood here until 2026-08-17.
 *
 * The old test asserted ENOTSUP and arms_tx == 0, on the reasoning that bit 3
 * of the reply was unmeasured so refusing kept the gap honest. What it
 * actually pinned was a dead path: a master originates 0xAA (see
 * test_acknowledged_data_is_a_one_packet_burst, and ant_radio_radiant.c sets
 * cfg.slot_opener for every master), so an acknowledger that refuses 0xAA
 * cannot complete a single master-originated acknowledged transfer. On the
 * bench that is exactly what happened, indefinitely, with this suite green.
 *
 * So the assertion is inverted rather than deleted, and it now checks the
 * whole reply: the right control byte, at the measured turnaround, carrying
 * the broadcast buffer.
 */
ZTEST(transfer, test_a_slot_opening_packet_is_acknowledged)
{
	struct radiant_frame in;
	struct radiant_ctrl_fields fields;
	const struct fake_radio_arm *tx;
	radiant_time_t t_data;

	t_data = radiant_radio_now() + 10000u;

	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_ctrl_fields(0u, true, true, &fields));
	zassert_equal(RADIANT_FRAME_OK,
		      radiant_frame_make(&in, &cfg_slave.id, &fields, peer_payload,
				     RADIANT_TRANSFER_PKT_BYTES));
	zassert_equal(RADIANT_CTRL_ACK_DATA_OPEN, in.ctrl_byte,
		      "this test is only meaningful for 0xAA");

	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_on_data(&xfer, &in, t_data),
		      "0xAA must be acknowledged, not refused");
	zassert_equal(0u, radiant_transfer_stats(&xfer)->unackable_openers,
		      "0xAA is no longer an unackable opener");
	zassert_equal(RADIANT_TRANSFER_STATE_TX_REPLY, radiant_transfer_state(&xfer));

	/* Tick first: posting is not committing. */
	commit();
	tx = last_arm_of(FAKE_RADIO_ARM_TX);
	zassert_not_null(tx, "the acknowledgement was never armed");
	zassert_equal(RADIANT_CTRL_ACK_LAST_SEQ1, tx->body[BODY_CTRL],
		      "0xAA must be answered with 0xF2, got 0x%02X",
		      tx->body[BODY_CTRL]);
	zassert_equal(t_data + (radiant_time_t)RADIANT_TRANSFER_REPLY_US, tx->t_sync_at,
		      "the reply must sit on the measured 1560 us turnaround");

	/* And the originator of that 0xAA must accept what we just sent, or the
	 * two halves disagree and the transfer fails as FAIL_BAD_ACK instead. */
	zassert_true(radiant_transfer_ack_matches(in.ctrl_byte, tx->body[BODY_CTRL]),
		     "0x%02X does not accept 0x%02X", in.ctrl_byte,
		     tx->body[BODY_CTRL]);

	/* Let the armed reply actually go out. end_of_test() requires an idle
	 * radio, and unlike the refusal this replaces there IS now a transmit
	 * pending - which is the whole point of the test. */
	fake_radio_advance_to(tx->t_sync_at + 1u);
	zassert_equal(RADIANT_TRANSFER_STATE_IDLE, radiant_transfer_state(&xfer),
		      "TX_REPLY must return to IDLE once the ack is away");
	end_of_test();
}

/*
 * Closing a channel while it is only acknowledging a peer's packet must not
 * tell the host a transfer failed: the host never started one (review
 * 2026-10-31, E2-5.3). radiant_transfer.h: exactly one of TX_COMPLETED or
 * TX_FAILED follows an ACCEPTED CALL - and TX_REPLY is no call of the host's.
 * The spurious TX_FAILED also released the bridge's shared burst block.
 */
ZTEST(transfer, test_aborting_our_own_acknowledgement_raises_no_transfer_event)
{
	struct radiant_frame in;
	struct radiant_ctrl_fields fields;
	radiant_time_t t_data = radiant_radio_now() + 10000u;

	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_ctrl_fields(0u, false, false, &fields));
	zassert_equal(RADIANT_FRAME_OK,
		      radiant_frame_make(&in, &cfg_slave.id, &fields, peer_payload,
					 RADIANT_TRANSFER_PKT_BYTES));
	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_on_data(&xfer, &in, t_data));
	commit();
	zassert_not_null(last_arm_of(FAKE_RADIO_ARM_TX), "setup: reply not armed");
	zassert_equal(RADIANT_TRANSFER_STATE_TX_REPLY, radiant_transfer_state(&xfer));

	/* antr_channel_close()'s order: engine abort, then the slot cancel. */
	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_abort(&xfer));
	zassert_ok(radiant_sched_cancel(CH_XFER));
	fake_radio_advance_to(t_data + RADIANT_TRANSFER_REPLY_US + 1000u);

	zassert_equal(0u, n_ev, "the host was told a transfer it never started failed");
	zassert_equal(0u, radiant_transfer_stats(&xfer)->transfers_failed, NULL);
	/* 0x82 opened an inbound burst, and closing it is the one thing that
	 * is reported: as RX_FAILED, never a transmit event. */
	zassert_equal(1u, n_rx_failed, NULL);
	zassert_equal(RADIANT_TRANSFER_STATE_IDLE, radiant_transfer_state(&xfer));
	end_of_test();
}

/* The non-final opener, 0x8A -> 0xD2, on the same path. Split from the 0xAA
 * case so a mapping that gets `last` wrong fails one test and not both. */
ZTEST(transfer, test_a_non_final_slot_opening_packet_is_acknowledged)
{
	struct radiant_frame in;
	struct radiant_ctrl_fields fields;
	const struct fake_radio_arm *tx;

	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_ctrl_fields(0u, false, true, &fields));
	zassert_equal(RADIANT_FRAME_OK,
		      radiant_frame_make(&in, &cfg_slave.id, &fields, peer_payload,
				     RADIANT_TRANSFER_PKT_BYTES));
	zassert_equal(RADIANT_CTRL_BURST_OPEN_SEQ0, in.ctrl_byte);

	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_on_data(&xfer, &in, radiant_radio_now() + 10000u));
	zassert_equal(0u, radiant_transfer_stats(&xfer)->unackable_openers);

	commit();
	tx = last_arm_of(FAKE_RADIO_ARM_TX);
	zassert_not_null(tx, NULL);
	zassert_equal(RADIANT_CTRL_ACK_SEQ1, tx->body[BODY_CTRL],
		      "0x8A must be answered with 0xD2, got 0x%02X",
		      tx->body[BODY_CTRL]);
	zassert_false(radiant_ctrl_is_last(tx->body[BODY_CTRL]),
		      "bit 5 is echoed: a non-final opener gets a non-final ack");

	fake_radio_advance_to(tx->t_sync_at + 1u);
	/* A non-final packet opened an inbound burst; close it the way
	 * antr_channel_close() does. */
	zassert_equal(RADIANT_TRANSFER_STATE_WAIT_NEXT, radiant_transfer_state(&xfer));
	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_abort(&xfer));
	zassert_ok(radiant_sched_cancel(CH_XFER));
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
		      radiant_transfer_ctrl_fields(0u, true, false, &fields));
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
		zassert_equal(RADIANT_CTRL_ACK_LAST_SEQ1, arm->body[BODY_CTRL],
			      "a repeat must be the identical frame");
		zassert_equal(0u, arm->follow_on_us,
			      "a repeated acknowledgement reserved follow-on air");
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
		      radiant_transfer_ctrl_fields(0u, true, false, &fields));
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
	/* Data expects an acknowledgement, so it carries the air for the
	 * window that hears it. */
	zassert_true(arm->follow_on_us >= RADIANT_TRANSFER_REPLY_US +
					  RADIANT_TRANSFER_ACK_GUARD_US,
		     "originated data reserved %u us, too little for its ACK "
		     "window", (unsigned)arm->follow_on_us);

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

/* ---------------------------------------------------------------------------
 * Pre-staged continuation blocks (2026-11-12)
 *
 * The next burst block may be submitted while the one before it is still on
 * the air. The engine holds it and chains it from the acknowledgement's own
 * callback, so a block boundary never waits for the host's round trip - which
 * on the cc26xx (600 us arm lead) did not fit in the 1550 us gap.
 * ---------------------------------------------------------------------------
 */

/* Like drive(), but the host keeps one block staged at all times: whenever
 * the engine is mid-block with nothing staged, the next block goes in. Returns
 * true if the engine ever had to wait for a block. */
static bool drive_staged(void)
{
	uint32_t acked_tx = 0u;
	bool waited = false;

	for (uint32_t guard = 0u; guard < 400u; guard++) {
		enum radiant_transfer_state st = radiant_transfer_state(&xfer);

		if (st == RADIANT_TRANSFER_STATE_IDLE) {
			return waited;
		}
		if (st == RADIANT_TRANSFER_STATE_WAIT_BLOCK) {
			waited = true;
			if (!feed_next_block()) {
				(void)radiant_transfer_abort(&xfer);
				return waited;
			}
			continue;
		}
		if ((st == RADIANT_TRANSFER_STATE_TX_DATA ||
		     st == RADIANT_TRANSFER_STATE_WAIT_ACK) &&
		    xfer.staged == NULL && next_block < n_blocks) {
			zassert_true(feed_next_block(),
				     "block %u was refused while one was in flight",
				     (unsigned)next_block);
		}
		if (st == RADIANT_TRANSFER_STATE_WAIT_ACK &&
		    fake_radio_stats()->arms_tx != acked_tx) {
			acked_tx = fake_radio_stats()->arms_tx;
			if (acks_to_air > 0u) {
				acks_to_air--;
				air_the_ack();
			}
		}
		if (!fake_radio_step()) {
			return waited;
		}
	}
	zassert_unreachable("the exchange did not converge in 400 steps");
	return waited;
}

/* Eight back-to-back 8-byte blocks, the bench case: every block after the
 * first is staged, every packet leaves on the same grid from the
 * acknowledgement callback, and each block is still released exactly once. */
ZTEST(transfer, test_staged_blocks_chain_at_ack_plus_1550_with_no_host_hop)
{
	const struct radiant_transfer_stats *st;
	radiant_time_t t[8];
	uint32_t n_tx = 0u;

	reset_blocks();
	for (uint32_t i = 0u; i < 8u; i++) {
		queue_block(i, 8u,
			    i == 0u ? RADIANT_TRANSFER_SEG_START
			    : i == 7u ? RADIANT_TRANSFER_SEG_END
				      : RADIANT_TRANSFER_SEG_CONTINUE,
			    (uint8_t)(0x10u * (i + 1u)));
	}
	zassert_true(feed_next_block());
	zassert_false(drive_staged(),
		      "the engine waited in WAIT_BLOCK although the next block "
		      "was always staged");

	zassert_equal(8u, n_ev, "events: %u", (unsigned)n_ev);
	for (uint32_t i = 0u; i < 7u; i++) {
		zassert_equal(RADIANT_TRANSFER_EV_NEXT_BLOCK, evlog[i].ev);
		zassert_equal_ptr(blocks[i], evlog[i].block,
				  "release %u carried the wrong block", i);
		zassert_is_null(evlog[i].staged_block);
	}
	zassert_equal(RADIANT_TRANSFER_EV_TX_COMPLETED, evlog[7].ev);
	zassert_equal_ptr(blocks[7], evlog[7].block);
	zassert_is_null(evlog[7].staged_block);

	st = radiant_transfer_stats(&xfer);
	zassert_equal(8u, st->blocks_accepted);
	zassert_equal(7u, st->blocks_staged);
	zassert_equal(8u, st->blocks_released);
	zassert_equal(8u, st->packets_sent);

	/* Same spacing between every pair of packets: each was armed from its
	 * predecessor's acknowledgement at ack t_sync + NEXT_PACKET_US. */
	for (uint32_t i = 0u; i < fake_radio_arm_count() && n_tx < 8u; i++) {
		const struct fake_radio_arm *a = fake_radio_arm(i);

		if (a != NULL && a->kind == FAKE_RADIO_ARM_TX &&
		    a->rc == RADIANT_RADIO_OK_RC) {
			t[n_tx++] = a->t_sync_at;
		}
	}
	zassert_equal(8u, n_tx);
	for (uint32_t i = 2u; i < 8u; i++) {
		zassert_equal(t[1] - t[0], t[i] - t[i - 1u],
			      "packet %u is %d us after its predecessor, not %d",
			      i, (int)(t[i] - t[i - 1u]), (int)(t[1] - t[0]));
	}
	end_of_test();
}

/* Only one block may be staged, and never behind the END block. */
ZTEST(transfer, test_staging_refuses_a_second_block_and_anything_after_end)
{
	reset_blocks();
	queue_block(0u, 8u, RADIANT_TRANSFER_SEG_START, 0x10u);
	queue_block(1u, 8u, RADIANT_TRANSFER_SEG_CONTINUE, 0x20u);
	queue_block(2u, 8u, RADIANT_TRANSFER_SEG_END, 0x30u);

	zassert_true(feed_next_block());
	zassert_true(feed_next_block(), "the first continuation was not staged");
	zassert_equal(RADIANT_TRANSFER_EBUSY,
		      radiant_transfer_burst_block(&xfer, blocks[2], 8u,
						   RADIANT_TRANSFER_SEG_END,
						   RADIANT_TIME_NEVER),
		      "a second block was staged behind the first");
	(void)radiant_transfer_abort(&xfer);
	while (fake_radio_step()) {
	}

	/* A single START|END block: nothing may follow it. */
	reset_blocks();
	memset(evlog, 0, sizeof(evlog));
	n_ev = 0u;
	queue_block(0u, 8u, RADIANT_TRANSFER_SEG_START | RADIANT_TRANSFER_SEG_END,
		    0x40u);
	queue_block(1u, 8u, RADIANT_TRANSFER_SEG_END, 0x50u);
	zassert_true(feed_next_block());
	zassert_equal(RADIANT_TRANSFER_EBUSY,
		      radiant_transfer_burst_block(&xfer, blocks[1], 8u,
						   RADIANT_TRANSFER_SEG_END,
						   RADIANT_TIME_NEVER),
		      "a block was staged behind the END block");
	(void)radiant_transfer_abort(&xfer);
	while (fake_radio_step()) {
	}
	end_of_test();
}

/* Abort mid-burst with a block staged: ONE terminal, carrying both blocks. */
ZTEST(transfer, test_abort_mid_burst_releases_the_staged_block_too)
{
	const struct radiant_transfer_stats *st;

	reset_blocks();
	queue_block(0u, 8u, RADIANT_TRANSFER_SEG_START, 0x10u);
	queue_block(1u, 8u, RADIANT_TRANSFER_SEG_CONTINUE, 0x20u);
	zassert_true(feed_next_block());
	zassert_true(feed_next_block());

	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_abort(&xfer));
	for (uint32_t guard = 0u; guard < 50u && fake_radio_step(); guard++) {
	}

	zassert_equal(1u, n_ev, "events: %u", (unsigned)n_ev);
	zassert_equal(RADIANT_TRANSFER_EV_TX_FAILED, evlog[0].ev);
	zassert_equal(RADIANT_TRANSFER_FAIL_ABORTED, evlog[0].fail);
	zassert_equal_ptr(blocks[0], evlog[0].block);
	zassert_equal_ptr(blocks[1], evlog[0].staged_block,
			  "the staged block was not handed back");
	st = radiant_transfer_stats(&xfer);
	zassert_equal(st->blocks_accepted, st->blocks_released,
		      "accepted %u, released %u", st->blocks_accepted,
		      st->blocks_released);
	zassert_true(radiant_transfer_is_idle(&xfer));
	end_of_test();
}

/* No acknowledgement at all: exactly one TX_FAILED, NO_ACK, and it releases
 * the staged block as well as the one in flight. */
ZTEST(transfer, test_a_failed_transfer_releases_the_staged_block_once)
{
	const struct radiant_transfer_stats *st;

	reset_blocks();
	queue_block(0u, 8u, RADIANT_TRANSFER_SEG_START, 0x10u);
	queue_block(1u, 8u, RADIANT_TRANSFER_SEG_CONTINUE, 0x20u);
	queue_block(2u, 8u, RADIANT_TRANSFER_SEG_END, 0x30u);
	acks_to_air = 0u;
	zassert_true(feed_next_block());
	(void)drive_staged();

	zassert_equal(1u, n_ev, "events: %u", (unsigned)n_ev);
	zassert_equal(RADIANT_TRANSFER_EV_TX_FAILED, evlog[0].ev);
	zassert_equal(RADIANT_TRANSFER_FAIL_NO_ACK, evlog[0].fail);
	zassert_equal_ptr(blocks[0], evlog[0].block);
	zassert_equal_ptr(blocks[1], evlog[0].staged_block);
	st = radiant_transfer_stats(&xfer);
	zassert_equal(2u, st->blocks_accepted);
	zassert_equal(2u, st->blocks_released);
	zassert_equal(1u, st->transfers_failed);
	end_of_test();
}

/* ---------------------------------------------------------------------------
 * Inbound multi-packet bursts: the acknowledger follows the burst
 *
 * Until 2026-11-12 the engine acknowledged an inbound burst packet and went
 * IDLE, and nothing listened at ack t_sync + 1550 us where the peer's next
 * packet arrives: a burst sent TO RadiANT never got past packet 0 on air.
 * ---------------------------------------------------------------------------
 */

static void inbound_payload(uint32_t i, uint8_t out[RADIANT_TRANSFER_PKT_BYTES])
{
	uint8_t b;

	for (b = 0u; b < RADIANT_TRANSFER_PKT_BYTES; b++) {
		out[b] = (uint8_t)(0x40u + 8u * i + b);
	}
}

/* Packet i of a master's burst (bit 3 on packet 0 only). */
static uint8_t inbound_ctrl(uint32_t i, bool last)
{
	uint8_t c = 0u;

	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_ctrl(i, last, true, &c));
	return c;
}

/* Packet 0 arrives on the owner's tracked window and is handed to
 * radiant_transfer_on_data(), as api_tracked_frame() does. Returns its t_sync. */
static radiant_time_t inbound_open(uint8_t ctrl0)
{
	struct radiant_ctrl_fields fields;
	struct radiant_frame in;
	uint8_t pay[RADIANT_TRANSFER_PKT_BYTES];
	radiant_time_t t0 = radiant_radio_now() + 10000u;

	inbound_payload(0u, pay);
	zassert_equal(RADIANT_FRAME_OK, radiant_ctrl_decode(ctrl0, &fields));
	zassert_equal(RADIANT_FRAME_OK,
		      radiant_frame_make(&in, &cfg_slave.id, &fields, pay,
					 RADIANT_TRANSFER_PKT_BYTES));
	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_on_data(&xfer, &in, t0));
	commit();
	return t0;
}

/* Air a frame with control byte `ctrl` carrying inbound packet i's payload at
 * the centre of the window armed now - ack t_sync + 1550, where the peer's
 * next packet (or its retry) goes. Returns that t_sync. */
static radiant_time_t air_in_next_window(uint8_t ctrl, uint32_t i)
{
	const struct fake_radio_arm *rx = last_arm_of(FAKE_RADIO_ARM_RX);
	uint8_t pay[RADIANT_TRANSFER_PKT_BYTES];
	uint8_t frame[AIR_LEN];
	radiant_time_t t;

	zassert_not_null(rx, "no continuation window was opened");
	inbound_payload(i, pay);
	(void)build_peer_frame(frame, ctrl, pay);
	t = rx->t_open + (radiant_time_t)RADIANT_TRANSFER_ACK_GUARD_US;
	zassert_equal(RADIANT_RADIO_OK_RC, fake_radio_air_frame(t, frame, AIR_LEN));
	return t;
}

/* Run the armed acknowledgement out; returns its t_sync. */
static radiant_time_t send_armed_ack(void)
{
	const struct fake_radio_arm *tx = last_arm_of(FAKE_RADIO_ARM_TX);
	radiant_time_t at;

	zassert_not_null(tx, "no acknowledgement was armed");
	zassert_true(radiant_ctrl_is_ack(tx->body[BODY_CTRL]),
		     "the armed transmit is 0x%02X, not an acknowledgement",
		     tx->body[BODY_CTRL]);
	at = tx->t_sync_at;
	fake_radio_advance_to(at + 1u);
	return at;
}

/* Close an engine left mid-burst the way antr_channel_close() does. */
static void close_like_the_owner(void)
{
	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_abort(&xfer));
	zassert_ok(radiant_sched_cancel(CH_XFER));
}

ZTEST(transfer, test_an_inbound_burst_is_followed_packet_by_packet)
{
	const uint8_t c[3] = { inbound_ctrl(0u, false), inbound_ctrl(1u, false),
			       inbound_ctrl(2u, true) };
	const struct fake_radio_arm *rx = NULL;
	const struct fake_radio_arm *tx;
	uint8_t pay[RADIANT_TRANSFER_PKT_BYTES];
	radiant_time_t t_data = inbound_open(c[0]);
	radiant_time_t ack;
	uint32_t i;

	zassert_equal(RADIANT_CTRL_BURST_OPEN_SEQ0, c[0]);
	for (i = 0u; i < 3u; i++) {
		tx = last_arm_of(FAKE_RADIO_ARM_TX);
		zassert_not_null(tx);
		/* Packet 0 opens the master's slot and is answered on the
		 * slot-reply turnaround; the rest at the burst one. */
		zassert_equal(t_data + (radiant_time_t)(i == 0u ?
					RADIANT_TRANSFER_OPENER_REPLY_US :
					RADIANT_TRANSFER_REPLY_US),
			      tx->t_sync_at, "packet %u: ACK misplaced", i);
		zassert_equal(radiant_ctrl_reply_for(c[i]), tx->body[BODY_CTRL],
			      "packet %u: acknowledged with 0x%02X", i,
			      tx->body[BODY_CTRL]);
		zassert_equal(i + 1u, n_rx_data, "packet %u not delivered once", i);
		inbound_payload(i, pay);
		zassert_mem_equal(rx_payload, pay, RADIANT_TRANSFER_PKT_BYTES,
				  "packet %u delivered out of order", i);
		zassert_equal(i == 2u, rx_last);

		ack = send_armed_ack();
		zassert_equal(RADIANT_TRANSFER_STATE_WAIT_NEXT,
			      radiant_transfer_state(&xfer),
			      "packet %u: no window after its ACK", i);
		rx = last_arm_of(FAKE_RADIO_ARM_RX);
		zassert_not_null(rx);
		zassert_equal(ack + RADIANT_TRANSFER_NEXT_PACKET_US -
				      RADIANT_TRANSFER_ACK_GUARD_US, rx->t_open);
		zassert_equal(ack + RADIANT_TRANSFER_NEXT_PACKET_US +
				      RADIANT_TRANSFER_ACK_GUARD_US, rx->t_close);
		zassert_true(rx->follow_on_us >=
				     RADIANT_TRANSFER_REPLY_US + RADIANT_TRANSFER_ACK_GUARD_US,
			     "the window must reserve the ACK it may need");
		if (i == 2u) {
			break;
		}
		t_data = air_in_next_window(c[i + 1u], i + 1u);
		fake_radio_advance_to(t_data + 100u);
		zassert_equal(RADIANT_TRANSFER_STATE_TX_REPLY,
			      radiant_transfer_state(&xfer));
	}

	/* One window after LAST, for a retry of LAST; it closes empty and the
	 * burst is complete, with no repeat of the final ACK. */
	fake_radio_advance_to(rx->t_close + 5000u);
	zassert_equal(RADIANT_TRANSFER_STATE_IDLE, radiant_transfer_state(&xfer));
	zassert_equal(3u, fake_radio_stats()->arms_tx, "an ACK was repeated after LAST");
	zassert_equal(3u, radiant_transfer_stats(&xfer)->acks_sent);
	zassert_equal(0u, n_rx_failed);
	zassert_equal(1u, n_inbound_ended);
	zassert_equal(RADIANT_TRANSFER_STATE_IDLE, state_at_inbound_ended,
		      "the owner must find the engine idle when told to repost");
	zassert_equal(0u, n_ev, "an inbound burst raised a transmit event");
	end_of_test();
}

/* THE REGRESSION GUARD: standalone acknowledged data - a LAST packet with no
 * burst open, which is how Zwift's FE-C commands arrive - arms nothing after
 * its acknowledgement, exactly as before inbound bursts existed. */
ZTEST(transfer, test_acknowledged_data_arms_no_window_after_its_ack)
{
	(void)inbound_open(RADIANT_CTRL_ACK_DATA_OPEN);
	(void)send_armed_ack();

	zassert_equal(RADIANT_TRANSFER_STATE_IDLE, radiant_transfer_state(&xfer));
	zassert_equal(0u, fake_radio_stats()->arms_rx,
		      "acknowledged data opened a receive window");
	zassert_equal(1u, fake_radio_stats()->arms_tx);
	zassert_equal(0u, n_inbound_ended, "acknowledged data is not a burst");
	zassert_equal(0u, n_rx_failed);
	end_of_test();
}

/* Our ACK was lost and the peer sent the same packet again: same sequence
 * bit. It is answered with the very same ACK and not delivered twice. */
ZTEST(transfer, test_a_duplicate_is_reacknowledged_and_not_delivered)
{
	const struct fake_radio_arm *tx;
	uint8_t first_reply[RADIANT_RADIO_BODY_MAX];
	radiant_time_t t;

	(void)inbound_open(inbound_ctrl(0u, false));
	memcpy(first_reply, last_arm_of(FAKE_RADIO_ARM_TX)->body, sizeof(first_reply));
	(void)send_armed_ack();

	/* The host writes a new broadcast meanwhile: a re-ACK still repeats
	 * the ACK that was lost, byte for byte. */
	bcast_payload[0] ^= 0xFFu;
	t = air_in_next_window(inbound_ctrl(0u, false), 0u);
	fake_radio_advance_to(t + 100u);
	bcast_payload[0] ^= 0xFFu;

	zassert_equal(RADIANT_TRANSFER_STATE_TX_REPLY, radiant_transfer_state(&xfer));
	tx = last_arm_of(FAKE_RADIO_ARM_TX);
	zassert_equal(t + RADIANT_TRANSFER_REPLY_US, tx->t_sync_at);
	zassert_mem_equal(first_reply, tx->body, BODY_LEN, "the re-ACK differs");
	zassert_equal(1u, n_rx_data, "the duplicate was delivered again");
	zassert_equal(1u, radiant_transfer_stats(&xfer)->rx_duplicates);

	(void)send_armed_ack();
	zassert_equal(RADIANT_TRANSFER_STATE_WAIT_NEXT, radiant_transfer_state(&xfer));

	/* And the next packet proper still goes up. */
	t = air_in_next_window(inbound_ctrl(1u, true), 1u);
	fake_radio_advance_to(t + 100u);
	zassert_equal(2u, n_rx_data);
	zassert_true(rx_last);
	(void)send_armed_ack();
	fake_radio_advance_to(radiant_radio_now() + 5000u);
	zassert_equal(RADIANT_TRANSFER_STATE_IDLE, radiant_transfer_state(&xfer));
	zassert_equal(0u, n_rx_failed);
	end_of_test();
}

/* Nothing in the window: the saved ACK goes out again 3143 us after the last
 * one (run 0's measured repeat), and the next window follows it. Twenty
 * repeats, then the burst has failed - exactly one rx_failed. */
ZTEST(transfer, test_empty_windows_repeat_the_ack_then_fail_the_burst_once)
{
	const struct fake_radio_arm *rx;
	const struct fake_radio_arm *tx;
	radiant_time_t prev;
	uint32_t repeats = 0u;
	uint32_t guard;

	(void)inbound_open(inbound_ctrl(0u, false));

	for (guard = 0u; guard < 64u; guard++) {
		prev = send_armed_ack();
		zassert_equal(RADIANT_TRANSFER_STATE_WAIT_NEXT,
			      radiant_transfer_state(&xfer));
		rx = last_arm_of(FAKE_RADIO_ARM_RX);
		zassert_equal(prev + RADIANT_TRANSFER_NEXT_PACKET_US -
				      RADIANT_TRANSFER_ACK_GUARD_US, rx->t_open,
			      "window %u misplaced", repeats);
		fake_radio_advance_to(rx->t_close + 1000u);
		if (radiant_transfer_state(&xfer) == RADIANT_TRANSFER_STATE_IDLE) {
			break;
		}
		zassert_equal(RADIANT_TRANSFER_STATE_TX_REPLY,
			      radiant_transfer_state(&xfer));
		tx = last_arm_of(FAKE_RADIO_ARM_TX);
		zassert_equal(prev + RADIANT_TRANSFER_REPLY_RETRY_US, tx->t_sync_at,
			      "repeat %u not at +3143", repeats);
		zassert_equal(RADIANT_CTRL_ACK_SEQ1, tx->body[BODY_CTRL]);
		repeats++;
	}

	zassert_equal(RADIANT_TRANSFER_REPLY_ATTEMPTS_MAX - 1u, repeats,
		      "%u repeats; the measured limit is 21 attempts", repeats);
	zassert_equal(RADIANT_TRANSFER_REPLY_ATTEMPTS_MAX - 1u,
		      radiant_transfer_stats(&xfer)->ack_retransmits);
	zassert_equal(1u, n_rx_failed);
	zassert_equal(1u, n_inbound_ended);
	zassert_equal(1u, n_rx_data);
	zassert_equal(0u, n_ev);
	end_of_test();
}

/* The peer went back to broadcasting mid-burst: it abandoned the burst. The
 * engine reports it, goes IDLE, and hands the broadcast back unconsumed so the
 * owner handles it as the slot frame it is. */
static bool last_rx_consumed;

static void sched_rx_recording(uint8_t ch, uint8_t filter_index,
			       const struct radiant_rx_event *e, void *user)
{
	ARG_UNUSED(filter_index);
	ARG_UNUSED(user);
	if (ch == CH_XFER) {
		last_rx_consumed = radiant_transfer_on_rx_event(&xfer, e);
	}
}

static const struct radiant_sched_cbs sched_cbs_recording = {
	.rx = sched_rx_recording,
	.tx = sched_tx,
	.done = sched_done,
};

ZTEST(transfer, test_a_broadcast_in_the_window_ends_the_burst_unconsumed)
{
	radiant_time_t t;

	zassert_equal(RADIANT_RADIO_OK_RC, radiant_sched_init(&sched_cbs_recording, NULL));
	last_rx_consumed = true;

	(void)inbound_open(inbound_ctrl(0u, false));
	(void)send_armed_ack();
	t = air_in_next_window(RADIANT_CTRL_BROADCAST, 1u);
	fake_radio_advance_to(t + 2000u);

	zassert_false(last_rx_consumed, "the broadcast was swallowed");
	zassert_equal(RADIANT_TRANSFER_STATE_IDLE, radiant_transfer_state(&xfer));
	zassert_equal(1u, n_rx_failed);
	zassert_equal(1u, n_inbound_ended);
	zassert_equal(1u, n_rx_data, "a broadcast is not a burst packet");
	end_of_test();
}

ZTEST(transfer, test_abort_mid_burst_reports_rx_failed_once)
{
	(void)inbound_open(inbound_ctrl(0u, false));
	(void)send_armed_ack();
	zassert_equal(RADIANT_TRANSFER_STATE_WAIT_NEXT, radiant_transfer_state(&xfer));

	close_like_the_owner();
	fake_radio_advance_to(radiant_radio_now() + 10000u);

	zassert_equal(RADIANT_TRANSFER_STATE_IDLE, radiant_transfer_state(&xfer));
	zassert_equal(1u, n_rx_failed);
	zassert_equal(0u, n_inbound_ended, "abort's caller is tearing down");
	zassert_equal(0u, n_ev, "the host never started a transfer");
	end_of_test();
}

ZTEST(transfer, test_abort_after_last_reports_nothing)
{
	radiant_time_t t;

	(void)inbound_open(inbound_ctrl(0u, false));
	(void)send_armed_ack();
	t = air_in_next_window(inbound_ctrl(1u, true), 1u);
	fake_radio_advance_to(t + 100u);
	(void)send_armed_ack();
	zassert_equal(RADIANT_TRANSFER_STATE_WAIT_NEXT, radiant_transfer_state(&xfer),
		      "setup: the window after LAST");

	close_like_the_owner();
	fake_radio_advance_to(radiant_radio_now() + 10000u);
	zassert_equal(0u, n_rx_failed, "a completed burst is not a failed one");
	zassert_equal(0u, n_ev);
	end_of_test();
}

ZTEST(transfer, test_submit_while_following_a_burst_is_busy)
{
	uint8_t blk[RADIANT_TRANSFER_PKT_BYTES] = { 1u };

	(void)inbound_open(inbound_ctrl(0u, false));
	(void)send_armed_ack();
	zassert_equal(RADIANT_TRANSFER_STATE_WAIT_NEXT, radiant_transfer_state(&xfer));

	zassert_equal(RADIANT_TRANSFER_EBUSY,
		      radiant_transfer_burst_block(&xfer, blk, sizeof(blk),
						   RADIANT_TRANSFER_SEG_START |
							   RADIANT_TRANSFER_SEG_END,
						   RADIANT_TIME_NEVER));
	zassert_equal(RADIANT_TRANSFER_EBUSY,
		      radiant_transfer_ack_data(&xfer, blk, sizeof(blk),
						RADIANT_TIME_NEVER));
	zassert_equal(0u, n_ev, "a refused block raised an event");

	close_like_the_owner();
	end_of_test();
}

/* EVENT_TRANSFER_TX_START's predicate: the first attempt of packet 0 of a
 * multi-packet burst, and nothing else. */
ZTEST(transfer, test_cur_starts_burst_holds_for_one_transmit_per_burst)
{
	uint8_t one[RADIANT_TRANSFER_PKT_BYTES] = { 7u };
	struct radiant_transfer_cfg master;

	/* A two-packet burst: packet 0 yes, packet 1 no. */
	reset_blocks();
	queue_block(0u, 16u, RADIANT_TRANSFER_SEG_START | RADIANT_TRANSFER_SEG_END,
		    0x10u);
	zassert_true(feed_next_block());
	zassert_true(radiant_transfer_cur_starts_burst(&xfer));
	run_one_packet_exchange();
	zassert_equal(RADIANT_TRANSFER_STATE_TX_DATA, radiant_transfer_state(&xfer));
	zassert_false(radiant_transfer_cur_starts_burst(&xfer), "packet 1");
	drive();
	zassert_equal(RADIANT_TRANSFER_EV_TX_COMPLETED, evlog[n_ev - 1u].ev);

	/* A one-packet burst. */
	reset_blocks();
	queue_block(0u, 8u, RADIANT_TRANSFER_SEG_START | RADIANT_TRANSFER_SEG_END,
		    0x20u);
	zassert_true(feed_next_block());
	zassert_false(radiant_transfer_cur_starts_burst(&xfer), "one-packet burst");
	drive();

	/* Acknowledged data. */
	zassert_equal(RADIANT_TRANSFER_OK,
		      radiant_transfer_ack_data(&xfer, one, sizeof(one),
						RADIANT_TIME_NEVER));
	commit();
	zassert_false(radiant_transfer_cur_starts_burst(&xfer), "acknowledged data");
	reset_blocks();
	drive();

	/* A retry of packet 0 (a master's, which is retried on its own). */
	make_cfg(&master, true, CH_XFER);
	zassert_equal(RADIANT_TRANSFER_OK, radiant_transfer_init(&xfer, &master));
	reset_blocks();
	queue_block(0u, 16u, RADIANT_TRANSFER_SEG_START | RADIANT_TRANSFER_SEG_END,
		    0x30u);
	zassert_true(feed_next_block());
	zassert_true(fake_radio_step(), "packet 0 never went out");
	zassert_equal(RADIANT_TRANSFER_STATE_WAIT_ACK, radiant_transfer_state(&xfer));
	zassert_true(fake_radio_step(), "the window never closed");
	zassert_equal(RADIANT_TRANSFER_STATE_TX_DATA, radiant_transfer_state(&xfer),
		      "setup: packet 0 was not retried");
	zassert_false(radiant_transfer_cur_starts_burst(&xfer), "retry of packet 0");
	drive();
	end_of_test();
}
