/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The bridge's host trace and key wipe with RadiANT security COMPILED OUT:
 * no host security messages, no pairing. A 0xF2 or 0xF5 sent to such a build
 * is refused, but its bytes still crossed the parser and the trace, so they
 * are masked and wiped exactly as in test_bridge_trace_sec.c. The cases are in
 * bridge_trace_cases.h.
 */

#undef CONFIG_RADIANT_SEC_HOST_MESSAGES
#undef CONFIG_RADIANT_SEC_PAIRING_X25519

#include "bridge_trace_cases.h"

ZTEST_SUITE(bridge_trace, NULL, NULL, trace_before, NULL, NULL);

ZTEST(bridge_trace, test_a_network_key_shows_only_its_network_number)
{
	check_network_key_masked();
}

ZTEST(bridge_trace, test_an_encrypt_key_shows_only_its_key_number)
{
	check_encrypt_key_masked();
}

ZTEST(bridge_trace, test_a_radiant_root_key_shows_only_channel_and_length)
{
	check_radiant_key_masked();
}

ZTEST(bridge_trace, test_a_pairing_message_shows_only_its_header)
{
	check_pairing_masked();
}

ZTEST(bridge_trace, test_an_outbound_pairing_reply_is_masked_too)
{
	check_outbound_pairing_masked();
}

ZTEST(bridge_trace, test_a_plain_frame_is_traced_whole_and_kept)
{
	check_plain_frame_whole();
}

ZTEST(bridge_trace, test_every_key_body_is_wiped_from_the_parser)
{
	check_key_bodies_wiped();
}

ZTEST(bridge_trace, test_a_key_frame_with_a_bad_checksum_is_wiped)
{
	check_bad_checksum_key_wiped();
}

ZTEST(bridge_trace, test_any_frame_that_fails_its_checksum_is_wiped)
{
	check_bad_checksum_any_id_wiped();
}

ZTEST(bridge_trace, test_an_rng_seed_shows_only_its_type)
{
	check_rng_seed_masked_and_wiped();
}

ZTEST(bridge_trace, test_a_crypto_id_info_is_traced_whole_and_kept)
{
	check_crypto_id_info_whole();
}

ZTEST(bridge_trace, test_a_128_bit_network_key_shows_only_its_network_number)
{
	check_network_key_128_masked();
}

ZTEST(bridge_trace, test_an_nvm_crypto_key_op_shows_only_operation_and_index)
{
	check_crypto_key_nvm_masked();
}

ZTEST(bridge_trace, test_a_checksum_error_logs_the_id_not_the_checksum)
{
	check_checksum_error_line_holds_no_checksum();
}
