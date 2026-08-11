/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_node_ident.c - the hostless node's counters, epoch and pairing scalar.
 *
 * Provenance: docs/decisions/0009-hostless-node-identity.md. The ADR says its
 * own assertions belong to the phase that implements it and names three: the
 * boot counter advancing across a simulated reboot, the pairing counter
 * advancing BEFORE the public key is transmitted, and the caps.has_rng seam
 * compiling out. All three are here.
 *
 * The scalar and device-number vectors come from tools/radiant_crypto.py's
 * node_pair_scalar() and node_tier0_devnum(), which share no code with
 * src/node/node_ident.c - so these assert agreement between two
 * implementations rather than self-consistency of one.
 *
 * ── Why a fake NVM rather than the real backend ────────────────────────────
 *
 * The bench board's test application has no storage partition, and the two
 * properties this file has to prove - a counter that survives a power cycle,
 * and a write that fails - are properties of node_nvm.h, not of NVS. So
 * radiant_core/tests/fake_nvm.c is the storage backend for this image exactly
 * as fake_radio.c is its radio, and node_ident.c is compiled unmodified.
 */

#include <zephyr/ztest.h>

#include <string.h>

#include <radiant_core/radiant_sec.h>

#include "fake_nvm.h"
#include "node_ident.h"
#include "node_nvm.h"

/* K_dev = 00 01 02 ... 0f, the vector key in tools/test_radiant_crypto.py. */
static const uint8_t k_dev[NODE_IDENT_K_DEV_BYTES] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
};

/* tools/radiant_crypto.py: node_tier0_devnum(bytes(range(16))). */
#define VECTOR_DEVNUM 51235u

/* node_pair_scalar(k_dev, 1, 51235). */
static const uint8_t scalar_ctr1[NODE_IDENT_PAIR_SCALAR_BYTES] = {
	0x7F, 0x80, 0xF8, 0x17, 0x77, 0x6A, 0xC5, 0x26,
	0x4D, 0x9F, 0x45, 0xDA, 0x00, 0x9C, 0xE2, 0xFC,
	0x81, 0x65, 0xD1, 0xDC, 0xE8, 0x31, 0x47, 0x78,
	0x32, 0x25, 0xE0, 0x5E, 0xDC, 0xA6, 0xEF, 0x10,
};

/* node_pair_scalar(k_dev, 2, 51235). */
static const uint8_t scalar_ctr2[NODE_IDENT_PAIR_SCALAR_BYTES] = {
	0xD4, 0xBD, 0x67, 0xBC, 0xFE, 0xB0, 0x44, 0x1E,
	0x47, 0x85, 0x98, 0x3D, 0x30, 0x88, 0x36, 0x19,
	0x36, 0x41, 0x22, 0xA1, 0xA9, 0x42, 0x47, 0x7B,
	0x8F, 0x52, 0xA9, 0x0E, 0xAC, 0x9A, 0x06, 0x42,
};

/* node_pair_scalar(k_dev, 7, 51235). */
static const uint8_t scalar_ctr7[NODE_IDENT_PAIR_SCALAR_BYTES] = {
	0xDC, 0x7B, 0x9E, 0x17, 0xF3, 0x34, 0x14, 0x5F,
	0x42, 0x87, 0xAA, 0x80, 0x5A, 0x4B, 0xE0, 0x2B,
	0x49, 0xA8, 0x19, 0x49, 0x3D, 0xFA, 0x29, 0xF1,
	0xF4, 0x08, 0x5A, 0x66, 0xD1, 0xB5, 0xC4, 0x38,
};

#define TEST_CH     0u
#define TEST_PERIOD 8182u    /* the ANT+ 4 Hz period, in 1/32768 s */

static const uint8_t test_root[16] = {
	0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
	0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
};

static void node_before(void *f)
{
	ARG_UNUSED(f);
	fake_nvm_wipe();
	node_ident_reset();
	radiant_sec_reset();
}

