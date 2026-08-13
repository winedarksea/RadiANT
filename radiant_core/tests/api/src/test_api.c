/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_api.c - ztest coverage for radiant_api.c's composition (previously
 * untested; the module suites below it were). Clean-room; see
 * docs/decisions/0002-clean-room-policy.md.
 *
 * Two constraints:
 *   1. Never k_sleep() while a fake_radio operation is armed - ztest is
 *      cooperative and the event thread (prio 6) only runs when this thread
 *      blocks, so a sleep loses ordering control. The housekeeping-watchdog
 *      tests sleep deliberately, with the radio quiet first.
 *   2. This suite cannot reproduce H1 (the thread/interrupt race in
 *      radiant_sched.c): fake_radio delivers callbacks synchronously, so an
 *      ISR preempting thread context is unreachable here. A green run proves
 *      the logic, not the concurrency.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <string.h>

#include "fake_radio.h"

#include <radiant_core/radiant_api.h>
#include <radiant_core/radiant_channel.h>
#include <radiant_core/radiant_event.h>
#include <radiant_core/radiant_frame.h>
#include <radiant_core/radiant_sched.h>
#include <radiant_core/radiant_search.h>
#include <radiant_core/radiant_transfer.h>

#include "ant_radio.h"
#include "ant_wire.h"

/* A real sensor (#14871 / 0x3A17, bicycle power, trans type 5) so byte
 * comparisons against capture lines need no translation. */
#define CH          0u
#define DEVNUM      0x3A17u
#define DEVNUM_LO   0x17u
#define DEVNUM_HI   0x3Au
#define DEVTYPE     0x0Bu
#define TRANSTYPE   5u
#define RF_INDEX    57u

/* Body layout in ANT's tracking geometry: [ttype][ctrl][d0..d7]. */
#define BODY_TTYPE  0u
#define BODY_CTRL   1u
#define BODY_D0     2u

/* An on-air frame as fake_radio stores it: address bytes then body bytes, no
 * preamble and no CRC. 5 + 10. */
#define AIR_LEN     15u

#define PERIOD_US \
	radiant_channel_counts_to_us(RADIANT_CHANNEL_PERIOD_ANT_PLUS)

/* antr_on_message() is resolved at link time; the firmware's definition is
 * src/ant_serial_bridge.c, this file's records instead of serialising -
 * everything the host would see passes through here. */

#define MSGLOG_MAX 64

struct msg_rec {
	uint8_t id;
	uint8_t len;
	uint8_t data[ANTW_MAX_SIZE_VALUE];
};

static struct msg_rec msglog[MSGLOG_MAX];
static uint32_t       n_msg; /* attempted, so an overflow is visible */

void antr_on_message(const struct antr_msg *msg)
{
	if (msg == NULL || msg->data == NULL) {
		return;
	}
	if (n_msg < MSGLOG_MAX) {
		uint8_t n = msg->len;

		if (n > (uint8_t)sizeof(msglog[0].data)) {
			n = (uint8_t)sizeof(msglog[0].data);
		}
		msglog[n_msg].id = msg->id;
		msglog[n_msg].len = n;
		memcpy(msglog[n_msg].data, msg->data, n);
	}
	n_msg++;
}

/* Deliver whatever the core has queued, from this thread - keeps assertions
 * deterministic instead of sleeping for the event thread (constraint 1). */
static void drain(void)
{
	(void)radiant_event_drain(0u);
}

static uint32_t count_msgs(uint8_t id)
{
	uint32_t n = 0u;
	uint32_t i;

	for (i = 0u; i < n_msg && i < MSGLOG_MAX; i++) {
		if (msglog[i].id == id) {
			n++;
		}
	}
	return n;
}

/* Unsolicited channel events, by code. MESG_RESPONSE_EVENT_ID carries both
 * command replies and channel events; byte 1 == MESG_EVENT_ID distinguishes
 * the two, else a "none raised" assertion would pass for the wrong reason. */
static uint32_t count_channel_events(uint8_t ch, uint8_t code)
{
	uint32_t n = 0u;
	uint32_t i;

	for (i = 0u; i < n_msg && i < MSGLOG_MAX; i++) {
		if (msglog[i].id != (uint8_t)ANTW_MESG_RESPONSE_EVENT_ID ||
		    msglog[i].len < 3u) {
			continue;
		}
		if (msglog[i].data[0] == ch &&
		    msglog[i].data[1] == (uint8_t)ANTW_MESG_EVENT_ID &&
		    msglog[i].data[2] == code) {
			n++;
		}
	}
	return n;
}

/* ---------------------------------------------------------------------------
 * Fixture
 * ---------------------------------------------------------------------------
 */

static const uint8_t ant_plus_key[8] = {
	0xB9u, 0xA5u, 0x21u, 0xFBu, 0xBDu, 0x72u, 0xC3u, 0x45u
};

/* What the host writes into the channel's broadcast buffer, and therefore what
 * every acknowledgement this master sends must carry. */
static const uint8_t bcast_payload[8] = {
	0x10u, 0xBDu, 0xFFu, 0x50u, 0xDEu, 0x11u, 0x64u, 0x00u
};

/* The slave's payload, from the capture: C0 xx 5A A5 00 11 22 33. */
static const uint8_t peer_payload[8] = {
	0xC0u, 0x00u, 0x5Au, 0xA5u, 0x00u, 0x11u, 0x22u, 0x33u
};

static void *api_setup(void)
{
	fake_radio_reset();
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR, antr_init(),
		      "antr_init() failed");
	return NULL;
}

/*
 * antr_init() creates a thread and must run once for the suite; per-test
 * reset is antr_stack_reset() plus a fresh mock. k_sched_lock() guards the
 * mock reset because fake_radio_reset() puts the HAL back to pre-init, and
 * the event thread calling radiant_radio_now() in that window would be
 * asking a radio that doesn't exist.
 */
static void api_before(void *f)
{
	ARG_UNUSED(f);

	(void)antr_stack_reset();

	k_sched_lock();
	fake_radio_reset();
	(void)radiant_radio_init(radiant_sched_radio_cbs(), NULL);
	(void)radiant_radio_enable();
	k_sched_unlock();

	memset(msglog, 0, sizeof(msglog));
	n_msg = 0u;
}

ZTEST_SUITE(api, NULL, api_setup, api_before, NULL, NULL);

/* Close the channel first: a live channel keeps the radio armed forever
 * (correct behaviour), which would make an unconditional is_idle() a false
 * alarm. */
static void end_of_test(void)
{
	(void)antr_channel_close(CH);
	fake_radio_advance(1000u);
	drain();

	zassert_true(fake_radio_is_idle(), "radio not idle: %s",
		     fake_radio_busy_reason());
	zassert_equal(0u, fake_radio_viol_count(), "contract violation: %s",
		      fake_radio_viol_name(fake_radio_viol(0)->code));
	zassert_true(n_msg <= MSGLOG_MAX, "the message log overflowed");
}

/* ---------------------------------------------------------------------------
 * Driving a master channel
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

/* Last accepted transmit with this control byte (0x0A = broadcast, 0xF2 =
 * acknowledgement) - distinguishes the two in the arm log. */
static const struct fake_radio_arm *last_tx_with_ctrl(uint8_t ctrl)
{
	uint32_t n = fake_radio_arm_count();

	while (n > 0u) {
		const struct fake_radio_arm *a = fake_radio_arm(--n);

		if (a == NULL || a->kind != FAKE_RADIO_ARM_TX ||
		    a->rc != RADIANT_RADIO_OK_RC) {
			continue;
		}
		if (a->body[BODY_CTRL] == ctrl) {
			return a;
		}
	}
	return NULL;
}

static uint32_t count_tx_with_ctrl(uint8_t ctrl)
{
	uint32_t n = fake_radio_arm_count();
	uint32_t found = 0u;
	uint32_t i;

	for (i = 0u; i < n; i++) {
		const struct fake_radio_arm *a = fake_radio_arm(i);

		if (a == NULL || a->kind != FAKE_RADIO_ARM_TX ||
		    a->rc != RADIANT_RADIO_OK_RC) {
			continue;
		}
		if (a->body[BODY_CTRL] == ctrl) {
			found++;
		}
	}
	return found;
}

