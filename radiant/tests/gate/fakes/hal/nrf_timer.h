/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fakes/hal/nrf_timer.h - MPSL_TIMER0 as a register model in RAM.
 *
 * Provenance: original clean-room work. The entry points and the constant names
 * are the ones radiant_radio_nrf_gate_mpsl.c calls, taken from its own call
 * sites; no nrfx source was copied.
 *
 * ---------------------------------------------------------------------------
 * WHY THE REAL HAL IS SHADOWED RATHER THAN USED
 * ---------------------------------------------------------------------------
 *
 * The gate's most delicate arithmetic is in gate_release(), and all of it is
 * about a COUNTER THAT MOVES WHILE THE FUNCTION RUNS:
 *
 *     CAPTURE1 -> read CC1 -> decide -> write CC0 -> CAPTURE1 again -> re-read
 *
 * BUG 9 is exactly the case where the counter passed the compare between the
 * write and the re-read, so the only signal that could have ended the timeslot
 * never fired. Against the real HAL pointed at a real TIMER that never runs,
 * CAPTURE1 reads the same value twice and that branch is unreachable; pointed at
 * a TIMER that does run, it is a race a test cannot aim. So the counter is
 * modelled instead: `counter` is what the next capture reads and `counter_step`
 * is how far it moves per capture, which makes "the counter overtook the
 * compare" a two-line setup rather than a coin toss.
 *
 * The shadow is contained to the `app` target of THIS application (see
 * CMakeLists.txt): the radiant module is a separate CMake target and never
 * sees this file, so nothing outside these tests compiles against it.
 *
 * IT IS A REGISTER MODEL AND NOT A STUB. Every write lands somewhere a test can
 * read back, which is what lets a scenario assert that the backstop compare was
 * PUT BACK rather than merely that the code did not crash.
 */

#ifndef RADIANT_TESTS_GATE_FAKE_HAL_NRF_TIMER_H_
#define RADIANT_TESTS_GATE_FAKE_HAL_NRF_TIMER_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FAKE_TIMER_CC_COUNT 6u

typedef struct fake_timer {
	/* The registers. */
	uint32_t cc[FAKE_TIMER_CC_COUNT];
	bool     event_compare[FAKE_TIMER_CC_COUNT];
	uint32_t inten;

	/*
	 * The model. `counter` is what the NEXT capture will read; it then
	 * advances by counter_step, so a test that wants the counter to overtake
	 * a compare between two captures sets a step and a test that wants a
	 * frozen counter leaves it at zero (which is also the default, so a
	 * scenario that does not care about the race is not accidentally in
	 * one).
	 */
	uint32_t counter;
	uint32_t counter_step;

	/* What a test asserts on. */
	uint32_t captures;
	uint32_t cc_writes[FAKE_TIMER_CC_COUNT];
	uint32_t int_enables;
	uint32_t int_disables;
} fake_timer_t;

/*
 * The constants the gate names. Values are arbitrary but distinct - this model
 * indexes by channel rather than by register offset, so the only requirement is
 * that the compare-0 event, the compare-0 interrupt and the capture-1 task are
 * told apart.
 */
#define NRF_TIMER_CC_CHANNEL0       0u
#define NRF_TIMER_CC_CHANNEL1       1u
#define NRF_TIMER_EVENT_COMPARE0    0u
#define NRF_TIMER_EVENT_COMPARE1    1u
#define NRF_TIMER_INT_COMPARE0_MASK (1u << 16)
#define NRF_TIMER_INT_COMPARE1_MASK (1u << 17)
/* Capture tasks are named by the channel they capture INTO. */
#define NRF_TIMER_TASK_CAPTURE0     0u
#define NRF_TIMER_TASK_CAPTURE1     1u

static inline void nrf_timer_cc_set(fake_timer_t *t, uint32_t channel,
				    uint32_t value)
{
	if (channel >= FAKE_TIMER_CC_COUNT) {
		return;
	}
	t->cc[channel] = value;
	t->cc_writes[channel]++;
}

static inline uint32_t nrf_timer_cc_get(fake_timer_t *t, uint32_t channel)
{
	if (channel >= FAKE_TIMER_CC_COUNT) {
		return 0u;
	}
	return t->cc[channel];
}

static inline void nrf_timer_int_enable(fake_timer_t *t, uint32_t mask)
{
	t->inten |= mask;
	t->int_enables++;
}

static inline void nrf_timer_int_disable(fake_timer_t *t, uint32_t mask)
{
	t->inten &= ~mask;
	t->int_disables++;
}

static inline void nrf_timer_event_clear(fake_timer_t *t, uint32_t event)
{
	if (event >= FAKE_TIMER_CC_COUNT) {
		return;
	}
	t->event_compare[event] = false;
}

static inline bool nrf_timer_event_check(fake_timer_t *t, uint32_t event)
{
	if (event >= FAKE_TIMER_CC_COUNT) {
		return false;
	}
	return t->event_compare[event];
}

/*
 * Only the capture tasks are modelled, because they are the only ones the gate
 * triggers. A capture takes the current counter into its channel and then
 * advances the counter - see the header comment.
 */
static inline void nrf_timer_task_trigger(fake_timer_t *t, uint32_t task)
{
	if (task >= FAKE_TIMER_CC_COUNT) {
		return;
	}
	t->cc[task] = t->counter;
	t->counter += t->counter_step;
	t->captures++;
}

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_TESTS_GATE_FAKE_HAL_NRF_TIMER_H_ */
