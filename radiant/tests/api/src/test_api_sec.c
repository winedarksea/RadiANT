/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The on-air half of the zero-cost claim. The `zero-cost` CI job compares
 * section sizes and symbol sets, but can't see the air; this file checks
 * that with CONFIG_RADIANT_SEC compiled in and every switch off at runtime,
 * fake_radio TX bodies and addresses are identical to without it.
 *
 * Joins the `api` suite (rather than its own) to inherit its setup and run
 * in both twister scenarios - feature absent and present - which is what
 * catches "the transform ran when nothing asked it to".
 */

#include <zephyr/ztest.h>
#include <string.h>

#include <radiant/ant_radio.h>
#include <radiant/ant_wire.h>
#include "ant_radio_radiant_priv.h"

#include <radiant/radiant_channel.h>
#include <radiant/radiant_frame.h>
#include <radiant/radiant_radio_hal.h>
#include <radiant/radiant_sec.h>
#include <radiant/radiant_transfer.h>

#include "fake_radio.h"

#define SEC_CH        0u
#define SEC_DEVNUM    0x3A17u
#define SEC_DEVTYPE   0x0Bu
#define SEC_TRANSTYPE 0x05u
#define SEC_RF        57u

/*
 * A transmitted body is [trans_type][ctrl][d0..d7], so the host's payload
 * starts at body[2], not body[1] (the ANT control byte). Matters here
 * because payload byte [1] (body[3]) is what radiant_sec owns on a secured
 * channel - an off-by-one read would make that ownership look like it leaked
 * into a header field the transform must never touch.
 */
#define SEC_BODY_D0  2u
#define SEC_BODY_LEN 10u

/* Every byte distinguishable from a zero, a counter, or a tag byte. Byte [0]
 * is 0x05, inside the default secured page range (0x01..0x0F) - matters for
 * the acknowledgement test below, which needs a page the transform actually
 * touches (0x10 would be one past the range). */
static const uint8_t sec_payload[8] = {
	0x05, 0xA5, 0xC3, 0x5A, 0x77, 0x18, 0x2B, 0x9E,
};

/* The ANT+ network key derives address A6 C5 (what radiant_frame_addr()
 * takes and what goes on air) - the key's own first two bytes (B9 A5) never
 * appear in a frame; confusing the two breaks address assertions for the
 * wrong reason. */
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

/* Run the armed master slot out, then its turnaround: api_pump_locked()
 * skips a channel whose scheduler slot is still pending, and the turnaround
 * IS that slot until it closes. A reduced copy of test_api.c's
 * run_one_quiet_period() - separate because the two files test different
 * things about the same sequence. */
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
 * Get the channel to a slot that actually carries the host's payload. A
 * master's first slot after open carries zeros by design -
 * api_post_master_tx() transmits from the first slot regardless, since a
 * master waiting for a write would deadlock every host library. The write
 * lands on the SECOND slot; inspecting the first would read an unwritten
 * buffer and call the zeros a transform.
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

	/* The assertion: body is [trans_type][ctrl][8 payload bytes], byte for
	 * byte the host's. With SEC=n this is a regression guard; with =y
	 * it's the claim - an unkeyed, unconfigured channel transmits exactly
	 * what it would without the transform compiled in. */
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

	/* A master retransmits its current body every slot regardless of new
	 * writes. If a transform wrote through to the host's buffer instead
	 * of a copy, the second slot would encrypt already-encrypted bytes -
	 * a receiver would decode only the first slot, looking like a range
	 * problem. */
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

	/* The address path is untouched by design - v1 is payload transforms
	 * only. Asserting on a master's own transmit rather than a slave's
	 * receive filters avoids dragging in whatever frame config the search
	 * happens to be sweeping with. */
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

/* The peer's frame, hand-assembled from docs/ant-radio-link.md rather than
 * via the encoder under test:
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
	 * The acknowledgement path is the one that leaks. A master answers
	 * acknowledged data with its own broadcast buffer - the host's
	 * plaintext. Sent verbatim under X_CONF, that hands a passive
	 * listener the plaintext of the ciphertext it heard 1560 us earlier -
	 * a voluntary two-time pad. The fix answers with the ciphertext
	 * already built for the current counter, byte-for-byte identical to
	 * the broadcast slot. That equality is true in both twister scenarios
	 * (both plaintext without the feature, both ciphertext with it),
	 * unlike "the reply is not the plaintext" which only runs one way.
	 */
	sec_open((uint8_t)ANTW_CHANNEL_TYPE_MASTER);

