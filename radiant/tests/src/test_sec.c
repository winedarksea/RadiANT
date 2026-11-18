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

/* A second root, for re-keying a live channel. */
static const uint8_t other_root[16] = {
	0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78,
	0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0,
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
				enum radiant_sec_verdict verdict, uint8_t w,
				uint8_t switches, uint16_t seq)
{
	ARG_UNUSED(w);
	ARG_UNUSED(switches);
	ARG_UNUSED(seq);
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
	zassert_equal(RADIANT_SEC_RX_DEFER, radiant_sec_rx_ingest(RX_CH, air, 8u, true, t_sync));
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
	 * epoch. Closed means closed (item 5): keyed and pending, a secured
	 * page is withheld on TX and dropped on RX, never passed in the
	 * clear. */
	struct radiant_sec_stats st;
	struct radiant_sec_state state;

	memcpy(before, pay, sizeof(pay));
	zassert_equal(RADIANT_SEC_EUNARMED,
		      radiant_sec_tx_transform(TX_CH, pay, sizeof(pay),
					       (uint64_t)radiant_radio_now()));
	zassert_mem_equal(pay, before, sizeof(pay));

	zassert_equal(RADIANT_SEC_RX_DROP,
		      radiant_sec_rx_ingest(RX_CH, pay, sizeof(pay), true, 0u));
	radiant_sec_pump();
	zassert_equal(0u, log_n_pkt);
	radiant_sec_get_stats(RX_CH, &st);
	zassert_equal(1u, st.dropped_policy);

	zassert_true(radiant_sec_channel_is_secured(TX_CH));
	radiant_sec_get_state(TX_CH, &state);
	zassert_true(state.keyed);
	zassert_false(state.secured);

	epoch_both(1000u);
	radiant_sec_get_state(TX_CH, &state);
	zassert_true(state.keyed && state.secured);
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_tx_transform(TX_CH, pay, sizeof(pay),
					       slot_t(0u)));
	zassert_true(memcmp(pay, before, sizeof(pay)) != 0);
}

ZTEST(radiant_sec, test_the_epoch_only_moves_forwards)
{
	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 2u);

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_epoch(TX_CH, 100u, 0u, TEST_PERIOD));
	zassert_equal(RADIANT_SEC_EINVAL, radiant_sec_set_epoch(TX_CH, 100u, 0u, TEST_PERIOD));
	zassert_equal(RADIANT_SEC_EINVAL, radiant_sec_set_epoch(TX_CH, 99u, 0u, TEST_PERIOD));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_epoch(TX_CH, 101u, 0u, TEST_PERIOD));

	/* A wrap must always have headroom to advance into. */
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_set_epoch(TX_CH, 0xFFFFFFFFu, 0u, TEST_PERIOD));
}

/* Item 5: a different root on a live channel makes it PENDING - withheld and
 * dropped, never clear - until the next epoch, which may be any epoch (new
 * keys, no nonce under them used). Ciphertext queued under the old root dies
 * with it. */
ZTEST(radiant_sec, test_a_new_root_on_a_live_channel_fails_closed_until_0xf3)
{
	uint8_t                  pay[8] = { TEST_PAGE, 0xAA, 1, 2, 3, 4, 5, 6 };
	uint8_t                  air[8];
	struct radiant_sec_stats st;
	struct radiant_sec_state state;
	uint32_t                 i;

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 4u);
	epoch_both(7u);
	for (i = 0u; i < 4u; i++) {
		hop(i, false, -1);
	}
	tx_hop(4u, air);
	zassert_equal(RADIANT_SEC_RX_DEFER,
		      radiant_sec_rx_ingest(RX_CH, air, 8u, true, slot_t(4u)));

	zassert_ok(radiant_sec_set_key(TX_CH, other_root, 128, TEST_DEVNUM));
	zassert_ok(radiant_sec_set_key(RX_CH, other_root, 128, TEST_DEVNUM));
	radiant_sec_pump();
	zassert_equal(4u, log_n_pkt, "%u", log_n_pkt);

	zassert_equal(RADIANT_SEC_EUNARMED,
		      radiant_sec_tx_transform(TX_CH, pay, 8u, slot_t(5u)));
	zassert_equal(0xAAu, pay[1]);
	zassert_equal(RADIANT_SEC_RX_DROP,
		      radiant_sec_rx_ingest(RX_CH, pay, 8u, true, slot_t(5u)));
	radiant_sec_get_stats(RX_CH, &st);
	zassert_equal(1u, st.dropped_policy);
	radiant_sec_get_state(RX_CH, &state);
	zassert_true(state.keyed);
	zassert_false(state.secured);
	zassert_true(radiant_sec_channel_is_secured(RX_CH));

	/* Below the old epoch: accepted, the high-water mark went with the
	 * old root. Then the new root runs. */
	epoch_both(3u);
	log_n_pkt = 0u;
	log_n_verdict = 0u;
	for (i = 0u; i < 8u; i++) {
		hop(i, false, -1);
	}
	zassert_equal(8u, log_n_pkt);
	zassert_equal(1u, verdicts_for(0u, RADIANT_SEC_VERDICT_VERIFIED));
}

/* Item 9: the same root again is a no-op - an idempotent 0xF2 re-send is not a
 * gap. The stream carries on mid-window and every window verifies. */
ZTEST(radiant_sec, test_the_same_root_again_keeps_the_channel_running)
{
	struct radiant_sec_state state;
	uint32_t                 i;

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 4u);
	epoch_both(7u);
	for (i = 0u; i < 6u; i++) {
		hop(i, false, -1);
	}

	key_both();
	radiant_sec_get_state(TX_CH, &state);
	zassert_true(state.secured);
	zassert_equal(7u, state.epoch);

	for (i = 6u; i < 12u; i++) {
		hop(i, false, -1);
	}
	zassert_equal(12u, log_n_pkt);
	zassert_equal(1u, verdicts_for(0u, RADIANT_SEC_VERDICT_VERIFIED));
	zassert_equal(1u, verdicts_for(4u, RADIANT_SEC_VERDICT_VERIFIED));
	zassert_equal(0u, verdicts_for(4u, RADIANT_SEC_VERDICT_UNVERIFIED));
}

/* Item 9: re-sending the same root does not reset D3's gate, so the epoch
 * still only moves forwards. A different base device number is a different
 * key set (the KDF binds it), and that one does reset it. */
ZTEST(radiant_sec, test_the_same_root_then_an_older_epoch_is_refused)
{
	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 2u);
	zassert_ok(radiant_sec_set_epoch(TX_CH, 100u, 0u, TEST_PERIOD));

	zassert_ok(radiant_sec_set_key(TX_CH, test_root, 128, TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_set_epoch(TX_CH, 100u, 0u, TEST_PERIOD));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_set_epoch(TX_CH, 99u, 0u, TEST_PERIOD));
	zassert_true(radiant_sec_channel_is_secured(TX_CH));

	/* A bad key length on a live channel changes nothing either. */
	zassert_not_equal(RADIANT_SEC_OK,
			  radiant_sec_set_key(TX_CH, other_root, 64, TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_set_epoch(TX_CH, 100u, 0u, TEST_PERIOD));

	zassert_ok(radiant_sec_set_key(TX_CH, test_root, 128,
				       TEST_DEVNUM + 1u));
	zassert_ok(radiant_sec_set_epoch(TX_CH, 5u, 0u, TEST_PERIOD));
}

