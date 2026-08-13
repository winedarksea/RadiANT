/* SPDX-License-Identifier: Apache-2.0 */
/*
 * DO NOT EDIT - generated from protocol/ant_wire.yaml
 * by scripts/gen_ant_wire.py. Edit the YAML and re-run the generator;
 * `scripts/gen_ant_wire.py --check` fails the build if these drift apart.
 *
 * ANT serial protocol constants (host <-> dongle, 0xA4-framed).
 * Source of truth: protocol/ant_wire.yaml
 * Naming authority: ANT Message Protocol and Usage Rev 5.1 (D00000652)
 * Human-readable companion: docs/ant-serial-protocol.md
 *
 * Why every name here carries an ANTW_ prefix: sdk-ant's ant_parameters.h
 * defines its error codes as computed expressions rather than literals, so an
 * unprefixed constant of ours would be a hard macro redefinition error if it
 * ever shared a translation unit with sdk-ant's - which src/ant_radio_sdk_ant.c
 * does, on purpose, to BUILD_ASSERT every ANTW_* constant against its sdk-ant
 * counterpart. It also keeps src/ant_serial_bridge.c textually free of
 * Garmin's API names, which matters for the clean-room boundary.
 *
 * The Python module deliberately does NOT carry the prefix - Python already
 * has module namespaces, so `ant_wire.MESG_ASSIGN_CHANNEL_ID` is unambiguous.
 *
 * Nothing here describes the on-air link layer. This header is about bytes
 * on a USB bulk endpoint or a UART, and about nothing else.
 */

#ifndef ANT_WIRE_H_
#define ANT_WIRE_H_

#include <stdint.h>

/* ------------------------------------------------------------------------
 * Framing
 * The checksum is the XOR of every byte of the frame from the SYNC byte
 * through the last payload byte, inclusive. Leaving SYNC out of the sum
 * yields a value that differs by exactly 0xA4, so two implementations that
 * both omit it agree with each other perfectly and with nobody else.
 * ------------------------------------------------------------------------ */

/*
 * SYNC byte on every frame the host sends and on every frame this dongle
 * sends. The only value the parser resynchronises on. [bridge]
 */
#define ANTW_SYNC_TX                                         0xA4

/*
 * Alternate SYNC used by the bidirectional/asynchronous serial variants of
 * the protocol. Never emitted or accepted by this dongle; recorded so a
 * parser can recognise and reject it. [rev5.1 sec 7.1 + verify:sdk-ant-shim]
 */
#define ANTW_SYNC_RX                                         0xA5

/*
 * Bytes of frame around the payload: SYNC + LEN + ID + checksum. Total frame
 * length is LEN + 4. [bridge]
 */
#define ANTW_MSG_OVERHEAD                                    4

/*
 * Largest value the LEN byte may carry: 38, per src/usb_ant_class.c:30, which
 * both USB class files size their frame buffer from (42 with overhead). Not
 * 20 - the longest message the *host* sends, reasoning from the encryption
 * path - since an advanced-burst message reaches 25 on its own; sizing
 * msg_body[] from 20 would make handle_burst() silently reject every
 * 24-byte burst packet. [src/usb_ant_class.c:30]
 */
#define ANTW_MAX_SIZE_VALUE                                  38

/*
 * MAX_SIZE_VALUE minus the leading channel/index byte that almost every
 * message carries. [src/usb_ant_class.c:30, bridge]
 */
#define ANTW_MAX_DATA_SIZE                                   37

/*
 * MAX_DATA_SIZE + MSG_OVERHEAD, and a trap: it is one byte SHORT of the
 * largest frame that can legally arrive, because the LEN byte is allowed to
 * reach MAX_SIZE_VALUE, which counts the channel byte that MAX_DATA_SIZE does
 * not. A full extended-data message sized from this writes its checksum one
 * past the end of the buffer and hands the USB layer a length one over it -
 * from sdk-ant's work queue thread. Size buffers from MAX_FRAME_SIZE, never
 * from this. [bridge]
 */
#define ANTW_MESG_MAX_SIZE                                   41

/*
 * MAX_SIZE_VALUE + MSG_OVERHEAD. The correct buffer size, and the 42 both USB
 * class files independently quote. [src/usb_ant_class.c:30,
 * src/usb_ant_class_next.c:55]
 */
#define ANTW_MAX_FRAME_SIZE                                  42

/*
 * An ANT data payload: 8 bytes, on air and on the wire. Also the width of a
 * selective-data-update mask. [stub]
 */
#define ANTW_ANT_MAX_PAYLOAD_SIZE                            8

/*
 * The leading channel-number byte. Several reply sizes are written as
 * <payload> + CHANNEL_NUM_SIZE precisely because the reply leads with an
 * index that is not part of the data. [bridge]
 */
#define ANTW_CHANNEL_NUM_SIZE                                1

/*
 * Largest advanced-burst block in one serial frame. Plain burst carries 8;
 * advanced burst carries 8, 16 or 24. [bridge]
 */
#define ANTW_ADV_BURST_BLOCK_MAX                             24

/* ------------------------------------------------------------------------
 * Message identifiers
 * The ID byte of a frame. Direction, payload length and whether the bridge
 * implements each one are tabulated in docs/ant-serial-protocol.md.
 * ------------------------------------------------------------------------ */

/* Reserved: never a valid message id. [rev5.1 sec 9.3 + verify:sdk-ant-shim] */
#define ANTW_MESG_INVALID_ID                                 0x00

/*
 * Not a frame. Byte 1 of a RESPONSE_EVENT payload is 0x01 when the message is
 * an unsolicited channel event rather than a reply to a command; the event
 * code is byte 2. [bridge, tools (ant_session.EVENT_MARKER)]
 */
#define ANTW_MESG_EVENT_ID                                   0x01

/*
 * Null-padded ASCII version string, MESG_VERSION_SIZE bytes. Requested with
 * MESG_REQUEST. A stub build answers STUB0.01B00, which is deliberately not a
 * real ANT version string. [tools]
 */
#define ANTW_MESG_VERSION_ID                                 0x3E

/*
 * [channel, message id or 0x01, code]. Every command gets one, and every
 * channel event arrives as one with 0x01 in the middle byte. [bridge]
 */
#define ANTW_MESG_RESPONSE_EVENT_ID                          0x40

/*
 * [channel]. Refused with CHANNEL_IN_WRONG_STATE until the channel has
 * actually closed - a close is asynchronous. [tools]
 */
#define ANTW_MESG_UNASSIGN_CHANNEL_ID                        0x41

/*
 * [channel, channel type, network, (extended assignment)]. The fourth byte is
 * optional and defaults to 0x00. [bridge, tools]
 */
#define ANTW_MESG_ASSIGN_CHANNEL_ID                          0x42

/*
 * [channel, period lo, period hi] in 1/32768 s counts. 8070 is the ANT+
 * heart-rate period, 8182 the power period. [bridge, tools]
 */
#define ANTW_MESG_CHANNEL_MESG_PERIOD_ID                     0x43

/*
 * [channel, timeout] in 2.5 s increments; 0xFF means never time out. [bridge,
 * tools]
 */
#define ANTW_MESG_CHANNEL_SEARCH_TIMEOUT_ID                  0x44

/* [channel, offset from 2400 MHz]. ANT+ is 57, i.e. 2457 MHz. [bridge, tools] */
#define ANTW_MESG_CHANNEL_RADIO_FREQ_ID                      0x45

/*
 * [network number, key[8]]. The key travels host to dongle, which is why no
 * build of this firmware needs a network-key secret. [bridge, tools]
 */
#define ANTW_MESG_NETWORK_KEY_ID                             0x46

/*
 * [filler, level]. Device-wide transmit power - what ANT_SetTransmitPower()
 * sends, and what Zwift calls while setting up a search. Answering it with
 * INVALID_MESSAGE stalls the search. [bridge, readme]
 */
#define ANTW_MESG_RADIO_TX_POWER_ID                          0x47

/* Continuous-wave test transmit. Not bridged. [rev5.1 sec 9.5, host-api] */
#define ANTW_MESG_RADIO_CW_MODE_ID                           0x48

/* [channel, waveform lo, waveform hi]. Search duty cycle. [bridge] */
#define ANTW_MESG_SEARCH_WAVEFORM_ID                         0x49

/*
 * Resets the ANT protocol stack, NOT the MCU. Answered with the startup
 * message and no RESPONSE_EVENT. Rebooting here drops the device off the bus
 * and every host session begins with this. [bridge, tools]
 */
#define ANTW_MESG_SYSTEM_RESET_ID                            0x4A

/* [channel]. [bridge, tools] */
#define ANTW_MESG_OPEN_CHANNEL_ID                            0x4B

/*
 * [channel]. Asynchronous: the channel is not free until EVENT_CHANNEL_CLOSED
 * arrives. [bridge, tools]
 */
#define ANTW_MESG_CLOSE_CHANNEL_ID                           0x4C

