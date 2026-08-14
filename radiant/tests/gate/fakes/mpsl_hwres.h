/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fakes/mpsl_hwres.h - which timer the gate borrows.
 *
 * Provenance: original clean-room work. nrfxlib's mpsl_hwres.h defines
 * MPSL_TIMER0 as one of NRF_TIMER0 / NRF_TIMER0_NS / NRF_TIMER10 / NRF_TIMER020
 * depending on the part; the only property the gate depends on is that it is a
 * TIMER MPSL owns and lends for the duration of a grant, so here it is the
 * register model in fakes/hal/nrf_timer.h.
 *
 * Pointing it at a real peripheral would defeat the purpose twice over: the
 * board this suite runs on has no MPSL in the image to lend anything, and a
 * TIMER that is not running reads the same capture value for ever - which is
 * precisely the branch of gate_release() that BUG 9 lives in.
 */

#ifndef RADIANT_TESTS_GATE_FAKE_MPSL_HWRES_H_
#define RADIANT_TESTS_GATE_FAKE_MPSL_HWRES_H_

#include <hal/nrf_timer.h>

extern fake_timer_t fake_mpsl_timer0;

#define MPSL_TIMER0 (&fake_mpsl_timer0)

#endif /* RADIANT_TESTS_GATE_FAKE_MPSL_HWRES_H_ */
