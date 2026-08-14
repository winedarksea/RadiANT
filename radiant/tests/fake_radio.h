/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fake_radio.h - the test-side control surface of the in-memory radio backend.
 *
 * Provenance: original clean-room work. Written against
 * radiant/include/radiant/radiant_radio_hal.h, docs/backends.md, docs/ant-radio-link.md
 * and the measurements in docs/spike-a-results.md, plus public
 * nRF52840/nRF54L15 datasheet figures and public Silicon Labs RAIL
 * documentation for the capability presets. Nothing here derives from
 * sdk-ant, from libant.a, or from any ANT+ device profile
 * document.
 *
 * ---------------------------------------------------------------------------
 * What this is
 * ---------------------------------------------------------------------------
 * radiant/tests/fake_radio.c defines every function in radiant_radio_hal.h; the test
 * binary links it instead of a backend. Six of the seven core modules (frame,
 * sched, channel, search, events, transfer) never touch hardware - they are
 * developed against this file and verified in CI on Linux, because native_sim
 * does not build on Windows.
 *
 * That makes the mock load-bearing: anything it permits that real silicon
 * forbids becomes code that deadlocks on the bench, and anything it reports
 * that silicon does not becomes tests that prove nothing. So it is
 * deliberately *stricter* than a stub:
 *
 *   - it validates per-operation configuration on every arm call, so a module
 *     that forgets a field gets RADIANT_RADIO_EINVAL rather than inheriting
 *     the previous operation's state;
 *   - it enforces the callback contract's MUST NOTs it can observe, and
 *     records what it cannot enforce as a violation a test can assert on;
 *   - it models the *air*, not the API: a frame is a byte stream that exists
 *     at an instant, heard only if a window with a matching filter was open.
 *     A window that hears nothing is the default outcome, not a special case;
 *   - the clock starts one second below 2^32 microseconds, so a core module
 *     storing a timestamp in a uint32_t fails in its first virtual second.
 *     RAIL_Time_t is 32-bit and extending it is the backend's duty.
 *
 * ---------------------------------------------------------------------------
 * How a suite uses it
 * ---------------------------------------------------------------------------
 *   #include "../fake_radio.h"          // suites live in tests/src/
 *
 *   static void rx_cb(const struct radiant_rx_event *e, void *user) { ... }
 *   static const struct radiant_radio_cbs cbs = { .rx = rx_cb, .tx = NULL };
 *
 *   static void setup(void *f) { ARG_UNUSED(f); fake_radio_reset(); }
 *
 *   ZTEST(my_suite, test_window_hears_the_master)
 *   {
 *           uint8_t frame[FAKE_RADIO_AIR_FRAME_MAX];
 *           uint8_t n = fake_radio_build_ant_frame(frame, 14871u, 0x0Bu, 5u,
 *                                                  payload);
 *
 *           zassert_ok(radiant_radio_init(&cbs, NULL));
 *           zassert_ok(radiant_radio_enable());
 *
 *           radiant_time_t t = radiant_radio_now() + 100000u;
 *           fake_radio_air_frame(t, frame, n);
 *
 *           struct radiant_rx_req req = { ... .t_open = t - 500, .t_close = t + 500 };
 *           uint32_t op;
 *           zassert_ok(radiant_radio_rx(&req, &op));
 *           fake_radio_advance_to(req.t_close + 1u);
 *
 *           zassert_true(fake_radio_is_idle(), "%s", fake_radio_busy_reason());
 *           zassert_equal(0u, fake_radio_viol_count(), "%s",
 *                         fake_radio_viol_name(fake_radio_viol(0)->code));
 *   }
 *
 * Two habits catch otherwise-invisible bugs in every suite: finish with
 * fake_radio_is_idle() (catches a module that left an operation armed) and
 * fake_radio_viol_count() == 0 (catches a callback that would deadlock a real
 * radio ISR).
 */

#ifndef RADIANT_TESTS_FAKE_RADIO_H_
#define RADIANT_TESTS_FAKE_RADIO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <radiant/radiant_radio_hal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Fixed sizes
 *
 * All state is static: the mock allocates nothing. Exceeding any of these
 * limits is recorded as a violation rather than silently dropped.
 * ---------------------------------------------------------------------------
 */

/* Filters the mock can model in one window. Matches the nRF ceiling (one base
 * address plus eight prefixes); caps.max_filters may be set lower but not
 * higher. */
#define FAKE_RADIO_MAX_FILTERS     8

