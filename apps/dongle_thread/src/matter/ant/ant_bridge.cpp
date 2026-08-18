/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Package E4 and the handoff half of E6: RadiANT's sample bus on one side, the
 * vendored Matter bridge core on the other, and the thread boundary between
 * them.
 *
 * ===========================================================================
 * THE CONCURRENCY RULE, WHICH IS THE WHOLE DESIGN
 * ===========================================================================
 *
 * CHIP's event loop is its own thread at K_PRIO_PREEMPT(1) - above the ANT
 * host thread (5), above radiant's event thread (6) and far above the bridge
 * pump (7), which is where samples are drained. Two things follow and both are
 * prohibitions:
 *
 *  1. THE PUMP THREAD MUST NEVER TAKE THE CHIP STACK LOCK. LockChipStack()
 *     from a priority-7 thread lets it block the priority-1 thread that is
 *     servicing BLE commissioning and OpenThread. That is precisely the
 *     coupling src/bridge_pump.c:22-28 exists to prevent, in the other
 *     direction, and it is why that file is a thread of its own rather than a
 *     work item. Zephyr's priority inheritance bounds the inversion; it does
 *     not remove it, and the bound is "however long a CHIP data model
 *     operation takes".
 *
 *  2. NOTHING MAY BE ALLOCATED PER SAMPLE. At 4 Hz per field with up to
 *     sixteen endpoints, a per-sample chip::Platform::New() would run at 64 Hz
 *     against CONFIG_CHIP_MALLOC_SYS_HEAP_SIZE - 10 240 bytes shared with
 *     apps/common, the ANT stack and (docs/matter-ram-budget.md) OpenThread.
 *     Note that Nrf::BridgedDeviceDataProvider::NotifyReachableStatusChange()
 *     does exactly that, which is why this file does not call it and posts the
 *     Reachable update through the same batched path as everything else.
 *
 * So: a STATIC SHADOW, one row per radiant endpoint, written by the pump
 * thread under an ordinary k_mutex; and a SINGLE-IN-FLIGHT flush onto the CHIP
 * thread via PlatformMgr().ScheduleWork(), armed at 1 Hz, that batches every
 * dirty path into one pass. The mutex boundary is exactly the shadow: it is
 * never held across a CHIP call, and no CHIP API is ever called from any
 * thread but CHIP's own.
 *
 * ===========================================================================
 * ENDPOINT IDENTITY - TRAP 6 AND TRAP 8
 * ===========================================================================
 *
 * TRAP 6. radiant_matter.c assigns endpoint ids from 1 upward and its header
 * is explicit that they are an OPAQUE KEY and not a CHIP endpoint number - on
 * this bridge CHIP endpoint 1 is the Aggregator. The mapping is done here, and
 * "here" is the whole of the fix: nothing in radiant/ is renumbered, its ztests
 * still pin the numbering they always did, and the CHIP side of the map is
 * whatever Nrf::BridgeManager hands out (the first dynamic endpoint above the
 * fixed ones, which is 3 in this ZAP).
 *
 * TRAP 8, AND THE PLAN IS WRONG ABOUT THE KEY. The ruling is "persist the long
 * term id, users will not want to re-add entities after every reboot", and the
 * key it names is `struct radiant_binding::uuid`. That uuid CANNOT serve:
 * radiant_binding.c:46-52 generates it as `(k_cycle_get_32() << 32) | ++seq`
 * at bind time, and radiant_binding.h says in as many words that the table is
 * RAM-backed and unpersisted. It is therefore different on every boot for the
 * same physical sensor, and keying persistence on it would allocate a fresh
 * set of endpoints at every power cycle until the sixteen ran out - the exact
 * failure trap 8 is about, arrived at by following trap 8's own instructions.
 *
 * What IS stable across a reboot is the sensor's ANT channel identity, so the
 * key persisted here is a 64-bit FNV-1a hash of (devnum, devtype, trans_type),
 * paired with the field_id. Two consequences, stated because neither is free:
 *
 *   - radiant_binding.h opens with "nothing downstream may key on an ANT
 *     device number", and this is downstream and does key on one. The rule is
 *     right for its own reason - a device number is 16 bits and re-rollable -
 *     and the consequence here is bounded and visible: if a sensor re-rolls
 *     its device number, its Matter entities are replaced by new ones, which
 *     is the same thing a user sees when they re-pair. A COLLISION between two
 *     sensors would be worse, and (devnum, devtype, trans_type) is the same
 *     triple radiant_binding_find() treats as identity, so a collision here is
 *     a collision there.
 *   - The stored key is a hash rather than the triple itself, and that is not
 *     obfuscation: it makes the persisted UniqueID a fixed-width opaque
 *     string, which is what Bridged Device Basic Information's UniqueID is
 *     supposed to be.
 *
 * ===========================================================================
 * WHAT THIS FILE DOES NOT DO
 * ===========================================================================
 *
 * BATTERY. radiant_matter.c's `RADIANT_FIELD_BATTERY_SOC` row targets Power
 * Source (0x002F) / BatPercentRemaining on the source's primary endpoint, and
 * this file DECLINES it, with a counter. It is not an oversight and it is not
 * hard-to-do; it is a cluster whose conformant attribute set (Status, Order,
 * Description, EndpointList, and with the BAT feature BatChargeLevel,
 * BatReplacementNeeded and BatReplaceability) is a design decision of its own
 * on a part with 33 KB of RAM left, it would have to be added to EVERY bridged
 * device type because the primary endpoint may be any of them, and no goal in
 * docs/radiant-bridge.md section 8.1 depends on it. The data-model half is
 * built and ztest-covered in radiant_matter.c either way; only the CHIP half
 * is absent, and `declined_cluster_writes` in the log says so.
 */

#include "ant_matter.h"
#include "ant_data_provider.h"

