/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_node_ident.c - the hostless node's counters, epoch and pairing scalar.
 *
 * Provenance: docs/decisions/0009-hostless-node-identity.md, which names three
 * required assertions: the boot counter advancing across a simulated reboot,
 * the pairing counter advancing BEFORE the public key is transmitted, and the
 * caps.has_rng seam compiling out. All three are here.
 *
 * Scalar and device-number vectors come from tools/radiant_crypto.py's
 * node_pair_scalar() and node_tier0_devnum(), which share no code with
 * src/node/node_ident.c, so these assert agreement between two independent
 * implementations.
 *
 * A fake NVM stands in for the real backend: the bench board's test
 * application has no storage partition, and the two properties this file has
 * to prove - a counter that survives a power cycle, and a write that fails -
 * are properties of node_nvm.h, not of NVS. radiant/tests/fake_nvm.c is
 * the storage backend here, exactly as fake_radio.c is the radio, and
 * node_ident.c is compiled unmodified.
 */

#include <zephyr/ztest.h>

#include <string.h>

#include <radiant/radiant_channel.h>
#include <radiant/radiant_radio_hal.h>
#include <radiant/radiant_sec.h>
#include <radiant/radiant_sec_compat.h>

#include "fake_nvm.h"
#include "node_ident.h"
#include "node_nvm.h"

#if defined(CONFIG_RADIANT_TEST_FAKE_RNG)
#include "fake_rng.h"
#endif

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

/* The node's clock, for the same-minute reboot case. Strong, displacing
 * node_ident.c's weak "no clock"; answers "no clock" unless a test sets it. */
static bool     wall_valid;
static uint64_t wall_s;

bool node_ident_wall_seconds(uint64_t *out)
{
	*out = wall_s;
	return wall_valid;
}

static void node_before(void *f)
{
	ARG_UNUSED(f);
	fake_nvm_wipe();
#if defined(CONFIG_RADIANT_TEST_FAKE_RNG)
	fake_rng_reset();
#endif
	wall_valid = false;
	wall_s = 0u;
	node_ident_reset();
	radiant_sec_reset();
	radiant_sec_compat_reset();
}

/* And after: node_ident.c owns radiant_sec_epoch_advanced() in this image, so
 * a booted identity with an armed channel left behind would follow - and put
 * ceilings on - the next suite's channels. node_ident_reset() never fails, so
 * this after() cannot skip anything behind it. */
static void node_after(void *f)
{
	ARG_UNUSED(f);
	node_ident_reset();
	radiant_sec_reset();
	radiant_sec_compat_reset();
}

/* A factory-provisioned, powered-up node. Most tests start here. */
static void provisioned_and_booted(void)
{
	zassert_equal(NODE_IDENT_OK, node_ident_provision(k_dev));
	zassert_equal(NODE_IDENT_OK, node_ident_boot());
}

ZTEST_SUITE(node_ident, NULL, NULL, node_before, node_after, NULL);

/* ── Provisioning ─────────────────────────────────────────────────────────── */

#if !defined(CONFIG_RADIANT_NODE_IDENT_SELF_PROVISION)
/* Self-provisioning off (no entropy backend, or a fixture that opts out):
 * the behaviour before ADR 0020, unchanged. */
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

	/* And a boot with nothing to boot from wrote nothing. */
	zassert_equal(0u, fake_nvm_store_count());
	zassert_false(node_ident_is_provisioned());
}
#endif /* !CONFIG_RADIANT_NODE_IDENT_SELF_PROVISION */

#if defined(CONFIG_RADIANT_NODE_IDENT_SELF_PROVISION)
/* ── First-boot provisioning (ADR 0020) ───────────────────────────────────── */

ZTEST(node_ident, test_first_boot_generates_and_stores_k_dev)
{
	uint8_t  expect[NODE_IDENT_K_DEV_BYTES];
	uint8_t  stored[NODE_IDENT_K_DEV_BYTES];
	uint32_t v;
	uint16_t devnum;

	/* A factory-fresh part: no K_dev record at all. */
	zassert_false(node_ident_is_provisioned());
	zassert_equal(NODE_IDENT_OK, node_ident_boot());

	/* One draw, and the stored key is that draw - not a derivation of it
	 * and not a second draw. */
	zassert_equal(1u, fake_rng_draw_count());
	fake_rng_expected(0u, expect, sizeof(expect));
	zassert_true(fake_nvm_peek(NODE_NVM_KEY_KDEV, stored, sizeof(stored)));
	zassert_mem_equal(expect, stored, sizeof(stored));
	zassert_true(node_ident_is_provisioned());

	/* Provisioning zeroed the counters with the key, then this boot
	 * advanced the boot counter once, exactly as a factory-provisioned
	 * node's first boot does. */
	zassert_true(fake_nvm_peek_u32(NODE_NVM_KEY_BOOT, &v));
	zassert_equal(1u, v);
	zassert_true(fake_nvm_peek_u32(NODE_NVM_KEY_PAIR, &v));
	zassert_equal(0u, v);
	zassert_equal(NODE_IDENT_OK, node_ident_epoch(&v));
	zassert_equal(1u, v);

	/* And a Tier 0 device number came out of it. */
	zassert_equal(NODE_IDENT_OK, node_ident_devnum(&devnum));
	zassert_not_equal(0u, devnum);
}

ZTEST(node_ident, test_the_generated_identity_survives_a_power_cycle)
{
	uint8_t  first[NODE_IDENT_K_DEV_BYTES];
	uint8_t  after[NODE_IDENT_K_DEV_BYTES];
	uint16_t d1;
	uint16_t d2;
	uint32_t counter;

	zassert_equal(NODE_IDENT_OK, node_ident_boot());
	zassert_equal(NODE_IDENT_OK, node_ident_devnum(&d1));
	zassert_true(fake_nvm_peek(NODE_NVM_KEY_KDEV, first, sizeof(first)));

	/* The second and later boots find the record and draw nothing: a
	 * redraw here would be a new device number on every power-up. */
	for (uint32_t expected = 2u; expected <= 4u; expected++) {
		fake_nvm_reboot();
		node_ident_reset();
		zassert_equal(NODE_IDENT_OK, node_ident_boot());
		zassert_equal(1u, fake_rng_draw_count());
		zassert_equal(NODE_IDENT_OK, node_ident_devnum(&d2));
		zassert_equal(d1, d2);
		zassert_equal(NODE_IDENT_OK, node_ident_boot_counter(&counter));
		zassert_equal(expected, counter);
	}
	zassert_true(fake_nvm_peek(NODE_NVM_KEY_KDEV, after, sizeof(after)));
	zassert_mem_equal(first, after, sizeof(first));
}

