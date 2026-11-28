/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ctrl_arbiter.h - WHO IS ALLOWED TO COMMAND THIS MACHINE: the FE-C /
 * FTMS control arbiter (CONFIG_RADIANT_CTRL_ARBITER).
 *
 * Fitness equipment with two control transports - an ANT+ FE-C controller (a
 * head unit) and a BLE FTMS client (a phone) - needs one answer to "whose
 * setpoint does the machine obey". This module is that answer. It moved here
 * from apps/treadmill/src/treadmill_control.{h,c} so a treadmill and a smart
 * trainer share one policy.
 *
 * ─── What this is not ──────────────────────────────────────────────────────
 *
 * IT IS NOT A SUPPRESSION RULE. Nothing here ever stops a transmitter. FE-C
 * data pages, ANT+ SDM and the FTMS data notifications all keep running for
 * every listener regardless of who holds control, and a head unit tracking the
 * machine while a phone drives it sees a complete, correct stream throughout.
 * Sharing the air is a radio question, answered by the MPSL gate. Deciding
 * whose grade the deck moves to is a semantic question with no radio content
 * at all - it would need answering even if the two transports were on
 * separate chips.
 *
 * ─── Why it is needed ──────────────────────────────────────────────────────
 *
 * Both control paths land on one machine state, so two controllers writing
 * the same setpoint is last-writer-wins with no signal to either: a head unit
 * running a simulated climb and a phone running an interval workout both
 * think they own the deck, both are told PASS/SUCCESS, and the grade
 * oscillates between two setpoints with nothing reporting a conflict.
 *
 * ─── The policy: explicit beats implicit ───────────────────────────────────
 *
 * FTMS §4.16.2 requires an explicit Request Control (0x00) before any
 * procedure - it HAS an acquire. FE-C has no such handshake: a controller
 * simply sends a command page and reads the page 71 answer. So an explicit
 * claim outranks an implicit one, and each side already has the wire signal
 * needed to say no:
 *
 *   to an FE-C controller   page 71 status REJECTED (3)
 *   to an FTMS client       result code CONTROL_NOT_PERMITTED (0x05)
 *
 * ANT+ IS CONNECTIONLESS, WHICH IS WHY THERE IS A TIMEOUT. A BLE client that
 * walks away generates a disconnect and its claim is released by an event. An
 * FE-C controller that walks away generates nothing at all, so an idle timeout
 * is the ONLY way an implicit ANT claim is ever released. A BLE claim never
 * lapses on a timer: a phone that has taken control is still there whether or
 * not it has commanded anything this minute.
 *
 * ─── No kernel here, and the three-thread race it relies on being closed ───
 *
 * No Zephyr header, no lock, no clock: it is TOLD what time it is, so
 * radiant/tests/ftms drives the whole policy with no radio and no stack.
 *
 * The lock-freedom is sound for one reason only: EVERYTHING THAT TOUCHES THIS
 * MODULE RUNS ON THE SYSTEM WORKQUEUE. An FE-C page arrives on the
 * radiant_event thread, an FTMS write on the BT RX thread and the node tick on
 * the workqueue - three writers. Each control path copies its request and
 * submits a k_work before it reaches this module (apps/treadmill's fec_cmd ring
 * and radiant_ble_ftms.c's cp_work item), which closes that race without a
 * lock. docs/treadmill-reference-design.md §4a has the history. Read those
 * before adding a caller.
 */
#ifndef RADIANT_CTRL_ARBITER_H_
#define RADIANT_CTRL_ARBITER_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum radiant_ctrl_src {
	RADIANT_CTRL_NONE = 0,
	RADIANT_CTRL_BLE,
	RADIANT_CTRL_ANT,
};

/* The four FE-C command pages, spelled here so this header does not drag
 * radiant_profile_fec_tx.h into the tests that drive the policy. Applications
 * that include both _Static_assert them equal. */
#define RADIANT_CTRL_PAGE_BASIC_RESISTANCE 0x30u /* page 48 */
#define RADIANT_CTRL_PAGE_TARGET_POWER     0x31u /* page 49 */
#define RADIANT_CTRL_PAGE_WIND_RESISTANCE  0x32u /* page 50 */
#define RADIANT_CTRL_PAGE_TRACK_RESISTANCE 0x33u /* page 51 */

/* Bits of the `gated_pages` mask passed to radiant_ctrl_init(): one per
 * command page 48..51. */
#define RADIANT_CTRL_GATE(page) (1u << ((page) - RADIANT_CTRL_PAGE_BASIC_RESISTANCE))

/* The simulation-mode command set (pages 50 and 51), which is what a treadmill
 * gates. A trainer that implements basic resistance and target power gates all
 * four. */
#define RADIANT_CTRL_GATE_SIMULATION                     \
	(RADIANT_CTRL_GATE(RADIANT_CTRL_PAGE_WIND_RESISTANCE) | \
	 RADIANT_CTRL_GATE(RADIANT_CTRL_PAGE_TRACK_RESISTANCE))

/*
 * Reset to "nobody owns it".
 *
 *   ant_timeout_ms  idle timeout after which an implicit ANT claim lapses;
 *                   0 disables it.
 *   gated_pages     RADIANT_CTRL_GATE() bits: the FE-C command pages that
 *                   need the token. Gate exactly the pages that MOVE the
 *                   machine and that this machine implements. Pages 50 and 51
 *                   go together: honouring half of the simulation parameter
 *                   set and refusing the other half is how a controller
 *                   decides simulation mode is broken. A page the machine does
 *                   not implement stays ungated so it keeps answering
 *                   NOT_SUPPORTED: REJECTED there would misreport a missing
 *                   capability as a missing permission.
 *
 * Reads - page 70 (Request Data Page), page 55 (User Configuration) - are
 * never gated: refusing a read makes a machine look broken to a head unit that
 * is merely observing it.
 */
void radiant_ctrl_init(uint32_t ant_timeout_ms, uint8_t gated_pages);

/* Does commanding this FE-C page require owning the machine? */
bool radiant_ctrl_fec_page_is_gated(uint8_t fec_page);

/*
 * `src` wants to command the machine.
 *
 *   explicit_req   true for an FTMS Request Control (0x00), a deliberate
 *                  acquire. false for an FE-C command page, a claim inferred
 *                  from the act of commanding.
 *
 * Returns 0 if `src` owns the machine when this returns, or -EACCES. A caller
 * that gets -EACCES must refuse its command ON THE WIRE (page 71 REJECTED, or
 * 0x05). An explicit claim PREEMPTS an implicit owner; check
 * radiant_ctrl_owner() first if the preempted side needs telling.
 */
int radiant_ctrl_claim(enum radiant_ctrl_src src, bool explicit_req, uint32_t now_ms);

/* `src` is done, or has gone away (FTMS Reset, a BLE disconnect). A release by
 * a source that does not hold it is a no-op. */
void radiant_ctrl_release(enum radiant_ctrl_src src);

/*
 * Take the token from whoever holds it - a machine-side event that ends every
 * remote session, such as a safety-key pull. Returns the previous owner so the
 * caller can tell it (FTMS 0xFF Control Permission Lost; an FE-C controller
 * learns through REJECTED on its next command, as it has no connection).
 */
enum radiant_ctrl_src radiant_ctrl_revoke(void);

/* Advance the clock: releases an ANT owner idle for the timeout. A BLE owner
 * is released by an event, never here. Wrap-safe across a 32-bit uptime. */
void radiant_ctrl_tick(uint32_t now_ms);

enum radiant_ctrl_src radiant_ctrl_owner(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_CTRL_ARBITER_H_ */
