/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The on-air half of the zero-cost claim, as an assertion rather than a shell
 * script.
 *
 * The `zero-cost` CI job compares two builds' section sizes and symbol sets,
 * which is what "costs no flash" means. It cannot see the air. This file is the
 * other half: with CONFIG_RADIANT_SEC compiled in and every switch off at
 * runtime, the recorded fake_radio TX bodies and addresses must be exactly what
 * they are without it.
 *
 * These tests join the `api` suite rather than declaring one of their own, so
 * they inherit its setup - one antr_init() for the image, antr_stack_reset()
 * per test - and so they run in BOTH twister scenarios for this application:
 * once with the feature absent and once with it present. That is the only shape
 * that can catch "the transform ran when nothing asked it to", because a suite
 * which only ever runs one way has nothing to compare against.
 */

#include <zephyr/ztest.h>
#include <string.h>

#include "ant_radio.h"
#include "ant_wire.h"

#include <radiant_core/radiant_channel.h>
#include <radiant_core/radiant_frame.h>
#include <radiant_core/radiant_radio_hal.h>
#include <radiant_core/radiant_sec.h>
#include <radiant_core/radiant_transfer.h>

#include "fake_radio.h"

#define SEC_CH        0u
#define SEC_DEVNUM    0x3A17u
#define SEC_DEVTYPE   0x0Bu
#define SEC_TRANSTYPE 0x05u
#define SEC_RF        57u

/*
 * A transmitted body is [trans_type][ctrl][d0..d7] - the 2-byte header of the
 * tracking frame configuration, which every transmitter uses. The host's eight
 * payload bytes therefore start at body[2], NOT at body[1]: byte [1] is the ANT
 * control byte and has never been payload.
 *
 * This matters here more than it does elsewhere, because byte [1] OF THE
 * PAYLOAD - body[3] - is the byte radiant_sec takes ownership of on a secured
 * channel. Reading the body off by one would make that ownership look like it
 * had leaked one byte to the left, into a header field the transform must never
 * touch.
 */
#define SEC_BODY_D0  2u
#define SEC_BODY_LEN 10u

/*
 * Chosen so that every byte is distinguishable from a zero, from a counter and
 * from a tag byte.
 *
 * Byte [0] is 0x05, INSIDE the default secured page range of 0x01..0x0F. That
 * matters for the acknowledgement test below, which is only meaningful if the
 * page it sends is one the transform actually touches - and 0x10, the obvious
 * choice, is one past the top of the default range.
 */
static const uint8_t sec_payload[8] = {
	0x05, 0xA5, 0xC3, 0x5A, 0x77, 0x18, 0x2B, 0x9E,
};

/* The ANT+ network key. What it derives is A6 C5 - the network ADDRESS, which
 * is what goes on the air and what radiant_frame_addr() takes. The key's own
 * first two bytes (B9 A5) never appear in a frame, and confusing the two is the
 * easiest way to write an address assertion that fails for a reason that has
 * nothing to do with what it was testing. */
static const uint8_t sec_net_key[8] = {
	0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45,
};

static const uint8_t sec_net_addr[RADIANT_NET_ADDR_LEN] = {
	RADIANT_NET_ADDR_ANT_PLUS_0,
	RADIANT_NET_ADDR_ANT_PLUS_1,
};

static void sec_open(uint8_t type)
{
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_network_address_set(0u, sec_net_key));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_assign(SEC_CH, type, 0u, 0u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_id_set(SEC_CH, SEC_DEVNUM, SEC_DEVTYPE,
					  SEC_TRANSTYPE));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_period_set(SEC_CH,
					      RADIANT_CHANNEL_PERIOD_ANT_PLUS));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_radio_freq_set(SEC_CH, SEC_RF));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_open(SEC_CH));
}

static void sec_close(void)
{
	(void)antr_channel_close(SEC_CH);
	(void)antr_channel_unassign(SEC_CH);
}

static const struct fake_radio_arm *sec_last_tx(void)
{
	uint32_t i = fake_radio_arm_count();

	while (i-- > 0u) {
		const struct fake_radio_arm *a = fake_radio_arm(i);

		if (a != NULL && a->kind == FAKE_RADIO_ARM_TX &&
		    a->rc == RADIANT_RADIO_OK_RC) {
			return a;
		}
	}
	return NULL;
}