/* A factory-provisioned, powered-up node. Most tests start here. */
static void provisioned_and_booted(void)
{
	zassert_equal(NODE_IDENT_OK, node_ident_provision(k_dev));
	zassert_equal(NODE_IDENT_OK, node_ident_boot());
}

ZTEST_SUITE(node_ident, NULL, NULL, node_before, NULL, NULL);

/* ── Provisioning ─────────────────────────────────────────────────────────── */

ZTEST(node_ident, test_unprovisioned_node_cannot_boot)
{
	/* No K_dev: no identity, no pairing path, and - the part that matters -
	 * no epoch, so nothing can enable a transform. ADR 0009: a device
	 * shipped without K_dev has neither. */
	zassert_false(node_ident_is_provisioned());
	zassert_equal(NODE_IDENT_ENOKEY, node_ident_boot());

	uint32_t epoch;

	zassert_equal(NODE_IDENT_ESTATE, node_ident_epoch(&epoch));
	zassert_equal(NODE_IDENT_ESTATE, node_ident_arm_sec(TEST_CH,
							    TEST_PERIOD));
}

ZTEST(node_ident, test_an_all_zero_key_is_refused)
{
	uint8_t zeros[NODE_IDENT_K_DEV_BYTES] = { 0 };

	/* The one wrong value that would otherwise look provisioned: a buffer
	 * that never got filled. Its scalar and its device number are
	 * computable by anybody. */
	zassert_equal(NODE_IDENT_EINVAL, node_ident_provision(zeros));
	zassert_false(node_ident_is_provisioned());
}

ZTEST(node_ident, test_provisioning_zeroes_both_counters_with_the_key)
{
	uint32_t v;

	provisioned_and_booted();
	zassert_equal(NODE_IDENT_OK, node_ident_pair_window_open(NULL));

	zassert_true(fake_nvm_peek_u32(NODE_NVM_KEY_BOOT, &v));
	zassert_equal(1u, v);
	zassert_true(fake_nvm_peek_u32(NODE_NVM_KEY_PAIR, &v));
	zassert_equal(1u, v);

	/* A factory reset is a NEW key, and the counters go with it - never
	 * independently, because a reset counter under an unchanged key is a
	 * repeated scalar and a replayed epoch at once. */
	uint8_t other[NODE_IDENT_K_DEV_BYTES];

	memset(other, 0xA5, sizeof(other));
	zassert_equal(NODE_IDENT_OK, node_ident_provision(other));
	zassert_true(fake_nvm_peek_u32(NODE_NVM_KEY_BOOT, &v));
	zassert_equal(0u, v);
	zassert_true(fake_nvm_peek_u32(NODE_NVM_KEY_PAIR, &v));
	zassert_equal(0u, v);

	/* And the identity moved with it. */
	uint16_t devnum;

	zassert_equal(NODE_IDENT_OK, node_ident_boot());
	zassert_equal(NODE_IDENT_OK, node_ident_devnum(&devnum));
	zassert_not_equal(VECTOR_DEVNUM, devnum);
}

/* ── The boot counter ─────────────────────────────────────────────────────── */

ZTEST(node_ident, test_boot_counter_survives_a_power_cycle)
{
	uint32_t counter;

	provisioned_and_booted();
	zassert_equal(NODE_IDENT_OK, node_ident_boot_counter(&counter));
	zassert_equal(1u, counter);

	/*
	 * The assertion ADR 0009 asks for by name. fake_nvm_reboot() keeps the
	 * contents and forgets the init flag, so this is the same code path a
	 * cold start takes - a counter that only incremented in RAM would read
	 * 1 again here, and that node would reuse its epoch.
	 */
	for (uint32_t expected = 2u; expected <= 5u; expected++) {
		fake_nvm_reboot();
		node_ident_reset();
		zassert_equal(NODE_IDENT_OK, node_ident_boot());
		zassert_equal(NODE_IDENT_OK, node_ident_boot_counter(&counter));
		zassert_equal(expected, counter,
			      "boot counter did not survive the power cycle");
	}
}