#include "bridged_device_types/measurement_sensor.h"
#include "bridged_device_types/occupancy_sensor.h"

#include "bridge_manager.h"
#include "bridge_storage_manager.h"

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/PlatformManager.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

/* All three carry their own `extern "C"` guards, so they are included plainly.
 * Wrapping them in one here would put <zephyr/sys/iterable_sections.h> - which
 * radiant_bridge.h pulls in - inside C linkage, which is a way to break a C++
 * build that only shows up on an unlucky include order. */
#include "radiant_binding.h"
#include "radiant_bridge.h"
#include "radiant_matter.h"
#include "radiant_naming.h"

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;

namespace
{

/*
 * The three device-type numbers have to agree across a language boundary:
 * radiant_matter.h states them for the data model, radiant_bridged_device.h
 * for the CHIP device types. Asserted here rather than in either header
 * because this is the only translation unit that includes both, and the same
 * reasoning bridge_core_asserts.cpp records for trap 15.
 */
static_assert(RadiantMatter::kDeviceTypeOccupancySensor == MATTER_DEVTYPE_OCCUPANCY_SENSOR, "");
static_assert(RadiantMatter::kDeviceTypeTemperatureSensor == MATTER_DEVTYPE_TEMPERATURE_SENSOR, "");
static_assert(RadiantMatter::kDeviceTypeHumiditySensor == MATTER_DEVTYPE_HUMIDITY_SENSOR, "");
static_assert(RadiantMatter::kDeviceTypePressureSensor == MATTER_DEVTYPE_PRESSURE_SENSOR, "");

/*
 * Attribute slots per endpoint. THREE, and the number is the pressure row:
 * MeasuredValue, ScaledValue and the constant Scale, all on one endpoint from
 * one sample. Occupancy needs the same three (Occupancy plus the two
 * constants), temperature and humidity need one. Raising it costs 16 bytes per
 * endpoint per slot; a row that needs more than this loses its extra
 * attributes with a warning rather than corrupting a neighbour.
 */
constexpr uint8_t kMaxAttrsPerEndpoint = 3;

/* 1 Hz, and the pace rather than a tuning knob. See finding 7: this is an
 * efficiency measure against a controller that subscribes with
 * MinIntervalFloor = 0, and one report per second per attribute is already far
 * more than a household automation reacts to. */
constexpr uint32_t kFlushPeriodMs = 1000;

struct AttrCell {
	uint32_t cluster;
	uint32_t attribute;
	int64_t value;
	bool used;
	bool dirty;
};

struct Row {
	bool used;
	uint16_t radiantEndpoint; /* radiant_matter.c's opaque key */
	uint32_t source;
	uint8_t fieldId;
	/* The section 7 vocabulary type, carried straight from
	 * struct radiant_matter_endpoint. It used to be re-derived from
	 * deviceType inside ComposeLabel(); see that function. */
	uint8_t fieldType;
	uint16_t deviceType;

	/* Filled in on the CHIP thread when the device is created or adopted
	 * from storage. kInvalidEndpointId until then. */
	EndpointId chipEndpoint;
	RadiantMatter::AntDataProvider *provider;

	bool needCreate;
	bool needRemove;

	/* Which BridgeStorageManager slot this endpoint was stored in, so a
	 * renamed endpoint can be re-persisted rather than reverting to its
	 * boot-time name at the next power cycle. Only meaningful once the
	 * endpoint has been created or adopted. */
	bool storageIndexValid;
	uint8_t storageIndex;

	AttrCell attrs[kMaxAttrsPerEndpoint];
};

Row sRows[RADIANT_MATTER_MAX_ENDPOINTS];

struct SourceState {
	bool bound;
	uint64_t key; /* the persisted identity - see TRAP 8 above */
	char label[16];
	bool reachable;
	bool reachableDirty;