/* Pending covers the secured range only: the descriptor and the common pages
 * travel as on an armed channel, and a key with no switch on is not keyed. */
ZTEST(radiant_sec, test_an_out_of_range_page_is_still_plain_while_pending)
{
	uint8_t pay[8] = { 0x50u, 0xAA, 1, 2, 3, 4, 5, 6 };
	uint8_t before[8];

	memcpy(before, pay, sizeof(pay));
	key_both();
	zassert_ok(radiant_sec_tx_transform(TX_CH, pay, 8u, 0u));
	pay[0] = TEST_PAGE;
	before[0] = TEST_PAGE;
	zassert_ok(radiant_sec_tx_transform(TX_CH, pay, 8u, 0u),
		   "no switch on, but withheld");
	zassert_false(radiant_sec_channel_is_secured(TX_CH));

	configure_both(RADIANT_SEC_SW_CONF | RADIANT_SEC_SW_AUTH, 2u);
	pay[0] = 0x50u;
	before[0] = 0x50u;
	zassert_ok(radiant_sec_tx_transform(TX_CH, pay, 8u, 0u));
	pay[0] = 0x00u;
	before[0] = 0x00u;
	zassert_ok(radiant_sec_tx_transform(TX_CH, pay, 8u, 0u));
	zassert_mem_equal(pay, before, 8u);
	zassert_equal(RADIANT_SEC_RX_PLAIN,
		      radiant_sec_rx_ingest(RX_CH, pay, 8u, true, 0u));
	pay[0] = 0x51u;
	zassert_equal(RADIANT_SEC_RX_PLAIN,
		      radiant_sec_rx_ingest(RX_CH, pay, 8u, true, 0u));
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

	zassert_equal(12u, log_n_pkt);
	zassert_equal(1u, verdicts_for(0u, RADIANT_SEC_VERDICT_VERIFIED));
	zassert_equal(1u, verdicts_for(4u, RADIANT_SEC_VERDICT_VERIFIED));
	zassert_equal(2u, log_n_verdict);
}

/*
 * How far on the window carrying a tag may lie. The sender's tag rides past
 * windows that carried only clear pages, so the judging window need not be
 * the next one; but 256 counters on is where byte [1] repeats, so from there
 * a forged window aliasing a pending one would bind a host to its verdict
 * (review of fix/sec-verdict-event, finding 1). A genuine stream with that
 * gap is refused too - fail-safe. One case: window 0, then the sender's next
 * secured window `span` counters later.
 */
static void span_case(uint8_t w, uint32_t span, bool verifies)
{
	uint32_t i;

	sec_before(NULL);
	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, w);
	epoch_both(7u);
	for (i = 0u; i < w; i++) {
		hop(i, false, -1);
	}
	for (i = span; i < span + w; i++) {
		hop(i, false, -1);
	}
	zassert_equal(1u, log_n_verdict, "W=%u span %u", w, span);
	zassert_equal(1u,
		      verdicts_for(0u, verifies ? RADIANT_SEC_VERDICT_VERIFIED
					   : RADIANT_SEC_VERDICT_UNVERIFIED),
		      "W=%u span %u", w, span);
}

ZTEST(radiant_sec, test_a_tag_may_ride_past_clear_windows_up_to_256)
{
	static const uint8_t ws[] = { 2u, 4u, 8u };

	for (uint32_t k = 0u; k < ARRAY_SIZE(ws); k++) {
		uint8_t w = ws[k];

		span_case(w, w, true);               /* the next window */
		span_case(w, 256u - w, true);        /* the last before byte [1]
						      * repeats */
		span_case(w, 256u, false);           /* the alias itself */
		span_case(w, 256u + w, false);
	}
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
		/* Slot index, not a host counter: a repeated counter is keystream reuse. */
		zassert_equal(i, pay[1]);
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
	/* The descriptor stays clear, or a protected node looks dead. */
	zassert_mem_equal(pay, before, sizeof(pay));

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
			zassert_equal(RADIANT_SEC_OK, rc, "W=%u", w);
		} else {
			/* W must divide 256 and 65536 to resync after a loss. */
			zassert_equal(RADIANT_SEC_EINVAL, rc, "W=%u", w);
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

	zassert_equal(1u, verdicts_for(0u, RADIANT_SEC_VERDICT_UNVERIFIED));
	zassert_equal(0u, verdicts_for(0u, RADIANT_SEC_VERDICT_VERIFIED));

	/* And the damage is bounded to that window: the next one is clean. */
	zassert_equal(1u, verdicts_for(4u, RADIANT_SEC_VERDICT_VERIFIED));

	/* D6: delivered and marked, not discarded - discarding would let one
	 * injected frame cost W legitimate packets (a W-for-1 DoS). */
	zassert_equal(12u, log_n_pkt);
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

	zassert_equal(1u, verdicts_for(0u, RADIANT_SEC_VERDICT_UNVERIFIED));
	zassert_equal(1u, verdicts_for(4u, RADIANT_SEC_VERDICT_VERIFIED));
	zassert_equal(11u, log_n_pkt);
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
		      radiant_sec_rx_ingest(RX_CH, pay, sizeof(pay), false, 0u));

	radiant_sec_pump();
	zassert_equal(0u, log_n_pkt);

	radiant_sec_get_stats(RX_CH, &st);
	zassert_equal(1u, st.dropped_non_broadcast);
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

	zassert_equal(0u, verdicts_for(0u, RADIANT_SEC_VERDICT_VERIFIED));
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
	zassert_equal(1u, ch1);
	zassert_equal(1u, ch2);
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
	zassert_equal(4u, log_n_pkt, "%u packets", log_n_pkt);
	for (i = 0; i < log_n_pkt && i < LOG_MAX; i++) {
		zassert_equal(RADIANT_SEC_VERDICT_VERIFIED, log_pkt[i].verdict);
		zassert_equal(4u + i, log_pkt[i].pay[2], "delivery %u was packet %u", i,
			      log_pkt[i].pay[2]);
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
		zassert_not_equal(5u, log_pkt[i].pay[2], "marked %d", (int)log_pkt[i].verdict);
		zassert_equal(RADIANT_SEC_VERDICT_VERIFIED, log_pkt[i].verdict);
	}

	zassert_equal(ARRAY_SIZE(want), log_n_pkt, "%u packets", log_n_pkt);
	for (i = 0; i < ARRAY_SIZE(want); i++) {
		zassert_equal(want[i], log_pkt[i].pay[2], "delivery %u was packet %u", i,
			      log_pkt[i].pay[2]);
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
	zassert_ok(rc, "i=%u", i);
	memcpy(pay, conf_plain, 8u);
	zassert_not_equal(RADIANT_SEC_OK, radiant_sec_tx_transform(TX_CH, pay, 8u, slot_t(i)));
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
	zassert_equal(0u, log_n_pkt);
}

