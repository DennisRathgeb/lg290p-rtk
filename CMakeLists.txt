cmake_minimum_required(VERSION 3.22)
project(lgrtk VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LGRTK_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
option(LGRTK_BUILD_APPS "Build the lgrtk CLI" ${PROJECT_IS_TOP_LEVEL})
option(LGRTK_BUILD_EXAMPLES "Build the examples" ${PROJECT_IS_TOP_LEVEL})
option(LGRTK_WITH_PROJ "Build lgrtk::projection (needs PROJ)" ON)
option(LGRTK_WERROR "Treat warnings as errors" OFF)

include(cmake/Warnings.cmake)
find_package(Threads REQUIRED)

add_library(lgrtk
  src/version.cc
  src/framer.cc
)
add_library(lgrtk::lgrtk ALIAS lgrtk)
target_include_directories(lgrtk PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(lgrtk PUBLIC Threads::Threads PRIVATE lgrtk_warnings)

if(LGRTK_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