ZTEST(node_ident, test_a_failed_write_means_the_node_did_not_boot)
{
	uint32_t v;

	zassert_equal(NODE_IDENT_OK, node_ident_provision(k_dev));

	/* "If the boot counter cannot be advanced and persisted, no transform
	 * enables." The 0xF3 refusal, relocated to the node. */
	fake_nvm_fail_stores(1u);
	zassert_equal(NODE_IDENT_EIO, node_ident_boot());

	zassert_equal(NODE_IDENT_ESTATE, node_ident_epoch(&v));
	zassert_equal(NODE_IDENT_ESTATE, node_ident_arm_sec(TEST_CH,
							    TEST_PERIOD));
	zassert_equal(NODE_IDENT_ESTATE, node_ident_pair_window_open(NULL));

	/* And the stored counter did not move, so the next attempt is not
	 * skipping a value it never used. */
	zassert_true(fake_nvm_peek_u32(NODE_NVM_KEY_BOOT, &v));
	zassert_equal(0u, v);

	/* The failure was transient; the node boots normally afterwards. */
	zassert_equal(NODE_IDENT_OK, node_ident_boot());
	zassert_equal(NODE_IDENT_OK, node_ident_boot_counter(&v));
	zassert_equal(1u, v);
}

ZTEST(node_ident, test_a_store_that_does_not_stick_is_caught_at_the_write)
{
	uint32_t v;

	zassert_equal(NODE_IDENT_OK, node_ident_provision(k_dev));

	/* The backend reports success and writes nothing - a queued write, a
	 * cache flushed later, a driver that lies. node_ident.c re-reads every
	 * counter it stores, so this cannot become a live epoch. */
	fake_nvm_swallow_stores(true);
	zassert_equal(NODE_IDENT_EIO, node_ident_boot());
	zassert_equal(NODE_IDENT_ESTATE, node_ident_epoch(&v));
}

ZTEST(node_ident, test_one_write_per_boot_and_one_per_pairing_window)
{
	/* ADR 0009's flash-wear argument is a write frequency - "once per boot,
	 * once per pairing window" - and it is the whole reason this design was
	 * acceptable where the per-epoch ratchet was not. So it is counted. */
	provisioned_and_booted();

	uint32_t base = fake_nvm_store_count();

	fake_nvm_reboot();
	node_ident_reset();
	zassert_equal(NODE_IDENT_OK, node_ident_boot());
	zassert_equal(base + 1u, fake_nvm_store_count(),
		      "a boot cost more than one write");

	base = fake_nvm_store_count();
	zassert_equal(NODE_IDENT_OK, node_ident_pair_window_open(NULL));
	zassert_equal(base + 1u, fake_nvm_store_count(),
		      "a pairing window cost more than one write");

	/* Deriving and closing cost nothing. */
	uint8_t scalar[NODE_IDENT_PAIR_SCALAR_BYTES];

	base = fake_nvm_store_count();
	zassert_equal(NODE_IDENT_OK, node_ident_pair_scalar(scalar));
	node_ident_pair_window_close();
	zassert_equal(base, fake_nvm_store_count());
}

/* ── The epoch ────────────────────────────────────────────────────────────── */

ZTEST(node_ident, test_epoch_is_the_boot_counter_when_there_is_no_clock)
{
	uint32_t epoch;
	uint32_t counter;

	provisioned_and_booted();
	zassert_equal(NODE_IDENT_OK, node_ident_epoch(&epoch));
	zassert_equal(NODE_IDENT_OK, node_ident_boot_counter(&counter));
	zassert_equal(counter, epoch);
	zassert_equal(1u, epoch);

	fake_nvm_reboot();
	node_ident_reset();
	zassert_equal(NODE_IDENT_OK, node_ident_boot());
	zassert_equal(NODE_IDENT_OK, node_ident_epoch(&epoch));
	zassert_equal(2u, epoch);
}

