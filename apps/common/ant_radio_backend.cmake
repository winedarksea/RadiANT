# ── The ANT_RADIO axis: sdk_ant, core or stub ────────────────────────────────
#
# Shared between apps/dongle and apps/dongle_thread - the two applications
# with a serial bridge to a host, which is the only reason this axis exists at
# all. apps/hrm_ble never includes this file: it has no host, no bridge and no
# antr_* backend choice - CONFIG_RADIANT=y unconditionally, same as
# radiant_select_backend() alone gives every other node application.
#
# Lives in apps/common/, not in the top-level cmake/ directory beside
# radiant_backend.cmake, on purpose: radiant_backend.cmake is shipped a second
# time inside the module itself (radiant/cmake/radiant_backend.cmake) for an
# out-of-tree consumer to use, because RADIANT_BACKEND is a property of the
# module. ANT_RADIO is not - it is which of three antr_* IMPLEMENTATIONS this
# application links, and sdk-ant, being private and non-redistributable, must
# never be reachable from anything the module's own directory points at. So
# this file stays application-side, permanently.
#
# Must be include()d BEFORE find_package(Zephyr), same reason
# radiant_backend.cmake gives: appending sdk-ant to ZEPHYR_EXTRA_MODULES has to
# happen before Kconfig exists, so CMake decides and Kconfig only mirrors the
# decision afterward. Depends on radiant_sysbuild_value() from
# cmake/radiant_backend.cmake, so that file must be include()d first.