#if defined(CONFIG_RADIANT_SEC)
	{
		static const uint8_t root[16] = {
			0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
			0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
		};

		/* Keyed through radiant_sec directly (the host surface is P6)
		 * since this assertion is about the air, not key delivery. */
		int rc[4];

		/* Under api_lock, as every real caller is. */
		k_mutex_lock(&api_lock, K_FOREVER);
		rc[0] = radiant_sec_set_key(SEC_CH, root, 128, SEC_DEVNUM);
		rc[1] = radiant_sec_set_devnum(SEC_CH, SEC_DEVNUM);
		rc[2] = radiant_sec_configure(SEC_CH, RADIANT_SEC_SW_CONF, 2u,
					      RADIANT_SEC_PAGE_LO_DEFAULT,
					      RADIANT_SEC_PAGE_HI_DEFAULT);
		rc[3] = radiant_sec_set_epoch(SEC_CH, 1u, 0u, 8182u);
		k_mutex_unlock(&api_lock);
		for (int i = 0; i < 4; i++) {
			zassert_equal(RADIANT_SEC_OK, rc[i], "setup step %d", i);
		}
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
	/* sec_payload[0] is inside the default secured range, so this is a
	 * transformed page rather than one that only looks untouched. */
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

#if defined(CONFIG_RADIANT_SEC) && defined(CONFIG_RADIANT_SEC_LOCK_CHECK)
/*
 * TODO security item 8 follow-up. A secured master's re-post after a MISSED,
 * DENIED or completed slot used to be made from api_sched_done(), the
 * scheduler's terminal callback (the radio ISR on a direct build, the SWI
 * under MPSL), where radiant_sec_tx_transform() ran without api_lock: the one
 * shared AES key schedule rewritten under whatever thread held it. Now the
 * callback leaves a secured master to the pump, which posts under api_lock.
 * This image's fake radio runs the callback on the test thread without the
 * lock, so the old path counts a violation (and halts a CONFIG_ASSERT build)
 * exactly as the real one would be wrong; the new one counts none, and the
 * denied master still comes back.
 */
ZTEST(api, test_sec_a_denied_secured_master_slot_is_posted_by_the_pump_under_the_lock)
{
	static const uint8_t root[16] = {
		0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
		0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
	};
	const uint32_t period = (uint32_t)radiant_channel_counts_to_us(
		RADIANT_CHANNEL_PERIOD_ANT_PLUS);
	uint32_t       base;
	uint32_t       tx_before;
	int            rc[4];

	sec_open((uint8_t)ANTW_CHANNEL_TYPE_MASTER);
	k_mutex_lock(&api_lock, K_FOREVER);
	rc[0] = radiant_sec_set_key(SEC_CH, root, 128, SEC_DEVNUM);
	rc[1] = radiant_sec_set_devnum(SEC_CH, SEC_DEVNUM);
	rc[2] = radiant_sec_configure(SEC_CH, RADIANT_SEC_SW_CONF, 2u,
				      RADIANT_SEC_PAGE_LO_DEFAULT,
				      RADIANT_SEC_PAGE_HI_DEFAULT);
	rc[3] = radiant_sec_set_epoch(SEC_CH, 1u, 0u, 8182u);
	k_mutex_unlock(&api_lock);
	for (int i = 0; i < 4; i++) {
		zassert_equal(RADIANT_SEC_OK, rc[i], "setup step %d", i);
	}
	zassert_true(radiant_sec_channel_is_secured(SEC_CH));

	/* From here, so the completion of the very first slot counts: that
	 * terminal is where the old code re-posted the master. */
	base = antr_api_lock_violations;
	sec_write();
	sec_run_slot();
	sec_write();
	tx_before = fake_radio_stats()->arms_tx;
	zassert_true(tx_before > 0u, "setup: nothing was ever transmitted");

	/* The arbiter accepts the next slots and then never grants them: the
	 * DENIED terminal arrives in the radio callback (here, inside
	 * fake_radio_advance_to()), which is where the old code re-posted. Then
	 * it gives the air back; the host keeps writing, as a real one does
	 * every period. */
	fake_radio_force_terminal_repeat(RADIANT_RADIO_STATUS_DENIED, 3u);
	for (int i = 0; i < 6; i++) {
		fake_radio_advance_to(radiant_radio_now() + period);
		sec_write();
	}

	zassert_equal(base, antr_api_lock_violations,
		      "a secured master was re-posted outside api_lock (the "
		      "scheduler's terminal callback)");
	/* Three denied arms plus at least one that went out once the air came
	 * back: only the pump can post those, so a master it stops posting
	 * after a denial falls short. */
	zassert_true(fake_radio_stats()->arms_tx >= tx_before + 4u,
		     "the denied secured master never transmitted again "
		     "(%u arms after %u)", fake_radio_stats()->arms_tx,
		     tx_before);
	sec_close();
}
#endif

#if defined(CONFIG_RADIANT_SEC)
/* Did any transmit arm from `from` on carry `pay` as its payload? */
static bool sec_tx_carried(uint32_t from, const uint8_t pay[8])
{
	for (uint32_t i = from; i < fake_radio_arm_count(); i++) {
		const struct fake_radio_arm *a = fake_radio_arm(i);

		if (a != NULL && a->kind == FAKE_RADIO_ARM_TX &&
		    a->body_len >= SEC_BODY_LEN &&
		    memcmp(&a->body[SEC_BODY_D0], pay, 8u) == 0) {
			return true;
		}
	}
	return false;
}

/*
 * Review 2026-11-13 item 5: a keyed master with no epoch (pending) used to put
 * the host's secured page on the air in the clear, every slot, until 0xF3, and
 * to acknowledge with the same plaintext buffer. Now its slots are withheld
 * and it does not acknowledge from a buffer it never built; a page outside the
 * secured range still goes out, and 0xF3 brings the transform in.
 */
ZTEST(api, test_sec_a_pending_master_sends_nothing_in_the_clear)
{
	static const uint8_t root[16] = {
		0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
		0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
	};
	static const uint8_t common[8] = { 0x50u, 0xFFu, 0xFFu, 1, 2, 3, 4, 5 };
	const uint32_t period = (uint32_t)radiant_channel_counts_to_us(
		RADIANT_CHANNEL_PERIOD_ANT_PLUS);
	const struct fake_radio_arm *tx;
	uint8_t                      frame[SEC_AIR_LEN];
	uint8_t                      buf[8];
	uint32_t                     mark;
	radiant_time_t               t_tx;
	int                          rc[3];

	/* The slot armed at open, unkeyed; the host then writes a secured page
	 * and keys the channel while that slot is still ahead. */
	sec_open((uint8_t)ANTW_CHANNEL_TYPE_MASTER);
	tx = sec_last_tx();
	zassert_not_null(tx);
	t_tx = tx->t_sync_at;
	sec_write();
	k_mutex_lock(&api_lock, K_FOREVER);
	rc[0] = radiant_sec_set_key(SEC_CH, root, 128, SEC_DEVNUM);
	rc[1] = radiant_sec_set_devnum(SEC_CH, SEC_DEVNUM);
	rc[2] = radiant_sec_configure(SEC_CH,
				      RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_CONF,
				      2u, RADIANT_SEC_PAGE_LO_DEFAULT,
				      RADIANT_SEC_PAGE_HI_DEFAULT);
	k_mutex_unlock(&api_lock);
	for (int i = 0; i < 3; i++) {
		zassert_equal(RADIANT_SEC_OK, rc[i], "step %d", i);
	}
	zassert_true(radiant_sec_channel_is_secured(SEC_CH));
	mark = fake_radio_arm_count();

	/* Acknowledged data in that slot's turnaround: the ack would carry the
	 * plaintext buffer, so there is none. */
	fake_radio_advance_to(t_tx + 1u);
	sec_build_peer_frame(frame, RADIANT_CTRL_BURST_LAST_SEQ0);
	zassert_equal(RADIANT_RADIO_OK_RC,
		      fake_radio_air_frame(t_tx + RADIANT_TRANSFER_SLOT_REPLY_US,
					   frame, SEC_AIR_LEN));
	fake_radio_advance_to(t_tx + RADIANT_TRANSFER_SLOT_REPLY_US +
			      RADIANT_TRANSFER_REPLY_US + 10u);
	zassert_is_null(sec_last_tx_with_ctrl(RADIANT_CTRL_ACK_LAST_SEQ1),
			"a pending master acknowledged");

	/* Every later slot of the secured page is withheld. */
	for (int i = 0; i < 6; i++) {
		sec_write();
		fake_radio_advance_to(radiant_radio_now() + period);
	}
	zassert_false(sec_tx_carried(mark, sec_payload),
		      "a pending master sent its secured page in the clear");

	/* Outside the secured range: travels as on any channel. */
	memcpy(buf, common, sizeof(buf));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_broadcast_message_tx(SEC_CH, 8u, buf));
	fake_radio_advance_to(radiant_radio_now() + 2u * period);
	zassert_true(sec_tx_carried(mark, common),
		     "a pending master withheld a common page");

	/* 0xF3 arms it, and the secured page goes out transformed. */
	k_mutex_lock(&api_lock, K_FOREVER);
	rc[0] = radiant_sec_set_epoch(SEC_CH, 1u, 0u, 8182u);
	k_mutex_unlock(&api_lock);
	zassert_equal(RADIANT_SEC_OK, rc[0]);
	for (int i = 0; i < 3; i++) {
		sec_write();
		fake_radio_advance_to(radiant_radio_now() + period);
	}
	tx = sec_last_tx();
	zassert_not_null(tx);
	zassert_equal(sec_payload[0], tx->body[SEC_BODY_D0]);
	zassert_true(memcmp(&tx->body[SEC_BODY_D0 + 2u], &sec_payload[2],
			    5u) != 0, "armed, but still in the clear");
	sec_close();
}
#endif

