/* SPDX-License-Identifier: Apache-2.0 */

/* See ant_reset_cause.h. */

#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>

#include "ant_reset_cause.h"

static bool read_done;
static bool read_ok;
static uint32_t cached;

bool ant_reset_cause(uint32_t *cause)
{
	if (!read_done) {
		read_done = true;
#if defined(CONFIG_HWINFO)
		/* Zephyr's weak z_impl_hwinfo_get_reset_cause() answers -ENOSYS
		 * on a part with no driver, and the hwinfo API is only linked
		 * when CONFIG_HWINFO is set - hence the #if rather than
		 * IS_ENABLED(), which would leave an unresolved symbol.
		 */
		uint32_t value = 0;

		if (hwinfo_get_reset_cause(&value) == 0) {
			cached = value;
			read_ok = true;
			/* The register latches until cleared; left alone, the
			 * next boot would report this cause on top of its own.
			 */
			(void)hwinfo_clear_reset_cause();
		}
#endif
	}

	*cause = cached;
	return read_ok;
}
