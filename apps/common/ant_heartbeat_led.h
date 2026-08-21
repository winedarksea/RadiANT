/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The mono led0 heartbeat: the last thing every ANT-serial boot sequence does.
 *
 * WHY THIS IS A SHARED FILE. It was a while(1) loop written out twice - once in
 * apps/common/ant_dongle_main.c for apps/dongle and apps/dongle_thread, once
 * again in apps/dongle_ti/src/main.c, which is a hand-written main() for a board
 * the shared boot sequence does not fit. The second copy is exactly the kind of
 * thing that file's own header warns about (see its note on the missing
 * ring_buf_init(), the port's longest-standing bug): a hand-written copy loses
 * things silently. One flash pattern, one off-switch, one implementation.
 */

#ifndef ANT_HEARTBEAT_LED_H_
#define ANT_HEARTBEAT_LED_H_

#include <zephyr/kernel.h>

#if defined(CONFIG_ANT_DONGLE_HEARTBEAT_LED)

/*
 * Flash led0 forever. NEVER RETURNS - it is the caller's whole remaining
 * program, called once the transport is up and there is nothing else to do.
 *
 * Boards with no `led0` alias, and boards whose led0 does not come ready, get a
 * thread that sleeps rather than a busy one; the caller cannot tell and does not
 * need to.
 */
void ant_heartbeat_led_run(void);

#else /* !CONFIG_ANT_DONGLE_HEARTBEAT_LED */

/*
 * The off-switch. An inline rather than a macro, unlike ant_activity_note_msg()
 * beside it: there is no argument to leave unevaluated, this is not on any hot
 * path, and the caller still needs a call that does not return.
 *
 * Defined here rather than left to the application so that apps/dongle_ti gets
 * a correct no-op even in a tree where it has not rsourced Kconfig.heartbeat_led
 * at all - an undefined symbol must not become a link error in a file this far
 * from the person who turned the feature off.
 */
static inline void ant_heartbeat_led_run(void)
{
	for (;;) {
		k_sleep(K_FOREVER);
	}
}

#endif /* CONFIG_ANT_DONGLE_HEARTBEAT_LED */

#endif /* ANT_HEARTBEAT_LED_H_ */
