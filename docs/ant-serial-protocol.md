# The ANT serial protocol

Checked by: `scripts/gen_ant_wire.py --check`

<!-- SPDX-License-Identifier: Apache-2.0 -->

This is the host-to-dongle protocol: the bytes that cross a USB bulk endpoint,
or a UART on a part with no USB peripheral. It is implemented by
[`src/ant_serial_bridge.c`](../src/ant_serial_bridge.c) on the device side and
by the five tools in [`tools/`](../tools/) on the host side. Nothing here
describes the on-air link layer; that lives in `docs/ant-radio-link.md`.

Every constant below comes from
[`protocol/ant_wire.yaml`](../protocol/ant_wire.yaml) and is rendered from it
into three places at once: `src/ant_wire.h`, `tools/ant_wire.py`, and the
generated region at the bottom of this file. Edit the YAML and re-run the
generator; nothing else.

The naming authority is the *ANT Message Protocol and Usage Rev 5.1*
(D00000652), which is free to download and requires no login. Values were
extracted from this repository, not from sdk-ant's headers — see *Provenance*.

---

## Framing

Every message, in both directions, is the same five-part frame:

```
[SYNC] [LEN] [ID] [payload: LEN bytes] [XOR]
```

- **SYNC** is `0xA4`. It is the only byte the parser resynchronises on, so a
  stream that loses framing recovers at the next `0xA4` — which means a payload
  byte that happens to be `0xA4` can start a false frame. The checksum is what
  throws that frame away again.
- **LEN** counts the payload only, and the payload almost always begins with a
  channel number. `LEN` therefore counts that channel byte. Total frame length
  is `LEN + 4`.
- **ID** is the message identifier, tabulated below.
- **XOR** is the checksum. See the next section; it is the single most
  expensive thing in this document to get wrong.

There is no escaping, no bit stuffing and no length-prefixed transport framing
underneath. On USB the frames are simply concatenated into bulk transfers and a
frame may span two of them, which is why every host-side reader here is a
reassembling state machine rather than a `read()` that expects one message.

**The largest legal frame is 42 bytes**, from a LEN byte at its maximum of 38.
Size every buffer from that. There are two ways to get it wrong and both are
quiet. Reasoning from the longest message a *host* sends — one info-type byte
plus 19 of encryption user data — gives 20, and a `msg_body[20]` makes
`handle_burst()` compute `size = 19`, fail its `size % 8` check, and reject
every 24-byte advanced-burst packet without a word. Reasoning from
`MESG_MAX_SIZE` gives 41, which is one short, because the LEN byte is allowed
to count a channel byte that `MESG_MAX_DATA_SIZE` does not — so a full
extended-data message writes its checksum one past the end of the buffer, from
sdk-ant's work-queue thread. `src/usb_ant_class.c:30` and
`src/usb_ant_class_next.c:55` both independently state the 42.

## The checksum rule, worked

**The XOR covers every byte from SYNC through the last payload byte,
inclusive.**

This is the one deliberate duplication between `README.md` and `docs/` in this
project, and it is deliberate because its absence cost a week. The parser once
seeded its running XOR at `0` instead of at the SYNC byte it had just consumed.
Every checksum it computed was off by exactly `0xA4`, so every frame a real
host sent was dropped without a word and every frame the dongle sent was
rejected at the other end. Nothing else looked wrong — the device enumerated,
bound its driver, and Zwift listed it as present. It simply never executed a
single command.

The reason a test suite cannot save you here is that the host-side tools had
the *same* bug. Two implementations that both omit SYNC agree with each other
perfectly, and with nobody else. Every green test was two wrong implementations
shaking hands.

Worked, on the shortest message with a guaranteed reply — *request
capabilities*, payload `[channel 0x00, requested id 0x54]`:

```
byte:      A4    02    4D    00    54
running:   A4 -> A6 -> EB -> EB -> BF
           ^     ^     ^     ^     ^
           SYNC  LEN   ID    ch    req id
```

- `0xA4 ^ 0x02 = 0xA6`
- `0xA6 ^ 0x4D = 0xEB`
- `0xEB ^ 0x00 = 0xEB`
- `0xEB ^ 0x54 = 0xBF`

The frame on the wire is `a4 02 4d 00 54 bf`.

Omit the SYNC byte and you compute `0x02 ^ 0x4D ^ 0x00 ^ 0x54 = 0x1B` instead.
Note that `0xBF ^ 0x1B = 0xA4`: **the error is always exactly the SYNC byte**,
in every frame, in both directions. If you are debugging a dongle that answers
nothing at all, XOR the checksum you computed against the one on the wire
before you look anywhere else.

The reference implementation is `checksum()` in
[`tools/ant_wire.py`](../tools/ant_wire.py), and the same three lines appear as
`running_xor` in the bridge's `process_byte()`.

## Request and response

The protocol has three shapes of exchange, and confusing them is the second
most common way to misread a capture.

1. **Command, acknowledged.** The host sends a configuration message; the
   dongle answers with `MESG_RESPONSE_EVENT` (`0x40`) carrying
   `[channel, the command's own id, code]`. Code `0x00` is
   `RESPONSE_NO_ERROR`; anything else is a refusal.
2. **Request, answered with data.** The host sends `MESG_REQUEST` (`0x4D`) with
   `[index, wanted id]` and the dongle replies with the message that was asked
   for. A refusal comes back as a `0x40` naming `0x4D`, not the wanted id.
   For several messages the leading `index` byte is **not a channel**: it is a
   selective-data-update mask number, an encryption info type, or `0`/`1`
   selecting advanced-burst capabilities versus current configuration.
3. **Unsolicited channel event.** A `0x40` whose *middle* byte is
   `MESG_EVENT_ID` (`0x01`) rather than a message id. Byte 2 is then an event
   code, not a response code, and the two share one number space. A reader that
   does not check the middle byte will report `EVENT_TX` (`0x03`) as a reply to
   message `0x03`.

`MESG_SYSTEM_RESET` (`0x4A`) is the exception to (1): it is answered by
`MESG_STARTUP_MESG` (`0x6F`) and no `0x40` at all. It resets the ANT *stack*,
not the MCU — every host library opens a session with it and then keeps using
the same USB handle, so rebooting here makes that handle stale at the exact
point every session begins.

## Extended receive messages

By default a received broadcast is anonymous: `[channel, data[8]]` and nothing
more, so you can hear a sensor but not name it. `MESG_ANTLIB_CONFIG` (`0x6E`)
turns on up to three extra fields, appended after the payload behind a single
flag byte at offset 9.

The fields always arrive in the same order — channel id, RSSI, receive
timestamp — but **each is present only if its flag bit is set**, so an offset is
only correct relative to which of the earlier fields turned up. Reading RSSI at
a fixed offset works right up until a run is made without the channel id, and
then quietly reports the device number as a signal strength.

Two settings matter in practice:

- **`0x80`** — device id only. What `ant_scan.py` and `ant_session.py` ask for:
  enough to name a sensor, nothing else.
- **`0xE0`** — channel id **and** RSSI **and** receive timestamp. What
  `ant_verify.py` asks for, and what `ant_core` must assemble. The timestamp is
  16 bits of the radio's own 32768 Hz counter, so it wraps every two seconds —
  and it is the only clock on this path that is not the host's. It reads
  0.009 ms of timing error where the host clock reads 2.6 ms, which is why the
  A/B timing gate is read against it and not against arrival times.

Sending `0x6E` with a zero configuration byte clears all of them; there is no
separate clear message on the wire.

## Burst

`MESG_BURST_DATA` (`0x50`) and `MESG_ADV_BURST_DATA` (`0x72`) stream a transfer
as a run of frames. Byte 0 of the payload is not a channel number:

```
bit:  7     6  5     4  3  2  1  0
      last  sequence  channel (5 bits)
```

Five bits of channel is why **32 channels is the serial protocol's natural
ceiling**, and why `ant_core` is sized for 32 from the first line rather than
retrofitted later.

There is no per-packet acknowledgement on success — like a real stick, the host
learns the outcome once from `EVENT_TRANSFER_TX_COMPLETED` or `_TX_FAILED`. The
device side holds a single 24-byte block behind a semaphore released only by
`EVENT_TRANSFER_NEXT_DATA_BLOCK`, `_TX_COMPLETED` or `_TX_FAILED`. That event is
consumed internally and never reaches the wire; a stack that fails to raise it
exactly once per accepted block stalls host bursts by 1000 ms per packet.

## Capabilities

`MESG_CAPABILITIES` (`0x54`) is what a host reads to decide what it is talking
to. Two things about it are easy to get wrong and are worth stating before the
decode:

**It reports what the ANT *stack* can do, not what the bridge implements.**
Those are separate lists and nothing keeps them in step. A bit that is
advertised and unimplemented is a trap: the host reads the bit, sends the
message, gets `INVALID_MESSAGE` back, and gives up.
[`tools/ant_features.py`](../tools/ant_features.py) exists to walk every
optional bit and probe the message behind it, and it knows which mismatches are
deliberate.

**The mismatches that remain are deliberate.** High duty search and encryption
are advertised and not bridged. Clearing those bits would make this dongle
report something a real ANTUSB-m does not, and hosts read these bytes to decide
what they are talking to — reporting an ANTUSB-m's capabilities and declining
the message is closer to the device being impersonated than reporting
capabilities no ANTUSB-m has ever reported.

The reply this firmware gives is `080800b23200fd8d0f`, identically on nRF52840
over USB and on nRF54L15 over UART. It is decoded byte by byte and bit by bit
in the generated section below — generated, so that it cannot drift away from
the constants the firmware is built with. Two of its zeros are load-bearing:
scan mode and LED are both reported **off**, which is exactly why a host that
holds `ANT_OpenRxScanMode` and `ANT_EnableLED` never sends `0x5B` or `0x68`.

