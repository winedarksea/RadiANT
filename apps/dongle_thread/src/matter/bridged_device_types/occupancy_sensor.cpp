/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Package E5. See occupancy_sensor.h for the two traps this file exists to
 * hold still (19: not in the ZAP; 16: OccupancySensorType is not PIR).
 */

#include "occupancy_sensor.h"

namespace
{
DESCRIPTOR_CLUSTER_ATTRIBUTES(descriptorAttrs);
BRIDGED_DEVICE_BASIC_INFORMATION_CLUSTER_ATTRIBUTES(bridgedDeviceBasicAttrs);
}; /* namespace */

using namespace ::chip;
using namespace ::chip::app;
using namespace Nrf;

/*
 * Three attributes, and all three are mandatory for this cluster in Matter
 * 1.4: Occupancy is the reading, OccupancySensorType and
 * OccupancySensorTypeBitmap are the "what kind of sensor is this" pair. The
 * ClusterRevision row is appended by DECLARE_DYNAMIC_ATTRIBUTE_LIST_END() and
 * is answered from GetOccupancySensingClusterRevision().
 *
 * Every one of them carries ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE) whether asked
 * for or not - the macro ORs it in unconditionally - which is trap 5: there is
 * no "write it and the stack stores it" path on a dynamic endpoint and this
 * object IS the storage.
 */
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(occupancyAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Clusters::OccupancySensing::Attributes::Occupancy::Id, BITMAP8, 1, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(Clusters::OccupancySensing::Attributes::OccupancySensorType::Id, ENUM8, 1, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(Clusters::OccupancySensing::Attributes::OccupancySensorTypeBitmap::Id, BITMAP8, 1, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(Clusters::OccupancySensing::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
	DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

/*
 * Descriptor and BridgedDeviceBasicInformation are not optional on a bridged
 * endpoint and are declared through the vendored macros so that every device
 * type in this product declares them identically. Identify is NOT in this list
 * and that is not an omission: Nrf::MatterBridgedDevice::Init() registers it as
 * a CODE-DRIVEN cluster through CodegenDataModelProvider's registry, which is a
 * different mechanism from the ember attribute table this list describes.
 */
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedOccupancyClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::OccupancySensing::Id, occupancyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
	DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
	DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id, bridgedDeviceBasicAttrs,
				ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
	DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(bridgedOccupancyEndpoint, bridgedOccupancyClusters);

static constexpr EmberAfDeviceType kBridgedOccupancyDeviceTypes[] = {
	{ static_cast<chip::DeviceTypeId>(RadiantMatter::kDeviceTypeOccupancySensor),
	  RadiantMatter::kOccupancySensorDeviceVersion },
	{ static_cast<chip::DeviceTypeId>(MatterBridgedDevice::DeviceType::BridgedNode),
	  MatterBridgedDevice::kDefaultDynamicEndpointVersion }
};

static constexpr uint8_t kOccupancyDataVersionSize = MATTER_ARRAY_SIZE(bridgedOccupancyClusters);

OccupancySensorDevice::OccupancySensorDevice(const char *uniqueID, const char *nodeLabel)
	: RadiantMatter::RadiantBridgedDevice(uniqueID, nodeLabel)
{
	mDataVersionSize = kOccupancyDataVersionSize;
	mEp = &bridgedOccupancyEndpoint;
	mDeviceTypeList = kBridgedOccupancyDeviceTypes;
	mDeviceTypeListSize = ARRAY_SIZE(kBridgedOccupancyDeviceTypes);
	/* Out of CHIP's own heap (CONFIG_CHIP_MALLOC_SYS_HEAP_SIZE), not the
	 * kernel one - trap 10. A null here is reported by
	 * BridgeManager::CreateEndpoint() as CHIP_ERROR_NO_MEMORY rather than
	 * crashing, which is why this is not asserted. */
	mDataVersion = static_cast<DataVersion *>(chip::Platform::MemoryAlloc(sizeof(DataVersion) * mDataVersionSize));
}

CHIP_ERROR OccupancySensorDevice::HandleRead(ClusterId clusterId, AttributeId attributeId, uint8_t *buffer,
					     uint16_t maxReadLength)
{
	switch (clusterId) {
	case Clusters::OccupancySensing::Id:
		return HandleReadOccupancySensing(attributeId, buffer, maxReadLength);
	default:
		return CHIP_ERROR_INVALID_ARGUMENT;
	}
}

CHIP_ERROR OccupancySensorDevice::HandleReadOccupancySensing(AttributeId attributeId, uint8_t *buffer,
							     uint16_t maxReadLength)
{
	switch (attributeId) {
	case Clusters::OccupancySensing::Attributes::Occupancy::Id:
		return CopyAttribute(&mOccupancy, sizeof(mOccupancy), buffer, maxReadLength);
	case Clusters::OccupancySensing::Attributes::OccupancySensorType::Id:
		return CopyAttribute(&mOccupancySensorType, sizeof(mOccupancySensorType), buffer, maxReadLength);
	case Clusters::OccupancySensing::Attributes::OccupancySensorTypeBitmap::Id:
		return CopyAttribute(&mOccupancySensorTypeBitmap, sizeof(mOccupancySensorTypeBitmap), buffer,
				     maxReadLength);
	case Clusters::OccupancySensing::Attributes::ClusterRevision::Id: {
		uint16_t clusterRevision = GetOccupancySensingClusterRevision();
		return CopyAttribute(&clusterRevision, sizeof(clusterRevision), buffer, maxReadLength);
	}
	case Clusters::OccupancySensing::Attributes::FeatureMap::Id: {
		uint32_t featureMap = GetOccupancySensingFeatureMap();
		return CopyAttribute(&featureMap, sizeof(featureMap), buffer, maxReadLength);
	}
	default:
		return CHIP_ERROR_INVALID_ARGUMENT;
	}
}

CHIP_ERROR OccupancySensorDevice::HandleAttributeChange(chip::ClusterId clusterId, chip::AttributeId attributeId,
							void *data, size_t dataSize)
{
	if (!data) {
		return CHIP_ERROR_INVALID_ARGUMENT;
	}

	switch (clusterId) {
	case Clusters::BridgedDeviceBasicInformation::Id:
		/* The one cluster on the vendored contract - Reachable arrives
		 * as a bool. See radiant_bridged_device.h. */
		return HandleWriteDeviceBasicInformation(clusterId, attributeId, data, dataSize);
	case Clusters::OccupancySensing::Id: {
		int64_t v;

		/* All three are one byte wide. Refused rather than masked: a
		 * value outside 0..255 arriving here means radiant_matter.c's
		 * conversion table and this attribute's type disagree, which is
		 * a bug to be seen and not a reading to be truncated. */
		if (!Narrow(data, dataSize, 0, UINT8_MAX, v)) {
			return CHIP_ERROR_INVALID_ARGUMENT;
		}

		switch (attributeId) {
		case Clusters::OccupancySensing::Attributes::Occupancy::Id:
			mOccupancy = static_cast<uint8_t>(v);
			return CHIP_NO_ERROR;
		case Clusters::OccupancySensing::Attributes::OccupancySensorType::Id:
			mOccupancySensorType = static_cast<uint8_t>(v);
			return CHIP_NO_ERROR;
		case Clusters::OccupancySensing::Attributes::OccupancySensorTypeBitmap::Id:
			mOccupancySensorTypeBitmap = static_cast<uint8_t>(v);
			return CHIP_NO_ERROR;
		default:
			return CHIP_ERROR_INVALID_ARGUMENT;
		}
	}
	default:
		return CHIP_ERROR_INVALID_ARGUMENT;
	}
}
