/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Compat attestation, driven end to end inside one process: the emit hooks, the
 * receive path, the verdicts and the epoch recovery of
 * docs/radiant-security.md sections 11.3 and 11.4.
 *
 * Channel 0 is the sensor and channel 1 is the receiver, the same 1:N shape
 * test_sec.c uses. The clock is not fake_radio's here and not anything else's:
 * radiant_sec_compat takes every instant as an argument, so a test chooses
 * them - which is what lets a day of attestation at T = 20 s run in a
 * millisecond and lets the 65 536-boot row of the cost table be an assertion
 * rather than a paragraph.
 *
 * ── THE PAGE NUMBERS ARE IN THIS FILE AND NOT IN THE MODULE ────────────────
 *
 * radiant_sec_compat.c contains no page number, so byte [0] is composed HERE,
 * from the subtype the emit hook returned, exactly as src/profiles/ will
 * compose it. That is not a test shortcut - it is the interface, and writing
 * the test this way is what proves the interface is usable by a profile that
 * knows nothing about attestation beyond "put this nibble in the page byte".
 * The values mirror tools/ant_pages.py's COMPAT_PAGE_BASE.
 *
 * WHAT THESE TESTS ARE FOR, in one line each:
 *
 *   Tier I, the default configuration and therefore first
 *     the cadence            a page every T seconds, ~1.2% of slots
 *     loss independence      20% of EVERYTHING ELSE lost, every delivered page
 *                            still verifies. This is the entire reason the tier
 *                            exists and it is an assertion, not a paragraph
 *     the replay             a recorded page fails on the counter
 *     the skipped page       a PINNED receiver says UNVERIFIED, not CLEAR
 *
 *   Tier II, when enabled
 *     the window             an N = 8 window verifies
 *     the flipped bit        one verdict, at the attestation page, and no more
 *     the lost packet        one window unverified, the NEXT one verifies
 *
 *   Both
 *     the subtype            a Tier I and a Tier II tag over one counter value
 *                            differ, through the emit path rather than at the
 *                            primitive
 *
 *   Epoch recovery, which is what removing the epoch field has to earn
 *     the hint search        50, 1 000 and 65 536 boots stale, with the
 *                            operation count asserted against the cost table
 *     the hint collision     24 bits collide; 48 bits decide
 *     the tag search         the same set with advertise = off, which is the
 *                            path that otherwise gets written and never run
 *     the re-provision       the epoch moved BACKWARDS, and the bounded
 *                            absolute scan finds it
 *     the miss               nothing matches, and the search still ends
 */

#include <zephyr/ztest.h>
#include <string.h>

#include <radiant_core/radiant_sec_compat.h>

#define TX_CH 0u
#define RX_CH 1u

/* The measured ANT+ 4.005 Hz period, in counts of 1/32768 s, and the interval
 * it makes: one transmitted message every ~249.7 ms. */
#define TEST_PERIOD    8182u
#define PERIOD_US      (((uint64_t)TEST_PERIOD * 1000000u) / 32768u)

#define TEST_T_S       RADIANT_SEC_COMPAT_T_DEFAULT_S
#define TEST_DEVNUM    0x2C41u
#define TEST_N         8u

/*
 * The page byte, composed here. 0x70 is the beacon and the nibble base of the
 * attestation claim, so an attestation page is COMPAT_PAGE_BASE | subtype -
 * the same nibble that went into the MAC'd nonce at position 9.
 */
#define PAGE_BASE      0x70u
#define PAGE_MASK      0x7Fu   /* byte 0 bit 7 is a page-change toggle */
#define DATA_PAGE      0x04u   /* a synthetic data page; no profile meaning */

/*
 * An origin above 2^32 microseconds, deliberately. The module takes 64-bit
 * instants and a test that anchored at zero would pass on an implementation
 * that truncated every one of them to 32 bits - which is the same trap
 * fake_radio's clock origin exists to spring on the rest of radiant_core.
 */
#define T_ORIGIN       ((uint64_t)0x100000000ull + 4321u)

static const uint8_t compat_root[16] = {
	0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78,
	0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0,
};

/*
 * A genuine 24-bit hint collision under the key above, found by birthday search
 * offline and pinned here.
 *
 * IT IS A REAL COLLISION AND THE TEST ASSERTS THAT FIRST. Simulating one - by
 * handing the search a hint that belongs to nobody - would test the search's
 * failure path and call it a collision, which is the opposite of the claim
 * being made: that a hint match is not a confirmation, and that the 48-bit
 * attestation tag is what settles it.
 */
#define HINT_COLLISION_DECOY 55089u
#define HINT_COLLISION_TRUE  55171u

/* ── What the module reported ───────────────────────────────────────────── */

#define VLOG_MAX 64

struct vrec {
	uint8_t                  ch;
	uint8_t                  sub;
	uint16_t                 counter;
	enum radiant_sec_verdict verdict;
};

static struct vrec vlog[VLOG_MAX];
static uint32_t    vlog_n;

/* The strong definition displacing the weak one in radiant_sec_compat.c. */
void radiant_sec_compat_verdict(uint8_t ch, uint8_t sub, uint16_t counter,
				enum radiant_sec_verdict verdict)
{
	if (vlog_n < VLOG_MAX) {
		vlog[vlog_n].ch = ch;
		vlog[vlog_n].sub = sub;
		vlog[vlog_n].counter = counter;
		vlog[vlog_n].verdict = verdict;
	}
	vlog_n++;
}

static uint32_t verdicts(uint8_t sub, enum radiant_sec_verdict want)
{
	uint32_t n = 0u;
	uint32_t i;

	for (i = 0u; i < vlog_n && i < VLOG_MAX; i++) {
		if (vlog[i].sub == sub && vlog[i].verdict == want) {
			n++;
		}
	}
	return n;
}

