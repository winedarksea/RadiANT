/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work. The frame this file repairs over and
 * over is the one Spike A pulled off the air on real silicon
 * (docs/spike-a-results.md, archive/captures/radio/2026-08-09-nrf54l15-run*.log)
 * with its independently established CRC; the arithmetic being tested is the
 * standard linearity of a CRC over GF(2). Nothing here derives from sdk-ant,
 * from libant.a, or from any adopter-gated ANT+ device profile document.
 *
 * ---------------------------------------------------------------------------
 * The suite for radiant_core/src/radiant_crc_repair.c
 * ---------------------------------------------------------------------------
 * The feature turns a frame that failed its CRC into one that did not, which
 * means every bug in it produces a frame that claims to be good and is not.
 * There is no error code for that and no way to see it from the host: a
 * mis-repaired power page is a plausible number, and the accumulators it feeds
 * are cumulative, so it looks like the sensor rather than like the receiver.
 *
 * So this suite is built around two questions rather than around coverage:
 *
 *   1. Does it repair EVERY single-bit error, exactly, back to the transmitted
 *      bytes? Not a sample - all 96 of them, checked against the original.
 *   2. How often does it repair a MULTI-bit error into something wrong? That
 *      number is the whole safety argument for the feature and it is quoted in
 *      RADIANT_CORE_CRC_REPAIR's help text. A number in a help text that
 *      nothing measures is a number nobody should believe, so this file
 *      enumerates every two-bit error there is and samples the three-bit ones.
 *
 * Question 2's first answer came out of this suite rather than out of the plan
 * that asked for the feature. The plan assumed a flat 1-in-585 chance for any
 * multi-bit error; the measurement is that NO two-bit error is ever repaired,
 * because the polynomial carries an (x + 1) factor and syndrome popcount
 * therefore carries error parity. The exposure is only odd-weight errors of
 * three bits and up. Both the help text and the module header now say so.
 *
 * Everything else - the refusal to serve a search window, the refusal to run
 * without a received CRC - is about keeping question 2's answer confined to a
 * population where it is acceptable.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <radiant_core/radiant_crc_repair.h>
#include <radiant_core/radiant_frame.h>
#include "../fake_radio.h"

/* ---------------------------------------------------------------------------
 * Ground truth
 *
 * The same 15 bytes test_frame.c uses, split the way a tracked window sees
 * them: five address bytes the hardware matched and never handed over, ten body
 * bytes the DMA delivered. The CRC is the one measured on air, not one this
 * file computed.
 * ---------------------------------------------------------------------------
 */

static const uint8_t spike_addr[RADIANT_FRAME_ADDR_LEN_TRACKING] = {
	0xA6, 0xC5, 0x17, 0x3A, 0x0B,
};
static const uint8_t spike_body[] = {
	0x05, 0x0A, 0x10, 0xBD, 0xFF, 0x50, 0xDE, 0x11, 0x64, 0x00,
};
#define SPIKE_CRC 0x199Au

#define BODY_LEN  ((uint8_t)sizeof(spike_body))
#define BODY_BITS ((unsigned int)(BODY_LEN * 8u))
#define CRC_BITS  ((unsigned int)(RADIANT_FRAME_CRC_BYTES * 8u))

static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	fake_radio_reset();
	zassert_true(radiant_crc_repair_init(),
		     "the syndrome table is not a bijection - two single-bit "
		     "errors share a syndrome, which CRC-16/CCITT over 136 bits "
		     "cannot do, so the polynomial or the frame geometry has "
		     "changed");
}

ZTEST_SUITE(radiant_crc_repair, NULL, NULL, before, NULL, NULL);

/* Flip bit `bit` of a body, counting from the LAST byte's least significant bit
 * - the same numbering radiant_crc_repair.c uses internally, so a test that
 * disagrees with it about which bit is which fails loudly rather than silently
 * testing a different bit. */
static void flip(uint8_t *body, unsigned int bit)
{
	body[BODY_LEN - 1u - (bit / 8u)] ^= (uint8_t)(1u << (bit % 8u));
}

