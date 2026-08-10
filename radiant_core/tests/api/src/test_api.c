/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_api.c - the composition, which is the part that was never tested.
 *
 * Provenance: clean-room. Written against src/ant_radio.h, the radiant_core
 * headers, docs/spike-b-part2-results.md (every microsecond figure asserted
 * here) and archive/captures/radio/2026-08-09-spike-b2-runA-burst-seq.log (the
 * reference trace of a working master). Nothing here derives from sdk-ant, from
 * libant.a, from disassembly of any binary, or from any adopter-gated ANT+
 * device profile document. See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * Why this file exists
 * ---------------------------------------------------------------------------
 * radiant_api.c is 2700 lines and had zero ztest coverage. Every module below
 * it is tested; the composition was not - and the whole master-exchange path
 * lives in the composition. The bug that motivated this suite is exactly a
 * composition bug: a master answered a slave's first acknowledged exchange on a
 * channel and none after it, with every module underneath behaving correctly in
 * isolation.
 *
 * ---------------------------------------------------------------------------
 * Two constraints, both learned the hard way
 * ---------------------------------------------------------------------------
 *   1. NEVER k_sleep() WHILE A FAKE_RADIO OPERATION IS ARMED. ztest runs at a
 *      cooperative priority and radiant_api.c's event thread at 6, so the event
 *      thread runs only when this thread blocks - which makes a sleep the one
 *      place the suite loses control of the ordering. Where the event thread IS
 *      the subject (the housekeeping watchdog), the sleep is deliberate, the
 *      radio is quiet first, and it says so.
 *   2. THIS SUITE CANNOT REPRODUCE H1, the thread-against-interrupt race in
 *      radiant_sched.c. fake_radio delivers callbacks synchronously on the
 *      calling thread, so "an ISR preempts a thread-context pass" is
 *      unreachable in the mock by construction. What this suite proves is that
 *      the logic closes the loop; the bench proves the concurrency does. Saying
 *      so here is the point - a green run is not evidence about H1 either way.
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
#include <radiant_core/radiant_transfer.h>

#include "ant_radio.h"
#include "ant_wire.h"

/* ---------------------------------------------------------------------------
 * The channel under test
 *
 * Spike A's and Spike B's own sensor: #14871 (0x3A17), device type 0x0B
 * (bicycle power), transmission type 5. Using the real one means a byte
 * comparison against a capture line needs no translation.
 * ---------------------------------------------------------------------------
 */
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

/* ---------------------------------------------------------------------------
 * The bridge, which in this image is us
 *
 * antr_on_message() is resolved at link time and exactly one translation unit
 * defines it. In the firmware that is src/ant_serial_bridge.c; here it is this
 * file, and it records rather than serialises. Everything the host would see
 * comes through here, which makes it the only honest place to assert on
 * "what the host was told".
 * ---------------------------------------------------------------------------
 */

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

/*
 * Deliver whatever the core has queued, from THIS thread.
 *
 * radiant_event_drain() is public and re-entrant-safe, and calling it here
 * rather than sleeping to let the event thread do it is what keeps every
 * assertion below deterministic. See constraint 1 in the header.
 */
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
 * antr_init() creates a Zephyr thread and must run once for the suite, so the
 * per-test reset is antr_stack_reset() plus a fresh mock.
 *
 * k_sched_lock() around the mock half is not decoration: fake_radio_reset()
 * puts the HAL back to pre-init, and the event thread calling
 * radiant_radio_now() in that window would be asking a radio that does not
 * exist. Interrupts are irrelevant here - the mock has none - so locking the
 * scheduler is exactly the right size of exclusion.
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

/*
 * The end-of-test pair. The channel is closed first, because unlike the module
 * suites this one leaves a live channel behind that would keep the radio armed
 * for ever - which is the correct behaviour and would make an unconditional
 * is_idle() assertion a false alarm.
 */
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

/* The last accepted transmit carrying this control byte. A master's own
 * broadcast is 0x0A and its acknowledgement is 0xF2, so this is how the two
 * are told apart in the arm log without threading any state through. */
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
 * test. A test that built its expected bytes with the code under test would
 * assert that the module agrees with itself. Layout from
 * docs/ant-radio-link.md, confirmed byte for byte by Spike A:
 *
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

/*
 * What a real host does on EVENT_TX: write the next payload. This is the
 * scenario rather than a workaround for one - the ANT pattern is that the host
 * paces its writes off EVENT_TX, and it is also what pumps a scheduling pass in
 * thread context, which is the half of H1's collision the mock can model.
 */
