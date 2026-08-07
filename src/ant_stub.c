/*
 * ANT radio stub — compiled instead of sdk-ant's libant.a when
 * CONFIG_ANT_DONGLE_RADIO_STUB=y.
 *
 * Purpose: bring up and iterate on the USB half of the dongle (enumeration,
 * WinUSB auto-binding, the 0xA4 serial bridge) without a working ANT build.
 * ant_serial_bridge.c and usb_ant_class.c are unchanged and unaware of this
 * file — every entry point here has the exact signature declared in sdk-ant's
 * ant_interface.h / ant_init.h.
 *
 * Behaviour: configuration calls are accepted and discarded; getters return
 * zeroed or obviously-synthetic data. Nothing is ever transmitted, and the
 * registered event callback is never invoked, so a host sees a dongle that
 * enumerates and answers reset / capabilities / version but finds no sensors.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ant_interface.h"
#include "ant_parameters.h"
#include "ant_init.h"

LOG_MODULE_REGISTER(ant_stub, LOG_LEVEL_INF);

/* Advertised channel count. The real build takes this from
 * CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED, which does not exist when CONFIG_ANT=n.
 */
#define STUB_MAX_CHANNELS  8
#define STUB_MAX_NETWORKS  3

/* 20-byte MESG_VERSION_ID payload. Deliberately not a real ANT version string
 * so a stub image is identifiable from the host side.
 */
static const char stub_version[] = "STUB0.01B00";

static ant_evt_callback_t stub_evt_cb;

/* ── Init / callback registration (ant_init.h) ─────────────────────────────── */

