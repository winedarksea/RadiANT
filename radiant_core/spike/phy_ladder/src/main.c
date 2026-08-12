/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Spike phy_ladder: the instrument for ADR 0007's outstanding bench gate -
 * ">= 6 dB improvement in the 5 %-loss point for the coded PHY at S=8 versus
 * 1 M GFSK, on the same rig".
 *
 * Provenance: clean-room. Written from
 *   - docs/decisions/0007-long-range-phy.md, "Verification status", which is
 *     where that gate is recorded as owed and unmeasured, and its "Two
 *     limitations" section, which is where the 1m-long objective below comes
 *     from,
 *   - radiant_core/include/radiant_core/radiant_radio_hal.h, the frozen backend
 *     contract (the arm/callback rules, t_sync, caps),
 *   - radiant_core/include/radiant_core/radiant_frame.h and the two shipping
 *     encoders it declares - radiant_frame_encode() and radiant_frame_lr_body(),
 *   - radiant_core/spike/x1m_len/src/main.c, this repository's own spike, for
 *     the build guard, the frame-B bytes this program's third mode receives,
 *     and the shape of a bench program,
 *   - tools/ab_gates.toml's header, for the "read loss (exact), never the
 *     wall-clock line" rule this program's counting obeys.
 * Nothing here derives from sdk-ant, from libant.a, or from any adopter-gated
 * ANT+ device profile document. See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * Why this is firmware and not tools/ant_sens.py
 * ---------------------------------------------------------------------------
 * The ladder tools/ant_sens.py runs - walk the master's transmit power down
 * until the receiver misses 5 % - is exactly the right measurement, and it
 * cannot be used here. It drives boards over the ANT serial protocol, and NO
 * ANT SERIAL MESSAGE SELECTS A PHY. There is no message to add without
 * inventing one, so the ladder has to live where the PHY selection lives:
 * inside an image that calls radiant_radio_tx() with a struct radiant_pkt_format.
 *
 * ---------------------------------------------------------------------------
 * One application, two roles, selected at run time
 * ---------------------------------------------------------------------------
 * A rung of a ladder is one power setting. A build-time switch, or two
 * applications, would make a twelve-rung ladder cost a dozen flashes - and on
 * this bench one of the two radios is the Adafruit Feather, whose flashes are
 * rationed and physically gated. So the role and the PHY are console commands:
 *
 *     role tx | rx
 *     phy 1m | s8 | 1m-long        (1m-long is RX only - see below)
 *     power <dbm>                  (TX role)
 *     start | stop
 *     stats
 *     help
 *
 * ---------------------------------------------------------------------------
 * COUNTING LOSS WITHOUT A CLOCK, which is the whole design
 * ---------------------------------------------------------------------------
 * tools/ab_gates.toml states the rule this project already learned the hard
 * way: read `loss (exact)`, never the wall-clock line. A wall-clock figure
 * divides elapsed time by a nominal period and so quietly assumes the
 * transmitter's crystal, the receiver's crystal and the operator's stopwatch
 * all agree. They do not, and the disagreement is the same order as the effect
 * being measured.
 *
 * So the transmitter puts an incrementing 32-bit SEQUENCE NUMBER in the
 * payload and the receiver counts gaps in it:
 *
 *     expected = highest_seq_seen - first_seq_seen + 1
 *     received = frames that decoded and carried this program's marker
 *     loss     = (expected - received) / expected
 *
 * There is no clock anywhere in that. The only three numbers the gate may be
 * read from are the three printed on the `[rx] seq` line, and `stats` prints
 * no rate, no packets-per-second and no elapsed time on purpose - a figure
 * divided by wall-clock time is exactly the number this project has already
 * once mistaken for a result.
 *
 * A CONSEQUENCE THAT MUST BE OBEYED AT THE BENCH: `start` on the TX role resets
 * the sequence to zero. A receiver that is already running when that happens
 * sees the sequence go BACKWARDS, and every number after it is nonsense. That
 * is not silently repaired here - it is counted, and `stats` marks the run VOID
 * if it ever happened. Restart the TX first, then the RX. See README.md.
 *
 * ---------------------------------------------------------------------------
 * The shipping frame formats, not local copies
 * ---------------------------------------------------------------------------
 * radiant_frame_format(RADIANT_FRAME_CFG_TRACKING) for 1 M and
 * radiant_frame_format(RADIANT_FRAME_CFG_LR) for S=8, with radiant_frame_encode()
 * and radiant_frame_lr_body() composing the bodies. A ladder run against a
 * locally-declared format would measure a format nothing ships, and the number
 * would then have to be re-measured the first time the shipping one was used.
 *
 * The ONE local format in this file is the third mode's, and it is local
 * because the thing it is byte-compatible with is also local to another spike -
 * see "The secondary objective" below.
 *
 * ---------------------------------------------------------------------------
 * The secondary objective: `phy 1m-long`
 * ---------------------------------------------------------------------------
 * ADR 0007 records the 2026-08-11 capture that proved a stock ANT dongle
 * IGNORES a length-extended 1 M frame, and then records, in the same breath,
 * exactly what that capture does not prove:
 *
 *   > This run proves HARMLESSNESS, not USABILITY [...] A frame that was
 *   > malformed for some reason unrelated to its length would produce a
 *   > byte-for-byte identical result.
 *
 * Closing it is one run: a radiant_core receiver reading spike x1m_len's frame
 * B off the air. `phy 1m-long` is that receiver. Its format is a local
 * RADIANT_LEN_FIXED / addr_len 5 / body_len 30 declaration because that is what
 * frame B is - a local declaration in radiant_core/spike/x1m_len/src/main.c -
 * and the two must be byte-identical or the run proves nothing. This mode
 * therefore checks all thirty body bytes against what x1m_len builds, not just
 * that something arrived: "a frame arrived" is consistent with a matcher firing
 * on noise, and this is the one mode where the frame carries no sequence number
 * to cross-check against.
 *
 * ---------------------------------------------------------------------------
 * Why RX re-arms from the callback and TX does not
 * ---------------------------------------------------------------------------
 * radiant_radio_hal.h permits re-arming inside a completion callback and calls
 * it the low-jitter path. Spike x1m_len declined it, because at 8 transmissions
 * a second nothing needed it. Here the two sides differ:
 *
 *   TRANSMIT is thread-driven, as x1m_len is, for the same reason: the schedule
 *   is absolute, so thread latency moves when the arm call happens and not when
 *   the frame leaves the antenna.
 *
 *   RECEIVE re-arms from the terminal event, and that IS load-bearing. Every
 *   microsecond between one window closing and the next opening is a
 *   microsecond in which a transmitted frame is lost to the instrument rather
 *   than to the link - and this program's entire output is a loss figure. The
 *   windows are a second long each, so the residual gap is the backend's arm
 *   path, order 10^-4 of the run. It is not zero, and README.md says so rather
 *   than letting a reader assume it is.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <radiant_core/radiant_frame.h>
