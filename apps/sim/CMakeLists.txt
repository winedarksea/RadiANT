cmake_minimum_required(VERSION 3.20.0)

# Same sdk-ant handling as apps/common/ant_radio_backend.cmake, and
# deliberately a copy rather than an include: the two applications are built
# independently and `west build apps/sim` must not depend on the dongle's
# build files existing or being current.
#
# The duplication is why the resolution order below has to be spelled out twice
# rather than factored into a shared .cmake file: a factored version would be a
# file in the dongle's tree that apps/sim includes, which reintroduces exactly
# the dependency this copy exists to avoid. Keep the two blocks in step by hand
# - they are short, and the only thing that differs is the number of `..`
# needed to reach a sibling checkout (this file is one directory deeper) and
# the application name in the messages.
#
# Unlike the dongle, this one genuinely cannot build without the module - the
# profile libraries and the radio both come from it, and there is no stub
# equivalent here.
if(DEFINED ENV{SDK_ANT_DIR})
  file(TO_CMAKE_PATH "$ENV{SDK_ANT_DIR}" ANT_MODULE_DIR_DEFAULT)
else()
  set(ANT_MODULE_DIR_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk-ant")
endif()

set(ANT_MODULE_DIR "${ANT_MODULE_DIR_DEFAULT}" CACHE PATH
    "Path to the sdk-ant Zephyr module (the directory holding zephyr/module.yml)")

if(EXISTS "${ANT_MODULE_DIR}/zephyr/module.yml")
  message(STATUS "ant_sim: sdk-ant module at ${ANT_MODULE_DIR}")
  list(APPEND ZEPHYR_EXTRA_MODULES "${ANT_MODULE_DIR}")
else()
  message(FATAL_ERROR
    "ant_sim: no sdk-ant module at '${ANT_MODULE_DIR}'.\n"
    "This application is built entirely out of sdk-ant's ANT+ profile "
    "libraries and its radio, so there is nothing to build without it. Either "
    "set the SDK_ANT_DIR environment variable to the sdk-ant checkout (the "
    "per-machine setting, shared with the dongle application), or pass "
    "-DANT_MODULE_DIR=<path> for a single build. Both must point at the "
    "directory that contains zephyr/module.yml.\n"
    "With neither set, the default is a sibling checkout at "
    "'${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk-ant'. In a west workspace, "
    "`west config manifest.group-filter +ant && west update` puts it there.")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ant_sim)

target_sources(app PRIVATE
  src/main.c
  src/sim_signal.c
)