ZTEST(node_ident, test_a_factory_key_is_never_replaced)
{
	uint16_t devnum;

	/* A product that writes K_dev at the factory keeps it: the record
	 * exists, so there is no first boot to provision. */
	zassert_equal(NODE_IDENT_OK, node_ident_provision(k_dev));
	zassert_equal(NODE_IDENT_OK, node_ident_boot());
	zassert_equal(0u, fake_rng_draw_count());
	zassert_equal(NODE_IDENT_OK, node_ident_devnum(&devnum));
	zassert_equal(VECTOR_DEVNUM, devnum);
}

ZTEST(node_ident, test_a_k_dev_read_error_is_eio_and_never_reprovisions)
{
	uint8_t  stored[NODE_IDENT_K_DEV_BYTES];
	uint16_t devnum;
	uint32_t stores;

	zassert_equal(NODE_IDENT_OK, node_ident_provision(k_dev));
	fake_nvm_reboot();
	node_ident_reset();

	/* THE TRIGGER RULE. A read that fails is not a record that is absent,
	 * and treating it as one would re-roll the identity on a flaky flash
	 * read - a new device number, every paired receiver orphaned, and no
	 * error anywhere. */
	stores = fake_nvm_store_count();
	fake_nvm_fail_loads(1u);
	zassert_equal(NODE_IDENT_EIO, node_ident_boot());
	zassert_equal(0u, fake_rng_draw_count());
	zassert_equal(stores, fake_nvm_store_count());
	zassert_true(fake_nvm_peek(NODE_NVM_KEY_KDEV, stored, sizeof(stored)));
	zassert_mem_equal(k_dev, stored, sizeof(stored));

	/* Nothing booted, so nothing can enable a transform. */
	zassert_equal(NODE_IDENT_ESTATE, node_ident_devnum(&devnum));

	/* The error was transient: the same identity comes back. */
	zassert_equal(NODE_IDENT_OK, node_ident_boot());
	zassert_equal(NODE_IDENT_OK, node_ident_devnum(&devnum));
	zassert_equal(VECTOR_DEVNUM, devnum);
}

ZTEST(node_ident, test_an_rng_failure_writes_nothing)
{
	uint32_t v;

	fake_rng_fail_next(1u);
	zassert_equal(NODE_IDENT_ENOKEY, node_ident_boot());
	zassert_equal(1u, fake_rng_draw_count());
	zassert_equal(0u, fake_nvm_store_count());
	zassert_false(node_ident_is_provisioned());
	zassert_equal(NODE_IDENT_ESTATE, node_ident_epoch(&v));

	/* The next boot with working entropy provisions normally. */
	fake_nvm_reboot();
	node_ident_reset();
	zassert_equal(NODE_IDENT_OK, node_ident_boot());
	zassert_true(node_ident_is_provisioned());
}

ZTEST(node_ident, test_an_rng_that_returns_zeros_is_not_a_key)
{
	/* A backend that reports success with sixteen zero bytes. The
	 * all-zero refusal in node_ident_provision() is what stops it - and it
	 * is reported as the entropy failure it is, not as a bad argument. */
	fake_rng_return_zeros(true);
	zassert_equal(NODE_IDENT_ENOKEY, node_ident_boot());
	zassert_equal(1u, fake_rng_draw_count());
	zassert_equal(0u, fake_nvm_store_count());
	zassert_false(node_ident_is_provisioned());
}

/* Release review 2026-11-08, item 7. 5d00..00 derives #37976, an eight-bit
 * alternating run in its on-air address for the ANT+ types; the reviewer's
 * example. The number is permanent, so the first draw is checked before it is
 * stored. */
static const uint8_t hard_key[NODE_IDENT_K_DEV_BYTES] = { 0x5Du };

ZTEST(node_ident, test_a_hard_to_receive_draw_is_redrawn_and_the_good_key_persisted)
{
	uint8_t  good[NODE_IDENT_K_DEV_BYTES];
	uint8_t  stored[NODE_IDENT_K_DEV_BYTES];
	uint16_t devnum;

	fake_rng_force_next(hard_key, sizeof(hard_key));
	/* The second draw is the generator's own draw 1... which is itself a bad
	 * number (#8540, run 8), so the key persisted is the third draw. */
	fake_rng_expected(2u, good, sizeof(good));

	zassert_equal(NODE_IDENT_OK, node_ident_boot());
	zassert_equal(3u, fake_rng_draw_count());
	zassert_true(fake_nvm_peek(NODE_NVM_KEY_KDEV, stored, sizeof(stored)));
	zassert_mem_equal(good, stored, sizeof(stored));
	zassert_equal(NODE_IDENT_OK, node_ident_devnum(&devnum));
	zassert_not_equal(37976u, devnum);
	zassert_equal(13433u, devnum);
}

ZTEST(node_ident, test_an_all_zero_redraw_is_still_an_rng_failure)
{
	static const uint8_t zero_key[NODE_IDENT_K_DEV_BYTES];

	/* The zero rule is not first-draw-only: a hard-to-receive draw followed
	 * by a backend that now returns zeros must fail as an RNG failure, not be
	 * redrawn again or stored. */
	fake_rng_force_next(hard_key, sizeof(hard_key));
	fake_rng_force_next(zero_key, sizeof(zero_key));
	zassert_equal(NODE_IDENT_ENOKEY, node_ident_boot());
	zassert_equal(2u, fake_rng_draw_count());
	zassert_equal(0u, fake_nvm_store_count());
	zassert_false(node_ident_is_provisioned());
}

ZTEST(node_ident, test_a_good_first_draw_is_not_redrawn)
{
	zassert_equal(NODE_IDENT_OK, node_ident_boot());
	zassert_equal(1u, fake_rng_draw_count());
}

