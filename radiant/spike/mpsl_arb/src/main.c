/* SPDX-License-Identifier: Apache-2.0 */
/*
 * P0 - what MPSL's timeslot arbiter actually does, measured rather than assumed.
 *
 * Clean-room: written against nrfxlib's mpsl_timeslot.h and the in-tree
 * nrf/samples/mpsl/timeslot and nrf/drivers/mpsl/flash_sync patterns. Nothing
 * derives from sdk-ant or libant.a.
 *
 * Why this exists and is throwaway: the arbitrated backend (P3) is a set of
 * margins, a deadline timer and a status set, each sized from a number. Only
 * MPSL_TIMESLOT_START_JITTER_US is published (0 on this part, GRTC-scheduled).
 * Unpublished elsewhere:
 *   - GRANT LATENCY for an EARLIEST request - mpsl_timeslot.h only says
 *     SIGNAL_START has a latency relative to timeslot start, no figure.
 *   - HOW OFTEN a NORMAL chain is BLOCKED at 32 req/s (eight tracked ANT+
 *     sensors) and ~90/s (those plus a sweep).
 *   - HOW LATE BLOCKED arrives relative to the requested start - sizes the
 *     front end's deadline timer, which is required rather than defensive:
 *     BLOCKED runs from mpsl_low_priority_process(), so between arm and
 *     signal the core's single operation slot holds a dead operation and
 *     every other channel's window expires behind it as MISSED.
 *   - WHETHER AN EXTENSION CHAIN GROWS - ADR 0013's "sweep is the elastic
 *     consumer" is repeated SIGNAL_ACTION_EXTEND, on the argument that MPSL
 *     only grants an extension with nothing else scheduled in that region.
 *     If extensions don't grow, that design collapses to a fixed slice.
 *   - WHETHER A FREE-RUNNING TIMER SURVIVES TIMESLOT EDGES - radiant's
 *     absolute timebase is a 1 MHz TIMER the HAL promises never goes
 *     backwards; if MPSL disturbed a non-MPSL_TIMER0 instance, the backend
 *     would need a different clock and P3 would be a much larger change.
 *
 * Touches no HAL, includes no radiant. Anything proved here must be
 * re-proved by P3's own gates on the shipping path; exists only to derive
 * P3's constants, expected to be deleted once they are.
 *
 * Deliberately not here: THE RADIO, not one register. mpsl.rst makes the
 * application responsible for the RADIO interrupt if the timeslot API drives
 * radio access; a spike that also drove the radio would measure two things
 * and attribute the result to one. Every timeslot here is granted, timed,
 * and given back having done nothing - every number is a floor.
 *
 * Two ways this program can assert, both deliberate:
 *   1. OVERSTAYED - closing a timeslot late is a crash, not a lost window.
 *      Every timeslot ends from its own SIGNAL_START or an EXTEND_FAILED
 *      with no work in between, so this should be unreachable; if reached,
 *      the assert handler prints it and P3's tail margin needs to be bigger
 *      than this spike would have suggested.
 *   2. AN ACTION RETURNED FROM A LOW-PRIORITY SIGNAL - BLOCKED, CANCELLED
 *      and SESSION_IDLE run in mpsl_low_priority_process(); returning
 *      anything but ACTION_NONE from one asserts inside MPSL. That's why
 *      recovery here is a semaphore and a thread, not a callback request -
 *      the same constraint P3's recovery is under.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

#include <hal/nrf_timer.h>

#include <mpsl_timeslot.h>
#include <mpsl_hwres.h>
#include <nrf_errno.h>

#if defined(CONFIG_BT)
#include <zephyr/bluetooth/bluetooth.h>

/* File scope, not style: BT_DATA_BYTES takes the address of a compound
 * literal, which has automatic storage duration inside a block - so a
 * function-local `static const struct bt_data ad[]` is a compile error
 * ("initializer element is not constant"), not a silent lifetime bug. */
static const struct bt_data adv_data[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
};
#endif

BUILD_ASSERT(CONFIG_MPSL_TIMESLOT_SESSION_COUNT >= 1,
	     "CONFIG_MPSL_TIMESLOT_SESSION_COUNT defaults to 0, which makes "
	     "every session_open() fail while the program still runs and "
	     "prints zeroes");

/* The free-running absolute clock: a 1 MHz TIMER that is NOT MPSL_TIMER0, on
 * the instance the board overlay reserves, programmed like radiant's
 * nrf backend does its own - prescaler derived from base frequency rather
 * than named, since nRF54L TIMERs don't run from 16 MHz and
 * nrf_timer_frequency_set() doesn't exist for them. Read from both the
 * timeslot callback and thread context safely: a capture task plus a CC
 * read, no read-modify-write. */

#define FREE_TIMER_NODE DT_CHOSEN(spike_free_timer)
BUILD_ASSERT(DT_NODE_HAS_STATUS(FREE_TIMER_NODE, okay),
	     "the chosen spike,free-timer is disabled, and an unclocked "
	     "peripheral answers a register write with a bus fault");

#define FREE_TIMER ((NRF_TIMER_Type *)DT_REG_ADDR(FREE_TIMER_NODE))
#define FREE_TIMER_CC 0u

/*
 * Why this function exists: the free-running TIMER's PCLK comes from HFCLK,
 * which is the internal RC until something requests the crystal. MPSL cycles
 * the crystal on/off at XTAL_GUARANTEED timeslot edges, so without holding a
 * request the TIMER runs off the RC between grants and the crystal during
 * them. Measured, not a rounding error: without this the free timer read
 * 0.42% fast over 10 s (RC accuracy), and the same 0.4% showed up in the
 * anchor error.
 *
 * A requirement on P3, not just this spike: radiant's nrf backend
 * already holds the HFXO at init and the arbitrated gate must keep doing so
 * (Zephyr's clock control refcounts, so the request survives MPSL's own
 * cycling). Drop it and the 1 MHz absolute timebase gains a 0.4% rate error
 * between grants - about a millisecond of slot-placement error per 250 ms
 * ANT+ period, silently, with every counter reading zero.
 */
static void hfxo_hold(void)
{
	const struct device *clk = DEVICE_DT_GET_ONE(nordic_nrf_clock);

	if (!device_is_ready(clk)) {
		printk("*** clock controller not ready - the TIMER is on the "
		       "RC and every number below is 0.4 %% wrong\n");
		return;
	}
	if (clock_control_on(clk, CLOCK_CONTROL_NRF_SUBSYS_HF) < 0) {
		printk("*** HFXO did not start - see above\n");
	}
}

static void free_timer_start(void)
{
	uint32_t base_hz = NRF_TIMER_BASE_FREQUENCY_GET(FREE_TIMER);
	uint32_t presc = 0;

	nrf_timer_mode_set(FREE_TIMER, NRF_TIMER_MODE_TIMER);
	nrf_timer_bit_width_set(FREE_TIMER, NRF_TIMER_BIT_WIDTH_32);
	while ((base_hz >> presc) > 1000000u) {
		presc++;
	}
	__ASSERT((base_hz >> presc) == 1000000u,
		 "TIMER base %u Hz cannot be divided to exactly 1 MHz", base_hz);
	nrf_timer_prescaler_set(FREE_TIMER, presc);
	nrf_timer_task_trigger(FREE_TIMER, NRF_TIMER_TASK_CLEAR);
	nrf_timer_task_trigger(FREE_TIMER, NRF_TIMER_TASK_START);
}

/* 32 bits at 1 MHz wraps every ~71.6 minutes. Not extended, deliberately: a
 * measurement run here is seconds long, and a wrap-extension counter would be
 * one more thing that could be wrong in a program whose whole job is to be
 * trusted about time. Runs that straddle a wrap show up as a negative delta and
 * are discarded by the caller. */
static inline uint32_t now_us(void)
{
	nrf_timer_task_trigger(FREE_TIMER,
			       nrf_timer_capture_task_get(FREE_TIMER_CC));
	return nrf_timer_cc_get(FREE_TIMER, FREE_TIMER_CC);
}

/* Min, max, mean and a coarse log-ish histogram. Not a percentile (would
 * need samples kept) - what matters here is the TAIL, since grant latency's
 * distribution has an unbounded one that min_arm_lead_us cannot absorb. */

#define NBUCKET 10u

/* Upper edges in microseconds. The first four are around the numbers that
 * matter: ARM_SETUP+RAMP_UP+AIR_LEAD is 168 us on nRF54L, the HFXO startup is
 * 1650 us, and a tracked ANT+ slot is 640 us. */
static const uint32_t bucket_us[NBUCKET] = {
	50u, 100u, 200u, 500u, 1000u, 2000u, 5000u, 20000u, 100000u, 0xFFFFFFFFu
};

struct stat {
	uint32_t n;
	uint32_t min;
	uint32_t max;
	uint64_t sum;
	uint32_t hist[NBUCKET];
};

static void stat_reset(struct stat *s)
{
	memset(s, 0, sizeof(*s));
	s->min = 0xFFFFFFFFu;
}