#include <radiant_core/radiant_radio_hal.h>

/* ---------------------------------------------------------------------------
 * Build-time guards
 * ---------------------------------------------------------------------------
 */

/*
 * THE ONE THAT MATTERS, copied verbatim in intent from
 * radiant_core/spike/x1m_len/src/main.c because it has already cost this
 * project real bench sessions.
 *
 * CONFIG_RADIANT_CORE_BACKEND_NRF is a `depends on SOC_COMPATIBLE_NRF52X ||
 * SOC_COMPATIBLE_NRF54LX` choice entry. Ask for it on a part outside that pair -
 * an nRF5340, either core - and nothing errors: the dependency is unmet,
 * Kconfig falls back to RADIANT_CORE_BACKEND_NULL, and the image builds, boots,
 * accepts every console command and refuses every arm with ENOTSUP.
 *
 * On a ladder that is worse than on x1m_len. The null backend's receiver hears
 * nothing, so it manufactures 100 % loss at every rung - which looks exactly
 * like a rig whose attenuation is too high, and an operator would spend the
 * session moving boards closer together.
 */
BUILD_ASSERT(IS_ENABLED(CONFIG_RADIANT_CORE_BACKEND_NRF),
	     "This spike transmits and receives. CONFIG_RADIANT_CORE_BACKEND_NRF "
	     "did not take - it depends on SOC_COMPATIBLE_NRF52X || "
	     "SOC_COMPATIBLE_NRF54LX and Kconfig fell back to the null backend, "
	     "which refuses every arm. Build for nrf54l15dk/nrf54l15/cpuapp or "
	     "adafruit_feather_nrf52840/nrf52840/uf2; the nRF5340 DK cannot run "
	     "this at all - see README.md.");

/* The backend asserts this for itself; repeated here because this file is what
 * an operator reads when the board is silent. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT),
	     "The nrf backend owns the RADIO outright; CONFIG_BT must be off.");

/* ---------------------------------------------------------------------------
 * The channel this program uses
 * ---------------------------------------------------------------------------
 */

#define RF_INDEX        RADIANT_RF_INDEX_ANT_PLUS  /* 57 - the bench's channel */

#define DEVICE_TYPE     0x60u   /* the RadiANT telemetry envelope's type */
#define TRANS_TYPE      0x05u

/*
 * 0x60C0, deliberately neither of spike x1m_len's numbers. Both spikes may be
 * on this bench at once - `phy 1m-long` exists precisely so that they are - and
 * a ladder whose 1 M receiver also matched x1m_len's control frame would be
 * counting somebody else's packets into its own loss figure.
 */
#define DEVNUM_LADDER   0x60C0u

/* Spike x1m_len's frame B, which `phy 1m-long` receives. These three constants
 * and the thirty body bytes below are the interface between the two programs;
 * they are duplicated rather than shared because a spike that reached into
 * another spike's source is a spike that breaks when that one is deleted. */
#define DEVNUM_X1M_LONG 0x60B0u
#define X1M_LONG_BODY_LEN 30u
#define X1M_LONG_TRANS_TYPE 0x05u
#define X1M_LONG_CTRL       0x0Au

/*
 * 20 Hz, and THE SAME 20 Hz ON BOTH PHYS.
 *
 * The gate is a comparison, so anything that differs between the two arms
 * other than the PHY is a confound. Cadence is the one that would be tempting
 * to change - a coded frame is 1.3 ms of airtime against 0.1 ms at 1 M, so
 * "slow S=8 down a bit" looks harmless - and it is not: the collision exposure
 * of a bench with other ANT traffic on RF 57 scales with how often this program
 * is on the air, and this bench has a characterised ~0.4 % collision floor.
 * Same rate, same exposure, same floor on both arms.
 *
 * 20 Hz also sizes a rung: 1,000 packets in 50 s, which resolves a 5 % point to
 * about +/-0.7 % (1-sigma binomial) - comfortably finer than the 6 dB the gate
 * is asking about.
 */
#define SLOT_US         50000u

/* 1 s. Long, because the gap between one window closing and the next opening is
 * loss this instrument invents; see the header. */
#define RX_WINDOW_US    1000000u

/* Payload: [seq32 LE][phy tag][3 marker bytes]. Eight bytes, which is what the
 * tracking format is fixed at, and which the LR format is happy to carry. */
#define PAYLOAD_LEN     8u
#define PHY_TAG_1M      0x01u
#define PHY_TAG_S8      0x08u
#define MARK0           0xA5u
#define MARK1           0x5Au
#define MARK2           0xC3u

BUILD_ASSERT(PAYLOAD_LEN == RADIANT_FRAME_PAYLOAD_STD,
	     "the tracking format is fixed at eight payload bytes");
BUILD_ASSERT(X1M_LONG_BODY_LEN <= RADIANT_RADIO_BODY_MAX,
	     "x1m_len's frame B no longer fits RADIANT_RADIO_BODY_MAX");

/* ---------------------------------------------------------------------------
 * The one local packet format
 *
 * Byte-for-byte spike x1m_len's fmt_long: RADIANT_LEN_FIXED on
 * RADIANT_PHY_1M_GFSK, five address bytes, thirty body bytes, and ANT's CRC.
 * It is local for the reason given in the header - the thing it must match is
 * itself local to another spike - and it is the only format in this file that
 * radiant_frame.c does not supply.
 * ---------------------------------------------------------------------------
 */
