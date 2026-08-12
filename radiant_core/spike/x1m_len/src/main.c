/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Spike x1m_len: does a longer-than-standard 1 M frame stay INVISIBLE to a
 * stock ANT+ dongle doing a wildcard search?
 *
 * Provenance: clean-room. Written from
 *   - docs/decisions/0007-long-range-phy.md, section "Does 1 M also qualify? -
 *     NOT SETTLED", which is the question this program exists to answer,
 *   - radiant_core/include/radiant_core/radiant_radio_hal.h, the frozen backend
 *     contract, and radiant_core/include/radiant_core/radiant_frame.h for the
 *     ANT+ network address and the CRC parameters,
 *   - docs/ant-radio-link.md and docs/spike-b-part2-results.md for the ANT
 *     tracking frame's geometry and for what byte 3 actually is,
 *   - radiant_core/spike/rx_raw/src/main.c, this repository's own spike, for
 *     the shape of a bench program.
 * Nothing here derives from sdk-ant, from libant.a, or from any adopter-gated
 * ANT+ device profile document. See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * The question
 * ---------------------------------------------------------------------------
 * ADR 0007 permits length extension "exactly where the PHY already makes us
 * invisible", and records one thing it deliberately did not settle: whether a
 * RadiANT-authored frame that is LONGER than an ANT frame, on the ORDINARY 1 M
 * GFSK PHY, on a device type no stock receiver opens, is simply dropped on CRC
 * by a stock dongle in wildcard search - which keeps hearing every other sensor
 * normally. If it is, the descriptor-set collapse (the largest battery item in
 * that record) arrives without the coded PHY at all.
 *
 * That is a claim about somebody else's receiver, so it cannot be settled in a
 * unit test and it cannot be settled by reading. It needs a transmitter and a
 * stock dongle. This is the transmitter.
 *
 * ---------------------------------------------------------------------------
 * The A/B control, which is the whole design
 * ---------------------------------------------------------------------------
 * Two frames, alternating on the same radio, at the same power, at the same
 * cadence, on the same device type 0x60:
 *
 *   FRAME A - the CONTROL. Device number 0x60A0. Ordinary ANT tracking
 *   geometry: 5-byte on-air address [A6 C5 A0 60 60], 10-byte body
 *   [trans_type][control 0x0A][8 payload]. This is a well-formed ordinary ANT
 *   frame and a stock dongle in wildcard search SHOULD report device 0x60A0.
 *
 *   FRAME B - the EXPERIMENT. Device number 0x60B0. The same address geometry,
 *   [A6 C5 B0 60 60], and a 30-BYTE body instead of 10. A stock dongle should
 *   NEVER report device 0x60B0.
 *
 * WITHOUT FRAME A, "B WAS NOT SEEN" IS NOT A RESULT. It is equally consistent
 * with a transmitter that never keyed, an address arithmetic mistake, a dongle
 * that cannot hear this board across the bench, a wrong frequency, or a dongle
 * that was not really searching. Frame A removes every one of those at once:
 * it differs from B in exactly one property - the number of body bytes - so if
 * A appears in the scan and B does not, the only thing that can have caused the
 * difference is frame length. The control is not a nicety here; it is what
 * converts a negative observation into evidence.
 *
 * ---------------------------------------------------------------------------
 * Why this needed no change to shipping code, which is itself a result
 * ---------------------------------------------------------------------------
 * Both formats below are LOCAL static consts in this file. Nothing in
 * radiant_core was touched to build them, and that is not a coincidence - it is
 * what apply_format() in src/radiant_radio_nrf.c already says:
 *
 *   - on 1 M it requires RADIANT_LEN_FIXED (Spike B: byte 3 of an ANT frame is
 *     a control byte, not a length, so a length-decoding format on this PHY is
 *     something the backend must refuse),
 *   - and having required it, it accepts ANY fmt->body_len from 1 to
 *     RADIANT_RADIO_BODY_MAX, which ADR 0007 raised to 40 for the coded
 *     format's sake.
 *
 * So a 30-byte static-length 1 M body is expressible today, through the public
 * HAL, with no register knowledge in this file at all. That is the second thing
 * this spike demonstrates, and it demonstrates it at build time rather than in
 * prose.
 *
 * ---------------------------------------------------------------------------
 * Why it drives the HAL from a thread instead of chaining in the callback
 * ---------------------------------------------------------------------------
 * radiant_radio_hal.h permits re-arming from inside a completion callback and
 * calls it the low-jitter path. This program does not need low jitter - it
 * needs to be obviously correct at 8 transmissions per second - so the callback
 * does the one thing a callback here can do without adding a concurrency
 * question: it records the status and gives a semaphore. The schedule itself is
 * absolute (every slot is a fixed number of microseconds after the last), so
 * thread latency changes when the arm call happens and not when the frame goes
 * out; the backend works backwards from t_sync either way.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <radiant_core/radiant_frame.h>