/*
 * [channel or index, requested message id]. The reply is the requested
 * message; a refusal is a RESPONSE_EVENT naming 0x4D. For several messages
 * the first byte is not a channel at all - it is a mask number, an encryption
 * info type, or 0/1 for advanced burst capabilities/configuration. [bridge,
 * tools]
 */
#define ANTW_MESG_REQUEST_ID                                 0x4D

/*
 * [channel, data[8]] outbound. Inbound it may carry the extended fields after
 * the payload - see ext_flags. [bridge, tools]
 */
#define ANTW_MESG_BROADCAST_DATA_ID                          0x4E

/*
 * [channel, data[8]]. The message a host sends to set trainer resistance; the
 * outcome arrives later as EVENT_TRANSFER_TX_COMPLETED or
 * EVENT_TRANSFER_TX_FAILED. [bridge, tools]
 */
#define ANTW_MESG_ACKNOWLEDGED_DATA_ID                       0x4F

/*
 * [seq<<5 | channel, data[8]], bit 7 of byte 0 marks the last packet. Only
 * bits 0-4 are the channel number, which is why 32 channels is the serial
 * protocol's natural ceiling. No per-packet acknowledgement on success.
 * [bridge, tools]
 */
#define ANTW_MESG_BURST_DATA_ID                              0x50

/*
 * [channel, device number lo, device number hi, device type, transmission
 * type]. All-zero is the wildcard that matches anything. [bridge, tools]
 */
#define ANTW_MESG_CHANNEL_ID_ID                              0x51

/* [channel, status]. Requested with MESG_REQUEST. [bridge, tools] */
#define ANTW_MESG_CHANNEL_STATUS_ID                          0x52

/* Enter continuous-wave test mode. Not bridged. [rev5.1 sec 9.5, host-api] */
#define ANTW_MESG_RADIO_CW_INIT_ID                           0x53

/*
 * What the ANT *stack* can do - a superset of what the bridge implements.
 * Requested with MESG_REQUEST. See the capabilities section for the byte and
 * bit layout, and for the exact reply this firmware gives. [bridge, observed]
 */
#define ANTW_MESG_CAPABILITIES_ID                            0x54

/*
 * [filler, mode] on the way in, [channel, mode] on the way back. Dispatched
 * at src/ant_serial_bridge.c and requested in handle_request(). A Nordic
 * extension: not in Rev 5.1, and no witness inside this repository - the id
 * was recovered by the Wave 2 shim and is BUILD_ASSERTed in
 * src/ant_radio_sdk_ant.c. [sdk-ant-shim + verify:sdk-ant-shim]
 */
#define ANTW_MESG_CHANNEL_CRC_MODE_ID                        0x58

/*
 * [channel, device number lo, device number hi, device type, transmission
 * type, list index]. [bridge]
 */
#define ANTW_MESG_ID_LIST_ADD_ID                             0x59

/* [channel, list size, include/exclude flag]. [bridge] */
#define ANTW_MESG_ID_LIST_CONFIG_ID                          0x5A

/*
 * Background scanning. Bridged as of 2026-08-10: refuses with
 * ANTW_CLOSE_ALL_CHANNELS unless every channel is closed, then takes over
 * channel 0 as a wildcard background-scan slave via MESG_ASSIGN_CHANNEL's
 * extended byte. The optional synchronous-only byte is accepted and ignored.
 * radiant_core backend only - the capabilities reply advertises this there
 * and OFF elsewhere. [bridge]
 */
#define ANTW_MESG_OPEN_RX_SCAN_MODE_ID                       0x5B

/*
 * Legacy extended broadcast: channel id inline instead of behind a flag byte.
 * Superseded by MESG_ANTLIB_CONFIG's flag mechanism; this dongle never emits
 * it. [rev5.1 sec 9.5, host-api]
 */
#define ANTW_MESG_EXT_BROADCAST_DATA_ID                      0x5D

/*
 * Legacy extended acknowledged data. See MESG_EXT_BROADCAST_DATA_ID. [rev5.1
 * sec 9.5, host-api]
 */
#define ANTW_MESG_EXT_ACKNOWLEDGED_DATA_ID                   0x5E

/*
 * Legacy extended burst data. See MESG_EXT_BROADCAST_DATA_ID. [rev5.1 sec
 * 9.5, host-api]
 */
#define ANTW_MESG_EXT_BURST_DATA_ID                          0x5F

/*
 * [channel, level] or [channel, level, custom]. Per-channel transmit power.
 * Only takes on an assigned channel, so the bridge holds the requested level
 * and reapplies it after each successful assign. The third byte is an
 * extension of this firmware's and is read only when the level carries
 * RADIO_TX_POWER_LVL_CUSTOM: that level names a raw radio register value and
 * there is nowhere else on the wire to put one, so without it the custom
 * level silently means register 0. No retail stick sends three bytes and none
 * needs to - a two-byte message means exactly what it always did. [bridge,
 * readme]
 */
#define ANTW_MESG_CHANNEL_RADIO_TX_POWER_ID                  0x60

/*
 * Read the device serial number. Not bridged. [rev5.1 sec 9.5 + verify:sdk-
 * ant-shim]
 */
#define ANTW_MESG_GET_SERIAL_NUM_ID                          0x61

/*
 * [channel, timeout]. Low-priority search timeout, in 2.5 s increments.
 * [bridge]
 */
#define ANTW_MESG_SET_LP_SEARCH_TIMEOUT_ID                   0x63

/*
 * Set the channel's device number from the device's own serial number.
 * Implied by the ANT_DLL export ANT_SetSerialNumChannelId; not bridged.
 * Corroborated by its neighbours in the id sequence (0x63, 0x66). [rev5.1
 * sec 9.5 + verify:sdk-ant-shim]
 */
#define ANTW_MESG_SERIAL_NUM_SET_CHANNEL_ID_ID               0x65

/*
 * [filler, enable]. The older, narrower way of asking for the channel id on
 * every received message; the bridge maps it onto lib config bit
 * MESG_OUT_INC_DEVICE_ID. Hosts still send it instead of 0x6E, and without it
 * every broadcast arrives anonymous. [bridge]
 */
#define ANTW_MESG_RX_EXT_MESGS_ENABLE_ID                     0x66

/*
 * Not bridged, and the LED capability bit is reported OFF - which is why a
 * host that has ANT_EnableLED never sends it. [readme]
 */
#define ANTW_MESG_ENABLE_LED_FLASH_ID                        0x68

/*
 * Force the crystal oscillator on. Implied by the ANT_DLL export
 * ANT_CrystalEnable, which Zwift resolves. Not bridged. Corroborated by the
 * adjacent 0x6E (ANT_LibConfigCustom). [rev5.1 sec 9.5 + verify:sdk-ant-shim]
 */
#define ANTW_MESG_XTAL_ENABLE_ID                             0x6D

/*
 * [filler, config bits]. Turns the extended output fields on. Zero means
 * clear everything - there is no separate clear message on the wire. 0xE0 is
 * channel id + RSSI + RX timestamp; 0x80 is device id only. [bridge, tools]
 */
#define ANTW_MESG_ANTLIB_CONFIG_ID                           0x6E

/*
 * [reason]. Sent once after the stack has reset. A host that does not see
 * this concludes the dongle is dead. [bridge, tools]
 */
#define ANTW_MESG_STARTUP_MESG_ID                            0x6F

/* [channel, freq0, freq1, freq2]. Frequency-agility hop table. [bridge] */
#define ANTW_MESG_AUTO_FREQ_CONFIG_ID                        0x70

/*
 * [channel, threshold, (custom threshold)]. Only pair with something close
 * by. [bridge]
 */
#define ANTW_MESG_PROX_SEARCH_CONFIG_ID                      0x71

/*
 * Same header byte as MESG_BURST_DATA, but 8, 16 or 24 data bytes. Accepting
 * this while never bridging 0x78 gives a dongle that takes 24-byte packets
 * and can never send one. [bridge, readme]
 */
#define ANTW_MESG_ADV_BURST_DATA_ID                          0x72

/*
 * Not bridged - sdk-ant exposes no API for it - and correspondingly NOT
 * advertised in the capabilities reply. The one advanced-options-3 bit that
 * is honestly zero. [tools, readme]
 */
#define ANTW_MESG_EVENT_BUFFERING_CONFIG_ID                  0x74

/* [channel, priority]. [bridge, rev5.1 sec 9.5 + verify:sdk-ant-shim] */
#define ANTW_MESG_SET_SEARCH_CH_PRIORITY_ID                  0x75

/*
 * Advertised in the capabilities reply and deliberately not bridged: sdk-ant
 * has no single-chip API for it. Clearing the bit instead would make this
 * dongle report something no ANTUSB-m reports. [tools, readme]
 */
#define ANTW_MESG_HIGH_DUTY_SEARCH_MODE_ID                   0x77

