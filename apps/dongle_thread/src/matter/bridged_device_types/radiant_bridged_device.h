/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The one thing every RadiANT bridged device type has in common, and the two
 * rules the E4/E5 glue is built on.
 *
 * WHAT THIS IS. A thin layer between Nrf::MatterBridgedDevice (vendored, see
 * src/matter/README.md) and the three concrete device types this product
 * instantiates - occupancy_sensor.cpp and measurement_sensor.cpp. It exists
 * for exactly two reasons and would not be worth a file for either alone:
 *
 *  1. `SetReachable()`. The base declares SetIsReachable() protected, and a
 *     device RESTORED from persistent storage at boot must start unreachable:
 *     the endpoint is real (Matter forbids reusing an id for a different
 *     device, so it has to come back), but no ANT+ packet has been heard yet
 *     and reporting the last value from before a reboot as live would be a
 *     lie. There is no path to that through the vendored API, and editing the
 *     vendored file would break the "byte-for-byte identical to its source"
 *     claim the README makes.
 *
 *  2. `HandleAttributeChange()`'s ARGUMENT CONTRACT, stated in one place.
 *     Upstream's device types each interpret `data`/`dataSize` for themselves
 *     and every one of them expects the exact on-wire width of the attribute
 *     (humidity_sensor.cpp: CopyAttribute(data, dataSize, &int16, 2)). Ours do
 *     not: every measurement arriving from RadiANT is an int64_t, because
 *     radiant_matter.c's conversion is int64 arithmetic and narrowing it in
 *     the caller would put the range check in the wrong place. So:
 *
 *       For every cluster EXCEPT BridgedDeviceBasicInformation, `data` is a
 *       `const int64_t *` and `dataSize` is sizeof(int64_t). The device is
 *       what narrows, and a value that does not fit the attribute is REFUSED
 *       (CHIP_ERROR_INVALID_ARGUMENT), never saturated - the same rule
 *       radiant_matter_convert() applies one layer up, for the same reason: a
 *       saturated reading is a plausible wrong number in front of a user.
 *
 *       BridgedDeviceBasicInformation keeps the vendored contract, because
 *       Nrf::MatterBridgedDevice::HandleWriteDeviceBasicInformation() is what
 *       handles it and it wants a bool for Reachable.
 *
 * WHAT THIS IS NOT. Not a device type - it declares no cluster list, no
 * endpoint and no device-type id, and it is abstract. Not a place to put
 * RadiANT knowledge: nothing here knows what a binding, a source or a field_id
 * is. That is ant/ant_bridge.cpp's job and the seam is deliberate.
 */

#pragma once

#include "matter_bridged_device.h"

#include <cstdint>
#include <cstring>

namespace RadiantMatter
{

/*
 * Matter Device Library ids this product instantiates. Named here rather than
 * added to Nrf::MatterBridgedDevice::DeviceType, which is a vendored enum:
 * OccupancySensor and PressureSensor are simply not in it, and adding them
 * would be an edit to a file src/matter/README.md promises is unmodified.
 *
 * These must agree with radiant/src/bridge/radiant_matter.h's
 * MATTER_DEVTYPE_* constants, which is asserted in ant/ant_bridge.cpp rather
 * than here so that this header stays free of RadiANT includes.
 */
constexpr uint16_t kDeviceTypeOccupancySensor = 0x0107;
constexpr uint16_t kDeviceTypeTemperatureSensor = 0x0302;
constexpr uint16_t kDeviceTypePressureSensor = 0x0305;
constexpr uint16_t kDeviceTypeHumiditySensor = 0x0307;

/* Matter device-type revisions, as of the 1.4 Device Library. Wrong ones do
 * not fail a build and do not fail commissioning; they show up as a
 * certification failure much later, so they are named rather than left as
 * bare literals in four device-type tables. */
constexpr uint8_t kOccupancySensorDeviceVersion = 5;
constexpr uint8_t kTemperatureSensorDeviceVersion = 2;
constexpr uint8_t kHumiditySensorDeviceVersion = 2;
constexpr uint8_t kPressureSensorDeviceVersion = 2;

class RadiantBridgedDevice : public Nrf::MatterBridgedDevice {
public:
	explicit RadiantBridgedDevice(const char *uniqueID, const char *nodeLabel)
		: Nrf::MatterBridgedDevice(uniqueID, nodeLabel)
	{
	}

	/* See reason 1 in the header comment. Public on purpose and used from
	 * exactly one place - the restore path in ant/ant_bridge.cpp, before
	 * Server::Init() has anything to report to. */
	void SetReachable(bool reachable) { SetIsReachable(reachable); }

	/* ANT+ sensors in this product are receive-only: there is no path from
	 * a Matter write back to the sensor. Writes to NodeLabel are handled by
	 * the device itself (they are a Matter-side label, not a sensor
	 * property) and everything else is refused rather than silently
	 * accepted - a controller that thinks it turned something off and did
	 * not is worse than one that got an error. */
	CHIP_ERROR HandleWrite(chip::ClusterId clusterId, chip::AttributeId attributeId, uint8_t *buffer,
			       size_t size) override
	{
		if (clusterId != chip::app::Clusters::BridgedDeviceBasicInformation::Id) {
			return CHIP_ERROR_INVALID_ARGUMENT;
		}
		if (attributeId != chip::app::Clusters::BridgedDeviceBasicInformation::Attributes::NodeLabel::Id) {
			return CHIP_ERROR_INVALID_ARGUMENT;
		}
		return HandleWriteDeviceBasicInformation(clusterId, attributeId, buffer, size);
	}

protected:
	/*
	 * The narrowing, in one place, refusing rather than saturating. `min`
	 * and `max` are the attribute's own representable range, not the
	 * cluster's valid range - a MeasuredValue of -27315 is a legal int16s
	 * and an absurd temperature, and deciding which absurd readings to
	 * reject is the decoder's job and not this layer's.
	 */
	static bool Narrow(const void *data, size_t dataSize, int64_t min, int64_t max, int64_t &out)
	{
		if (data == nullptr || dataSize != sizeof(int64_t)) {
			return false;
		}
		int64_t v;
		memcpy(&v, data, sizeof(v));
		if (v < min || v > max) {
			return false;
		}
		out = v;
		return true;
	}
};

} /* namespace RadiantMatter */