ZTEST(api, test_sec_an_unsecured_channel_reports_clear)
{
	/* "Clear" is the honest word for a channel with no transform on it, and
	 * it is a different statement from "unverified". A host that cannot
	 * tell those apart cannot act on either. */
	zassert_equal(RADIANT_SEC_VERDICT_CLEAR,
		      radiant_sec_last_verdict(SEC_CH));
	zassert_false(radiant_sec_channel_is_secured(SEC_CH));
}

#if defined(CONFIG_RADIANT_SEC)
/*
 * Review 2026-11-13 item 2: a secured MASTER refuses secured-range pages.
 * Its RX shares the epoch and keys its TX uses, so a forged broadcast heard
 * in its reply window - in the last 127 slots before a wrap, one with a chosen
 * counter byte - used to advance that epoch for good, and its own windows
 * reflected back verified. Now ingest drops them before the pump, counted in
 * dropped_direction, while plain pages (slave replies) still reach the host.
 *
 * The fake clock cannot put a master 65 400 slots into an epoch (that is
 * ~4.5 h of fake_radio time), so the near-wrap advance itself is asserted in
 * test_sec.c; here the forged byte is expected + 127, the most an attacker can
 * reach, and the assertions are that it never reaches the pump at all.
 */

uint32_t api_test_count_bcast(uint8_t ch, uint8_t page);

