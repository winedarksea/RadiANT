/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ble_dis.c - the Device Information Service strings from
 * radiant_product, applied through the settings runtime API into Zephyr's
 * DIS (subsys/bluetooth/services/dis.c, BT_DIS_SETTINGS). Nothing is stored:
 * settings_runtime_set() calls the "bt/dis" handler directly.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <radiant/radiant_ble_dis.h>
#include <radiant/radiant_product.h>

LOG_MODULE_DECLARE(radiant_ble_adv, CONFIG_RADIANT_BLE_LOG_LEVEL);

/* dis.c truncates a value to CONFIG_BT_DIS_STR_MAX in silence; refuse a
 * product string that would be cut, rather than show a phone half a name. */
BUILD_ASSERT(sizeof(CONFIG_RADIANT_PRODUCT_MANUFACTURER_NAME) - 1u <=
	     CONFIG_BT_DIS_STR_MAX,
	     "CONFIG_RADIANT_PRODUCT_MANUFACTURER_NAME is longer than "
	     "CONFIG_BT_DIS_STR_MAX and DIS would truncate it");
BUILD_ASSERT(sizeof(CONFIG_RADIANT_PRODUCT_MODEL_NAME) - 1u <=
	     CONFIG_BT_DIS_STR_MAX,
	     "CONFIG_RADIANT_PRODUCT_MODEL_NAME is longer than "
	     "CONFIG_BT_DIS_STR_MAX and DIS would truncate it");
/* And radiant_product_dis_strings()' own buffers, which truncate at
 * RADIANT_PRODUCT_STR_MAX - 1 whatever CONFIG_BT_DIS_STR_MAX allows. */
BUILD_ASSERT(sizeof(CONFIG_RADIANT_PRODUCT_MANUFACTURER_NAME) <= RADIANT_PRODUCT_STR_MAX,
	     "CONFIG_RADIANT_PRODUCT_MANUFACTURER_NAME is longer than "
	     "RADIANT_PRODUCT_STR_MAX - 1 and would be truncated");
BUILD_ASSERT(sizeof(CONFIG_RADIANT_PRODUCT_MODEL_NAME) <= RADIANT_PRODUCT_STR_MAX,
	     "CONFIG_RADIANT_PRODUCT_MODEL_NAME is longer than "
	     "RADIANT_PRODUCT_STR_MAX - 1 and would be truncated");
/* The longest generated string: a 10-digit serial. */
BUILD_ASSERT(CONFIG_BT_DIS_STR_MAX >= 10, "a decimal 32-bit serial needs 10");

static int set_one(const char *key, const char *val, int first_err)
{
	int err = settings_runtime_set(key, val, strlen(val));

	if (err != 0) {
		LOG_ERR("DIS %s: settings_runtime_set %d", key, err);
		return first_err != 0 ? first_err : err;
	}
	return first_err;
}

int radiant_ble_dis_apply(uint32_t serial)
{
	struct radiant_product_dis d;
	int err = radiant_product_dis_strings(radiant_product_get(), serial, &d);

	if (err != 0) {
		return err;
	}

	err = set_one("bt/dis/manuf", d.manufacturer, 0);
	err = set_one("bt/dis/model", d.model, err);
#if defined(CONFIG_BT_DIS_SERIAL_NUMBER)
	/* Absent with CONFIG_RADIANT_PRODUCT_PRIVACY: a node with a privacy
	 * posture publishes no serial on either radio. */
	err = set_one("bt/dis/serial", d.serial, err);
#endif
	err = set_one("bt/dis/fw", d.fw_rev, err);
	err = set_one("bt/dis/hw", d.hw_rev, err);
	err = set_one("bt/dis/sw", d.sw_rev, err);

	if (err == 0) {
		LOG_INF("DIS: %s %s serial %s fw %s hw %s", d.manufacturer,
			d.model,
			IS_ENABLED(CONFIG_BT_DIS_SERIAL_NUMBER) ? d.serial : "(none)",
			d.fw_rev, d.hw_rev);
	}
	return err;
}
