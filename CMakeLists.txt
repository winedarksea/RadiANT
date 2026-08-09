cmake_minimum_required(VERSION 3.20.0)

# ── How a build setting reaches this file under sysbuild ────────────────────
#
# Everything below has to be decided *before* find_package(Zephyr), because
# `list(APPEND ZEPHYR_EXTRA_MODULES ...)` is read during it. That rules out
# every Zephyr helper - zephyr_get(), the sysbuild_cache target, zephyr_file()
# - since none of them exist yet. It also rules out Kconfig, which is why the
# backend is a CMake cache variable and Kconfig only mirrors the result.
#
# What is left is the sysbuild cache file, and reading it by hand is not a
# workaround - it is the only mechanism that works here, so it is worth
# recording exactly what was measured:
#
#   west build ... -- -DANT_MODULE_DIR=X   lands in the *sysbuild* CMake cache.
#     Sysbuild copies its whole cache into <build>/<image>_sysbuild_cache.txt
#     and hands the image `-DSYSBUILD_CACHE:FILEPATH=<that file>`. The image
#     loads it onto a `sysbuild_cache` target, but nothing reads an entry from
#     there unless someone calls zephyr_get(<name>) - which we cannot, this
#     early. So the value is present and inert, and the image keeps its own
#     default. Silently.
#   -Dant_dongle_ANT_MODULE_DIR=X          same story, one scope narrower: it
#     appears in the sysbuild cache as an UNINITIALIZED entry and is likewise
#     never read.
#   --no-sysbuild, or the SDK_ANT_DIR environment variable, do work - because
#     then the value is in this CMake process's own cache or environment.
#
# That difference matters more than it looks: `build_all.ps1`'s fourth
# assertion and CI's "no sdk-ant on disk" job both *prove* independence by
# pointing ANT_MODULE_DIR at a path that cannot exist. If the override never
# reached the image, those builds would quietly resolve the real checkout and
# pass while proving nothing - a green light that means nothing is worse than
# no light at all.
#
# So: parse the sysbuild cache file ourselves, with zephyr_get()'s own
# precedence (image-local `<image>_<VAR>` beats global `<VAR>`), and let the
# ordinary CMake cache and the environment take over below when sysbuild is
# not in the picture.
function(ant_sysbuild_value var out)
  set(${out} "" PARENT_SCOPE)
  if(NOT SYSBUILD OR NOT DEFINED SYSBUILD_CACHE OR NOT EXISTS "${SYSBUILD_CACHE}")
    return()
  endif()

  file(STRINGS "${SYSBUILD_CACHE}" _lines ENCODING UTF-8)

  # The image's own name, as sysbuild knows it - "ant_dongle" here, but read it
  # rather than assume it, because it is what prefixes an image-local setting.
  set(_image "")
  foreach(_line IN LISTS _lines)
    if(_line MATCHES "^SYSBUILD_NAME:[^=]*=(.*)$")
      set(_image "${CMAKE_MATCH_1}")
    endif()
  endforeach()

  set(_names "${var}")
  if(_image)
    list(PREPEND _names "${_image}_${var}")
  endif()

  foreach(_name IN LISTS _names)
    foreach(_line IN LISTS _lines)
      if(_line MATCHES "^${_name}:[^=]*=(.*)$")
        set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
        return()
      endif()
    endforeach()
  endforeach()
endfunction()

# ── Where sdk-ant is ────────────────────────────────────────────────────────
#
# Location of the sdk-ant Zephyr module. Override on the command line with
#   -DANT_MODULE_DIR=<path>
# It supplies both the ANT headers (include/, init/) and the prebuilt libant.a.
# Only the sdk_ant backend needs it; core and stub builds never look at it.
#
# The default is resolved rather than hardcoded, because sdk-ant is a private
# adopter-gated checkout that lands wherever its owner put it:
#   1. $ENV{SDK_ANT_DIR} if the environment names one. This is the per-machine
#      setting - set it once in a shell profile and every build in the tree
#      picks it up, including sim/ and any out-of-tree consumer.
#   2. Otherwise a sibling checkout next to this repository. That is where
#      west.yml places it (<topdir>/sdk-ant, in the `ant` group) and where a
#      manual `git clone` alongside this repository naturally lands, so the
#      common case needs no configuration at all.
# Neither is a guess about a particular user's home directory.
ant_sysbuild_value(ANT_MODULE_DIR ANT_MODULE_DIR_SYSBUILD)
if(ANT_MODULE_DIR_SYSBUILD)
  # FORCE, because sysbuild is the outer authority: on a re-configure our own
  # cache entry from the previous run would otherwise win over a changed -D.
  set(ANT_MODULE_DIR "${ANT_MODULE_DIR_SYSBUILD}" CACHE PATH
      "Path to the sdk-ant Zephyr module (the directory holding zephyr/module.yml)" FORCE)
