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
 * Why every name here carries an ANTW_ prefix
 *
 * This is not house style. sdk-ant's ant_parameters.h defines its error codes as
 * computed expressions - NRF_ANT_ERROR_OFFSET + INVALID_MESSAGE - rather than as
 * literals. C permits a macro to be redefined only with an identical token
 * sequence, so a header of ours that spelled a constant INVALID_MESSAGE could
 * never share a translation unit with sdk-ant's: it is a hard redefinition error,
 * not a warning, and no include order fixes it.
 *
 * The prefix is exactly what makes the two coexist. src/ant_radio_sdk_ant.c is
 * the one file permitted to include both, and it is where every ANTW_* constant
 * is BUILD_ASSERTed against its sdk-ant counterpart. That check costs nothing,
 * runs only where sdk-ant is present - which is the only place it can run - and
 * is why keeping the sdk-ant backend is worth more than a fallback.
 *
 * It also makes src/ant_serial_bridge.c textually free of Garmin's API names,
 * which is not cosmetic either: this is a clean-room rebuild, and the boundary is
 * easier to defend when it is visible in the source.
 *
 * The Python module deliberately does NOT carry the prefix. The collision it
 * solves is a C preprocessor problem, and Python already has module namespaces:
 * `ant_wire.MESG_ASSIGN_CHANNEL_ID` is unambiguous, and keeping the bare spelling
 * makes converting the five tools a matter of deleting a local definition and
 * adding an import, with no renaming at all.
 *
 * Every ANTW_* name below is now an alias of radiant_core's own
 * RADIANT_WIRE_* definition (radiant_core/include/radiant_core/radiant_wire.h) -
 * not a second, independently-generated copy of the same value. That is what
 * lets radiant_core reach no path outside itself for a wire constant while this
 * file, and every ANTW_* call site in this application, stays untouched.
 *
 * Nothing here describes the on-air link layer. This header is about
 * bytes on a USB bulk endpoint or a UART, and about nothing else.
 */

#ifndef ANT_WIRE_H_
#define ANT_WIRE_H_

#include <radiant_core/radiant_wire.h>

/* ------------------------------------------------------------------------
 * Framing
 * The checksum is the XOR of every byte of the frame from the SYNC byte
 * through the last payload byte, inclusive. Leaving SYNC out of the sum
 * yields a value that differs by exactly 0xA4, so two implementations that
 * both omit it agree with each other perfectly and with nobody else.
 * ------------------------------------------------------------------------ */

#define ANTW_SYNC_TX                                         RADIANT_WIRE_SYNC_TX

#define ANTW_SYNC_RX                                         RADIANT_WIRE_SYNC_RX

#define ANTW_MSG_OVERHEAD                                    RADIANT_WIRE_MSG_OVERHEAD

#define ANTW_MAX_SIZE_VALUE                                  RADIANT_WIRE_MAX_SIZE_VALUE

#define ANTW_MAX_DATA_SIZE                                   RADIANT_WIRE_MAX_DATA_SIZE

#define ANTW_MESG_MAX_SIZE                                   RADIANT_WIRE_MESG_MAX_SIZE

#define ANTW_MAX_FRAME_SIZE                                  RADIANT_WIRE_MAX_FRAME_SIZE

#define ANTW_ANT_MAX_PAYLOAD_SIZE                            RADIANT_WIRE_ANT_MAX_PAYLOAD_SIZE

#define ANTW_CHANNEL_NUM_SIZE                                RADIANT_WIRE_CHANNEL_NUM_SIZE

#define ANTW_ADV_BURST_BLOCK_MAX                             RADIANT_WIRE_ADV_BURST_BLOCK_MAX

/* ------------------------------------------------------------------------
 * Message identifiers
 * The ID byte of a frame. Direction, payload length and whether the bridge
 * implements each one are tabulated in docs/ant-serial-protocol.md.
 * ------------------------------------------------------------------------ */

#define ANTW_MESG_INVALID_ID                                 RADIANT_WIRE_MESG_INVALID_ID

#define ANTW_MESG_EVENT_ID                                   RADIANT_WIRE_MESG_EVENT_ID

#define ANTW_MESG_VERSION_ID                                 RADIANT_WIRE_MESG_VERSION_ID

#define ANTW_MESG_RESPONSE_EVENT_ID                          RADIANT_WIRE_MESG_RESPONSE_EVENT_ID

#define ANTW_MESG_UNASSIGN_CHANNEL_ID                        RADIANT_WIRE_MESG_UNASSIGN_CHANNEL_ID

#define ANTW_MESG_ASSIGN_CHANNEL_ID                          RADIANT_WIRE_MESG_ASSIGN_CHANNEL_ID

#define ANTW_MESG_CHANNEL_MESG_PERIOD_ID                     RADIANT_WIRE_MESG_CHANNEL_MESG_PERIOD_ID

#define ANTW_MESG_CHANNEL_SEARCH_TIMEOUT_ID                  RADIANT_WIRE_MESG_CHANNEL_SEARCH_TIMEOUT_ID

#define ANTW_MESG_CHANNEL_RADIO_FREQ_ID                      RADIANT_WIRE_MESG_CHANNEL_RADIO_FREQ_ID

#define ANTW_MESG_NETWORK_KEY_ID                             RADIANT_WIRE_MESG_NETWORK_KEY_ID

