/* SPDX-License-Identifier: Apache-2.0 */

/*
 * <radiant/radiant_ant.h> implemented on Nordic's prebuilt libant.a, from sdk-ant.
 *
 * Two halves. The first is fifty forwarders, almost all a single renamed
 * call; the exceptions (error narrowing, seven const casts, event inversion)
 * are documented where they happen.
 *
 * The second is a BUILD_ASSERT block comparing every RADIANT_WIRE_* constant in
 * <radiant/radiant_wire.h> against its sdk-ant counterpart. This is the only
 * translation unit permitted to include both headers - the RADIANT_WIRE_ prefix is
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
 * written from <radiant/radiant_ant.h> and docs/sdk-ant-contract.md only. libant.a
 * itself was never disassembled, inspected or instrumented, and that
 * prohibition does not expire.
 */

#include <stdint.h>

#include <zephyr/toolchain.h>

#include <radiant/radiant_ant.h>
#include <radiant/radiant_wire.h>

/* sdk-ant. Everything below this line that is not RADIANT_WIRE_/radiant_ant_/RADIANT_ANT_ comes from
 * here, and nothing else in the firmware includes these.
 */
#include "ant_error.h"
#include "ant_init.h"
#include "ant_interface.h"
#include "ant_parameters.h"

/* ── Error narrowing ───────────────────────────────────────────────────────── */

/*
 * radiant_ant_err_t is a uint8_t whose value *is* the wire byte. sdk-ant's ant_err_t
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
 * contract change in <radiant/radiant_ant.h>, not a quiet fix here.
 */
static inline radiant_ant_err_t wire_err(ant_err_t err)
{
	return (radiant_ant_err_t)((uint32_t)err & 0xFFu);
}

/* ── Event inversion ───────────────────────────────────────────────────────── */

/*
 * sdk-ant hands events back through a callback taking ant_evt_t, a
 * four-level nested union. The shim flattens the three fields the bridge
 * needs into struct radiant_ant_msg and calls radiant_ant_on_message(); nothing above this
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
	const struct radiant_ant_msg msg = {
		.id   = p_evt->message.ANT_MESSAGE_ucMesgID,
		.len  = p_evt->message.ANT_MESSAGE_ucSize,
		.data = p_evt->message.ANT_MESSAGE_aucMesgData,
	};

	radiant_ant_on_message(&msg);
}

/* ── Initialisation and stack lifecycle ────────────────────────────────────── */

/*
 * The one forwarder that does not narrow its return value: ant_init() reports
 * "no memory for the channel count" as -NRF_ENOMEM, whose low byte would be
 * uninterpretable in main()'s log. <radiant/radiant_ant.h> says this returns 0 or
 * RADIANT_WIRE_INVALID_PARAMETER_PROVIDED, so it does - harmless since no host exists
 * yet to see the value.
 */
radiant_ant_err_t radiant_ant_init(void)
{
	if (ant_init() != NRF_ANT_SUCCESS) {
		return RADIANT_WIRE_INVALID_PARAMETER_PROVIDED;
	}

	/* Registered after the stack is up, before this returns - satisfies the
	 * header's "no radiant_ant_on_message() before radiant_ant_init() returns 0" rule.
	 */
	if (ant_cb_register(shim_evt_cb) != NRF_ANT_SUCCESS) {
		return RADIANT_WIRE_INVALID_PARAMETER_PROVIDED;
	}

	return RADIANT_WIRE_RESPONSE_NO_ERROR;
}

radiant_ant_err_t radiant_ant_stack_reset(void)
{
	return wire_err(ant_stack_reset());
}

/* ── Channel lifecycle ─────────────────────────────────────────────────────── */

radiant_ant_err_t radiant_ant_channel_assign(uint8_t channel, uint8_t type, uint8_t network,
			       uint8_t ext_assign)
{
	return wire_err(ant_channel_assign(channel, type, network, ext_assign));
}

radiant_ant_err_t radiant_ant_channel_unassign(uint8_t channel)
{
	return wire_err(ant_channel_unassign(channel));
}

radiant_ant_err_t radiant_ant_channel_ext_assign_set(uint8_t channel, uint8_t ext_assign)
{
	/* No in-place sdk-ant counterpart. The bridge falls back to unassign
	 * and reassign, which is what this backend always did, so its
	 * conformance transcript stays byte-identical. It has no RadiANT
	 * channel security for that fallback to lose.
	 */
	ARG_UNUSED(channel);
	ARG_UNUSED(ext_assign);
	return RADIANT_WIRE_INVALID_MESSAGE;
}

/* The offset form is the only one implemented: sdk-ant's plain
 * ant_channel_open() is a macro over this with CHANNEL_START_OFFSET_NONE, and
 * forwarding to the macro would recurse through our own static inline.
 */
BUILD_ASSERT(CHANNEL_START_OFFSET_NONE == 0,
	     "sdk-ant's no-offset value is no longer 0, so radiant_ant_channel_open()'s "
	     "hardcoded 0 in <radiant/radiant_ant.h> no longer means the same thing");

radiant_ant_err_t radiant_ant_channel_open_with_offset(uint8_t channel, uint16_t offset)
{
	return wire_err(ant_channel_open_with_offset(channel, offset));
}

radiant_ant_err_t radiant_ant_channel_close(uint8_t channel)
{
	return wire_err(ant_channel_close(channel));
}

/* ── Channel configuration ─────────────────────────────────────────────────── */

radiant_ant_err_t radiant_ant_channel_id_set(uint8_t channel, uint16_t device_number,
			       uint8_t device_type, uint8_t trans_type)
{
	return wire_err(ant_channel_id_set(channel, device_number, device_type,
					   trans_type));
}

radiant_ant_err_t radiant_ant_channel_period_set(uint8_t channel, uint16_t period)
{
	return wire_err(ant_channel_period_set(channel, period));
}

radiant_ant_err_t radiant_ant_channel_radio_freq_set(uint8_t channel, uint8_t freq)
{
	return wire_err(ant_channel_radio_freq_set(channel, freq));
}

radiant_ant_err_t radiant_ant_channel_radio_tx_power_set(uint8_t channel, uint8_t level,
					   uint8_t custom)
{
	return wire_err(ant_channel_radio_tx_power_set(channel, level, custom));
}

radiant_ant_err_t radiant_ant_channel_search_timeout_set(uint8_t channel, uint8_t timeout)
{
	return wire_err(ant_channel_search_timeout_set(channel, timeout));
}

radiant_ant_err_t radiant_ant_channel_low_priority_rx_search_timeout_set(uint8_t channel,
							   uint8_t timeout)
{
	return wire_err(ant_channel_low_priority_rx_search_timeout_set(channel,
								       timeout));
}