static void stat_add(struct stat *s, uint32_t v)
{
	uint32_t i;

	s->n++;
	s->sum += v;
	if (v < s->min) {
		s->min = v;
	}
	if (v > s->max) {
		s->max = v;
	}
	for (i = 0u; i < NBUCKET; i++) {
		if (v <= bucket_us[i]) {
			s->hist[i]++;
			return;
		}
	}
}

static void stat_print(const char *name, const struct stat *s)
{
	uint32_t i;

	if (s->n == 0u) {
		printk("%s: no samples\n", name);
		return;
	}
	printk("%s: n=%u min=%u mean=%u max=%u us\n", name, s->n, s->min,
	       (uint32_t)(s->sum / s->n), s->max);
	printk("%s: hist", name);
	for (i = 0u; i < NBUCKET; i++) {
		if (s->hist[i] != 0u) {
			printk(" <=%u:%u", bucket_us[i], s->hist[i]);
		}
	}
	printk("\n");
}

/* ---------------------------------------------------------------------------
 * The session
 * ---------------------------------------------------------------------------
 */

/* The return parameter must outlive the callback (mpsl_timeslot.h: "must not
 * point to a stack variable"). Getting it wrong builds a request from
 * whatever the stack was reused for - a schedule that walks, not a crash.
 * One static per session; the next request lives beside it for the same
 * reason. */
static mpsl_timeslot_signal_return_param_t rv;
static mpsl_timeslot_request_t next_req;

static mpsl_timeslot_session_id_t session;
static bool session_open;

/* What the current run is doing. Read in the callback, written by the thread
 * only while no request is outstanding. */
enum mode {
	MODE_IDLE = 0,
	MODE_EARLIEST,   /* one EARLIEST at a time, thread re-requests */
	MODE_CHAIN,      /* NORMAL chained from inside SIGNAL_START */
	MODE_EXTEND,     /* one grant, grown by repeated ACTION_EXTEND */
};

static volatile enum mode mode;
static volatile uint8_t   prio = MPSL_TIMESLOT_PRIORITY_NORMAL;
static volatile uint32_t  chain_distance_us = 31250u; /* 32/s */
static volatile uint32_t  slot_len_us = 1000u;

/* The anchor rule, which the plan calls the most error-prone code in the
 * backend: distance_us is measured from the PREVIOUS TIMESLOT'S START, not from
 * an absolute time, and when a request is BLOCKED no timeslot starts - so the
 * anchor does not move and the next distance must be recomputed against the
 * older anchor. This is the value that verifies it. */
static volatile uint32_t anchor_us;
static volatile uint32_t predicted_start_us;
static volatile uint32_t blocks_since_grant;

/*
 * Set by BLOCKED/CANCELLED, cleared by the thread that re-requests. Without
 * it the chain stalls at the first block, and the failure looked plausible
 * rather than broken: SESSION_IDLE fires for reasons other than a broken
 * chain, so the recovery thread woke while healthy, issued a NORMAL request
 * into a non-IDLE session, got -NRF_EAGAIN, and the run ended at its first
 * wake-up. Measured "block rate" was 99/10000 at 32/s and 3333/10000 at
 * 90/s against a 1 s BLE advertiser - a devastating-looking contention
 * result that was really 100 grants in 20 s instead of 640: a stopped
 * chain, not an MPSL refusal.
 */
static volatile bool chain_broken;

static struct stat st_latency;   /* EARLIEST: request -> SIGNAL_START */
static struct stat st_blocked;   /* BLOCKED arriving AFTER the requested start */
static struct stat st_blocked_early; /* ... and before it */
static struct stat st_anchor;    /* |actual start - predicted start| on a chain */
static struct stat st_extend;    /* total achieved length of an extension chain */

static volatile uint32_t n_start, n_blocked, n_cancelled, n_idle;
static volatile uint32_t n_blocked_early;
static volatile uint32_t n_ext_ok, n_ext_fail, n_invalid, n_overstay;
static volatile uint32_t ext_total_us;

static K_SEM_DEFINE(idle_sem, 0, 1);

/* Enough to notice a disturbance, small enough that a run never approaches the
 * 100 ms MPSL_TIMESLOT_LENGTH_MAX_US. */
#define EXT_STEP_US 1000u
#define EXT_CAP_US  90000u

/* How far clear of `now` a recovery request must be placed - above the
 * measured EARLIEST grant latency (1692-1695 us, the HFXO startup), or a
 * request placed nearer than the arbiter can honour gets refused for a
 * reason unrelated to contention and miscounted as contention. */
#define RECOVER_MARGIN_US 3000u

static void request_normal(uint32_t distance_us)
{
	next_req.request_type = MPSL_TIMESLOT_REQ_TYPE_NORMAL;
	next_req.params.normal.hfclk = MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED;
	next_req.params.normal.priority = prio;
	next_req.params.normal.distance_us = distance_us;
	next_req.params.normal.length_us = slot_len_us;
	predicted_start_us = anchor_us + distance_us;
}

static mpsl_timeslot_signal_return_param_t *on_signal(
	mpsl_timeslot_session_id_t id, uint32_t signal)
{
	uint32_t t = now_us();

	(void)id;

	/* The default for every path, including every low-priority signal.
	 * Returning anything else from one of those asserts inside MPSL, so the
	 * safe value is assigned first and overwritten only where it is legal. */
	rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_NONE;

	switch (signal) {
	case MPSL_TIMESLOT_SIGNAL_START:
		n_start++;
		switch (mode) {
		case MODE_EARLIEST:
			stat_add(&st_latency, t - anchor_us);
			anchor_us = t;
			/* End immediately. The thread issues the next EARLIEST
			 * from SESSION_IDLE, because EARLIEST may not be
			 * requested from inside a timeslot. */
			rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
			break;
		case MODE_CHAIN:
			/*
			 * The anchor check: predicted_start_us is the anchor
			 * in force at request time plus the distance asked
			 * for. If BLOCKED truly leaves the anchor unmoved,
			 * this difference stays at the jitter (0 us here)
			 * regardless of blocks in between; otherwise it walks
			 * by one distance per block, and P3's schedule walks.
			 */
			if (predicted_start_us != 0u) {
				uint32_t d = (t > predicted_start_us)
						     ? (t - predicted_start_us)
						     : (predicted_start_us - t);

				stat_add(&st_anchor, d);
			}
			anchor_us = t;
			blocks_since_grant = 0u;
			request_normal(chain_distance_us);
			rv.callback_action =
				MPSL_TIMESLOT_SIGNAL_ACTION_REQUEST;
			rv.params.request.p_next = &next_req;
			break;
		case MODE_EXTEND:
			anchor_us = t;
			ext_total_us = slot_len_us;
			rv.callback_action =
				MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND;
			rv.params.extend.length_us = EXT_STEP_US;
			break;
		default:
			rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
			break;
		}
		break;

	case MPSL_TIMESLOT_SIGNAL_EXTEND_SUCCEEDED:
		n_ext_ok++;
		ext_total_us += EXT_STEP_US;
		if (ext_total_us + EXT_STEP_US <= EXT_CAP_US) {
			rv.callback_action =
				MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND;
			rv.params.extend.length_us = EXT_STEP_US;
		} else {
			/* Capped by us rather than by the arbiter, and counted
			 * as such: a chain that always ends here says the
			 * elastic model works and the cap is what bounded it,
			 * which is a different result from one that always ends
			 * in EXTEND_FAILED. */
			stat_add(&st_extend, ext_total_us);
			rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
		}
		break;

	case MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED:
		/* The yield point, discovered rather than configured. Arrives
		 * with the timeslot still live, letting P3 close the window
		 * cleanly and report a partial instead of being cut off
		 * mid-operation - ending here is correct, not a fallback. */
		n_ext_fail++;
		stat_add(&st_extend, ext_total_us);
		rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
		break;

	case MPSL_TIMESLOT_SIGNAL_BLOCKED:
		/* Low-priority context (mpsl_low_priority_process(), a
		 * cooperative thread) - no action may be returned, so recovery
		 * is handed to the thread below. This is why P3 can't rely on
		 * this signal alone and needs its own deadline timer: by the
		 * time this runs, the window it refers to is already past. */
		n_blocked++;
		blocks_since_grant++;
		chain_broken = true;
		/* The number that sizes P3's deadline timer - both signs
		 * matter. LATE (st_blocked) is dangerous: between arm and
		 * this signal the core's single operation slot holds a dead
		 * operation, and every other channel's window expires behind
		 * it as MISSED. EARLY means the denial can become a terminal
		 * event in time for the core to reuse the slot - a front end
		 * wouldn't need a deadline timer at all if this were always
		 * true. The ratio answers "required or merely defensive". */
		if (predicted_start_us != 0u) {
			if (t > predicted_start_us) {
				stat_add(&st_blocked, t - predicted_start_us);
			} else {
				n_blocked_early++;
				stat_add(&st_blocked_early,
					 predicted_start_us - t);
			}
		}
		break;

	case MPSL_TIMESLOT_SIGNAL_CANCELLED:
		n_cancelled++;
		chain_broken = true;
		break;

	case MPSL_TIMESLOT_SIGNAL_SESSION_IDLE:
		n_idle++;
		k_sem_give(&idle_sem);
		break;

	case MPSL_TIMESLOT_SIGNAL_INVALID_RETURN:
		n_invalid++;
		break;

	case MPSL_TIMESLOT_SIGNAL_OVERSTAYED:
		/* Unreachable by construction here - no timeslot does any work
		 * - and MPSL asserts after processing this signal anyway, so
		 * the counter exists to be printed by the assert handler rather
		 * than by the command loop. */
		n_overstay++;
		break;

	default:
		break;
	}

	return &rv;
}