static const uint8_t sec_dir_root[16] = {
	0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
	0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
};

/* A secured master that has transmitted (its TX latched); returns the instant
 * of the slot whose reply window the tests speak into. */
static radiant_time_t sec_dir_master(const struct fake_radio_arm **tx_out)
{
	const struct fake_radio_arm *tx;
	int                          rc[4];

	sec_open((uint8_t)ANTW_CHANNEL_TYPE_MASTER);
	k_mutex_lock(&api_lock, K_FOREVER);
	rc[0] = radiant_sec_set_key(SEC_CH, sec_dir_root, 128, SEC_DEVNUM);
	rc[1] = radiant_sec_set_devnum(SEC_CH, SEC_DEVNUM);
	rc[2] = radiant_sec_configure(SEC_CH, RADIANT_SEC_SW_AUTH, 2u,
				      RADIANT_SEC_PAGE_LO_DEFAULT,
				      RADIANT_SEC_PAGE_HI_DEFAULT);
	rc[3] = radiant_sec_set_epoch(SEC_CH, 1u, 0u, 8182u);
	k_mutex_unlock(&api_lock);
	for (int i = 0; i < 4; i++) {
		zassert_equal(RADIANT_SEC_OK, rc[i], "setup step %d", i);
	}
	zassert_true(radiant_sec_channel_is_secured(SEC_CH));

	sec_write();
	sec_run_slot();
	sec_write();
	tx = sec_last_tx();
	zassert_not_null(tx);
	if (tx_out != NULL) {
		*tx_out = tx;
	}
	return tx->t_sync_at;
}

