/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_rx_torn.h - the nRF backend's late-ISR guard, as a pure decision.
 *
 * Provenance: clean-room. Written from radiant_radio_nrf.c's own END branch
 * and the nRF RADIO's documented event order (ADDRESS, then payload DMA, then
 * END). Nothing derives from sdk-ant or any ANT+ document.
 *
 * WHY. With END->START the receiver re-arms itself after every frame and
 * writes the next frame into the same buffer as soon as that frame's ADDRESS
 * matches. A RADIO interrupt that runs late (interrupts held off by a thread
 * scheduler pass) can then read frame N's CRCSTATUS beside frame N+1's bytes
 * and frame N+1's CC_SYNC capture. The END branch therefore snapshots the
 * buffer and registers, re-reads CC_SYNC, and asks this function whether the
 * snapshot is one frame's.
 *
 * Pure over plain integers, no Zephyr or register access, so
 * radiant/tests/src/test_rx_torn.c runs it on any board.
 */

#ifndef RADIANT_RX_TORN_H_
#define RADIANT_RX_TORN_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * True if a snapshot taken between two reads of CC_SYNC cannot be trusted as
 * the frame the first read dates.
 *
 * sync_first  CC_SYNC read before the snapshot (the ADDRESS capture, raw
 *             32-bit 1 MHz counter).
 * sync_second CC_SYNC read again after the snapshot's buffer copy.
 * now         the same counter, captured after sync_second.
 * air_us      the least time from that frame's ADDRESS to its END: if less
 *             than this has passed, the frame sync_first dates is still on
 *             the air, so it is not the frame whose END is being handled and
 *             the buffer holds a mix.
 *
 * Torn if the reads differ (a new ADDRESS landed during the copy and its DMA
 * may have overwritten part of it), or if fewer than air_us have passed since
 * the capture. The elapsed time is modulo 2^32, so it is right across the
 * counter's wrap. A difference of 2^31 or more means `now` is behind the
 * capture, which the hardware cannot produce (the capture precedes the read
 * of it, which precedes `now`); that is treated as torn, since a drop is
 * counted and a delivery of an unknown frame is not.
 */
static inline bool radiant_rx_torn(uint32_t sync_first, uint32_t sync_second,
				   uint32_t now, uint32_t air_us)
{
	uint32_t elapsed = now - sync_first;

	if (sync_second != sync_first) {
		return true;
	}
	return elapsed < air_us || elapsed >= 0x80000000u;
}

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_RX_TORN_H_ */
