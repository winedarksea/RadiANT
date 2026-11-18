/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The RADIO, a TIMER and (D)PPI as RAM, for radiant/tests/backend. Original
 * work; the names are the ones radiant_radio_nrf.c calls, taken from its own
 * call sites. native_sim only: see testcase.yaml.
 */
#ifndef RADIANT_TESTS_BACKEND_FAKE_NRFX_H_
#define RADIANT_TESTS_BACKEND_FAKE_NRFX_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint32_t BASE0, BASE1, PREFIX0, PREFIX1, PCNF0, PCNF1, CRCCNF, CRCPOLY,
		 CRCINIT, PACKETPTR, RXADDRESSES, RSSISAMPLE, RXCRC, RXMATCH,
		 TXPOWER, MODE, FREQUENCY, TXADDRESS;
} NRF_RADIO_Type;
typedef struct { uint32_t unused; } NRF_TIMER_Type;
typedef struct { uint32_t unused; } NRF_TEMP_Type;

extern NRF_RADIO_Type fake_radio_regs;
extern NRF_TIMER_Type fake_timer_regs;
extern NRF_TEMP_Type  fake_temp_regs;
#define NRF_RADIO (&fake_radio_regs)
#define NRF_TEMP  (&fake_temp_regs)

#define RADIO_0_IRQn 3
#define RADIO_1_IRQn 4

typedef enum {
	NRF_RADIO_EVENT_ADDRESS, NRF_RADIO_EVENT_DISABLED, NRF_RADIO_EVENT_END,
	NRF_RADIO_EVENT_RXREADY, FAKE_RADIO_EVENT_COUNT
} nrf_radio_event_t;
typedef enum {
	NRF_RADIO_TASK_DISABLE, NRF_RADIO_TASK_RSSISTART, NRF_RADIO_TASK_RXEN,
	NRF_RADIO_TASK_STOP, NRF_RADIO_TASK_TXEN
} nrf_radio_task_t;
typedef enum { NRF_RADIO_MODE_NRF_1MBIT, NRF_RADIO_MODE_BLE_LR125KBIT } nrf_radio_mode_t;
typedef enum {
	NRF_TIMER_TASK_CLEAR = 100, NRF_TIMER_TASK_START = 101,
	NRF_TIMER_TASK_STOP = 102
} nrf_timer_task_t;
typedef int nrf_timer_event_t;
typedef int nrf_timer_cc_channel_t;
typedef enum { NRF_TIMER_MODE_TIMER } nrf_timer_mode_t;
typedef enum { NRF_TIMER_BIT_WIDTH_32 } nrf_timer_bit_width_t;
typedef enum { NRF_TEMP_TASK_START, NRF_TEMP_TASK_STOP } nrf_temp_task_t;
typedef enum { NRF_TEMP_EVENT_DATARDY } nrf_temp_event_t;
typedef int8_t nrf_radio_txpower_t;
typedef uint32_t nrfx_gppi_handle_t;

#define NRF_RADIO_INT_DISABLED_MASK            (1u << 0)
#define NRF_RADIO_INT_END_MASK                 (1u << 1)
#define NRF_RADIO_INT_RXREADY_MASK             (1u << 2)
#define NRF_RADIO_SHORT_ADDRESS_RSSISTART_MASK (1u << 0)
#define NRF_RADIO_SHORT_END_START_MASK         (1u << 1)
#define NRF_RADIO_SHORT_READY_START_MASK       (1u << 2)
#define NRF_TIMER_BASE_FREQUENCY_GET(t)        16000000u

#define RADIO_CRCCNF_LEN_Pos 0
#define RADIO_CRCCNF_LEN_Two 2
#define RADIO_CRCCNF_SKIPADDR_Pos 8
#define RADIO_CRCCNF_SKIPADDR_Skip 1
#define RADIO_CRCCNF_SKIPADDR_Include 0
#define RADIO_MODECNF0_RU_Fast 1
#define RADIO_MODECNF0_RU_Msk 1
#define RADIO_PCNF0_CILEN_Pos 22
#define RADIO_PCNF0_LFLEN_Pos 0
#define RADIO_PCNF0_PLEN_LongRange 3
#define RADIO_PCNF0_PLEN_Pos 24
#define RADIO_PCNF0_TERMLEN_Pos 29
#define RADIO_PCNF1_BALEN_Pos 16
#define RADIO_PCNF1_ENDIAN_Big 1
#define RADIO_PCNF1_ENDIAN_Pos 24
#define RADIO_PCNF1_MAXLEN_Pos 0
#define RADIO_PCNF1_STATLEN_Pos 8
#define RADIO_PCNF1_WHITEEN_Disabled 0
#define RADIO_PCNF1_WHITEEN_Pos 25
#define RADIO_RXMATCH_RXMATCH_Msk 7
#define RADIO_TXPOWER_TXPOWER_Neg40dBm (-40)
#define RADIO_TXPOWER_TXPOWER_Neg20dBm (-20)
#define RADIO_TXPOWER_TXPOWER_Neg16dBm (-16)
#define RADIO_TXPOWER_TXPOWER_Neg12dBm (-12)
#define RADIO_TXPOWER_TXPOWER_Neg8dBm  (-8)
#define RADIO_TXPOWER_TXPOWER_Neg4dBm  (-4)
#define RADIO_TXPOWER_TXPOWER_0dBm     0
#define RADIO_TXPOWER_TXPOWER_Pos2dBm  2
#define RADIO_TXPOWER_TXPOWER_Pos3dBm  3
#define RADIO_TXPOWER_TXPOWER_Pos4dBm  4
#define RADIO_TXPOWER_TXPOWER_Pos5dBm  5
#define RADIO_TXPOWER_TXPOWER_Pos6dBm  6
#define RADIO_TXPOWER_TXPOWER_Pos7dBm  7
#define RADIO_TXPOWER_TXPOWER_Pos8dBm  8

