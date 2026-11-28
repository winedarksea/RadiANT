/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The FE-C / FTMS control arbiter (radiant_ctrl_arbiter.c). These cases moved
 * here from apps/treadmill/tests/pages with the module itself.
 *
 * WHAT THEY DO NOT COVER, stated so a pass is not over-read: they prove the
 * POLICY, not the plumbing. That both control paths marshal onto the system
 * workqueue before they reach the token is what makes the lock-freedom sound,
 * and it is not visible from here.
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/ztest.h>

#include <radiant/radiant_ctrl_arbiter.h>

#define CTRL_TIMEOUT_MS 30000u

static void arbiter_reset(void *fixture)
{
	ARG_UNUSED(fixture);
	radiant_ctrl_init(CTRL_TIMEOUT_MS, RADIANT_CTRL_GATE_SIMULATION);
}

ZTEST(radiant_ctrl_arbiter, test_an_explicit_ble_claim_preempts_an_ant_owner)
{
	radiant_ctrl_init(CTRL_TIMEOUT_MS, RADIANT_CTRL_GATE_SIMULATION);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_NONE, NULL);

	/* An FE-C page 51 arrives first and claims implicitly - there is no
	 * FE-C acquire handshake, so commanding IS the claim. */
	zassert_equal(radiant_ctrl_claim(RADIANT_CTRL_ANT, false, 1000u),
		      0, NULL);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_ANT, NULL);

	/* Then a phone runs FTMS Request Control, which IS an explicit
	 * acquire, and takes the machine. This asymmetry is the whole policy
	 * and it is spec-native: FTMS 4.16.2 has the handshake, FE-C has
	 * none. */
	zassert_equal(radiant_ctrl_claim(RADIANT_CTRL_BLE, true, 2000u),
		      0, NULL);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_BLE, NULL);
}

ZTEST(radiant_ctrl_arbiter, test_ant_never_preempts_a_ble_owner)
{
	radiant_ctrl_init(CTRL_TIMEOUT_MS, RADIANT_CTRL_GATE_SIMULATION);

	zassert_equal(radiant_ctrl_claim(RADIANT_CTRL_BLE, true, 1000u),
		      0, NULL);

	/* The direction that must NOT work. An FE-C page 51 now gets -EACCES,
	 * which the application turns into page 71 status
	 * RADIANT_PROFILE_FEC_CMD_STATUS_REJECTED (3) - the profile's own constant for
	 * this, and the only signal ANT+ has, since there is no connection to
	 * drop and no status characteristic to notify. */
	zassert_equal(radiant_ctrl_claim(RADIANT_CTRL_ANT, false, 2000u),
		      -EACCES, NULL);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_BLE,
		      "a refused claim must not disturb the owner");

	/* Refusal is not a one-off sulk: it holds for as long as the BLE
	 * client has it, including past the ANT idle timeout, which does not
	 * apply to a BLE owner at all. */
	radiant_ctrl_tick(1000u + CTRL_TIMEOUT_MS * 3u);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_BLE,
		      "the idle timeout is for ANT+ only - a phone that has "
		      "taken control is still there whether or not it has "
		      "commanded anything this minute");
	zassert_equal(radiant_ctrl_claim(RADIANT_CTRL_ANT, false,
					      1000u + CTRL_TIMEOUT_MS * 3u),
		      -EACCES, NULL);
}

