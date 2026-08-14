/* SPDX-License-Identifier: Apache-2.0 */

/*
 * src/ant_radio.h implemented on Nordic's prebuilt libant.a, from sdk-ant.
 *
 * Two halves. The first is fifty forwarders, almost all a single renamed
 * call; the exceptions (error narrowing, seven const casts, event inversion)
 * are documented where they happen.
 *
 * The second is a BUILD_ASSERT block comparing every ANTW_* constant in
 * src/ant_wire.h against its sdk-ant counterpart. This is the only
 * translation unit permitted to include both headers - the ANTW_ prefix is
 * what makes that legal, since sdk-ant spells its error codes as computed
 * expressions and an unprefixed macro of the same name would be a hard
 * redefinition error. Every YAML value with no witness elsewhere in the repo
 * is confirmed here at compile time. A drifted constant fires the assert by
 * name; a drifted signature just stops the file compiling.
 *
 * Read scope is the clean-room control: writing this file is the only role
 * permitted to read sdk-ant, and only its headers (docs/decisions/0002-clean-
 * room-policy.md). This file is glue against a licensed API, not part of
 * radiant, and must not inform radiant - a clean-room backend is
 * written from src/ant_radio.h and docs/sdk-ant-contract.md only. libant.a
 * itself was never disassembled, inspected or instrumented, and that
 * prohibition does not expire.
 */

#include <stdint.h>

#include <zephyr/toolchain.h>

#include "ant_radio.h"
#include "ant_wire.h"

/* sdk-ant. Everything below this line that is not ANTW_/antr_/ANTR_ comes from
 * here, and nothing else in the firmware includes these.
 */
#include "ant_error.h"
#include "ant_init.h"
#include "ant_interface.h"
#include "ant_parameters.h"

/* ── Error narrowing ───────────────────────────────────────────────────────── */

/*
 * antr_err_t is a uint8_t whose value *is* the wire byte. sdk-ant's ant_err_t
 * is an int32_t carrying NRF_ANT_ERROR_OFFSET (0x4000) added to the wire
 * code. 0x4000 is a multiple of 256, so truncating to the low byte loses
 * nothing - checked below by BUILD_ASSERT for every sdk-ant error code, so a
 * future non-256-aligned offset fails the build rather than putting a wrong
 * byte on the wire.
 *
 * sdk-ant also returns plain negative errnos for misuse the ANT response
 * codes have no spelling for; their low byte is not a meaningful wire code
 * (-22 narrows to 0xEA, unrecognised by any host). Left untouched
 * deliberately: every capture and Zwift session was recorded against this
 * truncation, and tools/ant_conformance.py's pass condition is a
 * byte-identical diff against those captures. Handling the errno family is a
 * contract change in src/ant_radio.h, not a quiet fix here.
 */
static inline antr_err_t wire_err(ant_err_t err)
{
	return (antr_err_t)((uint32_t)err & 0xFFu);
}

/* ── Event inversion ───────────────────────────────────────────────────────── */

/*
 * sdk-ant hands events back through a callback taking ant_evt_t, a
 * four-level nested union. The shim flattens the three fields the bridge
 * needs into struct antr_msg and calls antr_on_message(); nothing above this
 * line sees an sdk-ant type.
 *
 * Lifetime: pointing straight into the event satisfies the "readable until
 * return, backend may reuse after" contract for free, since sdk-ant's work
 * handler owns it as an automatic variable for the whole callback - no copy
 * needed.
 *
 * Serialisation: the contract's "never called concurrently with itself" is
 * satisfied structurally, since sdk-ant drains events on one dedicated work
 * queue.
 */
static void shim_evt_cb(ant_evt_t *p_evt)
{
	const struct antr_msg msg = {
		.id   = p_evt->message.ANT_MESSAGE_ucMesgID,
		.len  = p_evt->message.ANT_MESSAGE_ucSize,
		.data = p_evt->message.ANT_MESSAGE_aucMesgData,
	};

	antr_on_message(&msg);
}

/* ── Initialisation and stack lifecycle ────────────────────────────────────── */

/*
 * The one forwarder that does not narrow its return value: ant_init() reports
 * "no memory for the channel count" as -NRF_ENOMEM, whose low byte would be
 * uninterpretable in main()'s log. src/ant_radio.h says this returns 0 or
 * ANTW_INVALID_PARAMETER_PROVIDED, so it does - harmless since no host exists
 * yet to see the value.
 */