#define ANTW_MESG_RADIO_TX_POWER_ID                          RADIANT_WIRE_MESG_RADIO_TX_POWER_ID

#define ANTW_MESG_RADIO_CW_MODE_ID                           RADIANT_WIRE_MESG_RADIO_CW_MODE_ID

#define ANTW_MESG_SEARCH_WAVEFORM_ID                         RADIANT_WIRE_MESG_SEARCH_WAVEFORM_ID

#define ANTW_MESG_SYSTEM_RESET_ID                            RADIANT_WIRE_MESG_SYSTEM_RESET_ID

#define ANTW_MESG_OPEN_CHANNEL_ID                            RADIANT_WIRE_MESG_OPEN_CHANNEL_ID

#define ANTW_MESG_CLOSE_CHANNEL_ID                           RADIANT_WIRE_MESG_CLOSE_CHANNEL_ID

#define ANTW_MESG_REQUEST_ID                                 RADIANT_WIRE_MESG_REQUEST_ID

#define ANTW_MESG_BROADCAST_DATA_ID                          RADIANT_WIRE_MESG_BROADCAST_DATA_ID

#define ANTW_MESG_ACKNOWLEDGED_DATA_ID                       RADIANT_WIRE_MESG_ACKNOWLEDGED_DATA_ID

#define ANTW_MESG_BURST_DATA_ID                              RADIANT_WIRE_MESG_BURST_DATA_ID

#define ANTW_MESG_CHANNEL_ID_ID                              RADIANT_WIRE_MESG_CHANNEL_ID_ID

#define ANTW_MESG_CHANNEL_STATUS_ID                          RADIANT_WIRE_MESG_CHANNEL_STATUS_ID

#define ANTW_MESG_RADIO_CW_INIT_ID                           RADIANT_WIRE_MESG_RADIO_CW_INIT_ID

#define ANTW_MESG_CAPABILITIES_ID                            RADIANT_WIRE_MESG_CAPABILITIES_ID

#define ANTW_MESG_CHANNEL_CRC_MODE_ID                        RADIANT_WIRE_MESG_CHANNEL_CRC_MODE_ID

#define ANTW_MESG_ID_LIST_ADD_ID                             RADIANT_WIRE_MESG_ID_LIST_ADD_ID

#define ANTW_MESG_ID_LIST_CONFIG_ID                          RADIANT_WIRE_MESG_ID_LIST_CONFIG_ID

#define ANTW_MESG_OPEN_RX_SCAN_MODE_ID                       RADIANT_WIRE_MESG_OPEN_RX_SCAN_MODE_ID

#define ANTW_MESG_EXT_BROADCAST_DATA_ID                      RADIANT_WIRE_MESG_EXT_BROADCAST_DATA_ID

#define ANTW_MESG_EXT_ACKNOWLEDGED_DATA_ID                   RADIANT_WIRE_MESG_EXT_ACKNOWLEDGED_DATA_ID

#define ANTW_MESG_EXT_BURST_DATA_ID                          RADIANT_WIRE_MESG_EXT_BURST_DATA_ID

#define ANTW_MESG_CHANNEL_RADIO_TX_POWER_ID                  RADIANT_WIRE_MESG_CHANNEL_RADIO_TX_POWER_ID

#define ANTW_MESG_GET_SERIAL_NUM_ID                          RADIANT_WIRE_MESG_GET_SERIAL_NUM_ID

#define ANTW_MESG_SET_LP_SEARCH_TIMEOUT_ID                   RADIANT_WIRE_MESG_SET_LP_SEARCH_TIMEOUT_ID

#define ANTW_MESG_SERIAL_NUM_SET_CHANNEL_ID_ID               RADIANT_WIRE_MESG_SERIAL_NUM_SET_CHANNEL_ID_ID

#define ANTW_MESG_RX_EXT_MESGS_ENABLE_ID                     RADIANT_WIRE_MESG_RX_EXT_MESGS_ENABLE_ID

#define ANTW_MESG_ENABLE_LED_FLASH_ID                        RADIANT_WIRE_MESG_ENABLE_LED_FLASH_ID

#define ANTW_MESG_XTAL_ENABLE_ID                             RADIANT_WIRE_MESG_XTAL_ENABLE_ID

#define ANTW_MESG_ANTLIB_CONFIG_ID                           RADIANT_WIRE_MESG_ANTLIB_CONFIG_ID

#define ANTW_MESG_STARTUP_MESG_ID                            RADIANT_WIRE_MESG_STARTUP_MESG_ID

#define ANTW_MESG_AUTO_FREQ_CONFIG_ID                        RADIANT_WIRE_MESG_AUTO_FREQ_CONFIG_ID

#define ANTW_MESG_PROX_SEARCH_CONFIG_ID                      RADIANT_WIRE_MESG_PROX_SEARCH_CONFIG_ID

#define ANTW_MESG_ADV_BURST_DATA_ID                          RADIANT_WIRE_MESG_ADV_BURST_DATA_ID

#define ANTW_MESG_EVENT_BUFFERING_CONFIG_ID                  RADIANT_WIRE_MESG_EVENT_BUFFERING_CONFIG_ID

#define ANTW_MESG_SET_SEARCH_CH_PRIORITY_ID                  RADIANT_WIRE_MESG_SET_SEARCH_CH_PRIORITY_ID

#define ANTW_MESG_HIGH_DUTY_SEARCH_MODE_ID                   RADIANT_WIRE_MESG_HIGH_DUTY_SEARCH_MODE_ID