## Provenance, and the clean-room boundary

This is a clean-room rebuild, and the boundary is expressed as read scope
rather than as a promise. Every constant in `protocol/ant_wire.yaml` carries a
`source` tag recording where it came from:

| Tag | Means |
|---|---|
| `bridge` | a visible use site in `src/ant_serial_bridge.c`, usually with the value stated in its own prose comment |
| `stub` | `src/ant_stub.c` |
| `tools` | one of the five Python tools, which hand-duplicated these values independently of the firmware |
| `readme` | `README.md`, including its decoded examples |
| `observed` | read off a real reply from real hardware |
| `rev5.1 sec ...` | the free spec, which is the naming authority |
| `inferred` | forced by arithmetic or by an adjacent value in one of the above |

sdk-ant's headers were not opened while writing the YAML. Where that leaves a
value genuinely unrecoverable, the entry is marked `verify: sdk-ant-shim` and
**no macro is emitted**. `src/ant_radio_sdk_ant.c` is the one translation unit
permitted to include both `ant_wire.h` and sdk-ant's `ant_parameters.h`; it
recovers the value, `BUILD_ASSERT`s it, and the number comes back here
afterwards. Inventing a plausible number would be worse than leaving the hole,
because a wrong dispatch id fails silently and a missing macro fails at compile
time.

## Why this file is generated, and not cross-checked

The obvious alternative is a CI test that compares `src/ant_wire.h` against
`tools/ant_wire.py`. It is worse in four ways. It costs the same to write. It
leaves two files to hand-edit, so the duplication survives and only the
disagreement is caught. It is structurally blind to a constant that exists on
one side only, which is the commonest way the two drift.

And decisively: **it would have passed during the checksum bug.** Both sides
had `SYNC = 0xA4` and agreed perfectly. That bug was in a *rule*, not in a
constant, and no amount of constant-comparison reaches it.

What does reach rule-level drift is a third implementation that neither side
wrote. Two are available and both run in CI without hardware: golden frame
vectors in `tools/test_ant_wire.py` taken from Garmin's own `Device0.txt` (via
`ANT_SetDebugLogDirectory`), and the `.antser` captures replayed by
`tools/test_ant_golden.py`. Generation is for making one number appear in three
places without anyone typing it three times. That is all it claims, and the
claim is checked by `scripts/gen_ant_wire.py --check`.

One thing stays out of the generator on purpose:
[`tools/ant_pages.py`](../tools/ant_pages.py). Its constants are ANT+ *profile*
semantics with real reasoning attached, it is the shared contract with the
`zephyr_aerosense` project, and it already has tests. `ant_scan.py`'s
`ANT_PLUS_KEY` stays where it is too — that is a network key, not a protocol
constant.

---

<!-- BEGIN GENERATED: ant_wire (scripts/gen_ant_wire.py) -->

*Everything from here to the end marker is generated from `protocol/ant_wire.yaml`. Edit the YAML, not this section.*

## Framing constants

| Constant | Value | Meaning | Provenance |
|---|---|---|---|
| `ANTW_SYNC_TX` | `0xA4` | SYNC byte on every frame the host sends and on every frame this dongle sends. The only value the parser resynchronises on. | bridge |
| `ANTW_SYNC_RX` | `0xA5` | Alternate SYNC used by the bidirectional/asynchronous serial variants of the protocol. Never emitted or accepted by this dongle; recorded so a parser can recognise and reject it. | rev5.1 sec 7.1 + verify:sdk-ant-shim |
| `ANTW_MSG_OVERHEAD` | `4` | Bytes of frame around the payload: SYNC + LEN + ID + checksum. Total frame length is LEN + 4. | bridge |
| `ANTW_MAX_SIZE_VALUE` | `38` | Largest value the LEN byte may carry. src/usb_ant_class.c:30 states it outright - 'a size byte's worth of body at its MESG_MAX_SIZE_VALUE maximum of 38' - and both USB class files size their frame buffer from the resulting 42. An earlier draft of this file said 20, reasoning from the encryption path: one info-type byte plus ENCRYPTION_USER_DATA_SIZE fills exactly 20, and ant_features.py calls that 'the longest message the parser ever sees'. That is the longest message the *host* sends, not the ceiling. An advanced-burst data message is one channel byte plus ADV_BURST_BLOCK_MAX, so LEN reaches 25 on its own, and an extended receive message adds a flag byte plus three appended fields on top of a payload. Sizing msg_body[] from 20 makes handle_burst() see size = 19, fail its size % 8 check, and silently reject every 24-byte burst packet. | src/usb_ant_class.c:30 |
| `ANTW_MAX_DATA_SIZE` | `37` | MAX_SIZE_VALUE minus the leading channel/index byte that almost every message carries. | src/usb_ant_class.c:30, bridge |
| `ANTW_MESG_MAX_SIZE` | `41` | MAX_DATA_SIZE + MSG_OVERHEAD, and a trap: it is one byte SHORT of the largest frame that can legally arrive, because the LEN byte is allowed to reach MAX_SIZE_VALUE, which counts the channel byte that MAX_DATA_SIZE does not. A full extended-data message sized from this writes its checksum one past the end of the buffer and hands the USB layer a length one over it - from sdk-ant's work queue thread. Size buffers from MAX_FRAME_SIZE, never from this. | bridge |
| `ANTW_MAX_FRAME_SIZE` | `42` | MAX_SIZE_VALUE + MSG_OVERHEAD. The correct buffer size, and the 42 both USB class files independently quote. | src/usb_ant_class.c:30, src/usb_ant_class_next.c:55 |
| `ANTW_ANT_MAX_PAYLOAD_SIZE` | `8` | An ANT data payload: 8 bytes, on air and on the wire. Also the width of a selective-data-update mask. | stub |
| `ANTW_CHANNEL_NUM_SIZE` | `1` | The leading channel-number byte. Several reply sizes are written as <payload> + CHANNEL_NUM_SIZE precisely because the reply leads with an index that is not part of the data. | bridge |
| `ANTW_ADV_BURST_BLOCK_MAX` | `24` | Largest advanced-burst block in one serial frame. Plain burst carries 8; advanced burst carries 8, 16 or 24. | bridge |

## Messages

`dir` is `h2d` host to dongle, `d2h` dongle to host, `both`, or `marker` for an identifier that is a field inside another message rather than a frame of its own. `len` is the LEN byte, so it counts the leading channel or index byte where the message has one. `br` is whether `dispatch()` in `src/ant_serial_bridge.c` implements it today.