radiant_ant_err_t radiant_ant_channel_radio_crc_mode_set(uint8_t channel, uint8_t mode)
{
	return wire_err(ant_channel_radio_crc_mode_set(channel, mode));
}

radiant_ant_err_t radiant_ant_auto_freq_hop_table_set(uint8_t channel, uint8_t freq0,
					uint8_t freq1, uint8_t freq2)
{
	return wire_err(ant_auto_freq_hop_table_set(channel, freq0, freq1,
						    freq2));
}

radiant_ant_err_t radiant_ant_enhanced_channel_spacing_enable(uint8_t enable)
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
radiant_ant_err_t radiant_ant_broadcast_message_tx(uint8_t channel, uint8_t size,
				     const uint8_t *data)
{
	return wire_err(ant_broadcast_message_tx(channel, size,
						 (uint8_t *)data));
}

radiant_ant_err_t radiant_ant_acknowledge_message_tx(uint8_t channel, uint8_t size,
				       const uint8_t *data)
{
	return wire_err(ant_acknowledge_message_tx(channel, size,
						   (uint8_t *)data));
}

/* The segment byte is forwarded untouched; all three RADIANT_ANT_BURST_SEGMENT_*
 * values are asserted equal to their sdk-ant counterparts below.
 */
radiant_ant_err_t radiant_ant_burst_tx(uint8_t channel, uint16_t size, uint8_t *data,
			 uint8_t segment)
{
	return wire_err(ant_burst_handler_request(channel, size, data,
						  segment));
}

/* One of the two names in the contract that is not a prefix swap: sdk-ant's is
 * the verb-less ant_pending_transmit(). docs/sdk-ant-contract.md records both.
 */
radiant_ant_err_t radiant_ant_pending_transmit_get(uint8_t channel, uint8_t *pending)
{
	return wire_err(ant_pending_transmit(channel, pending));
}

radiant_ant_err_t radiant_ant_pending_transmit_clear(uint8_t channel, uint8_t *success)
{
	return wire_err(ant_pending_transmit_clear(channel, success));
}

/* ── Search behaviour ──────────────────────────────────────────────────────── */

radiant_ant_err_t radiant_ant_search_waveform_set(uint8_t channel, uint16_t waveform)
{
	return wire_err(ant_search_waveform_set(channel, waveform));
}

radiant_ant_err_t radiant_ant_prox_search_set(uint8_t channel, uint8_t threshold,
				uint8_t custom)
{
	return wire_err(ant_prox_search_set(channel, threshold, custom));
}

radiant_ant_err_t radiant_ant_search_channel_priority_set(uint8_t channel, uint8_t priority)
{
	return wire_err(ant_search_channel_priority_set(channel, priority));
}

radiant_ant_err_t radiant_ant_search_channel_priority_get(uint8_t channel, uint8_t *priority)
{
	return wire_err(ant_search_channel_priority_get(channel, priority));
}

radiant_ant_err_t radiant_ant_active_search_sharing_cycles_set(uint8_t channel,
						 uint8_t cycles)
{
	return wire_err(ant_active_search_sharing_cycles_set(channel, cycles));
}

radiant_ant_err_t radiant_ant_active_search_sharing_cycles_get(uint8_t channel,
						 uint8_t *cycles)
{
	return wire_err(ant_active_search_sharing_cycles_get(channel, cycles));
}

radiant_ant_err_t radiant_ant_id_list_add(uint8_t channel, const uint8_t *device_id,
			    uint8_t index)
{
	return wire_err(ant_id_list_add(channel, (uint8_t *)device_id, index));
}

radiant_ant_err_t radiant_ant_id_list_config(uint8_t channel, uint8_t size, uint8_t inc_exc)
{
	return wire_err(ant_id_list_config(channel, size, inc_exc));
}

/* ── Networks and keys ─────────────────────────────────────────────────────── */

/* The one buffer-taking function that needs no cast. */
radiant_ant_err_t radiant_ant_network_address_set(uint8_t network, const uint8_t *key)
{
	return wire_err(ant_network_address_set(network, key));
}

/* ── Library configuration ─────────────────────────────────────────────────── */

radiant_ant_err_t radiant_ant_lib_config_set(uint8_t config)
{
	return wire_err(ant_lib_config_set(config));
}

radiant_ant_err_t radiant_ant_lib_config_clear(uint8_t config)
{
	return wire_err(ant_lib_config_clear(config));
}

radiant_ant_err_t radiant_ant_lib_config_get(uint8_t *config)
{
	return wire_err(ant_lib_config_get(config));
}

radiant_ant_err_t radiant_ant_event_filtering_set(uint16_t filter)
{
	return wire_err(ant_event_filtering_set(filter));
}

radiant_ant_err_t radiant_ant_event_filtering_get(uint16_t *filter)
{
	return wire_err(ant_event_filtering_get(filter));
}

radiant_ant_err_t radiant_ant_adv_burst_config_set(const uint8_t *config, uint8_t size)
{
	return wire_err(ant_adv_burst_config_set((uint8_t *)config, size));
}

radiant_ant_err_t radiant_ant_adv_burst_config_get(uint8_t request_type, uint8_t *config)
{
	return wire_err(ant_adv_burst_config_get(request_type, config));
}

radiant_ant_err_t radiant_ant_sdu_mask_set(uint8_t mask_index, const uint8_t *mask)
{
	return wire_err(ant_sdu_mask_set(mask_index, (uint8_t *)mask));
}

radiant_ant_err_t radiant_ant_sdu_mask_get(uint8_t mask_index, uint8_t *mask)
{
	return wire_err(ant_sdu_mask_get(mask_index, mask));
}

radiant_ant_err_t radiant_ant_sdu_mask_config(uint8_t channel, uint8_t mask_config)
{
	return wire_err(ant_sdu_mask_config(channel, mask_config));
}

/* ── Status and queries ────────────────────────────────────────────────────── */

radiant_ant_err_t radiant_ant_capabilities_get(uint8_t *capabilities)
{
	return wire_err(ant_capabilities_get(capabilities));
}

radiant_ant_err_t radiant_ant_version_get(uint8_t *version)
{
	return wire_err(ant_version_get(version));
}

radiant_ant_err_t radiant_ant_serial_number_get(uint32_t *serial)
{
	/* No sdk-ant counterpart, and the reference stack does not advertise
	 * CAPABILITIES_SERIAL_NUMBER_ENABLED, so refusing keeps this backend's
	 * conformance transcript byte-identical to the one recorded from it.
	 */
	ARG_UNUSED(serial);
	return RADIANT_WIRE_INVALID_MESSAGE;
}

radiant_ant_err_t radiant_ant_channel_status_get(uint8_t channel, uint8_t *status)
{
	return wire_err(ant_channel_status_get(channel, status));
}

