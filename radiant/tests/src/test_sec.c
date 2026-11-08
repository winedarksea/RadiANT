/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The two payload transforms, driven end to end inside one process. Channel 0
 * is the sensor, channel 1 the receiver; both share one root key, epoch, and
 * on-air device number (the 1:N model - sensor keeps zero per-receiver state).
 *
 * Uses fake_radio's virtual clock so t_sync can be chosen rather than waited
 * for. The receiver derives the slot index from epoch phase + channel period,
 * not arrival history, and the sender derives its counter from each slot's
 * t_sync, which is what makes mid-stream join and gap cases testable at all.
 *
 * Coverage: happy path, flipped bit (forgery caught once), lost packet (one
 * window breaks, next recovers), injected ack (D15 - MAC not skippable by
 * message type), epoch gate (D3/D17 - no transform before epoch set), clear
 * page (descriptor/common pages untouched), and a keyless injector against
 * the clock: forged counters must not move it, a verified window must not
 * verify twice, a late replay must not grow its delay, while real drift
 * is still followed. And the counter's meaning (TODO item 7): it is the
 * sender's SLOT index, so clear slots, skipped slots and a sparse silence
 * cannot walk it away from the receiver's clock, and a slot posted twice
 * resends the bytes it already built.
 */

#include <zephyr/ztest.h>
#include <string.h>

#include <radiant/radiant_channel.h>
#include <radiant/radiant_sec.h>
#include <radiant/radiant_radio_hal.h>

#include "fake_radio.h"

#define TX_CH        0u
#define RX_CH        1u
#define TEST_DEVNUM  0x3A17u
#define TEST_PERIOD  8182u        /* the measured ANT+ 4.005 Hz period */
#define TEST_PAGE    0x05u

static const uint8_t test_root[16] = {
	0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
	0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
};

/* ── What the pump produced ─────────────────────────────────────────────── */

#define LOG_MAX 64

struct delivered {
	uint8_t                  ch;
	uint8_t                  pay[8];
	uint8_t                  len;
	enum radiant_sec_verdict verdict;
	uint64_t                 t_sync;
};

struct verdict_rec {
	uint8_t                  ch;
	uint16_t                 window_start;
	enum radiant_sec_verdict verdict;
};

static struct delivered   log_pkt[LOG_MAX];
static uint32_t           log_n_pkt;
static struct verdict_rec log_verdict[LOG_MAX];
static uint32_t           log_n_verdict;

/* Every window the receiver judged VERIFIED, for streams far longer than
 * log_verdict holds. Indexed by window_start / 2, since every legal W is even
 * and so every window starts on an even counter. */
static uint8_t verified_map[65536u / 2u / 8u];

static bool window_was_verified(uint16_t start)
{
	return (verified_map[(start / 2u) / 8u] &
		(uint8_t)(1u << ((start / 2u) % 8u))) != 0u;
}

/* Strong definitions displacing the weak ones in radiant_sec.c. Direct calls
 * rather than an ops struct because a function-pointer table is a .data
 * relocation the linker can't garbage-collect, defeating the size gate. */
void radiant_sec_deliver(uint8_t ch, const uint8_t *pay, uint8_t len,
			 enum radiant_sec_verdict verdict, uint64_t t_sync)
{
	if (log_n_pkt < LOG_MAX) {
		log_pkt[log_n_pkt].ch = ch;
		memcpy(log_pkt[log_n_pkt].pay, pay,
		       (len > 8u) ? 8u : (size_t)len);
		log_pkt[log_n_pkt].len = len;
		log_pkt[log_n_pkt].verdict = verdict;
		log_pkt[log_n_pkt].t_sync = t_sync;
	}
	log_n_pkt++;
}

void radiant_sec_window_verdict(uint8_t ch, uint16_t window_start,
				enum radiant_sec_verdict verdict)
{
	if (log_n_verdict < LOG_MAX) {
		log_verdict[log_n_verdict].ch = ch;
		log_verdict[log_n_verdict].window_start = window_start;
		log_verdict[log_n_verdict].verdict = verdict;
	}
	log_n_verdict++;
	if (ch == RX_CH && verdict == RADIANT_SEC_VERDICT_VERIFIED) {
		verified_map[(window_start / 2u) / 8u] |=
			(uint8_t)(1u << ((window_start / 2u) % 8u));
	}
}

/* ── Harness ────────────────────────────────────────────────────────────── */

/* The slot spacing, as radiant_channel spaces a master's slots: the period in
 * whole microseconds, ROUNDED (radiant_channel_counts_to_us()). The sender
 * derives its counter from the slot's t_sync, so the harness has to put slots
 * where a real channel puts them; the receiver's own period (truncated, 1 us
 * shorter at 8182 counts) is a drift its re-anchor absorbs. */
static uint64_t period_us(void)
{
	return (uint64_t)radiant_channel_counts_to_us(TEST_PERIOD);
}

/* Epoch anchor as the receiver computed it. radiant_sec_set_epoch() anchors on
 * radiant_radio_now(), and fake_radio's clock starts one second below 2^32, not
 * zero - so t_sync must be anchor-relative (sec_anchor + index*period), or
 * expected_index() floors every packet to 0 and the test passes without
 * exercising D4's time-derived counter reconstruction at all. */
static uint64_t sec_anchor;

/* The instant of slot `slot` on the sender's clock, which is also when the
 * receiver hears it. The sender's counter IS this slot index. */
static uint64_t slot_t(uint32_t slot)
{
	return sec_anchor + (uint64_t)slot * period_us();
}

static void key_both(void)
{
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_key(TX_CH, test_root, 128, TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_key(RX_CH, test_root, 128, TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_devnum(TX_CH, TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_devnum(RX_CH, TEST_DEVNUM));
}

static void configure_both(uint8_t switches, uint8_t w)
{
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_configure(TX_CH, switches, w,
					    RADIANT_SEC_PAGE_LO_DEFAULT,
					    RADIANT_SEC_PAGE_HI_DEFAULT));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_configure(RX_CH, switches, w,
					    RADIANT_SEC_PAGE_LO_DEFAULT,
					    RADIANT_SEC_PAGE_HI_DEFAULT));
}

static void epoch_both(uint32_t epoch)
{
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_epoch(TX_CH, epoch, 0u, TEST_PERIOD));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_epoch(RX_CH, epoch, 0u, TEST_PERIOD));
	sec_anchor = (uint64_t)radiant_radio_now();
}

/* Build packet `index` - the one in slot `index` - as the sensor would and hand
 * back exactly what went on the air, so a test can deliver it late, twice, or
 * never. */
static void tx_hop(uint32_t index, uint8_t air[8])
{
	memset(air, 0, 8u);
	air[0] = TEST_PAGE;
	air[1] = 0xAAu;                 /* the host's byte [1], which the
					 * transform must overwrite */
	air[2] = (uint8_t)index;
	air[3] = 0x11u;
	air[4] = 0x22u;
	air[5] = 0x33u;
	air[6] = 0x44u;
	air[7] = 0x55u;                 /* likewise the tag byte */

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_tx_transform(TX_CH, air, 8u, slot_t(index)));
}

/* Hand on-air bytes to the receiver as heard at `t_sync`, and run the pump. */
static void ingest_at(const uint8_t air[8], uint64_t t_sync)
{
	zassert_equal(RADIANT_SEC_RX_DEFER,
		      radiant_sec_rx_ingest(RX_CH, air, 8u, true, t_sync),
		      "a secured-range broadcast must be taken by the "
		      "transform, not delivered in ISR context");
	radiant_sec_pump();
}

/* Build packet `index` as the sensor would, and hand it to the receiver unless
 * `drop`. `corrupt_bit` flips one payload bit, simulating an active injector. */
static void hop(uint32_t index, bool drop, int corrupt_bit)
{
	uint8_t pay[8];

	tx_hop(index, pay);

	if (corrupt_bit >= 0) {
		pay[corrupt_bit / 8] ^= (uint8_t)(1u << (corrupt_bit % 8));
	}

	if (drop) {
		return;
	}

	/* Packet i is one period after i-1, measured from the epoch anchor. */
	ingest_at(pay, sec_anchor + (uint64_t)index * period_us());
}

static uint32_t verdicts_for(uint16_t start, enum radiant_sec_verdict want)
{
	uint32_t n = 0;
	uint32_t i;

	for (i = 0; i < log_n_verdict && i < LOG_MAX; i++) {
		if (log_verdict[i].window_start == start &&
		    log_verdict[i].verdict == want) {
			n++;
		}
	}
	return n;
}

static void sec_before(void *f)
{
	ARG_UNUSED(f);
	fake_radio_reset();
	radiant_sec_reset();
	memset(log_pkt, 0, sizeof(log_pkt));
	memset(log_verdict, 0, sizeof(log_verdict));
	memset(verified_map, 0, sizeof(verified_map));
	log_n_pkt = 0u;
	log_n_verdict = 0u;
	sec_anchor = 0u;
}

ZTEST_SUITE(radiant_sec, NULL, NULL, sec_before, NULL, NULL);

/* ── The gate ───────────────────────────────────────────────────────────── */

ZTEST(radiant_sec, test_nothing_transforms_until_the_epoch_advances)
{
	uint8_t pay[8] = { TEST_PAGE, 0xAA, 1, 2, 3, 4, 5, 6 };
	uint8_t before[8];

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 4u);

	/* D3/D17: a reboot that restarts the counter under an unchanged epoch is
	 * a two-time pad for X_CONF and a full replay against X_AUTH, so the
	 * gate covers both switches and stays closed until a host sets an
	 * epoch. */
	memcpy(before, pay, sizeof(pay));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_tx_transform(TX_CH, pay, sizeof(pay),
					       (uint64_t)radiant_radio_now()));
	zassert_mem_equal(pay, before, sizeof(pay),
			  "a transform ran before the epoch was set");

	zassert_equal(RADIANT_SEC_RX_PLAIN,
		      radiant_sec_rx_ingest(RX_CH, pay, sizeof(pay), true, 0u));

	epoch_both(1000u);
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_tx_transform(TX_CH, pay, sizeof(pay),
					       slot_t(0u)));
	zassert_true(memcmp(pay, before, sizeof(pay)) != 0,
		     "the transform is still inert after a fresh epoch");
}