static int open_session(void)
{
	int32_t rc;

	if (session_open) {
		return 0;
	}
	rc = mpsl_timeslot_session_open(on_signal, &session);
	if (rc != 0) {
		printk("session_open failed: %d\n", (int)rc);
		return (int)rc;
	}
	session_open = true;
	return 0;
}

static void close_session(void)
{
	if (!session_open) {
		return;
	}
	(void)mpsl_timeslot_session_close(session);
	session_open = false;
	/* SESSION_CLOSED may still be in flight; the next open is separated by
	 * the command loop's own round trip through the console, which is
	 * milliseconds. */
	k_sleep(K_MSEC(50));
}

/*
 * The first request in a session must be EARLIEST, and EARLIEST is forbidden
 * from inside a timeslot - so every session needs an explicit bootstrap from
 * thread context before any real work. P3 has the same obligation and it is the
 * one piece of the state machine with no equivalent anywhere in the existing
 * backend.
 */
static int request_earliest(uint32_t timeout_us)
{
	int32_t rc;

	next_req.request_type = MPSL_TIMESLOT_REQ_TYPE_EARLIEST;
	next_req.params.earliest.hfclk =
		MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED;
	next_req.params.earliest.priority = prio;
	next_req.params.earliest.length_us = slot_len_us;
	next_req.params.earliest.timeout_us = timeout_us;

	predicted_start_us = 0u;
	anchor_us = now_us();
	rc = mpsl_timeslot_request(session, &next_req);
	if (rc != 0) {
		printk("request(EARLIEST) failed: %d%s\n", (int)rc,
		       (rc == -NRF_EAGAIN) ? " (session not IDLE - exactly one "
					     "request may be outstanding)"
					   : "");
	}
	return (int)rc;
}

/* ---------------------------------------------------------------------------
 * Runs
 * ---------------------------------------------------------------------------
 */

static void run_earliest(uint32_t iterations)
{
	uint32_t i;

	stat_reset(&st_latency);
	n_start = 0u;
	n_idle = 0u;
	mode = MODE_EARLIEST;

	if (open_session() != 0) {
		return;
	}

	for (i = 0u; i < iterations; i++) {
		k_sem_reset(&idle_sem);
		if (request_earliest(1000000u) != 0) {
			break;
		}
		if (k_sem_take(&idle_sem, K_MSEC(2000)) != 0) {
			printk("timed out waiting for SESSION_IDLE at "
			       "iteration %u\n", i);
			break;
		}
		/* A gap between iterations, so consecutive EARLIEST requests
		 * are independent samples of the arbiter rather than one long
		 * chain. */
		k_sleep(K_MSEC(5));
	}

	mode = MODE_IDLE;
	close_session();

	printk("\n== EARLIEST, priority=%s, length=%u us, %s ==\n",
	       (prio == MPSL_TIMESLOT_PRIORITY_HIGH) ? "HIGH" : "NORMAL",
	       slot_len_us, IS_ENABLED(CONFIG_BT) ? "BLE on" : "no second stack");
	stat_print("grant_latency", &st_latency);
	printk("starts=%u idle=%u blocked=%u cancelled=%u\n", n_start, n_idle,
	       n_blocked, n_cancelled);
}

static void run_chain(uint32_t distance_us, uint32_t seconds)
{
	uint32_t deadline;

	stat_reset(&st_blocked);
	stat_reset(&st_blocked_early);
	stat_reset(&st_anchor);
	n_start = 0u;
	n_blocked = 0u;
	n_blocked_early = 0u;
	n_cancelled = 0u;
	blocks_since_grant = 0u;
	chain_distance_us = distance_us;
	mode = MODE_CHAIN;

	if (open_session() != 0) {
		return;
	}
	if (request_earliest(1000000u) != 0) {
		mode = MODE_IDLE;
		close_session();
		return;
	}

	deadline = k_uptime_get_32() + (seconds * 1000u);
	while (k_uptime_get_32() < deadline) {
		/* Recovery from the thread, not the signal: a BLOCKED request
		 * leaves the session IDLE with the chain broken, and re-arming
		 * from the BLOCKED callback would assert. This loop is the
		 * spike's stand-in for P3's deadline timer. The next distance
		 * comes from the UNMOVED anchor plus enough whole periods to
		 * be in the future - the anchor rule as arithmetic; st_anchor
		 * flags it on the next grant if wrong. */
		(void)k_sem_take(&idle_sem, K_MSEC(50));

		/* Only when the chain is actually broken - SESSION_IDLE fires
		 * for other reasons too, and acting on the semaphore alone
		 * issued a NORMAL request into a non-IDLE session, got
		 * -NRF_EAGAIN, and killed the run at its first wake-up. */
		if (!chain_broken || mode != MODE_CHAIN) {
			continue;
		}
		chain_broken = false;

		/*
		 * Next distance is computed from the clock, not the block
		 * count - a result, not a tidy-up. `distance * (blocks + 1)`
		 * is the obvious anchor-rule reading and works at 32/s but
		 * spirals at 90/s: at an 11.1 ms distance, thread-context
		 * recovery costs more than one distance, so by the time the
		 * request is placed its target instant has passed, gets
		 * refused, and the next attempt falls one distance further
		 * behind. Measured: blocked_lateness running to 13 s and a
		 * "block rate" of 5283/10000 that was this loop failing to
		 * keep up, not MPSL refusing anything.
		 *
		 * So the skip is however many whole distances from the anchor
		 * it takes to clear `now` - still whole distances, preserving
		 * the anchor's grid exactly.
		 *
		 * P3 inherits this: its front end recovers the same
		 * thread-context way, and a tracked ANT+ slot at 249.7 ms has
		 * far more headroom than 11.1 ms, but a burst turnaround at
		 * 1.55 ms has far less - why the arbitrated backend refuses
		 * those synchronously instead of re-requesting.
		 */
		{
			uint32_t now = now_us();
			uint32_t elapsed = now - anchor_us;
			uint32_t skip = (elapsed + RECOVER_MARGIN_US) /
						distance_us + 1u;

			request_normal(distance_us * skip);
		}
		if (mpsl_timeslot_request(session, &next_req) != 0) {
			/*
			 * Left broken so the next iteration tries again rather
			 * than ending the run. A request refused here is
			 * ordinarily just "the session has not gone IDLE yet",
			 * which is a wait rather than a failure - and treating
			 * it as a failure is exactly what produced a block rate
			 * that was really a stopped chain.
			 */
			chain_broken = true;
		}
	}

	mode = MODE_IDLE;
	close_session();

	printk("\n== NORMAL chain, distance=%u us (%u/s), priority=%s, %s ==\n",
	       distance_us, (distance_us != 0u) ? (1000000u / distance_us) : 0u,
	       (prio == MPSL_TIMESLOT_PRIORITY_HIGH) ? "HIGH" : "NORMAL",
	       IS_ENABLED(CONFIG_BT) ? "BLE on" : "no second stack");
	printk("starts=%u blocked=%u cancelled=%u", n_start, n_blocked,
	       n_cancelled);
	if (n_start + n_blocked > 0u) {
		printk("  block rate = %u/10000\n",
		       (uint32_t)(((uint64_t)n_blocked * 10000u) /
				  (n_start + n_blocked)));
	} else {
		printk("\n");
	}
	/* The number that sizes P3's deadline timer, and the count that says
	 * whether the timer is required at all. */
	printk("blocked: %u after the requested start, %u before it\n",
	       n_blocked - n_blocked_early, n_blocked_early);
	stat_print("blocked_lateness", &st_blocked);
	stat_print("blocked_earliness", &st_blocked_early);
	/* The number that says whether the anchor rule above is right. On this
	 * part MPSL_TIMESLOT_START_JITTER_US is 0, so anything beyond a few
	 * microseconds here is a walking schedule rather than jitter. */
	stat_print("anchor_error", &st_anchor);
}

