/* SPDX-License-Identifier: Apache-2.0 */
/*
 * hr_profile_store.h - settings-backed storage for radiant_hr_profile
 * (docs/radiant-bridge.md section 6.5). See hr_profile_store.c.
 */

#ifndef HR_PROFILE_STORE_H_
#define HR_PROFILE_STORE_H_

#include <stdint.h>

#include <zephyr/sys/util.h>

#include <radiant/radiant_hr_profile.h>

#if defined(CONFIG_RADIANT_BRIDGE_RULES) && defined(CONFIG_SETTINGS) && \
	(CONFIG_RADIANT_BRIDGE_HR_PROFILES > 0)
#define HR_PROFILE_STORE_ENABLED 1
/* Register the settings store with radiant_hr_profile. Call once at boot,
 * after settings_subsys_init(), before the first sample is drained. */
void hr_profile_store_init(void);
#else
#define HR_PROFILE_STORE_ENABLED 0
static inline void hr_profile_store_init(void)
{
}
#endif

/* User eviction: erase the strap's stored profile and drop its slot. Safe
 * from any thread (radiant_hr_profile_forget() defers the RAM drop). */
static inline void hr_profile_store_forget(uint64_t uuid)
{
#if defined(CONFIG_RADIANT_BRIDGE_RULES)
	radiant_hr_profile_forget(uuid);
#else
	ARG_UNUSED(uuid);
#endif
}

#endif /* HR_PROFILE_STORE_H_ */