static const struct radiant_pkt_format fmt_1m_long = {
	.phy          = RADIANT_PHY_1M_GFSK,
	.addr_len     = (uint8_t)RADIANT_FRAME_ADDR_LEN_TRACKING,
	.len_mode     = RADIANT_LEN_FIXED,
	.body_len     = X1M_LONG_BODY_LEN,
	.len_offset   = 0,
	.len_bias     = 0,
	.max_body_len = X1M_LONG_BODY_LEN,
	.crc = {
		.width_bits  = 16,
		.poly        = RADIANT_CRC_POLY,
		.init        = RADIANT_CRC_INIT,
		.xor_out     = 0,
		.reflect_in  = false,
		.reflect_out = false,
		.cover_addr  = true,
	},
};

/* ---------------------------------------------------------------------------
 * State
 * ---------------------------------------------------------------------------
 */

enum role {
	ROLE_NONE = 0,
	ROLE_TX,
	ROLE_RX,
};

enum ladder_mode {
	MODE_1M = 0,     /* RADIANT_FRAME_CFG_TRACKING, the comparison's baseline */
	MODE_S8,         /* RADIANT_FRAME_CFG_LR, the comparison's subject */
	MODE_1M_LONG,    /* x1m_len frame B - RX only, the secondary objective */
};

static const char *const mode_name[] = { "1m", "s8", "1m-long" };

static enum role        cur_role = ROLE_NONE;
static enum ladder_mode cur_mode = MODE_1M;
static int8_t           cur_power_dbm;

/* Read from the radio callback as well as from the console thread. */
static volatile bool running;

static const struct radiant_radio_caps *caps;

/* --- transmit counters ---------------------------------------------------- */
static uint32_t tx_seq;        /* the next sequence number to put on the air */
static uint32_t tx_sent;
static uint32_t tx_failed;
static uint32_t tx_refused;
static uint32_t tx_timeouts;
static uint32_t tx_resyncs;
static int      tx_last_refusal;

/* --- receive counters ----------------------------------------------------- */
static uint32_t rx_frames;      /* OK events carrying this program's marker */
static uint32_t rx_alien;       /* OK events that decoded but were not ours */
static uint32_t rx_decode_fail; /* OK events the shipping decoder rejected */
static uint32_t rx_crc_fail;
static uint32_t rx_windows;     /* terminal TIMEOUTs - the "still alive" count */
static uint32_t rx_dups;
static uint32_t rx_backwards;   /* the VOID condition; see the header */
static uint32_t rx_rearm_fail;
static bool     rx_have_first;
static uint32_t rx_first_seq;
static uint32_t rx_high_seq;
static int32_t  rx_rssi_min;
static int32_t  rx_rssi_max;
static int32_t  rx_rssi_sum;
static uint32_t rx_rssi_n;
/* `phy 1m-long` only: the thirty received bytes matched x1m_len's frame B
 * exactly, or they did not. */
static uint32_t rx_long_exact;
static uint32_t rx_long_wrong;

/* ---------------------------------------------------------------------------
 * Frame construction
 * ---------------------------------------------------------------------------
 */

static void payload_fill(uint8_t *p, uint32_t seq, uint8_t phy_tag)
{
	p[0] = (uint8_t)(seq & 0xFFu);
	p[1] = (uint8_t)((seq >> 8) & 0xFFu);
	p[2] = (uint8_t)((seq >> 16) & 0xFFu);
	p[3] = (uint8_t)((seq >> 24) & 0xFFu);
	p[4] = phy_tag;
	p[5] = MARK0;
	p[6] = MARK1;
	p[7] = MARK2;
}

static bool payload_is_ours(const uint8_t *p, uint8_t len)
{
	return len == PAYLOAD_LEN && p[5] == MARK0 && p[6] == MARK1 &&
	       p[7] == MARK2;
}

