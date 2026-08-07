# Partition Manager arrives with the sdk-ant nRF Connect SDK module and decides
# where the application is linked, overriding zephyr,code-partition. On the
# Feather that silently produces an image linked at 0x0 and written to 0x26000.
# The static map restates the board's own layout in the form PM reads; see
# pm_static_adafruit_feather_nrf52840.yml for the whole story.
#
# PM_STATIC_YML_FILE is read in sysbuild scope, which is why this lives here
# rather than in the application's CMakeLists.txt. Sysbuild include()s this
# file from inside a function, so a plain set() would be scoped away before
# partition_manager() ever looks - hence the cache entry.
if(BOARD MATCHES "adafruit_feather_nrf52840" AND NOT DEFINED PM_STATIC_YML_FILE)
  set(PM_STATIC_YML_FILE
      ${CMAKE_CURRENT_LIST_DIR}/pm_static_adafruit_feather_nrf52840.yml
      CACHE INTERNAL "Static partition map, see the file itself for why")
endif()
