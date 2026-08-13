/* SPDX-License-Identifier: Apache-2.0 */

/*
 * ANT radio stub - compiled instead of a real radio backend when the build is
 * configured with -DANT_RADIO=stub.
 *
 * Purpose: bring up and iterate on the USB half of the dongle (enumeration,
 * WinUSB auto-binding, the 0xA4 serial bridge) without a working ANT build.
 * ant_serial_bridge.c and usb_ant_class.c are unchanged and unaware of this
 * file - every entry point here matches the signature in src/ant_radio.h.
 *
 * Also the cheapest proof the src/ant_radio.h seam holds: this file builds
 * and links with no sdk-ant present at all - no sdk-ant header, no include
 * path, no CONFIG_ANT* symbol, libant.a not on the link line. Anything added
 * here that reaches for an sdk-ant name silently reintroduces the dependency
 * this file exists to disprove.
 *
 * Behaviour: configuration calls are accepted and discarded; getters return
 * zeroed or obviously-synthetic data. Nothing is ever transmitted and
 * antr_on_message() is never called, so a host sees a dongle that enumerates
 * and answers reset / capabilities / version but finds no sensors.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ant_radio.h"
#include "ant_wire.h"

LOG_MODULE_REGISTER(ant_radio_stub, LOG_LEVEL_INF);

/* Advertised channel count. A real build sizes this from its own stack
 * configuration; there is no such symbol here, because a stub build sources no
 * radio Kconfig at all. The sdk-ant total-channels setting - the CONFIG_ANT*
 * family generally - does not exist in this image and cannot be referred to
 * even conditionally.
 */
#define STUB_MAX_CHANNELS  8
#define STUB_MAX_NETWORKS  3

/* 20-byte MESG_VERSION_ID payload. Deliberately not a real ANT version string
 * so a stub image is identifiable from the host side.
 */
static const char stub_version[] = "STUB0.01B00";

/*
 * Three constants this file needs (ANTW_SDU_MASK_ACK_CONFIG_BIT,
 * ANTW_MAX_SUPPORTED_ENCRYPTION_MODE, ANTW_MESG_CONFIG_ADV_BURST_REQ_
 * CAPABILITIES_SIZE) were once UNRESOLVED and stood in behind local STUB_*
 * placeholders rather than a conditional sdk-ant include, which would have
 * reintroduced the dependency this file exists to disprove. All three have
 * since been recovered into src/ant_wire.h and BUILD_ASSERTed against
 * sdk-ant elsewhere, so the real names are used directly now. One of the
 * three placeholder guesses was wrong (encryption mode 1 vs the real 2);
 * this stub's behaviour changed accordingly, towards what a real stack says.
 */

/* ── Configuration state ───────────────────────────────────────────────────── */

/* Configuration is discarded except where the protocol lets a host read it
 * back - a set/get pair that always reads zero couldn't distinguish a
 * mangling bridge from a discarding stub - so those are stored and returned.
 */
#define STUB_SDU_MASKS  4

#define STUB_CRYPTO_KEYS   4
#define STUB_CRYPTO_ID_SIZE (ANTW_MESG_CONFIG_ENCRYPT_REQ_CONFIG_ID_SIZE - \
			     ANTW_CHANNEL_NUM_SIZE)

static uint16_t stub_event_filter;
/* [enable, rf payload size, required modes, 0, 0, optional modes, 0, 0,
 *  stall count lsb, stall count msb, retry extension] */
static uint8_t  stub_adv_burst_cfg[11];
static uint8_t  stub_sdu_mask[STUB_SDU_MASKS][ANTW_ANT_MAX_PAYLOAD_SIZE];
static uint8_t  stub_sdu_channel_cfg[STUB_MAX_CHANNELS];
static uint8_t  stub_crypto_id[STUB_CRYPTO_ID_SIZE];
static uint8_t  stub_crypto_user_data[ANTW_ENCRYPTION_USER_DATA_SIZE];
static uint8_t  stub_crypto_key[STUB_CRYPTO_KEYS][ANTW_ENCRYPTION_KEY_SIZE];
static uint8_t  stub_crypto_channel_mode[STUB_MAX_CHANNELS];

/* ── Initialisation and stack lifecycle ────────────────────────────────────── */

antr_err_t antr_init(void)
{
	LOG_WRN("ANT radio is STUBBED — no sensor traffic will be seen");
	return ANTW_RESPONSE_NO_ERROR;
}

/*
 * There is no antr_cb_register() to answer here and no callback to hold: the
 * event path is inverted, so a backend calls antr_on_message() - which
 * src/ant_serial_bridge.c defines - and the symbol is resolved at link time.
 * This stub never calls it, which is the whole of its event behaviour.
 */

