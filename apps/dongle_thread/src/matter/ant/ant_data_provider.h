/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Package E4: the ANT+ side of the bridging relationship.
 *
 * nrf/applications/matter_bridge/doc/adding_bridged_protocol.rst names four
 * things a new protocol has to bring. This product needs one and a half of
 * them, and saying which is the point of this comment:
 *
 *   "MyProtocol Data Provider"            -> this class.
 *   "MyProtocol MyDevice Data Provider"   -> NOT NEEDED. Upstream splits per
 *       device type because a BLE provider has to know which GATT
 *       characteristic to subscribe to. RadiANT's samples arrive already
 *       decoded, already converted into the cluster's own units, and already
 *       carrying (cluster, attribute) - that is what radiant_matter.c IS - so
 *       one provider serves every device type and a per-type subclass would
 *       be an empty one.
 *   "MyProtocol Connectivity Manager"     -> NOT NEEDED. Its job is mapping a
 *       protocol device id to a provider instance. RadiANT's binding table
 *       (radiant_binding.h) already is that map, and ant_bridge.cpp's shadow
 *       holds the provider pointer beside the binding's source index. A second
 *       map would be a second thing to keep in step.
 *   "MyProtocol Bridged Device Factory"   -> ant_bridge.cpp, which is also
 *       where persistence lives.
 *
 * WHY THERE IS ALMOST NOTHING IN THIS CLASS. The seam the vendored core wants
 * is `mUpdateAttributeCallback`, which is set to Nrf::BridgeManager::HandleUpdate
 * at construction and reaches the Matter device and the reporting engine. All
 * this class does is call it. The interesting code is on the other side of the
 * seam - the shadow, the flush pacing and the endpoint mapping - and it is in
 * ant_bridge.cpp because it is per-bridge state, not per-provider state.
 *
 * DIRECTION. This is a receive-only bridge. UpdateState() is the Matter ->
 * sensor direction and there is no such direction: an ANT+ heart-rate strap
 * has no writable state, and the derived booleans are computed here rather
 * than reported by anything. It returns CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE,
 * which is the value BridgeManager::HandleWrite() explicitly treats as "fine,
 * not every writable attribute is reflected in the provider" rather than as a
 * failure.
 */

#pragma once

#include "bridged_device_data_provider.h"

namespace RadiantMatter
{

class AntDataProvider : public Nrf::BridgedDeviceDataProvider {
public:
	explicit AntDataProvider(UpdateAttributeCallback updateCallback, InvokeCommandCallback commandCallback = nullptr)
		: Nrf::BridgedDeviceDataProvider(updateCallback, commandCallback)
	{
	}

	/* Called once by BridgeManager::AddSingleDevice() the first time this
	 * provider is seen. There is nothing to start: the ANT+ radio, the
	 * sample bus and the pump are all up long before Matter is, and this
	 * provider is a destination rather than a poller. */
	void Init() override {}

	/*
	 * MUST BE CALLED ON THE CHIP THREAD. It reaches
	 * Nrf::BridgeManager::HandleUpdate, which walks the device map, writes
	 * the Matter device's shadow and calls
	 * MatterReportingAttributeChangeCallback - all of which are CHIP data
	 * model operations. ant_bridge.cpp only ever calls it from inside a
	 * PlatformMgr().ScheduleWork() handler, which is what makes that true
	 * WITHOUT the sample-producing thread ever taking the CHIP stack lock.
	 */
	void NotifyUpdateState(chip::ClusterId clusterId, chip::AttributeId attributeId, void *data,
			       size_t dataSize) override
	{
		if (mUpdateAttributeCallback) {
			mUpdateAttributeCallback(*this, clusterId, attributeId, data, dataSize);
		}
	}

	CHIP_ERROR UpdateState(chip::ClusterId, chip::AttributeId, uint8_t *) override
	{
		return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE;
	}
};

} /* namespace RadiantMatter */
