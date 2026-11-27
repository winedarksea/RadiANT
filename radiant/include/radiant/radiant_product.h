/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_product.h - one product identity record feeding both radios.
 *
 * A mixed ANT+/BLE product states who made it, what model it is and which
 * revision it runs in two places: ANT+ common pages 80/81 (numbers) and the
 * Bluetooth Device Information Service (strings). Before this record every
 * application filled pages 80/81 from placeholder `1`s and BLE had no DIS at
 * all, so the two radios could not agree because neither said anything real.
 *
 * The record comes from Kconfig (CONFIG_RADIANT_PRODUCT_*, see
 * radiant/src/ble/Kconfig). Everything below the "kernel-free" line is a pure
 * function of its arguments, so radiant/tests/ble runs it on native_sim.
 *
 * Per-unit values (device number, serial) are not in the record: they come
 * from the node identity (radiant_node_ident.h) at boot. radiant_product_unit
 * carries them, and radiant_product_unit_boot() is the one place that decides
 * between the identity record and the development fallback number.
 */
#ifndef RADIANT_PRODUCT_H_
#define RADIANT_PRODUCT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct radiant_profile_common_id;

/* The ANT+ development manufacturer id. A shipped product needs its own. */
#define RADIANT_PRODUCT_DEV_MANUFACTURER_ID 255u

/* The Kconfig defaults, which strict mode refuses. Kept here, not only in
 * Kconfig, so the C check and the CMake check name the same strings. */
#define RADIANT_PRODUCT_PLACEHOLDER_MANUFACTURER_NAME "RadiANT development"
#define RADIANT_PRODUCT_PLACEHOLDER_MODEL_NAME        "RadiANT reference"
/* product.conf.example's values all start with this, and strict refuses any
 * string that still does. */
#define RADIANT_PRODUCT_TEMPLATE_PREFIX               "Your "

/* Longest string the helpers produce, terminator included. DIS strings are
 * further bounded by CONFIG_BT_DIS_STR_MAX where they are applied. */
#define RADIANT_PRODUCT_STR_MAX 32u

struct radiant_product {
	uint16_t    manufacturer_id;   /* page 80, 16 bits */
	uint16_t    model_number;      /* page 80, numeric */
	uint8_t     hw_rev;            /* page 80 */
	uint8_t     sw_rev_main;       /* page 81 byte 3 */
	uint8_t     sw_rev_supp;       /* page 81 byte 2, 0xFF = unused */
	const char *manufacturer_name; /* DIS Manufacturer Name String */
	const char *model_name;        /* DIS Model Number String */
	const char *version;           /* DIS Firmware/Software Revision String */
};

/* What a unit is, once its identity has booted. */
struct radiant_product_unit {
	uint16_t devnum;        /* ANT channel id device number */
	uint32_t serial;        /* page 81 serial and the DIS serial */
	bool     from_identity; /* false: the development fallback number */
};

/* The DIS strings, ready to hand to the stack. */
struct radiant_product_dis {
	char manufacturer[RADIANT_PRODUCT_STR_MAX];
	char model[RADIANT_PRODUCT_STR_MAX];
	char serial[RADIANT_PRODUCT_STR_MAX];
	char hw_rev[RADIANT_PRODUCT_STR_MAX];
	char fw_rev[RADIANT_PRODUCT_STR_MAX];
	char sw_rev[RADIANT_PRODUCT_STR_MAX];
};

/* Why a record is not shippable. Bits, so one check reports every reason. */
#define RADIANT_PRODUCT_BAD_MANUFACTURER_ID   0x01u /* 255, the development id */
#define RADIANT_PRODUCT_BAD_MANUFACTURER_NAME 0x02u /* empty or the placeholder */
#define RADIANT_PRODUCT_BAD_MODEL_NAME        0x04u /* empty or the placeholder */
#define RADIANT_PRODUCT_BAD_BLE_NAME          0x08u /* a default or template BLE name */
#define RADIANT_PRODUCT_BAD_NULL              0x80u /* no record */

/* ── Kernel-free ──────────────────────────────────────────────────────────── */

/*
 * Pages 80/81 from the record. `serial` is the unit's 32-bit serial
 * (radiant_product_unit.serial). Writes every field, so an uninitialised
 * struct never reaches the air. Returns 0, or -EINVAL for a NULL argument.
 */
int radiant_product_fill_common_id(const struct radiant_product *p,
				   uint32_t serial,
				   struct radiant_profile_common_id *id);

