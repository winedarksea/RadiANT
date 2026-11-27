/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_product_kconfig.c - the product record this image was built with,
 * from CONFIG_RADIANT_PRODUCT_* and the repository's VERSION file
 * (radiant/CMakeLists.txt passes RADIANT_BUILD_VERSION_* when it finds one).
 */

#include <stdint.h>

#include <zephyr/sys/util.h>

#include <radiant/radiant_product.h>

#if defined(RADIANT_BUILD_VERSION_MAJOR)
#define VERSION_STRING                                                         \
	STRINGIFY(RADIANT_BUILD_VERSION_MAJOR) "."                              \
	STRINGIFY(RADIANT_BUILD_VERSION_MINOR) "."                              \
	STRINGIFY(RADIANT_BUILD_VERSION_PATCH)
#else
/* radiant/ used standalone, with no VERSION beside it. */
#define VERSION_STRING "0.0.0"
#define RADIANT_BUILD_VERSION_MAJOR 0
#define RADIANT_BUILD_VERSION_MINOR 0
#define RADIANT_BUILD_VERSION_PATCH 0
#endif

#if defined(CONFIG_RADIANT_PRODUCT_SW_REV_FROM_VERSION)
/* main = major * 10 + minor, supplemental = patch: see
 * radiant_product_sw_rev_from_version(), which is the same arithmetic. */
#define SW_MAIN                                                                \
	((RADIANT_BUILD_VERSION_MAJOR * 10 + RADIANT_BUILD_VERSION_MINOR) > 255 || \
	 RADIANT_BUILD_VERSION_MINOR > 9                                        \
		 ? 0xFF                                                         \
		 : (RADIANT_BUILD_VERSION_MAJOR * 10 + RADIANT_BUILD_VERSION_MINOR))
#define SW_SUPP                                                                \
	((RADIANT_BUILD_VERSION_MAJOR * 10 + RADIANT_BUILD_VERSION_MINOR) > 255 || \
	 RADIANT_BUILD_VERSION_MINOR > 9 || RADIANT_BUILD_VERSION_PATCH > 99    \
		 ? 0xFF                                                         \
		 : RADIANT_BUILD_VERSION_PATCH)
#else
#define SW_MAIN CONFIG_RADIANT_PRODUCT_SW_REV_MAIN
#define SW_SUPP CONFIG_RADIANT_PRODUCT_SW_REV_SUPP
#endif

static const struct radiant_product product = {
	.manufacturer_id = CONFIG_RADIANT_PRODUCT_MANUFACTURER_ID,
	.model_number = CONFIG_RADIANT_PRODUCT_MODEL_NUMBER,
	.hw_rev = CONFIG_RADIANT_PRODUCT_HW_REV,
	.sw_rev_main = (uint8_t)SW_MAIN,
	.sw_rev_supp = (uint8_t)SW_SUPP,
	.manufacturer_name = CONFIG_RADIANT_PRODUCT_MANUFACTURER_NAME,
	.model_name = CONFIG_RADIANT_PRODUCT_MODEL_NAME,
	.version = VERSION_STRING,
};

const struct radiant_product *radiant_product_get(void)
{
	return &product;
}