ZTEST(node_ident, test_sixteen_bad_draws_keep_the_last_rather_than_fail_the_boot)
{
	uint8_t  stored[NODE_IDENT_K_DEV_BYTES];
	uint16_t devnum;

	for (uint32_t i = 0u; i < 20u; i++) {
		fake_rng_force_next(hard_key, sizeof(hard_key));
	}
	zassert_equal(NODE_IDENT_OK, node_ident_boot());
	zassert_equal(16u, fake_rng_draw_count());
	zassert_true(fake_nvm_peek(NODE_NVM_KEY_KDEV, stored, sizeof(stored)));
	zassert_mem_equal(hard_key, stored, sizeof(stored));
	zassert_equal(NODE_IDENT_OK, node_ident_devnum(&devnum));
	zassert_equal(37976u, devnum);
}

ZTEST(node_ident, test_a_store_failure_while_provisioning_is_eio)
{
	uint32_t v;

	/* The key write itself fails. */
	fake_nvm_fail_stores(1u);
	zassert_equal(NODE_IDENT_EIO, node_ident_boot());
	zassert_false(node_ident_is_provisioned());
	zassert_equal(NODE_IDENT_ESTATE, node_ident_epoch(&v));

	/* A write reported durable that never landed: provisioning's
	 * read-back catches it, at the write. */
	fake_nvm_wipe();
	fake_rng_reset();
	node_ident_reset();
	fake_nvm_swallow_stores(true);
	zassert_equal(NODE_IDENT_EIO, node_ident_boot());
	zassert_false(node_ident_is_provisioned());
}
#endif /* CONFIG_RADIANT_NODE_IDENT_SELF_PROVISION */