	/* The three naming inputs. devtype/devnum come from the binding;
	 * `subtype` is the FE-C equipment type and is LEARNED FROM TRAFFIC -
	 * see radiant_naming.h - so it is 0 until a page 16 arrives and may
	 * change afterwards, which is what labelDirty is for. One byte per
	 * source; no string is cached anywhere. */
	uint8_t devtype;
	uint16_t devnum;
	uint8_t subtype;
	bool labelDirty;
};

SourceState sSources[RADIANT_BINDING_MAX];

/*
 * Devices loaded from persistent storage at boot. They already exist as CHIP
 * endpoints by the time anything below runs - RestoreBridgedDevices() creates
 * them inside BridgeManager::Init() - and sit here unclaimed until a sample
 * from the matching sensor arrives and adopts one.
 */
struct RestoredDevice {
	bool used;
	bool claimed;
	char uniqueID[Nrf::MatterBridgedDevice::kUniqueIDSize + 1];
	uint16_t endpointId;
	uint16_t deviceType;
	uint8_t index;
	RadiantMatter::AntDataProvider *provider;
};

RestoredDevice sRestored[Nrf::BridgeManager::kMaxBridgedDevices];

K_MUTEX_DEFINE(sShadowLock);

/* Single-in-flight. The flush handler clears it FIRST, so a write that lands
 * while a flush is running arms the next one rather than being swallowed. */
atomic_t sFlushPending;

/* Diagnostics. Not silent drops: every one of these has a reason and the
 * reason is invisible from a controller, so they are counted and reported on
 * the console rather than left to be inferred from a missing entity. */
uint32_t sDeclinedClusterWrites;
uint32_t sNoAttrSlot;
uint32_t sCreateFailures;

k_work_delayable sFlushTimer;
uint32_t sPacerTicks;

/* ------------------------------------------------------------------------ */
/* Identity                                                                  */
/* ------------------------------------------------------------------------ */

uint64_t StableKey(const struct radiant_binding *b)
{
	/* FNV-1a, 64-bit. Not a security property and not a checksum: it is a
	 * fixed-width label for a four-byte tuple, chosen because it is four
	 * lines and has no state. */
	uint64_t h = 0xcbf29ce484222325ULL;
	const uint8_t bytes[] = { static_cast<uint8_t>(b->devnum & 0xFFu),
				  static_cast<uint8_t>((b->devnum >> 8) & 0xFFu), b->devtype, b->trans_type };

	for (uint8_t byte : bytes) {
		h ^= byte;
		h *= 0x100000001b3ULL;
	}
	return h;
}

void FormatUniqueID(char *out, size_t len, uint64_t key, uint8_t fieldId)
{
	/* 18 characters plus a NUL, against kUniqueIDSize = 32. Fixed width so
	 * that a truncated compare can never match the wrong device. */
	snprintf(out, len, "%08x%08x%02x", static_cast<unsigned int>(key >> 32),
		 static_cast<unsigned int>(key & 0xFFFFFFFFu), fieldId);
}

/*
 * THE NODE LABEL, and NodeLabel is the only carrier there is.
 *
 * MatterBridgedDevice serves exactly Reachable, UniqueID, NodeLabel,
 * ClusterRevision and FeatureMap - VendorName, ProductName, ProductLabel and
 * SerialNumber are not in its attribute list, and adding them means editing
 * vendored code that src/matter/README.md promises is byte-identical to
 * upstream. So the whole of an endpoint's user-visible identity is one string
 * of at most kNodeLabelSize bytes.
 *
 * radiant_naming.c composes it; this function is only the snapshot. It takes
 * the shadow lock, copies the six scalars out and releases it BEFORE
 * formatting, so no lock is held across snprintf - and it is callable from
 * either thread for the same reason.
 *
 * `out` must be RADIANT_NAMING_MAX bytes. That is not advice: SetNodeLabel()
 * memcpy()s the length it is given into a kNodeLabelSize buffer with no check
 * of its own, and it is vendored, so the bound has to come from here.
 */
void ComposeLabel(char out[RADIANT_NAMING_MAX], const Row &row)
{
	uint8_t devtype;
	uint16_t devnum;
	uint8_t subtype;
	char label[sizeof(SourceState::label)];
	uint8_t fieldId;
	uint8_t fieldType;

	k_mutex_lock(&sShadowLock, K_FOREVER);
	devtype = sSources[row.source].devtype;
	devnum = sSources[row.source].devnum;
	subtype = sSources[row.source].subtype;
	memcpy(label, sSources[row.source].label, sizeof(label));
	fieldId = row.fieldId;
	fieldType = row.fieldType;
	k_mutex_unlock(&sShadowLock);

	label[sizeof(label) - 1] = '\0';

	/*
	 * THE REAL VOCABULARY TYPE, not one re-derived from the Matter device
	 * type. This used to pass RADIANT_FIELD_OCCUPANCY when the device type
	 * was Occupancy Sensor and 0 otherwise, justified by "every other device
	 * type has no suffix anyway".
	 *
	 * That justification expired when radiant_naming.c gained a row for
	 * every series a source can carry. A 0 here would now miss "Temp",
	 * "Temp Min" and "Temp Max" and fall through to the "#<id>" backstop
	 * instead - three temperature endpoints reading "#33", "#39" and "#40".
	 * struct radiant_matter_endpoint has carried field_type all along, so
	 * Row simply keeps it and nothing is re-derived from anything.
	 */
	(void)radiant_naming_format(out, RADIANT_NAMING_MAX, devtype, devnum, subtype, label, fieldType, fieldId);
}

/* ------------------------------------------------------------------------ */
/* The shadow - pump thread side                                             */
/* ------------------------------------------------------------------------ */

/* Row lookup. Caller holds sShadowLock. */
Row *FindRow(uint16_t radiantEndpoint)
{
	for (Row &r : sRows) {
		if (r.used && r.radiantEndpoint == radiantEndpoint) {
			return &r;
		}
	}
	return nullptr;
}

/*
 * Allocates a shadow row for a radiant endpoint id, reading its (source,
 * field_id, device_type) out of radiant_matter.c's own table.
 *
 * THAT TABLE IS READ ONLY FROM THIS THREAD. radiant_matter.c's endpoint array
 * is written by matter_publish(), which runs on the bridge pump thread, and
 * this function is called from radiant_matter_attr_write(), which
 * matter_publish() calls - the same thread, inside the same call. Reading it
 * from the CHIP thread would be a race with no lock to take, which is why the
 * shadow copies what it needs instead of holding a pointer into it.
 */
Row *AllocRow(uint16_t radiantEndpoint)
{
	const struct radiant_matter_endpoint *e = nullptr;

	for (uint16_t i = 0; i < radiant_matter_endpoint_count(); i++) {
		const struct radiant_matter_endpoint *cand = radiant_matter_endpoint_at(i);

		if (cand != nullptr && cand->endpoint_id == radiantEndpoint) {
			e = cand;
			break;
		}
	}
	if (e == nullptr || e->source >= RADIANT_BINDING_MAX) {
		return nullptr;
	}

	for (Row &r : sRows) {
		if (r.used) {
			continue;
		}
		memset(&r, 0, sizeof(r));
		r.used = true;
		r.radiantEndpoint = radiantEndpoint;
		r.source = e->source;
		r.fieldId = e->field_id;
		r.fieldType = e->field_type;
		r.deviceType = e->device_type;
		r.chipEndpoint = kInvalidEndpointId;
		r.needCreate = true;
		return &r;
	}
	return nullptr;
}

bool ClusterIsServed(uint32_t cluster)
{
	switch (cluster) {
	case Clusters::OccupancySensing::Id:
	case Clusters::TemperatureMeasurement::Id:
	case Clusters::RelativeHumidityMeasurement::Id:
	case Clusters::PressureMeasurement::Id:
		return true;
	default:
		/* Power Source, today. See "WHAT THIS FILE DOES NOT DO". */
		return false;
	}
}

/* ------------------------------------------------------------------------ */
/* The flush - CHIP thread side                                              */
/* ------------------------------------------------------------------------ */

Nrf::MatterBridgedDevice *MakeDevice(uint16_t deviceType, const char *uniqueID, const char *label)
{
	switch (deviceType) {
	case RadiantMatter::kDeviceTypeOccupancySensor:
		return chip::Platform::New<OccupancySensorDevice>(uniqueID, label);
	case RadiantMatter::kDeviceTypeTemperatureSensor:
	case RadiantMatter::kDeviceTypeHumiditySensor:
	case RadiantMatter::kDeviceTypePressureSensor:
		return chip::Platform::New<MeasurementSensorDevice>(deviceType, uniqueID, label);
	default:
		return nullptr;
	}
}

/* Rewrites the whole index/count pair after any add or remove. Cheap (two
 * settings keys) and it is the only way to keep the stored list in step with
 * BridgeManager's, which is the list RestoreBridgedDevices() walks. */
void PersistIndexes()
{
	uint8_t indexes[Nrf::BridgeManager::kMaxBridgedDevices];
	uint8_t count = 0;

	if (Nrf::BridgeManager::Instance().GetDevicesIndexes(indexes, sizeof(indexes), count) != CHIP_NO_ERROR) {
		LOG_ERR("matter: cannot read bridged device indexes; storage left stale");
		return;
	}
	if (!Nrf::BridgeStorageManager::Instance().StoreBridgedDevicesIndexes(indexes, count) ||
	    !Nrf::BridgeStorageManager::Instance().StoreBridgedDevicesCount(count)) {
		LOG_ERR("matter: cannot persist bridged device list");
	}
}

/*
 * Creates the CHIP endpoint for a shadow row, or adopts the one a previous
 * boot left behind. Runs on the CHIP thread.
 */
void CreateEndpointForRow(Row &row)
{
	char uniqueID[Nrf::MatterBridgedDevice::kUniqueIDSize + 1];
	char nodeLabel[RADIANT_NAMING_MAX];
	uint16_t deviceType;
	uint64_t key;

	k_mutex_lock(&sShadowLock, K_FOREVER);
	deviceType = row.deviceType;
	key = sSources[row.source].key;
	FormatUniqueID(uniqueID, sizeof(uniqueID), key, row.fieldId);
	k_mutex_unlock(&sShadowLock);

	ComposeLabel(nodeLabel, row);

	/* Adopt, if a previous boot stored this exact (sensor, field). This is
	 * the whole of trap 8's answer: the endpoint id comes back from
	 * storage, so a controller's entity survives a power cycle. */
	for (RestoredDevice &rd : sRestored) {
		if (!rd.used || rd.claimed || strcmp(rd.uniqueID, uniqueID) != 0) {
			continue;
		}
		if (rd.deviceType != deviceType) {
			/* Same sensor and field, different Matter device type.
			 * Matter forbids reusing an endpoint id for a
			 * different device, so the stored one is retired
			 * rather than repurposed. */
			LOG_WRN("matter: stored endpoint %u was device type 0x%04x, now 0x%04x - not reused",
				rd.endpointId, rd.deviceType, deviceType);
			continue;
		}
		rd.claimed = true;
		k_mutex_lock(&sShadowLock, K_FOREVER);
		row.chipEndpoint = rd.endpointId;
		row.provider = rd.provider;
		row.storageIndex = rd.index;
		row.storageIndexValid = true;
		/*
		 * THE ADOPTION BRANCH RETURNS HERE, WHICH IS WHY THIS FLAG IS
		 * SET. The restored device was constructed from the NodeLabel
		 * that was in storage - which, for any bridge that ever booted
		 * the old scheme, is the 18-character hex UniqueID - and this
		 * early return discards the name just composed. Without the
		 * flag those endpoints would keep hex names for the life of the
		 * device, since nothing else ever revisits a name. The flush
		 * pushes and re-persists the composed one instead.
		 */
		sSources[row.source].labelDirty = true;
		k_mutex_unlock(&sShadowLock);
		LOG_INF("matter: adopted stored endpoint %u for %s", rd.endpointId, uniqueID);
		return;
	}

	auto *provider = chip::Platform::New<RadiantMatter::AntDataProvider>(Nrf::BridgeManager::HandleUpdate,
									     Nrf::BridgeManager::HandleCommand);
	if (provider == nullptr) {
		sCreateFailures++;
		LOG_ERR("matter: out of CHIP heap creating a data provider (trap 10)");
		return;
	}

	/* The composed name, never the UniqueID. A controller used to show
	 * "3f2a91c40e77b21205" for every endpoint of every sensor, because the
	 * only production bind site passes NULL for the label and this line
	 * fell back to the hex id. */
	Nrf::MatterBridgedDevice *device = MakeDevice(deviceType, uniqueID, nodeLabel);
	if (device == nullptr) {
		chip::Platform::Delete(provider);
		sCreateFailures++;
		LOG_ERR("matter: no bridged device type for 0x%04x", deviceType);
		return;
	}

	Nrf::MatterBridgedDevice *devices[] = { device };
	uint8_t indexes[1] = { 0 };

	/* Takes ownership of both objects unconditionally, including on
	 * failure - see BridgeManager::AddDevices(). Nothing is deleted here. */
	CHIP_ERROR err = Nrf::BridgeManager::Instance().AddBridgedDevices(devices, provider, 1, indexes);

	if (err != CHIP_NO_ERROR) {
		sCreateFailures++;
		LOG_ERR("matter: AddBridgedDevices failed for %s: %" CHIP_ERROR_FORMAT, uniqueID, err.Format());
		return;
	}

	EndpointId assigned = device->GetEndpointId();

	k_mutex_lock(&sShadowLock, K_FOREVER);
	row.chipEndpoint = assigned;
	row.provider = provider;
	row.storageIndex = indexes[0];
	row.storageIndexValid = true;
	k_mutex_unlock(&sShadowLock);

	Nrf::BridgeStorageManager::BridgedDevice stored;
	stored.mEndpointId = assigned;
	stored.mDeviceType = deviceType;
	stored.mUniqueIDLength = strlen(uniqueID);
	memcpy(stored.mUniqueID, uniqueID, stored.mUniqueIDLength);
	stored.mNodeLabelLength = strlen(device->GetNodeLabel());
	memcpy(stored.mNodeLabel, device->GetNodeLabel(), stored.mNodeLabelLength);
	stored.mUserData = nullptr;
	stored.mUserDataSize = 0;

	if (!Nrf::BridgeStorageManager::Instance().StoreBridgedDevice(stored, indexes[0])) {
		/* The endpoint exists and works; it just will not survive a
		 * reboot. Worth a line, not worth unwinding. */
		LOG_ERR("matter: endpoint %u created but not persisted", assigned);
	} else {
		PersistIndexes();
	}

	/* The NodeLabel is in this line because it is the ONLY carrier a
	 * controller shows for a bridged device, and without a commissioned
	 * controller on the bench this log is the only place it can be read. */
	LOG_INF("matter: endpoint %u = radiant endpoint %u (source %u field %u, device type 0x%04x) \"%s\"",
		assigned, row.radiantEndpoint, static_cast<unsigned int>(row.source), row.fieldId, deviceType,
		nodeLabel);
}

/*
 * Rewrites one stored endpoint record with a new NodeLabel. Runs on the CHIP
 * thread, from the flush.
 *
 * StoreBridgedDevice() used to run exactly once per endpoint, at creation. A
 * name that improves later - the FE-C subtype arriving after the endpoint
 * exists, or an adopted endpoint carrying a name from an older scheme - would
 * therefore be correct until the next power cycle and then revert, which is a
 * worse experience than never having improved.
 */
void PersistRowLabel(const Row &row, const char *nodeLabel)
{
	Nrf::BridgeStorageManager::BridgedDevice stored;
	char uniqueID[Nrf::MatterBridgedDevice::kUniqueIDSize + 1];
	uint64_t key;

	if (!row.storageIndexValid) {
		return;
	}

	k_mutex_lock(&sShadowLock, K_FOREVER);
	key = sSources[row.source].key;
	k_mutex_unlock(&sShadowLock);

	FormatUniqueID(uniqueID, sizeof(uniqueID), key, row.fieldId);

	stored.mEndpointId = row.chipEndpoint;
	stored.mDeviceType = row.deviceType;
	stored.mUniqueIDLength = strlen(uniqueID);
	memcpy(stored.mUniqueID, uniqueID, stored.mUniqueIDLength);
	stored.mNodeLabelLength = strlen(nodeLabel);
	memcpy(stored.mNodeLabel, nodeLabel, stored.mNodeLabelLength);
	stored.mUserData = nullptr;
	stored.mUserDataSize = 0;

	if (!Nrf::BridgeStorageManager::Instance().StoreBridgedDevice(stored, row.storageIndex)) {
		/* The live name is already correct; only the next boot loses
		 * it. Worth a line, not worth unwinding. */
		LOG_ERR("matter: endpoint %u renamed but not re-persisted", row.chipEndpoint);
	}
}

void RemoveEndpointForRow(Row &row)
{
	uint8_t pairIndex = 0;

	if (row.chipEndpoint != kInvalidEndpointId) {
		if (Nrf::BridgeManager::Instance().RemoveBridgedDevice(row.chipEndpoint, pairIndex) == CHIP_NO_ERROR) {
			Nrf::BridgeStorageManager::Instance().RemoveBridgedDevice(pairIndex);
			PersistIndexes();
			LOG_INF("matter: removed endpoint %u (unbind)", row.chipEndpoint);
		}
	}

	k_mutex_lock(&sShadowLock, K_FOREVER);
	memset(&row, 0, sizeof(row));
	k_mutex_unlock(&sShadowLock);
}

void FlushHandler(intptr_t)
{
	/* Cleared first: a write landing during this pass must arm the next
	 * flush rather than be lost behind a flag that is still set. */
	atomic_set(&sFlushPending, 0);

	for (Row &row : sRows) {
		AttrCell pending[kMaxAttrsPerEndpoint];
		bool needCreate;
		bool needRemove;
		bool used;
		uint32_t source;

		k_mutex_lock(&sShadowLock, K_FOREVER);
		used = row.used;
		source = row.source;
		needCreate = row.needCreate;
		needRemove = row.needRemove;
		row.needCreate = false;
		row.needRemove = false;
		for (uint8_t i = 0; i < kMaxAttrsPerEndpoint; i++) {
			pending[i] = row.attrs[i];
			row.attrs[i].dirty = false;
		}
		k_mutex_unlock(&sShadowLock);

		if (!used) {
			continue;
		}

		if (needRemove) {
			RemoveEndpointForRow(row);
			continue;
		}

		if (needCreate) {
			CreateEndpointForRow(row);
		}

		/* Re-read the pointer: CreateEndpointForRow() may have set it,
		 * or may have failed and left it null. A row with no provider
		 * keeps accumulating values and will be retried, because every
		 * write re-dirties its cell. */
		k_mutex_lock(&sShadowLock, K_FOREVER);
		RadiantMatter::AntDataProvider *provider = row.provider;
		bool reachable = sSources[source].reachable;
		bool reachableDirty = sSources[source].reachableDirty;
		bool labelDirty = sSources[source].labelDirty;
		k_mutex_unlock(&sShadowLock);

		if (provider == nullptr) {
			if (needCreate) {
				/* Ask again next second rather than never. */
				k_mutex_lock(&sShadowLock, K_FOREVER);
				row.needCreate = true;
				k_mutex_unlock(&sShadowLock);
			}
			continue;
		}

		for (const AttrCell &cell : pending) {
			if (!cell.used || !cell.dirty) {
				continue;
			}
			int64_t value = cell.value;

			/* TEMPORARY, package G items 3/4: nothing on this path
			 * logged an individual attribute write, so "the pipeline
			 * runs end to end" was not a measurement. One line per
			 * write, at the 1 Hz flush pace (post-deadband/heartbeat),
			 * not per sample - see docs/g-hardware-bringup.md. */
			LOG_INF("matter: chip endpoint %u cluster 0x%04x attr 0x%04x <- %lld",
				row.chipEndpoint, static_cast<unsigned int>(cell.cluster),
				static_cast<unsigned int>(cell.attribute), static_cast<long long>(value));

			/* int64_t and sizeof(int64_t), always. The device
			 * narrows and refuses what does not fit - see
			 * bridged_device_types/radiant_bridged_device.h. */
			provider->NotifyUpdateState(cell.cluster, cell.attribute, &value, sizeof(value));
		}

		if (labelDirty) {
			/*
			 * THE LATE RENAME. Two things make it necessary: the
			 * FE-C equipment type is learned from traffic and may
			 * arrive after the endpoint exists, and an adopted
			 * endpoint comes back from storage with whatever name
			 * the boot that created it chose.
			 *
			 * The same batched path Reachable uses, one line above,
			 * and for the same reasons - no CHIP heap allocation
			 * and no second hop onto this thread.
			 *
			 * NOT the AttrCell shadow: that is int64_t only, and a
			 * string does not fit in it. NOT emberAfWriteAttribute()
			 * either: that path is gated on the endpoint being
			 * reachable and expects Pascal-string framing, whereas
			 * this one lands in HandleWriteDeviceBasicInformation()
			 * -> SetNodeLabel(), which takes a plain pointer and a
			 * length. The length excludes the NUL deliberately:
			 * SetNodeLabel() memsets its 32-byte buffer first, so a
			 * 31-character name is still terminated, and passing
			 * the NUL would make a full-length name one byte too
			 * long for a memcpy that does not check.
			 */
			char nodeLabel[RADIANT_NAMING_MAX];

			ComposeLabel(nodeLabel, row);
			provider->NotifyUpdateState(Clusters::BridgedDeviceBasicInformation::Id,
						    Clusters::BridgedDeviceBasicInformation::Attributes::NodeLabel::Id,
						    nodeLabel, strlen(nodeLabel));
			PersistRowLabel(row, nodeLabel);
			LOG_INF("matter: endpoint %u renamed to \"%s\"", row.chipEndpoint, nodeLabel);
		}

		if (reachableDirty) {
			bool r = reachable;

			/* NOT NotifyReachableStatusChange(): that allocates a
			 * context out of the CHIP heap and schedules another
			 * pass onto this same thread. We are already on it. */
			provider->NotifyUpdateState(Clusters::BridgedDeviceBasicInformation::Id,
						    Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id,
						    &r, sizeof(r));
		}
	}

	/* One clear per pass, after every row has seen it. Both flags are per
	 * SOURCE and every row of that source has to act on them, which is why
	 * they cannot be cleared inside the loop above. */
	k_mutex_lock(&sShadowLock, K_FOREVER);
	for (SourceState &s : sSources) {
		s.reachableDirty = false;
		s.labelDirty = false;
	}
	k_mutex_unlock(&sShadowLock);
}

bool AnythingDirty()
{
	bool dirty = false;

	k_mutex_lock(&sShadowLock, K_FOREVER);
	for (const Row &row : sRows) {
		if (!row.used) {
			continue;
		}
		if (row.needCreate || row.needRemove) {
			dirty = true;
			break;
		}
		for (const AttrCell &cell : row.attrs) {
			if (cell.used && cell.dirty) {
				dirty = true;
				break;
			}
		}
		if (dirty) {
			break;
		}
	}
	if (!dirty) {
		for (const SourceState &s : sSources) {
			if (s.reachableDirty || s.labelDirty) {
				dirty = true;
				break;
			}
		}
	}
	k_mutex_unlock(&sShadowLock);
	return dirty;
}

/*
 * THE 1 Hz PACER, and it is a delayable work item rather than a thread or a
 * k_timer for two reasons: a k_timer handler runs in ISR context and
 * ScheduleWork()'s queue put is not something to do from there on a whim, and
 * a thread would cost a stack for something that runs for microseconds.
 *
 * It reschedules itself unconditionally rather than being armed by a write, so
 * that the LAST change before a sensor goes quiet is still flushed. A pacer
 * armed by writes would leave a final value sitting in the shadow forever.
 */
void FlushTimerHandler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (AnythingDirty() && atomic_cas(&sFlushPending, 0, 1)) {
		if (DeviceLayer::PlatformMgr().ScheduleWork(FlushHandler, 0) != CHIP_NO_ERROR) {
			atomic_set(&sFlushPending, 0);
		}
	}