/*
 * [filler, enable, rf payload size, required modes, 0, 0, optional modes, 0,
 * 0, (stall lo, stall hi, retry extension)]. Request index 0 returns
 * capabilities, index 1 the current configuration (reply drops the enable
 * byte). REQUEST REPLY: 0x00 -> 4 bytes
 * (ANTW_MESG_CONFIG_ADV_BURST_REQ_CAPABILITIES_SIZE); 0x01 -> 10 bytes
 * (ANTW_MESG_CONFIG_ADV_BURST_REQ_CONFIG_SIZE). Neither reply repeats the
 * selecting byte, so nothing in the reply itself says which shape it is -
 * only pairing it with the request that drew it does. [bridge, tools]
 */
#define ANTW_MESG_CONFIG_ADV_BURST_ID                        0x78

/*
 * Command is [filter lo, filter hi] with no channel byte; reply is
 * [filler, filter lo, filter hi] - one byte longer, which is what the
 * round-trip check in ant_features.py exists to catch. REQUEST REPLY: one
 * shape, 3 bytes (ANTW_MESG_EVENT_FILTER_CONFIG_REQ_SIZE). [bridge, tools]
 */
#define ANTW_MESG_EVENT_FILTER_CONFIG_ID                     0x79

/*
 * [channel, mask config]. Binds one of the masks to a channel;
 * INVALID_SDU_MASK (0xFF) turns selective updates back off. [bridge, tools]
 */
#define ANTW_MESG_SDU_CONFIG_ID                              0x7A

/*
 * [mask index, mask[8]]. byte 0 is a mask number, not a channel. The reply
 * has the same shape, which is why its size is ANT_MAX_PAYLOAD_SIZE +
 * CHANNEL_NUM_SIZE. [bridge, tools]
 */
#define ANTW_MESG_SDU_SET_MASK_ID                            0x7B

/*
 * User NVM configuration page. No sdk-ant API; not bridged. Implied by the
 * ANT_DLL export ANT_ConfigUserNVM, corroborated by its neighbours (0x7B
 * ANT_SetSelectiveDataUpdateMask, 0x7D-0x7F encryption). [rev5.1 sec 9.5 +
 * verify:sdk-ant-shim]
 */
#define ANTW_MESG_USER_CONFIG_PAGE_ID                        0x7C

/*
 * Write [channel, mode, key number, decimation rate] - compiled out unless
 * CONFIG_ANT_DONGLE_ENCRYPTION. Read side always bridged: an
 * ENCRYPTION_INFO_GET_* request returns [info type, data...], the type
 * echoed at byte 0. REQUEST REPLY: 0x00 -> 2 bytes
 * (ANTW_MESG_CONFIG_ENCRYPT_REQ_CAPABILITIES_SIZE); 0x01 -> 5 bytes
 * (ANTW_MESG_CONFIG_ENCRYPT_REQ_CONFIG_ID_SIZE); 0x02 -> 20 bytes
 * (ANTW_MESG_CONFIG_ENCRYPT_REQ_CONFIG_USER_DATA_SIZE). Unlike the
 * advanced-burst reply above, the echoed type byte lets a reader check length
 * with no memory of the request. An unrecognised index is refused with a
 * RESPONSE_EVENT naming 0x4D. [bridge, tools]
 */
#define ANTW_MESG_ENCRYPT_ENABLE_ID                          0x7D

/*
 * [key number, key[16]]. That the key is 128 bits is stated in prose only; no
 * size constant names it. [bridge, tools]
 */
#define ANTW_MESG_SET_ENCRYPT_KEY_ID                         0x7E

/*
 * [info type, data...]. How much data depends on the type, and the length
 * must be checked against it here - the stack reads a fixed count per type
 * with nothing to bound it. RNG seed is refused on purpose: no size for it is
 * documented anywhere. [bridge, tools]
 */
#define ANTW_MESG_SET_ENCRYPT_INFO_ID                        0x7F

/* [channel, cycles]. [bridge, rev5.1 sec 9.5 + verify:sdk-ant-shim] */
#define ANTW_MESG_ACTIVE_SEARCH_SHARING_ID                   0x81

/*
 * [filler, enable]. Enhanced channel spacing. A Nordic extension: not in Rev
 * 5.1, and no witness inside this repository - the id was recovered by the
 * Wave 2 shim and is BUILD_ASSERTed in src/ant_radio_sdk_ant.c. [sdk-ant-shim
 * + verify:sdk-ant-shim]
 */
#define ANTW_MESG_ECS_ENABLE_ID                              0x89

/*
 * [channel] to clear, [channel, pending] on request. A Nordic extension: not
 * in Rev 5.1, and no witness inside this repository - the id was recovered by
 * the Wave 2 shim and is BUILD_ASSERTed in src/ant_radio_sdk_ant.c. Note that
 * the *request* form of this id replies with MESG_PENDING_TRANSMIT_GET_SIZE
 * bytes, which is a separate constant from MESG_PENDING_TRANSMIT_CLEAR's own
 * 1-byte command payload. [sdk-ant-shim + verify:sdk-ant-shim]
 */
#define ANTW_MESG_PENDING_TRANSMIT_CLEAR_ID                  0x8C

/*
 * Serial-layer error: bad checksum, bad length, or a frame that arrived too
 * slowly. This firmware drops malformed frames silently instead - recorded so
 * a host-side parser can name the byte. [rev5.1 sec 7.4 + verify:sdk-ant-
 * shim]
 */
#define ANTW_MESG_SERIAL_ERROR_ID                            0xAE

/*
 * ANTW_MESG_RSSI_SEARCH_THRESHOLD_ID is UNRESOLVED. Proximity search
 * threshold in RSSI rather than the bins MESG_PROX_SEARCH_CONFIG (0x71)
 * uses. Implied by the ANT_DLL export ANT_RSSI_SetSearchThreshold. Believed
 * to sit in the 0xC0 AP2-era extension block, but no value there is
 * corroborated anywhere in this repository, and sdk-ant v2.1.0 defines no
 * such id either. Recover it in src/ant_radio_sdk_ant.c and BUILD_ASSERT it
 * there; see the unresolved table in docs/ant-serial-protocol.md.
 * [host-api + verify:sdk-ant-shim]
 */

/*
 * ANTW_MESG_SLEEP_ID is UNRESOLVED. Low-power sleep state. Implied by the
 * ANT_DLL export ANT_SleepMessage, which Zwift resolves. Same 0xC0 block and
 * lack of corroboration as MESG_RSSI_SEARCH_THRESHOLD_ID; sdk-ant references
 * the name only inside a commented-out, never-defined dispatch arm. Recover
 * in src/ant_radio_sdk_ant.c and BUILD_ASSERT it there; see the unresolved
 * table in docs/ant-serial-protocol.md. [host-api + verify:sdk-ant-shim]
 */

/* ------------------------------------------------------------------------
 * Reply sizes
 * The LEN byte the bridge puts on each reply it composes itself. Where a
 * request reply is a different shape from the command sharing its id, the
 * message's reply_len block in protocol/ant_wire.yaml points at the
 * constant here rather than restating the number.
 * ------------------------------------------------------------------------ */

/* [channel, message id, code]. [bridge] */
#define ANTW_MESG_RESPONSE_EVENT_SIZE                        3

/* [reason]. [bridge] */
#define ANTW_MESG_STARTUP_MESG_SIZE                          1

/* Nine bytes, confirmed by the observed reply 080800b23200fd8d0f. [observed] */
#define ANTW_MESG_CAPABILITIES_SIZE                          9

/*
 * Null-padded version string. src/ant_radio_stub.c calls it a 20-byte
 * payload; nothing in this repository shows the reply on the wire. [stub +
 * verify:sdk-ant-shim]
 */
#define ANTW_MESG_VERSION_SIZE                               20

/* [channel, status]. [inferred + verify:sdk-ant-shim] */
#define ANTW_MESG_CHANNEL_STATUS_SIZE                        2

/* [channel, number lo, number hi, device type, transmission type]. [bridge] */
#define ANTW_MESG_CHANNEL_ID_SIZE                            5

/* [channel, period lo, period hi]. [bridge] */
#define ANTW_MESG_CHANNEL_MESG_PERIOD_SIZE                   3

/* [channel, frequency]. [bridge] */
#define ANTW_MESG_CHANNEL_RADIO_FREQ_SIZE                    2

/* [channel, mode]. [inferred + verify:sdk-ant-shim] */
#define ANTW_MESG_CHANNEL_CRC_MODE_SIZE                      2

/* [channel, priority]. [inferred + verify:sdk-ant-shim] */
#define ANTW_MESG_SET_SEARCH_CH_PRIORITY_SIZE                2

/* [channel, pending]. [inferred + verify:sdk-ant-shim] */
#define ANTW_MESG_PENDING_TRANSMIT_GET_SIZE                  2

/* [filler, config bits]. [bridge] */
#define ANTW_MESG_ANTLIB_CONFIG_SIZE                         2

/* [channel, cycles]. [inferred + verify:sdk-ant-shim] */
#define ANTW_MESG_ACTIVE_SEARCH_SHARING_REQ_SIZE             2

