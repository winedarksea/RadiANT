# SPDX-License-Identifier: Apache-2.0
#
# The trainer simulator's physics and fault model, under test.
#
# The same shape as apps/treadmill/tests/pages and for the same reason: both
# units under test include no Zephyr header, take no lock and read no clock,
# so this image is two production .c files beside one test file - no radiant,
# no backend, no radio - and ten thousand fault decisions take microseconds.
#
# profile_fec_tx.h is included for its STRUCTS only (the model fills page
# structs); no profile .c file is linked, and nothing here needs one. Its
# include chain (profile_sched.h -> profile_handoff.h) reaches
# radiant/include for a type, hence that directory - headers only.
#
# On Windows there is no native_sim; use
#   scripts\run_ztest_hw.ps1 -App apps\trainer\tests\model
# on a DK.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(trainer_model_test)

target_sources(app PRIVATE
  src/main.c
  ../../src/trainer_model.c
  ../../src/trainer_ackfault.c
)
target_include_directories(app PRIVATE
  ../../src
  ../../../../radiant/src/profiles
  ../../../../radiant/include
)