/* D1-4: the constant-time compare is still an equality. Constant time itself
 * is not unit-testable; the sec suites staying green is the real guard. */
ZTEST(radiant_sec, test_ct_equal_is_equality)
{
	uint8_t a[16];
	uint8_t b[16];

	memset(a, 0x5A, sizeof(a));
	memcpy(b, a, sizeof(b));
	zassert_true(radiant_sec_ct_equal(a, b, sizeof(a)));
	zassert_true(radiant_sec_ct_equal(a, b, 0u));
	b[0] ^= 0x01u;
	zassert_false(radiant_sec_ct_equal(a, b, sizeof(a)));
	b[0] ^= 0x01u;
	b[15] ^= 0x80u;
	zassert_false(radiant_sec_ct_equal(a, b, sizeof(a)));
}

/* D1-6(b): a failed key import must not leave an allocated, keyless context
 * behind - configure on it then answered OK where ENOKEY is owed. */
ZTEST(radiant_sec, test_a_failed_import_leaves_no_keyless_context)
{
	zassert_not_equal(RADIANT_SEC_OK, radiant_sec_set_key(RX_CH, test_root, 64, TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_ENOKEY,
		      radiant_sec_configure(RX_CH, RADIANT_SEC_SW_AUTH, 2u,
					    RADIANT_SEC_PAGE_LO_DEFAULT,
					    RADIANT_SEC_PAGE_HI_DEFAULT));
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
	zassert_equal(TEST_PAGE, air[0]);
	zassert_equal(0u, air[1]);
	zassert_true(memcmp(&air[2], &conf_plain[2], 5u) != 0);

	zassert_equal(RADIANT_SEC_RX_DEFER,
		      radiant_sec_rx_ingest(RX_CH, air, 8u, true, sec_anchor));
	radiant_sec_pump();

	zassert_equal(1u, log_n_pkt, "%u packets", log_n_pkt);
	zassert_mem_equal(&log_pkt[0].pay[2], &conf_plain[2], 5u);
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
	zassert_mem_equal(air, want_conf_only, 8u);

	/* And the receiver gives byte [7] back, which the five-byte receiver
	 * would have left as ciphertext. */
	zassert_equal(RADIANT_SEC_RX_DEFER,
		      radiant_sec_rx_ingest(RX_CH, air, 8u, true, sec_anchor));
	radiant_sec_pump();
	zassert_equal(1u, log_n_pkt);
	zassert_equal(conf_plain[7], log_pkt[0].pay[7]);
	zassert_mem_equal(&log_pkt[0].pay[2], &conf_plain[2], 6u);

	radiant_sec_channel_release(TX_CH);
	radiant_sec_channel_release(RX_CH);
	key_both();
	configure_both(RADIANT_SEC_SW_CONF | RADIANT_SEC_SW_AUTH, 2u);
	epoch_both(7u);
	tx_air(0u, air);
	zassert_mem_equal(air, want_with_auth, 7u);
	zassert_equal(0x00u, air[7]);
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
	zassert_true(memcmp(&a[2], &b[2], 5u) != 0);
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
	zassert_mem_equal(&log_pkt[0].pay[2], &conf_plain[2], 5u);
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
	zassert_true(memcmp(&log_pkt[0].pay[2], &conf_plain[2], 5u) != 0);
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

	zassert_equal(0u, again[1]);
	zassert_true(memcmp(&first[2], &again[2], 5u) != 0);
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
	zassert_true(verdicts_for(0u, RADIANT_SEC_VERDICT_VERIFIED) > 0u);

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
	zassert_equal(RADIANT_SEC_VERDICT_CLEAR, radiant_sec_last_verdict(RX_CH));

	log_n_verdict = 0u;
	tier2_window(TIER2_NEW_DEVNUM, 0u, w);
	tier2_window(TIER2_NEW_DEVNUM, w, w);

	zassert_equal(RADIANT_SEC_VERDICT_VERIFIED, radiant_sec_last_verdict(RX_CH));
	zassert_true(verdicts_for(0u, RADIANT_SEC_VERDICT_VERIFIED) > 0u);
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

	zassert_equal(RADIANT_SEC_VERDICT_UNVERIFIED, radiant_sec_last_verdict(RX_CH));
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
		zassert_equal(0x22u, log_pkt[i].pay[4], "delivery %u (packet %u)", i,
			      log_pkt[i].pay[2]);
		zassert_equal(RADIANT_SEC_VERDICT_VERIFIED, log_pkt[i].verdict);
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

	zassert_equal(1u, verdicts_for(0u, RADIANT_SEC_VERDICT_UNVERIFIED));
	zassert_equal(0u,
		      verdicts_for(4u, RADIANT_SEC_VERDICT_VERIFIED) +
				      verdicts_for(4u, RADIANT_SEC_VERDICT_UNVERIFIED));
	zassert_equal(1u, verdicts_for(8u, RADIANT_SEC_VERDICT_VERIFIED));
	zassert_equal(1u, verdicts_for(12u, RADIANT_SEC_VERDICT_VERIFIED));

	/* Windows 8 and 12 come out, in order; window 16 is still held. */
	zassert_equal(8u, log_n_pkt, "%u packets", log_n_pkt);
	for (i = 0; i < log_n_pkt && i < LOG_MAX; i++) {
		zassert_equal(RADIANT_SEC_VERDICT_VERIFIED, log_pkt[i].verdict);
		zassert_equal(8u + i, log_pkt[i].pay[2], "delivery %u was packet %u", i,
			      log_pkt[i].pay[2]);
	}
	zassert_equal(4u, dropped_policy_now());
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
	zassert_equal(0u, dropped_policy_now());

	epoch_both(46u);
	zassert_equal(2u, dropped_policy_now());

	for (i = 0; i < 8u; i++) {
		hop(i, false, -1);
	}

	assert_no_marked_delivery();
	zassert_equal(4u, log_n_pkt, "%u packets", log_n_pkt);
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

	zassert_equal(0u, dropped_policy_now());
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_try_devnum(RX_CH, TIER2_NEW_DEVNUM));
	zassert_equal(2u, dropped_policy_now());

	for (i = 0; i < 2u * w; i++) {
		hop(i, false, -1);
	}

	assert_no_marked_delivery();
	zassert_equal(4u, log_n_pkt, "%u packets", log_n_pkt);
	zassert_equal(RADIANT_SEC_VERDICT_VERIFIED, radiant_sec_last_verdict(RX_CH));
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
	zassert_equal(0u, log_n_pkt);
	zassert_equal(0u, dropped_policy_now());

	/* The sender's own wrap - epoch + 1, counter back to 0 - happens inside
	 * this hop: slot 65536 is past the end of epoch 47. */
	hop(65536u, false, -1);
	zassert_equal(2u, dropped_policy_now());
	{
		struct radiant_sec_stats st;

		radiant_sec_get_stats(RX_CH, &st);
		zassert_equal(1u, st.epoch_advances);
		zassert_equal(0u, st.dropped_replay);
	}

	for (i = 65537u; i < 65536u + 12u; i++) {
		hop(i, false, -1);
	}
	assert_no_marked_delivery();
	zassert_equal(1u, verdicts_for(0u, RADIANT_SEC_VERDICT_VERIFIED));
	/* Windows 0 and 4 released; window 8 still waits for its verdict. */
	zassert_equal(8u, log_n_pkt, "%u packets", log_n_pkt);
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
	zassert_equal(1u, verdicts_for(0u, RADIANT_SEC_VERDICT_VERIFIED));
	before = rx_expected_at(t);
	zassert_equal(now_i, before, "clock %u", before);

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
	zassert_equal(before, e, "%u forgeries: %u -> %u", i, before, e);

	/* Then replay the recorded windows, as if they were current. */
	for (i = 0u; i < 32u; i++) {
		ingest_at(rec[i], t + (uint64_t)i * period_us());
	}
	zassert_equal(0u, verified_count());
	if ((switches & RADIANT_SEC_SW_DROP_UNVER) != 0u) {
		zassert_equal(0u, log_n_pkt, "%u packets", log_n_pkt);
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
	zassert_equal(0u, st.epoch_advances);

	/* The real sender, carrying on. The close of window 300 judges the last
	 * forged window; the closes of 304 and 308 judge 300 and 304. */
	verified = st.windows_verified;
	for (i = 300u; i < 312u; i++) {
		hop(i, false, -1);
	}
	st = rx_stats();
	zassert_equal(verified + 2u, st.windows_verified);
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
	zassert_equal(1u, verdicts_for(8u, RADIANT_SEC_VERDICT_VERIFIED));
	replay_before = rx_stats().dropped_replay;

	/* Window 8 and the window carrying its tag, again, 12 periods late. */
	for (i = 8u; i < 16u; i++) {
		ingest_at(rec[i], sec_anchor + (uint64_t)(i + 12u) * period_us());
	}

	zassert_equal(1u, verdicts_for(8u, RADIANT_SEC_VERDICT_VERIFIED));
	zassert_equal(replay_before + 8u, rx_stats().dropped_replay);
	if ((switches & RADIANT_SEC_SW_DROP_UNVER) != 0u) {
		for (i = 0u; i < log_n_pkt && i < LOG_MAX; i++) {
			if (log_pkt[i].pay[2] == 8u) {
				n8++;
			}
		}
		zassert_equal(1u, n8, "%u times", n8);
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
	zassert_equal(0u, st.windows_unverified, "%d ppm: %u", ppm, st.windows_unverified);
	zassert_equal(30000u / 4u - 1u, st.windows_verified, "%d ppm: %u", ppm,
		      st.windows_verified);
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
	zassert_equal(verified + 10u, rx_stats().windows_verified);

	log_n_verdict = 0u;
	for (i = 80u; i < 120u; i++) {
		ingest_at(rec[i - 40u],
			  sec_anchor + (uint64_t)(i + 200u) * period_us());
	}
	/* Window 76 is the one exception, and not a hole: it was replayed in
	 * the first batch, and its tag bytes ride in packets 80..83. */
	for (i = 0u; i < log_n_verdict && i < LOG_MAX; i++) {
		zassert_false(log_verdict[i].verdict == RADIANT_SEC_VERDICT_VERIFIED &&
			      log_verdict[i].window_start != 76u, "window %u",
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
			      radiant_sec_tx_transform(TX_CH, air, 8u, slot_t(slot)),
			      "%s: slot %u", label, slot);
		if (sk == STREAM_CLEAR) {
			zassert_equal(0xAAu, air[1], "%s: slot %u", label, slot);
			zassert_equal(RADIANT_SEC_RX_PLAIN,
				      radiant_sec_rx_ingest(RX_CH, air, 8u, true, slot_t(slot)),
				      "%s: slot %u", label, slot);
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

	zassert_true(ok);
}

ZTEST(radiant_sec, test_skipped_slots_do_not_desync_the_counter)
{
	bool ok = true;

	ok &= stream_case(RADIANT_SEC_SW_AUTH, 2u, skip_slot, "skip1in40");
	ok &= stream_case(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_CONF, 8u,
			  skip_slot, "skip1in40");

	zassert_true(ok);
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
		zassert_equal((uint8_t)slots[i], air[1], "slot %u counter %u", slots[i], air[1]);
		ingest_at(air, slot_t(slots[i]));
	}

	radiant_sec_get_stats(RX_CH, &st);
	zassert_equal(0u, st.dropped_replay, "%u refused as replays",
		      st.dropped_replay);
	zassert_equal(ARRAY_SIZE(slots), log_n_pkt, "%u delivered", log_n_pkt);
	for (i = 0u; i < ARRAY_SIZE(slots); i++) {
		zassert_mem_equal(&log_pkt[i].pay[2], &conf_plain[2], 5u, "slot %u", slots[i]);
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
		zassert_mem_equal(first, again, 8u);
		ingest_at(first, slot_t(5u));
	}

	zassert_equal(1u, verdicts_for(0u, RADIANT_SEC_VERDICT_VERIFIED));
	zassert_equal(1u, verdicts_for(4u, RADIANT_SEC_VERDICT_VERIFIED));

	memcpy(air, conf_plain, sizeof(air));
	zassert_not_equal(RADIANT_SEC_OK, radiant_sec_tx_transform(TX_CH, air, 8u, slot_t(10u)));
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
	zassert_mem_equal(pay, before, sizeof(pay));

	/* And configuring one with no key is a host sequencing error rather
	 * than a state worth inventing a context for. */
	zassert_equal(RADIANT_SEC_ENOKEY,
		      radiant_sec_configure(TX_CH, RADIANT_SEC_SW_AUTH, 2u,
					    0x01u, 0x0Fu));

	radiant_sec_reset();
	zassert_equal(RADIANT_SEC_ENOKEY,
		      radiant_sec_set_epoch(RX_CH, 99u, 0u, TEST_PERIOD));
}

/* ── The epoch ceiling and the direction guard (review 2026-11-13) ──────── */

static uint32_t tx_epoch_now(uint8_t ch)
{
	struct radiant_sec_state st;

	radiant_sec_get_state(ch, &st);
	return st.epoch;
}

/*
 * Item 1: an epoch authority's reservation. A wrap past the ceiling is not
 * taken - the secured page is WITHHELD (EAGAIN) while clear pages still go out
 * - and nothing is spent: raise the ceiling and the very next slot advances
 * and transmits, decodably. Unlike headroom exhaustion, which is permanent.
 */
ZTEST(radiant_sec, test_a_wrap_past_the_epoch_ceiling_is_withheld_until_it_is_raised)
{
	struct radiant_sec_stats st;
	uint8_t                  pay[8];
	uint8_t                  clear[8] = { 0x50u, 1, 2, 3, 4, 5, 6, 7 };
	uint8_t                  clear_before[8];

	key_both();
	configure_both(RADIANT_SEC_SW_CONF, 2u);
	epoch_both(7u);
	zassert_ok(radiant_sec_set_epoch_ceiling(TX_CH, 7u));
	zassert_equal(RADIANT_SEC_ENOKEY, radiant_sec_set_epoch_ceiling(5u, 7u));

	tx_air(0u, pay);

	/* A sparse sender's next slot is past the wrap: withheld, twice. */
	memcpy(pay, conf_plain, 8u);
	zassert_equal(RADIANT_SEC_EAGAIN,
		      radiant_sec_tx_transform(TX_CH, pay, 8u,
					       slot_t(65536u)));
	memcpy(pay, conf_plain, 8u);
	zassert_equal(RADIANT_SEC_EAGAIN,
		      radiant_sec_tx_transform(TX_CH, pay, 8u,
					       slot_t(65537u)));
	zassert_equal(7u, tx_epoch_now(TX_CH));
	radiant_sec_get_stats(TX_CH, &st);
	zassert_equal(0u, st.epoch_advances);

	/* A clear page is not held back by a secured page's wrap. */
	memcpy(clear_before, clear, sizeof(clear));
	zassert_ok(radiant_sec_tx_transform(TX_CH, clear, 8u, slot_t(65537u)));
	zassert_mem_equal(clear, clear_before, sizeof(clear));

	/* Raised: the next slot advances and transmits, and a receiver with no
	 * ceiling follows it to the same epoch. */
	zassert_ok(radiant_sec_set_epoch_ceiling(TX_CH, 8u));
	memcpy(pay, conf_plain, 8u);
	zassert_ok(radiant_sec_tx_transform(TX_CH, pay, 8u, slot_t(65538u)));
	zassert_equal(8u, tx_epoch_now(TX_CH));
	zassert_equal(2u, pay[1]);
	ingest_at(pay, slot_t(65538u));
	zassert_equal(1u, log_n_pkt);
	zassert_mem_equal(&log_pkt[0].pay[2], &conf_plain[2], 5u);

	/* Not spent: the next wrap is withheld again, not refused for good. */
	memcpy(pay, conf_plain, 8u);
	zassert_equal(RADIANT_SEC_EAGAIN,
		      radiant_sec_tx_transform(TX_CH, pay, 8u,
					       slot_t(2u * 65536u)));
	zassert_ok(radiant_sec_set_epoch_ceiling(TX_CH, 9u));
	memcpy(pay, conf_plain, 8u);
	zassert_ok(radiant_sec_tx_transform(TX_CH, pay, 8u,
					    slot_t(2u * 65536u + 1u)));
	zassert_equal(9u, tx_epoch_now(TX_CH));
}

/* A ceiling below a gap of several wraps holds them all, then takes them in
 * one step once it allows the lot. */
ZTEST(radiant_sec, test_a_multi_wrap_gap_waits_for_a_ceiling_that_covers_it)
{
	uint8_t pay[8];

	key_both();
	configure_both(RADIANT_SEC_SW_CONF, 2u);
	epoch_both(7u);
	zassert_ok(radiant_sec_set_epoch_ceiling(TX_CH, 8u));
	tx_air(0u, pay);

	memcpy(pay, conf_plain, 8u);
	zassert_equal(RADIANT_SEC_EAGAIN,
		      radiant_sec_tx_transform(TX_CH, pay, 8u,
					       slot_t(3u * 65536u)));
	zassert_equal(7u, tx_epoch_now(TX_CH));
	zassert_ok(radiant_sec_set_epoch_ceiling(TX_CH, 10u));
	memcpy(pay, conf_plain, 8u);
	zassert_ok(radiant_sec_tx_transform(TX_CH, pay, 8u,
					    slot_t(3u * 65536u + 1u)));
	zassert_equal(10u, tx_epoch_now(TX_CH));
}

/*
 * Item 2, defence in depth: whatever reaches the pump for a context whose TX
 * has latched is refused, so no RX path can move the epoch the
 * channel transmits under. The forged page resolves past the wrap - taken, it
 * would advance the shared epoch one wrap early.
 */
ZTEST(radiant_sec, test_the_pump_refuses_a_context_that_transmits)
{
	struct radiant_sec_stats st;
	uint8_t                  pay[8];
	uint8_t                  forged[8] = { TEST_PAGE, 0x00, 1, 2, 3, 4, 5, 6 };

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 2u);
	epoch_both(7u);
	tx_air(0u, pay);

	/* Channel 0 is not a radiant_channel master in this image, so ingest
	 * takes it; the pump must not. */
	zassert_equal(RADIANT_SEC_RX_DEFER,
		      radiant_sec_rx_ingest(TX_CH, forged, 8u, true,
					    slot_t(65536u - 1u)));
	radiant_sec_pump();

	zassert_equal(7u, tx_epoch_now(TX_CH));
	zassert_equal(0u, log_n_pkt);
	radiant_sec_get_stats(TX_CH, &st);
	/* Uncounted here: the ingest-side master refusal is the counted one. */
	zassert_equal(0u, st.epoch_advances);

	/* And the receiver beside it is untouched by the guard. */
	hop(1u, false, -1);
	zassert_equal(1u, log_n_pkt);
}

/* Re-review M1/L1: a channel an authority follows refuses anyone else's
 * set_epoch (a host 0xF3 would run wraps nobody reserved); a new root, or the
 * authority clearing its own ceiling, frees it. A raise never lowers a
 * ceiling nor creates one. */
ZTEST(radiant_sec, test_a_followed_channel_refuses_set_epoch_and_a_raise_only_raises)
{
	uint8_t pay[8];

	key_both();
	configure_both(RADIANT_SEC_SW_CONF, 2u);
	epoch_both(7u);

	/* No ceiling: a raise does not invent one. */
	zassert_ok(radiant_sec_raise_epoch_ceiling(TX_CH, 7u));
	tx_air(0u, pay);
	memcpy(pay, conf_plain, 8u);
	zassert_ok(radiant_sec_tx_transform(TX_CH, pay, 8u, slot_t(65536u)));

	/* A ceiling, then a lower "raise": still the higher one. */
	zassert_ok(radiant_sec_set_epoch_ceiling(TX_CH, 9u));
	zassert_ok(radiant_sec_raise_epoch_ceiling(TX_CH, 8u));
	memcpy(pay, conf_plain, 8u);
	zassert_ok(radiant_sec_tx_transform(TX_CH, pay, 8u,
					    slot_t(2u * 65536u)));
	zassert_equal(9u, tx_epoch_now(TX_CH));
	memcpy(pay, conf_plain, 8u);
	zassert_equal(RADIANT_SEC_EAGAIN,
		      radiant_sec_tx_transform(TX_CH, pay, 8u,
					       slot_t(3u * 65536u)));

	/* A host epoch on the followed channel is refused, state untouched. */
	zassert_equal(RADIANT_SEC_ESTATE,
		      radiant_sec_set_epoch(TX_CH, 100u, 0u, TEST_PERIOD));
	zassert_equal(9u, tx_epoch_now(TX_CH));

	/* The authority clearing its own ceiling frees it... */
	zassert_ok(radiant_sec_set_epoch_ceiling(TX_CH, UINT32_MAX));
	zassert_ok(radiant_sec_set_epoch(TX_CH, 100u, 0u, TEST_PERIOD));

	/* The same root again is a no-op (item 9): still followed, still
	 * refused - re-sending the root is no way round the authority. */
	zassert_ok(radiant_sec_set_epoch_ceiling(TX_CH, 100u));
	zassert_equal(RADIANT_SEC_ESTATE,
		      radiant_sec_set_epoch(TX_CH, 200u, 0u, TEST_PERIOD));
	zassert_ok(radiant_sec_set_key(TX_CH, test_root, 128, TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_ESTATE,
		      radiant_sec_set_epoch(TX_CH, 200u, 0u, TEST_PERIOD));

	/* ...but a new root frees it, and a raise does not bring it back. */
	zassert_ok(radiant_sec_set_key(TX_CH, other_root, 128,
				       TEST_DEVNUM));
	zassert_ok(radiant_sec_raise_epoch_ceiling(TX_CH, 300u));
	zassert_ok(radiant_sec_set_epoch(TX_CH, 200u, 0u, TEST_PERIOD));
	sec_anchor = (uint64_t)radiant_radio_now();
	tx_air(0u, pay);
	memcpy(pay, conf_plain, 8u);
	zassert_ok(radiant_sec_tx_transform(TX_CH, pay, 8u, slot_t(3u * 65536u)));
	zassert_equal(203u, tx_epoch_now(TX_CH));
}

/* ── An epoch that began before boot (review 2026-11-17 item 3) ─────────── */

/*
 * The host's 0xF3 phase is the epoch's age, and an epoch normally outlives the
 * receiver's uptime: a receiver joins a sender that has run for an hour. The
 * anchor (radiant_radio_now() - phase) is then before this clock's zero. It
 * used to be clamped to zero there, which made a receiver 10 s after boot
 * count from its boot rather than from the epoch: genuine windows ~1000 s on
 * never verified, recordings ~1000 s old did, and a verified window could not
 * pull the anchor back below zero, so it never healed. The same clamp sat in
 * the master's slot latch.
 *
 * The sender here is "normally anchored": its recordings are made on a clock
 * that has run longer than the epoch (late_record()), and the clock is then
 * moved back to a receiver 10 s after its boot. A sender anchored on the
 * receiver's clock would carry the same clamp on master and agree with it.
 */

#define LATE_UPTIME_US 10000000ull     /* the receiver booted 10 s ago */
#define LATE_SENDER_US 20000000000ull  /* a sender up ~5.6 h */
#define LATE_AGE       4000u           /* ~1000 s into the epoch, in slots */

/* Set `ch`'s epoch `k` slots in, as a host 0xF3 does. sec_anchor becomes slot
 * 0's instant - modular, so "before zero" wraps, and slot_t() of a slot after
 * boot is still exact. */
static void epoch_late(uint8_t ch, uint32_t epoch, uint32_t k)
{
	zassert_ok(radiant_sec_set_epoch(ch, epoch, (uint64_t)k * period_us(),
					 TEST_PERIOD));
	sec_anchor = (uint64_t)radiant_radio_now() - (uint64_t)k * period_us();
}

/* A sender up for hours, whose epoch began `from` slots before now, records
 * slots from..from+n-1 into rec[] as they go on the air. Then the clock moves
 * to a receiver LATE_UPTIME_US after its boot. */
static void late_record(uint32_t epoch, uint32_t from, uint32_t n)
{
	uint32_t i;

	zassert_true(n <= REC_MAX);
	fake_radio_set_origin((radiant_time_t)LATE_SENDER_US);
	epoch_late(TX_CH, epoch, from);
	for (i = 0u; i < n; i++) {
		tx_hop(from + i, rec[i]);
	}
	fake_radio_set_origin((radiant_time_t)LATE_UPTIME_US);
}

/* rec[i] heard at slot k + i of the receiver's own epoch_late(). */
static void late_hop(uint32_t k, uint32_t i)
{
	ingest_at(rec[i], slot_t(k + i));
}

/* Ten whole windows of W=4 heard: each closes on its own last packet, and
 * every close but the first judges the window before it. */
#define LATE_JUDGED 9u

ZTEST(radiant_sec, test_a_late_joiner_verifies_current_windows)
{
	struct radiant_sec_stats st;
	uint32_t                 i;

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 4u);
	late_record(73u, LATE_AGE, 40u);
	epoch_late(RX_CH, 73u, LATE_AGE);

	zassert_equal(LATE_AGE, rx_expected_at(slot_t(LATE_AGE)));
	for (i = 0u; i < 40u; i++) {
		late_hop(LATE_AGE, i);
	}
	st = rx_stats();
	zassert_equal(LATE_JUDGED, st.windows_verified, "%u",
		      st.windows_verified);
	zassert_equal(0u, st.windows_unverified, "%u", st.windows_unverified);
	zassert_equal(LATE_AGE + 40u, rx_expected_at(slot_t(LATE_AGE + 40u)));
}

/*
 * The exploit on master: the epoch's first 40 slots, recorded ~1000 s ago and
 * replayed to a receiver 10 s after its boot. Far outside the 128-slot slack,
 * so nearest-rollover resolution sends each packet to a counter 256 on and its
 * MAC fails - a MAC failure, not a replay drop.
 */
static void late_old_replay(uint8_t switches)
{
	uint32_t i;

	key_both();
	configure_both(switches, 4u);
	late_record(79u, 0u, 40u);
	epoch_late(RX_CH, 79u, LATE_AGE);

	for (i = 0u; i < 40u; i++) {
		late_hop(LATE_AGE, i);
	}
	zassert_equal(0u, verified_count());
	zassert_equal(0u, rx_stats().windows_verified);
	if ((switches & RADIANT_SEC_SW_DROP_UNVER) != 0u) {
		zassert_equal(0u, log_n_pkt, "%u packets", log_n_pkt);
	}
}

ZTEST(radiant_sec, test_a_late_joiner_refuses_a_recording_older_than_the_slack)
{
	late_old_replay(RADIANT_SEC_SW_AUTH);
}

ZTEST(radiant_sec, test_a_late_joiner_refuses_an_old_recording_under_the_drop_policy)
{
	late_old_replay(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_DROP_UNVER);
}

/* Inside the slack a recording is genuine bytes and verifies - the documented
 * cost of tracking drift at all - but only once: the floor drops it after. */
ZTEST(radiant_sec, test_a_late_joiner_accepts_a_recording_inside_the_slack_once)
{
	uint32_t replay_before;
	uint32_t i;

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 4u);
	late_record(83u, LATE_AGE - 60u, 40u);
	epoch_late(RX_CH, 83u, LATE_AGE);

	/* 60 slots late. */
	for (i = 0u; i < 40u; i++) {
		late_hop(LATE_AGE, i);
	}
	zassert_equal(LATE_JUDGED, rx_stats().windows_verified, "%u",
		      rx_stats().windows_verified);
	replay_before = rx_stats().dropped_replay;

	/* The nine verified windows again, 100 slots late: every one below the
	 * floor. */
	for (i = 0u; i < 4u * LATE_JUDGED; i++) {
		late_hop(LATE_AGE + 40u, i);
	}
	zassert_equal(LATE_JUDGED, rx_stats().windows_verified);
	zassert_equal(replay_before + 4u * LATE_JUDGED,
		      rx_stats().dropped_replay, "%u",
		      rx_stats().dropped_replay - replay_before);
	for (i = 0u; i < LATE_JUDGED; i++) {
		zassert_equal(1u, verdicts_for((uint16_t)(LATE_AGE - 60u + 4u * i),
					       RADIANT_SEC_VERDICT_VERIFIED));
	}
}

/* A phase past a whole counter wrap: the receiver's first packet advances its
 * epoch, as the sender's first slot did. */
ZTEST(radiant_sec, test_a_phase_past_a_wrap_advances_the_epoch_on_the_first_packet)
{
	const uint32_t k = 65536u + LATE_AGE;
	struct radiant_sec_state s;
	uint32_t i;

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 4u);
	late_record(89u, k, 40u);
	epoch_late(RX_CH, 89u, k);

	zassert_equal((uint16_t)k, rx_expected_at(slot_t(k)));
	zassert_equal(0u, rx_stats().epoch_advances);
	late_hop(k, 0u);
	zassert_equal(1u, rx_stats().epoch_advances);
	radiant_sec_get_state(RX_CH, &s);
	zassert_equal(90u, s.epoch);
	for (i = 1u; i < 40u; i++) {
		late_hop(k, i);
	}
	zassert_equal(LATE_JUDGED, rx_stats().windows_verified, "%u",
		      rx_stats().windows_verified);
	zassert_equal(0u, rx_stats().windows_unverified);
}