/* An on-air frame as the mock stores it: address bytes then body bytes, with
 * no preamble and no CRC. Sized for the HAL's own two ceilings. */
#define FAKE_RADIO_AIR_FRAME_MAX   (RADIANT_RADIO_ADDR_MAX + RADIANT_RADIO_BODY_MAX)

#define FAKE_RADIO_AIR_MAX         256  /* frames queued on the air at once */
#define FAKE_RADIO_ARM_LOG_MAX     64   /* arm calls recorded, accepted or not */
#define FAKE_RADIO_EVENT_LOG_MAX   128  /* events recorded */
#define FAKE_RADIO_VIOL_MAX        16   /* contract violations recorded */

/* Events fired inside one fake_radio_advance*() call before a livelock is
 * declared - catches a core callback that re-arms a zero-length window
 * forever, instead of hanging CI. */
#define FAKE_RADIO_ADVANCE_BUDGET  4096

/* Clock origin: one second below 2^32 microseconds, so every test crosses
 * the 32-bit boundary within its first virtual second. A test that needs
 * round numbers calls fake_radio_set_origin(). */
#define FAKE_RADIO_T_ORIGIN        ((radiant_time_t)0xFFF0BDC0ULL)

/* Written over the event body buffer the moment a callback returns, so a
 * callback that retained event->body (forbidden - it's a DMA buffer on real
 * hardware) reads this instead of the frame it thought it kept. */
#define FAKE_RADIO_BODY_POISON     0xDDu

/*
 * Spike A defaults (docs/spike-a-results.md): the bench link ran at -17 dBm
 * with ~240/60s frames and essentially zero loss, while a 3-byte search
 * address triggered the matcher on noise several times a second at
 * -54..-101 dBm - why radiant_search.c must gate on CRC, not match count.
 */
#define FAKE_RADIO_RSSI_BENCH_DBM  (-17)
#define FAKE_RADIO_RSSI_NOISE_DBM  (-85)

/* 8182 / 32768 s, the 4.005 Hz ANT+ default period Spike A measured, in whole
 * microseconds. */
#define FAKE_RADIO_ANT_PERIOD_US   249725u

/* ---------------------------------------------------------------------------
 * A frame on the air
 * ---------------------------------------------------------------------------
 *
 * bytes[] is the on-air byte stream from the first address byte to the last
 * body byte: no preamble, no CRC. It is deliberately not pre-split into
 * "address" and "body" - that split is a property of the receiver's
 * configuration, not of the frame:
 *
 *   Spike A, tracking:  addr_len 5 -> address [A6 C5 dl dh dt], body 10 bytes
 *                       [tt][0x0A][d0..d7]
 *   Spike A, search:    addr_len 3 -> address [A6 C5 dl],       body 12 bytes
 *                       [dh][dt][tt][0x0A][d0..d7]
 *
 * One queued frame behaves correctly for both window shapes, exercising
 * radiant_search.c's recovery of devnum_hi and device type from the body.
 */
enum fake_radio_match {
	/* Match bytes[0 .. filter.addr_len) against each filter in turn,
	 * report the first match's index. How radiant_search.c recovers
	 * devnum_lo from filter_index. */
	FAKE_RADIO_MATCH_ADDR = 0,
	/* Deliver to filter_index whatever the window's filters are, for
	 * tests that care about the index and not about building addresses. */
	FAKE_RADIO_MATCH_INDEX = 1,
	/* A noise trigger: the hardware matcher fired on something that was
	 * not a frame. Delivered as CRC_FAIL with filter_index taken modulo
	 * the filter count, skipping every length check. */
	FAKE_RADIO_MATCH_NOISE = 2
};

struct fake_radio_air {
	/* Absolute time, on the HAL timebase, at which the last bit of the
	 * on-air address is at the antenna - the t_sync contract in
	 * radiant_radio_hal.h, from the transmitter's side. */
	radiant_time_t t_sync;

	uint8_t bytes[FAKE_RADIO_AIR_FRAME_MAX];
	uint8_t len;

	/* Sense inverted on purpose: a zero-initialised struct is a *good*
	 * frame, so a test that forgets to set anything gets the common case. */
	bool crc_bad;

	/* rssi_dbm is only read when rssi_set is true; otherwise the mock uses
	 * the default (FAKE_RADIO_RSSI_BENCH_DBM or whatever
	 * fake_radio_set_default_rssi() last set). */
	bool   rssi_set;
	int8_t rssi_dbm;

