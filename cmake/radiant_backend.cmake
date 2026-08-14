# ── Shared backend-selection machinery ───────────────────────────────────────
#
# Every RadiANT application (the dongle, strap and, later, dongle_thread and
# dongle_ti) needs to make the same two decisions before find_package(Zephyr):
# which HAL backend radiant_core links, and whether a value handed to
# sysbuild's outer image (`west build ... -- -DRADIANT_BACKEND=nrf`) actually
# reaches this inner one. Both used to be copy-pasted per app; this file is
# the one implementation, included by each app's CMakeLists.txt before its own
# find_package(Zephyr) call.

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
function(radiant_sysbuild_value var out)
  set(${out} "" PARENT_SCOPE)
  if(NOT SYSBUILD OR NOT DEFINED SYSBUILD_CACHE OR NOT EXISTS "${SYSBUILD_CACHE}")
    return()
  endif()

  file(STRINGS "${SYSBUILD_CACHE}" _lines ENCODING UTF-8)

  # The image's own name, as sysbuild knows it - e.g. "ant_dongle" - because it
  # is what prefixes an image-local setting.
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

# ── Which radiant_core HAL backend ───────────────────────────────────────────
#
# Appends radiant_core to ZEPHYR_EXTRA_MODULES and resolves RADIANT_BACKEND
# (null, nrf or cc26xx), writing the Kconfig fragment text that selects it
# into `conf_out` in the caller's scope. The caller decides what to do with
# that text - the dongle app embeds it inside a larger generated fragment
# alongside its ANT_RADIO choice; strap writes it standalone - because only
# the caller knows whether it has other backend axes (sdk_ant/core/stub) of
# its own.
#
# `default_backend` is a per-app default, and the asymmetry between apps is
# deliberate: `null` is right for the dongle's compile-check posture (see
# strap/CMakeLists.txt's original note, now here), `nrf` is right for a strap
# whose whole job is producing images somebody flashes. A null-radio strap
# boots, logs "transmitting", and puts nothing on the air - indistinguishable
# from a dead antenna, and it has cost this project a Feather flash and a
# whole session once already.
#
# RADIANT_BACKEND is forwarded from sysbuild the same way ANT_RADIO is, and
# for the same reason: `west build -- -DRADIANT_BACKEND=nrf` sets it on the
# SYSBUILD cache, not on this image's, so without this the flag is accepted
# and silently ignored - which is how a "verified" build of the nrf backend
# turns out to have compiled the null one.
function(radiant_select_backend module_dir default_backend conf_out)
  list(APPEND ZEPHYR_EXTRA_MODULES "${module_dir}")
  set(ZEPHYR_EXTRA_MODULES "${ZEPHYR_EXTRA_MODULES}" PARENT_SCOPE)

  radiant_sysbuild_value(RADIANT_BACKEND RADIANT_BACKEND_SYSBUILD)
  if(RADIANT_BACKEND_SYSBUILD)
    set(RADIANT_BACKEND "${RADIANT_BACKEND_SYSBUILD}" CACHE STRING
        "radiant_core HAL backend: null, nrf or cc26xx" FORCE)
  else()
    set(RADIANT_BACKEND "${default_backend}" CACHE STRING
        "radiant_core HAL backend: null (refuses every arm), nrf (the RADIO) or cc26xx (TI's RF core)")
  endif()
  set_property(CACHE RADIANT_BACKEND PROPERTY STRINGS null nrf cc26xx)

  if(RADIANT_BACKEND STREQUAL "null")
    set(_conf "CONFIG_RADIANT_CORE_BACKEND_NULL=y")
  elseif(RADIANT_BACKEND STREQUAL "nrf")
    set(_conf "CONFIG_RADIANT_CORE_BACKEND_NRF=y")
  elseif(RADIANT_BACKEND STREQUAL "cc26xx")
    set(_conf "CONFIG_RADIANT_CORE_BACKEND_CC26XX=y")
  else()
    message(FATAL_ERROR
      "radiant: RADIANT_BACKEND='${RADIANT_BACKEND}' is not a backend.\n"
      "  null    the lifecycle and the clock are real; every arm is refused.\n"
      "  nrf     the RADIO, one TIMER and four (D)PPI channels, owned\n"
      "          outright. Receives AND transmits - see\n"
      "          radiant_core/src/radiant_radio_nrf.c.\n"
      "  cc26xx  TI's RF core in proprietary mode, on CC13x2/CC26x2. Needs\n"
      "          the hal_ti module - see scripts/fetch_hal_ti.ps1.\n"
      "Pass one with -DRADIANT_BACKEND=<value>.")
  endif()
  message(STATUS "radiant: HAL backend ${RADIANT_BACKEND}")
  set(${conf_out} "${_conf}" PARENT_SCOPE)
