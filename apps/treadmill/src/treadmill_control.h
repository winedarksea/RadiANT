/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * treadmill_control.h - WHO IS ALLOWED TO COMMAND THIS MACHINE.
 *
 * ─── What this is not ──────────────────────────────────────────────────────
 *
 * IT IS NOT A SUPPRESSION RULE. Nothing here ever stops a transmitter. FE-C
 * pages 16/17/19/80/81, ANT+ SDM 0x7C and the FTMS Treadmill Data notification
 * all keep running for every listener regardless of who holds control, and a
 * head unit tracking the machine while a phone drives it sees a complete,
 * correct stream throughout. apps/treadmill/Kconfig's "THERE IS NO SUPPRESSION
 * RULE HERE" stands unamended: that ruling is about the ANT+ *transmitter*,
 * which stays on, and it was never about command ownership.
 *
 * The two are genuinely independent and conflating them is the mistake this
 * header exists to prevent. Sharing the air is a radio question, answered by
 * the MPSL gate and measured by [gates.coexistence]. Deciding whose grade the
 * deck actually moves to is a semantic question with no radio content at all -
 * it would need answering even if the two transports were on separate chips.
 *
 * ─── Why it is needed ──────────────────────────────────────────────────────
 *
 * treadmill_state.h says both control paths "land HERE, in one place, which is
 * the whole symmetry this application exists to get right". True, and that is
 * exactly what creates the problem: two controllers writing
 * target_incline_centi_pct is last-writer-wins, with no signal to either. A
 * head unit running a simulated climb and a phone running an interval workout
 * both think they own the deck, both are told PASS/SUCCESS, and the grade
 * oscillates between two setpoints with nothing anywhere reporting a conflict.
 *
 * ─── The policy: explicit beats implicit ───────────────────────────────────
 *
 * The asymmetry below is the whole design, and it is spec-native rather than
 * invented. FTMS §4.16.2 already requires an explicit Request Control (0x00)
 * handshake before ANY procedure - it HAS an acquire. FE-C has no such
 * handshake: a controller simply sends page 51 and reads the page 71 answer.
 * So an explicit claim outranks an implicit one, and each side already has the
 * wire signal needed to say no:
 *
 *   to an FE-C controller   page 71 status PROFILE_FEC_CMD_STATUS_REJECTED (3)
 *   to an FTMS client       result code CONTROL_NOT_PERMITTED (0x05)
 *
 * Neither is invented for this: both are pre-existing constants in the two
 * specifications, meaning precisely this.
 *
 * ANT+ IS CONNECTIONLESS, WHICH IS WHY THERE IS A TIMEOUT. A BLE client that
 * walks away generates a disconnect and its claim is released by an event. An
 * FE-C controller that walks away generates nothing at all - it simply stops
 * sending page 51 - so an idle timeout is the ONLY way an implicit ANT claim
 * can ever be released. Without it, one page 51 early in a session would lock
 * a phone out of the machine forever.
 *
 * ─── No kernel here, and that is the testability argument ──────────────────
 *
 * Same rule as treadmill_state.c: no Zephyr header, no lock, no clock. It is
 * TOLD what time it is. That is what lets apps/treadmill/tests/pages drive the
 * whole policy in a ztest with no radio and no stack, which matters more here
 * than anywhere else in this application - NOTHING tests treadmill_ble.c. There
 * is no ztest for the GATT service, write_cp() or the advertising payload at
 * all, so arbitration living inside that file would be arbitration nothing can
 * reach. It lives here for that reason.
 *
 * The lock-freedom is safe for the same reason treadmill_state's is, and only
 * for that reason: EVERYTHING THAT TOUCHES THIS MODULE RUNS ON THE SYSTEM
 * WORKQUEUE. Both control paths marshal onto it before they get here - the
 * FE-C path through main.c's fec_cmd work item, the FTMS path through
 * treadmill_ble.c's cp_work item. Read those before adding a caller.
 */

