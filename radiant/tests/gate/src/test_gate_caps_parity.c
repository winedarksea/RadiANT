/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The MPSL gate's share of the nRF caps (its placement lead and window
 * ceiling) equals radiant/tests/common/caps_presets.h, which the backend's
 * own parity test (radiant/tests/backend/src/test_caps_parity.c) composes
 * with the hardware lead. TODO "Backend drift" item 7.
 */

#include <zephyr/ztest.h>

#include "radiant_radio_nrf_gate.h"

#include "../../common/caps_presets.h"

ZTEST_SUITE(radiant_gate_caps_parity, NULL, NULL, NULL, NULL, NULL);

ZTEST(radiant_gate_caps_parity, test_gate_lead_and_window_match_the_preset_table)
{
	zassert_equal(CAPS_PRESET_MPSL_GATE_LEAD_US, gate_min_arm_lead_us());
	zassert_equal(CAPS_PRESET_MPSL_MAX_WINDOW_US, gate_max_window_us());
}
