/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_channel.c - the per-channel state machine, its configuration, and the
 * slot clock.
 *
 * Provenance: clean-room. Written from docs/sdk-ant-contract.md and
 * src/ant_radio.h (the per-function semantics, the ordering constraints and
 * the permitted error codes), from the free ANT Message Protocol and Usage
 * Rev 5.1 (D00000652) for the channel-status byte layout and the 32768 Hz
 * period unit, and from this bench's own measurements in
 * docs/ant-radio-link.md and docs/spike-b-part2-results.md - specifically the
 * master's one-period open-to-first-transmission delay, the 249,696.4 us slot
 * period and the acknowledged-exchange turnarounds that set the period floor.
 * Nothing here derives from sdk-ant, from libant.a, from disassembly of any
 * binary, from rtl_433's expression, or from any adopter-gated ANT+ device
 * profile document. See docs/decisions/0002-clean-room-policy.md.
 *
 * This file includes no Zephyr header, nothing from the application, and no
 * HAL entry point, on purpose. Its gate is
 *
 *   arm-zephyr-eabi-gcc -c -std=c11 -Wall -Wextra -Werror \
 *       -I radiant_core/include -I radiant_core/tests -fsyntax-only \
 *       radiant_core/src/radiant_channel.c
 *
 * so a compile error here is never confused with a Zephyr or a board problem,
 * and the module can be exercised by a host-side tool with nothing but a C
 * compiler.
 *
 * ---------------------------------------------------------------------------
 * The three rules this file is written to
 * ---------------------------------------------------------------------------
 * 1. EVERY REFUSAL RETURNS THE EXACT WIRE BYTE. antr_err_t is a uint8_t whose
 *    value goes on the air to the host unchanged, so a wrong code is not a
 *    style problem, it is a defect tools/ant_conformance.py's byte-diff will
 *    find. Where two codes are close - a bad channel NUMBER is
 *    INVALID_MESSAGE, a bad parameter VALUE is INVALID_PARAMETER_PROVIDED -
 *    the range check runs first and separately, so the two can never be
 *    reached by one path.
 *
 * 2. NO HAL CALLS. Not radiant_radio_now(), not radiant_radio_abort(). Time arrives as
 *    an argument. That is what lets the state machine be tested as a pure
 *    function and what stops a channel from arming a window radiant_sched.c wanted
 *    to merge with another channel's.
 *
 * 3. AN OPERATION ID IS THE ONLY THING THAT LINKS A CHANNEL TO THE RADIO. The
 *    HAL guarantees that a cancelled operation's terminal event still arrives,
 *    with no fault injection needed, so "an event for an operation nobody
 *    owns" is a normal outcome and not an anomaly. Every path that consumes an
 *    event looks the op up first and counts the miss.
 */

#include <string.h>

#include <radiant_core/radiant_channel.h>

/* C11 rather than Zephyr's BUILD_ASSERT, because this file includes no Zephyr
 * header. */

/* The count is the serial protocol's ceiling: the burst header carries the
 * channel in the low five bits of the channel byte. A build that raised it
 * would produce bursts addressed to the wrong channel and nothing else would
 * complain. */
_Static_assert(RADIANT_CHANNEL_COUNT <= 32u,
	       "more channels than the burst header's five bits can address");
_Static_assert(RADIANT_CHANNEL_COUNT >= 1u, "a stack with no channels");

/* The state fits in the uint8_t the per-channel record stores it in. */
_Static_assert(RADIANT_CH_STATE_COUNT <= 255, "state enum outgrew its uint8_t");

/* ---------------------------------------------------------------------------
 * Per-channel record
 *
 * Flat, statically sized, 32 of them. The plan budgets ~72 B per channel for
 * 32 channels = 2.3 KB, which is what makes a 32-channel dongle fit inside the
 * footprint libant.a needed for eight; the _Static_assert below is what keeps
 * a later field addition honest about spending it.
 *
 * Field order is by descending alignment so the record packs without the
 * compiler inserting holes. It is not aesthetic: at 32 copies, four bytes of
 * padding is 128 bytes of RAM.
 * ---------------------------------------------------------------------------
 */

/* flags */
#define CHF_ID_SET               0x01u /* radiant_channel_id_set() has been called */
#define CHF_ACQUIRED             0x02u /* the acquired ID below is valid */
#define CHF_CLOSING_FROM_TRACKING 0x04u /* what CLOSING is closing from */

struct chan {
	/*
	 * The next instant this channel wants the radio, as a t_sync: the
	 * moment the last bit of the on-air address should be at the antenna.
	 * radiant_radio_hal.h defines the term; using anything else here would put
	 * one backend's ramp-up time into every timing constant in the core.
	 *
	 * 64 bits, not 32, and that is load-bearing rather than generous. The
	 * mock's clock deliberately starts one second below 2^32 us, so a
	 * uint32_t here fails inside the first virtual second of the first
	 * test rather than on a dongle left plugged in over a weekend.
	 */
	radiant_time_t t_next;

	/* When the current search began. The deadline is derived from this and
	 * the two timeout bytes rather than stored, so a host that extends a
	 * timeout mid-search gets the extension and not a restart. */
	radiant_time_t t_search_start;

	/* The radio operation currently attributed to this channel, or 0. */
	uint32_t op;

	uint16_t period;            /* 32768 Hz counts */
	uint16_t waveform;
	uint16_t device_number;     /* as configured; 0 is the slave wildcard */
	uint16_t acq_device_number; /* as acquired, wildcards resolved */

	/*
	 * Exponential average of |t_sync - predicted| over clean consecutive
	 * slots, in SIXTEENTHS of a microsecond. See radiant_channel_guard_us().
	 *
	 * Sixteenths rather than microseconds because the quantity being
	 * averaged is about nine of them: at whole-microsecond resolution a
	 * 1/8-weight EWMA cannot represent the difference between 9 and 10, and
	 * every update would round away the thing being measured. 16 x 4095 us
	 * is the range, which is ten times the widest guard there is.
	 */
	uint16_t resid_q4_us;

	/*
	 * The master's own announced clock accuracy in ppm, or 0 for a master
	 * that has announced nothing. See radiant_channel_clock_accuracy_set().
	 *
	 * Two bytes rather than the three-bit wire code, because the core must
	 * not carry a profile's vocabulary: src/profiles/ owns the ladder and
	 * converts, and this module owns the arithmetic. It fits inside the
	 * padding the record already had, so the 72 B budget is unmoved.
	 */
	uint16_t clk_ppm;

	uint8_t state;              /* enum radiant_channel_state */
	uint8_t type;
	uint8_t network;
	uint8_t ext_assign;
	uint8_t device_type;
	uint8_t trans_type;
	uint8_t acq_device_type;
	uint8_t acq_trans_type;
	uint8_t rf_freq;
	uint8_t crc_mode;
	uint8_t tx_power_level;
	uint8_t tx_power_custom;
	uint8_t search_timeout;     /* 2.5 s ticks, high priority */
	uint8_t lp_search_timeout;  /* 2.5 s ticks, low priority */
	uint8_t search_priority;
	uint8_t prox_threshold;
	uint8_t prox_custom;
	uint8_t sharing_cycles;
	uint8_t hop[3];
	uint8_t sdu_mask_config;
	uint8_t miss_count;
	/*
	 * Consecutive slots lost to an arbitrated backend lending the radio to
	 * another stack. Weighted exactly as miss_count in the guard and cleared
	 * wherever it is cleared, but kept apart from it so that a denial never
	 * counts towards RX_FAIL_GO_TO_SEARCH. Always zero on a backend that owns
	 * the radio outright.
	 */
	uint8_t denied_count;
	/*
	 * Clean consecutive slots the estimator above has seen since it was last
	 * reset. Saturates; only the comparison against
	 * RADIANT_CHANNEL_GUARD_LOCK_SLOTS matters. Zero means "no evidence",
	 * and no evidence means the widest guard there is.
	 */
	uint8_t lock_slots;
	uint8_t flags;
};

_Static_assert(sizeof(struct chan) <= 72u,
	       "per-channel record exceeded the 72 B budget; 32 of these are "
	       "the whole reason a 32-channel stack fits");

static struct chan channels[RADIANT_CHANNEL_COUNT];

/* Raised by radiant_sched.c at init from caps.min_arm_lead_us. The protocol floor
 * is the default and is also the minimum a caller can set. */