static void run_extend(uint32_t iterations)
{
	uint32_t i;

	stat_reset(&st_extend);
	n_ext_ok = 0u;
	n_ext_fail = 0u;
	mode = MODE_EXTEND;

	if (open_session() != 0) {
		return;
	}

	for (i = 0u; i < iterations; i++) {
		k_sem_reset(&idle_sem);
		if (request_earliest(1000000u) != 0) {
			break;
		}
		if (k_sem_take(&idle_sem, K_MSEC(2000)) != 0) {
			printk("timed out at iteration %u\n", i);
			break;
		}
		k_sleep(K_MSEC(5));
	}

	mode = MODE_IDLE;
	close_session();

	printk("\n== EXTENSION chain, step=%u us, cap=%u us, %s ==\n",
	       EXT_STEP_US, EXT_CAP_US,
	       IS_ENABLED(CONFIG_BT) ? "BLE on" : "no second stack");
	printk("extend_ok=%u extend_failed=%u\n", n_ext_ok, n_ext_fail);
	/* If achieved length is at the cap every time, the elastic model works
	 * and OUR cap bounded it. If it lands well short with extend_failed
	 * non-zero, the arbiter bounded it - which is the answer ADR 0013's
	 * "discovered rather than configured" claim needs. */
	stat_print("achieved_length", &st_extend);
}

/*
 * Does a TIMER that is not MPSL_TIMER0 keep its count across timeslot edges?
 *
 * Compared against the kernel's own clock rather than against itself, because
 * a timer that stopped and restarted would still look monotonic to a reader
 * that only ever asked it. k_uptime is driven by GRTC on this part, which is
 * also what MPSL schedules off - so a disagreement between the two is the
 * measurement, and agreement to within the millisecond resolution of k_uptime
 * is the result the design needs.
 */
static void run_timer_continuity(uint32_t seconds)
{
	uint32_t t0, u0, t1, u1;
	int32_t  drift_ms;

	n_start = 0u;
	t0 = now_us();
	u0 = k_uptime_get_32();

	run_chain(chain_distance_us, seconds);

	t1 = now_us();
	u1 = k_uptime_get_32();

	drift_ms = (int32_t)((t1 - t0) / 1000u) - (int32_t)(u1 - u0);

	printk("\n== TIMER continuity across %u timeslots ==\n", n_start);
	printk("free timer %u ms, kernel %u ms, difference %d ms\n",
	       (t1 - t0) / 1000u, u1 - u0, drift_ms);
	/* Read the verdict with hfxo_hold() in mind - a failure here has two
	 * causes needing different answers: ~0.4% fast scaling with run
	 * length means the TIMER is on the internal RC (not a timeslot
	 * problem - what this measured before hfxo_hold() existed); an error
	 * that does NOT scale with run length means timeslot edges really
	 * are disturbing the TIMER. The ppm figure distinguishes the two. */
	printk("%s (%d ppm)\n",
	       (drift_ms > -5 && drift_ms < 5)
		       ? "the free-running TIMER survived every timeslot edge"
		       : "*** DISTURBED or off-crystal - see the note in "
			 "run_timer_continuity()",
	       (u1 - u0) ? (int)((int64_t)drift_ms * 1000000 /
				 (int64_t)(u1 - u0))
			 : 0);
}

/* ---------------------------------------------------------------------------
 * ADR 0018 Phase 0c: three API facts that decide which candidates are viable.
 *
 *   F1  Does MPSL let ONE timeslot be extended past 100 ms in total?
 *       (MPSL_TIMESLOT_LENGTH_MAX_US documents only the "event"; the gate
 *       assumes not.) Just-in-time 1 ms chain to 300 ms, the gate's pattern:
 *       an MPSL_TIMER0 compare END_MARGIN before the end asks for the next
 *       step. Plus one 30 ms step mid-grant, at 40 ms and straddling 100 ms.
 *       Plus the old back-to-back chain (EXTEND from EXTEND_SUCCEEDED).
 *   F2  Two sessions in this one client. Session 1 holds an extended NORMAL
 *       grant; session 2 asks for a short slot inside it, placed either
 *       before session 1's extensions reach that instant (A) or after one
 *       big extension already covers it (B).
 *   F3  ACTION_REQUEST returned at a grant's end, the next start a short
 *       gap after the close. Grant / BLOCKED rate per gap.
 *
 * Nothing in a callback calls the kernel except SESSION_IDLE's semaphore:
 * START/TIMER0/EXTEND run in a zero-latency IRQ (radiant-zli-kernel-calls),
 * so their results reach the thread through volatile flags it polls.
 * SESSION_IDLE runs from mpsl_low_priority_process(), as above.
 * ---------------------------------------------------------------------------
 */

#define FX_ELASTIC_MARGIN_US 2000u  /* gate END_MARGIN_EXTEND_US */
#define FX_FIXED_MARGIN_US   750u   /* gate END_MARGIN_FIXED_US */
#define FX_INIT_LEN_US       5000u
#define FX_MAXREC            64u

enum fx_mode {
	FX_NONE = 0,
	FX_F1,
	FX_F2,
	FX_F3,
};

static volatile enum fx_mode fx_mode;
static volatile uint32_t fx_iter;      /* printed by the assert handler */

static mpsl_timeslot_signal_return_param_t fx_rv[2];
static mpsl_timeslot_request_t fx_req[2];
static mpsl_timeslot_session_id_t fx_sid[2];
static bool fx_is_open[2];
static K_SEM_DEFINE(fx_idle0, 0, 1);
static K_SEM_DEFINE(fx_idle1, 0, 1);

static volatile uint32_t fx_invalid, fx_overstay, fx_cancel0, fx_cancel1;

/* The thread's own capture channel, so a timeslot callback that captures
 * CC0 between the thread's trigger and read cannot hand the thread its
 * value. */
static inline uint32_t now_us_thr(void)
{
	nrf_timer_task_trigger(FREE_TIMER, nrf_timer_capture_task_get(1));
	return nrf_timer_cc_get(FREE_TIMER, 1);
}

/* MPSL_TIMER0 runs from zero at each timeslot start and is ours inside it.
 * CC1 as the capture, as the gate does. */
static inline uint32_t mt0_now(void)
{
	nrf_timer_task_trigger(MPSL_TIMER0, nrf_timer_capture_task_get(1));
	return nrf_timer_cc_get(MPSL_TIMER0, 1);
}

/* The gate's ordering: cc_set, event_clear, int_enable (stale compare). */
static void fx_cc_arm(uint32_t at)
{
	nrf_timer_cc_set(MPSL_TIMER0, NRF_TIMER_CC_CHANNEL0, at);
	nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0);
	nrf_timer_int_enable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE0_MASK);
}

static void fx_cc_disarm(void)
{
	nrf_timer_int_disable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE0_MASK);
	nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0);
}

static int fx_open(int idx, mpsl_timeslot_callback_t cb)
{
	int32_t rc;

	if (fx_is_open[idx]) {
		return 0;
	}
	rc = mpsl_timeslot_session_open(cb, &fx_sid[idx]);
	printk("FX|session_open idx=%d rc=%d id=%u\n", idx, (int)rc,
	       (unsigned)fx_sid[idx]);
	if (rc == 0) {
		fx_is_open[idx] = true;
	}
	return (int)rc;
}

static void fx_close_all(void)
{
	for (int i = 0; i < 2; i++) {
		if (fx_is_open[i]) {
			(void)mpsl_timeslot_session_close(fx_sid[i]);
			fx_is_open[i] = false;
		}
	}
	k_sleep(K_MSEC(50));
}

static int fx_earliest(int idx, uint8_t pr, uint32_t len_us, uint8_t hfclk)
{
	int32_t rc;

	fx_req[idx].request_type = MPSL_TIMESLOT_REQ_TYPE_EARLIEST;
	fx_req[idx].params.earliest.hfclk = hfclk;
	fx_req[idx].params.earliest.priority = pr;
	fx_req[idx].params.earliest.length_us = len_us;
	fx_req[idx].params.earliest.timeout_us = 1000000u;
	rc = mpsl_timeslot_request(fx_sid[idx], &fx_req[idx]);
	if (rc != 0) {
		printk("FX|earliest idx=%d rc=%d\n", idx, (int)rc);
	}
	return (int)rc;
}

static void fx_normal(int idx, uint8_t pr, uint32_t dist_us, uint32_t len_us,
		      uint8_t hfclk)
{
	fx_req[idx].request_type = MPSL_TIMESLOT_REQ_TYPE_NORMAL;
	fx_req[idx].params.normal.hfclk = hfclk;
	fx_req[idx].params.normal.priority = pr;
	fx_req[idx].params.normal.distance_us = dist_us;
	fx_req[idx].params.normal.length_us = len_us;
}

static const char *stack_name(void)
{
	return IS_ENABLED(CONFIG_BT) ? "ble" : "none";
}

/* ------------------------------- F1 ------------------------------------- */

#define F1_TARGET_US 300000u
#define F1_STEP_US   1000u
#define F1_BIG_US    30000u

enum { END_TARGET = 1, END_EXTFAIL = 2 };