/* A master's own broadcast is 0x0A and its acknowledgement is 0xF2, so the
 * control byte is how the two are told apart in the arm log. */
static const struct fake_radio_arm *sec_last_tx_with_ctrl(uint8_t ctrl)
{
	uint32_t i = fake_radio_arm_count();

	while (i-- > 0u) {
		const struct fake_radio_arm *a = fake_radio_arm(i);

		if (a != NULL && a->kind == FAKE_RADIO_ARM_TX &&
		    a->rc == RADIANT_RADIO_OK_RC && a->body[1] == ctrl) {
			return a;
		}
	}
	return NULL;
}

static void sec_write(void)
{
	uint8_t buf[8];

	memcpy(buf, sec_payload, sizeof(buf));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_broadcast_message_tx(SEC_CH, 8u, buf));
}

/*
 * Run the currently armed master slot out, then run its turnaround out too.
 *
 * Both halves are needed before the next slot can be posted: api_pump_locked()
 * skips a channel whose scheduler slot is still pending, and between a master's
 * transmit and its turnaround closing the turnaround IS that slot. This is
 * radiant_core/tests/api/src/test_api.c's run_one_quiet_period() reduced to the
 * part these tests need, and it is a copy rather than a shared helper because
 * the two files are testing different things about the same sequence.
 */
static void sec_run_slot(void)
{
	const struct fake_radio_arm *tx = sec_last_tx();
	radiant_time_t               t_sync;

	zassert_not_null(tx, "no master slot was armed");
	t_sync = tx->t_sync_at;

	fake_radio_advance_to(t_sync + 1u);
	zassert_true(tx->terminated, "the master's slot did not complete");
	fake_radio_advance_to(t_sync + RADIANT_TRANSFER_SLOT_REPLY_US +
			      RADIANT_TRANSFER_ACK_GUARD_US + 10u);
}

/*
 * Get the channel to a slot that actually carries the host's payload.
 *
 * A MASTER'S FIRST SLOT AFTER OPEN CARRIES ZEROS, by documented decision:
 * api_post_master_tx() transmits from the first slot with or without host data,
 * because the ANT pattern is that the host paces its writes off EVENT_TX and a
 * master that waited for a write would deadlock with every host library there
 * is. So the write lands on the SECOND slot, and a test that inspected the
 * first would be reading an unwritten buffer and calling the zeros a transform.
 */
static const struct fake_radio_arm *sec_slot_with_payload(void)
{
	sec_write();
	sec_run_slot();
	sec_write();
	return sec_last_tx();
}

ZTEST(api, test_sec_a_masters_body_is_the_hosts_payload_when_nothing_is_on)
{
	const struct fake_radio_arm *tx;

	sec_open((uint8_t)ANTW_CHANNEL_TYPE_MASTER);
	tx = sec_slot_with_payload();
	zassert_not_null(tx, "the master armed no transmit");

	/*
	 * THE ASSERTION. The frame body is [trans_type][ctrl][8 payload bytes],
	 * and the payload must be the host's, byte for byte - not a counter in
	 * byte [1], not a tag in byte [7], not ciphertext in [2..6].
	 *
	 * With CONFIG_RADIANT_SEC=n this is trivially true and the test is a
	 * regression guard. With =y it is the claim: the transform is compiled
	 * in, and a channel nobody keyed and nobody configured transmits
	 * exactly what it would have transmitted without it.
	 */
	zassert_equal(SEC_BODY_LEN, tx->body_len, "body is %u bytes, not %u",
		      (unsigned)tx->body_len, (unsigned)SEC_BODY_LEN);
	zassert_equal(SEC_TRANSTYPE, tx->body[0], "the header moved");
	zassert_mem_equal(&tx->body[SEC_BODY_D0], sec_payload, 8u,
			  "a channel with no key and no switches changed the "
			  "bytes on the air");
	sec_close();
}

