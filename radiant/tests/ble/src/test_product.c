/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_product.c - radiant_product.c: one identity record feeding ANT+ pages
 * 80/81 and the Bluetooth DIS strings, the strict-identity checks, and the
 * decision between the identity record and the shared development device
 * number.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <radiant/radiant_product.h>
#include <radiant/radiant_profile_common.h>

static const struct radiant_product shipping = {
	.manufacturer_id = 1234u,
	.model_number = 4321u,
	.hw_rev = 7u,
	.sw_rev_main = 13u,
	.sw_rev_supp = 2u,
	.manufacturer_name = "Acme Fitness",
	.model_name = "Strap 2",
	.version = "1.3.2",
};

static const struct radiant_product placeholder = {
	.manufacturer_id = RADIANT_PRODUCT_DEV_MANUFACTURER_ID,
	.model_number = 1u,
	.hw_rev = 1u,
	.sw_rev_main = 13u,
	.sw_rev_supp = 0u,
	.manufacturer_name = RADIANT_PRODUCT_PLACEHOLDER_MANUFACTURER_NAME,
	.model_name = RADIANT_PRODUCT_PLACEHOLDER_MODEL_NAME,
	.version = "1.3.0",
};

ZTEST_SUITE(product, NULL, NULL, NULL, NULL, NULL);

/* ── Pages 80/81 ─────────────────────────────────────────────────────────── */

ZTEST(product, test_common_id_comes_from_the_record_and_the_serial)
{
	struct radiant_profile_common_id id;

	memset(&id, 0xA5, sizeof(id));
	zassert_ok(radiant_product_fill_common_id(&shipping, 0x993BC823u, &id));
	zassert_equal(7u, id.hw_revision);
	zassert_equal(1234u, id.manufacturer_id);
	zassert_equal(4321u, id.model_number);
	zassert_equal(13u, id.sw_revision_main);
	zassert_equal(2u, id.sw_revision_supplemental);
	zassert_equal(0x993BC823u, id.serial_number);
}

/* And through the real page encoders, so the record lands in the bytes a head
 * unit reads. */
ZTEST(product, test_the_record_reaches_pages_80_and_81)
{
	struct radiant_profile_common_id id;
	uint8_t p80[8];
	uint8_t p81[8];

	zassert_ok(radiant_product_fill_common_id(&shipping, 0x993BC823u, &id));
	zassert_ok(radiant_profile_common_80(&id, p80));
	zassert_ok(radiant_profile_common_81(&id, p81));

	zassert_equal(0x50u, p80[0]);
	zassert_equal(7u, p80[3]);                               /* HW rev */
	zassert_equal(1234u, p80[4] | (p80[5] << 8));            /* manufacturer */
	zassert_equal(4321u, p80[6] | (p80[7] << 8));            /* model */

	zassert_equal(0x51u, p81[0]);
	zassert_equal(2u, p81[2]);                               /* SW supp */
	zassert_equal(13u, p81[3]);                              /* SW main */
	zassert_equal(0x993BC823u, (uint32_t)p81[4] | ((uint32_t)p81[5] << 8) |
				   ((uint32_t)p81[6] << 16) | ((uint32_t)p81[7] << 24));
}

ZTEST(product, test_fill_refuses_null)
{
	struct radiant_profile_common_id id;

	zassert_equal(-EINVAL, radiant_product_fill_common_id(NULL, 1u, &id));
	zassert_equal(-EINVAL, radiant_product_fill_common_id(&shipping, 1u, NULL));
}

/* ── DIS strings ─────────────────────────────────────────────────────────── */

ZTEST(product, test_dis_strings_come_from_the_same_record)
{
	struct radiant_product_dis d;

	zassert_ok(radiant_product_dis_strings(&shipping, 0x993BC823u, &d));
	zassert_str_equal("Acme Fitness", d.manufacturer);
	zassert_str_equal("Strap 2", d.model);
	/* Decimal, as a head unit shows page 81's serial: the two radios name
	 * the unit with the same number. */
	zassert_str_equal("2570831907", d.serial);
	zassert_str_equal("7", d.hw_rev);
	zassert_str_equal("1.3.2", d.fw_rev);
	zassert_str_equal("1.3.2", d.sw_rev);
}

ZTEST(product, test_dis_strings_bound_and_null_safe)
{
	struct radiant_product_dis d;
	struct radiant_product long_names = shipping;
	static const char forty[] = "0123456789012345678901234567890123456789";

	long_names.manufacturer_name = forty;
	long_names.model_name = NULL;
	zassert_ok(radiant_product_dis_strings(&long_names, 0xFFFFFFFEu, &d));
	zassert_equal(RADIANT_PRODUCT_STR_MAX - 1u, strlen(d.manufacturer));
	zassert_mem_equal(forty, d.manufacturer, RADIANT_PRODUCT_STR_MAX - 1u);
	zassert_str_equal("", d.model);
	zassert_str_equal("4294967294", d.serial);

	zassert_equal(-EINVAL, radiant_product_dis_strings(NULL, 1u, &d));
	zassert_equal(-EINVAL, radiant_product_dis_strings(&shipping, 1u, NULL));
}