static void host_writes_broadcast(void)
{
	uint8_t buf[8];

	memcpy(buf, bcast_payload, sizeof(buf));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_broadcast_message_tx(CH, 8u, buf));
}

/*
 * Run the master's own slot to completion and return the t_sync it achieved.
 *
 * The caller has already written a payload, so a transmit is posted and armed;
 * this advances just past it and no further, so that the turnaround the
 * completion arms is inspectable before anything is injected into it.
 */
static radiant_time_t run_one_master_slot(void)
{
	const struct fake_radio_arm *tx = last_tx_with_ctrl(RADIANT_CTRL_BROADCAST);
	radiant_time_t               t_sync;

	/*
	 * ARMED, NOT TRANSMITTED. fake_radio records an arm at the instant the
	 * call is made, and the host's write pumps a pass that arms the slot
	 * synchronously - so the record exists long before any of it is on the
	 * air, and a helper that waited for a NEW record would wait for ever.
	 * The record is updated in place as the operation runs, which is what
	 * makes `terminated` the honest question to ask.
	 */
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

	/* Just past its terminal event, which is what arms the turnaround, and
	 * no further - so the turnaround is inspectable before anything is
	 * injected into it. */
	fake_radio_advance_to(t_sync + 1u);
	zassert_true(tx->terminated, "the master's slot did not complete");
	return t_sync;
}

/*
 * One whole period with nothing from the slave.
 *
 * THE TURNAROUND HAS TO CLOSE BEFORE THE NEXT SLOT CAN BE POSTED, and that is
 * not incidental: api_pump_locked() skips any channel whose scheduler slot is
 * still pending, and between a master's transmit and its turnaround's close the
 * turnaround IS that slot. A test that wrote the next payload without running
 * the window out would find nothing armed and blame the pump.
 */
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
 * THE ONE THIS SUITE EXISTS FOR.
 *
 * A master answers a slave's acknowledged data with 0xF2 carrying its own
 * broadcast buffer, 1560 us after the slave's packet - and it does so on EVERY
 * exchange, not only the first. The symptom that started all of this was an
 * sdk-ant slave accepting exchange 1 on a channel and nothing after it.
 *
 * The reference trace is
 * archive/captures/radio/2026-08-09-spike-b2-runA-burst-seq.log, where three
 * standalone exchanges seconds apart (n=1878, n=1891, n=1903) are all A2 -> F2:
 * the sequence bit resets at the start of every transfer, so the reply byte is
 * the same every time and "right for exchange 1, wrong for exchange 2" is
 * falsified by data already on disk.
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

		/*
		 * The turnaround, armed from inside the completion of the
		 * transmit it is placed against. Measured: a slave's reply
		 * t_sync is master t_sync + 2190 us, and the guard band is
		 * sized on the observed range rather than on a standard
		 * deviation.
		 */
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

		/*
		 * And the exchange did not drag the master's phase. A master
		 * owns the slot grid rather than following one, so the frame it
		 * just heard - a slave's reply 2.19 ms INTO its own period -
		 * must not re-anchor anything. In the reference trace the
		 * master's next broadcast is +245914 us after the reply, which
		 * is exactly one period after its own previous broadcast.
		 */
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
 * H3, and it went red before the fix.
 *
 * A listening master's own transmit produced no done() at all. The chain:
 * hal_tx() called the tx callback BEFORE end_armed(); api_sched_tx() posted the
 * turnaround; radiant_sched_request_rx() called drop_slot() on a slot still in
 * flight, which aborted the armed operation and cleared it; end_armed() then
 * returned immediately because nothing was armed any more. Every period, on
 * every listening master.
 *
 * Two things it cost. A channel the host is CLOSING never finished closing if
 * the close raced a master slot, because radiant_channel_on_terminal() is the
 * only route to chan_finish_close(); and radiant_radio_abort() was called on
 * the real RADIO from inside a completed transmit's own ISR, four times a
 * second, purely as a side effect of posting the turnaround.
 */
ZTEST(api, test_the_masters_own_transmit_produces_a_done)
{
	uint32_t before;
	uint32_t slots;

	open_channel((uint8_t)ANTW_CHANNEL_TYPE_MASTER);

	before = radiant_api_stats_get()->sched_dones;

	for (slots = 0u; slots < 4u; slots++) {
		/* The turnaround runs out empty as well, so each period
		 * contributes exactly two completed operations. */
		(void)run_one_quiet_period();
	}

	/*
	 * Two per period: the transmit and the turnaround. The bug produced
	 * exactly one - the turnaround's - so a >= would have passed while
	 * broken and the equality is the whole test.
	 */
	zassert_equal(8u, radiant_api_stats_get()->sched_dones - before,
		      "a listening master's own transmit lost its completion");

	end_of_test();
}