static uint32_t payload_seq(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const struct radiant_channel_id id_ladder = {
	.device_number = DEVNUM_LADDER,
	.device_type   = DEVICE_TYPE,
	.trans_type    = TRANS_TYPE,
};

static const struct radiant_channel_id id_x1m_long = {
	.device_number = DEVNUM_X1M_LONG,
	.device_type   = DEVICE_TYPE,
	.trans_type    = TRANS_TYPE,
};

/*
 * The thirty bytes spike x1m_len's frame_build() writes for frame B, rebuilt
 * here from the same rule rather than copied as a literal array: a literal
 * would be one edit away from disagreeing with the transmitter silently, and a
 * disagreement in these bytes is a failed run that looks like a broken frame.
 *
 *   body[0] = transmission type
 *   body[1] = control byte (broadcast)
 *   body[2] = the low byte of the device number
 *   body[i] = i, for i in 3..29
 */
static uint8_t x1m_long_expect[X1M_LONG_BODY_LEN];

static void x1m_long_expect_build(void)
{
	x1m_long_expect[0] = X1M_LONG_TRANS_TYPE;
	x1m_long_expect[1] = X1M_LONG_CTRL;
	x1m_long_expect[2] = (uint8_t)(DEVNUM_X1M_LONG & 0xFFu);
	for (uint8_t i = 3; i < X1M_LONG_BODY_LEN; i++) {
		x1m_long_expect[i] = i;
	}
}

/* ---------------------------------------------------------------------------
 * The HAL callbacks
 *
 * These run in the RADIO interrupt at the backend's highest priority. What they
 * may do is enumerated in radiant_radio_hal.h and it is short: read the event,
 * re-arm, signal an ISR-safe object, return. Nothing here blocks, allocates,
 * prints or retains evt->body.
 * ---------------------------------------------------------------------------
 */

static K_SEM_DEFINE(tx_done, 0, 1);
static K_SEM_DEFINE(engine_wake, 0, 1);

static volatile enum radiant_radio_status tx_status;

static void on_tx(const struct radiant_tx_event *evt, void *user)
{
	ARG_UNUSED(user);

	tx_status = evt->status;
	k_sem_give(&tx_done);
}

/* Static because the backend keeps the pointer until the terminal event, and
 * because rx_arm() is called from interrupt context where a stack-lifetime
 * request would be a use-after-return the moment the ISR unwound. */
static struct radiant_rx_filter rx_filter;
static struct radiant_rx_req    rx_req;
static uint32_t                 rx_lead_us;
static volatile bool            rx_arm_owed;

static const struct radiant_pkt_format *mode_rx_format(enum ladder_mode m)
{
	switch (m) {
	case MODE_1M:
		return radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	case MODE_S8:
		return radiant_frame_format(RADIANT_FRAME_CFG_LR);
	default:
		return &fmt_1m_long;
	}
}

static int rx_arm(void)
{
	radiant_time_t now = radiant_radio_now();
	uint32_t op = 0;

	rx_req.t_open  = now + (radiant_time_t)rx_lead_us;
	rx_req.t_close = rx_req.t_open + (radiant_time_t)RX_WINDOW_US;

	return radiant_radio_rx(&rx_req, &op);
}

/* Fold one sequence number into the loss accounting. No clock, by construction:
 * the only inputs are the number in the payload and the numbers seen before
 * it. */
static void seq_record(uint32_t seq)
{
	rx_frames++;

	if (!rx_have_first) {
		rx_have_first = true;
		rx_first_seq  = seq;
		rx_high_seq   = seq;
		return;
	}
	if (seq > rx_high_seq) {
		rx_high_seq = seq;
	} else if (seq == rx_high_seq) {
		rx_dups++;
	} else if (seq < rx_first_seq) {
		/* Only a restarted transmitter can do this. Counted, never
		 * repaired - see the header. */
		rx_backwards++;
	}
	/* seq strictly between first and high is an ordinary out-of-order or
	 * late frame and is exactly what the gap counting is for; nothing to
	 * record beyond the increment above. */
}

static void rx_handle_ok(const struct radiant_rx_event *evt)
{
	if (evt->has_rssi) {
		if (rx_rssi_n == 0u || evt->rssi_dbm < rx_rssi_min) {
			rx_rssi_min = evt->rssi_dbm;
		}
		if (rx_rssi_n == 0u || evt->rssi_dbm > rx_rssi_max) {
			rx_rssi_max = evt->rssi_dbm;
		}
		rx_rssi_sum += evt->rssi_dbm;
		rx_rssi_n++;
	}

	if (cur_mode == MODE_1M_LONG) {
		/*
		 * No sequence number exists in x1m_len's frame B, so the
		 * question here is not "how many" but "is it the frame". All
		 * thirty bytes are checked: a matcher firing on noise also
		 * produces an event, and "something arrived" is not the claim
		 * ADR 0007 is owed.
		 */
		if (evt->body_len == X1M_LONG_BODY_LEN &&
		    memcmp(evt->body, x1m_long_expect, X1M_LONG_BODY_LEN) == 0) {
			rx_long_exact++;
		} else {
			rx_long_wrong++;
		}
		return;
	}

	if (cur_mode == MODE_S8) {
		struct radiant_channel_id id;
		const uint8_t *payload = NULL;
		uint8_t payload_len = 0;
		uint8_t ctrl = 0;

		if (radiant_frame_lr_parse(evt->body, evt->body_len, &id, &ctrl,
					   &payload, &payload_len) !=
		    RADIANT_FRAME_OK) {
			rx_decode_fail++;
			return;
		}
		if (!payload_is_ours(payload, payload_len)) {
			rx_alien++;
			return;
		}
		seq_record(payload_seq(payload));
		return;
	}

	/*
	 * 1 M, through the shipping decoder rather than by reading bytes out of
	 * evt->body directly. The address is not in the buffer - in tracking
	 * geometry the matcher consumed it and radiant_radio_hal.h says a
	 * matched byte never reaches RAM - so it is refilled here from the
	 * filter this program armed, which is the same recovery radiant_api.c
	 * performs. RADIANT_FRAME_TRUSTED_CRC because caps.crc_in_hw is true and
	 * an OK event means a verified CRC by contract; the received CRC bytes
	 * are not in the buffer to re-check.
	 */
	{
		struct radiant_frame_wire w;
		struct radiant_frame f;

		if (evt->body_len > sizeof(w.body)) {
			rx_decode_fail++;
			return;
		}
		memset(&w, 0, sizeof(w));
		memcpy(w.addr, rx_filter.addr, RADIANT_FRAME_ADDR_MAX);
		w.addr_len = rx_filter.addr_len;
		memcpy(w.body, evt->body, evt->body_len);
		w.body_len = evt->body_len;

		if (radiant_frame_decode(RADIANT_FRAME_CFG_TRACKING, &w,
					 RADIANT_FRAME_TRUSTED_CRC,
					 &f) != RADIANT_FRAME_OK) {
			rx_decode_fail++;
			return;
		}
		if (!payload_is_ours(f.payload, f.payload_len)) {
			rx_alien++;
			return;
		}
		seq_record(payload_seq(f.payload));
	}
}

static void on_rx(const struct radiant_rx_event *evt, void *user)
{
	ARG_UNUSED(user);

	switch (evt->status) {
	case RADIANT_RADIO_STATUS_OK:
		rx_handle_ok(evt);
		return;   /* not terminal: STOP_ON_FIRST is deliberately unset */
	case RADIANT_RADIO_STATUS_CRC_FAIL:
		rx_crc_fail++;
		return;
	case RADIANT_RADIO_STATUS_TIMEOUT:
		rx_windows++;
		break;
	default:
		break;    /* ABORTED or FAILED - both terminal */
	}

	if (!running || cur_role != ROLE_RX) {
		return;
	}

	/*
	 * The low-jitter path, and here it is the accurate path: the gap
	 * between windows is loss this instrument would invent. On a refusal,
	 * hand the retry to the engine thread rather than spinning in an ISR.
	 */
	if (rx_arm() != RADIANT_RADIO_OK_RC) {
		rx_rearm_fail++;
		rx_arm_owed = true;
		k_sem_give(&engine_wake);
	}
}

static const struct radiant_radio_cbs cbs = {
	.rx = on_rx,
	.tx = on_tx,
	.ed = NULL,   /* CONFIG_RADIANT_CORE_ED_SCAN is off; the backend must not call it */
};

/* ---------------------------------------------------------------------------
 * Statistics
 * ---------------------------------------------------------------------------
 */

static void stats_reset(void)
{
	tx_seq = 0;
	tx_sent = 0;
	tx_failed = 0;
	tx_refused = 0;
	tx_timeouts = 0;
	tx_resyncs = 0;
	tx_last_refusal = 0;

	rx_frames = 0;
	rx_alien = 0;
	rx_decode_fail = 0;
	rx_crc_fail = 0;
	rx_windows = 0;
	rx_dups = 0;
	rx_backwards = 0;
	rx_rearm_fail = 0;
	rx_have_first = false;
	rx_first_seq = 0;
	rx_high_seq = 0;
	rx_rssi_min = 0;
	rx_rssi_max = 0;
	rx_rssi_sum = 0;
	rx_rssi_n = 0;
	rx_long_exact = 0;
	rx_long_wrong = 0;
}

/* Loss in hundredths of a percent, so that nothing here needs floating point
 * (CONFIG_CBPRINTF_FP_SUPPORT is off, as in every spike in this tree).
 * UINT32_MAX means "not defined yet" - no frame has arrived. */
static uint32_t loss_hundredths(uint32_t expected, uint32_t received)
{
	if (expected == 0u) {
		return UINT32_MAX;
	}
	if (received >= expected) {
		return 0u;
	}
	return (uint32_t)(((uint64_t)(expected - received) * 10000u) / expected);
}

static void stats_print(void)
{
	if (cur_role == ROLE_TX) {
		printk("[tx] phy=%s power=%d dBm running=%d | sent=%u failed=%u "
		       "refused=%u (last rc=%d) timeout=%u resync=%u next_seq=%u\n",
		       mode_name[cur_mode], cur_power_dbm, running ? 1 : 0,
		       tx_sent, tx_failed, tx_refused, tx_last_refusal,
		       tx_timeouts, tx_resyncs, tx_seq);
		return;
	}
	if (cur_role != ROLE_RX) {
		printk("[--] no role selected. `role tx` or `role rx`.\n");
		return;
	}

	printk("[rx] phy=%s running=%d | windows=%u crc_fail=%u decode_fail=%u "
	       "alien=%u rearm_fail=%u\n",
	       mode_name[cur_mode], running ? 1 : 0, rx_windows, rx_crc_fail,
	       rx_decode_fail, rx_alien, rx_rearm_fail);

	if (cur_mode == MODE_1M_LONG) {
		/*
		 * Deliberately NOT a loss figure. Frame B carries no sequence
		 * number, so there is nothing to count gaps in, and a
		 * percentage here would be a wall-clock number wearing a
		 * disguise. The claim this mode exists to support is binary.
		 */
		printk("[rx] x1m_len frame B: exact=%u wrong_bytes=%u  "
		       "(no sequence in this frame - loss is NOT defined here)\n",
		       rx_long_exact, rx_long_wrong);
		printk("[rx] READABLE if exact>0 and wrong_bytes==0. See README.md.\n");
	} else if (!rx_have_first) {
		printk("[rx] seq: nothing received yet - expected/loss undefined\n");
	} else {
		uint32_t expected = rx_high_seq - rx_first_seq + 1u;
		uint32_t h = loss_hundredths(expected, rx_frames);

		printk("[rx] seq first=%u high=%u expected=%u received=%u "
		       "dups=%u loss=%u.%02u%%\n",
		       rx_first_seq, rx_high_seq, expected, rx_frames, rx_dups,
		       h / 100u, h % 100u);
	}

	if (rx_rssi_n > 0u) {
		printk("[rx] rssi min=%d mean=%d max=%d dBm over %u frames\n",
		       (int)rx_rssi_min, (int)(rx_rssi_sum / (int32_t)rx_rssi_n),
		       (int)rx_rssi_max, rx_rssi_n);
	}
	if (rx_backwards > 0u) {
		printk("[rx] *** VOID: the sequence went BACKWARDS %u times. The "
		       "transmitter restarted mid-run; every number above is "
		       "meaningless. Restart the TX first, then the RX. ***\n",
		       rx_backwards);
	}
}

/* ---------------------------------------------------------------------------
 * The engine thread
 *
 * Transmit is driven from here on an absolute schedule, exactly as spike
 * x1m_len drives its two frames: thread latency then moves when the arm call
 * happens rather than when the frame leaves the antenna. Receive needs this
 * thread only to retry an arm the interrupt could not place.
 * ---------------------------------------------------------------------------
 */

/* Static, because the backend DMAs out of req.body from the arm call to the
 * completion callback and a stack buffer would be a lifetime question this
 * program has no reason to raise. */
static struct radiant_frame_wire tx_wire;
static uint8_t                   tx_lr_body[RADIANT_FRAME_LR_BODY_MAX];
static uint8_t                   tx_addr[RADIANT_RADIO_ADDR_MAX];

/* Compose the next frame. Returns its body length, or a negative
 * RADIANT_FRAME_* code. */
static int tx_build(struct radiant_tx_req *req, uint32_t seq)
{
	uint8_t payload[PAYLOAD_LEN];
	int rc;

	if (cur_mode == MODE_S8) {
		payload_fill(payload, seq, PHY_TAG_S8);

		rc = radiant_frame_addr(RADIANT_FRAME_CFG_LR,
					radiant_net_addr_ant_plus, &id_ladder,
					tx_addr, sizeof(tx_addr));
		if (rc < 0) {
			return rc;
		}
		req->addr_len = (uint8_t)rc;

		rc = radiant_frame_lr_body(&id_ladder, RADIANT_CTRL_BROADCAST,
					   payload, PAYLOAD_LEN, tx_lr_body,
					   sizeof(tx_lr_body));
		if (rc < 0) {
			return rc;
		}
		req->fmt  = radiant_frame_format(RADIANT_FRAME_CFG_LR);
		req->body = tx_lr_body;
		memcpy(req->addr, tx_addr, RADIANT_RADIO_ADDR_MAX);
		return rc;
	}

	/* 1 M, through the ordinary encode path. */
	{
		struct radiant_frame f;
		struct radiant_ctrl_fields ctrl = {
			.exchange    = false,
			.ack         = false,
			.last        = false,
			.seq         = false,
			/* Produces 0x0A - RADIANT_CTRL_BROADCAST, the only
			 * encoding with bit 7 clear that has ever been on this
			 * bench's air. radiant_frame_make() refuses anything
			 * radiant_ctrl_observed() has not seen, which is the
			 * check this program wants rather than a byte set by
			 * hand. */
			.slot_opener = true,
		};

		payload_fill(payload, seq, PHY_TAG_1M);

		rc = radiant_frame_make(&f, &id_ladder, &ctrl, payload,
					PAYLOAD_LEN);
		if (rc != RADIANT_FRAME_OK) {
			return rc;
		}
		rc = radiant_frame_encode(RADIANT_FRAME_CFG_TRACKING,
					  radiant_net_addr_ant_plus, &f,
					  &tx_wire);
		if (rc != RADIANT_FRAME_OK) {
			return rc;
		}
		req->fmt      = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
		req->body     = tx_wire.body;
		req->addr_len = tx_wire.addr_len;
		memcpy(req->addr, tx_wire.addr, RADIANT_RADIO_ADDR_MAX);
		return (int)tx_wire.body_len;
	}
}

static void tx_run(void)
{
	struct radiant_tx_req req;
	radiant_time_t next;
	uint32_t lead;

	memset(&req, 0, sizeof(req));
	req.rf_index = RF_INDEX;

	lead = (uint32_t)caps->min_arm_lead_us + 3000u;
	next = radiant_radio_now() + lead + 1000u;

	while (running) {
		radiant_time_t now = radiant_radio_now();
		uint32_t op = 0;
		int body_len;
		int rc;

		/* Fell behind - a console write, or a refused arm that cost a
		 * slot. Re-anchor rather than firing catch-up frames, which
		 * would change the cadence the two PHYs are supposed to share
		 * and therefore change the collision exposure between arms. */
		if (next < now + lead) {
			next = now + lead;
			tx_resyncs++;
		} else {
			radiant_time_t arm_at = next - lead;

			if (arm_at > now) {
				k_sleep(K_USEC((int64_t)(arm_at - now)));
			}
			if (!running) {
				break;
			}
		}

		body_len = tx_build(&req, tx_seq);
		if (body_len < 0) {
			tx_refused++;
			tx_last_refusal = body_len;
			printk("[tx] FRAME BUILD FAILED rc=%d - stopping\n",
			       body_len);
			running = false;
			break;
		}
		req.body_len  = (uint8_t)body_len;
		req.power.dbm = cur_power_dbm;
		req.power.use_raw = false;
		req.t_sync_at = next;

		rc = radiant_radio_tx(&req, &op);
		if (rc != RADIANT_RADIO_OK_RC) {
			/*
			 * A refusal is a RESULT, not a hiccup. ENOTSUP on
			 * `phy s8` means this part's RADIO has no coded mode
			 * and the entire comparison is unavailable on this
			 * board - which must be loud, because the ladder would
			 * otherwise read as "S=8 loses every packet".
			 *
			 * Printed on the first occurrence of each distinct
			 * code and then once every hundred, rather than every
			 * time: at 20 Hz an unconditional line is 20 writes a
			 * second down a USB CDC console whose ring buffer
			 * discards when full, and the first line - the one
			 * that says WHICH refusal it is - would be the one
			 * lost. The counter carries the rest.
			 */
			int prev = tx_last_refusal;

			tx_refused++;
			tx_last_refusal = rc;
			if (rc != prev || (tx_refused % 100u) == 0u) {
				printk("[tx] ARM REFUSED rc=%d x%u (ENOTSUP=%d "
				       "EINVAL=%d ETIME=%d EBUSY=%d ESTATE=%d)\n",
				       rc, tx_refused,
				       RADIANT_RADIO_ENOTSUP, RADIANT_RADIO_EINVAL,
				       RADIANT_RADIO_ETIME, RADIANT_RADIO_EBUSY,
				       RADIANT_RADIO_ESTATE);
			}
		} else if (k_sem_take(&tx_done, K_MSEC(200)) != 0) {
			/* The HAL guarantees exactly one terminal event per
			 * accepted arm, so this is the backend wedged rather
			 * than a slow frame. Free the slot. */
			tx_timeouts++;
			(void)radiant_radio_abort();
			(void)k_sem_take(&tx_done, K_MSEC(50));
		} else if (tx_status == RADIANT_RADIO_STATUS_OK) {
			tx_sent++;
		} else {
			tx_failed++;
		}

		/*
		 * THE SEQUENCE ADVANCES WHATEVER HAPPENED, and that is not
		 * sloppiness. A frame the transmitter refused to arm is a frame
		 * the receiver never had a chance at, and it must show up in
		 * the receiver's gap count - otherwise a transmitter defect
		 * would be invisible on the only line the gate is read from.
		 * The `refused`/`failed` counters here are how an operator
		 * tells that case from a link one.
		 */
		tx_seq++;
		next += SLOT_US;
	}
}

static void rx_run(void)
{
	while (running) {
		if (rx_arm_owed) {
			rx_arm_owed = false;
			if (rx_arm() != RADIANT_RADIO_OK_RC) {
				rx_rearm_fail++;
				rx_arm_owed = true;
			}
		}
		/* Everything else happens in the callback. */
		(void)k_sem_take(&engine_wake, K_MSEC(200));
	}
}

static void engine_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (1) {
		if (!running) {
			(void)k_sem_take(&engine_wake, K_FOREVER);
			continue;
		}
		if (cur_role == ROLE_TX) {
			tx_run();
		} else if (cur_role == ROLE_RX) {
			rx_run();
		} else {
			running = false;
		}
	}
}