/*
 * [filler, filter lo, filter hi] - one byte longer than the command that set
 * it. [bridge, tools]
 */
#define ANTW_MESG_EVENT_FILTER_CONFIG_REQ_SIZE               3

/*
 * [size, required, 0, 0, optional, 0, 0, stall lo, stall hi, retry]. The
 * 11-byte configuration minus the enable byte the command carried. [stub,
 * tools]
 */
#define ANTW_MESG_CONFIG_ADV_BURST_REQ_CONFIG_SIZE           10

/*
 * The advanced-burst capabilities reply: byte 0 is a max packet size code,
 * byte 1 a supported-modes bitfield, bytes 2-3 reserved and zero. Recovered
 * and BUILD_ASSERTed in src/ant_radio_sdk_ant.c, since it is the LEN byte on
 * a reply the bridge composes and no local substitute was legitimate while
 * unresolved. Used at src/ant_radio_stub.c:254 to bound a memset.
 * [sdk-ant-shim + verify:sdk-ant-shim]
 */
#define ANTW_MESG_CONFIG_ADV_BURST_REQ_CAPABILITIES_SIZE     4

/* [info type, supported mode]. [bridge] */
#define ANTW_MESG_CONFIG_ENCRYPT_REQ_CAPABILITIES_SIZE       2

/* [info type, crypto id[4]]. [bridge] */
#define ANTW_MESG_CONFIG_ENCRYPT_REQ_CONFIG_ID_SIZE          5

/*
 * [info type, custom user data[19]]. The longest frame body the parser ever
 * sees. [bridge, tools]
 */
#define ANTW_MESG_CONFIG_ENCRYPT_REQ_CONFIG_USER_DATA_SIZE   20

/* ------------------------------------------------------------------------
 * Channel types
 * Byte 1 of MESG_ASSIGN_CHANNEL.
 * ------------------------------------------------------------------------ */

/*
 * Bidirectional receive. What every host opens to hear a sensor. [tools
 * (ant_verify)]
 */
#define ANTW_CHANNEL_TYPE_SLAVE                              0x00

/*
 * Bidirectional transmit. What ant_sim.py asks for to make this dongle
 * impersonate a sensor. [tools (ant_sim)]
 */
#define ANTW_CHANNEL_TYPE_MASTER                             0x10

/* Shared bidirectional receive. [rev5.1 sec 5.2.1 + verify:sdk-ant-shim] */
#define ANTW_CHANNEL_TYPE_SHARED_SLAVE                       0x20

/* Shared bidirectional transmit. [rev5.1 sec 5.2.1 + verify:sdk-ant-shim] */
#define ANTW_CHANNEL_TYPE_SHARED_MASTER                      0x30

/*
 * Receive only - never transmits, so it cannot acknowledge. [rev5.1 sec 5.2.1
 * + verify:sdk-ant-shim]
 */
#define ANTW_CHANNEL_TYPE_SLAVE_RX_ONLY                      0x40

/*
 * Transmit only - never opens a receive window. [rev5.1 sec 5.2.1 +
 * verify:sdk-ant-shim]
 */
#define ANTW_CHANNEL_TYPE_MASTER_TX_ONLY                     0x50

/* ------------------------------------------------------------------------
 * Extended assignment
 * Optional byte 3 of MESG_ASSIGN_CHANNEL. The bridge passes it straight
 * through.
 * ------------------------------------------------------------------------ */

/* Background scanning enable. [rev5.1 sec 5.2.1 + verify:sdk-ant-shim] */
#define ANTW_EXT_PARAM_ALWAYS_SEARCH                         0x01

/*
 * Frequency agility enable. ANT+ profiles forbid it. [rev5.1 sec 5.2.1 +
 * verify:sdk-ant-shim]
 */
#define ANTW_EXT_PARAM_FREQUENCY_AGILITY                     0x04

/*
 * Automatic shared-slave address management. [rev5.1 sec 5.2.1 + verify:sdk-
 * ant-shim]
 */
#define ANTW_EXT_PARAM_AUTO_SHARED_SLAVE                     0x08

/* Fast channel initiation. [readme] */
#define ANTW_EXT_PARAM_FAST_INITIATION_MODE                  0x10

/* Asynchronous transmission channel. [readme] */
#define ANTW_EXT_PARAM_ASYNC_TX_MODE                         0x20

/* ------------------------------------------------------------------------
 * Channel status
 * Low two bits of byte 1 of a MESG_CHANNEL_STATUS reply.
 * ------------------------------------------------------------------------ */

/* No channel type or network set. [stub] */
#define ANTW_STATUS_UNASSIGNED_CHANNEL                       0x00

/*
 * Assigned but not open. Assigning again is refused with
 * CHANNEL_IN_WRONG_STATE, which is why every host opens a session with a
 * system reset. [rev5.1 sec 9.5.7.1 + verify:sdk-ant-shim]
 */
#define ANTW_STATUS_ASSIGNED_CHANNEL                         0x01

/* Open and searching. [rev5.1 sec 9.5.7.1 + verify:sdk-ant-shim] */
#define ANTW_STATUS_SEARCHING_CHANNEL                        0x02

/* Open and tracking a master. [rev5.1 sec 9.5.7.1 + verify:sdk-ant-shim] */
#define ANTW_STATUS_TRACKING_CHANNEL                         0x03

/*
 * The bits above; the rest of the byte carries network number and channel
 * type. [rev5.1 sec 9.5.7.1 + verify:sdk-ant-shim]
 */
#define ANTW_STATUS_CHANNEL_STATE_MASK                       0x03

/* ------------------------------------------------------------------------
 * Radio transmit power levels
 * Byte 1 of MESG_RADIO_TX_POWER (device-wide) and byte 1 of
 * MESG_CHANNEL_RADIO_TX_POWER (per channel). The dBm figures are the
 * levels a retail ANT stick exposes; what a given radio actually emits is
 * a property of the part.
 * ------------------------------------------------------------------------ */

/*
 * -20 dBm. A deliberate request to be quiet - a bike shop, or a race paddock
 * with forty trainers in one room. [bridge]
 */
#define ANTW_RADIO_TX_POWER_LVL_0                            0x00

/* -12 dBm. [bridge] */
#define ANTW_RADIO_TX_POWER_LVL_1                            0x01

/* -4 dBm. [bridge] */
#define ANTW_RADIO_TX_POWER_LVL_2                            0x02

/*
 * 0 dBm. The ANT default, and what a host sends when it has no opinion - the
 * common case. [bridge]
 */
#define ANTW_RADIO_TX_POWER_LVL_3                            0x03

/*
 * +4 dBm. The ceiling on the nRF51422 inside the stick being impersonated, so
 * a host asking for it is asking for everything the hardware has. [bridge]
 */
#define ANTW_RADIO_TX_POWER_LVL_4                            0x04

/*
 * +8 dBm. Exists only on the nRF52820, nRF52833 and nRF52840. No host will
 * ever request it by name, because no retail stick has ever had it. [bridge]
 */
#define ANTW_RADIO_TX_POWER_LVL_5                            0x05

/*
 * Names a raw register value rather than a level, so it is passed through
 * untouched. [bridge]
 */
#define ANTW_RADIO_TX_POWER_LVL_CUSTOM                       0x80

/* ------------------------------------------------------------------------
 * Library configuration bits
 * Byte 1 of MESG_ANTLIB_CONFIG. Each bit appends one field to every
 * received data message; the fields arrive in bit order behind a flag
 * byte. Sending 0x00 clears all of them - there is no separate clear
 * message on the wire.
 * ------------------------------------------------------------------------ */

/*
 * Reconfigure the radio on every event rather than only on change. [rev5.1
 * sec 9.5.2.9 + verify:sdk-ant-shim]
 */
#define ANTW_LIB_CONFIG_RADIO_CONFIG_ALWAYS                  0x01

/*
 * Append the receive timestamp: 16 bits of the radio's own 32768 Hz counter,
 * so it wraps every two seconds. The only clock on the path that is not the
 * host's - it reads 0.009 ms of timing error where the host clock reads 2.6
 * ms. [tools (ant_verify)]
 */
#define ANTW_LIB_CONFIG_MESG_OUT_INC_TIME_STAMP              0x20

/*
 * Append RSSI. The difference between a packet lost to a collision and one
 * lost to a fade. [tools (ant_verify)]
 */
#define ANTW_LIB_CONFIG_MESG_OUT_INC_RSSI                    0x40

/*
 * Append the channel id. Without it every broadcast arrives anonymous and no
 * sensor can be named. This is also what MESG_RX_EXT_MESGS_ENABLE maps onto.
 * [bridge, tools]
 */
#define ANTW_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID               0x80

/*
 * ANTW_LIB_CONFIG_DEVICE_ID_ONLY is a Python-side alias of an identical value
 * and is deliberately not defined here. Alias: the narrow setting ant_scan.py
 * and ant_session.py use - identity, nothing else.
 */

