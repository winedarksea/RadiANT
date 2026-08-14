# SPDX-License-Identifier: Apache-2.0
"""ANT serial protocol constants and lookup tables.

DO NOT EDIT - generated from protocol/ant_wire.yaml
by scripts/gen_ant_wire.py. Edit the YAML and re-run the generator;
`scripts/gen_ant_wire.py --check` fails the build if these drift apart.

Source of truth: protocol/ant_wire.yaml
Naming authority: ANT Message Protocol and Usage Rev 5.1 (D00000652)
Human-readable companion: docs/ant-serial-protocol.md

Standard library only, and no imports at all - this module is meant to be
copied into another project on its own. The names are the bare sdk-ant
spellings rather than the ANTW_ ones the C header uses: the prefix exists to
dodge a C preprocessor collision, and Python already has module namespaces.

Nothing here describes the on-air link layer - this is bytes on a USB bulk
endpoint or a UART, and nothing else.
"""

# ---------------------------------------------------------------------------
# Framing
# The checksum is the XOR of every byte of the frame from the SYNC byte through
# the last payload byte, inclusive. Leaving SYNC out of the sum yields a value
# that differs by exactly 0xA4, so two implementations that both omit it agree
# with each other perfectly and with nobody else.
# ---------------------------------------------------------------------------

# SYNC byte on every frame the host sends and on every frame this dongle sends.
# The only value the parser resynchronises on. [bridge]
SYNC_TX = 0xA4

# Alternate SYNC used by the bidirectional/asynchronous serial variants of the
# protocol. Never emitted or accepted by this dongle; recorded so a parser can
# recognise and reject it. [rev5.1 sec 7.1 + verify:sdk-ant-shim]
SYNC_RX = 0xA5

# Bytes of frame around the payload: SYNC + LEN + ID + checksum. Total frame
# length is LEN + 4. [bridge]
MSG_OVERHEAD = 4

# Largest value the LEN byte may carry (src/usb_ant_class.c:30), not the 20
# implied by the encryption path - that is the longest message the *host*
# sends. An advanced-burst data message alone reaches 25. Sizing msg_body[]
# from 20 makes handle_burst() see size = 19, fail its size % 8 check, and
# silently reject every 24-byte burst packet. [src/usb_ant_class.c:30]
MAX_SIZE_VALUE = 38

# MAX_SIZE_VALUE minus the leading channel/index byte that almost every message
# carries. [src/usb_ant_class.c:30, bridge]
MAX_DATA_SIZE = 37

# MAX_DATA_SIZE + MSG_OVERHEAD - one byte SHORT of the largest legal frame,
# because LEN can reach MAX_SIZE_VALUE, which counts the channel byte that
# MAX_DATA_SIZE does not. Sizing a buffer from this writes the checksum one
# past the end. Size buffers from MAX_FRAME_SIZE, never from this. [bridge]
MESG_MAX_SIZE = 41

# MAX_SIZE_VALUE + MSG_OVERHEAD. The correct buffer size, and the 42 both USB
# class files independently quote. [src/usb_ant_class.c:30,
# src/usb_ant_class_next.c:55]
MAX_FRAME_SIZE = 42

# An ANT data payload: 8 bytes, on air and on the wire. Also the width of a
# selective-data-update mask. [stub]
ANT_MAX_PAYLOAD_SIZE = 8

# The leading channel-number byte. Several reply sizes are written as <payload>
# + CHANNEL_NUM_SIZE precisely because the reply leads with an index that is
# not part of the data. [bridge]
CHANNEL_NUM_SIZE = 1

# Largest advanced-burst block in one serial frame. Plain burst carries 8;
# advanced burst carries 8, 16 or 24. [bridge]
ADV_BURST_BLOCK_MAX = 24

# ---------------------------------------------------------------------------
# Message identifiers
# ---------------------------------------------------------------------------

# Reserved: never a valid message id. [rev5.1 sec 9.3 + verify:sdk-ant-shim]
MESG_INVALID_ID = 0x00

# Not a frame. Byte 1 of a RESPONSE_EVENT payload is 0x01 when the message is
# an unsolicited channel event rather than a reply to a command; the event code
# is byte 2. [bridge, tools (ant_session.EVENT_MARKER)]
MESG_EVENT_ID = 0x01

# Null-padded ASCII version string, MESG_VERSION_SIZE bytes. Requested with
# MESG_REQUEST. A stub build answers STUB0.01B00, which is deliberately not a
# real ANT version string. [tools]
MESG_VERSION_ID = 0x3E

# [channel, message id or 0x01, code]. Every command gets one, and every
# channel event arrives as one with 0x01 in the middle byte. [bridge]
MESG_RESPONSE_EVENT_ID = 0x40

# [channel]. Refused with CHANNEL_IN_WRONG_STATE until the channel has actually
# closed - a close is asynchronous. [tools]
MESG_UNASSIGN_CHANNEL_ID = 0x41

# [channel, channel type, network, (extended assignment)]. The fourth byte is
# optional and defaults to 0x00. [bridge, tools]
MESG_ASSIGN_CHANNEL_ID = 0x42

# [channel, period lo, period hi] in 1/32768 s counts. 8070 is the ANT+ heart-
# rate period, 8182 the power period. [bridge, tools]
MESG_CHANNEL_MESG_PERIOD_ID = 0x43

# [channel, timeout] in 2.5 s increments; 0xFF means never time out. [bridge,
# tools]
MESG_CHANNEL_SEARCH_TIMEOUT_ID = 0x44

# [channel, offset from 2400 MHz]. ANT+ is 57, i.e. 2457 MHz. [bridge, tools]
MESG_CHANNEL_RADIO_FREQ_ID = 0x45

# [network number, key[8]]. The key travels host to dongle, which is why no
# build of this firmware needs a network-key secret. [bridge, tools]
MESG_NETWORK_KEY_ID = 0x46

# [filler, level]. Device-wide transmit power - what ANT_SetTransmitPower()
# sends, and what Zwift calls while setting up a search. Answering it with
# INVALID_MESSAGE stalls the search. [bridge, readme]
MESG_RADIO_TX_POWER_ID = 0x47

# Continuous-wave test transmit. Not bridged. [rev5.1 sec 9.5, host-api]
MESG_RADIO_CW_MODE_ID = 0x48

# [channel, waveform lo, waveform hi]. Search duty cycle. [bridge]
MESG_SEARCH_WAVEFORM_ID = 0x49

# Resets the ANT protocol stack, NOT the MCU. Answered with the startup message
# and no RESPONSE_EVENT. Rebooting here drops the device off the bus and every
# host session begins with this. [bridge, tools]
MESG_SYSTEM_RESET_ID = 0x4A

# [channel]. [bridge, tools]
MESG_OPEN_CHANNEL_ID = 0x4B

# [channel]. Asynchronous: the channel is not free until EVENT_CHANNEL_CLOSED
# arrives. [bridge, tools]
MESG_CLOSE_CHANNEL_ID = 0x4C

# [channel or index, requested message id]. The reply is the requested message;
# a refusal is a RESPONSE_EVENT naming 0x4D. For several messages the first
# byte is not a channel at all - it is a mask number, an encryption info type,
# or 0/1 for advanced burst capabilities/configuration. [bridge, tools]
MESG_REQUEST_ID = 0x4D

# [channel, data[8]] outbound. Inbound it may carry the extended fields after
# the payload - see ext_flags. [bridge, tools]
MESG_BROADCAST_DATA_ID = 0x4E

# [channel, data[8]]. The message a host sends to set trainer resistance; the
# outcome arrives later as EVENT_TRANSFER_TX_COMPLETED or
# EVENT_TRANSFER_TX_FAILED. [bridge, tools]
MESG_ACKNOWLEDGED_DATA_ID = 0x4F

# [seq<<5 | channel, data[8]], bit 7 of byte 0 marks the last packet. Only bits
# 0-4 are the channel number, which is why 32 channels is the serial protocol's
# natural ceiling. No per-packet acknowledgement on success. [bridge, tools]
MESG_BURST_DATA_ID = 0x50

# [channel, device number lo, device number hi, device type, transmission
# type]. All-zero is the wildcard that matches anything. [bridge, tools]
MESG_CHANNEL_ID_ID = 0x51

# [channel, status]. Requested with MESG_REQUEST. [bridge, tools]
MESG_CHANNEL_STATUS_ID = 0x52

# Enter continuous-wave test mode. Not bridged. [rev5.1 sec 9.5, host-api]
MESG_RADIO_CW_INIT_ID = 0x53

# What the ANT *stack* can do - a superset of what the bridge implements.
# Requested with MESG_REQUEST. See the capabilities section for the byte and
# bit layout, and for the exact reply this firmware gives. [bridge, observed]
MESG_CAPABILITIES_ID = 0x54

# [filler, mode] in, [channel, mode] back. Dispatched at
# src/ant_serial_bridge.c. A Nordic extension, not in Rev 5.1; confirmed by a
# BUILD_ASSERT in src/ant_radio_sdk_ant.c. [sdk-ant-shim + verify:sdk-ant-shim]
MESG_CHANNEL_CRC_MODE_ID = 0x58

# [channel, device number lo, device number hi, device type, transmission type,
# list index]. [bridge]
MESG_ID_LIST_ADD_ID = 0x59

# [channel, list size, include/exclude flag]. [bridge]
MESG_ID_LIST_CONFIG_ID = 0x5A

# Background scanning. Bridged as of 2026-08-10: refuses with
# ANTW_CLOSE_ALL_CHANNELS unless every channel is closed, then takes over
# channel 0 as a wildcard background-scan slave - the same mechanism
# MESG_ASSIGN_CHANNEL's extended byte already reaches, one message at a time.
# The optional synchronous-channel-packets-only byte is accepted and ignored;
# reporting everything is a superset of the restricted subset. radiant
# backend only - the capabilities reply advertises this on radiant and OFF
# elsewhere, which is why a host with the call (ANT_OpenRxScanMode) sends it
# only there. [bridge]
MESG_OPEN_RX_SCAN_MODE_ID = 0x5B

# Legacy extended broadcast: channel id inline instead of behind a flag byte.
# Superseded by MESG_ANTLIB_CONFIG's flag mechanism; this dongle never emits
# it. [rev5.1 sec 9.5, host-api]
MESG_EXT_BROADCAST_DATA_ID = 0x5D

# Legacy extended acknowledged data. See MESG_EXT_BROADCAST_DATA_ID. [rev5.1
# sec 9.5, host-api]
MESG_EXT_ACKNOWLEDGED_DATA_ID = 0x5E

# Legacy extended burst data. See MESG_EXT_BROADCAST_DATA_ID. [rev5.1 sec 9.5,
# host-api]
MESG_EXT_BURST_DATA_ID = 0x5F

# [channel, level] or [channel, level, custom]. Per-channel transmit power.
# Only takes on an assigned channel, so the bridge holds the requested level
# and reapplies it after each successful assign. The third byte is an extension
# of this firmware's and is read only when the level carries
# RADIO_TX_POWER_LVL_CUSTOM: that level names a raw radio register value and
# there is nowhere else on the wire to put one, so without it the custom level
# silently means register 0. No retail stick sends three bytes and none needs
# to - a two-byte message means exactly what it always did. [bridge, readme]
MESG_CHANNEL_RADIO_TX_POWER_ID = 0x60

# Read the device serial number. Not bridged. [rev5.1 sec 9.5 + verify:sdk-ant-
# shim]
MESG_GET_SERIAL_NUM_ID = 0x61

# [channel, timeout]. Low-priority search timeout, in 2.5 s increments.
# [bridge]
MESG_SET_LP_SEARCH_TIMEOUT_ID = 0x63

# Set the channel's device number from the device's own serial number. Implied
# by the ANT_DLL export ANT_SetSerialNumChannelId, left unmapped by the
# archive export table; its neighbours 0x63 and 0x66 are confirmed by that
# table. Not bridged. [rev5.1 sec 9.5 + verify:sdk-ant-shim]
MESG_SERIAL_NUM_SET_CHANNEL_ID_ID = 0x65

# [filler, enable]. The older, narrower way of asking for the channel id on
# every received message; the bridge maps it onto lib config bit
# MESG_OUT_INC_DEVICE_ID. Hosts still send it instead of 0x6E, and without it
# every broadcast arrives anonymous. [bridge]
MESG_RX_EXT_MESGS_ENABLE_ID = 0x66

# Not bridged, and the LED capability bit is reported OFF - which is why a host
# that has ANT_EnableLED never sends it. [readme]
MESG_ENABLE_LED_FLASH_ID = 0x68

# Force the crystal oscillator on. Implied by the ANT_DLL export
# ANT_CrystalEnable (which Zwift resolves); the archive export table left it
# unmapped, but confirms the adjacent 0x6E (ANT_LibConfigCustom). Not bridged.
# [rev5.1 sec 9.5 + verify:sdk-ant-shim]
MESG_XTAL_ENABLE_ID = 0x6D

# [filler, config bits]. Turns the extended output fields on. Zero means clear
# everything - there is no separate clear message on the wire. 0xE0 is channel
# id + RSSI + RX timestamp; 0x80 is device id only. [bridge, tools]
MESG_ANTLIB_CONFIG_ID = 0x6E

# [reason]. Sent once after the stack has reset. A host that does not see this
# concludes the dongle is dead. [bridge, tools]
MESG_STARTUP_MESG_ID = 0x6F

# [channel, freq0, freq1, freq2]. Frequency-agility hop table. [bridge]
MESG_AUTO_FREQ_CONFIG_ID = 0x70

# [channel, threshold, (custom threshold)]. Only pair with something close by.
# [bridge]
MESG_PROX_SEARCH_CONFIG_ID = 0x71

# Same header byte as MESG_BURST_DATA, but 8, 16 or 24 data bytes. Accepting
# this while never bridging 0x78 gives a dongle that takes 24-byte packets and
# can never send one. [bridge, readme]
MESG_ADV_BURST_DATA_ID = 0x72

