/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Persistent diagnostic log — see src/diag_flash_log.c.
 */

#ifndef ANT_DONGLE_DIAG_FLASH_LOG_H_
#define ANT_DONGLE_DIAG_FLASH_LOG_H_

#include <stddef.h>

#if defined(CONFIG_ANT_DONGLE_FLASH_LOG)

/*
 * Write everything logged so far to the diagnostic flash region. Safe to call
 * repeatedly (each call erases and rewrites); must be called from a thread,
 * never an ISR. Returns 0, or the first flash error encountered.
 */
int diag_flash_log_flush(void);

#else

static inline int diag_flash_log_flush(void)
{
	return 0;
}

#endif /* CONFIG_ANT_DONGLE_FLASH_LOG */

#endif /* ANT_DONGLE_DIAG_FLASH_LOG_H_ */