ZTEST(radiant_ctrl_arbiter, test_an_idle_ant_claim_expires_and_a_command_refreshes_it)
{
	radiant_ctrl_init(CTRL_TIMEOUT_MS, RADIANT_CTRL_GATE_SIMULATION);

	zassert_equal(radiant_ctrl_claim(RADIANT_CTRL_ANT, false, 1000u),
		      0, NULL);

	/* Just short of the timeout: still owned. */
	radiant_ctrl_tick(1000u + CTRL_TIMEOUT_MS - 1u);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_ANT, NULL);

	/* Every command pushes the deadline out, so a controller that keeps
	 * commanding keeps the machine. */
	zassert_equal(radiant_ctrl_claim(RADIANT_CTRL_ANT, false,
					      1000u + CTRL_TIMEOUT_MS - 1u),
		      0, NULL);
	radiant_ctrl_tick(1000u + CTRL_TIMEOUT_MS + 1u);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_ANT,
		      "a refresh must move the deadline, not just succeed");

	/* Stop commanding and it lapses. THIS IS THE ONLY RELEASE AN ANT
	 * CLAIM HAS: ANT+ is connectionless, so a head unit that walks away
	 * generates no event whatsoever. Without this, one page 51 early in a
	 * session would lock a phone out until power cycle. */
	radiant_ctrl_tick(1000u + CTRL_TIMEOUT_MS * 2u + 1u);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_NONE, NULL);

	/* And a phone can now take it with no explicit acquire needed,
	 * because nobody holds it. */
	zassert_equal(radiant_ctrl_claim(RADIANT_CTRL_BLE, false,
					      1000u + CTRL_TIMEOUT_MS * 2u + 2u),
		      0, NULL);
}

ZTEST(radiant_ctrl_arbiter, test_release_hands_the_machine_back_to_ant)
{
	radiant_ctrl_init(CTRL_TIMEOUT_MS, RADIANT_CTRL_GATE_SIMULATION);

	zassert_equal(radiant_ctrl_claim(RADIANT_CTRL_BLE, true, 1000u),
		      0, NULL);
	zassert_equal(radiant_ctrl_claim(RADIANT_CTRL_ANT, false, 1100u),
		      -EACCES, NULL);

	/* An FTMS Reset (0x01) or a disconnect. Both reach here. */
	radiant_ctrl_release(RADIANT_CTRL_BLE);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_NONE, NULL);

	/* The head unit's very next page 51 is applied rather than REJECTED,
	 * with no handshake of its own - which is the point, since FE-C has
	 * none to offer. */
	zassert_equal(radiant_ctrl_claim(RADIANT_CTRL_ANT, false, 1200u),
		      0, NULL);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_ANT, NULL);

	/* A release by somebody who does not hold it is a no-op and not an
	 * error - a disconnect callback should not have to ask first. */
	radiant_ctrl_release(RADIANT_CTRL_BLE);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_ANT, NULL);
}

ZTEST(radiant_ctrl_arbiter, test_the_ant_timeout_survives_an_uptime_wrap)
{
	/* k_uptime_get_32() wraps every 49.7 days and a treadmill in a gym is
	 * powered for months. Signed arithmetic here would make the claim
	 * expire instantly at the wrap; unsigned subtraction yields the true
	 * interval across it. */
	const uint32_t before_wrap = 0xFFFFF000u;

	radiant_ctrl_init(CTRL_TIMEOUT_MS, RADIANT_CTRL_GATE_SIMULATION);
	zassert_equal(radiant_ctrl_claim(RADIANT_CTRL_ANT, false,
					      before_wrap),
		      0, NULL);

	/* 0x2000 counts past the claim, having wrapped through zero. That is
	 * 8192 ms, well inside the 30 s timeout, so the claim must stand. */
	radiant_ctrl_tick(before_wrap + 0x2000u);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_ANT,
		      "an uptime wrap is not an idle timeout");

	/* And past the real deadline it still expires. */
	radiant_ctrl_tick(before_wrap + CTRL_TIMEOUT_MS + 1u);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_NONE, NULL);
}