/*
 * The peer's frame, hand-assembled rather than run through the encoder under
 * test (else the test would just assert the module agrees with itself).
 * Layout from docs/ant-radio-link.md:
 *     A6 C5 | dl dh | dtype | ttype | ctrl | d0..d7
 */
static uint8_t build_peer_frame(uint8_t *out, uint8_t ctrl, const uint8_t *payload8)
{
	out[0] = RADIANT_NET_ADDR_ANT_PLUS_0;
	out[1] = RADIANT_NET_ADDR_ANT_PLUS_1;
	out[2] = DEVNUM_LO;
	out[3] = DEVNUM_HI;
	out[4] = DEVTYPE;
	out[5] = (uint8_t)TRANSTYPE;
	out[6] = ctrl;
	memcpy(&out[7], payload8, 8u);
	return AIR_LEN;
}

/* Open one channel of `type`, configured as the bench's own sensor. */
static void open_channel(uint8_t type)
{
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_network_address_set(0u, ant_plus_key));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_assign(CH, type, 0u, 0u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_id_set(CH, DEVNUM, DEVTYPE, TRANSTYPE));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_period_set(CH,
					      RADIANT_CHANNEL_PERIOD_ANT_PLUS));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_radio_freq_set(CH, RF_INDEX));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_open(CH));
}

/* What a real host does on EVENT_TX: write the next payload. This also pumps
 * a scheduling pass in thread context - the half of H1's collision the mock
 * can model. */
static void host_writes_broadcast(void)
{
	uint8_t buf[8];

	memcpy(buf, bcast_payload, sizeof(buf));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_broadcast_message_tx(CH, 8u, buf));
}

/*
 * Run the master's own slot to completion and return the t_sync it achieved.
 * Advances just past it and no further, so the turnaround the completion
 * arms is inspectable before anything is injected into it.
 */
static radiant_time_t run_one_master_slot(void)
{
	const struct fake_radio_arm *tx = last_tx_with_ctrl(RADIANT_CTRL_BROADCAST);
	radiant_time_t               t_sync;

	/* Armed, not transmitted: the record is updated in place as the op
	 * runs, so `terminated` is the honest question, not a new record. */
	zassert_not_null(tx,
			 "the master's own slot was never armed: arms=%u "
			 "(tx=%u rx=%u rejected=%u etime=%u estate=%u) "
			 "pumps=%u state=%d",
			 fake_radio_arm_count(), fake_radio_stats()->arms_tx,
			 fake_radio_stats()->arms_rx,
			 fake_radio_stats()->arms_rejected,
			 fake_radio_stats()->rc_etime,
			 fake_radio_stats()->rc_estate,
			 radiant_api_stats_get()->pumps,
			 (int)radiant_channel_state_get(CH));
	zassert_false(tx->terminated,
		      "the armed slot had already run; the caller did not post "
		      "a new one: arms=%u (tx=%u rx=%u rejected=%u etime=%u "
		      "estate=%u ebusy=%u) pumps=%u dones=%u missed=%u "
		      "pending=%d state=%d now=%u last_t_sync=%u",
		      fake_radio_arm_count(), fake_radio_stats()->arms_tx,
		      fake_radio_stats()->arms_rx,
		      fake_radio_stats()->arms_rejected,
		      fake_radio_stats()->rc_etime,
		      fake_radio_stats()->rc_estate,
		      fake_radio_stats()->rc_ebusy,
		      radiant_api_stats_get()->pumps,
		      radiant_api_stats_get()->sched_dones,
		      radiant_api_stats_get()->slots_missed,
		      (int)radiant_sched_pending(CH),
		      (int)radiant_channel_state_get(CH),
		      (unsigned)radiant_radio_now(),
		      (unsigned)tx->t_sync_at);

	t_sync = tx->t_sync_at;

	/* Just past the terminal event that arms the turnaround, no further. */
	fake_radio_advance_to(t_sync + 1u);
	zassert_true(tx->terminated, "the master's slot did not complete");
	return t_sync;
}

/* One whole period with nothing from the slave. The turnaround must close
 * before the next slot posts: api_pump_locked() skips a channel whose
 * scheduler slot is still pending, and between a transmit and its
 * turnaround's close the turnaround IS that slot. */
static radiant_time_t run_one_quiet_period(void)
{
	radiant_time_t t_tx;

	host_writes_broadcast();
	t_tx = run_one_master_slot();
	fake_radio_advance_to(t_tx + RADIANT_TRANSFER_SLOT_REPLY_US +
			      RADIANT_TRANSFER_ACK_GUARD_US + 10u);
	return t_tx;
}

/* ---------------------------------------------------------------------------
 * The tests
 * ---------------------------------------------------------------------------
 */

/*
 * THE ONE THIS SUITE EXISTS FOR: a master must answer a slave's acknowledged
 * data with 0xF2 carrying its own broadcast buffer, 1560 us after the
 * slave's packet, on EVERY exchange - not only the first. Reference trace:
 * archive/captures/radio/2026-08-09-spike-b2-runA-burst-seq.log.
 */
ZTEST(api, test_a_master_answers_every_slave_exchange_not_only_the_first)
{
	uint8_t  frame[AIR_LEN];
	uint32_t period;
	radiant_time_t prev_slot = 0u;

	open_channel((uint8_t)ANTW_CHANNEL_TYPE_MASTER);

	for (period = 0u; period < 8u; period++) {
		const struct fake_radio_arm *turn;
		const struct fake_radio_arm *reply;
		radiant_time_t               t_tx;

		host_writes_broadcast();
		t_tx = run_one_master_slot();

		/* The turnaround, armed from inside the completion of the
		 * transmit it is placed against. Measured: slave reply
		 * t_sync = master t_sync + 2190 us; guard band sized on the
		 * observed range. */
		turn = last_arm_of(FAKE_RADIO_ARM_RX);
		zassert_not_null(turn, "period %u: no turnaround was armed",
				 period);
		zassert_equal(t_tx + RADIANT_TRANSFER_SLOT_REPLY_US -
				      RADIANT_TRANSFER_ACK_GUARD_US,
			      turn->t_open,
			      "period %u: the turnaround did not open on the "
			      "measured edge", period);
		zassert_equal(t_tx + RADIANT_TRANSFER_SLOT_REPLY_US +
				      RADIANT_TRANSFER_ACK_GUARD_US,
			      turn->t_close, "period %u: wrong close edge",
			      period);
		zassert_true(turn->from_callback,
			     "period %u: the turnaround was not armed from "
			     "inside a callback - a pump running in thread "
			     "context has neither the instant nor the deadline",
			     period);

		/* The slave speaks. */
		(void)build_peer_frame(frame, RADIANT_CTRL_BURST_LAST_SEQ0,
				       peer_payload);
		zassert_equal(RADIANT_RADIO_OK_RC,
			      fake_radio_air_frame(t_tx +
						   RADIANT_TRANSFER_SLOT_REPLY_US,
						   frame, AIR_LEN));

		fake_radio_advance_to(t_tx + RADIANT_TRANSFER_SLOT_REPLY_US +
				      RADIANT_TRANSFER_REPLY_US + 10u);

		reply = last_tx_with_ctrl(RADIANT_CTRL_ACK_LAST_SEQ1);
		zassert_not_null(reply,
				 "period %u: the master did not answer 0xA2 "
				 "with 0xF2", period);
		zassert_equal(t_tx + RADIANT_TRANSFER_SLOT_REPLY_US +
				      RADIANT_TRANSFER_REPLY_US,
			      reply->t_sync_at,
			      "period %u: the reply missed the 1560 us "
			      "turnaround", period);
		zassert_mem_equal(&reply->body[BODY_D0], bcast_payload, 8u,
				  "period %u: the acknowledgement did not carry "
				  "the broadcast buffer", period);
		zassert_equal(period + 1u,
			      count_tx_with_ctrl(RADIANT_CTRL_ACK_LAST_SEQ1),
			      "period %u: exactly one reply per exchange",
			      period);

		/* The exchange must not drag the master's phase: a master owns
		 * the slot grid, so the slave's reply must not re-anchor it. */
		if (period > 0u) {
			zassert_equal(PERIOD_US, t_tx - prev_slot,
				      "period %u: the slot clock moved by "
				      "something other than one period",
				      period);
		}
		prev_slot = t_tx;
	}

	drain();
	zassert_equal(8u, count_msgs((uint8_t)ANTW_MESG_ACKNOWLEDGED_DATA_ID),
		      "the host was not told about every exchange");

	end_of_test();
}