/* ── Strict identity ─────────────────────────────────────────────────────── */

ZTEST(product, test_a_real_identity_passes_the_strict_check)
{
	zassert_equal(0u, radiant_product_check(&shipping));
}

/* The Kconfig defaults fail every check at once, and say which. */
ZTEST(product, test_the_defaults_fail_every_strict_check)
{
	zassert_equal(RADIANT_PRODUCT_BAD_MANUFACTURER_ID |
		      RADIANT_PRODUCT_BAD_MANUFACTURER_NAME |
		      RADIANT_PRODUCT_BAD_MODEL_NAME,
		      radiant_product_check(&placeholder));
}

ZTEST(product, test_each_strict_check_on_its_own)
{
	struct radiant_product p = shipping;

	p.manufacturer_id = 255u;
	zassert_equal(RADIANT_PRODUCT_BAD_MANUFACTURER_ID, radiant_product_check(&p));

	p = shipping;
	p.manufacturer_name = "";
	zassert_equal(RADIANT_PRODUCT_BAD_MANUFACTURER_NAME, radiant_product_check(&p));
	p.manufacturer_name = NULL;
	zassert_equal(RADIANT_PRODUCT_BAD_MANUFACTURER_NAME, radiant_product_check(&p));

	p = shipping;
	p.model_name = RADIANT_PRODUCT_PLACEHOLDER_MODEL_NAME;
	zassert_equal(RADIANT_PRODUCT_BAD_MODEL_NAME, radiant_product_check(&p));
	p.model_name = "";
	zassert_equal(RADIANT_PRODUCT_BAD_MODEL_NAME, radiant_product_check(&p));

	/* A name that merely starts like the placeholder is a real name. */
	p = shipping;
	p.model_name = "RadiANT reference 2";
	zassert_equal(0u, radiant_product_check(&p));
	/* 0 and 254 are not the development id. */
	p.manufacturer_id = 254u;
	zassert_equal(0u, radiant_product_check(&p));

	zassert_equal(RADIANT_PRODUCT_BAD_NULL, radiant_product_check(NULL));
}

/* product.conf.example's own values are placeholders too: copying the
 * template without editing it must not satisfy strict mode. */
ZTEST(product, test_template_values_fail_the_strict_check)
{
	struct radiant_product p = shipping;

	p.manufacturer_name = "Your Company";
	p.model_name = "Your HR Strap";
	zassert_equal(RADIANT_PRODUCT_BAD_MANUFACTURER_NAME |
		      RADIANT_PRODUCT_BAD_MODEL_NAME, radiant_product_check(&p));
	p.model_name = "Your Treadmill";
	zassert_equal(RADIANT_PRODUCT_BAD_MANUFACTURER_NAME |
		      RADIANT_PRODUCT_BAD_MODEL_NAME, radiant_product_check(&p));
	p.manufacturer_name = "Your anything";
	p.model_name = "Yours Truly";
	zassert_equal(RADIANT_PRODUCT_BAD_MANUFACTURER_NAME, radiant_product_check(&p),
		      "only the template prefix, with its space, is refused");
}

ZTEST(product, test_default_and_template_ble_names_fail)
{
	zassert_equal(RADIANT_PRODUCT_BAD_BLE_NAME, radiant_product_check_ble_name(NULL));
	zassert_equal(RADIANT_PRODUCT_BAD_BLE_NAME, radiant_product_check_ble_name(""));
	zassert_equal(RADIANT_PRODUCT_BAD_BLE_NAME, radiant_product_check_ble_name("Zephyr"));
	zassert_equal(RADIANT_PRODUCT_BAD_BLE_NAME,
		      radiant_product_check_ble_name("RadiANT HR"));
	zassert_equal(RADIANT_PRODUCT_BAD_BLE_NAME,
		      radiant_product_check_ble_name("RadiANT Treadmill"));
	zassert_equal(RADIANT_PRODUCT_BAD_BLE_NAME,
		      radiant_product_check_ble_name("Your HR Strap"));
	zassert_equal(0u, radiant_product_check_ble_name("Acme Strap 2"));
	zassert_equal(0u, radiant_product_check_ble_name("RadiANT Test"));
}

/* docs/radiant-security.md 5.4: a privacy posture puts serial 0xFFFFFFFF on
 * page 81 (and so page 2's upper half is 0xFFFF); without one, serial32. */
ZTEST(product, test_privacy_posture_withholds_the_serial)
{
	struct radiant_profile_common_id id;
	uint8_t p81[8];

	zassert_equal(0x993BC823u, radiant_product_public_serial(false, 0x993BC823u));
	zassert_equal(0xFFFFFFFFu, radiant_product_public_serial(true, 0x993BC823u));

	zassert_ok(radiant_product_fill_common_id(
		&shipping, radiant_product_public_serial(true, 0x993BC823u), &id));
	zassert_ok(radiant_profile_common_81(&id, p81));
	zassert_equal(0xFFu, p81[4]);
	zassert_equal(0xFFu, p81[5]);
	zassert_equal(0xFFu, p81[6]);
	zassert_equal(0xFFu, p81[7]);
	zassert_equal(0xFFFFu, (uint16_t)(radiant_product_public_serial(true, 1u) >> 16));
}

