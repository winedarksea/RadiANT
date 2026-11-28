/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_source_custom.c - CONFIG_SMART_TRAINER_SOURCE_CUSTOM.
 *
 * NOTHING HERE IMPLEMENTS THE SEAM, deliberately. Your driver fills in a
 * struct trainer_source_ops (src/trainer_source.h) and calls
 * trainer_source_register() from a SYS_INIT at APPLICATION level, then calls
 * trainer_source_report() with each reading. Use src/trainer_sim.c as the
 * worked example.
 *
 * If nothing registers, the trainer transmits FE-C with every measurement
 * invalid and declares no control mode: correct, visible, and never a
 * simulated rider on a real machine. main() logs the missing driver as an
 * error at boot.
 */