| ID | Name | dir | len | br | Reply | Meaning | Provenance |
|---|---|---|---|---|---|---|---|
| `0x00` | `ANTW_MESG_INVALID_ID` | marker |  | no | - | Reserved: never a valid message id. | rev5.1 sec 9.3 + verify:sdk-ant-shim |
| `0x01` | `ANTW_MESG_EVENT_ID` | marker |  | yes | - | Not a frame. Byte 1 of a RESPONSE_EVENT payload is 0x01 when the message is an unsolicited channel event rather than a reply to a command; the event code is byte 2. | bridge, tools (ant_session.EVENT_MARKER) |
| `0x3E` | `ANTW_MESG_VERSION_ID` | d2h | 20 | yes | - | Null-padded ASCII version string, MESG_VERSION_SIZE bytes. Requested with MESG_REQUEST. A stub build answers STUB0.01B00, which is deliberately not a real ANT version string. | tools |
| `0x40` | `ANTW_MESG_RESPONSE_EVENT_ID` | d2h | 3 | yes | - | [channel, message id or 0x01, code]. Every command gets one, and every channel event arrives as one with 0x01 in the middle byte. | bridge |
| `0x41` | `ANTW_MESG_UNASSIGN_CHANNEL_ID` | h2d | 1 | yes | `MESG_RESPONSE_EVENT_ID` | [channel]. Refused with CHANNEL_IN_WRONG_STATE until the channel has actually closed - a close is asynchronous. | tools |
| `0x42` | `ANTW_MESG_ASSIGN_CHANNEL_ID` | h2d | 3..4 | yes | `MESG_RESPONSE_EVENT_ID` | [channel, channel type, network, (extended assignment)]. The fourth byte is optional and defaults to 0x00. | bridge, tools |
| `0x43` | `ANTW_MESG_CHANNEL_MESG_PERIOD_ID` | both | 3 | yes | - | [channel, period lo, period hi] in 1/32768 s counts. 8070 is the ANT+ heart-rate period, 8182 the power period. | bridge, tools |
| `0x44` | `ANTW_MESG_CHANNEL_SEARCH_TIMEOUT_ID` | h2d | 2 | yes | `MESG_RESPONSE_EVENT_ID` | [channel, timeout] in 2.5 s increments; 0xFF means never time out. | bridge, tools |
| `0x45` | `ANTW_MESG_CHANNEL_RADIO_FREQ_ID` | both | 2 | yes | - | [channel, offset from 2400 MHz]. ANT+ is 57, i.e. 2457 MHz. | bridge, tools |
| `0x46` | `ANTW_MESG_NETWORK_KEY_ID` | h2d | 9 | yes | `MESG_RESPONSE_EVENT_ID` | [network number, key[8]]. The key travels host to dongle, which is why no build of this firmware needs a network-key secret. | bridge, tools |
| `0x47` | `ANTW_MESG_RADIO_TX_POWER_ID` | h2d | 2 | yes | `MESG_RESPONSE_EVENT_ID` | [filler, level]. Device-wide transmit power - what ANT_SetTransmitPower() sends, and what Zwift calls while setting up a search. Answering it with INVALID_MESSAGE stalls the search. | bridge, readme |
| `0x48` | `ANTW_MESG_RADIO_CW_MODE_ID` | h2d | 3 | no | - | Continuous-wave test transmit. Not bridged. | rev5.1 sec 9.5, host-api |
| `0x49` | `ANTW_MESG_SEARCH_WAVEFORM_ID` | h2d | 3 | yes | `MESG_RESPONSE_EVENT_ID` | [channel, waveform lo, waveform hi]. Search duty cycle. | bridge |
| `0x4A` | `ANTW_MESG_SYSTEM_RESET_ID` | h2d | 1 | yes | `MESG_STARTUP_MESG_ID` | Resets the ANT protocol stack, NOT the MCU. Answered with the startup message and no RESPONSE_EVENT. Rebooting here drops the device off the bus and every host session begins with this. | bridge, tools |
| `0x4B` | `ANTW_MESG_OPEN_CHANNEL_ID` | h2d | 1 | yes | `MESG_RESPONSE_EVENT_ID` | [channel]. | bridge, tools |
| `0x4C` | `ANTW_MESG_CLOSE_CHANNEL_ID` | h2d | 1 | yes | `MESG_RESPONSE_EVENT_ID` | [channel]. Asynchronous: the channel is not free until EVENT_CHANNEL_CLOSED arrives. | bridge, tools |
| `0x4D` | `ANTW_MESG_REQUEST_ID` | h2d | 2 | yes | - | [channel or index, requested message id]. The reply is the requested message; a refusal is a RESPONSE_EVENT naming 0x4D. For several messages the first byte is not a channel at all - it is a mask number, an encryption info type, or 0/1 for advanced burst capabilities/configuration. | bridge, tools |
| `0x4E` | `ANTW_MESG_BROADCAST_DATA_ID` | both | 9..20 | yes | - | [channel, data[8]] outbound. Inbound it may carry the extended fields after the payload - see ext_flags. | bridge, tools |
| `0x4F` | `ANTW_MESG_ACKNOWLEDGED_DATA_ID` | both | 9..20 | yes | - | [channel, data[8]]. The message a host sends to set trainer resistance; the outcome arrives later as EVENT_TRANSFER_TX_COMPLETED or EVENT_TRANSFER_TX_FAILED. | bridge, tools |
| `0x50` | `ANTW_MESG_BURST_DATA_ID` | both | 9 | yes | - | [seq<<5 \| channel, data[8]], bit 7 of byte 0 marks the last packet. Only bits 0-4 are the channel number, which is why 32 channels is the serial protocol's natural ceiling. No per-packet acknowledgement on success. | bridge, tools |
| `0x51` | `ANTW_MESG_CHANNEL_ID_ID` | both | 5 | yes | - | [channel, device number lo, device number hi, device type, transmission type]. All-zero is the wildcard that matches anything. | bridge, tools |
| `0x52` | `ANTW_MESG_CHANNEL_STATUS_ID` | d2h | 2 | yes | - | [channel, status]. Requested with MESG_REQUEST. | bridge, tools |
| `0x53` | `ANTW_MESG_RADIO_CW_INIT_ID` | h2d |  | no | - | Enter continuous-wave test mode. Not bridged. | rev5.1 sec 9.5, host-api |
| `0x54` | `ANTW_MESG_CAPABILITIES_ID` | d2h | 9 | yes | - | What the ANT *stack* can do - a superset of what the bridge implements. Requested with MESG_REQUEST. See the capabilities section for the byte and bit layout, and for the exact reply this firmware gives. | bridge, observed |
| `0x59` | `ANTW_MESG_ID_LIST_ADD_ID` | h2d | 6 | yes | `MESG_RESPONSE_EVENT_ID` | [channel, device number lo, device number hi, device type, transmission type, list index]. | bridge |
| `0x5A` | `ANTW_MESG_ID_LIST_CONFIG_ID` | h2d | 3 | yes | `MESG_RESPONSE_EVENT_ID` | [channel, list size, include/exclude flag]. | bridge |
| `0x5B` | `ANTW_MESG_OPEN_RX_SCAN_MODE_ID` | h2d | 1..2 | no | - | Background scanning. Not bridged, and scan mode is reported OFF in the capabilities reply - which is why a host that has the call (ANT_OpenRxScanMode) never sends it. | readme |
| `0x5D` | `ANTW_MESG_EXT_BROADCAST_DATA_ID` | d2h | 13 | no | - | Legacy extended broadcast: channel id inline instead of behind a flag byte. Superseded by MESG_ANTLIB_CONFIG's flag mechanism; this dongle never emits it. | rev5.1 sec 9.5, host-api |
| `0x5E` | `ANTW_MESG_EXT_ACKNOWLEDGED_DATA_ID` | both | 13 | no | - | Legacy extended acknowledged data. See MESG_EXT_BROADCAST_DATA_ID. | rev5.1 sec 9.5, host-api |
| `0x5F` | `ANTW_MESG_EXT_BURST_DATA_ID` | both | 13 | no | - | Legacy extended burst data. See MESG_EXT_BROADCAST_DATA_ID. | rev5.1 sec 9.5, host-api |
| `0x60` | `ANTW_MESG_CHANNEL_RADIO_TX_POWER_ID` | h2d | 2 | yes | `MESG_RESPONSE_EVENT_ID` | [channel, level]. Per-channel transmit power. Only takes on an assigned channel, so the bridge holds the requested level and reapplies it after each successful assign. | bridge, readme |
| `0x61` | `ANTW_MESG_GET_SERIAL_NUM_ID` | h2d |  | no | - | Read the device serial number. Not bridged. | rev5.1 sec 9.5 + verify:sdk-ant-shim |
| `0x63` | `ANTW_MESG_SET_LP_SEARCH_TIMEOUT_ID` | h2d | 2 | yes | `MESG_RESPONSE_EVENT_ID` | [channel, timeout]. Low-priority search timeout, in 2.5 s increments. | bridge |
| `0x65` | `ANTW_MESG_SERIAL_NUM_SET_CHANNEL_ID_ID` | h2d | 3 | no | - | Set the channel's device number from the device's own serial number. Implied by the ANT_DLL export ANT_SetSerialNumChannelId, which the archive's export table left unmapped. Not bridged. Both neighbours in the id sequence are confirmed by that same export table (0x63 ANT_SetLowPriorityChannelSearchTimeout, 0x66 ANT_RxExtMesgsEnable), which is the corroboration this value rests on. | rev5.1 sec 9.5 + verify:sdk-ant-shim |
| `0x66` | `ANTW_MESG_RX_EXT_MESGS_ENABLE_ID` | h2d | 2 | yes | `MESG_RESPONSE_EVENT_ID` | [filler, enable]. The older, narrower way of asking for the channel id on every received message; the bridge maps it onto lib config bit MESG_OUT_INC_DEVICE_ID. Hosts still send it instead of 0x6E, and without it every broadcast arrives anonymous. | bridge |
| `0x68` | `ANTW_MESG_ENABLE_LED_FLASH_ID` | h2d | 2 | no | - | Not bridged, and the LED capability bit is reported OFF - which is why a host that has ANT_EnableLED never sends it. | readme |
| `0x6D` | `ANTW_MESG_XTAL_ENABLE_ID` | h2d | 1 | no | - | Force the crystal oscillator on. Implied by the ANT_DLL export ANT_CrystalEnable, which Zwift resolves and the archive's export table left unmapped. Not bridged. The adjacent 0x6E is confirmed by that export table (ANT_LibConfigCustom), which is the corroboration this value rests on. | rev5.1 sec 9.5 + verify:sdk-ant-shim |
| `0x6E` | `ANTW_MESG_ANTLIB_CONFIG_ID` | both | 2 | yes | - | [filler, config bits]. Turns the extended output fields on. Zero means clear everything - there is no separate clear message on the wire. 0xE0 is channel id + RSSI + RX timestamp; 0x80 is device id only. | bridge, tools |
| `0x6F` | `ANTW_MESG_STARTUP_MESG_ID` | d2h | 1 | yes | - | [reason]. Sent once after the stack has reset. A host that does not see this concludes the dongle is dead. | bridge, tools |
| `0x70` | `ANTW_MESG_AUTO_FREQ_CONFIG_ID` | h2d | 4 | yes | `MESG_RESPONSE_EVENT_ID` | [channel, freq0, freq1, freq2]. Frequency-agility hop table. | bridge |
| `0x71` | `ANTW_MESG_PROX_SEARCH_CONFIG_ID` | h2d | 2..3 | yes | `MESG_RESPONSE_EVENT_ID` | [channel, threshold, (custom threshold)]. Only pair with something close by. | bridge |
| `0x72` | `ANTW_MESG_ADV_BURST_DATA_ID` | both | 9..25 | yes | - | Same header byte as MESG_BURST_DATA, but 8, 16 or 24 data bytes. Accepting this while never bridging 0x78 gives a dongle that takes 24-byte packets and can never send one. | bridge, readme |
| `0x74` | `ANTW_MESG_EVENT_BUFFERING_CONFIG_ID` | h2d | 6 | no | - | Not bridged - sdk-ant exposes no API for it - and correspondingly NOT advertised in the capabilities reply. The one advanced-options-3 bit that is honestly zero. | tools, readme |
| `0x75` | `ANTW_MESG_SET_SEARCH_CH_PRIORITY_ID` | both | 2 | yes | - | [channel, priority]. | bridge, rev5.1 sec 9.5 + verify:sdk-ant-shim |
| `0x77` | `ANTW_MESG_HIGH_DUTY_SEARCH_MODE_ID` | h2d | 2 | no | - | Advertised in the capabilities reply and deliberately not bridged: sdk-ant has no single-chip API for it. Clearing the bit instead would make this dongle report something no ANTUSB-m reports. | tools, readme |
| `0x78` | `ANTW_MESG_CONFIG_ADV_BURST_ID` | both | 9..12 | yes | - | [filler, enable, rf payload size, required modes, 0, 0, optional modes, 0, 0, (stall lo, stall hi, retry extension)]. Requesting it with index 0 returns capabilities, with index 1 the current configuration - and the reply drops the enable byte the command carried. | bridge, tools |
| `0x79` | `ANTW_MESG_EVENT_FILTER_CONFIG_ID` | both | 2..3 | yes | - | Command is [filter lo, filter hi] with no channel byte; the reply is [filler, filter lo, filter hi]. That asymmetry is real and is what the round-trip check in ant_features.py exists to catch. | bridge, tools |
| `0x7A` | `ANTW_MESG_SDU_CONFIG_ID` | h2d | 2 | yes | `MESG_RESPONSE_EVENT_ID` | [channel, mask config]. Binds one of the masks to a channel; INVALID_SDU_MASK (0xFF) turns selective updates back off. | bridge, tools |
| `0x7B` | `ANTW_MESG_SDU_SET_MASK_ID` | both | 9 | yes | - | [mask index, mask[8]]. byte 0 is a mask number, not a channel. The reply has the same shape, which is why its size is ANT_MAX_PAYLOAD_SIZE + CHANNEL_NUM_SIZE. | bridge, tools |
| `0x7C` | `ANTW_MESG_USER_CONFIG_PAGE_ID` | h2d | var | no | - | User NVM configuration page. No sdk-ant API; not bridged. Implied by the ANT_DLL export ANT_ConfigUserNVM, which the archive's export table left unmapped. Both neighbours are confirmed by that table (0x7B ANT_SetSelectiveDataUpdateMask, 0x7D-0x7F encryption), which is the corroboration this value rests on. | rev5.1 sec 9.5 + verify:sdk-ant-shim |
| `0x7D` | `ANTW_MESG_ENCRYPT_ENABLE_ID` | both | 4 | yes | - | Write [channel, mode, key number, decimation rate] - compiled out unless CONFIG_ANT_DONGLE_ENCRYPTION. The read side is always bridged: requesting it with an ENCRYPTION_INFO_GET_* index returns [info type, data...], the info type echoed at byte 0. | bridge, tools |
| `0x7E` | `ANTW_MESG_SET_ENCRYPT_KEY_ID` | h2d | 17 | yes | `MESG_RESPONSE_EVENT_ID` | [key number, key[16]]. That the key is 128 bits is stated in prose only; no size constant names it. | bridge, tools |
| `0x7F` | `ANTW_MESG_SET_ENCRYPT_INFO_ID` | h2d | 2..20 | yes | `MESG_RESPONSE_EVENT_ID` | [info type, data...]. How much data depends on the type, and the length must be checked against it here - the stack reads a fixed count per type with nothing to bound it. RNG seed is refused on purpose: no size for it is documented anywhere. | bridge, tools |
| `0x81` | `ANTW_MESG_ACTIVE_SEARCH_SHARING_ID` | both | 2 | yes | - | [channel, cycles]. | bridge, rev5.1 sec 9.5 + verify:sdk-ant-shim |
| `0xAE` | `ANTW_MESG_SERIAL_ERROR_ID` | d2h | 1 | no | - | Serial-layer error: bad checksum, bad length, or a frame that arrived too slowly. This firmware drops malformed frames silently instead - recorded so a host-side parser can name the byte. | rev5.1 sec 7.4 + verify:sdk-ant-shim |
| **unresolved** | `ANTW_MESG_RSSI_SEARCH_THRESHOLD_ID` | h2d | 2 | no | - | Proximity search threshold expressed in RSSI rather than in the proximity bins MESG_PROX_SEARCH_CONFIG (0x71) uses. Implied by the ANT_DLL export ANT_RSSI_SetSearchThreshold. Believed to sit in the 0xC0 block of AP2-era extension messages, but no value in that block is corroborated by anything in this repository - the whole host-callable API in archive/host-api/ant_dll_exports.json tops out at 0x7B - so no number is recorded here. | host-api + verify:sdk-ant-shim |
| **unresolved** | `ANTW_MESG_SLEEP_ID` | h2d | 1 | no | - | Put the device into its low-power sleep state. Implied by the ANT_DLL export ANT_SleepMessage, which Zwift resolves. Same 0xC0 block and the same lack of corroboration as MESG_RSSI_SEARCH_THRESHOLD_ID; no number is recorded. | host-api + verify:sdk-ant-shim |
| **unresolved** | `ANTW_MESG_CHANNEL_CRC_MODE_ID` | both | 2 | yes | - | [filler, mode] on the way in, [channel, mode] on the way back. Dispatched at src/ant_serial_bridge.c and requested in handle_request(). The numeric id is a Nordic extension that appears nowhere in this repository and is not in Rev 5.1. | bridge + verify:sdk-ant-shim |
| **unresolved** | `ANTW_MESG_PENDING_TRANSMIT_CLEAR_ID` | both | 2 | yes | - | [channel] to clear, [channel, pending] on request. Dispatched by the bridge; the numeric id is a Nordic extension not present in this repository or in Rev 5.1. | bridge + verify:sdk-ant-shim |
| **unresolved** | `ANTW_MESG_ECS_ENABLE_ID` | h2d | 2 | yes | - | [filler, enable]. Enhanced channel spacing. Dispatched by the bridge; the numeric id is a Nordic extension not present in this repository or in Rev 5.1. | bridge + verify:sdk-ant-shim |

