# Compatibility matrix: the release gate for the reference designs

**Status: skeleton.** Every result cell below is empty on purpose. The owner fills
them in on the bench. A row with an empty cell has not passed.

This file decides whether a dual-radio reference design (`apps/hrm_ble`,
`apps/treadmill`, `apps/smart_trainer`) can be released. Each product has a
**required set** of devices and apps (§4). Each of those is one row, and each row
runs the **checks** in §3 against the **pass criteria** in §2, once with the other
radio off and once with it active. A release needs every required row to pass on
the image being released. Results from an earlier image hash do not carry over.

The protocol tools in §5 are preconditions: run them before the device rows. They
catch on the bench, in minutes, the failures a device row would otherwise find in
an afternoon. They do not replace any row, and they are not Bluetooth SIG
qualification or ANT+ certification (§7).

---

## 1. What every row records

A result with any of these fields missing is not counted.

| Field | What to write |
|---|---|
| Device / app | Make and model, or app name, exactly as the owner names it. |
| Device firmware / app version | The version string the device or app reports (Settings → About, or the app store page). |
| Board | e.g. `nrf54l15dk/nrf54l15/cpuapp`, `nrf52840dk/nrf52840`. |
| Image hash | SHA-256 of the exact file that was flashed (`merged.hex` for a sysbuild image): `Get-FileHash <file> -Algorithm SHA256`. |
| RadiANT commit | `git rev-parse --short HEAD` of the build tree, plus `-dirty` if it was dirty. |
| Build | App, preset (e.g. `dual.conf` + `tests/product_test.conf`), and any extra `-D` flags. |
| Date, tester | |
| Evidence | Path to the transcript, capture, or saved-activity export under `archive/` or `captures/`. |

---

## 2. Pass criteria

| Criterion | Limit | How it is measured |
|---|---|---|
| Discovery | ≤ 10 s from power-on | Stopwatch from applying power until the device appears in the app or watch's sensor list. Report the worst of 3 power-ons. |
| Re-advertise | ≤ 2 s after disconnect | **Authoritative: the device's own console** (`BLE disconnected` → `BLE advertising (restart)`), via `reconnect-soak -DeviceLog` or `-Mode device-log` (§5.2), or a phone / nRF central. The Windows-side range from `reconnect-soak` alone is not authoritative: Windows holds a released link for ~9 s, so that figure measures Windows (see the note below this table). |
| Data resumes | ≤ 10 s after the link is restored | `reconnect-soak` for the Windows central (Connected → first notification; a cycle that never connected is not judged here). For a device row, stopwatch from the link coming back (the app shows it connected) until a value updates. |

**The Windows central limits the re-advertise and connect timing.** The first
soak, on 2026-11-27 (HRM, nRF54L15, 50 cycles), was compared against the
device's console:

- The device re-advertised 0.47–0.49 ms after every one of its 43 disconnects.
- The Windows upper bound was about 9 s on every clean cycle. Windows keeps a
  released link for about 8.9 s (its lazy close) before it disconnects.
- Windows took 2–27 s to connect to a device that was advertising the whole
  time, and 8 of 50 connects timed out at 30 s.
- In one stretch, Windows neither saw nor connected to the device for 60 s.

So the Windows numbers are host numbers. The re-advertise row is judged on the
device-log figure, or on a phone or nRF central. Connect time from Windows is
recorded but not judged.
| ANT+ loss, relative | Δ vs a same-window control ≤ **0.5 pp** | `max_delta_pp` in [`tools/ab_gates.toml`](../tools/ab_gates.toml) `[gates.coexistence]`, read by `tools/ant_ab.py`'s coexistence gate. |
| ANT+ loss, absolute | ≤ **1.5 %** with the other radio active | `absolute_ceiling_pct` in the same block. **Always record the absolute value**, even when the row is judged on Δ. |

The 0.5 pp and 1.5 % figures are quoted from `tools/ab_gates.toml` as it stood when
this file was written. If the two disagree, the TOML wins. Update this table in
the same commit that changes the TOML.

**Judging the ANT+ loss under the bench-room rule.** This room has no quiet
channel. Its loss floor has measured 5–8 % and moves from one sitting to the next.
So the relative check is judged against a control receiver measured in the *same
window*, never against a number from another day. The absolute check is judged
as follows:

- If the same-window control is itself under 1.5 %, judge the absolute value
  against 1.5 % directly.
- If the control is over 1.5 %, the room floor exceeds the ceiling and the
  absolute check cannot be judged in that window. Record the value and mark the
  check *re-run*. Repeat the absolute check in a quieter window or place before
  the row can pass. A pass on Δ alone is not enough.