radiant_ant_err_t radiant_ant_channel_id_get(uint8_t channel, uint16_t *device_number,
			       uint8_t *device_type, uint8_t *trans_type)
{
	return wire_err(ant_channel_id_get(channel, device_number, device_type,
					   trans_type));
}

radiant_ant_err_t radiant_ant_channel_period_get(uint8_t channel, uint16_t *period)
{
	return wire_err(ant_channel_period_get(channel, period));
}

radiant_ant_err_t radiant_ant_channel_radio_freq_get(uint8_t channel, uint8_t *freq)
{
	return wire_err(ant_channel_radio_freq_get(channel, freq));
}

radiant_ant_err_t radiant_ant_channel_radio_crc_mode_get(uint8_t channel, uint8_t *mode)
{
	return wire_err(ant_channel_radio_crc_mode_get(channel, mode));
}

/* ── Encryption (ANT+ AES-CTR) ─────────────────────────────────────────────── */

radiant_ant_err_t radiant_ant_crypto_channel_enable(uint8_t channel, uint8_t enable,
				      uint8_t key_num, uint8_t decimation)
{
	return wire_err(ant_crypto_channel_enable(channel, enable, key_num,
						  decimation));
}

radiant_ant_err_t radiant_ant_crypto_key_set(uint8_t key_num, const uint8_t *key)
{
	return wire_err(ant_crypto_key_set(key_num, (uint8_t *)key));
}

radiant_ant_err_t radiant_ant_crypto_info_set(uint8_t type, const uint8_t *info)
{
	return wire_err(ant_crypto_info_set(type, (uint8_t *)info));
}

radiant_ant_err_t radiant_ant_crypto_info_get(uint8_t type, uint8_t *info)
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
 * witness elsewhere in the repo) are confirmed here, plus every other RADIANT_WIRE_*
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
 * eleven names differ between the two headers and six RADIANT_WIRE_ constants have
 * no single counterpart and are asserted against an expression instead.
 */

/*
 * WIRE_EQ(ours, theirs) - the whole table below is built from this.
 *
 * The message names the constant on both sides, so a failure reads
 * "RADIANT_WIRE_MESG_ECS_ENABLE_ID has drifted from sdk-ant's MESG_ECS_ENABLE_ID"
 * rather than "static assertion failed" over a line number.
 */