#ifndef TREADMILL_CONTROL_H_
#define TREADMILL_CONTROL_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum treadmill_ctrl_src {
	TREADMILL_CTRL_NONE = 0,
	TREADMILL_CTRL_BLE,
	TREADMILL_CTRL_ANT,
};

/*
 * The two FE-C pages the token gates, spelled here as well so this header does
 * not drag profile_fec_tx.h into the ztest that exercises the policy - the same
 * trick treadmill_state.h uses for the FE state values, and _Static_assert'd
 * equal to PROFILE_FEC_PAGE_* in main.c so the two spellings cannot drift.
 */
#define TREADMILL_CTRL_PAGE_WIND_RESISTANCE  0x32u /* page 50 */
#define TREADMILL_CTRL_PAGE_TRACK_RESISTANCE 0x33u /* page 51 */

/*
 * Does commanding this page require owning the machine?
 *
 * TRUE FOR A SHORT LIST ON PURPOSE - pages 50 and 51, the simulation-mode
 * command set, which are the only pages that move the deck. They are gated
 * together rather than page 51 alone because accepting half of a parameter set
 * and refusing the other half is how a controller decides simulation mode is
 * broken and stops sending page 51 too; if a controller may not have the deck,
 * it may not have either half.
 *
 * FALSE FOR THE READS, and that is the important half. Page 70 (Request Data
 * Page) and page 55 (User Configuration) are answered whoever holds the token.
 * Refusing a read would make the machine look broken to a head unit that is
 * merely observing it - which is the opposite of what the token is for, since
 * nothing here is allowed to degrade what a listener sees. Pages 48 and 49 stay
 * false too: this machine does not implement them for anybody, and answering
 * REJECTED where NOT_SUPPORTED is true would misreport a capability as a
 * permission.
 */
bool treadmill_control_page_is_gated(uint8_t fec_page);

/*
 * Reset to "nobody owns it", with `ant_timeout_ms` as the idle timeout after
 * which an implicit ANT claim lapses. 0 disables the timeout, which is what
 * TREADMILL_CTRL_POLICY_NONE uses along with never refusing anything.
 *
 * Called from main() at boot, and from each ztest case - which is the reason it
 * is a separate function from a static initialiser.
 */
void treadmill_control_init(uint32_t ant_timeout_ms);

/*
 * `src` wants to command the machine.
 *
 *   explicit_req   true for an FTMS Request Control (0x00), which is an
 *                  acquire the client deliberately performed. false for an
 *                  FE-C command page, which is a claim inferred from the mere
 *                  act of commanding - there is no FE-C acquire to perform.
 *
 * Returns 0 if `src` owns the machine when this returns, or -EACCES if it does
 * not. A caller that gets -EACCES must refuse the command it was about to
 * apply, and must say so on the wire: page 71 REJECTED, or 0x05.
 *
 * An explicit claim PREEMPTS an implicit owner. Check treadmill_control_owner()
 * BEFORE calling if the preempted side needs telling - once this returns, the
 * previous owner is gone.
 */
int treadmill_control_claim(enum treadmill_ctrl_src src, bool explicit_req,
			    uint32_t now_ms);

/* `src` is done, or has gone away: FTMS Reset (0x01), a BLE disconnect. A
 * release by a source that does not hold it is a no-op rather than an error -
 * a disconnect callback should not have to ask first. */
void treadmill_control_release(enum treadmill_ctrl_src src);

/*
 * Advance the clock. Releases an ANT owner that has not commanded anything for
 * `ant_timeout_ms`; does nothing to a BLE owner, which is released by an event
 * instead. Called from main.c's node tick.
 */
void treadmill_control_tick(uint32_t now_ms);

enum treadmill_ctrl_src treadmill_control_owner(void);

#ifdef __cplusplus
}
#endif

#endif /* TREADMILL_CONTROL_H_ */