ZTEST(api, test_sec_a_retransmission_is_byte_identical)
{
	const struct fake_radio_arm *tx;
	uint8_t                      first[8];
	uint8_t                      i;

	/*
	 * A master retransmits its current body every slot whether or not the
	 * host wrote a new one. If a transform ever wrote through to the host's
	 * buffer instead of working on a copy, the second slot would encrypt
	 * already-encrypted bytes - so a receiver would decode the first slot
	 * and nothing after it, which is a failure that looks exactly like a
	 * range problem.
	 */
	sec_open((uint8_t)ANTW_CHANNEL_TYPE_MASTER);
	tx = sec_slot_with_payload();
	zassert_not_null(tx);
	memcpy(first, &tx->body[SEC_BODY_D0], sizeof(first));
	zassert_mem_equal(first, sec_payload, 8u, "the first slot already "
			  "differs; the wrong slot is being inspected");

	for (i = 0u; i < 4u; i++) {
		sec_run_slot();
		tx = sec_last_tx();
		zassert_not_null(tx);
		zassert_mem_equal(&tx->body[SEC_BODY_D0], first, 8u,
				  "slot %u differs from the first without the "
				  "host writing anything", (unsigned)i);
	}
	sec_close();
}

ZTEST(api, test_sec_the_on_air_address_is_the_one_the_channel_id_derives)
{
	struct radiant_channel_id id = {
		.device_number = SEC_DEVNUM,
		.device_type = SEC_DEVTYPE,
		.trans_type = SEC_TRANSTYPE,
	};
	const struct fake_radio_arm *tx;
	uint8_t                      want[RADIANT_RADIO_ADDR_MAX];

	/*
	 * The address path is untouched by design - v1 is payload transforms
	 * and nothing else - and this is that design decision as an assertion.
	 * X_PRIV would have rewritten exactly these bytes, every 128 seconds,
	 * on every node; its withdrawal is what makes this test writable at
	 * all.
	 *
	 * A master's own transmit is the address to assert on, because it is
	 * derived once from the channel ID and never negotiated. A slave's
	 * receive filters would drag in whichever frame configuration the
	 * search happens to be sweeping with, and pinning that here would make
	 * an unrelated search change fail as a security regression.
	 */
	zassert_equal((int)RADIANT_FRAME_ADDR_LEN_TRACKING,
		      radiant_frame_addr(RADIANT_FRAME_CFG_TRACKING,
					 sec_net_addr, &id, want,
					 sizeof(want)));

	sec_open((uint8_t)ANTW_CHANNEL_TYPE_MASTER);
	tx = sec_slot_with_payload();
	zassert_not_null(tx);

	zassert_equal(RADIANT_FRAME_ADDR_LEN_TRACKING, tx->addr_len,
		      "the transmit changed address length");
	zassert_mem_equal(tx->addr, want, RADIANT_FRAME_ADDR_LEN_TRACKING,
			  "the address on the air is not the one the channel "
			  "ID derives; a payload transform must not touch the "
			  "address path");
	sec_close();
}

/*
 * The peer's frame, hand-assembled from docs/ant-radio-link.md rather than run
 * through the encoder under test:
 *
 *     A6 C5 | dl dh | dtype | ttype | ctrl | d0..d7
 */
#define SEC_AIR_LEN 15u

static void sec_build_peer_frame(uint8_t *out, uint8_t ctrl)
{
	out[0] = RADIANT_NET_ADDR_ANT_PLUS_0;
	out[1] = RADIANT_NET_ADDR_ANT_PLUS_1;
	out[2] = (uint8_t)(SEC_DEVNUM & 0xFFu);
	out[3] = (uint8_t)(SEC_DEVNUM >> 8);
	out[4] = SEC_DEVTYPE;
	out[5] = SEC_TRANSTYPE;
	out[6] = ctrl;
	memset(&out[7], 0x5Au, 8u);
}