struct f1_rec {
	uint32_t total_us;    /* granted length when it ended */
	uint32_t held_us;     /* free timer, START -> END returned */
	uint32_t ext_ok;
	uint32_t ext_fail;
	uint32_t fail_ask_us; /* total the refused step would have reached */
	uint32_t fail_step_us;
	int32_t  drift_us;    /* free-timer elapsed minus MPSL_TIMER0 elapsed */
	uint32_t mt0_end;     /* MPSL_TIMER0 at the end (wrap / reset check) */
	uint8_t  big;         /* 0 none, 1 ok, 2 refused */
	uint8_t  reason;
};

static volatile bool f1_jit;           /* false: back-to-back chain */
static volatile uint32_t f1_big_at;    /* 0: no big step */
static struct f1_rec f1_recs[FX_MAXREC];
static volatile uint32_t f1_n;
static uint32_t f1_t0, f1_m0, f1_granted, f1_step;
static bool f1_big_done;

static uint32_t f1_next_step(void)
{
	uint32_t step = F1_STEP_US;

	if (f1_big_at != 0u && !f1_big_done && f1_granted >= f1_big_at) {
		step = F1_BIG_US;
	}
	if (f1_granted + step > F1_TARGET_US) {
		step = F1_TARGET_US - f1_granted;
	}
	return step;
}

static void f1_finish(uint8_t reason)
{
	uint32_t t = now_us();
	uint32_t m = mt0_now();
	struct f1_rec *r;

	fx_cc_disarm();
	if (f1_n < FX_MAXREC) {
		r = &f1_recs[f1_n];
		r->total_us = f1_granted;
		r->held_us = t - f1_t0;
		r->drift_us = (int32_t)(t - f1_t0) - (int32_t)(m - f1_m0);
		r->mt0_end = m;
		r->reason = reason;
	}
	f1_n++;
	fx_rv[0].callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
}

static bool f1_try_extend(void)
{
	uint32_t step = f1_next_step();

	if (step < MPSL_TIMESLOT_EXTENSION_TIME_MIN_US) {
		return false;
	}
	f1_step = step;
	fx_rv[0].callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND;
	fx_rv[0].params.extend.length_us = step;
	return true;
}

static mpsl_timeslot_signal_return_param_t *f1_cb(uint32_t signal, uint32_t t)
{
	switch (signal) {
	case MPSL_TIMESLOT_SIGNAL_START:
		f1_t0 = t;
		f1_m0 = mt0_now();
		f1_granted = FX_INIT_LEN_US;
		f1_big_done = false;
		if (f1_n < FX_MAXREC) {
			memset(&f1_recs[f1_n], 0, sizeof(f1_recs[0]));
		}
		fx_cc_arm(FX_INIT_LEN_US - FX_ELASTIC_MARGIN_US);
		if (!f1_jit) {
			(void)f1_try_extend();
		}
		break;
	case MPSL_TIMESLOT_SIGNAL_TIMER0:
		nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0);
		if (!(f1_jit && f1_try_extend())) {
			f1_finish(END_TARGET);
		}
		break;
	case MPSL_TIMESLOT_SIGNAL_EXTEND_SUCCEEDED:
		f1_granted += f1_step;
		if (f1_n < FX_MAXREC) {
			f1_recs[f1_n].ext_ok++;
			if (f1_step == F1_BIG_US) {
				f1_recs[f1_n].big = 1u;
			}
		}
		if (f1_step == F1_BIG_US) {
			f1_big_done = true;
		}
		nrf_timer_cc_set(MPSL_TIMER0, NRF_TIMER_CC_CHANNEL0,
				 f1_granted - FX_ELASTIC_MARGIN_US);
		if (!f1_jit) {
			(void)f1_try_extend();
		}
		break;
	case MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED:
		if (f1_n < FX_MAXREC) {
			f1_recs[f1_n].ext_fail++;
			f1_recs[f1_n].fail_ask_us = f1_granted + f1_step;
			f1_recs[f1_n].fail_step_us = f1_step;
			if (f1_step == F1_BIG_US) {
				f1_recs[f1_n].big = 2u;
			}
		}
		f1_finish(END_EXTFAIL);
		break;
	case MPSL_TIMESLOT_SIGNAL_CANCELLED:
		fx_cancel0++;
		break;
	case MPSL_TIMESLOT_SIGNAL_SESSION_IDLE:
		k_sem_give(&fx_idle0);
		break;
	default:
		break;
	}
	return &fx_rv[0];
}

/* ------------------------------- F2 ------------------------------------- */

#define F2_S1_CAP_US    90000u
#define F2_S2_OFFSET_US 40000u
#define F2_S2_LEN_US    2000u
#define F2_B_BIG_US     60000u

struct f2_rec {
	uint32_t s1_t0, s1_end, s1_granted, s1_fail_ask;
	uint32_t s2_req_t, s2_target, s2_start, s2_end, s2_block_t;
	uint8_t  s1_reason; /* 0 never ended by us, END_TARGET (cap), END_EXTFAIL */
	uint8_t  s2_out;    /* 0 none, 1 granted, 2 blocked, 3 cancelled, 4 skipped */
	uint8_t  s1_ext_fail;
};

static volatile bool f2_variant_b;
/* Session 1's chain step. 1 ms is the gate's EXTEND_STEP_US; the 200 us
 * (MPSL_TIMESLOT_EXTENSION_TIME_MIN_US) variant resolves how close to
 * session 2's start MPSL lets session 1 extend. */
static volatile uint32_t f2_chain_step = 1000u;
static volatile bool f2_s2_boot;
static struct f2_rec f2_cur;
static volatile bool f2_s1_started, f2_s1_big_ok, f2_s1_big_fail;
static volatile bool f2_s1_ended;
static uint32_t f2_s1_granted, f2_s1_step;
static volatile uint32_t f2_anchor2;

static mpsl_timeslot_signal_return_param_t *f2_cb_s1(uint32_t signal, uint32_t t)
{
	mpsl_timeslot_signal_return_param_t *p = &fx_rv[0];

	switch (signal) {
	case MPSL_TIMESLOT_SIGNAL_START:
		f2_cur.s1_t0 = t;
		f2_s1_granted = FX_INIT_LEN_US;
		fx_cc_arm(FX_INIT_LEN_US - FX_ELASTIC_MARGIN_US);
		f2_s1_started = true;
		if (f2_variant_b) {
			f2_s1_step = F2_B_BIG_US;
			p->callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND;
			p->params.extend.length_us = F2_B_BIG_US;
		}
		break;
	case MPSL_TIMESLOT_SIGNAL_TIMER0:
		nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0);
		if (f2_s1_granted + f2_chain_step <= F2_S1_CAP_US) {
			f2_s1_step = f2_chain_step;
			p->callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND;
			p->params.extend.length_us = f2_chain_step;
		} else {
			fx_cc_disarm();
			f2_cur.s1_end = t;
			f2_cur.s1_granted = f2_s1_granted;
			f2_cur.s1_reason = END_TARGET;
			f2_s1_ended = true;
			p->callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
		}
		break;
	case MPSL_TIMESLOT_SIGNAL_EXTEND_SUCCEEDED:
		f2_s1_granted += f2_s1_step;
		nrf_timer_cc_set(MPSL_TIMER0, NRF_TIMER_CC_CHANNEL0,
				 f2_s1_granted - FX_ELASTIC_MARGIN_US);
		if (f2_s1_step == F2_B_BIG_US) {
			f2_s1_big_ok = true;
		}
		break;
	case MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED:
		fx_cc_disarm();
		f2_cur.s1_ext_fail++;
		f2_cur.s1_fail_ask = f2_s1_granted + f2_s1_step;
		f2_cur.s1_end = t;
		f2_cur.s1_granted = f2_s1_granted;
		f2_cur.s1_reason = END_EXTFAIL;
		if (f2_s1_step == F2_B_BIG_US) {
			f2_s1_big_fail = true;
		}
		f2_s1_ended = true;
		p->callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
		break;
	case MPSL_TIMESLOT_SIGNAL_CANCELLED:
		fx_cancel0++;
		break;
	case MPSL_TIMESLOT_SIGNAL_SESSION_IDLE:
		k_sem_give(&fx_idle0);
		break;
	default:
		break;
	}
	return p;
}

static mpsl_timeslot_signal_return_param_t *f2_cb_s2(uint32_t signal, uint32_t t)
{
	mpsl_timeslot_signal_return_param_t *p = &fx_rv[1];

	switch (signal) {
	case MPSL_TIMESLOT_SIGNAL_START:
		f2_anchor2 = t;
		if (f2_s2_boot) {
			p->callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
			break;
		}
		f2_cur.s2_start = t;
		f2_cur.s2_out = 1u;
		/* Hold it as a tracked window would, then end. */
		fx_cc_arm(F2_S2_LEN_US - FX_FIXED_MARGIN_US);
		break;
	case MPSL_TIMESLOT_SIGNAL_TIMER0:
		fx_cc_disarm();
		f2_cur.s2_end = t;
		p->callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
		break;
	case MPSL_TIMESLOT_SIGNAL_BLOCKED:
		f2_cur.s2_out = 2u;
		f2_cur.s2_block_t = t;
		break;
	case MPSL_TIMESLOT_SIGNAL_CANCELLED:
		fx_cancel1++;
		f2_cur.s2_out = 3u;
		f2_cur.s2_block_t = t;
		break;
	case MPSL_TIMESLOT_SIGNAL_SESSION_IDLE:
		k_sem_give(&fx_idle1);
		break;
	default:
		break;
	}
	return p;
}