ZTEST(radiant_sec, test_the_epoch_only_moves_forwards)
{
	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 2u);

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_epoch(TX_CH, 100u, 0u, TEST_PERIOD));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_set_epoch(TX_CH, 100u, 0u, TEST_PERIOD),
		      "the same epoch was accepted twice");
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_set_epoch(TX_CH, 99u, 0u, TEST_PERIOD),
		      "an epoch went backwards");
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_epoch(TX_CH, 101u, 0u, TEST_PERIOD));

	/* A wrap must always have headroom to advance into. */
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_set_epoch(TX_CH, 0xFFFFFFFFu, 0u, TEST_PERIOD));
}

/* ── The happy path ─────────────────────────────────────────────────────── */

ZTEST(radiant_sec, test_a_window_verifies)
{
	uint32_t i;

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 4u);
	epoch_both(7u);

	/* Three windows: window 0's own tag bytes carry nothing (the tag lags
	 * one window and there is no window -1), but window 0 itself is judged
	 * like any other - windows 4 and 8 authenticate 0 and 4. */
	for (i = 0; i < 12u; i++) {
		hop(i, false, -1);
	}

	zassert_equal(12u, log_n_pkt, "every packet must be delivered");
	zassert_equal(1u, verdicts_for(0u, RADIANT_SEC_VERDICT_VERIFIED),
		      "window 0 did not verify exactly once");
	zassert_equal(1u, verdicts_for(4u, RADIANT_SEC_VERDICT_VERIFIED),
		      "window 4 did not verify exactly once");
	zassert_equal(2u, log_n_verdict,
		      "expected exactly two window verdicts from three "
		      "windows: the tag lags one window, so the last one is "
		      "still waiting for a tag that has not been sent");
}

ZTEST(radiant_sec, test_the_transform_owns_byte_one)
{
	uint8_t pay[8];
	uint8_t i;

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 2u);
	epoch_both(3u);

	for (i = 0u; i < 5u; i++) {
		memset(pay, 0, sizeof(pay));
		pay[0] = TEST_PAGE;
		pay[1] = 0xAAu;  /* whatever the host wrote */
		zassert_equal(RADIANT_SEC_OK,
			      radiant_sec_tx_transform(TX_CH, pay, sizeof(pay),
						       slot_t(i)));
		zassert_equal(i, pay[1],
			      "byte [1] must be the SLOT index. A host "
			      "counter repeats across the retransmissions a "
			      "master makes every slot, and a repeated counter "
			      "under one (epoch, devnum) is keystream reuse");
	}
}

ZTEST(radiant_sec, test_a_page_outside_the_range_is_untouched)
{
	uint8_t pay[8] = { 0x00, 0x12, 1, 2, 3, 4, 5, 6 };  /* the descriptor */
	uint8_t before[8];

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_CONF, 2u);
	epoch_both(5u);

	memcpy(before, pay, sizeof(pay));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_tx_transform(TX_CH, pay, sizeof(pay),
					       slot_t(0u)));
	zassert_mem_equal(pay, before, sizeof(pay),
			  "the descriptor was transformed; it must stay in the "
			  "clear or a protected node is indistinguishable from "
			  "a dead one");

	/* Same on the way in, including the common pages. */
	pay[0] = 0x51u;   /* common page 81 */
	zassert_equal(RADIANT_SEC_RX_PLAIN,
		      radiant_sec_rx_ingest(RX_CH, pay, sizeof(pay), true, 0u));
}

ZTEST(radiant_sec, test_the_page_range_is_bounded)
{
	key_both();

	/* 0x00 would make the node's own descriptor unreadable/undiscoverable
	 * from one host typo. */
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_configure(TX_CH, RADIANT_SEC_SW_AUTH, 2u,
					    0x00u, 0x0Fu));
	/* 0x50 is common page 80, which every ANT+ receiver already
	 * understands and which must never be transformed. */
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_configure(TX_CH, RADIANT_SEC_SW_AUTH, 2u,
					    0x01u, 0x50u));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_configure(TX_CH, RADIANT_SEC_SW_AUTH, 2u,
					    0x0Fu, 0x01u));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_configure(TX_CH, RADIANT_SEC_SW_AUTH, 2u,
					    0x01u, 0x1Fu));
}

ZTEST(radiant_sec, test_only_w_two_four_and_eight_are_accepted)
{
	uint8_t w;

	key_both();

	for (w = 0u; w <= 16u; w++) {
		int rc = radiant_sec_configure(TX_CH, RADIANT_SEC_SW_AUTH, w,
					       0x01u, 0x0Fu);

		if (w == 2u || w == 4u || w == 8u) {
			zassert_equal(RADIANT_SEC_OK, rc, "W=%u refused", w);
		} else {
			zassert_equal(RADIANT_SEC_EINVAL, rc,
				      "W=%u accepted. W must divide 256 and "
				      "65536 or the counter-derived window "
				      "stops resynchronising after a lost "
				      "packet", w);
		}
	}
}

ZTEST(radiant_sec, test_descriptor_encryption_is_refused)
{
	key_both();

	/* Underspecified, not merely unbuilt: the descriptor has no counter
	 * (byte [1] is a frame index), so it has no nonce. */
	zassert_equal(RADIANT_SEC_ENOTSUP,
		      radiant_sec_configure(TX_CH,
					    RADIANT_SEC_SW_CONF |
						    RADIANT_SEC_SW_DESC_CONF,
					    2u, 0x01u, 0x0Fu));
}

/* ── The attacks ────────────────────────────────────────────────────────── */

ZTEST(radiant_sec, test_a_flipped_bit_is_caught_once_one_window_later)
{
	uint32_t i;

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 4u);
	epoch_both(11u);

	/* Corrupt one bit of packet 1's payload, inside window 0. */
	for (i = 0; i < 12u; i++) {
		hop(i, false, (i == 1u) ? (2 * 8 + 3) : -1);
	}

	zassert_equal(1u, verdicts_for(0u, RADIANT_SEC_VERDICT_UNVERIFIED),
		      "the forged window was not reported unverified exactly "
		      "once");
	zassert_equal(0u, verdicts_for(0u, RADIANT_SEC_VERDICT_VERIFIED));

	/* And the damage is bounded to that window: the next one is clean. */
	zassert_equal(1u, verdicts_for(4u, RADIANT_SEC_VERDICT_VERIFIED),
		      "a single forged packet poisoned the following window");

	/* D6: delivered and marked, not discarded - discarding would let one
	 * injected frame cost W legitimate packets (a W-for-1 DoS). */
	zassert_equal(12u, log_n_pkt,
		      "packets were discarded rather than marked");
}

ZTEST(radiant_sec, test_a_lost_packet_breaks_one_window_and_the_next_recovers)
{
	uint32_t i;

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 4u);
	epoch_both(13u);

	/* Drop packet 2, which is inside window 0. Against a measured ~0.4%
	 * bench loss floor this is the ordinary case, not an exotic one. */
	for (i = 0; i < 12u; i++) {
		hop(i, i == 2u, -1);
	}

	zassert_equal(1u, verdicts_for(0u, RADIANT_SEC_VERDICT_UNVERIFIED),
		      "the window with a hole in it should be unverifiable");
	zassert_equal(1u, verdicts_for(4u, RADIANT_SEC_VERDICT_VERIFIED),
		      "THE WHOLE POINT OF DERIVING THE WINDOW FROM THE COUNTER: "
		      "one lost packet must not desynchronise every window "
		      "after it");
	zassert_equal(11u, log_n_pkt, "the surviving packets were not delivered");
}

ZTEST(radiant_sec, test_a_secured_page_as_acknowledged_data_is_dropped)
{
	struct radiant_sec_stats st;
	uint8_t                  pay[8] = { TEST_PAGE, 0, 1, 2, 3, 4, 5, 6 };

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 2u);
	epoch_both(17u);

	/* D15: "transform broadcast only" governs what a secured channel
	 * initiates, but decode still handles acknowledged-data/burst. Without
	 * this guard, a secured-range page sent as acknowledged data skips the
	 * MAC entirely and is delivered as implicitly trusted. */
	zassert_equal(RADIANT_SEC_RX_DROP,
		      radiant_sec_rx_ingest(RX_CH, pay, sizeof(pay), false, 0u),
		      "a non-broadcast secured-range page was not dropped");

	radiant_sec_pump();
	zassert_equal(0u, log_n_pkt, "the dropped frame was delivered anyway");

	radiant_sec_get_stats(RX_CH, &st);
	zassert_equal(1u, st.dropped_non_broadcast,
		      "the drop must be counted, or it is invisible to a host");
}

ZTEST(radiant_sec, test_a_receiver_with_the_wrong_key_verifies_nothing)
{
	static const uint8_t other[16] = { 0xde, 0xad, 0xbe, 0xef };
	uint32_t             i;

	key_both();
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_key(RX_CH, other, 128, TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_devnum(RX_CH, TEST_DEVNUM));
	configure_both(RADIANT_SEC_SW_AUTH, 4u);
	epoch_both(19u);

	for (i = 0; i < 12u; i++) {
		hop(i, false, -1);
	}

	zassert_equal(0u, verdicts_for(0u, RADIANT_SEC_VERDICT_VERIFIED),
		      "a window verified under the wrong key");
	zassert_equal(1u, verdicts_for(0u, RADIANT_SEC_VERDICT_UNVERIFIED));
}

/* ── 1:N, which is the property the whole design exists for ─────────────── */