/*
 * H3. A listening master's own transmit produced no done() at all: hal_tx()
 * called the tx callback before end_armed(); api_sched_tx() posted the
 * turnaround; radiant_sched_request_rx() called drop_slot() on the still-
 * in-flight slot, aborting the armed op; end_armed() then returned with
 * nothing armed to finish. Every period, on every listening master. Cost: a
 * closing channel never finished closing if the close raced a master slot,
 * and radiant_radio_abort() fired on the real radio from inside a completed
 * transmit's own ISR, 4x/s.
 */
ZTEST(api, test_the_masters_own_transmit_produces_a_done)
{
	uint32_t before;
	uint32_t slots;

	open_channel((uint8_t)ANTW_CHANNEL_TYPE_MASTER);

	before = radiant_api_stats_get()->sched_dones;

	for (slots = 0u; slots < 4u; slots++) {
		/* The turnaround runs out empty too, so each period
		 * contributes exactly two completed operations. */
		(void)run_one_quiet_period();
	}

	/* Two per period: transmit + turnaround. The bug produced only one
	 * (the turnaround's), so >= would pass while broken - equality is
	 * the whole test. */
	zassert_equal(8u, radiant_api_stats_get()->sched_dones - before,
		      "a listening master's own transmit lost its completion");

	end_of_test();
}

/*
 * H2's fingerprint: do the master's own 0x0A broadcasts continue after an
 * exchange? If they stop, the transfer is wedged (api_pump_locked() skips a
 * channel whose transfer engine isn't idle, so the whole broadcast slot goes
 * with it); if they continue with only the reply missing, the fault is in
 * the receive/decide path instead.
 */
ZTEST(api, test_the_master_keeps_broadcasting_after_an_exchange)
{
	uint8_t        frame[AIR_LEN];
	radiant_time_t t_tx;
	uint32_t       after_exchange;

	open_channel((uint8_t)ANTW_CHANNEL_TYPE_MASTER);

	host_writes_broadcast();
	t_tx = run_one_master_slot();

	(void)build_peer_frame(frame, RADIANT_CTRL_BURST_LAST_SEQ0, peer_payload);
	zassert_equal(RADIANT_RADIO_OK_RC,
		      fake_radio_air_frame(t_tx + RADIANT_TRANSFER_SLOT_REPLY_US,
					   frame, AIR_LEN));
	fake_radio_advance_to(t_tx + RADIANT_TRANSFER_SLOT_REPLY_US +
			      RADIANT_TRANSFER_REPLY_US + 10u);
	zassert_not_null(last_tx_with_ctrl(RADIANT_CTRL_ACK_LAST_SEQ1),
			 "the exchange itself did not happen");

	after_exchange = count_tx_with_ctrl(RADIANT_CTRL_BROADCAST);

	/* Three more periods, each with a quiet turnaround. */
	(void)run_one_quiet_period();
	(void)run_one_quiet_period();
	(void)run_one_quiet_period();

	zassert_equal(after_exchange + 3u,
		      count_tx_with_ctrl(RADIANT_CTRL_BROADCAST),
		      "the master stopped broadcasting after an exchange");

	end_of_test();
}

/*
 * The H2 watchdog. The wedge is synthesised with radiant_sched_cancel(),
 * which is the exact shape of the real failure: cancelling is silent, so no
 * done() arrives, the transfer engine is left mid-reply with nothing armed,
 * and api_feed_xfer_terminal() (reachable only from a completion) never
 * runs. Before the watchdog that state was permanent. The sleeps here
 * deliberately hand control to the event thread since housekeeping is the
 * subject, and the radio is quiet by then.
 */
ZTEST(api, test_a_wedged_reply_is_recovered_by_housekeeping)
{
	uint8_t        frame[AIR_LEN];
	radiant_time_t t_tx;

	open_channel((uint8_t)ANTW_CHANNEL_TYPE_MASTER);

	host_writes_broadcast();
	t_tx = run_one_master_slot();

	/* Put the engine into TX_REPLY with the reply armed... */
	(void)build_peer_frame(frame, RADIANT_CTRL_BURST_LAST_SEQ0, peer_payload);
	zassert_equal(RADIANT_RADIO_OK_RC,
		      fake_radio_air_frame(t_tx + RADIANT_TRANSFER_SLOT_REPLY_US,
					   frame, AIR_LEN));
	fake_radio_advance_to(t_tx + RADIANT_TRANSFER_SLOT_REPLY_US + 10u);

	/* ...and take the arm away without telling anyone. */
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_sched_cancel(CH));
	zassert_false(radiant_sched_pending(CH),
		      "the wedge was not set up: something is still pending");
	zassert_equal(0u, radiant_api_stats_get()->xfer_watchdogs,
		      "the watchdog fired before it should have");

	/* One housekeeping interval to notice the engine is stuck... */
	k_sleep(K_MSEC(3 * RADIANT_API_HOUSEKEEP_MS));
	zassert_equal(0u, radiant_api_stats_get()->xfer_watchdogs,
		      "the watchdog must not fire on the first observation - "
		      "it is a deadline, not a level");

	/* ...then a deadline's worth of the RADIO's clock, which is the one the
	 * watchdog reasons about. Real time and virtual time are different
	 * clocks here and only one of them is the deadline. */
	fake_radio_advance(((uint64_t)RADIANT_API_XFER_WATCHDOG_MS * 1000u) + 1000u);
	k_sleep(K_MSEC(3 * RADIANT_API_HOUSEKEEP_MS));

	zassert_equal(1u, radiant_api_stats_get()->xfer_watchdogs,
		      "a transfer left non-idle with nothing armed was never "
		      "recovered");

	end_of_test();
}

/*
 * A constant transmit timing error is not free for a master, unlike for a
 * slave (radiant_radio_hal.h: a slave's reported t_sync error cancels out of
 * the period estimate). A master's next slot anchors on the t_sync its last
 * transmit ACHIEVED, so a backend systematically late by d drifts its frames
 * by d every period, cumulatively, with nothing to notice it - hence
 * radiant_dbg_tx_err and T_SYNC_CAL_TX_US applying to the arm path, not just
 * the report. Asserts the exact relationship: constant offset -> constant
 * period error of the same size, with the exchange still working throughout.
 */
ZTEST(api, test_a_constant_transmit_offset_moves_the_period_by_exactly_that)
{
	const int32_t  offset_us = -9;   /* mirrors T_SYNC_CAL_US in the nrf backend */
	radiant_time_t prev = 0u;
	uint32_t       i;

	fake_radio_set_tx_offset_us(offset_us);
	open_channel((uint8_t)ANTW_CHANNEL_TYPE_MASTER);

	for (i = 0u; i < 5u; i++) {
		radiant_time_t t_tx = run_one_quiet_period();

		if (i > 0u) {
			zassert_equal(PERIOD_US + (radiant_time_t)offset_us,
				      t_tx - prev,
				      "period %u: a constant transmit offset must "
				      "move the slot grid by exactly itself - "
				      "neither cancelling out nor compounding",
				      i);
		}
		prev = t_tx;
	}

	/* And nothing about it is visible from the outside: no missed slots, no
	 * failures, no host event. That is the point. */
	zassert_equal(0u, radiant_api_stats_get()->slots_missed);
	zassert_equal(0u, radiant_api_stats_get()->sched_failed);

	end_of_test();
}