/* ---------------------------------------------------------------------------
 * The frame this file assumes
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_crc_repair, test_the_undamaged_frame_has_the_measured_crc)
{
	uint16_t crc;

	/* If this fails, every other test in the file is repairing towards the
	 * wrong answer and would still pass. It goes first for that reason. */
	crc = radiant_crc16(RADIANT_CRC_INIT, spike_addr, sizeof(spike_addr));
	crc = radiant_crc16(crc, spike_body, BODY_LEN);
	zassert_equal(SPIKE_CRC, crc,
		      "expected the on-air CRC 0x%04X, computed 0x%04X",
		      SPIKE_CRC, crc);
}

/* ---------------------------------------------------------------------------
 * Every single-bit error, and only those
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_crc_repair, test_every_body_bit_is_repaired_to_the_original)
{
	unsigned int bit;

	for (bit = 0u; bit < BODY_BITS; bit++) {
		uint8_t body[sizeof(spike_body)];
		int rc;

		memcpy(body, spike_body, BODY_LEN);
		flip(body, bit);

		/*
		 * The CRC handed in is the one that was TRANSMITTED. That is
		 * what a bit flipped in flight does: the sender's CRC arrives
		 * intact and no longer matches the bytes underneath it.
		 * Recomputing it here instead would make the syndrome
		 * identically zero and the whole exercise circular.
		 */
		rc = radiant_crc_repair(RADIANT_FRAME_CFG_TRACKING, spike_addr,
					sizeof(spike_addr), body, BODY_LEN,
					SPIKE_CRC);

		zassert_equal(RADIANT_CRC_REPAIR_BODY, rc,
			      "bit %u was not recognised as a body error (rc=%d)",
			      bit, rc);
		zassert_mem_equal(body, spike_body, BODY_LEN,
				  "bit %u was repaired to the wrong bytes", bit);
	}
}

ZTEST(radiant_crc_repair, test_every_crc_bit_is_recognised_and_the_body_untouched)
{
	unsigned int bit;

	for (bit = 0u; bit < CRC_BITS; bit++) {
		uint8_t body[sizeof(spike_body)];
		int rc;

		/* The damage is in the CRC field rather than the data, which is
		 * a case a receiver has no other way to tell from a corrupted
		 * payload - and getting it wrong would mean flipping a bit in a
		 * body that was already correct. */
		memcpy(body, spike_body, BODY_LEN);
		rc = radiant_crc_repair(RADIANT_FRAME_CFG_TRACKING, spike_addr,
					sizeof(spike_addr), body, BODY_LEN,
					(uint32_t)(SPIKE_CRC ^ (1u << bit)));

		zassert_equal(RADIANT_CRC_REPAIR_IN_CRC, rc,
			      "CRC bit %u was not recognised (rc=%d)", bit, rc);
		zassert_mem_equal(body, spike_body, BODY_LEN,
				  "CRC bit %u: the body was modified when the "
				  "damage was not in it", bit);
	}
}

ZTEST(radiant_crc_repair, test_every_single_bit_error_has_its_own_syndrome)
{
	/*
	 * The bijection, demonstrated rather than asserted about the table.
	 *
	 * radiant_crc_repair_init() checks its own table for duplicates, and a
	 * test that only asked it whether it was happy would be testing the
	 * check against itself. This drives all 96 errors through the public
	 * entry point and requires each one to come back as its own kind of
	 * repair with the original bytes restored - which is the property the
	 * table exists to have, expressed in terms of what a caller sees.
	 *
	 * The two loops above already prove it for each class separately. What
	 * is left, and what this covers, is that no body bit is mistaken for a
	 * CRC bit or vice versa.
	 */
	unsigned int bit;
	unsigned int body_repairs = 0u;
	unsigned int crc_repairs = 0u;

	for (bit = 0u; bit < BODY_BITS; bit++) {
		uint8_t body[sizeof(spike_body)];

		memcpy(body, spike_body, BODY_LEN);
		flip(body, bit);
		if (radiant_crc_repair(RADIANT_FRAME_CFG_TRACKING, spike_addr,
				       sizeof(spike_addr), body, BODY_LEN,
				       SPIKE_CRC) == RADIANT_CRC_REPAIR_BODY) {
			body_repairs++;
		}
	}
	for (bit = 0u; bit < CRC_BITS; bit++) {
		uint8_t body[sizeof(spike_body)];

		memcpy(body, spike_body, BODY_LEN);
		if (radiant_crc_repair(RADIANT_FRAME_CFG_TRACKING, spike_addr,
				       sizeof(spike_addr), body, BODY_LEN,
				       (uint32_t)(SPIKE_CRC ^ (1u << bit))) ==
		    RADIANT_CRC_REPAIR_IN_CRC) {
			crc_repairs++;
		}
	}

	zassert_equal(BODY_BITS, body_repairs, NULL);
	zassert_equal(CRC_BITS, crc_repairs, NULL);
}