/* Joined 20 slots before a wrap, the receiver follows the sender through it. */
ZTEST(radiant_sec, test_a_late_joiner_follows_the_sender_through_a_wrap)
{
	const uint32_t k = 65536u - 20u;
	struct radiant_sec_state s;
	uint32_t i;

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 4u);
	late_record(97u, k, 40u);
	epoch_late(RX_CH, 97u, k);

	for (i = 0u; i < 40u; i++) {
		late_hop(k, i);
	}
	radiant_sec_get_state(RX_CH, &s);
	zassert_equal(98u, s.epoch);
	zassert_equal(1u, rx_stats().epoch_advances);
	/* Before the wrap 65516..65528 are judged (65532's verdict would ride
	 * in new-epoch window 0, which carries none); after it 0..12. */
	for (i = 0u; i < 4u; i++) {
		zassert_true(window_was_verified((uint16_t)(k + 4u * i)),
			     "window %u", k + 4u * i);
		zassert_true(window_was_verified((uint16_t)(4u * i)),
			     "window %u", 4u * i);
	}
	zassert_equal(8u, rx_stats().windows_verified, "%u",
		      rx_stats().windows_verified);
	zassert_equal(0u, rx_stats().windows_unverified);
}

/*
 * The master's latch had the same clamp: given an epoch older than its uptime,
 * it counted slots from boot. Here its epoch is 20 slots past a wrap, so its
 * first slot must advance the epoch and carry counter 20 - and a receiver
 * anchored normally (epoch + 1, 20 slots in, after boot) verifies it.
 */