#include <radiant_core/radiant_radio_hal.h>

/* ---------------------------------------------------------------------------
 * Build-time guards
 * ---------------------------------------------------------------------------
 */

/*
 * THE ONE THAT MATTERS. CONFIG_RADIANT_CORE_BACKEND_NRF is a `depends on
 * SOC_COMPATIBLE_NRF52X || SOC_COMPATIBLE_NRF54LX` choice entry. Ask for it on
 * a part outside that pair - an nRF5340, either core - and nothing errors: the
 * dependency is unmet, Kconfig falls back to RADIANT_CORE_BACKEND_NULL, and the
 * image builds, boots, runs this loop and refuses every arm with ENOTSUP. The
 * counters below would read zero and the operator would be looking at a dongle
 * that heard nothing from a board that transmitted nothing.
 *
 * That failure has already cost this project a session once (radiant_core/
 * Kconfig records it against a Zephyr symbol rename), and here it would be
 * worse than a wasted session: "neither A nor B was seen" is a result shape
 * this experiment can produce, and a null backend manufactures it.
 */
BUILD_ASSERT(IS_ENABLED(CONFIG_RADIANT_CORE_BACKEND_NRF),
	     "This spike transmits. CONFIG_RADIANT_CORE_BACKEND_NRF did not "
	     "take - it depends on SOC_COMPATIBLE_NRF52X || "
	     "SOC_COMPATIBLE_NRF54LX and Kconfig fell back to the null "
	     "backend, which refuses every arm. Build for the nRF54L15 DK "
	     "(nrf54l15dk/nrf54l15/cpuapp); the nRF5340 DK cannot run this at "
	     "all - see README.md.");

/* The backend asserts this for itself; repeated here because this file is what
 * an operator reads when the board is silent. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT),
	     "The nrf backend owns the RADIO outright; CONFIG_BT must be off.");

/* ---------------------------------------------------------------------------
 * The two frames
 * ---------------------------------------------------------------------------
 */

#define DEVICE_TYPE      0x60u   /* the RadiANT telemetry envelope's type */
#define TRANS_TYPE       0x05u
#define CONTROL_BYTE     0x0Au   /* a broadcast, per docs/spike-b-part2-results.md */

#define DEVNUM_A         0x60A0u /* the control - a stock dongle SHOULD see this */
#define DEVNUM_B         0x60B0u /* the experiment - it should NEVER see this */

/* Header bytes ahead of the payload in ANT's tracking geometry:
 * [transmission type][control byte]. Deliberately spelled out rather than
 * taken from RADIANT_FRAME_HDR_LEN_TRACKING, so that the two body lengths
 * below read as what they are. */
#define HDR_LEN          2u

#define BODY_LEN_A       10u     /* 2 header + 8 payload - a standard ANT body */
#define BODY_LEN_B       30u     /* 2 header + 28 payload - the long one */

/* 4 Hz each, interleaved A B A B, so 8 transmissions a second on the air and
 * neither frame is favoured by the schedule. */
#define SLOT_US          125000u