## Reply sizes

| Constant | Value | Meaning | Provenance |
|---|---|---|---|
| `ANTW_MESG_RESPONSE_EVENT_SIZE` | 3 | [channel, message id, code]. | bridge |
| `ANTW_MESG_STARTUP_MESG_SIZE` | 1 | [reason]. | bridge |
| `ANTW_MESG_CAPABILITIES_SIZE` | 9 | Nine bytes, confirmed by the observed reply 080800b23200fd8d0f. | observed |
| `ANTW_MESG_VERSION_SIZE` | 20 | Null-padded version string. src/ant_stub.c calls it a 20-byte payload; nothing in this repository shows the reply on the wire. | stub + verify:sdk-ant-shim |
| `ANTW_MESG_CHANNEL_STATUS_SIZE` | 2 | [channel, status]. | inferred + verify:sdk-ant-shim |
| `ANTW_MESG_CHANNEL_ID_SIZE` | 5 | [channel, number lo, number hi, device type, transmission type]. | bridge |
| `ANTW_MESG_CHANNEL_MESG_PERIOD_SIZE` | 3 | [channel, period lo, period hi]. | bridge |
| `ANTW_MESG_CHANNEL_RADIO_FREQ_SIZE` | 2 | [channel, frequency]. | bridge |
| `ANTW_MESG_CHANNEL_CRC_MODE_SIZE` | 2 | [channel, mode]. | inferred + verify:sdk-ant-shim |
| `ANTW_MESG_SET_SEARCH_CH_PRIORITY_SIZE` | 2 | [channel, priority]. | inferred + verify:sdk-ant-shim |
| `ANTW_MESG_PENDING_TRANSMIT_GET_SIZE` | 2 | [channel, pending]. | inferred + verify:sdk-ant-shim |
| `ANTW_MESG_ANTLIB_CONFIG_SIZE` | 2 | [filler, config bits]. | bridge |
| `ANTW_MESG_ACTIVE_SEARCH_SHARING_REQ_SIZE` | 2 | [channel, cycles]. | inferred + verify:sdk-ant-shim |
| `ANTW_MESG_EVENT_FILTER_CONFIG_REQ_SIZE` | 3 | [filler, filter lo, filter hi] - one byte longer than the command that set it. | bridge, tools |
| `ANTW_MESG_CONFIG_ADV_BURST_REQ_CONFIG_SIZE` | 10 | [size, required, 0, 0, optional, 0, 0, stall lo, stall hi, retry]. The 11-byte configuration minus the enable byte the command carried. | stub, tools |
| `ANTW_MESG_CONFIG_ADV_BURST_REQ_CAPABILITIES_SIZE` | (unresolved) | The advanced-burst capabilities reply: byte 0 is a maximum packet size code, byte 1 is a supported-modes bitfield. Its declared length is not visible in this repository - ant_features.py reads only the first two bytes and prints the rest as hex, and no captured reply is committed. This one is on the wire, so it cannot be substituted with a local value: it is the LEN byte the bridge puts on the reply. | tools + verify:sdk-ant-shim |
| `ANTW_MESG_CONFIG_ENCRYPT_REQ_CAPABILITIES_SIZE` | 2 | [info type, supported mode]. | bridge |
| `ANTW_MESG_CONFIG_ENCRYPT_REQ_CONFIG_ID_SIZE` | 5 | [info type, crypto id[4]]. | bridge |
| `ANTW_MESG_CONFIG_ENCRYPT_REQ_CONFIG_USER_DATA_SIZE` | 20 | [info type, custom user data[19]]. The longest frame body the parser ever sees. | bridge, tools |

## Channel types

Byte 1 of MESG_ASSIGN_CHANNEL.

| Constant | Value | Meaning | Provenance |
|---|---|---|---|
| `ANTW_CHANNEL_TYPE_SLAVE` | `0x00` | Bidirectional receive. What every host opens to hear a sensor. | tools (ant_verify) |
| `ANTW_CHANNEL_TYPE_MASTER` | `0x10` | Bidirectional transmit. What ant_sim.py asks for to make this dongle impersonate a sensor. | tools (ant_sim) |
| `ANTW_CHANNEL_TYPE_SHARED_SLAVE` | `0x20` | Shared bidirectional receive. | rev5.1 sec 5.2.1 + verify:sdk-ant-shim |
| `ANTW_CHANNEL_TYPE_SHARED_MASTER` | `0x30` | Shared bidirectional transmit. | rev5.1 sec 5.2.1 + verify:sdk-ant-shim |
| `ANTW_CHANNEL_TYPE_SLAVE_RX_ONLY` | `0x40` | Receive only - never transmits, so it cannot acknowledge. | rev5.1 sec 5.2.1 + verify:sdk-ant-shim |
| `ANTW_CHANNEL_TYPE_MASTER_TX_ONLY` | `0x50` | Transmit only - never opens a receive window. | rev5.1 sec 5.2.1 + verify:sdk-ant-shim |