/* ------------------------------- F3 ------------------------------------- */

#define F3_LEN_US 5000u

static volatile uint32_t f3_gap_us;
static volatile uint32_t f3_target;   /* short-notice attempts wanted */
static volatile uint8_t  f3_hfclk = MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED;
static volatile bool     f3_pending_short, f3_broken, f3_stop;
static volatile uint32_t f3_ok, f3_blk, f3_rec_ok, f3_rec_blk, f3_cancel;
static volatile uint32_t f3_anchor;
static uint32_t f3_pred;
static struct stat st_f3_err, st_f3_blk_late, st_f3_blk_early;

static mpsl_timeslot_signal_return_param_t *f3_cb(uint32_t signal, uint32_t t)
{
	mpsl_timeslot_signal_return_param_t *p = &fx_rv[0];

	switch (signal) {
	case MPSL_TIMESLOT_SIGNAL_START:
		if (f3_pending_short) {
			f3_ok++;
			stat_add(&st_f3_err, (t > f3_pred) ? (t - f3_pred)
							   : (f3_pred - t));
		} else {
			f3_rec_ok++;
		}
		f3_pending_short = false;
		f3_anchor = t;
		fx_cc_arm(F3_LEN_US - FX_FIXED_MARGIN_US);
		break;
	case MPSL_TIMESLOT_SIGNAL_TIMER0: {
		uint32_t m = mt0_now();

		fx_cc_disarm();
		if (f3_stop || f3_ok + f3_blk >= f3_target) {
			f3_stop = true;
			p->callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
			break;
		}
		/* distance is from MPSL's own start, which MPSL_TIMER0 counts
		 * from: next start = this close + gap. */
		fx_normal(0, MPSL_TIMESLOT_PRIORITY_NORMAL, m + f3_gap_us,
			  F3_LEN_US, f3_hfclk);
		f3_pred = t + f3_gap_us;
		f3_pending_short = true;
		p->callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_REQUEST;
		p->params.request.p_next = &fx_req[0];
		break;
	}
	case MPSL_TIMESLOT_SIGNAL_BLOCKED:
		if (f3_pending_short) {
			f3_blk++;
			if (t > f3_pred) {
				stat_add(&st_f3_blk_late, t - f3_pred);
			} else {
				stat_add(&st_f3_blk_early, f3_pred - t);
			}
		} else {
			f3_rec_blk++;
		}
		f3_pending_short = false;
		f3_broken = true;
		break;
	case MPSL_TIMESLOT_SIGNAL_CANCELLED:
		f3_cancel++;
		f3_pending_short = false;
		f3_broken = true;
		break;
	case MPSL_TIMESLOT_SIGNAL_SESSION_IDLE:
		k_sem_give(&fx_idle0);
		break;
	default:
		break;
	}
	return p;
}

/* ----------------------------- dispatch --------------------------------- */

static mpsl_timeslot_signal_return_param_t *fx_cb0(
	mpsl_timeslot_session_id_t id, uint32_t signal)
{
	uint32_t t = now_us();

	(void)id;
	fx_rv[0].callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_NONE;

	if (signal == MPSL_TIMESLOT_SIGNAL_INVALID_RETURN) {
		fx_invalid++;
		return &fx_rv[0];
	}
	if (signal == MPSL_TIMESLOT_SIGNAL_OVERSTAYED) {
		fx_overstay++;
		return &fx_rv[0];
	}

	switch (fx_mode) {
	case FX_F1:
		return f1_cb(signal, t);
	case FX_F2:
		return f2_cb_s1(signal, t);
	case FX_F3:
		return f3_cb(signal, t);
	default:
		if (signal == MPSL_TIMESLOT_SIGNAL_START) {
			fx_rv[0].callback_action =
				MPSL_TIMESLOT_SIGNAL_ACTION_END;
		} else if (signal == MPSL_TIMESLOT_SIGNAL_SESSION_IDLE) {
			k_sem_give(&fx_idle0);
		}
		return &fx_rv[0];
	}
}

static mpsl_timeslot_signal_return_param_t *fx_cb1(
	mpsl_timeslot_session_id_t id, uint32_t signal)
{
	uint32_t t = now_us();

	(void)id;
	fx_rv[1].callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_NONE;
	if (signal == MPSL_TIMESLOT_SIGNAL_INVALID_RETURN) {
		fx_invalid++;
		return &fx_rv[1];
	}
	if (signal == MPSL_TIMESLOT_SIGNAL_OVERSTAYED) {
		fx_overstay++;
		return &fx_rv[1];
	}
	if (fx_mode == FX_F2) {
		return f2_cb_s2(signal, t);
	}
	if (signal == MPSL_TIMESLOT_SIGNAL_START) {
		fx_rv[1].callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
	} else if (signal == MPSL_TIMESLOT_SIGNAL_SESSION_IDLE) {
		k_sem_give(&fx_idle1);
	}
	return &fx_rv[1];
}

/* ------------------------------ F1 run ---------------------------------- */

static void run_f1_one(const char *name, bool jit, uint32_t big_at,
		       uint32_t iters)
{
	uint32_t i, u0, t0, u1, t1;
	uint32_t n_target = 0, n_fail = 0, n_past100 = 0, n_big_ok = 0;
	uint32_t n_big_fail = 0;
	uint32_t max_total = 0, min_fail_ask = 0xFFFFFFFFu;
	int32_t dmin = INT32_MAX, dmax = INT32_MIN;

	f1_jit = jit;
	f1_big_at = big_at;
	f1_n = 0u;
	fx_invalid = fx_overstay = fx_cancel0 = 0u;
	fx_mode = FX_F1;

	if (fx_open(0, fx_cb0) != 0) {
		fx_mode = FX_NONE;
		return;
	}
	u0 = k_uptime_get_32();
	t0 = now_us_thr();
	for (i = 0u; i < iters && i < FX_MAXREC; i++) {
		fx_iter = i;
		k_sem_reset(&fx_idle0);
		if (fx_earliest(0, MPSL_TIMESLOT_PRIORITY_NORMAL,
				FX_INIT_LEN_US,
				MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED) != 0) {
			break;
		}
		if (k_sem_take(&fx_idle0, K_MSEC(2000)) != 0) {
			printk("F1|%s timeout at iter %u\n", name, i);
			break;
		}
		k_sleep(K_MSEC(5));
	}
	t1 = now_us_thr();
	u1 = k_uptime_get_32();
	fx_mode = FX_NONE;
	fx_close_all();

	for (i = 0u; i < f1_n && i < FX_MAXREC; i++) {
		struct f1_rec *r = &f1_recs[i];

		printk("F1|%s|%s|g=%u total=%u held=%u ext_ok=%u ext_fail=%u "
		       "fail_ask=%u fail_step=%u big=%u drift=%d mt0_end=%u "
		       "end=%s\n",
		       name, stack_name(), i, r->total_us, r->held_us,
		       r->ext_ok, r->ext_fail, r->fail_ask_us, r->fail_step_us,
		       r->big, r->drift_us, r->mt0_end,
		       (r->reason == END_TARGET) ? "target" : "extfail");
		if (r->reason == END_TARGET) {
			n_target++;
		} else {
			n_fail++;
			if (r->fail_ask_us < min_fail_ask) {
				min_fail_ask = r->fail_ask_us;
			}
		}
		if (r->total_us > MPSL_TIMESLOT_LENGTH_MAX_US) {
			n_past100++;
		}
		if (r->total_us > max_total) {
			max_total = r->total_us;
		}
		if (r->drift_us < dmin) {
			dmin = r->drift_us;
		}
		if (r->drift_us > dmax) {
			dmax = r->drift_us;
		}
		n_big_ok += (r->big == 1u);
		n_big_fail += (r->big == 2u);
	}
	printk("F1S|%s|%s|grants=%u reached_300ms=%u extfail=%u past_100ms=%u "
	       "max_total=%u min_fail_ask=%u big_ok=%u big_fail=%u "
	       "drift_min=%d drift_max=%d invalid=%u overstay=%u "
	       "cancelled=%u\n",
	       name, stack_name(), f1_n, n_target, n_fail, n_past100, max_total,
	       (min_fail_ask == 0xFFFFFFFFu) ? 0u : min_fail_ask, n_big_ok,
	       n_big_fail, dmin, dmax, fx_invalid, fx_overstay, fx_cancel0);
	printk("F1S|%s|%s|run free=%u us kernel=%u ms diff=%d us\n", name,
	       stack_name(), t1 - t0, u1 - u0,
	       (int)((int32_t)(t1 - t0) - (int32_t)((u1 - u0) * 1000u)));
}

