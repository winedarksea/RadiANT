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
 * binary, from rtl_433's expression, or from any ANT+ device
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
 * Three rules this file is written to:
 * 1. Every refusal returns the exact wire byte (antr_err_t is a uint8_t sent
 *    to the host unchanged; tools/ant_conformance.py byte-diffs it). A bad
 *    channel NUMBER is INVALID_MESSAGE, a bad parameter VALUE is
 *    INVALID_PARAMETER_PROVIDED, and the range check runs first and
 *    separately so the two paths never collide.
 * 2. No HAL calls (not radiant_radio_now(), not radiant_radio_abort()) - time
 *    arrives as an argument, so the state machine is a pure function and
 *    can't arm a window radiant_sched.c wanted to merge with another
 *    channel's.
 * 3. An operation id is the only link between a channel and the radio. The
 *    HAL guarantees a cancelled operation's terminal event still arrives, so
 *    "an event for an operation nobody owns" is normal; every consumer looks
 *    the op up first and counts the miss.
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
 * Per-channel record: flat, statically sized, 32 of them, budgeted at ~72 B
 * each (2.3 KB total) so a 32-channel dongle fits the footprint libant.a
 * needed for eight; the _Static_assert below enforces the budget.
 *
 * Field order is by descending alignment so the record packs without holes -
 * at 32 copies, four bytes of padding is 128 bytes of RAM.
 * ---------------------------------------------------------------------------
 */

/* flags */
#define CHF_ID_SET               0x01u /* radiant_channel_id_set() has been called */
#define CHF_ACQUIRED             0x02u /* the acquired ID below is valid */
#define CHF_CLOSING_FROM_TRACKING 0x04u /* what CLOSING is closing from */

struct chan {
	/* The next instant this channel wants the radio, as a t_sync (the
	 * moment the last bit of the on-air address hits the antenna, per
	 * radiant_radio_hal.h). 64 bits is load-bearing: the mock's clock starts
	 * one second below 2^32 us, so a uint32_t here fails inside the first
	 * virtual second of the first test. */
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

	/* Exponential average of |t_sync - predicted| over clean consecutive
	 * slots, in SIXTEENTHS of a microsecond (see radiant_channel_guard_us()).
	 * Sixteenths because the quantity is typically ~9 of them, and at
	 * whole-microsecond resolution a 1/8-weight EWMA can't represent 9 vs
	 * 10. 16 x 4095 us is ten times the widest guard there is. */
	uint16_t resid_q4_us;