## Extended assignment

Optional byte 3 of MESG_ASSIGN_CHANNEL. The bridge passes it straight through.

| Constant | Value | Meaning | Provenance |
|---|---|---|---|
| `ANTW_EXT_PARAM_ALWAYS_SEARCH` | `0x01` | Background scanning enable. | rev5.1 sec 5.2.1 + verify:sdk-ant-shim |
| `ANTW_EXT_PARAM_FREQUENCY_AGILITY` | `0x04` | Frequency agility enable. ANT+ profiles forbid it. | rev5.1 sec 5.2.1 + verify:sdk-ant-shim |
| `ANTW_EXT_PARAM_AUTO_SHARED_SLAVE` | `0x08` | Automatic shared-slave address management. | rev5.1 sec 5.2.1 + verify:sdk-ant-shim |
| `ANTW_EXT_PARAM_FAST_INITIATION_MODE` | `0x10` | Fast channel initiation. | readme |
| `ANTW_EXT_PARAM_ASYNC_TX_MODE` | `0x20` | Asynchronous transmission channel. | readme |

## Channel status

Low two bits of byte 1 of a MESG_CHANNEL_STATUS reply.

| Constant | Value | Meaning | Provenance |
|---|---|---|---|
| `ANTW_STATUS_UNASSIGNED_CHANNEL` | `0x00` | No channel type or network set. | stub |
| `ANTW_STATUS_ASSIGNED_CHANNEL` | `0x01` | Assigned but not open. Assigning again is refused with CHANNEL_IN_WRONG_STATE, which is why every host opens a session with a system reset. | rev5.1 sec 9.5.7.1 + verify:sdk-ant-shim |
| `ANTW_STATUS_SEARCHING_CHANNEL` | `0x02` | Open and searching. | rev5.1 sec 9.5.7.1 + verify:sdk-ant-shim |
| `ANTW_STATUS_TRACKING_CHANNEL` | `0x03` | Open and tracking a master. | rev5.1 sec 9.5.7.1 + verify:sdk-ant-shim |
| `ANTW_STATUS_CHANNEL_STATE_MASK` | `0x03` | The bits above; the rest of the byte carries network number and channel type. | rev5.1 sec 9.5.7.1 + verify:sdk-ant-shim |

## Radio transmit power levels

Byte 1 of MESG_RADIO_TX_POWER (device-wide) and byte 1 of MESG_CHANNEL_RADIO_TX_POWER (per channel). The dBm figures are the levels a retail ANT stick exposes; what a given radio actually emits is a property of the part.

| Constant | Value | Meaning | Provenance |
|---|---|---|---|
| `ANTW_RADIO_TX_POWER_LVL_0` | `0x00` | -20 dBm. A deliberate request to be quiet - a bike shop, or a race paddock with forty trainers in one room. | bridge |
| `ANTW_RADIO_TX_POWER_LVL_1` | `0x01` | -12 dBm. | bridge |
| `ANTW_RADIO_TX_POWER_LVL_2` | `0x02` | -4 dBm. | bridge |
| `ANTW_RADIO_TX_POWER_LVL_3` | `0x03` | 0 dBm. The ANT default, and what a host sends when it has no opinion - the common case. | bridge |
| `ANTW_RADIO_TX_POWER_LVL_4` | `0x04` | +4 dBm. The ceiling on the nRF51422 inside the stick being impersonated, so a host asking for it is asking for everything the hardware has. | bridge |
| `ANTW_RADIO_TX_POWER_LVL_5` | `0x05` | +8 dBm. Exists only on the nRF52820, nRF52833 and nRF52840. No host will ever request it by name, because no retail stick has ever had it. | bridge |
| `ANTW_RADIO_TX_POWER_LVL_CUSTOM` | `0x80` | Names a raw register value rather than a level, so it is passed through untouched. | bridge |

## Library configuration bits

Byte 1 of MESG_ANTLIB_CONFIG. Each bit appends one field to every received data message; the fields arrive in bit order behind a flag byte. Sending 0x00 clears all of them - there is no separate clear message on the wire.

| Constant | Value | Meaning | Provenance |
|---|---|---|---|
| `ANTW_LIB_CONFIG_RADIO_CONFIG_ALWAYS` | `0x01` | Reconfigure the radio on every event rather than only on change. | rev5.1 sec 9.5.2.9 + verify:sdk-ant-shim |
| `ANTW_LIB_CONFIG_MESG_OUT_INC_TIME_STAMP` | `0x20` | Append the receive timestamp: 16 bits of the radio's own 32768 Hz counter, so it wraps every two seconds. The only clock on the path that is not the host's - it reads 0.009 ms of timing error where the host clock reads 2.6 ms. | tools (ant_verify) |
| `ANTW_LIB_CONFIG_MESG_OUT_INC_RSSI` | `0x40` | Append RSSI. The difference between a packet lost to a collision and one lost to a fade. | tools (ant_verify) |
| `ANTW_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID` | `0x80` | Append the channel id. Without it every broadcast arrives anonymous and no sensor can be named. This is also what MESG_RX_EXT_MESGS_ENABLE maps onto. | bridge, tools |
| `ANTW_LIB_CONFIG_DEVICE_ID_ONLY` | `0x80` | Alias: the narrow setting ant_scan.py and ant_session.py use - identity, nothing else. | tools |
| `ANTW_LIB_CONFIG_ALL_EXT_FIELDS` | `0xE0` | Channel id + RSSI + RX timestamp, all three. What ant_verify.py asks for, and what ant_core must assemble: the timestamp is the figure the timing gate is read against. | tools (ant_verify) |
| `ANTW_LIB_CONFIG_MASK_ALL` | `0xFF` | Passed to the clear path when a host sends 0x6E with a zero config byte. | bridge + verify:sdk-ant-shim |

## Extended message flag bits

Byte 9 of a received data message - immediately after the 8-byte payload - when any lib config bit is set. The fields follow in a fixed order, but each is present only if its flag bit is set, so an offset is only correct relative to which of the earlier ones turned up. Reading RSSI at a fixed offset works right up until a run is made without the channel id, and then quietly reports the device number as a signal strength.

| Constant | Value | Meaning | Provenance |
|---|---|---|---|
| `ANTW_EXT_FLAG_CHANNEL_ID` | `0x80` | 4 bytes follow: device number lo, device number hi, device type (bit 7 is the pairing bit), transmission type. | tools (ant_verify, ant_scan) |
| `ANTW_EXT_FLAG_RSSI` | `0x40` | 3 bytes follow: measurement type, value, threshold configuration. | tools (ant_verify) |
| `ANTW_EXT_FLAG_RX_TIMESTAMP` | `0x20` | 2 bytes follow: the 32768 Hz receive timestamp, little endian. | tools (ant_verify) |

## RSSI measurement types

Byte 0 of the RSSI extended field.

| Constant | Value | Meaning | Provenance |
|---|---|---|---|
| `ANTW_RSSI_MEASUREMENT_TYPE_DBM` | `0x20` | The one measurement type that carries dBm. Anything else is a proprietary scale, and a number on an unknown scale is worse than no number. | tools (ant_verify) |

## Startup message reasons

The single payload byte of MESG_STARTUP_MESG. Zero is not a bit: it means a power-on or a command reset with no other flag set.

| Constant | Value | Meaning | Provenance |
|---|---|---|---|
| `ANTW_STARTUP_POWER_ON_RESET` | `0x00` | Power-on, or a reset by command. | tools (ant_probe) |
| `ANTW_STARTUP_HARDWARE_RESET_LINE` | `0x01` | Hardware reset line. | tools (ant_probe) |
| `ANTW_STARTUP_WATCH_DOG_RESET` | `0x02` | Watchdog. | tools (ant_probe) |
| `ANTW_STARTUP_COMMAND_RESET` | `0x20` | Reset by MESG_SYSTEM_RESET. What this firmware reports - it sends 0x00, because the stack reset is indistinguishable from a cold start from the host's side. | tools (ant_probe) |
| `ANTW_STARTUP_SYNCHRONOUS_RESET` | `0x40` | Synchronous serial reset. | tools (ant_probe) |
| `ANTW_STARTUP_SUSPEND_RESET` | `0x80` | Resume from suspend. | tools (ant_probe) |

## Channel event codes

Byte 2 of a RESPONSE_EVENT whose byte 1 is MESG_EVENT_ID (0x01). These arrive unsolicited; the response codes below arrive in answer to a command. They share one number space.