/*
 * H2's fingerprint, from the sniffer side: do the master's own 0x0A broadcasts
 * continue after an exchange?
 *
 * If they stop, a transfer is wedged - api_pump_locked() skips a channel whose
 * transfer engine is not idle, so a stuck engine takes the channel's broadcast
 * slot down with it and the dongle goes quiet while looking perfectly alive.
 * If they continue and only the reply is missing, the fault is in the
 * receive/decide path instead. This is that discriminator, on the desk.
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
 * The H2 watchdog, which exists regardless of root cause.
 *
 * The wedge is synthesised with radiant_sched_cancel(), and that is not a
 * contrivance - it is the exact shape of the real failure. Cancelling is silent
 * for the channel that asked, so no done() arrives; the transfer engine is left
 * mid-reply with nothing armed; api_feed_xfer_terminal() is reachable only from
 * api_sched_done() and only a completion calls that. Before the watchdog, that
 * state was permanent and silent for the life of the process.
 *
 * The sleeps are the one place this suite hands control to the event thread on
 * purpose - housekeeping IS the subject - and the radio is quiet by then,
 * because a wedged channel posts nothing.
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
 * A CONSTANT TRANSMIT TIMING ERROR IS NOT FREE FOR A MASTER, and the asymmetry
 * with the receive side is the whole point of pinning it.
 *
 * radiant_radio_hal.h's "THE FAILURE MODE, WHICH IS SILENT" is about a slave: a
 * constant error in the t_sync it REPORTS cancels out of the period estimate,
 * the drift PLL still locks, and the only cost is an off-centre window. A
 * master is the other case. Its next slot is anchored on the t_sync its last
 * transmit ACHIEVED, so a backend that is systematically late by d puts its
 * frames one period plus d apart - for ever, cumulatively, with nothing
 * anywhere to notice.
 *
 * That is why the nrf backend carries radiant_dbg_tx_err (achieved minus
 * requested, every frame) and why T_SYNC_CAL_TX_US is applied to the ARM path
 * and not only to the report. This test is the core-side half of that claim,
 * and it asserts the exact relationship rather than merely that nothing
 * exploded: a constant offset produces a constant period error of the same
 * size, and the exchange keeps working throughout, which is precisely what
 * makes it invisible on the air.
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
 * The slave side: a background scan
 *
 * Everything above drives a master. This half exists because the bug that cost
 * a whole session lived here and nothing in this suite could see it - the
 * module suites test radiant_search.c's own decisions, and radiant_search.c was
 * right; what was wrong was what radiant_api.c did with the answer.
 * ---------------------------------------------------------------------------
 */

/*
 * A host's broadcast, as it reaches antr_on_message() with lib config asking
 * for the extended tail:
 *
 *     [channel][d0..d7][flag][devnum lo][devnum hi][device type][trans type]
 *
 * The device number is the whole point of a scan - it is how the host tells one
 * sensor from another - so counting messages per device number is the honest
 * question to ask of a discovery mechanism. See radiant_event.h's layout
 * comment; index 9 is the flag byte because the payload is always 8.
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

/*
 * Zwift's actual discovery channel, byte for byte from a USBPcap capture of a
 * real pairing session: ASSIGN_CHANNEL 00 40 00 01 - channel 0, SLAVE_RX_ONLY,
 * network 0, extended assignment ALWAYS_SEARCH - then a wildcard channel ID and
 * an open. This is NOT MESG_OPEN_RX_SCAN_MODE; the capture shows Zwift never
 * sends 0x5B. Writing the assignment out here rather than reusing
 * open_channel() is deliberate: the four bytes ARE the scenario.
 */
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