/* A slave's broadcast with payload `pay`, aired in the master's reply window
 * after slot `t_tx`, and the turnaround run out. */
static void sec_dir_air(radiant_time_t t_tx, const uint8_t pay[8])
{
	uint8_t frame[SEC_AIR_LEN];

	sec_build_peer_frame(frame, RADIANT_CTRL_SLAVE_BROADCAST);
	memcpy(&frame[7], pay, 8u);
	fake_radio_advance_to(t_tx + 1u);
	zassert_equal(RADIANT_RADIO_OK_RC,
		      fake_radio_air_frame(t_tx + RADIANT_TRANSFER_SLOT_REPLY_US,
					   frame, SEC_AIR_LEN));
	fake_radio_advance_to(t_tx + RADIANT_TRANSFER_SLOT_REPLY_US +
			      RADIANT_TRANSFER_REPLY_US + 10u);
}

static void sec_dir_read(struct radiant_sec_stats *st,
			 struct radiant_sec_state *state)
{
	k_mutex_lock(&api_lock, K_FOREVER);
	radiant_sec_pump();
	radiant_sec_get_stats(SEC_CH, st);
	radiant_sec_get_state(SEC_CH, state);
	k_mutex_unlock(&api_lock);
}

ZTEST(api, test_sec_a_master_refuses_a_forged_secured_page)
{
	struct radiant_sec_stats st;
	struct radiant_sec_state state;
	radiant_time_t           t_tx;
	uint8_t                  forged[8] = { 0x05u, 0x00u, 1, 2, 3, 4, 5, 6 };

	t_tx = sec_dir_master(NULL);
	sec_dir_read(&st, &state);
	/* expected + 127: the counter byte that resolves furthest ahead. */
	forged[1] = (uint8_t)(state.expected_index + 127u);

	sec_dir_air(t_tx, forged);
	sec_dir_read(&st, &state);

	zassert_equal(1u, st.dropped_direction,
		      "the forged page was not refused as a master's RX");
	zassert_equal(1u, state.epoch, "the forged page moved the epoch");
	zassert_equal(0u, st.epoch_advances);
	zassert_equal(0u, st.windows_verified + st.windows_unverified,
		      "the forged page reached the window machinery");
	zassert_equal(0u, api_test_count_bcast(SEC_CH, 0x05u),
		      "the forged page reached the host");
	sec_close();
}

ZTEST(api, test_sec_a_masters_own_window_reflected_back_is_not_delivered)
{
	const struct fake_radio_arm *tx;
	struct radiant_sec_stats     st;
	struct radiant_sec_state     state;
	radiant_time_t               t_tx;
	uint8_t                      own[8];

	t_tx = sec_dir_master(&tx);
	memcpy(own, &tx->body[SEC_BODY_D0], sizeof(own));
	zassert_equal(sec_payload[0], own[0], "not a secured page");

	/* The master's own bytes, played back into its own reply window. */
	sec_dir_air(t_tx, own);
	sec_dir_read(&st, &state);

	zassert_equal(1u, st.dropped_direction);
	zassert_equal(0u, st.windows_verified,
		      "a reflected window verified at the master");
	zassert_equal(0u, api_test_count_bcast(SEC_CH, own[0]),
		      "the reflected page reached the host");
	sec_close();
}