| Constant | Value | Meaning | Provenance |
|---|---|---|---|
| `ANTW_EVENT_RX_SEARCH_TIMEOUT` | `0x01` | The search window expired without finding a master. | tools (ant_session) |
| `ANTW_EVENT_RX_FAIL` | `0x02` | A receive slot came and went with nothing valid in it. This is the event that makes loss accounting possible: the radio reports one for every packet lost on the air and nothing at all for one lost in the host, so a run that loses more than it can account for is losing it locally. | tools (ant_session, ant_verify) |
| `ANTW_EVENT_TX` | `0x03` | A payload has gone on the air; load the next one. ant_sim.py paces itself on this rather than on a host timer. | tools (ant_session, ant_sim) |
| `ANTW_EVENT_TRANSFER_RX_FAILED` | `0x04` | An inbound burst did not complete. | tools (ant_session) |
| `ANTW_EVENT_TRANSFER_TX_COMPLETED` | `0x05` | Acknowledged data or a burst was acknowledged by the far end. Also releases the bridge's single burst block. | bridge, tools |
| `ANTW_EVENT_TRANSFER_TX_FAILED` | `0x06` | It went out and nothing acknowledged it. Also releases the burst block. | bridge, tools |
| `ANTW_EVENT_CHANNEL_CLOSED` | `0x07` | The channel is now free to unassign. A close is asynchronous, so unassigning before this arrives is refused. | bridge, tools |
| `ANTW_EVENT_RX_FAIL_GO_TO_SEARCH` | `0x08` | Enough consecutive misses that the channel has dropped back into search. | tools (ant_session) |
| `ANTW_EVENT_CHANNEL_COLLISION` | `0x09` | Two channels wanted the radio at the same instant. | tools (ant_session) |
| `ANTW_EVENT_TRANSFER_TX_START` | `0x0A` | Progress, not an outcome - keep waiting. | tools (ant_session) |
| `ANTW_EVENT_TRANSFER_NEXT_DATA_BLOCK` | `0x11` | The stack has finished with the burst block it was handed and the next may overwrite it. The bridge consumes this internally and never puts it on the wire: a real stick frames bursts itself. If ant_core fails to raise it exactly once per accepted block, host bursts stall 1000 ms per packet. | bridge, rev5.1 sec 9.5.6 + verify:sdk-ant-shim |

## Response codes

Byte 2 of a RESPONSE_EVENT answering a command. Same number space as the event codes.

| Constant | Value | Meaning | Provenance |
|---|---|---|---|
| `ANTW_RESPONSE_NO_ERROR` | `0x00` | Accepted. A host that gets anything else for a message it needs gives up, and says nothing useful about which one. | bridge, tools |
| `ANTW_CHANNEL_IN_WRONG_STATE` | `0x15` | The commonest real-world refusal: assigning a channel a previous run left assigned, or unassigning one that has not finished closing. | rev5.1 sec 9.5.6 + verify:sdk-ant-shim |
| `ANTW_CHANNEL_NOT_OPENED` | `0x16` | Data was sent on a channel that is not open. | rev5.1 sec 9.5.6 + verify:sdk-ant-shim |
| `ANTW_CHANNEL_ID_NOT_SET` | `0x18` | Opened before MESG_CHANNEL_ID. | rev5.1 sec 9.5.6 + verify:sdk-ant-shim |
| `ANTW_CLOSE_ALL_CHANNELS` | `0x19` | Scan mode requires every other channel closed. | rev5.1 sec 9.5.6 + verify:sdk-ant-shim |
| `ANTW_TRANSFER_IN_PROGRESS` | `0x1F` | A burst is already running on this channel. The bridge answers with this when the host outruns the single burst block. | bridge, rev5.1 sec 9.5.6 + verify:sdk-ant-shim |
| `ANTW_TRANSFER_SEQUENCE_NUMBER_ERROR` | `0x20` | Burst packets arrived out of order. | rev5.1 sec 9.5.6 + verify:sdk-ant-shim |
| `ANTW_TRANSFER_IN_ERROR` | `0x21` | Burst aborted. | rev5.1 sec 9.5.6 + verify:sdk-ant-shim |
| `ANTW_TRANSFER_BUSY` | `0x22` | The channel is busy with another transfer. Distinct from TRANSFER_IN_PROGRESS (0x1F), which is what this bridge answers when the host outruns its single burst block. | rev5.1 sec 9.5.6 + verify:sdk-ant-shim |
| `ANTW_MESSAGE_SIZE_EXCEEDS_LIMIT` | `0x27` | Payload longer than the message allows. | rev5.1 sec 9.5.6 + verify:sdk-ant-shim |
| `ANTW_INVALID_MESSAGE` | `0x28` | The dispatcher does not implement this message, or its body was too short to run. Distinguishing 'the bridge refused it' from 'the stack refused it' is exactly this code: anything else means the message was decoded and handed on. | bridge, tools |
| `ANTW_INVALID_NETWORK_NUMBER` | `0x29` | Network number above the maximum the capabilities reply advertises. | rev5.1 sec 9.5.6 + verify:sdk-ant-shim |
| `ANTW_INVALID_LIST_ID` | `0x30` | Inclusion/exclusion list index out of range. | rev5.1 sec 9.5.6 + verify:sdk-ant-shim |
| `ANTW_INVALID_SCAN_TX_CHANNEL` | `0x31` | Transmit attempted on a channel other than 0 in scan mode. | rev5.1 sec 9.5.6 + verify:sdk-ant-shim |
| `ANTW_INVALID_PARAMETER_PROVIDED` | `0x33` | The stack decoded the message and disliked a value. Measured example: MESG_SET_ENCRYPT_KEY answers 51 when CONFIG_ANT_ENCRYPTED_CHANNELS is 0, because the valid key index range is then empty. | readme, tools |
| `ANTW_EVENT_SERIAL_QUE_OVERFLOW` | `0x34` | The host is sending faster than the serial layer drains. | rev5.1 sec 9.5.6 + verify:sdk-ant-shim |
| `ANTW_EVENT_QUE_OVERFLOW` | `0x35` | The event queue overflowed - events were dropped. | rev5.1 sec 9.5.6 + verify:sdk-ant-shim |

## Advanced burst configuration

Fields of MESG_CONFIG_ADV_BURST.

| Constant | Value | Meaning | Provenance |
|---|---|---|---|
| `ANTW_ADV_BURST_MODE_DISABLE` | `0x00` | Enable byte: off. The shipping default. | tools (ant_features) |
| `ANTW_ADV_BURST_MODE_ENABLE` | `0x01` | Enable byte: on. | tools (ant_features) |
| `ANTW_ADV_BURST_MODES_SIZE_8_BYTES` | `0x01` | RF payload size code: 8 bytes. | inferred + verify:sdk-ant-shim |
| `ANTW_ADV_BURST_MODES_SIZE_16_BYTES` | `0x02` | RF payload size code: 16 bytes. | inferred + verify:sdk-ant-shim |
| `ANTW_ADV_BURST_MODES_SIZE_24_BYTES` | `0x03` | RF payload size code: 24 bytes. What the stub advertises and what ant_features.py configures. | stub, tools |
| `ANTW_ADV_BURST_MODES_FREQ_HOP` | `0x01` | Optional-modes bit: frequency hopping. | stub, tools |

## Selective data update

Values of the mask-config byte of MESG_SDU_CONFIG.

| Constant | Value | Meaning | Provenance |
|---|---|---|---|
| `ANTW_INVALID_SDU_MASK` | `0xFF` | Detaches a channel from every mask - the way to turn selective updates back off. | stub, tools |
| `ANTW_SDU_MASK_ACK_CONFIG_BIT` | `(unresolved)` | Set alongside a mask number to apply the mask to acknowledged data as well as broadcast. src/ant_stub.c:529 masks it off before range-checking the mask number; the bit's value appears nowhere in this repository. Never on the wire in a direction this dongle originates - the host sets it - so the stub does not need the real value to behave correctly. | stub + verify:sdk-ant-shim |

## Encryption

Info types and modes for MESG_ENCRYPT_ENABLE / MESG_SET_ENCRYPT_INFO. The set and get info types do not line up: set 0 is the crypto id and get 0 is the supported mode, so a round trip writes with one number and reads with the next. That is not a bug in the tools.

| Constant | Value | Meaning | Provenance |
|---|---|---|---|
| `ANTW_ENCRYPTION_DISABLED_MODE` | `0x00` | Channel encryption off. | tools (ant_features) |
| `ANTW_ENCRYPTION_INFO_SET_CRYPTO_ID` | `0x00` | Write: 4-byte crypto id. | tools (ant_features) |
| `ANTW_ENCRYPTION_INFO_SET_CUSTOM_USER_DATA` | `0x01` | Write: 19 bytes of custom user data. | tools (ant_features) |
| `ANTW_ENCRYPTION_INFO_SET_RNG_SEED` | `0x02` | Write: RNG seed. Deliberately refused by this bridge - sdk-ant calls it platform specific and defines no size for it anywhere, and the stack does not take its randomness from the host in any case. Refusing beats guessing a length. | bridge, tools |
| `ANTW_ENCRYPTION_INFO_GET_SUPPORTED_MODE` | `0x00` | Read: the supported encryption mode. | tools (ant_features) |
| `ANTW_ENCRYPTION_INFO_GET_CRYPTO_ID` | `0x01` | Read: the 4-byte crypto id. | tools (ant_features) |
| `ANTW_ENCRYPTION_INFO_GET_CUSTOM_USER_DATA` | `0x02` | Read: the 19 bytes of custom user data. | tools (ant_features) |
| `ANTW_ENCRYPTION_USER_DATA_SIZE` | `19` | Bytes of custom user data. One info-type byte plus these fills MAX_SIZE_VALUE exactly. | stub, tools |
| `ANTW_ENCRYPTION_KEY_SIZE` | `16` | 128-bit key. No sdk-ant constant names this; it is stated only in the prose of ant_crypto_key_set(), which takes a bare pointer. | bridge, stub |
| `ANTW_MAX_SUPPORTED_ENCRYPTION_MODE` | `(unresolved)` | The value src/ant_stub.c answers an ENCRYPTION_INFO_GET_SUPPORTED_MODE request with, and the upper bound it range-checks a channel-enable against. Not visible in this repository. It does reach the wire, but only out of the stub, whose replies are already declared synthetic. | stub + verify:sdk-ant-shim |

## Burst header byte

Byte 0 of MESG_BURST_DATA / MESG_ADV_BURST_DATA. Not a channel number on its own.