ZTEST(node_ident, test_epoch_from_a_clock_is_coarse_real_time)
{
	/* One hour after the base date is 60 minutes. */
	zassert_equal(60u, node_ident_epoch_from(3u, true,
						 NODE_IDENT_EPOCH_BASE_S +
						 3600ull));

	/* An invalid clock falls back to the boot counter, which is the honest
	 * default on every target this project currently builds for: neither
	 * nRF5340 nor nRF54L15 keeps real time across a power cycle. */
	zassert_equal(3u, node_ident_epoch_from(3u, false, 0ull));

	/* A clock reading before the base date is not a clock. */
	zassert_equal(3u, node_ident_epoch_from(3u, true, 1000ull));

	/* The boot counter is a floor: a clock that comes back wrong cannot
	 * hand radiant_core an epoch this node has already used. */
	zassert_equal(9000u,
		      node_ident_epoch_from(9000u, true,
					    NODE_IDENT_EPOCH_BASE_S + 60ull));
}

ZTEST(node_ident, test_the_node_is_its_own_epoch_authority)
{
	uint32_t epoch;

	provisioned_and_booted();

	/* No host, no 0xF3. The node derives its epoch and hands it to
	 * radiant_core through the SAME entry point a host would use - ADR 0009
	 * decision 7: radiant_core still only ever receives an epoch through
	 * its existing API. */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_key(TEST_CH, test_root, 128,
					  VECTOR_DEVNUM));
	zassert_equal(NODE_IDENT_OK, node_ident_arm_sec(TEST_CH, TEST_PERIOD));
	zassert_equal(NODE_IDENT_OK, node_ident_epoch(&epoch));
	zassert_equal(1u, epoch);

	/* The next power-up moves it forward, and radiant_core accepts it. */
	fake_nvm_reboot();
	node_ident_reset();
	zassert_equal(NODE_IDENT_OK, node_ident_boot());
	zassert_equal(NODE_IDENT_OK, node_ident_arm_sec(TEST_CH, TEST_PERIOD));
}

ZTEST(node_ident, test_core_still_refuses_an_epoch_that_went_backwards)
{
	/* The node owns the epoch now, and radiant_core's monotonicity check is
	 * NOT thereby redundant: a re-provisioned node genuinely restarts its
	 * counter at 1, and on a channel that has already seen a higher epoch
	 * that must be refused rather than followed. D3's enforceable half
	 * survives the change of authority. */
	provisioned_and_booted();
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_key(TEST_CH, test_root, 128,
					  VECTOR_DEVNUM));

	for (int i = 0; i < 3; i++) {
		fake_nvm_reboot();
		node_ident_reset();
		zassert_equal(NODE_IDENT_OK, node_ident_boot());
		zassert_equal(NODE_IDENT_OK,
			      node_ident_arm_sec(TEST_CH, TEST_PERIOD));
	}

	/* A factory reset takes the counter back to 1 under a new key. */
	uint8_t other[NODE_IDENT_K_DEV_BYTES];

	memset(other, 0x5A, sizeof(other));
	zassert_equal(NODE_IDENT_OK, node_ident_provision(other));
	zassert_equal(NODE_IDENT_OK, node_ident_boot());

	uint32_t epoch;

	zassert_equal(NODE_IDENT_OK, node_ident_epoch(&epoch));
	zassert_equal(1u, epoch);
	zassert_equal(NODE_IDENT_EIO, node_ident_arm_sec(TEST_CH, TEST_PERIOD),
		      "radiant_core accepted an epoch that moved backwards");
}

/* ── Tier 0 identity ──────────────────────────────────────────────────────── */

ZTEST(node_ident, test_tier0_device_number_is_derived_from_k_dev)
{
	uint16_t devnum;

	provisioned_and_booted();
	zassert_equal(NODE_IDENT_OK, node_ident_devnum(&devnum));
	zassert_equal(VECTOR_DEVNUM, devnum,
		      "the Tier 0 device number disagrees with "
		      "tools/radiant_crypto.py");

	/* Derived rather than stored, so a power cycle cannot lose it. */
	fake_nvm_reboot();
	node_ident_reset();
	zassert_equal(NODE_IDENT_OK, node_ident_boot());
	zassert_equal(NODE_IDENT_OK, node_ident_devnum(&devnum));
	zassert_equal(VECTOR_DEVNUM, devnum);
}