/* ── The identity-fallback decision ──────────────────────────────────────── */

#define IDENT_DEVNUM   51235u
#define IDENT_SERIAL   0x993BC823u
#define FALLBACK       4242u
#define BOOT_FAILED    (-4) /* RADIANT_NODE_IDENT_EIO */

ZTEST(product, test_a_booted_identity_is_used_in_both_modes)
{
	for (int strict = 0; strict <= 1; strict++) {
		struct radiant_product_unit u;

		memset(&u, 0, sizeof(u));
		zassert_ok(radiant_product_unit_resolve(strict != 0, 0, IDENT_DEVNUM, 0,
							IDENT_SERIAL, FALLBACK, &u));
		zassert_equal(IDENT_DEVNUM, u.devnum);
		zassert_equal(IDENT_SERIAL, u.serial);
		zassert_true(u.from_identity);
	}
}

/* Development builds keep today's behaviour: the Kconfig number, and the
 * serial that went with it (serial = devnum). */
ZTEST(product, test_development_falls_back_to_the_kconfig_number)
{
	struct radiant_product_unit u;

	zassert_ok(radiant_product_unit_resolve(false, BOOT_FAILED, 0u, -1, 0u,
						FALLBACK, &u));
	zassert_equal(FALLBACK, u.devnum);
	zassert_equal(FALLBACK, u.serial);
	zassert_false(u.from_identity);
}

/* Strict never hands out the shared number: it says "stay off-air and retry"
 * and leaves the output untouched, so nothing derived from it can leak. */
ZTEST(product, test_strict_never_transmits_on_the_shared_number)
{
	struct radiant_product_unit u = { .devnum = 0u, .serial = 0u,
					  .from_identity = false };

	zassert_equal(-EAGAIN, radiant_product_unit_resolve(true, BOOT_FAILED, 0u, -1,
							    0u, FALLBACK, &u));
	zassert_equal(0u, u.devnum);
	zassert_not_equal(FALLBACK, u.serial);

	/* A boot that "succeeded" with devnum 0, or whose serial could not be
	 * derived, is no identity either. */
	zassert_equal(-EAGAIN, radiant_product_unit_resolve(true, 0, 0u, 0,
							    IDENT_SERIAL, FALLBACK, &u));
	zassert_equal(-EAGAIN, radiant_product_unit_resolve(true, 0, IDENT_DEVNUM, -2,
							    0u, FALLBACK, &u));
	zassert_equal(0u, u.devnum);
}

ZTEST(product, test_strict_retry_backs_off_to_an_hour)
{
	zassert_equal(1000u, radiant_product_retry_ms(0u));
	zassert_equal(1000u, radiant_product_retry_ms(1u));
	zassert_equal(2000u, radiant_product_retry_ms(2u));
	zassert_equal(32000u, radiant_product_retry_ms(6u));
	zassert_equal(2048000u, radiant_product_retry_ms(12u));
	zassert_equal(RADIANT_PRODUCT_RETRY_MAX_MS, radiant_product_retry_ms(13u));
	zassert_equal(RADIANT_PRODUCT_RETRY_MAX_MS, radiant_product_retry_ms(0xFFFFFFFFu));
	/* Each attempt may write the boot counter: an hour's cap keeps a
	 * persistent fault to a few writes an hour. */
	zassert_equal(3600000u, RADIANT_PRODUCT_RETRY_MAX_MS);
}

ZTEST(product, test_resolve_refuses_null)
{
	zassert_equal(-EINVAL, radiant_product_unit_resolve(false, 0, 1u, 0, 1u, 1u,
							    NULL));
}

/* ── Software revision from VERSION ──────────────────────────────────────── */

ZTEST(product, test_sw_revision_from_version)
{
	uint8_t m;
	uint8_t s;

	radiant_product_sw_rev_from_version(1u, 3u, 0u, &m, &s);
	zassert_equal(13u, m);
	zassert_equal(0u, s);

	radiant_product_sw_rev_from_version(2u, 9u, 99u, &m, &s);
	zassert_equal(29u, m);
	zassert_equal(99u, s);

	radiant_product_sw_rev_from_version(1u, 3u, 100u, &m, &s);
	zassert_equal(13u, m);
	zassert_equal(0xFFu, s, "a patch that does not fit is 'unused'");

	radiant_product_sw_rev_from_version(1u, 10u, 0u, &m, &s);
	zassert_equal(0xFFu, m);
	zassert_equal(0xFFu, s);

	radiant_product_sw_rev_from_version(25u, 5u, 0u, &m, &s);
	zassert_equal(255u, m);
	radiant_product_sw_rev_from_version(25u, 6u, 0u, &m, &s);
	zassert_equal(0xFFu, m);
	zassert_equal(0xFFu, s);
}