static uint32_t verdicts_at(uint8_t sub, uint16_t counter,
			    enum radiant_sec_verdict want)
{
	uint32_t n = 0u;
	uint32_t i;

	for (i = 0u; i < vlog_n && i < VLOG_MAX; i++) {
		if (vlog[i].sub == sub && vlog[i].counter == counter &&
		    vlog[i].verdict == want) {
			n++;
		}
	}
	return n;
}

/* ── Harness ────────────────────────────────────────────────────────────── */

static uint64_t t_of(uint32_t slot)
{
	return T_ORIGIN + (uint64_t)slot * PERIOD_US;
}

/* The subtype a receiver reads out of byte [0] - the layer above's job, done
 * here so the round trip through the page byte is part of what is asserted
 * rather than being short-circuited by the emit hook's return value. */
static uint8_t sub_of(const uint8_t *pay)
{
	uint8_t page = pay[0] & PAGE_MASK;

	if (page != (PAGE_BASE | RADIANT_SEC_COMPAT_SUBTYPE_TIER_I) &&
	    page != (PAGE_BASE | RADIANT_SEC_COMPAT_SUBTYPE_TIER_II)) {
		return 0u;
	}
	return page & 0x0Fu;
}

static void setup_pair(uint8_t tx_sw, uint8_t rx_sw, uint32_t tx_epoch,
		       uint32_t rx_epoch)
{
	radiant_sec_compat_reset();
	vlog_n = 0u;

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_key(TX_CH, compat_root, 128,
						 TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_key(RX_CH, compat_root, 128,
						 TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_devnum(TX_CH, TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_devnum(RX_CH, TEST_DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_configure(TX_CH, tx_sw, TEST_N,
						   TEST_T_S));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_configure(RX_CH, rx_sw, TEST_N,
						   TEST_T_S));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_epoch(TX_CH, tx_epoch, T_ORIGIN, 0u,
						   TEST_PERIOD));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_epoch(RX_CH, rx_epoch, T_ORIGIN, 0u,
						   TEST_PERIOD));
}

/*
 * One transmitted slot, exactly as a profile would drive it:
 *
 *   ask whether this slot owes an attestation page; compose byte [0] if it
 *   does, otherwise build an ordinary data page; tell the module what actually
 *   went on the air; hand it to the receiver unless it was lost.
 *
 * `air` receives the eight bytes that went out, for the tests that care.
 * `corrupt_bit` flips one bit on the way, which is what an active injector does.
 */
static int slot_run(uint32_t slot, bool deliver, int corrupt_bit, uint8_t *air,
		    int *rx_rc)
{
	uint8_t  pay[RADIANT_SEC_COMPAT_MSG_BYTES];
	uint8_t  body[RADIANT_SEC_COMPAT_BODY_BYTES];
	uint64_t t = t_of(slot);
	int      sub;
	int      rc;

	sub = radiant_sec_compat_tx_attest(TX_CH, t, body, sizeof(body));
	zassert_true(sub >= 0, "the emit hook refused: %d", sub);

	if (sub > 0) {
		pay[0] = (uint8_t)(PAGE_BASE | (uint8_t)sub);
		memcpy(&pay[1], body, sizeof(body));
	} else {
		pay[0] = DATA_PAGE;
		pay[1] = (uint8_t)slot;
		pay[2] = 0x11u;
		pay[3] = 0x22u;
		pay[4] = 0x33u;
		pay[5] = 0x44u;
		pay[6] = 0x55u;
		pay[7] = 0x66u;
	}

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_tx_sent(TX_CH, pay, sizeof(pay)));

	if (corrupt_bit >= 0) {
		pay[corrupt_bit / 8] ^= (uint8_t)(1u << (corrupt_bit % 8));
	}
	if (air != NULL) {
		memcpy(air, pay, sizeof(pay));
	}

	if (deliver) {
		rc = radiant_sec_compat_rx(RX_CH, sub_of(pay), pay, sizeof(pay),
					   t);
		if (rx_rc != NULL) {
			*rx_rc = rc;
		}
	}
	return sub;
}

static int slot_at(uint32_t slot, bool deliver)
{
	return slot_run(slot, deliver, -1, NULL, NULL);
}

static void compat_before(void *f)
{
	ARG_UNUSED(f);
	radiant_sec_compat_reset();
	memset(vlog, 0, sizeof(vlog));
	vlog_n = 0u;
}

ZTEST_SUITE(sec_compat_stream, NULL, NULL, compat_before, NULL, NULL);

/* ═══ Tier I, the default configuration ══════════════════════════════════ */

ZTEST(sec_compat_stream, test_tier1_emits_at_T_and_verifies_on_receipt)
{
	struct radiant_sec_compat_stats st;
	uint32_t                        emitted = 0u;
	uint32_t                        i;
	int                             rx_rc = RADIANT_SEC_OK;

	setup_pair(RADIANT_SEC_COMPAT_SW_TIER_I, RADIANT_SEC_COMPAT_SW_TIER_I,
		   4242u, 4242u);

	/* 500 slots at ~4 Hz is a little over two minutes of air, which at
	 * T = 20 s is seven attestation pages. */
	for (i = 0u; i < 500u; i++) {
		if (slot_run(i, true, -1, NULL, &rx_rc) > 0) {
			emitted++;
			zassert_equal(RADIANT_SEC_OK, rx_rc,
				      "a Tier I page did not verify ON RECEIPT, "
				      "which is the only property this tier "
				      "has");
		}
	}

	zassert_equal(7u, emitted,
		      "expected one page per %u s interval over 500 slots, got "
		      "%u", (unsigned)TEST_T_S, emitted);

	/*
	 * THE COMPATIBILITY CLAIM, AS ARITHMETIC. 1.2% of slots is what makes
	 * the cost of authentication smaller than the 1.65% ANT+ itself spends
	 * on common pages 80 and 81 - the sentence the whole tier rests on - so
	 * the ceiling is asserted rather than described.
	 */
	zassert_true(emitted * 100u <= 500u * 2u,
		     "Tier I took %u of 500 slots; the claim is ~1.2%%",
		     emitted);

	radiant_sec_compat_get_stats(RX_CH, &st);
	zassert_equal(emitted, st.tier1_verified, NULL);
	zassert_equal(0u, st.tier1_unverified, NULL);
	zassert_equal(emitted, verdicts(RADIANT_SEC_COMPAT_SUBTYPE_TIER_I,
					RADIANT_SEC_VERDICT_VERIFIED), NULL);
}

