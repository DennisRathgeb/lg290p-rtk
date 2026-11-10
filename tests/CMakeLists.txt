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
)
target_link_libraries(lgrtk_tests PRIVATE lgrtk::lgrtk GTest::gtest_main lgrtk_warnings)
target_compile_definitions(lgrtk_tests PRIVATE LGRTK_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
gtest_discover_tests(lgrtk_tests DISCOVERY_TIMEOUT 30)
