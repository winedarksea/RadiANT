/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Package E4's other three bridged device types: Temperature Sensor (0x0302),
 * Humidity Sensor (0x0307) and Pressure Sensor (0x0305) - plus Flow Sensor
 * (0x0306), which exists only as docs/radiant-bridge.md section 8.3b's
 * default-off raw-bpm passenger (radiant_matter_set_hr_passenger()). Home
 * Assistant displays exactly the bpm figure (unit shown: m^3/h).
 * MeasuredValue holds bpm x 10 because the cluster's unit is 0.1 m^3/h and
 * HA divides by 10. Deliberately mislabelled; nothing else in this product
 * produces a flow reading.
 *
 * ONE CLASS FOR THREE DEVICE TYPES, AND THAT IS A DELIBERATE DEPARTURE FROM
 * UPSTREAM'S ONE-FILE-PER-TYPE SHAPE. nrf/applications/matter_bridge has a
 * temperature_sensor.cpp and a humidity_sensor.cpp that differ in a cluster
 * id, three attribute ids and two Kconfig-supplied bounds; copying that shape
 * for a third would be three near-identical files. What genuinely differs
 * between the three is the STATIC METADATA - the cluster list, the attribute
 * list and the device-type table, which the DECLARE_DYNAMIC_* macros can only
 * produce at file scope - so there are three sets of those in the .cpp and one
 * class that points `mEp` at whichever the constructor was asked for. That is
 * the same mechanism upstream uses, with the switch moved from the linker to a
 * constructor argument.
 *
 * Occupancy is NOT here, and not for symmetry's sake: it is package E5's own
 * deliverable, it carries two constant attributes with a trap attached to each
 * (see occupancy_sensor.h), and folding it in would bury that argument.
 *
 * PRESSURE IS THE ONE WITH THREE ATTRIBUTES. Pressure Measurement's mandatory
 * MeasuredValue is an int16s in 0.1 kPa (Matter 1.5.1 section 2.4.5.1), a
 * 100 Pa step against a sensor that resolves 10 Pa. ScaledValue + Scale is the
 * cluster's own answer, in PASCALS x 10^Scale (section 2.4.5.5 - a different
 * base unit from MeasuredValue's, which is the trap), and needs the EXT
 * feature bit; radiant_matter.c's pressure row already writes all three (Scale
 * through the `mul == 0` constant mechanism), so this file's job is to have
 * somewhere to put them, to declare the feature bit that makes them legal,
 * and to serve Min/MaxScaledValue, which EXT makes mandatory and which are
 * constants here for the same reason Min/MaxMeasuredValue are.
 *
 * See radiant_bridged_device.h for the HandleAttributeChange argument contract
 * (every measurement arrives as an int64_t and is refused, not saturated, if
 * it does not fit).
 */

#pragma once

#include "radiant_bridged_device.h"

class MeasurementSensorDevice : public RadiantMatter::RadiantBridgedDevice {
public:
	/* `deviceType` must be one of RadiantMatter::kDeviceType{Temperature,
	 * Humidity,Pressure,Flow}Sensor. Anything else leaves mEp null, which
	 * Nrf::BridgeManager::CreateEndpoint() rejects with
	 * CHIP_ERROR_NO_MEMORY rather than dereferencing - checked there, so
	 * this constructor does not need to abort. */
	MeasurementSensorDevice(uint16_t deviceType, const char *uniqueID, const char *nodeLabel);

	uint16_t GetDeviceType() const override { return mDeviceType; }

	CHIP_ERROR HandleRead(chip::ClusterId clusterId, chip::AttributeId attributeId, uint8_t *buffer,
			      uint16_t maxReadLength) override;
	CHIP_ERROR HandleAttributeChange(chip::ClusterId clusterId, chip::AttributeId attributeId, void *data,
					 size_t dataSize) override;

	/* Cluster revisions, from the Matter 1.4 Application Clusters
	 * specification. */
	static constexpr uint16_t GetTemperatureMeasurementClusterRevision() { return 4; }
	static constexpr uint16_t GetRelativeHumidityMeasurementClusterRevision() { return 3; }
	static constexpr uint16_t GetPressureMeasurementClusterRevision() { return 3; }
	static constexpr uint16_t GetFlowMeasurementClusterRevision() { return 3; }

	/* Pressure's EXT feature bit (bit 0), which is what makes ScaledValue
	 * and Scale legal to serve. Temperature and humidity have no features. */
	static constexpr uint32_t kPressureFeatureMapExt = 0x01;

private:
	CHIP_ERROR HandleReadMeasurement(chip::AttributeId attributeId, uint8_t *buffer, uint16_t maxReadLength);
	CHIP_ERROR HandleReadPressure(chip::AttributeId attributeId, uint8_t *buffer, uint16_t maxReadLength);
	CHIP_ERROR HandleReadFlow(chip::AttributeId attributeId, uint8_t *buffer, uint16_t maxReadLength);

	uint16_t mDeviceType;

	/*
	 * The reading, in the cluster's own units, and the two bounds.
	 *
	 * Min/MaxMeasuredValue are MANDATORY on all three clusters and are
	 * CONSTANTS here rather than tracked minima of what has been seen. Two
	 * reasons: the cluster defines them as the range the sensor is capable
	 * of reporting rather than the range it has happened to report, and a
	 * tracked minimum would move a subscribed attribute every time a sensor
	 * saw a new record - reporting traffic for a number nobody reads.
	 */
	int16_t mMeasuredValue = 0;
	int16_t mMinMeasuredValue = 0;
	int16_t mMaxMeasuredValue = 0;

	/* Flow only: Flow Measurement's three are int16u, and the stored value
	 * (bpm x 10, the 0.1 m^3/h encoding that HA shows as plain bpm)
	 * never reaches int16s's limit, but storing it unsigned keeps the
	 * two bytes copied out by HandleReadFlow() honest about their type. */
	uint16_t mFlowMeasuredValue = 0;

	/* Pressure only; left at zero and unread for the other two. Scale is
	 * an int8s and is written once, at endpoint creation, by
	 * radiant_matter.c's `mul == 0` constant mechanism. Min/MaxScaledValue
	 * are set by the constructor, in ScaledValue's units (10 Pa per count
	 * at Scale -1), and nothing writes them afterwards. */
	int16_t mScaledValue = 0;
	int16_t mMinScaledValue = 0;
	int16_t mMaxScaledValue = 0;
	int8_t mScale = 0;
};