ZTEST(radiant_sec, test_two_receivers_share_one_key_with_no_sensor_state)
{
	uint32_t i;
	uint32_t ch1 = 0u;
	uint32_t ch2 = 0u;

	key_both();
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_key(2u, test_root, 128, TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_OK, radiant_sec_set_devnum(2u, TEST_DEVNUM));
	configure_both(RADIANT_SEC_SW_AUTH, 4u);
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_configure(2u, RADIANT_SEC_SW_AUTH, 4u,
					    RADIANT_SEC_PAGE_LO_DEFAULT,
					    RADIANT_SEC_PAGE_HI_DEFAULT));
	epoch_both(23u);
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_epoch(2u, 23u, 0u, TEST_PERIOD));

	for (i = 0; i < 8u; i++) {
		uint8_t  pay[8];
		uint64_t t_sync = (uint64_t)i * period_us();

		memset(pay, 0, sizeof(pay));
		pay[0] = TEST_PAGE;
		pay[2] = (uint8_t)i;
		zassert_equal(RADIANT_SEC_OK,
			      radiant_sec_tx_transform(TX_CH, pay, sizeof(pay),
						       slot_t(i)));

		/* The sensor transmits once; both receivers consume the same
		 * eight bytes with no per-receiver state - unlike BLE's pairwise
		 * LTK model. */
		zassert_equal(RADIANT_SEC_RX_DEFER,
			      radiant_sec_rx_ingest(RX_CH, pay, sizeof(pay),
						    true, t_sync));
		zassert_equal(RADIANT_SEC_RX_DEFER,
			      radiant_sec_rx_ingest(2u, pay, sizeof(pay), true,
						    t_sync));
		radiant_sec_pump();
	}

	for (i = 0; i < log_n_verdict && i < LOG_MAX; i++) {
		zassert_equal(RADIANT_SEC_VERDICT_VERIFIED,
			      log_verdict[i].verdict);
		if (log_verdict[i].ch == RX_CH) {
			ch1++;
		} else if (log_verdict[i].ch == 2u) {
			ch2++;
		}
	}
	zassert_equal(1u, ch1, "receiver 1 did not verify window 0");
	zassert_equal(1u, ch2, "receiver 2 did not verify window 0");
}

/* ── The policy bit ─────────────────────────────────────────────────────── */

ZTEST(radiant_sec, test_the_drop_policy_holds_a_window_back)
{
	uint32_t i;

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_DROP_UNVER, 4u);
	epoch_both(29u);

	for (i = 0; i < 12u; i++) {
		hop(i, false, (i == 1u) ? (3 * 8 + 1) : -1);
	}

	/* Window 0 is forged, so under this policy its packets never reach a
	 * host; window 4 verifies (by window 8's tag) and is released. Window
	 * 8 is still held: its verdict needs a fourth window's tag, which has
	 * not been sent - the policy's latency is the detection latency. The
	 * default is the other way (deliver, marked) - the choice of suspect
	 * reading vs. hole is the application's call.
	 *
	 * Indices 4..7, not 8..11. Before D1-1 this released 8..11, on window
	 * 4's verdict, and dropped window 0 unjudged as "nothing authenticates
	 * the first window" - wrong twice: window 4's tag authenticates window
	 * 0, and window 8 had not been checked at all. */
	zassert_equal(4u, log_n_pkt,
		      "expected only the verified window to be released, got "
		      "%u packets", log_n_pkt);
	for (i = 0; i < log_n_pkt && i < LOG_MAX; i++) {
		zassert_equal(RADIANT_SEC_VERDICT_VERIFIED,
			      log_pkt[i].verdict,
			      "an unverified packet escaped the drop policy");
		zassert_equal(4u + i, log_pkt[i].pay[2],
			      "delivery %u was packet %u, expected window 4's "
			      "packet %u", i, log_pkt[i].pay[2], 4u + i);
	}
}

/*
 * D1-1, review 2026-11-06: under DROP_UNVER a window must be released on ITS
 * OWN verdict, which exists only once the window after it has closed - never
 * on the verdict of the window before it, which is the one that happens to be
 * judged at the moment it closes. Released early, a packet injected into any
 * window rode out as VERIFIED on the strength of a genuine predecessor, with
 * no key needed: the forgery was detected one window later, by which time the
 * packets it covered had already been delivered as trusted.
 *
 * Window 0 is genuine, window 1 carries one forged bit (byte 3 of hop 5, in
 * the clear under X_AUTH alone, so the receiver's own MAC over window 1
 * disagrees with the sender's tag sent in window 2), windows 2 and 3 are
 * genuine. Window 0 verifies when window 1 closes (window 1's tag bytes are
 * untouched and authenticate it), window 1 fails when window 2 closes, window
 * 2 verifies when window 3 closes, and window 3 has no successor yet and stays
 * held. So exactly windows 0 and 2 come out - indices 0..3 and 8..11 - and
 * nothing from window 1, hop 5 above all.
 */
ZTEST(radiant_sec, test_the_drop_policy_never_releases_a_window_on_its_predecessors_verdict)
{
	static const uint8_t want[] = { 0u, 1u, 2u, 3u, 8u, 9u, 10u, 11u };
	uint32_t i;

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_DROP_UNVER, 4u);
	epoch_both(37u);

	for (i = 0; i < 16u; i++) {
		hop(i, false, (i == 5u) ? (3 * 8 + 1) : -1);
	}

	/* The detection half was never in doubt; asserted so a failure below
	 * is read as a delivery bug and not as a MAC that missed the bit. */
	zassert_equal(1u, verdicts_for(4u, RADIANT_SEC_VERDICT_UNVERIFIED),
		      "setup: the forged window 1 was not judged unverified");

	for (i = 0; i < log_n_pkt && i < LOG_MAX; i++) {
		zassert_not_equal(5u, log_pkt[i].pay[2],
				  "the forged packet (hop 5, window 1) was "
				  "delivered, marked %d - released on window 0's "
				  "verdict before its own window's MAC was checked",
				  (int)log_pkt[i].verdict);
		zassert_equal(RADIANT_SEC_VERDICT_VERIFIED, log_pkt[i].verdict,
			      "an unverified packet escaped the drop policy");
	}

	zassert_equal(ARRAY_SIZE(want), log_n_pkt,
		      "expected windows 0 and 2 (8 packets), got %u packets",
		      log_n_pkt);
	for (i = 0; i < ARRAY_SIZE(want); i++) {
		zassert_equal(want[i], log_pkt[i].pay[2],
			      "delivery %u was packet %u, expected %u", i,
			      log_pkt[i].pay[2], want[i]);
	}
}

/* ── X_CONF ─────────────────────────────────────────────────────────────── */

/* Plaintext every confidentiality test sends. Byte [0] is the page and byte [1]
 * the transform-owned counter, so only [2..6] (5 bytes) are the sender's to
 * hide - the rest of the budget is taken by X_AUTH's byte [7] and the counter.
 */
static const uint8_t conf_plain[8] = {
	TEST_PAGE, 0xAAu, 0xDEu, 0xADu, 0xBEu, 0xEFu, 0x42u, 0x55u,
};

/* Transform one packet in slot `index` and hand back exactly what went on the
 * air (hop() deliberately doesn't expose this). */
static void tx_air(uint32_t index, uint8_t air[8])
{
	memcpy(air, conf_plain, 8u);
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_tx_transform(TX_CH, air, 8u, slot_t(index)));
}

/*
 * D1-6(a), review 2026-10-31: a counter wrap with no epoch headroom left must
 * stop secured TX, not carry on reusing (epoch, devnum, counter) nonces - a
 * two-time pad under X_CONF. A host epoch at the limit gets exactly one wrap.
 */
ZTEST(radiant_sec, test_a_wrap_with_no_epoch_left_stops_tx_instead_of_reusing_a_counter)
{
	uint8_t  pay[8];
	uint32_t i;
	int      rc = RADIANT_SEC_OK;

	key_both();
	configure_both(RADIANT_SEC_SW_CONF, 2u);
	zassert_ok(radiant_sec_set_epoch(TX_CH,
					 0xFFFFFFFFu - RADIANT_SEC_EPOCH_HEADROOM,
					 0u, TEST_PERIOD));
	sec_anchor = (uint64_t)radiant_radio_now();
	/* Wrap 1 advances into the headroom; wrap 2 has nowhere to go. */
	for (i = 0; i < 2u * 65536u && rc == RADIANT_SEC_OK; i++) {
		memcpy(pay, conf_plain, 8u);
		rc = radiant_sec_tx_transform(TX_CH, pay, 8u, slot_t(i));
	}
	zassert_ok(rc, "a transform failed before the second wrap (i=%u)", i);
	memcpy(pay, conf_plain, 8u);
	zassert_not_equal(RADIANT_SEC_OK,
			  radiant_sec_tx_transform(TX_CH, pay, 8u, slot_t(i)),
			  "TX continued with counter 0 under an epoch already used");
}

/*
 * A6-1, review 2026-10-31: ciphertext queued for the pump belongs to the
 * context it was ingested under. Released and re-keyed for a different sensor
 * before the event thread ran, the channel must not judge it under the new key.
 */
ZTEST(radiant_sec, test_release_drops_ciphertext_still_queued_for_the_pump)
{
	static const uint8_t other[16] = { 0xdeu, 0xadu, 0xbeu, 0xefu };
	uint8_t pay[8] = { TEST_PAGE, 0, 1, 2, 3, 4, 5, 6 };

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 2u);
	epoch_both(41u);
	zassert_equal(RADIANT_SEC_RX_DEFER,
		      radiant_sec_rx_ingest(RX_CH, pay, 8u, true, sec_anchor));

	radiant_sec_channel_release(RX_CH);
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_key(RX_CH, other, 128, TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_configure(RX_CH, RADIANT_SEC_SW_AUTH, 2u,
					    RADIANT_SEC_PAGE_LO_DEFAULT,
					    RADIANT_SEC_PAGE_HI_DEFAULT));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_epoch(RX_CH, 1u, 0u, TEST_PERIOD));

	radiant_sec_pump();
	zassert_equal(0u, log_n_pkt,
		      "a frame queued for the released context was judged "
		      "under the next one");
}

