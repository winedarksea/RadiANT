/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Package E4's temperature, humidity and pressure bridged device types. See
 * measurement_sensor.h for why they share one class and three sets of static
 * metadata.
 */

#include "measurement_sensor.h"

namespace
{
DESCRIPTOR_CLUSTER_ATTRIBUTES(descriptorAttrs);
BRIDGED_DEVICE_BASIC_INFORMATION_CLUSTER_ATTRIBUTES(bridgedDeviceBasicAttrs);
}; /* namespace */

using namespace ::chip;
using namespace ::chip::app;
using namespace Nrf;

/* ── Temperature Measurement (0x0402) ───────────────────────────────────── */

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(tempSensorAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Id, INT16S, 2, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(Clusters::TemperatureMeasurement::Attributes::MinMeasuredValue::Id, INT16S, 2, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(Clusters::TemperatureMeasurement::Attributes::MaxMeasuredValue::Id, INT16S, 2, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(Clusters::TemperatureMeasurement::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
	DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedTempClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::TemperatureMeasurement::Id, tempSensorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
			nullptr),
	DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
	DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id, bridgedDeviceBasicAttrs,
				ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
	DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(bridgedTempEndpoint, bridgedTempClusters);

static constexpr EmberAfDeviceType kBridgedTempDeviceTypes[] = {
	{ static_cast<chip::DeviceTypeId>(RadiantMatter::kDeviceTypeTemperatureSensor),
	  RadiantMatter::kTemperatureSensorDeviceVersion },
	{ static_cast<chip::DeviceTypeId>(MatterBridgedDevice::DeviceType::BridgedNode),
	  MatterBridgedDevice::kDefaultDynamicEndpointVersion }
};

/* ── Relative Humidity Measurement (0x0405) ─────────────────────────────── */

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(humiSensorAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Clusters::RelativeHumidityMeasurement::Attributes::MeasuredValue::Id, INT16U, 2, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(Clusters::RelativeHumidityMeasurement::Attributes::MinMeasuredValue::Id, INT16U, 2,
				  0),
	DECLARE_DYNAMIC_ATTRIBUTE(Clusters::RelativeHumidityMeasurement::Attributes::MaxMeasuredValue::Id, INT16U, 2,
				  0),
	DECLARE_DYNAMIC_ATTRIBUTE(Clusters::RelativeHumidityMeasurement::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
	DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedHumidityClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::RelativeHumidityMeasurement::Id, humiSensorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
			nullptr),
	DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
	DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id, bridgedDeviceBasicAttrs,
				ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
	DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(bridgedHumidityEndpoint, bridgedHumidityClusters);

static constexpr EmberAfDeviceType kBridgedHumidityDeviceTypes[] = {
	{ static_cast<chip::DeviceTypeId>(RadiantMatter::kDeviceTypeHumiditySensor),
	  RadiantMatter::kHumiditySensorDeviceVersion },
	{ static_cast<chip::DeviceTypeId>(MatterBridgedDevice::DeviceType::BridgedNode),
	  MatterBridgedDevice::kDefaultDynamicEndpointVersion }
};

/* ── Pressure Measurement (0x0403) ──────────────────────────────────────── */

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(pressureSensorAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Clusters::PressureMeasurement::Attributes::MeasuredValue::Id, INT16S, 2, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(Clusters::PressureMeasurement::Attributes::MinMeasuredValue::Id, INT16S, 2, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(Clusters::PressureMeasurement::Attributes::MaxMeasuredValue::Id, INT16S, 2, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(Clusters::PressureMeasurement::Attributes::ScaledValue::Id, INT16S, 2, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(Clusters::PressureMeasurement::Attributes::Scale::Id, INT8S, 1, 0),
	DECLARE_DYNAMIC_ATTRIBUTE(Clusters::PressureMeasurement::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
	DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedPressureClusters)
DECLARE_DYNAMIC_CLUSTER(Clusters::PressureMeasurement::Id, pressureSensorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
			nullptr),
	DECLARE_DYNAMIC_CLUSTER(Clusters::Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
	DECLARE_DYNAMIC_CLUSTER(Clusters::BridgedDeviceBasicInformation::Id, bridgedDeviceBasicAttrs,
				ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
	DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(bridgedPressureEndpoint, bridgedPressureClusters);

static constexpr EmberAfDeviceType kBridgedPressureDeviceTypes[] = {
	{ static_cast<chip::DeviceTypeId>(RadiantMatter::kDeviceTypePressureSensor),
	  RadiantMatter::kPressureSensorDeviceVersion },
	{ static_cast<chip::DeviceTypeId>(MatterBridgedDevice::DeviceType::BridgedNode),
	  MatterBridgedDevice::kDefaultDynamicEndpointVersion }
};

/* ── The class ──────────────────────────────────────────────────────────── */

MeasurementSensorDevice::MeasurementSensorDevice(uint16_t deviceType, const char *uniqueID, const char *nodeLabel)
	: RadiantMatter::RadiantBridgedDevice(uniqueID, nodeLabel), mDeviceType(deviceType)
{
	switch (deviceType) {
	case RadiantMatter::kDeviceTypeTemperatureSensor:
		mEp = &bridgedTempEndpoint;
		mDeviceTypeList = kBridgedTempDeviceTypes;
		mDeviceTypeListSize = ARRAY_SIZE(kBridgedTempDeviceTypes);
		mDataVersionSize = MATTER_ARRAY_SIZE(bridgedTempClusters);
		/* -40.00 to +125.00 degC in the cluster's 0.01 degC units. The
		 * envelope of every ANT+ thermometer this product decodes, not
		 * the range of any one of them: the cluster asks what the
		 * endpoint is capable of reporting. */
		mMinMeasuredValue = -4000;
		mMaxMeasuredValue = 12500;
		break;
	case RadiantMatter::kDeviceTypeHumiditySensor:
		mEp = &bridgedHumidityEndpoint;
		mDeviceTypeList = kBridgedHumidityDeviceTypes;
		mDeviceTypeListSize = ARRAY_SIZE(kBridgedHumidityDeviceTypes);
		mDataVersionSize = MATTER_ARRAY_SIZE(bridgedHumidityClusters);
		/* 0.00 to 100.00 %RH in 0.01 % units - the whole of the
		 * quantity, so there is nothing narrower to claim. */
		mMinMeasuredValue = 0;
		mMaxMeasuredValue = 10000;
		break;
	case RadiantMatter::kDeviceTypePressureSensor:
		mEp = &bridgedPressureEndpoint;
		mDeviceTypeList = kBridgedPressureDeviceTypes;
		mDeviceTypeListSize = ARRAY_SIZE(kBridgedPressureDeviceTypes);
		mDataVersionSize = MATTER_ARRAY_SIZE(bridgedPressureClusters);
		/* Whole kPa, which is MeasuredValue's unit. 30 kPa is above the
		 * summit of Everest and 120 kPa below any recorded sea-level
		 * pressure; wider would be dishonest and narrower would clip a
		 * legitimate reading. */
		mMinMeasuredValue = 30;
		mMaxMeasuredValue = 120;
		break;
	default:
		/* mEp stays null. See the header: CreateEndpoint() rejects it. */
		return;
	}

	mDataVersion = static_cast<DataVersion *>(chip::Platform::MemoryAlloc(sizeof(DataVersion) * mDataVersionSize));
}

CHIP_ERROR MeasurementSensorDevice::HandleRead(ClusterId clusterId, AttributeId attributeId, uint8_t *buffer,
					       uint16_t maxReadLength)
{
	switch (clusterId) {
	case Clusters::TemperatureMeasurement::Id:
	case Clusters::RelativeHumidityMeasurement::Id:
		return HandleReadMeasurement(attributeId, buffer, maxReadLength);
	case Clusters::PressureMeasurement::Id:
		return HandleReadPressure(attributeId, buffer, maxReadLength);
	default:
		return CHIP_ERROR_INVALID_ARGUMENT;
	}
}

/*
 * Temperature and humidity share this: the three MeasuredValue attributes and
 * FeatureMap have the same ids in both clusters (0x0000/0x0001/0x0002 and
 * 0xFFFC), and the only thing that differs is signedness, which the ember
 * metadata above already states and which does not change the two bytes copied
 * out here. ClusterRevision is the one that has to know which cluster it is.
 */
CHIP_ERROR MeasurementSensorDevice::HandleReadMeasurement(AttributeId attributeId, uint8_t *buffer,
							  uint16_t maxReadLength)
{
	switch (attributeId) {
	case Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Id:
		return CopyAttribute(&mMeasuredValue, sizeof(mMeasuredValue), buffer, maxReadLength);
	case Clusters::TemperatureMeasurement::Attributes::MinMeasuredValue::Id:
		return CopyAttribute(&mMinMeasuredValue, sizeof(mMinMeasuredValue), buffer, maxReadLength);
	case Clusters::TemperatureMeasurement::Attributes::MaxMeasuredValue::Id:
		return CopyAttribute(&mMaxMeasuredValue, sizeof(mMaxMeasuredValue), buffer, maxReadLength);
	case Clusters::TemperatureMeasurement::Attributes::ClusterRevision::Id: {
		uint16_t rev = (mDeviceType == RadiantMatter::kDeviceTypeTemperatureSensor) ?
				       GetTemperatureMeasurementClusterRevision() :
				       GetRelativeHumidityMeasurementClusterRevision();
		return CopyAttribute(&rev, sizeof(rev), buffer, maxReadLength);
	}
	case Clusters::TemperatureMeasurement::Attributes::FeatureMap::Id: {
		uint32_t featureMap = 0;
		return CopyAttribute(&featureMap, sizeof(featureMap), buffer, maxReadLength);
	}
	default:
		return CHIP_ERROR_INVALID_ARGUMENT;
	}
}

CHIP_ERROR MeasurementSensorDevice::HandleReadPressure(AttributeId attributeId, uint8_t *buffer,
						       uint16_t maxReadLength)
{
	switch (attributeId) {
	case Clusters::PressureMeasurement::Attributes::MeasuredValue::Id:
		return CopyAttribute(&mMeasuredValue, sizeof(mMeasuredValue), buffer, maxReadLength);
	case Clusters::PressureMeasurement::Attributes::MinMeasuredValue::Id:
		return CopyAttribute(&mMinMeasuredValue, sizeof(mMinMeasuredValue), buffer, maxReadLength);
	case Clusters::PressureMeasurement::Attributes::MaxMeasuredValue::Id:
		return CopyAttribute(&mMaxMeasuredValue, sizeof(mMaxMeasuredValue), buffer, maxReadLength);
	case Clusters::PressureMeasurement::Attributes::ScaledValue::Id:
		return CopyAttribute(&mScaledValue, sizeof(mScaledValue), buffer, maxReadLength);
	case Clusters::PressureMeasurement::Attributes::Scale::Id:
		return CopyAttribute(&mScale, sizeof(mScale), buffer, maxReadLength);
	case Clusters::PressureMeasurement::Attributes::ClusterRevision::Id: {
		uint16_t rev = GetPressureMeasurementClusterRevision();
		return CopyAttribute(&rev, sizeof(rev), buffer, maxReadLength);
	}
	case Clusters::PressureMeasurement::Attributes::FeatureMap::Id: {
		uint32_t featureMap = kPressureFeatureMapExt;
		return CopyAttribute(&featureMap, sizeof(featureMap), buffer, maxReadLength);
	}
	default:
		return CHIP_ERROR_INVALID_ARGUMENT;
	}
}

CHIP_ERROR MeasurementSensorDevice::HandleAttributeChange(chip::ClusterId clusterId, chip::AttributeId attributeId,
							  void *data, size_t dataSize)
{
	int64_t v;

	if (!data) {
		return CHIP_ERROR_INVALID_ARGUMENT;
	}

	if (clusterId == Clusters::BridgedDeviceBasicInformation::Id) {
		return HandleWriteDeviceBasicInformation(clusterId, attributeId, data, dataSize);
	}

	if (clusterId == Clusters::PressureMeasurement::Id &&
	    attributeId == Clusters::PressureMeasurement::Attributes::Scale::Id) {
		if (!Narrow(data, dataSize, INT8_MIN, INT8_MAX, v)) {
			return CHIP_ERROR_INVALID_ARGUMENT;
		}
		mScale = static_cast<int8_t>(v);
		return CHIP_NO_ERROR;
	}

	if (clusterId != Clusters::TemperatureMeasurement::Id &&
	    clusterId != Clusters::RelativeHumidityMeasurement::Id && clusterId != Clusters::PressureMeasurement::Id) {
		return CHIP_ERROR_INVALID_ARGUMENT;
	}

	/*
	 * INT16S for temperature and pressure, INT16U for humidity, and the
	 * bound is taken from the DEVICE TYPE rather than from the cluster id
	 * so that a humidity value of -1 is refused rather than stored as
	 * 65535. Refused, not clamped: see radiant_bridged_device.h.
	 */
	const bool isUnsigned = (mDeviceType == RadiantMatter::kDeviceTypeHumiditySensor);

	if (!Narrow(data, dataSize, isUnsigned ? 0 : INT16_MIN, isUnsigned ? UINT16_MAX : INT16_MAX, v)) {
		return CHIP_ERROR_INVALID_ARGUMENT;
	}

	/* MeasuredValue is 0x0000 in all three clusters, ScaledValue is 0x0010
	 * in Pressure Measurement only. */
	if (attributeId == Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Id) {
		mMeasuredValue = static_cast<int16_t>(v);
		return CHIP_NO_ERROR;
	}
	if (clusterId == Clusters::PressureMeasurement::Id &&
	    attributeId == Clusters::PressureMeasurement::Attributes::ScaledValue::Id) {
		mScaledValue = static_cast<int16_t>(v);
		return CHIP_NO_ERROR;
	}

	return CHIP_ERROR_INVALID_ARGUMENT;
}