/*
 * Cooperative, so that a console write - which on the Feather goes through USB -
 * can never sit between this thread's k_sleep() expiring and the arm call that
 * has to follow it.
 */
K_THREAD_DEFINE(engine_tid, 2048, engine_thread, NULL, NULL, NULL,
		K_PRIO_COOP(7), 0, 0);

/* ---------------------------------------------------------------------------
 * start / stop
 * ---------------------------------------------------------------------------
 */

static int start(void)
{
	if (cur_role == ROLE_NONE) {
		printk("refused: no role. `role tx` or `role rx` first.\n");
		return -1;
	}
	if (cur_role == ROLE_TX && cur_mode == MODE_1M_LONG) {
		printk("refused: `1m-long` is a RECEIVE mode only. Its "
		       "transmitter is radiant_core/spike/x1m_len, which is "
		       "what the received bytes are checked against.\n");
		return -1;
	}
	if (running) {
		printk("already running. `stop` first.\n");
		return -1;
	}

	stats_reset();

	if (cur_role == ROLE_RX) {
		const struct radiant_pkt_format *fmt = mode_rx_format(cur_mode);
		enum radiant_frame_cfg cfg =
			(cur_mode == MODE_S8) ? RADIANT_FRAME_CFG_LR
					      : RADIANT_FRAME_CFG_TRACKING;
		const struct radiant_channel_id *id =
			(cur_mode == MODE_1M_LONG) ? &id_x1m_long : &id_ladder;
		int rc;

		memset(&rx_filter, 0, sizeof(rx_filter));
		rc = radiant_frame_addr(cfg, radiant_net_addr_ant_plus, id,
					rx_filter.addr, sizeof(rx_filter.addr));
		if (rc < 0) {
			printk("refused: radiant_frame_addr() = %d\n", rc);
			return -1;
		}
		rx_filter.addr_len = (uint8_t)rc;

		memset(&rx_req, 0, sizeof(rx_req));
		rx_req.fmt       = fmt;
		rx_req.rf_index  = RF_INDEX;
		rx_req.filters   = &rx_filter;
		rx_req.n_filters = 1;
		/* CRC failures are counted rather than ignored: at the bottom
		 * of a ladder the difference between "the address matched and
		 * the body was corrupt" and "nothing was heard at all" is the
		 * difference between a link at its sensitivity limit and a rig
		 * that is not connected. */
		rx_req.flags     = RADIANT_RX_REPORT_CRC_FAIL;

		rx_lead_us  = (uint32_t)caps->min_arm_lead_us + 2000u;
		rx_arm_owed = false;

		printk("[rx] addr=%02X %02X %02X %02X %02X (%u bytes) fmt=%s\n",
		       rx_filter.addr[0], rx_filter.addr[1], rx_filter.addr[2],
		       rx_filter.addr[3], rx_filter.addr[4],
		       rx_filter.addr_len, mode_name[cur_mode]);

		running = true;
		rc = rx_arm();
		if (rc != RADIANT_RADIO_OK_RC) {
			printk("[rx] first arm refused rc=%d - retrying from "
			       "the engine thread\n", rc);
			rx_rearm_fail++;
			rx_arm_owed = true;
		}
	} else {
		running = true;
		printk("[tx] phy=%s power=%d dBm, %u Hz, seq from 0\n",
		       mode_name[cur_mode], cur_power_dbm, 1000000u / SLOT_US);
	}

	k_sem_give(&engine_wake);
	return 0;
}