else()
  if(DEFINED ENV{SDK_ANT_DIR})
    file(TO_CMAKE_PATH "$ENV{SDK_ANT_DIR}" ANT_MODULE_DIR_DEFAULT)
  else()
    set(ANT_MODULE_DIR_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/../sdk-ant")
  endif()

  set(ANT_MODULE_DIR "${ANT_MODULE_DIR_DEFAULT}" CACHE PATH
      "Path to the sdk-ant Zephyr module (the directory holding zephyr/module.yml)")
endif()

if(EXISTS "${ANT_MODULE_DIR}/zephyr/module.yml")
  set(ANT_MODULE_PRESENT TRUE)
else()
  set(ANT_MODULE_PRESENT FALSE)
endif()

# ── Which radio backend ─────────────────────────────────────────────────────
#
# CMake decides, Kconfig mirrors. This is not a preference: appending to
# ZEPHYR_EXTRA_MODULES has to happen before find_package(Zephyr), therefore
# before Kconfig exists at all, so a Kconfig symbol cannot decide whether
# sdk-ant's Kconfig gets sourced. The decision is made here and written back
# out as CONFIG_ANT_DONGLE_RADIO_* below, so that .config records which
# backend was compiled and CI can assert on it.
#
# The default follows what is on disk, so that neither of the two normal
# situations needs a flag: with an sdk-ant checkout resolved, `sdk_ant` is the
# proven build and the one release artifacts come from; with no checkout - a
# fork, a fresh clone, a CI job with no secret - `core` is the only honest
# answer, and it fails at the link rather than at a path that was never the
# user's fault.
ant_sysbuild_value(ANT_RADIO ANT_RADIO_SYSBUILD)
if(ANT_RADIO_SYSBUILD)
  set(ANT_RADIO "${ANT_RADIO_SYSBUILD}" CACHE STRING
      "ANT radio backend: sdk_ant, core or stub" FORCE)
else()
  if(ANT_MODULE_PRESENT)
    set(ANT_RADIO_DEFAULT "sdk_ant")
  else()
    set(ANT_RADIO_DEFAULT "core")
  endif()
  set(ANT_RADIO "${ANT_RADIO_DEFAULT}" CACHE STRING
      "ANT radio backend: sdk_ant, core or stub")
endif()
set_property(CACHE ANT_RADIO PROPERTY STRINGS sdk_ant core stub)

if(NOT ANT_RADIO MATCHES "^(sdk_ant|core|stub)$")
  message(FATAL_ERROR
    "ant_dongle: ANT_RADIO='${ANT_RADIO}' is not a radio backend.\n"
    "Valid values are:\n"
    "  sdk_ant  Nordic's prebuilt libant.a from sdk-ant, through the thin\n"
    "           forwarders in src/ant_radio_sdk_ant.c. Needs an sdk-ant\n"
    "           checkout; see ANT_MODULE_DIR.\n"
    "  core     the clean-room ant_core stack. No sdk-ant of any kind.\n"
    "  stub     src/ant_radio_stub.c - accepts everything, transmits nothing.\n"
    "Pass one with -DANT_RADIO=<value>.")
endif()

if(ANT_RADIO STREQUAL "sdk_ant")
  if(ANT_MODULE_PRESENT)
    message(STATUS "ant_dongle: radio backend sdk_ant, module at ${ANT_MODULE_DIR}")
    list(APPEND ZEPHYR_EXTRA_MODULES "${ANT_MODULE_DIR}")
  else()
    message(FATAL_ERROR
      "ant_dongle: ANT_RADIO=sdk_ant but no sdk-ant module at "
      "'${ANT_MODULE_DIR}'.\n"
      "Either set the SDK_ANT_DIR environment variable to the sdk-ant "
      "checkout (the per-machine setting, picked up by this application and "
      "by sim/), or pass -DANT_MODULE_DIR=<path> for a single build. Both "
      "must point at the directory that contains zephyr/module.yml.\n"
      "With neither set, the default is a sibling checkout at "
      "'${CMAKE_CURRENT_SOURCE_DIR}/../sdk-ant'. In a west workspace, "
      "`west config manifest.group-filter +ant && west update` puts it "
      "there.\n"
      "If you do not have sdk-ant at all, that is a supported configuration: "
      "build with -DANT_RADIO=core or -DANT_RADIO=stub and nothing here will "
      "look for it.")
  endif()
else()
  # Nothing to do, and that is the point. The module is never appended, so
  # sdk-ant's Kconfig is never sourced, CONFIG_ANT never comes into existence,
  # and there is no CONFIG_ANT=n to set anywhere - a symbol that does not exist
  # cannot be assigned to, and Zephyr treats assigning to one as fatal.
  message(STATUS "ant_dongle: radio backend ${ANT_RADIO}, no sdk-ant module")
endif()

# ── Telling Kconfig what CMake decided ──────────────────────────────────────
#
# A generated fragment rather than a line in prj.conf, for two reasons. The
# backend is not known until the block above has run, and the CONFIG_ANT*
# settings name symbols that only exist when sdk-ant's Kconfig was sourced -
# stating them unconditionally in prj.conf would make every core or stub build
# fail on an assignment to an undefined symbol.
#
# It lands in the application binary directory because Zephyr globs *.conf
# there and merges the result *after* everything else, including
# -DEXTRA_CONF_FILE. That ordering is what makes this authoritative: the
# backend CMake actually compiled and the backend Kconfig records cannot
# disagree.
if(ANT_RADIO STREQUAL "sdk_ant")
  set(ANT_RADIO_CONF
"# Generated by CMakeLists.txt from -DANT_RADIO. Do not edit; do not commit.
CONFIG_ANT_DONGLE_RADIO_SDK_ANT=y

# ANT stack - 8 channels gives Zwift headroom for HRM + power + cadence + extras
CONFIG_ANT=y
CONFIG_ANT_EVALUATION_KEY=y
CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED=8
CONFIG_ANT_BURST_QUEUE_SIZE=256
")
elseif(ANT_RADIO STREQUAL "core")
  set(ANT_RADIO_CONF
"# Generated by CMakeLists.txt from -DANT_RADIO. Do not edit; do not commit.
CONFIG_ANT_DONGLE_RADIO_CORE=y
")
else()
  set(ANT_RADIO_CONF
"# Generated by CMakeLists.txt from -DANT_RADIO. Do not edit; do not commit.
CONFIG_ANT_DONGLE_RADIO_STUB=y
")
endif()

# Write only on a change. Zephyr adds every merged fragment to
# CMAKE_CONFIGURE_DEPENDS, so rewriting an identical file each configure would
# hand ninja a freshly-stamped dependency of the configure step it just ran.
set(ANT_RADIO_CONF_FILE "${CMAKE_CURRENT_BINARY_DIR}/ant_radio_backend.conf")
set(ANT_RADIO_CONF_EXISTING "")
if(EXISTS "${ANT_RADIO_CONF_FILE}")
  file(READ "${ANT_RADIO_CONF_FILE}" ANT_RADIO_CONF_EXISTING)
endif()
if(NOT ANT_RADIO_CONF_EXISTING STREQUAL ANT_RADIO_CONF)
  file(WRITE "${ANT_RADIO_CONF_FILE}" "${ANT_RADIO_CONF}")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ant_dongle)

target_sources(app PRIVATE
  src/main.c
  src/ant_serial_bridge.c
)

# Exactly one transport, chosen by Kconfig. All three implement
# src/ant_transport.h, so the bridge and main() do not know which is present.
if(CONFIG_ANT_DONGLE_TRANSPORT_USB_LEGACY)
  target_sources(app PRIVATE src/usb_ant_class.c)
elseif(CONFIG_ANT_DONGLE_TRANSPORT_USB_NEXT)
  target_sources(app PRIVATE src/usb_ant_class_next.c)
elseif(CONFIG_ANT_DONGLE_TRANSPORT_UART)
  target_sources(app PRIVATE src/ant_uart_transport.c)
else()
  message(FATAL_ERROR "ant_dongle: no transport selected")
endif()

if(CONFIG_ANT_DONGLE_FLASH_LOG)
  target_sources(app PRIVATE src/diag_flash_log.c)
endif()

# Exactly one radio backend, chosen by ANT_RADIO above. All three implement
# src/ant_radio.h, so the bridge and main() do not know which is present -
# the same shape as the transport choice, deliberately.
#
# Each is guarded on existing, and a missing one is a warning rather than a
# hard error, because the three implementations arrive at different times: the
# stub and the sdk-ant shim in this same wave, ant_core in Phase 5. A build
# whose backend has not been written yet must still configure and still reach
# the link, because "it fails with 42 undefined antr_* symbols" is a useful
# answer and "CMake cannot find a source file" is not - it hides every other
# problem behind the first missing file.
if(ANT_RADIO STREQUAL "sdk_ant")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/ant_radio_sdk_ant.c")
    target_sources(app PRIVATE src/ant_radio_sdk_ant.c)
  else()
    message(WARNING
      "ant_dongle: ANT_RADIO=sdk_ant but src/ant_radio_sdk_ant.c does not "
      "exist. libant.a is linked, but nothing forwards src/ant_radio.h onto "
      "it, so the link will fail with undefined antr_* references.")
  endif()
elseif(ANT_RADIO STREQUAL "stub")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/ant_radio_stub.c")
    target_sources(app PRIVATE src/ant_radio_stub.c)
  else()
    message(WARNING
      "ant_dongle: ANT_RADIO=stub but src/ant_radio_stub.c does not exist. "
      "The link will fail with undefined antr_* references.")
  endif()
else()
  # ant_core is a subdirectory rather than a file list: it is a whole stack
  # with its own Kconfig-selected backends, and it must stay buildable outside
  # this application (ant_core/tests builds it with no dongle in sight).
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ant_core/CMakeLists.txt")
    add_subdirectory(ant_core)
  else()
    message(WARNING
      "ant_dongle: ANT_RADIO=core but ant_core/CMakeLists.txt does not exist "
      "- the clean-room stack is Phase 5 and only its header and test harness "
      "are in the tree. The link will fail with undefined antr_* references.")
  endif()
endif()
