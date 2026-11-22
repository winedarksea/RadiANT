/* SPDX-License-Identifier: Apache-2.0 */
/*
 * A compat context goes with its channel. radiant_ant_channel_unassign() and
 * radiant_ant_stack_reset() (SYSTEM_RESET) used to release only the radiant_sec
 * context, so a compat context on an unassigned channel stayed installed with
 * its epoch. radiant_sec_compat_live_twin() then kept reporting it, and
 * radiant_node_ident_arm_compat() refused every later arm under the same root and
 * base device number with RADIANT_NODE_IDENT_EBUSY until reboot (review 2026-11-15).
 *
 * Compiled only into scenarios with CONFIG_RADIANT_SEC_COMPAT (sec_host).
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_RADIANT_SEC_COMPAT)

#include <zephyr/ztest.h>

#include <radiant/radiant_ant.h>
#include <radiant/radiant_wire.h>
#include <radiant/radiant_sec.h>
#include <radiant/radiant_sec_compat.h>

/* test_api.c: the api suite's end-of-case violation check. */
void api_assert_no_viol(void);

#define CA      0u
#define CB      1u
#define DEVNUM  0x3A17u
#define PERIOD  8070u

static const uint8_t root[16] = {
	0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
	0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f,
};

static const uint8_t net_key[8] = {
	0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45,
};

static void assign(uint8_t ch)
{
	zassert_equal((radiant_ant_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_ant_channel_assign(ch,
					  (uint8_t)RADIANT_WIRE_CHANNEL_TYPE_MASTER,
					  0u, 0u));
}

/* Both channels keyed under one root and one base number, A running. Starts
 * from no compat state at all, so a leak from the other test (with one of the
 * two releases reverted) fails only the test that covers that release. */
static void key_both_run_a(void)
{
	radiant_sec_compat_reset();
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_key(CA, root, 128u, DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_key(CB, root, 128u, DEVNUM));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_compat_set_epoch(CA, 7u, 0u, 0u, PERIOD));
	zassert_true(radiant_sec_compat_live_twin(CB),
		     "precondition: A running under B's root and number is "
		     "B's live twin");
}

ZTEST(api, test_sec_compat_unassign_releases_the_compat_context)
{
	zassert_equal((radiant_ant_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_ant_network_address_set(0u, net_key));
	assign(CA);
	assign(CB);
	key_both_run_a();

	zassert_equal((radiant_ant_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_ant_channel_unassign(CA));

	zassert_false(radiant_sec_compat_live_twin(CB),
		      "an unassigned channel's compat context still counts as "
		      "a live twin: node_ident's next arm is EBUSY until reboot");
	zassert_equal(RADIANT_SEC_ENOKEY,
		      radiant_sec_compat_set_epoch(CA, 8u, 0u, 0u, PERIOD),
		      "the compat key outlived radiant_ant_channel_unassign()");

	(void)radiant_ant_channel_unassign(CB);

	api_assert_no_viol();
}

ZTEST(api, test_sec_compat_system_reset_releases_every_compat_context)
{
	zassert_equal((radiant_ant_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_ant_network_address_set(0u, net_key));
	assign(CA);
	assign(CB);
	key_both_run_a();

	zassert_equal((radiant_ant_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR, radiant_ant_stack_reset());

	zassert_equal(RADIANT_SEC_ENOKEY,
		      radiant_sec_compat_set_epoch(CA, 8u, 0u, 0u, PERIOD),
		      "channel A's compat key outlived SYSTEM_RESET");
	zassert_equal(RADIANT_SEC_ENOKEY,
		      radiant_sec_compat_set_epoch(CB, 8u, 0u, 0u, PERIOD),
		      "channel B's compat key outlived SYSTEM_RESET");

	api_assert_no_viol();
}

#endif /* CONFIG_RADIANT_SEC_COMPAT */
