/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_product_boot.c - boot the node identity and decide the unit's
 * device number and serial, honouring CONFIG_RADIANT_PRODUCT_STRICT.
 *
 * The decision is radiant_product_unit_resolve() (kernel-free, tested on
 * native_sim). What this file adds is the retry loop a strict build needs:
 * the caller has opened no channel and started no BLE yet, so not returning
 * IS staying off-air. A strict build must never fall back to the shared
 * Kconfig device number - every unit built from one image would then share
 * it, and a gym or a race full of identical sensors is the failure the
 * identity record exists to prevent.
 *
 * Compiled only with CONFIG_RADIANT_NODE_IDENT: radiant_node_ident.c lives in
 * the application (ADR 0009 decision 7) and is linked by it.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <radiant/radiant_node_ident.h>
#include <radiant/radiant_product.h>

LOG_MODULE_REGISTER(radiant_product, LOG_LEVEL_INF);

static void log_check(void)
{
	uint32_t bad = radiant_product_check(radiant_product_get());

	if (bad == 0u) {
		return;
	}
	/* A strict build cannot get here: radiant/CMakeLists.txt refused it. */
	LOG_WRN("development product identity (0x%02x):%s%s%s - not for "
		"shipping; see CONFIG_RADIANT_PRODUCT_STRICT", bad,
		(bad & RADIANT_PRODUCT_BAD_MANUFACTURER_ID) ? " manufacturer id 255" : "",
		(bad & RADIANT_PRODUCT_BAD_MANUFACTURER_NAME) ? " placeholder manufacturer name" : "",
		(bad & RADIANT_PRODUCT_BAD_MODEL_NAME) ? " placeholder model name" : "");
}

int radiant_product_unit_boot(uint16_t fallback_devnum,
			      struct radiant_product_unit *out)
{
	const bool strict = IS_ENABLED(CONFIG_RADIANT_PRODUCT_STRICT);
	uint32_t attempt = 0u;

	if (out == NULL) {
		return -EINVAL;
	}
	if (!strict) {
		log_check();
	}

	for (;;) {
		uint16_t devnum = 0u;
		uint32_t serial = 0u;
		int serial_rc = -1;
		int rc;
		int boot_rc = radiant_node_ident_boot();

		if (boot_rc == 0) {
			(void)radiant_node_ident_devnum(&devnum);
			serial_rc = radiant_node_ident_serial32(&serial);
		}

		rc = radiant_product_unit_resolve(strict, boot_rc, devnum, serial_rc,
						  serial, fallback_devnum, out);
		if (rc == 0 && out->from_identity) {
			LOG_INF("device number %u, serial %u, from the identity record",
				out->devnum, out->serial);
			return 0;
		}
		if (rc == 0) {
			LOG_ERR("identity boot failed: %s (%d); falling back to "
				"Kconfig device number %u, no transforms - every unit "
				"built from this image shares it",
				radiant_node_ident_strerror(boot_rc), boot_rc,
				fallback_devnum);
			return boot_rc != 0 ? boot_rc : -EIO;
		}

		attempt++;
		uint32_t ms = radiant_product_retry_ms(attempt);

		LOG_ERR("identity boot failed: %s (%d); strict identity: staying "
			"off the air, retry %u in %u ms",
			radiant_node_ident_strerror(boot_rc), boot_rc, attempt, ms);
		k_msleep((int32_t)ms);
	}
}