ZTEST(node_ident, test_device_number_is_never_the_wildcard)
{
	uint8_t  key[NODE_IDENT_K_DEV_BYTES];
	uint16_t devnum;

	/* 0 is the ANT wildcard and a node claiming it is a node no receiver
	 * can address. */
	for (unsigned int i = 1u; i < 64u; i++) {
		memset(key, (int)i, sizeof(key));
		fake_nvm_wipe();
		node_ident_reset();
		zassert_equal(NODE_IDENT_OK, node_ident_provision(key));
		zassert_equal(NODE_IDENT_OK, node_ident_boot());
		zassert_equal(NODE_IDENT_OK, node_ident_devnum(&devnum));
		zassert_not_equal(0u, devnum);
	}
}

/* ── The pairing scalar and its counter ───────────────────────────────────── */

ZTEST(node_ident, test_pair_scalar_matches_the_python_vectors)
{
	uint8_t  scalar[NODE_IDENT_PAIR_SCALAR_BYTES];
	uint32_t ctr;

	provisioned_and_booted();

	zassert_equal(NODE_IDENT_OK, node_ident_pair_window_open(&ctr));
	zassert_equal(1u, ctr);
	zassert_equal(NODE_IDENT_OK, node_ident_pair_scalar(scalar));
	zassert_mem_equal(scalar_ctr1, scalar, sizeof(scalar),
			  "scalar for pair_counter 1 disagrees with "
			  "tools/radiant_crypto.py");
	node_ident_pair_window_close();

	zassert_equal(NODE_IDENT_OK, node_ident_pair_window_open(&ctr));
	zassert_equal(2u, ctr);
	zassert_equal(NODE_IDENT_OK, node_ident_pair_scalar(scalar));
	zassert_mem_equal(scalar_ctr2, scalar, sizeof(scalar));
	node_ident_pair_window_close();

	/* And the pure derivation, at a counter this node has not reached. */
	zassert_equal(NODE_IDENT_OK,
		      node_ident_pair_derive(k_dev, 7u, VECTOR_DEVNUM, scalar));
	zassert_mem_equal(scalar_ctr7, scalar, sizeof(scalar));
}

ZTEST(node_ident, test_the_scalar_is_deterministic_and_the_counter_is_not)
{
	uint8_t a[NODE_IDENT_PAIR_SCALAR_BYTES];
	uint8_t b[NODE_IDENT_PAIR_SCALAR_BYTES];

	/* Deterministic in the counter - which is exactly why the counter may
	 * never repeat, and is the whole of the reasoning below. */
	zassert_equal(NODE_IDENT_OK,
		      node_ident_pair_derive(k_dev, 4u, 1u, a));
	zassert_equal(NODE_IDENT_OK,
		      node_ident_pair_derive(k_dev, 4u, 1u, b));
	zassert_mem_equal(a, b, sizeof(a));

	zassert_equal(NODE_IDENT_OK,
		      node_ident_pair_derive(k_dev, 5u, 1u, b));
	zassert_true(memcmp(a, b, sizeof(a)) != 0,
		     "two pair_counters produced the same private key");

	/* The two halves are not the same block repeated, which is what
	 * deriving 32 bytes from a 128-bit KDF would give. */
	zassert_true(memcmp(a, a + 16, 16) != 0);
}