/* ---------------------------------------------------------------------------
 * Multi-bit errors: the number the Kconfig help promises
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_crc_repair, test_no_two_bit_error_is_ever_repaired)
{
	unsigned int a;
	unsigned int b;
	unsigned int pairs = 0u;

	/*
	 * EVERY two-bit error in the body - 80 choose 2, which is 3160 of them -
	 * and not one of them may be repaired. Not "rarely": never.
	 *
	 * This is stronger than the plan for this feature assumed, and the
	 * reason is a property of the polynomial rather than luck. CRC-16/CCITT
	 * is x^16 + x^12 + x^5 + 1, which evaluates to 0 at x = 1 and is
	 * therefore divisible by (x + 1). For any error e, e = q*p + r gives
	 * e(1) = r(1), and e(1) is the PARITY of the error's weight. So the
	 * syndrome of an odd-weight error always has odd popcount and the
	 * syndrome of an even-weight error always has even popcount, and the
	 * two sets are disjoint. A two-bit error cannot land on a one-bit
	 * error's syndrome no matter how the bits fall.
	 *
	 * That matters for the safety argument, because two-bit errors are by
	 * far the most common multi-bit case at the knee: the false-accept
	 * population is not "everything that is not one bit", it is odd-weight
	 * errors of three bits and up, which are another order of magnitude
	 * down again. test_three_bit_errors_collide_at_the_predicted_rate below
	 * measures what is actually left.
	 *
	 * EAGREE must not appear either: that would mean a frame the backend
	 * called a CRC failure computes to a matching CRC here, which is a
	 * configuration fault rather than a damaged frame and has an entirely
	 * different fix.
	 */
	for (a = 0u; a < BODY_BITS; a++) {
		for (b = a + 1u; b < BODY_BITS; b++) {
			uint8_t body[sizeof(spike_body)];
			int rc;

			memcpy(body, spike_body, BODY_LEN);
			flip(body, a);
			flip(body, b);
			pairs++;

			rc = radiant_crc_repair(RADIANT_FRAME_CFG_TRACKING,
						spike_addr, sizeof(spike_addr),
						body, BODY_LEN, SPIKE_CRC);

			zassert_equal(RADIANT_CRC_REPAIR_ENONE, rc,
				      "bits %u and %u were not refused (rc=%d) "
				      "- the (x + 1) factor of the polynomial "
				      "should make that impossible", a, b, rc);
		}
	}

	zassert_equal(3160u, pairs, "the loop did not cover what it claims");
}

ZTEST(radiant_crc_repair, test_the_syndrome_parity_rule_the_above_rests_on)
{
	unsigned int bit;

	/*
	 * The property stated directly, so that a future change to the
	 * polynomial fails HERE with an explanation rather than three tests
	 * away with a rate that drifted.
	 *
	 * Every single-bit error's syndrome must have odd popcount. If a
	 * polynomial without the (x + 1) factor were adopted, this is the test
	 * that would go red, and the two-bit guarantee above would quietly
	 * become a probability.
	 */
	for (bit = 0u; bit < BODY_BITS; bit++) {
		uint8_t vector[sizeof(spike_body)];
		uint16_t syndrome;
		unsigned int popcount = 0u;
		unsigned int i;

		memset(vector, 0, sizeof(vector));
		vector[0] = (uint8_t)(1u << (bit % 8u));
		syndrome = radiant_crc16(0u, vector, (bit / 8u) + 1u);

		for (i = 0u; i < 16u; i++) {
			popcount += (unsigned int)((syndrome >> i) & 1u);
		}
		zassert_equal(1u, popcount & 1u,
			      "bit %u has an even-popcount syndrome 0x%04X, so "
			      "the polynomial has lost its (x + 1) factor and "
			      "two-bit errors are no longer excluded", bit,
			      syndrome);
	}
}