static void stop(void)
{
	if (!running) {
		printk("not running.\n");
		return;
	}
	running = false;
	k_sem_give(&engine_wake);
	(void)radiant_radio_abort();
	/* Let the engine thread notice and unwind before anything reports. */
	k_sleep(K_MSEC(50));
	printk("stopped.\n");
	stats_print();
}

/* ---------------------------------------------------------------------------
 * Console
 *
 * A line parser rather than Zephyr's shell, on the same terms as every other
 * spike in this tree: no spike here has ever pulled in the shell, the command
 * set is six words, and the shell's serial backend defaults to gating on DTR
 * (SHELL_BACKEND_SERIAL_CHECK_DTR) - which on this bench is a known way to end
 * up looking at a board that appears dead. printk plus uart_poll_in() works
 * identically on the DK's UART VCOM and on the Feather's USB CDC ACM.
 * ---------------------------------------------------------------------------
 */

static const struct device *const console_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

#define LINE_MAX 48
static char line_buf[LINE_MAX];
static size_t line_len;

static void banner(void)
{
	printk("\n============================================================\n");
	printk("Spike phy_ladder - ADR 0007's owed gate: is the 5%%-loss point\n");
	printk("for S=8 at least 6 dB better than for 1 M GFSK on this rig?\n");
	printk("Board: %s   backend: %s\n", CONFIG_BOARD_TARGET, caps->name);
	printk("RF %u (%u MHz)  power range %d..%d dBm  min_arm_lead=%u us\n",
	       RF_INDEX, radiant_rf_index_to_mhz(RF_INDEX),
	       caps->tx_power_min_dbm, caps->tx_power_max_dbm,
	       caps->min_arm_lead_us);
	printk("PHYs this build advertises: %u", caps->n_phys);
	for (uint8_t i = 0; i < caps->n_phys; i++) {
		printk(" %s", caps->phys[i] == RADIANT_PHY_LR_CODED ? "S=8"
								   : "1M");
	}
	printk("\n");
	if (caps->n_phys < 2u) {
		printk("*** This backend advertises ONE PHY. `phy s8` will be "
		       "refused ENOTSUP and the gate CANNOT be measured on this "
		       "board. ***\n");
	}
	printk("------------------------------------------------------------\n");
	printk("  role tx | rx        phy 1m | s8 | 1m-long (rx only)\n");
	printk("  power <dbm>         start | stop | stats | help\n");
	printk("Loss is counted from PAYLOAD SEQUENCE GAPS. No clock is "
	       "involved and\n");
	printk("no rate is printed: read expected/received/loss and nothing "
	       "else.\n");
	printk("Restart the TX before the RX at every rung - `start` resets "
	       "the\n");
	printk("sequence, and a receiver that sees it go backwards marks the "
	       "run VOID.\n");
	printk("============================================================\n\n");
}