static uint16_t period_floor = RADIANT_CHANNEL_PERIOD_MIN_COUNTS;

/* The same shape, for the tracked-window guard: raised at init by whoever holds
 * the backend capabilities, never lowered below the core's own minimum. */
static uint16_t guard_floor = RADIANT_CHANNEL_GUARD_MIN_US;

static uint32_t stale_ops;
static uint32_t search_timeouts;

/* ---------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------------
 */

static bool ch_valid(uint8_t channel)
{
	return channel < RADIANT_CHANNEL_COUNT;
}

static struct chan *ch_at(uint8_t channel)
{
	return &channels[channel];
}

static bool state_on_air(uint8_t state)
{
	return state == RADIANT_CH_STATE_SEARCHING ||
	       state == RADIANT_CH_STATE_TRACKING ||
	       state == RADIANT_CH_STATE_CLOSING;
}

static bool state_assigned(uint8_t state)
{
	return state != RADIANT_CH_STATE_UNASSIGNED;
}

static radiant_time_t period_us(const struct chan *c)
{
	return radiant_channel_counts_to_us(c->period);
}

/* ---------------------------------------------------------------------------
 * The window-guard estimator
 *
 * Integer only, and that is a constraint rather than a preference: there is no
 * float anywhere in this module, this code runs inside the radio event path,
 * and a soft-float multiply there would cost more than the window it is trying
 * to save.
 * ---------------------------------------------------------------------------
 */

/* Weight of a new sample in the running average, as a right shift. 1/8, so the
 * average has a memory of roughly eight slots - two seconds at the ANT+ period,
 * long enough to be an average and short enough to follow a master whose clock
 * moves with temperature. */
#define GUARD_EWMA_SHIFT 3u

/* The most the sixteenths field can hold, in whole microseconds. The residual
 * estimator's range is a property of its storage and not of any bound in the
 * protocol, which is why it is stated separately from the ceiling below. */
#define GUARD_RESID_FIELD_MAX_US (65535u / 16u)

/*
 * GUARD_MAX is sixteen worst-case periods, and writing that as arithmetic
 * rather than as the number 400 is what lets an announced clock re-derive it.
 * Eight is RADIANT_CHANNEL_RX_FAIL_TO_SEARCH - the misses a channel is allowed
 * before it gives up - and the other factor of two is the margin the constant
 * was given when it was a constant.
 */
#define GUARD_CEIL_PERIODS \
	(RADIANT_CHANNEL_GUARD_MAX_US / RADIANT_CHANNEL_DRIFT_WORST_US)

_Static_assert(RADIANT_CHANNEL_GUARD_MAX_US ==
		       GUARD_CEIL_PERIODS * RADIANT_CHANNEL_DRIFT_WORST_US,
	       "GUARD_MAX is no longer a whole number of worst-case periods, so "
	       "an announced clock accuracy can no longer re-derive it");
_Static_assert(GUARD_CEIL_PERIODS >= 2u * RADIANT_CHANNEL_RX_FAIL_TO_SEARCH,
	       "the ceiling no longer covers every period a channel may "
	       "extrapolate before it gives up");

/*
 * One period's worth of worst-case disagreement between this master's clock and
 * ours, in microseconds.
 *
 * RADIANT_CHANNEL_DRIFT_WORST_US whenever nothing has been announced, which is
 * every channel in a build that never calls the setter - that identity is what
 * makes the schedule block free when its fields are defaulted. When a master
 * HAS announced, the same quantity is recomputed at its ppm over its own
 * period, and the announcement may only widen: the constant is the floor of
 * this function, because a claim of 20 ppm is a claim and the 25 us is a
 * budget.
 */
static uint32_t drift_worst_us(const struct chan *c)
{
	uint64_t us;
	uint32_t ppm;

	if (c->clk_ppm == 0u) {
		return RADIANT_CHANNEL_DRIFT_WORST_US;
	}

	/* Relative accuracy: theirs plus ours. The receiver's own end of the
	 * budget does not improve because the master admitted to a bad clock. */
	ppm = (uint32_t)c->clk_ppm + RADIANT_CHANNEL_CLOCK_PPM_ANT;
	us = ((uint64_t)period_us(c) * (uint64_t)ppm) / 1000000u;

	if (us <= (uint64_t)RADIANT_CHANNEL_DRIFT_WORST_US) {
		return RADIANT_CHANNEL_DRIFT_WORST_US;
	}
	return (uint32_t)us;
}

/*
 * The widest this channel's guard may get: RADIANT_CHANNEL_GUARD_MAX_US, or the
 * same arithmetic at an announced clock's worse period.
 *
 * Bounded by a quarter period as well, because a window that reached into the
 * neighbouring slot would stop being a window. At every period and every point
 * of the ladder this bound is orders of magnitude away from binding; it is here
 * so that a future ladder entry, or a period floor change, cannot quietly turn
 * a receive window into a continuous scan.
 */
static uint32_t guard_ceiling_us(const struct chan *c)
{
	uint32_t drift = drift_worst_us(c);
	uint64_t ceiling;

	if (drift <= RADIANT_CHANNEL_DRIFT_WORST_US) {
		return RADIANT_CHANNEL_GUARD_MAX_US;
	}

	ceiling = (uint64_t)drift * GUARD_CEIL_PERIODS;
	if (ceiling > (period_us(c) / 4u)) {
		ceiling = period_us(c) / 4u;
	}
	if (ceiling < (uint64_t)RADIANT_CHANNEL_GUARD_MAX_US) {
		ceiling = RADIANT_CHANNEL_GUARD_MAX_US;
	}
	return (uint32_t)ceiling;
}

static void guard_reset(struct chan *c)
{
	c->resid_q4_us = 0u;
	c->lock_slots = 0u;
}

/*
 * Fold one measured residual into the average.
 *
 * `err_us` is |t_sync - predicted| for a slot whose PREVIOUS slot was also
 * heard. Slots after a miss are deliberately not offered here: their residual
 * covers two or more periods of drift, so feeding them in would measure the
 * miss rather than the master's clock, and the guard already charges an
 * extrapolated period at the spec bound instead.
 *
 * The first sample is taken whole rather than averaged in from zero. Ramping up
 * from zero would UNDERESTIMATE for the first several slots, and an
 * underestimate is the direction that loses packets; the lock counter then
 * withholds the result until there are enough samples for it to mean anything.
 */
static void guard_observe(struct chan *c, radiant_time_t err_us)
{
	/*
	 * The clamp is the channel's own ceiling, not the compiled-in one, and
	 * that matters as soon as a master announces a bad clock: an RC-clocked
	 * node at a 2 s heartbeat has a real residual of some hundreds of
	 * microseconds to a millisecond, and an estimator that recorded 400 us
	 * for all of them would be measuring its own clamp. With nothing
	 * announced the ceiling IS RADIANT_CHANNEL_GUARD_MAX_US and this is the
	 * same clamp it always was, to the microsecond.
	 */
	uint32_t cap = guard_ceiling_us(c);
	uint32_t sample;

	if (cap > GUARD_RESID_FIELD_MAX_US) {
		cap = GUARD_RESID_FIELD_MAX_US;
	}
	sample = (err_us > (radiant_time_t)cap) ? (cap * 16u)
						: (uint32_t)(err_us * 16u);

	if (c->lock_slots == 0u) {
		c->resid_q4_us = (uint16_t)sample;
	} else {
		uint32_t avg = c->resid_q4_us;

		avg -= (avg >> GUARD_EWMA_SHIFT);
		avg += (sample >> GUARD_EWMA_SHIFT);
		c->resid_q4_us = (uint16_t)((avg > (cap * 16u)) ? (cap * 16u)
								: avg);
	}

	if (c->lock_slots < 255u) {
		c->lock_slots++;
	}
}

/*
 * Reset one channel's configuration to its power-on defaults.
 *
 * Called from init, from assign and from unassign. Assign resetting the
 * configuration is the part worth arguing for: a channel reassigned to a
 * different type must not inherit the last session's channel ID, because a
 * host that then opens without setting one would silently track the previous
 * sensor instead of being told RADIANT_CH_ERR_ID_NOT_SET. Every host library sets
 * everything after assigning anyway, so nothing is lost.
 */