ZTEST(radiant_sec, test_a_master_whose_epoch_predates_boot_sends_the_right_slot)
{
	const uint32_t k = 65536u + 20u;
	struct radiant_sec_stats st;
	uint8_t                  air[8];
	uint32_t                 i;

	fake_radio_set_origin((radiant_time_t)LATE_UPTIME_US);
	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 4u);
	zassert_ok(radiant_sec_set_epoch(RX_CH, 102u, 20u * period_us(),
					 TEST_PERIOD));
	epoch_late(TX_CH, 101u, k);

	for (i = 0u; i < 40u; i++) {
		tx_hop(k + i, air);
		zassert_equal((uint8_t)(20u + i), air[1], "slot %u", i);
		ingest_at(air, slot_t(k + i));
	}
	zassert_equal(102u, tx_epoch_now(TX_CH));
	st = rx_stats();
	zassert_equal(LATE_JUDGED, st.windows_verified, "%u",
		      st.windows_verified);
	zassert_equal(0u, st.windows_unverified);
}

/*
 * The host's phase is off by 64 slots, late: the receiver's anchor lands ~1 s
 * after boot while the sender's epoch began ~15 s before it. The first
 * verified window takes the error whole (3.1), which moves the anchor before
 * boot - it was clamped at zero, leaving the clock 4 slots out for good.
 */