# Not bridged - sdk-ant exposes no API for it - and correspondingly NOT
# advertised in the capabilities reply. The one advanced-options-3 bit that is
# honestly zero. [tools, readme]
MESG_EVENT_BUFFERING_CONFIG_ID = 0x74

# [channel, priority]. [bridge, rev5.1 sec 9.5 + verify:sdk-ant-shim]
MESG_SET_SEARCH_CH_PRIORITY_ID = 0x75

# Advertised in the capabilities reply and deliberately not bridged: sdk-ant
# has no single-chip API for it. Clearing the bit instead would make this
# dongle report something no ANTUSB-m reports. [tools, readme]
MESG_HIGH_DUTY_SEARCH_MODE_ID = 0x77

# [filler, enable, rf payload size, required modes, 0, 0, optional modes, 0, 0,
# (stall lo, stall hi, retry extension)]. Index 0 requests capabilities (4-byte
# reply), index 1 the current config (10-byte reply, enable byte dropped) -
# selected by byte 0 of the MESG_REQUEST, which the reply does not repeat, so
# a reader must pair the reply with its request to know which shape it got.
# [bridge, tools]
MESG_CONFIG_ADV_BURST_ID = 0x78

# Command is [filter lo, filter hi], no channel byte; the reply is [filler,
# filter lo, filter hi] - one byte longer, checked by ant_features.py's
# round-trip test. The `2..3` payload_len above is the union of both shapes;
# left as-is because narrowing it would change the malformed-case set
# tools/ant_conformance.py generates and break the committed Tier 1 transcript.
# [bridge, tools]
MESG_EVENT_FILTER_CONFIG_ID = 0x79

# [channel, mask config]. Binds one of the masks to a channel; INVALID_SDU_MASK
# (0xFF) turns selective updates back off. [bridge, tools]
MESG_SDU_CONFIG_ID = 0x7A

# [mask index, mask[8]]. byte 0 is a mask number, not a channel. The reply has
# the same shape, which is why its size is ANT_MAX_PAYLOAD_SIZE +
# CHANNEL_NUM_SIZE. [bridge, tools]
MESG_SDU_SET_MASK_ID = 0x7B

# User NVM configuration page. No sdk-ant API; not bridged. Implied by the
# ANT_DLL export ANT_ConfigUserNVM, left unmapped by the archive export table;
# its neighbours 0x7B and 0x7D-0x7F are confirmed by that table.
# [rev5.1 sec 9.5 + verify:sdk-ant-shim]
MESG_USER_CONFIG_PAGE_ID = 0x7C

# Write [channel, mode, key number, decimation rate] - compiled out unless
# CONFIG_ANT_DONGLE_ENCRYPTION. Read side always bridged: an ENCRYPTION_INFO_
# GET_* index returns [info type, data...] with the type echoed at byte 0, so
# unlike 0x78/0x79 above the reply itself says which of the three shapes (2,
# 5 or 20 bytes) it is. An unknown index gets a RESPONSE_EVENT refusal instead
# of a reply. [bridge, tools]
MESG_ENCRYPT_ENABLE_ID = 0x7D

# [key number, key[16]]. That the key is 128 bits is stated in prose only; no
# size constant names it. [bridge, tools]
MESG_SET_ENCRYPT_KEY_ID = 0x7E

# [info type, data...]. How much data depends on the type, and the length must
# be checked against it here - the stack reads a fixed count per type with
# nothing to bound it. RNG seed is refused on purpose: no size for it is
# documented anywhere. [bridge, tools]
MESG_SET_ENCRYPT_INFO_ID = 0x7F

# [channel, cycles]. [bridge, rev5.1 sec 9.5 + verify:sdk-ant-shim]
MESG_ACTIVE_SEARCH_SHARING_ID = 0x81

# [filler, enable]. Enhanced channel spacing. A Nordic extension, not in Rev
# 5.1; confirmed by a BUILD_ASSERT in src/ant_radio_sdk_ant.c. [sdk-ant-shim +
# verify:sdk-ant-shim]
MESG_ECS_ENABLE_ID = 0x89

# [channel] to clear, [channel, pending] on request. A Nordic extension, not
# in Rev 5.1; confirmed by a BUILD_ASSERT in src/ant_radio_sdk_ant.c. The
# request form replies with MESG_PENDING_TRANSMIT_GET_SIZE bytes, separate
# from the 1-byte command payload. [sdk-ant-shim + verify:sdk-ant-shim]
MESG_PENDING_TRANSMIT_CLEAR_ID = 0x8C

# Serial-layer error: bad checksum, bad length, or a frame that arrived too
# slowly. This firmware drops malformed frames silently instead - recorded so a
# host-side parser can name the byte. [rev5.1 sec 7.4 + verify:sdk-ant-shim]
MESG_SERIAL_ERROR_ID = 0xAE

# MESG_RSSI_SEARCH_THRESHOLD_ID: UNRESOLVED. RSSI-based proximity threshold,
# implied by ANT_DLL export ANT_RSSI_SetSearchThreshold. Believed to sit in
# the 0xC0 AP2-era extension block, but archive/host-api/ant_dll_exports.json
# tops out at 0x7B and sdk-ant v2.1.0 defines no such id, so no number is
# recorded. [host-api + verify:sdk-ant-shim]

# MESG_SLEEP_ID: UNRESOLVED. Low-power sleep, implied by ANT_DLL export
# ANT_SleepMessage (which Zwift resolves). Same 0xC0 block and same lack of
# corroboration as MESG_RSSI_SEARCH_THRESHOLD_ID; sdk-ant references the name
# only inside a commented-out dispatch arm, so no number is recorded.
# [host-api + verify:sdk-ant-shim]

# ---------------------------------------------------------------------------
# Reply sizes
# ---------------------------------------------------------------------------

# [channel, message id, code]. [bridge]
MESG_RESPONSE_EVENT_SIZE = 3

# [reason]. [bridge]
MESG_STARTUP_MESG_SIZE = 1

# Nine bytes, confirmed by the observed reply 080800b23200fd8d0f. [observed]
MESG_CAPABILITIES_SIZE = 9

# Null-padded version string. src/ant_radio_stub.c calls it a 20-byte payload;
# nothing in this repository shows the reply on the wire. [stub + verify:sdk-
# ant-shim]
MESG_VERSION_SIZE = 20

# [channel, status]. [inferred + verify:sdk-ant-shim]
MESG_CHANNEL_STATUS_SIZE = 2

# [channel, number lo, number hi, device type, transmission type]. [bridge]
MESG_CHANNEL_ID_SIZE = 5

# [channel, period lo, period hi]. [bridge]
MESG_CHANNEL_MESG_PERIOD_SIZE = 3

# [channel, frequency]. [bridge]
MESG_CHANNEL_RADIO_FREQ_SIZE = 2

# [channel, mode]. [inferred + verify:sdk-ant-shim]
MESG_CHANNEL_CRC_MODE_SIZE = 2

# [channel, priority]. [inferred + verify:sdk-ant-shim]
MESG_SET_SEARCH_CH_PRIORITY_SIZE = 2

# [channel, pending]. [inferred + verify:sdk-ant-shim]
MESG_PENDING_TRANSMIT_GET_SIZE = 2

# [filler, config bits]. [bridge]
MESG_ANTLIB_CONFIG_SIZE = 2

# [channel, cycles]. [inferred + verify:sdk-ant-shim]
MESG_ACTIVE_SEARCH_SHARING_REQ_SIZE = 2

# [filler, filter lo, filter hi] - one byte longer than the command that set
# it. [bridge, tools]
MESG_EVENT_FILTER_CONFIG_REQ_SIZE = 3

# [size, required, 0, 0, optional, 0, 0, stall lo, stall hi, retry]. The
# 11-byte configuration minus the enable byte the command carried. [stub,
# tools]
MESG_CONFIG_ADV_BURST_REQ_CONFIG_SIZE = 10

# The advanced-burst capabilities reply: byte 0 max packet size code, byte 1
# supported-modes bitfield, bytes 2-3 reserved/zero. Not visible in this repo
# (ant_features.py reads only the first two bytes); confirmed by a
# BUILD_ASSERT in src/ant_radio_sdk_ant.c and used to bound the memset at
# src/ant_radio_stub.c:254. [sdk-ant-shim + verify:sdk-ant-shim]
MESG_CONFIG_ADV_BURST_REQ_CAPABILITIES_SIZE = 4

# [info type, supported mode]. [bridge]
MESG_CONFIG_ENCRYPT_REQ_CAPABILITIES_SIZE = 2

# [info type, crypto id[4]]. [bridge]
MESG_CONFIG_ENCRYPT_REQ_CONFIG_ID_SIZE = 5

# [info type, custom user data[19]]. The longest frame body the parser ever
# sees. [bridge, tools]
MESG_CONFIG_ENCRYPT_REQ_CONFIG_USER_DATA_SIZE = 20

# ---------------------------------------------------------------------------
# Channel types
# Byte 1 of MESG_ASSIGN_CHANNEL.
# ---------------------------------------------------------------------------

# Bidirectional receive. What every host opens to hear a sensor. [tools
# (ant_verify)]
CHANNEL_TYPE_SLAVE = 0x00

# Bidirectional transmit. What ant_sim.py asks for to make this dongle
# impersonate a sensor. [tools (ant_sim)]
CHANNEL_TYPE_MASTER = 0x10

# Shared bidirectional receive. [rev5.1 sec 5.2.1 + verify:sdk-ant-shim]
CHANNEL_TYPE_SHARED_SLAVE = 0x20

# Shared bidirectional transmit. [rev5.1 sec 5.2.1 + verify:sdk-ant-shim]
CHANNEL_TYPE_SHARED_MASTER = 0x30

# Receive only - never transmits, so it cannot acknowledge. [rev5.1 sec 5.2.1 +
# verify:sdk-ant-shim]
CHANNEL_TYPE_SLAVE_RX_ONLY = 0x40

# Transmit only - never opens a receive window. [rev5.1 sec 5.2.1 + verify:sdk-
# ant-shim]
CHANNEL_TYPE_MASTER_TX_ONLY = 0x50

# ---------------------------------------------------------------------------
# Extended assignment
# Optional byte 3 of MESG_ASSIGN_CHANNEL. The bridge passes it straight
# through.
# ---------------------------------------------------------------------------

# Background scanning enable. [rev5.1 sec 5.2.1 + verify:sdk-ant-shim]
EXT_PARAM_ALWAYS_SEARCH = 0x01

# Frequency agility enable. ANT+ profiles forbid it. [rev5.1 sec 5.2.1 +
# verify:sdk-ant-shim]
EXT_PARAM_FREQUENCY_AGILITY = 0x04

# Automatic shared-slave address management. [rev5.1 sec 5.2.1 + verify:sdk-
# ant-shim]
EXT_PARAM_AUTO_SHARED_SLAVE = 0x08

# Fast channel initiation. [readme]
EXT_PARAM_FAST_INITIATION_MODE = 0x10

# Asynchronous transmission channel. [readme]
EXT_PARAM_ASYNC_TX_MODE = 0x20

# ---------------------------------------------------------------------------
# Channel status
# Low two bits of byte 1 of a MESG_CHANNEL_STATUS reply.
# ---------------------------------------------------------------------------

# No channel type or network set. [stub]
STATUS_UNASSIGNED_CHANNEL = 0x00

# Assigned but not open. Assigning again is refused with
# CHANNEL_IN_WRONG_STATE, which is why every host opens a session with a system
# reset. [rev5.1 sec 9.5.7.1 + verify:sdk-ant-shim]
STATUS_ASSIGNED_CHANNEL = 0x01

# Open and searching. [rev5.1 sec 9.5.7.1 + verify:sdk-ant-shim]
STATUS_SEARCHING_CHANNEL = 0x02

# Open and tracking a master. [rev5.1 sec 9.5.7.1 + verify:sdk-ant-shim]
STATUS_TRACKING_CHANNEL = 0x03

# The bits above; the rest of the byte carries network number and channel type.
# [rev5.1 sec 9.5.7.1 + verify:sdk-ant-shim]
STATUS_CHANNEL_STATE_MASK = 0x03

# ---------------------------------------------------------------------------
# Radio transmit power levels
# Byte 1 of MESG_RADIO_TX_POWER (device-wide) and byte 1 of
# MESG_CHANNEL_RADIO_TX_POWER (per channel). The dBm figures are the levels a
# retail ANT stick exposes; what a given radio actually emits is a property of
# the part.
# ---------------------------------------------------------------------------

# -20 dBm. A deliberate request to be quiet - a bike shop, or a race paddock
# with forty trainers in one room. [bridge]
RADIO_TX_POWER_LVL_0 = 0x00

# -12 dBm. [bridge]
RADIO_TX_POWER_LVL_1 = 0x01

# -4 dBm. [bridge]
RADIO_TX_POWER_LVL_2 = 0x02

# 0 dBm. The ANT default, and what a host sends when it has no opinion - the
# common case. [bridge]
RADIO_TX_POWER_LVL_3 = 0x03

# +4 dBm. The ceiling on the nRF51422 inside the stick being impersonated, so a
# host asking for it is asking for everything the hardware has. [bridge]
RADIO_TX_POWER_LVL_4 = 0x04

# +8 dBm. Exists only on the nRF52820, nRF52833 and nRF52840. No host will ever
# request it by name, because no retail stick has ever had it. [bridge]
RADIO_TX_POWER_LVL_5 = 0x05

# Names a raw register value rather than a level, so it is passed through
# untouched. [bridge]
RADIO_TX_POWER_LVL_CUSTOM = 0x80

# ---------------------------------------------------------------------------
# Library configuration bits
# Byte 1 of MESG_ANTLIB_CONFIG. Each bit appends one field to every received
# data message; the fields arrive in bit order behind a flag byte. Sending 0x00
# clears all of them - there is no separate clear message on the wire.
# ---------------------------------------------------------------------------

