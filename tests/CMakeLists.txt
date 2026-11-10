find_package(GTest QUIET)
if(NOT GTest_FOUND)
  include(FetchContent)
  FetchContent_Declare(googletest URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  add_library(GTest::gtest_main ALIAS gtest_main)
endif()
include(GoogleTest)

add_executable(lgrtk_tests
  version_test.cc
  framer_test.cc
  nmea_test.cc
  epoch_test.cc
  lg290p_test.cc
  serial_test.cc
  receiver_test.cc
  projection_test.cc
  ntrip_test.cc
  sim/fake_caster.cc
  sim/sim_lg290p.cc
)
target_include_directories(lgrtk_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lgrtk_tests PRIVATE util lgrtk::lgrtk GTest::gtest_main lgrtk_warnings)
if(LGRTK_HAVE_PROJ)
  target_link_libraries(lgrtk_tests PRIVATE lgrtk::projection)
endif()
target_compile_definitions(lgrtk_tests PRIVATE LGRTK_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
gtest_discover_tests(lgrtk_tests DISCOVERY_TIMEOUT 30)

if(LGRTK_BUILD_APPS)
  add_executable(sim_lg290p_tool sim/sim_tool.cc sim/sim_lg290p.cc)
  target_include_directories(sim_lg290p_tool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(sim_lg290p_tool PRIVATE util lgrtk::lgrtk)
  add_test(NAME CliTest.EndToEnd
           COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cli_test.sh $<TARGET_FILE:lgrtk_cli> $<TARGET_FILE:sim_lg290p_tool>)
  set_tests_properties(CliTest.EndToEnd PROPERTIES TIMEOUT 120)
  if(LGRTK_BUILD_DAEMON)
    add_test(NAME DaemonTest.EndToEnd
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/daemon_test.sh $<TARGET_FILE:lgrtkd>
                     $<TARGET_FILE:sim_lg290p_tool> $<TARGET_FILE:lgrtk_cli>)
    set_tests_properties(DaemonTest.EndToEnd PROPERTIES TIMEOUT 120)
  endif()
endif()

if(LGRTK_BUILD_DAEMON)
  add_executable(lgrtkd_tests
    daemon/config_test.cc
    daemon/gpsd_json_test.cc
    daemon/sky_test.cc
    daemon/gpsd_server_test.cc
    daemon/raw_tap_test.cc
    daemon/chrony_test.cc
    daemon/sd_notify_test.cc
    daemon/control_test.cc
  )
  target_include_directories(lgrtkd_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lgrtkd_tests PRIVATE util lgrtkd_core GTest::gtest_main lgrtk_warnings)
  target_compile_definitions(lgrtkd_tests PRIVATE LGRTK_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  gtest_discover_tests(lgrtkd_tests DISCOVERY_TIMEOUT 30)
endif()