ZTEST(radiant_ctrl_arbiter, test_reads_are_never_gated_by_ownership)
{
	/* The deck-moving pages, and the reason the token exists. */
	zassert_true(radiant_ctrl_fec_page_is_gated(0x33u),
		     "page 51, Track Resistance - the grade command");
	zassert_true(radiant_ctrl_fec_page_is_gated(0x32u),
		     "page 50, Wind Resistance - the other half of the "
		     "simulation parameter set, gated with page 51 because "
		     "honouring one and refusing the other is how a controller "
		     "decides simulation mode is broken");

	/*
	 * AND THE ONES THAT MUST NOT BE. Refusing a read would make the
	 * machine look broken to a head unit that is merely observing it,
	 * which is the opposite of what the token is for: nothing here is
	 * allowed to degrade what a listener sees.
	 */
	zassert_false(radiant_ctrl_fec_page_is_gated(0x46u),
		      "page 70, Request Data Page - a READ, and refusing it "
		      "would make an observing head unit report the machine as "
		      "faulty");
	zassert_false(radiant_ctrl_fec_page_is_gated(0x37u),
		      "page 55, User Configuration");

	/* 48 and 49 answer NOT_SUPPORTED to everybody. Gating them would
	 * report a missing capability as a missing permission, which tells a
	 * controller to retry something that will never work. */
	zassert_false(radiant_ctrl_fec_page_is_gated(0x30u), "page 48");
	zassert_false(radiant_ctrl_fec_page_is_gated(0x31u), "page 49");

	/* The data pages this machine transmits are not commands at all and
	 * must never appear in the gated set. */
	zassert_false(radiant_ctrl_fec_page_is_gated(16u), NULL);
	zassert_false(radiant_ctrl_fec_page_is_gated(17u), NULL);
	zassert_false(radiant_ctrl_fec_page_is_gated(19u), NULL);
	zassert_false(radiant_ctrl_fec_page_is_gated(0x50u), NULL);
	zassert_false(radiant_ctrl_fec_page_is_gated(0x51u), NULL);
}

ZTEST(radiant_ctrl_arbiter, test_a_trainer_gates_all_four_command_pages)
{
	/* A smart trainer implements basic resistance and target power, so
	 * pages 48 and 49 move it and are gated with 50 and 51. */
	radiant_ctrl_init(CTRL_TIMEOUT_MS,
			  RADIANT_CTRL_GATE(RADIANT_CTRL_PAGE_BASIC_RESISTANCE) |
				  RADIANT_CTRL_GATE(RADIANT_CTRL_PAGE_TARGET_POWER) |
				  RADIANT_CTRL_GATE_SIMULATION);
	zassert_true(radiant_ctrl_fec_page_is_gated(0x30u), "page 48");
	zassert_true(radiant_ctrl_fec_page_is_gated(0x31u), "page 49");
	zassert_true(radiant_ctrl_fec_page_is_gated(0x32u), "page 50");
	zassert_true(radiant_ctrl_fec_page_is_gated(0x33u), "page 51");
	zassert_false(radiant_ctrl_fec_page_is_gated(0x37u), "page 55 is a read");
	zassert_false(radiant_ctrl_fec_page_is_gated(0x46u), "page 70 is a read");
}

ZTEST(radiant_ctrl_arbiter, test_revoke_takes_the_token_from_either_owner)
{
	/* A safety-key pull ends every remote session: the BLE owner is told
	 * (0xFF) by the caller, which is why the previous owner comes back. */
	zassert_equal(radiant_ctrl_claim(RADIANT_CTRL_BLE, true, 1000u), 0, NULL);
	zassert_equal(radiant_ctrl_revoke(), RADIANT_CTRL_BLE, NULL);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_NONE, NULL);

	zassert_equal(radiant_ctrl_claim(RADIANT_CTRL_ANT, false, 2000u), 0, NULL);
	zassert_equal(radiant_ctrl_revoke(), RADIANT_CTRL_ANT, NULL);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_NONE, NULL);

	/* Nothing held: still a no-op answer. */
	zassert_equal(radiant_ctrl_revoke(), RADIANT_CTRL_NONE, NULL);
}

ZTEST_SUITE(radiant_ctrl_arbiter, NULL, NULL, arbiter_reset, NULL, NULL);
