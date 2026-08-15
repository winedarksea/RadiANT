/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Package E5: the Occupancy Sensor bridged device type.
 *
 * Written on nrf/applications/matter_bridge/src/bridged_device_types/
 * humidity_sensor.{h,cpp}'s pattern, which is the path
 * doc/adding_bridged_matter_device.rst documents - a class deriving from the
 * bridged-device base, a static DECLARE_DYNAMIC_* cluster list, a device-type
 * table and a HandleRead/HandleAttributeChange pair. It is NOT a copy of that
 * file: it serves a different cluster, it carries the constant-attribute
 * mechanism described below, and it follows this repository's own contract for
 * HandleAttributeChange (see radiant_bridged_device.h).
 *
 * ---------------------------------------------------------------------------
 * TRAP 19: OccupancySensing IS NOT IN THE ZAP, AND MUST NOT BE PUT THERE
 * ---------------------------------------------------------------------------
 *
 * This device type serves Occupancy, OccupancySensorType and
 * OccupancySensorTypeBitmap from the application's own shadow, on a DYNAMIC
 * endpoint, and the ZAP data model in src/matter/default_zap/ does not mention
 * the cluster at all. That is deliberate and it is not an oversight to be
 * "fixed" by the next person who opens the ZAP GUI:
 *
 *   If OccupancySensing is declared in bridge.zap, CHIP compiles
 *   src/app/clusters/occupancy-sensor-server/occupancy-sensor-server.cpp, and
 *   its init callback runs for EVERY endpoint the ember layer knows about -
 *   including every dynamic one - calling emberAfWriteAttribute() to seed
 *   defaults. On a dynamic endpoint every attribute is external storage by
 *   construction (attribute-storage.h ORs ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE)
 *   into DECLARE_DYNAMIC_ATTRIBUTE unconditionally), so that write routes
 *   straight back into emberAfExternalAttributeWriteCallback, i.e. into
 *   Nrf::BridgeManager::HandleWrite, i.e. into this class - at init time, with
 *   values nobody chose.
 *
 * Nothing is lost by leaving it out. Attribute ids come from
 * <app-common/zap-generated/ids/Clusters.h>, which is CHIP's complete cluster
 * list and is not filtered by the ZAP; the encoding of an external attribute
 * is driven by the EmberAfAttributeMetadata in this file. The same is true of
 * TemperatureMeasurement and RelativeHumidityMeasurement, which ARE in the ZAP
 * (they ride on endpoint 2's bridged-device cluster template - see
 * docs/matter-ram-budget.md) and which have no server sources in the tree
 * either way.
 *
 * ---------------------------------------------------------------------------
 * TRAP 16: OccupancySensorType IS A CONSTANT AND SOMEBODY HAS TO WRITE IT
 * ---------------------------------------------------------------------------
 *
 * Zero-initialised, OccupancySensorType means PIR, and
 * home-assistant/core#164839 proposes remapping PIR occupancy endpoints from
 * `device_class: occupancy` to `motion`. "Strap is worn" and "bike in use" are
 * not motion. On a dynamic endpoint there is no default to fall back on: an
 * attribute nobody writes reads back as whatever this object was constructed
 * with.
 *
 * THE VALUES ARE NOT HARDCODED HERE. radiant/src/bridge/radiant_matter.h
 * already states them - MATTER_OCCUPANCY_TYPE_PHYSICAL_CONTACT and
 * MATTER_OCCUPANCY_BITMAP_PHYSICAL_CONTACT - and radiant_matter.c already
 * emits them through its `extra[]` mechanism, where `mul == 0` means
 * "constant, written once when the endpoint is created". So they arrive here
 * as ordinary HandleAttributeChange calls on the first sample, exactly like
 * pressure's Scale, and this file's members are only their storage. A second
 * copy of the constants in this file would be a second thing to keep in step
 * with a controller-visible consequence and no test that compares them.
 *
 * The members are initialised to the physical-contact values anyway, and that
 * is belt-and-braces rather than duplication: a device RESTORED from
 * persistent storage exists before its first sample does, and until that
 * sample lands its OccupancySensorType would otherwise read 0.
 */

#pragma once

#include "radiant_bridged_device.h"

class OccupancySensorDevice : public RadiantMatter::RadiantBridgedDevice {
public:
	OccupancySensorDevice(const char *uniqueID, const char *nodeLabel);

	uint16_t GetDeviceType() const override { return RadiantMatter::kDeviceTypeOccupancySensor; }

	CHIP_ERROR HandleRead(chip::ClusterId clusterId, chip::AttributeId attributeId, uint8_t *buffer,
			      uint16_t maxReadLength) override;
	CHIP_ERROR HandleAttributeChange(chip::ClusterId clusterId, chip::AttributeId attributeId, void *data,
					 size_t dataSize) override;

	/* Cluster revision 5 and the PHY feature bit (bit 2 - PIR is 0,
	 * ultrasonic is 1). The feature map and OccupancySensorTypeBitmap say
	 * the same thing in two places because the cluster defines both and a
	 * controller may read either; they are set from one constant for that
	 * reason. */
	static constexpr uint16_t GetOccupancySensingClusterRevision() { return 5; }
	static constexpr uint32_t GetOccupancySensingFeatureMap() { return 0x04; }

private:
	CHIP_ERROR HandleReadOccupancySensing(chip::AttributeId attributeId, uint8_t *buffer, uint16_t maxReadLength);

	uint8_t mOccupancy = 0;
	/* See "TRAP 16" above: these initialisers are the pre-first-sample
	 * value of a restored endpoint, not the source of truth. 3 is
	 * "physical contact" in the enum, 0x04 the matching bitmap bit. */
	uint8_t mOccupancySensorType = 3;
	uint8_t mOccupancySensorTypeBitmap = 0x04;
};
