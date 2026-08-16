/* SPDX-License-Identifier: Apache-2.0 */
/* The test-side control surface of the fake trampoline. See fake_swi.c. */

#ifndef RADIANT_TESTS_GATE_FAKE_SWI_H_
#define RADIANT_TESTS_GATE_FAKE_SWI_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct fake_swi_counts {
	uint32_t registers;
	uint32_t pends;
	/* Per producer, so a test can say WHICH deferral was asked for rather
	 * than only that one was. */
	uint32_t request;
	uint32_t deny;
	uint32_t deadline;
	/* The union of every bit ever pended - the cheap assertion that a new
	 * producer has actually been routed through here. */
	uint32_t bits_seen;
};

void                          fake_swi_reset(void);
const struct fake_swi_counts *fake_swi(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_TESTS_GATE_FAKE_SWI_H_ */