ZTEST(node_ident, test_every_error_code_has_a_reason)
{
	static const int codes[] = {
		NODE_IDENT_OK, NODE_IDENT_EINVAL, NODE_IDENT_ENOTSUP,
		NODE_IDENT_ENOKEY, NODE_IDENT_EIO, NODE_IDENT_ESTATE,
		NODE_IDENT_EAGAIN, NODE_IDENT_EBUSY, -99,
	};

	/* The apps print this on the fallback line; a NULL there is a fault
	 * in the one log line that says why the node has no identity. */
	for (size_t i = 0u; i < ARRAY_SIZE(codes); i++) {
		const char *s = node_ident_strerror(codes[i]);

		zassert_not_null(s);
		zassert_true(s[0] != '\0');
	}
	zassert_true(strcmp(node_ident_strerror(NODE_IDENT_EIO),
			    node_ident_strerror(NODE_IDENT_ENOKEY)) != 0);
	zassert_true(strcmp(node_ident_strerror(NODE_IDENT_EBUSY),
			    node_ident_strerror(-99)) != 0);
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
	zassert_true(fake_nvm_peek_u32(NODE_NVM_KEY_EPOCH, &v));
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

	/* The assertion ADR 0009 asks for by name. fake_nvm_reboot() keeps the
	 * contents and forgets the init flag, so this is the same code path a
	 * cold start takes - a RAM-only counter would read 1 again here, and
	 * that node would reuse its epoch. */
	for (uint32_t expected = 2u; expected <= 5u; expected++) {
		fake_nvm_reboot();
		node_ident_reset();
		zassert_equal(NODE_IDENT_OK, node_ident_boot());
		zassert_equal(NODE_IDENT_OK, node_ident_boot_counter(&counter));
		zassert_equal(expected, counter);
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

ZTEST(node_ident, test_two_writes_per_boot_and_one_per_pairing_window)
{
	/* ADR 0009's flash-wear argument is a write frequency - "once per boot,
	 * once per pairing window" - and it is the whole reason this design was
	 * acceptable where the per-epoch ratchet was not. So it is counted. The
	 * epoch reservation (radiant-security.md 3.5) adds one write per boot,
	 * the next free epoch, and one per counter wrap (counted below in the
	 * reservation tests). */
	provisioned_and_booted();

	uint32_t base = fake_nvm_store_count();

	fake_nvm_reboot();
	node_ident_reset();
	zassert_equal(NODE_IDENT_OK, node_ident_boot());
	zassert_equal(base + 2u, fake_nvm_store_count());

	base = fake_nvm_store_count();
	zassert_equal(NODE_IDENT_OK, node_ident_pair_window_open(NULL));
	zassert_equal(base + 1u, fake_nvm_store_count());

	/* Deriving and closing cost nothing. */
	uint8_t scalar[NODE_IDENT_PAIR_SCALAR_BYTES];

	base = fake_nvm_store_count();
	zassert_equal(NODE_IDENT_OK, node_ident_pair_scalar(scalar));
	node_ident_pair_window_close();
	zassert_equal(base, fake_nvm_store_count());
}

/* ── The epoch ────────────────────────────────────────────────────────────── */

ZTEST(node_ident, test_epoch_steps_past_its_reservation_when_there_is_no_clock)
{
	uint32_t epoch;
	uint32_t counter;
	uint32_t next;

	/* First boot of a fresh store: the boot counter is the floor. */
	provisioned_and_booted();
	zassert_equal(NODE_IDENT_OK, node_ident_epoch(&epoch));
	zassert_equal(NODE_IDENT_OK, node_ident_boot_counter(&counter));
	zassert_equal(counter, epoch);
	zassert_equal(1u, epoch);

	/* E + 1 is reserved for this session's first wrap, so N = E + 2 is in
	 * storage before E is used, and the next boot starts there: epochs step
	 * by two per boot, while the boot counter keeps counting boots. */
	zassert_true(fake_nvm_peek_u32(NODE_NVM_KEY_EPOCH, &next));
	zassert_equal(3u, next);
	zassert_equal(NODE_IDENT_OK, node_ident_epoch_next(&next));
	zassert_equal(3u, next);

	fake_nvm_reboot();
	node_ident_reset();
	zassert_equal(NODE_IDENT_OK, node_ident_boot());
	zassert_equal(NODE_IDENT_OK, node_ident_epoch(&epoch));
	zassert_equal(NODE_IDENT_OK, node_ident_boot_counter(&counter));
	zassert_equal(3u, epoch);
	zassert_equal(2u, counter);
}

ZTEST(node_ident, test_a_store_without_an_epoch_record_boots_from_the_counter)
{
	uint32_t epoch;
	uint32_t v = 41u;

	/* A store an older image wrote: boot counter, no N record. */
	provisioned_and_booted();
	zassert_equal(NODE_NVM_OK,
		      node_nvm_store(NODE_NVM_KEY_BOOT, &v, sizeof(v)));
	fake_nvm_forget(NODE_NVM_KEY_EPOCH);
	fake_nvm_reboot();
	node_ident_reset();
	zassert_equal(NODE_IDENT_OK, node_ident_boot());
	zassert_equal(NODE_IDENT_OK, node_ident_epoch(&epoch));
	zassert_equal(42u, epoch);
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
	 * hand radiant an epoch this node has already used. */
	zassert_equal(9000u,
		      node_ident_epoch_from(9000u, true,
					    NODE_IDENT_EPOCH_BASE_S + 60ull));
}

ZTEST(node_ident, test_the_node_is_its_own_epoch_authority)
{
	uint32_t epoch;

	provisioned_and_booted();

	/* No host, no 0xF3. The node derives its epoch and hands it to
	 * radiant through the SAME entry point a host would use - ADR 0009
	 * decision 7: radiant still only ever receives an epoch through
	 * its existing API. */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_key(TEST_CH, test_root, 128,
					  VECTOR_DEVNUM));
	zassert_equal(NODE_IDENT_OK, node_ident_arm_sec(TEST_CH, TEST_PERIOD));
	zassert_equal(NODE_IDENT_OK, node_ident_epoch(&epoch));
	zassert_equal(1u, epoch);

	/* The next power-up moves it forward, and radiant accepts it. */
	fake_nvm_reboot();
	node_ident_reset();
	zassert_equal(NODE_IDENT_OK, node_ident_boot());
	zassert_equal(NODE_IDENT_OK, node_ident_arm_sec(TEST_CH, TEST_PERIOD));
}

ZTEST(node_ident, test_core_still_refuses_an_epoch_that_went_backwards)
{
	/* The node owns the epoch now, and radiant's monotonicity check is
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
	zassert_equal(NODE_IDENT_EIO, node_ident_arm_sec(TEST_CH, TEST_PERIOD));
}

/* ── Tier 0 identity ──────────────────────────────────────────────────────── */

ZTEST(node_ident, test_tier0_device_number_is_derived_from_k_dev)
{
	uint16_t devnum;

	provisioned_and_booted();
	zassert_equal(NODE_IDENT_OK, node_ident_devnum(&devnum));
	zassert_equal(VECTOR_DEVNUM, devnum);

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
	zassert_mem_equal(scalar_ctr1, scalar, sizeof(scalar));
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
	zassert_true(memcmp(a, b, sizeof(a)) != 0);

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

	/* The assertion ADR 0009 asks for: read straight out of the simulated
	 * part, bypassing the layer under test, before any scalar has been
	 * derived - the counter has already moved. */
	zassert_true(fake_nvm_peek_u32(NODE_NVM_KEY_PAIR, &stored));
	zassert_equal(1u, stored);

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
	zassert_equal(2u, ctr);
	zassert_equal(NODE_IDENT_OK, node_ident_pair_scalar(second));
	zassert_true(memcmp(first, second, sizeof(first)) != 0);
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

#if !defined(CONFIG_RADIANT_SEC_HAS_RNG)
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
#else
ZTEST(node_ident, test_the_rng_seam_reports_a_backend)
{
	uint8_t buf[8];

	/* The other side of the same seam: a backend is linked, the cap says
	 * so, and policy learns nothing else about it (ADR 0009). */
	zassert_true(radiant_sec_caps()->has_rng);
	zassert_equal(RADIANT_SEC_OK, radiant_sec_rng(buf, sizeof(buf)));
}
#endif /* !CONFIG_RADIANT_SEC_HAS_RNG */

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

/* ── The epoch reservation (review 2026-11-13 item 1) ─────────────────────── */

#define TX_NODE_CH 0u
#define RX_NODE_CH 1u

static uint64_t node_anchor;

static uint64_t node_slot_t(uint32_t slot)
{
	return node_anchor +
	       (uint64_t)slot * radiant_channel_counts_to_us(TEST_PERIOD);
}

static void key_conf(uint8_t ch, uint8_t switches)
{
	zassert_ok(radiant_sec_set_key(ch, test_root, 128, VECTOR_DEVNUM));
	zassert_ok(radiant_sec_configure(ch, switches, 2u,
					 RADIANT_SEC_PAGE_LO_DEFAULT,
					 RADIANT_SEC_PAGE_HI_DEFAULT));
}

/* key_conf() under a chosen root and base device number, X_CONF. */
static void key_conf_as(uint8_t ch, const uint8_t *root, uint16_t devnum)
{
	zassert_ok(radiant_sec_set_key(ch, root, 128, devnum));
	zassert_ok(radiant_sec_configure(ch, RADIANT_SEC_SW_CONF, 2u,
					 RADIANT_SEC_PAGE_LO_DEFAULT,
					 RADIANT_SEC_PAGE_HI_DEFAULT));
}

/* A booted node with channel 0 armed as its secured transmitter. */
static uint32_t booted_and_armed(void)
{
	uint32_t epoch;

	provisioned_and_booted();
	key_conf(TX_NODE_CH, RADIANT_SEC_SW_CONF);
	zassert_equal(NODE_IDENT_OK,
		      node_ident_arm_sec(TX_NODE_CH, TEST_PERIOD));
	node_anchor = (uint64_t)radiant_radio_now();
	zassert_equal(NODE_IDENT_OK, node_ident_epoch(&epoch));
	return epoch;
}

static int node_tx(uint32_t slot)
{
	uint8_t pay[8] = { 0x05u, 0xAAu, 1, 2, 3, 4, 5, 6 };

	return radiant_sec_tx_transform(TX_NODE_CH, pay, sizeof(pay),
					node_slot_t(slot));
}

static uint32_t sec_epoch(uint8_t ch)
{
	struct radiant_sec_state st;

	radiant_sec_get_state(ch, &st);
	return st.epoch;
}

static uint32_t stored_next(void)
{
	uint32_t v = 0u;

	zassert_true(fake_nvm_peek_u32(NODE_NVM_KEY_EPOCH, &v));
	return v;
}

/* Let the system work queue run node_ident's persist item. */
static void let_work_run(void)
{
	k_sleep(K_MSEC(20));
}

/* Holds the system work queue, so a test can reboot "before the work item
 * runs" deterministically rather than by racing it. */
static K_SEM_DEFINE(wq_hold_sem, 0, 1);

static void wq_hold_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)k_sem_take(&wq_hold_sem, K_FOREVER);
}

static K_WORK_DEFINE(wq_hold_work, wq_hold_fn);

static void wq_hold(void)
{
	k_sem_reset(&wq_hold_sem);
	zassert_true(k_work_submit(&wq_hold_work) >= 0);
	k_sleep(K_MSEC(2));     /* let it start and block */
}

static void wq_release(void)
{
	k_sem_give(&wq_hold_sem);
	let_work_run();
}

static void node_reboot(void)
{
	fake_nvm_reboot();
	node_ident_reset();
	radiant_sec_reset();
	zassert_equal(NODE_IDENT_OK, node_ident_boot());
}

ZTEST(node_ident, test_a_wrap_then_a_reboot_never_reuses_an_epoch)
{
	uint32_t e0;
	uint32_t e1;

	e0 = booted_and_armed();
	zassert_ok(node_tx(0u));
	zassert_ok(node_tx(65536u));
	zassert_equal(e0 + 1u, sec_epoch(TX_NODE_CH));

	/* The hook fired on the TX advance: N moves to (E + 1) + 2. */
	let_work_run();
	zassert_equal(e0 + 3u, stored_next());

	node_reboot();
	zassert_equal(NODE_IDENT_OK, node_ident_epoch(&e1));
	zassert_true(e1 > e0 + 1u, "epoch %u, wrap %u", e1, e0 + 1u);
}

ZTEST(node_ident, test_a_reboot_before_the_persist_runs_still_skips_the_wrap)
{
	uint32_t e0;
	uint32_t e1;

	e0 = booted_and_armed();
	zassert_ok(node_tx(0u));

	wq_hold();
	zassert_ok(node_tx(65536u));
	zassert_equal(e0 + 1u, sec_epoch(TX_NODE_CH));
	/* The wrap happened and nothing new reached storage yet... */
	zassert_equal(e0 + 2u, stored_next());

	/* ...and the power fails here. The boot-time reservation already
	 * covered E + 1. */
	node_reboot();
	zassert_equal(NODE_IDENT_OK, node_ident_epoch(&e1));
	wq_release();
	zassert_equal(e0 + 2u, e1);
	zassert_true(e1 > e0 + 1u);
	/* The cancelled item never wrote a stale N over the new one. */
	zassert_equal(e1 + 2u, stored_next());
}

ZTEST(node_ident, test_the_second_wrap_waits_for_its_reservation)
{
	uint32_t e0;
	uint32_t writes;

	e0 = booted_and_armed();
	zassert_ok(node_tx(0u));

	wq_hold();
	zassert_ok(node_tx(65536u));
	/* The second wrap's epoch is not reserved yet: withheld, not spent. */
	zassert_equal(RADIANT_SEC_EAGAIN, node_tx(2u * 65536u));
	zassert_equal(e0 + 1u, sec_epoch(TX_NODE_CH));

	writes = fake_nvm_store_count();
	wq_release();
	zassert_equal(writes + 1u, fake_nvm_store_count());
	zassert_ok(node_tx(2u * 65536u + 1u));
	zassert_equal(e0 + 2u, sec_epoch(TX_NODE_CH));
	let_work_run();
	zassert_equal(e0 + 4u, stored_next());
}

ZTEST(node_ident, test_a_failed_persist_holds_the_ceiling_and_retries)
{
	uint32_t e0;

	e0 = booted_and_armed();
	zassert_ok(node_tx(0u));

	fake_nvm_fail_stores(1u);
	zassert_ok(node_tx(65536u));
	let_work_run();
	zassert_equal(e0 + 2u, stored_next());
	zassert_equal(RADIANT_SEC_EAGAIN, node_tx(2u * 65536u));

	/* The retry lands a second later. */
	k_sleep(K_MSEC(1100));
	zassert_equal(e0 + 3u, stored_next());
	zassert_ok(node_tx(2u * 65536u + 1u));
}

ZTEST(node_ident, test_the_hook_follows_an_rx_advance_too)
{
	uint8_t  air[8] = { 0x05u, 0x00u, 1, 2, 3, 4, 5, 6 };
	uint32_t e0;

	/* The same node receiving: the RX path advances the epoch at a wrap
	 * too, and that epoch is just as used. */
	provisioned_and_booted();
	key_conf(RX_NODE_CH, RADIANT_SEC_SW_AUTH);
	zassert_equal(NODE_IDENT_OK,
		      node_ident_arm_sec(RX_NODE_CH, TEST_PERIOD));
	node_anchor = (uint64_t)radiant_radio_now();
	zassert_equal(NODE_IDENT_OK, node_ident_epoch(&e0));

	/* Counter 0 heard at slot 65536: one wrap. */
	zassert_equal(RADIANT_SEC_RX_DEFER,
		      radiant_sec_rx_ingest(RX_NODE_CH, air, 8u, true,
					    node_slot_t(65536u)));
	radiant_sec_pump();
	zassert_equal(e0 + 1u, sec_epoch(RX_NODE_CH));
	let_work_run();
	zassert_equal(e0 + 3u, stored_next());
}

ZTEST(node_ident, test_a_channel_node_ident_did_not_arm_is_not_followed)
{
	uint8_t  pay[8] = { 0x05u, 0xAAu, 1, 2, 3, 4, 5, 6 };
	uint32_t writes;

	provisioned_and_booted();
	key_conf(TX_NODE_CH, RADIANT_SEC_SW_CONF);
	zassert_ok(radiant_sec_set_epoch(TX_NODE_CH, 100u, 0u, TEST_PERIOD));
	node_anchor = (uint64_t)radiant_radio_now();
	writes = fake_nvm_store_count();
	zassert_ok(radiant_sec_tx_transform(TX_NODE_CH, pay, 8u,
					    node_slot_t(0u)));
	zassert_ok(radiant_sec_tx_transform(TX_NODE_CH, pay, 8u,
					    node_slot_t(65536u)));
	let_work_run();
	zassert_equal(writes, fake_nvm_store_count());
}

ZTEST(node_ident, test_two_boots_in_one_wall_clock_minute_get_two_epochs)
{
	uint32_t e0;
	uint32_t e1;

	/* A clock 10 000 minutes past the base: the clock dominates the boot
	 * counter, and both boots read the same minute. */
	wall_valid = true;
	wall_s = NODE_IDENT_EPOCH_BASE_S + 600000ull + 7ull;
	provisioned_and_booted();
	zassert_equal(NODE_IDENT_OK, node_ident_epoch(&e0));
	zassert_equal(10000u, e0);

	wall_s += 30ull;        /* same minute */
	node_reboot();
	zassert_equal(NODE_IDENT_OK, node_ident_epoch(&e1));
	zassert_true(e1 > e0, "epoch %u", e0);
	zassert_equal(e0 + 2u, e1);

	/* A clock that later runs ahead still wins. */
	wall_s += 3600ull;
	node_reboot();
	zassert_equal(NODE_IDENT_OK, node_ident_epoch(&e1));
	zassert_equal(10060u, e1);
}

ZTEST(node_ident, test_an_epoch_reservation_that_cannot_persist_is_no_boot)
{
	uint32_t v;

	zassert_equal(NODE_IDENT_OK, node_ident_provision(k_dev));
	/* The boot counter write succeeds; the reservation's does not. */
	fake_nvm_fail_stores_after(1u, 1u);
	zassert_equal(NODE_IDENT_EIO, node_ident_boot());
	zassert_equal(NODE_IDENT_ESTATE, node_ident_epoch(&v));
}

#if defined(CONFIG_RADIANT_SEC_COMPAT)
ZTEST(node_ident, test_a_compat_wrap_is_reserved_and_followed)
{
	uint8_t  body[RADIANT_SEC_COMPAT_BODY_BYTES];
	uint64_t t0 = 1000000u;
	uint64_t wrap_us = 65536ull * RADIANT_SEC_COMPAT_T_DEFAULT_S *
			   1000000ull;
	uint32_t e0;

	provisioned_and_booted();
	zassert_equal(NODE_IDENT_OK, node_ident_epoch(&e0));
	zassert_ok(radiant_sec_compat_set_key(TX_NODE_CH, test_root, 128,
					      VECTOR_DEVNUM));
	zassert_equal(NODE_IDENT_OK,
		      node_ident_arm_compat(TX_NODE_CH, t0, TEST_PERIOD));

	zassert_true(radiant_sec_compat_tx_attest(TX_NODE_CH, t0, body,
						  sizeof(body)) >= 0);
	wq_hold();
	zassert_true(radiant_sec_compat_tx_attest(TX_NODE_CH, t0 + wrap_us,
						  body, sizeof(body)) > 0);
	zassert_equal(RADIANT_SEC_EAGAIN,
		      radiant_sec_compat_tx_attest(TX_NODE_CH,
						   t0 + 2u * wrap_us, body,
						   sizeof(body)));
	wq_release();
	zassert_equal(e0 + 3u, stored_next());
	zassert_true(radiant_sec_compat_tx_attest(TX_NODE_CH,
						  t0 + 2u * wrap_us, body,
						  sizeof(body)) > 0);
}
#endif

/* ── Review follow-ups (2026-11-13) ──────────────────────────────────────── */

/* Item 1: a sender silent for more than one wrap catches up by two at once.
 * The ceiling (E + 1) refuses it; the _wanted hook reserves E + 2 and raises
 * the ceiling, and TX resumes at the next slot instead of never. */
ZTEST(node_ident, test_a_tx_gap_of_two_wraps_is_reserved_and_resumes)
{
	uint32_t e0;
	uint32_t e1;

	e0 = booted_and_armed();
	zassert_ok(node_tx(0u));

	zassert_equal(RADIANT_SEC_EAGAIN, node_tx(2u * 65536u));
	zassert_equal(e0, sec_epoch(TX_NODE_CH));
	let_work_run();
	zassert_equal(e0 + 3u, stored_next());

	zassert_ok(node_tx(2u * 65536u + 1u));
	zassert_equal(e0 + 2u, sec_epoch(TX_NODE_CH));
	let_work_run();
	zassert_equal(e0 + 4u, stored_next());

	/* And the next boot is above both epochs the gap used. */
	node_reboot();
	zassert_equal(NODE_IDENT_OK, node_ident_epoch(&e1));
	zassert_equal(e0 + 4u, e1);
}

/* Re-review M2: only the first arm in a boot gets E. A re-arm of the same
 * channel, and an arm of another channel under the same root and device
 * number after the first was released, each get a fresh epoch - after
 * NODE_IDENT_EAGAIN until its reservation is durable - so no arm restarts the
 * counter under an epoch an earlier arm used. */
ZTEST(node_ident, test_every_arm_after_the_first_gets_a_fresh_epoch)
{
	uint32_t e0;
	uint32_t e1;
	uint32_t e2;

	e0 = booted_and_armed();
	zassert_equal(e0, sec_epoch(TX_NODE_CH));

	/* The same channel, re-keyed under the same root: EAGAIN, then F. */
	radiant_sec_channel_release(TX_NODE_CH);
	key_conf(TX_NODE_CH, RADIANT_SEC_SW_CONF);
	zassert_equal(NODE_IDENT_EAGAIN,
		      node_ident_arm_sec(TX_NODE_CH, TEST_PERIOD));
	let_work_run();
	zassert_equal(NODE_IDENT_OK,
		      node_ident_arm_sec(TX_NODE_CH, TEST_PERIOD));
	e1 = sec_epoch(TX_NODE_CH);
	zassert_true(e1 > e0 + 1u, "epoch %u, E %u", e1, e0);
	zassert_equal(e1 + 2u, stored_next());

	/* Re-armed again without a release (it carries the node's ceiling). */
	zassert_equal(NODE_IDENT_EAGAIN,
		      node_ident_arm_sec(TX_NODE_CH, TEST_PERIOD));
	let_work_run();
	zassert_equal(NODE_IDENT_OK,
		      node_ident_arm_sec(TX_NODE_CH, TEST_PERIOD));
	e2 = sec_epoch(TX_NODE_CH);
	zassert_true(e2 > e1 + 1u);

	/* Another channel, same root and devnum, after a release. */
	radiant_sec_channel_release(TX_NODE_CH);
	key_conf(RX_NODE_CH, RADIANT_SEC_SW_CONF);
	zassert_equal(NODE_IDENT_EAGAIN,
		      node_ident_arm_sec(RX_NODE_CH, TEST_PERIOD));
	let_work_run();
	zassert_equal(NODE_IDENT_OK,
		      node_ident_arm_sec(RX_NODE_CH, TEST_PERIOD));
	zassert_true(sec_epoch(RX_NODE_CH) > e2 + 1u);

	zassert_equal(NODE_IDENT_EINVAL, node_ident_arm_sec(32u, TEST_PERIOD));

	/* The next boot is above every epoch this one handed out. */
	e1 = sec_epoch(RX_NODE_CH);
	node_reboot();
	zassert_equal(NODE_IDENT_OK, node_ident_epoch(&e2));
	zassert_true(e2 > e1 + 1u);
}

/* Re-review M1: a host 0xF3 on a channel the node follows is refused (it
 * would run unreserved wraps under the node root). The node's root re-sent
 * changes nothing; a different root ends the following, and then that
 * channel's wraps neither drive N nor get capped. */
static const uint8_t host_root[16] = {
	0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78,
	0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0,
};

ZTEST(node_ident, test_a_host_epoch_on_a_followed_channel_is_refused)
{
	uint8_t  pay[8] = { 0x05u, 0xAAu, 1, 2, 3, 4, 5, 6 };
	uint32_t e0;
	uint32_t n0;
	uint32_t e1;

	e0 = booted_and_armed();
	n0 = stored_next();

	zassert_equal(RADIANT_SEC_ESTATE,
		      radiant_sec_set_epoch(TX_NODE_CH, 0x10000000u, 0u,
					    TEST_PERIOD));
	zassert_equal(e0, sec_epoch(TX_NODE_CH));

	/* Re-sending the node's own root is a no-op (item 9): still followed,
	 * still refused, still armed under e0. */
	zassert_ok(radiant_sec_set_key(TX_NODE_CH, test_root, 128,
				       VECTOR_DEVNUM));
	zassert_equal(RADIANT_SEC_ESTATE,
		      radiant_sec_set_epoch(TX_NODE_CH, 0x10000000u, 0u,
					    TEST_PERIOD));
	zassert_equal(e0, sec_epoch(TX_NODE_CH));
	zassert_ok(node_tx(0u));

	/* A host that replaces the root owns the channel, and the node lets
	 * go. */
	zassert_ok(radiant_sec_set_key(TX_NODE_CH, host_root, 128,
				       VECTOR_DEVNUM));
	zassert_ok(radiant_sec_set_epoch(TX_NODE_CH, 0x10000000u, 0u,
					 TEST_PERIOD));
	node_anchor = (uint64_t)radiant_radio_now();
	zassert_ok(radiant_sec_tx_transform(TX_NODE_CH, pay, 8u,
					    node_slot_t(0u)));
	zassert_ok(radiant_sec_tx_transform(TX_NODE_CH, pay, 8u,
					    node_slot_t(3u * 65536u)));
	let_work_run();
	zassert_equal(n0, stored_next());

	node_reboot();
	zassert_equal(NODE_IDENT_OK, node_ident_epoch(&e1));
	zassert_equal(n0, e1);
}

/* Item 3 too: a released, host-re-keyed channel is not capped by the work
 * item another armed channel's wrap queues. */
ZTEST(node_ident, test_a_released_channel_is_not_capped_by_another_wrap)
{
	uint8_t pay[8] = { 0x05u, 0xAAu, 1, 2, 3, 4, 5, 6 };

	booted_and_armed();
	/* Its own device number: two live channels under one root and number
	 * are refused (item 2 below). */
	key_conf_as(RX_NODE_CH, test_root, VECTOR_DEVNUM + 1u);
	zassert_equal(NODE_IDENT_EAGAIN,
		      node_ident_arm_sec(RX_NODE_CH, TEST_PERIOD));
	let_work_run();
	zassert_equal(NODE_IDENT_OK,
		      node_ident_arm_sec(RX_NODE_CH, TEST_PERIOD));

	radiant_sec_channel_release(RX_NODE_CH);
	key_conf(RX_NODE_CH, RADIANT_SEC_SW_CONF);
	zassert_ok(radiant_sec_set_epoch(RX_NODE_CH, 5000u, 0u, TEST_PERIOD));

	zassert_ok(node_tx(0u));
	zassert_ok(node_tx(65536u));
	let_work_run();     /* raises ceilings on every armed channel */

	zassert_ok(radiant_sec_tx_transform(RX_NODE_CH, pay, 8u,
					    node_slot_t(0u)));
	zassert_ok(radiant_sec_tx_transform(RX_NODE_CH, pay, 8u,
					    node_slot_t(3u * 65536u)));
	zassert_equal(5003u, sec_epoch(RX_NODE_CH));
}

/* Item 6: a persist that keeps failing backs off rather than retrying every
 * second; it still lands once storage recovers. */
ZTEST(node_ident, test_a_failing_persist_backs_off)
{
	uint32_t e0;
	uint32_t stores;

	e0 = booted_and_armed();
	zassert_ok(node_tx(0u));

	fake_nvm_fail_stores(100u);
	stores = fake_nvm_store_count();
	zassert_ok(node_tx(65536u));
	k_sleep(K_MSEC(3500));
	/* Tries at ~0, 1, 3 s: three, not four. A withheld second wrap's
	 * _wanted hooks must not cut the backoff short either. */
	for (int i = 0; i < 5; i++) {
		zassert_equal(RADIANT_SEC_EAGAIN,
			      node_tx(2u * 65536u + (uint32_t)i));
	}
	zassert_true(fake_nvm_store_count() - stores <= 3u,
		     "%u tries in 3.5 s",
		     fake_nvm_store_count() - stores);
	fake_nvm_fail_stores(0u);
	k_sleep(K_MSEC(4500));
	/* N covers the second wrap the withheld slots wanted (e0 + 2). */
	zassert_equal(e0 + 3u, stored_next());
}

/* ── Two live channels on one epoch (review 2026-11-14 item 2) ────────────── */

/* An arm after the first waits once for its fresh epoch's reservation. */
static int arm_sec_settled(uint8_t ch)
{
	int rc = node_ident_arm_sec(ch, TEST_PERIOD);

	if (rc == NODE_IDENT_EAGAIN) {
		let_work_run();
		rc = node_ident_arm_sec(ch, TEST_PERIOD);
	}
	return rc;
}

#if defined(CONFIG_RADIANT_SEC_COMPAT)
static int arm_compat_settled(uint8_t ch, uint64_t now_us)
{
	int rc = node_ident_arm_compat(ch, now_us, TEST_PERIOD);

	if (rc == NODE_IDENT_EAGAIN) {
		let_work_run();
		rc = node_ident_arm_compat(ch, now_us, TEST_PERIOD);
	}
	return rc;
}
#endif

/* B armed while A is live, same root and base device number: B takes
 * F = E + 2, its reservation (N = E + 4) raises A's ceiling to E + 3, and A's
 * second counter wrap lands on F - both channels then run the counter from 0
 * under one (root, epoch, devnum), a two-time pad. So B's arm is refused. The
 * branch shows the collision should an arm ever get through again. */
ZTEST(node_ident, test_a_second_live_arm_under_one_root_and_devnum_is_refused)
{
	uint32_t e0;
	int      rc;

	e0 = booted_and_armed();
	key_conf(RX_NODE_CH, RADIANT_SEC_SW_CONF);
	rc = arm_sec_settled(RX_NODE_CH);
	if (rc == NODE_IDENT_OK) {
		uint32_t f = sec_epoch(RX_NODE_CH);

		zassert_ok(node_tx(0u));
		zassert_ok(node_tx(65536u));
		let_work_run();
		zassert_ok(node_tx(2u * 65536u));
		zassert_not_equal(f, sec_epoch(TX_NODE_CH),
				  "A (armed at %u) reached B's epoch %u after "
				  "two wraps", e0, f);
	}
	zassert_equal(NODE_IDENT_EBUSY, rc, "arm B: %d", rc);

	/* Nothing moved: A keeps E, B has no epoch, and no epoch was
	 * reserved for the refused arm. */
	zassert_equal(e0, sec_epoch(TX_NODE_CH));
	zassert_equal(0u, sec_epoch(RX_NODE_CH));
	zassert_equal(e0 + 2u, stored_next());
	zassert_equal(NODE_IDENT_EBUSY,
		      node_ident_arm_sec(RX_NODE_CH, TEST_PERIOD));
}

/* The owner's rule (2026-11-14): several secured sensors run at once, so only
 * the real collision is refused. A different base device number or a
 * different root is a different key, and the same channel re-armed is the
 * M2 path (a fresh epoch), not a second live channel. */
ZTEST(node_ident, test_live_arms_that_differ_in_root_or_devnum_are_allowed)
{
	uint32_t e0;

	e0 = booted_and_armed();

	key_conf_as(RX_NODE_CH, test_root, VECTOR_DEVNUM + 1u);
	zassert_equal(NODE_IDENT_OK, arm_sec_settled(RX_NODE_CH));

	key_conf_as(2u, host_root, VECTOR_DEVNUM);
	zassert_equal(NODE_IDENT_OK, arm_sec_settled(2u));

	/* A itself again, while all three are live. */
	zassert_equal(NODE_IDENT_OK, arm_sec_settled(TX_NODE_CH));
	zassert_true(sec_epoch(TX_NODE_CH) > e0 + 1u);

	/* But a fourth under A's root and number still collides, past live
	 * channels in the table that do not match. */
	key_conf(3u, RADIANT_SEC_SW_CONF);
	zassert_equal(NODE_IDENT_EBUSY, arm_sec_settled(3u));
}

/* "Live" is radiant still holding A's ceiling, not node_ident's armed bit:
 * a host replacing A's root, or a release, ends it and B may arm. */
ZTEST(node_ident, test_a_released_or_rekeyed_channel_no_longer_blocks_an_arm)
{
	booted_and_armed();
	key_conf(RX_NODE_CH, RADIANT_SEC_SW_CONF);
	zassert_equal(NODE_IDENT_EBUSY, arm_sec_settled(RX_NODE_CH));

	/* A host takes A over with its own root: the node lets go. */
	zassert_ok(radiant_sec_set_key(TX_NODE_CH, host_root, 128,
				       VECTOR_DEVNUM));
	zassert_equal(NODE_IDENT_OK, arm_sec_settled(RX_NODE_CH));

	/* Now B is the live one; A back under the node root collides... */
	key_conf(TX_NODE_CH, RADIANT_SEC_SW_CONF);
	zassert_equal(NODE_IDENT_EBUSY, arm_sec_settled(TX_NODE_CH));

	/* ...until B is released. */
	radiant_sec_channel_release(RX_NODE_CH);
	zassert_equal(NODE_IDENT_OK, arm_sec_settled(TX_NODE_CH));
}

#if defined(CONFIG_RADIANT_SEC_COMPAT)
/* The compat twin: the same arm path, the same rule. */
ZTEST(node_ident, test_a_second_live_compat_arm_under_one_root_and_devnum_is_refused)
{
	uint64_t t0 = 1000000u;

	provisioned_and_booted();
	zassert_ok(radiant_sec_compat_set_key(TX_NODE_CH, test_root, 128,
					      VECTOR_DEVNUM));
	zassert_equal(NODE_IDENT_OK, arm_compat_settled(TX_NODE_CH, t0));

	zassert_ok(radiant_sec_compat_set_key(RX_NODE_CH, test_root, 128,
					      VECTOR_DEVNUM));
	zassert_equal(NODE_IDENT_EBUSY, arm_compat_settled(RX_NODE_CH, t0));

	/* A different base device number is a different key: allowed. */
	zassert_ok(radiant_sec_compat_set_key(RX_NODE_CH, test_root, 128,
					      VECTOR_DEVNUM + 1u));
	zassert_equal(NODE_IDENT_OK, arm_compat_settled(RX_NODE_CH, t0));

	/* And A re-armed while B is live. */
	zassert_equal(NODE_IDENT_OK, arm_compat_settled(TX_NODE_CH, t0));

	/* A released: B's old identity may arm on a third channel. */
	radiant_sec_compat_channel_release(TX_NODE_CH);
	zassert_ok(radiant_sec_compat_set_key(2u, test_root, 128,
					      VECTOR_DEVNUM));
	zassert_equal(NODE_IDENT_OK, arm_compat_settled(2u, t0));
}
#endif