| Constant | Value | Meaning | Provenance |
|---|---|---|---|
| `ANTW_BURST_HEADER_CHANNEL_MASK` | `0x1F` | Channel number. Five bits - which is why 32 channels is the serial protocol's natural ceiling, and why ant_core is sized for 32 from the first line. | bridge |
| `ANTW_BURST_HEADER_SEQ_SHIFT` | `5` | Sequence number occupies bits 5-6. | bridge |
| `ANTW_BURST_HEADER_SEQ_MASK` | `0x03` | Sequence number after shifting: 0-3, wrapping. | bridge |
| `ANTW_BURST_HEADER_LAST` | `0x80` | Bit 7: this is the last packet of the transfer. | bridge, tools |

## Capabilities reply, decoded

`MESG_CAPABILITIES` (`0x54`) carries 9 bytes. This firmware on nRF52840 over USB, and byte-identical on nRF54L15 over UART.

```
A4 09 54 08 08 00 B2 32 00 FD 8D 0F 06
^  ^  ^                             ^
|  |  |                             `- XOR checksum, SYNC included
|  |  `- MESG_CAPABILITIES_ID
|  `- LEN = 9
`- SYNC
```

| Byte | Value | Field | Meaning |
|---|---|---|---|
| 0 | `0x08` | `max_channels` | Simultaneous ANT channels the stack has allocated. |
| 1 | `0x08` | `max_networks` | Network keys the stack can hold. |
| 2 | `0x00` | `standard_options` | Negative capabilities: a set bit means the feature is ABSENT. |
| 3 | `0xB2` | `advanced_options` | Positive capabilities from here on: a set bit means present. |
| 4 | `0x32` | `advanced_options_2` | - |
| 5 | `0x00` | `max_sensrcore_channels` | SensRcore scripting channels. Zero everywhere here. |
| 6 | `0xFD` | `advanced_options_3` | The tech-bulletin features. This is the byte ant_features.py walks. |
| 7 | `0x8D` | `advanced_options_4` | Observed as 0x8D on this firmware. The bit names are NOT recoverable from this repository, and guessing them would put fiction in a file whose whole purpose is to be authoritative. The byte is recorded; the bits are not named. |
| 8 | `0x0F` | `advanced_options_5` | Observed as 0x0F. Bit names not recoverable here - see byte 7. |

Bit by bit, for every byte that is a bitfield:

**Byte 0, `max_channels` = `0x08` (`0b00001000`)**

Not a bitfield: the value is 8.

**Byte 1, `max_networks` = `0x08` (`0b00001000`)**

Not a bitfield: the value is 8.

**Byte 2, `standard_options` = `0x00` (`0b00000000`)**

| Bit | Mask | Name | Set? | Meaning |
|---|---|---|---|---|
| 0 | `0x01` | `ANTW_CAPABILITIES_NO_RECEIVE_CHANNELS` | no | Cannot receive. |
| 1 | `0x02` | `ANTW_CAPABILITIES_NO_TRANSMIT_CHANNELS` | no | Cannot transmit. |
| 2 | `0x04` | `ANTW_CAPABILITIES_NO_RECEIVE_MESSAGES` | no | No receive messages. |
| 3 | `0x08` | `ANTW_CAPABILITIES_NO_TRANSMIT_MESSAGES` | no | No transmit messages. |
| 4 | `0x10` | `ANTW_CAPABILITIES_NO_ACKD_MESSAGES` | no | No acknowledged data. |
| 5 | `0x20` | `ANTW_CAPABILITIES_NO_BURST_MESSAGES` | no | No burst. |

**Byte 3, `advanced_options` = `0xB2` (`0b10110010`)**

| Bit | Mask | Name | Set? | Meaning |
|---|---|---|---|---|
| 1 | `0x02` | `ANTW_CAPABILITIES_NETWORK_ENABLED` | **yes** | MESG_NETWORK_KEY is supported. |
| 3 | `0x08` | `ANTW_CAPABILITIES_SERIAL_NUMBER_ENABLED` | no | The device has a readable serial number. |
| 4 | `0x10` | `ANTW_CAPABILITIES_PER_CHANNEL_TX_POWER_ENABLED` | **yes** | MESG_CHANNEL_RADIO_TX_POWER (0x60) is supported. |
| 5 | `0x20` | `ANTW_CAPABILITIES_LOW_PRIORITY_SEARCH_ENABLED` | **yes** | MESG_SET_LP_SEARCH_TIMEOUT is supported. |
| 6 | `0x40` | `ANTW_CAPABILITIES_SCRIPT_ENABLED` | no | SensRcore scripting. |
| 7 | `0x80` | `ANTW_CAPABILITIES_SEARCH_LIST_ENABLED` | **yes** | Inclusion/exclusion lists (0x59, 0x5A). |

**Byte 4, `advanced_options_2` = `0x32` (`0b00110010`)**

| Bit | Mask | Name | Set? | Meaning |
|---|---|---|---|---|
| 0 | `0x01` | `ANTW_CAPABILITIES_LED_ENABLED` | no | MESG_ENABLE_LED_FLASH. Reported OFF here, which is why a host that has ANT_EnableLED never sends 0x68. |
| 1 | `0x02` | `ANTW_CAPABILITIES_EXT_MESSAGE_ENABLED` | **yes** | Extended output fields - the whole lib config mechanism. |
| 2 | `0x04` | `ANTW_CAPABILITIES_SCAN_MODE_ENABLED` | no | MESG_OPEN_RX_SCAN_MODE. Reported OFF here, which is why a host that has ANT_OpenRxScanMode never sends 0x5B. ant_core turns this on. |
| 4 | `0x10` | `ANTW_CAPABILITIES_PROX_SEARCH_ENABLED` | **yes** | MESG_PROX_SEARCH_CONFIG. |
| 5 | `0x20` | `ANTW_CAPABILITIES_EXT_ASSIGN_ENABLED` | **yes** | The optional fourth byte of MESG_ASSIGN_CHANNEL. |
| 6 | `0x40` | `ANTW_CAPABILITIES_FS_ANTFS_ENABLED` | no | ANT-FS file system. |
| 7 | `0x80` | `ANTW_CAPABILITIES_FIT1_ENABLED` | no | FIT1 support. |

**Byte 5, `max_sensrcore_channels` = `0x00` (`0b00000000`)**

Not a bitfield: the value is 0.

**Byte 6, `advanced_options_3` = `0xFD` (`0b11111101`)**

| Bit | Mask | Name | Set? | Meaning |
|---|---|---|---|---|
| 0 | `0x01` | `ANTW_CAPABILITIES_ADVANCED_BURST_ENABLED` | **yes** | MESG_CONFIG_ADV_BURST (0x78) and 24-byte burst packets. |
| 1 | `0x02` | `ANTW_CAPABILITIES_EVENT_BUFFERING_ENABLED` | no | MESG_EVENT_BUFFERING_CONFIG (0x74). The one bit this firmware honestly reports as zero, because sdk-ant has no API for it. |
| 2 | `0x04` | `ANTW_CAPABILITIES_EVENT_FILTERING_ENABLED` | **yes** | MESG_EVENT_FILTER_CONFIG (0x79). |
| 3 | `0x08` | `ANTW_CAPABILITIES_HIGH_DUTY_SEARCH_ENABLED` | **yes** | MESG_HIGH_DUTY_SEARCH_MODE (0x77). Advertised and not bridged, on purpose - see the README. |
| 4 | `0x10` | `ANTW_CAPABILITIES_SEARCH_SHARING_ENABLED` | **yes** | MESG_ACTIVE_SEARCH_SHARING (0x81). |
| 5 | `0x20` | `ANTW_CAPABILITIES_RADIO_COEX_CONFIG_ENABLED` | **yes** | Radio coexistence configuration. No host API reaches it. |
| 6 | `0x40` | `ANTW_CAPABILITIES_SELECTIVE_DATA_UPDATE_ENABLED` | **yes** | MESG_SDU_CONFIG / MESG_SDU_SET_MASK (0x7A, 0x7B). |
| 7 | `0x80` | `ANTW_CAPABILITIES_ENCRYPTED_CHANNEL_ENABLED` | **yes** | Single-channel encryption (0x7D-0x7F). Advertised whether or not the write side was compiled in: the bit describes what the radio layer can do, not what the bridge chose to expose. |

**Byte 7, `advanced_options_4` = `0x8D` (`0b10001101`)**

Observed as 0x8D on this firmware. The bit names are NOT recoverable from this repository, and guessing them would put fiction in a file whose whole purpose is to be authoritative. The byte is recorded; the bits are not named.

**Byte 8, `advanced_options_5` = `0x0F` (`0b00001111`)**

Observed as 0x0F. Bit names not recoverable here - see byte 7.

## RadiANT extension messages

**These are ours, not Garmin's.** They are not in Rev 5.1, no ANT device answers them and no ANT host sends them. They are recorded before anything implements them so the id space is reserved rather than argued about later, and they are kept in their own table so that "an ANT message" and "a RadiANT message" cannot be confused. `tools/ant_wire.py` exposes them as `RADIANT_MESSAGES`, deliberately not in `MESSAGES`. Semantics belong to `docs/radiant-security.md`; only the numbering is decided here.

**`0xE0` here is a message ID. Lib config `0xE0` (`ANTW_LIB_CONFIG_ALL_EXT_FIELDS` — channel id + RSSI + RX timestamp) is a completely different namespace that happens to share the byte.** One is the ID field of a frame; the other is a payload byte of message `0x6E`. Nothing relates them.