/*
 * Channel id + RSSI + RX timestamp, all three. What ant_verify.py asks for,
 * and what radiant_core must assemble: the timestamp is the figure the timing
 * gate is read against. [tools (ant_verify)]
 */
#define ANTW_LIB_CONFIG_ALL_EXT_FIELDS                       0xE0

/*
 * Passed to the clear path when a host sends 0x6E with a zero config byte.
 * [bridge + verify:sdk-ant-shim]
 */
#define ANTW_LIB_CONFIG_MASK_ALL                             0xFF

/* ------------------------------------------------------------------------
 * Extended message flag bits
 * Byte 9 of a received data message - immediately after the 8-byte payload
 * - when any lib config bit is set. The fields follow in a fixed order,
 * but each is present only if its flag bit is set, so an offset is only
 * correct relative to which of the earlier ones turned up. Reading RSSI at
 * a fixed offset works right up until a run is made without the channel
 * id, and then quietly reports the device number as a signal strength.
 * ------------------------------------------------------------------------ */

/*
 * 4 bytes follow: device number lo, device number hi, device type (bit 7 is
 * the pairing bit), transmission type. [tools (ant_verify, ant_scan)]
 */
#define ANTW_EXT_FLAG_CHANNEL_ID                             0x80

/*
 * 3 bytes follow: measurement type, value, threshold configuration. [tools
 * (ant_verify)]
 */
#define ANTW_EXT_FLAG_RSSI                                   0x40

/*
 * 2 bytes follow: the 32768 Hz receive timestamp, little endian. [tools
 * (ant_verify)]
 */
#define ANTW_EXT_FLAG_RX_TIMESTAMP                           0x20

/* ------------------------------------------------------------------------
 * RSSI measurement types
 * Byte 0 of the RSSI extended field.
 * ------------------------------------------------------------------------ */

/*
 * The one measurement type that carries dBm. Anything else is a proprietary
 * scale, and a number on an unknown scale is worse than no number. [tools
 * (ant_verify)]
 */
#define ANTW_RSSI_MEASUREMENT_TYPE_DBM                       0x20

/* ------------------------------------------------------------------------
 * Startup message reasons
 * The single payload byte of MESG_STARTUP_MESG. Zero is not a bit: it
 * means a power-on or a command reset with no other flag set.
 * ------------------------------------------------------------------------ */

/* Power-on, or a reset by command. [tools (ant_probe)] */
#define ANTW_STARTUP_POWER_ON_RESET                          0x00

/* Hardware reset line. [tools (ant_probe)] */
#define ANTW_STARTUP_HARDWARE_RESET_LINE                     0x01

/* Watchdog. [tools (ant_probe)] */
#define ANTW_STARTUP_WATCH_DOG_RESET                         0x02

/*
 * Reset by MESG_SYSTEM_RESET. What this firmware reports - it sends 0x00,
 * because the stack reset is indistinguishable from a cold start from the
 * host's side. [tools (ant_probe)]
 */
#define ANTW_STARTUP_COMMAND_RESET                           0x20

/* Synchronous serial reset. [tools (ant_probe)] */
#define ANTW_STARTUP_SYNCHRONOUS_RESET                       0x40

/* Resume from suspend. [tools (ant_probe)] */
#define ANTW_STARTUP_SUSPEND_RESET                           0x80

/* ------------------------------------------------------------------------
 * Channel event codes
 * Byte 2 of a RESPONSE_EVENT whose byte 1 is MESG_EVENT_ID (0x01). These
 * arrive unsolicited; the response codes below arrive in answer to a
 * command. They share one number space.
 * ------------------------------------------------------------------------ */

/* The search window expired without finding a master. [tools (ant_session)] */
#define ANTW_EVENT_RX_SEARCH_TIMEOUT                         0x01

/*
 * A receive slot came and went with nothing valid in it. This is the event
 * that makes loss accounting possible: the radio reports one for every packet
 * lost on the air and nothing at all for one lost in the host, so a run that
 * loses more than it can account for is losing it locally. [tools
 * (ant_session, ant_verify)]
 */
#define ANTW_EVENT_RX_FAIL                                   0x02

/*
 * A payload has gone on the air; load the next one. ant_sim.py paces itself
 * on this rather than on a host timer. [tools (ant_session, ant_sim)]
 */
#define ANTW_EVENT_TX                                        0x03

/* An inbound burst did not complete. [tools (ant_session)] */
#define ANTW_EVENT_TRANSFER_RX_FAILED                        0x04

/*
 * Acknowledged data or a burst was acknowledged by the far end. Also releases
 * the bridge's single burst block. [bridge, tools]
 */
#define ANTW_EVENT_TRANSFER_TX_COMPLETED                     0x05

/*
 * It went out and nothing acknowledged it. Also releases the burst block.
 * [bridge, tools]
 */
#define ANTW_EVENT_TRANSFER_TX_FAILED                        0x06

/*
 * The channel is now free to unassign. A close is asynchronous, so
 * unassigning before this arrives is refused. [bridge, tools]
 */
#define ANTW_EVENT_CHANNEL_CLOSED                            0x07

/*
 * Enough consecutive misses that the channel has dropped back into search.
 * [tools (ant_session)]
 */
#define ANTW_EVENT_RX_FAIL_GO_TO_SEARCH                      0x08

/* Two channels wanted the radio at the same instant. [tools (ant_session)] */
#define ANTW_EVENT_CHANNEL_COLLISION                         0x09

/* Progress, not an outcome - keep waiting. [tools (ant_session)] */
#define ANTW_EVENT_TRANSFER_TX_START                         0x0A

/*
 * Data was blocked because the application is servicing events too slowly to
 * keep up, distinct from EVENT_QUE_OVERFLOW (0x35): this is a host that
 * stopped draining, that is a queue sized too small. We do not raise this
 * today - see the sdk-ant comparison report - but the wire byte belongs in
 * the table regardless of whether anything emits it, so a host that ever does
 * see it decodes something other than an unknown event code. [rev5.1 sec
 * 9.5.6]
 */
#define ANTW_EVENT_RX_DATA_OVERFLOW                          0x0B

/*
 * The stack has finished with the burst block it was handed and the next may
 * overwrite it. The bridge consumes this internally and never puts it on the
 * wire: a real stick frames bursts itself. If radiant_core fails to raise it
 * exactly once per accepted block, host bursts stall 1000 ms per packet.
 * [bridge, rev5.1 sec 9.5.6 + verify:sdk-ant-shim]
 */
#define ANTW_EVENT_TRANSFER_NEXT_DATA_BLOCK                  0x11

/* ------------------------------------------------------------------------
 * Response codes
 * Byte 2 of a RESPONSE_EVENT answering a command. Same number space as the
 * event codes.
 * ------------------------------------------------------------------------ */

/*
 * Accepted. A host that gets anything else for a message it needs gives up,
 * and says nothing useful about which one. [bridge, tools]
 */
#define ANTW_RESPONSE_NO_ERROR                               0x00

/*
 * The commonest real-world refusal: assigning a channel a previous run left
 * assigned, or unassigning one that has not finished closing. [rev5.1 sec
 * 9.5.6 + verify:sdk-ant-shim]
 */
#define ANTW_CHANNEL_IN_WRONG_STATE                          0x15

/*
 * Data was sent on a channel that is not open. [rev5.1 sec 9.5.6 +
 * verify:sdk-ant-shim]
 */
#define ANTW_CHANNEL_NOT_OPENED                              0x16

/* Opened before MESG_CHANNEL_ID. [rev5.1 sec 9.5.6 + verify:sdk-ant-shim] */
#define ANTW_CHANNEL_ID_NOT_SET                              0x18

/*
 * Scan mode requires every other channel closed. [rev5.1 sec 9.5.6 +
 * verify:sdk-ant-shim]
 */
#define ANTW_CLOSE_ALL_CHANNELS                              0x19

/*
 * A burst is already running on this channel. The bridge answers with this
 * when the host outruns the single burst block. [bridge, rev5.1 sec 9.5.6 +
 * verify:sdk-ant-shim]
 */
#define ANTW_TRANSFER_IN_PROGRESS                            0x1F

/*
 * Burst packets arrived out of order. [rev5.1 sec 9.5.6 + verify:sdk-ant-
 * shim]
 */
#define ANTW_TRANSFER_SEQUENCE_NUMBER_ERROR                  0x20

/* Burst aborted. [rev5.1 sec 9.5.6 + verify:sdk-ant-shim] */
#define ANTW_TRANSFER_IN_ERROR                               0x21

/*
 * The channel is busy with another transfer. Distinct from
 * TRANSFER_IN_PROGRESS (0x1F), which is what this bridge answers when the
 * host outruns its single burst block. [rev5.1 sec 9.5.6 + verify:sdk-ant-
 * shim]
 */
#define ANTW_TRANSFER_BUSY                                   0x22

