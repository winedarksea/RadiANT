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

#include <cstdio>
#include <cstring>

/* All three carry their own `extern "C"` guards, so they are included plainly.
 * Wrapping them in one here would put <zephyr/sys/iterable_sections.h> - which
 * radiant_bridge.h pulls in - inside C linkage, which is a way to break a C++
 * build that only shows up on an unlucky include order. */
#include "radiant_binding.h"
#include "radiant_bridge.h"
#include "radiant_matter.h"

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
	uint16_t deviceType;

	/* Filled in on the CHIP thread when the device is created or adopted
	 * from storage. kInvalidEndpointId until then. */
	EndpointId chipEndpoint;
	RadiantMatter::AntDataProvider *provider;

	bool needCreate;
	bool needRemove;

	AttrCell attrs[kMaxAttrsPerEndpoint];
};

Row sRows[RADIANT_MATTER_MAX_ENDPOINTS];

struct SourceState {
	bool bound;
	uint64_t key; /* the persisted identity - see TRAP 8 above */
	char label[16];
	bool reachable;
	bool reachableDirty;
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
	char label[sizeof(SourceState::label)];
	uint16_t deviceType;
	uint64_t key;

	k_mutex_lock(&sShadowLock, K_FOREVER);
	deviceType = row.deviceType;
	key = sSources[row.source].key;
	memcpy(label, sSources[row.source].label, sizeof(label));
	FormatUniqueID(uniqueID, sizeof(uniqueID), key, row.fieldId);
	k_mutex_unlock(&sShadowLock);

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

	Nrf::MatterBridgedDevice *device = MakeDevice(deviceType, uniqueID, label[0] != '\0' ? label : uniqueID);
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

	LOG_INF("matter: endpoint %u = radiant endpoint %u (source %u field %u, device type 0x%04x)", assigned,
		row.radiantEndpoint, static_cast<unsigned int>(row.source), row.fieldId, deviceType);
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

			/* int64_t and sizeof(int64_t), always. The device
			 * narrows and refuses what does not fit - see
			 * bridged_device_types/radiant_bridged_device.h. */
			provider->NotifyUpdateState(cell.cluster, cell.attribute, &value, sizeof(value));
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

	/* One clear per pass, after every row has seen it. */
	k_mutex_lock(&sShadowLock, K_FOREVER);
	for (SourceState &s : sSources) {
		s.reachableDirty = false;
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
			if (s.reachableDirty) {
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

extern "C" void ant_matter_note_sample(uint32_t source, uint8_t flags)
{
	bool stale = (flags & RADIANT_SAMPLE_STALE) != 0u;

	if (source >= RADIANT_BINDING_MAX) {
		return;
	}

	k_mutex_lock(&sShadowLock, K_FOREVER);
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
	}

	k_mutex_unlock(&sShadowLock);
}
