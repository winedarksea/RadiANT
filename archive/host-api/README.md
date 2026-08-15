# `archive/host-api/` — what a Windows host can actually ask for

Checked by: `python -m json.tool archive/host-api/ant_dll_exports.json` for
well-formedness, and — once the change request below lands —
`tools/ant_features.py`, which will read this JSON instead of carrying the same
facts a second time. Until then the `.txt` files and the JSON are cross-checked
only by having been generated together, in one pass, from one source.

On Windows every ANT application reaches the stick through `ANT_DLL.dll`.
**Its export table therefore bounds what any of them can ask a dongle for** —
not by convention, by construction: a function that is not exported cannot be
called. And the subset a particular application resolves bounds what *that*
application can ask for.

That turns "which optional ANT features are worth bridging" from an argument
into a lookup. It is the evidence behind the optional-feature table in
`README.md`, behind `CONFIG_ANT_DONGLE_ENCRYPTION` being off by default, and
behind the plan's decision not to implement ANT+'s own AES-CTR encryption in
`radiant_core` at all.

**Names and ordinals are facts.** An export table is a list of identifiers and
numbers describing an interface; recording it is the same kind of act as
recording a USB PID. The binaries themselves are **not committed** — see *What
is not here*.

## Files

| File | Contents |
|---|---|
| [`ant_dll_exports.txt`](ant_dll_exports.txt) | All **154** exports of `ANT_DLL.dll`, with ordinals, in ordinal order |
| [`zwift_ant_imports.txt`](zwift_ant_imports.txt) | The **44** of them that `ZwiftApp.exe` resolves |
| [`ant_dll_exports.json`](ant_dll_exports.json) | The same 154, joined against this project's own protocol table — the machine-readable one |

### The JSON

A flat array of objects, one per export, in ordinal order:

```json
{
  "name": "ANT_SetTransmitPower",
  "ordinal": 143,
  "mesg_id": 71,
  "in_sdk_ant": true,
  "bridged": true,
  "zwift_uses": true
}
```

| Field | Meaning | `null` when |
|---|---|---|
| `name` | Export name, exactly as it appears in the export table | never |
| `ordinal` | Export ordinal | never — every export here is named and ordinal-numbered |
| `mesg_id` | The ANT serial message this call puts on the wire, as a decimal integer (`71` = `0x47`) | the call is host-local, or the message's id is not yet in `protocol/ant_wire.yaml`. See below |
| `in_sdk_ant` | Does sdk-ant expose an API that could serve this? | not stated anywhere in this repository |
| `bridged` | Does `dispatch()` in `src/ant_serial_bridge.c` implement the message? | there is no message to implement |
| `zwift_uses` | Does `ZwiftApp.exe` resolve this name? | never — it is a boolean |

**72 of the 154 carry a `mesg_id`; 82 are `null`, in two distinct groups, and
the JSON cannot tell them apart.** That is a real limitation of the six-field
shape, so the groups are written out here instead:

