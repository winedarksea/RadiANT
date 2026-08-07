# Partition Manager arrives with the sdk-ant nRF Connect SDK module and decides
# where the application is linked, overriding zephyr,code-partition. Left alone
# it gives `app` the whole 1 MB at 0x0 while the output file still carries the
# board's real load offset, so the image is linked for one address and written
# to another and boots into nothing. The static maps restate each board's own
# layout in the form PM reads; see the individual files for the whole story.
#
# Every board that boots under a bootloader needs an entry here. A board with
# no entry is not "unsupported" - it builds, flashes, and fails in the way
# described above, which reads as a hardware fault.
#
# PM_STATIC_YML_FILE is read in sysbuild scope, which is why this lives here
# rather than in the application's CMakeLists.txt. Sysbuild include()s this
# file from inside a function, so a plain set() would be scoped away before
# partition_manager() ever looks - hence the cache entry.
if(NOT DEFINED PM_STATIC_YML_FILE)
  # BOARD may or may not carry its qualifiers depending on how it was spelled
  # on the command line, so match against both.
  set(_board_spec "${BOARD}${BOARD_QUALIFIERS}")

  if(_board_spec MATCHES "adafruit_feather_nrf52840")
    set(_pm_static pm_static_adafruit_feather_nrf52840.yml)
  elseif(_board_spec MATCHES "nrf52840dongle" AND NOT _board_spec MATCHES "bare")
    # The /bare variant has no bootloader and wants the whole flash from 0x0,
    # which is what PM does unprompted - so leave it alone.
    set(_pm_static pm_static_nrf52840dongle_nrf52840.yml)
  endif()

  if(DEFINED _pm_static)
    set(PM_STATIC_YML_FILE ${CMAKE_CURRENT_LIST_DIR}/${_pm_static}
        CACHE INTERNAL "Static partition map, see the file itself for why")
  endif()
endif()