ZTEST(sec_compat_stream, test_every_delivered_tier1_page_verifies_through_loss)
{
	/*
	 * THE ASSERTION THIS TIER EXISTS FOR.
	 *
	 * The tag covers no payload, so nothing outside the page can break it:
	 * verification rate equals PAGE DELIVERY rate, independently of the
	 * interval and independently of what else was lost. A window CMAC
	 * cannot have that property at any length, and it is the reason the
	 * cheap tier is the one that is on by default.
	 *
	 * 20% loss is fifty times the characterised ~0.4% bench floor. If a
	 * single delivered page failed to verify here, the tier would be a
	 * window MAC wearing a different name.
	 */
	struct radiant_sec_compat_stats st;
	uint32_t                        emitted = 0u;
	uint32_t                        lost = 0u;
	uint32_t                        seed = 0x13579BDFu;
	uint32_t                        i;

	setup_pair(RADIANT_SEC_COMPAT_SW_TIER_I, RADIANT_SEC_COMPAT_SW_TIER_I,
		   77u, 77u);

	/* The loop is written out rather than going through slot_run(), because
	 * whether to drop depends on what the emit hook returned and that is
	 * only known after asking it. */
	for (i = 0u; i < 500u; i++) {
		uint8_t  pay[RADIANT_SEC_COMPAT_MSG_BYTES];
		uint8_t  body[RADIANT_SEC_COMPAT_BODY_BYTES];
		uint64_t t = t_of(i);
		int      sub;
		bool     drop;

		seed = seed * 1103515245u + 12345u;
		drop = ((seed >> 16) % 100u) < 20u;

		sub = radiant_sec_compat_tx_attest(TX_CH, t, body, sizeof(body));
		zassert_true(sub >= 0, NULL);
		if (sub > 0) {
			pay[0] = (uint8_t)(PAGE_BASE | (uint8_t)sub);
			memcpy(&pay[1], body, sizeof(body));
		} else {
			memset(pay, (uint8_t)i, sizeof(pay));
			pay[0] = DATA_PAGE;
		}
		zassert_equal(RADIANT_SEC_OK,
			      radiant_sec_compat_tx_sent(TX_CH, pay,
							 sizeof(pay)));

		/* EVERYTHING ELSE is dropped at 20%. The attestation pages
		 * themselves are delivered, because the claim under test is
		 * about DELIVERED pages: at the same 20% one page in five would
		 * simply not arrive, which is the "verification rate equals
		 * page delivery rate" half of the same sentence and needs no
		 * cryptography to demonstrate. */
		if (sub == 0 && drop) {
			lost++;
			continue;
		}
		if (sub > 0) {
			emitted++;
		}
		zassert_equal(RADIANT_SEC_OK,
			      radiant_sec_compat_rx(RX_CH, sub_of(pay), pay,
						    sizeof(pay), t),
			      "a delivered Tier I page failed to verify after "
			      "%u surrounding packets were lost", lost);
	}

	zassert_true(lost > 50u, "the loss model dropped almost nothing (%u)",
		     lost);
	zassert_equal(7u, emitted, NULL);

	radiant_sec_compat_get_stats(RX_CH, &st);
	zassert_equal(emitted, st.tier1_verified,
		      "%u of %u delivered pages verified with %u packets lost "
		      "around them", st.tier1_verified, emitted, lost);
	zassert_equal(0u, st.tier1_unverified,
		      "loss elsewhere unverified a Tier I page; the tag covers "
		      "no payload and cannot depend on it");
}

ZTEST(sec_compat_stream, test_a_replayed_tier1_page_is_rejected_by_the_counter)
{
	struct radiant_sec_compat_stats st;
	uint8_t                         captured[RADIANT_SEC_COMPAT_MSG_BYTES];
	uint32_t                        i;
	int                             rx_rc = RADIANT_SEC_OK;
	bool                            have = false;

	setup_pair(RADIANT_SEC_COMPAT_SW_TIER_I, RADIANT_SEC_COMPAT_SW_TIER_I,
		   909u, 909u);

	/* Capture the first page an attacker would record. */
	for (i = 0u; i < 200u && !have; i++) {
		uint8_t air[RADIANT_SEC_COMPAT_MSG_BYTES];

		if (slot_run(i, true, -1, air, &rx_rc) > 0) {
			memcpy(captured, air, sizeof(captured));
			have = true;
			zassert_equal(RADIANT_SEC_OK, rx_rc, NULL);
		}
	}
	zassert_true(have, "no Tier I page was emitted at all");

	/*
	 * The recording, replayed later. Its tag is perfectly valid - that is
	 * what makes replay the interesting attack on a tag that covers no
	 * payload - and the counter is what makes it worthless.
	 */
	zassert_equal(RADIANT_SEC_EREPLAY,
		      radiant_sec_compat_rx(RX_CH, sub_of(captured), captured,
					    sizeof(captured), t_of(300u)),
		      "a replayed Tier I page was accepted");

	radiant_sec_compat_get_stats(RX_CH, &st);
	zassert_equal(1u, st.tier1_replays,
		      "the replay must be counted, or it is invisible to a host");
	zassert_equal(1u, st.tier1_verified,
		      "the replay was counted as a fresh verification");
	zassert_equal(0u, st.tier1_unverified,
		      "a replay is not a forgery and must not be counted as one");
}