#define ANTW_MESG_CONFIG_ADV_BURST_ID                        RADIANT_WIRE_MESG_CONFIG_ADV_BURST_ID

#define ANTW_MESG_EVENT_FILTER_CONFIG_ID                     RADIANT_WIRE_MESG_EVENT_FILTER_CONFIG_ID

#define ANTW_MESG_SDU_CONFIG_ID                              RADIANT_WIRE_MESG_SDU_CONFIG_ID

#define ANTW_MESG_SDU_SET_MASK_ID                            RADIANT_WIRE_MESG_SDU_SET_MASK_ID

#define ANTW_MESG_USER_CONFIG_PAGE_ID                        RADIANT_WIRE_MESG_USER_CONFIG_PAGE_ID

#define ANTW_MESG_ENCRYPT_ENABLE_ID                          RADIANT_WIRE_MESG_ENCRYPT_ENABLE_ID

#define ANTW_MESG_SET_ENCRYPT_KEY_ID                         RADIANT_WIRE_MESG_SET_ENCRYPT_KEY_ID

#define ANTW_MESG_SET_ENCRYPT_INFO_ID                        RADIANT_WIRE_MESG_SET_ENCRYPT_INFO_ID

#define ANTW_MESG_ACTIVE_SEARCH_SHARING_ID                   RADIANT_WIRE_MESG_ACTIVE_SEARCH_SHARING_ID

#define ANTW_MESG_ECS_ENABLE_ID                              RADIANT_WIRE_MESG_ECS_ENABLE_ID

#define ANTW_MESG_PENDING_TRANSMIT_CLEAR_ID                  RADIANT_WIRE_MESG_PENDING_TRANSMIT_CLEAR_ID

#define ANTW_MESG_SERIAL_ERROR_ID                            RADIANT_WIRE_MESG_SERIAL_ERROR_ID

/* ------------------------------------------------------------------------
 * Reply sizes
 * The LEN byte the bridge puts on each reply it composes itself. Where a
 * request reply is a different shape from the command sharing its id, the
 * message's reply_len block in protocol/ant_wire.yaml points at the
 * constant here rather than restating the number.
 * ------------------------------------------------------------------------ */

#define ANTW_MESG_RESPONSE_EVENT_SIZE                        RADIANT_WIRE_MESG_RESPONSE_EVENT_SIZE

#define ANTW_MESG_STARTUP_MESG_SIZE                          RADIANT_WIRE_MESG_STARTUP_MESG_SIZE

#define ANTW_MESG_CAPABILITIES_SIZE                          RADIANT_WIRE_MESG_CAPABILITIES_SIZE

#define ANTW_MESG_VERSION_SIZE                               RADIANT_WIRE_MESG_VERSION_SIZE

#define ANTW_MESG_CHANNEL_STATUS_SIZE                        RADIANT_WIRE_MESG_CHANNEL_STATUS_SIZE

#define ANTW_MESG_CHANNEL_ID_SIZE                            RADIANT_WIRE_MESG_CHANNEL_ID_SIZE

#define ANTW_MESG_CHANNEL_MESG_PERIOD_SIZE                   RADIANT_WIRE_MESG_CHANNEL_MESG_PERIOD_SIZE

#define ANTW_MESG_CHANNEL_RADIO_FREQ_SIZE                    RADIANT_WIRE_MESG_CHANNEL_RADIO_FREQ_SIZE

#define ANTW_MESG_CHANNEL_CRC_MODE_SIZE                      RADIANT_WIRE_MESG_CHANNEL_CRC_MODE_SIZE

#define ANTW_MESG_SET_SEARCH_CH_PRIORITY_SIZE                RADIANT_WIRE_MESG_SET_SEARCH_CH_PRIORITY_SIZE

#define ANTW_MESG_PENDING_TRANSMIT_GET_SIZE                  RADIANT_WIRE_MESG_PENDING_TRANSMIT_GET_SIZE

#define ANTW_MESG_ANTLIB_CONFIG_SIZE                         RADIANT_WIRE_MESG_ANTLIB_CONFIG_SIZE

#define ANTW_MESG_ACTIVE_SEARCH_SHARING_REQ_SIZE             RADIANT_WIRE_MESG_ACTIVE_SEARCH_SHARING_REQ_SIZE

#define ANTW_MESG_EVENT_FILTER_CONFIG_REQ_SIZE               RADIANT_WIRE_MESG_EVENT_FILTER_CONFIG_REQ_SIZE

#define ANTW_MESG_CONFIG_ADV_BURST_REQ_CONFIG_SIZE           RADIANT_WIRE_MESG_CONFIG_ADV_BURST_REQ_CONFIG_SIZE

#define ANTW_MESG_CONFIG_ADV_BURST_REQ_CAPABILITIES_SIZE     RADIANT_WIRE_MESG_CONFIG_ADV_BURST_REQ_CAPABILITIES_SIZE

#define ANTW_MESG_CONFIG_ENCRYPT_REQ_CAPABILITIES_SIZE       RADIANT_WIRE_MESG_CONFIG_ENCRYPT_REQ_CAPABILITIES_SIZE

#define ANTW_MESG_CONFIG_ENCRYPT_REQ_CONFIG_ID_SIZE          RADIANT_WIRE_MESG_CONFIG_ENCRYPT_REQ_CONFIG_ID_SIZE