/* ---------------------------------------------------------------------------
 * The slave side: a background scan. radiant_search.c's own decisions are
 * tested by its module suite; what's wrong lived in what radiant_api.c did
 * with the answer.
 * ---------------------------------------------------------------------------
 */

/*
 * A host's broadcast with the extended tail (lib config ALL_EXT_FIELDS):
 *     [channel][d0..d7][flag][devnum lo][devnum hi][device type][trans type]
 * See radiant_event.h's layout comment; index 9 is the flag byte since the
 * payload is always 8.
 */
#define EXT_FLAG   9u
#define EXT_NUM_LO 10u
#define EXT_NUM_HI 11u
#define EXT_DTYPE  12u

static uint32_t count_bcast_from(uint16_t device_number)
{
	uint32_t n = 0u;
	uint32_t i;

	for (i = 0u; i < n_msg && i < MSGLOG_MAX; i++) {
		const struct msg_rec *m = &msglog[i];
		uint16_t              num;

		if (m->id != (uint8_t)ANTW_MESG_BROADCAST_DATA_ID ||
		    m->len < (EXT_DTYPE + 1u)) {
			continue;
		}
		if ((m->data[EXT_FLAG] & (uint8_t)ANTW_EXT_FLAG_CHANNEL_ID) == 0u) {
			continue;
		}
		num = (uint16_t)((uint16_t)m->data[EXT_NUM_LO] |
				 ((uint16_t)m->data[EXT_NUM_HI] << 8));
		if (num == device_number) {
			n++;
		}
	}
	return n;
}

/* Zwift's actual discovery channel: SLAVE_RX_ONLY, network 0, extended
 * assignment ALWAYS_SEARCH, wildcard channel ID. Not MESG_OPEN_RX_SCAN_MODE -
 * Zwift never sends 0x5B. Written out rather than via open_channel() since
 * the assignment bytes ARE the scenario. */
static void open_background_scan_channel(void)
{
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_network_address_set(0u, ant_plus_key));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_lib_config_set((uint8_t)ANTW_LIB_CONFIG_ALL_EXT_FIELDS));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_assign(CH,
					  (uint8_t)ANTW_CHANNEL_TYPE_SLAVE_RX_ONLY, 0u,
					  (uint8_t)ANTW_EXT_PARAM_ALWAYS_SEARCH));
	/* Wildcard in all three fields: find anything. */
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_id_set(CH, 0u, 0u, 0u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_period_set(CH, RADIANT_CHANNEL_PERIOD_ANT_PLUS));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_radio_freq_set(CH, RF_INDEX));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR, antr_channel_open(CH));
}

/* The armed search window and its filters, source of the device numbers used
 * below. A sweep set only matches 8 device-number low bytes at a time, so
 * reading the low byte from the actually-armed window (rather than
 * hardcoding one) makes the frame match by construction. */
static const struct fake_radio_arm *armed_search_window(void)
{
	const struct fake_radio_arm *w = last_arm_of(FAKE_RADIO_ARM_RX);

	zassert_not_null(w, "no search window was armed at all");
	zassert_false(w->terminated,
		      "the window found had already run - the sweep stopped");
	zassert_true(w->n_filters >= 2u,
		     "the nRF caps preset gives eight filters per window; got %u",
		     w->n_filters);
	return w;
}

/*
 * Let the event thread post the next window of the sweep. A SEARCHING
 * channel has nothing host-side to pump it - a sweep's next window comes
 * only from api_housekeep(), so without this it stops dead after one
 * window. Second of the two deliberate sleeps in this file (constraint 1):
 * legitimate because nothing is armed when it runs, and the assertion below
 * checks that rather than trusting it.
 */
static void run_housekeeping(void)
{
	zassert_true(fake_radio_is_idle(),
		     "sleeping with an operation armed gives the ordering away "
		     "to the event thread: %s",
		     fake_radio_busy_reason());
	k_sleep(K_MSEC(3 * RADIANT_API_HOUSEKEEP_MS));
}

static void air_device(const struct fake_radio_arm *w, uint8_t filter_index,
		       uint16_t number_hi, uint8_t dev_type, uint32_t at_offset_us,
		       uint16_t *out_number)
{
	uint8_t frame[FAKE_RADIO_AIR_FRAME_MAX];
	uint8_t len;
	uint16_t number = (uint16_t)(number_hi | w->filters[filter_index].addr[2]);

	len = fake_radio_build_ant_frame(frame, number, dev_type, 5u, peer_payload);
	zassert_equal(RADIANT_RADIO_OK_RC,
		      fake_radio_air_frame(w->t_open + at_offset_us, frame, len),
		      "could not put device %u on the air", (unsigned int)number);
	*out_number = number;
}

/*
 * Fix in api_search_acquired(): a background scan must never leave. It used
 * to call radiant_channel_on_acquired() for every match regardless of mode,
 * which unconditionally sets TRACKING - so the first sensor to answer
 * converted the scan into an ordinary tracked channel and every other sensor
 * became invisible (symptom: Zwift's pairing screen showed one sensor, a
 * different one each run).
 *
 * Two devices in the SAME window is the sharp half: if the channel left the
 * search on the first frame, the second has nowhere to be delivered. A third
 * device in a LATER window proves the sweep itself kept running.
 */
ZTEST(api, test_a_background_scan_reports_every_device_not_only_the_first)
{
	const struct fake_radio_arm *w;
	uint16_t                     dev_a;
	uint16_t                     dev_b;
	uint16_t                     dev_c;
	uint32_t                     op_first;

	open_background_scan_channel();

	w = armed_search_window();
	op_first = w->op;

	/* A power meter and a heart rate monitor, both inside one dwell. */
	air_device(w, 0u, 0x0500u, 0x0Bu, 1000u, &dev_a);
	air_device(w, 1u, 0x0600u, 0x78u, 5000u, &dev_b);

	fake_radio_advance_to(w->t_close + 1u);
	drain();

	zassert_true(count_bcast_from(dev_a) >= 1u,
		     "the first device to answer was never reported at all - "
		     "this is not the scan bug, the scan is not working");
	zassert_true(count_bcast_from(dev_b) >= 1u,
		     "device %u answered the same window as device %u and the "
		     "host was never told: the scan converted itself to a "
		     "tracked channel on the first match",
		     (unsigned int)dev_b, (unsigned int)dev_a);

	/* Still searching: a scan channel reading TRACKING has been acquired,
	 * and is one radiant_search_end() away from discovering nothing else
	 * for the rest of the session. */
	zassert_equal(RADIANT_CH_STATE_SEARCHING, radiant_channel_state_get(CH),
		      "a background scan must never leave the search - state %d",
		      (int)radiant_channel_state_get(CH));

	/* A third device, in whatever window the sweep moved on to. */
	run_housekeeping();
	w = armed_search_window();
	zassert_not_equal(op_first, w->op,
			  "the sweep did not move on to a second window");

	air_device(w, 0u, 0x0700u, 0x11u, 1000u, &dev_c);
	fake_radio_advance_to(w->t_close + 1u);
	drain();

	zassert_true(count_bcast_from(dev_c) >= 1u,
		     "device %u was found by a later window and not reported - "
		     "the sweep stopped serving the scan channel",
		     (unsigned int)dev_c);
	zassert_equal(RADIANT_CH_STATE_SEARCHING, radiant_channel_state_get(CH));

	end_of_test();
}

/* Broadcasts delivered on one channel, whatever device they came from. */
static uint32_t count_bcast_on(uint8_t ch)
{
	uint32_t n = 0u;
	uint32_t i;

	for (i = 0u; i < n_msg && i < MSGLOG_MAX; i++) {
		if (msglog[i].id == (uint8_t)ANTW_MESG_BROADCAST_DATA_ID &&
		    msglog[i].len >= 1u && msglog[i].data[0] == ch) {
			n++;
		}
	}
	return n;
}