ZTEST(api, test_sec_a_plain_slave_broadcast_still_reaches_a_secured_master)
{
	struct radiant_sec_stats st;
	struct radiant_sec_state state;
	radiant_time_t           t_tx;
	/* 0x50, a common page: outside every secured range. */
	const uint8_t            plain[8] = { 0x50u, 0xFFu, 0xFFu, 1, 2, 3, 4, 5 };

	t_tx = sec_dir_master(NULL);
	sec_dir_air(t_tx, plain);
	sec_dir_read(&st, &state);

	zassert_equal(1u, api_test_count_bcast(SEC_CH, 0x50u),
		      "a plain slave reply no longer reaches the host");
	zassert_equal(0u, st.dropped_direction);
	sec_close();
}
#endif /* CONFIG_RADIANT_SEC */

/*
 * Review 2026-11-13 item 11: a secured master refuses host acknowledged data
 * and bursts, which would carry the host's plaintext beside the protected
 * stream. A slave's acked data is its own traffic and still goes. Keyed and
 * left pending (no epoch): keyed is what "secured" means.
 */
#if defined(CONFIG_RADIANT_SEC)
static void sec_key_pending(void)
{
	int rc[3];

	k_mutex_lock(&api_lock, K_FOREVER);
	rc[0] = radiant_sec_set_key(SEC_CH, sec_dir_root, 128, SEC_DEVNUM);
	rc[1] = radiant_sec_set_devnum(SEC_CH, SEC_DEVNUM);
	rc[2] = radiant_sec_configure(SEC_CH, RADIANT_SEC_SW_CONF, 2u,
				      RADIANT_SEC_PAGE_LO_DEFAULT,
				      RADIANT_SEC_PAGE_HI_DEFAULT);
	k_mutex_unlock(&api_lock);
	for (int i = 0; i < 3; i++) {
		zassert_equal(RADIANT_SEC_OK, rc[i], "key step %d", i);
	}
	zassert_true(radiant_sec_channel_is_secured(SEC_CH));
}

ZTEST(api, test_sec_a_keyed_master_refuses_acked_and_burst)
{
	static uint8_t blk[8] = { 0x10u, 1, 2, 3, 4, 5, 6, 7 };
	uint32_t       arms;

	sec_open((uint8_t)ANTW_CHANNEL_TYPE_MASTER);
	sec_key_pending();
	arms = fake_radio_arm_count();

	zassert_equal((antr_err_t)ANTW_CHANNEL_IN_WRONG_STATE,
		      antr_acknowledge_message_tx(SEC_CH, 8u, blk));
	zassert_equal((antr_err_t)ANTW_CHANNEL_IN_WRONG_STATE,
		      antr_burst_tx(SEC_CH, 8u, blk,
				    ANTR_BURST_SEGMENT_START |
					    ANTR_BURST_SEGMENT_END));
	zassert_equal(arms, fake_radio_arm_count(), "something was armed");
	sec_close();
}

ZTEST(api, test_sec_a_keyed_slave_still_sends_acked_data)
{
	static const uint8_t blk[8] = { 0x46u, 0xFF, 0xFF, 0xFF, 0xFF, 1, 2, 1 };

	sec_open((uint8_t)ANTW_CHANNEL_TYPE_SLAVE);
	sec_key_pending();
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_acknowledge_message_tx(SEC_CH, 8u, blk));
	sec_close();
}
#endif

ZTEST(api, test_sec_an_unkeyed_master_still_sends_acked_and_burst)
{
	static uint8_t       burst_blk[8] = { 0x10u, 1, 2, 3, 4, 5, 6, 7 };
	static const uint8_t ack_blk[8] = { 0x11u, 1, 2, 3, 4, 5, 6, 7 };

	sec_open((uint8_t)ANTW_CHANNEL_TYPE_MASTER);
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_burst_tx(SEC_CH, 8u, burst_blk,
				    ANTR_BURST_SEGMENT_START |
					    ANTR_BURST_SEGMENT_END));
	sec_close();
	sec_open((uint8_t)ANTW_CHANNEL_TYPE_MASTER);
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_acknowledge_message_tx(SEC_CH, 8u, ack_blk));
	sec_close();
}
