/* SPDX-License-Identifier: Apache-2.0 */

/*
 * ANT+ USB Dongle — main entry point.
 *
 * The boot sequence itself lives in apps/common/ant_dongle_main.c, shared with
 * apps/dongle_thread; see apps/common/ant_dongle_main.h for why. This
 * application adds nothing to it — it is ANT+-only by design, and the weak
 * ant_dongle_post_radio_init() hook is deliberately left at its default no-op
 * here. The two coexistence loads that used to be #if'd into this file are in
 * apps/dongle_thread, which is the only application that links a second
 * wireless stack.
 */

#include "ant_dongle_main.h"

int main(void)
{
	return ant_dongle_main_run();
}