static void run_f1(void)
{
	printk("\nF1|BEGIN stack=%s\n", stack_name());
	run_f1_one("jit1", true, 0u, 20u);
	run_f1_one("jit1_big40", true, 40000u, 20u);
	run_f1_one("jit1_big85", true, 85000u, 20u);
	run_f1_one("b2b1", false, 0u, 20u);
	printk("F1|END\n");
}

/* ------------------------------ F2 run ---------------------------------- */

static bool poll_flag(volatile bool *f, uint32_t timeout_us)
{
	uint32_t t0 = now_us_thr();

	while (!*f) {
		if (now_us_thr() - t0 > timeout_us) {
			return false;
		}
		k_busy_wait(20);
	}
	return true;
}

static void run_f2_one(const char *name, bool variant_b, uint8_t s2_prio,
		       uint32_t iters)
{
	uint32_t i;
	uint32_t n_s2_ok = 0, n_s2_blk = 0, n_s2_can = 0, n_skip = 0;
	uint32_t n_s1_cut = 0, n_s1_extfail = 0, n_overlap = 0;
	struct stat st_hand, st_short, st_s2err;

	stat_reset(&st_hand);
	stat_reset(&st_short);
	stat_reset(&st_s2err);
	fx_invalid = fx_overstay = fx_cancel0 = fx_cancel1 = 0u;
	f2_variant_b = variant_b;
	fx_mode = FX_F2;

	if (fx_open(0, fx_cb0) != 0) {
		fx_mode = FX_NONE;
		return;
	}
	/* The header's own contradiction ("only one session can be open at a
	 * time" vs -NRF_ENOMEM "all sessions are already open"): the rc of
	 * this second open while the first is open is part of the answer. */
	if (fx_open(1, fx_cb1) != 0) {
		fx_mode = FX_NONE;
		fx_close_all();
		return;
	}

	/* Session 2's first request must be EARLIEST. */
	f2_s2_boot = true;
	k_sem_reset(&fx_idle1);
	if (fx_earliest(1, s2_prio, 1000u,
			MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED) != 0 ||
	    k_sem_take(&fx_idle1, K_MSEC(2000)) != 0) {
		printk("F2|%s session 2 bootstrap failed\n", name);
		fx_mode = FX_NONE;
		fx_close_all();
		return;
	}
	f2_s2_boot = false;
	k_sleep(K_MSEC(5));

	for (i = 0u; i < iters; i++) {
		struct f2_rec r;
		uint32_t now, dist;
		bool placed = false;

		fx_iter = i;
		memset(&f2_cur, 0, sizeof(f2_cur));
		f2_s1_started = f2_s1_big_ok = f2_s1_big_fail = false;
		f2_s1_ended = false;
		k_sem_reset(&fx_idle0);
		k_sem_reset(&fx_idle1);

		if (fx_earliest(0, MPSL_TIMESLOT_PRIORITY_NORMAL,
				FX_INIT_LEN_US,
				MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED) != 0) {
			break;
		}
		if (!poll_flag(&f2_s1_started, 1500000u)) {
			printk("F2|%s s1 never started at %u\n", name, i);
			break;
		}
		if (variant_b) {
			/* Wait for the one big step to be answered. */
			uint32_t w0 = now_us_thr();

			while (!f2_s1_big_ok && !f2_s1_big_fail &&
			       !f2_s1_ended && now_us_thr() - w0 < 10000u) {
				k_busy_wait(10);
			}
		}
		now = now_us_thr();
		f2_cur.s2_target = f2_cur.s1_t0 + F2_S2_OFFSET_US;
		if ((variant_b && !f2_s1_big_ok) ||
		    (int32_t)(f2_cur.s2_target - now) < 3000) {
			f2_cur.s2_out = 4u;
		} else {
			dist = f2_cur.s2_target - f2_anchor2;
			fx_normal(1, s2_prio, dist, F2_S2_LEN_US,
				  MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED);
			f2_cur.s2_req_t = now_us_thr();
			if (mpsl_timeslot_request(fx_sid[1], &fx_req[1]) == 0) {
				placed = true;
			} else {
				f2_cur.s2_out = 4u;
			}
		}
		(void)k_sem_take(&fx_idle0, K_MSEC(1000));
		if (placed) {
			(void)k_sem_take(&fx_idle1, K_MSEC(1000));
		}
		k_sleep(K_MSEC(2));
		r = f2_cur;

		printk("F2|%s|%s|i=%u s1_len=%u s1_granted=%u s1_end=%s "
		       "s1_fail_ask=%u s2=%s notice=%u s2_err=%d hand=%d "
		       "s2_len=%u\n",
		       name, stack_name(), i, r.s1_end - r.s1_t0, r.s1_granted,
		       (r.s1_reason == END_TARGET)    ? "cap"
		       : (r.s1_reason == END_EXTFAIL) ? "extfail"
						      : "unseen",
		       r.s1_fail_ask,
		       (r.s2_out == 1u)   ? "granted"
		       : (r.s2_out == 2u) ? "blocked"
		       : (r.s2_out == 3u) ? "cancelled"
		       : (r.s2_out == 4u) ? "skipped"
					  : "none",
		       r.s2_target - r.s2_req_t,
		       (r.s2_out == 1u) ? (int)(r.s2_start - r.s2_target)
					: (int)(r.s2_block_t - r.s2_target),
		       (r.s2_out == 1u) ? (int)(r.s2_start - r.s1_end) : 0,
		       (r.s2_out == 1u) ? (r.s2_end - r.s2_start) : 0u);

		switch (r.s2_out) {
		case 1u:
			n_s2_ok++;
			if ((int32_t)(r.s2_start - r.s1_end) < 0) {
				n_overlap++;
			} else {
				stat_add(&st_hand, r.s2_start - r.s1_end);
			}
			stat_add(&st_s2err, (r.s2_start > r.s2_target)
						    ? r.s2_start - r.s2_target
						    : r.s2_target - r.s2_start);
			break;
		case 2u:
			n_s2_blk++;
			break;
		case 3u:
			n_s2_can++;
			break;
		default:
			n_skip++;
			break;
		}
		if (r.s1_reason == END_EXTFAIL) {
			n_s1_extfail++;
			if (r.s2_out == 1u) {
				stat_add(&st_short, r.s2_target - r.s1_end);
			}
		}
		/* Session 1 ended with no END of ours: MPSL took the air some
		 * other way. */
		if (r.s1_reason == 0u) {
			n_s1_cut++;
		}
		k_sleep(K_MSEC(10));
	}
	fx_mode = FX_NONE;
	fx_close_all();

	printk("F2S|%s|%s|s2_prio=%s iters=%u s2_granted=%u s2_blocked=%u "
	       "s2_cancelled=%u skipped=%u s1_extfail=%u s1_end_unseen=%u "
	       "overlap=%u invalid=%u overstay=%u\n",
	       name, stack_name(),
	       (s2_prio == MPSL_TIMESLOT_PRIORITY_HIGH) ? "HIGH" : "NORMAL",
	       iters, n_s2_ok, n_s2_blk, n_s2_can, n_skip, n_s1_extfail,
	       n_s1_cut, n_overlap, fx_invalid, fx_overstay);
	stat_print("F2S handover", &st_hand);
	stat_print("F2S s1_shortfall", &st_short);
	stat_print("F2S s2_start_error", &st_s2err);
}

static void run_f2(void)
{
	printk("\nF2|BEGIN stack=%s sessions=%d\n", stack_name(),
	       CONFIG_MPSL_TIMESLOT_SESSION_COUNT);
	run_f2_one("A_high", false, MPSL_TIMESLOT_PRIORITY_HIGH, 30u);
	run_f2_one("A_normal", false, MPSL_TIMESLOT_PRIORITY_NORMAL, 30u);
	run_f2_one("B_high", true, MPSL_TIMESLOT_PRIORITY_HIGH, 30u);
	run_f2_one("B_normal", true, MPSL_TIMESLOT_PRIORITY_NORMAL, 30u);
	f2_chain_step = 200u;
	run_f2_one("A_high_step200", false, MPSL_TIMESLOT_PRIORITY_HIGH, 30u);
	f2_chain_step = 1000u;
	printk("F2|END\n");
}

/* ------------------------------ F3 run ---------------------------------- */