/*
 * Run virtual time forward while letting the event thread keep up. fake_radio
 * fires events on the calling thread, so a single long advance would starve
 * the sweep as a pure harness artefact; api_housekeep() only runs when this
 * thread blocks. So: step to the next due event (max 20 ms), deliver, yield.
 * This is the one place in the file that sleeps with operations armed -
 * deliberate, since the subject IS the event thread's pump racing a busy
 * radio. Assertions fed by this are lower bounds over a long window, not
 * exact orderings, which is what makes it safe.
 */
static void run_virtual_us(uint64_t total_us)
{
	uint64_t elapsed = 0u;

	while (elapsed < total_us) {
		radiant_time_t due = fake_radio_next_due();
		radiant_time_t now = radiant_radio_now();
		uint32_t       step = 20000u;

		if (due != RADIANT_TIME_NEVER) {
			uint32_t d = (uint32_t)(due - now); /* wrap-safe */

			if (d < step) {
				step = d + 1u;
			}
		}
		fake_radio_advance(step);
		elapsed += step;
		drain();
		k_sleep(K_MSEC(1));
	}
}

/* One sensor's frames for the whole run, on its own period. */
static void air_repeating(uint16_t number, uint8_t dev_type, radiant_time_t t_first,
			  uint32_t count)
{
	uint8_t frame[FAKE_RADIO_AIR_FRAME_MAX];
	uint8_t len = fake_radio_build_ant_frame(frame, number, dev_type, 5u,
						 peer_payload);

	(void)fake_radio_air_master(t_first, PERIOD_US, count, frame, len);
}

/*
 * The second bug: Zwift keeps its wildcard scan channel open alongside
 * already-paired channels and expects to keep discovering while they track.
 * A real capture showed that stopping dead - with one channel receiving at
 * 4 Hz, the scan reported zero devices for 25 s then 35 s across six scan
 * cycles - the sweep was getting no radio at all. api_post_search_window()
 * posts the sweep as radiant_sched.c's CONTINUOUS form so the scheduler fits
 * chunks into the gaps between tracked slots; this test asserts that claim.
 *
 * Twelve seconds is one and a half full sweeps (32 sets x 260 ms = 8.32 s),
 * so a healthy sweep must reach every set regardless of phase at tracking
 * start.
 */
ZTEST(api, test_the_scan_keeps_finding_devices_while_another_channel_tracks)
{
	const struct fake_radio_arm *w;
	uint16_t                     dev_a;
	const uint16_t               dev_b = 0x04A5u;
	radiant_time_t               t_acq;
	uint32_t                     scan_before;

	open_background_scan_channel();

	/* A second channel, ordinary wildcard slave - this is what Zwift opens
	 * for a sensor it has just paired. */
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_assign(1u,
					  (uint8_t)ANTW_CHANNEL_TYPE_SLAVE_RX_ONLY,
					  0u, 0u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_id_set(1u, 0u, 0u, 0u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_period_set(1u, RADIANT_CHANNEL_PERIOD_ANT_PLUS));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_radio_freq_set(1u, RF_INDEX));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR, antr_channel_open(1u));

	/* Device A alone in the first window (B not on air yet), so which
	 * channel takes which sensor is a fact, not a race. */
	w = armed_search_window();
	air_device(w, 0u, 0x0500u, 0x0Bu, 1000u, &dev_a);
	t_acq = w->t_open + 1000u;
	fake_radio_advance_to(w->t_close + 1u);
	drain();

	zassert_equal(RADIANT_CH_STATE_TRACKING, radiant_channel_state_get(1u),
		      "channel 1 did not acquire the only device on the air");

	/* From here A keeps its slot and B is simply present, all run long. */
	air_repeating(dev_a, 0x0Bu, t_acq + PERIOD_US, 48u);
	air_repeating(dev_b, 0x78u, t_acq + (PERIOD_US / 3u), 48u);

	scan_before = count_bcast_on(0u);
	run_virtual_us(12000000u);

	zassert_true(count_bcast_on(1u) > 0u,
		     "channel 1 stopped tracking, so this test is not measuring "
		     "what it claims to measure");
	zassert_true(count_bcast_from(dev_b) >= 1u,
		     "device %u was on the air for 12 s - one and a half full "
		     "sweeps - and the scan channel never reported it while "
		     "channel 1 was tracking. scan reports: %u before, %u after; "
		     "tracked packets: %u",
		     (unsigned int)dev_b, scan_before, count_bcast_on(0u),
		     count_bcast_on(1u));

	(void)antr_channel_close(1u);
	(void)antr_channel_close(CH);
	fake_radio_advance(1000u);
	drain();

	/* end_of_test()'s ending, minus the one violation this test always
	 * provokes: 12 s of tracked channel + sweep is a few hundred arm
	 * calls against a 64-entry mock log, so FAKE_RADIO_VIOL_LOG_FULL here
	 * is the harness running out of room, not a firmware contract
	 * violation. Every other code still fails the test. */
	zassert_true(fake_radio_is_idle(), "radio not idle: %s",
		     fake_radio_busy_reason());
	for (uint32_t i = 0u; i < fake_radio_viol_count() &&
			      i < (uint32_t)FAKE_RADIO_VIOL_MAX; i++) {
		enum fake_radio_viol code = fake_radio_viol(i)->code;

		/* NONE is a slot the violation log itself overflowed past. */
		zassert_true(code == FAKE_RADIO_VIOL_LOG_FULL ||
				     code == FAKE_RADIO_VIOL_NONE,
			     "contract violation: %s",
			     fake_radio_viol_name(code));
	}
}

/*
 * A channel that names its device must not wait for the round robin: since
 * devnum_lo picks a channel's device out to exactly one sweep set,
 * radiant_search_begin() pushes that set to the front, so acquisition should
 * cost one dwell, not one sweep.
 *
 * Measured 2026-08-10, one channel already tracking: ch1 (dev 8265, nothing
 * else tracking) acquired in 0.24 s; ch2 (dev 20329, ch1 tracking) took
 * 18.72 s though the device was on air throughout - the steer wasn't being
 * honoured.
 *
 * Device B is placed 16 sets (~4.2 s of round robin) from the sweep's
 * position when channel 2 opens, so passing inside the 2 s budget can only
 * be the steer, not the cursor arriving naturally.
 */
ZTEST(api, test_a_named_device_is_steered_to_rather_than_waited_for)
{
	const struct fake_radio_arm *w;
	uint16_t                     dev_a;
	uint16_t                     dev_b;
	uint8_t                      lo_now;
	radiant_time_t               t_acq;

	open_background_scan_channel();

	/* Channel 1: an ordinary wildcard slave. It acquires device A and then
	 * holds the radio to a 4 Hz tracked slot for the rest of the run, which
	 * is the condition the bug needs. */
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_assign(1u,
					  (uint8_t)ANTW_CHANNEL_TYPE_SLAVE_RX_ONLY,
					  0u, 0u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_id_set(1u, 0u, 0u, 0u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_period_set(1u, RADIANT_CHANNEL_PERIOD_ANT_PLUS));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_radio_freq_set(1u, RF_INDEX));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR, antr_channel_open(1u));

	w = armed_search_window();
	lo_now = w->filters[0].addr[2];
	air_device(w, 0u, 0x0500u, 0x0Bu, 1000u, &dev_a);
	t_acq = w->t_open + 1000u;
	fake_radio_advance_to(w->t_close + 1u);
	drain();

	zassert_equal(RADIANT_CH_STATE_TRACKING, radiant_channel_state_get(1u),
		      "setup: channel 1 did not acquire the only device on air");

	air_repeating(dev_a, 0x0Bu, t_acq + PERIOD_US, 64u);

	dev_b = (uint16_t)(0x0700u | (uint16_t)(uint8_t)(lo_now + 128u));
	air_repeating(dev_b, 0x78u, t_acq + (PERIOD_US / 3u), 64u);

	/* Channel 2 names device B: Zwift re-pairing a known sensor, the case
	 * that took 18.72 s. */
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_assign(2u,
					  (uint8_t)ANTW_CHANNEL_TYPE_SLAVE_RX_ONLY,
					  0u, 0u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_id_set(2u, dev_b, 0x78u, 0u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_period_set(2u, RADIANT_CHANNEL_PERIOD_ANT_PLUS));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_radio_freq_set(2u, RF_INDEX));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR, antr_channel_open(2u));

	run_virtual_us(2000000u);

	zassert_equal(RADIANT_CH_STATE_TRACKING, radiant_channel_state_get(2u),
		      "channel 2 named device %u and did not acquire it in 2 s, "
		      "while channel 1 tracked. Its set is sixteen sets from "
		      "where the sweep was, so the round robin alone needs "
		      "~4.2 s: the steer radiant_search_begin() pushed was not "
		      "honoured. On the bench this is an 18.7 s pairing delay "
		      "for a sensor the scan could already hear. tracked "
		      "packets on ch1: %u, reports of device %u: %u",
		      (unsigned int)dev_b, count_bcast_on(1u),
		      (unsigned int)dev_b, count_bcast_from(dev_b));

	(void)antr_channel_close(2u);
	(void)antr_channel_close(1u);
	(void)antr_channel_close(CH);
	fake_radio_advance(1000u);
	drain();

	/* Same ending as the sibling test above and for the same reason: the
	 * mock's 64-entry arm log overflows, not a firmware contract
	 * violation. */
	zassert_true(fake_radio_is_idle(), "radio not idle: %s",
		     fake_radio_busy_reason());
	for (uint32_t i = 0u; i < fake_radio_viol_count() &&
			      i < (uint32_t)FAKE_RADIO_VIOL_MAX; i++) {
		enum fake_radio_viol code = fake_radio_viol(i)->code;

		zassert_true(code == FAKE_RADIO_VIOL_LOG_FULL ||
				     code == FAKE_RADIO_VIOL_NONE,
			     "contract violation: %s",
			     fake_radio_viol_name(code));
	}
}