	/* The three counters, once a minute and only when non-zero. Each one is
	 * a thing a user would otherwise experience as "an entity that never
	 * appeared" with nothing anywhere to explain it. */
	if ((++sPacerTicks % 60u) == 0u &&
	    (sDeclinedClusterWrites | sNoAttrSlot | sCreateFailures) != 0u) {
		LOG_WRN("matter: declined_cluster_writes %u (battery/Power Source is not served - see "
			"ant_bridge.cpp), no_attr_slot %u, create_failures %u",
			sDeclinedClusterWrites, sNoAttrSlot, sCreateFailures);
	}

	k_work_schedule(&sFlushTimer, K_MSEC(kFlushPeriodMs));
}

/* ------------------------------------------------------------------------ */
/* Restore                                                                   */
/* ------------------------------------------------------------------------ */

CHIP_ERROR RestoreBridgedDevices()
{
	uint8_t count;
	uint8_t indexes[Nrf::BridgeManager::kMaxBridgedDevices] = { 0 };
	size_t indexesCount = 0;

	if (!Nrf::BridgeStorageManager::Instance().LoadBridgedDevicesCount(count)) {
		LOG_INF("matter: no bridged devices in storage");
		return CHIP_NO_ERROR;
	}
	if (!Nrf::BridgeStorageManager::Instance().LoadBridgedDevicesIndexes(
		    indexes, Nrf::BridgeManager::kMaxBridgedDevices, indexesCount)) {
		return CHIP_NO_ERROR;
	}

	for (size_t i = 0; i < indexesCount && i < ARRAY_SIZE(sRestored); i++) {
		Nrf::BridgeStorageManager::BridgedDevice stored;

		if (!Nrf::BridgeStorageManager::Instance().LoadBridgedDevice(stored, indexes[i])) {
			/* One unreadable entry is not a reason to abandon the
			 * rest, and it is certainly not a reason to fail
			 * BridgeManager::Init() - which is what returning an
			 * error here would do, leaving the bridge with no
			 * aggregator at all. */
			LOG_WRN("matter: stored bridged device %u unreadable - skipped", indexes[i]);
			continue;
		}

		auto *provider = chip::Platform::New<RadiantMatter::AntDataProvider>(
			Nrf::BridgeManager::HandleUpdate, Nrf::BridgeManager::HandleCommand);
		if (provider == nullptr) {
			LOG_ERR("matter: out of CHIP heap restoring endpoint %u", stored.mEndpointId);
			continue;
		}

		Nrf::MatterBridgedDevice *device =
			MakeDevice(stored.mDeviceType, stored.mUniqueID, stored.mNodeLabel);
		if (device == nullptr) {
			chip::Platform::Delete(provider);
			LOG_WRN("matter: stored device type 0x%04x is not one this build serves", stored.mDeviceType);
			continue;
		}

		/* UNREACHABLE UNTIL A PACKET IS HEARD. The endpoint has to come
		 * back - Matter forbids reusing its id for a different device -
		 * but the value in it is from before the reboot and nothing has
		 * confirmed the sensor is still in the room. */
		static_cast<RadiantMatter::RadiantBridgedDevice *>(device)->SetReachable(false);

		Nrf::MatterBridgedDevice *devices[] = { device };
		uint8_t pairIndexes[1] = { indexes[i] };
		uint16_t endpointIds[1] = { stored.mEndpointId };

		if (Nrf::BridgeManager::Instance().AddBridgedDevices(devices, provider, 1, pairIndexes, endpointIds) !=
		    CHIP_NO_ERROR) {
			LOG_ERR("matter: could not restore endpoint %u", stored.mEndpointId);
			continue;
		}

		RestoredDevice &rd = sRestored[i];
		rd.used = true;
		rd.claimed = false;
		memset(rd.uniqueID, 0, sizeof(rd.uniqueID));
		memcpy(rd.uniqueID, stored.mUniqueID,
		       stored.mUniqueIDLength < sizeof(rd.uniqueID) ? stored.mUniqueIDLength :
									sizeof(rd.uniqueID) - 1);
		rd.endpointId = stored.mEndpointId;
		rd.deviceType = stored.mDeviceType;
		rd.index = indexes[i];
		rd.provider = provider;

		LOG_INF("matter: restored endpoint %u (%s, type 0x%04x), unreachable until heard", rd.endpointId,
			rd.uniqueID, rd.deviceType);
	}

	return CHIP_NO_ERROR;
}

} /* namespace */