ZTEST(node_ident, test_the_counter_is_in_nvm_before_any_scalar_exists)
{
	uint32_t stored = 0u;
	uint8_t  scalar[NODE_IDENT_PAIR_SCALAR_BYTES];

	provisioned_and_booted();

	zassert_true(fake_nvm_peek_u32(NODE_NVM_KEY_PAIR, &stored));
	zassert_equal(0u, stored);

	/* Nothing to transmit before a window is open. */
	zassert_equal(NODE_IDENT_ESTATE, node_ident_pair_scalar(scalar));

	zassert_equal(NODE_IDENT_OK, node_ident_pair_window_open(NULL));

	/*
	 * THE ASSERTION ADR 0009 ASKS FOR. Read straight out of the simulated
	 * part, bypassing the layer under test, at the moment before any scalar
	 * has been derived - so the public key that will be computed from it
	 * cannot yet exist, and the counter has already moved.
	 */
	zassert_true(fake_nvm_peek_u32(NODE_NVM_KEY_PAIR, &stored));
	zassert_equal(1u, stored,
		      "pair_counter had not been persisted before the scalar "
		      "was available");

	zassert_equal(NODE_IDENT_OK, node_ident_pair_scalar(scalar));
}

ZTEST(node_ident, test_a_window_that_cannot_persist_is_not_entered)
{
	uint32_t stored = 0u;
	uint8_t  scalar[NODE_IDENT_PAIR_SCALAR_BYTES];

	provisioned_and_booted();

	/* "If the write fails, the node does not enter the pairing window.
	 * Fail closed; a node that cannot advance its counter has nothing safe
	 * to say." */
	fake_nvm_fail_stores(1u);
	zassert_equal(NODE_IDENT_EIO, node_ident_pair_window_open(NULL));
	zassert_false(node_ident_pair_window_is_open());
	zassert_equal(NODE_IDENT_ESTATE, node_ident_pair_scalar(scalar));

	zassert_true(fake_nvm_peek_u32(NODE_NVM_KEY_PAIR, &stored));
	zassert_equal(0u, stored);
}

ZTEST(node_ident, test_the_scalar_gate_refuses_a_counter_that_moved_elsewhere)
{
	uint8_t  scalar[NODE_IDENT_PAIR_SCALAR_BYTES];
	uint32_t other = 99u;

	provisioned_and_booted();
	zassert_equal(NODE_IDENT_OK, node_ident_pair_window_open(NULL));

	/* Something else moved the stored counter out from under the live
	 * window. Deriving under the stale value would be the repeat the whole
	 * rule exists to prevent, so the gate refuses rather than guessing. */
	zassert_equal(NODE_NVM_OK,
		      node_nvm_store(NODE_NVM_KEY_PAIR, &other,
				     sizeof(other)));
	zassert_equal(NODE_IDENT_EIO, node_ident_pair_scalar(scalar));
}

ZTEST(node_ident, test_an_abandoned_window_burns_its_counter)
{
	uint32_t ctr;
	uint8_t  first[NODE_IDENT_PAIR_SCALAR_BYTES];
	uint8_t  second[NODE_IDENT_PAIR_SCALAR_BYTES];

	provisioned_and_booted();

	/* The common case, not an edge case: the user walks away, the peer
	 * never answers, the window times out. An advance-on-completion
	 * implementation would reuse the scalar here, which is why this is the
	 * test that would catch that mistake. */
	zassert_equal(NODE_IDENT_OK, node_ident_pair_window_open(&ctr));
	zassert_equal(1u, ctr);
	zassert_equal(NODE_IDENT_OK, node_ident_pair_scalar(first));
	node_ident_pair_window_close();

	zassert_equal(NODE_IDENT_OK, node_ident_pair_window_open(&ctr));
	zassert_equal(2u, ctr, "an abandoned window reused its counter");
	zassert_equal(NODE_IDENT_OK, node_ident_pair_scalar(second));
	zassert_true(memcmp(first, second, sizeof(first)) != 0,
		     "a second pairing window produced the same private key, "
		     "which is a stable cross-session identifier on the air");
}

