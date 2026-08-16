/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * treadmill_hr_rx.h - phase 5: an ANT+ heart-rate SLAVE beside two masters.
 *
 * A third channel, device type 0x78, wildcard device number. Its bpm feeds
 * three consumers and the reason it is worth a channel at all is that all
 * three want the same number:
 *
 *   - FE-C page 16 byte 6, with the capabilities nibble's HR-source bits set
 *     to 1 (ANT+ HRM), so a head unit knows where the reading came from;
 *   - the FTMS Treadmill Data heart-rate field, flag bit 8;
 *   - a console display, when one exists. There is no display code anywhere in
 *     this tree and none is proposed; this stops at making the value
 *     available.
 *
 * ─── The acquisition rule, which is a contention decision ──────────────────
 *
 * A TRACKING slave beside two masters is the benign case. An ACQUISITION SWEEP
 * beside them is not: this project's own measurements say a sweeping receiver
 * is what starves the arbiter, and docs/decisions/0013 names the sweep as the
 * elastic consumer for exactly that reason. So this module acquires ONCE and
 * then stops searching.
 *
 * ─── "Strongest wins", and what this actually implements ───────────────────
 *
 * The obvious reading of "select the strongest strap" is: sweep, collect every
 * candidate with its RSSI, pick the best. THAT IS THE THING THE PARAGRAPH
 * ABOVE FORBIDS. An ANT slave locks onto the first channel id that matches its
 * (wildcard) filter; seeing several candidates means continuing to search
 * after one has been found, which is the sweep.
 *
 * What this does instead is what a head unit does: a PROXIMITY SEARCH
 * THRESHOLD. The channel refuses to acquire anything below an RSSI floor, so
 * the strap on the runner's chest is acquired and the one on the next
 * treadmill is not - without ever listening to both. The floor starts tight
 * and RELAXES one bin per search timeout, so a weak-but-present strap is still
 * found eventually and a room full of straps still resolves to the nearest.
 *
 * The difference from "strongest of N candidates" is real and is not hidden:
 * with two straps at similar RSSI this acquires whichever transmits first
 * above the current floor, not provably the stronger. Hysteresis and a
 * hold-down then keep it - a marginal second strap cannot cause flapping,
 * because nothing re-searches while a strap is being tracked and losing one
 * costs a full timeout before the floor moves.
 */

#ifndef TREADMILL_HR_RX_H_
#define TREADMILL_HR_RX_H_

#include <stdbool.h>
#include <stdint.h>

struct antr_msg;

/* Assign, configure and open the slave. Returns 0 or a negative errno; a
 * failure is logged and the node keeps running with no heart rate, which on
 * the air is FE-C page 16's 0xFF sentinel. */
int treadmill_hr_rx_start(uint8_t channel);

/* Feed one inbound message. Called from antr_on_message(); returns true if the
 * message was this module's. */
bool treadmill_hr_rx_message(const struct antr_msg *msg);

/* The device number of the strap being tracked, or 0 when none. */
uint16_t treadmill_hr_rx_device(void);

/* Implemented by main.c, called on the workqueue: one bpm reading, or
 * TREADMILL_HR_NONE when the strap is lost. */
void treadmill_node_heart_rate(uint8_t bpm);

#endif /* TREADMILL_HR_RX_H_ */