static void chan_defaults(struct chan *c)
{
	memset(c, 0, sizeof(*c));

	c->state = RADIANT_CH_STATE_UNASSIGNED;
	c->t_next = RADIANT_TIME_NEVER;
	c->t_search_start = RADIANT_TIME_NEVER;

	/*
	 * 8182 counts, the ANT+ 4.005 Hz default, measured here as a
	 * 249,696.4 us mean over 744 intervals. NOT 8192 - see the note in
	 * radiant_channel.h. A default that is 305 us per slot away from what
	 * every real sensor transmits at would let a channel opened without a
	 * period walk out of the master's window in under a minute, and the
	 * symptom would look like a receiver bug.
	 */
	c->period = RADIANT_CHANNEL_PERIOD_ANT_PLUS;

	/*
	 * 66, ANT's power-on default, deliberately NOT the ANT+ frequency.
	 * 57 would be more convenient and is exactly why it is wrong here: a
	 * host that forgot to set the frequency would half-work on the ANT+
	 * band, which is far harder to diagnose than finding nothing at all.
	 */
	c->rf_freq = 66u;

	c->crc_mode = RADIANT_CH_CRC_MODE_DEFAULT;

	/* Level 3 is 0 dBm, the middle of the range and what the bench link
	 * was characterised at. */
	c->tx_power_level = 3u;

	c->search_timeout = RADIANT_CHANNEL_SEARCH_TIMEOUT_DEFAULT;
	c->lp_search_timeout = RADIANT_CHANNEL_LP_SEARCH_TIMEOUT_DEFAULT;
	c->waveform = RADIANT_CH_SEARCH_WAVEFORM_DEFAULT;
	c->sdu_mask_config = RADIANT_CH_SDU_MASK_OFF;
}

/*
 * Leave the air.
 *
 * The single place a channel stops being open, so the CHANNEL_CLOSED event has
 * exactly one origin. A host library that reopens on that event hangs if it is
 * ever missed and double-closes if it is ever duplicated, and both failures
 * are invisible in a build log.
 */
static void chan_finish_close(uint8_t channel, struct chan *c)
{
	c->state = RADIANT_CH_STATE_ASSIGNED;
	c->op = 0u;
	c->t_next = RADIANT_TIME_NEVER;
	c->t_search_start = RADIANT_TIME_NEVER;
	c->miss_count = 0u;
	c->denied_count = 0u;
	c->flags &= (uint8_t)~(CHF_ACQUIRED | CHF_CLOSING_FROM_TRACKING);

	radiant_channel_event_out(channel, RADIANT_CH_EVENT_CHANNEL_CLOSED);
}

/*
 * Begin leaving the air. Completes immediately when no radio operation is
 * bound, and parks in CLOSING when one is - the terminal event that the HAL
 * promises will still arrive is what finishes the job.
 */
static void chan_begin_close(uint8_t channel, struct chan *c)
{
	if (c->state == RADIANT_CH_STATE_TRACKING) {
		c->flags |= CHF_CLOSING_FROM_TRACKING;
	} else {
		c->flags &= (uint8_t)~CHF_CLOSING_FROM_TRACKING;
	}

	if (c->op != 0u) {
		c->state = RADIANT_CH_STATE_CLOSING;
		return;
	}

	chan_finish_close(channel, c);
}

/* The absolute instant a searching channel gives up, or RADIANT_TIME_NEVER.
 *
 * Low priority runs after high priority expires, so the budget is the sum.
 * Either byte set to 255 makes the whole search infinite. Both zero makes the
 * deadline the instant the search began, so the next tick closes the channel -
 * which is the honest reading of "0 disables high-priority search" combined
 * with a low-priority search that is also disabled, and is better than a
 * channel that sits in SEARCHING forever with both timeouts off. */
/*
 * MESG_ASSIGN_CHANNEL's extended-assignment "always search" bit, named in
 * protocol/ant_wire.yaml as ANTW_EXT_PARAM_ALWAYS_SEARCH. This file is below
 * the wire layer and does not include ant_wire.h, so the value is restated here
 * the same way radiant_api.c restates it as API_EXT_ASSIGN_BACKGROUND_SCAN -
 * the byte reaches both of them through radiant_channel_assign() unchanged.
 */
#define CH_EXT_ASSIGN_BACKGROUND_SCAN 0x01u

static radiant_time_t search_deadline(const struct chan *c)
{
	radiant_time_t budget;

	if (c->t_search_start == RADIANT_TIME_NEVER) {
		return RADIANT_TIME_NEVER;
	}

	/*
	 * A BACKGROUND SCAN HAS NO DEADLINE, WHATEVER THE TIMEOUT BYTES SAY.
	 *
	 * ANTW_EXT_PARAM_ALWAYS_SEARCH is not a hint - "always search" is the
	 * entire meaning of the extended assignment bit, and a channel carrying
	 * it is a discovery channel the host intends to leave open for as long
	 * as it wants to keep finding sensors. Nothing ever acquires on it (see
	 * api_search_acquired()'s scan-mode branch), so the search-timeout
	 * machinery below cannot mean for it what it means everywhere else: a
	 * deadline that exists to stop a channel hunting forever for one device
	 * that is not there.
	 *
	 * Found on the bench, with the search counters logged once a second: a
	 * scan channel opened with the DEFAULT timeouts - which is what a host
	 * gets if it does not send MESG_SEARCH_TIMEOUT at all - reported
	 * devices normally and then stopped dead, permanently, at exactly 25 s.
	 * radiant_search_n_searching() read 0 from that moment on: the channel
	 * had timed out, raised RX_SEARCH_TIMEOUT and closed, and the host was
	 * left holding a discovery channel that would never discover anything
	 * again. It looks exactly like a dongle that "stops finding sensors
	 * after a while".
	 */
	if ((c->ext_assign & CH_EXT_ASSIGN_BACKGROUND_SCAN) != 0u) {
		return RADIANT_TIME_NEVER;
	}

	if (c->search_timeout == RADIANT_CHANNEL_SEARCH_INFINITE ||
	    c->lp_search_timeout == RADIANT_CHANNEL_SEARCH_INFINITE) {
		return RADIANT_TIME_NEVER;
	}

	budget = radiant_channel_search_ticks_to_us(c->search_timeout) +
		 radiant_channel_search_ticks_to_us(c->lp_search_timeout);

	return c->t_search_start + budget;
}

/* ---------------------------------------------------------------------------
 * Module lifecycle
 * ---------------------------------------------------------------------------
 */

void radiant_channel_init(void)
{
	uint8_t i;

	for (i = 0u; i < RADIANT_CHANNEL_COUNT; i++) {
		chan_defaults(&channels[i]);
	}

	period_floor = RADIANT_CHANNEL_PERIOD_MIN_COUNTS;
	/* Back to the core's own minimum, not to whatever the last backend
	 * raised it to. antr_init() calls this before the radio is enabled and
	 * sets the floor from caps afterwards, so a stale value here would be a
	 * previous backend's opinion applied to this one. */
	guard_floor = RADIANT_CHANNEL_GUARD_MIN_US;
	stale_ops = 0u;
	search_timeouts = 0u;
}

void radiant_channel_reset_all(void)
{
	uint8_t i;

	for (i = 0u; i < RADIANT_CHANNEL_COUNT; i++) {
		struct chan *c = &channels[i];

		/*
		 * Raise CHANNEL_CLOSED for anything that was on air, including
		 * a channel already in CLOSING - the host asked for that event
		 * and a reset must not swallow it. The contract says these are
		 * delivered and that the bridge discards them for a short
		 * window afterwards, because a real stick emits nothing
		 * between the reset command and the startup message. Deciding
		 * that here instead would make the backend's behaviour depend
		 * on a bridge-side window it cannot see.
		 */
		if (state_on_air(c->state)) {
			radiant_channel_event_out(i, RADIANT_CH_EVENT_CHANNEL_CLOSED);
		}

		chan_defaults(c);
	}

	/*
	 * Operations still in flight are orphaned rather than waited on: this
	 * module cannot abort one. radiant_sched.c calls radiant_radio_abort() before
	 * this and feeds the terminal event in afterwards, where it lands on
	 * no binding and is counted as stale - the same path a late event
	 * takes, which is exactly the point of having only one.
	 */
}