	/* The CRC as it "arrived", for driving single-bit repair. Only reaches
	 * the event when delivered as CRC_FAIL and caps.has_rx_crc is true. A
	 * test wanting a repairable frame sets crc_bad, flips one bit in
	 * `bytes`, and puts the ORIGINAL frame's CRC here. Off by default, so
	 * an existing CRC-failure test still carries no CRC. */
	bool     crc_rx_set;
	uint32_t crc_rx;

	enum fake_radio_match match;
	uint8_t               filter_index; /* MATCH_INDEX / MATCH_NOISE only */
};

/* ---------------------------------------------------------------------------
 * What the mock records
 * ---------------------------------------------------------------------------
 */

enum fake_radio_arm_kind {
	FAKE_RADIO_ARM_NONE = 0,
	FAKE_RADIO_ARM_TX,
	FAKE_RADIO_ARM_RX,
	FAKE_RADIO_ARM_ED
};

/* One arm call, accepted or rejected. Rejected calls are recorded too, with
 * op == 0 and rc set, so a test can assert a window was *never* requested
 * too late - on the scheduling decision itself, not just its outcome. */
struct fake_radio_arm {
	uint32_t                 op;      /* 0 if the call was rejected */
	enum fake_radio_arm_kind kind;
	int                      rc;      /* what the arm call returned */
	radiant_time_t               t_arm;   /* radiant_radio_now() at the call */
	bool                     from_callback; /* armed from inside a callback */

	struct radiant_pkt_format fmt;
	uint8_t               rf_index;
	struct radiant_tx_power   power;
	int8_t                power_dbm_eff; /* after clamping to the caps range */

	/* TX */
	radiant_time_t t_sync_at;
	/* The on-air address the transmit asked for. fake_radio rejects a
	 * transmit whose addr_len disagrees with fmt->addr_len, so the mock
	 * states the requirement rather than merely carrying it. */
	uint8_t    addr[RADIANT_RADIO_ADDR_MAX];
	uint8_t    addr_len;
	uint8_t    body[RADIANT_RADIO_BODY_MAX];
	uint8_t    body_len;

	/* RX */
	radiant_time_t           t_open;
	radiant_time_t           t_close;
	uint32_t             flags;
	/* Air the core asked to have held past this operation's end (struct
	 * radiant_rx_req::follow_on_us). The mock owns the radio, so it only
	 * records this rather than reserving it. */
	uint16_t             follow_on_us;
	struct radiant_rx_filter filters[FAKE_RADIO_MAX_FILTERS];
	uint8_t              n_filters;

	/* ED. rf_index above is the range's low end and t_open is t_start - a
	 * field meaning the same thing in two kinds is one field. */
	uint8_t  rf_index_hi;
	uint32_t ed_dwell_us;

	/* Outcome, filled in as the operation runs. */
	bool                  terminated;
	enum radiant_radio_status terminal_status;
	radiant_time_t            t_terminal;
	uint16_t              n_rx_events;  /* non-terminal OK / CRC_FAIL */
};

struct fake_radio_event {
	uint32_t                 op;
	enum fake_radio_arm_kind kind;
	enum radiant_radio_status    status;
	bool                     terminal;
	radiant_time_t               t;      /* radiant_radio_now() at delivery */
	radiant_time_t               t_sync;
	bool                     t_sync_exact;
	uint8_t                  filter_index;
	uint8_t                  body[RADIANT_RADIO_BODY_MAX];
	uint8_t                  body_len;
	bool                     has_rssi;
	int8_t                   rssi_dbm;
	bool                     has_crc_rx;
	uint32_t                 crc_rx;
	bool                     late;   /* injected for an operation that ended */

	/* ED, on RADIANT_RADIO_STATUS_OK only. rf_index is the index this dwell
	 * measured; the terminal event of a sweep carries none of these. */
	uint8_t  rf_index;
	int8_t   ed_min_dbm;
	int8_t   ed_mean_dbm;
	uint16_t ed_samples;
};

/* What the mock reports for an energy-detect dwell, per RF index, so a suite
 * can build a band with a loud patch and assert the map found it. Unset
 * indices report FAKE_RADIO_ED_MIN_DBM / _MEAN_DBM, five dB apart, so a
 * module that used one where it meant the other is caught. */
#define FAKE_RADIO_ED_MIN_DBM  (-95)
#define FAKE_RADIO_ED_MEAN_DBM (-90)
/* Samples the mock claims per dwell. Non-zero and not one, so that a consumer
 * which ignored the field is distinguishable from one that read it. */
#define FAKE_RADIO_ED_SAMPLES  16u