ZTEST(radiant_sec, test_the_first_verified_window_can_move_the_anchor_before_boot)
{
	uint32_t i;

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 4u);
	late_record(103u, 100u, 40u);
	epoch_late(RX_CH, 103u, 36u);

	for (i = 0u; i < 40u; i++) {
		late_hop(36u, i);
	}
	zassert_equal(LATE_JUDGED, rx_stats().windows_verified, "%u",
		      rx_stats().windows_verified);
	zassert_equal(140u, rx_expected_at(slot_t(36u + 40u)));
}

/*
 * A phase the anchor cannot carry is refused before anything changes: more
 * than INT32_MAX periods, or more whole wraps than the epoch has room to
 * advance into. The bound itself is accepted.
 */
ZTEST(radiant_sec, test_set_epoch_refuses_a_phase_it_cannot_represent)
{
	/* radiant_sec's own period: truncated, not rounded. */
	const uint64_t p = ((uint64_t)TEST_PERIOD * 1000000u) / 32768u;
	const uint32_t top = 0xFFFFFFFFu - RADIANT_SEC_EPOCH_HEADROOM;
	struct radiant_sec_state s;

	key_both();
	configure_both(RADIANT_SEC_SW_AUTH, 4u);

	/* A refusal leaves a fresh channel pending. */
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_set_epoch(RX_CH, 5u, UINT64_MAX, TEST_PERIOD));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_set_epoch(RX_CH, 5u,
					    ((uint64_t)INT32_MAX + 1u) * p,
					    TEST_PERIOD));
	radiant_sec_get_state(RX_CH, &s);
	zassert_false(s.secured);

	/* ...and an armed one as it was, the epoch not consumed. */
	zassert_ok(radiant_sec_set_epoch(RX_CH, 5u, 0u, TEST_PERIOD));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_set_epoch(RX_CH, 6u, UINT64_MAX, TEST_PERIOD));
	radiant_sec_get_state(RX_CH, &s);
	zassert_equal(5u, s.epoch);
	zassert_equal(0u, s.expected_index);
	zassert_ok(radiant_sec_set_epoch(RX_CH, 6u, (uint64_t)INT32_MAX * p,
					 TEST_PERIOD));

	/* One wrap past the top epoch has nowhere to advance into. */
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_set_epoch(TX_CH, top, 65536u * p, TEST_PERIOD));
	zassert_ok(radiant_sec_set_epoch(TX_CH, top, 65535u * p, TEST_PERIOD));
}

