#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <dk_buttons_and_leds.h>

#include <zephyr/usb/usb_device.h>
/** #include <zephyr/usb/usbd.h> */
/**  #include <zephyr/usb/class/usbd_msc.h> */
/** #include <zephyr/usb/class/usb_cdc.h> */

#include "ant_error.h"
#include "ant_interface.h"
#include "ant_parameters.h"
#include "ant_host_init.h"
#include "ant_channel_config.h"

LOG_MODULE_REGISTER(ant_broadcast_rx, LOG_LEVEL_INF);

/**@brief Helper function for converting ANT message buffer to string. */
static void convert_buf_to_hex_str(uint8_t* buf, uint8_t buf_size, char* hex_str) {
    char* p_str = hex_str;
    for (uint8_t i = 0; i < buf_size; i++) {
        uint8_t upper = ((buf[i] >> 4) & 0x0F);
        uint8_t lower = ((buf[i] >> 0) & 0x0F);
        *p_str++ = (upper < 0x0A) ? (upper + '0') : (upper - 0x0A + 'A');
        *p_str++ = (lower < 0x0A) ? (lower + '0') : (lower - 0x0A + 'A');
        *p_str++ = ' ';
    }
    *p_str = '\0';
}

/**@brief Function for handling a ANT stack event.
 *
 * @param[in] p_ant_evt  ANT stack event.
 */
static void ant_evt_handler(ant_evt_t* p_ant_evt) {
    // hex_str size: 2 digit hex representations + spaces + null termination
    char hex_str[MESG_BUFFER_SIZE * 3 + 1];

    if (p_ant_evt->event) {
        convert_buf_to_hex_str(p_ant_evt->message.ANT_MESSAGE_aucMessage,
                               p_ant_evt->message.ANT_MESSAGE_ucSize + MESG_SIZE_SIZE + MESG_ID_SIZE,
                               hex_str);
    }

    // print out applicable decoded events
    switch (p_ant_evt->event) {
        case EVENT_TX:
            LOG_INF("EVENT_TX - %s", hex_str);
            break;
        case EVENT_RX_SEARCH_TIMEOUT:
            LOG_INF("EVENT_RX_SEARCH_TIMEOUT - %s", hex_str);
            break;
        case EVENT_RX_FAIL:
            LOG_INF("EVENT_RX_FAIL - %s", hex_str);
            break;
        case EVENT_TRANSFER_RX_FAILED:
            LOG_INF("EVENT_TRANSFER_RX_FAILED - %s", hex_str);
            break;
        case EVENT_TRANSFER_TX_COMPLETED:
            LOG_INF("EVENT_TRANSFER_TX_COMPLETED - %s", hex_str);
            break;
        case EVENT_TRANSFER_TX_FAILED:
            LOG_INF("EVENT_TRANSFER_TX_FAILED - %s", hex_str);
            break;
        case EVENT_CHANNEL_CLOSED:
            LOG_INF("EVENT_CHANNEL_CLOSED - %s", hex_str);
            break;
        case EVENT_RX_FAIL_GO_TO_SEARCH:
            LOG_INF("EVENT_RX_FAIL_GO_TO_SEARCH - %s", hex_str);
            break;
        case EVENT_CHANNEL_COLLISION:
            LOG_INF("EVENT_CHANNEL_COLLISION - %s", hex_str);
            break;
        case EVENT_TRANSFER_TX_START:
            LOG_INF("EVENT_TRANSFER_TX_START - %s", hex_str);
            break;
        case EVENT_TRANSFER_NEXT_DATA_BLOCK:
            LOG_INF("EVENT_TRANSFER_NEXT_DATA_BLOCK - %s", hex_str);
            break;
        case EVENT_RX:
            LOG_INF("EVENT_RX - %s", hex_str);
            break;
        default:
            LOG_INF("%s", hex_str);
            break;
    }
}

/**@brief Function for ANT stack initialization. */
static ant_err_t ant_stack_setup(void) {
    ant_err_t err_code;

    err_code = ant_init();
    if (err_code == 0) {
        LOG_INF("ANT Version %s", ANT_VERSION_STRING);
    } else {
        LOG_ERR("ant_init failed: 0x%X", err_code);
        return err_code;
    }

    err_code = ant_cb_register(&ant_evt_handler);
    if (err_code) {
        LOG_ERR("ant_cb_register() failed: 0x%X", err_code);
    }

    return err_code;
}

/**@brief Function for setting up the ANT module to be ready for RX broadcast. */
static ant_err_t ant_channel_rx_broadcast_setup(void) {
    ant_channel_config_t broadcast_channel_config = {
        .channel_number     = CONFIG_BROADCAST_RX_CHANNEL_NUM,
        .channel_type       = CHANNEL_TYPE_SLAVE,
        .ext_assign         = 0x00,
        .rf_freq            = CONFIG_BROADCAST_RX_RF_FREQ,
        .transmission_type  = CONFIG_BROADCAST_RX_CHAN_ID_TRANS_TYPE,
        .device_type        = CONFIG_BROADCAST_RX_CHAN_ID_DEV_TYPE,
        .device_number      = CONFIG_BROADCAST_RX_CHAN_ID_DEV_NUM,
        .channel_period     = CONFIG_BROADCAST_RX_CHAN_PERIOD,
        .network_number     = CONFIG_BROADCAST_RX_NETWORK_NUM,
    };

    ant_err_t err_code = ant_channel_init(&broadcast_channel_config);
    if (err_code) {
        LOG_ERR("ant_channel_init() failed: 0x%X", err_code);
        return err_code;
    }

    // Open channel.
    err_code = ant_channel_open(CONFIG_BROADCAST_RX_CHANNEL_NUM);
    if (err_code) {
        LOG_ERR("ant_channel_open() failed: 0x%X", err_code);
        return err_code;
    }

    return 0;
}

static int usb_device_init(void) {
    int ret;

    // Initialize USB stack
    ret = usb_enable(NULL);
    if (ret != 0) {
        LOG_ERR("USB initialization failed: %d", ret);
        return ret;
    }

    LOG_INF("USB initialized successfully");
    return 0;
}

int main(void) {
    ant_err_t err_code;

    err_code = ant_stack_setup();
    if (err_code) {
        LOG_ERR("ant_stack_setup() failed: 0x%X", err_code);
        k_oops();
    }

    err_code = ant_channel_rx_broadcast_setup();
    if (err_code) {
        k_oops();
    }

    // Initialize LED and turn it on to indicate power/application running
    int ret = dk_leds_init();
    if (ret) {
        LOG_ERR("dk_leds_init failed: %d", ret);
        k_oops();
    }
    dk_set_led_on(DK_LED1);
    LOG_INF("LED1 turned on to indicate the application is running.");

    // Enable USB device. This will make the device appear as a USB ANT dongle
    // once the proper VID/PID and related USB configuration are set in prj.conf
    ret = usb_device_init();
    if (ret) {
        LOG_ERR("USB device initialization failed");
        k_oops();
    }
    LOG_INF("USB device enabled, now enumerating as ANT+ USB Dongle.");

    LOG_INF("ANT Broadcast RX example with LED and USB started.");

    // Enter main loop
    for (;;) {
        k_sleep(K_MSEC(1000));
    }

    return 0;
}
