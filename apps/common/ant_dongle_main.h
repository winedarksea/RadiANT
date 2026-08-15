/* SPDX-License-Identifier: Apache-2.0 */

#ifndef ANT_DONGLE_MAIN_H_
#define ANT_DONGLE_MAIN_H_

/*
 * The ANT-serial dongle boot sequence, shared by apps/dongle and
 * apps/dongle_thread.
 *
 * WHY THIS IS HERE AND NOT COPIED. The two applications ship the same
 * initialisation order, and that order is load-bearing in three separate
 * places (the UF2 settle before enumeration, antr_init() before the transport
 * asks for the HFXO, the bridge before ant_transport_enable()). Two copies of
 * it in two shipping products is two things that must not diverge and nothing
 * making them agree - and apps/dongle_thread carried exactly that copy until
 * this file existed.
 *
 * target_sources(app PRIVATE ../dongle/src/main.c) is NOT the alternative:
 * apps/hrm_ble/CMakeLists.txt states the rule this file obeys - an application
 * must not reach into another application's build files. apps/common/ is where
 * the shared half of an application legitimately lives, and this file is
 * added onto each consumer's own `app` target by apps/common/sources.cmake,
 * gated on CONFIG_ANT_DONGLE_MAIN, exactly like every other source there.
 */

/*
 * The whole boot sequence. Returns non-zero on the first failed step (the
 * caller's main() should return it); on success it does not return - it falls
 * into the heartbeat loop, the way the two main()s it replaces did.
 */
int ant_dongle_main_run(void);

/*
 * Runs after antr_init() has succeeded and before usb_ant_class_init() and the
 * transport. Weak, defaulting to a no-op; an application that needs to bring a
 * second wireless stack up overrides it (apps/dongle_thread does, for the two
 * coexistence loads).
 *
 * AFTER antr_init() IS THE CONTRACT, not an accident of ordering: the radio
 * backend holds an explicit HFXO request that is load-bearing - without it the
 * 1 MHz timebase runs 0.42 % fast on the internal RC between timeslots, which
 * shows up in no counter anywhere. A second stack started before that request
 * exists gets a subtly wrong clock and nothing says so.
 *
 * A weak symbol rather than a Kconfig #if because that is the in-tree idiom
 * for exactly this shape - radiant_matter_attr_write() in
 * radiant/src/bridge/radiant_matter.h is the same hook for the same reason.
 * Note the hazard a weak override carries (a signature typo silently keeps the
 * default): the default logs nothing, so apps/dongle_thread's own boot log
 * naming each load it started is what proves the override took.
 */
void ant_dongle_post_radio_init(void);

#endif /* ANT_DONGLE_MAIN_H_ */