ZTEST(sec_compat_stream, test_a_skipped_page_is_unverified_on_a_pinned_receiver)
{
	/*
	 * DOWNGRADE PROTECTION IS RECEIVER-SIDE, and this is the whole of it.
	 * Strip the attestation off the air and a naive receiver falls back to
	 * clear and calls that normal; a receiver that has enrolled this sensor
	 * pins it, and a stream with no attestation in it reads UNVERIFIED.
	 * CLEAR keeps meaning "no key here".
	 */
	const uint8_t pinned = RADIANT_SEC_COMPAT_SW_TIER_I |
			       RADIANT_SEC_COMPAT_SW_PIN;
	uint32_t      i;
	uint32_t      emitted = 0u;
	uint64_t      t_first_page = 0u;

	setup_pair(RADIANT_SEC_COMPAT_SW_TIER_I, pinned, 31u, 31u);

	/* Before anything has arrived, a pinned receiver is not entitled to
	 * call the stream verified. */
	zassert_equal(RADIANT_SEC_VERDICT_UNVERIFIED,
		      radiant_sec_compat_stream_verdict(RX_CH, t_of(0u)),
		      "a pinned receiver reported something other than "
		      "UNVERIFIED before any attestation arrived");

	/* Deliver exactly one page, then skip every one after it. */
	for (i = 0u; i < 200u; i++) {
		bool deliver = (emitted == 0u);
		int  sub = slot_run(i, deliver, -1, NULL, NULL);

		if (sub == RADIANT_SEC_COMPAT_SUBTYPE_TIER_I) {
			if (emitted == 0u) {
				t_first_page = t_of(i);
			}
			emitted++;
		}
	}
	zassert_true(emitted >= 3u, "not enough pages were emitted to skip one");

	/* Inside the horizon: verified, and it is an IDENTITY claim. */
	zassert_equal(RADIANT_SEC_VERDICT_VERIFIED,
		      radiant_sec_compat_stream_verdict(RX_CH,
							t_first_page + 1000u),
		      NULL);

	/*
	 * One whole interval of slack and no more: a single lost page at the
	 * ~0.4% floor is ordinary and must not un-pin a stream, and two
	 * consecutive misses are ~1.6e-5 and are a signal.
	 */
	zassert_equal(RADIANT_SEC_VERDICT_VERIFIED,
		      radiant_sec_compat_stream_verdict(
			      RX_CH, t_first_page +
					     (uint64_t)TEST_T_S * 1000000u + 1u),
		      "one missing page unpinned the stream");
	zassert_equal(RADIANT_SEC_VERDICT_UNVERIFIED,
		      radiant_sec_compat_stream_verdict(
			      RX_CH, t_first_page +
					     (uint64_t)RADIANT_SEC_COMPAT_STALE_INTERVALS *
						     TEST_T_S * 1000000u),
		      "a skipped attestation page read as anything but "
		      "UNVERIFIED. CLEAR here would be the downgrade");

	/*
	 * And the other half of the claim: UNPINNED, the same silence is CLEAR.
	 * If both answers were UNVERIFIED the distinction would be decorative,
	 * and CLEAR would have stopped meaning "no key here".
	 */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_configure(RX_CH,
						   RADIANT_SEC_COMPAT_SW_TIER_I,
						   TEST_N, TEST_T_S));
	zassert_equal(RADIANT_SEC_VERDICT_CLEAR,
		      radiant_sec_compat_stream_verdict(RX_CH, t_of(400u)),
		      NULL);
}

/* ═══ Tier II, when enabled ══════════════════════════════════════════════ */

#define BOTH_TIERS (RADIANT_SEC_COMPAT_SW_TIER_I | RADIANT_SEC_COMPAT_SW_TIER_II)

ZTEST(sec_compat_stream, test_an_n8_window_verifies)
{
	struct radiant_sec_compat_stats st;
	uint32_t                        i;

	setup_pair(BOTH_TIERS, BOTH_TIERS, 55u, 55u);

	/* Three windows of eight transmitted messages. Unlike the spread MAC
	 * there is no one-window tag lag here: the tag rides in the window it
	 * covers, so the first window a receiver hears is judged. */
	for (i = 0u; i < 24u; i++) {
		(void)slot_at(i, true);
	}

	radiant_sec_compat_get_stats(RX_CH, &st);
	zassert_equal(3u, st.tier2_verified,
		      "expected three verified windows, got %u verified and %u "
		      "unverified", st.tier2_verified, st.tier2_unverified);
	zassert_equal(0u, st.tier2_unverified, NULL);
	zassert_equal(1u, verdicts_at(RADIANT_SEC_COMPAT_SUBTYPE_TIER_II, 0u,
				      RADIANT_SEC_VERDICT_VERIFIED), NULL);
	zassert_equal(1u, verdicts_at(RADIANT_SEC_COMPAT_SUBTYPE_TIER_II, 1u,
				      RADIANT_SEC_VERDICT_VERIFIED), NULL);
	zassert_equal(1u, verdicts_at(RADIANT_SEC_COMPAT_SUBTYPE_TIER_II, 2u,
				      RADIANT_SEC_VERDICT_VERIFIED), NULL);
}