# ── Where sdk-ant is ──────────────────────────────────────────────────────
#
# Location of the sdk-ant Zephyr module. Override on the command line with
#   -DANT_MODULE_DIR=<path>
# It supplies both the ANT headers (include/, init/) and the prebuilt libant.a.
# Only the sdk_ant backend needs it; core and stub builds never look at it.
#
# The default is resolved rather than hardcoded, because sdk-ant is a private,
# non-redistributable checkout that lands wherever its owner put it:
#   1. $ENV{SDK_ANT_DIR} if the environment names one. This is the per-machine
#      setting - set it once in a shell profile and every build in the tree
#      picks it up, including apps/sim and any out-of-tree consumer.
#   2. Otherwise a sibling checkout next to this repository. That is where
#      west.yml places it (<topdir>/sdk-ant, in the `ant` group) and where a
#      manual `git clone` alongside this repository naturally lands, so the
#      common case needs no configuration at all.
# Neither is a guess about a particular user's home directory.
#
# `app_dir` is the calling application's own CMAKE_CURRENT_SOURCE_DIR, passed
# explicitly rather than read from the caller's scope, because this file is
# include()d - it runs in the caller's scope already - but the sibling-checkout
# default has to count "one more `..`" for apps/ now sitting two directories
# below the repository root instead of one, and a parameter makes that count
# visible at the call site instead of hidden in this file.
function(ant_select_radio app_dir default_radio radiant_backend_conf conf_out)
  radiant_sysbuild_value(ANT_MODULE_DIR ANT_MODULE_DIR_SYSBUILD)
  if(ANT_MODULE_DIR_SYSBUILD)
    # FORCE, because sysbuild is the outer authority: on a re-configure our
    # own cache entry from the previous run would otherwise win over a
    # changed -D.
    set(ANT_MODULE_DIR "${ANT_MODULE_DIR_SYSBUILD}" CACHE PATH
        "Path to the sdk-ant Zephyr module (the directory holding zephyr/module.yml)" FORCE)
  else()
    if(DEFINED ENV{SDK_ANT_DIR})
      file(TO_CMAKE_PATH "$ENV{SDK_ANT_DIR}" ANT_MODULE_DIR_DEFAULT)
    else()
      set(ANT_MODULE_DIR_DEFAULT "${app_dir}/../../../sdk-ant")
    endif()

    set(ANT_MODULE_DIR "${ANT_MODULE_DIR_DEFAULT}" CACHE PATH
        "Path to the sdk-ant Zephyr module (the directory holding zephyr/module.yml)")
  endif()

  if(EXISTS "${ANT_MODULE_DIR}/zephyr/module.yml")
    set(ANT_MODULE_PRESENT TRUE)
  else()
    set(ANT_MODULE_PRESENT FALSE)
  endif()

  # ── Which radio backend ───────────────────────────────────────────────────
  #
  # CMake decides, Kconfig mirrors. This is not a preference: appending to
  # ZEPHYR_EXTRA_MODULES has to happen before find_package(Zephyr), therefore
  # before Kconfig exists at all, so a Kconfig symbol cannot decide whether
  # sdk-ant's Kconfig gets sourced. The decision is made here and written back
  # out as CONFIG_ANT_DONGLE_RADIO_* below, so that .config records which
  # backend was compiled and CI can assert on it.
  #
  # The default follows what is on disk, so that neither of the two normal
  # situations needs a flag: with an sdk-ant checkout resolved, `sdk_ant` is
  # the proven build and the one release artifacts come from; with no
  # checkout - a fork, a fresh clone, a CI job with no secret - `core` is the
  # only honest answer, and it fails at the link rather than at a path that
  # was never the user's fault. `default_radio` lets a caller override this
  # (dongle_thread's coex arms may want a different default than the plain
  # dongle's).
  radiant_sysbuild_value(ANT_RADIO ANT_RADIO_SYSBUILD)
  if(ANT_RADIO_SYSBUILD)
    set(ANT_RADIO "${ANT_RADIO_SYSBUILD}" CACHE STRING
        "ANT radio backend: sdk_ant, core or stub" FORCE)
  else()
    if(NOT default_radio)
      if(ANT_MODULE_PRESENT)
        set(default_radio "sdk_ant")
      else()
        set(default_radio "core")
      endif()
    endif()
    set(ANT_RADIO "${default_radio}" CACHE STRING
        "ANT radio backend: sdk_ant, core or stub")
  endif()
  set_property(CACHE ANT_RADIO PROPERTY STRINGS sdk_ant core stub)
  set(ANT_RADIO "${ANT_RADIO}" PARENT_SCOPE)

  if(NOT ANT_RADIO MATCHES "^(sdk_ant|core|stub)$")
    message(FATAL_ERROR
      "ANT_RADIO='${ANT_RADIO}' is not a radio backend.\n"
      "Valid values are:\n"
      "  sdk_ant  Nordic's prebuilt libant.a from sdk-ant, through the thin\n"
      "           forwarders in apps/common/ant_radio_sdk_ant.c. Needs an\n"
      "           sdk-ant checkout; see ANT_MODULE_DIR.\n"
      "  core     the clean-room radiant stack. No sdk-ant of any kind.\n"
      "  stub     apps/common/ant_radio_stub.c - accepts everything,\n"
      "           transmits nothing.\n"
      "Pass one with -DANT_RADIO=<value>.")
  endif()

  if(ANT_RADIO STREQUAL "sdk_ant")
    if(ANT_MODULE_PRESENT)
      message(STATUS "${app_dir}: radio backend sdk_ant, module at ${ANT_MODULE_DIR}")
      list(APPEND ZEPHYR_EXTRA_MODULES "${ANT_MODULE_DIR}")
      set(ZEPHYR_EXTRA_MODULES "${ZEPHYR_EXTRA_MODULES}" PARENT_SCOPE)
    else()
      message(FATAL_ERROR
        "ANT_RADIO=sdk_ant but no sdk-ant module at '${ANT_MODULE_DIR}'.\n"
        "Either set the SDK_ANT_DIR environment variable to the sdk-ant "
        "checkout (the per-machine setting, picked up by every application "
        "and by apps/sim), or pass -DANT_MODULE_DIR=<path> for a single "
        "build. Both must point at the directory that contains "
        "zephyr/module.yml.\n"
        "With neither set, the default is a sibling checkout at "
        "'${app_dir}/../../../sdk-ant'. In a west workspace, "
        "`west config manifest.group-filter +ant && west update` puts it "
        "there.\n"
        "If you do not have sdk-ant at all, that is a supported "
        "configuration: build with -DANT_RADIO=core or -DANT_RADIO=stub and "
        "nothing here will look for it.")
    endif()
  else()
    # Nothing to do, and that is the point. The module is never appended, so
    # sdk-ant's Kconfig is never sourced, CONFIG_ANT never comes into
    # existence, and there is no CONFIG_ANT=n to set anywhere - a symbol that
    # does not exist cannot be assigned to, and Zephyr treats assigning to
    # one as fatal.
    message(STATUS "${app_dir}: radio backend ${ANT_RADIO}, no sdk-ant module")
  endif()

  # ── Telling Kconfig what CMake decided ────────────────────────────────────
  #
  # A generated fragment rather than a line in prj.conf, for two reasons. The
  # backend is not known until the block above has run, and the CONFIG_ANT*
  # settings name symbols that only exist when sdk-ant's Kconfig was sourced -
  # stating them unconditionally in prj.conf would make every core or stub
  # build fail on an assignment to an undefined symbol.
  if(ANT_RADIO STREQUAL "sdk_ant")
    set(_conf
"CONFIG_ANT_DONGLE_RADIO_SDK_ANT=y

# ANT stack - 8 channels gives Zwift headroom for HRM + power + cadence + extras
CONFIG_ANT=y
CONFIG_ANT_EVALUATION_KEY=y
CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED=8
CONFIG_ANT_BURST_QUEUE_SIZE=256
")
  elseif(ANT_RADIO STREQUAL "core")
    set(_conf
"CONFIG_ANT_DONGLE_RADIO_CORE=y

# radiant and its HAL backend, from -DRADIANT_BACKEND.
CONFIG_RADIANT=y
${radiant_backend_conf}
")
  else()
    set(_conf "CONFIG_ANT_DONGLE_RADIO_STUB=y\n")
  endif()

  set(${conf_out} "${_conf}" PARENT_SCOPE)
endfunction()

# ── The backend actually compiled is the backend that was asked for ─────────
#
# See cmake/radiant_backend.cmake's radiant_assert_backend() for the full
# rationale. Only relevant when ANT_RADIO chose radiant in the first place -
# callers that reach this only under `if(ANT_RADIO STREQUAL "core")` already
# guard it, but the function itself does too, so a future call site cannot
# assert against the wrong backend by forgetting the guard.
function(ant_assert_radio label)
  if(ANT_RADIO STREQUAL "core")
    radiant_assert_backend("${label}")
  endif()
endfunction()