#define ANTW_MESG_CONFIG_ENCRYPT_REQ_CONFIG_USER_DATA_SIZE   RADIANT_WIRE_MESG_CONFIG_ENCRYPT_REQ_CONFIG_USER_DATA_SIZE

/* ------------------------------------------------------------------------
 * Channel types
 * Byte 1 of MESG_ASSIGN_CHANNEL.
 * ------------------------------------------------------------------------ */

#define ANTW_CHANNEL_TYPE_SLAVE                              RADIANT_WIRE_CHANNEL_TYPE_SLAVE

#define ANTW_CHANNEL_TYPE_MASTER                             RADIANT_WIRE_CHANNEL_TYPE_MASTER

#define ANTW_CHANNEL_TYPE_SHARED_SLAVE                       RADIANT_WIRE_CHANNEL_TYPE_SHARED_SLAVE

#define ANTW_CHANNEL_TYPE_SHARED_MASTER                      RADIANT_WIRE_CHANNEL_TYPE_SHARED_MASTER

#define ANTW_CHANNEL_TYPE_SLAVE_RX_ONLY                      RADIANT_WIRE_CHANNEL_TYPE_SLAVE_RX_ONLY

#define ANTW_CHANNEL_TYPE_MASTER_TX_ONLY                     RADIANT_WIRE_CHANNEL_TYPE_MASTER_TX_ONLY

/* ------------------------------------------------------------------------
 * Extended assignment
 * Optional byte 3 of MESG_ASSIGN_CHANNEL. The bridge passes it straight
 * through.
 * ------------------------------------------------------------------------ */

#define ANTW_EXT_PARAM_ALWAYS_SEARCH                         RADIANT_WIRE_EXT_PARAM_ALWAYS_SEARCH

#define ANTW_EXT_PARAM_FREQUENCY_AGILITY                     RADIANT_WIRE_EXT_PARAM_FREQUENCY_AGILITY

#define ANTW_EXT_PARAM_AUTO_SHARED_SLAVE                     RADIANT_WIRE_EXT_PARAM_AUTO_SHARED_SLAVE

#define ANTW_EXT_PARAM_FAST_INITIATION_MODE                  RADIANT_WIRE_EXT_PARAM_FAST_INITIATION_MODE

#define ANTW_EXT_PARAM_ASYNC_TX_MODE                         RADIANT_WIRE_EXT_PARAM_ASYNC_TX_MODE

/* ------------------------------------------------------------------------
 * Channel status
 * Low two bits of byte 1 of a MESG_CHANNEL_STATUS reply.
 * ------------------------------------------------------------------------ */

#define ANTW_STATUS_UNASSIGNED_CHANNEL                       RADIANT_WIRE_STATUS_UNASSIGNED_CHANNEL

#define ANTW_STATUS_ASSIGNED_CHANNEL                         RADIANT_WIRE_STATUS_ASSIGNED_CHANNEL

#define ANTW_STATUS_SEARCHING_CHANNEL                        RADIANT_WIRE_STATUS_SEARCHING_CHANNEL

#define ANTW_STATUS_TRACKING_CHANNEL                         RADIANT_WIRE_STATUS_TRACKING_CHANNEL

#define ANTW_STATUS_CHANNEL_STATE_MASK                       RADIANT_WIRE_STATUS_CHANNEL_STATE_MASK

/* ------------------------------------------------------------------------
 * Radio transmit power levels
 * Byte 1 of MESG_RADIO_TX_POWER (device-wide) and byte 1 of
 * MESG_CHANNEL_RADIO_TX_POWER (per channel). The dBm figures are the
 * levels a retail ANT stick exposes; what a given radio actually emits is
 * a property of the part.
 * ------------------------------------------------------------------------ */

#define ANTW_RADIO_TX_POWER_LVL_0                            RADIANT_WIRE_RADIO_TX_POWER_LVL_0

#define ANTW_RADIO_TX_POWER_LVL_1                            RADIANT_WIRE_RADIO_TX_POWER_LVL_1

#define ANTW_RADIO_TX_POWER_LVL_2                            RADIANT_WIRE_RADIO_TX_POWER_LVL_2

#define ANTW_RADIO_TX_POWER_LVL_3                            RADIANT_WIRE_RADIO_TX_POWER_LVL_3

#define ANTW_RADIO_TX_POWER_LVL_4                            RADIANT_WIRE_RADIO_TX_POWER_LVL_4

#define ANTW_RADIO_TX_POWER_LVL_5                            RADIANT_WIRE_RADIO_TX_POWER_LVL_5

#define ANTW_RADIO_TX_POWER_LVL_CUSTOM                       RADIANT_WIRE_RADIO_TX_POWER_LVL_CUSTOM

/* ------------------------------------------------------------------------
 * Library configuration bits
 * Byte 1 of MESG_ANTLIB_CONFIG. Each bit appends one field to every
 * received data message; the fields arrive in bit order behind a flag
 * byte. Sending 0x00 clears all of them - there is no separate clear
 * message on the wire.
 * ------------------------------------------------------------------------ */

#define ANTW_LIB_CONFIG_RADIO_CONFIG_ALWAYS                  RADIANT_WIRE_LIB_CONFIG_RADIO_CONFIG_ALWAYS

