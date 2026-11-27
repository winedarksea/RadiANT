/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_product.c - the kernel-free half of <radiant/radiant_product.h>:
 * pages 80/81 and the DIS strings from one record, the shippability check
 * and the identity-fallback decision. No Zephyr header beyond libc, so
 * radiant/tests/ble compiles it unmodified on native_sim.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <radiant/radiant_product.h>
#include <radiant/radiant_profile_common.h>

int radiant_product_fill_common_id(const struct radiant_product *p,
				   uint32_t serial,
				   struct radiant_profile_common_id *id)
{
	if (p == NULL || id == NULL) {
		return -EINVAL;
	}
	memset(id, 0, sizeof(*id));
	id->hw_revision = p->hw_rev;
	id->manufacturer_id = p->manufacturer_id;
	id->model_number = p->model_number;
	id->sw_revision_main = p->sw_rev_main;
	id->sw_revision_supplemental = p->sw_rev_supp;
	id->serial_number = serial;
	return 0;
}

static void copy_str(char *dst, const char *src)
{
	size_t n = src == NULL ? 0u : strlen(src);

	if (n > RADIANT_PRODUCT_STR_MAX - 1u) {
		n = RADIANT_PRODUCT_STR_MAX - 1u;
	}
	if (n != 0u) {
		memcpy(dst, src, n);
	}
	dst[n] = '\0';
}

int radiant_product_dis_strings(const struct radiant_product *p, uint32_t serial,
				struct radiant_product_dis *out)
{
	if (p == NULL || out == NULL) {
		return -EINVAL;
	}
	memset(out, 0, sizeof(*out));
	copy_str(out->manufacturer, p->manufacturer_name);
	copy_str(out->model, p->model_name);
	(void)snprintf(out->serial, sizeof(out->serial), "%lu",
		       (unsigned long)serial);
	(void)snprintf(out->hw_rev, sizeof(out->hw_rev), "%u",
		       (unsigned int)p->hw_rev);
	copy_str(out->fw_rev, p->version);
	copy_str(out->sw_rev, p->version);
	return 0;
}

static bool placeholder(const char *s, const char *dflt)
{
	return s == NULL || s[0] == '\0' || strcmp(s, dflt) == 0;
}

uint32_t radiant_product_check(const struct radiant_product *p)
{
	uint32_t bad = 0u;

	if (p == NULL) {
		return RADIANT_PRODUCT_BAD_NULL;
	}
	if (p->manufacturer_id == RADIANT_PRODUCT_DEV_MANUFACTURER_ID) {
		bad |= RADIANT_PRODUCT_BAD_MANUFACTURER_ID;
	}
	if (placeholder(p->manufacturer_name,
			RADIANT_PRODUCT_PLACEHOLDER_MANUFACTURER_NAME)) {
		bad |= RADIANT_PRODUCT_BAD_MANUFACTURER_NAME;
	}
	if (placeholder(p->model_name, RADIANT_PRODUCT_PLACEHOLDER_MODEL_NAME)) {
		bad |= RADIANT_PRODUCT_BAD_MODEL_NAME;
	}
	return bad;
}

int radiant_product_unit_resolve(bool strict, int boot_rc, uint16_t ident_devnum,
				 int serial_rc, uint32_t ident_serial,
				 uint16_t fallback_devnum,
				 struct radiant_product_unit *out)
{
	if (out == NULL) {
		return -EINVAL;
	}
	if (boot_rc == 0 && ident_devnum != 0u && serial_rc == 0) {
		out->devnum = ident_devnum;
		out->serial = ident_serial;
		out->from_identity = true;
		return 0;
	}
	if (strict) {
		/* Leave `out` as it was: nothing here may be transmitted. */
		return -EAGAIN;
	}
	out->devnum = fallback_devnum;
	out->serial = fallback_devnum;
	out->from_identity = false;
	return 0;
}

uint32_t radiant_product_retry_ms(uint32_t attempt)
{
	uint32_t ms = 1000u;

	while (attempt > 1u && ms < 60000u) {
		ms *= 2u;
		attempt--;
	}
	return ms > 60000u ? 60000u : ms;
}

void radiant_product_sw_rev_from_version(uint32_t major, uint32_t minor,
					 uint32_t patch, uint8_t *main_out,
					 uint8_t *supp_out)
{
	if (major > 25u || minor > 9u || major * 10u + minor > 255u) {
		*main_out = 0xFFu;
		*supp_out = 0xFFu;
		return;
	}
	*main_out = (uint8_t)(major * 10u + minor);
	*supp_out = patch > 99u ? 0xFFu : (uint8_t)patch;
}
