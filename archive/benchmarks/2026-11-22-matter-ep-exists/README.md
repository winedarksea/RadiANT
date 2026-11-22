# Matter endpoint-exists retry: forced repro on the nRF54L15 DK (2026-11-22)

This is the on-target check for e7fa93c ("keep the pair across an endpoint-exists retry").

**Image.** `apps/dongle_thread`, built as `thread.conf;matter.conf`, with:

- `-DSB_EXTRA_CONF_FILE=matter_sysbuild.conf -DANT_RADIO=core -DRADIANT_BACKEND=nrf`;
- for the bench, `CONFIG_ANT_DONGLE_PAIRING_WINDOW_AT_BOOT=y`, `CONFIG_ANT_DONGLE_PAIRING_WINDOW_S=600` and `CONFIG_RADIANT_LOG_LEVEL_INF=y`.

Both images carry a debug-only patch, never committed, in `AddSingleDevice()` before
"Assign the free endpoint ID". Once per boot, when another bridged device is live, it
sets `mCurrentDynamicEndpointId` to that device's endpoint id. The next auto-id add
therefore hits `CHIP_ERROR_ENDPOINT_EXISTS`.

| Run | Source | Forced id | Outcome |
|---|---|---|---|
| Fix | e7fa93c + patch | 3 (live) | `Dynamic endpoint 3 exists: retrying with 4`, then `Added device to dynamic endpoint 4 (index=1)`. No failure, no id skipped. |
| Control | c58ca20 + patch | 3 (live) | `Cannot retrieve bridged device from index 1`, then `AddBridgedDevices failed ...: ac` (CHIP_ERROR_INTERNAL). The pair was erased before the retry. |

In the control, the RadiANT glue retried the same field on its next sample. It landed
at endpoint 5 (`index=1`), so one add failed and id 4 was burned. Stock NCS has no
glue-level retry, so there the device is dropped outright, which is what the upstream
note describes. In RadiANT the effect was a failed add, a wasted endpoint id and up to
one sample period of delay before the sensor appears.

**First control attempt.** Without `-Erase`, the control boot restored endpoints 3–6
from the settings store and every self channel was already tracking a paired sensor.
Nothing new could pair, so the forced path never ran. A full-erase flash (`-Erase`)
cleared the store, and the second auto-id add on the next boot tripped the patch.

The flash also needs `-Serial 1057737173` while the 5340 DK probe is attached.
Without it, J-Link cannot pick a probe and the script still prints "Flashed."

Raw logs: `bench-logs/2026-11-22-matter-ep-exists/` (gitignored). Matching lines:
`console-lines.txt`.