void radiant_channel_period_floor_set(uint16_t counts)
{
	if (counts > RADIANT_CHANNEL_PERIOD_MIN_COUNTS) {
		period_floor = counts;
	} else {
		period_floor = RADIANT_CHANNEL_PERIOD_MIN_COUNTS;
	}
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

radiant_channel_err_t radiant_channel_assign(uint8_t channel, uint8_t type,
				     uint8_t network, uint8_t ext_assign)
{
	struct chan *c;

	if (!ch_valid(channel)) {
		return RADIANT_CH_ERR_INVALID_MESSAGE;
	}

	c = ch_at(channel);
	if (state_assigned(c->state)) {
		return RADIANT_CH_ERR_WRONG_STATE;
	}

	if (network >= RADIANT_CHANNEL_NETWORK_COUNT) {
		return RADIANT_CH_ERR_INVALID_NETWORK;
	}

	/*
	 * `type` is stored without validation, and that is a reading of the
	 * contract rather than an omission. antr_channel_assign()'s permitted
	 * set is {NO_ERROR, INVALID_MESSAGE, CHANNEL_IN_WRONG_STATE,
	 * INVALID_NETWORK_NUMBER}: INVALID_MESSAGE is already spoken for by
	 * the channel number and none of the other three describes a bad type.
	 * Returning INVALID_PARAMETER_PROVIDED would put a byte on the wire
	 * that the contract says this function never produces, which is worse
	 * than accepting a type nothing will ever send. Only bit 4 is
	 * interpreted; the rest is carried and reported back in the status
	 * byte.
	 */
	chan_defaults(c);
	c->state = RADIANT_CH_STATE_ASSIGNED;
	c->type = type;
	c->network = network;
	c->ext_assign = ext_assign;

	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_unassign(uint8_t channel)
{
	struct chan *c;

	if (!ch_valid(channel)) {
		return RADIANT_CH_ERR_INVALID_MESSAGE;
	}

	c = ch_at(channel);

	/*
	 * Refuses a channel that is open OR closing, and never closes one on
	 * the caller's behalf. Silently closing first would swallow the
	 * CHANNEL_CLOSED event the host is waiting for; refusing a CLOSING
	 * channel is the same argument one step later, and is why
	 * radiant_channel_status_get() keeps reporting SEARCHING/TRACKING until
	 * the close completes rather than reporting ASSIGNED and inviting the
	 * call this rejects.
	 */
	if (c->state != RADIANT_CH_STATE_ASSIGNED) {
		return RADIANT_CH_ERR_WRONG_STATE;
	}

	chan_defaults(c);

	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_open(uint8_t channel, uint16_t offset,
				   radiant_time_t now)
{
	struct chan *c;
	radiant_time_t start;

	if (!ch_valid(channel)) {
		return RADIANT_CH_ERR_INVALID_MESSAGE;
	}

	c = ch_at(channel);
	if (c->state != RADIANT_CH_STATE_ASSIGNED) {
		return RADIANT_CH_ERR_WRONG_STATE;
	}
	if ((c->flags & CHF_ID_SET) == 0u) {
		return RADIANT_CH_ERR_ID_NOT_SET;
	}

	start = now + radiant_channel_counts_to_us(offset);

	if ((c->type & RADIANT_CH_TYPE_MASTER_BIT) != 0u) {
		/*
		 * A MASTER'S FIRST TRANSMISSION IS ONE FULL CHANNEL PERIOD
		 * AFTER THIS CALL, NOT ZERO.
		 *
		 * Measured, 8 of 8 runs: MESG_OPEN_CHANNEL to the first
		 * EVENT_TX was 250, 250, 250, 250, 250, 265, 266, 266 ms, with
		 * the 16 ms quantisation coming from the Windows clock rather
		 * than the radio (docs/ant-radio-link.md, "Master open-time").
		 * Whether the period is a collision probe or slot alignment is
		 * not observable from the air - a sniffer cannot see a receive
		 * window - so this reproduces the behaviour without claiming
		 * the mechanism.
		 *
		 * `offset` is added ON TOP of the period rather than replacing
		 * it. src/ant_radio.h describes it as the "delay before the
		 * channel's first slot", which alone would make offset 0 mean
		 * "transmit as soon as possible" and contradict the
		 * measurement. Read as a phase adjustment to the slot the
		 * channel would have taken anyway, both facts hold, and that
		 * is also the only reading under which offset does what it is
		 * for - phasing one master's slot away from another's.
		 */
		c->t_next = start + period_us(c);
		c->state = RADIANT_CH_STATE_TRACKING;
		c->t_search_start = RADIANT_TIME_NEVER;
	} else {
		/*
		 * A slave enters search with its first window at `start`.
		 * There is no period wait: a searching slave has nothing to
		 * align to yet, and the wildcard sweep's dwell is one channel
		 * period from the first window onwards, not from the second.
		 */
		c->t_next = start;
		c->state = RADIANT_CH_STATE_SEARCHING;
		c->t_search_start = start;
	}

	c->miss_count = 0u;
	c->denied_count = 0u;
	c->flags &= (uint8_t)~(CHF_ACQUIRED | CHF_CLOSING_FROM_TRACKING);

	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_close(uint8_t channel, radiant_time_t now)
{
	struct chan *c;

	(void)now; /* Closing is not scheduled: the channel stops wanting the
		    * radio immediately and the only remaining question is when
		    * the operation in flight ends, which is the terminal
		    * event's business rather than the clock's. The parameter
		    * stays because every other lifecycle call takes it and an
		    * asymmetric one invites a caller to look for a reason. */

	if (!ch_valid(channel)) {
		return RADIANT_CH_ERR_INVALID_MESSAGE;
	}

	c = ch_at(channel);

	/*
	 * A second close on a CLOSING channel is WRONG_STATE, not a second
	 * event. The host is already owed exactly one CHANNEL_CLOSED and
	 * raising a second would make a library that reopens on the event
	 * reopen twice.
	 */
	if (c->state != RADIANT_CH_STATE_SEARCHING &&
	    c->state != RADIANT_CH_STATE_TRACKING) {
		return RADIANT_CH_ERR_WRONG_STATE;
	}

	chan_begin_close(channel, c);

	return RADIANT_CH_OK;
}

/* ---------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------------
 */

/* The precondition shared by everything that retunes the radio: assigned, and
 * not on air. Retuning underneath a live host session is worse than refusing,
 * and every retail stick refuses. */
static radiant_channel_err_t require_assigned_closed(uint8_t channel,
						 struct chan **out)
{
	struct chan *c;

	if (!ch_valid(channel)) {
		return RADIANT_CH_ERR_INVALID_MESSAGE;
	}

	c = ch_at(channel);
	if (c->state != RADIANT_CH_STATE_ASSIGNED) {
		return RADIANT_CH_ERR_WRONG_STATE;
	}

	*out = c;
	return RADIANT_CH_OK;
}

/* The weaker precondition, for settings that change no radio parameter: the
 * channel must exist, but it may be on air. */
static radiant_channel_err_t require_assigned(uint8_t channel, struct chan **out)
{
	struct chan *c;

	if (!ch_valid(channel)) {
		return RADIANT_CH_ERR_INVALID_MESSAGE;
	}

	c = ch_at(channel);
	if (!state_assigned(c->state)) {
		return RADIANT_CH_ERR_WRONG_STATE;
	}

	*out = c;
	return RADIANT_CH_OK;
}

/* Readback preconditions are weaker still: a getter answers for any channel in
 * range, including an unassigned one, because "unassigned" is an answer. */
static radiant_channel_err_t require_range(uint8_t channel, struct chan **out)
{
	if (!ch_valid(channel)) {
		return RADIANT_CH_ERR_INVALID_MESSAGE;
	}

	*out = ch_at(channel);
	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_id_set(uint8_t channel, uint16_t device_number,
				     uint8_t device_type, uint8_t trans_type)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_assigned_closed(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}

	c->device_number = device_number;
	c->device_type = device_type;
	c->trans_type = trans_type;
	c->flags |= CHF_ID_SET;
	/* A new configured ID invalidates anything a previous session
	 * acquired; the two must never be readable at the same time. */
	c->flags &= (uint8_t)~CHF_ACQUIRED;

	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_id_get(uint8_t channel,
				     struct radiant_channel_id *out)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_range(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}
	if (out == NULL) {
		return RADIANT_CH_ERR_INVALID_PARAM;
	}

	if ((c->flags & CHF_ACQUIRED) != 0u) {
		out->device_number = c->acq_device_number;
		out->device_type = c->acq_device_type;
		out->trans_type = c->acq_trans_type;
	} else {
		out->device_number = c->device_number;
		out->device_type = c->device_type;
		out->trans_type = c->trans_type;
	}

	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_period_set(uint8_t channel, uint16_t period)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_assigned_closed(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}

	/*
	 * The floor is a schedulability bound, not a taste: a slave answers
	 * its master 2.19 ms after the address and an acknowledged exchange
	 * costs ~1.55 ms each way, so a period below about 8 ms cannot contain
	 * one slot's traffic. Accepting it would be promising a schedule the
	 * radio can never keep, and the host would see a channel that opens
	 * and then loses every slot.
	 */
	if (period < period_floor) {
		return RADIANT_CH_ERR_INVALID_PARAM;
	}

	c->period = period;

	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_period_get(uint8_t channel, uint16_t *out)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_range(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}
	if (out == NULL) {
		return RADIANT_CH_ERR_INVALID_PARAM;
	}

	*out = c->period;
	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_rf_freq_set(uint8_t channel, uint8_t freq)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_assigned_closed(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}
	if (freq > RADIANT_RF_INDEX_MAX) {
		return RADIANT_CH_ERR_INVALID_PARAM;
	}

	c->rf_freq = freq;
	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_rf_freq_get(uint8_t channel, uint8_t *out)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_range(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}
	if (out == NULL) {
		return RADIANT_CH_ERR_INVALID_PARAM;
	}

	*out = c->rf_freq;
	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_tx_power_set(uint8_t channel, uint8_t level,
					   uint8_t custom)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_assigned(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}

	/*
	 * Bit 7 means `custom` names a raw part-specific register value, so
	 * the level is not a level at all and there is nothing to range-check.
	 * With the bit clear, 0..5 are -20, -12, -4, 0, +4 and +8 dBm. Level 5
	 * is accepted even on a part that cannot produce +8: whether the
	 * hardware has it is a caps question, and radiant_sched.c clamps against
	 * caps.tx_power_max_dbm when it arms. Refusing here would make the
	 * same host message succeed or fail depending on the part, for a
	 * setting the HAL explicitly says a backend rounds rather than fails.
	 */
	if ((level & 0x80u) == 0u && level > 5u) {
		return RADIANT_CH_ERR_INVALID_PARAM;
	}

	c->tx_power_level = level;
	c->tx_power_custom = custom;
	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_tx_power_get(uint8_t channel, uint8_t *level,
					   uint8_t *custom)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_range(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}
	if (level == NULL || custom == NULL) {
		return RADIANT_CH_ERR_INVALID_PARAM;
	}

	*level = c->tx_power_level;
	*custom = c->tx_power_custom;
	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_search_timeout_set(uint8_t channel,
						 uint8_t timeout)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_assigned(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}

	/* Every value 0..255 is meaningful: 0 disables this priority's search,
	 * 255 makes it infinite, and everything between is 2.5 s per count.
	 * There is nothing to reject, which is why the contract lists no
	 * INVALID_PARAMETER_PROVIDED for this call. */
	c->search_timeout = timeout;
	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_lp_search_timeout_set(uint8_t channel,
						    uint8_t timeout)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_assigned(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}

	c->lp_search_timeout = timeout;
	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_crc_mode_set(uint8_t channel, uint8_t mode)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_assigned_closed(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}

	/*
	 * The supported set is exactly {default}. A backend that accepted a
	 * shorter CRC and quietly kept using the default would give the host a
	 * link that works and framing that does not - the host believes it
	 * configured something it did not, and the failure shows up as
	 * corruption rather than as a refusal.
	 */
	if (mode != RADIANT_CH_CRC_MODE_DEFAULT) {
		return RADIANT_CH_ERR_INVALID_PARAM;
	}

	c->crc_mode = mode;
	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_crc_mode_get(uint8_t channel, uint8_t *out)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_range(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}
	if (out == NULL) {
		return RADIANT_CH_ERR_INVALID_PARAM;
	}

	*out = c->crc_mode;
	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_freq_hop_table_set(uint8_t channel, uint8_t freq0,
						 uint8_t freq1, uint8_t freq2)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_assigned_closed(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}
	if (freq0 > RADIANT_RF_INDEX_MAX || freq1 > RADIANT_RF_INDEX_MAX ||
	    freq2 > RADIANT_RF_INDEX_MAX) {
		return RADIANT_CH_ERR_INVALID_PARAM;
	}

	c->hop[0] = freq0;
	c->hop[1] = freq1;
	c->hop[2] = freq2;
	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_freq_hop_table_get(uint8_t channel,
						 uint8_t out[3])
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_range(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}
	if (out == NULL) {
		return RADIANT_CH_ERR_INVALID_PARAM;
	}

	out[0] = c->hop[0];
	out[1] = c->hop[1];
	out[2] = c->hop[2];
	return RADIANT_CH_OK;
}

/* ---------------------------------------------------------------------------
 * Search configuration
 * ---------------------------------------------------------------------------
 */

radiant_channel_err_t radiant_channel_search_waveform_set(uint8_t channel,
						  uint16_t waveform)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_assigned(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}

	/*
	 * The value is stored and never acted on - see the header. Refusing
	 * anything but the two values sdk-ant names is what keeps ignoring it
	 * an honest contract rather than a silent one: a host that set a
	 * "custom" waveform sdk-ant itself says not to use would otherwise
	 * believe it took effect.
	 */
	if (waveform != (uint16_t)RADIANT_CH_SEARCH_WAVEFORM_DEFAULT &&
	    waveform != (uint16_t)RADIANT_CH_SEARCH_WAVEFORM_FAST) {
		return RADIANT_CH_ERR_INVALID_PARAM;
	}

	c->waveform = waveform;
	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_search_waveform_get(uint8_t channel,
						  uint16_t *out)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_range(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}
	if (out == NULL) {
		return RADIANT_CH_ERR_INVALID_PARAM;
	}

	*out = c->waveform;
	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_prox_search_set(uint8_t channel,
					      uint8_t threshold, uint8_t custom)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_assigned(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}
	if (threshold > RADIANT_CH_PROX_THRESHOLD_MAX) {
		return RADIANT_CH_ERR_INVALID_PARAM;
	}
	/*
	 * PERMANENT LIMITATION - see the header. Nothing reads prox_threshold
	 * back, so storing a non-zero value and reporting success would
	 * advertise a filter that never runs. 0 is accepted because it asks
	 * for exactly the behaviour this stack already has (no RSSI gate).
	 */
	if (threshold != 0u) {
		return RADIANT_CH_ERR_INVALID_PARAM;
	}

	c->prox_threshold = threshold;
	c->prox_custom = custom;
	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_prox_search_get(uint8_t channel,
					      uint8_t *threshold,
					      uint8_t *custom)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_range(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}
	if (threshold == NULL || custom == NULL) {
		return RADIANT_CH_ERR_INVALID_PARAM;
	}

	*threshold = c->prox_threshold;
	*custom = c->prox_custom;
	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_search_priority_set(uint8_t channel,
						  uint8_t priority)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_assigned(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}

	/* Any non-zero value is high priority. The contract documents 0 and 1
	 * and lists no INVALID_PARAMETER_PROVIDED, so a 2 is normalised rather
	 * than refused with a code this call is not allowed to return. */
	c->search_priority = (priority != 0u) ? 1u : 0u;
	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_search_priority_get(uint8_t channel,
						  uint8_t *out)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_range(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}
	if (out == NULL) {
		return RADIANT_CH_ERR_INVALID_PARAM;
	}

	*out = c->search_priority;
	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_sharing_cycles_set(uint8_t channel,
						 uint8_t cycles)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_assigned(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}

	c->sharing_cycles = cycles;
	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_sharing_cycles_get(uint8_t channel, uint8_t *out)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_range(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}
	if (out == NULL) {
		return RADIANT_CH_ERR_INVALID_PARAM;
	}

	*out = c->sharing_cycles;
	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_sdu_mask_config_set(uint8_t channel,
						  uint8_t mask_config)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_range(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}

	c->sdu_mask_config = mask_config;
	return RADIANT_CH_OK;
}

radiant_channel_err_t radiant_channel_sdu_mask_config_get(uint8_t channel,
						  uint8_t *out)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_range(channel, &c);

	if (err != RADIANT_CH_OK) {
		return err;
	}
	if (out == NULL) {
		return RADIANT_CH_ERR_INVALID_PARAM;
	}

	*out = c->sdu_mask_config;
	return RADIANT_CH_OK;
}

/* ---------------------------------------------------------------------------
 * Status
 * ---------------------------------------------------------------------------
 */

/* The low two bits of the status byte. CLOSING reports what it is closing
 * from, because the host has not yet been told the channel closed and a status
 * byte that says ASSIGNED would invite an unassign that radiant_channel_unassign()
 * then refuses. */
static uint8_t status_state_bits(const struct chan *c)
{
	switch (c->state) {
	case RADIANT_CH_STATE_UNASSIGNED:
		return RADIANT_CH_STATUS_UNASSIGNED;
	case RADIANT_CH_STATE_ASSIGNED:
		return RADIANT_CH_STATUS_ASSIGNED;
	case RADIANT_CH_STATE_SEARCHING:
		return RADIANT_CH_STATUS_SEARCHING;
	case RADIANT_CH_STATE_TRACKING:
		return RADIANT_CH_STATUS_TRACKING;
	case RADIANT_CH_STATE_CLOSING:
		return ((c->flags & CHF_CLOSING_FROM_TRACKING) != 0u)
			       ? RADIANT_CH_STATUS_TRACKING
			       : RADIANT_CH_STATUS_SEARCHING;
	default:
		return RADIANT_CH_STATUS_UNASSIGNED;
	}
}

radiant_channel_err_t radiant_channel_status_get(uint8_t channel, uint8_t *out)
{
	struct chan *c = NULL;
	radiant_channel_err_t err = require_range(channel, &c);
	uint8_t status;

	if (err != RADIANT_CH_OK) {
		return err;
	}
	if (out == NULL) {
		return RADIANT_CH_ERR_INVALID_PARAM;
	}

	/* Rev 5.1 section 9.5.7.1: state in bits 1:0, network in bits 3:2,
	 * channel type in bits 7:4. The type values already sit in the top
	 * nibble, so they are masked across rather than shifted; shifting them
	 * would report a channel type of zero on every live master and the
	 * only symptom would be a host that refuses to pair. */
	status = (uint8_t)(c->type & RADIANT_CH_STATUS_TYPE_MASK);
	status |= (uint8_t)(((uint8_t)(c->network << RADIANT_CH_STATUS_NETWORK_SHIFT)) &
			    RADIANT_CH_STATUS_NETWORK_MASK);
	status |= (uint8_t)(status_state_bits(c) & RADIANT_CH_STATUS_STATE_MASK);

	*out = status;
	return RADIANT_CH_OK;
}

enum radiant_channel_state radiant_channel_state_get(uint8_t channel)
{
	if (!ch_valid(channel)) {
		return RADIANT_CH_STATE_UNASSIGNED;
	}

	return (enum radiant_channel_state)channels[channel].state;
}

bool radiant_channel_is_master(uint8_t channel)
{
	const struct chan *c;

	if (!ch_valid(channel)) {
		return false;
	}

	c = &channels[channel];
	if (!state_assigned(c->state)) {
		return false;
	}

	return (c->type & RADIANT_CH_TYPE_MASTER_BIT) != 0u;
}

bool radiant_channel_is_open(uint8_t channel)
{
	if (!ch_valid(channel)) {
		return false;
	}

	return state_on_air(channels[channel].state);
}

uint8_t radiant_channel_type_get(uint8_t channel)
{
	return ch_valid(channel) ? channels[channel].type : 0u;
}

uint8_t radiant_channel_ext_assign_get(uint8_t channel)
{
	return ch_valid(channel) ? channels[channel].ext_assign : 0u;
}

uint8_t radiant_channel_network_get(uint8_t channel)
{
	return ch_valid(channel) ? channels[channel].network : 0u;
}

/* ---------------------------------------------------------------------------
 * The scheduler seam
 * ---------------------------------------------------------------------------
 */

radiant_time_t radiant_channel_next_slot(uint8_t channel)
{
	const struct chan *c;

	if (!ch_valid(channel)) {
		return RADIANT_TIME_NEVER;
	}

	c = &channels[channel];

	/*
	 * A CLOSING channel returns RADIANT_TIME_NEVER even though its clock still
	 * holds a time. It has stopped wanting the radio, and a scheduler that
	 * armed one more window for it would produce exactly the second
	 * in-flight operation that makes the close race hard.
	 */
	if (c->state != RADIANT_CH_STATE_SEARCHING &&
	    c->state != RADIANT_CH_STATE_TRACKING) {
		return RADIANT_TIME_NEVER;
	}

	return c->t_next;
}

uint32_t radiant_channel_guard_us(uint8_t channel)
{
	const struct chan *c;
	uint32_t guard;
	uint32_t floor_us;
	uint32_t drift;

	/*
	 * Every early return here is the channel's CEILING and not the floor,
	 * because every one of them means "no evidence". An out-of-range
	 * channel, one that is not tracking, and one whose estimator has not
	 * locked are all cases where the honest answer is the widest window this
	 * channel has - which is RADIANT_CHANNEL_GUARD_MAX_US exactly, unless
	 * the master has announced a clock that needs more. The unlocked case is
	 * the one that matters: an RC-clocked master is outside the old ceiling
	 * on its very first extrapolated slot, so a receiver that waited for the
	 * estimator to lock would wait for a lock that can never happen.
	 */
	if (!ch_valid(channel)) {
		return RADIANT_CHANNEL_GUARD_MAX_US;
	}

	c = &channels[channel];
	if (c->state != RADIANT_CH_STATE_TRACKING ||
	    c->lock_slots < RADIANT_CHANNEL_GUARD_LOCK_SLOTS) {
		return guard_ceiling_us(c);
	}

	/* Sixteenths back to microseconds. The multiply is done first so the
	 * safety factor is applied at the estimator's resolution rather than to
	 * an already-rounded number. */
	guard = ((uint32_t)c->resid_q4_us * RADIANT_CHANNEL_GUARD_K) / 16u;

	/*
	 * THE FLOOR IS APPLIED BEFORE THE MISS TERM, NOT AFTER, AND THE
	 * ORDERING IS THE SAFETY ARGUMENT.
	 *
	 * The guard must cover the period being received plus every period
	 * extrapolated since the last fresh sync: N misses means N+1 periods of
	 * possible disagreement, up to N = RX_FAIL_TO_SEARCH - 1 before the
	 * channel gives up. Adding after the clamp charges each extrapolated
	 * period on top of a floor that already covers the received one, which
	 * is the arithmetic that holds.
	 *
	 * Clamping afterwards instead - max(floor, k*avg + N*drift) - reads
	 * almost identically and is quietly weaker: with a well-behaved master
	 * the whole miss term disappears inside a floor that was sized for one
	 * period, and by seven misses the window covers 175 us of a 200 us
	 * worst case. The shortfall would appear only on a link that was
	 * already losing slots, as slightly worse recovery, with no error code
	 * anywhere.
	 */
	drift = drift_worst_us(c);

	/*
	 * The floor covers the period being RECEIVED, so an announced clock
	 * raises it: at 500 ppm and a 2 s heartbeat one period alone is 1.1 ms,
	 * and a 100 us floor under a 1.1 ms error is not a floor. With nothing
	 * announced, drift is the 25 us constant, the floor already exceeds it,
	 * and this line changes nothing - which is the whole compatibility
	 * claim, in one comparison.
	 */
	floor_us = guard_floor;
	if (floor_us < drift) {
		floor_us = drift;
	}
	if (guard < floor_us) {
		guard = floor_us;
	}
	/*
	 * TWO COUNTERS, ONE WEIGHT. A period extrapolated with no fresh sync is
	 * a period of unmeasured disagreement whether we chose not to listen or
	 * were not given the air, so a denial is charged at exactly the same
	 * rate as a miss. The counters are separate only because one of them is
	 * evidence about the master and the other is evidence about us; the
	 * arithmetic cannot tell them apart and must not try.
	 *
	 * Their sum is bounded: RX_FAIL_TO_SEARCH (8) and DENY_TO_SEARCH (16)
	 * each promote to SEARCHING before their own term can exceed the
	 * ceiling, and the clamp below covers any interleaving of the two.
	 */
	guard += (uint32_t)(c->miss_count + c->denied_count) * drift;

	if (guard > guard_ceiling_us(c)) {
		guard = guard_ceiling_us(c);
	}
	return guard;
}

void radiant_channel_clock_accuracy_set(uint8_t channel, uint16_t ppm)
{
	if (!ch_valid(channel)) {
		return;
	}

	/*
	 * Clamped, not refused. An announcement worse than the clamp is either a
	 * typo or hostile, and in both cases the widest window this core will
	 * build is the safe answer - it costs receive current on a channel that
	 * announced it needs it, and it cannot cost packets.
	 */
	channels[channel].clk_ppm = (ppm > RADIANT_CHANNEL_CLOCK_PPM_MAX)
					    ? (uint16_t)RADIANT_CHANNEL_CLOCK_PPM_MAX
					    : ppm;
}

uint16_t radiant_channel_clock_accuracy_get(uint8_t channel)
{
	if (!ch_valid(channel)) {
		return 0u;
	}
	return channels[channel].clk_ppm;
}

void radiant_channel_guard_floor_set(uint16_t us)
{
	if (us < RADIANT_CHANNEL_GUARD_MIN_US) {
		return;
	}
	guard_floor = (us > RADIANT_CHANNEL_GUARD_MAX_US)
			      ? (uint16_t)RADIANT_CHANNEL_GUARD_MAX_US
			      : us;
}

uint16_t radiant_channel_residual_us(uint8_t channel)
{
	const struct chan *c;

	if (!ch_valid(channel)) {
		return 0u;
	}

	c = &channels[channel];
	if (c->lock_slots < RADIANT_CHANNEL_GUARD_LOCK_SLOTS) {
		return 0u;
	}
	return (uint16_t)(c->resid_q4_us / 16u);
}

radiant_time_t radiant_channel_search_deadline(uint8_t channel)
{
	const struct chan *c;

	if (!ch_valid(channel)) {
		return RADIANT_TIME_NEVER;
	}

	c = &channels[channel];
	if (c->state != RADIANT_CH_STATE_SEARCHING) {
		return RADIANT_TIME_NEVER;
	}

	return search_deadline(c);
}

radiant_time_t radiant_channel_earliest_slot(uint8_t *channel_out)
{
	radiant_time_t best = RADIANT_TIME_NEVER;
	uint8_t best_ch = 0u;
	bool found = false;
	uint8_t i;

	for (i = 0u; i < RADIANT_CHANNEL_COUNT; i++) {
		radiant_time_t t = radiant_channel_next_slot(i);

		if (t == RADIANT_TIME_NEVER) {
			continue;
		}
		if (!found || t < best) {
			best = t;
			best_ch = i;
			found = true;
		}
	}

	if (found && channel_out != NULL) {
		*channel_out = best_ch;
	}

	return found ? best : RADIANT_TIME_NEVER;
}

void radiant_channel_bind_op(uint8_t channel, uint32_t op)
{
	if (!ch_valid(channel)) {
		return;
	}

	channels[channel].op = op;
}

int radiant_channel_op_owner(uint32_t op)
{
	uint8_t i;

	/* op 0 is not an operation - radiant_radio_tx()/rx() only ever hand out
	 * non-zero ids - so it must never match the cleared binding of an idle
	 * channel. */
	if (op == 0u) {
		return -1;
	}

	for (i = 0u; i < RADIANT_CHANNEL_COUNT; i++) {
		if (channels[i].op == op) {
			return (int)i;
		}
	}

	return -1;
}

void radiant_channel_on_slot(uint8_t channel, radiant_time_t t_sync)
{
	struct chan *c;

	if (!ch_valid(channel)) {
		return;
	}

	c = &channels[channel];
	if (c->state != RADIANT_CH_STATE_TRACKING) {
		return;
	}

	/*
	 * The residual, measured before the re-anchor throws away the
	 * prediction it is measured against. This is the only place in the core
	 * where a fresh sync and the instant that was predicted for it both
	 * exist, which is why the estimator lives here rather than anywhere it
	 * would read better.
	 *
	 * Only on a slot whose predecessor was heard (miss_count == 0). After a
	 * miss the prediction has been extrapolated one or more extra periods,
	 * so the difference is the drift over all of them; averaging that in
	 * would size a window from how lossy the link is rather than from how
	 * good the master's clock is.
	 *
	 * A DENIAL EXTRAPOLATES THE PREDICTION IDENTICALLY, so it must gate the
	 * estimator identically. This is the one place where "a denial is not
	 * evidence about the peer" would give the wrong answer if applied
	 * literally: the residual measured here is not evidence about the peer
	 * either, it is the accumulated error of OUR extrapolation, and after a
	 * run of denials it is the sum over all of them. Feeding that in would
	 * hand an EWMA with a memory of eight slots a sample of up to
	 * DENY_TO_SEARCH + 1 periods of drift, which clamps at
	 * RADIANT_CHANNEL_GUARD_MAX_US and then decays back over roughly eight
	 * further slots - so the first slot heard after a busy patch in the
	 * other stack would pin the window wide for two seconds afterwards, on a
	 * link that is behaving perfectly.
	 */
	if (c->miss_count == 0u && c->denied_count == 0u &&
	    c->t_next != RADIANT_TIME_NEVER) {
		guard_observe(c, (t_sync > c->t_next) ? (t_sync - c->t_next)
						     : (c->t_next - t_sync));
	}

	/*
	 * Re-anchor on the frame that was actually on the air, not on the time
	 * the channel predicted. That is what keeps a slave's window centred
	 * on the master it hears: a constant prediction error would otherwise
	 * accumulate, and the failure mode is silent - the drift estimate
	 * still locks, the window just slides until yield falls at the same
	 * order as the bench's ~0.4% collision floor.
	 */
	c->t_next = t_sync + period_us(c);
	c->miss_count = 0u;
	/* A slot heard is a fresh sync, and a fresh sync retires every period of
	 * extrapolation behind it - whichever counter charged it. */
	c->denied_count = 0u;
}

void radiant_channel_on_acquired(uint8_t channel, const struct radiant_channel_id *id,
			     radiant_time_t t_sync)
{
	struct chan *c;

	if (!ch_valid(channel) || id == NULL) {
		return;
	}

	c = &channels[channel];
	if (c->state != RADIANT_CH_STATE_SEARCHING) {
		return;
	}

	/* The wildcards resolve here and nowhere else. antr_channel_id_get()
	 * reporting the acquired ID is how a host learns which sensor it
	 * found, and the configured ID is kept untouched so that closing and
	 * reopening searches again instead of silently narrowing to the last
	 * sensor. */
	c->acq_device_number = id->device_number;
	c->acq_device_type = id->device_type;
	c->acq_trans_type = id->trans_type;
	c->flags |= CHF_ACQUIRED;

	c->state = RADIANT_CH_STATE_TRACKING;
	c->t_search_start = RADIANT_TIME_NEVER;
	c->t_next = t_sync + period_us(c);
	c->miss_count = 0u;
	c->denied_count = 0u;
	/* A newly acquired master is a master nothing has been measured about.
	 * Whatever the previous one's clock was doing is not evidence about this
	 * one, and the first window after acquisition is the one least able to
	 * afford a guess. */
	guard_reset(c);
}

bool radiant_channel_on_slot_missed(uint8_t channel, radiant_time_t now)
{
	struct chan *c;

	(void)now; /* The clock advances from the slot that was MISSED, not
		    * from the moment the miss was noticed. Advancing from
		    * `now` would let scheduler latency walk the phase away
		    * from the master, one late notification at a time. */

	if (!ch_valid(channel)) {
		return false;
	}

	c = &channels[channel];
	if (c->state != RADIANT_CH_STATE_SEARCHING &&
	    c->state != RADIANT_CH_STATE_TRACKING) {
		return false;
	}

	if (c->t_next != RADIANT_TIME_NEVER) {
		c->t_next += period_us(c);
	}

	/*
	 * A MASTER ADVANCES ITS SLOT AND NOTHING ELSE.
	 *
	 * The miss accounting below is a slave's: eight windows in a row that
	 * heard nothing means the sensor is gone, and going back to search is
	 * the right answer. A master has nothing to hear and nowhere to go
	 * back to - its transmit was preempted or armed too late, which is
	 * contention, not loss of a peer.
	 *
	 * The advance itself is not optional, and its absence is what wedged a
	 * board. A master's t_next only moves on a completed transmit
	 * (radiant_channel_on_slot); a missed one used to leave it where it
	 * was, in the past, for ever. radiant_api.c re-posts a pending master
	 * every pass, the scheduler refuses an unreachable instant without
	 * calling the backend, the refusal completes synchronously, the
	 * completion signals the event thread, and the event thread posts the
	 * same dead instant again - a hot loop that answers no host message and
	 * puts nothing on the air. There is no fault and no log line; the
	 * dongle simply stops. One missed transmit was enough to trigger it.
	 */
	if ((c->type & RADIANT_CH_TYPE_MASTER_BIT) != 0u) {
		return false;
	}

	if (c->state != RADIANT_CH_STATE_TRACKING) {
		/* A searching channel misses by definition until it acquires;
		 * counting those would send it back to a state it is already
		 * in. */
		return false;
	}

	if (c->miss_count < 255u) {
		c->miss_count++;
	}

	if (c->miss_count < RADIANT_CHANNEL_RX_FAIL_TO_SEARCH) {
		return false;
	}

	/*
	 * Back to search. The acquired ID is dropped with the tracking state -
	 * the channel is looking for its configured (possibly wildcard) ID
	 * again, and leaving the resolved one readable would tell a host it is
	 * still tracking a sensor that stopped transmitting.
	 */
	c->state = RADIANT_CH_STATE_SEARCHING;
	c->flags &= (uint8_t)~CHF_ACQUIRED;
	c->miss_count = 0u;
	c->denied_count = 0u;
	c->t_search_start = c->t_next;
	/* Eight consecutive misses is the channel admitting it does not know
	 * where the master is. An average built while it did know is not
	 * evidence about wherever it turns up next. */
	guard_reset(c);

	radiant_channel_event_out(channel, RADIANT_CH_EVENT_RX_FAIL_GO_TO_SEARCH);

	return true;
}

bool radiant_channel_on_slot_denied(uint8_t channel, radiant_time_t now)
{
	struct chan *c;
	radiant_time_t t_before;

	if (!ch_valid(channel)) {
		return false;
	}

	c = &channels[channel];
	if (c->state != RADIANT_CH_STATE_SEARCHING &&
	    c->state != RADIANT_CH_STATE_TRACKING) {
		return false;
	}

	t_before = c->t_next;

	/*
	 * THE CLOCK MOVES, AND IT MOVES FROM THE SLOT THAT WAS LOST.
	 *
	 * Identical to a miss, and non-optional for the identical reason: a
	 * master whose t_next is left in the past is re-posted by
	 * radiant_api.c's pump, refused by the scheduler without ever reaching
	 * the backend, and completes synchronously back into the pump - a hot
	 * loop with no fault and no log line. Under an arbiter that loop is
	 * reachable from a single denied transmit, which is a routine event
	 * rather than the rare one that wedged a board before.
	 *
	 * ONE PERIOD, NOT "WHOLE PERIODS UNTIL AHEAD OF now", AND THAT WAS
	 * TESTED RATHER THAN ASSUMED. The argument for skipping ahead is
	 * plausible: a synchronous refusal completes inside the pass that posted
	 * the request, so several denials could land inside one channel period,
	 * leave t_next behind `now`, and have the next window expire MISSED.
	 * test_a_quiet_tracked_channel_misses_the_same_either_way was written to
	 * check exactly that, by running twelve periods with every window denied
	 * and twelve with none, and comparing the miss counts. They are equal.
	 * The behaviour the skip-ahead would have fixed does not occur, so the
	 * simpler rule stands and the phase argument above is not weakened by an
	 * exception nothing needs.
	 */
	if (c->t_next != RADIANT_TIME_NEVER) {
		c->t_next += period_us(c);
	}

	/* A master has nothing to hear, so the accounting below - which is
	 * entirely about how confident a slave is that it still knows where its
	 * master is - does not apply to it. Same early return, same reason, as
	 * radiant_channel_on_slot_missed(). */
	if ((c->type & RADIANT_CH_TYPE_MASTER_BIT) != 0u) {
		return false;
	}

	if (c->state != RADIANT_CH_STATE_TRACKING) {
		return false;
	}

	if (c->denied_count < 255u) {
		c->denied_count++;
	}

	if (c->denied_count < RADIANT_CHANNEL_DENY_TO_SEARCH) {
		return false;
	}

	/*
	 * The guard has been at RADIANT_CHANNEL_GUARD_MAX_US for a while and can
	 * no longer cover what has accumulated behind it - see
	 * RADIANT_CHANNEL_DENY_TO_SEARCH. From here on, staying TRACKING would
	 * mean opening windows in a place the master demonstrably is not, and
	 * reporting to a host that the sensor is fine.
	 *
	 * PROMOTED RATHER THAN HANDLED SEPARATELY. The counter is folded into
	 * the miss path and the ordinary machinery does the rest, so there is
	 * exactly one place in this module that returns a channel to SEARCHING
	 * and exactly one origin for RX_FAIL_GO_TO_SEARCH. A second copy of that
	 * transition is how the two come to disagree.
	 *
	 * on_slot_missed() advances t_next itself, so the advance above is undone
	 * first: this function has already charged the period for this slot, and
	 * charging it twice would put the channel's search start one period into
	 * the future.
	 */
	c->denied_count = 0u;
	c->t_next = t_before;
	c->miss_count = (uint8_t)(RADIANT_CHANNEL_RX_FAIL_TO_SEARCH - 1u);
	return radiant_channel_on_slot_missed(channel, now);
}

uint8_t radiant_channel_denied_count(uint8_t channel)
{
	if (!ch_valid(channel)) {
		return 0u;
	}
	return channels[channel].denied_count;
}

int radiant_channel_on_terminal(uint32_t op, enum radiant_radio_status status,
			    radiant_time_t now)
{
	int owner;
	struct chan *c;

	(void)now;
	(void)status; /* The close completes whatever the terminal status was.
		       * TIMEOUT and ABORTED are indistinguishable to a channel
		       * that already asked to leave the air, and whether a
		       * window that ended empty counts as a missed slot is
		       * radiant_sched.c's call - only it knows whether the window
		       * was merged with another channel's. */

	owner = radiant_channel_op_owner(op);
	if (owner < 0) {
		/*
		 * THIS IS NOT AN ERROR PATH. The HAL guarantees a cancelled
		 * operation's terminal event still arrives, with no fault
		 * injection, so a close that raced a window always produces
		 * exactly one of these. Counting it is what makes the race
		 * observable in a test; treating it as an anomaly would make
		 * every clean close look like a fault.
		 */
		stale_ops++;
		return -1;
	}

	c = &channels[owner];
	c->op = 0u;

	if (c->state == RADIANT_CH_STATE_CLOSING) {
		chan_finish_close((uint8_t)owner, c);
	}

	return owner;
}

uint32_t radiant_channel_tick(radiant_time_t now)
{
	uint32_t timed_out = 0u;
	uint8_t i;

	for (i = 0u; i < RADIANT_CHANNEL_COUNT; i++) {
		struct chan *c = &channels[i];
		radiant_time_t deadline;

		if (c->state != RADIANT_CH_STATE_SEARCHING) {
			continue;
		}

		deadline = search_deadline(c);
		if (deadline == RADIANT_TIME_NEVER || now < deadline) {
			continue;
		}

		/*
		 * Both events, in this order. A host library waits for
		 * RX_SEARCH_TIMEOUT to know the search failed and for
		 * CHANNEL_CLOSED to know it may unassign; a search timeout
		 * that raised only the first would leave the channel
		 * permanently un-unassignable, and one that raised only the
		 * second would look like the host's own close came back.
		 */
		radiant_channel_event_out(i, RADIANT_CH_EVENT_RX_SEARCH_TIMEOUT);
		chan_begin_close(i, c);

		search_timeouts++;
		timed_out++;
	}

	return timed_out;
}

/* ---------------------------------------------------------------------------
 * Counters
 * ---------------------------------------------------------------------------
 */

uint32_t radiant_channel_stale_op_count(void)
{
	return stale_ops;
}

uint32_t radiant_channel_search_timeout_count(void)
{
	return search_timeouts;
}