/*
 * A scan chunk that ends part-way through its set must keep the slot. A
 * tracked channel ends a chunk 4x/s per channel; api_sched_done() used to
 * cancel the request each time and wait for the event thread to re-post the
 * same filters, buying nothing and costing the sweep the gap. Measured on
 * nRF54L15 (1 channel @ 4 Hz) before the fix: full sweep 12.8 s vs nominal
 * 8.3 s, with nothing else competing or failing.
 *
 * Asserts the chunk is SHORT first: a chunk that ran the full dwell finishes
 * its set legitimately and must drop the slot, so without that check a pass
 * would prove nothing.
 */
ZTEST(api, test_a_scan_chunk_that_ends_mid_set_keeps_its_slot)
{
	const struct fake_radio_arm *w;
	uint16_t                     dev_a;
	radiant_time_t               t_acq;
	uint32_t                     len;

	open_background_scan_channel();

	/* An ordinary wildcard slave. Once it acquires it demands the radio
	 * every 249.7 ms, and that is what cuts the scan's chunks short. */
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_assign(1u,
					  (uint8_t)ANTW_CHANNEL_TYPE_SLAVE_RX_ONLY,
					  0u, 0u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_id_set(1u, 0u, 0u, 0u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_period_set(1u, RADIANT_CHANNEL_PERIOD_ANT_PLUS));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_radio_freq_set(1u, RF_INDEX));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR, antr_channel_open(1u));

	w = armed_search_window();
	air_device(w, 0u, 0x0500u, 0x0Bu, 1000u, &dev_a);
	t_acq = w->t_open + 1000u;
	fake_radio_advance_to(w->t_close + 1u);
	drain();

	zassert_equal(RADIANT_CH_STATE_TRACKING, radiant_channel_state_get(1u),
		      "setup: channel 1 did not acquire");

	air_repeating(dev_a, 0x0Bu, t_acq + PERIOD_US, 32u);

	/* One pump so the tracked channel's slot is posted and visible - a
	 * scan chunk armed now ends before it. */
	run_housekeeping();

	w = armed_search_window();
	len = (uint32_t)(w->t_close - w->t_open);
	zassert_true(len < (uint32_t)RADIANT_SEARCH_DWELL_DEFAULT_US,
		     "setup: the chunk got %u us of a %u us dwell, so it "
		     "finished the set and dropping the slot would be correct - "
		     "this test cannot tell the two apart",
		     (unsigned int)len,
		     (unsigned int)RADIANT_SEARCH_DWELL_DEFAULT_US);

	/* Run the chunk out and stop. drain() delivers queued host messages
	 * but doesn't sleep, so the event thread hasn't pumped - anything in
	 * this slot now was left by the completion itself. */
	fake_radio_advance_to(w->t_close + 1u);
	drain();

	zassert_true(radiant_sched_pending(CH),
		     "a scan chunk ended %u us into a %u us dwell and the sweep "
		     "gave its slot back, to be re-posted by the next "
		     "housekeeping pump. The filters describe the same address "
		     "set either way - there was nothing to decide - so the "
		     "round trip is pure dead air, four times a second per "
		     "tracked channel. On hardware it costs a third of the "
		     "sweep's throughput and half the worst-case time to find a "
		     "sensor",
		     (unsigned int)len,
		     (unsigned int)RADIANT_SEARCH_DWELL_DEFAULT_US);

	(void)antr_channel_close(1u);
	(void)antr_channel_close(CH);
	fake_radio_advance(1000u);
	drain();

	zassert_true(fake_radio_is_idle(), "radio not idle: %s",
		     fake_radio_busy_reason());
}

/*
 * A background scan must still be searching long after an ordinary channel
 * would give up. Fix in search_deadline(): a scan channel with DEFAULT
 * timeouts hit the ordinary search deadline at 25 s, raised
 * RX_SEARCH_TIMEOUT, and closed itself - leaving a discovery channel that
 * could never discover anything again ("dongle stops finding sensors after
 * a while").
 *
 * 40 s is well past the 25 s default; asserting channel STATE rather than a
 * device report means a quiet bench can't pass by accident.
 */
ZTEST(api, test_a_background_scan_never_times_out)
{
	open_background_scan_channel();
	zassert_equal(RADIANT_CH_STATE_SEARCHING, radiant_channel_state_get(CH));

	run_virtual_us(40000000u);

	zassert_equal(RADIANT_CH_STATE_SEARCHING, radiant_channel_state_get(CH),
		      "a channel assigned ALWAYS_SEARCH stopped searching after "
		      "40 s - state %d. \"Always search\" is the entire meaning "
		      "of the extended assignment bit",
		      (int)radiant_channel_state_get(CH));

	(void)antr_channel_close(CH);
	fake_radio_advance(1000u);
	drain();
	zassert_true(fake_radio_is_idle(), "radio not idle: %s",
		     fake_radio_busy_reason());
}

/*
 * The other side of the same coin: an ordinary wildcard slave (no extended
 * assignment) is ACQUIRE mode and must leave the search and track the first
 * match - that's how normal pairing works. Confirms the fix is a mode check,
 * not "never acquire from a search callback".
 */
ZTEST(api, test_a_plain_wildcard_slave_still_acquires_and_tracks)
{
	const struct fake_radio_arm *w;
	uint16_t                     dev_a;

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_network_address_set(0u, ant_plus_key));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_lib_config_set((uint8_t)ANTW_LIB_CONFIG_ALL_EXT_FIELDS));
	/* Only difference from the test above: the extended assignment byte. */
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_assign(CH,
					  (uint8_t)ANTW_CHANNEL_TYPE_SLAVE_RX_ONLY,
					  0u, 0u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_id_set(CH, 0u, 0u, 0u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_period_set(CH, RADIANT_CHANNEL_PERIOD_ANT_PLUS));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_radio_freq_set(CH, RF_INDEX));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR, antr_channel_open(CH));

	w = armed_search_window();
	air_device(w, 0u, 0x0500u, 0x0Bu, 1000u, &dev_a);
	fake_radio_advance_to(w->t_close + 1u);
	drain();

	zassert_true(count_bcast_from(dev_a) >= 1u,
		     "the acquiring frame is delivered to the host, not dropped");
	zassert_equal(RADIANT_CH_STATE_TRACKING, radiant_channel_state_get(CH),
		      "an ACQUIRE-mode wildcard slave must take the first device "
		      "it matches - state %d",
		      (int)radiant_channel_state_get(CH));

	/* And it took the device's identity with it. */
	{
		struct radiant_channel_id id;

		zassert_equal(RADIANT_CH_OK, radiant_channel_id_get(CH, &id));
		zassert_equal(dev_a, id.device_number,
			      "the channel tracked something other than what it "
			      "acquired");
	}

	end_of_test();
}

