/* SPDX-License-Identifier: Apache-2.0 */

#ifndef ANT_HEALTH_H_
#define ANT_HEALTH_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

/*
 * What the dongle threw away, and the 0xF6 reply that carries it.
 *
 * WHY THIS EXISTS AT ALL. tools/ab_gates.toml's `unexplained_loss.max = 0` is
 * a SITTING INVALIDATOR rather than a gate: a hole in the accumulator with no
 * RX_FAIL beside it means the packet was lost on the host or inside the
 * dongle, and loss_accounting() cannot tell those two apart. Everything
 * counted here is dongle-side loss with no RX_FAIL, so a non-zero reading
 * splits that bucket. The check has already caught 0.4 pp of imaginary loss
 * once; a second, independent source of truth makes it sharper.
 *
 * And it exists on the WIRE, not in a log, because of where it has to work.
 * Today the only record of a dropped event is a LOG_WRN in
 * ant_serial_bridge.c. On a Feather - no debugger, and the board most people
 * flash - that line does not exist. A host-readable counter does.
 *
 * OFF BY DEFAULT (CONFIG_ANT_DONGLE_HEALTH_COUNTERS), and not out of caution.
 * The `zero-cost` CI job asserts identical section sizes and symbol sets
 * against the merge base, and counters on the common path fire it. With the
 * symbol off every note() below compiles to nothing at all - not to a call to
 * an empty function - so the shipping image is byte-identical and the job
 * stays a real check rather than one that had to be weakened.
 *
 * REQUEST-ONLY. Nothing here is ever emitted unsolicited. A host that never
 * sends MESG_REQUEST for 0xF6 sees a byte stream indistinguishable from a
 * build without the feature, which is what keeps this out of the conformance
 * transcripts that tools/ab_gates.toml requires to stay byte-identical.
 *
 * COUNTING IS NOT THE FIX, and the counters must not be read as asking for
 * one. See ANT_TX_QUEUE_DEPTH in usb_ant_class.c: deepening the queue converts
 * a drop into latency, which is the wrong trade for a protocol whose whole
 * value is a 0.345 ms p50. The number is a witness, not a backlog.
 */

enum ant_health_counter {
	/* An ANT event the host never saw: k_msgq_put(K_NO_WAIT) found the USB
	 * TX queue full. This is the one that matters most - it is silent,
	 * dongle-side packet loss. */
	ANT_HEALTH_TX_DROP,
	/* Bulk-OUT left unarmed because the bridge could not take more. Not
	 * loss: the host's own writes wait. Counted because a host that stalls
	 * is the usual reason TX_DROP is non-zero, and reading them together
	 * is what separates "our bug" from "their bug". */
	ANT_HEALTH_RX_BACKPRESSURE,
	/* A frame refused for an unusable network key. */
	ANT_HEALTH_KEY_REJECT,
	/* Scheduler windows that never opened, by reason. DENIED is the
	 * arbiter's, and is expected to be large in an arbitrated build - the
	 * sweep is the elastic consumer (ADR 0013). MISSED and FAILED are not
	 * expected to be large in any build. */
	ANT_HEALTH_SCHED_MISSED,
	ANT_HEALTH_SCHED_FAILED,
	ANT_HEALTH_SCHED_DENIED,
	/* A burst transfer that hit the 1000 ms back-pressure stall. The stall
	 * IS the back-pressure and must not be shortened - shortening it makes
	 * the host's transfer fail rather than wait - so this gets a counter
	 * and nothing more. Zwift uses acknowledged data, not burst, so a
	 * non-zero value here is somebody else's host. */
	ANT_HEALTH_BURST_STALL,
	/*
	 * A search window the pump declined to post WHILE A CHANNEL WAS STILL
	 * SEARCHING and its slot was held for something else. Discovery work
	 * was outstanding and the sweep could not get on the air to do it,
	 * which is a real stall.
	 *
	 * ITS COUNTERPART IS DELIBERATELY NOT HERE. The other half of that
	 * decline - no channel searching at all - is discovery having
	 * succeeded, not failed, and it is not loss of any kind. It also
	 * cannot be a health counter even if one wanted it to be: the pump
	 * runs about 31 times a second whatever else is happening, so on an
	 * idle dongle that number climbs forever and saturates in about 35
	 * minutes. Measured on the first hardware read of this reply - 231
	 * within nine seconds of boot, with no channel ever opened. Saturating
	 * sets the reply's `floor rather than total` flag, so an idle-counting
	 * field would have poisoned the interpretation of every OTHER field on
	 * any board left switched on.
	 */
	ANT_HEALTH_SWEEP_BUSY,
	/* Bytes the host-frame parser discarded while it was NOT synced on
	 * SYNC (0xA4) - the S_SYNC state's else-branch in
	 * ant_serial_bridge.c. Distinct from HOST_CSUM: this is bytes lost
	 * hunting for the start of a frame at all, before a length or id was
	 * ever seen. Also counts one per frame the parser was part way
	 * through when a USB host session ended (bridge_thread_fn()'s
	 * ant_transport_session() check) - framing lost the same way, at a
	 * bus reset instead of on noise. */
	ANT_HEALTH_HOST_DESYNC,
	/* A frame that DID find SYNC, LEN and ID, ran to its declared length,
	 * and then failed the trailing XOR checksum. Kept separate from
	 * HOST_DESYNC on purpose: that one is framing lost outright, this one
	 * is a frame that arrived but was corrupt, which points at a
	 * different fault (a noisy UART link vs. a host that fell out of
	 * step). The existing LOG_WRN stays - this counter is the copy that
	 * survives on a board with no debugger. */
	ANT_HEALTH_HOST_CSUM,
	/* A radio RX event whose channel op is no longer the one this
	 * channel owns by the time it is processed - api_sched_rx()'s
	 * ownership check. Counted at both sites that can hit it: the normal
	 * OK-status path (which also LOG_DBGs) and the CRC-repair path's
	 * silent twin, which has no log at all today. Without this the two
	 * looked identical to "no event happened". */
	ANT_HEALTH_RX_NOT_OWNER,
	/* A USB bulk-IN transfer that never completed within its timeout -
	 * the host stopped polling. Counted where the timeout is detected;
	 * W1 wires the USB_NEXT site, W6 adds the legacy stack's. */
	ANT_HEALTH_TX_TIMEOUT,
	/* A receiver-overrun event on the UART: the driver's uart_err_check()
	 * reported UART_ERROR_OVERRUN, i.e. at least one host byte was
	 * overwritten before the ISR read it. The flag (RSR.OE, ERRORSRC
	 * OVERRUN) is sticky until read, so this adds at most 1 per ISR pass
	 * however many bytes were lost in between: a floor on lost bytes,
	 * not a count of them. The transport counts only after
	 * ant_health_measured() said it can; until then [32..33] reads
	 * ANT_HEALTH_NOT_MEASURED. Expect HOST_CSUM/HOST_DESYNC beside it. Added
	 * 2026-11-16 for the TI command stall: the CC26xx UART ran with its
	 * FIFO off and dropped a byte every few tens of seconds under load
	 * (archive/benchmarks/2026-11-16-ti-cmd-stall/). */
	ANT_HEALTH_UART_RX_OVERRUN,