static void run_f3_one(uint32_t gap_us, uint8_t hfclk, uint32_t attempts)
{
	uint32_t deadline;

	stat_reset(&st_f3_err);
	stat_reset(&st_f3_blk_late);
	stat_reset(&st_f3_blk_early);
	f3_ok = f3_blk = f3_rec_ok = f3_rec_blk = f3_cancel = 0u;
	f3_pending_short = f3_broken = f3_stop = false;
	f3_gap_us = gap_us;
	f3_hfclk = hfclk;
	f3_target = attempts;
	fx_invalid = fx_overstay = 0u;
	fx_mode = FX_F3;

	if (fx_open(0, fx_cb0) != 0) {
		fx_mode = FX_NONE;
		return;
	}
	k_sem_reset(&fx_idle0);
	if (fx_earliest(0, MPSL_TIMESLOT_PRIORITY_NORMAL, F3_LEN_US, hfclk) !=
	    0) {
		fx_mode = FX_NONE;
		fx_close_all();
		return;
	}
	deadline = k_uptime_get_32() + 60000u;
	while (k_uptime_get_32() < deadline) {
		(void)k_sem_take(&fx_idle0, K_MSEC(50));
		if (f3_stop && !f3_broken) {
			break;
		}
		if (!f3_broken) {
			continue;
		}
		if (f3_ok + f3_blk >= f3_target) {
			break;
		}
		f3_broken = false;
		{
			/* Recovery, as run_chain(): whole 10 ms steps from
			 * the unmoved anchor, clear of now. */
			uint32_t el = now_us_thr() - f3_anchor;
			uint32_t skip = (el + RECOVER_MARGIN_US) / 10000u + 1u;

			fx_normal(0, MPSL_TIMESLOT_PRIORITY_NORMAL,
				  skip * 10000u, F3_LEN_US, hfclk);
		}
		if (mpsl_timeslot_request(fx_sid[0], &fx_req[0]) != 0) {
			f3_broken = true;
		}
	}
	fx_mode = FX_NONE;
	fx_close_all();

	printk("F3|%s|gap=%u hfclk=%s attempts=%u granted=%u blocked=%u "
	       "rate_blocked=%u/10000 recover_ok=%u recover_blocked=%u "
	       "cancelled=%u invalid=%u overstay=%u\n",
	       stack_name(), gap_us,
	       (hfclk == MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED) ? "xtal"
								    : "noguar",
	       f3_ok + f3_blk, f3_ok, f3_blk,
	       (f3_ok + f3_blk) ? (uint32_t)((uint64_t)f3_blk * 10000u /
					     (f3_ok + f3_blk))
				: 0u,
	       f3_rec_ok, f3_rec_blk, f3_cancel, fx_invalid, fx_overstay);
	stat_print("F3 start_error", &st_f3_err);
	stat_print("F3 blocked_late", &st_f3_blk_late);
	stat_print("F3 blocked_early", &st_f3_blk_early);
}

static void run_f3(void)
{
	static const uint32_t gaps[] = { 50u, 100u, 200u, 300u, 500u, 750u, 1000u,
					 1500u, 2000u, 3000u };

	printk("\nF3|BEGIN stack=%s\n", stack_name());
	for (int h = 0; h < 2; h++) {
		for (size_t g = 0; g < ARRAY_SIZE(gaps); g++) {
			fx_iter = gaps[g];
			run_f3_one(gaps[g],
				   (h == 0) ? MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED
					    : MPSL_TIMESLOT_HFCLK_CFG_NO_GUARANTEE,
				   300u);
		}
	}
	printk("F3|END\n");
}

#if defined(CONFIG_MPSL_ASSERT_HANDLER)
/* Prints, then reboots rather than halting (Phase 0c): an assert is a result,
 * and the host driver runs the next fact after the boot banner instead of
 * needing a J-Link reset per assert. */
void mpsl_assert_handle(const char *const file, const uint32_t line)
{
	printk("\n*** MPSL ASSERT %s:%u  fx_mode=%d fx_iter=%u starts=%u "
	       "blocked=%u ext_ok=%u ext_fail=%u overstay=%u fx_overstay=%u "
	       "fx_invalid=%u\n",
	       file, line, (int)fx_mode, fx_iter, n_start, n_blocked, n_ext_ok,
	       n_ext_fail, n_overstay, fx_overstay, fx_invalid);
	k_busy_wait(50000);
	sys_reboot(SYS_REBOOT_COLD);
}
#endif

/* ---------------------------------------------------------------------------
 * Console
 *
 * uart_poll_in() and a one-character command set, on the same terms as the
 * other spikes here: Zephyr's shell would cost tens of kilobytes in a program
 * whose flash figure is one of its results.
 * ---------------------------------------------------------------------------
 */

static const struct device *const console =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static void help(void)
{
	printk("\ncommands:\n"
	       "  e  EARLIEST grant latency, 100 iterations\n"
	       "  3  NORMAL chain at 32/s for 20 s (eight tracked sensors)\n"
	       "  9  NORMAL chain at 90/s for 20 s (those plus a sweep)\n"
	       "  x  extension chain, 50 iterations\n"
	       "  t  free-running TIMER continuity across timeslots\n"
	       "  p  toggle priority NORMAL <-> HIGH\n"
	       "  l  cycle slot length 1000 / 5000 / 20000 us\n"
	       "  a  ADR 0018 F1: extension past 100 ms (300 ms chain)\n"
	       "  b  ADR 0018 F2: two sessions, HIGH slot inside a held grant\n"
	       "  c  ADR 0018 F3: ACTION_REQUEST at grant end, short gaps\n"
	       "  ?  this\n");
}

int main(void)
{
	hfxo_hold();
	free_timer_start();

	printk("\n=== P0: MPSL timeslot arbiter ===\n");
	printk("MPSL: jitter=%u us  len=%u..%u us  ext_min=%u us  "
	       "ext_margin=%u us\n",
	       MPSL_TIMESLOT_START_JITTER_US, MPSL_TIMESLOT_LENGTH_MIN_US,
	       MPSL_TIMESLOT_LENGTH_MAX_US, MPSL_TIMESLOT_EXTENSION_TIME_MIN_US,
	       MPSL_TIMESLOT_EXTENSION_MARGIN_MIN_US);
	printk("context size=%u  sessions=%d  second stack: %s\n",
	       MPSL_TIMESLOT_CONTEXT_SIZE, CONFIG_MPSL_TIMESLOT_SESSION_COUNT,
	       IS_ENABLED(CONFIG_BT) ? "BLE advertiser" : "none");
	/*
	 * The 250 ms scan chunk, refused by arithmetic rather than by policy.
	 * MPSL_TIMESLOT_LENGTH_MAX_US is 100 000, so an ANT+ dwell of one
	 * channel period cannot be REQUESTED - which is why caps.max_window_us
	 * is a capability and not a tuning knob.
	 */
	BUILD_ASSERT(MPSL_TIMESLOT_LENGTH_MAX_US < 249700u,
		     "a full ANT+ channel period now fits in one timeslot, so "
		     "caps.max_window_us may no longer be needed for the scan");

#if defined(CONFIG_BT)
	{
		int err = bt_enable(NULL);

		if (err != 0) {
			printk("bt_enable failed: %d\n", err);
		} else {
			/*
			 * BT_LE_ADV_PARAM's interval is in 0.625 ms units.
			 * 1600..1920 is 1.0..1.2 s, which is the intended
			 * production setting; the adversarial sweep edits these
			 * two numbers and records which pair each result came
			 * from.
			 */
			err = bt_le_adv_start(
				BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, 1600, 1920,
						NULL),
				adv_data, ARRAY_SIZE(adv_data), NULL, 0);
			printk("advertiser: %s\n",
			       (err == 0) ? "1.0-1.2 s" : "FAILED");
		}
	}
#endif

	if (!device_is_ready(console)) {
		printk("console device not ready\n");
		return 0;
	}

	help();

	for (;;) {
		unsigned char c;

		if (uart_poll_in(console, &c) != 0) {
			k_sleep(K_MSEC(20));
			continue;
		}

		switch (c) {
		case 'e':
			run_earliest(100u);
			break;
		case '3':
			run_chain(31250u, 20u);
			break;
		case '9':
			run_chain(11111u, 20u);
			break;
		case 'x':
			run_extend(50u);
			break;
		case 't':
			run_timer_continuity(10u);
			break;
		case 'p':
			prio = (prio == MPSL_TIMESLOT_PRIORITY_NORMAL)
				       ? MPSL_TIMESLOT_PRIORITY_HIGH
				       : MPSL_TIMESLOT_PRIORITY_NORMAL;
			printk("priority = %s (and see finding 1: these two "
			       "rank us against ourselves, never against the "
			       "SoftDevice Controller)\n",
			       (prio == MPSL_TIMESLOT_PRIORITY_HIGH) ? "HIGH"
								     : "NORMAL");
			break;
		case 'l':
			slot_len_us = (slot_len_us == 1000u)   ? 5000u
				      : (slot_len_us == 5000u) ? 20000u
							       : 1000u;
			printk("slot length = %u us\n", slot_len_us);
			break;
		case 'a':
			run_f1();
			break;
		case 'b':
			run_f2();
			break;
		case 'c':
			run_f3();
			break;
		case '?':
		case 'h':
			help();
			break;
		default:
			break;
		}
	}

	return 0;
}
