/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * CONFIG_TREADMILL_SOURCE_CUSTOM: THE FILE THAT IMPLEMENTS NOTHING.
 *
 * This is the integrator's starting point and it is deliberately empty of
 * behaviour. It mirrors the absence apps/hrm_ble expresses by compiling no
 * source file at all under CONFIG_HRM_SENSOR_CUSTOM; this application compiles
 * this one instead, for one reason worth the file: so that there is somewhere
 * to put the instructions, in the tree, at the place the compiler will point a
 * reader.
 *
 * ─── What happens when you build this arm ──────────────────────────────────
 *
 * It LINKS. That matters: an integrator's very first build failing for a
 * reason unrelated to their driver is an expensive way to start, so this arm
 * is in CI and in scripts/build_all.ps1.
 *
 * At boot, treadmill_source_start() returns -ENODEV, logs an error naming
 * CONFIG_TREADMILL_SOURCE_CUSTOM, and the node keeps running: both ANT+
 * channels open, both masters transmit, and every page reports a stopped belt
 * at zero grade in the READY state.
 *
 * THAT IS THE CORRECT FAILURE AND IT IS NOT A BUG. There is deliberately no
 * fallback to the simulator. A treadmill quietly reporting a simulated runner
 * while somebody stands on it is invisible from the receiving end; one
 * reporting a stopped belt is not, and on a gym dashboard it reads as
 * "unoccupied", which is the safe direction to be wrong in.
 *
 * ─── What you write ────────────────────────────────────────────────────────
 *
 * A file that looks like src/treadmill_sim.c with the physics replaced by your
 * hardware:
 *
 *   1. A `struct treadmill_source_api` with `start` (required), and
 *      `set_incline` / `set_speed` if your machine can move. Return -ENOTSUP
 *      from either if it cannot, and clear the matching capability bit -
 *      FE-C page 54 bit 2 and FTMS's Inclination Target Setting bit. Doing one
 *      without the other is what makes a controller report your machine as
 *      broken rather than as fixed-incline.
 *   2. A `struct treadmill_source` naming it. The name is logged at boot,
 *      verbatim, so it should be your machine's and not "custom".
 *   3. A SYS_INIT() at APPLICATION level calling
 *      treadmill_source_register().
 *   4. Calls to treadmill_source_report_speed(), _report_incline(),
 *      _report_stride() and _report_state() from wherever your tachometer,
 *      deck sensor and console live - INCLUDING FROM AN ISR, which is what the
 *      report path is built for. Read src/treadmill_source.h's rule 1 first;
 *      it is the non-obvious part.
 *
 * _report_state() is the one that is easy to leave out and the one that costs
 * the most: it is the FE state that FE-C page 16 carries, that a dongle
 * decodes, and that becomes a gym's occupancy signal. A machine that never
 * calls it reports READY forever and every dashboard reads zero.
 *
 * See docs/treadmill-reference-design.md for the full porting guide and the
 * production checklist.
 */

#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include "treadmill_source.h"

LOG_MODULE_DECLARE(treadmill, CONFIG_TREADMILL_LOG_LEVEL);

/*
 * Not a weak stub of anything, not a registration, not a fallback. The
 * translation unit exists so the build has a place to say this at boot as well
 * as in a comment, and because a linker that is handed no file at all cannot
 * be asked to warn about one.
 */
static int treadmill_source_custom_notice(void)
{
	LOG_WRN("CONFIG_TREADMILL_SOURCE_CUSTOM: no source is compiled in. "
		"Your driver must call treadmill_source_register() from its "
		"own SYS_INIT before main() reaches treadmill_source_start(); "
		"until it does, this node reports a stopped belt. See "
		"src/treadmill_source_custom.c and "
		"docs/treadmill-reference-design.md.");
	return 0;
}

/*
 * PRE_KERNEL_2 rather than APPLICATION, and one priority ahead of where a
 * driver's own registration would sit, so that this warning is on the console
 * BEFORE any integrator's SYS_INIT logs its own success. A warning that
 * appears after the thing it warns about is a warning nobody reads.
 */
SYS_INIT(treadmill_source_custom_notice, APPLICATION, 0);