# Reconfigure the radio on every event rather than only on change. [rev5.1 sec
# 9.5.2.9 + verify:sdk-ant-shim]
LIB_CONFIG_RADIO_CONFIG_ALWAYS = 0x01

# Append the receive timestamp: 16 bits of the radio's own 32768 Hz counter, so
# it wraps every two seconds. The only clock on the path that is not the host's
# - it reads 0.009 ms of timing error where the host clock reads 2.6 ms. [tools
# (ant_verify)]
LIB_CONFIG_MESG_OUT_INC_TIME_STAMP = 0x20

# Append RSSI. The difference between a packet lost to a collision and one lost
# to a fade. [tools (ant_verify)]
LIB_CONFIG_MESG_OUT_INC_RSSI = 0x40

# Append the channel id. Without it every broadcast arrives anonymous and no
# sensor can be named. This is also what MESG_RX_EXT_MESGS_ENABLE maps onto.
# [bridge, tools]
LIB_CONFIG_MESG_OUT_INC_DEVICE_ID = 0x80

# Alias: the narrow setting ant_scan.py and ant_session.py use - identity,
# nothing else. [tools]
LIB_CONFIG_DEVICE_ID_ONLY = 0x80

# Channel id + RSSI + RX timestamp, all three. What ant_verify.py asks for, and
# what radiant must assemble: the timestamp is the figure the timing gate
# is read against. [tools (ant_verify)]
LIB_CONFIG_ALL_EXT_FIELDS = 0xE0

# Passed to the clear path when a host sends 0x6E with a zero config byte.
# [bridge + verify:sdk-ant-shim]
LIB_CONFIG_MASK_ALL = 0xFF

# ---------------------------------------------------------------------------
# Extended message flag bits
# Byte 9 of a received data message - immediately after the 8-byte payload -
# when any lib config bit is set. The fields follow in a fixed order, but each
# is present only if its flag bit is set, so an offset is only correct relative
# to which of the earlier ones turned up. Reading RSSI at a fixed offset works
# right up until a run is made without the channel id, and then quietly reports
# the device number as a signal strength.
# ---------------------------------------------------------------------------

# 4 bytes follow: device number lo, device number hi, device type (bit 7 is the
# pairing bit), transmission type. [tools (ant_verify, ant_scan)]
EXT_FLAG_CHANNEL_ID = 0x80

# 3 bytes follow: measurement type, value, threshold configuration. [tools
# (ant_verify)]
EXT_FLAG_RSSI = 0x40

# 2 bytes follow: the 32768 Hz receive timestamp, little endian. [tools
# (ant_verify)]
EXT_FLAG_RX_TIMESTAMP = 0x20

# ---------------------------------------------------------------------------
# RSSI measurement types
# Byte 0 of the RSSI extended field.
# ---------------------------------------------------------------------------

# The one measurement type that carries dBm. Anything else is a proprietary
# scale, and a number on an unknown scale is worse than no number. [tools
# (ant_verify)]
RSSI_MEASUREMENT_TYPE_DBM = 0x20

# ---------------------------------------------------------------------------
# Startup message reasons
# The single payload byte of MESG_STARTUP_MESG. Zero is not a bit: it means a
# power-on or a command reset with no other flag set.
# ---------------------------------------------------------------------------

# Power-on, or a reset by command. [tools (ant_probe)]
STARTUP_POWER_ON_RESET = 0x00

# Hardware reset line. [tools (ant_probe)]
STARTUP_HARDWARE_RESET_LINE = 0x01

# Watchdog. [tools (ant_probe)]
STARTUP_WATCH_DOG_RESET = 0x02

# Reset by MESG_SYSTEM_RESET. What this firmware reports - it sends 0x00,
# because the stack reset is indistinguishable from a cold start from the
# host's side. [tools (ant_probe)]
STARTUP_COMMAND_RESET = 0x20

# Synchronous serial reset. [tools (ant_probe)]
STARTUP_SYNCHRONOUS_RESET = 0x40

# Resume from suspend. [tools (ant_probe)]
STARTUP_SUSPEND_RESET = 0x80

# ---------------------------------------------------------------------------
# Channel event codes
# Byte 2 of a RESPONSE_EVENT whose byte 1 is MESG_EVENT_ID (0x01). These arrive
# unsolicited; the response codes below arrive in answer to a command. They
# share one number space.
# ---------------------------------------------------------------------------

# The search window expired without finding a master. [tools (ant_session)]
EVENT_RX_SEARCH_TIMEOUT = 0x01

# A receive slot came and went with nothing valid in it. This is the event that
# makes loss accounting possible: the radio reports one for every packet lost
# on the air and nothing at all for one lost in the host, so a run that loses
# more than it can account for is losing it locally. [tools (ant_session,
# ant_verify)]
EVENT_RX_FAIL = 0x02

# A payload has gone on the air; load the next one. ant_sim.py paces itself on
# this rather than on a host timer. [tools (ant_session, ant_sim)]
EVENT_TX = 0x03

# An inbound burst did not complete. [tools (ant_session)]
EVENT_TRANSFER_RX_FAILED = 0x04

# Acknowledged data or a burst was acknowledged by the far end. Also releases
# the bridge's single burst block. [bridge, tools]
EVENT_TRANSFER_TX_COMPLETED = 0x05

# It went out and nothing acknowledged it. Also releases the burst block.
# [bridge, tools]
EVENT_TRANSFER_TX_FAILED = 0x06

# The channel is now free to unassign. A close is asynchronous, so unassigning
# before this arrives is refused. [bridge, tools]
EVENT_CHANNEL_CLOSED = 0x07

# Enough consecutive misses that the channel has dropped back into search.
# [tools (ant_session)]
EVENT_RX_FAIL_GO_TO_SEARCH = 0x08

# Two channels wanted the radio at the same instant. [tools (ant_session)]
EVENT_CHANNEL_COLLISION = 0x09

# Progress, not an outcome - keep waiting. [tools (ant_session)]
EVENT_TRANSFER_TX_START = 0x0A

# Data blocked because the application is servicing events too slowly, distinct
# from EVENT_QUE_OVERFLOW (0x35) which is a queue sized too small. Not raised
# by this firmware today, but kept in the table so a host that ever sees it
# decodes something other than an unknown event code. [rev5.1 sec 9.5.6]
EVENT_RX_DATA_OVERFLOW = 0x0B

# The stack has finished with the burst block it was handed and the next may
# overwrite it. The bridge consumes this internally and never puts it on the
# wire: a real stick frames bursts itself. If radiant fails to raise it
# exactly once per accepted block, host bursts stall 1000 ms per packet.
# [bridge, rev5.1 sec 9.5.6 + verify:sdk-ant-shim]
EVENT_TRANSFER_NEXT_DATA_BLOCK = 0x11

# ---------------------------------------------------------------------------
# Response codes
# Byte 2 of a RESPONSE_EVENT answering a command. Same number space as the
# event codes.
# ---------------------------------------------------------------------------

# Accepted. A host that gets anything else for a message it needs gives up, and
# says nothing useful about which one. [bridge, tools]
RESPONSE_NO_ERROR = 0x00

# The commonest real-world refusal: assigning a channel a previous run left
# assigned, or unassigning one that has not finished closing. [rev5.1 sec 9.5.6
# + verify:sdk-ant-shim]
CHANNEL_IN_WRONG_STATE = 0x15

# Data was sent on a channel that is not open. [rev5.1 sec 9.5.6 + verify:sdk-
# ant-shim]
CHANNEL_NOT_OPENED = 0x16

# Opened before MESG_CHANNEL_ID. [rev5.1 sec 9.5.6 + verify:sdk-ant-shim]
CHANNEL_ID_NOT_SET = 0x18

# Scan mode requires every other channel closed. [rev5.1 sec 9.5.6 +
# verify:sdk-ant-shim]
CLOSE_ALL_CHANNELS = 0x19

# A burst is already running on this channel. The bridge answers with this when
# the host outruns the single burst block. [bridge, rev5.1 sec 9.5.6 +
# verify:sdk-ant-shim]
TRANSFER_IN_PROGRESS = 0x1F

# Burst packets arrived out of order. [rev5.1 sec 9.5.6 + verify:sdk-ant-shim]
TRANSFER_SEQUENCE_NUMBER_ERROR = 0x20

# Burst aborted. [rev5.1 sec 9.5.6 + verify:sdk-ant-shim]
TRANSFER_IN_ERROR = 0x21

# The channel is busy with another transfer. Distinct from TRANSFER_IN_PROGRESS
# (0x1F), which is what this bridge answers when the host outruns its single
# burst block. [rev5.1 sec 9.5.6 + verify:sdk-ant-shim]
TRANSFER_BUSY = 0x22

# Payload longer than the message allows. [rev5.1 sec 9.5.6 + verify:sdk-ant-
# shim]
MESSAGE_SIZE_EXCEEDS_LIMIT = 0x27

# The dispatcher does not implement this message, or its body was too short to
# run. Distinguishing 'the bridge refused it' from 'the stack refused it' is
# exactly this code: anything else means the message was decoded and handed on.
# [bridge, tools]
INVALID_MESSAGE = 0x28

# Network number above the maximum the capabilities reply advertises. [rev5.1
# sec 9.5.6 + verify:sdk-ant-shim]
INVALID_NETWORK_NUMBER = 0x29

# Inclusion/exclusion list index out of range. [rev5.1 sec 9.5.6 + verify:sdk-
# ant-shim]
INVALID_LIST_ID = 0x30

# Transmit attempted on a channel other than 0 in scan mode. [rev5.1 sec 9.5.6
# + verify:sdk-ant-shim]
INVALID_SCAN_TX_CHANNEL = 0x31

# The stack decoded the message and disliked a value. Measured example:
# MESG_SET_ENCRYPT_KEY answers 51 when CONFIG_ANT_ENCRYPTED_CHANNELS is 0,
# because the valid key index range is then empty. [readme, tools]
INVALID_PARAMETER_PROVIDED = 0x33

# The host is sending faster than the serial layer drains. [rev5.1 sec 9.5.6 +
# verify:sdk-ant-shim]
EVENT_SERIAL_QUE_OVERFLOW = 0x34

# The event queue overflowed - events were dropped. [rev5.1 sec 9.5.6 +
# verify:sdk-ant-shim]
EVENT_QUE_OVERFLOW = 0x35

# ---------------------------------------------------------------------------
# Advanced burst configuration
# Fields of MESG_CONFIG_ADV_BURST.
# ---------------------------------------------------------------------------

# Enable byte: off. The shipping default. [tools (ant_features)]
ADV_BURST_MODE_DISABLE = 0x00

# Enable byte: on. [tools (ant_features)]
ADV_BURST_MODE_ENABLE = 0x01

# RF payload size code: 8 bytes. [inferred + verify:sdk-ant-shim]
ADV_BURST_MODES_SIZE_8_BYTES = 0x01

# RF payload size code: 16 bytes. [inferred + verify:sdk-ant-shim]
ADV_BURST_MODES_SIZE_16_BYTES = 0x02

# RF payload size code: 24 bytes. What the stub advertises and what
# ant_features.py configures. [stub, tools]
ADV_BURST_MODES_SIZE_24_BYTES = 0x03

# Optional-modes bit: frequency hopping. [stub, tools]
ADV_BURST_MODES_FREQ_HOP = 0x01

# ---------------------------------------------------------------------------
# Selective data update
# Values of the mask-config byte of MESG_SDU_CONFIG.
# ---------------------------------------------------------------------------

# Detaches a channel from every mask - the way to turn selective updates back
# off. [stub, tools]
INVALID_SDU_MASK = 0xFF

# Set alongside a mask number to apply the mask to acknowledged data too. The
# backend masks this off before range-checking the mask number; confirmed by a
# BUILD_ASSERT in src/ant_radio_sdk_ant.c. Shares its value with
# INVALID_SDU_MASK's top bit, so the mask number must be range-checked after
# the bit is cleared, not before. [sdk-ant-shim + verify:sdk-ant-shim]
SDU_MASK_ACK_CONFIG_BIT = 0x80

# ---------------------------------------------------------------------------
# Encryption
# Info types and modes for MESG_ENCRYPT_ENABLE / MESG_SET_ENCRYPT_INFO. The set
# and get info types do not line up: set 0 is the crypto id and get 0 is the
# supported mode, so a round trip writes with one number and reads with the
# next. That is not a bug in the tools.
# ---------------------------------------------------------------------------

# Channel encryption off. [tools (ant_features)]
ENCRYPTION_DISABLED_MODE = 0x00

# Write: 4-byte crypto id. [tools (ant_features)]
ENCRYPTION_INFO_SET_CRYPTO_ID = 0x00

# Write: 19 bytes of custom user data. [tools (ant_features)]
ENCRYPTION_INFO_SET_CUSTOM_USER_DATA = 0x01

# Write: RNG seed. Deliberately refused by this bridge - sdk-ant calls it
# platform specific and defines no size for it anywhere, and the stack does not
# take its randomness from the host in any case. Refusing beats guessing a
# length. [bridge, tools]
ENCRYPTION_INFO_SET_RNG_SEED = 0x02

# Read: the supported encryption mode. [tools (ant_features)]
ENCRYPTION_INFO_GET_SUPPORTED_MODE = 0x00

# Read: the 4-byte crypto id. [tools (ant_features)]
ENCRYPTION_INFO_GET_CRYPTO_ID = 0x01

# Read: the 19 bytes of custom user data. [tools (ant_features)]
ENCRYPTION_INFO_GET_CUSTOM_USER_DATA = 0x02

# Bytes of custom user data. One info-type byte plus these fills MAX_SIZE_VALUE
# exactly. [stub, tools]
ENCRYPTION_USER_DATA_SIZE = 19

# 128-bit key. No sdk-ant constant names this; it is stated only in the prose
# of ant_crypto_key_set(), which takes a bare pointer. [bridge, stub]
ENCRYPTION_KEY_SIZE = 16