/* ------------------------------------------------------------------------ */
/* Public entry points                                                       */
/* ------------------------------------------------------------------------ */

namespace RadiantMatter
{

/*
 * Runs on the CHIP thread, from InitData::mPostServerInitClbk, which is after
 * Server::Init() and is the ONLY legal place to create dynamic endpoints:
 * emberAfSetDynamicEndpoint() writes into tables that
 * emberAfEndpointConfigure() - called from Server::Init() - has to have set up
 * first, and the reporting engine it notifies does not exist before that.
 */
CHIP_ERROR PostServerInit()
{
	CHIP_ERROR err = Nrf::BridgeManager::Instance().Init(RestoreBridgedDevices);

	if (err != CHIP_NO_ERROR) {
		LOG_ERR("matter: BridgeManager init failed: %" CHIP_ERROR_FORMAT, err.Format());
		return err;
	}

	k_work_init_delayable(&sFlushTimer, FlushTimerHandler);
	k_work_schedule(&sFlushTimer, K_MSEC(kFlushPeriodMs));

	LOG_INF("matter: ANT bridge ready (%u endpoints max, aggregator on endpoint %u)",
		static_cast<unsigned int>(RADIANT_MATTER_MAX_ENDPOINTS),
		static_cast<unsigned int>(Nrf::BridgeManager::kAggregatorEndpointId));
	return CHIP_NO_ERROR;
}

} /* namespace RadiantMatter */