/*
 * The armed search window, with its filters - which is where the device numbers
 * below come from.
 *
 * A sweep set only matches eight device-number low bytes at a time, so a test
 * that hardcoded a device number would be asserting about the sweep's phase
 * rather than about discovery, and would break the first time the sweep order
 * or the seen-cache steering changed. Reading the low byte out of the window
 * that is actually armed makes the frame match by construction.
 */
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
 * Let the event thread post the next window of the sweep.
 *
 * A SEARCHING channel has nothing host-side to pump it. A master's next slot is
 * posted when the host writes the next payload - that is what every test above
 * relies on - but a sweep's next window comes from api_housekeep() and from
 * nowhere else, so without this the sweep stops dead after one window and the
 * suite would be measuring its own pacing rather than the firmware's.
 *
 * This is the second of the two deliberate sleeps in this file (see constraint 1
 * at the top). It is legitimate for the same reason the housekeeping watchdog's
 * is: the window has just run out, so nothing is armed, and the assertion below
 * states that rather than trusting it.
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
 * THE ONE THE SLAVE HALF OF THIS SUITE EXISTS FOR, and it goes red before the
 * fix in api_search_acquired().
 *
 * A background scan is defined by never leaving: the host opens ONE wildcard
 * channel and expects to be told about every device on the air, indefinitely.
 * api_search_acquired() used to call radiant_channel_on_acquired() for every
 * match regardless of the search mode, and that function knows nothing about
 * modes - it unconditionally sets RADIANT_CH_STATE_TRACKING. The state check at
 * the end of the callback then ended the search. So the very first sensor to
 * answer converted the scan into an ordinary tracked channel locked to that one
 * device, and every other sensor in the room became invisible.
 *
 * The symptom on a real host was exactly this and nothing more: Zwift's pairing
 * screen showed ONE sensor, a different one each run, whichever happened to
 * transmit first.
 *
 * Two devices in the SAME window is the sharp half of the assertion - it needs
 * no second window and no timing argument, because if the channel left the
 * search on the first frame the second one has nowhere to be delivered. The
 * third device in a LATER window then proves the sweep itself kept running,
 * which is the other thing the conversion destroyed.
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

	/*
	 * And the channel is still searching. This is the mechanism rather than
	 * the symptom, and it is what makes the failure above unambiguous: a
	 * scan channel that reads TRACKING has been acquired, and an acquired
	 * channel is one radiant_search_end() away from discovering nothing
	 * else, for the rest of the session.
	 */
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
 * Run virtual time forward while letting the event thread keep up.
 *
 * Both halves are needed and neither is optional. fake_radio fires its events
 * on the calling thread, so a single long advance runs a whole second of radio
 * with no pump in between - the sweep would look starved for a reason that is
 * purely an artefact of the harness. The event thread is where api_housekeep()
 * re-posts the next chunk, and it only runs when this thread blocks.
 *
 * So: step to the next due event (never more than 20 ms at a time), deliver,
 * then yield. This is the one place in this file that sleeps with operations
 * armed, and it is deliberate - the subject of the test IS the interaction
 * between the event thread's pump and a radio that is busy with something else,
 * which is unreachable any other way. The assertions it feeds are lower bounds
 * over a long window rather than exact orderings, which is what makes that
 * safe.
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
 * THE SECOND BUG, AND THE ONE THAT MADE PAIRING FEEL BROKEN EVEN AFTER THE
 * SCAN-MODE FIX.
 *
 * Zwift keeps its wildcard scan channel open ALONGSIDE the channels it has
 * already paired, and expects to go on discovering while they track - that is
 * how a pairing screen adds a second sensor without dropping the first. A
 * USBPcap capture of a real session showed that stopping dead: with one channel
 * receiving at 4 Hz, the scan channel reported ZERO devices for 25 s and then
 * 35 s, across six full scan cycles, including a trainer it had already found
 * twice. The first report after the tracked channel closed arrived 1 ms later,
 * so nothing was slow - the sweep was getting no radio at all.
 *
 * api_post_search_window() posts the sweep as radiant_sched.c's CONTINUOUS form
 * precisely so the scheduler can fit chunks into the gaps between tracked
 * slots, and its comment claims "the sweep gets every microsecond the tracked
 * channels are not using". A tracked channel at 4 Hz uses about 2 ms in 250 ms.
 * This test states that claim as an assertion.
 *
 * Twelve seconds is one and a half full sweeps (32 sets x 260 ms = 8.32 s), so
 * a healthy sweep must reach every set - including whichever one device B sits
 * in - regardless of the phase it happened to be at when tracking started.
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

	/*
	 * Device A alone in the first window, so channel 1 acquires A and
	 * nothing else - B is not on the air yet, which makes which channel
	 * takes which sensor a fact rather than a race.
	 */
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

	/*
	 * end_of_test()'s ending, minus the one violation this test is
	 * guaranteed to provoke. Twelve seconds of a tracked channel plus a
	 * sweep is a few hundred arm calls and the mock records 64, so
	 * FAKE_RADIO_VIOL_LOG_FULL here is the harness running out of room to
	 * write in - not the firmware breaking a contract. Every other code
	 * still fails the test, which is the part that matters.
	 */
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
 * A background scan must still be searching long after an ordinary channel
 * would have given up. This went red before the fix in search_deadline().
 *
 * Found on the bench, not by inspection. With the search counters logged once a
 * second on a DK, a scan channel opened with the DEFAULT timeouts - which is
 * what a host gets when it never sends MESG_SEARCH_TIMEOUT - reported devices
 * happily and then stopped, permanently, at exactly 25 s:
 * radiant_search_n_searching() read 0 from that moment on. The channel had hit
 * the ordinary search deadline, raised RX_SEARCH_TIMEOUT and closed itself,
 * leaving the host holding a discovery channel that could never discover
 * anything again. From the outside that is "the dongle stops finding sensors
 * after a while", which is a symptom nobody would attribute to a timeout the
 * host never asked for.
 *
 * Forty seconds of virtual time is well past the 25 s default, and the
 * assertion is the channel STATE rather than a device report, so a quiet bench
 * cannot make it pass by accident.
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
 * The other side of the same coin, and the reason the fix is a mode check
 * rather than "never acquire from a search callback".
 *
 * An ordinary wildcard slave - no extended assignment - is ACQUIRE mode, and it
 * MUST leave the search and start tracking the first device it matches. That is
 * how every normal pairing works. A fix that made the scan case work by
 * removing the acquire path would break this, silently, and the dongle would
 * pair with nothing at all.
 */