# The value a backend answers an ENCRYPTION_INFO_GET_SUPPORTED_MODE request
# with, and the upper bound a channel-enable is range-checked against - the
# highest of the encryption modes above, not an independent number. Not
# visible in this repo; confirmed by a BUILD_ASSERT in
# src/ant_radio_sdk_ant.c, used at src/ant_radio_stub.c:278 and :541.
# [sdk-ant-shim + verify:sdk-ant-shim]
MAX_SUPPORTED_ENCRYPTION_MODE = 0x02

# ---------------------------------------------------------------------------
# Burst header byte
# Byte 0 of MESG_BURST_DATA / MESG_ADV_BURST_DATA / the legacy
# MESG_EXT_BURST_DATA. Not a channel number on its own: it is three fields
# packed into one byte, and the five-bit channel field is the ONLY place in the
# serial protocol where a channel number is squeezed below eight bits. Every
# other channel byte on the wire - including byte 0 of a RESPONSE_EVENT, which
# echoes whatever the command carried - is a plain uint8. Applying the five-bit
# ceiling to those is a category error: case frame/sync-in-payload deliberately
# sends channel 0xA4 in an antlib-config command so that a SYNC byte lands
# inside the payload, and the dongle correctly echoes 0xA4 back in a 0x40.
# ---------------------------------------------------------------------------

# Channel number. Five bits - which is why 32 channels is the serial protocol's
# natural ceiling, and why radiant is sized for 32 from the first line.
# Because the field is five bits wide, a header on the wire cannot express a
# channel above 31 at all; what a burst header CAN address that the device does
# not have is a channel above the count in byte 0 of the capabilities reply,
# and that is the bound worth checking on a transcript. [bridge]
BURST_HEADER_CHANNEL_MASK = 0x1F

# Sequence number occupies bits 5-6. [bridge]
BURST_HEADER_SEQ_SHIFT = 5

# Sequence number after shifting: 0-3, wrapping. [bridge]
BURST_HEADER_SEQ_MASK = 0x03

# Bit 7: this is the last packet of the transfer. [bridge, tools]
BURST_HEADER_LAST = 0x80

# ---------------------------------------------------------------------------
# RadiANT pairing sub-commands
# Byte [1] of MESG_RADIANT_PAIRING (0xF5). NOT ANT protocol - ours. Sub-
# commands rather than four message ids because they are one conversation with
# an order: enter, supply a scalar, exchange, leave. Separate ids would let a
# host skip a step and get a state error it could not localise. The reply
# echoes the sub-command in the same byte, because a public key and a
# fingerprint are both 'some bytes after a channel byte' otherwise.
# ---------------------------------------------------------------------------

# Leave pairing mode and wipe the exchange state, including any scalar the host
# supplied. [K4 (docs/radiant-security.md sec 7.4 and 8)]
RADIANT_PAIR_LEAVE = 0x00

# Enter pairing mode, timeout in seconds at [2]. Zero means the 60 s default
# and NEVER 'forever': a node in pairing mode accepts a key from whoever asks,
# so one forgotten command must not leave it open indefinitely. [K4
# (docs/radiant-security.md sec 7.4 and 8)]
RADIANT_PAIR_ENTER = 0x01

# Supply the host's 32-byte X25519 private scalar at [2..33]. The reply carries
# the local public key from [2]. The scalar comes from the host because the
# only entropy source on nRF54L is psa_rng/CRACEN and reaching it drags
# nrf_security into every build; the honest consequence is that a host-less
# node cannot pair this way. [K4 (docs/radiant-security.md sec 7.4 and 8)]
RADIANT_PAIR_SCALAR = 0x02

# Complete the exchange against the peer's 32-byte public key at [2..33]. The
# reply carries the six-digit comparison fingerprint as a u24 LE from [2]. A
# small-order peer key is refused: the result becomes a root key, so accepting
# one would let anyone able to inject a packet fix the group key to a value
# they already know. [K4 (docs/radiant-security.md sec 7.4 and 8)]
RADIANT_PAIR_EXCHANGE = 0x03

# ---------------------------------------------------------------------------
# Capabilities reply (MESG_CAPABILITIES)
# 9 bytes. Observed from this firmware: 080800b23200fd8d0f
# ---------------------------------------------------------------------------

CAPABILITIES_OFFSET_MAX_CHANNELS = 0
CAPABILITIES_OFFSET_MAX_NETWORKS = 1
CAPABILITIES_OFFSET_STANDARD_OPTIONS = 2
CAPABILITIES_OFFSET_ADVANCED_OPTIONS = 3
CAPABILITIES_OFFSET_ADVANCED_OPTIONS_2 = 4
CAPABILITIES_OFFSET_MAX_SENSRCORE_CHANNELS = 5
CAPABILITIES_OFFSET_ADVANCED_OPTIONS_3 = 6
CAPABILITIES_OFFSET_ADVANCED_OPTIONS_4 = 7
CAPABILITIES_OFFSET_ADVANCED_OPTIONS_5 = 8

# Cannot receive. [rev5.1 sec 9.5.7.4 + verify:sdk-ant-shim]
CAPABILITIES_NO_RECEIVE_CHANNELS = 0x01

# Cannot transmit. [rev5.1 sec 9.5.7.4 + verify:sdk-ant-shim]
CAPABILITIES_NO_TRANSMIT_CHANNELS = 0x02

# No receive messages. [rev5.1 sec 9.5.7.4 + verify:sdk-ant-shim]
CAPABILITIES_NO_RECEIVE_MESSAGES = 0x04

# No transmit messages. [rev5.1 sec 9.5.7.4 + verify:sdk-ant-shim]
CAPABILITIES_NO_TRANSMIT_MESSAGES = 0x08

# No acknowledged data. [rev5.1 sec 9.5.7.4 + verify:sdk-ant-shim]
CAPABILITIES_NO_ACKD_MESSAGES = 0x10

# No burst. [rev5.1 sec 9.5.7.4 + verify:sdk-ant-shim]
CAPABILITIES_NO_BURST_MESSAGES = 0x20

# MESG_NETWORK_KEY is supported. [stub]
CAPABILITIES_NETWORK_ENABLED = 0x02

# The device has a readable serial number. [stub]
CAPABILITIES_SERIAL_NUMBER_ENABLED = 0x08

# MESG_CHANNEL_RADIO_TX_POWER (0x60) is supported. [stub]
CAPABILITIES_PER_CHANNEL_TX_POWER_ENABLED = 0x10

# MESG_SET_LP_SEARCH_TIMEOUT is supported. [stub]
CAPABILITIES_LOW_PRIORITY_SEARCH_ENABLED = 0x20

# SensRcore scripting. [rev5.1 sec 9.5.7.4 + verify:sdk-ant-shim]
CAPABILITIES_SCRIPT_ENABLED = 0x40

# Inclusion/exclusion lists (0x59, 0x5A). [rev5.1 sec 9.5.7.4 + verify:sdk-ant-
# shim]
CAPABILITIES_SEARCH_LIST_ENABLED = 0x80

# MESG_ENABLE_LED_FLASH. Reported OFF here, which is why a host that has
# ANT_EnableLED never sends 0x68. [readme]
CAPABILITIES_LED_ENABLED = 0x01

# Extended output fields - the whole lib config mechanism. [stub]
CAPABILITIES_EXT_MESSAGE_ENABLED = 0x02

# MESG_OPEN_RX_SCAN_MODE. Reported OFF in this generated reply, which is why a
# host that has ANT_OpenRxScanMode never sends 0x5B on this build. radiant
# reports the bit on instead, and as of 2026-08-10 0x5B is bridged there to
# match - see the 0x5B message row and docs/backends.md. [stub, readme]
CAPABILITIES_SCAN_MODE_ENABLED = 0x04

# MESG_PROX_SEARCH_CONFIG. [rev5.1 sec 9.5.7.4 + verify:sdk-ant-shim]
CAPABILITIES_PROX_SEARCH_ENABLED = 0x10

# The optional fourth byte of MESG_ASSIGN_CHANNEL. [stub]
CAPABILITIES_EXT_ASSIGN_ENABLED = 0x20

# ANT-FS file system. [rev5.1 sec 9.5.7.4 + verify:sdk-ant-shim]
CAPABILITIES_FS_ANTFS_ENABLED = 0x40

# FIT1 support. [rev5.1 sec 9.5.7.4 + verify:sdk-ant-shim]
CAPABILITIES_FIT1_ENABLED = 0x80

# MESG_CONFIG_ADV_BURST (0x78) and 24-byte burst packets. [stub, tools]
CAPABILITIES_ADVANCED_BURST_ENABLED = 0x01

# MESG_EVENT_BUFFERING_CONFIG (0x74). The one bit this firmware honestly
# reports as zero, because sdk-ant has no API for it. [tools, readme]
CAPABILITIES_EVENT_BUFFERING_ENABLED = 0x02

# MESG_EVENT_FILTER_CONFIG (0x79). [stub, tools]
CAPABILITIES_EVENT_FILTERING_ENABLED = 0x04

# MESG_HIGH_DUTY_SEARCH_MODE (0x77). Advertised and not bridged, on purpose -
# see the README. [tools, readme]
CAPABILITIES_HIGH_DUTY_SEARCH_ENABLED = 0x08

# MESG_ACTIVE_SEARCH_SHARING (0x81). [tools (ant_features)]
CAPABILITIES_SEARCH_SHARING_ENABLED = 0x10

# Radio coexistence configuration. No host API reaches it. [tools
# (ant_features)]
CAPABILITIES_RADIO_COEX_CONFIG_ENABLED = 0x20

# MESG_SDU_CONFIG / MESG_SDU_SET_MASK (0x7A, 0x7B). [stub, tools]
CAPABILITIES_SELECTIVE_DATA_UPDATE_ENABLED = 0x40

# Single-channel encryption (0x7D-0x7F). Advertised whether or not the write
# side was compiled in: the bit describes what the radio layer can do, not what
# the bridge chose to expose. [stub, tools]
CAPABILITIES_ENCRYPTED_CHANNEL_ENABLED = 0x80

# RF-active notification: an event when the time to the next synchronous RF
# activity exceeds a configured threshold. Set in the observed reply. [sdk-ant-
# shim + verify:sdk-ant-shim]
CAPABILITIES_RFACTIVE_NOTIFICATION_ENABLED = 0x01

# Data filtering. Clear in the observed reply. [sdk-ant-shim + verify:sdk-ant-
# shim]
CAPABILITIES_DATA_FILTERING_ENABLED = 0x02

# Search uplink. Set in the observed reply. [sdk-ant-shim + verify:sdk-ant-
# shim]
CAPABILITIES_SEARCH_UPLINK_ENABLED = 0x04

# Group transmitter initiation - the master-side meaning of the channel search
# timeout. Set in the observed reply. [sdk-ant-shim + verify:sdk-ant-shim]
CAPABILITIES_GROUP_TRANSMITTER_INITIATION_ENABLED = 0x08

# Extended time base: a 4-byte RTC timestamp instead of the 2-byte ANT one.
# Clear in the observed reply. [sdk-ant-shim + verify:sdk-ant-shim]
CAPABILITIES_TIME_BASE_ENABLED = 0x10

# Time sync. Clear in the observed reply. [sdk-ant-shim + verify:sdk-ant-shim]
CAPABILITIES_TIME_SYNC_ENABLED = 0x20

# External PA/LNA GPIO control during radio events. Clear in the observed
# reply. [sdk-ant-shim + verify:sdk-ant-shim]
CAPABILITIES_PA_LNA_SUPPORT_ENABLED = 0x40

# Channel start offset - the second parameter of
# antr_channel_open_with_offset(). Set in the observed reply, which is the
# capability behind the offset form existing at all. [sdk-ant-shim +
# verify:sdk-ant-shim]
CAPABILITIES_CHANNEL_START_OFFSET_ENABLED = 0x01

# Background searching channel uplink. Set in the observed reply. [sdk-ant-shim
# + verify:sdk-ant-shim]
CAPABILITIES_SEARCHING_CHANNEL_UPLINK_ENABLED = 0x02

# ID-match fix for a shared-channel RX burst transfer. Set in the observed
# reply. [sdk-ant-shim + verify:sdk-ant-shim]
CAPABILITIES_ID_MATCH_FIX_ON_SHARED_CHANNEL_RXBURST = 0x04

# ---------------------------------------------------------------------------
# RadiANT extension messages - NOT ANT protocol
# Ours, not Garmin's: not in Rev 5.1, not answered by any ANT device. 0xF6-0xFA
# is reserved for the rest of the family. Kept out of MESSAGES on purpose, so a
# tool that walks the ANT protocol never sees them. These were first proposed
# at 0xE0-0xE4 and moved, because sdk-ant's MESG_EXT_ID_0 .. MESG_EXT_ID_4
# occupy exactly that range. Lib config 0xE0 (LIB_CONFIG_ALL_EXT_FIELDS) is a
# third, unrelated namespace and has not moved.
# ---------------------------------------------------------------------------

# Configure the two payload transforms on one channel. [0] channel; [1] switch
# bitmask - bit 0 X_CONF, bit 1 X_AUTH, bit 2 drop an unverified window instead
# of delivering it (default 0, deliver), bit 3 encrypt the descriptor set
# (REFUSED in v1 with ANTW_INVALID_PARAMETER_PROVIDED - the descriptor has no
# counter and therefore no nonce), bits 7..4 reserved must be 0; [2] MAC window
# W, the literal 2, 4 or 8 (W=1 is reserved for the reliable-command page); [3]
# secured page range low and [4] high, both bounded to 0x01..0x1F so the
# descriptor and the ANT+ common pages stay in the clear mechanically rather
# than by memory. The two switches are independent, not a ladder: X_AUTH alone
# is the most useful setting in the table. [K4 (docs/radiant-security.md sec 3
# and 9)]
MESG_RADIANT_SEC_CONFIG_ID = 0xF1