/* ── A policy change takes effect before 0xF1 answers (review 2026-11-17
 * item 2) ─────────────────────────────────────────────────────────────── */

/* Configure the sender alone, with the default page range unless given. */
static int tx_configure(uint8_t switches, uint8_t lo, uint8_t hi)
{
	return radiant_sec_configure(TX_CH, switches, 4u, lo, hi);
}

/*
 * The slot the adapter staged before a 0xF1 is posted again once the 0xF1 has
 * flushed it. Its bytes were built under the old policy - here X_AUTH alone,
 * the host's five bytes in the clear - so resending them would put the old
 * policy on the air after the new one was acknowledged, and building them
 * again would be a second ciphertext and a second absorb under one nonce. The
 * slot is withheld; the next is built under the new policy. Fails on master,
 * which resends the X_AUTH bytes.
 */
ZTEST(radiant_sec, test_a_policy_change_withholds_the_slot_already_built)
{
	uint8_t air[8];

	key_both();
	zassert_ok(tx_configure(RADIANT_SEC_SW_AUTH, RADIANT_SEC_PAGE_LO_DEFAULT,
				RADIANT_SEC_PAGE_HI_DEFAULT));
	epoch_both(90u);
	tx_air(0u, air);
	tx_air(1u, air);
	zassert_mem_equal(&air[2], &conf_plain[2], 5u, "setup: X_AUTH hid the bytes");

	zassert_ok(tx_configure(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_CONF,
				RADIANT_SEC_PAGE_LO_DEFAULT,
				RADIANT_SEC_PAGE_HI_DEFAULT));
	memcpy(air, conf_plain, sizeof(air));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_tx_transform(TX_CH, air, 8u, slot_t(1u)),
		      "the slot built under the old policy was sent again");

	/* Withheld again if posted again, and the next slot is the new one's. */
	memcpy(air, conf_plain, sizeof(air));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_tx_transform(TX_CH, air, 8u, slot_t(1u)));
	tx_air(2u, air);
	zassert_equal(2u, air[1]);
	zassert_true(memcmp(&air[2], &conf_plain[2], 5u) != 0,
		     "the slot after the change is not encrypted");
}