ZTEST(node_ident, test_the_pair_counter_survives_a_power_cycle_too)
{
	uint32_t ctr;

	provisioned_and_booted();
	zassert_equal(NODE_IDENT_OK, node_ident_pair_window_open(&ctr));
	zassert_equal(1u, ctr);
	node_ident_pair_window_close();

	/* A battery pulled mid-enrolment must not hand the next window the
	 * same private key. */
	fake_nvm_reboot();
	node_ident_reset();
	zassert_equal(NODE_IDENT_OK, node_ident_boot());
	zassert_equal(NODE_IDENT_OK, node_ident_pair_window_open(&ctr));
	zassert_equal(2u, ctr);
}

/* ── The precedence rule and the entropy seam ─────────────────────────────── */

ZTEST(node_ident, test_scalar_source_precedence)
{
	/* ADR 0009: host-supplied if present, else the RNG when caps.has_rng,
	 * else the KDF. Stated once, in one function, so it cannot be
	 * rediscovered differently in a second place. */
	zassert_equal(NODE_IDENT_SCALAR_HOST,
		      node_ident_scalar_src(true, true));
	zassert_equal(NODE_IDENT_SCALAR_HOST,
		      node_ident_scalar_src(true, false));
	zassert_equal(NODE_IDENT_SCALAR_RNG,
		      node_ident_scalar_src(false, true));
	zassert_equal(NODE_IDENT_SCALAR_KDF,
		      node_ident_scalar_src(false, false));
}

ZTEST(node_ident, test_the_rng_seam_compiles_out_and_refuses)
{
	uint8_t buf[8];

	/* No entropy backend is selected in this build, which is the DEFAULT
	 * configuration and not a degraded one - the deterministic scalar above
	 * is what makes it supportable. */
	zassert_false(radiant_sec_caps()->has_rng);

	memset(buf, 0xC7, sizeof(buf));
	zassert_equal(RADIANT_SEC_ENOTSUP, radiant_sec_rng(buf, sizeof(buf)));

	/* And it left the buffer alone rather than zeroing it, so a caller that
	 * ignored both the cap and the return code cannot mistake a refusal for
	 * eight bytes of entropy. */
	for (size_t i = 0u; i < sizeof(buf); i++) {
		zassert_equal(0xC7u, buf[i]);
	}
}

#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)

ZTEST(node_ident, test_pair_begin_persists_the_counter_before_the_pubkey)
{
	uint32_t stored = 0u;
	uint8_t  pub[NODE_IDENT_PAIR_SCALAR_BYTES];

	provisioned_and_booted();

	zassert_equal(NODE_IDENT_OK, node_ident_pair_begin(TEST_CH, 60u));

	/* By the time a public key can be read at all, the counter that
	 * produced it is durable. */
	zassert_true(fake_nvm_peek_u32(NODE_NVM_KEY_PAIR, &stored));
	zassert_equal(1u, stored);
	zassert_equal(NODE_IDENT_OK, node_ident_pair_pubkey(TEST_CH, pub));

	/* A second window is a different public key on the air. */
	uint8_t pub2[NODE_IDENT_PAIR_SCALAR_BYTES];

	radiant_sec_pair_leave(TEST_CH);
	node_ident_pair_window_close();
	zassert_equal(NODE_IDENT_OK, node_ident_pair_begin(TEST_CH, 60u));
	zassert_equal(NODE_IDENT_OK, node_ident_pair_pubkey(TEST_CH, pub2));
	zassert_true(memcmp(pub, pub2, sizeof(pub)) != 0);
}

ZTEST(node_ident, test_pair_begin_fails_closed_on_a_write_failure)
{
	uint8_t pub[NODE_IDENT_PAIR_SCALAR_BYTES];

	provisioned_and_booted();

	fake_nvm_fail_stores(1u);
	zassert_equal(NODE_IDENT_EIO, node_ident_pair_begin(TEST_CH, 60u));

	/* No window, no scalar installed, and therefore no public key to put on
	 * the air. */
	zassert_false(node_ident_pair_window_is_open());
	zassert_equal(NODE_IDENT_ESTATE, node_ident_pair_pubkey(TEST_CH, pub));
}

#endif /* CONFIG_RADIANT_SEC_PAIRING_X25519 */