| ID | Name | dir | len | Meaning |
|---|---|---|---|---|
| `0xE0` | `ANTW_MESG_RADIANT_SEC_CONFIG_ID` | h2d | var | Select which of X_PRIV, X_CONF and X_AUTH are on for a channel. The three switches are independent, not a ladder. |
| `0xE1` | `ANTW_MESG_RADIANT_SET_KEY_ID` | h2d | var | Install key material for a channel. |
| `0xE2` | `ANTW_MESG_RADIANT_EPOCH_ID` | both | var | Read or set the current 128 s epoch counter that X_PRIV's device number rotation is derived from. |
| `0xE3` | `ANTW_MESG_RADIANT_SEC_STATUS_ID` | d2h | var | Per-channel security state: which switches are active, replay counter high-water mark, spread-MAC verification result. |
| `0xE4` | `ANTW_MESG_RADIANT_PAIRING_ID` | both | var | Drive the in-the-clear pairing exchange. An X_PRIV node pairs with rotation off and the pairing bit set, then switches to rotating - pairing in the clear is structural, not an oversight. |
| `0xE5-0xEF` | *(reserved)* | - | - | Held for the rest of the RadiANT family. Claim by PR against docs/profile-registry.md, same process as a device type. |

### Non-collision, checked

Every witness available in this repository was checked before these ids were reserved:

- `src/ant_serial_bridge.c` `dispatch()` and `handle_request()` — highest id `0x81`.
- Every `MESG_*` definition in `tools/` — highest `0x7F`.
- `archive/host-api/ant_dll_exports.json`, taken from the real PE export directory of `ANT_DLL.dll` — 36 distinct message ids, highest `0x7B`. The entire host-callable ANT API tops out there.
- The capabilities reply — carries no id space at all, so nothing in it can collide.
- Rev 5.1's message table — highest ids known are the encryption block `0x7D`–`0x7F`, active search sharing `0x81`, NVM crypto key ops around `0x83`, the serial-layer `0xAE`–`0xAF` and port-IO `0xB4`–`0xB5` messages, and an AP2-era extension block around `0xC0`–`0xC7` (RSSI search threshold, sleep, Garmin ESN, USB info).

**Nothing is known to occupy `0xE0`–`0xEF`, so the reservation holds.** The honest limit is on the last bullet: it is recall of the public message table rather than a citation, which is exactly why the `0xC0` block is stated as a *range* and why `MESG_RSSI_SEARCH_THRESHOLD_ID` and `MESG_SLEEP_ID` carry no value in this file. `0xE0` sits `0x19` clear of the top of that block, so the reservation survives a misremembered member of it. `0xE0` is also not confusable with SYNC (`0xA4`/`0xA5`) by a resynchronising parser — which an id of `0xA4` genuinely would be.

## `ANT_DLL` exports with no message id

Host-library entry points that must correspond to *some* serial message, where neither this repository nor Rev 5.1 names the id. Nothing is invented for them. Listed so the gap is visible, and so the `null` `mesg_id` fields in `archive/host-api/ant_dll_exports.json` read as researched rather than unfinished. `_RTO` variants are the same call with a response timeout and are not listed separately.

| Export | Note |
|---|---|
| `ANT_NVM_Clear` | User NVM. sdk-ant exposes no API for any of these six, so none can be bridged whatever their ids turn out to be. |
| `ANT_NVM_Dump` | User NVM. |
| `ANT_NVM_EndSector` | User NVM. |
| `ANT_NVM_Lock` | User NVM. |
| `ANT_NVM_SetDefaultSector` | User NVM. |
| `ANT_NVM_Write` | User NVM. |

## Unresolved constants

These are used at a visible site in this repository but their numeric value is not recoverable from it, and Rev 5.1 does not name them either. No macro is emitted for any of them. `src/ant_radio_sdk_ant.c` is the one translation unit permitted to include both `ant_wire.h` and sdk-ant's `ant_parameters.h`; it recovers each value and `BUILD_ASSERT`s it, and the value comes back here afterwards. Inventing a number would be worse than leaving the hole, because a wrong dispatch id fails silently and a missing macro fails at compile time.

**The rule for consuming these: an unresolved constant may never appear in a file that builds without sdk-ant.** `ant_radio_sdk_ant.c` may `#define` it from sdk-ant, because sdk-ant is present by construction there. `ant_radio_stub.c` and `ant_core/**` may not, because the whole point of those builds is that sdk-ant is absent. Where the value never reaches the wire in a direction we originate, a file-local substitute is correct and the `Blocks` column says so; where it *is* a byte we transmit, there is no substitute and the message stays unimplemented until the shim resolves it.

| Constant | Section | Blocks | Why it is unresolved | Provenance |
|---|---|---|---|---|
| `MESG_RSSI_SEARCH_THRESHOLD_ID` | messages | nothing - not bridged | Proximity search threshold expressed in RSSI rather than in the proximity bins MESG_PROX_SEARCH_CONFIG (0x71) uses. Implied by the ANT_DLL export ANT_RSSI_SetSearchThreshold. Believed to sit in the 0xC0 block of AP2-era extension messages, but no value in that block is corroborated by anything in this repository - the whole host-callable API in archive/host-api/ant_dll_exports.json tops out at 0x7B - so no number is recorded here. | host-api + verify:sdk-ant-shim |
| `MESG_SLEEP_ID` | messages | nothing - not bridged | Put the device into its low-power sleep state. Implied by the ANT_DLL export ANT_SleepMessage, which Zwift resolves. Same 0xC0 block and the same lack of corroboration as MESG_RSSI_SEARCH_THRESHOLD_ID; no number is recorded. | host-api + verify:sdk-ant-shim |
| `MESG_CHANNEL_CRC_MODE_ID` | messages | src/ant_serial_bridge.c dispatch() and handle_request() | [filler, mode] on the way in, [channel, mode] on the way back. Dispatched at src/ant_serial_bridge.c and requested in handle_request(). The numeric id is a Nordic extension that appears nowhere in this repository and is not in Rev 5.1. | bridge + verify:sdk-ant-shim |
| `MESG_PENDING_TRANSMIT_CLEAR_ID` | messages | src/ant_serial_bridge.c dispatch() and handle_request() | [channel] to clear, [channel, pending] on request. Dispatched by the bridge; the numeric id is a Nordic extension not present in this repository or in Rev 5.1. | bridge + verify:sdk-ant-shim |
| `MESG_ECS_ENABLE_ID` | messages | src/ant_serial_bridge.c dispatch() | [filler, enable]. Enhanced channel spacing. Dispatched by the bridge; the numeric id is a Nordic extension not present in this repository or in Rev 5.1. | bridge + verify:sdk-ant-shim |
| `MESG_CONFIG_ADV_BURST_REQ_CAPABILITIES_SIZE` | message_sizes | src/ant_serial_bridge.c handle_request(); src/ant_stub.c:209 | The advanced-burst capabilities reply: byte 0 is a maximum packet size code, byte 1 is a supported-modes bitfield. Its declared length is not visible in this repository - ant_features.py reads only the first two bytes and prints the rest as hex, and no captured reply is committed. This one is on the wire, so it cannot be substituted with a local value: it is the LEN byte the bridge puts on the reply. | tools + verify:sdk-ant-shim |
| `SDU_MASK_ACK_CONFIG_BIT` | sdu | src/ant_stub.c:529 (substitutable - see the unresolved rule) | Set alongside a mask number to apply the mask to acknowledged data as well as broadcast. src/ant_stub.c:529 masks it off before range-checking the mask number; the bit's value appears nowhere in this repository. Never on the wire in a direction this dongle originates - the host sets it - so the stub does not need the real value to behave correctly. | stub + verify:sdk-ant-shim |
| `MAX_SUPPORTED_ENCRYPTION_MODE` | encryption | src/ant_stub.c:233, :483 (substitutable in a stub) | The value src/ant_stub.c answers an ENCRYPTION_INFO_GET_SUPPORTED_MODE request with, and the upper bound it range-checks a channel-enable against. Not visible in this repository. It does reach the wire, but only out of the stub, whose replies are already declared synthetic. | stub + verify:sdk-ant-shim |

## Provenance summary

211 constants in total. 64 carry a `verify: sdk-ant-shim` flag, meaning no file in this repository witnesses the value and the Wave 2 shim's `BUILD_ASSERT` block is what confirms it. 8 of those are unresolved outright.

| Source | Constants |
|---|---|
| K4 (docs/radiant-security.md sec 9) | 5 |
| bridge | 35 |
| bridge, observed | 1 |
| bridge, readme | 3 |
| bridge, rev5.1 sec 9.5 | 2 |
| bridge, rev5.1 sec 9.5.6 | 2 |
| bridge, stub | 1 |
| bridge, tools | 33 |
| bridge, tools (ant_session.EVENT_MARKER) | 1 |
| host-api | 2 |
| inferred | 7 |
| observed | 1 |
| readme | 5 |
| readme, tools | 1 |
| rev5.1 sec 5.2.1 | 7 |
| rev5.1 sec 7.1 | 1 |
| rev5.1 sec 7.4 | 1 |
| rev5.1 sec 9.3 | 1 |
| rev5.1 sec 9.5 | 4 |
| rev5.1 sec 9.5, host-api | 5 |
| rev5.1 sec 9.5.2.9 | 1 |
| rev5.1 sec 9.5.6 | 13 |
| rev5.1 sec 9.5.7.1 | 4 |
| rev5.1 sec 9.5.7.4 | 11 |
| src/usb_ant_class.c:30 | 1 |
| src/usb_ant_class.c:30, bridge | 1 |
| src/usb_ant_class.c:30, src/usb_ant_class_next.c:55 | 1 |
| stub | 11 |
| stub, readme | 1 |
| stub, tools | 9 |
| tools | 4 |
| tools (ant_features) | 10 |
| tools (ant_probe) | 6 |
| tools (ant_session) | 5 |
| tools (ant_session, ant_sim) | 1 |
| tools (ant_session, ant_verify) | 1 |
| tools (ant_sim) | 1 |
| tools (ant_verify) | 7 |
| tools (ant_verify, ant_scan) | 1 |
| tools, readme | 4 |

<!-- END GENERATED: ant_wire -->