/* D1-4: the constant-time compare is still an equality. Constant time itself
 * is not unit-testable; the sec suites staying green is the real guard. */
ZTEST(radiant_sec, test_ct_equal_is_equality)
{
	uint8_t a[16];
	uint8_t b[16];

	memset(a, 0x5A, sizeof(a));
	memcpy(b, a, sizeof(b));
	zassert_true(radiant_sec_ct_equal(a, b, sizeof(a)), NULL);
	zassert_true(radiant_sec_ct_equal(a, b, 0u), "n = 0 is equal");
	b[0] ^= 0x01u;
	zassert_false(radiant_sec_ct_equal(a, b, sizeof(a)), "byte 0 differs");
	b[0] ^= 0x01u;
	b[15] ^= 0x80u;
	zassert_false(radiant_sec_ct_equal(a, b, sizeof(a)), "byte 15 differs");
}

/* D1-6(b): a failed key import must not leave an allocated, keyless context
 * behind - configure on it then answered OK where ENOKEY is owed. */
ZTEST(radiant_sec, test_a_failed_import_leaves_no_keyless_context)
{
	zassert_not_equal(RADIANT_SEC_OK,
			  radiant_sec_set_key(RX_CH, test_root, 64, TEST_DEVNUM),
			  "setup: a 64-bit key was accepted");
	zassert_equal(RADIANT_SEC_ENOKEY,
		      radiant_sec_configure(RX_CH, RADIANT_SEC_SW_AUTH, 2u,
					    RADIANT_SEC_PAGE_LO_DEFAULT,
					    RADIANT_SEC_PAGE_HI_DEFAULT),
		      "a failed import left a context that accepts configure");
}

ZTEST(radiant_sec, test_confidentiality_hides_five_bytes_and_gives_them_back)
{
	uint8_t air[8];

	key_both();
	configure_both(RADIANT_SEC_SW_CONF, 2u);
	epoch_both(7u);

	tx_air(0u, air);

	/* Page number and byte [1] (counter, needed to derive the keystream)
	 * travel in the clear by construction; asserted here so a later
	 * "encrypt everything" change can't silently break decoding. */
	zassert_equal(TEST_PAGE, air[0],
		      "the page number must stay readable or the receiver "
		      "cannot tell which schema it is holding");
	zassert_equal(0u, air[1], "byte [1] is the counter, in the clear");
	zassert_true(memcmp(&air[2], &conf_plain[2], 5u) != 0,
		     "the five data bytes went out as plaintext");

	zassert_equal(RADIANT_SEC_RX_DEFER,
		      radiant_sec_rx_ingest(RX_CH, air, 8u, true, sec_anchor));
	radiant_sec_pump();

	zassert_equal(1u, log_n_pkt, "the receiver delivered %u packets",
		      log_n_pkt);
	zassert_mem_equal(&log_pkt[0].pay[2], &conf_plain[2], 5u,
			  "the receiver did not recover the plaintext");
	/* CLEAR, not VERIFIED: X_CONF alone hides the payload but authenticates
	 * nothing; a stream any attacker can still forge. */
	zassert_equal(RADIANT_SEC_VERDICT_CLEAR, log_pkt[0].verdict);
	zassert_equal(RADIANT_SEC_VERDICT_CLEAR, radiant_sec_last_verdict(RX_CH));
}

/*
 * TODO release-review 6, and the vectors shared with
 * tools/test_radiant_crypto.py (test_conf_vectors_shared_with_the_c_
 * implementation): root 00..ff, epoch 7, devnum 0x3A17, slot 0, plaintext
 * 05 00 de ad be ef 42 55 (tx_air() overwrites byte [1] with the counter, 0
 * here). Without X_AUTH, byte [7] is payload and is ciphertext: CTR covers six
 * bytes. With X_AUTH it is the tag byte (zero in the first window) and CTR
 * covers five.
 */
ZTEST(radiant_sec, test_conf_vectors_shared_with_python)
{
	static const uint8_t want_conf_only[8] = {
		0x05, 0x00, 0xC9, 0x00, 0x23, 0xAE, 0x88, 0x79,
	};
	static const uint8_t want_with_auth[7] = {
		0x05, 0x00, 0xC9, 0x00, 0x23, 0xAE, 0x88,
	};
	uint8_t air[8];

	key_both();
	configure_both(RADIANT_SEC_SW_CONF, 2u);
	epoch_both(7u);
	tx_air(0u, air);
	zassert_mem_equal(air, want_conf_only, 8u,
			  "X_CONF alone: byte [7] must be ciphertext");

	/* And the receiver gives byte [7] back, which the five-byte receiver
	 * would have left as ciphertext. */
	zassert_equal(RADIANT_SEC_RX_DEFER,
		      radiant_sec_rx_ingest(RX_CH, air, 8u, true, sec_anchor));
	radiant_sec_pump();
	zassert_equal(1u, log_n_pkt);
	zassert_equal(conf_plain[7], log_pkt[0].pay[7],
		      "the receiver left byte [7] as ciphertext");
	zassert_mem_equal(&log_pkt[0].pay[2], &conf_plain[2], 6u);

	radiant_sec_channel_release(TX_CH);
	radiant_sec_channel_release(RX_CH);
	key_both();
	configure_both(RADIANT_SEC_SW_CONF | RADIANT_SEC_SW_AUTH, 2u);
	epoch_both(7u);
	tx_air(0u, air);
	zassert_mem_equal(air, want_with_auth, 7u,
			  "X_CONF with X_AUTH: bytes [2..6] only");
	zassert_equal(0x00u, air[7], "byte [7] is the tag byte, not ciphertext");
}

ZTEST(radiant_sec, test_the_same_plaintext_twice_is_two_different_ciphertexts)
{
	uint8_t a[8];
	uint8_t b[8];

	key_both();
	configure_both(RADIANT_SEC_SW_CONF, 2u);
	epoch_both(7u);

	/* Why radiant_sec owns byte [1]: a master retransmits its body every
	 * slot, so a host-maintained counter would repeat, and two packets under
	 * one (epoch, devnum, counter) is keystream reuse - XORing the two
	 * ciphertexts cancels the key entirely. */
	tx_air(0u, a);
	tx_air(1u, b);

	zassert_equal(0u, a[1]);
	zassert_equal(1u, b[1]);
	zassert_true(memcmp(&a[2], &b[2], 5u) != 0,
		     "the same plaintext encrypted to the same ciphertext "
		     "twice; the counter is not reaching the nonce");
}

ZTEST(radiant_sec, test_a_receiver_joining_mid_stream_decrypts_with_no_history)
{
	uint8_t  air[8];
	uint32_t i;

	key_both();
	configure_both(RADIANT_SEC_SW_CONF, 2u);
	epoch_both(7u);

	/* D4: the receiver reconstructs the counter from epoch phase + channel
	 * period, not arrival history. Sender runs 500 packets ahead; receiver
	 * hears only the 501st - counting arrivals would decrypt against counter
	 * 0 and produce garbage. */
	for (i = 0u; i < 500u; i++) {
		tx_air(i, air);
	}
	tx_air(500u, air);

	zassert_equal(RADIANT_SEC_RX_DEFER,
		      radiant_sec_rx_ingest(RX_CH, air, 8u, true,
					    sec_anchor + 500ull * period_us()));
	radiant_sec_pump();

	zassert_equal(1u, log_n_pkt);
	zassert_mem_equal(&log_pkt[0].pay[2], &conf_plain[2], 5u,
			  "a receiver that joined at packet 500 did not "
			  "reconstruct the counter from time");
}

