/* SPDX-License-Identifier: Apache-2.0 */

/*
 * ant_reset_cause.h - why this boot happened, read once and kept.
 *
 * The hardware's reset-cause register (RESETREAS on nRF, RSTSRC on CC26xx)
 * latches until cleared, and more than one thing wants it: the UF2 settle in
 * ant_dongle_main.c, the boot log line, and the bridge's boot STARTUP message.
 * Each used to read and clear it for itself - or, in the settle's case, only
 * when CONFIG_ANT_DONGLE_UF2_SETTLE_MS was non-zero, so on every board without
 * a UF2 bootloader it was never cleared and accumulated causes across boots.
 * Now it is read and cleared exactly once, by the first caller, and every
 * caller sees that one value.
 */

#ifndef ANT_RESET_CAUSE_H_
#define ANT_RESET_CAUSE_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/hwinfo.h>

#include "ant_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The cause of this boot as Zephyr's RESET_* flags (zephyr/drivers/hwinfo.h).
 * The first call reads the hardware and clears it; later calls return the
 * cached value. Call it from the boot thread before anything else could.
 *
 * Returns false when the cause could not be read (no CONFIG_HWINFO, or a part
 * with no hwinfo driver); *cause is then 0. A true return with 0 is a real
 * reading: on nRF an empty RESETREAS means power-on.
 */
bool ant_reset_cause(uint32_t *cause);

/*
 * Map a cause onto MESG_STARTUP_MESG's reason byte. ANT's byte is a bitfield
 * with no bit for power-on (0x00 is "no flag set"), and a real ANT part reports
 * exactly these three hardware reasons:
 *
 *   watchdog, CPU lockup, software reset -> WATCH_DOG_RESET (0x02)
 *       the firmware reset itself or was reset for misbehaving: Zephyr's fatal
 *       error handler reboots with a software reset, so a crash lands here.
 *       A debugger's flash-and-reset also arrives as a software reset on nRF,
 *       which is why a fresh flash reports 0x02 - expected, not a fault.
 *   reset pin, debug/CTRL-AP reset -> HARDWARE_RESET_LINE (0x01)
 *       something outside asserted reset.
 *   everything else - power-on, brown-out, VBUS, wake from System OFF, or an
 *       unreadable cause -> POWER_ON_RESET (0x00). RESET_CLOCK is here too,
 *       because it means different things per driver: clock loss on CC26xx
 *       but a GRTC wake from System OFF on nRF54L. Calling the latter a
 *       watchdog would invent a fault.
 *
 * The order is the precedence: a register showing several causes reports the
 * one that most suggests a fault.
 */
static inline uint8_t ant_reset_cause_startup_reason(uint32_t cause)
{
	if (cause & (RESET_WATCHDOG | RESET_CPU_LOCKUP | RESET_SOFTWARE)) {
		return ANTW_STARTUP_WATCH_DOG_RESET;
	}
	if (cause & (RESET_PIN | RESET_DEBUG)) {
		return ANTW_STARTUP_HARDWARE_RESET_LINE;
	}
	return ANTW_STARTUP_POWER_ON_RESET;
}

#ifdef __cplusplus
}
#endif

#endif /* ANT_RESET_CAUSE_H_ */