/*
 * The model a test drives. The TIMER counts only when software captures it:
 * each capture advances the counter by counter_step (1 if 0) and then latches
 * it, and a compare whose value the counter passed through on the way, (old,
 * new], raises its event, as the real compare fires on reaching its value. A
 * compare written at or behind the counter never fires. (D)PPI connections
 * are bits in conn_on, by handle.
 */
struct fake_hw {
	bool     event[FAKE_RADIO_EVENT_COUNT];
	bool     crc_ok;
	uint32_t shorts;
	uint32_t counter;
	uint32_t counter_step;
	uint32_t cc[8];
	bool     tev[8];
	uint32_t conn_on;
	/*
	 * The late-ISR guard's staging. Every read of CC 0 (CC_SYNC) counts in
	 * cc0_reads, and if cc0_hook is set it runs with the new count BEFORE the
	 * value is returned, so a test can move the capture, raise an event or
	 * rewrite the DMA buffer at a chosen point in the END branch. Captures
	 * into CC 3 (CC_NOW, the software clock read) count in cc3_captures, and
	 * cc3_hook runs AFTER the value is latched, so it stages what happens
	 * just after that clock read.
	 */
	uint32_t cc0_reads;
	void   (*cc0_hook)(uint32_t nth_read);
	uint32_t cc3_captures;
	void   (*cc3_hook)(uint32_t nth_capture);
	/*
	 * The low-power scenario's TIMER (backend_lp_under_test.c): when set, a
	 * capture latches counter_fn() instead of advancing by counter_step, and
	 * START/STOP/CLEAR go to task_hook, so the TIMER can run against a
	 * simulated clock and be stopped. Unset (every other suite), the model
	 * above is unchanged.
	 */
	uint32_t (*counter_fn)(void);
	void   (*task_hook)(nrf_timer_task_t task);
	uint32_t ep_attached;   /* the last nrfx_gppi_ep_attach() endpoint */
	bool     disable_hangs; /* TASKS_DISABLE never raises EVENTS_DISABLED */
	uint32_t disable_tasks; /* TASKS_DISABLE triggers */
};
extern struct fake_hw fake_hw;

static inline bool nrf_radio_event_check(NRF_RADIO_Type *r, nrf_radio_event_t e)
{ (void)r; return fake_hw.event[e]; }
static inline void nrf_radio_event_clear(NRF_RADIO_Type *r, nrf_radio_event_t e)
{ (void)r; fake_hw.event[e] = false; }
static inline void nrf_radio_task_trigger(NRF_RADIO_Type *r, nrf_radio_task_t t)
{
	(void)r;
	if (t == NRF_RADIO_TASK_DISABLE) {
		fake_hw.disable_tasks++;
		if (!fake_hw.disable_hangs) {
			fake_hw.event[NRF_RADIO_EVENT_DISABLED] = true;
		}
	}
}
static inline bool nrf_radio_crc_status_check(NRF_RADIO_Type *r)
{ (void)r; return fake_hw.crc_ok; }
static inline void nrf_radio_shorts_set(NRF_RADIO_Type *r, uint32_t m)
{ (void)r; fake_hw.shorts = m; }
static inline void nrf_radio_int_enable(NRF_RADIO_Type *r, uint32_t m) { (void)r; (void)m; }
static inline void nrf_radio_int_disable(NRF_RADIO_Type *r, uint32_t m) { (void)r; (void)m; }
static inline void nrf_radio_mode_set(NRF_RADIO_Type *r, nrf_radio_mode_t m) { r->MODE = (uint32_t)m; }
static inline void nrf_radio_frequency_set(NRF_RADIO_Type *r, uint16_t f) { r->FREQUENCY = f; }
static inline void nrf_radio_txaddress_set(NRF_RADIO_Type *r, uint8_t a) { r->TXADDRESS = a; }
static inline void nrf_radio_txpower_set(NRF_RADIO_Type *r, nrf_radio_txpower_t p) { (void)r; (void)p; }
static inline void nrf_radio_fast_ramp_up_enable_set(NRF_RADIO_Type *r, bool e) { (void)r; (void)e; }
static inline bool nrf_radio_fast_ramp_up_check(NRF_RADIO_Type *r) { (void)r; return true; }
static inline void nrf_radio_reset(NRF_RADIO_Type *r) { (void)r; }
static inline uint32_t nrf_radio_event_address_get(NRF_RADIO_Type *r, nrf_radio_event_t e) { (void)r; return (uint32_t)e; }
static inline uint32_t nrf_radio_task_address_get(NRF_RADIO_Type *r, nrf_radio_task_t t) { (void)r; return (uint32_t)t; }

