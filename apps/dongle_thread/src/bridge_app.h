/* SPDX-License-Identifier: Apache-2.0 */
/*
 * bridge_app.h - the application half of the RadiANT sample bus.
 *
 * radiant/src/bridge/ is the bus, the binding table, the rule evaluator and
 * the liveness table, and it reaches no radiant header and no antr_* header at
 * all - that is a property radiant/tests/CMakeLists.txt states as the reason
 * those files are compiled where they are. Everything in THIS directory is the
 * other side of that line: it is allowed to know about ANT channels, about
 * antr_*, about sockets and about buttons.
 *
 * Four files sit behind it:
 *   bridge_pump.c    - the drain thread and the 1 Hz liveness tick. Nothing
 *                      called radiant_bridge_drain() anywhere in the tree
 *                      before this file existed.
 *   rx_tap.c         - the ingest point: ANT broadcasts -> struct radiant_sample.
 *   self_channels.c  - wildcard slave channels and the pairing window.
 *   mqtt_sink.c      - one RADIANT_SINK_DEFINE onto an MQTT broker.
 */

#ifndef BRIDGE_APP_H_
#define BRIDGE_APP_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * THE ONE CLOCK. Every t_us on the bus and every now_us handed to
 * radiant_liveness_tick() comes from here, and if those two ever came from
 * different clocks the liveness table would either expire everything at once
 * or nothing ever - neither of which announces itself. One function, so there
 * is one place to be wrong.
 *
 * k_uptime_ticks() rather than k_uptime_get(): the tick counter is the
 * finest-grained monotonic source Zephyr offers portably, and converting once
 * here keeps the conversion out of the RX path's arithmetic.
 */
uint64_t ant_bridge_now_us(void);

/*
 * The channel -> binding map the RX tap consults. This is where
 * docs/radiant-bridge.md section 10.1 rule 1 ("no promiscuous ingest, ever")
 * is enforced in code rather than in prose: a broadcast arriving on a channel
 * with no entry here is DROPPED, whatever it contains. Binding is the act of
 * adding an entry, and only self_channels.c's pairing window does that.
 *
 * Returns 0, or -EINVAL for a channel outside the allocation.
 */
int  ant_bridge_channel_bind(uint8_t channel, uint32_t source);
void ant_bridge_channel_unbind(uint8_t channel);

/* RADIANT_BINDING_NONE if the channel carries no binding. */
uint32_t ant_bridge_channel_source(uint8_t channel);

/* How many broadcasts the tap has dropped for want of a binding. Exposed
 * because "the bridge is silent" and "the bridge is dropping everything on
 * purpose" look identical from outside, and this is the number that tells
 * them apart. */
uint32_t ant_bridge_unbound_drops(void);

/*
 * Open the pairing window (CONFIG_ANT_DONGLE_PAIRING_WINDOW_S seconds) as
 * though the button had been pressed. Exists so that a shell command or a
 * future host message has one entry point rather than reaching into
 * self_channels.c's state; the button is not the only legitimate way for a
 * user to say "yes, this one", it is only the one a bare DK has.
 *
 * Compiled only when CONFIG_ANT_DONGLE_SELF_CHANNELS > 0.
 */
void self_channels_open_pairing_window(void);

#ifdef __cplusplus
}
#endif

#endif /* BRIDGE_APP_H_ */