# Install the one 16-byte root key for a channel. [0] channel; [1] key length
# in bits, 128 and nothing else in v1; [2..17] the key. Everything else -
# K_enc, K_auth, K_id, K_cmd - is derived from it, so a pairing moves exactly
# sixteen bytes. WRITE ONLY: there is no read arm anywhere, and MESG_REQUEST
# for 0xF2 answers ANTW_INVALID_MESSAGE rather than a key. [K4 (docs/radiant-
# security.md sec 3.4 and 9)]
MESG_RADIANT_SET_KEY_ID = 0xF2

# Set or read the epoch and its time anchor. [0] channel; [1] flags, bit 0 =
# the epoch is coarse real time (minutes since the RadiANT date) rather than a
# bare ordinal; [2..5] epoch (u32 LE); [6..13] microseconds into that epoch
# (u64 LE), which is the phase a receiver derives the packet counter from
# rather than from arrival history. REFUSES an epoch less than or equal to the
# current one, and refuses epochs near 0xFFFFFFFF so a counter wrap always has
# headroom. No transform enables until this has been set after a reset: a
# reboot that restarts the counter under an unchanged epoch is a two-time pad
# for X_CONF and a full session replay against X_AUTH. [K4 (docs/radiant-
# security.md sec 3.5 and 9)]
MESG_RADIANT_EPOCH_ID = 0xF3

# Per-channel security state, requested with MESG_REQUEST (0x4D). [0] channel;
# [1] switches, as in 0xF1; [2] W; [3] page range low; [4] high; [5..8] epoch
# (u32 LE); [9..10] expected packet index (u16 LE); [11..12] windows verified;
# [13..14] windows unverified; [15..16] frames dropped for a secured-range
# page; [17..18] frames dropped as replay/time-inconsistent; [19..20] windows
# dropped by deliver policy; [21] epoch advances since key install; [22] most
# recent verdict (0 clear, 1 verified, 2 unverified). Counters are u16 LE,
# saturating. Gives a host that ignores the per-message verdict flag an
# auditable stream. [K4 (docs/radiant-security.md sec 3.2 and 9)]
MESG_RADIANT_SEC_STATUS_ID = 0xF4

# Drive the in-the-clear pairing exchange. [0] channel; [1] sub-command - 0x00
# leave pairing mode, 0x01 enter it with a timeout in seconds at [2] (0 = 60 s
# default), 0x02 supply the host's 32-byte X25519 scalar at [2..33], 0x03
# begin the exchange. Reply echoes the sub-command at [1], carries the local
# public key or comparison fingerprint from [3]. The scalar comes from the
# host because the only entropy source on nRF54L is psa_rng/CRACEN and
# reaching it drags nrf_security into every build - so a host-less node cannot
# pair this way. [K4 (docs/radiant-security.md sec 7.4, 8 and 9)]
MESG_RADIANT_PAIRING_ID = 0xF5

# ---------------------------------------------------------------------------
# Lookup tables
# Built here rather than in each tool, so a name printed by ant_probe.py and a
# name printed by ant_verify.py cannot disagree.
# ---------------------------------------------------------------------------