	/* The master's announced clock accuracy in ppm, or 0 if none. Two
	 * bytes rather than the three-bit wire code, because the core must not
	 * carry a profile's vocabulary (src/profiles/ owns the ladder). Fits
	 * inside existing padding, so the 72 B budget is unmoved. */
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
	/* Consecutive slots lost to an arbitrated backend lending the radio to
	 * another stack. Weighted like miss_count in the guard, cleared
	 * wherever it's cleared, but kept separate so a denial never counts
	 * towards RX_FAIL_GO_TO_SEARCH. Always zero on a backend that owns the
	 * radio outright. */
	uint8_t denied_count;
	/* Clean consecutive slots the estimator above has seen since reset.
	 * Saturates; only the comparison against RADIANT_CHANNEL_GUARD_LOCK_SLOTS
	 * matters. Zero means "no evidence" -> widest guard. */
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
 * The window-guard estimator. Integer only: this code runs inside the radio
 * event path, and a soft-float multiply there would cost more than the
 * window it's trying to save.
 * ---------------------------------------------------------------------------
 */

/* Weight of a new sample in the running average, as a right shift. 1/8 gives
 * roughly eight slots of memory - two seconds at the ANT+ period, long enough
 * to average and short enough to follow a master's clock drifting with
 * temperature. */
#define GUARD_EWMA_SHIFT 3u

/* The most the sixteenths field can hold, in whole microseconds - a property
 * of storage, stated separately from the protocol ceiling below. */
#define GUARD_RESID_FIELD_MAX_US (65535u / 16u)

/* GUARD_MAX is sixteen worst-case periods, written as arithmetic so an
 * announced clock can re-derive it: 8 is RADIANT_CHANNEL_RX_FAIL_TO_SEARCH,
 * the other factor of two is margin. */
#define GUARD_CEIL_PERIODS \
	(RADIANT_CHANNEL_GUARD_MAX_US / RADIANT_CHANNEL_DRIFT_WORST_US)

_Static_assert(RADIANT_CHANNEL_GUARD_MAX_US ==
		       GUARD_CEIL_PERIODS * RADIANT_CHANNEL_DRIFT_WORST_US,
	       "GUARD_MAX is no longer a whole number of worst-case periods, so "
	       "an announced clock accuracy can no longer re-derive it");
_Static_assert(GUARD_CEIL_PERIODS >= 2u * RADIANT_CHANNEL_RX_FAIL_TO_SEARCH,
	       "the ceiling no longer covers every period a channel may "
	       "extrapolate before it gives up");

/* One period's worst-case disagreement between this master's clock and ours,
 * in microseconds. RADIANT_CHANNEL_DRIFT_WORST_US when nothing was announced
 * (the default for every channel in a build that never calls the setter). An
 * announcement can only widen the result: the constant is a floor, because a
 * claimed 20 ppm is a claim and the 25 us is a budget. */
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

/* The widest this channel's guard may get: RADIANT_CHANNEL_GUARD_MAX_US, or
 * the same arithmetic at an announced clock's worse period. Also bounded by a
 * quarter period, since a window reaching into the neighbouring slot would
 * stop being a window - not binding today, but guards against a future
 * ladder entry or period floor change quietly turning a receive window into
 * a continuous scan. */
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

/* Fold one measured residual into the average. `err_us` is
 * |t_sync - predicted| for a slot whose PREVIOUS slot was also heard - slots
 * after a miss are excluded because their residual covers two or more
 * periods of drift and would measure the miss, not the master's clock.
 *
 * The first sample is taken whole rather than ramped up from zero, since
 * ramping would underestimate for the first several slots (the direction
 * that loses packets); the lock counter withholds the result until there are
 * enough samples to mean anything. */
static void guard_observe(struct chan *c, radiant_time_t err_us)
{
	/* The clamp is the channel's own ceiling, not the compiled-in one: once
	 * a master announces a bad clock (e.g. an RC-clocked node with a
	 * hundreds-of-microseconds residual), clamping at the compiled-in value
	 * would just measure the clamp. With nothing announced the ceiling IS
	 * RADIANT_CHANNEL_GUARD_MAX_US, so this stays the same clamp as before. */
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

/* Reset one channel's configuration to its power-on defaults. Called from
 * init, assign and unassign. Assign resetting the config matters: a channel
 * reassigned to a different type must not inherit the last session's channel
 * ID, or a host that opens without setting one would silently track the
 * previous sensor instead of getting RADIANT_CH_ERR_ID_NOT_SET. */
static void chan_defaults(struct chan *c)
{
	memset(c, 0, sizeof(*c));

	c->state = RADIANT_CH_STATE_UNASSIGNED;
	c->t_next = RADIANT_TIME_NEVER;
	c->t_search_start = RADIANT_TIME_NEVER;

	/* 8182 counts, the ANT+ 4.005 Hz default, measured as a 249,696.4 us
	 * mean over 744 intervals. NOT 8192 - see radiant_channel.h. A default
	 * 305 us/slot off from what real sensors transmit at would let a
	 * channel opened without a period walk out of the master's window in
	 * under a minute, looking like a receiver bug. */
	c->period = RADIANT_CHANNEL_PERIOD_ANT_PLUS;

	/* 66, ANT's power-on default, deliberately NOT the ANT+ frequency (57):
	 * a host that forgot to set the frequency would half-work on the ANT+
	 * band, which is harder to diagnose than finding nothing at all. */
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

/* Leave the air. The single place a channel stops being open, so
 * CHANNEL_CLOSED has exactly one origin - a host library that reopens on it
 * hangs if missed and double-closes if duplicated, both invisible in a build
 * log. */
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

/* Begin leaving the air. Completes immediately when no radio operation is
 * bound, or parks in CLOSING when one is - the terminal event the HAL
 * promises will still arrive finishes the job. */
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
 * better than a channel that sits in SEARCHING forever with both timeouts
 * off. */
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
	 * ANTW_EXT_PARAM_ALWAYS_SEARCH means the channel is a discovery channel
	 * the host intends to leave open indefinitely. Nothing ever acquires on
	 * it (see api_search_acquired()'s scan-mode branch), so the ordinary
	 * search-timeout machinery - a deadline meant to stop hunting for one
	 * absent device - does not apply.
	 *
	 * Without this: a scan channel opened with default timeouts stops dead
	 * at exactly 25 s (RX_SEARCH_TIMEOUT, then closes), and
	 * radiant_search_n_searching() reads 0 from then on - a discovery
	 * channel that permanently stops discovering, looking like a dongle
	 * that "stops finding sensors after a while".
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
	/* Back to the core's own minimum, not whatever the last backend raised
	 * it to: antr_init() calls this before the radio is enabled and sets
	 * the floor from caps afterwards. */
	guard_floor = RADIANT_CHANNEL_GUARD_MIN_US;
	stale_ops = 0u;
	search_timeouts = 0u;
}

void radiant_channel_reset_all(void)
{
	uint8_t i;

	for (i = 0u; i < RADIANT_CHANNEL_COUNT; i++) {
		struct chan *c = &channels[i];

		/* Raise CHANNEL_CLOSED for anything on air, including a channel
		 * already in CLOSING - a reset must not swallow an event the
		 * host is owed. The bridge discards these for a short window
		 * afterwards (a real stick emits nothing between reset and the
		 * startup message); that's the bridge's call, not this one's. */
		if (state_on_air(c->state)) {
			radiant_channel_event_out(i, RADIANT_CH_EVENT_CHANNEL_CLOSED);
		}

		chan_defaults(c);
	}

	/* Operations still in flight are orphaned rather than waited on: this
	 * module cannot abort one. radiant_sched.c calls radiant_radio_abort()
	 * before this and feeds the terminal event in afterwards, where it
	 * lands on no binding and is counted stale, like any other late event. */
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

	/* `type` is stored without validation, deliberately: antr_channel_assign()'s
	 * permitted return set has no code for a bad type (INVALID_MESSAGE is
	 * already used by the channel number), so refusing here would put an
	 * out-of-contract byte on the wire. Only bit 4 is interpreted; the rest
	 * is carried and reported back in the status byte. */
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

	/* Refuses a channel that is open OR closing rather than closing it on
	 * the caller's behalf: silently closing first would swallow the
	 * CHANNEL_CLOSED event the host is waiting for. Same reason
	 * radiant_channel_status_get() keeps reporting SEARCHING/TRACKING until
	 * the close completes. */
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
		/* A MASTER'S FIRST TRANSMISSION IS ONE FULL CHANNEL PERIOD
		 * AFTER THIS CALL, NOT ZERO. Measured 8/8 runs: MESG_OPEN_CHANNEL
		 * to first EVENT_TX was consistently ~250-266 ms
		 * (docs/ant-radio-link.md, "Master open-time"); whether it's a
		 * collision probe or slot alignment isn't observable from the
		 * air, so this reproduces the behaviour without claiming the
		 * mechanism.
		 *
		 * `offset` is added ON TOP of the period, not in place of it -
		 * a phase adjustment to the slot the channel would have taken
		 * anyway, which is also the only reading under which offset
		 * does what it's for (phasing one master's slot from another's). */
		c->t_next = start + period_us(c);
		c->state = RADIANT_CH_STATE_TRACKING;
		c->t_search_start = RADIANT_TIME_NEVER;
	} else {
		/* A slave enters search with its first window at `start`, no
		 * period wait: it has nothing to align to yet. */
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
		    * radio immediately, and any in-flight operation ends via
		    * the terminal event, not the clock. Parameter kept for
		    * signature symmetry with the other lifecycle calls. */

	if (!ch_valid(channel)) {
		return RADIANT_CH_ERR_INVALID_MESSAGE;
	}

	c = ch_at(channel);

	/* A second close on a CLOSING channel is WRONG_STATE, not a second
	 * event: the host is owed exactly one CHANNEL_CLOSED. */
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

	/* The floor is a schedulability bound, not a taste: an acknowledged
	 * exchange costs ~1.55 ms each way, so a period below ~8 ms cannot
	 * contain one slot's traffic. Accepting it would promise a schedule
	 * the radio can never keep. */
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

	/* Bit 7 means `custom` names a raw register value, so there's nothing to
	 * range-check. With the bit clear, 0..5 are -20..+8 dBm; level 5 is
	 * accepted even where hardware can't reach +8 - radiant_sched.c carries
	 * power verbatim to the backend, which rounds to its nearest setting
	 * (radiant_radio_hal.h's struct radiant_tx_power contract), not a
	 * clamp against caps.tx_power_max_dbm. */
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

	/* The supported set is exactly {default}. Silently keeping the default
	 * for anything else would let the host believe it configured something
	 * it didn't, with the failure showing up as corruption, not refusal. */
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

	/* The value is stored and never acted on - see the header. Refusing
	 * anything but the two values sdk-ant names keeps that an honest
	 * contract rather than a silent one. */
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
	/* PERMANENT LIMITATION - see the header. Nothing reads prox_threshold
	 * back, so accepting non-zero would advertise a filter that never
	 * runs. 0 asks for the behaviour this stack already has (no RSSI gate). */
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
	 * channel type in bits 7:4. Type values already sit in the top nibble
	 * so they're masked across, not shifted. */
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

	/* A CLOSING channel returns RADIANT_TIME_NEVER even though its clock
	 * still holds a time: it has stopped wanting the radio, and arming one
	 * more window for it would produce a second in-flight operation. */
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

	/* Every early return here is the channel's CEILING, not the floor,
	 * because each means "no evidence": an out-of-range channel, one not
	 * tracking, or one whose estimator hasn't locked all get the widest
	 * window (RADIANT_CHANNEL_GUARD_MAX_US, or more for an announced
	 * clock). The unlocked case matters most: an RC-clocked master can be
	 * outside the old ceiling on its very first extrapolated slot, so
	 * waiting for lock would wait for a lock that can never happen. */
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
	 * THE FLOOR IS APPLIED BEFORE THE MISS TERM, NOT AFTER - the ordering is
	 * the safety argument. The guard must cover the period being received
	 * plus every period extrapolated since the last fresh sync (N misses =
	 * N+1 periods of disagreement). Applying the floor first and adding the
	 * miss term after gets that right; clamping afterwards instead
	 * (max(floor, k*avg + N*drift)) reads almost the same but is quietly
	 * weaker - with a well-behaved master the miss term can disappear
	 * inside the floor, and by seven misses the window covers only 175 us
	 * of a 200 us worst case, with no error code to show for it.
	 */
	drift = drift_worst_us(c);

	/* The floor covers the period being RECEIVED, so an announced clock
	 * raises it (at 500 ppm / 2 s heartbeat, one period alone is 1.1 ms).
	 * With nothing announced, drift is the 25 us constant, already below
	 * the floor, so this line changes nothing - the compatibility claim. */
	floor_us = guard_floor;
	if (floor_us < drift) {
		floor_us = drift;
	}
	if (guard < floor_us) {
		guard = floor_us;
	}
	/* TWO COUNTERS, ONE WEIGHT: a period extrapolated with no fresh sync is
	 * unmeasured disagreement whether we chose not to listen or weren't
	 * given the air, so a denial is charged at the same rate as a miss. The
	 * sum is bounded - RX_FAIL_TO_SEARCH (8) and DENY_TO_SEARCH (16) each
	 * promote to SEARCHING before their term can exceed the ceiling - and
	 * the clamp below covers any interleaving of the two. */
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

	/* Clamped, not refused: an announcement worse than the clamp is either
	 * a typo or hostile, and the widest window this core builds is the
	 * safe answer either way - it costs receive current, never packets. */
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
	 * prediction it's measured against - the only place a fresh sync and
	 * its predicted instant both exist.
	 *
	 * Only on a slot whose predecessor was heard (miss_count == 0): after a
	 * miss the prediction is extrapolated extra periods, so the difference
	 * would measure link loss rather than the master's clock.
	 *
	 * A denial extrapolates the prediction identically, so it gates the
	 * estimator identically - the residual here is our own extrapolation
	 * error either way, and after a run of denials it sums over all of
	 * them. Feeding that into an 8-slot EWMA would pin the window wide for
	 * ~two seconds after a busy patch in another stack, on a link behaving
	 * perfectly.
	 */
	if (c->miss_count == 0u && c->denied_count == 0u &&
	    c->t_next != RADIANT_TIME_NEVER) {
		guard_observe(c, (t_sync > c->t_next) ? (t_sync - c->t_next)
						     : (c->t_next - t_sync));
	}

	/* Re-anchor on the frame actually on the air, not the predicted time:
	 * a constant prediction error would otherwise accumulate silently -
	 * the drift estimate still locks, but the window slides until yield
	 * degrades to the bench's ~0.4% collision floor. */
	c->t_next = t_sync + period_us(c);
	c->miss_count = 0u;
	/* A slot heard is a fresh sync, retiring every period of extrapolation
	 * behind it - whichever counter charged it. */
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

	/* The wildcards resolve here and nowhere else: the configured ID stays
	 * untouched so closing and reopening searches again instead of
	 * silently narrowing to the last sensor. */
	c->acq_device_number = id->device_number;
	c->acq_device_type = id->device_type;
	c->acq_trans_type = id->trans_type;
	c->flags |= CHF_ACQUIRED;

	c->state = RADIANT_CH_STATE_TRACKING;
	c->t_search_start = RADIANT_TIME_NEVER;
	c->t_next = t_sync + period_us(c);
	c->miss_count = 0u;
	c->denied_count = 0u;
	/* A newly acquired master is one nothing has been measured about; the
	 * previous master's clock is not evidence about this one. */
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
	 * A MASTER ADVANCES ITS SLOT AND NOTHING ELSE. The miss accounting
	 * below is a slave's (eight silent windows means the sensor is gone);
	 * a master has nothing to hear - a preempted/late transmit is
	 * contention, not loss of a peer.
	 *
	 * The advance itself is not optional: a master's t_next only moves on
	 * a completed transmit, so a missed one left it stuck in the past
	 * forever, which produced a hot loop (repost -> scheduler refuses the
	 * unreachable instant -> completes synchronously -> reposts again) with
	 * no fault and no log line - the dongle simply stopped, from a single
	 * missed transmit.
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

	/* Back to search. The acquired ID is dropped with the tracking state -
	 * leaving it readable would tell a host it's still tracking a sensor
	 * that stopped transmitting. */
	c->state = RADIANT_CH_STATE_SEARCHING;
	c->flags &= (uint8_t)~CHF_ACQUIRED;
	c->miss_count = 0u;
	c->denied_count = 0u;
	c->t_search_start = c->t_next;
	/* An average built while the channel knew where the master was is not
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
	 * THE CLOCK MOVES, AND IT MOVES FROM THE SLOT THAT WAS LOST - identical
	 * to a miss and non-optional for the identical reason: a master whose
	 * t_next is left in the past gets re-posted, refused synchronously by
	 * the scheduler, and re-posted again, a hot loop reachable from a
	 * single denied transmit under an arbiter.
	 *
	 * ONE PERIOD, NOT "WHOLE PERIODS UNTIL AHEAD OF now" - tested, not
	 * assumed: test_a_quiet_tracked_channel_misses_the_same_either_way
	 * compares miss counts over twelve periods with every window denied
	 * vs. twelve with none. They're equal, so the simpler rule stands.
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
	 * The guard has been at RADIANT_CHANNEL_GUARD_MAX_US too long to cover
	 * what has accumulated behind it (RADIANT_CHANNEL_DENY_TO_SEARCH) -
	 * staying TRACKING now means opening windows where the master
	 * demonstrably isn't.
	 *
	 * PROMOTED RATHER THAN HANDLED SEPARATELY: folded into the miss path so
	 * there is exactly one place that returns a channel to SEARCHING and
	 * one origin for RX_FAIL_GO_TO_SEARCH. on_slot_missed() advances t_next
	 * itself, so the advance above is undone first to avoid charging this
	 * slot's period twice.
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

		/* Both events, in this order: RX_SEARCH_TIMEOUT tells the host
		 * the search failed, CHANNEL_CLOSED tells it it may unassign.
		 * Raising only one would strand the channel un-unassignable or
		 * look like the host's own close came back. */
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