A single A/B/A sitting in this room is not evidence either way.

---

## 3. The checks in every row

Each check runs twice. The *other radio* is the one this row is not testing:

- **ANT+ row:** BLE is advertising, and a BLE central (phone or Windows) is
  connected and subscribed.
- **BLE row:** ANT+ is broadcasting, and an ANT+ receiver (the watch or the
  dongle) is tracking it.

| # | Check | Procedure | Pass |
|---|---|---|---|
| C1 | Discovery | Power the device on. Search from the device or app under test. | Found within the §2 limit. |
| C2 | First pairing | Pair from a clean state: no bond on either side, and for equipment the pairing window open. | Paired, and data shows. |
| C3 | Bonded reconnect, window closed | With the bond from C2, close the pairing window (equipment) and reconnect. | Reconnects and encrypts using the stored keys. No new pairing is requested. |
| C4a | Reconnect after link drop | Drop the link from the central (app disconnect, or `reconnect-soak` for Windows). | Re-advertises and data resumes within the §2 limits. |
| C4b | Reconnect after going out of range | Carry the central out of range until the link drops (supervision timeout), then come back. | Reconnects without user action, if the app supports auto-reconnect. Data resumes within 10 s of the link coming back. |
| C4c | Reconnect after power cycle | Power-cycle the device under test. | Discovery within the C1 limit. The bonded central reconnects without re-pairing. |
| C5 | Bond storage full, then a replaced phone | **Equipment only. N/A for HRM.** Fill the bond table to `CONFIG_BT_MAX_PAIRED - 1`. Then pair a central the device has never seen, with the window open. Then with the window closed. | Window open: the new central pairs, and the least recently used bond is evicted, never a connected peer's. Window closed: the new central is refused, and no existing bond is touched. |
| C6 | Data present in a saved activity | Record a ≥ 5 min activity on the device or app and save it. Open the saved file or the app's activity page. | The profile's fields are present across the whole activity (HR for HRM; speed, distance and incline for the treadmill; power, cadence and speed for the trainer). There are no gaps longer than the app's own dropout marker. |
| C7 | ANT+ loss | ANT+ rows only. `tools/ant_ab.py` coexistence case: the same backend with the second stack on vs off, in one window, with a same-window control. | §2 relative and absolute criteria. |

C1–C6 are filled for both *other radio off* and *other radio on*. C7 exists only
with the other radio on, and is filled in the "on" column.

---

## 4. Required set and results

The owner names exact models and versions when M1 starts. The HRM rows are
already named. Leave a cell empty until it has been run. Write PASS, FAIL or N/A,
plus one line of evidence, e.g. `PASS 6.1 s worst of 3`.

### 4.1 HRM (`apps/hrm_ble`, dual preset)

| Row | Device / app | Radio | Device firmware / app version | Board | Image hash | RadiANT commit |
|---|---|---|---|---|---|---|
| HRM-1 | Garmin Forerunner 965, External HR sensor | ANT+ (0x78) | not recorded | nRF54L15 DK | `7d99c4ab…` | a899c1c |
| HRM-2 | Pixel 10 XL + nRF Connect for Mobile | BLE HRS (0x180D) | not recorded | nRF54L15 DK | `7d99c4ab…` | a899c1c |
| HRM-3 | Zwift on Windows | BLE HRS (0x180D) | | | | |

| Check | HRM-1 off | HRM-1 on | HRM-2 off | HRM-2 on | HRM-3 off | HRM-3 on |
|---|---|---|---|---|---|---|
| C1 discovery ≤ 10 s | | found, not timed (M1) | | found, not timed (M1) | | |
| C2 first pairing | | PASS (M1 step 1) | | PASS, connect (M1 step 1) | | |
| C3 bonded reconnect | | | | | | |
| C4a link drop | | PASS, undisturbed (M1 step 4) | | PASS (M1 step 4) | | |
| C4b out of range | | | | | | |
| C4c power cycle | | PASS (M1 step 4) | | PASS (M1 step 4) | | |
| C5 bonds full / replaced phone | N/A | N/A | N/A | N/A | N/A | N/A |
| C6 data in saved activity | | PASS (M1 step 2) | | | | |
| C7 ANT+ loss (abs %, Δ pp) | — | PASS 1.14 %, −0.04 pp (M1 step 6, Garmin USB-m receiver) | — | | — | |

