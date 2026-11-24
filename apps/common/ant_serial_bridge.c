/* SPDX-License-Identifier: Apache-2.0 */

/*
 * ANT Serial Bridge — translates the ANT USB serial protocol (0xA4-framed
 * messages) to/from the radio contract in radiant_ant.h.
 *
 * Inbound (host → chip):
 *   Parser thread drains ant_rx_ring_buf byte by byte through a state
 *   machine, validates the XOR checksum, and calls the matching radiant_ant_*
 *   function. Each command gets an RADIANT_WIRE_MESG_RESPONSE_EVENT_ID (0x40) ack.
 *
 * Outbound (chip → host):
 *   The radio backend calls radiant_ant_on_message() - implemented here - for every
 *   message the host should see; it is wrapped in a serial frame and pushed
 *   to usb_ant_send(). No registration step: the symbol resolves at link
 *   time, so ant_serial_bridge_init() only starts the parser thread.
 *
 * Wire frame format (ANT Serial Protocol):
 *   [SYNC=0xA4] [LEN] [ID] [payload: LEN bytes] [XOR checksum]
 *
 * The checksum is the XOR of every preceding byte INCLUDING the SYNC byte.
 * Omitting SYNC yields a value off by exactly 0xA4, so two implementations
 * that both omit it agree with each other and with nobody else - how that
 * bug once survived a passing test suite.
 *
 * Nothing here names a symbol from sdk-ant: protocol constants come from
 * <radiant/radiant_wire.h> (RADIANT_WIRE_*) and every radio call goes through <radiant/radiant_ant.h>
 * (radiant_ant_*), which is what lets this file build with no sdk-ant present.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>

#include "ant_transport.h"

#include "ant_activity.h"
#include "ant_health.h"
#if defined(CONFIG_ANT_DONGLE_DIAG)
#include <radiant/radiant_diag.h>
#endif
#include <radiant/radiant_ant.h>
#ifdef CONFIG_RADIANT_SEC_PAIRING_X25519
#include <radiant/radiant_sec.h>
#endif
#include "ant_reset_cause.h"
#include "ant_serial_bridge.h"
#include <radiant/radiant_wire.h>

LOG_MODULE_REGISTER(ant_bridge, LOG_LEVEL_INF);

/*
 * ANT_BRIDGE_PARSER_ONLY: radiant/tests/src/test_bridge_parser.c includes this
 * file with that defined, which compiles the host trace and the host-frame
 * parser below and nothing else; the test supplies dispatch() and the health
 * counter. Never defined in an image.
 */

/* ── Key-carrying messages ───────────────────────────────────────────────── */

#define KEY_MSG_NONE 0xFFU

/* Where the checksum-error line goes; the trace test captures it. */
#ifndef ANT_BRIDGE_CSUM_WRN
#define ANT_BRIDGE_CSUM_WRN(...) LOG_WRN(__VA_ARGS__)
#endif

/*
 * KEY_MSG_NONE for a message that carries no key material. For one that does,
 * how many leading body bytes are not key and may be shown: the network
 * number of a network key (0x46, and 0x76, its 128-bit form: Rev 5.1 sec
 * 9.5.2.25), the key number of an encryption key (0x7E), the operation and
 * NVM index of an NVM crypto key operation (0x83, Rev 5.1 sec 9.5.2.36: a
 * store carries a key, and its operation byte is not trusted to say which
 * form a frame is), the channel and key length of a RadiANT root key
 * (0xF2), the info type of an encryption-info message that sets the RNG
 * seed (0x7F type 0x02; other 0x7F types are not secret), and nothing of a
 * pairing message (0xF5), whose sub-command 0x02 carries the host's private
 * X25519 scalar. 0xF5 is judged by id alone, so no sub-command can leak by
 * being classified wrongly.
 *
 * Deliberately independent of the build's configuration: a 0xF2 or 0x7E sent
 * to a build that refuses it still crossed the parser buffer and the trace
 * byte for byte; 0x76 and 0x83 are never bridged at all, for the same
 * reason. Used by the host trace and by the parser-buffer wipe.
 * `body`/`len` are the message body (channel or type byte first).
 *
 * Not inline: inlined, the parser grew by 188 B of text on the Pro Micro
 * image; out of line the same code costs 72 B.
 */
static __noinline uint8_t key_msg_keep(uint8_t id, const uint8_t *body,
				       size_t len)
{
	switch (id) {
	case RADIANT_WIRE_MESG_NETWORK_KEY_ID:
	case RADIANT_WIRE_MESG_SET_128BIT_NETWORK_KEY_ID:
	case RADIANT_WIRE_MESG_SET_ENCRYPT_KEY_ID:
		return 1;
	case RADIANT_WIRE_MESG_SET_ENCRYPT_INFO_ID:
		return (len >= 1 && body[0] == RADIANT_WIRE_ENCRYPTION_INFO_SET_RNG_SEED)
			? 1 : KEY_MSG_NONE;
	case RADIANT_WIRE_MESG_CRYPTO_KEY_NVM_OP_ID:
	case RADIANT_WIRE_MESG_RADIANT_SET_KEY_ID:
		return 2;
	case RADIANT_WIRE_MESG_RADIANT_PAIRING_ID:
		return 0;
	default:
		return KEY_MSG_NONE;
	}
}

/* ── Exact-length messages ───────────────────────────────────────────────── */

/*
 * False for a body whose length differs from the one protocol/ant_wire.yaml
 * gives a message with no optional bytes: 0x46 network key (9), 0xF1 security
 * config (5), 0xF2 root key (18) and 0xF3 epoch (14). dispatch() refuses such
 * a body with INVALID_MESSAGE before any arm runs, as 0xF5 already refuses an
 * over-long one, rather than acting on its prefix (security review
 * 2026-11-22, Hardening). Messages with optional trailing bytes (0x42's
 * extended assignment, 0x5E's stall fields) are not listed and keep their
 * minimum-length checks in dispatch(). Outside ANT_BRIDGE_PARSER_ONLY so
 * test_bridge_parser.c can reach it.
 */
static inline bool body_len_ok(uint8_t id, uint8_t len)
{
	switch (id) {
	case RADIANT_WIRE_MESG_NETWORK_KEY_ID:
		return len == 9U;
	case RADIANT_WIRE_MESG_RADIANT_SEC_CONFIG_ID:
		return len == 5U;
	case RADIANT_WIRE_MESG_RADIANT_SET_KEY_ID:
		return len == 18U;
	case RADIANT_WIRE_MESG_RADIANT_EPOCH_ID:
		return len == 14U;
	default:
		return true;
	}
}

/* ── Host trace (CONFIG_ANT_DONGLE_HOST_TRACE) ─────────────────────────────── */

/*
 * One log line per whole frame, both directions: "H>" host to dongle, "<D"
 * dongle to host. For learning what an unfamiliar host (the Android ANT USB
 * Service, say) sends on a board with a console, where no USB capture of the
 * host side is possible. Off by default and never in a release: at 115200 baud
 * a line costs ~5 ms of log time.
 *
 * Key material is masked, not dropped, so the frame shape stays visible: a
 * key-carrying message (key_msg_keep()) keeps its header and non-key bytes,
 * and loses its key and its checksum. The checksum goes too because, with the
 * header known, it is the XOR of the key bytes.
 */
#ifdef CONFIG_ANT_DONGLE_HOST_TRACE
/* Where a trace line goes. The parser test (ANT_BRIDGE_PARSER_ONLY) defines
 * it to capture the line; an image logs it. */
#ifndef ANT_BRIDGE_TRACE_EMIT
#define ANT_BRIDGE_TRACE_EMIT(dir, line) LOG_INF("%s %s", (dir), (line))
#endif

static void trace_frame(const char *dir, const uint8_t *f, size_t n)
{
	static const char hex[] = "0123456789abcdef";
	char line[3 * RADIANT_WIRE_MAX_FRAME_SIZE + 1];
	size_t redact_from = n;
	size_t o = 0;

	if (n >= 3) {
		uint8_t keep = key_msg_keep(f[2], &f[3], n > 4 ? n - 4 : 0);

		if (keep != KEY_MSG_NONE) {
			redact_from = 3U + keep;	/* SYNC LEN ID kept */
		}
	}

	for (size_t i = 0; i < n && i < RADIANT_WIRE_MAX_FRAME_SIZE; i++) {
		bool mask = i >= redact_from;	/* checksum included */

		line[o++] = mask ? 'x' : hex[f[i] >> 4];
		line[o++] = mask ? 'x' : hex[f[i] & 0xF];
		line[o++] = ' ';
	}
	line[o ? o - 1 : 0] = '\0';
	ANT_BRIDGE_TRACE_EMIT(dir, line);
}
#define TRACE_TX(buf, n) trace_frame("<D", (buf), (n))
#else
#define TRACE_TX(buf, n) do { } while (0)
#endif

#if !defined(ANT_BRIDGE_PARSER_ONLY)

/* sdk-ant's Kconfig hangs this symbol off CONFIG_ANT, so on a radio-stub build
 * (CONFIG_ANT=n) it does not exist at all and the device-wide transmit power
 * fan-out below fails to compile. A core (radiant) build never defines it
 * either, yet its 0x54 reply advertises 32 channels - so bound the per-channel
 * tables and loops by the protocol's own ceiling, the five-bit channel field
 * of a burst header. A backend allocating fewer answers the extra iterations
 * with an error, which every loop below already ignores. The sdk_ant build
 * defines the symbol (8) and is unchanged.
 */
#ifndef CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED
#define CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED (RADIANT_WIRE_BURST_HEADER_CHANNEL_MASK + 1)
#endif

/* The transport - USB on either stack, or a bare UART - is declared in
 * ant_transport.h and selected at build time. Nothing below depends on which.
 */

/* ── Bridge thread ─────────────────────────────────────────────────────────── */

/* Every host command runs on this stack, 0xF5 pairing included. One Kconfig
 * symbol (apps/common/Kconfig.bridge), shared with the radiant/tests/api test
 * that runs a whole 0xF5 exchange on a thread of this size. */
#define BRIDGE_STACK_SIZE  CONFIG_ANT_DONGLE_BRIDGE_STACK_SIZE

/* Taken from <radiant/radiant_ant.h>, not respelled here, so this and a backend's
 * event thread (which must sit below it - see RADIANT_ANT_HOST_THREAD_PRIORITY)
 * stay consistent by construction.
 */
#define BRIDGE_PRIORITY    RADIANT_ANT_HOST_THREAD_PRIORITY

#ifdef CONFIG_RADIANT_SEC_PAIRING_X25519
/*
 * The deepest chain in the image: bridge_thread_fn() (dispatch is inlined
 * into it) -> radiant_ant_sec_pair() -> the radiant_sec_pair_* call. Measured on
 * nRF52840 at -Os (2026-11-17): bridge_thread_fn 144 B + radiant_ant_sec_pair 40 B,
 * so 256 B covers the bridge's share; the reserve covers an exception frame
 * with FP context (104 B) and the thread entry, with room to spare. Until the
 * 2026-11-17 security review the X25519 alone needed 2,636 B here and
 * overran this 2,048 B stack; scripts/check_stack_budget.py measures the
 * chain itself from the compiler's .su files in CI.
 */
#define BRIDGE_OWN_FRAMES_BYTES 256
#define BRIDGE_STACK_RESERVE    256
BUILD_ASSERT(BRIDGE_STACK_SIZE >= RADIANT_SEC_PAIR_STACK_BYTES +
				  BRIDGE_OWN_FRAMES_BYTES +
				  BRIDGE_STACK_RESERVE,
	     "CONFIG_ANT_DONGLE_BRIDGE_STACK_SIZE cannot hold a 0xF5 pairing "
	     "exchange (RADIANT_SEC_PAIR_STACK_BYTES)");
#endif

K_THREAD_STACK_DEFINE(bridge_stack, BRIDGE_STACK_SIZE);
static struct k_thread bridge_thread_data;

/*
 * Free bridge stack after a 0xF5 command, the deepest thing this thread runs.
 * Needs the 0xAA fill (CONFIG_INIT_STACKS) to mean anything, so it is a bench
 * aid, not a release feature; the number is what tools/ant_pair_bench.py's
 * high-water criterion is read against.
 */
#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519) && defined(CONFIG_INIT_STACKS) && \
	defined(CONFIG_THREAD_STACK_INFO)