static void cmd_role(const char *arg)
{
	if (running) {
		printk("refused: running. `stop` first.\n");
		return;
	}
	if (arg != NULL && strcmp(arg, "tx") == 0) {
		cur_role = ROLE_TX;
	} else if (arg != NULL && strcmp(arg, "rx") == 0) {
		cur_role = ROLE_RX;
	} else {
		printk("usage: role tx | rx\n");
		return;
	}
	stats_reset();
	printk("role=%s\n", cur_role == ROLE_TX ? "tx" : "rx");
}

static void cmd_phy(const char *arg)
{
	if (running) {
		printk("refused: running. `stop` first.\n");
		return;
	}
	if (arg == NULL) {
		printk("usage: phy 1m | s8 | 1m-long\n");
		return;
	}
	if (strcmp(arg, "1m") == 0) {
		cur_mode = MODE_1M;
	} else if (strcmp(arg, "s8") == 0) {
		cur_mode = MODE_S8;
	} else if (strcmp(arg, "1m-long") == 0) {
		cur_mode = MODE_1M_LONG;
	} else {
		printk("usage: phy 1m | s8 | 1m-long\n");
		return;
	}
	stats_reset();
	printk("phy=%s\n", mode_name[cur_mode]);

	if (cur_mode == MODE_S8) {
		bool have = false;

		for (uint8_t i = 0; i < caps->n_phys; i++) {
			have = have || (caps->phys[i] == RADIANT_PHY_LR_CODED);
		}
		if (!have) {
			printk("*** this backend does not advertise the coded "
			       "PHY; every arm will be refused ENOTSUP ***\n");
		}
	}
	if (cur_mode == MODE_1M_LONG) {
		printk("receiving spike x1m_len's frame B: dev 0x%04X, addr "
		       "A6 C5 B0 60 60, %u body bytes, all checked.\n",
		       DEVNUM_X1M_LONG, X1M_LONG_BODY_LEN);
	}
}