ZTEST(sec_compat_stream, test_a_flipped_bit_lands_once_at_the_attestation_page)
{
	uint32_t before_close;
	uint32_t i;

	setup_pair(BOTH_TIERS, BOTH_TIERS, 56u, 56u);

	/* One flipped payload bit in slot 3, which is inside window 0. */
	for (i = 0u; i < 7u; i++) {
		(void)slot_run(i, true, (i == 3u) ? (4 * 8 + 2) : -1, NULL, NULL);
	}

	/*
	 * NOTHING HAS BEEN SAID YET, and that is the tier's cost rather than a
	 * bug: a window CMAC cannot be judged until the window closes, which is
	 * N-1 messages of latency and the reason this tier is off by default.
	 */
	before_close = verdicts(RADIANT_SEC_COMPAT_SUBTYPE_TIER_II,
				RADIANT_SEC_VERDICT_UNVERIFIED) +
		       verdicts(RADIANT_SEC_COMPAT_SUBTYPE_TIER_II,
				RADIANT_SEC_VERDICT_VERIFIED);
	zassert_equal(0u, before_close,
		      "a window verdict arrived before the window closed");

	(void)slot_at(7u, true);   /* the attestation page */

	zassert_equal(1u, verdicts_at(RADIANT_SEC_COMPAT_SUBTYPE_TIER_II, 0u,
				      RADIANT_SEC_VERDICT_UNVERIFIED),
		      "the forged window was not reported unverified exactly "
		      "once, at the page that carries its tag");
	zassert_equal(0u, verdicts_at(RADIANT_SEC_COMPAT_SUBTYPE_TIER_II, 0u,
				      RADIANT_SEC_VERDICT_VERIFIED), NULL);

	/* And the damage is bounded to that window. */
	for (i = 8u; i < 16u; i++) {
		(void)slot_at(i, true);
	}
	zassert_equal(1u, verdicts_at(RADIANT_SEC_COMPAT_SUBTYPE_TIER_II, 1u,
				      RADIANT_SEC_VERDICT_VERIFIED),
		      "one forged packet poisoned the following window");
}

ZTEST(sec_compat_stream, test_a_dropped_packet_costs_one_window_and_no_more)
{
	struct radiant_sec_compat_stats st;
	uint32_t                        i;

	setup_pair(BOTH_TIERS, BOTH_TIERS, 57u, 57u);

	/* Drop slot 3, inside window 0. At a measured ~0.4% floor this is the
	 * ordinary case, not an exotic one - and it is the honest regression a
	 * window MAC has and the spread tag does not. */
	for (i = 0u; i < 24u; i++) {
		(void)slot_at(i, i != 3u);
	}

	zassert_equal(1u, verdicts_at(RADIANT_SEC_COMPAT_SUBTYPE_TIER_II, 0u,
				      RADIANT_SEC_VERDICT_UNVERIFIED),
		      "a window with a hole in it should be unverifiable");
	zassert_equal(1u, verdicts_at(RADIANT_SEC_COMPAT_SUBTYPE_TIER_II, 1u,
				      RADIANT_SEC_VERDICT_VERIFIED),
		      "THE NEXT WINDOW MUST RECOVER: one lost packet may not "
		      "desynchronise every window after it");
	zassert_equal(1u, verdicts_at(RADIANT_SEC_COMPAT_SUBTYPE_TIER_II, 2u,
				      RADIANT_SEC_VERDICT_VERIFIED), NULL);

	radiant_sec_compat_get_stats(RX_CH, &st);
	zassert_equal(2u, st.tier2_verified, NULL);
	zassert_equal(1u, st.tier2_unverified, NULL);
}

/* ═══ Both tiers ═════════════════════════════════════════════════════════ */

ZTEST(sec_compat_stream, test_the_subtype_reaches_the_nonce_through_the_emit_path)
{
	/*
	 * test_sec_compat.c asserts this at the primitive, by building two
	 * nonce blocks. This asserts it END TO END: the two tiers' first tags
	 * of an epoch are both over counter value 0 - interval 0 and window 0 -
	 * under one epoch and one device number, so if the subtype reached only
	 * the page byte the two tags would be interchangeable and a Tier II tag
	 * would replay as a Tier I one.
	 */
	uint8_t tier1_air[RADIANT_SEC_COMPAT_MSG_BYTES];
	uint8_t tier2_air[RADIANT_SEC_COMPAT_MSG_BYTES];
	uint8_t forged[RADIANT_SEC_COMPAT_MSG_BYTES];
	uint32_t i;

	setup_pair(BOTH_TIERS, BOTH_TIERS, 61u, 61u);

	/* Nothing is delivered yet: the receiver must meet the forgery before
	 * it has seen the genuine page, or the counter would reject the forgery
	 * as a replay and the tag would never be reached. */
	(void)slot_run(0u, false, -1, tier1_air, NULL);
	for (i = 1u; i < 7u; i++) {
		(void)slot_at(i, false);
	}
	(void)slot_run(7u, false, -1, tier2_air, NULL);

	zassert_equal(RADIANT_SEC_COMPAT_SUBTYPE_TIER_I, sub_of(tier1_air), NULL);
	zassert_equal(RADIANT_SEC_COMPAT_SUBTYPE_TIER_II, sub_of(tier2_air),
		      NULL);
	zassert_equal(0u, (uint32_t)tier1_air[1] | ((uint32_t)tier1_air[2] << 8),
		      "the Tier I counter should be 0 at the epoch anchor");
	zassert_equal(0u, tier2_air[1], "the first window index should be 0");

	zassert_true(memcmp(&tier1_air[3], &tier2_air[2],
			    RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES) != 0,
		     "one counter value, two subtypes, one tag: the subtype is "
		     "not reaching the MAC'd block through the emit path");

	/* The substitution, attempted: a Tier II tag dressed as a Tier I page. */
	forged[0] = PAGE_BASE | RADIANT_SEC_COMPAT_SUBTYPE_TIER_I;
	forged[1] = 0u;
	forged[2] = 0u;
	memcpy(&forged[3], &tier2_air[2], RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES);
	zassert_equal(RADIANT_SEC_EBADMAC,
		      radiant_sec_compat_rx(RX_CH, sub_of(forged), forged,
					    sizeof(forged), t_of(0u)),
		      "a Tier II tag verified as a Tier I tag");

	/* ...and the genuine page, over the same counter, does verify - so the
	 * refusal above is about the subtype and not about the counter. */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_rx(RX_CH, sub_of(tier1_air), tier1_air,
					    sizeof(tier1_air), t_of(0u)),
		      NULL);
}