static inline nrf_timer_task_t nrf_timer_capture_task_get(uint8_t cc) { return (nrf_timer_task_t)cc; }
static inline nrf_timer_event_t nrf_timer_compare_event_get(uint8_t cc) { return cc; }
static inline void nrf_timer_task_trigger(NRF_TIMER_Type *t, nrf_timer_task_t task)
{
	(void)t;
	if ((int)task < 8) {
		uint32_t old = fake_hw.counter;
		uint32_t step = (fake_hw.counter_step != 0u) ? fake_hw.counter_step : 1u;

		if (fake_hw.counter_fn != NULL) {
			step = fake_hw.counter_fn() - old;
		}
		fake_hw.counter = old + step;
		for (int c = 0; c < 8; c++) {
			if ((uint32_t)(fake_hw.cc[c] - old - 1u) < step) {
				fake_hw.tev[c] = true;
			}
		}
		fake_hw.cc[(int)task] = fake_hw.counter;
		if ((int)task == 3) {
			fake_hw.cc3_captures++;
			if (fake_hw.cc3_hook != NULL) {
				fake_hw.cc3_hook(fake_hw.cc3_captures);
			}
		}
	} else if (fake_hw.task_hook != NULL) {
		fake_hw.task_hook(task);
	}
}
static inline uint32_t nrf_timer_cc_get(NRF_TIMER_Type *t, uint8_t cc)
{
	(void)t;
	if (cc == 0u) {
		fake_hw.cc0_reads++;
		if (fake_hw.cc0_hook != NULL) {
			fake_hw.cc0_hook(fake_hw.cc0_reads);
		}
	}
	return fake_hw.cc[cc];
}
static inline void nrf_timer_cc_set(NRF_TIMER_Type *t, uint8_t cc, uint32_t v) { (void)t; fake_hw.cc[cc] = v; }
static inline bool nrf_timer_event_check(NRF_TIMER_Type *t, nrf_timer_event_t e) { (void)t; return fake_hw.tev[e]; }
static inline void nrf_timer_event_clear(NRF_TIMER_Type *t, nrf_timer_event_t e) { (void)t; fake_hw.tev[e] = false; }
static inline void nrf_timer_mode_set(NRF_TIMER_Type *t, nrf_timer_mode_t m) { (void)t; (void)m; }
static inline void nrf_timer_bit_width_set(NRF_TIMER_Type *t, nrf_timer_bit_width_t w) { (void)t; (void)w; }
static inline void nrf_timer_prescaler_set(NRF_TIMER_Type *t, uint32_t p) { (void)t; (void)p; }
static inline uint32_t nrf_timer_event_address_get(NRF_TIMER_Type *t, nrf_timer_event_t e) { (void)t; return (uint32_t)e; }
static inline uint32_t nrf_timer_task_address_get(NRF_TIMER_Type *t, nrf_timer_task_t k) { (void)t; return (uint32_t)k; }

static inline void nrf_temp_task_trigger(NRF_TEMP_Type *t, nrf_temp_task_t k) { (void)t; (void)k; }
static inline bool nrf_temp_event_check(NRF_TEMP_Type *t, nrf_temp_event_t e) { (void)t; (void)e; return true; }
static inline void nrf_temp_event_clear(NRF_TEMP_Type *t, nrf_temp_event_t e) { (void)t; (void)e; }
static inline int32_t nrf_temp_result_get(NRF_TEMP_Type *t) { (void)t; return 100; }

static inline uint32_t NVIC_GetEnableIRQ(int n) { (void)n; return 0u; }
static inline void NVIC_EnableIRQ(int n) { (void)n; }
static inline void NVIC_DisableIRQ(int n) { (void)n; }
static inline uint32_t NVIC_GetPendingIRQ(int n) { (void)n; return 0u; }
static inline void NVIC_ClearPendingIRQ(int n) { (void)n; }

static inline int nrfx_gppi_conn_alloc(uint32_t e, uint32_t t, nrfx_gppi_handle_t *h)
{ (void)e; (void)t; *h = 1u; return 0; }
static inline void nrfx_gppi_conn_enable(nrfx_gppi_handle_t h) { fake_hw.conn_on |= (1u << (h & 31u)); }
static inline void nrfx_gppi_conn_disable(nrfx_gppi_handle_t h) { fake_hw.conn_on &= ~(1u << (h & 31u)); }
static inline int nrfx_gppi_ep_attach(uint32_t ep, nrfx_gppi_handle_t h)
{ (void)h; fake_hw.ep_attached = ep; return 0; }

#endif