/* The guard beside it: the same policy again changes nothing, so a slot
 * posted again still resends exactly what it built. */
ZTEST(radiant_sec, test_an_identical_configure_keeps_a_reposted_slot)
{
	uint8_t first[8];
	uint8_t again[8];

	key_both();
	zassert_ok(tx_configure(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_CONF,
				RADIANT_SEC_PAGE_LO_DEFAULT,
				RADIANT_SEC_PAGE_HI_DEFAULT));
	epoch_both(91u);
	tx_air(0u, first);
	tx_air(1u, first);

	zassert_ok(tx_configure(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_CONF,
				RADIANT_SEC_PAGE_LO_DEFAULT,
				RADIANT_SEC_PAGE_HI_DEFAULT));
	memset(again, 0x99u, sizeof(again));
	again[0] = TEST_PAGE;
	zassert_ok(radiant_sec_tx_transform(TX_CH, again, 8u, slot_t(1u)));
	zassert_mem_equal(first, again, 8u);
}

/* A slot whose page the old range left clear was never built, so a range that
 * now covers it builds it - once, and a later re-post resends those bytes. */
ZTEST(radiant_sec, test_a_widened_range_builds_an_unbuilt_slot_once)
{
	uint8_t air[8];
	uint8_t built[8];

	key_both();
	zassert_ok(tx_configure(RADIANT_SEC_SW_CONF, 0x01u, 0x04u));
	epoch_both(92u);
	memcpy(air, conf_plain, sizeof(air));
	zassert_ok(radiant_sec_tx_transform(TX_CH, air, 8u, slot_t(0u)));
	zassert_mem_equal(air, conf_plain, 8u, "setup: page 5 was secured");

	zassert_ok(tx_configure(RADIANT_SEC_SW_CONF, 0x01u, 0x0Fu));
	memcpy(built, conf_plain, sizeof(built));
	zassert_ok(radiant_sec_tx_transform(TX_CH, built, 8u, slot_t(0u)));
	zassert_equal(0u, built[1]);
	zassert_true(memcmp(&built[2], &conf_plain[2], 5u) != 0,
		     "the widened range left the page in the clear");

	memset(air, 0x77u, sizeof(air));
	air[0] = TEST_PAGE;
	zassert_ok(radiant_sec_tx_transform(TX_CH, air, 8u, slot_t(0u)));
	zassert_mem_equal(air, built, 8u, "the slot was built twice");
}

/*
 * radiant_sec_policy_changing(), the port's say before a change is written:
 * this image's stand-in records the asks and answers what the test sets.
 * Every other suite that configures a channel sees OK, the weak default.
 */
static int      changing_rc = RADIANT_SEC_OK;
static uint32_t changing_calls;
static uint8_t  changing_ch;

int radiant_sec_policy_changing(uint8_t ch)
{
	changing_calls++;
	changing_ch = ch;
	return changing_rc;
}

/* Asked only for a valid tuple that differs from the installed one, and a
 * refusal is returned with nothing written: not the tuple, and not the mark
 * that withholds the slot already built. */
ZTEST(radiant_sec, test_only_a_change_asks_the_port_and_a_refusal_changes_nothing)
{
	const uint8_t            sw = RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_CONF;
	struct radiant_sec_state s;
	uint8_t                  first[8];
	uint8_t                  again[8];

	key_both();
	changing_calls = 0u;
	zassert_ok(tx_configure(sw, 0x01u, 0x0Fu));
	zassert_equal(1u, changing_calls, "a fresh context's first tuple");
	zassert_equal(TX_CH, changing_ch);
	zassert_ok(tx_configure(sw, 0x01u, 0x0Fu));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_configure(TX_CH, sw, 3u, 0x01u, 0x0Fu));
	zassert_equal(1u, changing_calls, "asked for an identical or invalid tuple");

	epoch_both(93u);
	tx_air(0u, first);
	changing_rc = RADIANT_SEC_ESTATE;
	zassert_equal(RADIANT_SEC_ESTATE,
		      tx_configure(RADIANT_SEC_SW_AUTH, 0x01u, 0x0Fu));
	changing_rc = RADIANT_SEC_OK;
	zassert_equal(2u, changing_calls);

	radiant_sec_get_state(TX_CH, &s);
	zassert_equal(sw, s.switches, "the refused tuple was installed");
	memset(again, 0x99u, sizeof(again));
	again[0] = TEST_PAGE;
	zassert_ok(radiant_sec_tx_transform(TX_CH, again, 8u, slot_t(0u)),
		   "a refused change withheld the slot");
	zassert_mem_equal(first, again, 8u);
}
