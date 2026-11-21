/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The nRF backend advertises exactly what radiant/tests/common/caps_presets.h
 * says it does, the table fake_radio.c builds its hardware presets from. A
 * caps change in radiant_radio_nrf.c fails here until the table (and with it
 * the fake) follows. TODO "Backend drift" item 7.
 *
 * The MPSL row is the backend's share with the stub gate answering the MPSL
 * gate's lead and window ceiling; radiant.gate checks that the real gate
 * answers those numbers (test_gate_caps_parity.c).
 */

#include <zephyr/ztest.h>

#include <radiant/radiant_radio_hal.h>

#include "backend_probe.h"
#include "../../common/caps_presets.h"

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	backend_reset();
}

ZTEST_SUITE(radiant_backend_caps_parity, NULL, NULL, before, NULL, NULL);

ZTEST(radiant_backend_caps_parity, test_direct_caps_match_the_preset_table)
{
	const struct radiant_radio_caps want =
		caps_preset_nrf54l(backend_cfg_short_match, backend_cfg_ed_scan);
	const char *diff = caps_preset_diff(&want, radiant_radio_caps_get());

	zassert_is_null(diff, "caps.%s differs from caps_preset_nrf54l()", diff);
}

ZTEST(radiant_backend_caps_parity, test_mpsl_caps_match_the_preset_table)
{
	const struct radiant_radio_caps want =
		caps_preset_nrf54l_mpsl(backend_cfg_short_match,
					backend_cfg_ed_scan);
	const char *diff;

	backend_set_gate_caps(CAPS_PRESET_MPSL_GATE_LEAD_US,
			      CAPS_PRESET_MPSL_MAX_WINDOW_US);
	diff = caps_preset_diff(&want, radiant_radio_caps_get());
	backend_set_gate_caps(0u, 0u);
	(void)radiant_radio_caps_get();

	zassert_is_null(diff, "caps.%s differs from caps_preset_nrf54l_mpsl()",
			diff);
}