# Every message the protocol defines, keyed by id.
#
# `payload_len` is the command - the h2d direction. `reply_len` is present only
# where a request's reply is a different shape from the command sharing its id,
# and a reader that has a record's direction in hand must consult it rather
# than payload_len for a d2h record. Absent means the reply, if there is one,
# has the command's shape.
MESSAGES = {
    0x00: {
        "name": 'MESG_INVALID_ID',
        "direction": 'marker',
        "payload_len": 0,
        "bridged": False,
        "desc": 'Reserved: never a valid message id.',
    },
    0x01: {
        "name": 'MESG_EVENT_ID',
        "direction": 'marker',
        "payload_len": 0,
        "bridged": True,
        "desc": 'Not a frame. Byte 1 of a RESPONSE_EVENT payload is 0x01 when the message is an unsolicited channel event rather than a reply to a command; the event code is byte 2.',
    },
    0x3E: {
        "name": 'MESG_VERSION_ID',
        "direction": 'd2h',
        "payload_len": 20,
        "bridged": True,
        "desc": 'Null-padded ASCII version string, MESG_VERSION_SIZE bytes. Requested with MESG_REQUEST. A stub build answers STUB0.01B00, which is deliberately not a real ANT version string.',
    },
    0x40: {
        "name": 'MESG_RESPONSE_EVENT_ID',
        "direction": 'd2h',
        "payload_len": 3,
        "bridged": True,
        "desc": '[channel, message id or 0x01, code]. Every command gets one, and every channel event arrives as one with 0x01 in the middle byte.',
    },
    0x41: {
        "name": 'MESG_UNASSIGN_CHANNEL_ID',
        "direction": 'h2d',
        "payload_len": 1,
        "bridged": True,
        "reply": 'MESG_RESPONSE_EVENT_ID',
        "desc": '[channel]. Refused with CHANNEL_IN_WRONG_STATE until the channel has actually closed - a close is asynchronous.',
    },
    0x42: {
        "name": 'MESG_ASSIGN_CHANNEL_ID',
        "direction": 'h2d',
        "payload_len": '3..4',
        "bridged": True,
        "reply": 'MESG_RESPONSE_EVENT_ID',
        "desc": '[channel, channel type, network, (extended assignment)]. The fourth byte is optional and defaults to 0x00.',
    },
    0x43: {
        "name": 'MESG_CHANNEL_MESG_PERIOD_ID',
        "direction": 'both',
        "payload_len": 3,
        "bridged": True,
        "desc": '[channel, period lo, period hi] in 1/32768 s counts. 8070 is the ANT+ heart-rate period, 8182 the power period.',
    },
    0x44: {
        "name": 'MESG_CHANNEL_SEARCH_TIMEOUT_ID',
        "direction": 'h2d',
        "payload_len": 2,
        "bridged": True,
        "reply": 'MESG_RESPONSE_EVENT_ID',
        "desc": '[channel, timeout] in 2.5 s increments; 0xFF means never time out.',
    },
    0x45: {
        "name": 'MESG_CHANNEL_RADIO_FREQ_ID',
        "direction": 'both',
        "payload_len": 2,
        "bridged": True,
        "desc": '[channel, offset from 2400 MHz]. ANT+ is 57, i.e. 2457 MHz.',
    },
    0x46: {
        "name": 'MESG_NETWORK_KEY_ID',
        "direction": 'h2d',
        "payload_len": 9,
        "bridged": True,
        "reply": 'MESG_RESPONSE_EVENT_ID',
        "desc": '[network number, key[8]]. The key travels host to dongle, which is why no build of this firmware needs a network-key secret.',
    },
    0x47: {
        "name": 'MESG_RADIO_TX_POWER_ID',
        "direction": 'h2d',
        "payload_len": 2,
        "bridged": True,
        "reply": 'MESG_RESPONSE_EVENT_ID',
        "desc": '[filler, level]. Device-wide transmit power - what ANT_SetTransmitPower() sends, and what Zwift calls while setting up a search. Answering it with INVALID_MESSAGE stalls the search.',
    },
    0x48: {
        "name": 'MESG_RADIO_CW_MODE_ID',
        "direction": 'h2d',
        "payload_len": 3,
        "bridged": False,
        "host_api": 'ANT_SetCWTestMode',
        "desc": 'Continuous-wave test transmit. Not bridged.',
    },
    0x49: {
        "name": 'MESG_SEARCH_WAVEFORM_ID',
        "direction": 'h2d',
        "payload_len": 3,
        "bridged": True,
        "reply": 'MESG_RESPONSE_EVENT_ID',
        "desc": '[channel, waveform lo, waveform hi]. Search duty cycle.',
    },
    0x4A: {
        "name": 'MESG_SYSTEM_RESET_ID',
        "direction": 'h2d',
        "payload_len": 1,
        "bridged": True,
        "reply": 'MESG_STARTUP_MESG_ID',
        "desc": 'Resets the ANT protocol stack, NOT the MCU. Answered with the startup message and no RESPONSE_EVENT. Rebooting here drops the device off the bus and every host session begins with this.',
    },
    0x4B: {
        "name": 'MESG_OPEN_CHANNEL_ID',
        "direction": 'h2d',
        "payload_len": 1,
        "bridged": True,
        "reply": 'MESG_RESPONSE_EVENT_ID',
        "desc": '[channel].',
    },
    0x4C: {
        "name": 'MESG_CLOSE_CHANNEL_ID',
        "direction": 'h2d',
        "payload_len": 1,
        "bridged": True,
        "reply": 'MESG_RESPONSE_EVENT_ID',
        "desc": '[channel]. Asynchronous: the channel is not free until EVENT_CHANNEL_CLOSED arrives.',
    },
    0x4D: {
        "name": 'MESG_REQUEST_ID',
        "direction": 'h2d',
        "payload_len": 2,
        "bridged": True,
        "desc": '[channel or index, requested message id]. The reply is the requested message; a refusal is a RESPONSE_EVENT naming 0x4D. For several messages the first byte is not a channel at all - it is a mask number, an encryption info type, or 0/1 for advanced burst capabilities/configuration.',
    },
    0x4E: {
        "name": 'MESG_BROADCAST_DATA_ID',
        "direction": 'both',
        "payload_len": '9..20',
        "bridged": True,
        "desc": '[channel, data[8]] outbound. Inbound it may carry the extended fields after the payload - see ext_flags.',
    },
    0x4F: {
        "name": 'MESG_ACKNOWLEDGED_DATA_ID',
        "direction": 'both',
        "payload_len": '9..20',
        "bridged": True,
        "desc": '[channel, data[8]]. The message a host sends to set trainer resistance; the outcome arrives later as EVENT_TRANSFER_TX_COMPLETED or EVENT_TRANSFER_TX_FAILED.',
    },
    0x50: {
        "name": 'MESG_BURST_DATA_ID',
        "direction": 'both',
        "payload_len": 9,
        "bridged": True,
        "desc": "[seq<<5 | channel, data[8]], bit 7 of byte 0 marks the last packet. Only bits 0-4 are the channel number, which is why 32 channels is the serial protocol's natural ceiling. No per-packet acknowledgement on success.",
    },
    0x51: {
        "name": 'MESG_CHANNEL_ID_ID',
        "direction": 'both',
        "payload_len": 5,
        "bridged": True,
        "desc": '[channel, device number lo, device number hi, device type, transmission type]. All-zero is the wildcard that matches anything.',
    },
    0x52: {
        "name": 'MESG_CHANNEL_STATUS_ID',
        "direction": 'd2h',
        "payload_len": 2,
        "bridged": True,
        "desc": '[channel, status]. Requested with MESG_REQUEST.',
    },
    0x53: {
        "name": 'MESG_RADIO_CW_INIT_ID',
        "direction": 'h2d',
        "payload_len": 0,
        "bridged": False,
        "host_api": 'ANT_InitCWTestMode',
        "desc": 'Enter continuous-wave test mode. Not bridged.',
    },
    0x54: {
        "name": 'MESG_CAPABILITIES_ID',
        "direction": 'd2h',
        "payload_len": 9,
        "bridged": True,
        "desc": 'What the ANT *stack* can do - a superset of what the bridge implements. Requested with MESG_REQUEST. See the capabilities section for the byte and bit layout, and for the exact reply this firmware gives.',
    },
    0x58: {
        "name": 'MESG_CHANNEL_CRC_MODE_ID',
        "direction": 'both',
        "payload_len": 2,
        "bridged": True,
        "reply": 'MESG_RESPONSE_EVENT_ID',
        "desc": '[filler, mode] on the way in, [channel, mode] on the way back. Dispatched at src/ant_serial_bridge.c and requested in handle_request(). A Nordic extension: not in Rev 5.1, and no witness inside this repository - the id was recovered by the Wave 2 shim and is BUILD_ASSERTed in src/ant_radio_sdk_ant.c.',
    },
    0x59: {
        "name": 'MESG_ID_LIST_ADD_ID',
        "direction": 'h2d',
        "payload_len": 6,
        "bridged": True,
        "reply": 'MESG_RESPONSE_EVENT_ID',
        "desc": '[channel, device number lo, device number hi, device type, transmission type, list index].',
    },
    0x5A: {
        "name": 'MESG_ID_LIST_CONFIG_ID',
        "direction": 'h2d',
        "payload_len": 3,
        "bridged": True,
        "reply": 'MESG_RESPONSE_EVENT_ID',
        "desc": '[channel, list size, include/exclude flag].',
    },
    0x5B: {
        "name": 'MESG_OPEN_RX_SCAN_MODE_ID',
        "direction": 'h2d',
        "payload_len": '1..2',
        "bridged": True,
        "reply": 'MESG_RESPONSE_EVENT_ID',
        "desc": "Background scanning. Bridged as of 2026-08-10: refuses with ANTW_CLOSE_ALL_CHANNELS unless every channel is closed, then takes over channel 0 as a wildcard background-scan slave - the same mechanism MESG_ASSIGN_CHANNEL's extended byte already reaches, one message at a time. The optional synchronous-channel-packets-only byte is accepted and ignored; reporting everything is a superset of the restricted subset. radiant backend only - the capabilities reply advertises this on radiant and OFF elsewhere, which is why a host with the call (ANT_OpenRxScanMode) sends it only there.",
    },
    0x5D: {
        "name": 'MESG_EXT_BROADCAST_DATA_ID',
        "direction": 'd2h',
        "payload_len": 13,
        "bridged": False,
        "host_api": 'ANT_SendExtBroadcastData',
        "desc": "Legacy extended broadcast: channel id inline instead of behind a flag byte. Superseded by MESG_ANTLIB_CONFIG's flag mechanism; this dongle never emits it.",
    },
    0x5E: {
        "name": 'MESG_EXT_ACKNOWLEDGED_DATA_ID',
        "direction": 'both',
        "payload_len": 13,
        "bridged": False,
        "host_api": 'ANT_SendExtAcknowledgedData',
        "desc": 'Legacy extended acknowledged data. See MESG_EXT_BROADCAST_DATA_ID.',
    },
    0x5F: {
        "name": 'MESG_EXT_BURST_DATA_ID',
        "direction": 'both',
        "payload_len": 13,
        "bridged": False,
        "host_api": 'ANT_SendExtBurstTransfer',
        "desc": 'Legacy extended burst data. See MESG_EXT_BROADCAST_DATA_ID.',
    },
    0x60: {
        "name": 'MESG_CHANNEL_RADIO_TX_POWER_ID',
        "direction": 'h2d',
        "payload_len": 2,
        "bridged": True,
        "reply": 'MESG_RESPONSE_EVENT_ID',
        "desc": "[channel, level] or [channel, level, custom]. Per-channel transmit power. Only takes on an assigned channel, so the bridge holds the requested level and reapplies it after each successful assign. The third byte is an extension of this firmware's and is read only when the level carries RADIO_TX_POWER_LVL_CUSTOM: that level names a raw radio register value and there is nowhere else on the wire to put one, so without it the custom level silently means register 0. No retail stick sends three bytes and none needs to - a two-byte message means exactly what it always did.",
    },
    0x61: {
        "name": 'MESG_GET_SERIAL_NUM_ID',
        "direction": 'h2d',
        "payload_len": 0,
        "bridged": False,
        "desc": 'Read the device serial number. Not bridged.',
    },
    0x63: {
        "name": 'MESG_SET_LP_SEARCH_TIMEOUT_ID',
        "direction": 'h2d',
        "payload_len": 2,
        "bridged": True,
        "reply": 'MESG_RESPONSE_EVENT_ID',
        "desc": '[channel, timeout]. Low-priority search timeout, in 2.5 s increments.',
    },
    0x65: {
        "name": 'MESG_SERIAL_NUM_SET_CHANNEL_ID_ID',
        "direction": 'h2d',
        "payload_len": 3,
        "bridged": False,
        "host_api": 'ANT_SetSerialNumChannelId',
        "desc": "Set the channel's device number from the device's own serial number. Implied by the ANT_DLL export ANT_SetSerialNumChannelId, which the archive's export table left unmapped. Not bridged. Both neighbours in the id sequence are confirmed by that same export table (0x63 ANT_SetLowPriorityChannelSearchTimeout, 0x66 ANT_RxExtMesgsEnable), which is the corroboration this value rests on.",
    },
    0x66: {
        "name": 'MESG_RX_EXT_MESGS_ENABLE_ID',
        "direction": 'h2d',
        "payload_len": 2,
        "bridged": True,
        "reply": 'MESG_RESPONSE_EVENT_ID',
        "desc": '[filler, enable]. The older, narrower way of asking for the channel id on every received message; the bridge maps it onto lib config bit MESG_OUT_INC_DEVICE_ID. Hosts still send it instead of 0x6E, and without it every broadcast arrives anonymous.',
    },
    0x68: {
        "name": 'MESG_ENABLE_LED_FLASH_ID',
        "direction": 'h2d',
        "payload_len": 2,
        "bridged": False,
        "desc": 'Not bridged, and the LED capability bit is reported OFF - which is why a host that has ANT_EnableLED never sends it.',
    },
    0x6D: {
        "name": 'MESG_XTAL_ENABLE_ID',
        "direction": 'h2d',
        "payload_len": 1,
        "bridged": False,
        "host_api": 'ANT_CrystalEnable',
        "desc": "Force the crystal oscillator on. Implied by the ANT_DLL export ANT_CrystalEnable, which Zwift resolves and the archive's export table left unmapped. Not bridged. The adjacent 0x6E is confirmed by that export table (ANT_LibConfigCustom), which is the corroboration this value rests on.",
    },
    0x6E: {
        "name": 'MESG_ANTLIB_CONFIG_ID',
        "direction": 'both',
        "payload_len": 2,
        "bridged": True,
        "desc": '[filler, config bits]. Turns the extended output fields on. Zero means clear everything - there is no separate clear message on the wire. 0xE0 is channel id + RSSI + RX timestamp; 0x80 is device id only.',
    },
    0x6F: {
        "name": 'MESG_STARTUP_MESG_ID',
        "direction": 'd2h',
        "payload_len": 1,
        "bridged": True,
        "desc": '[reason]. Sent once after the stack has reset. A host that does not see this concludes the dongle is dead.',
    },
    0x70: {
        "name": 'MESG_AUTO_FREQ_CONFIG_ID',
        "direction": 'h2d',
        "payload_len": 4,
        "bridged": True,
        "reply": 'MESG_RESPONSE_EVENT_ID',
        "desc": '[channel, freq0, freq1, freq2]. Frequency-agility hop table.',
    },
    0x71: {
        "name": 'MESG_PROX_SEARCH_CONFIG_ID',
        "direction": 'h2d',
        "payload_len": '2..3',
        "bridged": True,
        "reply": 'MESG_RESPONSE_EVENT_ID',
        "desc": '[channel, threshold, (custom threshold)]. Only pair with something close by.',
    },
    0x72: {
        "name": 'MESG_ADV_BURST_DATA_ID',
        "direction": 'both',
        "payload_len": '9..25',
        "bridged": True,
        "desc": 'Same header byte as MESG_BURST_DATA, but 8, 16 or 24 data bytes. Accepting this while never bridging 0x78 gives a dongle that takes 24-byte packets and can never send one.',
    },
    0x74: {
        "name": 'MESG_EVENT_BUFFERING_CONFIG_ID',
        "direction": 'h2d',
        "payload_len": 6,
        "bridged": False,
        "desc": 'Not bridged - sdk-ant exposes no API for it - and correspondingly NOT advertised in the capabilities reply. The one advanced-options-3 bit that is honestly zero.',
    },
    0x75: {
        "name": 'MESG_SET_SEARCH_CH_PRIORITY_ID',
        "direction": 'both',
        "payload_len": 2,
        "bridged": True,
        "desc": '[channel, priority].',
    },
    0x77: {
        "name": 'MESG_HIGH_DUTY_SEARCH_MODE_ID',
        "direction": 'h2d',
        "payload_len": 2,
        "bridged": False,
        "desc": 'Advertised in the capabilities reply and deliberately not bridged: sdk-ant has no single-chip API for it. Clearing the bit instead would make this dongle report something no ANTUSB-m reports.',
    },
    0x78: {
        "name": 'MESG_CONFIG_ADV_BURST_ID',
        "direction": 'both',
        "payload_len": '9..12',
        "bridged": True,
        "reply_len": {
            "selector": 'request_index',
            "desc": 'src/ant_serial_bridge.c picks the reply length from byte 0 of the MESG_REQUEST - `ch ? ..._REQ_CONFIG_SIZE : ..._REQ_CAPABILITIES_SIZE` - and neither reply repeats that byte, so nothing in the reply says which of the two it is. Pairing it with the request that drew it is the only way to know, and that is a fact about this message rather than a limitation of any reader.',
            "shapes": (
                {"when": 0x00, "len": 4, "size": 'MESG_CONFIG_ADV_BURST_REQ_CAPABILITIES_SIZE',
                 "desc": 'Capabilities: [max packet size code, supported modes, 0, 0]. Observed a4047803030000d8 - 24-byte packets, frequency hopping.'},
                {"when": 0x01, "len": 10, "size": 'MESG_CONFIG_ADV_BURST_REQ_CONFIG_SIZE',
                 "desc": 'Configuration: the 11-byte command minus the enable byte.'},
            ),
        },
        "desc": '[filler, enable, rf payload size, required modes, 0, 0, optional modes, 0, 0, (stall lo, stall hi, retry extension)]. Requesting it with index 0 returns capabilities, with index 1 the current configuration - and the reply drops the enable byte the command carried.',
    },
    0x79: {
        "name": 'MESG_EVENT_FILTER_CONFIG_ID',
        "direction": 'both',
        "payload_len": '2..3',
        "bridged": True,
        "reply_len": {
            "selector": 'none',
            "desc": 'One shape, one byte longer than the command. The `2..3` on payload_len above is the union of the two and predates this field; it is left as it stands because tools/ant_conformance.py derives its malformed-short and malformed-long cases from those bounds, so narrowing it would change the generated case set and the committed Tier 1 reference transcript would stop being reproducible. The reply side is exact here regardless, which is where the asymmetry actually shows.',
            "shapes": (
                {"when": None, "len": 3, "size": 'MESG_EVENT_FILTER_CONFIG_REQ_SIZE',
                 "desc": '[filler, filter lo, filter hi].'},
            ),
        },
        "desc": 'Command is [filter lo, filter hi] with no channel byte; the reply is [filler, filter lo, filter hi]. That asymmetry is real and is what the round-trip check in ant_features.py exists to catch.',
    },
    0x7A: {
        "name": 'MESG_SDU_CONFIG_ID',
        "direction": 'h2d',
        "payload_len": 2,
        "bridged": True,
        "reply": 'MESG_RESPONSE_EVENT_ID',
        "desc": '[channel, mask config]. Binds one of the masks to a channel; INVALID_SDU_MASK (0xFF) turns selective updates back off.',
    },
    0x7B: {
        "name": 'MESG_SDU_SET_MASK_ID',
        "direction": 'both',
        "payload_len": 9,
        "bridged": True,
        "desc": '[mask index, mask[8]]. byte 0 is a mask number, not a channel. The reply has the same shape, which is why its size is ANT_MAX_PAYLOAD_SIZE + CHANNEL_NUM_SIZE.',
    },
    0x7C: {
        "name": 'MESG_USER_CONFIG_PAGE_ID',
        "direction": 'h2d',
        "payload_len": 'var',
        "bridged": False,
        "host_api": 'ANT_ConfigUserNVM',
        "desc": "User NVM configuration page. No sdk-ant API; not bridged. Implied by the ANT_DLL export ANT_ConfigUserNVM, which the archive's export table left unmapped. Both neighbours are confirmed by that table (0x7B ANT_SetSelectiveDataUpdateMask, 0x7D-0x7F encryption), which is the corroboration this value rests on.",
    },
    0x7D: {
        "name": 'MESG_ENCRYPT_ENABLE_ID',
        "direction": 'both',
        "payload_len": 4,
        "bridged": True,
        "reply_len": {
            "selector": 'reply_byte_0',
            "desc": 'Three reply shapes behind one id, and the reply says which it is: the ENCRYPTION_INFO_GET_* type is echoed at byte 0, so a reader with no memory of the request can still check the length exactly. An index that is not one of these three is refused with a RESPONSE_EVENT naming 0x4D rather than answered with a 0x7D - the reply-size switch has no default that could send a zero-length frame, which is what case 4d-request-encrypt-enable-3 pins.',
            "shapes": (
                {"when": 0x00, "len": 2, "size": 'MESG_CONFIG_ENCRYPT_REQ_CAPABILITIES_SIZE',
                 "desc": '[info type, supported mode].'},
                {"when": 0x01, "len": 5, "size": 'MESG_CONFIG_ENCRYPT_REQ_CONFIG_ID_SIZE',
                 "desc": '[info type, crypto id[4]].'},
                {"when": 0x02, "len": 20, "size": 'MESG_CONFIG_ENCRYPT_REQ_CONFIG_USER_DATA_SIZE',
                 "desc": '[info type, custom user data[19]] - the longest frame body the parser ever sees.'},
            ),
        },
        "desc": 'Write [channel, mode, key number, decimation rate] - compiled out unless CONFIG_ANT_DONGLE_ENCRYPTION. The read side is always bridged: requesting it with an ENCRYPTION_INFO_GET_* index returns [info type, data...], the info type echoed at byte 0.',
    },
    0x7E: {
        "name": 'MESG_SET_ENCRYPT_KEY_ID',
        "direction": 'h2d',
        "payload_len": 17,
        "bridged": True,
        "reply": 'MESG_RESPONSE_EVENT_ID',
        "desc": '[key number, key[16]]. That the key is 128 bits is stated in prose only; no size constant names it.',
    },
    0x7F: {
        "name": 'MESG_SET_ENCRYPT_INFO_ID',
        "direction": 'h2d',
        "payload_len": '2..20',
        "bridged": True,
        "reply": 'MESG_RESPONSE_EVENT_ID',
        "desc": '[info type, data...]. How much data depends on the type, and the length must be checked against it here - the stack reads a fixed count per type with nothing to bound it. RNG seed is refused on purpose: no size for it is documented anywhere.',
    },
    0x81: {
        "name": 'MESG_ACTIVE_SEARCH_SHARING_ID',
        "direction": 'both',
        "payload_len": 2,
        "bridged": True,
        "desc": '[channel, cycles].',
    },
    0x89: {
        "name": 'MESG_ECS_ENABLE_ID',
        "direction": 'h2d',
        "payload_len": 2,
        "bridged": True,
        "reply": 'MESG_RESPONSE_EVENT_ID',
        "desc": '[filler, enable]. Enhanced channel spacing. A Nordic extension: not in Rev 5.1, and no witness inside this repository - the id was recovered by the Wave 2 shim and is BUILD_ASSERTed in src/ant_radio_sdk_ant.c.',
    },
    0x8C: {
        "name": 'MESG_PENDING_TRANSMIT_CLEAR_ID',
        "direction": 'both',
        "payload_len": 2,
        "bridged": True,
        "reply": 'MESG_RESPONSE_EVENT_ID',
        "desc": "[channel] to clear, [channel, pending] on request. A Nordic extension: not in Rev 5.1, and no witness inside this repository - the id was recovered by the Wave 2 shim and is BUILD_ASSERTed in src/ant_radio_sdk_ant.c. Note that the *request* form of this id replies with MESG_PENDING_TRANSMIT_GET_SIZE bytes, which is a separate constant from MESG_PENDING_TRANSMIT_CLEAR's own 1-byte command payload.",
    },
    0xAE: {
        "name": 'MESG_SERIAL_ERROR_ID',
        "direction": 'd2h',
        "payload_len": 1,
        "bridged": False,
        "desc": 'Serial-layer error: bad checksum, bad length, or a frame that arrived too slowly. This firmware drops malformed frames silently instead - recorded so a host-side parser can name the byte.',
    },
}