#define ANTW_LIB_CONFIG_MESG_OUT_INC_TIME_STAMP              RADIANT_WIRE_LIB_CONFIG_MESG_OUT_INC_TIME_STAMP

#define ANTW_LIB_CONFIG_MESG_OUT_INC_RSSI                    RADIANT_WIRE_LIB_CONFIG_MESG_OUT_INC_RSSI

#define ANTW_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID               RADIANT_WIRE_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID

/*
 * ANTW_LIB_CONFIG_DEVICE_ID_ONLY is a Python-side alias of an identical value
 * and is deliberately not defined here. Alias: the narrow setting ant_scan.py
 * and ant_session.py use - identity, nothing else.
 */

#define ANTW_LIB_CONFIG_ALL_EXT_FIELDS                       RADIANT_WIRE_LIB_CONFIG_ALL_EXT_FIELDS

#define ANTW_LIB_CONFIG_MASK_ALL                             RADIANT_WIRE_LIB_CONFIG_MASK_ALL

/* ------------------------------------------------------------------------
 * Extended message flag bits
 * Byte 9 of a received data message - immediately after the 8-byte payload
 * - when any lib config bit is set. The fields follow in a fixed order,
 * but each is present only if its flag bit is set, so an offset is only
 * correct relative to which of the earlier ones turned up. Reading RSSI at
 * a fixed offset works right up until a run is made without the channel
 * id, and then quietly reports the device number as a signal strength.
 * ------------------------------------------------------------------------ */

#define ANTW_EXT_FLAG_CHANNEL_ID                             RADIANT_WIRE_EXT_FLAG_CHANNEL_ID

#define ANTW_EXT_FLAG_RSSI                                   RADIANT_WIRE_EXT_FLAG_RSSI

#define ANTW_EXT_FLAG_RX_TIMESTAMP                           RADIANT_WIRE_EXT_FLAG_RX_TIMESTAMP

/* ------------------------------------------------------------------------
 * RSSI measurement types
 * Byte 0 of the RSSI extended field.
 * ------------------------------------------------------------------------ */

#define ANTW_RSSI_MEASUREMENT_TYPE_DBM                       RADIANT_WIRE_RSSI_MEASUREMENT_TYPE_DBM

/* ------------------------------------------------------------------------
 * Startup message reasons
 * The single payload byte of MESG_STARTUP_MESG. Zero is not a bit: it
 * means a power-on or a command reset with no other flag set.
 * ------------------------------------------------------------------------ */

#define ANTW_STARTUP_POWER_ON_RESET                          RADIANT_WIRE_STARTUP_POWER_ON_RESET

#define ANTW_STARTUP_HARDWARE_RESET_LINE                     RADIANT_WIRE_STARTUP_HARDWARE_RESET_LINE

#define ANTW_STARTUP_WATCH_DOG_RESET                         RADIANT_WIRE_STARTUP_WATCH_DOG_RESET

#define ANTW_STARTUP_COMMAND_RESET                           RADIANT_WIRE_STARTUP_COMMAND_RESET

#define ANTW_STARTUP_SYNCHRONOUS_RESET                       RADIANT_WIRE_STARTUP_SYNCHRONOUS_RESET

#define ANTW_STARTUP_SUSPEND_RESET                           RADIANT_WIRE_STARTUP_SUSPEND_RESET

/* ------------------------------------------------------------------------
 * Channel event codes
 * Byte 2 of a RESPONSE_EVENT whose byte 1 is MESG_EVENT_ID (0x01). These
 * arrive unsolicited; the response codes below arrive in answer to a
 * command. They share one number space.
 * ------------------------------------------------------------------------ */

#define ANTW_EVENT_RX_SEARCH_TIMEOUT                         RADIANT_WIRE_EVENT_RX_SEARCH_TIMEOUT

#define ANTW_EVENT_RX_FAIL                                   RADIANT_WIRE_EVENT_RX_FAIL

#define ANTW_EVENT_TX                                        RADIANT_WIRE_EVENT_TX

#define ANTW_EVENT_TRANSFER_RX_FAILED                        RADIANT_WIRE_EVENT_TRANSFER_RX_FAILED

#define ANTW_EVENT_TRANSFER_TX_COMPLETED                     RADIANT_WIRE_EVENT_TRANSFER_TX_COMPLETED

#define ANTW_EVENT_TRANSFER_TX_FAILED                        RADIANT_WIRE_EVENT_TRANSFER_TX_FAILED

#define ANTW_EVENT_CHANNEL_CLOSED                            RADIANT_WIRE_EVENT_CHANNEL_CLOSED

#define ANTW_EVENT_RX_FAIL_GO_TO_SEARCH                      RADIANT_WIRE_EVENT_RX_FAIL_GO_TO_SEARCH

#define ANTW_EVENT_CHANNEL_COLLISION                         RADIANT_WIRE_EVENT_CHANNEL_COLLISION

#define ANTW_EVENT_TRANSFER_TX_START                         RADIANT_WIRE_EVENT_TRANSFER_TX_START

#define ANTW_EVENT_RX_DATA_OVERFLOW                          RADIANT_WIRE_EVENT_RX_DATA_OVERFLOW

#define ANTW_EVENT_TRANSFER_NEXT_DATA_BLOCK                  RADIANT_WIRE_EVENT_TRANSFER_NEXT_DATA_BLOCK