static void log_pair_stack_high_water(void)
{
	size_t unused = 0u;
	size_t size = k_current_get()->stack_info.size;

	if (k_thread_stack_space_get(k_current_get(), &unused) == 0) {
		LOG_INF("bridge: 0xF5 stack high-water %u of %u bytes",
			(unsigned int)(size - unused), (unsigned int)size);
	}
}
#elif defined(CONFIG_RADIANT_SEC_PAIRING_X25519)
static inline void log_pair_stack_high_water(void)
{
}
#endif

/* Set while a system reset is tearing channels down, so the resulting
 * EVENT_CHANNEL_CLOSED storm is not forwarded to the host. A real stick emits
 * nothing between the reset command and the startup message.
 */
static atomic_t reset_in_progress;

/* How many radiant_ant_on_message() calls are past the activity hooks and not yet
 * returned. handle_system_reset() waits for it to reach zero; see there.
 * An atomic and not a lock, so radiant_ant_on_message() stays legal from any context
 * a backend calls it from.
 */
static atomic_t evt_inflight;

/* handle_system_reset()'s bounds on the two fences. Both are far above what
 * either takes when nothing is wrong, and exist only so a wedged event path
 * or host cannot hang the reset. */
#define RESET_EVT_FENCE_MS 10
#define RESET_TX_FENCE_MS  100

/* ── Frame helpers ─────────────────────────────────────────────────────────── */

/* Build and transmit an RADIANT_WIRE_MESG_RESPONSE_EVENT_ID (0x40) reply. */
static void send_response(uint8_t ch, uint8_t msg_id, uint8_t code)
{
	uint8_t buf[7];
	buf[0] = RADIANT_WIRE_SYNC_TX;                  /* 0xA4 */
	buf[1] = RADIANT_WIRE_MESG_RESPONSE_EVENT_SIZE; /* 3 */
	buf[2] = RADIANT_WIRE_MESG_RESPONSE_EVENT_ID;   /* 0x40 */
	buf[3] = ch;
	buf[4] = msg_id;
	buf[5] = code;
	buf[6] = buf[0] ^ buf[1] ^ buf[2] ^ buf[3] ^ buf[4] ^ buf[5];
	TRACE_TX(buf, sizeof(buf));
	usb_ant_send(buf, sizeof(buf));
}

/* The startup message, with its reason byte.
 *
 * Two callers. The RADIANT_WIRE_MESG_SYSTEM_RESET_ID arm of dispatch() sends
 * RADIANT_WIRE_STARTUP_COMMAND_RESET (0x20) after radiant_ant_stack_reset(), and that is the
 * only reason it ever reports for a reset the host asked for.
 * ant_serial_bridge_announce_boot() sends the hardware reset cause once at
 * boot, on UART builds only - see there for why.
 *
 * On the commanded reset's 0x20, not zero: sending 0x00
 * (RADIANT_WIRE_STARTUP_POWER_ON_RESET) after a commanded reset told every host
 * the stick had just cold-booted, which is exactly what a host watches this
 * byte to rule out: it is how an unexpected brown-out or watchdog reset is
 * told apart from the reset the host itself asked for.
 *
 * On the evidence, stated exactly, because it is weaker than it first looks.
 * There is NO observed retail-stick capture of this frame in this repository,
 * and the 0x20 vectors are not one: tools/test_ant_wire.py tags "startup
 * message, command reset" (a4016f20ea) `hand:`, not `observed:`. What does
 * support 0x20:
 *
 *   - Garmin's own header. ant_radio_sdk_ant.c asserts
 *     WIRE_EQ(STARTUP_COMMAND_RESET, RESET_CMD) and
 *     WIRE_EQ(STARTUP_POWER_ON_RESET, RESET_POR), so the vendor's own SDK
 *     names 0x20 the commanded reset and 0x00 the power-on one. That is the
 *     field's meaning from the source that defines it.
 *   - Every host-side fixture here independently encodes 0x20 as the answer to
 *     0x4A: tools/test_ant_wire.py, tools/test_ant_trace.py,
 *     tools/test_ant_conformance.py, tools/vectors/handmade-session.antser,
 *     and the worked example in archive/captures/serial/README.md.
 *
 * The three transcripts in archive/captures/serial/ all show a4016f00ca and
 * are NOT evidence against this: the reason byte is hardcoded here, in the
 * shared bridge, so all three recorded this one line rather than anything
 * their backends decided - conformance-sdk-ant.antser included.
 *
 * Nothing ever compared those fixtures against the firmware, which is how 745
 * host tests passed while every Zwift capture in captures/ shows a4016f00ca.
 * Re-record the transcripts after this change: they now differ by this frame,
 * and only by this frame.
 */
/* `queued`: through the writer thread's queue, behind whatever is already in
 * it, rather than as a direct write that may overtake those frames (see
 * handle_system_reset()). A queue that refuses it falls back to the direct
 * write: a lost STARTUP is worse than a misordered one. */
static void send_startup(uint8_t reason, bool queued)
{
	uint8_t buf[5];
	buf[0] = RADIANT_WIRE_SYNC_TX;
	buf[1] = RADIANT_WIRE_MESG_STARTUP_MESG_SIZE;  /* 1 */
	buf[2] = RADIANT_WIRE_MESG_STARTUP_MESG_ID;    /* 0x6F */
	buf[3] = reason;
	buf[4] = buf[0] ^ buf[1] ^ buf[2] ^ buf[3];
	TRACE_TX(buf, sizeof(buf));
	if (queued && usb_ant_send_async(buf, sizeof(buf)) == 0) {
		return;
	}
	usb_ant_send(buf, sizeof(buf));
}

/*
 * The unsolicited STARTUP a real ANT part sends when it comes out of reset,
 * with the hardware cause as its reason byte (ant_reset_cause.h has the
 * mapping). Only on a UART transport, where it is the one thing that tells a
 * host the board rebooted:
 *
 *   - A UART has no session boundary. On the CC2652P dongle the CP2102N keeps
 *     the host's USB handle open straight through an MCU reboot - exactly the
 *     shape of the Dynastream USB2 stick (an AP2 behind a CP210x), which does
 *     send this - so without it the host sees only its channels going quiet.
 *     The same is true of a DK's VCOM, which is how a 54L15 firmware fault was
 *     indistinguishable from a probe fault (TODO.md, 2026-11-07).
 *   - A USB build needs none of it: a reboot drops the device off the bus, the
 *     host must reopen, and the reopen's own SYSTEM_RESET gets its STARTUP.
 *
 * Safe against a host that is mid-reset: frames go out whole under the
 * transport's TX lock, and the bridge handles the host's commands in order, so
 * at worst a host's reset-wait is satisfied by this frame a few milliseconds
 * early and its next command still runs after the reset. Host tools flush the
 * port on open (tools/ant_probe.py SerialDevice) so a stale one cannot.
 */
void ant_serial_bridge_announce_boot(void)
{
#if defined(CONFIG_ANT_DONGLE_TRANSPORT_UART)
	uint32_t cause;

	(void)ant_reset_cause(&cause);
	send_startup(ant_reset_cause_startup_reason(cause), false);
#endif
}

static void send_message(uint8_t msg_id, const uint8_t *payload, uint8_t len)
{
	/* RADIANT_WIRE_MAX_FRAME_SIZE is RADIANT_WIRE_MAX_SIZE_VALUE + RADIANT_WIRE_MSG_OVERHEAD, the
	 * largest frame that can legally exist. Never RADIANT_WIRE_MESG_MAX_SIZE: that
	 * is built from RADIANT_WIRE_MAX_DATA_SIZE, which does not count the leading
	 * channel byte the LEN byte does, so it is one short and a full-length
	 * message would write its checksum past the end.
	 */
	uint8_t buf[RADIANT_WIRE_MAX_FRAME_SIZE];
	uint8_t xor = RADIANT_WIRE_SYNC_TX ^ len ^ msg_id;

	buf[0] = RADIANT_WIRE_SYNC_TX;
	buf[1] = len;
	buf[2] = msg_id;

	for (uint8_t index = 0; index < len; index++) {
		buf[3 + index] = payload[index];
		xor ^= payload[index];
	}

	buf[3 + len] = xor;
	TRACE_TX(buf, len + 4U);
	usb_ant_send(buf, len + 4U);
}

/* ── Transmit power ────────────────────────────────────────────────────────── */

/* The boosted level: "the most this radio has". A capability rule, not a SoC
 * list - the list this replaced named three nRF52 parts and held everything
 * else at LVL_4, which left the nRF54L15 at +4 dBm though it has +8.
 *
 *   - radiant (and the stub): always LVL_5. radiant_channel_tx_power_set()
 *     accepts it on every part and the backend rounds it to what the part can
 *     do - nearest table entry on nRF (+8 on nRF52840 and nRF54L15, 0 on the
 *     nRF5340 network core), highest entry not above it on TI (+5 on CC26xx).
 *   - sdk-ant: LVL_5 only where sdk-ant accepts it, the nRF52820/33/40; it
 *     refuses the level elsewhere, so other parts keep LVL_4 there.
 */
#if defined(CONFIG_ANT_DONGLE_RADIO_SDK_ANT) && \
	!(defined(CONFIG_SOC_NRF52840) || defined(CONFIG_SOC_NRF52833) || \
	  defined(CONFIG_SOC_NRF52820))
#define ANT_DONGLE_TX_POWER_MAX RADIANT_WIRE_RADIO_TX_POWER_LVL_4
#else
#define ANT_DONGLE_TX_POWER_MAX RADIANT_WIRE_RADIO_TX_POWER_LVL_5
#endif

/* What a channel transmits at when the host never says (and after every
 * SYSTEM_RESET). Zwift is this case: its one MESG_RADIO_TX_POWER is wiped by
 * the SYSTEM_RESET that follows it, so it always runs at this value.
 */
#if defined(CONFIG_ANT_DONGLE_TX_POWER_BOOST)
#define ANT_DONGLE_TX_POWER_DEFAULT ANT_DONGLE_TX_POWER_MAX
#else
#define ANT_DONGLE_TX_POWER_DEFAULT RADIANT_WIRE_RADIO_TX_POWER_LVL_3
#endif

/* What each channel should transmit at, whether or not it is assigned yet.
 * radiant_ant_channel_radio_tx_power_set() only takes on an assigned channel (why
 * the 0x47 fan-out below swallows its errors), and hosts set power before
 * assigning - holding it here and reapplying after each assign survives
 * that ordering.
 */
static uint8_t tx_power[CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED];

/* Raw register value paired with RADIANT_WIRE_RADIO_TX_POWER_LVL_CUSTOM, held per
 * channel for the same reason. Meaningless unless the matching tx_power[]
 * entry has the custom bit set, and not portable across parts.
 */
static uint8_t tx_power_custom[CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED];

static void tx_power_reset(void)
{
	for (uint8_t i = 0; i < ARRAY_SIZE(tx_power); i++) {
		tx_power[i] = ANT_DONGLE_TX_POWER_DEFAULT;
		tx_power_custom[i] = 0U;
	}
}

/*
 * Map a host's requested power level onto what this radio can actually do.
 * Only level 4 (+4 dBm, the ceiling on the impersonated nRF51422) is raised,
 * to the boosted maximum: a host asking for it wants everything the hardware
 * has, and no retail stick ever had LVL_5 to request by name, so this is the
 * only way a host can reach the extra dB on purpose.
 *
 * Every other level is honoured as asked - 0-2 (-20/-12/-4 dBm) as a
 * deliberate request to be quiet, and an EXPLICIT level 3 as a true 0 dBm.
 * Level 3 used to be raised too, which made "0 dBm" unrequestable: the
 * 2026-11-07 sensitivity ladder's 0 dBm rung on the 54L15 was really +4 and
 * failed its dial check. A host that never sets power is not affected by
 * this - it gets the boosted default from tx_power_reset(), not level 3.
 * RADIANT_WIRE_RADIO_TX_POWER_LVL_CUSTOM (0x80) names a raw register value, passed
 * through untouched.
 */
static uint8_t tx_power_resolve(uint8_t requested)
{
#if defined(CONFIG_ANT_DONGLE_TX_POWER_BOOST)
	if (requested == RADIANT_WIRE_RADIO_TX_POWER_LVL_4) {
		return ANT_DONGLE_TX_POWER_MAX;
	}
#endif
	return requested;
}