	ANT_HEALTH_COUNT
};

/* Which queue a depth sample describes. */
#define ANT_HEALTH_DEPTH_TX    0u
#define ANT_HEALTH_DEPTH_EVENT 1u

/* Bytes the shortest 0xF6 reply has - the 0xF6 payload_len in
 * protocol/ant_wire.yaml, which is the definition, and the minimum a host
 * accepts. Fields are only ever appended and an offset is never reused, and
 * a host tells which optional fields exist from the body length. The structure
 * version at out[0] stays 1 throughout the beta. */
#define ANT_HEALTH_PAYLOAD_LEN 30u

/* What ant_health_fill() writes: every field through [32..33]. The optional
 * fields from [30] on read ANT_HEALTH_NOT_MEASURED where this build or
 * transport cannot measure them - never 0, which would claim "none" - and
 * clamp at ANT_HEALTH_MEASURED_MAX so a real count never reads as the
 * sentinel. (Until 2026-11-16 a build without coverage answered 30 bytes
 * and one with it 32; [32..33] needs [30..31] present, so the sentinel
 * replaced the absence.) */
#define ANT_HEALTH_BODY_LEN 34u
#define ANT_HEALTH_NOT_MEASURED 0xFFFFu
#define ANT_HEALTH_MEASURED_MAX 0xFFFEu

#if defined(CONFIG_ANT_DONGLE_HEALTH_COUNTERS)

/*
 * ISR-SAFE BY CONSTRUCTION. api_sched_done() runs in the radio interrupt and
 * usb_ant_send_async() runs on whatever thread produced the event, so these
 * are atomics rather than a lock. A lock here would be a lock taken inside the
 * radio ISR, which is the one thing this backend must never do.
 *
 * They SATURATE rather than wrap, and the saturation is reported in the reply
 * flags. A wrapped counter is worse than a missing one: it reads as a small
 * number and is a huge one, and there is no way to tell from the value.
 */
void ant_health_note(enum ant_health_counter c);

/* Record a queue occupancy sample; keeps the maximum ever seen. */
void ant_health_depth(uint8_t which, uint32_t used);

/* Declare that this image can measure counter `c`, which turns its field
 * from ANT_HEALTH_NOT_MEASURED into a count. Only the fields that have a
 * not-measured state use it: today ANT_HEALTH_UART_RX_OVERRUN, called by
 * the UART transport when its driver reports overruns. */
void ant_health_measured(enum ant_health_counter c);

/*
 * Render the 0xF6 payload. Returns bytes written, or 0 if `cap` is too small.
 * Pulls the event-queue figures from radiant_event_stats_get() at call time
 * rather than mirroring them, so there is one owner of those two numbers.
 */
size_t ant_health_fill(uint8_t *out, size_t cap);

#else /* !CONFIG_ANT_DONGLE_HEALTH_COUNTERS */

/*
 * Macros, not empty inline functions. An inline function still evaluates its
 * arguments, and k_msgq_num_used_get() at a call site is a real read of a real
 * kernel object - which would show up in the `zero-cost` job's section sizes
 * and defeat the whole point of the symbol being off by default.
 */
#define ant_health_note(c)          ((void)0)
#define ant_health_depth(w, used)   ((void)0)
#define ant_health_measured(c)      ((void)0)
#define ant_health_fill(out, cap)   ((size_t)0)

#endif /* CONFIG_ANT_DONGLE_HEALTH_COUNTERS */

#endif /* ANT_HEALTH_H_ */
