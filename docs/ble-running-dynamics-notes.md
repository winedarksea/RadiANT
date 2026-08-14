# Notes towards a Garmin-compatible BLE Running Dynamics strap

**Status: not implemented.** This is a pointers document, recording where the
facts below came from so a later implementation attempt does not have to
re-derive them. Nothing in `strap/` does any of this today — see "What exists
today" below for what `strap_ble.c` actually is.

## Why this document exists and not code

The question this document answers is whether the BLE-side reverse-engineering
covered in [`docs/device-profiles.md` §3.13](device-profiles.md) — the Garmin
HRM 600 protocol documented at
[dropbars.be/blog/reverse-engineering-garmin-hrm600-running-dynamics](https://dropbars.be/blog/reverse-engineering-garmin-hrm600-running-dynamics)
and [codeberg.org/samdumont/openrd-ble-running-dynamics](https://codeberg.org/samdumont/openrd-ble-running-dynamics)
— could be added to the example BLE strap this project already has. The
honest answer is: not as an afternoon's addition. It is a different, larger
kind of work than adding an ANT+ profile, for a structural reason worth
stating up front — **there is no SIG-standard BLE service for running
dynamics.** The Bluetooth SIG's Running Speed and Cadence service (`0x1814`)
carries speed, cadence and optionally stride length; it has no field for
ground contact time, vertical oscillation, ground contact balance, or
vertical ratio. The only thing that carries the full metric set over BLE, as
far as either source establishes, is **Garmin's own proprietary Multi-Link /
GFDI / protobuf stack** — not an open standard, and not related to the ANT+
`0x1E` page layout this project already implements (see §3.13's corroboration
note for why the two are unrelated transports that happen to agree on scale
factors).

So "add BLE running dynamics" concretely means: build a device that speaks
Garmin's own proprietary link and application protocol well enough that a
real Garmin watch accepts it as a running-dynamics-capable HRM. That is
achievable — `openrd-ble-running-dynamics` already did it on an ESP32 — but
it is a second protocol stack (framing, reliability, a handshake with twelve
required steps in order) sitting *inside* a single BLE connection, not a
page table.

## What exists today

`strap/src/strap_ble.c` advertises the **standard SIG Heart Rate Service**
(`0x180D`) and nothing else. It exists for P8b of the multiprotocol
coexistence plan — "does BLE arbitrate nicely with an ANT+ `0x78` master
running on the same radio" — and is explicitly scoped to have "no services,
no connection handling and no profile" beyond that one measurement. It has
never attempted Garmin protocol compatibility and was not written with that
in mind; a phone's native Bluetooth HR app can read it, a Garmin watch
cannot get running dynamics from it (nor was it ever going to — a Garmin
watch does not expect RD over the SIG HRS at all, per everything below).

## The protocol, as documented by openrd

Recorded here as `[community, reverse-engineered, MIT-licensed]`. Both
sources describe a black-box-observation methodology — GATT enumeration,
observed pairing/connection/activity traffic on hardware the researcher
owns — and explicitly state they did **not** decompile the Garmin app,
capture raw BLE traffic into the public repo, or handle bond material/LTKs/
firmware. That is the same posture this project's own
[ADR 0002](decisions/0002-clean-room-policy.md) takes towards the ANT link
layer, applied to a different transport; ADR 0002 itself does not cover this
source or this transport, and a real implementation attempt should get its
own ADR before code exists, for the same reason 0002 argues for writing the
policy first (see 0002's own "Status" line). The upstream repository is
`codeberg.org/samdumont/openrd-ble-running-dynamics`, MIT licensed.

### Discovery and services

| Purpose | UUID |
|---|---|
| Garmin Multi-Link service | `6a4e2800-667b-11e3-949a-0800200c9a66` |
| Multi-Link control characteristic | `6a4e2803-...` |
| Multi-Link notify (strap → watch) | `6a4e2810-...` |
| Multi-Link write (watch → strap) | `6a4e2820-...` |
| Standard Heart Rate service / HR Measurement char | `180D` / `2A37` |
| Standard Running Speed and Cadence service / char | `1814` / `2A53` |
| Battery / Device Information | `180F` / `180A` |
| Garmin discovery service-data UUID (advertised) | `FE1F` |

A device advertises HR + RSC + the `FE1F` service data; the watch discovers
the Multi-Link service after connecting.

### Framing: three layers deep before you reach a running-dynamics field

1. **Multi-Link reliable header** (2 bytes, every ATT write/notify):
   `[0x80 | (handle_index << 4) | req_hi]  [req_lo << 6 | seq6]` — a
   stop-and-wait reliability layer with its own 6-bit sequence number, sitting
   *underneath* everything else. Default ATT MTU (23 bytes) leaves only 18
   bytes of payload per fragment once this header and the ATT overhead are
   subtracted; openrd recommends negotiating MTU ≥ 128 to avoid fragmenting
   every message.
2. **GFDI envelope**, COBS-framed (leading/trailing `0x00`):
   `[length:2 LE] [msg_type:2 LE] [counter:2 LE] [payload] [crc16:2 LE]`.
   Two message-type families matter: fixed types like `0x1388` (ACK) and
   `0x13A0` (DEVICE_INFORMATION), and a "compact" family with bit 15 set,
   suffixed `0x..39` for a request/notification and `0x..3a` for the paired
   response — **always echoing the requester's counter**.
3. **A protobuf-shaped payload** inside the compact frames, keyed by
   `EventSharing` alert type. Type `20` carries heart rate; **type `21`
   carries `RunningMetrics.Dynamics` and `RunningMetrics.SpeedDistance`**,
   which is the payload this project would actually need to emit. Types `22`
   and `23` run the other direction (watch → strap): activity state and
   algorithm inputs (GPS speed, grade) the strap can optionally use to refine
   its own numbers.

### Type 21 field layout (what a strap would encode)

`RunningMetrics.Dynamics` (protobuf field numbers, as recovered):

| Field # | Name | Scale | Matches this project's ANT+ constant |
|---|---|---|---|
| 1 | `vertical_oscillation_1_4ths_mm` | 0.25 mm | `PROFILE_RD_VERT_OSC_PER_MM` |
| 2 | `ground_contact_time_ms` | 1 ms | `PROFILE_RD_INVALID_GCT`'s unit |
| 3 | `stance_time_1_4ths_percent` | 0.25 % | `PROFILE_RD_STANCE_PER_PERCENT` |
| 4 | `ground_contact_balance_1_32nds_percent` | 1/32 % | `PROFILE_RD_BALANCE_PER_PERCENT` |
| 5 | `vertical_ratio_1_32nds_percent` | 1/32 % | `PROFILE_RD_VERT_RATIO_PER_PERCENT` |
| 6 | `step_length_mm` | 1 mm | matches |
| 7 | `is_module_right_side_up` | bool | inverse of `profile_rd`'s `upside_down` |
| 8 | `cadence_1_32_strides_per_min` | 1/32 strides/min | `PROFILE_RD_CADENCE_PER_STRIDE_MIN` |
| 9 | `step_count` | 1 | matches |
| 10 | `is_walking` | bool | matches `profile_rd_metrics::walking` |
| 11 | `step_speed_loss_data` | undocumented | not in the ANT+ spec either; openrd never observed it in captures |

`RunningMetrics.SpeedDistance` (protobuf field 2 of the same message):
speed in 1/256 m/s, distance in 1/16 m — neither of which the ANT+ RD profile
carries at all (speed rides ANT+ page `0x10` from the *display*, in a
different fixed-point shape: 1/256 m/s there too, so the scale at least
agrees).

**Every scale factor above already exists in `src/profiles/profile_rd.h`**,
independently derived from the ANT+ spec. A BLE encoder would reuse
`struct profile_rd_metrics` as its source of truth and just re-pack the same
values into protobuf-shaped varints instead of ANT+'s bit-packed pages — the
physiology layer is shared; only the wire format differs.

### The handshake (order matters; skipping a step silently disables RD)

1. Advertise `FE1F` service data + HR/RSC UUIDs; accept LE Secure Connections
   bonding.
2. Watch subscribes to notify (`...2810`), then writes a registration request
   to `...2820`.
3. Strap sends `DEVICE_INFORMATION` (msg_type `0x13A0`) repeatedly until
   ACKed with `0x1388`.
4. Strap sends one specific compact `0x8132` session-preamble frame.
5. Watch sends a `FeatureCapabilitiesRequest` (compact `...39`).
6. Strap responds (compact `...3a`, same counter) advertising running
   support.
7. Watch subscribes to alert types 22/23 (its own outbound channels).
8. Strap ACKs both.
9. Strap sends two fixed Core notifications.
10. Watch subscribes to alert type 20, then types 20+21 together.
11. Strap ACKs both subscriptions.
12. Strap streams type `20` (HR) and type `21` (RD) continuously while
    moving.

Missing any single step leaves HR and pace displaying normally while RD stays
silently absent — which is exactly the failure mode that makes this worth
documenting precisely rather than approximating.

## If this gets built

Scope it as its own thing, not a `strap_ble.c` addition — the existing file's
job (coexistence load) and this job (protocol-compatible RD simulator) share
nothing but "runs on the BLE stack." A real attempt would want, roughly:

1. An ADR mirroring `docs/decisions/0002-clean-room-policy.md`'s shape:
   permitted sources (this document, the openrd repo's own public artifacts,
   this project's own on-air observation of hardware it owns), forbidden
   sources (nothing here forbids anything openrd itself didn't already avoid
   — no APK decompilation, no firmware, no captured Garmin traffic), and
   which files may read which.
2. A GFDI/COBS/Multi-Link framing layer — new code, unrelated to
   `radiant`'s ANT framing.
3. A protobuf-varint encoder for `RunningMetrics.Dynamics` — small, and it
   can share `struct profile_rd_metrics` as input.
4. The twelve-step handshake as an explicit state machine, since silent
   partial failure is the documented failure mode.
5. A real Garmin watch (or the openrd ESP32 firmware acting as one) to test
   against — none of this is verifiable against `tests/fake_radio.c`-style
   mocks the way `radiant`'s ANT+ profiles are, because the thing being
   proven is interoperability with hardware this project does not control the
   spec of.