ant_err_t ant_init(void)
{
	LOG_WRN("ANT radio is STUBBED — no sensor traffic will be seen");
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_cb_register(ant_evt_callback_t evt_handler)
{
	stub_evt_cb = evt_handler;
	return NRF_ANT_SUCCESS;
}

/* ── Getters (ant_interface.h) ─────────────────────────────────────────────── */

ant_err_t ant_capabilities_get(uint8_t *aucCapabilities)
{
	aucCapabilities[0] = STUB_MAX_CHANNELS;  /* max ANT channels */
	aucCapabilities[1] = STUB_MAX_NETWORKS;  /* max networks */
	aucCapabilities[2] = 0x00;               /* standard options */
	aucCapabilities[3] = CAPABILITIES_NETWORK_ENABLED |
			     CAPABILITIES_SERIAL_NUMBER_ENABLED |
			     CAPABILITIES_PER_CHANNEL_TX_POWER_ENABLED |
			     CAPABILITIES_LOW_PRIORITY_SEARCH_ENABLED;
	aucCapabilities[4] = CAPABILITIES_EXT_MESSAGE_ENABLED |
			     CAPABILITIES_SCAN_MODE_ENABLED |
			     CAPABILITIES_EXT_ASSIGN_ENABLED;
	aucCapabilities[5] = 0x00;               /* max SensRcore channels */
	aucCapabilities[6] = 0x00;               /* advanced options 3 */
	aucCapabilities[7] = 0x00;               /* advanced options 4 */
	aucCapabilities[8] = 0x00;               /* advanced options 5 */

	return NRF_ANT_SUCCESS;
}

ant_err_t ant_version_get(uint8_t *aucVersion)
{
	memset(aucVersion, 0, MESG_VERSION_SIZE);
	memcpy(aucVersion, stub_version, sizeof(stub_version));
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_channel_status_get(uint8_t ucChannel, uint8_t *pucStatus)
{
	ARG_UNUSED(ucChannel);
	*pucStatus = STATUS_UNASSIGNED_CHANNEL;
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_channel_id_get(uint8_t ucChannel, uint16_t *pusDeviceNumber,
			     uint8_t *pucDeviceType, uint8_t *pucTransmitType)
{
	ARG_UNUSED(ucChannel);
	*pusDeviceNumber = 0;
	*pucDeviceType = 0;
	*pucTransmitType = 0;
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_channel_period_get(uint8_t ucChannel, uint16_t *pusPeriod)
{
	ARG_UNUSED(ucChannel);
	*pusPeriod = 0;
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_channel_radio_freq_get(uint8_t ucChannel, uint8_t *pucRfreq)
{
	ARG_UNUSED(ucChannel);
	*pucRfreq = 0;
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_channel_radio_crc_mode_get(uint8_t ucChannel, uint8_t *ucCRCModeOut)
{
	ARG_UNUSED(ucChannel);
	*ucCRCModeOut = 0;
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_search_channel_priority_get(uint8_t ucChannel, uint8_t *pucSearchPriority)
{
	ARG_UNUSED(ucChannel);
	*pucSearchPriority = 0;
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_pending_transmit(uint8_t ucChannel, uint8_t *pucPending)
{
	ARG_UNUSED(ucChannel);
	*pucPending = 0;
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_lib_config_get(uint8_t *pucANTLibConfig)
{
	*pucANTLibConfig = 0;
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_active_search_sharing_cycles_get(uint8_t ucChannel, uint8_t *pucCycles)
{
	ARG_UNUSED(ucChannel);
	*pucCycles = 0;
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_event_filtering_get(uint16_t *pusFilter)
{
	*pusFilter = 0;
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_adv_burst_config_get(uint8_t ucRequestType, uint8_t *aucConfig)
{
	ARG_UNUSED(ucRequestType);
	memset(aucConfig, 0, MESG_MAX_SIZE_VALUE);
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_sdu_mask_get(uint8_t ucMask, uint8_t *aucMask)
{
	ARG_UNUSED(ucMask);
	memset(aucMask, 0, MESG_ANT_MAX_PAYLOAD_SIZE + MESG_CHANNEL_NUM_SIZE);
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_crypto_info_get(uint8_t ucType, uint8_t *aucInfo)
{
	ARG_UNUSED(ucType);
	memset(aucInfo, 0, MESG_MAX_SIZE_VALUE);
	return NRF_ANT_SUCCESS;
}

/* ── Setters and data paths (ant_interface.h) ──────────────────────────────── */

ant_err_t ant_network_address_set(uint8_t ucNetwork, const uint8_t *aucNetworkKey)
{
	ARG_UNUSED(ucNetwork);
	ARG_UNUSED(aucNetworkKey);
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_channel_assign(uint8_t ucChannel, uint8_t ucChannelType,
			     uint8_t ucNetwork, uint8_t ucExtAssign)
{
	ARG_UNUSED(ucChannelType);
	ARG_UNUSED(ucNetwork);
	ARG_UNUSED(ucExtAssign);

	return (ucChannel < STUB_MAX_CHANNELS) ? NRF_ANT_SUCCESS
					       : NRF_ANT_ERROR_INVALID_MESSAGE;
}

ant_err_t ant_channel_id_set(uint8_t ucChannel, uint16_t usDeviceNumber,
			     uint8_t ucDeviceType, uint8_t ucTransmitType)
{
	ARG_UNUSED(ucChannel);
	ARG_UNUSED(usDeviceNumber);
	ARG_UNUSED(ucDeviceType);
	ARG_UNUSED(ucTransmitType);
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_channel_radio_freq_set(uint8_t ucChannel, uint8_t ucFreq)
{
	ARG_UNUSED(ucChannel);
	ARG_UNUSED(ucFreq);
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_channel_period_set(uint8_t ucChannel, uint16_t usPeriod)
{
	ARG_UNUSED(ucChannel);
	ARG_UNUSED(usPeriod);
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_channel_search_timeout_set(uint8_t ucChannel, uint8_t ucTimeout)
{
	ARG_UNUSED(ucChannel);
	ARG_UNUSED(ucTimeout);
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_channel_low_priority_rx_search_timeout_set(uint8_t ucChannel,
							 uint8_t ucTimeout)
{
	ARG_UNUSED(ucChannel);
	ARG_UNUSED(ucTimeout);
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_channel_radio_tx_power_set(uint8_t ucChannel, uint8_t ucTxPower,
					 uint8_t ucCustomTxPower)
{
	ARG_UNUSED(ucChannel);
	ARG_UNUSED(ucTxPower);
	ARG_UNUSED(ucCustomTxPower);
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_channel_open_with_offset(uint8_t ucChannel, uint16_t usOffset)
{
	ARG_UNUSED(usOffset);

	return (ucChannel < STUB_MAX_CHANNELS) ? NRF_ANT_SUCCESS
					       : NRF_ANT_ERROR_INVALID_MESSAGE;
}

ant_err_t ant_channel_close(uint8_t ucChannel)
{
	ARG_UNUSED(ucChannel);
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_channel_unassign(uint8_t ucChannel)
{
	ARG_UNUSED(ucChannel);
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_broadcast_message_tx(uint8_t ucChannel, uint8_t ucSize, uint8_t *aucMesg)
{
	ARG_UNUSED(ucChannel);
	ARG_UNUSED(ucSize);
	ARG_UNUSED(aucMesg);
	return NRF_ANT_SUCCESS;
}

ant_err_t ant_acknowledge_message_tx(uint8_t ucChannel, uint8_t ucSize, uint8_t *aucMesg)
{
	ARG_UNUSED(ucChannel);
	ARG_UNUSED(ucSize);
	ARG_UNUSED(aucMesg);
	return NRF_ANT_SUCCESS;
}