# RadiANT extensions, kept out of MESSAGES on purpose.
RADIANT_MESSAGES = {
    0xF1: {
        "name": 'MESG_RADIANT_SEC_CONFIG_ID',
        "direction": 'h2d',
        "payload_len": 5,
        "desc": 'Configure the two payload transforms on one channel. [0] channel; [1] switch bitmask - bit 0 X_CONF, bit 1 X_AUTH, bit 2 drop an unverified window instead of delivering it (default 0, deliver), bit 3 encrypt the descriptor set (REFUSED in v1 with ANTW_INVALID_PARAMETER_PROVIDED - the descriptor has no counter and therefore no nonce), bits 7..4 reserved must be 0; [2] MAC window W, the literal 2, 4 or 8 (W=1 is reserved for the reliable-command page); [3] secured page range low and [4] high, both bounded to 0x01..0x1F so the descriptor and the ANT+ common pages stay in the clear mechanically rather than by memory. The two switches are independent, not a ladder: X_AUTH alone is the most useful setting in the table.',
    },
    0xF2: {
        "name": 'MESG_RADIANT_SET_KEY_ID',
        "direction": 'h2d',
        "payload_len": 18,
        "desc": 'Install the one 16-byte root key for a channel. [0] channel; [1] key length in bits, 128 and nothing else in v1; [2..17] the key. Everything else - K_enc, K_auth, K_id, K_cmd - is derived from it, so a pairing moves exactly sixteen bytes. WRITE ONLY: there is no read arm anywhere, and MESG_REQUEST for 0xF2 answers ANTW_INVALID_MESSAGE rather than a key.',
    },
    0xF3: {
        "name": 'MESG_RADIANT_EPOCH_ID',
        "direction": 'both',
        "payload_len": 14,
        "desc": 'Set or read the epoch and its time anchor. [0] channel; [1] flags, bit 0 = the epoch is coarse real time (minutes since the RadiANT date) rather than a bare ordinal; [2..5] epoch (u32 LE); [6..13] microseconds into that epoch (u64 LE), which is the phase a receiver derives the packet counter from rather than from arrival history. REFUSES an epoch less than or equal to the current one, and refuses epochs near 0xFFFFFFFF so a counter wrap always has headroom. No transform enables until this has been set after a reset: a reboot that restarts the counter under an unchanged epoch is a two-time pad for X_CONF and a full session replay against X_AUTH.',
    },
    0xF4: {
        "name": 'MESG_RADIANT_SEC_STATUS_ID',
        "direction": 'd2h',
        "payload_len": 23,
        "desc": 'Per-channel security state, requested with MESG_REQUEST (0x4D). [0] channel; [1] switches currently active, as in 0xF1; [2] W; [3] page range low; [4] high; [5..8] epoch (u32 LE); [9..10] the expected packet index, low 16 bits (u16 LE); [11..12] windows verified; [13..14] windows unverified; [15..16] non-broadcast frames dropped for carrying a secured-range page; [17..18] frames dropped as replay or time-inconsistent; [19..20] windows dropped by the deliver policy; [21] epoch advances since the key was installed; [22] the most recent verdict - 0 clear, 1 verified, 2 unverified. All counters are u16 LE and saturate rather than wrapping. This exists so a host that ignores the per-message verdict flag still has an auditable stream: deliver-as-unverified only means something if unverified cannot be silently treated as verified.',
    },
    0xF5: {
        "name": 'MESG_RADIANT_PAIRING_ID',
        "direction": 'both',
        "payload_len": '2..34',
        "desc": "Drive the in-the-clear pairing exchange. [0] channel; [1] sub-command - 0x00 leave pairing mode, 0x01 enter it with a timeout in seconds at [2] (0 means the 60 s default), 0x02 supply the host's 32-byte X25519 scalar at [2..33], 0x03 begin the exchange. The reply echoes the sub-command at [1] and carries the local public key or the comparison fingerprint from [3]. THE SCALAR COMES FROM THE HOST because the only entropy source on nRF54L is psa_rng/CRACEN and reaching it drags nrf_security into every build; the honest consequence is that a host-less node cannot pair this way. Pairing in the clear is structural rather than an oversight - see docs/radiant-security.md section 7.4.",
    },
}
RADIANT_RESERVED_RANGE = '0xF6-0xFA'

MESSAGE_NAMES = {mid: info["name"] for mid, info in MESSAGES.items()}
MESSAGE_IDS = {info["name"]: mid for mid, info in MESSAGES.items()}

# Message ids the bridge's dispatch() implements today.
BRIDGED_MESSAGE_IDS = frozenset(
    mid for mid, info in MESSAGES.items() if info["bridged"]
)

CHANNEL_TYPES = {
    'CHANNEL_TYPE_SLAVE': 0x00,
    'CHANNEL_TYPE_MASTER': 0x10,
    'CHANNEL_TYPE_SHARED_SLAVE': 0x20,
    'CHANNEL_TYPE_SHARED_MASTER': 0x30,
    'CHANNEL_TYPE_SLAVE_RX_ONLY': 0x40,
    'CHANNEL_TYPE_MASTER_TX_ONLY': 0x50,
}
CHANNEL_TYPES_BY_VALUE = {value: name for name, value in CHANNEL_TYPES.items()}

EXT_ASSIGN = {
    'EXT_PARAM_ALWAYS_SEARCH': 0x01,
    'EXT_PARAM_FREQUENCY_AGILITY': 0x04,
    'EXT_PARAM_AUTO_SHARED_SLAVE': 0x08,
    'EXT_PARAM_FAST_INITIATION_MODE': 0x10,
    'EXT_PARAM_ASYNC_TX_MODE': 0x20,
}
EXT_ASSIGN_BY_VALUE = {value: name for name, value in EXT_ASSIGN.items()}

CHANNEL_STATUS = {
    'STATUS_UNASSIGNED_CHANNEL': 0x00,
    'STATUS_ASSIGNED_CHANNEL': 0x01,
    'STATUS_SEARCHING_CHANNEL': 0x02,
    'STATUS_TRACKING_CHANNEL': 0x03,
    'STATUS_CHANNEL_STATE_MASK': 0x03,
}
# No CHANNEL_STATUS_BY_VALUE: two names in this group share a value on purpose,
# and a reverse lookup would silently pick one.

RADIO_TX_POWER = {
    'RADIO_TX_POWER_LVL_0': 0x00,
    'RADIO_TX_POWER_LVL_1': 0x01,
    'RADIO_TX_POWER_LVL_2': 0x02,
    'RADIO_TX_POWER_LVL_3': 0x03,
    'RADIO_TX_POWER_LVL_4': 0x04,
    'RADIO_TX_POWER_LVL_5': 0x05,
    'RADIO_TX_POWER_LVL_CUSTOM': 0x80,
}
RADIO_TX_POWER_BY_VALUE = {value: name for name, value in RADIO_TX_POWER.items()}

LIB_CONFIG = {
    'LIB_CONFIG_RADIO_CONFIG_ALWAYS': 0x01,
    'LIB_CONFIG_MESG_OUT_INC_TIME_STAMP': 0x20,
    'LIB_CONFIG_MESG_OUT_INC_RSSI': 0x40,
    'LIB_CONFIG_MESG_OUT_INC_DEVICE_ID': 0x80,
    'LIB_CONFIG_DEVICE_ID_ONLY': 0x80,
    'LIB_CONFIG_ALL_EXT_FIELDS': 0xE0,
    'LIB_CONFIG_MASK_ALL': 0xFF,
}
# No LIB_CONFIG_BY_VALUE: two names in this group share a value on purpose, and
# a reverse lookup would silently pick one.

EXT_FLAGS = {
    'EXT_FLAG_CHANNEL_ID': 0x80,
    'EXT_FLAG_RSSI': 0x40,
    'EXT_FLAG_RX_TIMESTAMP': 0x20,
}
EXT_FLAGS_BY_VALUE = {value: name for name, value in EXT_FLAGS.items()}

RSSI = {
    'RSSI_MEASUREMENT_TYPE_DBM': 0x20,
}
RSSI_BY_VALUE = {value: name for name, value in RSSI.items()}

STARTUP_REASONS = {
    'STARTUP_POWER_ON_RESET': 0x00,
    'STARTUP_HARDWARE_RESET_LINE': 0x01,
    'STARTUP_WATCH_DOG_RESET': 0x02,
    'STARTUP_COMMAND_RESET': 0x20,
    'STARTUP_SYNCHRONOUS_RESET': 0x40,
    'STARTUP_SUSPEND_RESET': 0x80,
}
STARTUP_REASONS_BY_VALUE = {value: name for name, value in STARTUP_REASONS.items()}

EVENT_CODES = {
    'EVENT_RX_SEARCH_TIMEOUT': 0x01,
    'EVENT_RX_FAIL': 0x02,
    'EVENT_TX': 0x03,
    'EVENT_TRANSFER_RX_FAILED': 0x04,
    'EVENT_TRANSFER_TX_COMPLETED': 0x05,
    'EVENT_TRANSFER_TX_FAILED': 0x06,
    'EVENT_CHANNEL_CLOSED': 0x07,
    'EVENT_RX_FAIL_GO_TO_SEARCH': 0x08,
    'EVENT_CHANNEL_COLLISION': 0x09,
    'EVENT_TRANSFER_TX_START': 0x0A,
    'EVENT_RX_DATA_OVERFLOW': 0x0B,
    'EVENT_TRANSFER_NEXT_DATA_BLOCK': 0x11,
}
EVENT_CODES_BY_VALUE = {value: name for name, value in EVENT_CODES.items()}

RESPONSE_CODES = {
    'RESPONSE_NO_ERROR': 0x00,
    'CHANNEL_IN_WRONG_STATE': 0x15,
    'CHANNEL_NOT_OPENED': 0x16,
    'CHANNEL_ID_NOT_SET': 0x18,
    'CLOSE_ALL_CHANNELS': 0x19,
    'TRANSFER_IN_PROGRESS': 0x1F,
    'TRANSFER_SEQUENCE_NUMBER_ERROR': 0x20,
    'TRANSFER_IN_ERROR': 0x21,
    'TRANSFER_BUSY': 0x22,
    'MESSAGE_SIZE_EXCEEDS_LIMIT': 0x27,
    'INVALID_MESSAGE': 0x28,
    'INVALID_NETWORK_NUMBER': 0x29,
    'INVALID_LIST_ID': 0x30,
    'INVALID_SCAN_TX_CHANNEL': 0x31,
    'INVALID_PARAMETER_PROVIDED': 0x33,
    'EVENT_SERIAL_QUE_OVERFLOW': 0x34,
    'EVENT_QUE_OVERFLOW': 0x35,
}
RESPONSE_CODES_BY_VALUE = {value: name for name, value in RESPONSE_CODES.items()}

ADV_BURST = {
    'ADV_BURST_MODE_DISABLE': 0x00,
    'ADV_BURST_MODE_ENABLE': 0x01,
    'ADV_BURST_MODES_SIZE_8_BYTES': 0x01,
    'ADV_BURST_MODES_SIZE_16_BYTES': 0x02,
    'ADV_BURST_MODES_SIZE_24_BYTES': 0x03,
    'ADV_BURST_MODES_FREQ_HOP': 0x01,
}
# No ADV_BURST_BY_VALUE: two names in this group share a value on purpose, and
# a reverse lookup would silently pick one.

SDU = {
    'INVALID_SDU_MASK': 0xFF,
    'SDU_MASK_ACK_CONFIG_BIT': 0x80,
}
SDU_BY_VALUE = {value: name for name, value in SDU.items()}

ENCRYPTION = {
    'ENCRYPTION_DISABLED_MODE': 0x00,
    'ENCRYPTION_INFO_SET_CRYPTO_ID': 0x00,
    'ENCRYPTION_INFO_SET_CUSTOM_USER_DATA': 0x01,
    'ENCRYPTION_INFO_SET_RNG_SEED': 0x02,
    'ENCRYPTION_INFO_GET_SUPPORTED_MODE': 0x00,
    'ENCRYPTION_INFO_GET_CRYPTO_ID': 0x01,
    'ENCRYPTION_INFO_GET_CUSTOM_USER_DATA': 0x02,
    'ENCRYPTION_USER_DATA_SIZE': 19,
    'ENCRYPTION_KEY_SIZE': 16,
    'MAX_SUPPORTED_ENCRYPTION_MODE': 0x02,
}
# No ENCRYPTION_BY_VALUE: two names in this group share a value on purpose, and
# a reverse lookup would silently pick one.

BURST_HEADER = {
    'BURST_HEADER_CHANNEL_MASK': 0x1F,
    'BURST_HEADER_SEQ_SHIFT': 5,
    'BURST_HEADER_SEQ_MASK': 0x03,
    'BURST_HEADER_LAST': 0x80,
}
BURST_HEADER_BY_VALUE = {value: name for name, value in BURST_HEADER.items()}

RADIANT_PAIRING = {
    'RADIANT_PAIR_LEAVE': 0x00,
    'RADIANT_PAIR_ENTER': 0x01,
    'RADIANT_PAIR_SCALAR': 0x02,
    'RADIANT_PAIR_EXCHANGE': 0x03,
}
RADIANT_PAIRING_BY_VALUE = {value: name for name, value in RADIANT_PAIRING.items()}

