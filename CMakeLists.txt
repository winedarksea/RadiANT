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
# radiant_core is appended unconditionally, and the asymmetry with sdk-ant is
# deliberate. sdk-ant is appended only for -DANT_RADIO=sdk_ant because it may
# not be on disk at all; radiant_core is in this repository and always is. So
# it is always a module and CONFIG_RADIANT_CORE gates it, which is both the
# ordinary Zephyr idiom and a better test: an sdk_ant build then asserts
# CONFIG_RADIANT_CORE=n positively, rather than proving nothing by the module's
# absence. radiant_core/CMakeLists.txt returns immediately when the symbol is
# n, so an sdk_ant build cannot reach a <radiant_core/...> header either.
list(APPEND ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/radiant_core")

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
    "  core     the clean-room radiant_core stack. No sdk-ant of any kind.\n"
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
  # Which HAL backend radiant_core links. It has to be stated here rather than
  # left to a -DCONFIG_ override or an EXTRA_CONF_FILE, because this fragment is
  # merged LAST and would silently win against either - which is exactly what it
  # is for everywhere else, and exactly what makes it a trap here.
  #
  # Forwarded from sysbuild the same way ANT_RADIO is, and for the same reason:
  # `west build -- -DRADIANT_BACKEND=nrf` sets it on the SYSBUILD cache, not on
  # this image's, so without this the flag is accepted and silently ignored -
  # which is how a "verified" build of the nrf backend turns out to have
  # compiled the null one.
  ant_sysbuild_value(RADIANT_BACKEND RADIANT_BACKEND_SYSBUILD)
  if(RADIANT_BACKEND_SYSBUILD)
    set(RADIANT_BACKEND "${RADIANT_BACKEND_SYSBUILD}" CACHE STRING
        "radiant_core HAL backend: null or nrf" FORCE)
  else()
    set(RADIANT_BACKEND "null" CACHE STRING
        "radiant_core HAL backend: null (refuses every arm) or nrf (the RADIO)")
  endif()
  set_property(CACHE RADIANT_BACKEND PROPERTY STRINGS null nrf)

  if(RADIANT_BACKEND STREQUAL "null")
    set(RADIANT_BACKEND_CONF "CONFIG_RADIANT_CORE_BACKEND_NULL=y")
  elseif(RADIANT_BACKEND STREQUAL "nrf")
    set(RADIANT_BACKEND_CONF "CONFIG_RADIANT_CORE_BACKEND_NRF=y")
  else()
    message(FATAL_ERROR
      "ant_dongle: RADIANT_BACKEND='${RADIANT_BACKEND}' is not a backend.\n"
      "  null  the lifecycle and the clock are real; every arm is refused.\n"
      "  nrf   the RADIO, one TIMER and four (D)PPI channels, owned\n"
      "        outright. Receives AND transmits - see\n"
      "        radiant_core/src/radiant_radio_nrf.c.\n"
      "Pass one with -DRADIANT_BACKEND=<value>.")
  endif()
  message(STATUS "ant_dongle: radiant_core HAL backend ${RADIANT_BACKEND}")

  set(ANT_RADIO_CONF
"# Generated by CMakeLists.txt from -DANT_RADIO. Do not edit; do not commit.
CONFIG_ANT_DONGLE_RADIO_CORE=y

# radiant_core and its HAL backend, from -DRADIANT_BACKEND.
CONFIG_RADIANT_CORE=y
${RADIANT_BACKEND_CONF}
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

# ── The backend actually compiled is the backend that was asked for ──────────
#
# ANT_RADIO_CONF above writes CONFIG_RADIANT_CORE_BACKEND_NRF=y into a fragment
# merged last, so it cannot lose to another fragment. It CAN still lose to
# Kconfig itself: the symbol has dependencies, and an unmet one makes the
# assignment a no-op that falls back to the choice default. Zephyr prints a
# warning about that among a hundred others and carries on.
#
# It is not hypothetical and it is not cheap. RADIANT_CORE_BACKEND_NRF used to
# depend on SOC_SERIES_NRF52X || SOC_SERIES_NRF54LX; Zephyr 4.4 renamed both, so
# every -DRADIANT_BACKEND=nrf build on NCS v3.4.0 silently compiled the null
# backend - an image that enumerates, answers the host, runs every state machine
# and finds no sensors, which looks exactly like a radio that is not working.
# The dependency is fixed; this is what makes the next one loud on the first
# build rather than after a bench session.
if(ANT_RADIO STREQUAL "core" AND RADIANT_BACKEND STREQUAL "nrf"
   AND NOT CONFIG_RADIANT_CORE_BACKEND_NRF)
  message(FATAL_ERROR
    "ant_dongle: -DRADIANT_BACKEND=nrf was asked for and Kconfig did not "
    "select it.\n"
    "  CONFIG_RADIANT_CORE_BACKEND_NRF is unset, so the NULL backend was "
    "compiled instead and nothing would go on the air.\n"
    "  Almost always an unmet `depends on` in radiant_core/Kconfig for this "
    "board or this SDK - check SOC_COMPATIBLE_NRF52X / SOC_COMPATIBLE_NRF54LX "
    "and !BT in this build's .config.")
endif()

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

# ── The hostless node's identity (compat-C6) ────────────────────────────────
#
# docs/decisions/0009-hostless-node-identity.md. Default off and off in every
# dongle build: a dongle's epoch authority is the PC behind it, over 0xF3, and
# its pairing scalar arrives over 0xF5 - it wants none of this. The symbol and
# the wiring live here so that a node application built from this tree has a
# home for them, and so the storage backend is selected the same way every other
# backend in this project is.
#
# The counters' storage is a seam (src/node/node_nvm.h) with no default
# implementation, which is why the backend is a separate condition rather than
# being folded into the one above: a build that selects the identity and no
# backend must fail at the link, not fall back to RAM.
if(CONFIG_RADIANT_NODE_IDENT)
  target_sources(app PRIVATE src/node/node_ident.c)
  target_include_directories(app PRIVATE src/node)
endif()
if(CONFIG_RADIANT_NODE_NVM_SETTINGS)
  target_sources(app PRIVATE src/node/node_nvm_settings.c)
endif()

# Exactly one radio backend, chosen by ANT_RADIO above. All three implement
# src/ant_radio.h, so the bridge and main() do not know which is present -
# the same shape as the transport choice, deliberately.
#
# Each is guarded on existing, and a missing one is a warning rather than a
# hard error, because the three implementations arrive at different times: the
# stub and the sdk-ant shim in this same wave, radiant_core in Phase 5. A build
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
  # Nothing to do here, and that is the point. radiant_core is a Zephyr module
  # appended to ZEPHYR_EXTRA_MODULES above and switched on by
  # CONFIG_RADIANT_CORE in the generated fragment; it owns its own source list,
  # its own Kconfig-selected HAL backend, and must stay buildable outside this
  # application (radiant_core/tests builds it with no dongle in sight).
  message(STATUS "ant_dongle: radiant_core module supplies the antr_* backend")
endif()