ZTEST(radiant_crc_repair, test_three_bit_errors_collide_at_the_predicted_rate)
{
	unsigned int a;
	unsigned int b;
	unsigned int c;
	unsigned int triples = 0u;
	unsigned int false_repairs = 0u;
	unsigned int correct_repairs = 0u;

	/*
	 * What the false-accept rate actually is, measured rather than asserted.
	 *
	 * Odd-weight errors CAN collide with a single-bit syndrome, because
	 * both have odd popcount. There are 96 valid entries and 32768
	 * odd-popcount syndromes, so roughly 1 in 341 three-bit errors is
	 * repaired into something that is still wrong. That number is what
	 * RADIANT_CORE_CRC_REPAIR's help text promises and what the decision to
	 * run this on tracked windows only is built on, so it is measured here
	 * rather than left as arithmetic nobody checked.
	 *
	 * Subsampled with a stride: 80 choose 3 is 82160 and the full sweep
	 * takes several seconds on a DK, which is more than this suite's share
	 * of a 60 s console budget. The stride is fixed rather than random -
	 * a rate that only fails on some runs is not a test.
	 */
	for (a = 0u; a < BODY_BITS; a++) {
		for (b = a + 1u; b < BODY_BITS; b++) {
			for (c = b + 1u; c < BODY_BITS; c++) {
				uint8_t body[sizeof(spike_body)];
				int rc;

				triples++;
				if ((triples % 8u) != 0u) {
					continue;
				}

				memcpy(body, spike_body, BODY_LEN);
				flip(body, a);
				flip(body, b);
				flip(body, c);

				rc = radiant_crc_repair(
					RADIANT_FRAME_CFG_TRACKING, spike_addr,
					sizeof(spike_addr), body, BODY_LEN,
					SPIKE_CRC);

				zassert_not_equal(RADIANT_CRC_REPAIR_EAGREE, rc,
						  "three flipped bits computed "
						  "to a matching CRC");

				if (rc == RADIANT_CRC_REPAIR_BODY ||
				    rc == RADIANT_CRC_REPAIR_IN_CRC) {
					false_repairs++;
					if (memcmp(body, spike_body,
						   BODY_LEN) == 0) {
						correct_repairs++;
					}
				}
			}
		}
	}

	zassert_equal(0u, correct_repairs,
		      "a three-bit error was repaired back to the original "
		      "bytes, which no single flip can do - the test is wrong, "
		      "not the code");

	/*
	 * A wide band on purpose: this checks the ORDER of the rate the help
	 * text claims, not its distribution. Both ends are real constraints -
	 * far worse than 1 in 100 would mean the syndrome space is much smaller
	 * than argued and the feature is not safe on the terms it was accepted
	 * on; zero would mean the repair path was never reached and this test
	 * proves nothing at all.
	 */
	zassert_true(false_repairs > 0u,
		     "no three-bit error was falsely repaired in %u samples, "
		     "which is not what the arithmetic predicts - the repair "
		     "path is probably not being reached", triples / 8u);
	zassert_true(false_repairs * 100u < (triples / 8u),
		     "%u of %u three-bit errors falsely repaired - worse than "
		     "1 in 100, so RADIANT_CORE_CRC_REPAIR's false-accept "
		     "arithmetic is wrong", false_repairs, triples / 8u);
}

ZTEST(radiant_crc_repair, test_a_wholly_corrupted_body_is_refused)
{
	uint8_t body[sizeof(spike_body)];
	unsigned int i;
	unsigned int refused = 0u;

	/* Not a burst error in the CRC's sense - just noise, which is what a
	 * demodulator that lost lock produces. Almost all of it must be
	 * refused; the handful that are not are the same 1-in-683 collision
	 * measured above. */
	for (i = 0u; i < 200u; i++) {
		unsigned int j;

		for (j = 0u; j < BODY_LEN; j++) {
			/* A cheap deterministic scramble. Deterministic
			 * matters: a random failure nobody can reproduce is
			 * worse than no test. */
			body[j] = (uint8_t)((i * 37u) + (j * 91u) + 17u);
		}
		if (radiant_crc_repair(RADIANT_FRAME_CFG_TRACKING, spike_addr,
				       sizeof(spike_addr), body, BODY_LEN,
				       SPIKE_CRC) == RADIANT_CRC_REPAIR_ENONE) {
			refused++;
		}
	}

	zassert_true(refused >= 190u,
		     "only %u of 200 scrambled bodies were refused", refused);
}