void fake_radio_set_ed(uint8_t rf_index, int8_t min_dbm, int8_t mean_dbm);
void fake_radio_clear_ed(void);
/* Override the reported sample count for every dwell. Zero is the "this
 * dwell measured nothing" case; the HAL forbids an OK event for it, so the
 * mock delivers no event for that index, which a consumer must survive. */
void fake_radio_set_ed_samples(uint16_t samples);

/* What a window that hears nothing reports as the band's level. Off until
 * set (every terminal event carries has_noise false, as an incapable backend
 * would). The mock decides eligibility itself - terminal TIMEOUT with
 * nothing received - rather than letting a test declare it, since that is a
 * condition a core module could get wrong unnoticed. */
void fake_radio_set_noise(int8_t dbm);
void fake_radio_clear_noise(void);

struct fake_radio_stats {
	uint32_t arms_tx;
	uint32_t arms_rx;
	uint32_t arms_ed;
	uint32_t arms_rejected;
	uint32_t arms_addr_len_over_hw; /* addr_len > caps.addr_len_hw_max, so a
					 * real backend finishes the match in
					 * software: more spurious wakeups and
					 * more receive current */
	uint32_t rc_einval;
	uint32_t rc_enotsup;
	uint32_t rc_ebusy;
	uint32_t rc_etime;
	uint32_t rc_estate;
	uint32_t rc_forced;

	uint32_t ev_ok;
	uint32_t ev_crc_fail;
	uint32_t ev_timeout;
	uint32_t ev_aborted;
	uint32_t ev_failed;
	uint32_t ev_tx;
	uint32_t ev_ed;    /* per-index ED measurements delivered (OK only) */
	uint32_t ev_late;

	uint32_t crc_fail_suppressed;  /* CRC_FAIL frames the window did not ask
					* for; the count is still here, because
					* it is what makes "unexplained loss"
					* a meaningful figure */
	uint32_t air_delivered;
	uint32_t air_missed;           /* on the air with no window listening */
	uint32_t air_dropped_oversize; /* longer than fmt->max_body_len */
	uint32_t air_dropped_len;      /* length did not agree with len_mode */
	uint32_t callbacks;
};

enum fake_radio_viol {
	FAKE_RADIO_VIOL_NONE = 0,
	FAKE_RADIO_VIOL_ISR_LIFECYCLE,   /* init/enable/disable from a callback */
	FAKE_RADIO_VIOL_ISR_HARNESS,     /* clock/queue/reset from a callback */
	FAKE_RADIO_VIOL_ISR_WORK,        /* callback exceeded the HAL-call budget */
	FAKE_RADIO_VIOL_CB_NESTED,       /* a callback was about to nest */
	FAKE_RADIO_VIOL_DOUBLE_TERMINAL,
	FAKE_RADIO_VIOL_EVENT_AFTER_END,
	FAKE_RADIO_VIOL_CLOCK_BACKWARDS,
	FAKE_RADIO_VIOL_ADVANCE_BUDGET,
	FAKE_RADIO_VIOL_CAPS_UNMODELLABLE,
	FAKE_RADIO_VIOL_QUEUE_FULL,
	FAKE_RADIO_VIOL_LOG_FULL,
	FAKE_RADIO_VIOL_COUNT
};

struct fake_radio_viol_rec {
	enum fake_radio_viol code;
	const char          *detail;
	radiant_time_t           when;
	uint32_t             op;
};

/* ---------------------------------------------------------------------------
 * Harness lifecycle
 * ---------------------------------------------------------------------------
 */

/* Full reset: clock back to FAKE_RADIO_T_ORIGIN, caps back to the nRF preset,
 * air queue and every log emptied, no operation armed, HAL lifecycle back to
 * pre-init. Call from a ztest setup/before hook. The one place the clock may
 * go backwards - a reset is a new program run for monotonicity purposes. */
void fake_radio_reset(void);

/* reset() + radiant_radio_init() with both callbacks NULL + radiant_radio_enable().
 * For suites that assert on the event log rather than on callbacks. */
void fake_radio_bring_up(void);

/* True while a callback is executing. Core code may call this in an assertion;
 * the mock calls it to enforce the MUST NOTs. */
bool fake_radio_in_callback(void);

/* HAL calls one callback may make before the mock records
 * FAKE_RADIO_VIOL_ISR_WORK. A heuristic for "do work proportional to
 * anything - queue it and return": a callback that walks 32 channels calling
 * radiant_radio_now() trips it. Default 16; 0 disables the check. */