Notes for HRM: ANT+ has no pairing or bond, so for HRM-1, C2 means the watch's
sensor pairing (device number lock) and C3 means the watch re-finding the paired
sensor. HRS has no Control Point, so no FTMS-style control check applies. A
BLE-only app (nRF Connect) produces no saved activity, so for HRM-2, C6 is the
nRF Connect log export, held for ≥ 5 min.

### 4.2 Treadmill (`apps/treadmill`, dual preset)

| Row | Device / app | Radio | Device firmware / app version | Board | Image hash | RadiANT commit |
|---|---|---|---|---|---|---|
| TM-1 | Garmin watch, Foot Pod sensor (owner names model) | ANT+ SDM (0x7C) | | | | |
| TM-2 | Zwift (FTMS) | BLE FTMS (0x1826) | | | | |
| TM-3 | A phone FTMS app (owner names app + phone) | BLE FTMS (0x1826) | | | | |

| Check | TM-1 off | TM-1 on | TM-2 off | TM-2 on | TM-3 off | TM-3 on |
|---|---|---|---|---|---|---|
| C1 discovery ≤ 10 s | | | | | | |
| C2 first pairing | | | | | | |
| C3 bonded reconnect, window closed | | | | | | |
| C4a link drop | | | | | | |
| C4b out of range | | | | | | |
| C4c power cycle | | | | | | |
| C5 bonds full / replaced phone | N/A | N/A | | | | |
| C6 data in saved activity | | | | | | |
| C7 ANT+ loss (abs %, Δ pp) | — | | — | | — | |

### 4.3 Smart trainer (`apps/smart_trainer`, dual preset)

| Row | Device / app | Radio | Device firmware / app version | Board | Image hash | RadiANT commit |
|---|---|---|---|---|---|---|
| ST-1 | Garmin watch or Edge, Smart Trainer sensor (owner names model) | ANT+ FE-C (0x11) | | | | |
| ST-2 | Zwift (FTMS) | BLE FTMS (0x1826) | | | | |
| ST-3 | Zwift (FE-C) | ANT+ FE-C (0x11) | | | | |

| Check | ST-1 off | ST-1 on | ST-2 off | ST-2 on | ST-3 off | ST-3 on |
|---|---|---|---|---|---|---|
| C1 discovery ≤ 10 s | | | | | | |
| C2 first pairing | | | | | | |
| C3 bonded reconnect, window closed | | | | | | |
| C4a link drop | | | | | | |
| C4b out of range | | | | | | |
| C4c power cycle | | | | | | |
| C5 bonds full / replaced phone | N/A | N/A | | | N/A | N/A |
| C6 data in saved activity | | | | | | |
| C7 ANT+ loss (abs %, Δ pp) | — | | — | | — | |

For the trainer rows, C6 also checks that the controlling app's ERG or SIM
targets took effect. The saved power follows the target the app set. A trainer
that reports data but ignores control fails C6.

---

## 5. Protocol preconditions (Windows host radio)

Run these on the release image before the device rows, and record them as
results in the same way. They are all `scripts/ble_central.ps1` modes. Add
`-Json <file>` to keep the evidence.

### 5.1 `ftms-conformance` (treadmill and trainer)

```powershell
.\scripts\ble_central.ps1 -Mode ftms-conformance -Machine treadmill -Json ftms-treadmill.json
.\scripts\ble_central.ps1 -Mode ftms-conformance -Machine bike -Name "<trainer name>" -Json ftms-trainer.json
```

This mode pairs the host Just Works, so open the pairing window first. It first
unpairs Windows to test the unencrypted Control Point write. If the peripheral
still holds a bond for this host from an earlier run, clear the peripheral's
bonds first. Alternatively, pass `-SkipUnencrypted`, which reuses the bond and
skips that case.

**Pass criteria:**

- No FAIL.
- Every INCONCLUSIVE is re-run or explained in the evidence line.
- `-Strict` turns an INCONCLUSIVE into a failing exit, for scripted runs.

The cases, and what each one detects, are in the header and body of
`tools/ble_central_ftms.cs`. The tool's case IDs follow the structure of the SIG
FTMS test suite but are not FTMS.TS test IDs.

| Product | Image hash | Result (pass / fail / inconclusive / skip counts) | Evidence |
|---|---|---|---|
| Treadmill | | | |
| Smart trainer | | | |

### 5.2 `reconnect-soak` (all three apps)