static void cmd_power(const char *arg)
{
	long v;
	char *end;

	if (running) {
		/*
		 * Refused rather than applied, and the reason is the
		 * measurement rather than the implementation: a rung whose
		 * power changed halfway is a loss figure averaged over two
		 * powers, and nothing on the console afterwards would show
		 * that it had happened.
		 */
		printk("refused: running. A rung must be one power - `stop`, "
		       "`power`, `start`.\n");
		return;
	}
	if (arg == NULL) {
		printk("usage: power <dbm>   (range %d..%d on this backend)\n",
		       caps->tx_power_min_dbm, caps->tx_power_max_dbm);
		return;
	}
	v = strtol(arg, &end, 10);
	if (end == arg || *end != '\0' || v < -128 || v > 127) {
		printk("usage: power <dbm>\n");
		return;
	}
	cur_power_dbm = (int8_t)v;
	printk("power=%d dBm\n", cur_power_dbm);
	if (v < caps->tx_power_min_dbm || v > caps->tx_power_max_dbm) {
		/* The HAL says a backend rounds to the nearest setting it has
		 * and does not fail. Said out loud, because a ladder whose
		 * bottom three rungs are all really -40 dBm has three
		 * duplicate points and no 5 % crossing. */
		printk("*** outside %d..%d dBm - the backend will clamp, so "
		       "this rung is NOT the power you asked for ***\n",
		       caps->tx_power_min_dbm, caps->tx_power_max_dbm);
	}
}

static void dispatch(char *line)
{
	char *cmd = line;
	char *arg;

	while (*cmd == ' ') {
		cmd++;
	}
	arg = strchr(cmd, ' ');
	if (arg != NULL) {
		*arg++ = '\0';
		while (*arg == ' ') {
			arg++;
		}
		if (*arg == '\0') {
			arg = NULL;
		}
	}
	if (*cmd == '\0') {
		return;
	}

	if (strcmp(cmd, "role") == 0) {
		cmd_role(arg);
	} else if (strcmp(cmd, "phy") == 0) {
		cmd_phy(arg);
	} else if (strcmp(cmd, "power") == 0) {
		cmd_power(arg);
	} else if (strcmp(cmd, "start") == 0) {
		(void)start();
	} else if (strcmp(cmd, "stop") == 0) {
		stop();
	} else if (strcmp(cmd, "stats") == 0) {
		stats_print();
	} else if (strcmp(cmd, "help") == 0) {
		banner();
	} else {
		printk("unknown: %s   (help)\n", cmd);
	}
}

static void console_poll(void)
{
	unsigned char c;

	while (uart_poll_in(console_dev, &c) == 0) {
		if (c == '\r' || c == '\n') {
			printk("\n");
			line_buf[line_len] = '\0';
			dispatch(line_buf);
			line_len = 0;
		} else if ((c == '\b' || c == 0x7F)) {
			if (line_len > 0u) {
				line_len--;
				printk("\b \b");
			}
		} else if (c >= ' ' && c < 0x7F && line_len < LINE_MAX - 1u) {
			line_buf[line_len++] = (char)c;
			printk("%c", c);   /* no terminal echo over CDC ACM */
		}
	}
}

/* ---------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------------
 */

int main(void)
{
	int64_t next_report;
	int rc;

	caps = radiant_radio_caps_get();
	x1m_long_expect_build();

	/* The console is the whole of this program's input and output. On the
	 * Feather it is USB CDC ACM and does not exist until the host has
	 * enumerated it, so anything printed before then is gone - which is why
	 * `help` reprints the banner. */
	k_sleep(K_MSEC(1500));
	banner();

	if (!device_is_ready(console_dev)) {
		printk("FATAL: console device not ready\n");
		return 0;
	}

	rc = radiant_radio_init(&cbs, NULL);
	if (rc != RADIANT_RADIO_OK_RC) {
		printk("FATAL: radiant_radio_init() = %d\n", rc);
		return 0;
	}
	rc = radiant_radio_enable();
	if (rc != RADIANT_RADIO_OK_RC) {
		printk("FATAL: radiant_radio_enable() = %d\n", rc);
		return 0;
	}

	next_report = k_uptime_get() + 5000;

	while (1) {
		console_poll();

		/*
		 * A heartbeat while running, so that an operator can see the
		 * rung progressing without typing. It is the same line `stats`
		 * prints - there is no second, differently-computed figure
		 * anywhere in this program.
		 */
		if (running && k_uptime_get() >= next_report) {
			next_report = k_uptime_get() + 5000;
			stats_print();
		}
		k_sleep(K_MSEC(10));
	}

	return 0;
}
