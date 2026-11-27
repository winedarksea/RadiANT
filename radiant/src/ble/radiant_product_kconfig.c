/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_product_kconfig.c - the product record this image was built with,
 * from CONFIG_RADIANT_PRODUCT_* and the repository's VERSION file
 * (radiant/CMakeLists.txt passes RADIANT_BUILD_VERSION_* when it finds one,
 * and refuses a VERSION whose numbers page 81 cannot carry).
 *
 * The software revision goes through radiant_product_sw_rev_from_version(),
 * the encoder radiant/tests/ble tests, rather than a second copy of it here.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include <radiant/radiant_product.h>

#if defined(RADIANT_BUILD_VERSION_MAJOR)
#define VERSION_STRING                                                         \
	STRINGIFY(RADIANT_BUILD_VERSION_MAJOR) "."                              \
	STRINGIFY(RADIANT_BUILD_VERSION_MINOR) "."                              \
	STRINGIFY(RADIANT_BUILD_VERSION_PATCH)
/* The CMake parse refuses these already; repeated so a hand-passed define
 * cannot slip past either. */
#if defined(CONFIG_RADIANT_PRODUCT_SW_REV_FROM_VERSION)
BUILD_ASSERT(RADIANT_BUILD_VERSION_MINOR <= 9 &&
	     RADIANT_BUILD_VERSION_MAJOR * 10 + RADIANT_BUILD_VERSION_MINOR <= 255 &&
	     RADIANT_BUILD_VERSION_PATCH <= 99,
	     "VERSION does not fit page 81 (main = MAJOR*10+MINOR <= 255, "
	     "MINOR <= 9, PATCHLEVEL <= 99)");
#endif
#else
/* radiant/ used standalone, with no VERSION beside it. */
#define VERSION_STRING "0.0.0"
#define RADIANT_BUILD_VERSION_MAJOR 0
#define RADIANT_BUILD_VERSION_MINOR 0
#define RADIANT_BUILD_VERSION_PATCH 0
#endif

static struct radiant_product product = {
	.manufacturer_id = CONFIG_RADIANT_PRODUCT_MANUFACTURER_ID,
	.model_number = CONFIG_RADIANT_PRODUCT_MODEL_NUMBER,
	.hw_rev = CONFIG_RADIANT_PRODUCT_HW_REV,
#if !defined(CONFIG_RADIANT_PRODUCT_SW_REV_FROM_VERSION)
	.sw_rev_main = CONFIG_RADIANT_PRODUCT_SW_REV_MAIN,
	.sw_rev_supp = CONFIG_RADIANT_PRODUCT_SW_REV_SUPP,
#endif
	.manufacturer_name = CONFIG_RADIANT_PRODUCT_MANUFACTURER_NAME,
	.model_name = CONFIG_RADIANT_PRODUCT_MODEL_NAME,
	.version = VERSION_STRING,
};

static bool filled;

const struct radiant_product *radiant_product_get(void)
{
	/* Idempotent: two racing first calls write the same two bytes. */
	if (!filled) {
#if defined(CONFIG_RADIANT_PRODUCT_SW_REV_FROM_VERSION)
		radiant_product_sw_rev_from_version(RADIANT_BUILD_VERSION_MAJOR,
						    RADIANT_BUILD_VERSION_MINOR,
						    RADIANT_BUILD_VERSION_PATCH,
						    &product.sw_rev_main,
						    &product.sw_rev_supp);
#endif
		filled = true;
	}
	return &product;
}