/*
 * A tracked channel owes its next window immediately, not one pump later -
 * the invariant a background scan lives or dies on. radiant_sched.c ends a
 * scan chunk at t_back(committed, lead), so if a tracked slave's next window
 * instead waits for api_pump_locked() on the event thread, the scan gets
 * armed with no end in the gap and the pump's later post preempts it. On the
 * bench this cost everything: 109 scan chunks armed unbounded vs 6 bounded,
 * frames_ok frozen at 15 for 14 s.
 *
 * This asserts the POST rather than discovery, because the mock's
 * synchronous completions make the gap unobservable otherwise (the sibling
 * discovery test passes with or without the fix) - on hardware arm_next()
 * runs inside the completion callback before the event thread is scheduled,
 * so pinning "the slot is refilled by the completion itself" is what
 * actually catches it.
 */
ZTEST(api, test_a_tracked_channel_reposts_its_next_window_without_a_pump)
{
	const struct fake_radio_arm *w;
	uint16_t                     dev_a;

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_network_address_set(0u, ant_plus_key));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_lib_config_set((uint8_t)ANTW_LIB_CONFIG_ALL_EXT_FIELDS));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_assign(CH,
					  (uint8_t)ANTW_CHANNEL_TYPE_SLAVE_RX_ONLY,
					  0u, 0u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_id_set(CH, 0u, 0u, 0u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_period_set(CH, RADIANT_CHANNEL_PERIOD_ANT_PLUS));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_radio_freq_set(CH, RF_INDEX));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR, antr_channel_open(CH));

	/* Acquire, so the channel is TRACKING and its slots are predicted. */
	w = armed_search_window();
	air_device(w, 0u, 0x0500u, 0x0Bu, 1000u, &dev_a);
	fake_radio_advance_to(w->t_close + 1u);
	drain();
	zassert_equal(RADIANT_CH_STATE_TRACKING, radiant_channel_state_get(CH),
		      "setup: the channel did not acquire");

	/* One pump for the FIRST tracked window, which legitimately comes
	 * from the pump since nothing has completed yet. The test is about
	 * its successor. */
	run_housekeeping();

	/* Run that window to completion and stop; drain() doesn't sleep, so
	 * anything pending now was put there by the completion itself. */
	{
		const struct fake_radio_arm *tw = last_arm_of(FAKE_RADIO_ARM_RX);

		zassert_not_null(tw, "no tracked window was armed after acquiring");
		zassert_false(tw->terminated,
			      "setup: the tracked window had already ended");
		fake_radio_advance_to(tw->t_close + 1u);
		drain();
	}

	zassert_true(radiant_sched_pending(CH),
		     "a tracked channel finished its window and left its slot "
		     "EMPTY, to be refilled by the next housekeeping pump. On "
		     "hardware the scheduler picks the next operation inside "
		     "that same completion, finds nothing committed, and arms "
		     "the background scan with no end - which the pump's later "
		     "post then preempts. That is how a paired sensor stops a "
		     "dongle discovering any others");

	end_of_test();
}

/*
 * MASTER_TX_ONLY does not listen, and that is the one master shape ANT
 * documents as not doing so. Bit 4 of the channel type is the master bit and
 * 0x50 is the exception; api_master_listens() is the only place in radiant_api.c
 * that asks the type rather than asking "is this a master", so this pins it.
 */
ZTEST(api, test_a_master_tx_only_channel_opens_no_turnaround)
{
	uint32_t rx_arms;

	open_channel((uint8_t)ANTW_CHANNEL_TYPE_MASTER_TX_ONLY);

	host_writes_broadcast();
	(void)run_one_master_slot();
	host_writes_broadcast();
	(void)run_one_master_slot();

	rx_arms = fake_radio_stats()->arms_rx;
	zassert_equal(0u, rx_arms,
		      "a MASTER_TX_ONLY channel armed %u receive windows",
		      rx_arms);

	end_of_test();
}

/* ---------------------------------------------------------------------------
 * Denial - what the host must and must not be told
 * ---------------------------------------------------------------------------
 */

/*
 * A denied master transmit must not wedge the dongle. Reproduces the
 * failure at radiant_channel.c's "a master advances its slot and nothing
 * else": a master's t_next only advances on a completed transmit, so a slot
 * that never went out used to leave it stuck in the past - pump re-posts the
 * dead instant, scheduler refuses it without calling the backend, refusal
 * completes synchronously, event thread posts the same dead instant again.
 * No fault, no log, the dongle just stops. Under an arbiter, denials are
 * routine (whenever the other stack is on air at the wrong moment); the
 * saving branch keys on slot_kind/!slot_heard rather than the done reason,
 * so DENIED is covered by construction - asserted here rather than assumed.
 */
ZTEST(api, test_a_denied_master_transmit_does_not_wedge)
{
	radiant_time_t first;
	radiant_time_t later;
	uint32_t       arms_at_denial;
	uint32_t       tx_ok_before;

	open_channel((uint8_t)ANTW_CHANNEL_TYPE_MASTER);

	/* One good slot, so the channel is anchored and transmitting. */
	host_writes_broadcast();
	first = run_one_master_slot();
	tx_ok_before = fake_radio_stats()->arms_tx;
	zassert_true(tx_ok_before > 0u, "setup: nothing was ever transmitted");

	/* Now the arbiter takes the next several slots. force_arm_repeat
	 * covers the synchronous refusal - the harder case, completing inside
	 * the very pass that posted the request. */
	arms_at_denial = fake_radio_arm_count();
	fake_radio_force_arm_repeat(RADIANT_RADIO_EDENIED, 3u);
	host_writes_broadcast();
	run_virtual_us(4u * (uint64_t)PERIOD_US);
	drain();

	/* The wedge signature is unbounded arm attempts inside one period - a
	 * healthy dongle makes a handful, the hot loop made thousands. Bound
	 * is deliberately loose: a liveness assertion, not a scheduling one. */
	zassert_true(fake_radio_arm_count() - arms_at_denial < 200u,
		     "%u arm attempts in four periods: the post -> refuse -> "
		     "complete -> post loop is back",
		     fake_radio_arm_count() - arms_at_denial);

	zassert_true(radiant_api_stats_get()->slots_denied > 0u,
		     "the denied transmits were not counted as denials");
	zassert_equal(0u, radiant_api_stats_get()->slots_missed,
		     "a denied master transmit was charged to the miss counter");

	/* The channel is still due to transmit in the future, not pinned to a
	 * past instant. */
	later = radiant_channel_next_slot(CH);
	zassert_not_equal(RADIANT_TIME_NEVER, later, "the master left the air");
	zassert_true(later > first,
		     "a denied slot left t_next in the past - the wedge");

	/* When the air comes back, so does the master. */
	host_writes_broadcast();
	run_virtual_us(3u * (uint64_t)PERIOD_US);
	zassert_true(fake_radio_stats()->arms_tx > tx_ok_before,
		     "the master never transmitted again after the denials");

	end_of_test();
}

/*
 * A denied tracked window is invisible to the host. EVENT_RX_FAIL says "the
 * window ran and nothing was there"; a denial says "us", and a host acting
 * on it acts wrongly (Zwift tears down a channel that was never in
 * trouble). Twelve denials in a row is 1.5x what RX_FAIL_TO_SEARCH allows in
 * misses, so folding the two together would visibly drop the channel to
 * SEARCHING.
 */