static void tx_power_apply(uint8_t ch)
{
	if (ch < ARRAY_SIZE(tx_power)) {
		(void)radiant_ant_channel_radio_tx_power_set(ch, tx_power[ch],
						      tx_power_custom[ch]);
	}
}

/* ── Request sub-handler (opcode 0x4D) ─────────────────────────────────────── */

static void handle_request(const uint8_t *body, uint8_t len)
{
	radiant_ant_err_t err = 0;
	uint8_t payload[RADIANT_WIRE_MAX_SIZE_VALUE] = {0};
	uint16_t value16;

	if (len < 2) {
		send_response(0, RADIANT_WIRE_MESG_REQUEST_ID, RADIANT_WIRE_INVALID_MESSAGE);
		return;
	}
	uint8_t ch     = body[0];
	uint8_t req_id = body[1];

	switch (req_id) {
#if defined(CONFIG_ANT_DONGLE_HEALTH_COUNTERS)
	case RADIANT_WIRE_MESG_RADIANT_HEALTH_ID: {
		/*
		 * REQUEST-ONLY, which is the whole reason it is safe to add at
		 * all: nothing emits 0xF6 unsolicited, so a host that never
		 * asks sees a byte stream identical to a build without the
		 * feature - and tools/ab_gates.toml requires the conformance
		 * transcripts to stay byte-identical.
		 *
		 * The channel byte is ignored: everything counted is
		 * device-wide, and answering per-channel would imply a
		 * partition of the counters that does not exist.
		 */
		size_t n = ant_health_fill(payload, sizeof(payload));

		if (n == 0u) {
			err = (radiant_ant_err_t)RADIANT_WIRE_INVALID_MESSAGE;
			break;
		}
		send_message(RADIANT_WIRE_MESG_RADIANT_HEALTH_ID, payload, (uint8_t)n);
		break;
	}
#endif
#if defined(CONFIG_ANT_DONGLE_DIAG)
	case RADIANT_WIRE_MESG_RADIANT_DIAG_ID:
		/*
		 * Request-only, like 0xF6 beside it, and for the same reason:
		 * a host that never asks sees an unchanged byte stream. The
		 * channel byte is the PAGE (ADR 0023), not a channel; every
		 * page is device-wide.
		 */
		err = radiant_ant_diag_page(ch, payload);
		if (err == (radiant_ant_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR) {
			send_message(RADIANT_WIRE_MESG_RADIANT_DIAG_ID, payload,
				     (uint8_t)RADIANT_DIAG_BODY_LEN);
		}
		break;
#endif

	case RADIANT_WIRE_MESG_CAPABILITIES_ID: {
		err = radiant_ant_capabilities_get(payload);
#ifdef ANT_BRIDGE_SELF_CHANNEL_LOWEST
		/*
		 * Do not offer the host the channels the self channels own. A host
		 * SYSTEM_RESET wipes them, and the bridge takes them back on its
		 * next poll, 0-1 s later; a host that ASSIGNs one in that gap wins
		 * it and the bridge lets the bound sensor go, and one that is later
		 * is refused CHANNEL_IN_WRONG_STATE (review 2026-11-05 item 2).
		 * That race is left as designed in self_channels.c. What is fixed
		 * is that a conforming host - one that allocates from 0 below the
		 * max it is told - is told the truth and never enters it.
		 *
		 * Here, not in the backend: this is the host boundary, it covers
		 * radiant and sdk-ant alike, and the shared backend (and its
		 * tests/api ztest) stays the same on every image.
		 */
		BUILD_ASSERT(ANT_BRIDGE_SELF_CHANNEL_LOWEST > 0,
			     "the self channels would leave the host no channel");
		if (!err) {
			payload[RADIANT_WIRE_CAPABILITIES_OFFSET_MAX_CHANNELS] =
				MIN(payload[RADIANT_WIRE_CAPABILITIES_OFFSET_MAX_CHANNELS],
				    (uint8_t)ANT_BRIDGE_SELF_CHANNEL_LOWEST);
		}
#endif
		if (!err) {
			send_message(RADIANT_WIRE_MESG_CAPABILITIES_ID, payload,
				     RADIANT_WIRE_MESG_CAPABILITIES_SIZE);
		}
		break;
	}

	case RADIANT_WIRE_MESG_GET_SERIAL_NUM_ID:
	case RADIANT_WIRE_MESG_SERIAL_NUM_EXT_ID: {
		/*
		 * Both answered with the same 4-byte little-endian serial. 0xC6
		 * appends one 0x00, the byte a real Garmin stick sends there and
		 * whose meaning is unknown (protocol/ant_wire.yaml). Zwift asks
		 * for both right after its SYSTEM_RESET. A backend with no serial
		 * (sdk-ant) returns INVALID_MESSAGE, which reaches the host as it
		 * did before either arm existed. The channel byte is ignored,
		 * since a serial number is device-wide.
		 */
		uint32_t serial;

		err = radiant_ant_serial_number_get(&serial);
		if (!err) {
			sys_put_le32(serial, payload);
			if (req_id == RADIANT_WIRE_MESG_GET_SERIAL_NUM_ID) {
				send_message(RADIANT_WIRE_MESG_GET_SERIAL_NUM_ID, payload,
					     RADIANT_WIRE_MESG_SERIAL_NUM_SIZE);
			} else {
				payload[4] = 0x00u;
				send_message(RADIANT_WIRE_MESG_SERIAL_NUM_EXT_ID, payload,
					     RADIANT_WIRE_MESG_SERIAL_NUM_EXT_SIZE);
			}
		}
		break;
	}

	case RADIANT_WIRE_MESG_VERSION_ID: {
		err = radiant_ant_version_get(payload);
		if (!err) {
			send_message(RADIANT_WIRE_MESG_VERSION_ID, payload,
				     RADIANT_WIRE_MESG_VERSION_SIZE);
		}
		break;
	}

	case RADIANT_WIRE_MESG_CHANNEL_STATUS_ID: {
		payload[0] = ch;
		err = radiant_ant_channel_status_get(ch, &payload[1]);
		if (!err) {
			send_message(RADIANT_WIRE_MESG_CHANNEL_STATUS_ID, payload,
				     RADIANT_WIRE_MESG_CHANNEL_STATUS_SIZE);
		}
		break;
	}

	case RADIANT_WIRE_MESG_CHANNEL_ID_ID: {
		payload[0] = ch;
		err = radiant_ant_channel_id_get(ch, &value16, &payload[3], &payload[4]);
		if (!err) {
			payload[1] = (uint8_t)value16;
			payload[2] = (uint8_t)(value16 >> 8);
			send_message(RADIANT_WIRE_MESG_CHANNEL_ID_ID, payload,
				     RADIANT_WIRE_MESG_CHANNEL_ID_SIZE);
		}
		break;
	}

	case RADIANT_WIRE_MESG_CHANNEL_MESG_PERIOD_ID: {
		payload[0] = ch;
		err = radiant_ant_channel_period_get(ch, &value16);
		if (!err) {
			payload[1] = (uint8_t)value16;
			payload[2] = (uint8_t)(value16 >> 8);
			send_message(RADIANT_WIRE_MESG_CHANNEL_MESG_PERIOD_ID, payload,
				     RADIANT_WIRE_MESG_CHANNEL_MESG_PERIOD_SIZE);
		}
		break;
	}

	case RADIANT_WIRE_MESG_CHANNEL_RADIO_FREQ_ID: {
		payload[0] = ch;
		err = radiant_ant_channel_radio_freq_get(ch, &payload[1]);
		if (!err) {
			send_message(RADIANT_WIRE_MESG_CHANNEL_RADIO_FREQ_ID, payload,
				     RADIANT_WIRE_MESG_CHANNEL_RADIO_FREQ_SIZE);
		}
		break;
	}

	case RADIANT_WIRE_MESG_CHANNEL_CRC_MODE_ID: {
		payload[0] = ch;
		err = radiant_ant_channel_radio_crc_mode_get(ch, &payload[1]);
		if (!err) {
			send_message(RADIANT_WIRE_MESG_CHANNEL_CRC_MODE_ID, payload,
				     RADIANT_WIRE_MESG_CHANNEL_CRC_MODE_SIZE);
		}
		break;
	}

	case RADIANT_WIRE_MESG_SET_SEARCH_CH_PRIORITY_ID: {
		payload[0] = ch;
		err = radiant_ant_search_channel_priority_get(ch, &payload[1]);
		if (!err) {
			send_message(RADIANT_WIRE_MESG_SET_SEARCH_CH_PRIORITY_ID, payload,
				     RADIANT_WIRE_MESG_SET_SEARCH_CH_PRIORITY_SIZE);
		}
		break;
	}

	case RADIANT_WIRE_MESG_PENDING_TRANSMIT_CLEAR_ID: {
		payload[0] = ch;
		err = radiant_ant_pending_transmit_get(ch, &payload[1]);
		if (!err) {
			send_message(RADIANT_WIRE_MESG_PENDING_TRANSMIT_CLEAR_ID, payload,
				     RADIANT_WIRE_MESG_PENDING_TRANSMIT_GET_SIZE);
		}
		break;
	}

#ifdef CONFIG_RADIANT_SEC_HOST_MESSAGES
	case RADIANT_WIRE_MESG_RADIANT_SEC_STATUS_ID: {
		/*
		 * The only read arm in the family. 0xF2 has none anywhere - a
		 * root key goes in and never comes out, and a request for it
		 * falls through to the default below and is answered
		 * RADIANT_WIRE_INVALID_MESSAGE, which is the same answer a device that
		 * had never heard of 0xF2 would give.
		 */
		err = radiant_ant_sec_status_get(ch, payload, (uint8_t)sizeof(payload));
		if (!err) {
			send_message(RADIANT_WIRE_MESG_RADIANT_SEC_STATUS_ID, payload,
				     RADIANT_ANT_SEC_STATUS_LEN);
		}
		break;
	}
#endif /* CONFIG_RADIANT_SEC_HOST_MESSAGES */

	case RADIANT_WIRE_MESG_ANTLIB_CONFIG_ID: {
		payload[0] = ch;
		err = radiant_ant_lib_config_get(&payload[1]);
		if (!err) {
			send_message(RADIANT_WIRE_MESG_ANTLIB_CONFIG_ID, payload,
				     RADIANT_WIRE_MESG_ANTLIB_CONFIG_SIZE);
		}
		break;
	}

	case RADIANT_WIRE_MESG_ACTIVE_SEARCH_SHARING_ID: {
		payload[0] = ch;
		err = radiant_ant_active_search_sharing_cycles_get(ch, &payload[1]);
		if (!err) {
			send_message(RADIANT_WIRE_MESG_ACTIVE_SEARCH_SHARING_ID, payload,
				     RADIANT_WIRE_MESG_ACTIVE_SEARCH_SHARING_REQ_SIZE);
		}
		break;
	}

	case RADIANT_WIRE_MESG_EVENT_FILTER_CONFIG_ID: {
		payload[0] = ch;
		err = radiant_ant_event_filtering_get(&value16);
		if (!err) {
			payload[1] = (uint8_t)value16;
			payload[2] = (uint8_t)(value16 >> 8);
			send_message(RADIANT_WIRE_MESG_EVENT_FILTER_CONFIG_ID, payload,
				     RADIANT_WIRE_MESG_EVENT_FILTER_CONFIG_REQ_SIZE);
		}
		break;
	}

	case RADIANT_WIRE_MESG_CONFIG_ADV_BURST_ID: {
		/* body[0] is 0 for "what can you do" and 1 for "what are you
		 * set to", not a channel, and the two replies are different
		 * lengths.
		 */
		uint8_t reply_len =
			ch ? RADIANT_WIRE_MESG_CONFIG_ADV_BURST_REQ_CONFIG_SIZE :
			     RADIANT_WIRE_MESG_CONFIG_ADV_BURST_REQ_CAPABILITIES_SIZE;

		err = radiant_ant_adv_burst_config_get(ch, payload);
		if (!err) {
			send_message(RADIANT_WIRE_MESG_CONFIG_ADV_BURST_ID, payload,
				     reply_len);
		}
		break;
	}

	case RADIANT_WIRE_MESG_SDU_SET_MASK_ID: {
		/* [mask_index, mask[8]]. The 8 mask bytes start one in: the reply is
		 * the same shape as the command that set them, and the size constant
		 * is RADIANT_WIRE_ANT_MAX_PAYLOAD_SIZE *plus* RADIANT_WIRE_CHANNEL_NUM_SIZE precisely
		 * because of that leading index byte. Writing the mask at payload[0]
		 * instead fills the index slot with mask[0], shifts every byte down
		 * one, and pads the end with a zero the host reads as mask[7].
		 */
		payload[0] = ch;
		err = radiant_ant_sdu_mask_get(ch, &payload[1]);
		if (!err) {
			send_message(RADIANT_WIRE_MESG_SDU_SET_MASK_ID, payload,
				     RADIANT_WIRE_ANT_MAX_PAYLOAD_SIZE +
				     RADIANT_WIRE_CHANNEL_NUM_SIZE);
		}
		break;
	}

	case RADIANT_WIRE_MESG_ENCRYPT_ENABLE_ID: {
		/* Same leading byte, same reason: the info type is echoed at
		 * payload[0] and the requested data follows it. Each reply size below
		 * counts that byte - 2 = type + 1 mode byte, 5 = type + a 4-byte
		 * crypto ID, 20 = type + 19 bytes of custom user data.
		 */
		payload[0] = ch;
		err = radiant_ant_crypto_info_get(ch, &payload[1]);
		if (!err) {
			uint8_t reply_len = 0;

			switch (ch) {
			case RADIANT_WIRE_ENCRYPTION_INFO_GET_SUPPORTED_MODE:
				reply_len = RADIANT_WIRE_MESG_CONFIG_ENCRYPT_REQ_CAPABILITIES_SIZE;
				break;
			case RADIANT_WIRE_ENCRYPTION_INFO_GET_CRYPTO_ID:
				reply_len = RADIANT_WIRE_MESG_CONFIG_ENCRYPT_REQ_CONFIG_ID_SIZE;
				break;
			case RADIANT_WIRE_ENCRYPTION_INFO_GET_CUSTOM_USER_DATA:
				reply_len = RADIANT_WIRE_MESG_CONFIG_ENCRYPT_REQ_CONFIG_USER_DATA_SIZE;
				break;
			default:
				err = RADIANT_WIRE_INVALID_MESSAGE;
				break;
			}

			if (!err) {
				send_message(RADIANT_WIRE_MESG_ENCRYPT_ENABLE_ID, payload,
					     reply_len);
			}
		}
		break;
	}

	default:
		send_response(ch, RADIANT_WIRE_MESG_REQUEST_ID, RADIANT_WIRE_INVALID_MESSAGE);
		return;
	}

	if (err) {
		LOG_WRN("Request 0x%02X failed: %d", req_id, err);
		send_response(ch, RADIANT_WIRE_MESG_REQUEST_ID, err);
	}
}

/* ── Burst block ───────────────────────────────────────────────────────────── */

/*
 * radiant_ant_burst_tx() takes ownership of the buffer it is handed until the radio
 * has sent it, but `body` points into the parser's frame buffer, overwritten
 * by the next packet - so each block is copied somewhere that outlives the
 * call and held there. Advanced burst carries up to RADIANT_WIRE_ADV_BURST_BLOCK_MAX
 * (24) bytes per packet; plain burst carries 8. Defined ahead of the reset
 * handler, which has to hand the block back.
 */

/*
 * TWO BUFFERS ON THE RADIANT CORE, ONE ELSEWHERE. The RadiANT transfer engine
 * accepts the next block while the one before it is still on the air and
 * chains it from the acknowledgement's own callback (radiant_transfer_submit(),
 * "pre-staged continuation"). With one buffer every block boundary waited for
 * NEXT_DATA_BLOCK to come back through this thread, which on the cc26xx did
 * not fit inside the 1550 us gap and lost every chained packet (2026-11-12).
 * The sdk-ant and stub backends make no such promise, so they keep one.
 *
 * Each buffer has an owner channel (+1; 0 is free) and an issue number. Only
 * the owner's transfer events hand a buffer back: NEXT_DATA_BLOCK the oldest
 * it holds (the engine always finishes blocks in order), TX_COMPLETED/FAILED
 * every one it holds (a failed transfer also returns the staged block). Any
 * channel's TX_COMPLETED/FAILED used to free the block, so an acked
 * completion elsewhere let the next packet overwrite a block the engine was
 * still sending; the owner check keeps that fixed.
 */
#if defined(CONFIG_RADIANT_ANT_API)
#define BURST_BUFS 2
#else
#define BURST_BUFS 1
#endif
static uint8_t burst_block[BURST_BUFS][RADIANT_WIRE_ADV_BURST_BLOCK_MAX];
static K_SEM_DEFINE(burst_block_free, BURST_BUFS, BURST_BUFS);
static struct k_spinlock burst_lock;
static uint8_t  burst_owner[BURST_BUFS];   /* channel + 1, or 0 when free */
static uint32_t burst_issue[BURST_BUFS];
static uint32_t burst_issue_next;

/* After a successful k_sem_take(): claim a free buffer for `ch`. */
static int burst_buf_claim(uint8_t ch)
{
	k_spinlock_key_t key = k_spin_lock(&burst_lock);
	int idx = -1;

	for (int i = 0; i < BURST_BUFS; i++) {
		if (burst_owner[i] == 0u) {
			burst_owner[i] = (uint8_t)(ch + 1u);
			burst_issue[i] = burst_issue_next++;
			idx = i;
			break;
		}
	}
	k_spin_unlock(&burst_lock, key);
	return idx;
}

static void burst_buf_drop(int idx)
{
	k_spinlock_key_t key = k_spin_lock(&burst_lock);

	burst_owner[idx] = 0u;
	k_spin_unlock(&burst_lock, key);
	k_sem_give(&burst_block_free);
}

/* NEXT_DATA_BLOCK on `ch`: the oldest buffer it holds is finished with. */
static void burst_buf_release_oldest(uint8_t ch)
{
	k_spinlock_key_t key = k_spin_lock(&burst_lock);
	int idx = -1;

	for (int i = 0; i < BURST_BUFS; i++) {
		if (burst_owner[i] == (uint8_t)(ch + 1u) &&
		    (idx < 0 ||
		     (int32_t)(burst_issue[i] - burst_issue[idx]) < 0)) {
			idx = i;
		}
	}
	if (idx >= 0) {
		burst_owner[idx] = 0u;
	}
	k_spin_unlock(&burst_lock, key);
	if (idx >= 0) {
		k_sem_give(&burst_block_free);
	}
}

/* TX_COMPLETED/FAILED on `ch`: every buffer it holds comes back. */
static void burst_buf_release_all(uint8_t ch)
{
	k_spinlock_key_t key = k_spin_lock(&burst_lock);
	int n = 0;

	for (int i = 0; i < BURST_BUFS; i++) {
		if (burst_owner[i] == (uint8_t)(ch + 1u)) {
			burst_owner[i] = 0u;
			n++;
		}
	}
	k_spin_unlock(&burst_lock, key);
	while (n-- > 0) {
		k_sem_give(&burst_block_free);
	}
}

/* A stack reset: every buffer is free again, whoever held it. */
static void burst_buf_reset(void)
{
	k_spinlock_key_t key = k_spin_lock(&burst_lock);

	memset(burst_owner, 0, sizeof(burst_owner));
	k_spin_unlock(&burst_lock, key);
	k_sem_reset(&burst_block_free);
	for (int i = 0; i < BURST_BUFS; i++) {
		k_sem_give(&burst_block_free);
	}
}

/* ── What the host did to the stack ───────────────────────────────────────── */

/* See ant_serial_bridge.h. Written on the bridge thread, read from whichever
 * thread shares the stack with the host; the lock is what makes a snapshot's
 * key[] agree with its key_written. */
static struct k_spinlock host_state_lock;
static uint32_t host_reset_gen;
static uint32_t host_key_writes[ANT_BRIDGE_HOST_NETWORKS];
static bool     host_key_written[ANT_BRIDGE_HOST_NETWORKS];
static uint8_t  host_key[ANT_BRIDGE_HOST_NETWORKS][8];

static void host_state_note_reset(void)
{
	k_spinlock_key_t key = k_spin_lock(&host_state_lock);

	host_reset_gen++;
	memset(host_key_written, 0, sizeof(host_key_written));
	k_spin_unlock(&host_state_lock, key);
}

static void host_state_note_key(uint8_t net, const uint8_t *k)
{
	k_spinlock_key_t key;

	if (net >= ANT_BRIDGE_HOST_NETWORKS) {
		return;
	}
	key = k_spin_lock(&host_state_lock);
	host_key_writes[net]++;
	host_key_written[net] = true;
	memcpy(host_key[net], k, sizeof(host_key[net]));
	k_spin_unlock(&host_state_lock, key);
}

void ant_serial_bridge_host_state(uint8_t net, struct ant_bridge_host_state *out)
{
	k_spinlock_key_t key;

	memset(out, 0, sizeof(*out));
	key = k_spin_lock(&host_state_lock);
	out->reset_gen = host_reset_gen;
	/*
	 * reset_in_progress is not written under this lock (handle_system_reset()
	 * sets it before radiant_ant_stack_reset() and clears it after
	 * host_state_note_reset() bumps the generation and the reset's fences
	 * have passed), so the ORDER of these two reads is what makes them
	 * agree: generation first, flag second. Read that way, a flag of 0 next
	 * to an old generation means the reset had not begun when the flag was
	 * read, and a flag of 0 next to a new generation means the reset that
	 * bumped it had finished; the only way to get either pair wrong is to
	 * sit between the two reads for the whole of the reset's tail. On a
	 * single core the lock makes the pair atomic anyway; the order is for
	 * the reader on another one.
	 */
	out->reset_busy = atomic_get(&reset_in_progress) != 0;
	if (net < ANT_BRIDGE_HOST_NETWORKS) {
		out->key_writes = host_key_writes[net];
		out->key_written = host_key_written[net];
		memcpy(out->key, host_key[net], sizeof(out->key));
	}
	k_spin_unlock(&host_state_lock, key);
}

/* ── System reset ──────────────────────────────────────────────────────────── */

/* RADIANT_WIRE_MESG_SYSTEM_RESET_ID resets the ANT protocol stack, not the MCU: every
 * host library keeps using the same USB handle after sending reset, and
 * rebooting here would drop the device off the bus mid-session.
 */
static void handle_system_reset(void)
{
	atomic_set(&reset_in_progress, 1);

	(void)radiant_ant_stack_reset();
	/* After the reset, not before: a reader that sees the new generation
	 * must find the channels already gone. */
	host_state_note_reset();

	/* The stack is back to its defaults and every channel is unassigned, so
	 * anything a previous session asked for is gone. Forget it here too,
	 * or a deliberately quiet setting would outlive the reset that a real
	 * stick clears it with.
	 */
	tx_power_reset();

	/* A burst in flight when the reset landed is gone, and so is the
	 * TRANSFER_TX_* event that would have released its block: the stack
	 * flushes it, or radiant_ant_on_message() drops it while reset_in_progress
	 * is set. The semaphore is
	 * ours, not the stack's, so nothing else restores it - left taken,
	 * every later burst packet stalls 1000 ms and fails with
	 * TRANSFER_IN_PROGRESS until the board is power-cycled.
	 */
	burst_buf_reset();

	/*
	 * Nothing may reach the host between the reset and the STARTUP that
	 * follows except frames already queued before it, and nothing from
	 * before the reset may follow the STARTUP. radiant_ant_stack_reset() is
	 * synchronous on RadiANT: its channel-closed events have been raised,
	 * the event ring flushed, and any delivery that had already left the
	 * ring has returned. Two orderings are still open, and the two fences
	 * close them.
	 *
	 * 1. An event inside radiant_ant_on_message() that passed its
	 *    reset_in_progress check before the flag was set and is not yet
	 *    queued. This thread pre-empts the event thread, so that frame
	 *    could be queued after the STARTUP. The flag was set above, before
	 *    evt_inflight is read here; radiant_ant_on_message() bumps evt_inflight
	 *    before it reads the flag. Atomics are sequentially consistent, so
	 *    either this loop sees the call and waits for it, or the call sees
	 *    the flag and drops its frame. The sleep lets the lower-priority
	 *    event thread run to the end of the call.
	 */
	{
		k_timepoint_t end = sys_timepoint_calc(K_MSEC(RESET_EVT_FENCE_MS));

		while (atomic_get(&evt_inflight) != 0) {
			if (sys_timepoint_expired(end)) {
				LOG_WRN("reset: event path still busy after %d ms",
					RESET_EVT_FENCE_MS);
				break;
			}
			k_sleep(K_TICKS(1));
		}
	}

	/*
	 * 2. Events queued for the transport's writer thread before the reset.
	 *    The STARTUP is a direct write, so it would overtake them. Those
	 *    frames are older than the reset and the host may have them; they
	 *    go out first, and nothing older follows the STARTUP.
	 *
	 *    The bound is shorter than one stalled USB write (ANT_TX_TIMEOUT,
	 *    1000 ms, a host not polling IN), so a fence that times out sends
	 *    the STARTUP through the queue instead: FIFO with one consumer,
	 *    it then follows every older frame however long they take.
	 */
	bool fenced = ant_transport_tx_fence(K_MSEC(RESET_TX_FENCE_MS)) == 0;

	if (!fenced) {
		LOG_WRN("reset: transmit queue not drained in %d ms; STARTUP queued",
			RESET_TX_FENCE_MS);
	}

#if defined(CONFIG_ANT_DONGLE_RADIO_SDK_ANT)
	/* The sdk-ant backend, kept as the A/B reference, raises its
	 * channel-closed events asynchronously after ant_stack_reset()
	 * returns, so nothing above can see them coming. Give them time to
	 * land and be discarded. Real sticks are unresponsive for rather
	 * longer than this.
	 */
	k_sleep(K_MSEC(50));
#endif
	/* Before the flag clears, so a queued STARTUP is ahead of any event
	 * the new session raises. */
	send_startup(RADIANT_WIRE_STARTUP_COMMAND_RESET, !fenced);
	atomic_set(&reset_in_progress, 0);
}

/* ── Burst transmit ─────────────────────────────────────────────────────────── */

static void handle_burst(uint8_t msg_id, const uint8_t *body, uint8_t len)
{
	/* body: [seq << 5 | channel, data[8|16|24]], bit 7 marks the last
	 * packet of the transfer.
	 */
	uint8_t size = (len >= 1) ? (uint8_t)(len - 1) : 0;

	if (size < 8 || size > RADIANT_WIRE_ADV_BURST_BLOCK_MAX || (size % 8) != 0) {
		send_response(0, msg_id, RADIANT_WIRE_INVALID_MESSAGE);
		return;
	}

	uint8_t ch      = body[0] & RADIANT_WIRE_BURST_HEADER_CHANNEL_MASK;
	uint8_t seq     = (body[0] >> RADIANT_WIRE_BURST_HEADER_SEQ_SHIFT) &
			  RADIANT_WIRE_BURST_HEADER_SEQ_MASK;
	uint8_t segment = RADIANT_ANT_BURST_SEGMENT_CONTINUE;

	if (seq == 0) {
		segment |= RADIANT_ANT_BURST_SEGMENT_START;
	}
	if (body[0] & RADIANT_WIRE_BURST_HEADER_LAST) {
		segment |= RADIANT_ANT_BURST_SEGMENT_END;
	}

	/* Waiting for the stack to release the previous block, rather than
	 * queueing, is what applies back-pressure to the host. The timeout only
	 * exists so a transfer that dies mid-flight cannot wedge this thread.
	 */
	if (k_sem_take(&burst_block_free, K_MSEC(1000)) != 0) {
		/* A counter and nothing more, deliberately. Shortening the
		 * stall does not help anyone: the 1000 ms IS the back-pressure,
		 * so a shorter one makes the host's transfer FAIL where it
		 * currently waits. Zwift uses acknowledged data rather than
		 * burst, so a non-zero reading here is a different host's
		 * problem and worth being able to see from one. */
		ant_health_note(ANT_HEALTH_BURST_STALL);
		send_response(ch, msg_id, RADIANT_WIRE_TRANSFER_IN_PROGRESS);
		return;
	}

	/* Claimed before radiant_ant_burst_tx() accepts, so the owner is in place
	 * ahead of any event for this block. The semaphore counts free
	 * buffers, so a claim after a successful take always finds one. */
	int idx = burst_buf_claim(ch);

	if (idx < 0) {
		k_sem_give(&burst_block_free);
		send_response(ch, msg_id, RADIANT_WIRE_TRANSFER_IN_PROGRESS);
		return;
	}
	memcpy(burst_block[idx], &body[1], size);

	radiant_ant_err_t err = radiant_ant_burst_tx(ch, size, burst_block[idx], segment);

	if (err) {
		burst_buf_drop(idx);
		send_response(ch, msg_id, err);
	}

	/* No per-packet acknowledgement on success: like a real stick, the host
	 * learns the outcome from EVENT_TRANSFER_TX_COMPLETED or _TX_FAILED
	 * once the whole transfer has finished.
	 */
}

/* ── Inbound dispatcher ─────────────────────────────────────────────────────── */

static void dispatch(uint8_t msg_id, const uint8_t *body, uint8_t len)
{
	/* Starts as a failure so a too-short body falls through every
	 * `if (len >= N)` below as RADIANT_WIRE_INVALID_MESSAGE, rather than silently
	 * acknowledging a truncated command that never ran.
	 */
	radiant_ant_err_t err = RADIANT_WIRE_INVALID_MESSAGE;
	/* body[0] is channel number (or network number for NETWORK_KEY) */
	uint8_t ch = (len > 0) ? body[0] : 0;

	if (!body_len_ok(msg_id, len)) {
		send_response(ch, msg_id, RADIANT_WIRE_INVALID_MESSAGE);
		return;
	}

	switch (msg_id) {

	case RADIANT_WIRE_MESG_SYSTEM_RESET_ID:
		handle_system_reset(); /* sends the STARTUP */
		return; /* no RESPONSE_EVENT for reset */

	case RADIANT_WIRE_MESG_NETWORK_KEY_ID:
		/* body: [net_num, key[8]] */
		if (len >= 9) {
			err = radiant_ant_network_address_set(body[0], &body[1]);
			if (!err) {
				host_state_note_key(body[0], &body[1]);
			}
		}
		break;

	case RADIANT_WIRE_MESG_ASSIGN_CHANNEL_ID:
		/* body: [ch, type, net, (ext_assign optional)] */
		if (len >= 3) {
			uint8_t ext = (len >= 4) ? body[3] : 0x00;
			err = radiant_ant_channel_assign(body[0], body[1], body[2], ext);
			if (!err) {
				/* The channel exists now, so the power the host
				 * asked for earlier - or our boosted default,
				 * if it never asked - can finally be applied.
				 */
				tx_power_apply(body[0]);
			}
		}
		break;

	case RADIANT_WIRE_MESG_CHANNEL_ID_ID:
		/* body: [ch, dev_lsb, dev_msb, dev_type, trans_type] */
		if (len >= 5) {
			uint16_t dev = (uint16_t)body[1] |
				       ((uint16_t)body[2] << 8);
			err = radiant_ant_channel_id_set(body[0], dev, body[3], body[4]);
		}
		break;

	case RADIANT_WIRE_MESG_CHANNEL_RADIO_FREQ_ID:
		/* body: [ch, freq_offset_from_2400MHz] */
		if (len >= 2) {
			err = radiant_ant_channel_radio_freq_set(body[0], body[1]);
		}
		break;

	case RADIANT_WIRE_MESG_CHANNEL_MESG_PERIOD_ID:
		/* body: [ch, period_lsb, period_msb] (32 kHz counts) */
		if (len >= 3) {
			uint16_t period = (uint16_t)body[1] |
					  ((uint16_t)body[2] << 8);
			err = radiant_ant_channel_period_set(body[0], period);
		}
		break;

	case RADIANT_WIRE_MESG_CHANNEL_SEARCH_TIMEOUT_ID:
		/* body: [ch, timeout] (2.5 s increments) */
		if (len >= 2) {
			err = radiant_ant_channel_search_timeout_set(body[0], body[1]);
		}
		break;

	case RADIANT_WIRE_MESG_SET_LP_SEARCH_TIMEOUT_ID:
		/* body: [ch, timeout] */
		if (len >= 2) {
			err = radiant_ant_channel_low_priority_rx_search_timeout_set(
				body[0], body[1]);
		}
		break;

	case RADIANT_WIRE_MESG_CHANNEL_RADIO_TX_POWER_ID:
		/* body: [ch, tx_power] or [ch, tx_power, custom]
		 *
		 * The third byte is an additive extension: a two-byte message
		 * means what it always did, and the byte is only read when the
		 * level carries RADIANT_WIRE_RADIO_TX_POWER_LVL_CUSTOM. Without it, a
		 * custom-power request silently got register 0 (0 dBm on an
		 * nRF52840) - a failure invisible from the far end of the link,
		 * which is why tools/ant_sens.py checks against measured RSSI.
		 */
		if (len >= 2) {
			uint8_t level = tx_power_resolve(body[1]);
			uint8_t custom = (len >= 3) ? body[2] : 0U;

			if (body[0] < ARRAY_SIZE(tx_power)) {
				tx_power[body[0]] = level;
				tx_power_custom[body[0]] = custom;
			}
			/* Still call through for an out-of-range channel, so
			 * the stack returns the error a real stick would.
			 */
			err = radiant_ant_channel_radio_tx_power_set(body[0], level,
							      custom);
		}
		break;

	case RADIANT_WIRE_MESG_RADIO_TX_POWER_ID:
		/* body: [filler, tx_power]. Device-wide form (ANT_SetTransmitPower);
		 * 0x60 above is per-channel. The radio contract only exposes the
		 * per-channel setter, so fan it out - failures not propagated,
		 * since a host sets default power before assigning any channel.
		 */
		if (len >= 2) {
			uint8_t level = tx_power_resolve(body[1]);
			uint8_t custom = (len >= 3) ? body[2] : 0U;

			for (uint8_t i = 0; i < CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED;
			     i++) {
				tx_power[i] = level;
				tx_power_custom[i] = custom;
				(void)radiant_ant_channel_radio_tx_power_set(i, level,
								      custom);
			}
			err = 0;
		}
		break;

	case RADIANT_WIRE_MESG_OPEN_CHANNEL_ID:
		/* body: [ch] */
		if (len >= 1) {
			err = radiant_ant_channel_open(body[0]);
		}
		break;

	case RADIANT_WIRE_MESG_CLOSE_CHANNEL_ID:
		/* body: [ch] */
		if (len >= 1) {
			err = radiant_ant_channel_close(body[0]);
		}
		break;

	case RADIANT_WIRE_MESG_UNASSIGN_CHANNEL_ID:
		/* body: [ch] */
		if (len >= 1) {
			err = radiant_ant_channel_unassign(body[0]);
		}
		break;

	case RADIANT_WIRE_MESG_OPEN_RX_SCAN_MODE_ID: {
		/*
		 * body: [(synchronous channel packets only, optional)]. That flag
		 * restricts reporting to already-tracked periods; accepted and
		 * ignored here since reporting everything is a superset. len is
		 * not checked - nothing depends on it.
		 *
		 * Real scan mode takes over the whole radio, requiring every
		 * other channel closed first (else RADIANT_WIRE_CLOSE_ALL_CHANNELS).
		 * Channel 0 becomes a background-scan slave via
		 * MESG_ASSIGN_CHANNEL's extended byte (RADIANT_WIRE_EXT_PARAM_ALWAYS_
		 * SEARCH) - the same call ZwiftApp.exe resolves.
		 *
		 * Rev 5.1 section 9.5.4.5: scan mode opens channel 0 as the host
		 * already assigned and configured it. A host that set RF 57, a
		 * network key slot and a device-type filter expects the scan to
		 * use them. So an assigned channel 0 only has its extended
		 * assignment byte changed in place
		 * (radiant_ant_channel_ext_assign_set()). On RadiANT that keeps
		 * everything the host configured: ID, RF, period, ID list,
		 * search settings and the security context. A keyed channel
		 * with X_AUTH | DROP_UNVER stays judged as a scan; unassigning
		 * would release its keys and deliver its secured range unjudged.
		 * The channel ID is then read back and set again: a host that
		 * never sent 0x51 gets the 0/0/0 wildcard marked set, as the
		 * reassign path always did, instead of an open refused with
		 * ID_NOT_SET. Only the ID fields change.
		 *
		 * Backends with no in-place call (sdk-ant, stub) answer
		 * INVALID_MESSAGE, and only they take the old path: read back
		 * type, network, channel ID, RF and period, reassign with the
		 * always-search bit, put the five back. That reassign loses the
		 * ID list, the low-priority timeout and the search priority,
		 * but those backends have no channel security to lose.
		 *
		 * A master channel 0 cannot scan and is refused. An unassigned
		 * channel 0 keeps the old fallback: a wildcard slave on
		 * network 0, default RF, with no key to lose.
		 */
		uint8_t status;
		uint8_t i;
		bool restore = false;
		bool in_place = false;
		uint8_t saved_type = RADIANT_WIRE_CHANNEL_TYPE_SLAVE;
		uint8_t saved_net = 0U;
		uint16_t saved_dev = 0U;
		uint8_t saved_dtype = 0U;
		uint8_t saved_ttype = 0U;
		uint8_t saved_rf = 0U;
		uint16_t saved_period = 0U;

		for (i = 0; i < CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED; i++) {
			if (radiant_ant_channel_status_get(i, &status) ==
				    (radiant_ant_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR &&
			    (status & 0x03u) >= RADIANT_WIRE_STATUS_SEARCHING_CHANNEL) {
				break;
			}
		}
		if (i < CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED) {
			send_response(0, msg_id, RADIANT_WIRE_CLOSE_ALL_CHANNELS);
			return;
		}

		if (radiant_ant_channel_status_get(0, &status) ==
			    (radiant_ant_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR &&
		    (status & 0x03u) == RADIANT_WIRE_STATUS_ASSIGNED_CHANNEL) {
			saved_type = status & 0xF0u;
			saved_net = (status >> 2) & 0x03u;
			/* Every master type has bit 4 set (0x10, 0x30, 0x50). */
			if ((saved_type & RADIANT_WIRE_CHANNEL_TYPE_MASTER) != 0U) {
				send_response(0, msg_id,
					      RADIANT_WIRE_CHANNEL_IN_WRONG_STATE);
				return;
			}
			err = radiant_ant_channel_ext_assign_set(
				0, RADIANT_WIRE_EXT_PARAM_ALWAYS_SEARCH);
			if (!err) {
				in_place = true;
				/* An ID the host never set reads back as the
				 * 0/0/0 wildcard; setting it marks it set, as
				 * the old reassign path did, so the open is not
				 * refused with ID_NOT_SET. A set ID is
				 * rewritten unchanged. */
				err = radiant_ant_channel_id_get(0, &saved_dev,
							  &saved_dtype,
							  &saved_ttype);
				if (!err) {
					err = radiant_ant_channel_id_set(
						0, saved_dev, saved_dtype,
						saved_ttype);
				}
			} else if (err == (radiant_ant_err_t)RADIANT_WIRE_INVALID_MESSAGE) {
				/* Channel 0 is always in range: the backend
				 * has no in-place call. */
				err = radiant_ant_channel_id_get(0, &saved_dev,
							  &saved_dtype,
							  &saved_ttype);
				if (!err) {
					err = radiant_ant_channel_radio_freq_get(
						0, &saved_rf);
				}
				if (!err) {
					err = radiant_ant_channel_period_get(
						0, &saved_period);
				}
				if (!err) {
					err = radiant_ant_channel_unassign(0);
				}
				restore = true;
			}
		} else {
			err = (radiant_ant_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR;
		}
		if (!err && !in_place) {
			err = radiant_ant_channel_assign(0, saved_type, saved_net,
						  RADIANT_WIRE_EXT_PARAM_ALWAYS_SEARCH);
		}
		if (!err && !in_place) {
			err = radiant_ant_channel_id_set(0, saved_dev, saved_dtype,
						  saved_ttype);
		}
		if (!err && restore) {
			err = radiant_ant_channel_radio_freq_set(0, saved_rf);
		}
		if (!err && restore) {
			err = radiant_ant_channel_period_set(0, saved_period);
		}
		if (!err) {
			/* As the assign arm does: the power the host asked
			 * for (or the boosted default) survives the reassign.
			 * Harmless on the in-place path, which kept it.
			 */
			tx_power_apply(0);
			err = radiant_ant_channel_open(0);
		}
		send_response(0, msg_id, err ? err : RADIANT_WIRE_RESPONSE_NO_ERROR);
		return;
	}

	case RADIANT_WIRE_MESG_ANTLIB_CONFIG_ID:
		/* body: [filler, config_bits]. The value replaces the previous
		 * one: 0xE0 then 0x80 leaves 0x80, as on any other ANT device.
		 * Zero clears everything, which is how hosts drop the
		 * extended-message flags between sessions. The core API stays
		 * additive (0x66 and the apps rely on that), so the bridge sets
		 * the new bits first - radiant_ant_lib_config_set() refuses an unknown
		 * bit with no side effect - and only then clears the rest.
		 */
		if (len >= 2) {
			if (body[1] == 0U) {
				err = radiant_ant_lib_config_clear(
					RADIANT_WIRE_LIB_CONFIG_MASK_ALL);
			} else {
				uint8_t drop = (uint8_t)(RADIANT_WIRE_LIB_CONFIG_MASK_ALL &
							 ~body[1]);

				err = radiant_ant_lib_config_set(body[1]);
				if (!err && drop != 0U) {
					err = radiant_ant_lib_config_clear(drop);
				}
			}
		}
		break;

	case RADIANT_WIRE_MESG_RX_EXT_MESGS_ENABLE_ID:
		/* body: [filler, enable]. The older, narrower way of asking for
		 * the channel id on every received message. Hosts still send it
		 * instead of MESG_ANTLIB_CONFIG, and without it every broadcast
		 * arrives anonymous and no sensor can be identified.
		 */
		if (len >= 2) {
			err = body[1] ?
			      radiant_ant_lib_config_set(
				      RADIANT_WIRE_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID) :
			      radiant_ant_lib_config_clear(
				      RADIANT_WIRE_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID);
		}
		break;

	case RADIANT_WIRE_MESG_SEARCH_WAVEFORM_ID:
		/* body: [ch, waveform_lsb, waveform_msb] */
		if (len >= 3) {
			uint16_t waveform = (uint16_t)body[1] |
					    ((uint16_t)body[2] << 8);
			err = radiant_ant_search_waveform_set(body[0], waveform);
		}
		break;

	case RADIANT_WIRE_MESG_PROX_SEARCH_CONFIG_ID:
		/* body: [ch, threshold, (custom threshold optional)] */
		if (len >= 2) {
			uint8_t custom = (len >= 3) ? body[2] : 0;

			err = radiant_ant_prox_search_set(body[0], body[1], custom);
		}
		break;

	case RADIANT_WIRE_MESG_SET_SEARCH_CH_PRIORITY_ID:
		/* body: [ch, priority] */
		if (len >= 2) {
			err = radiant_ant_search_channel_priority_set(body[0], body[1]);
		}
		break;

	case RADIANT_WIRE_MESG_ACTIVE_SEARCH_SHARING_ID:
		/* body: [ch, cycles] */
		if (len >= 2) {
			err = radiant_ant_active_search_sharing_cycles_set(body[0],
								    body[1]);
		}
		break;

	case RADIANT_WIRE_MESG_AUTO_FREQ_CONFIG_ID:
		/* body: [ch, freq0, freq1, freq2] */
		if (len >= 4) {
			err = radiant_ant_auto_freq_hop_table_set(body[0], body[1],
							   body[2], body[3]);
		}
		break;

	case RADIANT_WIRE_MESG_CHANNEL_CRC_MODE_ID:
		/* body: [filler, mode] */
		if (len >= 2) {
			err = radiant_ant_channel_radio_crc_mode_set(body[0], body[1]);
		}
		break;

	case RADIANT_WIRE_MESG_ID_LIST_ADD_ID:
		/* body: [ch, dev_lsb, dev_msb, dev_type, trans_type, index] */
		if (len >= 6) {
			err = radiant_ant_id_list_add(body[0], &body[1], body[5]);
		}
		break;

	case RADIANT_WIRE_MESG_ID_LIST_CONFIG_ID:
		/* body: [ch, list_size, exclude_flag] */
		if (len >= 3) {
			err = radiant_ant_id_list_config(body[0], body[1], body[2]);
		}
		break;

	case RADIANT_WIRE_MESG_EVENT_FILTER_CONFIG_ID:
		/* body: [filler, filter_lsb, filter_msb] (Rev 5.1 sec 9.5.2.28),
		 * or the legacy [filter_lsb, filter_msb] with no filler. Three
		 * bytes is always the spec form; two is kept because the
		 * committed conformance transcripts send it.
		 */
		if (len >= 3) {
			uint16_t filter = (uint16_t)body[1] |
					  ((uint16_t)body[2] << 8);
			err = radiant_ant_event_filtering_set(filter);
		} else if (len == 2) {
			uint16_t filter = (uint16_t)body[0] |
					  ((uint16_t)body[1] << 8);
			err = radiant_ant_event_filtering_set(filter);
		}
		break;

	case RADIANT_WIRE_MESG_CONFIG_ADV_BURST_ID:
		/* body: [filler, config[8..11]]. The filler byte occupies the channel
		 * slot every message has and is not part of the configuration, so the
		 * buffer radiant_ant_adv_burst_config_set() documents starts at body[1]:
		 * [enable, rf payload size, required modes, 0, 0, optional modes, 0, 0]
		 * with an optional [stall lsb, stall msb, retry extension].
		 *
		 * Without this, RADIANT_WIRE_MESG_ADV_BURST_DATA_ID is accepted but advanced
		 * burst is never on, so nothing ever leaves at more than 8 bytes a
		 * packet.
		 */
		if (len >= 9 && len <= 12) {
			err = radiant_ant_adv_burst_config_set(&body[1],
							(uint8_t)(len - 1));
		}
		break;

	case RADIANT_WIRE_MESG_SDU_CONFIG_ID:
		/* body: [ch, mask_config]. Binds one of the masks below to a channel;
		 * RADIANT_WIRE_INVALID_SDU_MASK (0xFF) turns selective updates back off.
		 */
		if (len >= 2) {
			err = radiant_ant_sdu_mask_config(body[0], body[1]);
		}
		break;

	case RADIANT_WIRE_MESG_SDU_SET_MASK_ID:
		/* body: [mask_index, mask[8]]. body[0] is a mask number, not a channel
		 * - masks are defined here and attached to channels by MESG_SDU_CONFIG.
		 */
		if (len >= 9) {
			err = radiant_ant_sdu_mask_set(body[0], &body[1]);
		}
		break;

#ifdef CONFIG_ANT_DONGLE_ENCRYPTION
	/* The three writes that can put a channel into AES-CTR mode. Compiled out
	 * by default since no host on this platform sends them. Matching reads
	 * live in handle_request() and are always present.
	 */
	case RADIANT_WIRE_MESG_ENCRYPT_ENABLE_ID:
		/* body: [ch, mode, key_num, decimation_rate] */
		if (len >= 4) {
			err = radiant_ant_crypto_channel_enable(body[0], body[1], body[2],
							 body[3]);
		}
		break;

	case RADIANT_WIRE_MESG_SET_ENCRYPT_KEY_ID:
		/* body: [key_num, key[16]]. No wire constant names the key length;
		 * that it is 128 bits is stated only in the prose of
		 * radiant_ant_crypto_key_set(), which takes a bare pointer.
		 */
		if (len >= 1 + RADIANT_WIRE_ENCRYPTION_KEY_SIZE) {
			err = radiant_ant_crypto_key_set(body[0], &body[1]);
		}
		break;

	case RADIANT_WIRE_MESG_SET_ENCRYPT_INFO_ID:
		/* body: [info_type, data]. How much data depends on the type, and the
		 * length has to be checked against it here: radiant_ant_crypto_info_set()
		 * reads a fixed count per type with no size to bound it, so a
		 * truncated command would otherwise hand the radio whatever follows
		 * in the parser's frame buffer.
		 */
		if (len >= 2) {
			uint8_t want = 0;

			switch (body[0]) {
			case RADIANT_WIRE_ENCRYPTION_INFO_SET_CRYPTO_ID:
				want = RADIANT_WIRE_MESG_CONFIG_ENCRYPT_REQ_CONFIG_ID_SIZE;
				break;
			case RADIANT_WIRE_ENCRYPTION_INFO_SET_CUSTOM_USER_DATA:
				want = RADIANT_WIRE_MESG_CONFIG_ENCRYPT_REQ_CONFIG_USER_DATA_SIZE;
				break;
			/* RADIANT_WIRE_ENCRYPTION_INFO_SET_RNG_SEED is deliberately absent.
			 * It is documented as platform specific with no size defined
			 * for it anywhere, and the radio does not take its randomness
			 * from the host regardless - a backend seeds itself at init.
			 * Refusing beats guessing a length.
			 */
			default:
				break;
			}

			if (want && len >= want) {
				err = radiant_ant_crypto_info_set(body[0], &body[1]);
			}
		}
		break;
#endif /* CONFIG_ANT_DONGLE_ENCRYPTION */

#ifdef CONFIG_RADIANT_SEC_HOST_MESSAGES
	/*
	 * The RadiANT payload transforms: 0xF1 configure, 0xF2 key, 0xF3 epoch.
	 * 0xF4 is read-only (handle_request()); 0xF5 is pairing, with X25519.
	 * Unlike the encryption block above, the declarations are under the
	 * symbol too - these have exactly one implementation, not a contract
	 * every backend owes.
	 */
	case RADIANT_WIRE_MESG_RADIANT_SEC_CONFIG_ID:
		/* body: [ch, switches, W, page_lo, page_hi] */
		if (len >= 5) {
			err = radiant_ant_sec_config(body[0], body[1], body[2],
					      body[3], body[4]);
		}
		break;

	case RADIANT_WIRE_MESG_RADIANT_SET_KEY_ID:
		/* body: [ch, key_bits, key[16]].
		 *
		 * process_byte() wipes its frame buffer after this dispatch
		 * returns - see the note there, including what that wipe does
		 * not cover.
		 */
		if (len >= 18) {
			err = radiant_ant_sec_key_set(body[0], body[1], &body[2]);
		}
		break;

	case RADIANT_WIRE_MESG_RADIANT_EPOCH_ID:
		/* body: [ch, flags, epoch u32 LE, us_into_epoch u64 LE] */
		if (len >= 14) {
			uint32_t epoch;
			uint64_t us;
			int      i;

			epoch = (uint32_t)body[2] | ((uint32_t)body[3] << 8) |
				((uint32_t)body[4] << 16) |
				((uint32_t)body[5] << 24);

			us = 0u;
			for (i = 7; i >= 0; i--) {
				us = (us << 8) | (uint64_t)body[6 + i];
			}

			err = radiant_ant_sec_epoch_set(body[0], body[1], epoch, us);
		}
		break;

#ifdef CONFIG_RADIANT_SEC_PAIRING_X25519
	case RADIANT_WIRE_MESG_RADIANT_PAIRING_ID: {
		/* body: [ch, subcmd, arg...]. Reply is a 0xF5 of its own, not a
		 * response code, since the key, commitment and fingerprint
		 * sub-commands carry data back. */
		uint8_t reply[2 + 32];
		uint8_t reply_len = 0u;

		if (len < 2) {
			break;
		}
		err = radiant_ant_sec_pair(body[0], body[1],
				    (len > 2) ? &body[2] : NULL,
				    (uint8_t)(len - 2), reply,
				    (uint8_t)sizeof(reply), &reply_len);
		if (!err && reply_len > 0u) {
			send_message(RADIANT_WIRE_MESG_RADIANT_PAIRING_ID, reply,
				     reply_len);
		}
		/*
		 * The scalar just crossed the frame buffer. process_byte()
		 * wipes it after this dispatch for the same reason it wipes a
		 * 0xF2, and with the same honest limit: the USB ring and the
		 * transport buffer beneath it are not reachable from here.
		 */
		memset(reply, 0, sizeof(reply));
		log_pair_stack_high_water();
		break;
	}
#endif /* CONFIG_RADIANT_SEC_PAIRING_X25519 */
#endif /* CONFIG_RADIANT_SEC_HOST_MESSAGES */

	case RADIANT_WIRE_MESG_ECS_ENABLE_ID:
		/* body: [filler, enable] */
		if (len >= 2) {
			err = radiant_ant_enhanced_channel_spacing_enable(body[1]);
		}
		break;

	case RADIANT_WIRE_MESG_PENDING_TRANSMIT_CLEAR_ID:
		/* body: [ch] */
		if (len >= 1) {
			uint8_t cleared;

			err = radiant_ant_pending_transmit_clear(body[0], &cleared);
		}
		break;

	case RADIANT_WIRE_MESG_REQUEST_ID:
		handle_request(body, len);
		return; /* handle_request sends its own response */

	case RADIANT_WIRE_MESG_BROADCAST_DATA_ID:
		/* body: [ch, data[8]] */
		if (len >= 9) {
			err = radiant_ant_broadcast_message_tx(body[0], 8, &body[1]);
		}
		break;

	case RADIANT_WIRE_MESG_ACKNOWLEDGED_DATA_ID:
		/* body: [ch, data[8]] */
		if (len >= 9) {
			err = radiant_ant_acknowledge_message_tx(body[0], 8, &body[1]);
		}
		break;

	case RADIANT_WIRE_MESG_BURST_DATA_ID:
	case RADIANT_WIRE_MESG_ADV_BURST_DATA_ID:
		handle_burst(msg_id, body, len);
		return; /* handle_burst answers only on failure */

	default:
		LOG_WRN("Unknown msg_id 0x%02X len=%d", msg_id, len);
		send_response(ch, msg_id, RADIANT_WIRE_INVALID_MESSAGE);
		return;
	}

	send_response(ch, msg_id, err ? err : RADIANT_WIRE_RESPONSE_NO_ERROR);
}

#endif /* !ANT_BRIDGE_PARSER_ONLY */

/* ── Frame parser state machine ─────────────────────────────────────────────── */

enum parser_state {
	S_SYNC,
	S_LEN,
	S_ID,
	S_BODY,
	S_CSUM,
};

/*
 * The parser's whole state, at file scope rather than as statics inside
 * process_byte() so that parser_reset() can reach it. Owned by the bridge
 * thread alone: nothing else reads or writes it, which is why the transport
 * signals a host-session boundary through ant_transport_session() instead of
 * resetting this itself from its own callback context.
 */
static struct {
	enum parser_state state;
	uint8_t msg_len;
	uint8_t msg_id;
	/* Sized from the largest value the LEN byte may carry, which counts the
	 * leading channel byte as well as the payload. An advanced-burst data
	 * message alone reaches 25, so a buffer sized from the longest message
	 * a *host* sends would fail handle_burst()'s size % 8 check and reject
	 * every 24-byte burst packet without a word.
	 */
	uint8_t msg_body[RADIANT_WIRE_MAX_SIZE_VALUE]; /* channel + payload */
	uint8_t body_idx;
	uint8_t running_xor;
} parser = { .state = S_SYNC };

/* Back to hunting for SYNC, with nothing of the abandoned frame kept.
 *
 * The body is wiped with the rest, not just the index: a frame abandoned half
 * way may have been a key message (see the wipe in S_CSUM below), and the
 * point of that wipe is that no key outlives the frame that carried it in
 * this long-lived buffer. A reset that left the body behind would be a second
 * way for one to.
 */
static void parser_reset(void)
{
	memset(&parser, 0, sizeof(parser));
	parser.state = S_SYNC;
}

static void process_byte(uint8_t b)
{
	switch (parser.state) {
	case S_SYNC:
		if (b == RADIANT_WIRE_SYNC_TX) {
			/* Seed with SYNC, not 0: it is part of the checksum. */
			parser.running_xor = b;
			parser.state = S_LEN;
		} else {
			/* A byte that was not SYNC while hunting for a frame
			 * start - noise, or the tail of a frame this parser
			 * lost sync on. */
			ant_health_note(ANT_HEALTH_HOST_DESYNC);
		}
		break;

	case S_LEN:
		if (b == RADIANT_WIRE_SYNC_TX) {
			/* 0xA4 can never be a LEN (> RADIANT_WIRE_MAX_SIZE_VALUE): the
			 * SYNC just taken was a stray byte and this is the real
			 * frame's start. Re-seed rather than swallow ~164 bytes
			 * of the host's next commands. */
			ant_health_note(ANT_HEALTH_HOST_DESYNC);
			parser.running_xor = b;
			break;		/* stay in S_LEN */
		}
		if (b > RADIANT_WIRE_MAX_SIZE_VALUE) {
			/* No message is this long, so this frame lost a byte
			 * (on a UART, a receiver overrun). Most often it was
			 * the LEN itself, and b is the message id: taken as a
			 * length it would swallow up to 255 bytes - the host's
			 * next several commands, each resent and each eaten,
			 * for as long as the host keeps the link quiet. Hunt
			 * for the next SYNC instead; this byte is counted as
			 * the S_SYNC hunt counts the rest of the frame. The
			 * buffer would also truncate such a frame below. */
			ant_health_note(ANT_HEALTH_HOST_DESYNC);
			parser.state = S_SYNC;
			break;
		}
		parser.msg_len = b;
		parser.running_xor ^= b;
		parser.body_idx = 0;
		parser.state = S_ID;
		break;

	case S_ID:
		parser.msg_id = b;
		parser.running_xor ^= b;
		parser.state = (parser.msg_len == 0) ? S_CSUM : S_BODY;
		break;

	case S_BODY:
		if (parser.body_idx < sizeof(parser.msg_body)) {
			parser.msg_body[parser.body_idx] = b;
		}
		parser.body_idx++;
		parser.running_xor ^= b;
		if (parser.body_idx >= parser.msg_len) {
			parser.state = S_CSUM;
		}
		break;

	case S_CSUM:
		if (b == parser.running_xor) {
#ifdef CONFIG_ANT_DONGLE_HOST_TRACE
			{
				uint8_t f[RADIANT_WIRE_MAX_FRAME_SIZE];
				uint8_t bl = MIN(parser.msg_len,
						 (uint8_t)sizeof(parser.msg_body));

				f[0] = RADIANT_WIRE_SYNC_TX;
				f[1] = parser.msg_len;
				f[2] = parser.msg_id;
				memcpy(&f[3], parser.msg_body, bl);
				f[3 + bl] = b;
				trace_frame("H>", f, 4U + bl);
				/* The copy holds whatever key the frame did;
				 * volatile so the dead store is not dropped. */
				for (volatile uint8_t *v = f;
				     v < f + 4U + bl; v++) {
					*v = 0;
				}
			}
#endif
			dispatch(parser.msg_id, parser.msg_body,
				 MIN(parser.msg_len,
				     (uint8_t)sizeof(parser.msg_body)));
		} else {
			/*
			 * The id and length only, never the checksums: with
			 * the header known, the expected one is the XOR of
			 * the body, so for a key message it is the XOR of
			 * the key, and the received one differs from it only
			 * by the line error. The host trace masks the
			 * checksum of a key frame for the same reason.
			 */
			ANT_BRIDGE_CSUM_WRN("Checksum err: id 0x%02X len %u",
					    parser.msg_id,
					    (unsigned int)parser.msg_len);
			ant_health_note(ANT_HEALTH_HOST_CSUM);
		}
		if (b != parser.running_xor
		    || key_msg_keep(parser.msg_id, parser.msg_body,
				    MIN(parser.msg_len,
					(uint8_t)sizeof(parser.msg_body)))
		       != KEY_MSG_NONE) {
			/*
			 * Every key-carrying message (key_msg_keep(): 0x46,
			 * 0x76, 0x7E, 0x7F's RNG seed, 0x83, 0xF2, and 0xF5
			 * by id alone, since its sub-command 0x02 carries the
			 * host's private X25519 scalar), in every
			 * configuration: a key sent to a build that refuses
			 * the message still crossed this buffer.
			 *
			 * And every frame that failed its checksum, whatever
			 * its id: one that lost its id byte (a UART overrun)
			 * carries a key under an id that names none, and its
			 * damaged header cannot be trusted to say otherwise.
			 * The error path only, so no cost on a good frame.
			 *
			 * A key just crossed this static, long-lived
			 * buffer, which would otherwise hold it until a later
			 * message overwrote it - indefinitely on a quiet link.
			 *
			 * After the checksum test, not inside its success arm:
			 * a key frame that FAILED its checksum still left its
			 * body here, and used to be kept. Unconditional for the
			 * same reason a key rejected for bad length is wiped -
			 * it was still a key.
			 *
			 * DOES NOT COVER: the same bytes passed through the USB
			 * ring buffer and transport buffer beneath it, neither
			 * reachable or wiped from here, and were in host memory
			 * before that. Nor a damaged key frame whose checksum
			 * still passes by chance under a non-key id (about one
			 * damaged frame in 256): it is dispatched, not wiped.
			 * Nor a key frame that never reaches this state: one cut off part way stays in the buffer until
			 * a session boundary's parser_reset() wipes it, which
			 * happens only when the bridge gets the new session's
			 * first byte, or until later frames happen to overwrite
			 * it (only as far as their own lengths reach). On a
			 * link that goes quiet after the cut, that is
			 * indefinitely. This narrows the exposure window, it
			 * does not close it.
			 */
			memset(parser.msg_body, 0, sizeof(parser.msg_body));
		}
		if (b != parser.running_xor && b == RADIANT_WIRE_SYNC_TX) {
			/* A failed checksum that is a SYNC: most often this frame
			 * lost a byte and b is the NEXT frame's SYNC, read one
			 * place early as the checksum. Re-seed on it, as S_LEN
			 * does, rather than hunting past it and losing that
			 * frame too. If b really was this frame's (corrupt)
			 * checksum, the real next SYNC arrives as the LEN and
			 * S_LEN re-seeds on that instead, at the cost of one
			 * desync count. */
			parser.running_xor = b;
			parser.state = S_LEN;
		} else {
			parser.state = S_SYNC;
		}
		break;
	}
}

#if !defined(ANT_BRIDGE_PARSER_ONLY)

/* ── Bridge thread ─────────────────────────────────────────────────────────── */

static void bridge_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("ANT serial bridge thread started");

	/*
	 * Host session boundaries - see ant_transport_session().
	 *
	 * A USB bus reset or re-configuration ends the host's session, and a
	 * host that reset mid-frame starts the new one with a fresh SYNC -
	 * which a parser still holding the old half-frame would take as the
	 * next body byte, eating the new session's first command (typically
	 * its RESET_SYSTEM) whole. So the parser is reset at the boundary,
	 * here on its own thread; the transport only publishes where the
	 * boundary is and never touches the parser or the ring, which this
	 * thread may be inside at that very moment.
	 *
	 * AT the boundary in the byte stream, not at the moment the bump is
	 * noticed. The transport publishes the session's first byte as a
	 * stream index (its running put count at the bump); this thread counts
	 * its own gets the same way, in rx_got, and resets the parser
	 * immediately before processing the byte whose index equals it. Reset
	 * on noticing instead and a bridge that was blocked when the host
	 * reset - handle_burst()'s wait for the block, up to 1000 ms, or
	 * handle_system_reset()'s fences - would come back to old bytes
	 * still queued, reset on the first of them, re-parse an old
	 * half-frame from its own SYNC, and eat the new session's SYNC
	 * exactly as before the fix.
	 *
	 * The change is always seen in time. Checked after every get: a byte
	 * at index >= the boundary was put after the boundary was published
	 * and the counter bumped (both precede the transport's arm_rx()), and
	 * this thread got it after it was put, so the check that follows its
	 * get sees the new counter. The comparison is by uint32_t difference,
	 * so the two running counts may wrap.
	 *
	 * What the old session's bytes still in the ring get: they run
	 * through the OLD parser state, as if no boundary had happened. A
	 * COMPLETE old frame among them is parsed and executed in the new
	 * session - accepted, the host did send it, and discarding it would
	 * mean flushing the ring from the transport's callback, racing this
	 * thread inside ring_buf_get(). A PARTIAL old frame is still part way
	 * through when the boundary byte arrives; the reset abandons it there,
	 * counted once as HOST_DESYNC, and none of its bytes reach the reset
	 * parser, so nothing more is counted for it.
	 *
	 * Two bumps before the first boundary is reached: the latest wins.
	 * This thread only ever holds one pending boundary, and the transport
	 * only publishes the latest, so if both are noticed before the
	 * earlier one is reached, the parser is reset at the later boundary
	 * only. The intermediate session's bytes then run through the old
	 * parser state - its first command can be eaten by an old half-frame,
	 * exactly the pre-fix behaviour, confined to that one short-lived
	 * session - and the latest session still starts clean. A second bump
	 * noticed after the first boundary was reached is simply a new
	 * pending boundary and is handled exactly.
	 *
	 * Seeded from the transport rather than assumed to be 0: the host may
	 * already have configured us before this thread first ran. Nothing
	 * has been got yet and the parser is in S_SYNC, so that boundary
	 * needs no reset.
	 */
	uint32_t rx_got = 0;
	uint32_t boundary;
	uint32_t seen = ant_transport_session(&boundary);
	bool reset_pending = false;

	while (1) {
		/* Block until the USB OUT callback signals new data */
		k_sem_take(&ant_rx_sem, K_FOREVER);

		/* Drain the ring buffer completely before sleeping again */
		uint8_t b;
		while (ring_buf_get(&ant_rx_ring_buf, &b, 1) == 1) {
			/* This byte's stream index; see above. */
			uint32_t idx = rx_got++;
			uint32_t at;
			uint32_t now = ant_transport_session(&at);

			if (now != seen) {
				seen = now;
				boundary = at;
				reset_pending = true;
			}

			if (reset_pending && (int32_t)(idx - boundary) >= 0) {
				reset_pending = false;
				/* Captured before the reset, which puts it
				 * back to S_SYNC: a frame abandoned part way is
				 * framing lost, counted with the bytes the
				 * S_SYNC hunt discards. A boundary that found
				 * the parser idle lost nothing and counts
				 * nothing.
				 */
				if (parser.state != S_SYNC) {
					ant_health_note(ANT_HEALTH_HOST_DESYNC);
				}
				parser_reset();
			}

			process_byte(b);
		}

		usb_ant_resume_rx();
	}
}

/* ── Radio event handler (outbound path, chip → host) ───────────────────────── */

/*
 * The one definition of radiant_ant_on_message() in the image; the backend calls it,
 * nothing registers it. The struct it takes is wire-shaped on purpose - three
 * fields, no unions - since this function's whole job is wrapping those bytes
 * in a frame.
 *
 * Runs on the backend's own thread/work queue/callback context, never
 * re-entrantly, never before radiant_ant_init() has returned. Touches only
 * statically-initialised state, so it does not depend on the bridge thread
 * existing - keep it that way: radiant_ant_init() runs before
 * ant_serial_bridge_init().
 */
#if defined(CONFIG_ANT_DONGLE_RX_TAP)
/*
 * The RadiANT bridge's ingest point. Declared here rather than in a header
 * because that is the whole hook: one declaration and one call, both compiled
 * out entirely in any image that does not set the symbol - which is every
 * apps/dongle build, so that image stays byte-identical and the `zero-cost` CI
 * job keeps its meaning.
 *
 * Resolved at link time, exactly like radiant_ant_on_message() itself and for the
 * same reason: there is one definition per image (apps/dongle_thread/src/
 * rx_tap.c) and nothing to register, so there is nothing here that can fail.
 * It reads a copy and returns; what goes out the transport below is unchanged.
 */
void ant_dongle_rx_tap(const struct radiant_ant_msg *msg);
#endif

static void on_message_forward(const struct radiant_ant_msg *msg);

void radiant_ant_on_message(const struct radiant_ant_msg *msg)
{
#if defined(CONFIG_ANT_DONGLE_RX_TAP)
	ant_dongle_rx_tap(msg);
#endif

	/*
	 * The activity indicators' whole ingest: one atomic increment, and only
	 * for the data-bearing message IDs. Whichever indicator is compiled in -
	 * the mono led0 blink, the NeoPixel hue wheel - reads that one counter;
	 * with neither, this is a macro expanding to nothing.
	 *
	 * Not folded into ant_dongle_rx_tap() above because that hook is
	 * resolved at link time with one definition per image, already owned by
	 * apps/dongle_thread/src/rx_tap.c - two features cannot share it.
	 */
	ant_activity_note_msg(msg->id);

	/* Counted around the whole forwarding path, so every early return in it
	 * is covered; handle_system_reset()'s first fence waits on this. The
	 * increment comes before on_message_forward() reads reset_in_progress,
	 * and that order is what the fence relies on. */
	atomic_inc(&evt_inflight);
	on_message_forward(msg);
	atomic_dec(&evt_inflight);
}

/* radiant_ant_on_message()'s body past the hooks: the reset check, burst buffer
 * handover, event filter and the frame for the host. */
static void on_message_forward(const struct radiant_ant_msg *msg)
{
	/*
	 * Wire format: [0xA4][LEN][ID][channel+payload...][XOR checksum]
	 * Total size  = LEN + 4 bytes.
	 */
	uint8_t msg_len = msg->len;
	uint8_t msg_id  = msg->id;
	const uint8_t *msg_data = msg->data;

	if (atomic_get(&reset_in_progress)) {
		return;
	}

	/* A channel event arrives as [channel, MESG_EVENT_ID, code]. Three of
	 * them mean the burst handler has finished with the block it was given,
	 * so the next packet from the host may overwrite it.
	 */
	if (msg_id == RADIANT_WIRE_MESG_RESPONSE_EVENT_ID && msg_len >= 3 &&
	    msg_data[1] == RADIANT_WIRE_MESG_EVENT_ID) {
		switch (msg_data[2]) {
		case RADIANT_WIRE_EVENT_TRANSFER_NEXT_DATA_BLOCK:
			burst_buf_release_oldest(msg_data[0]);
			/* Internal flow control for the buffer handover above.
			 * A real stick frames bursts itself and never puts this
			 * on the wire, so neither do we.
			 */
			return;
		case RADIANT_WIRE_EVENT_TRANSFER_TX_COMPLETED:
		case RADIANT_WIRE_EVENT_TRANSFER_TX_FAILED:
			/* Only the burst's own channel, and only once: an acked
			 * completion elsewhere must not free a block in flight. */
			burst_buf_release_all(msg_data[0]);
			/* The buffer is released; now honour the host's event
			 * filter (bit code-1, MESG_EVENT_FILTER_CONFIG). The
			 * radiant core hands these two on even when filtered,
			 * for exactly this reason. */
			{
				uint16_t filter = 0u;

				if (radiant_ant_event_filtering_get(&filter) == 0 &&
				    (filter & (uint16_t)(1u << (msg_data[2] - 1u))) != 0u) {
					return;
				}
			}
			break;
		default:
			break;
		}
	}

	/* Guard against oversized messages */
	if (msg_len > RADIANT_WIRE_MAX_SIZE_VALUE) {
		LOG_WRN("Oversized ANT event: id=0x%02X len=%d", msg_id, msg_len);
		return;
	}

	/* Sized like send_message(), from RADIANT_WIRE_MAX_FRAME_SIZE - never
	 * RADIANT_WIRE_MESG_MAX_SIZE, which is one byte short since it doesn't count
	 * the channel byte the LEN ceiling does.
	 */
	uint8_t buf[RADIANT_WIRE_MAX_FRAME_SIZE];
	uint8_t xor = RADIANT_WIRE_SYNC_TX;

	buf[0] = RADIANT_WIRE_SYNC_TX;
	buf[1] = msg_len;   xor ^= msg_len;
	buf[2] = msg_id;    xor ^= msg_id;

	for (uint8_t i = 0; i < msg_len; i++) {
		buf[3 + i] = msg_data[i];
		xor ^= msg_data[i];
	}
	buf[3 + msg_len] = xor;

	TRACE_TX(buf, (size_t)(4 + msg_len));
	int ret = usb_ant_send_async(buf, (size_t)(4 + msg_len));
	if (ret) {
		LOG_WRN("Dropped ANT event 0x%02X due to USB TX backlog: %d",
			msg_id, ret);
	}
}

/* ── Public init ────────────────────────────────────────────────────────────── */

int ant_serial_bridge_init(void)
{
	tx_power_reset();

	/* No callback registration: radiant_ant_on_message() above is resolved at link
	 * time, so there is nothing here that can fail and nothing to report.
	 */
	k_thread_create(&bridge_thread_data, bridge_stack,
			K_THREAD_STACK_SIZEOF(bridge_stack),
			bridge_thread_fn, NULL, NULL, NULL,
			BRIDGE_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&bridge_thread_data, "ant_bridge");

	return 0;
}

#endif /* !ANT_BRIDGE_PARSER_ONLY */