/* ------------------------------------------------------------------------
 * Response codes
 * Byte 2 of a RESPONSE_EVENT answering a command. Same number space as the
 * event codes.
 * ------------------------------------------------------------------------ */

#define ANTW_RESPONSE_NO_ERROR                               RADIANT_WIRE_RESPONSE_NO_ERROR

#define ANTW_CHANNEL_IN_WRONG_STATE                          RADIANT_WIRE_CHANNEL_IN_WRONG_STATE

#define ANTW_CHANNEL_NOT_OPENED                              RADIANT_WIRE_CHANNEL_NOT_OPENED

#define ANTW_CHANNEL_ID_NOT_SET                              RADIANT_WIRE_CHANNEL_ID_NOT_SET

#define ANTW_CLOSE_ALL_CHANNELS                              RADIANT_WIRE_CLOSE_ALL_CHANNELS

#define ANTW_TRANSFER_IN_PROGRESS                            RADIANT_WIRE_TRANSFER_IN_PROGRESS

#define ANTW_TRANSFER_SEQUENCE_NUMBER_ERROR                  RADIANT_WIRE_TRANSFER_SEQUENCE_NUMBER_ERROR

#define ANTW_TRANSFER_IN_ERROR                               RADIANT_WIRE_TRANSFER_IN_ERROR

#define ANTW_TRANSFER_BUSY                                   RADIANT_WIRE_TRANSFER_BUSY

#define ANTW_MESSAGE_SIZE_EXCEEDS_LIMIT                      RADIANT_WIRE_MESSAGE_SIZE_EXCEEDS_LIMIT

#define ANTW_INVALID_MESSAGE                                 RADIANT_WIRE_INVALID_MESSAGE

#define ANTW_INVALID_NETWORK_NUMBER                          RADIANT_WIRE_INVALID_NETWORK_NUMBER

#define ANTW_INVALID_LIST_ID                                 RADIANT_WIRE_INVALID_LIST_ID

#define ANTW_INVALID_SCAN_TX_CHANNEL                         RADIANT_WIRE_INVALID_SCAN_TX_CHANNEL

#define ANTW_INVALID_PARAMETER_PROVIDED                      RADIANT_WIRE_INVALID_PARAMETER_PROVIDED

#define ANTW_EVENT_SERIAL_QUE_OVERFLOW                       RADIANT_WIRE_EVENT_SERIAL_QUE_OVERFLOW

#define ANTW_EVENT_QUE_OVERFLOW                              RADIANT_WIRE_EVENT_QUE_OVERFLOW

/* ------------------------------------------------------------------------
 * Advanced burst configuration
 * Fields of MESG_CONFIG_ADV_BURST.
 * ------------------------------------------------------------------------ */

#define ANTW_ADV_BURST_MODE_DISABLE                          RADIANT_WIRE_ADV_BURST_MODE_DISABLE

#define ANTW_ADV_BURST_MODE_ENABLE                           RADIANT_WIRE_ADV_BURST_MODE_ENABLE

#define ANTW_ADV_BURST_MODES_SIZE_8_BYTES                    RADIANT_WIRE_ADV_BURST_MODES_SIZE_8_BYTES

#define ANTW_ADV_BURST_MODES_SIZE_16_BYTES                   RADIANT_WIRE_ADV_BURST_MODES_SIZE_16_BYTES

#define ANTW_ADV_BURST_MODES_SIZE_24_BYTES                   RADIANT_WIRE_ADV_BURST_MODES_SIZE_24_BYTES

#define ANTW_ADV_BURST_MODES_FREQ_HOP                        RADIANT_WIRE_ADV_BURST_MODES_FREQ_HOP

/* ------------------------------------------------------------------------
 * Selective data update
 * Values of the mask-config byte of MESG_SDU_CONFIG.
 * ------------------------------------------------------------------------ */

#define ANTW_INVALID_SDU_MASK                                RADIANT_WIRE_INVALID_SDU_MASK

#define ANTW_SDU_MASK_ACK_CONFIG_BIT                         RADIANT_WIRE_SDU_MASK_ACK_CONFIG_BIT

/* ------------------------------------------------------------------------
 * Encryption
 * Info types and modes for MESG_ENCRYPT_ENABLE / MESG_SET_ENCRYPT_INFO.
 * The set and get info types do not line up: set 0 is the crypto id and
 * get 0 is the supported mode, so a round trip writes with one number and
 * reads with the next. That is not a bug in the tools.
 * ------------------------------------------------------------------------ */

#define ANTW_ENCRYPTION_DISABLED_MODE                        RADIANT_WIRE_ENCRYPTION_DISABLED_MODE

#define ANTW_ENCRYPTION_INFO_SET_CRYPTO_ID                   RADIANT_WIRE_ENCRYPTION_INFO_SET_CRYPTO_ID

#define ANTW_ENCRYPTION_INFO_SET_CUSTOM_USER_DATA            RADIANT_WIRE_ENCRYPTION_INFO_SET_CUSTOM_USER_DATA

#define ANTW_ENCRYPTION_INFO_SET_RNG_SEED                    RADIANT_WIRE_ENCRYPTION_INFO_SET_RNG_SEED

#define ANTW_ENCRYPTION_INFO_GET_SUPPORTED_MODE              RADIANT_WIRE_ENCRYPTION_INFO_GET_SUPPORTED_MODE

#define ANTW_ENCRYPTION_INFO_GET_CRYPTO_ID                   RADIANT_WIRE_ENCRYPTION_INFO_GET_CRYPTO_ID

