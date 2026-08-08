cmake_minimum_required(VERSION 3.20.0)

# Same sdk-ant handling as the root CMakeLists.txt, and deliberately a copy
# rather than an include: the two applications are built independently and
# `west build sim` must not depend on the dongle's build files existing or
# being current.
#
# Unlike the dongle, this one genuinely cannot build without the module - the
# profile libraries and the radio both come from it, and there is no stub
# equivalent here.
set(ANT_MODULE_DIR "C:/Users/Colin/sdk-ant" CACHE PATH
    "Path to the sdk-ant Zephyr module (the directory holding zephyr/module.yml)")

if(EXISTS "${ANT_MODULE_DIR}/zephyr/module.yml")
  message(STATUS "ant_sim: sdk-ant module at ${ANT_MODULE_DIR}")
  list(APPEND ZEPHYR_EXTRA_MODULES "${ANT_MODULE_DIR}")
else()
  message(FATAL_ERROR
    "ant_sim: no sdk-ant module at '${ANT_MODULE_DIR}'.\n"
    "This application is built entirely out of sdk-ant's ANT+ profile "
    "libraries and its radio, so there is nothing to build without it. Pass "
    "-DANT_MODULE_DIR=<path> pointing at the directory that contains "
    "zephyr/module.yml.")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ant_sim)

target_sources(app PRIVATE
  src/main.c
  src/sim_signal.c
)