*Host-local — there is no serial message and never will be.* `ANT_Init`,
`ANT_Close`, `ANT_Nap`, `ANT_LibVersion` (the DLL's own version, not the
device's), `ANT_GetDeviceUSBInfo`, `ANT_GetDeviceUSBVID`, `ANT_GetDeviceUSBPID`,
`ANT_AssignResponseFunction`, `ANT_AssignChannelEventFunction`,
`ANT_UnassignAllResponseFunctions`, `ANT_SetDebugLogDirectory`,
`ANT_WriteMessage` (a raw escape hatch that can carry *any* message), all 44
`ANTFS_*` entry points and all 4 `FIT_*` ones — ANT-FS and FIT are host-side
layers built on top of ordinary ANT channels, not single messages.

*Message exists, id not sourced here.* `ANT_SetSerialNumChannelId`,
`ANT_RSSI_SetSearchThreshold`, `ANT_CrystalEnable`, `ANT_SleepMessage`,
`ANT_ConfigUserNVM` and the six `ANT_NVM_*` calls. These are real ANT messages
whose ids `protocol/ant_wire.yaml` does not yet carry. **They are left `null`
rather than filled in from memory of the specification.** A wrong id here would
be worse than a missing one: it would look authoritative and it would silently
mis-drive `tools/ant_conformance.py`. Backfill them by adding the messages to
`protocol/ant_wire.yaml` — with a `source:` tag citing Rev 5.1 — and
regenerating, not by editing this JSON.

`_RTO` variants (`ANT_OpenChannel_RTO` and the like) are the same message with
a caller-supplied response timeout, and carry the same `mesg_id`, `bridged` and
`in_sdk_ant` as their base function. They are separate exports with separate
ordinals, so they are separate rows.

## How this was produced

Everything below is reproducible on any machine with Zwift installed. Nothing
was disassembled: the export directory and the name strings are read straight
out of the PE headers and the `.rdata` section.

**Source binaries**, both from a stock Zwift install, retrieved 2026-08-08:

| Binary | Path | Notes |
|---|---|---|
| `ANT_DLL.dll` | `C:\Program Files (x86)\Zwift\ANT_DLL.dll` | 247,416 bytes, SHA-256 `db4d99c5400085b1fce1988cd116c2250d7953238b31bd196419e0c6253d62f9`. 32-bit. Carries no `VERSIONINFO` resource, so there is no file version to record |
| `ZwiftApp.exe` | `C:\Program Files (x86)\Zwift\ZwiftApp.exe` | 47,353,464 bytes |

**Export names and ordinals** came from parsing the PE export directory
directly. The equivalent with standard tools, any one of which gives the same
list:

```powershell
# Visual Studio / Build Tools - the canonical one
dumpbin /exports "C:\Program Files (x86)\Zwift\ANT_DLL.dll"

# LLVM, if that is what is installed
llvm-readobj --coff-exports "C:\Program Files (x86)\Zwift\ANT_DLL.dll"
llvm-nm --extern-only --defined-only "C:\Program Files (x86)\Zwift\ANT_DLL.dll"

# MinGW binutils
objdump -p "C:\Program Files (x86)\Zwift\ANT_DLL.dll"
```

`llvm-nm` prints names but **not ordinals** — it is a fallback for confirming
the name list, not a substitute. `dumpbin /exports` and
`llvm-readobj --coff-exports` both give the ordinal column.

**Zwift's subset** is a whole-string scan, not an import-table read: the import
table is empty of `ANT_DLL.dll` because the DLL is loaded by name at runtime
and its entry points resolved individually. The names the loader asks for
therefore sit in the image as plain null-terminated ASCII. The match requires
the preceding byte to not be an identifier character and the following byte to
be `NUL`, so `ANT_AssignChannel` does not match inside
`ANT_AssignChannelExt` — getting that wrong inflates the count and was the one
place this was easy to fumble.

The result — 44 — independently corroborates the "~40 ANT functions it
resolves" already recorded in `README.md` from an earlier, separate look.

**`mesg_id` and `bridged`** are joined from
[`protocol/ant_wire.yaml`](../../protocol/ant_wire.yaml), which is this
project's single source of truth for the serial protocol and which carries a
`source:` provenance tag per message. **`in_sdk_ant`** is `true` wherever
`bridged` is `true` — the bridge can only forward what sdk-ant exposes — plus
the three explicit `nothing` verdicts in `README.md`'s optional-feature table
(`ANT_ConfigEventBuffer`, `ANT_ConfigHighDutySearch`, `ANT_ConfigUserNVM` and
the `ANT_NVM_*` family). Everywhere else it is `null`, because this repository
does not state it and **the sdk-ant tree may not be opened to find out** — that
read restriction is the clean-room boundary, and guessing around it would
defeat the point of having one.

## What the data says

**Zwift calls none of the optional features.** Not one of the 44 is
`ANT_ConfigureAdvancedBurst`, `ANT_ConfigSelectiveDataUpdate`,
`ANT_ConfigEventFilter`, `ANT_ConfigEventBuffer`, `ANT_ConfigHighDutySearch`,
`ANT_ConfigUserNVM` or any `ANT_NVM_*`. The three features this dongle bridges
were chosen because a host *could* reach them and the stack can do them, not
because anything asks.

**There is no encryption call in the export table at all.** `ANT_DLL.dll`
exports 154 functions and not one of them keys a channel or enables AES-CTR.
So `MESG_ENCRYPT_ENABLE` (`0x7D`), `MESG_SET_ENCRYPT_KEY` (`0x7E`) and
`MESG_SET_ENCRYPT_INFO` (`0x7F`) are **unreachable from any Windows ANT
application that exists**. That is the whole argument for
`CONFIG_ANT_DONGLE_ENCRYPTION` defaulting off, and for RadiANT replacing ANT+
encryption rather than reimplementing it: dead code that reserves stack RAM
shared with the plain channels cannot be paying for itself.

**`ANT_SetDebugLogDirectory` (ordinal 132) is here, and is a tool.** It makes
`ANT_DLL.dll` write `Device0.txt` — Garmin's own `Tx`/`Rx` byte log of a live
session. That file is the third implementation the checksum bug needed and
never had: golden frame vectors from a stack neither we nor our tools wrote.
It is the source for `tools/test_ant_wire.py` and for the `.antser` captures in
[`../captures/serial/`](../captures/serial/).

**Zwift resolves `ANT_OpenRxScanMode` and `ANT_EnableLED`** (ordinals 101 and
76). `ANT_EnableLED` still never sends `0x68`, because this dongle reports that
capability as off everywhere. `ANT_OpenRxScanMode` is different since
2026-08-10: the `radiant_core` backend's capabilities reply advertises scan
mode on (`sdk_ant` and `stub` still report it off), and `0x5B` is now bridged
to back that claim up — see `src/ant_serial_bridge.c`. That is the capability
bits doing real work either way: on the backends that report the bit off,
`bridged: false` in the JSON is consistent rather than contradictory, the
message is not implemented *and* not advertised; on `radiant_core`, `bridged`
is `true` for the same reason, an advertised-and-unimplemented capability being
the trap this project checks for at all.

## `2026-08-10-zwift-session-decoded.txt` — what Zwift actually sends

A decoded USBPcap recording of a real Zwift pairing session against this
dongle: 7614 bulk payloads, one line per ANT serial message, host and device
directions interleaved with timestamps. Captured on 2026-08-10 with scratch
tools (`cap_zwift.ps1`, `decode_pcap.py`) that no longer exist; this decode is
the surviving artifact, which is why it is here rather than regenerable.

It is the primary evidence for three things that were otherwise guesswork, and
in each case it **overturned** a reading taken from the code:

1. **Zwift's real discovery mechanism is `ASSIGN_CHANNEL`'s extended
   background-scan bit on a single wildcard channel 0** — not
   `MESG_OPEN_RX_SCAN_MODE` (`0x5B`), which the capture confirms Zwift never
   sends even after `0x5B` was implemented. Two documents in this repository
   had claimed otherwise.
2. **Zwift sets three network keys at startup and cycles channel 0 across
   networks 0, 1 and 2 every ~4-5 s**, tearing the channel down and rebuilding
   it each time. The first two lines of the capture show
   `NETWORK_KEY 00b9a521fbbd72c345` accepted and the other two refused.
3. **Zwift pairs a device within ~7 ms of the dongle first reporting it**, which
   is what established that time-to-pair is the dongle's latency and none of it
   is the application's.

No third-party binary is reproduced here — this is a recording of *our own
device's* traffic, one side of which happens to have been generated by Zwift.
The distinction is the same one `docs/preservation.md` draws for the radio
captures.

## What is not here

`ANT_DLL.dll` and `ZwiftApp.exe` are third-party binaries and are **not
committed**, under any name, in any form, including partially. There is no
redistribution right and no need: everything this project uses them for is in
the files above, and the procedure to regenerate those from your own copy
is written out in full.

Do not add them. `.gitignore` will not save you from a deliberate `git add -f`,
and 47 MB would blow the `archive/` budget in one commit anyway.
