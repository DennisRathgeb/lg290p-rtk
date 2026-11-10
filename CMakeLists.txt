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
  src/nmea.cc
  src/epoch.cc
  src/lg290p.cc
  src/serial.cc
  src/receiver.cc
  src/local_frame.cc
)
add_library(lgrtk::lgrtk ALIAS lgrtk)
target_include_directories(lgrtk PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(lgrtk PUBLIC Threads::Threads PRIVATE lgrtk_warnings)

# Optional PROJ-based projection component.
set(LGRTK_HAVE_PROJ OFF)
if(LGRTK_WITH_PROJ)
  find_package(PROJ CONFIG QUIET)
  if(NOT PROJ_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(PROJPC QUIET IMPORTED_TARGET proj)
    endif()
  endif()
  if(PROJ_FOUND OR PROJPC_FOUND)
    set(LGRTK_HAVE_PROJ ON)
    add_library(lgrtk_projection src/projection.cc)
    add_library(lgrtk::projection ALIAS lgrtk_projection)
    target_link_libraries(lgrtk_projection PUBLIC lgrtk
      PRIVATE $<IF:$<TARGET_EXISTS:PROJ::proj>,PROJ::proj,PkgConfig::PROJPC> lgrtk_warnings)
    target_compile_definitions(lgrtk_projection PUBLIC LGRTK_HAVE_PROJ=1)
    message(STATUS "lgrtk: PROJ found, building lgrtk::projection")
  else()
    message(STATUS "lgrtk: PROJ not found, lgrtk::projection disabled")
  endif()
endif()

if(LGRTK_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