ZTEST(radiant_sec, test_a_receiver_with_the_wrong_key_recovers_no_plaintext)
{
	static const uint8_t other[16] = {
		0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88,
		0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
	};
	uint8_t air[8];

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_key(TX_CH, test_root, 128, TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_key(RX_CH, other, 128, TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_OK, radiant_sec_set_devnum(TX_CH, TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_OK, radiant_sec_set_devnum(RX_CH, TEST_DEVNUM));
	configure_both(RADIANT_SEC_SW_CONF, 2u);
	epoch_both(7u);

	tx_air(0u, air);
	zassert_equal(RADIANT_SEC_RX_DEFER,
		      radiant_sec_rx_ingest(RX_CH, air, 8u, true, sec_anchor));
	radiant_sec_pump();

	zassert_equal(1u, log_n_pkt);
	zassert_true(memcmp(&log_pkt[0].pay[2], &conf_plain[2], 5u) != 0,
		     "the wrong key recovered the plaintext");
}

ZTEST(radiant_sec, test_the_counter_wrap_does_not_repeat_the_keystream)
{
	uint8_t  first[8];
	uint8_t  again[8];
	uint32_t i;

	key_both();
	configure_both(RADIANT_SEC_SW_CONF, 2u);
	epoch_both(7u);

	/* D14: at 4 Hz a 16-bit counter wraps every 4.5 hours; a wrap that left
	 * the epoch alone would replay the epoch's keystream (a delayed two-time
	 * pad). 65536 divides every legal W, so advancing the epoch on wrap
	 * doesn't break window alignment. Runs a full counter cycle to test it
	 * without reaching into the context. */
	tx_air(0u, first);
	for (i = 1u; i < 65536u; i++) {
		uint8_t scratch[8];

		tx_air(i, scratch);
	}
	tx_air(65536u, again);

	zassert_equal(0u, again[1], "the counter did not return to zero");
	zassert_true(memcmp(&first[2], &again[2], 5u) != 0,
		     "counter 0 of the next cycle reused counter 0's keystream; "
		     "the wrap did not advance the epoch");
}

/* ── Identity Tier 2: re-acquisition by key ─────────────────────────────── */

/* Tier 2 re-rolls the on-air device number every power-up; the base device
 * number (fixed at provisioning, what the KDF binds) does not, so keys are
 * unchanged and only the nonce's device number differs. TEST_DEVNUM is the
 * pre-cycle identity; TIER2_NEW_DEVNUM the post-cycle one. */
#define TIER2_NEW_DEVNUM 0x91C4u

/* Run one whole window of packets from a sensor whose on-air device number is
 * `devnum`, at counter indices `from .. from + w - 1`. */
static void tier2_window(uint16_t devnum, uint32_t from, uint8_t w)
{
	uint32_t i;

	zassert_equal(RADIANT_SEC_OK, radiant_sec_set_devnum(TX_CH, devnum));
	for (i = 0u; i < w; i++) {
		hop(from + i, false, -1);
	}
}

ZTEST(radiant_sec, test_a_keyed_receiver_re_acquires_across_a_power_cycle)
{
	const uint8_t w = 4u;

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, w);
	epoch_both(11u);

	/* Before the power cycle: both sides on TEST_DEVNUM. Two windows since
	 * the tag lags one window (window k's tag bytes authenticate k-1): the
	 * first window heard IS the one that verifies, but its verdict exists
	 * only once the second has closed. */
	tier2_window(TEST_DEVNUM, 0u, w);
	tier2_window(TEST_DEVNUM, w, w);
	zassert_true(verdicts_for(0u, RADIANT_SEC_VERDICT_VERIFIED) > 0u,
		     "the link did not verify before the power cycle, so what "
		     "follows would prove nothing");

	/* The power cycle: new on-air device number, same key, same epoch
	 * re-anchored (permitted only because reboot cleared epoch_set; a real
	 * node persists the issued epoch). A standard receiver is now lost since
	 * it tracks by device number - the documented cost of Tier 2. */
	radiant_sec_channel_release(TX_CH);
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_key(TX_CH, test_root, 128, TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_configure(TX_CH, RADIANT_SEC_SW_AUTH, w,
					    RADIANT_SEC_PAGE_LO_DEFAULT,
					    RADIANT_SEC_PAGE_HI_DEFAULT));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_epoch(TX_CH, 11u, 0u, TEST_PERIOD));

	/* No re-pair, no host consulted: wildcard-searched the device type,
	 * heard a candidate, asks the one question that survives a re-roll. */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_try_devnum(RX_CH, TIER2_NEW_DEVNUM));
	zassert_equal(RADIANT_SEC_VERDICT_CLEAR,
		      radiant_sec_last_verdict(RX_CH),
		      "a fresh trial must start with no verdict, or the "
		      "previous identity's answer is read as this one's");

	log_n_verdict = 0u;
	tier2_window(TIER2_NEW_DEVNUM, 0u, w);
	tier2_window(TIER2_NEW_DEVNUM, w, w);

	zassert_equal(RADIANT_SEC_VERDICT_VERIFIED,
		      radiant_sec_last_verdict(RX_CH),
		      "a keyed receiver did not re-acquire the sensor across a "
		      "power cycle; Tier 2 without this is just losing sensors");
	zassert_true(verdicts_for(0u, RADIANT_SEC_VERDICT_VERIFIED) > 0u,
		     "no window verified under the new device number");
}

ZTEST(radiant_sec, test_a_candidate_that_is_not_ours_never_verifies)
{
	static const uint8_t stranger[16] = {
		0x01, 0x01, 0x02, 0x03, 0x05, 0x08, 0x0d, 0x15,
		0x22, 0x37, 0x59, 0x90, 0xe9, 0x79, 0x62, 0xdb,
	};
	const uint8_t w = 4u;

	/* The other half of the claim: re-acquisition is only identification if
	 * it can also say no, or it silently "re-acquires" any stranger. */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_key(TX_CH, stranger, 128,
					  TIER2_NEW_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_key(RX_CH, test_root, 128, TEST_DEVNUM));
	configure_both(RADIANT_SEC_SW_AUTH, w);
	epoch_both(11u);

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_try_devnum(RX_CH, TIER2_NEW_DEVNUM));

	tier2_window(TIER2_NEW_DEVNUM, 0u, w);
	tier2_window(TIER2_NEW_DEVNUM, w, w);

	zassert_equal(RADIANT_SEC_VERDICT_UNVERIFIED,
		      radiant_sec_last_verdict(RX_CH),
		      "a node under a different root verified; the trial is "
		      "answering 'yes' to everything");
	zassert_equal(0u, verdicts_for(0u, RADIANT_SEC_VERDICT_VERIFIED));
}

ZTEST(radiant_sec, test_a_trial_before_the_epoch_is_known_is_refused)
{
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_key(RX_CH, test_root, 128, TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_configure(RX_CH, RADIANT_SEC_SW_AUTH, 4u,
					    RADIANT_SEC_PAGE_LO_DEFAULT,
					    RADIANT_SEC_PAGE_HI_DEFAULT));

	/* Refused rather than quietly started: with no epoch nothing can
	 * verify, so a trial here would report "not my sensor" about every
	 * candidate including the right one - and the caller would read that as
	 * the sensor being absent. */
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_try_devnum(RX_CH, TIER2_NEW_DEVNUM));
}

/* ── The drop policy against a subtly wrong D1-1 fix ────────────────────── */

/*
 * The cases test_the_drop_policy_never_releases_a_window_on_its_predecessors_
 * verdict cannot see: a window lost whole on air, and held packets outliving a
 * reset. A fix that paired buffers with windows by counter parity rather than
 * by close events, or that left a reset site without hold_reset(), passes
 * every DROP_UNVER case above and fails one of these.
 *
 * Marked packets are the ones that must never come out: hop()'s byte [4] is
 * 0x22, and bit 4*8 turns it into 0x23 on the air.
 */
#define HOLD_MARK_BIT (4 * 8)

static void assert_no_marked_delivery(void)
{
	uint32_t i;

	for (i = 0; i < log_n_pkt && i < LOG_MAX; i++) {
		zassert_equal(0x22u, log_pkt[i].pay[4],
			      "delivery %u (packet %u) was held before a reset "
			      "and came out after it", i, log_pkt[i].pay[2]);
		zassert_equal(RADIANT_SEC_VERDICT_VERIFIED, log_pkt[i].verdict,
			      "an unverified packet escaped the drop policy");
	}
}

static uint32_t dropped_policy_now(void)
{
	struct radiant_sec_stats st;

	radiant_sec_get_stats(RX_CH, &st);
	return st.dropped_policy;
}

/*
 * Window 4 (hops 4..7) never reaches the receiver at all, so it never opens or
 * closes. The close of window 8 therefore judges window 0, against window 8's
 * tag bytes - which carry window 4's tag, not window 0's - so window 0 is
 * UNVERIFIED and must be dropped whole, from the buffer that holds it. Window
 * 8 is then judged by window 12 and released, window 12 by window 16.
 *
 * Pairing buffers by window parity instead ((start / W) & 1) puts windows 0
 * and 8 in the same buffer, and this is where that shows.
 */
ZTEST(radiant_sec, test_the_drop_policy_drops_the_window_before_a_lost_one)
{
	uint32_t i;

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_DROP_UNVER, 4u);
	epoch_both(43u);

	for (i = 0; i < 20u; i++) {
		hop(i, i >= 4u && i < 8u, -1);
	}

	zassert_equal(1u, verdicts_for(0u, RADIANT_SEC_VERDICT_UNVERIFIED),
		      "window 0 was not judged unverified across the lost "
		      "window");
	zassert_equal(0u, verdicts_for(4u, RADIANT_SEC_VERDICT_VERIFIED) +
			  verdicts_for(4u, RADIANT_SEC_VERDICT_UNVERIFIED),
		      "a verdict was issued for a window never heard");
	zassert_equal(1u, verdicts_for(8u, RADIANT_SEC_VERDICT_VERIFIED));
	zassert_equal(1u, verdicts_for(12u, RADIANT_SEC_VERDICT_VERIFIED));

	/* Windows 8 and 12 come out, in order; window 16 is still held. */
	zassert_equal(8u, log_n_pkt, "expected 8..15 (8 packets), got %u",
		      log_n_pkt);
	for (i = 0; i < log_n_pkt && i < LOG_MAX; i++) {
		zassert_equal(RADIANT_SEC_VERDICT_VERIFIED, log_pkt[i].verdict);
		zassert_equal(8u + i, log_pkt[i].pay[2],
			      "delivery %u was packet %u, expected %u", i,
			      log_pkt[i].pay[2], 8u + i);
	}
	zassert_equal(4u, dropped_policy_now(),
		      "window 0's four packets were not counted as dropped");
}

/*
 * set_epoch mid-window. Hops 0 and 1 of epoch 45 are held in the open window's
 * buffer; the host then installs epoch 46 and the sender restarts at counter
 * 0. Without the reset, epoch 46's window 0 lands in the SAME buffer under the
 * SAME window start (0), nothing about it looks wrong, its fresh MAC covers
 * only epoch 46's packets and verifies - and the flush delivers all six,
 * epoch 45's two among them, as VERIFIED.
 */
ZTEST(radiant_sec, test_held_packets_do_not_survive_set_epoch)
{
	uint32_t i;

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_DROP_UNVER, 4u);
	epoch_both(45u);

	hop(0u, false, HOLD_MARK_BIT);
	hop(1u, false, HOLD_MARK_BIT);
	zassert_equal(0u, dropped_policy_now(), "setup: nothing dropped yet");

	epoch_both(46u);
	zassert_equal(2u, dropped_policy_now(),
		      "the two held packets were not dropped and counted by "
		      "the epoch change");

	for (i = 0; i < 8u; i++) {
		hop(i, false, -1);
	}

	assert_no_marked_delivery();
	zassert_equal(4u, log_n_pkt,
		      "expected epoch 46's window 0 alone (4 packets), got %u",
		      log_n_pkt);
	zassert_equal(2u, dropped_policy_now());
}

/*
 * try_devnum mid-window - the Tier 2 shape. Hops 0 and 1 are heard from the
 * sensor under its old device number; it power-cycles onto a new one,
 * restarting its counter under the same epoch, and the receiver tries the new
 * number. Exactly the set_epoch case above with a different reset site: same
 * buffer, same window start, a fresh MAC that verifies, six delivered without
 * the reset.
 */