void fake_radio_set_isr_call_budget(uint16_t calls);

/* ---------------------------------------------------------------------------
 * The virtual clock
 *
 * There is no wall clock anywhere in the mock. Time moves only when a test
 * moves it, and every event fires inside one of these calls, in time order,
 * with radiant_radio_now() reading the event's own instant while its callback
 * runs. A test that sleeps is a test that flakes.
 * ---------------------------------------------------------------------------
 */

/* Only legal with the radio idle and the air queue empty - it is for choosing
 * a readable origin at the top of a test, not for time travel. */
void fake_radio_set_origin(radiant_time_t t);

void fake_radio_advance(uint64_t us);
void fake_radio_advance_to(radiant_time_t t);

/* The instant of the next event the mock would fire, or RADIANT_TIME_NEVER if
 * nothing is pending. */
radiant_time_t fake_radio_next_due(void);

/* Advance to the next due event and fire exactly that one. Returns false if
 * nothing was pending. For tests that need to act between two events. */
bool fake_radio_step(void);

/* ---------------------------------------------------------------------------
 * Capabilities
 *
 * The caps query is the whole portability mechanism:
 *
 *     fake_radio_caps_mut()->max_filters = 2;      // or the RAIL preset
 *
 * radiant_sched.c must never hardcode 8, radiant_search.c must never hardcode
 * a 32-set sweep. Mutate caps between operations only - the returned pointer
 * is static for the program's lifetime, and real backends read their own
 * fields while an operation is in flight.
 * ---------------------------------------------------------------------------
 */
struct radiant_radio_caps *fake_radio_caps_mut(void);

/* nRF52840 / nRF54L15: max_filters 8 (one base plus eight prefixes),
 * addr_len_hw_max 5, crc_in_hw true. */
void fake_radio_caps_preset_nrf(void);

/* EFR32 / RAIL: max_filters 2 (two runtime sync words), addr_len_hw_max 4,
 * crc_in_hw false - RAIL's CRC engine cannot be made to cover a sync word that
 * varies per channel, so the backend verifies in software. */
void fake_radio_caps_preset_rail(void);

/* phys must have static storage duration: caps holds the pointer. */
void fake_radio_set_phys(const enum radiant_phy *phys, uint8_t n_phys);

/* A ready-made two-PHY list: 1 M first (required of any ANT+-compatible
 * backend), then the coded PHY. Both presets still default to 1 M alone;
 * combine with fake_radio_caps_preset_rail() to get two PHYs AND a non-zero
 * phy_switch_us - the nRF preset switches for free, so it can't tell a
 * scheduler that budgets PHY-switch time from one that doesn't. */
extern const enum radiant_phy fake_phys_1m_lr[];
extern const uint8_t          fake_phys_1m_lr_count;

/* ---------------------------------------------------------------------------
 * Putting frames on the air
 * ---------------------------------------------------------------------------
 */

/* Defaults: crc good, address matching, the bench RSSI. Always use this rather
 * than a bare struct if you are going to set only one or two fields. */
void fake_radio_air_init(struct fake_radio_air *f);

/* Returns RADIANT_RADIO_OK_RC, or RADIANT_RADIO_EINVAL / a full queue. */
int fake_radio_air_add(const struct fake_radio_air *f);

/* One good frame, address-matched, at the bench RSSI. */
int fake_radio_air_frame(radiant_time_t t_sync, const uint8_t *bytes, uint8_t len);

/* Same frame, count times, period_us apart, starting at t_first. Models the
 * measured link: 240 frames in 60 s at FAKE_RADIO_ANT_PERIOD_US with no loss.
 * A dropped frame is modelled by not queueing it. Returns how many were
 * queued. */
uint32_t fake_radio_air_master(radiant_time_t t_first, uint32_t period_us,
			       uint32_t count, const uint8_t *bytes,
			       uint8_t len);

/* count spurious matcher triggers spread evenly over [t0, t1], each carrying
 * deterministic pseudo-random bytes and a bad CRC (Spike A's several noise
 * triggers a second on a 3-byte search address). With RADIANT_RX_REPORT_CRC_FAIL
 * clear these are invisible to the core and only appear in
 * stats.crc_fail_suppressed. Returns how many were queued. */
uint32_t fake_radio_air_noise(radiant_time_t t0, radiant_time_t t1, uint32_t count,
			      int8_t rssi_dbm);

