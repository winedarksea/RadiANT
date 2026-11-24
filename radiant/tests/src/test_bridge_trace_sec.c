/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The bridge's host trace and key wipe with RadiANT host security messages
 * and pairing COMPILED IN, as on a security dongle image. The application's
 * own configuration has neither (CONFIG_RADIANT_SEC_HOST_MESSAGES depends on
 * the host adapter, which this application does not build), so they are set
 * here for this translation unit, the way test_bridge_parser.c sets the
 * health counters. The cases are in bridge_trace_cases.h; test_bridge_trace.c
 * runs them with security compiled out.
 */

#ifndef CONFIG_RADIANT_SEC_HOST_MESSAGES
#define CONFIG_RADIANT_SEC_HOST_MESSAGES 1
#endif
#ifndef CONFIG_RADIANT_SEC_PAIRING_X25519
#define CONFIG_RADIANT_SEC_PAIRING_X25519 1
#endif

#include "bridge_trace_cases.h"

ZTEST_SUITE(bridge_trace_sec, NULL, NULL, trace_before, NULL, NULL);

ZTEST(bridge_trace_sec, test_a_network_key_shows_only_its_network_number)
{
	check_network_key_masked();
}

ZTEST(bridge_trace_sec, test_an_encrypt_key_shows_only_its_key_number)
{
	check_encrypt_key_masked();
}

ZTEST(bridge_trace_sec, test_a_radiant_root_key_shows_only_channel_and_length)
{
	check_radiant_key_masked();
}

ZTEST(bridge_trace_sec, test_a_pairing_message_shows_only_its_header)
{
	check_pairing_masked();
}

ZTEST(bridge_trace_sec, test_an_outbound_pairing_reply_is_masked_too)
{
	check_outbound_pairing_masked();
}

ZTEST(bridge_trace_sec, test_a_plain_frame_is_traced_whole_and_kept)
{
	check_plain_frame_whole();
}

ZTEST(bridge_trace_sec, test_every_key_body_is_wiped_from_the_parser)
{
	check_key_bodies_wiped();
}

ZTEST(bridge_trace_sec, test_a_key_frame_with_a_bad_checksum_is_wiped)
{
	check_bad_checksum_key_wiped();
}

ZTEST(bridge_trace_sec, test_any_frame_that_fails_its_checksum_is_wiped)
{
	check_bad_checksum_any_id_wiped();
}

ZTEST(bridge_trace_sec, test_an_rng_seed_shows_only_its_type)
{
	check_rng_seed_masked_and_wiped();
}

ZTEST(bridge_trace_sec, test_a_crypto_id_info_is_traced_whole_and_kept)
{
	check_crypto_id_info_whole();
}

ZTEST(bridge_trace_sec, test_a_128_bit_network_key_shows_only_its_network_number)
{
	check_network_key_128_masked();
}

ZTEST(bridge_trace_sec, test_an_nvm_crypto_key_op_shows_only_operation_and_index)
{
	check_crypto_key_nvm_masked();
}

ZTEST(bridge_trace_sec, test_a_checksum_error_logs_the_id_not_the_checksum)
{
	check_checksum_error_line_holds_no_checksum();
}