#define ANTW_ENCRYPTION_INFO_GET_CUSTOM_USER_DATA            RADIANT_WIRE_ENCRYPTION_INFO_GET_CUSTOM_USER_DATA

#define ANTW_ENCRYPTION_USER_DATA_SIZE                       RADIANT_WIRE_ENCRYPTION_USER_DATA_SIZE

#define ANTW_ENCRYPTION_KEY_SIZE                             RADIANT_WIRE_ENCRYPTION_KEY_SIZE

#define ANTW_MAX_SUPPORTED_ENCRYPTION_MODE                   RADIANT_WIRE_MAX_SUPPORTED_ENCRYPTION_MODE

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

#define ANTW_BURST_HEADER_CHANNEL_MASK                       RADIANT_WIRE_BURST_HEADER_CHANNEL_MASK

#define ANTW_BURST_HEADER_SEQ_SHIFT                          RADIANT_WIRE_BURST_HEADER_SEQ_SHIFT

#define ANTW_BURST_HEADER_SEQ_MASK                           RADIANT_WIRE_BURST_HEADER_SEQ_MASK

#define ANTW_BURST_HEADER_LAST                               RADIANT_WIRE_BURST_HEADER_LAST

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

#define ANTW_RADIANT_PAIR_LEAVE                              RADIANT_WIRE_RADIANT_PAIR_LEAVE

#define ANTW_RADIANT_PAIR_ENTER                              RADIANT_WIRE_RADIANT_PAIR_ENTER

#define ANTW_RADIANT_PAIR_SCALAR                             RADIANT_WIRE_RADIANT_PAIR_SCALAR

#define ANTW_RADIANT_PAIR_EXCHANGE                           RADIANT_WIRE_RADIANT_PAIR_EXCHANGE

/* ------------------------------------------------------------------------
 * Capabilities reply (MESG_CAPABILITIES)
 * 9 bytes. Observed from this firmware: 080800b23200fd8d0f. Byte and bit
 * layout in docs/ant-serial-protocol.md.
 * ------------------------------------------------------------------------ */

#define ANTW_CAPABILITIES_OFFSET_MAX_CHANNELS                RADIANT_WIRE_CAPABILITIES_OFFSET_MAX_CHANNELS

#define ANTW_CAPABILITIES_OFFSET_MAX_NETWORKS                RADIANT_WIRE_CAPABILITIES_OFFSET_MAX_NETWORKS

#define ANTW_CAPABILITIES_OFFSET_STANDARD_OPTIONS            RADIANT_WIRE_CAPABILITIES_OFFSET_STANDARD_OPTIONS

#define ANTW_CAPABILITIES_NO_RECEIVE_CHANNELS                RADIANT_WIRE_CAPABILITIES_NO_RECEIVE_CHANNELS

#define ANTW_CAPABILITIES_NO_TRANSMIT_CHANNELS               RADIANT_WIRE_CAPABILITIES_NO_TRANSMIT_CHANNELS

#define ANTW_CAPABILITIES_NO_RECEIVE_MESSAGES                RADIANT_WIRE_CAPABILITIES_NO_RECEIVE_MESSAGES

#define ANTW_CAPABILITIES_NO_TRANSMIT_MESSAGES               RADIANT_WIRE_CAPABILITIES_NO_TRANSMIT_MESSAGES

#define ANTW_CAPABILITIES_NO_ACKD_MESSAGES                   RADIANT_WIRE_CAPABILITIES_NO_ACKD_MESSAGES

#define ANTW_CAPABILITIES_NO_BURST_MESSAGES                  RADIANT_WIRE_CAPABILITIES_NO_BURST_MESSAGES

#define ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS            RADIANT_WIRE_CAPABILITIES_OFFSET_ADVANCED_OPTIONS

#define ANTW_CAPABILITIES_NETWORK_ENABLED                    RADIANT_WIRE_CAPABILITIES_NETWORK_ENABLED

#define ANTW_CAPABILITIES_SERIAL_NUMBER_ENABLED              RADIANT_WIRE_CAPABILITIES_SERIAL_NUMBER_ENABLED

#define ANTW_CAPABILITIES_PER_CHANNEL_TX_POWER_ENABLED       RADIANT_WIRE_CAPABILITIES_PER_CHANNEL_TX_POWER_ENABLED

#define ANTW_CAPABILITIES_LOW_PRIORITY_SEARCH_ENABLED        RADIANT_WIRE_CAPABILITIES_LOW_PRIORITY_SEARCH_ENABLED

#define ANTW_CAPABILITIES_SCRIPT_ENABLED                     RADIANT_WIRE_CAPABILITIES_SCRIPT_ENABLED

#define ANTW_CAPABILITIES_SEARCH_LIST_ENABLED                RADIANT_WIRE_CAPABILITIES_SEARCH_LIST_ENABLED

#define ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_2          RADIANT_WIRE_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_2

#define ANTW_CAPABILITIES_LED_ENABLED                        RADIANT_WIRE_CAPABILITIES_LED_ENABLED

#define ANTW_CAPABILITIES_EXT_MESSAGE_ENABLED                RADIANT_WIRE_CAPABILITIES_EXT_MESSAGE_ENABLED

#define ANTW_CAPABILITIES_SCAN_MODE_ENABLED                  RADIANT_WIRE_CAPABILITIES_SCAN_MODE_ENABLED