/*
 * Payload longer than the message allows. [rev5.1 sec 9.5.6 + verify:sdk-ant-
 * shim]
 */
#define ANTW_MESSAGE_SIZE_EXCEEDS_LIMIT                      0x27

/*
 * The dispatcher does not implement this message, or its body was too short
 * to run. Distinguishing 'the bridge refused it' from 'the stack refused it'
 * is exactly this code: anything else means the message was decoded and
 * handed on. [bridge, tools]
 */
#define ANTW_INVALID_MESSAGE                                 0x28

/*
 * Network number above the maximum the capabilities reply advertises. [rev5.1
 * sec 9.5.6 + verify:sdk-ant-shim]
 */
#define ANTW_INVALID_NETWORK_NUMBER                          0x29

/*
 * Inclusion/exclusion list index out of range. [rev5.1 sec 9.5.6 +
 * verify:sdk-ant-shim]
 */
#define ANTW_INVALID_LIST_ID                                 0x30

/*
 * Transmit attempted on a channel other than 0 in scan mode. [rev5.1 sec
 * 9.5.6 + verify:sdk-ant-shim]
 */
#define ANTW_INVALID_SCAN_TX_CHANNEL                         0x31

/*
 * The stack decoded the message and disliked a value. Measured example:
 * MESG_SET_ENCRYPT_KEY answers 51 when CONFIG_ANT_ENCRYPTED_CHANNELS is 0,
 * because the valid key index range is then empty. [readme, tools]
 */
#define ANTW_INVALID_PARAMETER_PROVIDED                      0x33

/*
 * The host is sending faster than the serial layer drains. [rev5.1 sec 9.5.6
 * + verify:sdk-ant-shim]
 */
#define ANTW_EVENT_SERIAL_QUE_OVERFLOW                       0x34

/*
 * The event queue overflowed - events were dropped. [rev5.1 sec 9.5.6 +
 * verify:sdk-ant-shim]
 */
#define ANTW_EVENT_QUE_OVERFLOW                              0x35

/* ------------------------------------------------------------------------
 * Advanced burst configuration
 * Fields of MESG_CONFIG_ADV_BURST.
 * ------------------------------------------------------------------------ */

/* Enable byte: off. The shipping default. [tools (ant_features)] */
#define ANTW_ADV_BURST_MODE_DISABLE                          0x00

/* Enable byte: on. [tools (ant_features)] */
#define ANTW_ADV_BURST_MODE_ENABLE                           0x01

/* RF payload size code: 8 bytes. [inferred + verify:sdk-ant-shim] */
#define ANTW_ADV_BURST_MODES_SIZE_8_BYTES                    0x01

/* RF payload size code: 16 bytes. [inferred + verify:sdk-ant-shim] */
#define ANTW_ADV_BURST_MODES_SIZE_16_BYTES                   0x02

/*
 * RF payload size code: 24 bytes. What the stub advertises and what
 * ant_features.py configures. [stub, tools]
 */
#define ANTW_ADV_BURST_MODES_SIZE_24_BYTES                   0x03

/* Optional-modes bit: frequency hopping. [stub, tools] */
#define ANTW_ADV_BURST_MODES_FREQ_HOP                        0x01

/* ------------------------------------------------------------------------
 * Selective data update
 * Values of the mask-config byte of MESG_SDU_CONFIG.
 * ------------------------------------------------------------------------ */

/*
 * Detaches a channel from every mask - the way to turn selective updates back
 * off. [stub, tools]
 */
#define ANTW_INVALID_SDU_MASK                                0xFF

/*
 * Set alongside a mask number to apply the mask to acknowledged data as well
 * as broadcast. The radio backend masks it off before range-checking the mask
 * number. The bit's value appears nowhere in this repository, so it was
 * recovered by the Wave 2 shim and is BUILD_ASSERTed in
 * src/ant_radio_sdk_ant.c. Note that it shares its value with
 * INVALID_SDU_MASK's top bit, which is why the mask number must be range-
 * checked after the bit is cleared and not before. Used at
 * src/ant_radio_stub.c:587; the local placeholder that stood in for it there
 * guessed the value correctly and has been retired. [sdk-ant-shim +
 * verify:sdk-ant-shim]
 */
#define ANTW_SDU_MASK_ACK_CONFIG_BIT                         0x80

/* ------------------------------------------------------------------------
 * Encryption
 * Info types and modes for MESG_ENCRYPT_ENABLE / MESG_SET_ENCRYPT_INFO.
 * The set and get info types do not line up: set 0 is the crypto id and
 * get 0 is the supported mode, so a round trip writes with one number and
 * reads with the next. That is not a bug in the tools.
 * ------------------------------------------------------------------------ */

/* Channel encryption off. [tools (ant_features)] */
#define ANTW_ENCRYPTION_DISABLED_MODE                        0x00

/* Write: 4-byte crypto id. [tools (ant_features)] */
#define ANTW_ENCRYPTION_INFO_SET_CRYPTO_ID                   0x00

/* Write: 19 bytes of custom user data. [tools (ant_features)] */
#define ANTW_ENCRYPTION_INFO_SET_CUSTOM_USER_DATA            0x01

/*
 * Write: RNG seed. Deliberately refused by this bridge - sdk-ant calls it
 * platform specific and defines no size for it anywhere, and the stack does
 * not take its randomness from the host in any case. Refusing beats guessing
 * a length. [bridge, tools]
 */
#define ANTW_ENCRYPTION_INFO_SET_RNG_SEED                    0x02

/* Read: the supported encryption mode. [tools (ant_features)] */
#define ANTW_ENCRYPTION_INFO_GET_SUPPORTED_MODE              0x00

/* Read: the 4-byte crypto id. [tools (ant_features)] */
#define ANTW_ENCRYPTION_INFO_GET_CRYPTO_ID                   0x01

/* Read: the 19 bytes of custom user data. [tools (ant_features)] */
#define ANTW_ENCRYPTION_INFO_GET_CUSTOM_USER_DATA            0x02

/*
 * Bytes of custom user data. One info-type byte plus these fills
 * MAX_SIZE_VALUE exactly. [stub, tools]
 */
#define ANTW_ENCRYPTION_USER_DATA_SIZE                       19

/*
 * 128-bit key. No sdk-ant constant names this; it is stated only in the prose
 * of ant_crypto_key_set(), which takes a bare pointer. [bridge, stub]
 */
#define ANTW_ENCRYPTION_KEY_SIZE                             16

/*
 * The value a backend answers an ENCRYPTION_INFO_GET_SUPPORTED_MODE request
 * with, and the upper bound it range-checks a channel-enable against. It is
 * the highest of the encryption modes above - user-data request - rather than
 * an independent number. Not visible in this repository, so it was recovered
 * by the Wave 2 shim and is BUILD_ASSERTed in src/ant_radio_sdk_ant.c. Used
 * at src/ant_radio_stub.c:278 and :541; the local placeholder that stood in
 * for it there guessed 1 and was wrong, which is why a stub build's answer to
 * ENCRYPTION_INFO_GET_SUPPORTED_MODE changed when it retired. [sdk-ant-shim +
 * verify:sdk-ant-shim]
 */
#define ANTW_MAX_SUPPORTED_ENCRYPTION_MODE                   0x02

/* ------------------------------------------------------------------------
 * Burst header byte
 * Byte 0 of MESG_BURST_DATA / MESG_ADV_BURST_DATA / the legacy
 * MESG_EXT_BURST_DATA. Not a channel number on its own: it is three fields
 * packed into one byte, and the five-bit channel field is the ONLY place
 * in the serial protocol where a channel number is squeezed below eight
 * bits. Every other channel byte on the wire - including byte 0 of a
 * RESPONSE_EVENT, which echoes whatever the command carried - is a plain
 * uint8. Applying the five-bit ceiling to those is a category error: case
 * frame/sync-in-payload deliberately sends channel 0xA4 in an antlib-
 * config command so that a SYNC byte lands inside the payload, and the
 * dongle correctly echoes 0xA4 back in a 0x40.
 * ------------------------------------------------------------------------ */

/*
 * Channel number. Five bits - which is why 32 channels is the serial
 * protocol's natural ceiling, and why radiant_core is sized for 32 from the
 * first line. Because the field is five bits wide, a header on the wire
 * cannot express a channel above 31 at all; what a burst header CAN address
 * that the device does not have is a channel above the count in byte 0 of the
 * capabilities reply, and that is the bound worth checking on a transcript.
 * [bridge]
 */
#define ANTW_BURST_HEADER_CHANNEL_MASK                       0x1F

/* Sequence number occupies bits 5-6. [bridge] */
#define ANTW_BURST_HEADER_SEQ_SHIFT                          5

/* Sequence number after shifting: 0-3, wrapping. [bridge] */
#define ANTW_BURST_HEADER_SEQ_MASK                           0x03

/* Bit 7: this is the last packet of the transfer. [bridge, tools] */
#define ANTW_BURST_HEADER_LAST                               0x80