#define WIRE_EQ(ours, theirs)                                        \
	BUILD_ASSERT((RADIANT_WIRE_##ours) == (theirs),                    \
		     "RADIANT_WIRE_" #ours " has drifted from sdk-ant's " #theirs)

/* ── Framing ───────────────────────────────────────────────────────────────── */

WIRE_EQ(SYNC_TX, MESG_TX_SYNC);
WIRE_EQ(SYNC_RX, MESG_RX_SYNC);
WIRE_EQ(MSG_OVERHEAD, MESG_FRAME_SIZE);
WIRE_EQ(CHANNEL_NUM_SIZE, MESG_CHANNEL_NUM_SIZE);
WIRE_EQ(ANT_MAX_PAYLOAD_SIZE, ANT_STANDARD_DATA_PAYLOAD_SIZE);

/* The four size constants deciding whether a 24-byte burst packet survives
 * the parser: sdk-ant computes 38 as 8 payload + 1 extended bitfield + 28
 * extended data + 1 channel byte, matching src/usb_ant_class.c:30's figure
 * independently. Must be derived from this ceiling, not from the longest
 * host-sent message (20 or 24) - that undercounts it.
 */
WIRE_EQ(MAX_SIZE_VALUE, MESG_MAX_SIZE_VALUE);
WIRE_EQ(MAX_DATA_SIZE, MESG_MAX_DATA_SIZE);
WIRE_EQ(MESG_MAX_SIZE, MESG_MAX_SIZE);
WIRE_EQ(MAX_FRAME_SIZE, MESG_BUFFER_SIZE + MESG_SYNC_SIZE);

/*
 * RADIANT_WIRE_ADV_BURST_BLOCK_MAX (24) has no sdk-ant counterpart - sdk-ant names
 * the three sizes only as codes 1/2/3, never byte counts - so what's
 * asserted instead is the property the bridge depends on: a full
 * advanced-burst message still fits the LEN byte's ceiling.
 */
BUILD_ASSERT(RADIANT_WIRE_ADV_BURST_BLOCK_MAX + MESG_CHANNEL_NUM_SIZE <=
		     MESG_MAX_SIZE_VALUE,
	     "a maximum advanced-burst block plus its channel byte no longer "
	     "fits in sdk-ant's LEN ceiling");

/* ── Message identifiers ───────────────────────────────────────────────────── */

WIRE_EQ(MESG_INVALID_ID, MESG_INVALID_ID);
WIRE_EQ(MESG_EVENT_ID, MESG_EVENT_ID);
WIRE_EQ(MESG_VERSION_ID, MESG_VERSION_ID);
WIRE_EQ(MESG_RESPONSE_EVENT_ID, MESG_RESPONSE_EVENT_ID);
WIRE_EQ(MESG_UNASSIGN_CHANNEL_ID, MESG_UNASSIGN_CHANNEL_ID);
WIRE_EQ(MESG_ASSIGN_CHANNEL_ID, MESG_ASSIGN_CHANNEL_ID);
WIRE_EQ(MESG_CHANNEL_MESG_PERIOD_ID, MESG_CHANNEL_MESG_PERIOD_ID);
WIRE_EQ(MESG_CHANNEL_SEARCH_TIMEOUT_ID, MESG_CHANNEL_SEARCH_TIMEOUT_ID);
WIRE_EQ(MESG_CHANNEL_RADIO_FREQ_ID, MESG_CHANNEL_RADIO_FREQ_ID);
WIRE_EQ(MESG_NETWORK_KEY_ID, MESG_NETWORK_KEY_ID);
WIRE_EQ(MESG_RADIO_TX_POWER_ID, MESG_RADIO_TX_POWER_ID);
WIRE_EQ(MESG_RADIO_CW_MODE_ID, MESG_RADIO_CW_MODE_ID);
WIRE_EQ(MESG_SEARCH_WAVEFORM_ID, MESG_SEARCH_WAVEFORM_ID);
WIRE_EQ(MESG_SYSTEM_RESET_ID, MESG_SYSTEM_RESET_ID);
WIRE_EQ(MESG_OPEN_CHANNEL_ID, MESG_OPEN_CHANNEL_ID);
WIRE_EQ(MESG_CLOSE_CHANNEL_ID, MESG_CLOSE_CHANNEL_ID);
WIRE_EQ(MESG_REQUEST_ID, MESG_REQUEST_ID);
WIRE_EQ(MESG_BROADCAST_DATA_ID, MESG_BROADCAST_DATA_ID);
WIRE_EQ(MESG_ACKNOWLEDGED_DATA_ID, MESG_ACKNOWLEDGED_DATA_ID);
WIRE_EQ(MESG_BURST_DATA_ID, MESG_BURST_DATA_ID);
WIRE_EQ(MESG_CHANNEL_ID_ID, MESG_CHANNEL_ID_ID);
WIRE_EQ(MESG_CHANNEL_STATUS_ID, MESG_CHANNEL_STATUS_ID);
WIRE_EQ(MESG_RADIO_CW_INIT_ID, MESG_RADIO_CW_INIT_ID);
WIRE_EQ(MESG_CAPABILITIES_ID, MESG_CAPABILITIES_ID);
/* Recovered here, and the reason three bridge handlers stopped being
 * #ifdef-ed out without the bridge changing a line.
 */
WIRE_EQ(MESG_CHANNEL_CRC_MODE_ID, MESG_CHANNEL_CRC_MODE_ID);
WIRE_EQ(MESG_ID_LIST_ADD_ID, MESG_ID_LIST_ADD_ID);
WIRE_EQ(MESG_ID_LIST_CONFIG_ID, MESG_ID_LIST_CONFIG_ID);
/* Ours is spelled ..._RX_SCAN_MODE_ID; sdk-ant drops the MODE. */
WIRE_EQ(MESG_OPEN_RX_SCAN_MODE_ID, MESG_OPEN_RX_SCAN_ID);
WIRE_EQ(MESG_EXT_BROADCAST_DATA_ID, MESG_EXT_BROADCAST_DATA_ID);
WIRE_EQ(MESG_EXT_ACKNOWLEDGED_DATA_ID, MESG_EXT_ACKNOWLEDGED_DATA_ID);
WIRE_EQ(MESG_EXT_BURST_DATA_ID, MESG_EXT_BURST_DATA_ID);
WIRE_EQ(MESG_CHANNEL_RADIO_TX_POWER_ID, MESG_CHANNEL_RADIO_TX_POWER_ID);
/* RADIANT_WIRE_MESG_GET_SERIAL_NUM_ID (0x61): no sdk-ant counterpart. */
WIRE_EQ(MESG_SET_LP_SEARCH_TIMEOUT_ID, MESG_SET_LP_SEARCH_TIMEOUT_ID);
/* RADIANT_WIRE_MESG_SERIAL_NUM_SET_CHANNEL_ID_ID (0x65): no sdk-ant counterpart. */
WIRE_EQ(MESG_RX_EXT_MESGS_ENABLE_ID, MESG_RX_EXT_MESGS_ENABLE_ID);
/* RADIANT_WIRE_MESG_ENABLE_LED_FLASH_ID (0x68): no sdk-ant counterpart. */
/* RADIANT_WIRE_MESG_XTAL_ENABLE_ID (0x6D): no sdk-ant counterpart. */
WIRE_EQ(MESG_ANTLIB_CONFIG_ID, MESG_ANTLIB_CONFIG_ID);
WIRE_EQ(MESG_STARTUP_MESG_ID, MESG_STARTUP_MESG_ID);
WIRE_EQ(MESG_AUTO_FREQ_CONFIG_ID, MESG_AUTO_FREQ_CONFIG_ID);
WIRE_EQ(MESG_PROX_SEARCH_CONFIG_ID, MESG_PROX_SEARCH_CONFIG_ID);
WIRE_EQ(MESG_ADV_BURST_DATA_ID, MESG_ADV_BURST_DATA_ID);
WIRE_EQ(MESG_EVENT_BUFFERING_CONFIG_ID, MESG_EVENT_BUFFERING_CONFIG_ID);
WIRE_EQ(MESG_SET_SEARCH_CH_PRIORITY_ID, MESG_SET_SEARCH_CH_PRIORITY_ID);
WIRE_EQ(MESG_HIGH_DUTY_SEARCH_MODE_ID, MESG_HIGH_DUTY_SEARCH_MODE_ID);
WIRE_EQ(MESG_CONFIG_ADV_BURST_ID, MESG_CONFIG_ADV_BURST_ID);
WIRE_EQ(MESG_EVENT_FILTER_CONFIG_ID, MESG_EVENT_FILTER_CONFIG_ID);
WIRE_EQ(MESG_SDU_CONFIG_ID, MESG_SDU_CONFIG_ID);
WIRE_EQ(MESG_SDU_SET_MASK_ID, MESG_SDU_SET_MASK_ID);
/* RADIANT_WIRE_MESG_USER_CONFIG_PAGE_ID (0x7C): no sdk-ant counterpart. */
WIRE_EQ(MESG_ENCRYPT_ENABLE_ID, MESG_ENCRYPT_ENABLE_ID);
WIRE_EQ(MESG_SET_ENCRYPT_KEY_ID, MESG_SET_ENCRYPT_KEY_ID);
WIRE_EQ(MESG_SET_ENCRYPT_INFO_ID, MESG_SET_ENCRYPT_INFO_ID);
WIRE_EQ(MESG_ACTIVE_SEARCH_SHARING_ID, MESG_ACTIVE_SEARCH_SHARING_ID);
/* Recovered here. */
WIRE_EQ(MESG_ECS_ENABLE_ID, MESG_ECS_ENABLE_ID);
WIRE_EQ(MESG_PENDING_TRANSMIT_CLEAR_ID, MESG_PENDING_TRANSMIT_CLEAR_ID);
/* RADIANT_WIRE_MESG_SERIAL_ERROR_ID (0xAE): no sdk-ant counterpart. */

/*
 * The RadiANT ids (RADIANT_WIRE_MESG_RADIANT_*, 0xE0-0xE4) have no sdk-ant
 * counterpart by construction - they are ours. Not asserted "unclaimed"
 * because they aren't: sdk-ant reserves 0xE0-0xE4 as MESG_EXT_ID_0..4
 * (selected by MSG_EXT_ID_MASK), invisible to the YAML's repo-only collision
 * check. Reallocating the id is a separate change; nothing is done here.
 */

/* ── Reply sizes ───────────────────────────────────────────────────────────── */

WIRE_EQ(MESG_RESPONSE_EVENT_SIZE, MESG_RESPONSE_EVENT_SIZE);
WIRE_EQ(MESG_STARTUP_MESG_SIZE, MESG_STARTUP_MESG_SIZE);
WIRE_EQ(MESG_CAPABILITIES_SIZE, MESG_CAPABILITIES_SIZE);
WIRE_EQ(MESG_VERSION_SIZE, MESG_VERSION_SIZE);
WIRE_EQ(MESG_CHANNEL_STATUS_SIZE, MESG_CHANNEL_STATUS_SIZE);
WIRE_EQ(MESG_CHANNEL_ID_SIZE, MESG_CHANNEL_ID_SIZE);
WIRE_EQ(MESG_CHANNEL_MESG_PERIOD_SIZE, MESG_CHANNEL_MESG_PERIOD_SIZE);
WIRE_EQ(MESG_CHANNEL_RADIO_FREQ_SIZE, MESG_CHANNEL_RADIO_FREQ_SIZE);
WIRE_EQ(MESG_CHANNEL_CRC_MODE_SIZE, MESG_CHANNEL_CRC_MODE_SIZE);
WIRE_EQ(MESG_SET_SEARCH_CH_PRIORITY_SIZE, MESG_SET_SEARCH_CH_PRIORITY_SIZE);
WIRE_EQ(MESG_PENDING_TRANSMIT_GET_SIZE, MESG_PENDING_TRANSMIT_GET_SIZE);
WIRE_EQ(MESG_ANTLIB_CONFIG_SIZE, MESG_ANTLIB_CONFIG_SIZE);
WIRE_EQ(MESG_ACTIVE_SEARCH_SHARING_REQ_SIZE,
	MESG_ACTIVE_SEARCH_SHARING_REQ_SIZE);
WIRE_EQ(MESG_EVENT_FILTER_CONFIG_REQ_SIZE, MESG_EVENT_FILTER_CONFIG_REQ_SIZE);
WIRE_EQ(MESG_CONFIG_ADV_BURST_REQ_CONFIG_SIZE,
	MESG_CONFIG_ADV_BURST_REQ_CONFIG_SIZE);
/* Recovered here. It is a LEN byte the bridge transmits, so while it was
 * unresolved the capabilities form of the advanced-burst request declined
 * rather than replying with a plausible wrong length.
 */
WIRE_EQ(MESG_CONFIG_ADV_BURST_REQ_CAPABILITIES_SIZE,
	MESG_CONFIG_ADV_BURST_REQ_CAPABILITIES_SIZE);
WIRE_EQ(MESG_CONFIG_ENCRYPT_REQ_CAPABILITIES_SIZE,
	MESG_CONFIG_ENCRYPT_REQ_CAPABILITIES_SIZE);
WIRE_EQ(MESG_CONFIG_ENCRYPT_REQ_CONFIG_ID_SIZE,
	MESG_CONFIG_ENCRYPT_REQ_CONFIG_ID_SIZE);
WIRE_EQ(MESG_CONFIG_ENCRYPT_REQ_CONFIG_USER_DATA_SIZE,
	MESG_CONFIG_ENCRYPT_REQ_CONFIG_USER_DATA_SIZE);

/* ── Channel types and extended assignment ─────────────────────────────────── */

WIRE_EQ(CHANNEL_TYPE_SLAVE, CHANNEL_TYPE_SLAVE);
WIRE_EQ(CHANNEL_TYPE_MASTER, CHANNEL_TYPE_MASTER);
WIRE_EQ(CHANNEL_TYPE_SHARED_SLAVE, CHANNEL_TYPE_SHARED_SLAVE);
WIRE_EQ(CHANNEL_TYPE_SHARED_MASTER, CHANNEL_TYPE_SHARED_MASTER);
WIRE_EQ(CHANNEL_TYPE_SLAVE_RX_ONLY, CHANNEL_TYPE_SLAVE_RX_ONLY);
WIRE_EQ(CHANNEL_TYPE_MASTER_TX_ONLY, CHANNEL_TYPE_MASTER_TX_ONLY);

WIRE_EQ(EXT_PARAM_ALWAYS_SEARCH, EXT_PARAM_ALWAYS_SEARCH);
WIRE_EQ(EXT_PARAM_FREQUENCY_AGILITY, EXT_PARAM_FREQUENCY_AGILITY);
WIRE_EQ(EXT_PARAM_AUTO_SHARED_SLAVE, EXT_PARAM_AUTO_SHARED_SLAVE);
WIRE_EQ(EXT_PARAM_FAST_INITIATION_MODE, EXT_PARAM_FAST_INITIATION_MODE);
WIRE_EQ(EXT_PARAM_ASYNC_TX_MODE, EXT_PARAM_ASYNC_TX_MODE);

/* ── Channel status ────────────────────────────────────────────────────────── */

WIRE_EQ(STATUS_UNASSIGNED_CHANNEL, STATUS_UNASSIGNED_CHANNEL);
WIRE_EQ(STATUS_ASSIGNED_CHANNEL, STATUS_ASSIGNED_CHANNEL);
WIRE_EQ(STATUS_SEARCHING_CHANNEL, STATUS_SEARCHING_CHANNEL);
WIRE_EQ(STATUS_TRACKING_CHANNEL, STATUS_TRACKING_CHANNEL);
WIRE_EQ(STATUS_CHANNEL_STATE_MASK, STATUS_CHANNEL_STATE_MASK);

/* ── Transmit power ────────────────────────────────────────────────────────── */

WIRE_EQ(RADIO_TX_POWER_LVL_0, RADIO_TX_POWER_LVL_0);
WIRE_EQ(RADIO_TX_POWER_LVL_1, RADIO_TX_POWER_LVL_1);
WIRE_EQ(RADIO_TX_POWER_LVL_2, RADIO_TX_POWER_LVL_2);
WIRE_EQ(RADIO_TX_POWER_LVL_3, RADIO_TX_POWER_LVL_3);
WIRE_EQ(RADIO_TX_POWER_LVL_4, RADIO_TX_POWER_LVL_4);
WIRE_EQ(RADIO_TX_POWER_LVL_5, RADIO_TX_POWER_LVL_5);
WIRE_EQ(RADIO_TX_POWER_LVL_CUSTOM, RADIO_TX_POWER_LVL_CUSTOM);

/* ── Library configuration and extended fields ─────────────────────────────── */

/* sdk-ant prefixes these ANT_LIB_CONFIG_; ours drop the leading ANT_. */
WIRE_EQ(LIB_CONFIG_RADIO_CONFIG_ALWAYS, ANT_LIB_CONFIG_RADIO_CONFIG_ALWAYS);
WIRE_EQ(LIB_CONFIG_MESG_OUT_INC_TIME_STAMP,
	ANT_LIB_CONFIG_MESG_OUT_INC_TIME_STAMP);
WIRE_EQ(LIB_CONFIG_MESG_OUT_INC_RSSI, ANT_LIB_CONFIG_MESG_OUT_INC_RSSI);
WIRE_EQ(LIB_CONFIG_MESG_OUT_INC_DEVICE_ID,
	ANT_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID);
WIRE_EQ(LIB_CONFIG_MASK_ALL, ANT_LIB_CONFIG_MASK_ALL);

/*
 * 0xE0 is not a constant sdk-ant names; it is the OR of the three extended
 * field bits, which is what the tools request in one go rather than the
 * device-ID-only 0x80. Asserting the composition is stronger than asserting
 * the literal, because it fails if any one of the three bits moves.
 */
WIRE_EQ(LIB_CONFIG_ALL_EXT_FIELDS,
	ANT_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID |
		ANT_LIB_CONFIG_MESG_OUT_INC_RSSI |
		ANT_LIB_CONFIG_MESG_OUT_INC_TIME_STAMP);

/* The flag byte that appears in a received message, one bit per appended
 * field. Ours are named for what the field is; sdk-ant's for the bitfield.
 */
WIRE_EQ(EXT_FLAG_CHANNEL_ID, ANT_EXT_MESG_BITFIELD_DEVICE_ID);
WIRE_EQ(EXT_FLAG_RSSI, ANT_EXT_MESG_BITFIELD_RSSI);
WIRE_EQ(EXT_FLAG_RX_TIMESTAMP, ANT_EXT_MESG_BITFIELD_TIME_STAMP);
WIRE_EQ(RSSI_MEASUREMENT_TYPE_DBM, RSSI_DBM_TYPE);

/* ── Startup reason ────────────────────────────────────────────────────────── */

WIRE_EQ(STARTUP_POWER_ON_RESET, RESET_POR);
WIRE_EQ(STARTUP_HARDWARE_RESET_LINE, RESET_RST);
WIRE_EQ(STARTUP_WATCH_DOG_RESET, RESET_WDT);
WIRE_EQ(STARTUP_COMMAND_RESET, RESET_CMD);
WIRE_EQ(STARTUP_SYNCHRONOUS_RESET, RESET_SYNC);
WIRE_EQ(STARTUP_SUSPEND_RESET, RESET_SUSPEND);

/* ── Events and response codes ─────────────────────────────────────────────── */

WIRE_EQ(RESPONSE_NO_ERROR, RESPONSE_NO_ERROR);
WIRE_EQ(EVENT_RX_SEARCH_TIMEOUT, EVENT_RX_SEARCH_TIMEOUT);
WIRE_EQ(EVENT_RX_FAIL, EVENT_RX_FAIL);
WIRE_EQ(EVENT_TX, EVENT_TX);
WIRE_EQ(EVENT_TRANSFER_RX_FAILED, EVENT_TRANSFER_RX_FAILED);
WIRE_EQ(EVENT_TRANSFER_TX_COMPLETED, EVENT_TRANSFER_TX_COMPLETED);
WIRE_EQ(EVENT_TRANSFER_TX_FAILED, EVENT_TRANSFER_TX_FAILED);
WIRE_EQ(EVENT_CHANNEL_CLOSED, EVENT_CHANNEL_CLOSED);
WIRE_EQ(EVENT_RX_FAIL_GO_TO_SEARCH, EVENT_RX_FAIL_GO_TO_SEARCH);
WIRE_EQ(EVENT_CHANNEL_COLLISION, EVENT_CHANNEL_COLLISION);
WIRE_EQ(EVENT_TRANSFER_TX_START, EVENT_TRANSFER_TX_START);
/* The one the burst buffer-ownership contract turns on. */
WIRE_EQ(EVENT_TRANSFER_NEXT_DATA_BLOCK, EVENT_TRANSFER_NEXT_DATA_BLOCK);
WIRE_EQ(CHANNEL_IN_WRONG_STATE, CHANNEL_IN_WRONG_STATE);
WIRE_EQ(CHANNEL_NOT_OPENED, CHANNEL_NOT_OPENED);
WIRE_EQ(CHANNEL_ID_NOT_SET, CHANNEL_ID_NOT_SET);
WIRE_EQ(CLOSE_ALL_CHANNELS, CLOSE_ALL_CHANNELS);
WIRE_EQ(TRANSFER_IN_PROGRESS, TRANSFER_IN_PROGRESS);
WIRE_EQ(TRANSFER_SEQUENCE_NUMBER_ERROR, TRANSFER_SEQUENCE_NUMBER_ERROR);
WIRE_EQ(TRANSFER_IN_ERROR, TRANSFER_IN_ERROR);
WIRE_EQ(TRANSFER_BUSY, TRANSFER_BUSY);
WIRE_EQ(MESSAGE_SIZE_EXCEEDS_LIMIT, MESSAGE_SIZE_EXCEEDS_LIMIT);
WIRE_EQ(INVALID_MESSAGE, INVALID_MESSAGE);
WIRE_EQ(INVALID_NETWORK_NUMBER, INVALID_NETWORK_NUMBER);
WIRE_EQ(INVALID_LIST_ID, INVALID_LIST_ID);
WIRE_EQ(INVALID_SCAN_TX_CHANNEL, INVALID_SCAN_TX_CHANNEL);
WIRE_EQ(INVALID_PARAMETER_PROVIDED, INVALID_PARAMETER_PROVIDED);
/* RADIANT_WIRE_EVENT_SERIAL_QUE_OVERFLOW (0x34): no sdk-ant counterpart - the serial
 * queue it names is the host link's, which sdk-ant does not own.
 */
WIRE_EQ(EVENT_QUE_OVERFLOW, EVENT_QUE_OVERFLOW);

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

#define RADIANT_WIRE_ERR_NARROWS(code)                                                 \
	BUILD_ASSERT(((NRF_ANT_ERROR_##code) & 0xFF) == (RADIANT_WIRE_##code),         \
		     "narrowing NRF_ANT_ERROR_" #code " no longer yields "     \
		     "RADIANT_WIRE_" #code)

RADIANT_WIRE_ERR_NARROWS(CHANNEL_IN_WRONG_STATE);
RADIANT_WIRE_ERR_NARROWS(CHANNEL_NOT_OPENED);
RADIANT_WIRE_ERR_NARROWS(CHANNEL_ID_NOT_SET);
RADIANT_WIRE_ERR_NARROWS(CLOSE_ALL_CHANNELS);
RADIANT_WIRE_ERR_NARROWS(TRANSFER_IN_PROGRESS);
RADIANT_WIRE_ERR_NARROWS(TRANSFER_SEQUENCE_NUMBER_ERROR);
RADIANT_WIRE_ERR_NARROWS(TRANSFER_IN_ERROR);
RADIANT_WIRE_ERR_NARROWS(TRANSFER_BUSY);
RADIANT_WIRE_ERR_NARROWS(MESSAGE_SIZE_EXCEEDS_LIMIT);
RADIANT_WIRE_ERR_NARROWS(INVALID_MESSAGE);
RADIANT_WIRE_ERR_NARROWS(INVALID_NETWORK_NUMBER);
RADIANT_WIRE_ERR_NARROWS(INVALID_LIST_ID);
RADIANT_WIRE_ERR_NARROWS(INVALID_SCAN_TX_CHANNEL);
RADIANT_WIRE_ERR_NARROWS(INVALID_PARAMETER_PROVIDED);

BUILD_ASSERT(NRF_ANT_SUCCESS == RADIANT_WIRE_RESPONSE_NO_ERROR,
	     "success is no longer zero on both sides");

/* ── Burst ─────────────────────────────────────────────────────────────────── */

/*
 * The burst header byte. Ours splits it into a channel mask, sequence
 * shift/mask, and last-packet bit; sdk-ant uses whole-byte masks and an
 * increment. The middle two asserts check that RADIANT_WIRE_BURST_HEADER_SEQ_SHIFT
 * lands on sdk-ant's sequence field at the width the bridge's seq == 0
 * "first block" test depends on.
 */
WIRE_EQ(BURST_HEADER_CHANNEL_MASK, CHANNEL_NUMBER_MASK);
WIRE_EQ(BURST_HEADER_LAST, SEQUENCE_LAST_MESSAGE);
BUILD_ASSERT((1U << RADIANT_WIRE_BURST_HEADER_SEQ_SHIFT) == SEQUENCE_NUMBER_INC,
	     "RADIANT_WIRE_BURST_HEADER_SEQ_SHIFT no longer selects sdk-ant's burst "
	     "sequence field");
BUILD_ASSERT((RADIANT_WIRE_BURST_HEADER_SEQ_MASK << RADIANT_WIRE_BURST_HEADER_SEQ_SHIFT) ==
		     (SEQUENCE_NUMBER_MASK & ~SEQUENCE_LAST_MESSAGE),
	     "RADIANT_WIRE_BURST_HEADER_SEQ_MASK no longer covers sdk-ant's burst "
	     "sequence field");

/*
 * The three RADIANT_ANT_BURST_SEGMENT_* values from <radiant/radiant_ant.h>. All three match
 * their sdk-ant counterparts, which is what makes radiant_ant_burst_tx() above a
 * plain forward. These three asserts are the whole guarantee behind that: if
 * either side ever moves, the file stops compiling instead of quietly marking
 * the last block of every burst as a middle one.
 */
BUILD_ASSERT(RADIANT_ANT_BURST_SEGMENT_CONTINUE == BURST_SEGMENT_CONTINUE,
	     "RADIANT_ANT_BURST_SEGMENT_CONTINUE has drifted from sdk-ant's "
	     "BURST_SEGMENT_CONTINUE");
BUILD_ASSERT(RADIANT_ANT_BURST_SEGMENT_START == BURST_SEGMENT_START,
	     "RADIANT_ANT_BURST_SEGMENT_START has drifted from sdk-ant's "
	     "BURST_SEGMENT_START");
BUILD_ASSERT(RADIANT_ANT_BURST_SEGMENT_END == BURST_SEGMENT_END,
	     "RADIANT_ANT_BURST_SEGMENT_END has drifted from sdk-ant's "
	     "BURST_SEGMENT_END - forwarding the segment byte untouched would "
	     "mark the last block of every burst as a middle block");
BUILD_ASSERT((RADIANT_ANT_BURST_SEGMENT_START | RADIANT_ANT_BURST_SEGMENT_END) !=
		     RADIANT_ANT_BURST_SEGMENT_START,
	     "RADIANT_ANT_BURST_SEGMENT_START and _END overlap, so a block that is "
	     "both first and last cannot be expressed");

/* ── Advanced burst ────────────────────────────────────────────────────────── */

WIRE_EQ(ADV_BURST_MODE_DISABLE, ADV_BURST_MODE_DISABLE);
WIRE_EQ(ADV_BURST_MODE_ENABLE, ADV_BURST_MODE_ENABLE);
WIRE_EQ(ADV_BURST_MODES_SIZE_8_BYTES, ADV_BURST_MODES_SIZE_8_BYTES);
WIRE_EQ(ADV_BURST_MODES_SIZE_16_BYTES, ADV_BURST_MODES_SIZE_16_BYTES);
WIRE_EQ(ADV_BURST_MODES_SIZE_24_BYTES, ADV_BURST_MODES_SIZE_24_BYTES);
WIRE_EQ(ADV_BURST_MODES_FREQ_HOP, ADV_BURST_MODES_FREQ_HOP);

/* ── Selective data update ─────────────────────────────────────────────────── */

WIRE_EQ(INVALID_SDU_MASK, INVALID_SDU_MASK);
/* Recovered here. It shares a bit with INVALID_SDU_MASK, which is exactly why
 * a backend must clear it before range-checking the mask number.
 */
WIRE_EQ(SDU_MASK_ACK_CONFIG_BIT, SDU_MASK_ACK_CONFIG_BIT);

/* ── Encryption ────────────────────────────────────────────────────────────── */

WIRE_EQ(ENCRYPTION_DISABLED_MODE, ENCRYPTION_DISABLED_MODE);
WIRE_EQ(ENCRYPTION_INFO_SET_CRYPTO_ID, ENCRYPTION_INFO_SET_CRYPTO_ID);
WIRE_EQ(ENCRYPTION_INFO_SET_CUSTOM_USER_DATA,
	ENCRYPTION_INFO_SET_CUSTOM_USER_DATA);
WIRE_EQ(ENCRYPTION_INFO_SET_RNG_SEED, ENCRYPTION_INFO_SET_RNG_SEED);
WIRE_EQ(ENCRYPTION_INFO_GET_SUPPORTED_MODE, ENCRYPTION_INFO_GET_SUPPORTED_MODE);
WIRE_EQ(ENCRYPTION_INFO_GET_CRYPTO_ID, ENCRYPTION_INFO_GET_CRYPTO_ID);
WIRE_EQ(ENCRYPTION_INFO_GET_CUSTOM_USER_DATA,
	ENCRYPTION_INFO_GET_CUSTOM_USER_DATA);
WIRE_EQ(ENCRYPTION_USER_DATA_SIZE, ENCRYPTION_USER_DATA_SIZE);
WIRE_EQ(ENCRYPTION_KEY_SIZE, SIZE_OF_ENCRYPTED_KEY);
/* Recovered here. It is the highest of the modes above rather than an
 * independent number, so it is asserted against the mode as well as the value.
 */
WIRE_EQ(MAX_SUPPORTED_ENCRYPTION_MODE, MAX_SUPPORTED_ENCRYPTION_MODE);
BUILD_ASSERT(RADIANT_WIRE_MAX_SUPPORTED_ENCRYPTION_MODE ==
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
BUILD_ASSERT(RADIANT_WIRE_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_5 + 1 ==
		     MESG_CAPABILITIES_SIZE,
	     "the capabilities reply is no longer exactly as long as the last "
	     "byte offset radiant_wire.h names");

/* byte 2 - standard options. Ours say MESSAGES where sdk-ant says TRANSFER,
 * and RECEIVE/TRANSMIT where sdk-ant says RX/TX.
 */
WIRE_EQ(CAPABILITIES_NO_RECEIVE_CHANNELS, CAPABILITIES_NO_RX_CHANNELS);
WIRE_EQ(CAPABILITIES_NO_TRANSMIT_CHANNELS, CAPABILITIES_NO_TX_CHANNELS);
WIRE_EQ(CAPABILITIES_NO_RECEIVE_MESSAGES, CAPABILITIES_NO_RX_MESSAGES);
WIRE_EQ(CAPABILITIES_NO_TRANSMIT_MESSAGES, CAPABILITIES_NO_TX_MESSAGES);
WIRE_EQ(CAPABILITIES_NO_ACKD_MESSAGES, CAPABILITIES_NO_ACKD_MESSAGES);
WIRE_EQ(CAPABILITIES_NO_BURST_MESSAGES, CAPABILITIES_NO_BURST_TRANSFER);

/* byte 3 - advanced options. */
WIRE_EQ(CAPABILITIES_NETWORK_ENABLED, CAPABILITIES_NETWORK_ENABLED);
WIRE_EQ(CAPABILITIES_SERIAL_NUMBER_ENABLED, CAPABILITIES_SERIAL_NUMBER_ENABLED);
WIRE_EQ(CAPABILITIES_PER_CHANNEL_TX_POWER_ENABLED,
	CAPABILITIES_PER_CHANNEL_TX_POWER_ENABLED);
WIRE_EQ(CAPABILITIES_LOW_PRIORITY_SEARCH_ENABLED,
	CAPABILITIES_LOW_PRIORITY_SEARCH_ENABLED);
WIRE_EQ(CAPABILITIES_SCRIPT_ENABLED, CAPABILITIES_SCRIPT_ENABLED);
WIRE_EQ(CAPABILITIES_SEARCH_LIST_ENABLED, CAPABILITIES_SEARCH_LIST_ENABLED);

/* byte 4 - advanced options 2. */
WIRE_EQ(CAPABILITIES_LED_ENABLED, CAPABILITIES_LED_ENABLED);
WIRE_EQ(CAPABILITIES_EXT_MESSAGE_ENABLED, CAPABILITIES_EXT_MESSAGE_ENABLED);
WIRE_EQ(CAPABILITIES_SCAN_MODE_ENABLED, CAPABILITIES_SCAN_MODE_ENABLED);
WIRE_EQ(CAPABILITIES_PROX_SEARCH_ENABLED, CAPABILITIES_PROX_SEARCH_ENABLED);
WIRE_EQ(CAPABILITIES_EXT_ASSIGN_ENABLED, CAPABILITIES_EXT_ASSIGN_ENABLED);
WIRE_EQ(CAPABILITIES_FS_ANTFS_ENABLED, CAPABILITIES_FS_ANTFS_ENABLED);
WIRE_EQ(CAPABILITIES_FIT1_ENABLED, CAPABILITIES_FIT1_ENABLED);

/* byte 6 - advanced options 3. Two names carry an extra MODE in sdk-ant. */
WIRE_EQ(CAPABILITIES_ADVANCED_BURST_ENABLED,
	CAPABILITIES_ADVANCED_BURST_ENABLED);
WIRE_EQ(CAPABILITIES_EVENT_BUFFERING_ENABLED,
	CAPABILITIES_EVENT_BUFFERING_ENABLED);
WIRE_EQ(CAPABILITIES_EVENT_FILTERING_ENABLED,
	CAPABILITIES_EVENT_FILTERING_ENABLED);
WIRE_EQ(CAPABILITIES_HIGH_DUTY_SEARCH_ENABLED,
	CAPABILITIES_HIGH_DUTY_SEARCH_MODE_ENABLED);
WIRE_EQ(CAPABILITIES_SEARCH_SHARING_ENABLED,
	CAPABILITIES_ACTIVE_SEARCH_SHARING_MODE_ENABLED);
WIRE_EQ(CAPABILITIES_RADIO_COEX_CONFIG_ENABLED,
	CAPABILITIES_RADIO_COEX_CONFIG_ENABLED);
WIRE_EQ(CAPABILITIES_SELECTIVE_DATA_UPDATE_ENABLED,
	CAPABILITIES_SELECTIVE_DATA_UPDATE_ENABLED);
WIRE_EQ(CAPABILITIES_ENCRYPTED_CHANNEL_ENABLED,
	CAPABILITIES_ENCRYPTED_CHANNEL_ENABLED);

/*
 * byte 7 - advanced options 4. K1 recorded the byte (0x8D) and refused to name
 * its bits rather than guess; these seven names are the recovery, and the
 * asserts are what stop them being a guess in turn. Bit 0x80 is set in the
 * observed reply and has no name here either, so docs/ant-serial-protocol.md
 * still decodes that one bit as unnamed - which is the honest answer, not an
 * omission.
 */
WIRE_EQ(CAPABILITIES_RFACTIVE_NOTIFICATION_ENABLED,
	CAPABILITIES_RFACTIVE_NOTIFICATION_ENABLED);
WIRE_EQ(CAPABILITIES_DATA_FILTERING_ENABLED,
	CAPABILITIES_DATA_FILTERING_ENABLED);
WIRE_EQ(CAPABILITIES_SEARCH_UPLINK_ENABLED,
	CAPABILITIES_SEARCH_UPLINK_ENABLED);
WIRE_EQ(CAPABILITIES_GROUP_TRANSMITTER_INITIATION_ENABLED,
	CAPABILITIES_GROUP_TRANSMITTER_INITIATION_ENABLED);
WIRE_EQ(CAPABILITIES_TIME_BASE_ENABLED, CAPABILITIES_TIME_BASE_ENABLED);
WIRE_EQ(CAPABILITIES_TIME_SYNC_ENABLED, CAPABILITIES_TIME_SYNC_ENABLED);
WIRE_EQ(CAPABILITIES_PA_LNA_SUPPORT_ENABLED,
	CAPABILITIES_PA_LNA_SUPPORT_ENABLED);

/* byte 8 - advanced options 5. Same story: three of the four set bits in the
 * observed 0x0F have names, and 0x08 does not.
 */
WIRE_EQ(CAPABILITIES_CHANNEL_START_OFFSET_ENABLED,
	CAPABILITIES_CHANNEL_START_OFFSET_ENABLED);
WIRE_EQ(CAPABILITIES_SEARCHING_CHANNEL_UPLINK_ENABLED,
	CAPABILITIES_SEARCHING_CHANNEL_UPLINK_ENABLED);
WIRE_EQ(CAPABILITIES_ID_MATCH_FIX_ON_SHARED_CHANNEL_RXBURST,
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
