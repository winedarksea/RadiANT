/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_diag.c - storage for the 0xF7 diagnostic counters and the A0 cycle
 * clock. See radiant/include/radiant/radiant_diag.h. Built only with
 * CONFIG_RADIANT_DIAG_COUNTERS.
 *
 * Provenance: clean-room. See the header.
 */

#include <string.h>

#include <radiant/radiant_diag.h>

struct radiant_diag_core radiant_diag_core;

uint32_t radiant_diag_pass_now;

/* A0: cycles per microsecond, or 0 while the cycle clock is not usable. */
static uint32_t diag_cyc_per_us;

const struct radiant_diag_core *radiant_diag_core_get(void)
{
	return &radiant_diag_core;
}

void radiant_diag_core_reset(void)
{
	memset(&radiant_diag_core, 0, sizeof(radiant_diag_core));
}

#if defined(CONFIG_CPU_CORTEX_M_HAS_DWT)
/* Architectural addresses (ARMv7-M / ARMv8-M Mainline). */
#define DIAG_DEMCR         (*(volatile uint32_t *)0xE000EDFCu)
#define DIAG_DEMCR_TRCENA  (1u << 24)
#define DIAG_DWT_CTRL      (*(volatile uint32_t *)0xE0001000u)
#define DIAG_DWT_CYCCNTENA (1u << 0)
#endif

void radiant_diag_cycles_init(uint32_t core_hz)
{
	diag_cyc_per_us = 0u;
#if defined(CONFIG_CPU_CORTEX_M_HAS_DWT)
	if (core_hz >= 1000000u) {
		volatile uint32_t spin;
		uint32_t c0;

		DIAG_DEMCR |= DIAG_DEMCR_TRCENA;
		DIAG_DWT_CTRL |= DIAG_DWT_CYCCNTENA;
		c0 = radiant_diag_cycles();
		for (spin = 0u; spin < 64u; spin++) {
		}
		/* A counter that did not move (no CYCCNT, or one held off by the
		 * secure side or a debugger) times nothing. */
		if (radiant_diag_cycles() != c0) {
			diag_cyc_per_us = core_hz / 1000000u;
		}
	}
#else
	(void)core_hz;
#endif
}

uint32_t radiant_diag_cycles_to_us(uint32_t cycles)
{
	if (diag_cyc_per_us == 0u) {
		return RADIANT_DIAG_NOT_MEASURED;
	}
	return cycles / diag_cyc_per_us;
}
