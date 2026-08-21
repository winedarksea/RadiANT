/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The receive-activity counter's storage, and nothing else. See ant_activity.h
 * for what writes it and what reads it.
 *
 * WHY ONE VARIABLE GETS ITS OWN TRANSLATION UNIT. It used to be defined in
 * ant_rgb_led.c, which was correct while the NeoPixel was the only reader. With
 * two possible readers the owner would otherwise depend on which indicator
 * happens to be compiled into a given image - and an image with the led0 blink
 * but no NeoPixel would fail at the link rather than anywhere informative. One
 * file gated on the shared symbol makes the ownership a build fact instead of a
 * coincidence.
 */

#include <zephyr/sys/atomic.h>

#include "ant_activity.h"

atomic_t ant_activity_pkts;