/*
 * THE SEAM. radiant_matter.c declares this __weak and calls it for every
 * attribute of every mapped field; this is the strong definition that a Matter
 * image provides. C linkage, because the declaration is in a C header.
 *
 * RUNS ON THE BRIDGE PUMP THREAD (priority 7), inside
 * radiant_bridge_drain(). It must not block for long and must not touch CHIP.
 * All it does is copy into the shadow.
 */
extern "C" void radiant_matter_attr_write(uint16_t endpoint_id, uint32_t cluster, uint32_t attribute, int64_t value)
{
	if (!ClusterIsServed(cluster)) {
		sDeclinedClusterWrites++;
		return;
	}

	k_mutex_lock(&sShadowLock, K_FOREVER);

	Row *row = FindRow(endpoint_id);

	if (row == nullptr) {
		row = AllocRow(endpoint_id);
		if (row == nullptr) {
			k_mutex_unlock(&sShadowLock);
			return;
		}
	}

	AttrCell *cell = nullptr;

	for (AttrCell &c : row->attrs) {
		if (c.used && c.cluster == cluster && c.attribute == attribute) {
			cell = &c;
			break;
		}
		if (!c.used && cell == nullptr) {
			cell = &c;
		}
	}
	if (cell == nullptr) {
		sNoAttrSlot++;
		k_mutex_unlock(&sShadowLock);
		return;
	}

	cell->used = true;
	cell->cluster = cluster;
	cell->attribute = attribute;
	cell->value = value;
	/*
	 * DIRTY UNCONDITIONALLY, even when the value is unchanged. The
	 * deadband is radiant_matter.c's and it has already run - including
	 * the heartbeat, which exists precisely so that an unchanged value is
	 * re-reported periodically. Suppressing an identical value here would
	 * silently undo the heartbeat one layer below.
	 */
	cell->dirty = true;

	k_mutex_unlock(&sShadowLock);
}

