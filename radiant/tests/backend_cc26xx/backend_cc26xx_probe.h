/* SPDX-License-Identifier: Apache-2.0 */
/*
 * What a radiant.backend_cc26xx test sees of the backend beyond the fake RF
 * driver (fakes/ti/drivers/rf/RF.h): the core's callbacks as the backend
 * delivered them, a few reads of the backend's private state, and helpers
 * that build requests and on-air payloads for the tracking format.
 */
#ifndef RADIANT_TESTS_BACKEND_CC26XX_PROBE_H_
#define RADIANT_TESTS_BACKEND_CC26XX_PROBE_H_

#include <stdbool.h>
#include <stdint.h>

#include <radiant/radiant_radio_hal.h>

#define CC_PROBE_LOG 16

/* One core rx callback, copied (body included) while it ran. */
struct cc_rx_rec {
	uint32_t       op;
	int            status;
	radiant_time_t t_sync;
	bool           t_sync_exact;
	uint8_t        filter_index;
	uint8_t        wild_value;
	uint8_t        body[RADIANT_RADIO_BODY_MAX];
	uint8_t        body_len;
	bool           has_rssi;
	int8_t         rssi_dbm;
	bool           has_crc_rx;
	uint32_t       crc_rx;
	bool           busy_in_cb;  /* the backend still held an op during it */
};

struct cc_tx_rec {
	uint32_t       op;
	int            status;
	radiant_time_t t_sync;
	bool           t_sync_exact;
	bool           busy_in_cb;
};

/*
 * Every core callback, in order. rx[]/tx[] keep the first CC_PROBE_LOG of
 * each; order[] is 'R'/'T' per callback. on_rx/on_tx, when set, run inside
 * the callback after it is recorded, so a test can arm the next operation
 * from there exactly as the core does (a chained reply).
 */
struct cc_probe {
	uint32_t         rx_events;
	uint32_t         tx_events;
	struct cc_rx_rec rx[CC_PROBE_LOG];
	struct cc_tx_rec tx[CC_PROBE_LOG];
	char             order[2 * CC_PROBE_LOG + 1];
	void (*on_rx)(const struct radiant_rx_event *evt);
	void (*on_tx)(const struct radiant_tx_event *evt);
};

struct cc_probe *cc_probe(void);

/* Fresh backend and fake: fake_rf_reset(), the RAT at CC_RAT_START, the probe
 * cleared, radiant_radio_init(), and radiant_radio_enable() if `enable`. */
#define CC_RAT_START (4u * 1000000u)  /* 1 s into the RAT */
void cc_reset(bool enable);

/* The backend's private state. kind: 0 none, 1 rx, 2 tx (enum op_kind). */
int      cc_kind(void);
uint32_t cc_op_id(void);
int16_t  cc_cmd_handle(void);
int16_t  cc_retired_handle(void);
uint8_t  cc_hw_addr_len(void);
uint8_t  cc_sw_addr_len(void);
bool     cc_delivered_terminal(void);
/* Seed the backend's next operation id (st.next_op_id), to test its wrap. */
void     cc_set_next_op_id(uint32_t v);
uint32_t cc_sw_addr_mismatch(void);

/* The tracking format and a fixed test address (A6 C5 34 12 78). */
const struct radiant_pkt_format *cc_fmt(void);
extern const uint8_t cc_addr[RADIANT_RADIO_ADDR_MAX];

/* A one-filter tracking receive on cc_addr, [t_open, t_open + len_us]. */
void cc_rx_req(struct radiant_rx_req *req, struct radiant_rx_filter *filt,
	       radiant_time_t t_open, uint32_t len_us);
/* A tracking transmit on cc_addr at t_sync, body from `body`. */
void cc_tx_req(struct radiant_tx_req *req, const uint8_t *body,
	       radiant_time_t t_sync);

/*
 * The bytes the RF core would store after the sync word for a frame on
 * `addr`: the address bytes past the hardware match (hw_len), the body, then
 * the CRC (big-endian, CRC-16/CCITT over the whole address and body, computed
 * here independently of the backend). `corrupt_crc` flips its low bit.
 * Returns the length written.
 */
uint8_t  cc_payload(const uint8_t *addr, uint8_t hw_len, const uint8_t *body,
		    bool corrupt_crc, uint8_t *out);

/* The RAT capture the RF core would append for a frame whose t_sync is t,
 * on the operation now armed (the software-matched bytes' airtime removed). */
uint32_t cc_rat_for_t_sync(radiant_time_t t);

#endif
