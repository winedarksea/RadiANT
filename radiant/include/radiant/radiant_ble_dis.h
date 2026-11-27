/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ble_dis.h - the Device Information Service from radiant_product.
 *
 * CONFIG_RADIANT_BLE selects BT_DIS with BT_DIS_SETTINGS, and the Manufacturer
 * Name, Model Number, Serial Number and Firmware/Hardware/Software Revision
 * characteristics. Manufacturer and model are also the BT_DIS_*_STR Kconfig
 * defaults, so the stack's own BUILD_ASSERTs check that they fit
 * CONFIG_BT_DIS_STR_MAX; the per-unit serial and the revisions can only be
 * known at run time and are set here, through the settings runtime API
 * (nothing is written to flash).
 *
 * PnP ID is turned off by default under RADIANT_BLE: its stock values claim
 * Bluetooth SIG company id 0x0000, which belongs to somebody else. A product
 * with a real vendor id turns it back on in its own .conf.
 */
#ifndef RADIANT_BLE_DIS_H_
#define RADIANT_BLE_DIS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Set every DIS string from radiant_product_get() and the unit's serial.
 * Call after bt_enable() and after any settings_load() (which would replace
 * the values with stored ones), before advertising starts. Returns 0 or the
 * first settings_runtime_set() error; a failure leaves the Kconfig strings
 * in place and is not a reason to stay off the air.
 */
int radiant_ble_dis_apply(uint32_t serial);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_BLE_DIS_H_ */