/*
 * The control, and not optional: "denials add no missed slot" is only
 * meaningful if ordinary empty windows over the same span add the same
 * amount - a quiet tracked channel legitimately misses slots regardless.
 * The claim is that denials are no different, not that the count is zero.
 * (Written as its own test because the original version asserted an
 * absolute zero, failed on an unrelated miss, and cost an afternoon of
 * chasing the wrong mechanisms before anyone compared against no-denial.)
 */
static uint32_t api_tracked_quiet_run(bool deny)
{
	const struct fake_radio_arm *w;
	uint16_t                     dev_a;
	uint32_t                     missed_before;

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_network_address_set(0u, ant_plus_key));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_assign(CH,
					  (uint8_t)ANTW_CHANNEL_TYPE_SLAVE_RX_ONLY,
					  0u, 0u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_id_set(CH, 0u, 0u, 0u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_period_set(CH, RADIANT_CHANNEL_PERIOD_ANT_PLUS));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_radio_freq_set(CH, RF_INDEX));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR, antr_channel_open(CH));

	w = armed_search_window();
	air_device(w, 0u, 0x0500u, 0x0Bu, 1000u, &dev_a);
	fake_radio_advance_to(w->t_close + 1u);
	drain();
	zassert_equal(RADIANT_CH_STATE_TRACKING, radiant_channel_state_get(CH),
		      "setup: the channel did not acquire");

	missed_before = radiant_api_stats_get()->sched_missed;
	if (deny) {
		fake_radio_force_terminal_repeat(RADIANT_RADIO_STATUS_DENIED, 64u);
	}
	run_virtual_us(12u * (uint64_t)PERIOD_US);
	drain();

	return radiant_api_stats_get()->sched_missed - missed_before;
}

ZTEST(api, test_a_quiet_tracked_channel_misses_the_same_either_way)
{
	uint32_t quiet = api_tracked_quiet_run(false);

	end_of_test();
	(void)antr_stack_reset();
	memset(msglog, 0, sizeof(msglog));
	n_msg = 0u;

	/* The number itself isn't the assertion; that denying every window
	 * doesn't add to it is - a denial is not a miss. */
	zassert_equal(quiet, api_tracked_quiet_run(true),
		      "denying every window changed how many slots were missed "
		      "(%u quiet). A denial must be invisible to the miss "
		      "accounting, or a busy second stack reads to a host as a "
		      "sensor going away", quiet);

	end_of_test();
}

ZTEST(api, test_a_denied_tracked_window_is_not_reported_as_an_rx_failure)
{
	const struct fake_radio_arm *w;
	uint16_t                     dev_a;

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_network_address_set(0u, ant_plus_key));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_assign(CH,
					  (uint8_t)ANTW_CHANNEL_TYPE_SLAVE_RX_ONLY,
					  0u, 0u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_id_set(CH, 0u, 0u, 0u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_period_set(CH, RADIANT_CHANNEL_PERIOD_ANT_PLUS));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_radio_freq_set(CH, RF_INDEX));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR, antr_channel_open(CH));

	w = armed_search_window();
	air_device(w, 0u, 0x0500u, 0x0Bu, 1000u, &dev_a);
	fake_radio_advance_to(w->t_close + 1u);
	drain();
	zassert_equal(RADIANT_CH_STATE_TRACKING, radiant_channel_state_get(CH),
		      "setup: the channel did not acquire");

	/* Every operation from here on is accepted and then never granted. */
	fake_radio_force_terminal_repeat(RADIANT_RADIO_STATUS_DENIED, 64u);
	run_virtual_us(12u * (uint64_t)PERIOD_US);
	drain();

	/* Miss/RX_FAIL counts are asserted by the control test above, not
	 * here, as a delta (a quiet channel legitimately misses one slot
	 * regardless of denials). What's left here is that the denial reached
	 * the API layer and that sensor-state stayed put. */
	zassert_true(radiant_api_stats_get()->sched_denied > 0u,
		     "no denial ever reached the API layer, so this test proved "
		     "nothing");
	zassert_equal(0u,
		      count_channel_events(CH,
					   (uint8_t)ANTW_EVENT_RX_FAIL_GO_TO_SEARCH),
		      "denials dropped a live channel back to searching");
	zassert_equal(RADIANT_CH_STATE_TRACKING, radiant_channel_state_get(CH),
		      "the channel stopped tracking a sensor that never went away");
	zassert_true(radiant_channel_denied_count(CH) > 0u,
		     "the denials were not charged to the channel's guard at all");

	end_of_test();
}

/*
 * Acknowledged data stays reported as acknowledged data, and a truncated
 * inbound burst doesn't turn every later exchange into a burst. in_pkts is
 * only cleared on a packet carrying LAST, so a burst whose final packet is
 * missed used to leave it non-zero forever, misreporting every later
 * message as MESG_BURST_DATA - a host-facing lie. The abandoned-burst
 * watchdog now clears it.
 */
ZTEST(api, test_the_second_acknowledged_data_is_reported_as_acknowledged_data)
{
	uint8_t        frame[AIR_LEN];
	radiant_time_t t_tx;
	uint32_t       i;

	open_channel((uint8_t)ANTW_CHANNEL_TYPE_MASTER);

	for (i = 0u; i < 2u; i++) {
		host_writes_broadcast();
		t_tx = run_one_master_slot();

		(void)build_peer_frame(frame, RADIANT_CTRL_BURST_LAST_SEQ0,
				       peer_payload);
		zassert_equal(RADIANT_RADIO_OK_RC,
			      fake_radio_air_frame(t_tx +
						   RADIANT_TRANSFER_SLOT_REPLY_US,
						   frame, AIR_LEN));
		fake_radio_advance_to(t_tx + RADIANT_TRANSFER_SLOT_REPLY_US +
				      RADIANT_TRANSFER_REPLY_US + 10u);
	}

	drain();
	zassert_equal(2u, count_msgs((uint8_t)ANTW_MESG_ACKNOWLEDGED_DATA_ID),
		      "the second acknowledged message was reported as "
		      "something else");
	zassert_equal(0u, count_msgs((uint8_t)ANTW_MESG_BURST_DATA_ID));

	/* A burst that stops after its first packet: 0x82 isn't LAST, so
	 * in_pkts goes to one and nothing on the air clears it. */
	host_writes_broadcast();
	t_tx = run_one_master_slot();
	(void)build_peer_frame(frame, RADIANT_CTRL_BURST_SEQ0, peer_payload);
	zassert_equal(RADIANT_RADIO_OK_RC,
		      fake_radio_air_frame(t_tx + RADIANT_TRANSFER_SLOT_REPLY_US,
					   frame, AIR_LEN));
	fake_radio_advance_to(t_tx + RADIANT_TRANSFER_SLOT_REPLY_US +
			      RADIANT_TRANSFER_REPLY_US + 10u);
	drain();
	zassert_equal(1u, count_msgs((uint8_t)ANTW_MESG_BURST_DATA_ID),
		      "the truncated burst's first packet was not reported as "
		      "burst data");

	/* The peer never sends the last packet. A deadline's worth of the
	 * radio's clock, and housekeeping abandons the inbound transfer. */
	fake_radio_advance(((uint64_t)RADIANT_API_XFER_WATCHDOG_MS * 1000u) + 1000u);
	k_sleep(K_MSEC(3 * RADIANT_API_HOUSEKEEP_MS));

	host_writes_broadcast();
	t_tx = run_one_master_slot();
	(void)build_peer_frame(frame, RADIANT_CTRL_BURST_LAST_SEQ0, peer_payload);
	zassert_equal(RADIANT_RADIO_OK_RC,
		      fake_radio_air_frame(t_tx + RADIANT_TRANSFER_SLOT_REPLY_US,
					   frame, AIR_LEN));
	fake_radio_advance_to(t_tx + RADIANT_TRANSFER_SLOT_REPLY_US +
			      RADIANT_TRANSFER_REPLY_US + 10u);
	drain();

	zassert_equal(3u, count_msgs((uint8_t)ANTW_MESG_ACKNOWLEDGED_DATA_ID),
		      "an acknowledged message after a truncated burst was "
		      "still reported as burst data");

	end_of_test();
}