ZTEST(api, test_a_plain_wildcard_slave_still_acquires_and_tracks)
{
	const struct fake_radio_arm *w;
	uint16_t                     dev_a;

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_network_address_set(0u, ant_plus_key));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_lib_config_set((uint8_t)ANTW_LIB_CONFIG_ALL_EXT_FIELDS));
	/* Same channel, same wildcard ID - the ONLY difference from the test
	 * above is the extended assignment byte. */
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

	/* And it took the device's identity with it, which is what makes the
	 * next period a tracked slot rather than more searching. */
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
 * A TRACKED CHANNEL OWES ITS NEXT WINDOW IMMEDIATELY, NOT ONE PUMP LATER.
 *
 * This is the invariant a background scan lives or dies on, and it is worth
 * stating as a mechanism rather than as an outcome because the outcome is not
 * reproducible here at all.
 *
 * radiant_sched.c ends a scan chunk at t_back(committed, lead), so a sweep and a
 * tracked channel interleave exactly - but only against work it can already
 * see. If a tracked slave's next window waits for api_pump_locked() on the
 * event thread, then in the instant after its previous window completes there
 * is no committed work, the scan is armed with NO end, and the pump's later
 * post preempts it. On the bench that costs everything: 109 scan chunks armed
 * unbounded against 6 bounded, one preemption per chunk, and frames_ok frozen
 * at 15 for fourteen seconds while the sweep ran flat out and decoded nothing.
 * The dongle stops finding sensors the moment one is paired.
 *
 * Why this test asserts the POST and not the discovery: the mock's completions
 * are synchronous, so the pump follows the callback closely enough that
 * test_the_scan_keeps_finding_devices_while_another_channel_tracks passes with
 * or without the fix. It cannot see the gap. The gap is real on hardware, where
 * arm_next() runs inside the completion callback and the event thread has not
 * been scheduled yet. So this pins the thing that differs - that the slot is
 * refilled by the completion itself - which is checkable here and is precisely
 * what supplies the scheduler the fact it was missing.
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

	/*
	 * One pump, so the FIRST tracked window is posted and armed - that one
	 * legitimately comes from the pump, because nothing has completed yet.
	 * The question this test asks is about its successor.
	 */
	run_housekeeping();

	/*
	 * Now run that window to completion and stop. drain() delivers queued
	 * host messages; it deliberately does NOT sleep, so the event thread's
	 * housekeeping pump has not run again. Anything pending on this channel
	 * now was put there by the completion itself.
	 */
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

/*
 * Acknowledged data is reported as acknowledged data, exchange after exchange -
 * and a truncated inbound burst does not turn every later one into a burst.
 *
 * in_pkts is only cleared on a packet carrying LAST, so an inbound multi-packet
 * burst whose final packet is missed left it non-zero for ever, and every later
 * acknowledged message on that channel was reported to the host as
 * MESG_BURST_DATA with a sequence number counted from a burst that ended
 * minutes earlier. Nothing different went on the air; it was a host-facing lie,
 * which is worse rather than better. The abandoned-burst watchdog is what
 * clears it now.
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

	/* Now a burst that stops after its first packet: 0x82 is not LAST, so
	 * in_pkts goes to one and nothing on the air will ever clear it. */
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