ZTEST(radiant_sec, test_held_packets_do_not_survive_try_devnum)
{
	const uint8_t w = 4u;
	uint32_t      i;

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_DROP_UNVER, w);
	epoch_both(11u);

	hop(0u, false, HOLD_MARK_BIT);
	hop(1u, false, HOLD_MARK_BIT);

	radiant_sec_channel_release(TX_CH);
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_key(TX_CH, test_root, 128, TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_configure(TX_CH,
					    RADIANT_SEC_SW_AUTH |
						    RADIANT_SEC_SW_DROP_UNVER,
					    w, RADIANT_SEC_PAGE_LO_DEFAULT,
					    RADIANT_SEC_PAGE_HI_DEFAULT));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_epoch(TX_CH, 11u, 0u, TEST_PERIOD));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_devnum(TX_CH, TIER2_NEW_DEVNUM));

	zassert_equal(0u, dropped_policy_now(), "setup: nothing dropped yet");
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_try_devnum(RX_CH, TIER2_NEW_DEVNUM));
	zassert_equal(2u, dropped_policy_now(),
		      "the two held packets were not dropped and counted by "
		      "the trial");

	for (i = 0; i < 2u * w; i++) {
		hop(i, false, -1);
	}

	assert_no_marked_delivery();
	zassert_equal(4u, log_n_pkt,
		      "expected the new identity's window 0 alone (4 "
		      "packets), got %u", log_n_pkt);
	zassert_equal(RADIANT_SEC_VERDICT_VERIFIED,
		      radiant_sec_last_verdict(RX_CH),
		      "setup: the new identity did not verify");
}

/*
 * The epoch-advance site in rx_process(): a receiver that sees time cross a
 * counter wrap advances its own epoch, and must drop what it held from the
 * old one there.
 *
 * Reached WITHOUT running the sender through 65536 slots: the two pre-wrap
 * packets (counters 65532/65533, an open window) are ingested raw - they are
 * only ever held, so nothing needs their MAC - and the sender's first transform
 * is slot 65536 itself. Its counter is the slot index, so that one call is its
 * own wrap: epoch 48, counter 0.
 *
 * The packet that carries the advance (counter 0 of epoch 48) must itself be
 * accepted. It used to be refused as a replay: `expected` was computed
 * against the pre-advance anchor (65536) while the resolved counter was 0, so
 * window 0 of every new epoch lost its first packet and could never verify.
 * `expected` is now rebased onto the advanced anchor, which this pins.
 */
ZTEST(radiant_sec, test_held_packets_do_not_survive_an_epoch_advance_at_the_wrap)
{
	uint32_t i;

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_DROP_UNVER, 4u);
	epoch_both(47u);

	for (i = 65532u; i < 65534u; i++) {
		uint8_t pay[8] = {
			TEST_PAGE, (uint8_t)i, (uint8_t)i, 0x11u,
			0x23u, 0x33u, 0x44u, 0x00u,
		};

		zassert_equal(RADIANT_SEC_RX_DEFER,
			      radiant_sec_rx_ingest(RX_CH, pay, sizeof(pay), true,
						    sec_anchor +
							    (uint64_t)i *
								    period_us()));
		radiant_sec_pump();
	}
	zassert_equal(0u, log_n_pkt, "setup: an open window was released");
	zassert_equal(0u, dropped_policy_now(), "setup: nothing dropped yet");

	/* The sender's own wrap - epoch + 1, counter back to 0 - happens inside
	 * this hop: slot 65536 is past the end of epoch 47. */
	hop(65536u, false, -1);
	zassert_equal(2u, dropped_policy_now(),
		      "the receiver's epoch advance kept the old epoch's held "
		      "packets");
	{
		struct radiant_sec_stats st;

		radiant_sec_get_stats(RX_CH, &st);
		zassert_equal(1u, st.epoch_advances,
			      "setup: the receiver did not advance its epoch");
		zassert_equal(0u, st.dropped_replay,
			      "the packet that carried the epoch advance was "
			      "refused as a replay, so window 0 of the new "
			      "epoch can never verify");
	}

	for (i = 65537u; i < 65536u + 12u; i++) {
		hop(i, false, -1);
	}
	assert_no_marked_delivery();
	zassert_equal(1u, verdicts_for(0u, RADIANT_SEC_VERDICT_VERIFIED),
		      "window 0 of the new epoch did not verify");
	/* Windows 0 and 4 released; window 8 still waits for its verdict. */
	zassert_equal(8u, log_n_pkt,
		      "expected windows 0 and 4 of epoch 48 (8 packets), got %u",
		      log_n_pkt);
}

/* ── The clock against a keyless injector ───────────────────────────────── */

/*
 * The receiver's clock (epoch_anchor) decides which counter a packet resolves
 * to and whether it is a replay, so input nobody has verified must not move
 * it. It used to re-anchor on every packet that passed the replay check -
 * before any MAC was checked - and that check lets through anything up to 128
 * periods behind. A forged packet claiming expected-127 pulled the clock back
 * 127 periods; three of them and recorded genuine windows came round again as
 * VERIFIED. Claiming expected+127 instead walked it forward into
 * advance_epoch(), which is monotone: the real sender was never heard again.
 *
 * The attacker here is stronger than a real one: it reads the receiver's own
 * expected_index before every forgery.
 */

#define REC_MAX 80u

static uint8_t rec[REC_MAX][8];

/* A packet built without the key: page and counter byte chosen, everything
 * else - tag byte included - arbitrary. */
static void forge(uint64_t t_sync, uint8_t low)
{
	const uint8_t air[8] = { TEST_PAGE, low,   0xF0u, 0x0Fu,
				 0x5Au,     0xA5u, 0x3Cu, 0xC3u };

	ingest_at(air, t_sync);
}

/* The receiver's expected counter at `t`. get_state reads radiant_radio_now(),
 * so the fake clock is moved there first. */
static uint16_t rx_expected_at(uint64_t t)
{
	struct radiant_sec_state s;

	fake_radio_advance_to((radiant_time_t)t);
	radiant_sec_get_state(RX_CH, &s);
	return s.expected_index;
}

static struct radiant_sec_stats rx_stats(void)
{
	struct radiant_sec_stats st;

	radiant_sec_get_stats(RX_CH, &st);
	return st;
}

static uint32_t verified_count(void)
{
	uint32_t n = 0u;
	uint32_t i;

	for (i = 0; i < log_n_verdict && i < LOG_MAX; i++) {
		if (log_verdict[i].verdict == RADIANT_SEC_VERDICT_VERIFIED) {
			n++;
		}
	}
	return n;
}

static void forged_drag_back(uint8_t switches)
{
	const uint32_t now_i = 300u;
	uint64_t       t;
	uint16_t       before;
	uint16_t       e;
	uint32_t       i;

	key_both();
	configure_both(switches, 4u);
	epoch_both(53u);
	t = sec_anchor + (uint64_t)now_i * period_us();

	/* Windows 0..7 recorded off the air as they go by. */
	for (i = 0u; i < now_i; i++) {
		if (i < 32u) {
			tx_hop(i, rec[i]);
			ingest_at(rec[i], sec_anchor + (uint64_t)i * period_us());
		} else {
			hop(i, false, -1);
		}
	}
	zassert_equal(1u, verdicts_for(0u, RADIANT_SEC_VERDICT_VERIFIED),
		      "setup: window 0 never verified the first time");
	before = rx_expected_at(t);
	zassert_equal(now_i, before, "setup: the receiver's clock reads %u",
		      before);

	log_n_pkt = 0u;
	log_n_verdict = 0u;

	/* Drag it back: each forgery claims the oldest counter the replay check
	 * lets through, until the clock reads 0. */
	for (i = 0u; i < 40u; i++) {
		e = rx_expected_at(t);
		if (e == 0u) {
			break;
		}
		forge(t, (uint8_t)((e > 127u) ? (e - 127u) : 0u));
	}
	e = rx_expected_at(t);
	zassert_equal(before, e,
		      "%u keyless forgeries moved the receiver's clock from %u "
		      "to %u", i, before, e);

	/* Then replay the recorded windows, as if they were current. */
	for (i = 0u; i < 32u; i++) {
		ingest_at(rec[i], t + (uint64_t)i * period_us());
	}
	zassert_equal(0u, verified_count(),
		      "a recorded window was replayed and judged VERIFIED: "
		      "replay protection is gone");
	if ((switches & RADIANT_SEC_SW_DROP_UNVER) != 0u) {
		zassert_equal(0u, log_n_pkt,
			      "the drop policy released %u replayed or forged "
			      "packets", log_n_pkt);
	}
}

ZTEST(radiant_sec, test_a_forged_counter_cannot_drag_the_clock_back)
{
	forged_drag_back(RADIANT_SEC_SW_AUTH);
}

ZTEST(radiant_sec, test_a_forged_counter_cannot_drag_the_clock_back_under_the_drop_policy)
{
	forged_drag_back(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_DROP_UNVER);
}

ZTEST(radiant_sec, test_a_forged_walk_forward_cannot_advance_the_epoch)
{
	struct radiant_sec_stats st;
	uint32_t                 verified;
	uint64_t                 t;
	uint32_t                 i;

	/* The radio up for ~5.6 h rather than fake_radio's default ~71 min:
	 * the walk sets the anchor to t - counter * period, so below 65536
	 * periods of uptime it clamps at 0 short of the wrap. On master that
	 * clamp hid the epoch advance this test exists for (the sender still
	 * stopped verifying, for the clamp's reason instead). */
	fake_radio_set_origin((radiant_time_t)20000000000ull);
	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 4u);
	epoch_both(59u);
	for (i = 0u; i < 300u; i++) {
		hop(i, false, -1);
	}
	t = sec_anchor + 300ull * period_us();

	/* (65536 - 300) / 127 = 514 steps to the wrap; 600 to be sure. */
	for (i = 0u; i < 600u; i++) {
		forge(t, (uint8_t)(rx_expected_at(t) + 127u));
	}
	st = rx_stats();
	zassert_equal(0u, st.epoch_advances,
		      "keyless forgeries walked the receiver into the next "
		      "epoch; the epoch is monotone, so the real sender is "
		      "never verified again");

	/* The real sender, carrying on. The close of window 300 judges the last
	 * forged window; the closes of 304 and 308 judge 300 and 304. */
	verified = st.windows_verified;
	for (i = 300u; i < 312u; i++) {
		hop(i, false, -1);
	}
	st = rx_stats();
	zassert_equal(verified + 2u, st.windows_verified,
		      "the real sender no longer verifies after the forgeries");
	zassert_equal(RADIANT_SEC_VERDICT_VERIFIED,
		      radiant_sec_last_verdict(RX_CH));
}