/*
 * The FE-C equipment type's field_id, hardcoded with the citation rather than
 * by including radiant_power_adapter.h - the same way radiant_rules.c spells
 * FEC_STATE_IN_USE, and for the same reason: this file is above the profile
 * decoders and has no business pulling a page codec's header in.
 *
 * radiant_power_adapter.h: RADIANT_POWER_FIELD_FEC_TYPE = 0x0D, posted as
 * RADIANT_FIELD_ENUM_GENERIC from FE-C page 16 byte 1 (Table 8-8).
 */
constexpr uint8_t kFecTypeFieldId = 0x0Du;
constexpr uint8_t kFecDeviceType = 0x11u;

extern "C" void ant_matter_note_sample(uint32_t source, uint8_t flags, uint8_t field_type, uint8_t field_id,
				       int64_t raw)
{
	bool stale = (flags & RADIANT_SAMPLE_STALE) != 0u;

	if (source >= RADIANT_BINDING_MAX) {
		return;
	}

	k_mutex_lock(&sShadowLock, K_FOREVER);

	/*
	 * THE SUBTYPE, AND IT IS GATED ON THE BINDING'S DEVICE TYPE TOO. Field
	 * ids 0x00-0x1F belong to whichever profile adapter owns the source
	 * (radiant_bridge.h's allocation block), so 0x0D means "equipment type"
	 * only on an FE-C binding; on any other device type it is some other
	 * adapter's series and must not be read as one.
	 */
	if (field_type == RADIANT_FIELD_ENUM_GENERIC && field_id == kFecTypeFieldId &&
	    sSources[source].devtype == kFecDeviceType && raw >= 0 && raw <= UINT8_MAX) {
		uint8_t subtype = static_cast<uint8_t>(raw);

		if (sSources[source].subtype != subtype) {
			sSources[source].subtype = subtype;
			/* Only when it CHANGES - a treadmill repeats this
			 * value four times a second, and a rename per message
			 * would be a report storm on every controller
			 * subscribed to NodeLabel. */
			sSources[source].labelDirty = true;
		}
	}

	/*
	 * REACHABILITY IS PER SENSOR, NOT PER FIELD, and the flag is per
	 * sample. So any STALE sample marks the sensor unreachable and any
	 * fresh one marks it reachable again. That is coarser than the liveness
	 * table, deliberately: Bridged Device Basic Information's Reachable
	 * describes the bridged NODE, and a strap whose heart-rate field has
	 * expired is not half-present.
	 */
	if (sSources[source].reachable != !stale) {
		sSources[source].reachable = !stale;
		sSources[source].reachableDirty = true;
	}
	k_mutex_unlock(&sShadowLock);
}