/* ---------------------------------------------------------------------------
 * The refusals that keep the false-accept rate where it was argued for
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_crc_repair, test_a_search_window_is_refused_by_the_module_itself)
{
	uint8_t body[sizeof(spike_body)];

	/*
	 * THE TEST THIS WHOLE FEATURE'S SAFETY RESTS ON.
	 *
	 * A search window's CRC failures are the 19-27 noise-triggered address
	 * matches per 15 seconds that radiant_search.c counts and discards -
	 * three matched bytes, from nothing at all. Repairing one in 683 of
	 * those would manufacture a device number out of thermal noise and
	 * report a sensor that does not exist.
	 *
	 * The refusal is inside the module rather than at the call site
	 * precisely so that it cannot be forgotten by a future caller who has
	 * an rx_event in hand and does not know which window produced it.
	 */
	memcpy(body, spike_body, BODY_LEN);
	flip(body, 3u);
	zassert_equal(RADIANT_CRC_REPAIR_EINVAL,
		      radiant_crc_repair(RADIANT_FRAME_CFG_SEARCH, spike_addr,
					 RADIANT_FRAME_ADDR_LEN_SEARCH, body,
					 BODY_LEN, SPIKE_CRC),
		      "a search-format window was served a repair");

	/* And the body was not touched on the way to being refused. */
	flip(body, 3u);
	zassert_mem_equal(body, spike_body, BODY_LEN, NULL);
}

ZTEST(radiant_crc_repair, test_bad_arguments_are_refused_without_touching_anything)
{
	uint8_t body[sizeof(spike_body)];

	memcpy(body, spike_body, BODY_LEN);
	zassert_equal(RADIANT_CRC_REPAIR_EINVAL,
		      radiant_crc_repair(RADIANT_FRAME_CFG_TRACKING, NULL,
					 sizeof(spike_addr), body, BODY_LEN,
					 SPIKE_CRC), NULL);
	zassert_equal(RADIANT_CRC_REPAIR_EINVAL,
		      radiant_crc_repair(RADIANT_FRAME_CFG_TRACKING, spike_addr,
					 sizeof(spike_addr), NULL, BODY_LEN,
					 SPIKE_CRC), NULL);
	zassert_equal(RADIANT_CRC_REPAIR_EINVAL,
		      radiant_crc_repair(RADIANT_FRAME_CFG_TRACKING, spike_addr,
					 sizeof(spike_addr), body, 0u,
					 SPIKE_CRC), NULL);
	zassert_equal(RADIANT_CRC_REPAIR_EINVAL,
		      radiant_crc_repair(RADIANT_FRAME_CFG_TRACKING, spike_addr,
					 sizeof(spike_addr), body,
					 RADIANT_FRAME_BODY_MAX + 1u,
					 SPIKE_CRC), NULL);
	zassert_mem_equal(body, spike_body, BODY_LEN, NULL);
}

ZTEST(radiant_crc_repair, test_a_frame_whose_crc_actually_matches_is_named_separately)
{
	uint8_t body[sizeof(spike_body)];

	/*
	 * The backend said CRC_FAIL and the software CRC over the same bytes
	 * says otherwise. That is a different polynomial or a different
	 * coverage - a configuration fault - and reporting it as "no single bit
	 * explains this" would send whoever reads the counter looking for a
	 * marginal link instead of a misconfigured CRC engine.
	 */
	memcpy(body, spike_body, BODY_LEN);
	zassert_equal(RADIANT_CRC_REPAIR_EAGREE,
		      radiant_crc_repair(RADIANT_FRAME_CFG_TRACKING, spike_addr,
					 sizeof(spike_addr), body, BODY_LEN,
					 SPIKE_CRC), NULL);
	zassert_mem_equal(body, spike_body, BODY_LEN, NULL);
}

ZTEST(radiant_crc_repair, test_init_is_idempotent)
{
	zassert_true(radiant_crc_repair_init(), NULL);
	zassert_true(radiant_crc_repair_init(), NULL);
	zassert_true(radiant_crc_repair_ready(), NULL);
}

