/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The TI backend advertises exactly what radiant/tests/common/caps_presets.h
 * says it does, the table fake_radio_caps_preset_cc26xx*() is built from. A
 * caps change in radiant_radio_cc26xx.c fails here until the table (and with
 * it the fake) follows. TODO "Backend drift" item 7.
 *
 * Runs in all three scenarios: plain and coex (3-byte sync word) and
 * short_match (the shipping 2-byte one). Checked after enable, which is
 * where the coex build patches its leads and window ceiling.
 */

#include <zephyr/ztest.h>

#include <radiant/radiant_radio_hal.h>

#include "backend_cc26xx_probe.h"
#include "../../common/caps_presets.h"

#if defined(BACKEND_SHORT_MATCH)
#define CC_SHORT_MATCH true
#else
#define CC_SHORT_MATCH false
#endif

#if defined(BACKEND_COEX)
#define CC_COEX true
#else
#define CC_COEX false
#endif

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	cc_reset(true);
}

ZTEST_SUITE(radiant_backend_cc26xx_caps_parity, NULL, NULL, before, NULL, NULL);

ZTEST(radiant_backend_cc26xx_caps_parity, test_caps_match_the_preset_table)
{
	const struct radiant_radio_caps want =
		caps_preset_cc26xx(CC_SHORT_MATCH, CC_COEX);
	const char *diff = caps_preset_diff(&want, radiant_radio_caps_get());

	zassert_is_null(diff, "caps.%s differs from caps_preset_cc26xx(%d, %d)",
			diff, CC_SHORT_MATCH, CC_COEX);
}
