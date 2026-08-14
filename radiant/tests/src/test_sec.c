/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The two payload transforms, driven end to end inside one process. Channel 0
 * is the sensor, channel 1 the receiver; both share one root key, epoch, and
 * on-air device number (the 1:N model - sensor keeps zero per-receiver state).
 *
 * Uses fake_radio's virtual clock so t_sync can be chosen rather than waited
 * for. The receiver derives packet index from epoch phase + channel period,
 * not arrival history, which is what makes mid-stream join and gap cases
 * testable at all.
 *
 * Coverage: happy path, flipped bit (forgery caught once), lost packet (one
 * window breaks, next recovers), injected ack (D15 - MAC not skippable by
 * message type), epoch gate (D3/D17 - no transform before epoch set), clear
 * page (descriptor/common pages untouched).
 */

#include <zephyr/ztest.h>
#include <string.h>

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
}

/* ── Harness ────────────────────────────────────────────────────────────── */

static uint64_t period_us(void)
{
	return ((uint64_t)TEST_PERIOD * 1000000u) / 32768u;
}

/* Epoch anchor as the receiver computed it. radiant_sec_set_epoch() anchors on
 * radiant_radio_now(), and fake_radio's clock starts one second below 2^32, not
 * zero - so t_sync must be anchor-relative (sec_anchor + index*period), or
 * expected_index() floors every packet to 0 and the test passes without
 * exercising D4's time-derived counter reconstruction at all. */
static uint64_t sec_anchor;

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

/* Build packet `index` as the sensor would, and hand it to the receiver unless
 * `drop`. `corrupt_bit` flips one payload bit, simulating an active injector. */
static void hop(uint32_t index, bool drop, int corrupt_bit)
{
	uint8_t  pay[8];
	uint64_t t_sync;
	int      rc;

	memset(pay, 0, sizeof(pay));
	pay[0] = TEST_PAGE;
	pay[1] = 0xAAu;                 /* the host's byte [1], which the
					 * transform must overwrite */
	pay[2] = (uint8_t)index;
	pay[3] = 0x11u;
	pay[4] = 0x22u;
	pay[5] = 0x33u;
	pay[6] = 0x44u;
	pay[7] = 0x55u;                 /* likewise the tag byte */

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_tx_transform(TX_CH, pay, sizeof(pay)));

	if (corrupt_bit >= 0) {
		pay[corrupt_bit / 8] ^= (uint8_t)(1u << (corrupt_bit % 8));
	}

	/* Packet i is one period after i-1, measured from the epoch anchor. */
	t_sync = sec_anchor + (uint64_t)index * period_us();

	if (drop) {
		return;
	}

	rc = radiant_sec_rx_ingest(RX_CH, pay, sizeof(pay), true, t_sync);
	zassert_equal(RADIANT_SEC_RX_DEFER, rc,
		      "a secured-range broadcast must be taken by the "
		      "transform, not delivered in ISR context");
	radiant_sec_pump();
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
		      radiant_sec_tx_transform(TX_CH, pay, sizeof(pay)));
	zassert_mem_equal(pay, before, sizeof(pay),
			  "a transform ran before the epoch was set");

	zassert_equal(RADIANT_SEC_RX_PLAIN,
		      radiant_sec_rx_ingest(RX_CH, pay, sizeof(pay), true, 0u));

	epoch_both(1000u);
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_tx_transform(TX_CH, pay, sizeof(pay)));
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

	/* Three windows: window 0 has no tag (it lags one window), so its
	 * verdict never comes; windows 4 and 8 authenticate 0 and 4. */
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
			      radiant_sec_tx_transform(TX_CH, pay,
						       sizeof(pay)));
		zassert_equal(i, pay[1],
			      "byte [1] must count TRANSMISSIONS. A host "
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
		      radiant_sec_tx_transform(TX_CH, pay, sizeof(pay)));
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
			      radiant_sec_tx_transform(TX_CH, pay,
						       sizeof(pay)));

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
	 * host; window 4 verifies and is released. The default is the other way
	 * (deliver, marked) - the choice of suspect reading vs. hole is the
	 * application's call. The first window is also held since nothing
	 * authenticates it, a stated cost of the one-window tag lag. */
	zassert_equal(4u, log_n_pkt,
		      "expected only the verified window to be released, got "
		      "%u packets", log_n_pkt);
	for (i = 0; i < log_n_pkt && i < LOG_MAX; i++) {
		zassert_equal(RADIANT_SEC_VERDICT_VERIFIED,
			      log_pkt[i].verdict,
			      "an unverified packet escaped the drop policy");
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

/* Transform one packet at `index` and hand back exactly what went on the air
 * (hop() deliberately doesn't expose this). */
static void tx_air(uint32_t index, uint8_t air[8])
{
	ARG_UNUSED(index);

	memcpy(air, conf_plain, 8u);
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_tx_transform(TX_CH, air, 8u));
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
	tx_air(0u, again);

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
	 * the tag lags one window (window k authenticates k-1), so the first
	 * window heard is never the one that verifies. */
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
		      radiant_sec_tx_transform(TX_CH, pay, sizeof(pay)));
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