/* ------------------------------------------------------------------------
 * RadiANT pairing sub-commands
 * Byte [1] of MESG_RADIANT_PAIRING (0xF5). NOT ANT protocol - ours. Sub-
 * commands rather than four message ids because they are one conversation
 * with an order: enter, supply a scalar, exchange, leave. Separate ids
 * would let a host skip a step and get a state error it could not
 * localise. The reply echoes the sub-command in the same byte, because a
 * public key and a fingerprint are both 'some bytes after a channel byte'
 * otherwise.
 * ------------------------------------------------------------------------ */

/*
 * Leave pairing mode and wipe the exchange state, including any scalar the
 * host supplied. [K4 (docs/radiant-security.md sec 7.4 and 8)]
 */
#define ANTW_RADIANT_PAIR_LEAVE                              0x00

/*
 * Enter pairing mode, timeout in seconds at [2]. Zero means the 60 s default
 * and NEVER 'forever': a node in pairing mode accepts a key from whoever
 * asks, so one forgotten command must not leave it open indefinitely. [K4
 * (docs/radiant-security.md sec 7.4 and 8)]
 */
#define ANTW_RADIANT_PAIR_ENTER                              0x01

/*
 * Supply the host's 32-byte X25519 private scalar at [2..33]. The reply
 * carries the local public key from [2]. The scalar comes from the host
 * because the only entropy source on nRF54L is psa_rng/CRACEN and reaching it
 * drags nrf_security into every build; the honest consequence is that a host-
 * less node cannot pair this way. [K4 (docs/radiant-security.md sec 7.4 and
 * 8)]
 */
#define ANTW_RADIANT_PAIR_SCALAR                             0x02

/*
 * Complete the exchange against the peer's 32-byte public key at [2..33]. The
 * reply carries the six-digit comparison fingerprint as a u24 LE from [2]. A
 * small-order peer key is refused: the result becomes a root key, so
 * accepting one would let anyone able to inject a packet fix the group key to
 * a value they already know. [K4 (docs/radiant-security.md sec 7.4 and 8)]
 */
#define ANTW_RADIANT_PAIR_EXCHANGE                           0x03

/* ------------------------------------------------------------------------
 * Capabilities reply (MESG_CAPABILITIES)
 * 9 bytes. Observed from this firmware: 080800b23200fd8d0f. Byte and bit
 * layout in docs/ant-serial-protocol.md.
 * ------------------------------------------------------------------------ */

/* byte 0 - max_channels. Simultaneous ANT channels the stack has allocated. */
#define ANTW_CAPABILITIES_OFFSET_MAX_CHANNELS                0

/* byte 1 - max_networks. Network keys the stack can hold. */
#define ANTW_CAPABILITIES_OFFSET_MAX_NETWORKS                1

/*
 * byte 2 - standard_options. Negative capabilities: a set bit means the
 * feature is ABSENT.
 */
#define ANTW_CAPABILITIES_OFFSET_STANDARD_OPTIONS            2

/* Cannot receive. [rev5.1 sec 9.5.7.4 + verify:sdk-ant-shim] */
#define ANTW_CAPABILITIES_NO_RECEIVE_CHANNELS                0x01

/* Cannot transmit. [rev5.1 sec 9.5.7.4 + verify:sdk-ant-shim] */
#define ANTW_CAPABILITIES_NO_TRANSMIT_CHANNELS               0x02

/* No receive messages. [rev5.1 sec 9.5.7.4 + verify:sdk-ant-shim] */
#define ANTW_CAPABILITIES_NO_RECEIVE_MESSAGES                0x04

/* No transmit messages. [rev5.1 sec 9.5.7.4 + verify:sdk-ant-shim] */
#define ANTW_CAPABILITIES_NO_TRANSMIT_MESSAGES               0x08

/* No acknowledged data. [rev5.1 sec 9.5.7.4 + verify:sdk-ant-shim] */
#define ANTW_CAPABILITIES_NO_ACKD_MESSAGES                   0x10

/* No burst. [rev5.1 sec 9.5.7.4 + verify:sdk-ant-shim] */
#define ANTW_CAPABILITIES_NO_BURST_MESSAGES                  0x20

/*
 * byte 3 - advanced_options. Positive capabilities from here on: a set bit
 * means present.
 */
#define ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS            3

/* MESG_NETWORK_KEY is supported. [stub] */
#define ANTW_CAPABILITIES_NETWORK_ENABLED                    0x02

/* The device has a readable serial number. [stub] */
#define ANTW_CAPABILITIES_SERIAL_NUMBER_ENABLED              0x08

/* MESG_CHANNEL_RADIO_TX_POWER (0x60) is supported. [stub] */
#define ANTW_CAPABILITIES_PER_CHANNEL_TX_POWER_ENABLED       0x10

/* MESG_SET_LP_SEARCH_TIMEOUT is supported. [stub] */
#define ANTW_CAPABILITIES_LOW_PRIORITY_SEARCH_ENABLED        0x20

/* SensRcore scripting. [rev5.1 sec 9.5.7.4 + verify:sdk-ant-shim] */
#define ANTW_CAPABILITIES_SCRIPT_ENABLED                     0x40

/*
 * Inclusion/exclusion lists (0x59, 0x5A). [rev5.1 sec 9.5.7.4 + verify:sdk-
 * ant-shim]
 */
#define ANTW_CAPABILITIES_SEARCH_LIST_ENABLED                0x80

/* byte 4 - advanced_options_2. */
#define ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_2          4

/*
 * MESG_ENABLE_LED_FLASH. Reported OFF here, which is why a host that has
 * ANT_EnableLED never sends 0x68. [readme]
 */
#define ANTW_CAPABILITIES_LED_ENABLED                        0x01

/* Extended output fields - the whole lib config mechanism. [stub] */
#define ANTW_CAPABILITIES_EXT_MESSAGE_ENABLED                0x02

/*
 * MESG_OPEN_RX_SCAN_MODE. Reported OFF in this generated reply, which is why
 * a host that has ANT_OpenRxScanMode never sends 0x5B on this build.
 * radiant_core reports the bit on instead, and as of 2026-08-10 0x5B is
 * bridged there to match - see the 0x5B message row and docs/backends.md.
 * [stub, readme]
 */
#define ANTW_CAPABILITIES_SCAN_MODE_ENABLED                  0x04

/* MESG_PROX_SEARCH_CONFIG. [rev5.1 sec 9.5.7.4 + verify:sdk-ant-shim] */
#define ANTW_CAPABILITIES_PROX_SEARCH_ENABLED                0x10

/* The optional fourth byte of MESG_ASSIGN_CHANNEL. [stub] */
#define ANTW_CAPABILITIES_EXT_ASSIGN_ENABLED                 0x20

/* ANT-FS file system. [rev5.1 sec 9.5.7.4 + verify:sdk-ant-shim] */
#define ANTW_CAPABILITIES_FS_ANTFS_ENABLED                   0x40

/* FIT1 support. [rev5.1 sec 9.5.7.4 + verify:sdk-ant-shim] */
#define ANTW_CAPABILITIES_FIT1_ENABLED                       0x80

/*
 * byte 5 - max_sensrcore_channels. SensRcore scripting channels. Zero
 * everywhere here.
 */
#define ANTW_CAPABILITIES_OFFSET_MAX_SENSRCORE_CHANNELS      5

/*
 * byte 6 - advanced_options_3. The tech-bulletin features. This is the byte
 * ant_features.py walks.
 */
#define ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_3          6

/* MESG_CONFIG_ADV_BURST (0x78) and 24-byte burst packets. [stub, tools] */
#define ANTW_CAPABILITIES_ADVANCED_BURST_ENABLED             0x01

/*
 * MESG_EVENT_BUFFERING_CONFIG (0x74). The one bit this firmware honestly
 * reports as zero, because sdk-ant has no API for it. [tools, readme]
 */
#define ANTW_CAPABILITIES_EVENT_BUFFERING_ENABLED            0x02

/* MESG_EVENT_FILTER_CONFIG (0x79). [stub, tools] */
#define ANTW_CAPABILITIES_EVENT_FILTERING_ENABLED            0x04

/*
 * MESG_HIGH_DUTY_SEARCH_MODE (0x77). Advertised and not bridged, on purpose -
 * see the README. [tools, readme]
 */
#define ANTW_CAPABILITIES_HIGH_DUTY_SEARCH_ENABLED           0x08

/* MESG_ACTIVE_SEARCH_SHARING (0x81). [tools (ant_features)] */
#define ANTW_CAPABILITIES_SEARCH_SHARING_ENABLED             0x10

/*
 * Radio coexistence configuration. No host API reaches it. [tools
 * (ant_features)]
 */
#define ANTW_CAPABILITIES_RADIO_COEX_CONFIG_ENABLED          0x20

/* MESG_SDU_CONFIG / MESG_SDU_SET_MASK (0x7A, 0x7B). [stub, tools] */
#define ANTW_CAPABILITIES_SELECTIVE_DATA_UPDATE_ENABLED      0x40

