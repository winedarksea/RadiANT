/* SPDX-License-Identifier: Apache-2.0 */
/*
 * caps_presets.h - what each real backend advertises, as one table.
 *
 * fake_radio.c builds its hardware presets from these, and the backend
 * harnesses (radiant/tests/backend, radiant/tests/backend_cc26xx, and
 * radiant/tests/gate for the MPSL gate's share) assert that the real
 * radiant_radio_caps_get() equals them. A backend caps change therefore fails
 * a parity test until this table follows it, and the fake follows the table.
 *
 * The values are the backends' own, as found in radiant_radio_nrf.c,
 * radiant_radio_nrf_gate_mpsl.c and radiant_radio_cc26xx.c (2026-11-20). Each
 * builder takes the Kconfig knobs that the real field is a function of, so a
 * harness built with a knob off still checks the row it really compiled.
 *
 * Not here: the fake's own presets fake_radio_caps_preset_nrf()/_rail(),
 * whose timing figures are deliberately placeholders, and anything the HAL
 * does not advertise (arm floors, delivery delays) - those are fake-only
 * models in fake_radio.c.
 */

#ifndef RADIANT_TESTS_COMMON_CAPS_PRESETS_H_
#define RADIANT_TESTS_COMMON_CAPS_PRESETS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <radiant/radiant_radio_hal.h>

/* ---- nRF54L15, direct gate (radiant_radio_nrf.c) ---- */

/* ARM_SETUP_US 80 + RAMP_UP_US 40 + (1 preamble + 5 address bytes) * 8 us. */
#define CAPS_PRESET_NRF54L_ARM_LEAD_US   168u
#define CAPS_PRESET_NRF54L_RAMP_UP_US    40u
#define CAPS_PRESET_NRF54L_TURNAROUND_US 40u

/* ---- The MPSL gate's share (radiant_radio_nrf_gate_mpsl.c) ---- */

/* gate_min_arm_lead_us(): PLACE_LEAD_US (Kconfig default 24000) +
 * HEAD_MARGIN_US 250. The backend adds its own ARM_LEAD_US on top. */
#define CAPS_PRESET_MPSL_GATE_LEAD_US    24250u
/* gate_max_window_us(): MPSL_TIMESLOT_LENGTH_MAX_US 100000 less
 * GRANT_OVERHEAD_US (250 head + 250 tail + 2000 extend end + 300 frame tail
 * + 3000 follow-on). */
#define CAPS_PRESET_MPSL_MAX_WINDOW_US   94200u

/* ---- CC26x2 (radiant_radio_cc26xx.c) ---- */

#define CAPS_PRESET_CC26XX_LEAD_US          600u
#define CAPS_PRESET_CC26XX_IN_GRANT_US      400u
/* Coex: MIN_ARM_LEAD_US 600 + CC26XX_COEX_PHY_SWITCH_LEAD_US 814. */
#define CAPS_PRESET_CC26XX_COEX_LEAD_US     1414u
#define CAPS_PRESET_CC26XX_COEX_IN_GRANT_US 600u
#define CAPS_PRESET_CC26XX_COEX_MAX_WINDOW  20000u
/* BYTE_US 8 + RX_END_SLOP_US (Kconfig default 500). */
#define CAPS_PRESET_CC26XX_CLOSE_HOLD_US    508u
#define CAPS_PRESET_CC26XX_RAMP_UP_US       150u
#define CAPS_PRESET_CC26XX_TURNAROUND_US    250u

static inline const enum radiant_phy *caps_preset_phys_1m(void)
{
	static const enum radiant_phy phys[] = { RADIANT_PHY_1M_GFSK };

	return phys;
}

/* nRF54L15 on the direct gate: what radiant_radio_nrf.c advertises with the
 * coded PHY off. */
static inline struct radiant_radio_caps caps_preset_nrf54l(bool short_match,
							   bool ed_scan)
{
	struct radiant_radio_caps c;

	memset(&c, 0, sizeof(c));
	c.name = "nrf54l RADIO (direct)";
	c.max_filters = 8u;
	c.max_addr_groups = 2u;
	c.min_filter_hamming_bits = 0u;
	c.filter_wildcard_dev = short_match;
	c.addr_len_hw_max = 5u;
	c.max_body_len = RADIANT_RADIO_BODY_MAX;
	c.phys = caps_preset_phys_1m();
	c.n_phys = 1u;
	c.phy_switch_us = 0u;
	c.ramp_up_us = CAPS_PRESET_NRF54L_RAMP_UP_US;
	c.rx_to_tx_us = CAPS_PRESET_NRF54L_TURNAROUND_US;
	c.tx_to_rx_us = CAPS_PRESET_NRF54L_TURNAROUND_US;
	c.min_arm_lead_us = CAPS_PRESET_NRF54L_ARM_LEAD_US;
	c.min_arm_lead_in_grant_us = CAPS_PRESET_NRF54L_ARM_LEAD_US;
	c.rx_close_hold_us = 0u;
	c.time_resolution_ns = 1000u;
	c.max_window_us = 0u;
	c.has_sync_timestamp = true;
	c.has_rssi = true;
	c.has_ed_scan = ed_scan;
	c.has_rx_crc = true;
	c.crc_in_hw = true;
	c.tx_power_min_dbm = -40;
	c.tx_power_max_dbm = 8;
	return c;
}

/* nRF54L15 on the MPSL gate (D2 off): the gate's placement lead is added to
 * min_arm_lead_us, the in-grant lead stays the hardware's, and windows are
 * capped by the timeslot ceiling. */
