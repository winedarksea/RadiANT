/* SPDX-License-Identifier: Apache-2.0 */

/*
 * ANT+ USB Dongle + Thread — main entry point.
 *
 * The boot sequence is apps/common/ant_dongle_main.c, byte-for-byte the same
 * one apps/dongle boots through; see apps/common/ant_dongle_main.h. What makes
 * this application different is the second wireless stack, and the whole of
 * that difference is the hook below.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ant_dongle_main.h"

#if defined(CONFIG_ANT_DONGLE_BLE_COEX_LOAD)
#include "ble_coex_load.h"
#endif

#if defined(CONFIG_ANT_DONGLE_THREAD_COEX_LOAD)
#include "thread_coex_load.h"
#endif

LOG_MODULE_REGISTER(ant_dongle_thread, LOG_LEVEL_INF);

/*
 * Overrides the weak default in apps/common/ant_dongle_main.c. Runs after
 * antr_init() has succeeded and before the transport comes up.
 *
 * AFTER antr_init() is the whole reason the hook is at that point rather than
 * in main(): the radio backend holds an explicit HFXO request that is
 * load-bearing - without it the 1 MHz timebase runs 0.42 % fast on the
 * internal RC between timeslots, invisible in any counter.
 *
 * Failures here are logged, not fatal, because both loads are bench
 * instruments: ANT+ with no second stack beats refusing to boot. The two are
 * legal together - that build is the three-stack case - so this is a second
 * `if`, not an `else`.
 *
 * The LOG_INF lines are not decoration. A weak override that failed to take
 * (a signature typo is the classic way) is otherwise completely silent, since
 * the default does nothing and boots fine; these lines are what proves on the
 * console that this file, and not the default, is what ran.
 */
void ant_dongle_post_radio_init(void)
{
#if defined(CONFIG_ANT_DONGLE_BLE_COEX_LOAD)
	LOG_INF("starting BLE coexistence load");
	(void)ble_coex_load_start();
#endif

#if defined(CONFIG_ANT_DONGLE_THREAD_COEX_LOAD)
	LOG_INF("starting Thread coexistence load");
	(void)thread_coex_load_start();
#endif

#if !defined(CONFIG_ANT_DONGLE_BLE_COEX_LOAD) && \
	!defined(CONFIG_ANT_DONGLE_THREAD_COEX_LOAD)
	/* No load configured. Say so anyway: this application without a
	 * coexistence load on the air is apps/dongle with extra flash, and a
	 * gate run against it measures arbitration against nothing.
	 */
	LOG_INF("no coexistence load configured");
#endif
}

int main(void)
{
	return ant_dongle_main_run();
}