/*
 * Single-channel encryption (0x7D-0x7F). Advertised whether or not the write
 * side was compiled in: the bit describes what the radio layer can do, not
 * what the bridge chose to expose. [stub, tools]
 */
#define ANTW_CAPABILITIES_ENCRYPTED_CHANNEL_ENABLED          0x80

/*
 * byte 7 - advanced_options_4. Observed as 0x8D on this firmware; the bit
 * names were recovered and BUILD_ASSERTed in src/ant_radio_sdk_ant.c. Seven
 * of the eight are named below; bit 0x80 has no corroborated name and stays
 * unnamed.
 */
#define ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_4          7

/*
 * RF-active notification: an event when the time to the next synchronous RF
 * activity exceeds a configured threshold. Set in the observed reply. [sdk-
 * ant-shim + verify:sdk-ant-shim]
 */
#define ANTW_CAPABILITIES_RFACTIVE_NOTIFICATION_ENABLED      0x01

/*
 * Data filtering. Clear in the observed reply. [sdk-ant-shim + verify:sdk-
 * ant-shim]
 */
#define ANTW_CAPABILITIES_DATA_FILTERING_ENABLED             0x02

/*
 * Search uplink. Set in the observed reply. [sdk-ant-shim + verify:sdk-ant-
 * shim]
 */
#define ANTW_CAPABILITIES_SEARCH_UPLINK_ENABLED              0x04

/*
 * Group transmitter initiation - the master-side meaning of the channel
 * search timeout. Set in the observed reply. [sdk-ant-shim + verify:sdk-ant-
 * shim]
 */
#define ANTW_CAPABILITIES_GROUP_TRANSMITTER_INITIATION_ENABLED 0x08

/*
 * Extended time base: a 4-byte RTC timestamp instead of the 2-byte ANT one.
 * Clear in the observed reply. [sdk-ant-shim + verify:sdk-ant-shim]
 */
#define ANTW_CAPABILITIES_TIME_BASE_ENABLED                  0x10

/*
 * Time sync. Clear in the observed reply. [sdk-ant-shim + verify:sdk-ant-
 * shim]
 */
#define ANTW_CAPABILITIES_TIME_SYNC_ENABLED                  0x20

/*
 * External PA/LNA GPIO control during radio events. Clear in the observed
 * reply. [sdk-ant-shim + verify:sdk-ant-shim]
 */
#define ANTW_CAPABILITIES_PA_LNA_SUPPORT_ENABLED             0x40

/*
 * byte 8 - advanced_options_5. Observed as 0x0F. Three of the four set bits
 * are named below, recovered the same way as byte 7; bit 0x08 is set in the
 * observed reply and has no name in the source the shim could check.
 */
#define ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_5          8

/*
 * Channel start offset - the second parameter of
 * antr_channel_open_with_offset(). Set in the observed reply, which is the
 * capability behind the offset form existing at all. [sdk-ant-shim +
 * verify:sdk-ant-shim]
 */
#define ANTW_CAPABILITIES_CHANNEL_START_OFFSET_ENABLED       0x01

/*
 * Background searching channel uplink. Set in the observed reply. [sdk-ant-
 * shim + verify:sdk-ant-shim]
 */
#define ANTW_CAPABILITIES_SEARCHING_CHANNEL_UPLINK_ENABLED   0x02

/*
 * ID-match fix for a shared-channel RX burst transfer. Set in the observed
 * reply. [sdk-ant-shim + verify:sdk-ant-shim]
 */
#define ANTW_CAPABILITIES_ID_MATCH_FIX_ON_SHARED_CHANNEL_RXBURST 0x04

/* ------------------------------------------------------------------------
 * RadiANT extension messages - NOT ANT protocol
 * Ours, not Garmin's: not in Rev 5.1, not answered by any ANT device.
 * 0xF6-0xFA is reserved for the rest of the family. Semantics live in
 * docs/radiant-security.md; only the numbering is decided in the YAML.
 * These were first proposed at 0xE0-0xE4 and moved: sdk-ant defines
 * MESG_EXT_ID_0 .. MESG_EXT_ID_4 as exactly 0xE0-0xE4, an extended-
 * message-id block selected by MSG_EXT_ID_MASK. Do not re-propose that
 * range. Lib config 0xE0 (ANTW_LIB_CONFIG_ALL_EXT_FIELDS) is a third,
 * unrelated namespace and has not moved - one is a frame's ID field, the
 * other a payload byte of message 0x6E.
 * ------------------------------------------------------------------------ */

/*
 * Configure the two payload transforms on one channel. [0] channel; [1]
 * switch bitmask - bit 0 X_CONF, bit 1 X_AUTH, bit 2 drop an unverified
 * window instead of delivering it (default 0, deliver), bit 3 encrypt the
 * descriptor set (REFUSED in v1 with ANTW_INVALID_PARAMETER_PROVIDED - the
 * descriptor has no counter and therefore no nonce), bits 7..4 reserved must
 * be 0; [2] MAC window W, the literal 2, 4 or 8 (W=1 is reserved for the
 * reliable-command page); [3] secured page range low and [4] high, both
 * bounded to 0x01..0x1F so the descriptor and the ANT+ common pages stay in
 * the clear mechanically rather than by memory. The two switches are
 * independent, not a ladder: X_AUTH alone is the most useful setting in the
 * table. [K4 (docs/radiant-security.md sec 3 and 9)]
 */
#define ANTW_MESG_RADIANT_SEC_CONFIG_ID                      0xF1

/*
 * Install the one 16-byte root key for a channel. [0] channel; [1] key length
 * in bits, 128 and nothing else in v1; [2..17] the key. Everything else -
 * K_enc, K_auth, K_id, K_cmd - is derived from it, so a pairing moves exactly
 * sixteen bytes. WRITE ONLY: there is no read arm anywhere, and MESG_REQUEST
 * for 0xF2 answers ANTW_INVALID_MESSAGE rather than a key. [K4 (docs/radiant-
 * security.md sec 3.4 and 9)]
 */
#define ANTW_MESG_RADIANT_SET_KEY_ID                         0xF2

/*
 * Set or read the epoch and its time anchor. [0] channel; [1] flags, bit 0 =
 * the epoch is coarse real time (minutes since the RadiANT date) rather than
 * a bare ordinal; [2..5] epoch (u32 LE); [6..13] microseconds into that epoch
 * (u64 LE), which is the phase a receiver derives the packet counter from
 * rather than from arrival history. REFUSES an epoch less than or equal to
 * the current one, and refuses epochs near 0xFFFFFFFF so a counter wrap
 * always has headroom. No transform enables until this has been set after a
 * reset: a reboot that restarts the counter under an unchanged epoch is a
 * two-time pad for X_CONF and a full session replay against X_AUTH. [K4
 * (docs/radiant-security.md sec 3.5 and 9)]
 */
#define ANTW_MESG_RADIANT_EPOCH_ID                           0xF3

/*
 * Per-channel security state, requested with MESG_REQUEST (0x4D). [0]
 * channel; [1] switches currently active, as in 0xF1; [2] W; [3] page range
 * low; [4] high; [5..8] epoch (u32 LE); [9..10] the expected packet index,
 * low 16 bits (u16 LE); [11..12] windows verified; [13..14] windows
 * unverified; [15..16] non-broadcast frames dropped for carrying a secured-
 * range page; [17..18] frames dropped as replay or time-inconsistent;
 * [19..20] windows dropped by the deliver policy; [21] epoch advances since
 * the key was installed; [22] the most recent verdict - 0 clear, 1 verified,
 * 2 unverified. All counters are u16 LE and saturate rather than wrapping.
 * This exists so a host that ignores the per-message verdict flag still has
 * an auditable stream: deliver-as-unverified only means something if
 * unverified cannot be silently treated as verified. [K4 (docs/radiant-
 * security.md sec 3.2 and 9)]
 */
#define ANTW_MESG_RADIANT_SEC_STATUS_ID                      0xF4

/*
 * Drive the in-the-clear pairing exchange. [0] channel; [1] sub-command -
 * 0x00 leave pairing mode, 0x01 enter it with a timeout in seconds at [2] (0
 * means the 60 s default), 0x02 supply the host's 32-byte X25519 scalar at
 * [2..33], 0x03 begin the exchange. The reply echoes the sub-command at [1]
 * and carries the local public key or the comparison fingerprint from [3].
 * THE SCALAR COMES FROM THE HOST because the only entropy source on nRF54L is
 * psa_rng/CRACEN and reaching it drags nrf_security into every build; the
 * honest consequence is that a host-less node cannot pair this way. Pairing
 * in the clear is structural rather than an oversight - see docs/radiant-
 * security.md section 7.4. [K4 (docs/radiant-security.md sec 7.4, 8 and 9)]
 */
#define ANTW_MESG_RADIANT_PAIRING_ID                         0xF5

#endif /* ANT_WIRE_H_ */
