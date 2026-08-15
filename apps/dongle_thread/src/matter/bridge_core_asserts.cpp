/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * A translation unit whose entire content is static_asserts.
 *
 * It emits no code, defines no symbol and is discarded by --gc-sections
 * without leaving a byte in the image. That is the point: it is a compile-time
 * check that lives in the only place that can see both sides of it.
 *
 * WHY IT IS A SEPARATE FILE. The numbers it compares live on opposite sides of
 * the language boundary. CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT comes from
 * src/matter/chip_project_config.h, which CHIP's own headers pull in before
 * anything of RadiANT's exists, so it cannot include radiant_matter.h.
 * RADIANT_MATTER_MAX_ENDPOINTS lives in radiant/src/bridge/radiant_matter.h,
 * which this repository must not edit from here and which is a C header
 * compiled into C translation units. A third file that includes both is the
 * only place the comparison can be written, and radiant_matter.h:168 already
 * says "the glue static_asserts the three together" - this is that glue,
 * brought forward from package E4 to E3 because it costs nothing now and
 * because the failure it catches is invisible at runtime.
 *
 * WHY IT IS AT E3 AT ALL, when nothing constructs a bridged device yet: see
 * trap 15 in the Matter plan and the long note in chip_project_config.h. Too
 * small a dynamic-endpoint count does not fail the build, does not log, and
 * does not return an error - it produces an Nth bridged endpoint with no
 * Descriptor cluster. There is no runtime check that would find it and no
 * bench observation short of enumerating sixteen sensors that would provoke
 * it.
 */

/* Nrf::BridgeManager::kMaxBridgedDevices is the number the bridge core itself
 * uses to size its device map, and it is defined as
 * CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT. Including this header rather than
 * <platform/CHIPDeviceConfig.h> alone means the assertion is made against the
 * value the VENDORED CORE actually uses, not against a macro that some later
 * edit could stop feeding it. It also makes this file a compile check on the
 * vendored header itself. */
#include "bridge_manager.h"

#include <zephyr/devicetree.h>

/* ---------------------------------------------------------------------------
 * Trap 15: the three numbers that must agree.
 * ------------------------------------------------------------------------- */

/* radiant_matter.h is a C header. It is included last and unguarded on
 * purpose: if RadiANT's bridge is not in this image there is nothing to agree
 * with, and CMake only compiles this file when CONFIG_RADIANT_BRIDGE is set. */
#include "radiant_matter.h"

static_assert(CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT == RADIANT_MATTER_MAX_ENDPOINTS,
	      "CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT (src/matter/chip_project_config.h, which "
	      "defines it as CONFIG_BRIDGE_MAX_DYNAMIC_ENDPOINTS_NUMBER) and RADIANT_MATTER_MAX_ENDPOINTS "
	      "(radiant/src/bridge/radiant_matter.h) disagree. Trap 15: the CHIP side sizes the "
	      "Descriptor cluster's per-endpoint server array as fixed+dynamic, so a CHIP side that is "
	      "SMALLER gives the Nth bridged endpoint no Descriptor at all - enumerable, no cluster "
	      "list, no warning, no error. A CHIP side that is LARGER wastes a DataVersion array and a "
	      "Descriptor slot per unreachable endpoint. Change all three or none.");

static_assert(Nrf::BridgeManager::kMaxBridgedDevices == RADIANT_MATTER_MAX_ENDPOINTS,
	      "The vendored BridgeManager's device-map size and RADIANT_MATTER_MAX_ENDPOINTS disagree. "
	      "This is the same trap as above seen from the core's side: BridgeManager would accept "
	      "more (or fewer) bridged devices than radiant_matter.c will ever allocate endpoints for.");

/* Not a trap, a sanity floor. CONFIG_BRIDGE_AGGREGATOR_ENDPOINT_ID is 1
 * because default_zap/bridge.zap puts the Aggregator device type on endpoint 1;
 * the two are not connected by anything the compiler can see, and a mismatch
 * would make BridgeManager parent every dynamic endpoint to an endpoint that
 * does not exist. Endpoint 0 is always the root node, so 0 is never right. */
static_assert(Nrf::BridgeManager::kAggregatorEndpointId != 0,
	      "The aggregator cannot be endpoint 0; endpoint 0 is the Matter root node. See "
	      "CONFIG_BRIDGE_AGGREGATOR_ENDPOINT_ID in src/matter/Kconfig and the Aggregator "
	      "endpoint in default_zap/bridge.zap.");