static inline struct radiant_radio_caps caps_preset_nrf54l_mpsl(bool short_match,
								bool ed_scan)
{
	struct radiant_radio_caps c = caps_preset_nrf54l(short_match, ed_scan);

	c.min_arm_lead_us = (uint16_t)(CAPS_PRESET_NRF54L_ARM_LEAD_US +
				       CAPS_PRESET_MPSL_GATE_LEAD_US);
	c.max_window_us = CAPS_PRESET_MPSL_MAX_WINDOW_US;
	return c;
}

/* CC26x2. short_match: CONFIG_RADIANT_SEARCH_SHORT_MATCH (default y), which
 * shortens the hardware sync word to two bytes and enables the wildcard.
 * coex: CONFIG_RADIANT_BACKEND_CC26XX_COEX, as patched by
 * radiant_radio_enable(). */
static inline struct radiant_radio_caps caps_preset_cc26xx(bool short_match,
							   bool coex)
{
	struct radiant_radio_caps c;

	memset(&c, 0, sizeof(c));
	c.name = "cc26xx";
	c.max_filters = 2u;
	c.max_addr_groups = 2u;
	c.min_filter_hamming_bits = 4u;
	c.filter_wildcard_dev = short_match;
	c.addr_len_hw_max = short_match ? 2u : 3u;
	c.max_body_len = RADIANT_RADIO_BODY_MAX;
	c.phys = caps_preset_phys_1m();
	c.n_phys = 1u;
	c.phy_switch_us = 0u;
	c.ramp_up_us = CAPS_PRESET_CC26XX_RAMP_UP_US;
	c.rx_to_tx_us = CAPS_PRESET_CC26XX_TURNAROUND_US;
	c.tx_to_rx_us = CAPS_PRESET_CC26XX_TURNAROUND_US;
	c.min_arm_lead_us = coex ? CAPS_PRESET_CC26XX_COEX_LEAD_US
				 : CAPS_PRESET_CC26XX_LEAD_US;
	c.min_arm_lead_in_grant_us = coex ? CAPS_PRESET_CC26XX_COEX_IN_GRANT_US
					  : CAPS_PRESET_CC26XX_IN_GRANT_US;
	c.rx_close_hold_us = CAPS_PRESET_CC26XX_CLOSE_HOLD_US;
	c.time_resolution_ns = 250u;
	c.max_window_us = coex ? CAPS_PRESET_CC26XX_COEX_MAX_WINDOW : 0u;
	c.has_sync_timestamp = true;
	c.has_rssi = true;
	c.has_ed_scan = false;
	c.has_rx_crc = true;
	c.crc_in_hw = false;
	c.tx_power_min_dbm = -20;
	c.tx_power_max_dbm = 5;
	return c;
}

/*
 * The first field in which got differs from want, or NULL if none does. name
 * is not compared (the fake names its presets "fake:..."), and phys is
 * compared by content, not by pointer. A field added to struct
 * radiant_radio_caps must be added here too.
 */
static inline const char *caps_preset_diff(const struct radiant_radio_caps *want,
					   const struct radiant_radio_caps *got)
{
#define CAPS_PRESET_CMP(f)                                                   \
	do {                                                                 \
		if (want->f != got->f) {                                     \
			return #f;                                           \
		}                                                            \
	} while (0)
	CAPS_PRESET_CMP(max_filters);
	CAPS_PRESET_CMP(max_addr_groups);
	CAPS_PRESET_CMP(min_filter_hamming_bits);
	CAPS_PRESET_CMP(filter_wildcard_dev);
	CAPS_PRESET_CMP(addr_len_hw_max);
	CAPS_PRESET_CMP(max_body_len);
	CAPS_PRESET_CMP(n_phys);
	for (uint8_t i = 0u; i < want->n_phys; i++) {
		if (want->phys == NULL || got->phys == NULL ||
		    want->phys[i] != got->phys[i]) {
			return "phys";
		}
	}
	CAPS_PRESET_CMP(phy_switch_us);
	CAPS_PRESET_CMP(ramp_up_us);
	CAPS_PRESET_CMP(rx_to_tx_us);
	CAPS_PRESET_CMP(tx_to_rx_us);
	CAPS_PRESET_CMP(min_arm_lead_us);
	CAPS_PRESET_CMP(min_arm_lead_in_grant_us);
	CAPS_PRESET_CMP(rx_close_hold_us);
	CAPS_PRESET_CMP(time_resolution_ns);
	CAPS_PRESET_CMP(max_window_us);
	CAPS_PRESET_CMP(has_sync_timestamp);
	CAPS_PRESET_CMP(has_rssi);
	CAPS_PRESET_CMP(has_ed_scan);
	CAPS_PRESET_CMP(has_rx_crc);
	CAPS_PRESET_CMP(crc_in_hw);
	CAPS_PRESET_CMP(tx_power_min_dbm);
	CAPS_PRESET_CMP(tx_power_max_dbm);
#if defined(CONFIG_RADIANT_SCHED_BOOKING)
	CAPS_PRESET_CMP(booking);
	CAPS_PRESET_CMP(min_arm_lead_elastic_us);
	CAPS_PRESET_CMP(scan_tail_us);
#endif
#undef CAPS_PRESET_CMP
	return NULL;
}

#endif /* RADIANT_TESTS_COMMON_CAPS_PRESETS_H_ */