/*
 * The DIS strings. The serial is decimal, as a head unit shows page 81's;
 * the hardware revision is decimal; firmware and software revision are
 * p->version. Strings longer than RADIANT_PRODUCT_STR_MAX - 1 are truncated.
 * Returns 0, or -EINVAL for a NULL argument.
 */
int radiant_product_dis_strings(const struct radiant_product *p, uint32_t serial,
				struct radiant_product_dis *out);

/*
 * 0 when the record is shippable, else RADIANT_PRODUCT_BAD_* bits. A name is
 * refused when empty, a RadiANT placeholder, or still a product.conf.example
 * value (starts with RADIANT_PRODUCT_TEMPLATE_PREFIX). Strict builds refuse a
 * non-zero answer at configure time (radiant/CMakeLists.txt repeats these
 * checks); development builds log it at boot. The model number and hardware
 * revision are not checked: their placeholder 1 is also a legitimate value.
 */
uint32_t radiant_product_check(const struct radiant_product *p);

/*
 * RADIANT_PRODUCT_BAD_BLE_NAME when the advertised BLE name is empty,
 * Zephyr's default "Zephyr", one of this repository's development names
 * ("RadiANT HR", "RadiANT Treadmill"), or starts with the template prefix;
 * else 0.
 */
uint32_t radiant_product_check_ble_name(const char *name);

/*
 * The serial a node puts on the air: `serial`, or 0xFFFFFFFF ("not
 * supplied", page 81's sentinel) for a node with a privacy posture
 * (docs/radiant-security.md section 5.4, CONFIG_RADIANT_PRODUCT_PRIVACY).
 */
uint32_t radiant_product_public_serial(bool privacy, uint32_t serial);

/*
 * The development-fallback decision, separated from the kernel so it can be
 * tested. `boot_rc` is radiant_node_ident_boot()'s answer; `ident_devnum` and
 * `ident_serial` are what the identity reported (ignored when boot failed).
 *
 *   boot ok, devnum != 0   -> the identity's devnum and serial
 *   boot failed, !strict   -> `fallback_devnum`, serial = fallback_devnum
 *                             (today's behaviour), from_identity = false
 *   boot failed, strict    -> -EAGAIN: stay off-air and retry. A strict
 *                             build never transmits on the shared number.
 *
 * A booted identity that reports devnum 0 or a failed serial derivation is
 * treated as a failed boot. Returns 0 or -EAGAIN (or -EINVAL for NULL out).
 */
int radiant_product_unit_resolve(bool strict, int boot_rc, uint16_t ident_devnum,
				 int serial_rc, uint32_t ident_serial,
				 uint16_t fallback_devnum,
				 struct radiant_product_unit *out);

/*
 * The strict-mode retry delay after `attempt` failed boots (1-based):
 * 1 s doubling to one hour. Long, because each identity boot attempt may
 * write the boot counter to flash before it fails again.
 */
#define RADIANT_PRODUCT_RETRY_MAX_MS 3600000u
uint32_t radiant_product_retry_ms(uint32_t attempt);

/*
 * ANT+ page 81's software revision from a dotted version: main =
 * major * 10 + minor, supplemental = patch. A main that does not fit (minor
 * > 9, or major * 10 + minor > 255) gives 0xFF/0xFF; a patch > 99 gives
 * supplemental 0xFF (unused).
 */
void radiant_product_sw_rev_from_version(uint32_t major, uint32_t minor,
					 uint32_t patch, uint8_t *main_out,
					 uint8_t *supp_out);

/* ── Kconfig and kernel ───────────────────────────────────────────────────── */

/* The record this image was built with (CONFIG_RADIANT_PRODUCT_*). */
const struct radiant_product *radiant_product_get(void);

/*
 * Boot the node identity and decide the unit's devnum and serial
 * (radiant_product_unit_resolve()). In a strict build a failed boot does not
 * return: it logs, sleeps radiant_product_retry_ms() and retries, so the
 * caller - which has not opened any channel or started BLE yet - stays
 * off-air. In a development build it logs the failure at error level and
 * returns the fallback. Also logs radiant_product_check()'s findings in a
 * development build. Returns 0, or the boot error when it fell back.
 *
 * Needs CONFIG_RADIANT_NODE_IDENT.
 */
int radiant_product_unit_boot(uint16_t fallback_devnum,
			      struct radiant_product_unit *out);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PRODUCT_H_ */