ZTEST(api, test_sec_an_acknowledgement_carries_what_the_broadcast_put_on_air)
{
	const struct fake_radio_arm *bcast;
	const struct fake_radio_arm *reply;
	uint8_t                      on_air[8];
	uint8_t                      frame[SEC_AIR_LEN];
	radiant_time_t               t_tx;

	/*
	 * THE ACKNOWLEDGEMENT PATH IS THE ONE THAT LEAKS.
	 *
	 * A master answers a slave's acknowledged data with its own broadcast
	 * buffer - measured, on every exchange ever captured. That buffer is the
	 * host's PLAINTEXT. Under X_CONF, sending it verbatim hands a passive
	 * listener the plaintext of the very page whose ciphertext it heard
	 * 1560 us earlier: a two-time pad, given away voluntarily, on a channel
	 * whose whole purpose is to not do that.
	 *
	 * The fix is to answer with the ciphertext already built for the current
	 * counter - same nonce, same plaintext, so byte-for-byte the same bytes
	 * that went out in the broadcast slot. That equality is the assertion,
	 * and it is a true statement in BOTH twister scenarios: without the
	 * feature both are the plaintext, with it both are the ciphertext. A
	 * test that asserted "the reply is not the plaintext" could only run one
	 * way and would not catch the reply drifting from the broadcast.
	 */
	sec_open((uint8_t)ANTW_CHANNEL_TYPE_MASTER);

#if defined(CONFIG_RADIANT_SEC)
	{
		static const uint8_t root[16] = {
			0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
			0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
		};

		/* Keyed through radiant_sec directly: the host serial surface
		 * that would normally carry these is P6, and this assertion is
		 * about the air rather than about how the key arrived. */
		zassert_equal(RADIANT_SEC_OK,
			      radiant_sec_set_key(SEC_CH, root, 128,
						  SEC_DEVNUM));
		zassert_equal(RADIANT_SEC_OK,
			      radiant_sec_set_devnum(SEC_CH, SEC_DEVNUM));
		zassert_equal(RADIANT_SEC_OK,
			      radiant_sec_configure(SEC_CH,
						    RADIANT_SEC_SW_CONF, 2u,
						    RADIANT_SEC_PAGE_LO_DEFAULT,
						    RADIANT_SEC_PAGE_HI_DEFAULT));
		zassert_equal(RADIANT_SEC_OK,
			      radiant_sec_set_epoch(SEC_CH, 1u, 0u, 8182u));
		zassert_true(radiant_sec_channel_is_secured(SEC_CH),
			     "the channel did not arm; the rest of this test "
			     "would assert nothing");
	}
#endif

	sec_write();
	sec_run_slot();
	sec_write();

	bcast = sec_last_tx();
	zassert_not_null(bcast);
	t_tx = bcast->t_sync_at;
	memcpy(on_air, &bcast->body[SEC_BODY_D0], sizeof(on_air));

#if defined(CONFIG_RADIANT_SEC)
	/* sec_payload[0] is inside the default secured range, so this really is
	 * a transformed page rather than one that fell through the range gate
	 * and only looks untouched. */
	zassert_true(memcmp(&on_air[2], &sec_payload[2], 5u) != 0,
		     "the broadcast went out in the clear; the exchange below "
		     "would then prove nothing");
#endif

	/* Let the master's own slot and its turnaround exist, then speak into
	 * the turnaround as a slave sending acknowledged data. */
	fake_radio_advance_to(t_tx + 1u);
	sec_build_peer_frame(frame, RADIANT_CTRL_BURST_LAST_SEQ0);
	zassert_equal(RADIANT_RADIO_OK_RC,
		      fake_radio_air_frame(t_tx + RADIANT_TRANSFER_SLOT_REPLY_US,
					   frame, SEC_AIR_LEN));
	fake_radio_advance_to(t_tx + RADIANT_TRANSFER_SLOT_REPLY_US +
			      RADIANT_TRANSFER_REPLY_US + 10u);

	reply = sec_last_tx_with_ctrl(RADIANT_CTRL_ACK_LAST_SEQ1);
	zassert_not_null(reply, "the master did not answer 0xA2 with 0xF2");
	zassert_mem_equal(&reply->body[SEC_BODY_D0], on_air, sizeof(on_air),
			  "the acknowledgement did not carry the bytes the "
			  "broadcast slot put on the air");
	sec_close();
}

ZTEST(api, test_sec_an_unsecured_channel_reports_clear)
{
	/* "Clear" is the honest word for a channel with no transform on it, and
	 * it is a different statement from "unverified". A host that cannot
	 * tell those apart cannot act on either. */
	zassert_equal(RADIANT_SEC_VERDICT_CLEAR,
		      radiant_sec_last_verdict(SEC_CH));
	zassert_false(radiant_sec_channel_is_secured(SEC_CH));
}