#define ANTW_CAPABILITIES_PROX_SEARCH_ENABLED                RADIANT_WIRE_CAPABILITIES_PROX_SEARCH_ENABLED

#define ANTW_CAPABILITIES_EXT_ASSIGN_ENABLED                 RADIANT_WIRE_CAPABILITIES_EXT_ASSIGN_ENABLED

#define ANTW_CAPABILITIES_FS_ANTFS_ENABLED                   RADIANT_WIRE_CAPABILITIES_FS_ANTFS_ENABLED

#define ANTW_CAPABILITIES_FIT1_ENABLED                       RADIANT_WIRE_CAPABILITIES_FIT1_ENABLED

#define ANTW_CAPABILITIES_OFFSET_MAX_SENSRCORE_CHANNELS      RADIANT_WIRE_CAPABILITIES_OFFSET_MAX_SENSRCORE_CHANNELS

#define ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_3          RADIANT_WIRE_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_3

#define ANTW_CAPABILITIES_ADVANCED_BURST_ENABLED             RADIANT_WIRE_CAPABILITIES_ADVANCED_BURST_ENABLED

#define ANTW_CAPABILITIES_EVENT_BUFFERING_ENABLED            RADIANT_WIRE_CAPABILITIES_EVENT_BUFFERING_ENABLED

#define ANTW_CAPABILITIES_EVENT_FILTERING_ENABLED            RADIANT_WIRE_CAPABILITIES_EVENT_FILTERING_ENABLED

#define ANTW_CAPABILITIES_HIGH_DUTY_SEARCH_ENABLED           RADIANT_WIRE_CAPABILITIES_HIGH_DUTY_SEARCH_ENABLED

#define ANTW_CAPABILITIES_SEARCH_SHARING_ENABLED             RADIANT_WIRE_CAPABILITIES_SEARCH_SHARING_ENABLED

#define ANTW_CAPABILITIES_RADIO_COEX_CONFIG_ENABLED          RADIANT_WIRE_CAPABILITIES_RADIO_COEX_CONFIG_ENABLED

#define ANTW_CAPABILITIES_SELECTIVE_DATA_UPDATE_ENABLED      RADIANT_WIRE_CAPABILITIES_SELECTIVE_DATA_UPDATE_ENABLED

#define ANTW_CAPABILITIES_ENCRYPTED_CHANNEL_ENABLED          RADIANT_WIRE_CAPABILITIES_ENCRYPTED_CHANNEL_ENABLED

#define ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_4          RADIANT_WIRE_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_4

#define ANTW_CAPABILITIES_RFACTIVE_NOTIFICATION_ENABLED      RADIANT_WIRE_CAPABILITIES_RFACTIVE_NOTIFICATION_ENABLED

#define ANTW_CAPABILITIES_DATA_FILTERING_ENABLED             RADIANT_WIRE_CAPABILITIES_DATA_FILTERING_ENABLED

#define ANTW_CAPABILITIES_SEARCH_UPLINK_ENABLED              RADIANT_WIRE_CAPABILITIES_SEARCH_UPLINK_ENABLED

#define ANTW_CAPABILITIES_GROUP_TRANSMITTER_INITIATION_ENABLED RADIANT_WIRE_CAPABILITIES_GROUP_TRANSMITTER_INITIATION_ENABLED

#define ANTW_CAPABILITIES_TIME_BASE_ENABLED                  RADIANT_WIRE_CAPABILITIES_TIME_BASE_ENABLED

#define ANTW_CAPABILITIES_TIME_SYNC_ENABLED                  RADIANT_WIRE_CAPABILITIES_TIME_SYNC_ENABLED

#define ANTW_CAPABILITIES_PA_LNA_SUPPORT_ENABLED             RADIANT_WIRE_CAPABILITIES_PA_LNA_SUPPORT_ENABLED

#define ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_5          RADIANT_WIRE_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_5

#define ANTW_CAPABILITIES_CHANNEL_START_OFFSET_ENABLED       RADIANT_WIRE_CAPABILITIES_CHANNEL_START_OFFSET_ENABLED

#define ANTW_CAPABILITIES_SEARCHING_CHANNEL_UPLINK_ENABLED   RADIANT_WIRE_CAPABILITIES_SEARCHING_CHANNEL_UPLINK_ENABLED

#define ANTW_CAPABILITIES_ID_MATCH_FIX_ON_SHARED_CHANNEL_RXBURST RADIANT_WIRE_CAPABILITIES_ID_MATCH_FIX_ON_SHARED_CHANNEL_RXBURST

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

#define ANTW_MESG_RADIANT_SEC_CONFIG_ID                      RADIANT_WIRE_MESG_RADIANT_SEC_CONFIG_ID

#define ANTW_MESG_RADIANT_SET_KEY_ID                         RADIANT_WIRE_MESG_RADIANT_SET_KEY_ID

#define ANTW_MESG_RADIANT_EPOCH_ID                           RADIANT_WIRE_MESG_RADIANT_EPOCH_ID

#define ANTW_MESG_RADIANT_SEC_STATUS_ID                      RADIANT_WIRE_MESG_RADIANT_SEC_STATUS_ID

#define ANTW_MESG_RADIANT_PAIRING_ID                         RADIANT_WIRE_MESG_RADIANT_PAIRING_ID

#endif /* ANT_WIRE_H_ */