/*
 * The replay floor. Re-anchoring only on verified windows is not enough on its
 * own: a window already verified could be replayed - genuine bytes, inside the
 * 128-period slack - and verify again. Each window may be VERIFIED at most
 * once per epoch.
 */
static void verified_once(uint8_t switches)
{
	uint32_t replay_before;
	uint32_t n8 = 0u;
	uint32_t i;

	key_both();
	configure_both(switches, 4u);
	epoch_both(61u);
	for (i = 0u; i < 20u; i++) {
		tx_hop(i, rec[i]);
		ingest_at(rec[i], sec_anchor + (uint64_t)i * period_us());
	}
	zassert_equal(1u, verdicts_for(8u, RADIANT_SEC_VERDICT_VERIFIED),
		      "setup: window 8 did not verify");
	replay_before = rx_stats().dropped_replay;

	/* Window 8 and the window carrying its tag, again, 12 periods late. */
	for (i = 8u; i < 16u; i++) {
		ingest_at(rec[i], sec_anchor + (uint64_t)(i + 12u) * period_us());
	}

	zassert_equal(1u, verdicts_for(8u, RADIANT_SEC_VERDICT_VERIFIED),
		      "a replay of window 8 was judged VERIFIED a second time");
	zassert_equal(replay_before + 8u, rx_stats().dropped_replay,
		      "the replayed packets were not refused as replays");
	if ((switches & RADIANT_SEC_SW_DROP_UNVER) != 0u) {
		for (i = 0u; i < log_n_pkt && i < LOG_MAX; i++) {
			if (log_pkt[i].pay[2] == 8u) {
				n8++;
			}
		}
		zassert_equal(1u, n8, "packet 8 was released %u times", n8);
	}
}

ZTEST(radiant_sec, test_a_verified_window_never_verifies_twice)
{
	verified_once(RADIANT_SEC_SW_AUTH);
}

ZTEST(radiant_sec, test_a_verified_window_never_verifies_twice_under_the_drop_policy)
{
	verified_once(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_DROP_UNVER);
}

/*
 * The other side of the fix: the clock must still follow a real sender. A
 * 0.5 % rate error (ten times a bad crystal) over 30000 packets is 150
 * periods - past the 128-period slack, so a receiver that stopped tracking
 * would lose the sender part-way through. Verified-window re-anchoring, with
 * its slew limited to period/16 per window, has to keep up the whole way.
 */
static void drift_case(int32_t ppm)
{
	struct radiant_sec_stats st;
	uint8_t                  air[8];
	uint32_t                 i;

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 4u);
	epoch_both(67u);
	for (i = 0u; i < 30000u; i++) {
		int64_t at = (int64_t)i * (int64_t)period_us() *
			     (1000000 + (int64_t)ppm) / 1000000;

		tx_hop(i, air);
		ingest_at(air, sec_anchor + (uint64_t)at);
	}
	st = rx_stats();
	zassert_equal(0u, st.windows_unverified,
		      "%d ppm: %u windows failed to verify", ppm,
		      st.windows_unverified);
	zassert_equal(30000u / 4u - 1u, st.windows_verified,
		      "%d ppm: %u windows verified", ppm, st.windows_verified);
}

ZTEST(radiant_sec, test_the_clock_still_follows_a_slow_sender)
{
	drift_case(5000);
}

ZTEST(radiant_sec, test_the_clock_still_follows_a_fast_sender)
{
	drift_case(-5000);
}

/*
 * Jam and replay late. The attacker records the real sender while jamming it
 * and replays what it recorded later. Delivered 100 periods late it verifies
 * (the bytes are genuine, and 100 is inside the slack), and that is the
 * documented cost of tracking drift at all. What must not happen is for the
 * clock to FOLLOW the replay: per-packet re-anchoring caught up with the
 * delay at once, so the next batch could be 100 periods later again, and so
 * on without limit. With the slew limited to period/16 per verified window,
 * a replay 200 periods late is past the slack, resolves to the wrong counter
 * (256 on), and fails its MAC.
 */
ZTEST(radiant_sec, test_a_delayed_replay_cannot_grow_its_delay)
{
	uint32_t verified;
	uint32_t i;

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 4u);
	epoch_both(71u);
	for (i = 0u; i < 40u; i++) {
		hop(i, false, -1);
	}
	/* 40..119 jammed: recorded, never heard on time. */
	for (i = 40u; i < 120u; i++) {
		tx_hop(i, rec[i - 40u]);
	}

	verified = rx_stats().windows_verified;
	for (i = 40u; i < 80u; i++) {
		ingest_at(rec[i - 40u],
			  sec_anchor + (uint64_t)(i + 100u) * period_us());
	}
	zassert_equal(verified + 10u, rx_stats().windows_verified,
		      "setup: windows 36..72 did not verify 100 periods late");

	log_n_verdict = 0u;
	for (i = 80u; i < 120u; i++) {
		ingest_at(rec[i - 40u],
			  sec_anchor + (uint64_t)(i + 200u) * period_us());
	}
	/* Window 76 is the one exception, and not a hole: it was replayed in
	 * the first batch, and its tag bytes ride in packets 80..83. */
	for (i = 0u; i < log_n_verdict && i < LOG_MAX; i++) {
		zassert_false(log_verdict[i].verdict ==
					      RADIANT_SEC_VERDICT_VERIFIED &&
				      log_verdict[i].window_start != 76u,
			      "window %u verified 200 periods late: the clock "
			      "followed the replay",
			      log_verdict[i].window_start);
	}
}

/* ── The counter is the slot index (TODO item 7) ────────────────────────── */

/*
 * The receiver's expected_index() is elapsed time over the period: one count
 * per SLOT. A sender that counted only the slots it transformed fell one count
 * behind for every slot it did not: the descriptor frames and common pages of
 * every 121-message cycle, every slot MPSL denied, every slot a sparse node
 * kept silent. The receiver slews at most period/16 per verified window, which
 * is slower than that gap grows, so within minutes the gap passed the
 * 128-period slack and nothing on the channel verified again.
 *
 * These run long streams and check the receiver against a model computed from
 * the slot index alone: window k (slots kW..kW+W-1) verifies exactly when all
 * of its slots carried secured packets AND so did every slot of the next window
 * that carried any secured packet at all - that window holds k's tag bytes.
 * A clear or skipped slot inside a window therefore costs that window and the
 * one before it, and never makes anything wrongly VERIFIED.
 */

enum stream_slot {
	STREAM_SECURED,
	STREAM_CLEAR,      /* transmitted, outside the secured page range */
	STREAM_SKIPPED,    /* never transformed, never transmitted */
};

#define STREAM_SLOTS 20000u
#define STREAM_CYCLE 121u

/* The 0x60 interleave: descriptor frames at m = 0..stream_desc_frames-1, page
 * 0x50 at m = 119 and 0x51 at m = 120 - one contiguous clear run per cycle,
 * 2 + stream_desc_frames slots long. */
static uint8_t stream_desc_frames;

static enum stream_slot cycle_slot(uint32_t slot, uint8_t *page)
{
	uint32_t m = slot % STREAM_CYCLE;

	if (m < stream_desc_frames) {
		*page = 0x00u;
		return STREAM_CLEAR;
	}
	if (m == 119u) {
		*page = 0x50u;
		return STREAM_CLEAR;
	}
	if (m == 120u) {
		*page = 0x51u;
		return STREAM_CLEAR;
	}
	*page = TEST_PAGE;
	return STREAM_SECURED;
}

/* A master that loses one slot in forty to MPSL: the slot clock advances a
 * full period and the transform is never called for it. */
static enum stream_slot skip_slot(uint32_t slot, uint8_t *page)
{
	*page = TEST_PAGE;
	return ((slot % 40u) == 39u) ? STREAM_SKIPPED : STREAM_SECURED;
}

static bool window_full(enum stream_slot (*kind)(uint32_t, uint8_t *),
			uint32_t k, uint8_t w)
{
	uint8_t  page;
	uint32_t s;

	for (s = k * w; s < (k + 1u) * w; s++) {
		if (kind(s, &page) != STREAM_SECURED) {
			return false;
		}
	}
	return true;
}

static bool window_any(enum stream_slot (*kind)(uint32_t, uint8_t *),
		       uint32_t k, uint8_t w)
{
	uint8_t  page;
	uint32_t s;

	for (s = k * w; s < (k + 1u) * w; s++) {
		if (kind(s, &page) == STREAM_SECURED) {
			return true;
		}
	}
	return false;
}

/* Runs one stream and prints what it measured. Returns false, saying why,
 * instead of asserting, so one ZTEST reports every W before failing. */