ZTEST(radiant_crc_repair, test_the_table_has_no_duplicates_for_either_geometry)
{
	uint16_t syndromes[RADIANT_CRC_REPAIR_ENTRIES];
	size_t n = 0u;
	size_t i;
	size_t j;

	/*
	 * The bijection over the WHOLE table, both frame geometries at once,
	 * rebuilt here independently of the module's own check.
	 *
	 * radiant_crc_repair_init() checks itself and returns false on a
	 * collision, and the fixture asserts on that - but a self-check is the
	 * code agreeing with itself. This recomputes every syndrome from
	 * radiant_crc16() directly and compares them pairwise.
	 *
	 * Both geometries, because the table is indexed from the END of the
	 * covered data and is sized for RADIANT_FRAME_BODY_MAX - the longest
	 * body either configuration can produce, which is the search header
	 * plus the longest payload. A tracking frame's 10 standard body bytes
	 * and a search frame's 12 are both suffixes of that range, so a table
	 * that is a bijection across the whole of it is one for both, and
	 * stays one if either geometry changes.
	 */
	zassert_true(RADIANT_FRAME_HDR_LEN_TRACKING + RADIANT_FRAME_PAYLOAD_STD
			     <= RADIANT_FRAME_BODY_MAX,
		     "the tracking geometry no longer fits the table's range");
	zassert_true(RADIANT_FRAME_HDR_LEN_SEARCH + RADIANT_FRAME_PAYLOAD_STD
			     <= RADIANT_FRAME_BODY_MAX,
		     "the search geometry no longer fits the table's range");

	for (i = 0u; i < (size_t)RADIANT_FRAME_BODY_MAX * 8u; i++) {
		uint8_t vector[RADIANT_FRAME_BODY_MAX];

		memset(vector, 0, sizeof(vector));
		vector[0] = (uint8_t)(1u << (i % 8u));
		syndromes[n] = radiant_crc16(0u, vector, (i / 8u) + 1u);
		zassert_not_equal(0u, syndromes[n],
				  "body bit %u is invisible to the CRC", i);
		n++;
	}
	for (i = 0u; i < (size_t)RADIANT_FRAME_CRC_BYTES * 8u; i++) {
		syndromes[n] = (uint16_t)(1u << i);
		n++;
	}
	zassert_equal(RADIANT_CRC_REPAIR_ENTRIES, n, NULL);

	for (i = 0u; i < n; i++) {
		for (j = i + 1u; j < n; j++) {
			zassert_not_equal(syndromes[i], syndromes[j],
					  "entries %u and %u share syndrome "
					  "0x%04X, so a repair between them "
					  "would be a coin toss", i, j,
					  syndromes[i]);
		}
	}
}

ZTEST(radiant_crc_repair, test_an_absent_received_crc_cannot_produce_a_repair)
{
	uint8_t body[sizeof(spike_body)];
	int rc;

	/*
	 * Defence in depth for the caps.has_rx_crc gate.
	 *
	 * The gate that stops repair on a backend which keeps no received CRC
	 * lives in radiant_api.c, and this module has no capability knowledge
	 * to check for itself. So the question worth asking here is what
	 * happens if that gate is ever bypassed: a backend without the
	 * capability leaves crc_rx at zero, and zero is what would arrive.
	 *
	 * The answer must be a refusal and not a confident wrong answer. An
	 * undamaged frame against a zero CRC produces the syndrome 0x199A,
	 * which either names no single-bit error at all or names one and
	 * corrupts a frame that was already correct - and the second outcome,
	 * on EVERY CRC failure, is the shape of the bug this test exists to
	 * make impossible to introduce quietly.
	 */
	memcpy(body, spike_body, BODY_LEN);
	rc = radiant_crc_repair(RADIANT_FRAME_CFG_TRACKING, spike_addr,
				sizeof(spike_addr), body, BODY_LEN, 0u);

	zassert_equal(RADIANT_CRC_REPAIR_ENONE, rc,
		      "a zero received CRC produced rc=%d rather than a "
		      "refusal", rc);
	zassert_mem_equal(body, spike_body, BODY_LEN,
			  "an undamaged frame was modified on the strength of "
			  "a CRC the backend never supplied");
}

/* ---------------------------------------------------------------------------
 * What the HAL hands over, and what it refuses to
 * ---------------------------------------------------------------------------
 */

static uint32_t n_rx;
static bool     last_has_crc_rx;
static uint32_t last_crc_rx;

static void repair_rx_cb(const struct radiant_rx_event *evt, void *user)
{
	ARG_UNUSED(user);

	/*
	 * CRC_FAIL only, and the filter is load-bearing rather than tidy: the
	 * window's terminal TIMEOUT event arrives through this same callback
	 * and legitimately carries has_crc_rx false, so recording every event
	 * would overwrite the frame's answer with the timeout's and every test
	 * below would fail for a reason that has nothing to do with the CRC.
	 */
	if (evt->status != RADIANT_RADIO_STATUS_CRC_FAIL) {
		return;
	}
	n_rx++;
	last_has_crc_rx = evt->has_crc_rx;
	last_crc_rx = evt->crc_rx;
}