antr_err_t antr_stack_reset(void)
{
	/* There is no state to discard: the stub accepts configuration and
	 * throws it away. It still has to exist, because the bridge calls it
	 * for MESG_SYSTEM_RESET.
	 */
	return ANTW_RESPONSE_NO_ERROR;
}

/* ── Getters ───────────────────────────────────────────────────────────────── */

antr_err_t antr_capabilities_get(uint8_t *capabilities)
{
	capabilities[0] = STUB_MAX_CHANNELS;  /* max ANT channels */
	capabilities[1] = STUB_MAX_NETWORKS;  /* max networks */
	capabilities[2] = 0x00;               /* standard options */
	capabilities[3] = ANTW_CAPABILITIES_NETWORK_ENABLED |
			  ANTW_CAPABILITIES_SERIAL_NUMBER_ENABLED |
			  ANTW_CAPABILITIES_PER_CHANNEL_TX_POWER_ENABLED |
			  ANTW_CAPABILITIES_LOW_PRIORITY_SEARCH_ENABLED;
	capabilities[4] = ANTW_CAPABILITIES_EXT_MESSAGE_ENABLED |
			  ANTW_CAPABILITIES_SCAN_MODE_ENABLED |
			  ANTW_CAPABILITIES_EXT_ASSIGN_ENABLED;
	capabilities[5] = 0x00;               /* max SensRcore channels */
	/* Advanced options 3 reports exactly the optional features this file
	 * implements, so tools/ant_features.py means something against a stub
	 * build. Encryption is advertised whether or not its write side was
	 * compiled in, which matches the real stack: the bit describes what the
	 * radio layer can do, not what the bridge chose to expose.
	 */
	capabilities[6] = ANTW_CAPABILITIES_ADVANCED_BURST_ENABLED |
			  ANTW_CAPABILITIES_EVENT_FILTERING_ENABLED |
			  ANTW_CAPABILITIES_SELECTIVE_DATA_UPDATE_ENABLED |
			  ANTW_CAPABILITIES_ENCRYPTED_CHANNEL_ENABLED;
	capabilities[7] = 0x00;               /* advanced options 4 */
	capabilities[8] = 0x00;               /* advanced options 5 */

	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_version_get(uint8_t *version)
{
	memset(version, 0, ANTW_MESG_VERSION_SIZE);
	memcpy(version, stub_version, sizeof(stub_version));
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_channel_status_get(uint8_t channel, uint8_t *status)
{
	ARG_UNUSED(channel);
	*status = ANTW_STATUS_UNASSIGNED_CHANNEL;
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_channel_id_get(uint8_t channel, uint16_t *device_number,
			       uint8_t *device_type, uint8_t *trans_type)
{
	ARG_UNUSED(channel);
	*device_number = 0;
	*device_type = 0;
	*trans_type = 0;
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_channel_period_get(uint8_t channel, uint16_t *period)
{
	ARG_UNUSED(channel);
	*period = 0;
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_channel_radio_freq_get(uint8_t channel, uint8_t *freq)
{
	ARG_UNUSED(channel);
	*freq = 0;
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_channel_radio_crc_mode_get(uint8_t channel, uint8_t *mode)
{
	ARG_UNUSED(channel);
	*mode = 0;
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_search_channel_priority_get(uint8_t channel, uint8_t *priority)
{
	ARG_UNUSED(channel);
	*priority = 0;
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_pending_transmit_get(uint8_t channel, uint8_t *pending)
{
	ARG_UNUSED(channel);
	*pending = 0;
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_lib_config_get(uint8_t *config)
{
	*config = 0;
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_active_search_sharing_cycles_get(uint8_t channel,
						 uint8_t *cycles)
{
	ARG_UNUSED(channel);
	*cycles = 0;
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_event_filtering_get(uint16_t *filter)
{
	*filter = stub_event_filter;
	return ANTW_RESPONSE_NO_ERROR;
}

/* Configuration drops the enable byte the set took; capabilities is a different
 * and shorter structure. Neither includes the leading byte the serial reply
 * carries - the bridge supplies that.
 */
antr_err_t antr_adv_burst_config_get(uint8_t request_type, uint8_t *config)
{
	if (request_type) {
		memcpy(config, &stub_adv_burst_cfg[1],
		       sizeof(stub_adv_burst_cfg) - 1);
	} else {
		memset(config, 0,
		       ANTW_MESG_CONFIG_ADV_BURST_REQ_CAPABILITIES_SIZE);
		config[0] = ANTW_ADV_BURST_MODES_SIZE_24_BYTES;
		config[1] = ANTW_ADV_BURST_MODES_FREQ_HOP;
	}
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_sdu_mask_get(uint8_t mask_index, uint8_t *mask)
{
	if (mask_index >= STUB_SDU_MASKS) {
		return ANTW_INVALID_PARAMETER_PROVIDED;
	}
	memcpy(mask, stub_sdu_mask[mask_index], ANTW_ANT_MAX_PAYLOAD_SIZE);
	return ANTW_RESPONSE_NO_ERROR;
}

/* Each of these writes exactly what its serial reply size accounts for, minus
 * the leading info-type byte. Zeroing the whole buffer instead overruns the
 * caller's, which is sized for the largest reply *including* that byte.
 */
antr_err_t antr_crypto_info_get(uint8_t type, uint8_t *info)
{
	switch (type) {
	case ANTW_ENCRYPTION_INFO_GET_SUPPORTED_MODE:
		info[0] = ANTW_MAX_SUPPORTED_ENCRYPTION_MODE;
		break;
	case ANTW_ENCRYPTION_INFO_GET_CRYPTO_ID:
		memcpy(info, stub_crypto_id, sizeof(stub_crypto_id));
		break;
	case ANTW_ENCRYPTION_INFO_GET_CUSTOM_USER_DATA:
		memcpy(info, stub_crypto_user_data,
		       sizeof(stub_crypto_user_data));
		break;
	default:
		return ANTW_INVALID_PARAMETER_PROVIDED;
	}
	return ANTW_RESPONSE_NO_ERROR;
}

/* ── Setters and data paths ────────────────────────────────────────────────── */

antr_err_t antr_network_address_set(uint8_t network, const uint8_t *key)
{
	ARG_UNUSED(network);
	ARG_UNUSED(key);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_channel_assign(uint8_t channel, uint8_t type, uint8_t network,
			       uint8_t ext_assign)
{
	ARG_UNUSED(type);
	ARG_UNUSED(network);
	ARG_UNUSED(ext_assign);

	return (channel < STUB_MAX_CHANNELS) ? ANTW_RESPONSE_NO_ERROR
					     : ANTW_INVALID_MESSAGE;
}

antr_err_t antr_channel_id_set(uint8_t channel, uint16_t device_number,
			       uint8_t device_type, uint8_t trans_type)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(device_number);
	ARG_UNUSED(device_type);
	ARG_UNUSED(trans_type);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_channel_radio_freq_set(uint8_t channel, uint8_t freq)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(freq);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_channel_period_set(uint8_t channel, uint16_t period)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(period);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_channel_search_timeout_set(uint8_t channel, uint8_t timeout)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(timeout);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_channel_low_priority_rx_search_timeout_set(uint8_t channel,
							   uint8_t timeout)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(timeout);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_channel_radio_tx_power_set(uint8_t channel, uint8_t level,
					   uint8_t custom)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(level);
	ARG_UNUSED(custom);
	return ANTW_RESPONSE_NO_ERROR;
}

/* antr_channel_open() is a static inline in src/ant_radio.h over this
 * function, so only the offset form is defined here - defining both would be a
 * duplicate symbol.
 */
antr_err_t antr_channel_open_with_offset(uint8_t channel, uint16_t offset)
{
	ARG_UNUSED(offset);

	return (channel < STUB_MAX_CHANNELS) ? ANTW_RESPONSE_NO_ERROR
					     : ANTW_INVALID_MESSAGE;
}

antr_err_t antr_channel_close(uint8_t channel)
{
	ARG_UNUSED(channel);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_channel_unassign(uint8_t channel)
{
	ARG_UNUSED(channel);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_broadcast_message_tx(uint8_t channel, uint8_t size,
				     const uint8_t *data)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(size);
	ARG_UNUSED(data);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_acknowledge_message_tx(uint8_t channel, uint8_t size,
				       const uint8_t *data)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(size);
	ARG_UNUSED(data);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_burst_tx(uint8_t channel, uint16_t size, uint8_t *data,
			 uint8_t segment)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(size);
	ARG_UNUSED(data);
	ARG_UNUSED(segment);
	/* Deliberately declines burst-contract obligation B3 (src/ant_radio.h):
	 * never raises the release event, so a burst against this stub stalls
	 * 1000 ms per packet and reports ANTW_TRANSFER_IN_PROGRESS. Accepted -
	 * this stub exists for USB bring-up, not burst.
	 */
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_lib_config_set(uint8_t config)
{
	ARG_UNUSED(config);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_lib_config_clear(uint8_t config)
{
	ARG_UNUSED(config);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_search_waveform_set(uint8_t channel, uint16_t waveform)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(waveform);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_prox_search_set(uint8_t channel, uint8_t threshold,
				uint8_t custom)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(threshold);
	ARG_UNUSED(custom);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_search_channel_priority_set(uint8_t channel, uint8_t priority)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(priority);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_active_search_sharing_cycles_set(uint8_t channel,
						 uint8_t cycles)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(cycles);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_auto_freq_hop_table_set(uint8_t channel, uint8_t freq0,
					uint8_t freq1, uint8_t freq2)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(freq0);
	ARG_UNUSED(freq1);
	ARG_UNUSED(freq2);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_channel_radio_crc_mode_set(uint8_t channel, uint8_t mode)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(mode);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_id_list_add(uint8_t channel, const uint8_t *device_id,
			    uint8_t index)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(device_id);
	ARG_UNUSED(index);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_id_list_config(uint8_t channel, uint8_t size, uint8_t inc_exc)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(size);
	ARG_UNUSED(inc_exc);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_event_filtering_set(uint16_t filter)
{
	stub_event_filter = filter;
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_adv_burst_config_set(const uint8_t *config, uint8_t size)
{
	/* 8 octets is the required part, 11 the whole thing. The real stack
	 * rejects anything outside that, and so does this - a bridge that passed
	 * the wrong slice of the message would otherwise look fine here.
	 */
	if (size < 8 || size > sizeof(stub_adv_burst_cfg)) {
		return ANTW_INVALID_PARAMETER_PROVIDED;
	}
	memset(stub_adv_burst_cfg, 0, sizeof(stub_adv_burst_cfg));
	memcpy(stub_adv_burst_cfg, config, size);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_sdu_mask_set(uint8_t mask_index, const uint8_t *mask)
{
	if (mask_index >= STUB_SDU_MASKS) {
		return ANTW_INVALID_PARAMETER_PROVIDED;
	}
	memcpy(stub_sdu_mask[mask_index], mask, ANTW_ANT_MAX_PAYLOAD_SIZE);
	return ANTW_RESPONSE_NO_ERROR;
}

/* The encryption writes are defined unconditionally, whatever the dongle's
 * encryption Kconfig says: this file stands in for a whole radio backend,
 * which exports them regardless of which of them the bridge chooses to call.
 */
antr_err_t antr_crypto_channel_enable(uint8_t channel, uint8_t enable,
				      uint8_t key_num, uint8_t decimation)
{
	ARG_UNUSED(decimation);

	if (channel >= STUB_MAX_CHANNELS || key_num >= STUB_CRYPTO_KEYS ||
	    enable > ANTW_MAX_SUPPORTED_ENCRYPTION_MODE) {
		return ANTW_INVALID_PARAMETER_PROVIDED;
	}
	stub_crypto_channel_mode[channel] = enable;
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_crypto_key_set(uint8_t key_num, const uint8_t *key)
{
	if (key_num >= STUB_CRYPTO_KEYS) {
		return ANTW_INVALID_PARAMETER_PROVIDED;
	}
	memcpy(stub_crypto_key[key_num], key, sizeof(stub_crypto_key[0]));
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_crypto_info_set(uint8_t type, const uint8_t *info)
{
	switch (type) {
	case ANTW_ENCRYPTION_INFO_SET_CRYPTO_ID:
		memcpy(stub_crypto_id, info, sizeof(stub_crypto_id));
		break;
	case ANTW_ENCRYPTION_INFO_SET_CUSTOM_USER_DATA:
		memcpy(stub_crypto_user_data, info,
		       sizeof(stub_crypto_user_data));
		break;
	case ANTW_ENCRYPTION_INFO_SET_RNG_SEED:
		/* Accepted and dropped. The bridge never sends this one - see
		 * the note in its dispatcher - but a real backend takes it.
		 */
		break;
	default:
		return ANTW_INVALID_PARAMETER_PROVIDED;
	}
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_sdu_mask_config(uint8_t channel, uint8_t mask_config)
{
	if (channel >= STUB_MAX_CHANNELS) {
		return ANTW_INVALID_MESSAGE;
	}
	/* ANTW_INVALID_SDU_MASK disables selective updates for the channel; any
	 * other value has to name a mask that exists.
	 */
	if (mask_config != ANTW_INVALID_SDU_MASK &&
	    (mask_config & ~ANTW_SDU_MASK_ACK_CONFIG_BIT) >= STUB_SDU_MASKS) {
		return ANTW_INVALID_PARAMETER_PROVIDED;
	}
	stub_sdu_channel_cfg[channel] = mask_config;
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_enhanced_channel_spacing_enable(uint8_t enable)
{
	ARG_UNUSED(enable);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_pending_transmit_clear(uint8_t channel, uint8_t *success)
{
	ARG_UNUSED(channel);
	*success = 1;
	return ANTW_RESPONSE_NO_ERROR;
}