/* ═══ Occasional contact: recovering an epoch nobody broadcasts ══════════ */

/*
 * The bound, as one expression, so a test cannot assert a number the header
 * does not stand behind: every phase is bounded and `ops` counts across all
 * three.
 */
#define OPS_CEILING (RADIANT_SEC_COMPAT_EPOCH_FORWARD + 1u +		\
		     RADIANT_SEC_COMPAT_EPOCH_BACKWARD +		\
		     RADIANT_SEC_COMPAT_EPOCH_SCAN + 1u)

/* Run one full Tier II window and report whether it verified. The confirmation
 * step of every recovery below: a 48-bit tag is what a 24-bit hint match has to
 * survive. */
static bool window_verifies(uint32_t first_slot)
{
	uint32_t before = verdicts(RADIANT_SEC_COMPAT_SUBTYPE_TIER_II,
				   RADIANT_SEC_VERDICT_VERIFIED);
	uint32_t i;

	for (i = first_slot; i < first_slot + 2u * TEST_N; i++) {
		(void)slot_at(i, true);
	}
	return verdicts(RADIANT_SEC_COMPAT_SUBTYPE_TIER_II,
			RADIANT_SEC_VERDICT_VERIFIED) > before;
}

static void recover_by_hint(uint32_t epoch, uint32_t staleness)
{
	uint8_t  hint[RADIANT_SEC_COMPAT_HINT_BYTES];
	uint32_t found = 0u;
	uint32_t ops = 0u;
	uint8_t  path = 0xFFu;

	setup_pair(BOTH_TIERS, BOTH_TIERS, epoch, epoch - staleness);

	/* The beacon's key-group hint, as the sensor would put it on the air.
	 * K_id is epoch-less, so both ends compute it from the same root. */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_hint(TX_CH, epoch, hint), NULL);

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_recover_hint(RX_CH, hint,
						      epoch - staleness, &found,
						      &ops, &path),
		      "a receiver %u boots behind did not recover the epoch",
		      staleness);
	zassert_equal(epoch, found, "recovered epoch %u, wanted %u", found,
		      epoch);
	zassert_equal(RADIANT_SEC_COMPAT_PATH_FORWARD, path, NULL);

	/*
	 * THE COST TABLE, AS AN ASSERTION. docs/radiant-security.md section
	 * 11.3 quotes 50 / 1 000 / 65 536 CMAC operations; the search tries
	 * `last_seen` itself first, because a sensor that has not rebooted
	 * since is the common case and not an edge case, so the number is one
	 * higher and that off-by-one is stated rather than rounded away.
	 */
	zassert_equal(staleness + 1u, ops,
		      "%u boots behind cost %u operations, not %u", staleness,
		      ops, staleness + 1u);
	zassert_true(ops <= OPS_CEILING, NULL);

	/* And the 48-bit confirmation. */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_adopt_epoch(RX_CH, found), NULL);
	zassert_true(window_verifies(0u),
		     "the recovered epoch did not verify a window, so the "
		     "recovery proved nothing");
}

ZTEST(sec_compat_stream, test_a_receiver_50_boots_behind_recovers_by_hint)
{
	recover_by_hint(500000u, 50u);
}

ZTEST(sec_compat_stream, test_a_receiver_1000_boots_behind_recovers_by_hint)
{
	recover_by_hint(500000u, 1000u);
}

ZTEST(sec_compat_stream, test_a_receiver_65536_boots_behind_recovers_by_hint)
{
	/* The absurd row of the cost table, and the ceiling itself: 65 536
	 * boots behind is the last candidate the forward phase tries. */
	recover_by_hint(500000u, RADIANT_SEC_COMPAT_EPOCH_FORWARD);
}

ZTEST(sec_compat_stream, test_a_hint_collision_is_settled_by_the_48_bit_tag)
{
	uint8_t  decoy_hint[RADIANT_SEC_COMPAT_HINT_BYTES];
	uint8_t  true_hint[RADIANT_SEC_COMPAT_HINT_BYTES];
	uint32_t last_seen = HINT_COLLISION_DECOY - 5u;
	uint32_t found = 0u;
	uint32_t ops = 0u;
	uint8_t  path = 0xFFu;

	setup_pair(BOTH_TIERS, BOTH_TIERS, HINT_COLLISION_TRUE, last_seen);

	/* First: the vector is a REAL collision. A pinned pair that stopped
	 * colliding - because the key changed, or the hint width did - would
	 * otherwise leave this test passing while testing nothing. */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_hint(TX_CH, HINT_COLLISION_DECOY,
					      decoy_hint), NULL);
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_hint(TX_CH, HINT_COLLISION_TRUE,
					      true_hint), NULL);
	zassert_mem_equal(decoy_hint, true_hint, sizeof(true_hint),
			  "the pinned epochs no longer collide; this test is "
			  "vacuous until a new pair is found");
	zassert_true(HINT_COLLISION_DECOY < HINT_COLLISION_TRUE, NULL);

	/* The search stops at the collision, which is what 24 bits buys. */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_recover_hint(RX_CH, true_hint,
						      last_seen, &found, &ops,
						      &path), NULL);
	zassert_equal(HINT_COLLISION_DECOY, found,
		      "the forward search did not stop at the colliding epoch");
	zassert_equal(6u, ops, NULL);

	/* ...and the window refuses it. THE POINT: a hint match is not a
	 * confirmation, and 40 or 48 bits cannot be survived by a 24-bit
	 * collision. */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_adopt_epoch(RX_CH, found), NULL);
	zassert_false(window_verifies(0u),
		      "a colliding epoch verified a window; the hint is being "
		      "trusted as a confirmation");

	/* The receiver resumes the search past the collision and finds the real
	 * one, which is what makes a collision a delay rather than a failure. */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_recover_hint(RX_CH, true_hint,
						      found + 1u, &found, &ops,
						      &path), NULL);
	zassert_equal(HINT_COLLISION_TRUE, found, NULL);
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_adopt_epoch(RX_CH, found), NULL);
	zassert_true(window_verifies(2u * TEST_N),
		     "the resumed search's epoch did not verify");
}