endfunction()

# ── The backend actually compiled is the backend that was asked for ─────────
#
# radiant_select_backend()'s conf text is merged last, so it cannot lose to
# another fragment. It CAN still lose to Kconfig itself: the symbol has
# dependencies, and an unmet one makes the assignment a no-op that falls back
# to the choice default. Zephyr prints a warning about that among a hundred
# others and carries on.
#
# It is not hypothetical and it is not cheap. RADIANT_CORE_BACKEND_NRF used to
# depend on SOC_SERIES_NRF52X || SOC_SERIES_NRF54LX; Zephyr 4.4 renamed both, so
# every -DRADIANT_BACKEND=nrf build on NCS v3.4.0 silently compiled the null
# backend - an image that enumerates, answers the host, runs every state machine
# and finds no sensors, which looks exactly like a radio that is not working.
# The dependency is fixed; this is what makes the next one loud on the first
# build rather than after a bench session.
#
# WRITTEN AGAINST THE VALUE RATHER THAN AGAINST `nrf` SPECIFICALLY, and that
# generalisation is the point of it. The cc26xx arm has to depend on
# SOC_SERIES_CC13X2_CC26X2 - there is no SOC_COMPATIBLE_* symbol for any TI
# part, so the construct this guard was written to survive is the only one
# available there. A guard that covered one backend would have let the second
# one walk into the same trap.
#
# Call this AFTER find_package(Zephyr), once Kconfig has actually run.
# `label` names the app in the error message, so it points at what was built.
function(radiant_assert_backend label)
  string(TOUPPER "${RADIANT_BACKEND}" RADIANT_BACKEND_UPPER)
  if(NOT CONFIG_RADIANT_CORE_BACKEND_${RADIANT_BACKEND_UPPER})
    # Which one Kconfig did land on, so the message names the image that was
    # actually built rather than only the one that was not.
    set(RADIANT_BACKEND_GOT "none at all")
    foreach(_cand NULL NRF CC26XX NONE)
      if(CONFIG_RADIANT_CORE_BACKEND_${_cand})
        set(RADIANT_BACKEND_GOT "CONFIG_RADIANT_CORE_BACKEND_${_cand}")
      endif()
    endforeach()
    message(FATAL_ERROR
      "${label}: -DRADIANT_BACKEND=${RADIANT_BACKEND} was asked for and "
      "Kconfig did not select it.\n"
      "  CONFIG_RADIANT_CORE_BACKEND_${RADIANT_BACKEND_UPPER} is unset; "
      "Kconfig chose ${RADIANT_BACKEND_GOT} instead, and if that is the null "
      "backend then nothing would go on the air.\n"
      "  Almost always an unmet `depends on` in radiant_core/Kconfig for this "
      "board or this SDK. For nrf, check SOC_COMPATIBLE_NRF52X / "
      "SOC_COMPATIBLE_NRF54LX and !BT in this build's .config. For cc26xx, "
      "check SOC_SERIES_CC13X2_CC26X2 - that symbol has no SOC_COMPATIBLE_* "
      "equivalent and a rename of it is exactly the failure this guard "
      "exists for.")
  endif()
endfunction()
