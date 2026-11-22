# Matter bridge: endpoint-exists retry drops the device (draft for Nordic)

Status: draft, not posted. Posting it (DevZone or a GitHub issue on
`nrfconnect/sdk-nrf`) is the owner's call.

## Summary

In `applications/matter_bridge/src/core/bridge_manager.cpp` (NCS v3.4.0),
`BridgeManager::AddSingleDevice()` assigns an automatic endpoint id in a
`do { ... } while (err == CHIP_ERROR_SENTINEL)` loop. `CreateEndpoint()`
returns `CHIP_ERROR_SENTINEL` when `emberAfSetDynamicEndpoint()` reports
`CHIP_ERROR_ENDPOINT_EXISTS`, so that the loop retries with the next id.

The failure branch inside the loop erases the pair from `mDevicesMap` for any
error, SENTINEL included:

```cpp
err = CreateEndpoint(index, mCurrentDynamicEndpointId);

if (err != CHIP_NO_ERROR) {
	...
	mDevicesMap.Erase(index);
}
```

The retry's `CreateEndpoint()` then fails its `mDevicesMap.Contains(index)`
check and returns `CHIP_ERROR_INTERNAL`, so the loop exits and the device is
dropped. The retry can never succeed.

## When it happens

Whenever `mCurrentDynamicEndpointId` points at an id a live dynamic endpoint
already holds. Upstream raises the counter above every restored id, so a stock
bridge rarely gets there; it does after the counter wraps, or in any
derivative that seeds the counter differently (we hit it while reviewing a
persisted endpoint high-water mark).

## Fix

Erase only for errors other than SENTINEL:

```cpp
if (err != CHIP_NO_ERROR && err != CHIP_ERROR_SENTINEL) {
	...
	mDevicesMap.Erase(index);
}
```

The loop still terminates: at most `kMaxBridgedDevices - 1` other dynamic
endpoints exist and the counter advances once per try, so a free id is reached
within `kMaxBridgedDevices` tries. The explicit-id branch above it erases on
SENTINEL correctly, since a caller-chosen id cannot be retried.