/*
 * The formats. Both are RADIANT_LEN_FIXED on RADIANT_PHY_1M_GFSK, which is what
 * makes a 30-byte body cost no backend change - see the header comment. The CRC
 * block is byte-for-byte fmt_tracking's in radiant_core/src/radiant_frame.c:
 * CRC-16/CCITT-FALSE covering the on-air address. Any other CRC and frame A
 * would not be an ANT frame, which would remove the control.
 */
static const struct radiant_pkt_format fmt_std = {
	.phy          = RADIANT_PHY_1M_GFSK,
	.addr_len     = (uint8_t)RADIANT_FRAME_ADDR_LEN_TRACKING,
	.len_mode     = RADIANT_LEN_FIXED,
	.body_len     = BODY_LEN_A,
	.len_offset   = 0,
	.len_bias     = 0,
	.max_body_len = BODY_LEN_A,
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

static const struct radiant_pkt_format fmt_long = {
	.phy          = RADIANT_PHY_1M_GFSK,
	.addr_len     = (uint8_t)RADIANT_FRAME_ADDR_LEN_TRACKING,
	.len_mode     = RADIANT_LEN_FIXED,
	.body_len     = BODY_LEN_B,
	.len_offset   = 0,
	.len_bias     = 0,
	.max_body_len = BODY_LEN_B,
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

/* 40 is RADIANT_RADIO_BODY_MAX, raised by ADR 0007 for the coded format. The
 * long frame here is inside it, and that is exactly why no shipping code had to
 * move. If a future edit lengthens frame B past this, the backend would refuse
 * the arm with EINVAL at run time; failing at build time is better. */
BUILD_ASSERT(BODY_LEN_B <= RADIANT_RADIO_BODY_MAX,
	     "frame B's body no longer fits RADIANT_RADIO_BODY_MAX");
BUILD_ASSERT(BODY_LEN_A != BODY_LEN_B,
	     "A and B must differ in length and in nothing else");

struct frame_def {
	const char                      *name;
	const struct radiant_pkt_format *fmt;
	uint16_t                         devnum;
	uint8_t                          addr[RADIANT_RADIO_ADDR_MAX];
	uint8_t                          body[RADIANT_RADIO_BODY_MAX];
	uint8_t                          body_len;
	/* Counted separately, because "A went out and B did not" and "neither
	 * went out" are different results and the operator must be able to tell
	 * them apart from the console alone. */
	uint32_t                         sent;
	uint32_t                         failed;
	uint32_t                         refused;
};

static struct frame_def frames[2] = {
	{ .name = "A/std ", .fmt = &fmt_std,  .devnum = DEVNUM_A, .body_len = BODY_LEN_A },
	{ .name = "B/long", .fmt = &fmt_long, .devnum = DEVNUM_B, .body_len = BODY_LEN_B },
};

/*
 * Fill in the on-air address and the body.
 *
 * The address is [net0][net1][devnum_lo][devnum_hi][device_type], first byte on
 * the air first - the geometry docs/ant-radio-link.md records and the one
 * radiant_frame.c's fmt_tracking uses. The network bytes come from
 * radiant_net_addr_ant_plus rather than from a literal A6 C5 here: two copies of
 * a constant is two places for it to be wrong, and this program's whole claim
 * is that it is on the air a stock dongle is listening to.
 *
 * There is no bit- or byte-reversal anywhere in this file. That is the backend's
 * job and radiant_radio_hal.h says so; a spike that did its own would be testing
 * its own arithmetic rather than the shipping path.
 */
static void frame_build(struct frame_def *f)
{
	f->addr[0] = radiant_net_addr_ant_plus[0];
	f->addr[1] = radiant_net_addr_ant_plus[1];
	f->addr[2] = (uint8_t)(f->devnum & 0xFFu);
	f->addr[3] = (uint8_t)((f->devnum >> 8) & 0xFFu);
	f->addr[4] = DEVICE_TYPE;

	f->body[0] = TRANS_TYPE;
	f->body[1] = CONTROL_BYTE;

	/*
	 * The payload is filler and is deliberately identical in shape for both
	 * frames: a marker byte so a raw capture can tell them apart by eye,
	 * then an incrementing ramp. A stock dongle in wildcard search decides
	 * whether to report a device from the address and the CRC, so nothing
	 * about this content can affect the result - which is the point of
	 * saying so rather than choosing something meaningful and leaving a
	 * reader to wonder whether it mattered.
	 */
	f->body[2] = (uint8_t)(f->devnum & 0xFFu);
	for (uint8_t i = 3; i < f->body_len; i++) {
		f->body[i] = i;
	}
}

/* ---------------------------------------------------------------------------
 * The HAL callbacks
 *
 * These run in the RADIO interrupt at the backend's highest priority. The
 * contract permits exactly one useful thing here for a program shaped like this
 * one - signal an ISR-safe object and return - and that is all they do.
 * ---------------------------------------------------------------------------
 */

static K_SEM_DEFINE(tx_done, 0, 1);
static volatile enum radiant_radio_status tx_status;
static volatile uint32_t tx_op_seen;

static void on_tx(const struct radiant_tx_event *evt, void *user)
{
	ARG_UNUSED(user);

	tx_status  = evt->status;
	tx_op_seen = evt->op;
	k_sem_give(&tx_done);
}

/* No receive is ever armed, so this exists only because the table needs it.
 * Leaving it NULL would be a null dereference on any event the backend chose to
 * deliver; a counter is cheaper than an assumption. */
static uint32_t stray_rx;

static void on_rx(const struct radiant_rx_event *evt, void *user)
{
	ARG_UNUSED(evt);
	ARG_UNUSED(user);

	stray_rx++;
}

static const struct radiant_radio_cbs cbs = {
	.rx = on_rx,
	.tx = on_tx,
	.ed = NULL,   /* CONFIG_RADIANT_CORE_ED_SCAN is off; the backend must not call it */
};

/* ---------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------------
 */

static uint32_t resyncs;
static uint32_t timeouts;

static void banner(const struct radiant_radio_caps *caps)
{
	printk("\n============================================================\n");
	printk("Spike x1m_len - ADR 0007: is a LONG 1 M frame invisible to a\n");
	printk("stock ANT+ dongle in wildcard search?\n");
	printk("Board: %s   backend: %s\n", CONFIG_BOARD_TARGET, caps->name);
	printk("RF index %u (%u MHz), 1 M GFSK, %d dBm\n",
	       RADIANT_RF_INDEX_ANT_PLUS,
	       radiant_rf_index_to_mhz(RADIANT_RF_INDEX_ANT_PLUS), 0);
	printk("min_arm_lead=%u us  ramp_up=%u us\n",
	       caps->min_arm_lead_us, caps->ramp_up_us);

	for (int i = 0; i < 2; i++) {
		const struct frame_def *f = &frames[i];

		printk("%s dev=0x%04X addr=%02X %02X %02X %02X %02X body=%u bytes\n",
		       f->name, f->devnum,
		       f->addr[0], f->addr[1], f->addr[2], f->addr[3], f->addr[4],
		       f->body_len);
	}
	printk("A is the CONTROL and a stock dongle SHOULD report 0x%04X.\n",
	       DEVNUM_A);
	printk("B is the EXPERIMENT and it should NEVER report 0x%04X.\n",
	       DEVNUM_B);
	printk("Both at 4 Hz, interleaved. Neither seen = the rig is broken;\n");
	printk("conclude nothing. See README.md.\n");
	printk("============================================================\n\n");
}

int main(void)
{
	const struct radiant_radio_caps *caps = radiant_radio_caps_get();
	struct radiant_tx_req req = {
		.rf_index = RADIANT_RF_INDEX_ANT_PLUS,
		.power    = { .dbm = 0, .use_raw = false, .raw = 0 },
		.addr_len = (uint8_t)RADIANT_FRAME_ADDR_LEN_TRACKING,
	};
	radiant_time_t next;
	uint32_t lead;
	int64_t next_report;
	unsigned int slot = 0;
	int rc;

	/* The console is the whole output of this program, so give the host a
	 * moment to attach before the banner scrolls past. */
	k_sleep(K_MSEC(1500));

	for (int i = 0; i < 2; i++) {
		frame_build(&frames[i]);
	}
	banner(caps);

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

	/*
	 * The arm lead. caps.min_arm_lead_us is the backend's own floor and an
	 * arm inside it is refused ETIME rather than run late; the extra
	 * milliseconds are this program's slack for thread scheduling and for
	 * the printk that happens once a second. Being generous costs nothing
	 * here - the schedule is absolute, so slack moves when the arm call
	 * happens and not when the frame leaves the antenna.
	 */
	lead = (uint32_t)caps->min_arm_lead_us + 3000u;

	next = radiant_radio_now() + lead + 1000u;
	next_report = k_uptime_get() + 1000;

	while (1) {
		struct frame_def *f = &frames[slot & 1u];
		radiant_time_t now = radiant_radio_now();
		uint32_t op = 0;

		/*
		 * Fell behind - a long console write, or a refused arm that cost
		 * a slot. Re-anchor rather than firing a burst of catch-up
		 * frames at the dongle, which would change the cadence the two
		 * frames are supposed to share. Counted, because a resync rate
		 * above zero means the 4 Hz claim in the banner is not quite
		 * what is on the air.
		 */
		if (next < now + lead) {
			next = now + lead;
			resyncs++;
		} else {
			radiant_time_t arm_at = next - lead;

			if (arm_at > now) {
				k_sleep(K_USEC((int64_t)(arm_at - now)));
			}
		}

		req.fmt       = f->fmt;
		req.body      = f->body;
		req.body_len  = f->body_len;
		req.t_sync_at = next;
		memcpy(req.addr, f->addr, RADIANT_RADIO_ADDR_MAX);

		rc = radiant_radio_tx(&req, &op);
		if (rc != RADIANT_RADIO_OK_RC) {
			/*
			 * A refusal is a RESULT, not a hiccup, and the loudest
			 * one would be ENOTSUP on frame B alone: that would mean
			 * the backend cannot express a 30-byte 1 M body after
			 * all, and this spike's premise - that today's code
			 * already does this - is wrong. So it is printed, once
			 * per occurrence, rather than folded into a counter.
			 */
			f->refused++;
			printk("[arm] %s REFUSED rc=%d (ENOTSUP=%d EINVAL=%d "
			       "ETIME=%d EBUSY=%d)\n", f->name, rc,
			       RADIANT_RADIO_ENOTSUP, RADIANT_RADIO_EINVAL,
			       RADIANT_RADIO_ETIME, RADIANT_RADIO_EBUSY);
		} else if (k_sem_take(&tx_done, K_MSEC(200)) != 0) {
			/*
			 * No terminal event inside a whole slot. The HAL
			 * guarantees exactly one per accepted arm, so this is
			 * the backend wedged rather than a slow frame - abort so
			 * the operation slot is free and the next arm is not a
			 * stream of EBUSY.
			 */
			timeouts++;
			(void)radiant_radio_abort();
			(void)k_sem_take(&tx_done, K_MSEC(50));
		} else if (tx_status == RADIANT_RADIO_STATUS_OK) {
			f->sent++;
		} else {
			f->failed++;
		}

		next += SLOT_US;
		slot++;

		if (k_uptime_get() >= next_report) {
			next_report += 1000;
			printk("[tx] A/std sent=%u fail=%u refuse=%u | "
			       "B/long sent=%u fail=%u refuse=%u | "
			       "resync=%u timeout=%u stray_rx=%u\n",
			       frames[0].sent, frames[0].failed, frames[0].refused,
			       frames[1].sent, frames[1].failed, frames[1].refused,
			       resyncs, timeouts, stray_rx);
		}
	}

	return 0;
}