# Capabilities: byte index -> (field name, ((bit, name, description), ...)).
CAPABILITY_BYTES = {
    0: ('max_channels', (
    )),
    1: ('max_networks', (
    )),
    2: ('standard_options', (
        (0x01, 'CAPABILITIES_NO_RECEIVE_CHANNELS', 'Cannot receive.'),
        (0x02, 'CAPABILITIES_NO_TRANSMIT_CHANNELS', 'Cannot transmit.'),
        (0x04, 'CAPABILITIES_NO_RECEIVE_MESSAGES', 'No receive messages.'),
        (0x08, 'CAPABILITIES_NO_TRANSMIT_MESSAGES', 'No transmit messages.'),
        (0x10, 'CAPABILITIES_NO_ACKD_MESSAGES', 'No acknowledged data.'),
        (0x20, 'CAPABILITIES_NO_BURST_MESSAGES', 'No burst.'),
    )),
    3: ('advanced_options', (
        (0x02, 'CAPABILITIES_NETWORK_ENABLED', 'MESG_NETWORK_KEY is supported.'),
        (0x08, 'CAPABILITIES_SERIAL_NUMBER_ENABLED', 'The device has a readable serial number.'),
        (0x10, 'CAPABILITIES_PER_CHANNEL_TX_POWER_ENABLED', 'MESG_CHANNEL_RADIO_TX_POWER (0x60) is supported.'),
        (0x20, 'CAPABILITIES_LOW_PRIORITY_SEARCH_ENABLED', 'MESG_SET_LP_SEARCH_TIMEOUT is supported.'),
        (0x40, 'CAPABILITIES_SCRIPT_ENABLED', 'SensRcore scripting.'),
        (0x80, 'CAPABILITIES_SEARCH_LIST_ENABLED', 'Inclusion/exclusion lists (0x59, 0x5A).'),
    )),
    4: ('advanced_options_2', (
        (0x01, 'CAPABILITIES_LED_ENABLED', 'MESG_ENABLE_LED_FLASH. Reported OFF here, which is why a host that has ANT_EnableLED never sends 0x68.'),
        (0x02, 'CAPABILITIES_EXT_MESSAGE_ENABLED', 'Extended output fields - the whole lib config mechanism.'),
        (0x04, 'CAPABILITIES_SCAN_MODE_ENABLED', 'MESG_OPEN_RX_SCAN_MODE. Reported OFF in this generated reply, which is why a host that has ANT_OpenRxScanMode never sends 0x5B on this build. radiant reports the bit on instead, and as of 2026-08-10 0x5B is bridged there to match - see the 0x5B message row and docs/backends.md.'),
        (0x10, 'CAPABILITIES_PROX_SEARCH_ENABLED', 'MESG_PROX_SEARCH_CONFIG.'),
        (0x20, 'CAPABILITIES_EXT_ASSIGN_ENABLED', 'The optional fourth byte of MESG_ASSIGN_CHANNEL.'),
        (0x40, 'CAPABILITIES_FS_ANTFS_ENABLED', 'ANT-FS file system.'),
        (0x80, 'CAPABILITIES_FIT1_ENABLED', 'FIT1 support.'),
    )),
    5: ('max_sensrcore_channels', (
    )),
    6: ('advanced_options_3', (
        (0x01, 'CAPABILITIES_ADVANCED_BURST_ENABLED', 'MESG_CONFIG_ADV_BURST (0x78) and 24-byte burst packets.'),
        (0x02, 'CAPABILITIES_EVENT_BUFFERING_ENABLED', 'MESG_EVENT_BUFFERING_CONFIG (0x74). The one bit this firmware honestly reports as zero, because sdk-ant has no API for it.'),
        (0x04, 'CAPABILITIES_EVENT_FILTERING_ENABLED', 'MESG_EVENT_FILTER_CONFIG (0x79).'),
        (0x08, 'CAPABILITIES_HIGH_DUTY_SEARCH_ENABLED', 'MESG_HIGH_DUTY_SEARCH_MODE (0x77). Advertised and not bridged, on purpose - see the README.'),
        (0x10, 'CAPABILITIES_SEARCH_SHARING_ENABLED', 'MESG_ACTIVE_SEARCH_SHARING (0x81).'),
        (0x20, 'CAPABILITIES_RADIO_COEX_CONFIG_ENABLED', 'Radio coexistence configuration. No host API reaches it.'),
        (0x40, 'CAPABILITIES_SELECTIVE_DATA_UPDATE_ENABLED', 'MESG_SDU_CONFIG / MESG_SDU_SET_MASK (0x7A, 0x7B).'),
        (0x80, 'CAPABILITIES_ENCRYPTED_CHANNEL_ENABLED', 'Single-channel encryption (0x7D-0x7F). Advertised whether or not the write side was compiled in: the bit describes what the radio layer can do, not what the bridge chose to expose.'),
    )),
    7: ('advanced_options_4', (
        (0x01, 'CAPABILITIES_RFACTIVE_NOTIFICATION_ENABLED', 'RF-active notification: an event when the time to the next synchronous RF activity exceeds a configured threshold. Set in the observed reply.'),
        (0x02, 'CAPABILITIES_DATA_FILTERING_ENABLED', 'Data filtering. Clear in the observed reply.'),
        (0x04, 'CAPABILITIES_SEARCH_UPLINK_ENABLED', 'Search uplink. Set in the observed reply.'),
        (0x08, 'CAPABILITIES_GROUP_TRANSMITTER_INITIATION_ENABLED', 'Group transmitter initiation - the master-side meaning of the channel search timeout. Set in the observed reply.'),
        (0x10, 'CAPABILITIES_TIME_BASE_ENABLED', 'Extended time base: a 4-byte RTC timestamp instead of the 2-byte ANT one. Clear in the observed reply.'),
        (0x20, 'CAPABILITIES_TIME_SYNC_ENABLED', 'Time sync. Clear in the observed reply.'),
        (0x40, 'CAPABILITIES_PA_LNA_SUPPORT_ENABLED', 'External PA/LNA GPIO control during radio events. Clear in the observed reply.'),
    )),
    8: ('advanced_options_5', (
        (0x01, 'CAPABILITIES_CHANNEL_START_OFFSET_ENABLED', 'Channel start offset - the second parameter of antr_channel_open_with_offset(). Set in the observed reply, which is the capability behind the offset form existing at all.'),
        (0x02, 'CAPABILITIES_SEARCHING_CHANNEL_UPLINK_ENABLED', 'Background searching channel uplink. Set in the observed reply.'),
        (0x04, 'CAPABILITIES_ID_MATCH_FIX_ON_SHARED_CHANNEL_RXBURST', 'ID-match fix for a shared-channel RX burst transfer. Set in the observed reply.'),
    )),
}

# The reply this firmware actually gives, for tools that want to diff it.
OBSERVED_CAPABILITIES = bytes.fromhex('080800b23200fd8d0f')

# Constants whose value is not recoverable without sdk-ant. Name -> why.
UNRESOLVED = {
    'MESG_RSSI_SEARCH_THRESHOLD_ID': 'Proximity search threshold expressed in RSSI rather than in the proximity bins MESG_PROX_SEARCH_CONFIG (0x71) uses. Implied by the ANT_DLL export ANT_RSSI_SetSearchThreshold. Believed to sit in the 0xC0 block of AP2-era extension messages, but no value in that block is corroborated by anything in this repository - the whole host-callable API in archive/host-api/ant_dll_exports.json tops out at 0x7B - so no number is recorded here. The Wave 2 shim looked and came back empty as well: sdk-ant v2.1.0 defines no such id anywhere in its headers or sources, so this stays unresolved rather than merely unverified.',
    'MESG_SLEEP_ID': 'Put the device into its low-power sleep state. Implied by the ANT_DLL export ANT_SleepMessage, which Zwift resolves. Same 0xC0 block and the same lack of corroboration as MESG_RSSI_SEARCH_THRESHOLD_ID; no number is recorded. The Wave 2 shim found the name referenced in sdk-ant but never defined - the only occurrence is inside a commented-out dispatch arm - so there is no value to recover there either.',
}

# Constants with no witness in this repository; asserted only in sdk-ant-shim.
VERIFY_IN_SHIM = (
    'SYNC_RX',
    'MESG_INVALID_ID',
    'MESG_CHANNEL_CRC_MODE_ID',
    'MESG_GET_SERIAL_NUM_ID',
    'MESG_SERIAL_NUM_SET_CHANNEL_ID_ID',
    'MESG_XTAL_ENABLE_ID',
    'MESG_SET_SEARCH_CH_PRIORITY_ID',
    'MESG_USER_CONFIG_PAGE_ID',
    'MESG_ACTIVE_SEARCH_SHARING_ID',
    'MESG_ECS_ENABLE_ID',
    'MESG_PENDING_TRANSMIT_CLEAR_ID',
    'MESG_SERIAL_ERROR_ID',
    'MESG_RSSI_SEARCH_THRESHOLD_ID',
    'MESG_SLEEP_ID',
    'MESG_VERSION_SIZE',
    'MESG_CHANNEL_STATUS_SIZE',
    'MESG_CHANNEL_CRC_MODE_SIZE',
    'MESG_SET_SEARCH_CH_PRIORITY_SIZE',
    'MESG_PENDING_TRANSMIT_GET_SIZE',
    'MESG_ACTIVE_SEARCH_SHARING_REQ_SIZE',
    'MESG_CONFIG_ADV_BURST_REQ_CAPABILITIES_SIZE',
    'CHANNEL_TYPE_SHARED_SLAVE',
    'CHANNEL_TYPE_SHARED_MASTER',
    'CHANNEL_TYPE_SLAVE_RX_ONLY',
    'CHANNEL_TYPE_MASTER_TX_ONLY',
    'EXT_PARAM_ALWAYS_SEARCH',
    'EXT_PARAM_FREQUENCY_AGILITY',
    'EXT_PARAM_AUTO_SHARED_SLAVE',
    'STATUS_ASSIGNED_CHANNEL',
    'STATUS_SEARCHING_CHANNEL',
    'STATUS_TRACKING_CHANNEL',
    'STATUS_CHANNEL_STATE_MASK',
    'LIB_CONFIG_RADIO_CONFIG_ALWAYS',
    'LIB_CONFIG_MASK_ALL',
    'EVENT_TRANSFER_NEXT_DATA_BLOCK',
    'CHANNEL_IN_WRONG_STATE',
    'CHANNEL_NOT_OPENED',
    'CHANNEL_ID_NOT_SET',
    'CLOSE_ALL_CHANNELS',
    'TRANSFER_IN_PROGRESS',
    'TRANSFER_SEQUENCE_NUMBER_ERROR',
    'TRANSFER_IN_ERROR',
    'TRANSFER_BUSY',
    'MESSAGE_SIZE_EXCEEDS_LIMIT',
    'INVALID_NETWORK_NUMBER',
    'INVALID_LIST_ID',
    'INVALID_SCAN_TX_CHANNEL',
    'EVENT_SERIAL_QUE_OVERFLOW',
    'EVENT_QUE_OVERFLOW',
    'ADV_BURST_MODES_SIZE_8_BYTES',
    'ADV_BURST_MODES_SIZE_16_BYTES',
    'SDU_MASK_ACK_CONFIG_BIT',
    'MAX_SUPPORTED_ENCRYPTION_MODE',
    'CAPABILITIES_NO_RECEIVE_CHANNELS',
    'CAPABILITIES_NO_TRANSMIT_CHANNELS',
    'CAPABILITIES_NO_RECEIVE_MESSAGES',
    'CAPABILITIES_NO_TRANSMIT_MESSAGES',
    'CAPABILITIES_NO_ACKD_MESSAGES',
    'CAPABILITIES_NO_BURST_MESSAGES',
    'CAPABILITIES_SCRIPT_ENABLED',
    'CAPABILITIES_SEARCH_LIST_ENABLED',
    'CAPABILITIES_PROX_SEARCH_ENABLED',
    'CAPABILITIES_FS_ANTFS_ENABLED',
    'CAPABILITIES_FIT1_ENABLED',
    'CAPABILITIES_RFACTIVE_NOTIFICATION_ENABLED',
    'CAPABILITIES_DATA_FILTERING_ENABLED',
    'CAPABILITIES_SEARCH_UPLINK_ENABLED',
    'CAPABILITIES_GROUP_TRANSMITTER_INITIATION_ENABLED',
    'CAPABILITIES_TIME_BASE_ENABLED',
    'CAPABILITIES_TIME_SYNC_ENABLED',
    'CAPABILITIES_PA_LNA_SUPPORT_ENABLED',
    'CAPABILITIES_CHANNEL_START_OFFSET_ENABLED',
    'CAPABILITIES_SEARCHING_CHANNEL_UPLINK_ENABLED',
    'CAPABILITIES_ID_MATCH_FIX_ON_SHARED_CHANNEL_RXBURST',
)

# ---------------------------------------------------------------------------
# The one rule that is not a constant
# ---------------------------------------------------------------------------


def checksum(data: bytes) -> int:
    """XOR of every byte handed in, SYNC included.

    Pass the whole frame except the checksum byte itself. Seeding at 0 and
    starting after SYNC - which is the natural-looking mistake - produces a
    value exactly 0xA4 off, and two implementations that both make it agree
    with each other and with nobody else. That cost this project a week.
    """
    total = 0
    for byte in data:
        total ^= byte
    return total


def frame(msg_id: int, payload: bytes = b"") -> bytes:
    """Wrap a message: SYNC, LEN, ID, payload, XOR."""
    head = bytes([SYNC_TX, len(payload), msg_id]) + bytes(payload)
    return head + bytes([checksum(head)])


def unframe(buf: bytes):
    """(msg_id, payload) for one complete frame, or None if it is not valid.

    Rejects on SYNC, on length, and on the checksum. It does not resynchronise:
    that is the caller's ring buffer's job.
    """
    if len(buf) < MSG_OVERHEAD or buf[0] != SYNC_TX:
        return None
    total = buf[1] + MSG_OVERHEAD
    if len(buf) < total or checksum(buf[: total - 1]) != buf[total - 1]:
        return None
    return buf[2], bytes(buf[3 : total - 1])