antr_err_t antr_init(void)
{
	if (ant_init() != NRF_ANT_SUCCESS) {
		return ANTW_INVALID_PARAMETER_PROVIDED;
	}

	/* Registered after the stack is up, before this returns - satisfies the
	 * header's "no antr_on_message() before antr_init() returns 0" rule.
	 */
	if (ant_cb_register(shim_evt_cb) != NRF_ANT_SUCCESS) {
		return ANTW_INVALID_PARAMETER_PROVIDED;
	}

	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_stack_reset(void)
{
	return wire_err(ant_stack_reset());
}

/* ── Channel lifecycle ─────────────────────────────────────────────────────── */

antr_err_t antr_channel_assign(uint8_t channel, uint8_t type, uint8_t network,
			       uint8_t ext_assign)
{
	return wire_err(ant_channel_assign(channel, type, network, ext_assign));
}

antr_err_t antr_channel_unassign(uint8_t channel)
{
	return wire_err(ant_channel_unassign(channel));
}

/* The offset form is the only one implemented: sdk-ant's plain
 * ant_channel_open() is a macro over this with CHANNEL_START_OFFSET_NONE, and
 * forwarding to the macro would recurse through our own static inline.
 */
BUILD_ASSERT(CHANNEL_START_OFFSET_NONE == 0,
	     "sdk-ant's no-offset value is no longer 0, so antr_channel_open()'s "
	     "hardcoded 0 in src/ant_radio.h no longer means the same thing");

antr_err_t antr_channel_open_with_offset(uint8_t channel, uint16_t offset)
{
	return wire_err(ant_channel_open_with_offset(channel, offset));
}

antr_err_t antr_channel_close(uint8_t channel)
{
	return wire_err(ant_channel_close(channel));
}

/* ── Channel configuration ─────────────────────────────────────────────────── */

antr_err_t antr_channel_id_set(uint8_t channel, uint16_t device_number,
			       uint8_t device_type, uint8_t trans_type)
{
	return wire_err(ant_channel_id_set(channel, device_number, device_type,
					   trans_type));
}

antr_err_t antr_channel_period_set(uint8_t channel, uint16_t period)
{
	return wire_err(ant_channel_period_set(channel, period));
}

antr_err_t antr_channel_radio_freq_set(uint8_t channel, uint8_t freq)
{
	return wire_err(ant_channel_radio_freq_set(channel, freq));
}

antr_err_t antr_channel_radio_tx_power_set(uint8_t channel, uint8_t level,
					   uint8_t custom)
{
	return wire_err(ant_channel_radio_tx_power_set(channel, level, custom));
}

antr_err_t antr_channel_search_timeout_set(uint8_t channel, uint8_t timeout)
{
	return wire_err(ant_channel_search_timeout_set(channel, timeout));
}

antr_err_t antr_channel_low_priority_rx_search_timeout_set(uint8_t channel,
							   uint8_t timeout)
{
	return wire_err(ant_channel_low_priority_rx_search_timeout_set(channel,
								       timeout));
}

antr_err_t antr_channel_radio_crc_mode_set(uint8_t channel, uint8_t mode)
{
	return wire_err(ant_channel_radio_crc_mode_set(channel, mode));
}

antr_err_t antr_auto_freq_hop_table_set(uint8_t channel, uint8_t freq0,
					uint8_t freq1, uint8_t freq2)
{
	return wire_err(ant_auto_freq_hop_table_set(channel, freq0, freq1,
						    freq2));
}

antr_err_t antr_enhanced_channel_spacing_enable(uint8_t enable)
{
	return wire_err(ant_enhanced_channel_spacing_enable(enable));
}

/* ── Data transfer ─────────────────────────────────────────────────────────── */

/*
 * The const casts start here. Our contract takes const uint8_t * for buffers
 * the callee only reads; sdk-ant takes mutable pointers for all but one
 * (ant_network_address_set()), so seven casts land here instead of scattered
 * through the bridge.
 */
antr_err_t antr_broadcast_message_tx(uint8_t channel, uint8_t size,
				     const uint8_t *data)
{
	return wire_err(ant_broadcast_message_tx(channel, size,
						 (uint8_t *)data));
}

antr_err_t antr_acknowledge_message_tx(uint8_t channel, uint8_t size,
				       const uint8_t *data)
{
	return wire_err(ant_acknowledge_message_tx(channel, size,
						   (uint8_t *)data));
}

/* The segment byte is forwarded untouched; all three ANTR_BURST_SEGMENT_*
 * values are asserted equal to their sdk-ant counterparts below.
 */
antr_err_t antr_burst_tx(uint8_t channel, uint16_t size, uint8_t *data,
			 uint8_t segment)
{
	return wire_err(ant_burst_handler_request(channel, size, data,
						  segment));
}

/* One of the two names in the contract that is not a prefix swap: sdk-ant's is
 * the verb-less ant_pending_transmit(). docs/sdk-ant-contract.md records both.
 */
antr_err_t antr_pending_transmit_get(uint8_t channel, uint8_t *pending)
{
	return wire_err(ant_pending_transmit(channel, pending));
}

antr_err_t antr_pending_transmit_clear(uint8_t channel, uint8_t *success)
{
	return wire_err(ant_pending_transmit_clear(channel, success));
}

/* ── Search behaviour ──────────────────────────────────────────────────────── */

antr_err_t antr_search_waveform_set(uint8_t channel, uint16_t waveform)
{
	return wire_err(ant_search_waveform_set(channel, waveform));
}

antr_err_t antr_prox_search_set(uint8_t channel, uint8_t threshold,
				uint8_t custom)
{
	return wire_err(ant_prox_search_set(channel, threshold, custom));
}

antr_err_t antr_search_channel_priority_set(uint8_t channel, uint8_t priority)
{
	return wire_err(ant_search_channel_priority_set(channel, priority));
}

antr_err_t antr_search_channel_priority_get(uint8_t channel, uint8_t *priority)
{
	return wire_err(ant_search_channel_priority_get(channel, priority));
}

antr_err_t antr_active_search_sharing_cycles_set(uint8_t channel,
						 uint8_t cycles)
{
	return wire_err(ant_active_search_sharing_cycles_set(channel, cycles));
}

antr_err_t antr_active_search_sharing_cycles_get(uint8_t channel,
						 uint8_t *cycles)
{
	return wire_err(ant_active_search_sharing_cycles_get(channel, cycles));
}

antr_err_t antr_id_list_add(uint8_t channel, const uint8_t *device_id,
			    uint8_t index)
{
	return wire_err(ant_id_list_add(channel, (uint8_t *)device_id, index));
}

antr_err_t antr_id_list_config(uint8_t channel, uint8_t size, uint8_t inc_exc)
{
	return wire_err(ant_id_list_config(channel, size, inc_exc));
}

/* ── Networks and keys ─────────────────────────────────────────────────────── */

/* The one buffer-taking function that needs no cast. */
antr_err_t antr_network_address_set(uint8_t network, const uint8_t *key)
{
	return wire_err(ant_network_address_set(network, key));
}

/* ── Library configuration ─────────────────────────────────────────────────── */

antr_err_t antr_lib_config_set(uint8_t config)
{
	return wire_err(ant_lib_config_set(config));
}

antr_err_t antr_lib_config_clear(uint8_t config)
{
	return wire_err(ant_lib_config_clear(config));
}

antr_err_t antr_lib_config_get(uint8_t *config)
{
	return wire_err(ant_lib_config_get(config));
}

antr_err_t antr_event_filtering_set(uint16_t filter)
{
	return wire_err(ant_event_filtering_set(filter));
}

antr_err_t antr_event_filtering_get(uint16_t *filter)
{
	return wire_err(ant_event_filtering_get(filter));
}

antr_err_t antr_adv_burst_config_set(const uint8_t *config, uint8_t size)
{
	return wire_err(ant_adv_burst_config_set((uint8_t *)config, size));
}

antr_err_t antr_adv_burst_config_get(uint8_t request_type, uint8_t *config)
{
	return wire_err(ant_adv_burst_config_get(request_type, config));
}

antr_err_t antr_sdu_mask_set(uint8_t mask_index, const uint8_t *mask)
{
	return wire_err(ant_sdu_mask_set(mask_index, (uint8_t *)mask));
}

antr_err_t antr_sdu_mask_get(uint8_t mask_index, uint8_t *mask)
{
	return wire_err(ant_sdu_mask_get(mask_index, mask));
}

antr_err_t antr_sdu_mask_config(uint8_t channel, uint8_t mask_config)
{
	return wire_err(ant_sdu_mask_config(channel, mask_config));
}

/* ── Status and queries ────────────────────────────────────────────────────── */

antr_err_t antr_capabilities_get(uint8_t *capabilities)
{
	return wire_err(ant_capabilities_get(capabilities));
}

antr_err_t antr_version_get(uint8_t *version)
{
	return wire_err(ant_version_get(version));
}

antr_err_t antr_channel_status_get(uint8_t channel, uint8_t *status)
{
	return wire_err(ant_channel_status_get(channel, status));
}

antr_err_t antr_channel_id_get(uint8_t channel, uint16_t *device_number,
			       uint8_t *device_type, uint8_t *trans_type)
{
	return wire_err(ant_channel_id_get(channel, device_number, device_type,
					   trans_type));
}

antr_err_t antr_channel_period_get(uint8_t channel, uint16_t *period)
{
	return wire_err(ant_channel_period_get(channel, period));
}

antr_err_t antr_channel_radio_freq_get(uint8_t channel, uint8_t *freq)
{
	return wire_err(ant_channel_radio_freq_get(channel, freq));
}

antr_err_t antr_channel_radio_crc_mode_get(uint8_t channel, uint8_t *mode)
{
	return wire_err(ant_channel_radio_crc_mode_get(channel, mode));
}

/* ── Encryption (ANT+ AES-CTR) ─────────────────────────────────────────────── */

antr_err_t antr_crypto_channel_enable(uint8_t channel, uint8_t enable,
				      uint8_t key_num, uint8_t decimation)
{
	return wire_err(ant_crypto_channel_enable(channel, enable, key_num,
						  decimation));
}

antr_err_t antr_crypto_key_set(uint8_t key_num, const uint8_t *key)
{
	return wire_err(ant_crypto_key_set(key_num, (uint8_t *)key));
}

antr_err_t antr_crypto_info_set(uint8_t type, const uint8_t *info)
{
	return wire_err(ant_crypto_info_set(type, (uint8_t *)info));
}

antr_err_t antr_crypto_info_get(uint8_t type, uint8_t *info)
{
	return wire_err(ant_crypto_info_get(type, info));
}

/* ==========================================================================
 * The BUILD_ASSERT block
 * ==========================================================================
 *
 * Compiled away to nothing. Exists so a number in protocol/ant_wire.yaml
 * cannot quietly stop being the number the radio uses.
 *
 * 66 of the 74 constants tools/ant_wire.py lists in VERIFY_IN_SHIM (no
 * witness elsewhere in the repo) are confirmed here, plus every other ANTW_*
 * constant with an sdk-ant counterpart. Where a name has none, that is
 * stated at the point an assert would otherwise be.
 *
 * Eight VERIFY_IN_SHIM names have no sdk-ant counterpart at all and remain
 * unconfirmed: MESG_GET_SERIAL_NUM_ID (0x61), MESG_SERIAL_NUM_SET_CHANNEL_
 * ID_ID (0x65), MESG_XTAL_ENABLE_ID (0x6D), MESG_USER_CONFIG_PAGE_ID (0x7C),
 * MESG_SERIAL_ERROR_ID (0xAE), EVENT_SERIAL_QUE_OVERFLOW (0x34) - none
 * implemented by sdk-ant or bridged - plus MESG_RSSI_SEARCH_THRESHOLD_ID and
 * MESG_SLEEP_ID, still `value: null`: neither appears in sdk-ant either
 * (the latter only inside a commented-out, never-defined dispatch arm).
 *
 * Hand-written rather than generated, because the mapping isn't mechanical:
 * eleven names differ between the two headers and six ANTW_ constants have
 * no single counterpart and are asserted against an expression instead.
 */

/*
 * ANTW_EQ(ours, theirs) - the whole table below is built from this.
 *
 * The message names the constant on both sides, so a failure reads
 * "ANTW_MESG_ECS_ENABLE_ID has drifted from sdk-ant's MESG_ECS_ENABLE_ID"
 * rather than "static assertion failed" over a line number.
 */
#define ANTW_EQ(ours, theirs)                                             \
	BUILD_ASSERT((ANTW_##ours) == (theirs),                           \
		     "ANTW_" #ours " has drifted from sdk-ant's " #theirs)

/* ── Framing ───────────────────────────────────────────────────────────────── */

ANTW_EQ(SYNC_TX, MESG_TX_SYNC);
ANTW_EQ(SYNC_RX, MESG_RX_SYNC);
ANTW_EQ(MSG_OVERHEAD, MESG_FRAME_SIZE);
ANTW_EQ(CHANNEL_NUM_SIZE, MESG_CHANNEL_NUM_SIZE);
ANTW_EQ(ANT_MAX_PAYLOAD_SIZE, ANT_STANDARD_DATA_PAYLOAD_SIZE);

/* The four size constants deciding whether a 24-byte burst packet survives
 * the parser: sdk-ant computes 38 as 8 payload + 1 extended bitfield + 28
 * extended data + 1 channel byte, matching src/usb_ant_class.c:30's figure
 * independently (an earlier 20/24, derived from the longest host-sent
 * message rather than the ceiling, was wrong).
 */
ANTW_EQ(MAX_SIZE_VALUE, MESG_MAX_SIZE_VALUE);
ANTW_EQ(MAX_DATA_SIZE, MESG_MAX_DATA_SIZE);
ANTW_EQ(MESG_MAX_SIZE, MESG_MAX_SIZE);
ANTW_EQ(MAX_FRAME_SIZE, MESG_BUFFER_SIZE + MESG_SYNC_SIZE);

/*
 * ANTW_ADV_BURST_BLOCK_MAX (24) has no sdk-ant counterpart - sdk-ant names
 * the three sizes only as codes 1/2/3, never byte counts - so what's
 * asserted instead is the property the bridge depends on: a full
 * advanced-burst message still fits the LEN byte's ceiling.
 */
BUILD_ASSERT(ANTW_ADV_BURST_BLOCK_MAX + MESG_CHANNEL_NUM_SIZE <=
		     MESG_MAX_SIZE_VALUE,
	     "a maximum advanced-burst block plus its channel byte no longer "
	     "fits in sdk-ant's LEN ceiling");

/* ── Message identifiers ───────────────────────────────────────────────────── */

ANTW_EQ(MESG_INVALID_ID, MESG_INVALID_ID);
ANTW_EQ(MESG_EVENT_ID, MESG_EVENT_ID);
ANTW_EQ(MESG_VERSION_ID, MESG_VERSION_ID);
ANTW_EQ(MESG_RESPONSE_EVENT_ID, MESG_RESPONSE_EVENT_ID);
ANTW_EQ(MESG_UNASSIGN_CHANNEL_ID, MESG_UNASSIGN_CHANNEL_ID);
ANTW_EQ(MESG_ASSIGN_CHANNEL_ID, MESG_ASSIGN_CHANNEL_ID);
ANTW_EQ(MESG_CHANNEL_MESG_PERIOD_ID, MESG_CHANNEL_MESG_PERIOD_ID);
ANTW_EQ(MESG_CHANNEL_SEARCH_TIMEOUT_ID, MESG_CHANNEL_SEARCH_TIMEOUT_ID);
ANTW_EQ(MESG_CHANNEL_RADIO_FREQ_ID, MESG_CHANNEL_RADIO_FREQ_ID);
ANTW_EQ(MESG_NETWORK_KEY_ID, MESG_NETWORK_KEY_ID);
ANTW_EQ(MESG_RADIO_TX_POWER_ID, MESG_RADIO_TX_POWER_ID);
ANTW_EQ(MESG_RADIO_CW_MODE_ID, MESG_RADIO_CW_MODE_ID);
ANTW_EQ(MESG_SEARCH_WAVEFORM_ID, MESG_SEARCH_WAVEFORM_ID);
ANTW_EQ(MESG_SYSTEM_RESET_ID, MESG_SYSTEM_RESET_ID);
ANTW_EQ(MESG_OPEN_CHANNEL_ID, MESG_OPEN_CHANNEL_ID);
ANTW_EQ(MESG_CLOSE_CHANNEL_ID, MESG_CLOSE_CHANNEL_ID);
ANTW_EQ(MESG_REQUEST_ID, MESG_REQUEST_ID);
ANTW_EQ(MESG_BROADCAST_DATA_ID, MESG_BROADCAST_DATA_ID);
ANTW_EQ(MESG_ACKNOWLEDGED_DATA_ID, MESG_ACKNOWLEDGED_DATA_ID);
ANTW_EQ(MESG_BURST_DATA_ID, MESG_BURST_DATA_ID);
ANTW_EQ(MESG_CHANNEL_ID_ID, MESG_CHANNEL_ID_ID);
ANTW_EQ(MESG_CHANNEL_STATUS_ID, MESG_CHANNEL_STATUS_ID);
ANTW_EQ(MESG_RADIO_CW_INIT_ID, MESG_RADIO_CW_INIT_ID);
ANTW_EQ(MESG_CAPABILITIES_ID, MESG_CAPABILITIES_ID);
/* Recovered here, and the reason three bridge handlers stopped being
 * #ifdef-ed out without the bridge changing a line.
 */
ANTW_EQ(MESG_CHANNEL_CRC_MODE_ID, MESG_CHANNEL_CRC_MODE_ID);
ANTW_EQ(MESG_ID_LIST_ADD_ID, MESG_ID_LIST_ADD_ID);
ANTW_EQ(MESG_ID_LIST_CONFIG_ID, MESG_ID_LIST_CONFIG_ID);
/* Ours is spelled ..._RX_SCAN_MODE_ID; sdk-ant drops the MODE. */
ANTW_EQ(MESG_OPEN_RX_SCAN_MODE_ID, MESG_OPEN_RX_SCAN_ID);
ANTW_EQ(MESG_EXT_BROADCAST_DATA_ID, MESG_EXT_BROADCAST_DATA_ID);
ANTW_EQ(MESG_EXT_ACKNOWLEDGED_DATA_ID, MESG_EXT_ACKNOWLEDGED_DATA_ID);
ANTW_EQ(MESG_EXT_BURST_DATA_ID, MESG_EXT_BURST_DATA_ID);
ANTW_EQ(MESG_CHANNEL_RADIO_TX_POWER_ID, MESG_CHANNEL_RADIO_TX_POWER_ID);
/* ANTW_MESG_GET_SERIAL_NUM_ID (0x61): no sdk-ant counterpart. */
ANTW_EQ(MESG_SET_LP_SEARCH_TIMEOUT_ID, MESG_SET_LP_SEARCH_TIMEOUT_ID);
/* ANTW_MESG_SERIAL_NUM_SET_CHANNEL_ID_ID (0x65): no sdk-ant counterpart. */
ANTW_EQ(MESG_RX_EXT_MESGS_ENABLE_ID, MESG_RX_EXT_MESGS_ENABLE_ID);
/* ANTW_MESG_ENABLE_LED_FLASH_ID (0x68): no sdk-ant counterpart. */
/* ANTW_MESG_XTAL_ENABLE_ID (0x6D): no sdk-ant counterpart. */
ANTW_EQ(MESG_ANTLIB_CONFIG_ID, MESG_ANTLIB_CONFIG_ID);
ANTW_EQ(MESG_STARTUP_MESG_ID, MESG_STARTUP_MESG_ID);
ANTW_EQ(MESG_AUTO_FREQ_CONFIG_ID, MESG_AUTO_FREQ_CONFIG_ID);
ANTW_EQ(MESG_PROX_SEARCH_CONFIG_ID, MESG_PROX_SEARCH_CONFIG_ID);
ANTW_EQ(MESG_ADV_BURST_DATA_ID, MESG_ADV_BURST_DATA_ID);
ANTW_EQ(MESG_EVENT_BUFFERING_CONFIG_ID, MESG_EVENT_BUFFERING_CONFIG_ID);
ANTW_EQ(MESG_SET_SEARCH_CH_PRIORITY_ID, MESG_SET_SEARCH_CH_PRIORITY_ID);
ANTW_EQ(MESG_HIGH_DUTY_SEARCH_MODE_ID, MESG_HIGH_DUTY_SEARCH_MODE_ID);
ANTW_EQ(MESG_CONFIG_ADV_BURST_ID, MESG_CONFIG_ADV_BURST_ID);
ANTW_EQ(MESG_EVENT_FILTER_CONFIG_ID, MESG_EVENT_FILTER_CONFIG_ID);
ANTW_EQ(MESG_SDU_CONFIG_ID, MESG_SDU_CONFIG_ID);
ANTW_EQ(MESG_SDU_SET_MASK_ID, MESG_SDU_SET_MASK_ID);
/* ANTW_MESG_USER_CONFIG_PAGE_ID (0x7C): no sdk-ant counterpart. */
ANTW_EQ(MESG_ENCRYPT_ENABLE_ID, MESG_ENCRYPT_ENABLE_ID);
ANTW_EQ(MESG_SET_ENCRYPT_KEY_ID, MESG_SET_ENCRYPT_KEY_ID);
ANTW_EQ(MESG_SET_ENCRYPT_INFO_ID, MESG_SET_ENCRYPT_INFO_ID);
ANTW_EQ(MESG_ACTIVE_SEARCH_SHARING_ID, MESG_ACTIVE_SEARCH_SHARING_ID);
/* Recovered here. */
ANTW_EQ(MESG_ECS_ENABLE_ID, MESG_ECS_ENABLE_ID);
ANTW_EQ(MESG_PENDING_TRANSMIT_CLEAR_ID, MESG_PENDING_TRANSMIT_CLEAR_ID);
/* ANTW_MESG_SERIAL_ERROR_ID (0xAE): no sdk-ant counterpart. */

/*
 * The RadiANT ids (ANTW_MESG_RADIANT_*, 0xE0-0xE4) have no sdk-ant
 * counterpart by construction - they are ours. Not asserted "unclaimed"
 * because they aren't: sdk-ant reserves 0xE0-0xE4 as MESG_EXT_ID_0..4
 * (selected by MSG_EXT_ID_MASK), invisible to the YAML's repo-only collision
 * check. Reallocating the id is a separate change; nothing is done here.
 */

/* ── Reply sizes ───────────────────────────────────────────────────────────── */

ANTW_EQ(MESG_RESPONSE_EVENT_SIZE, MESG_RESPONSE_EVENT_SIZE);
ANTW_EQ(MESG_STARTUP_MESG_SIZE, MESG_STARTUP_MESG_SIZE);
ANTW_EQ(MESG_CAPABILITIES_SIZE, MESG_CAPABILITIES_SIZE);
ANTW_EQ(MESG_VERSION_SIZE, MESG_VERSION_SIZE);
ANTW_EQ(MESG_CHANNEL_STATUS_SIZE, MESG_CHANNEL_STATUS_SIZE);
ANTW_EQ(MESG_CHANNEL_ID_SIZE, MESG_CHANNEL_ID_SIZE);
ANTW_EQ(MESG_CHANNEL_MESG_PERIOD_SIZE, MESG_CHANNEL_MESG_PERIOD_SIZE);
ANTW_EQ(MESG_CHANNEL_RADIO_FREQ_SIZE, MESG_CHANNEL_RADIO_FREQ_SIZE);
ANTW_EQ(MESG_CHANNEL_CRC_MODE_SIZE, MESG_CHANNEL_CRC_MODE_SIZE);
ANTW_EQ(MESG_SET_SEARCH_CH_PRIORITY_SIZE, MESG_SET_SEARCH_CH_PRIORITY_SIZE);
ANTW_EQ(MESG_PENDING_TRANSMIT_GET_SIZE, MESG_PENDING_TRANSMIT_GET_SIZE);
ANTW_EQ(MESG_ANTLIB_CONFIG_SIZE, MESG_ANTLIB_CONFIG_SIZE);
ANTW_EQ(MESG_ACTIVE_SEARCH_SHARING_REQ_SIZE,
	MESG_ACTIVE_SEARCH_SHARING_REQ_SIZE);
ANTW_EQ(MESG_EVENT_FILTER_CONFIG_REQ_SIZE, MESG_EVENT_FILTER_CONFIG_REQ_SIZE);
ANTW_EQ(MESG_CONFIG_ADV_BURST_REQ_CONFIG_SIZE,
	MESG_CONFIG_ADV_BURST_REQ_CONFIG_SIZE);
/* Recovered here. It is a LEN byte the bridge transmits, so while it was
 * unresolved the capabilities form of the advanced-burst request declined
 * rather than replying with a plausible wrong length.
 */
ANTW_EQ(MESG_CONFIG_ADV_BURST_REQ_CAPABILITIES_SIZE,
	MESG_CONFIG_ADV_BURST_REQ_CAPABILITIES_SIZE);
ANTW_EQ(MESG_CONFIG_ENCRYPT_REQ_CAPABILITIES_SIZE,
	MESG_CONFIG_ENCRYPT_REQ_CAPABILITIES_SIZE);
ANTW_EQ(MESG_CONFIG_ENCRYPT_REQ_CONFIG_ID_SIZE,
	MESG_CONFIG_ENCRYPT_REQ_CONFIG_ID_SIZE);
ANTW_EQ(MESG_CONFIG_ENCRYPT_REQ_CONFIG_USER_DATA_SIZE,
	MESG_CONFIG_ENCRYPT_REQ_CONFIG_USER_DATA_SIZE);

/* ── Channel types and extended assignment ─────────────────────────────────── */

ANTW_EQ(CHANNEL_TYPE_SLAVE, CHANNEL_TYPE_SLAVE);
ANTW_EQ(CHANNEL_TYPE_MASTER, CHANNEL_TYPE_MASTER);
ANTW_EQ(CHANNEL_TYPE_SHARED_SLAVE, CHANNEL_TYPE_SHARED_SLAVE);
ANTW_EQ(CHANNEL_TYPE_SHARED_MASTER, CHANNEL_TYPE_SHARED_MASTER);
ANTW_EQ(CHANNEL_TYPE_SLAVE_RX_ONLY, CHANNEL_TYPE_SLAVE_RX_ONLY);
ANTW_EQ(CHANNEL_TYPE_MASTER_TX_ONLY, CHANNEL_TYPE_MASTER_TX_ONLY);

ANTW_EQ(EXT_PARAM_ALWAYS_SEARCH, EXT_PARAM_ALWAYS_SEARCH);
ANTW_EQ(EXT_PARAM_FREQUENCY_AGILITY, EXT_PARAM_FREQUENCY_AGILITY);
ANTW_EQ(EXT_PARAM_AUTO_SHARED_SLAVE, EXT_PARAM_AUTO_SHARED_SLAVE);
ANTW_EQ(EXT_PARAM_FAST_INITIATION_MODE, EXT_PARAM_FAST_INITIATION_MODE);
ANTW_EQ(EXT_PARAM_ASYNC_TX_MODE, EXT_PARAM_ASYNC_TX_MODE);

/* ── Channel status ────────────────────────────────────────────────────────── */

ANTW_EQ(STATUS_UNASSIGNED_CHANNEL, STATUS_UNASSIGNED_CHANNEL);
ANTW_EQ(STATUS_ASSIGNED_CHANNEL, STATUS_ASSIGNED_CHANNEL);
ANTW_EQ(STATUS_SEARCHING_CHANNEL, STATUS_SEARCHING_CHANNEL);
ANTW_EQ(STATUS_TRACKING_CHANNEL, STATUS_TRACKING_CHANNEL);
ANTW_EQ(STATUS_CHANNEL_STATE_MASK, STATUS_CHANNEL_STATE_MASK);

/* ── Transmit power ────────────────────────────────────────────────────────── */

ANTW_EQ(RADIO_TX_POWER_LVL_0, RADIO_TX_POWER_LVL_0);
ANTW_EQ(RADIO_TX_POWER_LVL_1, RADIO_TX_POWER_LVL_1);
ANTW_EQ(RADIO_TX_POWER_LVL_2, RADIO_TX_POWER_LVL_2);
ANTW_EQ(RADIO_TX_POWER_LVL_3, RADIO_TX_POWER_LVL_3);
ANTW_EQ(RADIO_TX_POWER_LVL_4, RADIO_TX_POWER_LVL_4);
ANTW_EQ(RADIO_TX_POWER_LVL_5, RADIO_TX_POWER_LVL_5);
ANTW_EQ(RADIO_TX_POWER_LVL_CUSTOM, RADIO_TX_POWER_LVL_CUSTOM);

/* ── Library configuration and extended fields ─────────────────────────────── */

/* sdk-ant prefixes these ANT_LIB_CONFIG_; ours drop the leading ANT_. */
ANTW_EQ(LIB_CONFIG_RADIO_CONFIG_ALWAYS, ANT_LIB_CONFIG_RADIO_CONFIG_ALWAYS);
ANTW_EQ(LIB_CONFIG_MESG_OUT_INC_TIME_STAMP,
	ANT_LIB_CONFIG_MESG_OUT_INC_TIME_STAMP);
ANTW_EQ(LIB_CONFIG_MESG_OUT_INC_RSSI, ANT_LIB_CONFIG_MESG_OUT_INC_RSSI);
ANTW_EQ(LIB_CONFIG_MESG_OUT_INC_DEVICE_ID,
	ANT_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID);
ANTW_EQ(LIB_CONFIG_MASK_ALL, ANT_LIB_CONFIG_MASK_ALL);

/*
 * 0xE0 is not a constant sdk-ant names; it is the OR of the three extended
 * field bits, which is what the tools request in one go rather than the
 * device-ID-only 0x80. Asserting the composition is stronger than asserting
 * the literal, because it fails if any one of the three bits moves.
 */
ANTW_EQ(LIB_CONFIG_ALL_EXT_FIELDS,
	ANT_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID |
		ANT_LIB_CONFIG_MESG_OUT_INC_RSSI |
		ANT_LIB_CONFIG_MESG_OUT_INC_TIME_STAMP);

/* The flag byte that appears in a received message, one bit per appended
 * field. Ours are named for what the field is; sdk-ant's for the bitfield.
 */
ANTW_EQ(EXT_FLAG_CHANNEL_ID, ANT_EXT_MESG_BITFIELD_DEVICE_ID);
ANTW_EQ(EXT_FLAG_RSSI, ANT_EXT_MESG_BITFIELD_RSSI);
ANTW_EQ(EXT_FLAG_RX_TIMESTAMP, ANT_EXT_MESG_BITFIELD_TIME_STAMP);
ANTW_EQ(RSSI_MEASUREMENT_TYPE_DBM, RSSI_DBM_TYPE);

/* ── Startup reason ────────────────────────────────────────────────────────── */

ANTW_EQ(STARTUP_POWER_ON_RESET, RESET_POR);
ANTW_EQ(STARTUP_HARDWARE_RESET_LINE, RESET_RST);
ANTW_EQ(STARTUP_WATCH_DOG_RESET, RESET_WDT);
ANTW_EQ(STARTUP_COMMAND_RESET, RESET_CMD);
ANTW_EQ(STARTUP_SYNCHRONOUS_RESET, RESET_SYNC);
ANTW_EQ(STARTUP_SUSPEND_RESET, RESET_SUSPEND);

/* ── Events and response codes ─────────────────────────────────────────────── */

ANTW_EQ(RESPONSE_NO_ERROR, RESPONSE_NO_ERROR);
ANTW_EQ(EVENT_RX_SEARCH_TIMEOUT, EVENT_RX_SEARCH_TIMEOUT);
ANTW_EQ(EVENT_RX_FAIL, EVENT_RX_FAIL);
ANTW_EQ(EVENT_TX, EVENT_TX);
ANTW_EQ(EVENT_TRANSFER_RX_FAILED, EVENT_TRANSFER_RX_FAILED);
ANTW_EQ(EVENT_TRANSFER_TX_COMPLETED, EVENT_TRANSFER_TX_COMPLETED);
ANTW_EQ(EVENT_TRANSFER_TX_FAILED, EVENT_TRANSFER_TX_FAILED);
ANTW_EQ(EVENT_CHANNEL_CLOSED, EVENT_CHANNEL_CLOSED);
ANTW_EQ(EVENT_RX_FAIL_GO_TO_SEARCH, EVENT_RX_FAIL_GO_TO_SEARCH);
ANTW_EQ(EVENT_CHANNEL_COLLISION, EVENT_CHANNEL_COLLISION);
ANTW_EQ(EVENT_TRANSFER_TX_START, EVENT_TRANSFER_TX_START);
/* The one the burst buffer-ownership contract turns on. */
ANTW_EQ(EVENT_TRANSFER_NEXT_DATA_BLOCK, EVENT_TRANSFER_NEXT_DATA_BLOCK);
ANTW_EQ(CHANNEL_IN_WRONG_STATE, CHANNEL_IN_WRONG_STATE);
ANTW_EQ(CHANNEL_NOT_OPENED, CHANNEL_NOT_OPENED);
ANTW_EQ(CHANNEL_ID_NOT_SET, CHANNEL_ID_NOT_SET);
ANTW_EQ(CLOSE_ALL_CHANNELS, CLOSE_ALL_CHANNELS);
ANTW_EQ(TRANSFER_IN_PROGRESS, TRANSFER_IN_PROGRESS);
ANTW_EQ(TRANSFER_SEQUENCE_NUMBER_ERROR, TRANSFER_SEQUENCE_NUMBER_ERROR);
ANTW_EQ(TRANSFER_IN_ERROR, TRANSFER_IN_ERROR);
ANTW_EQ(TRANSFER_BUSY, TRANSFER_BUSY);
ANTW_EQ(MESSAGE_SIZE_EXCEEDS_LIMIT, MESSAGE_SIZE_EXCEEDS_LIMIT);
ANTW_EQ(INVALID_MESSAGE, INVALID_MESSAGE);
ANTW_EQ(INVALID_NETWORK_NUMBER, INVALID_NETWORK_NUMBER);
ANTW_EQ(INVALID_LIST_ID, INVALID_LIST_ID);
ANTW_EQ(INVALID_SCAN_TX_CHANNEL, INVALID_SCAN_TX_CHANNEL);
ANTW_EQ(INVALID_PARAMETER_PROVIDED, INVALID_PARAMETER_PROVIDED);
/* ANTW_EVENT_SERIAL_QUE_OVERFLOW (0x34): no sdk-ant counterpart - the serial
 * queue it names is the host link's, which sdk-ant does not own.
 */
ANTW_EQ(EVENT_QUE_OVERFLOW, EVENT_QUE_OVERFLOW);

/* ── The error narrowing itself ────────────────────────────────────────────── */

/*
 * The assert that matters most here: it decides every error byte the dongle
 * puts on the wire. wire_err() keeps only the low byte, correct iff the
 * offset is a multiple of 256 (it is, 0x4000) - checked rather than assumed,
 * once for the offset and once per code, since a code could wrap into it.
 */
BUILD_ASSERT((NRF_ANT_ERROR_OFFSET & 0xFF) == 0,
	     "sdk-ant's error offset is no longer 256-aligned, so narrowing an "
	     "ant_err_t to its low byte no longer yields the wire code");

#define ANTW_ERR_NARROWS(code)                                                 \
	BUILD_ASSERT(((NRF_ANT_ERROR_##code) & 0xFF) == (ANTW_##code),         \
		     "narrowing NRF_ANT_ERROR_" #code " no longer yields "     \
		     "ANTW_" #code)

ANTW_ERR_NARROWS(CHANNEL_IN_WRONG_STATE);
ANTW_ERR_NARROWS(CHANNEL_NOT_OPENED);
ANTW_ERR_NARROWS(CHANNEL_ID_NOT_SET);
ANTW_ERR_NARROWS(CLOSE_ALL_CHANNELS);
ANTW_ERR_NARROWS(TRANSFER_IN_PROGRESS);
ANTW_ERR_NARROWS(TRANSFER_SEQUENCE_NUMBER_ERROR);
ANTW_ERR_NARROWS(TRANSFER_IN_ERROR);
ANTW_ERR_NARROWS(TRANSFER_BUSY);
ANTW_ERR_NARROWS(MESSAGE_SIZE_EXCEEDS_LIMIT);
ANTW_ERR_NARROWS(INVALID_MESSAGE);
ANTW_ERR_NARROWS(INVALID_NETWORK_NUMBER);
ANTW_ERR_NARROWS(INVALID_LIST_ID);
ANTW_ERR_NARROWS(INVALID_SCAN_TX_CHANNEL);
ANTW_ERR_NARROWS(INVALID_PARAMETER_PROVIDED);

BUILD_ASSERT(NRF_ANT_SUCCESS == ANTW_RESPONSE_NO_ERROR,
	     "success is no longer zero on both sides");

/* ── Burst ─────────────────────────────────────────────────────────────────── */

/*
 * The burst header byte. Ours splits it into a channel mask, sequence
 * shift/mask, and last-packet bit; sdk-ant uses whole-byte masks and an
 * increment. The middle two asserts check that ANTW_BURST_HEADER_SEQ_SHIFT
 * lands on sdk-ant's sequence field at the width the bridge's seq == 0
 * "first block" test depends on.
 */
ANTW_EQ(BURST_HEADER_CHANNEL_MASK, CHANNEL_NUMBER_MASK);
ANTW_EQ(BURST_HEADER_LAST, SEQUENCE_LAST_MESSAGE);
BUILD_ASSERT((1U << ANTW_BURST_HEADER_SEQ_SHIFT) == SEQUENCE_NUMBER_INC,
	     "ANTW_BURST_HEADER_SEQ_SHIFT no longer selects sdk-ant's burst "
	     "sequence field");
BUILD_ASSERT((ANTW_BURST_HEADER_SEQ_MASK << ANTW_BURST_HEADER_SEQ_SHIFT) ==
		     (SEQUENCE_NUMBER_MASK & ~SEQUENCE_LAST_MESSAGE),
	     "ANTW_BURST_HEADER_SEQ_MASK no longer covers sdk-ant's burst "
	     "sequence field");

/*
 * The three ANTR_BURST_SEGMENT_* values from src/ant_radio.h. All three match
 * their sdk-ant counterparts, which is what makes antr_burst_tx() above a
 * plain forward. These three asserts are the whole guarantee behind that: if
 * either side ever moves, the file stops compiling instead of quietly marking
 * the last block of every burst as a middle one.
 */
BUILD_ASSERT(ANTR_BURST_SEGMENT_CONTINUE == BURST_SEGMENT_CONTINUE,
	     "ANTR_BURST_SEGMENT_CONTINUE has drifted from sdk-ant's "
	     "BURST_SEGMENT_CONTINUE");
BUILD_ASSERT(ANTR_BURST_SEGMENT_START == BURST_SEGMENT_START,
	     "ANTR_BURST_SEGMENT_START has drifted from sdk-ant's "
	     "BURST_SEGMENT_START");
BUILD_ASSERT(ANTR_BURST_SEGMENT_END == BURST_SEGMENT_END,
	     "ANTR_BURST_SEGMENT_END has drifted from sdk-ant's "
	     "BURST_SEGMENT_END - forwarding the segment byte untouched would "
	     "mark the last block of every burst as a middle block");
BUILD_ASSERT((ANTR_BURST_SEGMENT_START | ANTR_BURST_SEGMENT_END) !=
		     ANTR_BURST_SEGMENT_START,
	     "ANTR_BURST_SEGMENT_START and _END overlap, so a block that is "
	     "both first and last cannot be expressed");

/* ── Advanced burst ────────────────────────────────────────────────────────── */

ANTW_EQ(ADV_BURST_MODE_DISABLE, ADV_BURST_MODE_DISABLE);
ANTW_EQ(ADV_BURST_MODE_ENABLE, ADV_BURST_MODE_ENABLE);
ANTW_EQ(ADV_BURST_MODES_SIZE_8_BYTES, ADV_BURST_MODES_SIZE_8_BYTES);
ANTW_EQ(ADV_BURST_MODES_SIZE_16_BYTES, ADV_BURST_MODES_SIZE_16_BYTES);
ANTW_EQ(ADV_BURST_MODES_SIZE_24_BYTES, ADV_BURST_MODES_SIZE_24_BYTES);
ANTW_EQ(ADV_BURST_MODES_FREQ_HOP, ADV_BURST_MODES_FREQ_HOP);

/* ── Selective data update ─────────────────────────────────────────────────── */

ANTW_EQ(INVALID_SDU_MASK, INVALID_SDU_MASK);
/* Recovered here. It shares a bit with INVALID_SDU_MASK, which is exactly why
 * a backend must clear it before range-checking the mask number.
 */
ANTW_EQ(SDU_MASK_ACK_CONFIG_BIT, SDU_MASK_ACK_CONFIG_BIT);

/* ── Encryption ────────────────────────────────────────────────────────────── */

ANTW_EQ(ENCRYPTION_DISABLED_MODE, ENCRYPTION_DISABLED_MODE);
ANTW_EQ(ENCRYPTION_INFO_SET_CRYPTO_ID, ENCRYPTION_INFO_SET_CRYPTO_ID);
ANTW_EQ(ENCRYPTION_INFO_SET_CUSTOM_USER_DATA,
	ENCRYPTION_INFO_SET_CUSTOM_USER_DATA);
ANTW_EQ(ENCRYPTION_INFO_SET_RNG_SEED, ENCRYPTION_INFO_SET_RNG_SEED);
ANTW_EQ(ENCRYPTION_INFO_GET_SUPPORTED_MODE, ENCRYPTION_INFO_GET_SUPPORTED_MODE);
ANTW_EQ(ENCRYPTION_INFO_GET_CRYPTO_ID, ENCRYPTION_INFO_GET_CRYPTO_ID);
ANTW_EQ(ENCRYPTION_INFO_GET_CUSTOM_USER_DATA,
	ENCRYPTION_INFO_GET_CUSTOM_USER_DATA);
ANTW_EQ(ENCRYPTION_USER_DATA_SIZE, ENCRYPTION_USER_DATA_SIZE);
ANTW_EQ(ENCRYPTION_KEY_SIZE, SIZE_OF_ENCRYPTED_KEY);
/* Recovered here. It is the highest of the modes above rather than an
 * independent number, so it is asserted against the mode as well as the value.
 */
ANTW_EQ(MAX_SUPPORTED_ENCRYPTION_MODE, MAX_SUPPORTED_ENCRYPTION_MODE);
BUILD_ASSERT(ANTW_MAX_SUPPORTED_ENCRYPTION_MODE ==
		     ENCRYPTION_USER_DATA_REQUEST_MODE,
	     "the highest supported encryption mode is no longer the user-data "
	     "request mode");

/* ── Capabilities ──────────────────────────────────────────────────────────── */

/*
 * The nine offsets are ours - sdk-ant documents the byte order in the prose of
 * ant_capabilities_get() rather than naming constants for it - so what is
 * asserted is that the last one is inside the reply the stack writes. That is
 * the property that matters: an off-by-one here reads past the buffer the
 * bridge handed down.
 */
BUILD_ASSERT(ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_5 + 1 ==
		     MESG_CAPABILITIES_SIZE,
	     "the capabilities reply is no longer exactly as long as the last "
	     "byte offset ant_wire.h names");

/* byte 2 - standard options. Ours say MESSAGES where sdk-ant says TRANSFER,
 * and RECEIVE/TRANSMIT where sdk-ant says RX/TX.
 */
ANTW_EQ(CAPABILITIES_NO_RECEIVE_CHANNELS, CAPABILITIES_NO_RX_CHANNELS);
ANTW_EQ(CAPABILITIES_NO_TRANSMIT_CHANNELS, CAPABILITIES_NO_TX_CHANNELS);
ANTW_EQ(CAPABILITIES_NO_RECEIVE_MESSAGES, CAPABILITIES_NO_RX_MESSAGES);
ANTW_EQ(CAPABILITIES_NO_TRANSMIT_MESSAGES, CAPABILITIES_NO_TX_MESSAGES);
ANTW_EQ(CAPABILITIES_NO_ACKD_MESSAGES, CAPABILITIES_NO_ACKD_MESSAGES);
ANTW_EQ(CAPABILITIES_NO_BURST_MESSAGES, CAPABILITIES_NO_BURST_TRANSFER);

/* byte 3 - advanced options. */
ANTW_EQ(CAPABILITIES_NETWORK_ENABLED, CAPABILITIES_NETWORK_ENABLED);
ANTW_EQ(CAPABILITIES_SERIAL_NUMBER_ENABLED, CAPABILITIES_SERIAL_NUMBER_ENABLED);
ANTW_EQ(CAPABILITIES_PER_CHANNEL_TX_POWER_ENABLED,
	CAPABILITIES_PER_CHANNEL_TX_POWER_ENABLED);
ANTW_EQ(CAPABILITIES_LOW_PRIORITY_SEARCH_ENABLED,
	CAPABILITIES_LOW_PRIORITY_SEARCH_ENABLED);
ANTW_EQ(CAPABILITIES_SCRIPT_ENABLED, CAPABILITIES_SCRIPT_ENABLED);
ANTW_EQ(CAPABILITIES_SEARCH_LIST_ENABLED, CAPABILITIES_SEARCH_LIST_ENABLED);

/* byte 4 - advanced options 2. */
ANTW_EQ(CAPABILITIES_LED_ENABLED, CAPABILITIES_LED_ENABLED);
ANTW_EQ(CAPABILITIES_EXT_MESSAGE_ENABLED, CAPABILITIES_EXT_MESSAGE_ENABLED);
ANTW_EQ(CAPABILITIES_SCAN_MODE_ENABLED, CAPABILITIES_SCAN_MODE_ENABLED);
ANTW_EQ(CAPABILITIES_PROX_SEARCH_ENABLED, CAPABILITIES_PROX_SEARCH_ENABLED);
ANTW_EQ(CAPABILITIES_EXT_ASSIGN_ENABLED, CAPABILITIES_EXT_ASSIGN_ENABLED);
ANTW_EQ(CAPABILITIES_FS_ANTFS_ENABLED, CAPABILITIES_FS_ANTFS_ENABLED);
ANTW_EQ(CAPABILITIES_FIT1_ENABLED, CAPABILITIES_FIT1_ENABLED);

/* byte 6 - advanced options 3. Two names carry an extra MODE in sdk-ant. */
ANTW_EQ(CAPABILITIES_ADVANCED_BURST_ENABLED,
	CAPABILITIES_ADVANCED_BURST_ENABLED);
ANTW_EQ(CAPABILITIES_EVENT_BUFFERING_ENABLED,
	CAPABILITIES_EVENT_BUFFERING_ENABLED);
ANTW_EQ(CAPABILITIES_EVENT_FILTERING_ENABLED,
	CAPABILITIES_EVENT_FILTERING_ENABLED);
ANTW_EQ(CAPABILITIES_HIGH_DUTY_SEARCH_ENABLED,
	CAPABILITIES_HIGH_DUTY_SEARCH_MODE_ENABLED);
ANTW_EQ(CAPABILITIES_SEARCH_SHARING_ENABLED,
	CAPABILITIES_ACTIVE_SEARCH_SHARING_MODE_ENABLED);
ANTW_EQ(CAPABILITIES_RADIO_COEX_CONFIG_ENABLED,
	CAPABILITIES_RADIO_COEX_CONFIG_ENABLED);
ANTW_EQ(CAPABILITIES_SELECTIVE_DATA_UPDATE_ENABLED,
	CAPABILITIES_SELECTIVE_DATA_UPDATE_ENABLED);
ANTW_EQ(CAPABILITIES_ENCRYPTED_CHANNEL_ENABLED,
	CAPABILITIES_ENCRYPTED_CHANNEL_ENABLED);

/*
 * byte 7 - advanced options 4. K1 recorded the byte (0x8D) and refused to name
 * its bits rather than guess; these seven names are the recovery, and the
 * asserts are what stop them being a guess in turn. Bit 0x80 is set in the
 * observed reply and has no name here either, so docs/ant-serial-protocol.md
 * still decodes that one bit as unnamed - which is the honest answer, not an
 * omission.
 */
ANTW_EQ(CAPABILITIES_RFACTIVE_NOTIFICATION_ENABLED,
	CAPABILITIES_RFACTIVE_NOTIFICATION_ENABLED);
ANTW_EQ(CAPABILITIES_DATA_FILTERING_ENABLED,
	CAPABILITIES_DATA_FILTERING_ENABLED);
ANTW_EQ(CAPABILITIES_SEARCH_UPLINK_ENABLED,
	CAPABILITIES_SEARCH_UPLINK_ENABLED);
ANTW_EQ(CAPABILITIES_GROUP_TRANSMITTER_INITIATION_ENABLED,
	CAPABILITIES_GROUP_TRANSMITTER_INITIATION_ENABLED);
ANTW_EQ(CAPABILITIES_TIME_BASE_ENABLED, CAPABILITIES_TIME_BASE_ENABLED);
ANTW_EQ(CAPABILITIES_TIME_SYNC_ENABLED, CAPABILITIES_TIME_SYNC_ENABLED);
ANTW_EQ(CAPABILITIES_PA_LNA_SUPPORT_ENABLED,
	CAPABILITIES_PA_LNA_SUPPORT_ENABLED);

/* byte 8 - advanced options 5. Same story: three of the four set bits in the
 * observed 0x0F have names, and 0x08 does not.
 */
ANTW_EQ(CAPABILITIES_CHANNEL_START_OFFSET_ENABLED,
	CAPABILITIES_CHANNEL_START_OFFSET_ENABLED);
ANTW_EQ(CAPABILITIES_SEARCHING_CHANNEL_UPLINK_ENABLED,
	CAPABILITIES_SEARCHING_CHANNEL_UPLINK_ENABLED);
ANTW_EQ(CAPABILITIES_ID_MATCH_FIX_ON_SHARED_CHANNEL_RXBURST,
	CAPABILITIES_ID_MATCH_FIX_ON_SHARED_CHANNEL_RXBURST);

/* ── Build sizing ──────────────────────────────────────────────────────────── */

/*
 * Not a wire constant, but the same class of mistake: the generated Kconfig
 * fragment asks for eight channels, and sdk-ant caps the number the stack can
 * allocate. A build that asked for more would fail at run time inside
 * ant_init(), which reports it as an error code main() prints and nobody can
 * interpret. Here it is a compile error naming the limit.
 */
BUILD_ASSERT(CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED <= MAX_ANT_CHANNELS,
	     "CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED exceeds sdk-ant's "
	     "MAX_ANT_CHANNELS");