```powershell
.\scripts\ble_central.ps1 -Mode reconnect-soak -Name "RadiANT HR" -Notify hrs -Cycles 50 -Json soak-hrm.json
.\scripts\ble_central.ps1 -Mode reconnect-soak -Name "RadiANT Treadmill" -Notify treadmill -Cycles 50 -Json soak-tm.json
.\scripts\ble_central.ps1 -Mode reconnect-soak -Name "<trainer name>" -Notify bike -Cycles 50 -Json soak-st.json
```

**Pass criteria:** all three summary lines PASS:

- re-advertise ≤ 2 s
- data ≤ 10 s
- every cycle clean

Capture the device console for the whole soak and pass it to the tool:

```powershell
.\scripts\ble_central.ps1 -Mode reconnect-soak -Name "RadiANT HR" -Cycles 50 -DeviceLog soak-console.log -Json soak-hrm.json
.\scripts\ble_central.ps1 -Mode device-log -DeviceLog soak-console.log   # offline, after the fact
```

With `-DeviceLog`, the re-advertise verdict comes from the device log. Without
it, the Windows bracket can only PASS (upper bound ≤ 2 s) or be INCONCLUSIVE.
It never FAILs unless the drop was confirmed and the scanner gap was measured.
In this room the Windows bracket is about 9 s, so expect INCONCLUSIVE.

The *every cycle clean* line is INCONCLUSIVE, not FAIL, when the only problems
are host-side: connect timeouts, or no sighting of the device advertising
again. An error after the link is up (subscribe failed, no notification) is a
FAIL.

Also record the *scanner sighting gap*. The tool measures it in a 15 s window
before cycle 1 (`-GapSeconds`). Re-advertise cannot be resolved more finely
than that gap.

By default the scan is paused while each connection is set up. The slow
connects in the first run fit the scan and the connection competing for the
radio controller, but that is not proven. `-WatchWhileConnecting` restores the
old behaviour so the two can be compared. The JSON records, for each cycle:

- wall-clock times, to line up with the console's host-time prefix
- every connection-status change the link and the probe saw
- connects that came up after the tool had given up

`tools/ble_central_soak.cs` documents how the tool forces a real disconnect on
Windows and how it brackets the re-advertise time.

| App | Image hash | Re-advertise, device log (median / max) | Re-advertise, Windows ub (median / max) | Data (median / max) | Clean cycles | Scanner gap | Result |
|---|---|---|---|---|---|---|---|
| HRM | `7d99c4ab…` | 0.478 / 0.493 ms | 21.9 / 28.7 s | 1.05 / 1.26 s | 43/50 connected | n/a (0 sightings) | PASS (re-advertise, data); clean INCONCLUSIVE (host) |
| Treadmill | | | | | | | |
| Smart trainer | | | | | | | |

---

## 6. The "manufacturer-ready" wording gate

How the docs describe a design is gated, as well as the release.

- A reference design is **not** described as *manufacturer-ready* until it has
  passed its §4 rows behind the source seam with real hardware:
  - HRM: one **real sensor driver**, i.e. a real heart-rate front end feeding
    `hrm_sensor`, not the simulator.
  - Treadmill or trainer: one **real controller integration**, i.e. a real
    motor or brake controller behind `treadmill_source` / `trainer_source`.
- Until then, each reference doc, its README line and any release notes say
  **"reference design, simulator-validated"**. Passing the matrix with the
  simulator source behind the seam proves the radio, protocol and bond
  behaviour. It does not prove the seam carries a real machine.
- When a design clears the gate, update this section with the driver or
  integration, its commit, and the rows it passed. The wording in the doc
  changes in the same commit.

| Design | Real driver / integration | Commit | Rows passed | Wording allowed |
|---|---|---|---|---|
| HRM | | | | reference design, simulator-validated |
| Treadmill | | | | reference design, simulator-validated |
| Smart trainer | | | | reference design, simulator-validated |

---

## 7. What remains the manufacturer's job

Each reference doc carries a handoff section that lists these. Nothing in this
matrix does them:

- **Bluetooth SIG qualification:** the product listing and its QDID. The tools
  in §5 follow the FTMS test suite's shape, but they are not a qualification run.
- **ANT+ interoperability and certification checks**, for each device profile
  the product claims.
- **Regulatory testing** (FCC, ISED, CE/RED, and the rest) for the product's
  radio, antenna and enclosure.


HRM evidence: `archive/benchmarks/2026-11-27-hrm-m1/README.md` (M1, 2026-11-27). Image hash in full: `7d99c4abd1b971f87c830e0ad6e6e2d5ba922d26ae07fdbdaa7bc158297f80d1`. In M1 the ANT+ and BLE radios were always on together, so only the "on" columns are filled.
