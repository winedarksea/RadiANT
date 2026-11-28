# SPDX-License-Identifier: Apache-2.0
#
# The smart trainer kernel-free sources against ztest. No radiant module, no
# backend, no BLE stack: the production .c files below include no Zephyr
# header, so they compile beside the tests unmodified.
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(smart_trainer_glue_test)

set(R ../../../../radiant)
target_sources(app PRIVATE
  src/main.c
  src/equip.c
  src/sim_glue.c
  src/cmd.c
  ../../src/trainer_equip.c
  ../../src/trainer_cmd.c
  ../../src/trainer_sim_model.c
  # apps/trainer model, reused in place as in the application.
  ../../../trainer/src/trainer_model.c
  ${R}/src/ble/radiant_ble_ftms_core.c
  ${R}/src/ble/radiant_ctrl_arbiter.c
  # The FE-C master, for the decoders and the page-70/71 round trips.
  ${R}/src/profiles/radiant_profile_fec_tx.c
  ${R}/src/profiles/radiant_profile_sched.c
  ${R}/src/profiles/radiant_profile_schedule.c
  ${R}/src/profiles/radiant_profile_telemetry.c
  ${R}/src/profiles/radiant_profile_bits.c
  ${R}/src/profiles/radiant_profile_common.c
  ${R}/src/profiles/radiant_profile_handoff.c
)
target_include_directories(app PRIVATE ../../src ../../../trainer/src ${R}/include)