void     fake_radio_air_clear(void);
uint32_t fake_radio_air_pending(void); /* queued, not yet heard or missed */
void     fake_radio_set_default_rssi(int8_t dbm);

/*
 * Build the ANT+ on-air byte stream, from docs/ant-radio-link.md and confirmed
 * byte for byte by docs/spike-a-results.md:
 *
 *     A6 C5 | dl dh | dtype | ttype | 0x0A | d0..d7        (15 bytes)
 *
 * out must have room for FAKE_RADIO_AIR_FRAME_MAX bytes; payload must be 8.
 * Returns the length written. No preamble and no CRC: neither is on this side
 * of the HAL.
 */
uint8_t fake_radio_build_ant_frame(uint8_t *out, uint16_t devnum,
				   uint8_t dev_type, uint8_t trans_type,
				   const uint8_t *payload8);

/* ---------------------------------------------------------------------------
 * Provoking the failure modes
 * ---------------------------------------------------------------------------
 */

/* Make the next arm call return rc without arming anything (still recorded).
 * Chiefly for RADIANT_RADIO_EBUSY: a thread-context arm can race the callback
 * that may be about to consume the operation slot, and a test cannot
 * otherwise make that race happen on demand. RADIANT_RADIO_OK_RC clears a
 * pending force. */
void fake_radio_force_next_arm(int rc);

/* Replace the terminal status of the next accepted operation - the way to get
 * RADIANT_RADIO_STATUS_FAILED, "the backend could not complete it", which nothing
 * else in the mock produces. */
void fake_radio_force_next_terminal(enum radiant_radio_status status);

/*
 * The same two, for a RUN of operations - the shape an arbitrated backend
 * fails in (it refuses every window that lands inside another stack's
 * activity, not just one), which a one-shot can't express and re-arming
 * between each would mean re-arming from inside the callback under test.
 *
 * force_arm_repeat refuses the next `count` ARM CALLS with rc (synchronous
 * RADIANT_RADIO_EDENIED). force_terminal_repeat replaces the terminal status
 * of the next `count` ACCEPTED OPERATIONS (accepted-then-never-granted
 * RADIANT_RADIO_STATUS_DENIED). They compose - genuinely different moments.
 *
 * RADIANT_RADIO_OK_RC, or a count of 0, cancels a run in progress.
 */
void fake_radio_force_arm_repeat(int rc, uint32_t count);
void fake_radio_force_terminal_repeat(enum radiant_radio_status status,
				      uint32_t count);

/* Offset, in microseconds, between the t_sync a transmit asks for and the
 * t_sync it reports. Models a backend that cannot schedule exactly; the
 * channel state machine's drift handling should survive it. Default 0. */
void fake_radio_set_tx_offset_us(int32_t us);

/* Deliver one event carrying the id of an operation that has already ended -
 * the race the op id exists for: on real hardware the frame was already in
 * the receiver's pipeline when abort() ran, so the event arrives after the
 * core moved on. The mock never does this by itself. Marked late in the log;
 * does not count as a terminal event. */
int fake_radio_inject_late_rx(uint32_t op, const struct fake_radio_air *f,
			      uint8_t filter_index);
int fake_radio_inject_late_tx(uint32_t op, enum radiant_radio_status status);

/* ---------------------------------------------------------------------------
 * What a test asserts on
 * ---------------------------------------------------------------------------
 */

/* No operation armed or running, and nothing deferred. The end-of-test check
 * that catches a module which left the radio armed. */
bool        fake_radio_is_idle(void);
/* NULL when idle, otherwise a short static description, for the message
 * argument of the assertion. */
const char *fake_radio_busy_reason(void);

uint32_t                     fake_radio_arm_count(void); /* including rejects */
const struct fake_radio_arm *fake_radio_arm(uint32_t index);
const struct fake_radio_arm *fake_radio_arm_for_op(uint32_t op);
const struct fake_radio_arm *fake_radio_last_arm(void);

uint32_t                       fake_radio_event_count(void);
const struct fake_radio_event *fake_radio_event(uint32_t index);
const struct fake_radio_event *fake_radio_last_event(void);

const struct fake_radio_stats *fake_radio_stats(void);

uint32_t                          fake_radio_viol_count(void);
/* Never NULL: index past the end returns a record with code
 * FAKE_RADIO_VIOL_NONE, so an assertion message can be formatted
 * unconditionally. */
const struct fake_radio_viol_rec *fake_radio_viol(uint32_t index);
const char                       *fake_radio_viol_name(enum fake_radio_viol v);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_TESTS_FAKE_RADIO_H_ */