static bool stream_case(uint8_t switches, uint8_t w,
			enum stream_slot (*kind)(uint32_t, uint8_t *),
			const char *label)
{
	struct radiant_sec_stats st;
	uint32_t                 n_win = STREAM_SLOTS / w;
	uint32_t                 secured = 0u;
	uint32_t                 in_verified = 0u;
	uint32_t                 expected = 0u;
	uint32_t                 missing = 0u;
	uint32_t                 extra = 0u;
	uint32_t                 last_cycle = 0u;
	uint32_t                 slot;
	uint32_t                 k;

	sec_before(NULL);
	key_both();
	configure_both(switches, w);
	epoch_both(73u);

	for (slot = 0u; slot < STREAM_SLOTS; slot++) {
		uint8_t          air[8];
		uint8_t          page;
		enum stream_slot sk = kind(slot, &page);

		if (sk == STREAM_SKIPPED) {
			continue;
		}
		air[0] = page;
		air[1] = 0xAAu;
		air[2] = (uint8_t)slot;
		air[3] = (uint8_t)(slot >> 8);
		air[4] = 0x22u;
		air[5] = 0x33u;
		air[6] = 0x44u;
		air[7] = 0x55u;
		zassert_equal(RADIANT_SEC_OK,
			      radiant_sec_tx_transform(TX_CH, air, 8u,
						       slot_t(slot)),
			      "%s: slot %u refused", label, slot);
		if (sk == STREAM_CLEAR) {
			zassert_equal(0xAAu, air[1],
				      "%s: clear slot %u was transformed", label,
				      slot);
			zassert_equal(RADIANT_SEC_RX_PLAIN,
				      radiant_sec_rx_ingest(RX_CH, air, 8u, true,
							    slot_t(slot)),
				      "%s: clear slot %u was taken", label,
				      slot);
		} else {
			secured++;
			ingest_at(air, slot_t(slot));
		}
	}

	/* The model. A window's verdict exists only once the window holding its
	 * tag has closed in-window, so the last one or two are not judged. */
	for (k = 0u; k < n_win; k++) {
		bool     want = false;
		uint32_t next;

		if (window_full(kind, k, w)) {
			for (next = k + 1u;
			     next < n_win && !window_any(kind, next, w); next++) {
			}
			if (next >= n_win) {
				continue;   /* never judged in this run */
			}
			want = window_full(kind, next, w);
		}
		if (want) {
			expected++;
		}
		if (want && !window_was_verified((uint16_t)(k * w))) {
			missing++;
		}
		if (!want && window_was_verified((uint16_t)(k * w))) {
			extra++;
		}
		if (window_was_verified((uint16_t)(k * w))) {
			in_verified += w;
			if (k * w >= STREAM_SLOTS - STREAM_CYCLE) {
				last_cycle++;
			}
		}
	}

	radiant_sec_get_stats(RX_CH, &st);
	TC_PRINT("%s W=%u sw=0x%02x: verified %u unverified %u replay %u | "
		 "model %u windows, missing %u, extra %u | secured %u, in "
		 "verified windows %u (%u.%u %%)\n",
		 label, w, switches, st.windows_verified,
		 st.windows_unverified, st.dropped_replay, expected, missing,
		 extra, secured, in_verified,
		 (secured != 0u) ? (in_verified * 100u) / secured : 0u,
		 (secured != 0u) ? ((in_verified * 1000u) / secured) % 10u
				 : 0u);

	if (st.dropped_replay != 0u) {
		TC_PRINT("  FAIL: %u packets refused as replays\n",
			 st.dropped_replay);
	}
	if (missing != 0u) {
		TC_PRINT("  FAIL: %u of %u windows the slot index says should "
			 "verify did not - the sender's counter left the "
			 "receiver's clock\n", missing, expected);
	}
	if (extra != 0u) {
		TC_PRINT("  FAIL: %u windows verified that the slot index says "
			 "hold a hole or lost their tag\n", extra);
	}
	if (last_cycle == 0u) {
		TC_PRINT("  FAIL: nothing verified in the last cycle\n");
	}
	return st.dropped_replay == 0u && missing == 0u && extra == 0u &&
	       last_cycle > 0u;
}

ZTEST(radiant_sec, test_clear_slots_inside_a_secured_stream_do_not_desync_the_counter)
{
	bool ok = true;

	/* The worst case of section 6's interleave: ten descriptor frames, so a
	 * twelve-slot clear run every cycle. */
	stream_desc_frames = 10u;
	ok &= stream_case(RADIANT_SEC_SW_AUTH, 2u, cycle_slot, "clear12");
	ok &= stream_case(RADIANT_SEC_SW_AUTH, 4u, cycle_slot, "clear12");
	ok &= stream_case(RADIANT_SEC_SW_AUTH, 8u, cycle_slot, "clear12");
	ok &= stream_case(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_CONF, 2u,
			  cycle_slot, "clear12");
	ok &= stream_case(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_CONF, 8u,
			  cycle_slot, "clear12");

	/* And the shortest: three descriptor frames, a five-slot run. With the
	 * W = 2/4/8 runs above, these are the numbers docs/radiant-security.md
	 * 3.2 quotes. */
	stream_desc_frames = 3u;
	ok &= stream_case(RADIANT_SEC_SW_AUTH, 2u, cycle_slot, "clear5");
	ok &= stream_case(RADIANT_SEC_SW_AUTH, 4u, cycle_slot, "clear5");
	ok &= stream_case(RADIANT_SEC_SW_AUTH, 8u, cycle_slot, "clear5");

	zassert_true(ok, "a stream above failed; its FAIL lines say how");
}

ZTEST(radiant_sec, test_skipped_slots_do_not_desync_the_counter)
{
	bool ok = true;

	ok &= stream_case(RADIANT_SEC_SW_AUTH, 2u, skip_slot, "skip1in40");
	ok &= stream_case(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_CONF, 8u,
			  skip_slot, "skip1in40");

	zassert_true(ok, "a stream above failed; its FAIL lines say how");
}

/*
 * Slot-aligned sparse under X_CONF alone: three packets, a thousand silent
 * slots, three more. The silence is far past the 128-period slack, so a counter
 * that counted transmissions sent counter 3 at slot 1003, the receiver resolved
 * it to the rollover nearest 1003, and decrypted under the wrong nonce.
 * (Sparse with X_AUTH is refused in v1: the tag lags a window, so a burst
 * never carries its own tag and never verifies.)
 */
ZTEST(radiant_sec, test_slot_aligned_sparse_conf_survives_a_long_silence)
{
	static const uint32_t slots[] = { 0u, 1u, 2u, 1003u, 1004u, 1005u };
	struct radiant_sec_stats st;
	uint8_t                  air[8];
	uint32_t                 i;

	key_both();
	configure_both(RADIANT_SEC_SW_CONF, 2u);
	epoch_both(79u);

	for (i = 0u; i < ARRAY_SIZE(slots); i++) {
		tx_air(slots[i], air);
		zassert_equal((uint8_t)slots[i], air[1],
			      "slot %u went out with counter byte %u", slots[i],
			      air[1]);
		ingest_at(air, slot_t(slots[i]));
	}

	radiant_sec_get_stats(RX_CH, &st);
	zassert_equal(0u, st.dropped_replay, "%u refused as replays",
		      st.dropped_replay);
	zassert_equal(ARRAY_SIZE(slots), log_n_pkt, "%u delivered", log_n_pkt);
	for (i = 0u; i < ARRAY_SIZE(slots); i++) {
		zassert_mem_equal(&log_pkt[i].pay[2], &conf_plain[2], 5u,
				  "slot %u did not decrypt to the plaintext",
				  slots[i]);
	}
}

/*
 * A slot posted twice - request_tx refused and the slot re-posted, the shape
 * the arming path can produce - must resend the bytes already built for it: a
 * second ciphertext under the same nonce is keystream reuse, and a second
 * absorb into the window CMAC breaks a window the receiver hears only once.
 * A slot EARLIER than one already built is refused outright.
 */
ZTEST(radiant_sec, test_a_reposted_slot_reuses_its_bytes)
{
	uint8_t  first[8];
	uint8_t  again[8];
	uint8_t  air[8];
	uint32_t i;

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_CONF, 4u);
	epoch_both(83u);

	for (i = 0u; i < 12u; i++) {
		if (i != 5u) {
			hop(i, false, -1);
			continue;
		}
		tx_hop(5u, first);
		/* The host wrote a new body before the re-post. */
		memset(again, 0x99u, sizeof(again));
		again[0] = TEST_PAGE;
		zassert_equal(RADIANT_SEC_OK,
			      radiant_sec_tx_transform(TX_CH, again, 8u,
						       slot_t(5u)));
		zassert_mem_equal(first, again, 8u,
				  "a re-posted slot built different bytes: a "
				  "second ciphertext under slot 5's nonce");
		ingest_at(first, slot_t(5u));
	}

	zassert_equal(1u, verdicts_for(0u, RADIANT_SEC_VERDICT_VERIFIED),
		      "setup: window 0 did not verify");
	zassert_equal(1u, verdicts_for(4u, RADIANT_SEC_VERDICT_VERIFIED),
		      "window 4, which holds the re-posted slot, did not "
		      "verify: the re-post was absorbed into its MAC twice");

	memcpy(air, conf_plain, sizeof(air));
	zassert_not_equal(RADIANT_SEC_OK,
			  radiant_sec_tx_transform(TX_CH, air, 8u, slot_t(10u)),
			  "slot 10 was built again after slot 11");
}

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

ZTEST(radiant_sec, test_release_and_reset_wipe_the_keys)
{
	uint8_t pay[8] = { TEST_PAGE, 0xAA, 1, 2, 3, 4, 5, 6 };
	uint8_t before[8];

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 2u);
	epoch_both(31u);

	radiant_sec_channel_release(TX_CH);
	memcpy(before, pay, sizeof(pay));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_tx_transform(TX_CH, pay, sizeof(pay),
					       slot_t(0u)));
	zassert_mem_equal(pay, before, sizeof(pay),
			  "a released channel still transforms");

	/* And configuring one with no key is a host sequencing error rather
	 * than a state worth inventing a context for. */
	zassert_equal(RADIANT_SEC_ENOKEY,
		      radiant_sec_configure(TX_CH, RADIANT_SEC_SW_AUTH, 2u,
					    0x01u, 0x0Fu));

	radiant_sec_reset();
	zassert_equal(RADIANT_SEC_ENOKEY,
		      radiant_sec_set_epoch(RX_CH, 99u, 0u, TEST_PERIOD));
}