static void repair_tx_cb(const struct radiant_tx_event *evt, void *user)
{
	ARG_UNUSED(evt);
	ARG_UNUSED(user);
}

static const struct radiant_radio_cbs repair_cbs = { repair_rx_cb, repair_tx_cb };

/* Arm a tracked window that asks for CRC failures, put one damaged frame in it,
 * and return what the event carried. */
static void run_one_crc_fail(bool crc_rx_set)
{
	struct radiant_rx_filter filter;
	struct radiant_rx_req    req;
	struct fake_radio_air    air;
	uint32_t             op = 0u;
	radiant_time_t           t0;

	n_rx = 0u;
	last_has_crc_rx = false;
	last_crc_rx = 0u;

	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_init(&repair_cbs, NULL),
		      NULL);
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_enable(), NULL);
	t0 = radiant_radio_now();

	memset(&filter, 0, sizeof(filter));
	memcpy(filter.addr, spike_addr, sizeof(spike_addr));
	filter.addr_len = RADIANT_FRAME_ADDR_LEN_TRACKING;

	memset(&req, 0, sizeof(req));
	req.fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &filter;
	req.n_filters = 1u;
	req.t_open = t0 + 1000u;
	req.t_close = t0 + 500000u;
	req.flags = RADIANT_RX_REPORT_CRC_FAIL;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op), NULL);

	/* The whole on-air frame - address THEN body - because that is what the
	 * mock matches its filters against and then splits at fmt->addr_len. A
	 * body on its own matches nothing and is delivered nowhere. */
	fake_radio_air_init(&air);
	air.t_sync = t0 + 2000u;
	memcpy(air.bytes, spike_addr, sizeof(spike_addr));
	memcpy(air.bytes + sizeof(spike_addr), spike_body, BODY_LEN);
	/* One flipped bit, in the body, as the air does it. */
	air.bytes[sizeof(spike_addr)] ^= 0x01u;
	air.len = (uint8_t)(sizeof(spike_addr) + BODY_LEN);
	air.crc_bad = true;
	air.crc_rx_set = crc_rx_set;
	air.crc_rx = SPIKE_CRC;
	zassert_equal(RADIANT_RADIO_OK_RC, fake_radio_air_add(&air), NULL);

	fake_radio_advance_to(t0 + 600000u);
	zassert_true(n_rx >= 1u, "no receive event at all");
	zassert_equal(0u, fake_radio_viol_count(), "contract violation: %s",
		      fake_radio_viol_name(fake_radio_viol(0)->code));
}

ZTEST(radiant_crc_repair, test_a_backend_with_the_capability_hands_over_the_crc)
{
	fake_radio_caps_preset_nrf();
	run_one_crc_fail(true);

	zassert_true(last_has_crc_rx,
		     "the mock withheld a CRC the backend claims to keep");
	zassert_equal(SPIKE_CRC, last_crc_rx, NULL);
}

ZTEST(radiant_crc_repair, test_a_backend_without_the_capability_hands_over_nothing)
{
	/*
	 * The gate that makes the repair path inert rather than wrong on a
	 * backend that reports only a pass/fail bit. Without it the core would
	 * form a syndrome against a zero and "repair" whichever bit that
	 * happened to name - on every CRC failure, on a window that had no
	 * information in it at all.
	 */
	fake_radio_caps_preset_rail();
	zassert_false(radiant_radio_caps_get()->has_rx_crc,
		      "this test needs a backend without the capability");

	run_one_crc_fail(true);

	zassert_false(last_has_crc_rx,
		      "a backend that keeps no received CRC reported one");
	zassert_equal(0u, last_crc_rx, NULL);
}

ZTEST(radiant_crc_repair, test_an_event_that_carries_no_crc_says_so)
{
	/* The capability is there, but this particular frame was not given a
	 * CRC to report. has_crc_rx is per event and not per backend, and a
	 * core that read crc_rx without checking it would get a zero syndrome
	 * and a confident wrong answer. */
	fake_radio_caps_preset_nrf();
	run_one_crc_fail(false);

	zassert_false(last_has_crc_rx, NULL);
	zassert_equal(0u, last_crc_rx, NULL);
}