extern "C" void ant_matter_binding_changed(uint32_t source, const struct radiant_binding *b)
{
	if (source >= RADIANT_BINDING_MAX) {
		return;
	}

	k_mutex_lock(&sShadowLock, K_FOREVER);

	if (b == nullptr) {
		/* UNBIND, AND THIS IS THE ONLY PATH THAT REMOVES AN ENDPOINT.
		 * See ant_matter.h. */
		sSources[source].bound = false;
		sSources[source].reachable = false;
		sSources[source].reachableDirty = false;
		sSources[source].labelDirty = false;
		sSources[source].subtype = 0u;
		for (Row &r : sRows) {
			if (r.used && r.source == source) {
				r.needRemove = true;
			}
		}
	} else {
		sSources[source].bound = true;
		sSources[source].key = StableKey(b);
		memset(sSources[source].label, 0, sizeof(sSources[source].label));
		strncpy(sSources[source].label, b->label, sizeof(sSources[source].label) - 1);
		sSources[source].reachable = true;
		sSources[source].reachableDirty = true;

		/* The naming inputs the BINDING carries. The subtype does not
		 * come from here - it is learned from page 16 - and is cleared
		 * because this may be a different physical machine on a slot a
		 * treadmill used to hold. */
		sSources[source].devtype = b->devtype;
		sSources[source].devnum = b->devnum;
		sSources[source].subtype = 0u;
		/* A re-bind can change the device number or the human label of
		 * a source whose endpoints already exist, so the name has to be
		 * pushed rather than only used at creation. Harmless on a fresh
		 * bind: the rows do not exist yet, and the flag is per source
		 * and cleared once per pass. */
		sSources[source].labelDirty = true;
	}

	k_mutex_unlock(&sShadowLock);
}
