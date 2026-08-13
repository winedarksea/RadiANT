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
 *   - WHETHER A FREE-RUNNING TIMER SURVIVES TIMESLOT EDGES - radiant_core's
 *     absolute timebase is a 1 MHz TIMER the HAL promises never goes
 *     backwards; if MPSL disturbed a non-MPSL_TIMER0 instance, the backend
 *     would need a different clock and P3 would be a much larger change.
 *
 * Touches no HAL, includes no radiant_core. Anything proved here must be
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
 * the instance the board overlay reserves, programmed like radiant_core's
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
 * A requirement on P3, not just this spike: radiant_core's nrf backend
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

#if defined(CONFIG_MPSL_ASSERT_HANDLER)
void mpsl_assert_handle(const char *const file, const uint32_t line)
{
	printk("\n*** MPSL ASSERT %s:%u  starts=%u blocked=%u ext_ok=%u "
	       "ext_fail=%u overstay=%u\n",
	       file, line, n_start, n_blocked, n_ext_ok, n_ext_fail, n_overstay);
	k_fatal_halt(K_ERR_KERNEL_PANIC);
}
#endif

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