/* ── advertise = off: no hint to search against ─────────────────────────── */

/*
 * The path that otherwise gets written and never exercised. With no beacon
 * there is nothing to match, so candidate epochs are trial-verified against the
 * attestation tag itself: one derivation and one block per candidate for
 * Tier I, N blocks for Tier II. An advertise-off node is slower to re-acquire,
 * not undiscoverable to a keyholder.
 */
static void recover_by_tier1_tag(uint32_t epoch, uint32_t staleness)
{
	uint8_t  air[RADIANT_SEC_COMPAT_MSG_BYTES];
	uint32_t found = 0u;
	uint32_t ops = 0u;
	uint8_t  path = 0xFFu;
	uint16_t counter;

	setup_pair(BOTH_TIERS, BOTH_TIERS, epoch, epoch - staleness);

	/* One Tier I page off the air, and nothing else known about the
	 * sender. */
	zassert_equal(RADIANT_SEC_COMPAT_SUBTYPE_TIER_I,
		      slot_run(0u, false, -1, air, NULL), NULL);
	counter = (uint16_t)((uint32_t)air[1] | ((uint32_t)air[2] << 8));

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_recover_tier1(RX_CH, TEST_DEVNUM,
						       counter, &air[3],
						       epoch - staleness, &found,
						       &ops, &path),
		      "the advertise-off path failed %u boots behind",
		      staleness);
	zassert_equal(epoch, found, NULL);
	zassert_equal(RADIANT_SEC_COMPAT_PATH_FORWARD, path, NULL);
	zassert_equal(staleness + 1u, ops, NULL);
	zassert_true(ops <= OPS_CEILING, NULL);

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_adopt_epoch(RX_CH, found), NULL);
	zassert_true(window_verifies(1u),
		     "the epoch recovered from a tag did not verify a window");
}

ZTEST(sec_compat_stream, test_advertise_off_recovers_50_boots_behind)
{
	recover_by_tier1_tag(400000u, 50u);
}

ZTEST(sec_compat_stream, test_advertise_off_recovers_1000_boots_behind)
{
	recover_by_tier1_tag(400000u, 1000u);
}

ZTEST(sec_compat_stream, test_advertise_off_recovers_65536_boots_behind)
{
	recover_by_tier1_tag(400000u, RADIANT_SEC_COMPAT_EPOCH_FORWARD);
}

ZTEST(sec_compat_stream, test_advertise_off_recovers_against_a_window_tag)
{
	/*
	 * The same path against Tier II, which costs N blocks per candidate
	 * rather than one. Worth its own test because the two tags differ in
	 * what they cover: this one has to be handed the covered messages as
	 * well, and a receiver that had them in the wrong order would recover
	 * nothing.
	 */
	const uint32_t epoch = 300000u;
	const uint32_t staleness = 50u;
	uint8_t        covered[TEST_N - 1u][RADIANT_SEC_COMPAT_MSG_BYTES];
	uint8_t        air[RADIANT_SEC_COMPAT_MSG_BYTES];
	uint32_t       found = 0u;
	uint32_t       ops = 0u;
	uint8_t        path = 0xFFu;
	uint32_t       i;

	setup_pair(BOTH_TIERS, BOTH_TIERS, epoch, epoch - staleness);

	for (i = 0u; i < TEST_N - 1u; i++) {
		(void)slot_run(i, false, -1, covered[i], NULL);
	}
	zassert_equal(RADIANT_SEC_COMPAT_SUBTYPE_TIER_II,
		      slot_run(TEST_N - 1u, false, -1, air, NULL), NULL);

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_recover_tier2(RX_CH, TEST_DEVNUM,
						       air[1], &covered[0][0],
						       TEST_N, &air[2],
						       epoch - staleness, &found,
						       &ops, &path), NULL);
	zassert_equal(epoch, found, NULL);
	zassert_equal(staleness + 1u, ops, NULL);
	zassert_equal(RADIANT_SEC_COMPAT_PATH_FORWARD, path, NULL);
}

/* ── The re-provisioned sensor, and the bound ───────────────────────────── */

ZTEST(sec_compat_stream, test_a_re_provisioned_sensor_is_found_by_the_scan)
{
	/*
	 * The epoch MOVED BACKWARDS. For a hostless strap the epoch is the boot
	 * counter, so a re-provisioned one is at a small number again and a
	 * forward search from where this receiver last saw it can never
	 * succeed. The bounded absolute scan is what answers, and asserting
	 * WHICH PHASE answered is the point: a search that quietly ran forever
	 * would also produce the right epoch eventually.
	 */
	const uint32_t last_seen = 5000u;
	const uint32_t reset_to = 3u;
	uint8_t        hint[RADIANT_SEC_COMPAT_HINT_BYTES];
	uint32_t       found = 0u;
	uint32_t       ops = 0u;
	uint8_t        path = 0xFFu;

	setup_pair(BOTH_TIERS, BOTH_TIERS, reset_to, last_seen);

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_hint(TX_CH, reset_to, hint), NULL);
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_recover_hint(RX_CH, hint, last_seen,
						      &found, &ops, &path),
		      NULL);

	zassert_equal(reset_to, found, NULL);
	zassert_equal(RADIANT_SEC_COMPAT_PATH_SCAN, path,
		      "a re-provisioned sensor must be found by the bounded "
		      "absolute scan, not by a forward search that ran on");
	/* Forward exhausted, the small backward probe spent, and then found at
	 * the fourth candidate of the scan. Spelled out rather than bounded
	 * loosely, because the whole claim is that each phase has a ceiling. */
	zassert_equal(RADIANT_SEC_COMPAT_EPOCH_FORWARD + 1u +
			      RADIANT_SEC_COMPAT_EPOCH_BACKWARD + reset_to + 1u,
		      ops, "the scan cost %u operations", ops);
	zassert_true(ops <= OPS_CEILING, NULL);

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_adopt_epoch(RX_CH, found),
		      "adopting a LOWER epoch was refused; a receiver is not "
		      "the epoch authority and a re-provisioned sensor really "
		      "is at a smaller one");
	zassert_true(window_verifies(0u), NULL);
}

ZTEST(sec_compat_stream, test_a_search_that_matches_nothing_still_ends)
{
	/*
	 * The claim that makes every number above safe to run in a receiver's
	 * event thread: not "the search finds it", but "the search ENDS".
	 *
	 * `last_seen` sits four below the top of the counter, which does two
	 * things at once. It exercises the overflow guard - a forward search
	 * from 0xFFFFFFFC must stop at 0xFFFFFFFF rather than wrapping to zero
	 * and walking the whole space again - and it makes this test cost one
	 * scan rather than two, which matters because every operation here is a
	 * real CMAC. The forward phase's own ceiling is asserted exactly by the
	 * re-provision test above, which exhausts it.
	 *
	 * The sensor's epoch is above the absolute scan's ceiling and below the
	 * backward probe, so no phase can match and the answer is a refusal at
	 * a stated cost rather than a receiver that never comes back.
	 */
	const uint32_t last_seen = 0xFFFFFFFCu;
	const uint32_t unreachable = 150000u;
	const uint32_t expect_ops = 4u + RADIANT_SEC_COMPAT_EPOCH_BACKWARD +
				    RADIANT_SEC_COMPAT_EPOCH_SCAN + 1u;
	uint8_t        hint[RADIANT_SEC_COMPAT_HINT_BYTES];
	uint32_t       found = 0xDEADu;
	uint32_t       ops = 0u;
	uint8_t        path = 0xFFu;

	setup_pair(BOTH_TIERS, BOTH_TIERS, unreachable, 1u);

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_hint(TX_CH, unreachable, hint), NULL);
	zassert_equal(RADIANT_SEC_EBADMAC,
		      radiant_sec_compat_recover_hint(RX_CH, hint, last_seen,
						      &found, &ops, &path),
		      "an epoch outside every bounded phase was somehow found");
	zassert_equal(RADIANT_SEC_COMPAT_PATH_NONE, path, NULL);
	zassert_equal(expect_ops, ops,
		      "the exhausted search cost %u operations, not the %u four "
		      "candidates below the top of the counter buys", ops,
		      expect_ops);
	zassert_true(ops <= OPS_CEILING, NULL);
}

/* ── Refusals ───────────────────────────────────────────────────────────── */

ZTEST(sec_compat_stream, test_the_shape_of_a_misconfigured_channel)
{
	uint8_t body[RADIANT_SEC_COMPAT_BODY_BYTES];

	/* Nothing is due on a channel with no key, and it is 0 rather than an
	 * error so a profile can call the hook unconditionally in its slot
	 * loop. */
	zassert_equal(0, radiant_sec_compat_tx_attest(TX_CH, t_of(0u), body,
						      sizeof(body)));
	zassert_equal(RADIANT_SEC_ENOKEY,
		      radiant_sec_compat_configure(TX_CH, BOTH_TIERS, TEST_N,
						   TEST_T_S));

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_key(TX_CH, compat_root, 128,
						 TEST_DEVNUM));

	/* N is enumerated, not ranged. */
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_compat_configure(TX_CH, BOTH_TIERS, 5u,
						   TEST_T_S));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_compat_configure(TX_CH, BOTH_TIERS, 64u,
						   TEST_T_S));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_compat_configure(TX_CH, 0x80u, TEST_N,
						   TEST_T_S));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_compat_configure(TX_CH, BOTH_TIERS, TEST_N,
						   0u));

	/* And the epoch gate is D3's, unchanged: forward only, with headroom
	 * for a wrap to advance into. */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_configure(TX_CH, BOTH_TIERS, TEST_N,
						   TEST_T_S));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_epoch(TX_CH, 100u, T_ORIGIN, 0u,
						   TEST_PERIOD));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_compat_set_epoch(TX_CH, 100u, T_ORIGIN, 0u,
						   TEST_PERIOD));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_compat_set_epoch(TX_CH, 99u, T_ORIGIN, 0u,
						   TEST_PERIOD));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_compat_set_epoch(TX_CH, 0xFFFFFFFFu, T_ORIGIN,
						   0u, TEST_PERIOD));

	/* A released channel attests nothing. */
	radiant_sec_compat_channel_release(TX_CH);
	zassert_equal(0, radiant_sec_compat_tx_attest(TX_CH, t_of(0u), body,
						      sizeof(body)));
}
