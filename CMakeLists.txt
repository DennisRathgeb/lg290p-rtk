cmake_minimum_required(VERSION 3.22)
project(lgrtk VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LGRTK_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
option(LGRTK_BUILD_APPS "Build the lgrtk CLI" ${PROJECT_IS_TOP_LEVEL})
option(LGRTK_BUILD_DAEMON "Build the lgrtkd daemon (needs toml++ and nlohmann/json, fetched if absent)" ${PROJECT_IS_TOP_LEVEL})
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
  src/ntrip.cc
)
add_library(lgrtk::lgrtk ALIAS lgrtk)
target_include_directories(lgrtk PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_include_directories(lgrtk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lgrtk PUBLIC Threads::Threads PRIVATE $<BUILD_INTERFACE:lgrtk_warnings>)

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
      PRIVATE $<IF:$<TARGET_EXISTS:PROJ::proj>,PROJ::proj,PkgConfig::PROJPC> $<BUILD_INTERFACE:lgrtk_warnings>)
    target_compile_definitions(lgrtk_projection PUBLIC LGRTK_HAVE_PROJ=1)
    message(STATUS "lgrtk: PROJ found, building lgrtk::projection")
  else()
    message(STATUS "lgrtk: PROJ not found, lgrtk::projection disabled")
  endif()
endif()

# ---- daemon: internal core library (not installed) + lgrtkd ----
if(LGRTK_BUILD_DAEMON)
  include(cmake/DaemonDeps.cmake)
  add_library(lgrtkd_core STATIC
    src/daemon/config.cc
    src/daemon/gpsd_json.cc
    src/daemon/sky.cc
    src/daemon/event_loop.cc
    src/daemon/gpsd_server.cc
    src/daemon/raw_tap.cc
    src/daemon/chrony.cc
    src/daemon/sd_notify.cc
    src/daemon/control.cc
    src/daemon/daemon.cc
  )
  target_include_directories(lgrtkd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(lgrtkd_core
    PUBLIC lgrtk nlohmann_json::nlohmann_json
    PRIVATE tomlplusplus::tomlplusplus $<BUILD_INTERFACE:lgrtk_warnings>)
  add_executable(lgrtkd apps/lgrtkd/main.cc)
  target_link_libraries(lgrtkd PRIVATE lgrtkd_core lgrtk_warnings)
endif()

if(LGRTK_BUILD_APPS)
  add_executable(lgrtk_cli apps/lgrtk/main.cc)
  set_target_properties(lgrtk_cli PROPERTIES OUTPUT_NAME lgrtk)
  target_link_libraries(lgrtk_cli PRIVATE lgrtk lgrtk_warnings)
  if(LGRTK_HAVE_PROJ)
    target_link_libraries(lgrtk_cli PRIVATE lgrtk::projection)
  endif()
  if(LGRTK_BUILD_DAEMON)
    target_link_libraries(lgrtk_cli PRIVATE lgrtkd_core)
    target_compile_definitions(lgrtk_cli PRIVATE LGRTK_HAVE_CTL=1)
  endif()
endif()

if(LGRTK_BUILD_EXAMPLES)
  add_executable(example_print_fixes examples/print_fixes.cc)
  target_link_libraries(example_print_fixes PRIVATE lgrtk lgrtk_warnings)
  if(LGRTK_HAVE_PROJ)
    add_executable(example_ntrip_rover examples/ntrip_rover.cc)
    target_link_libraries(example_ntrip_rover PRIVATE lgrtk::projection lgrtk_warnings)
  endif()
endif()

# ---- install: library, headers, CMake package, pkg-config, CLI ----
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
set_target_properties(lgrtk PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
set(_lgrtk_targets lgrtk)
if(LGRTK_HAVE_PROJ)
  set_target_properties(lgrtk_projection PROPERTIES EXPORT_NAME projection
    VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
  list(APPEND _lgrtk_targets lgrtk_projection)
endif()
install(TARGETS ${_lgrtk_targets} EXPORT lgrtkTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/lgrtk DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT lgrtkTargets NAMESPACE lgrtk:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lgrtk)
configure_package_config_file(cmake/lgrtkConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/lgrtkConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lgrtk)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/lgrtkConfigVersion.cmake
  COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/lgrtkConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/lgrtkConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lgrtk)
configure_file(cmake/lgrtk.pc.in ${CMAKE_CURRENT_BINARY_DIR}/lgrtk.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/lgrtk.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
if(LGRTK_BUILD_APPS)
  install(TARGETS lgrtk_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---- daemon and its system files ----
if(LGRTK_BUILD_DAEMON)
  # Group owning serial devices: uucp on Arch, dialout on Debian/Ubuntu.
  if(EXISTS /etc/arch-release)
    set(_lgrtk_group uucp)
  else()
    set(_lgrtk_group dialout)
  endif()
  set(LGRTK_SERIAL_GROUP ${_lgrtk_group} CACHE STRING "Group that owns serial devices")
  set(LGRTK_SYSCONFDIR ${CMAKE_INSTALL_FULL_SYSCONFDIR} CACHE PATH "Where lgrtk/lgrtkd.toml lives")
  set(LGRTK_SYSTEMD_UNITDIR lib/systemd/system CACHE PATH "systemd unit directory (relative to the prefix)")
  set(LGRTK_UDEV_RULESDIR lib/udev/rules.d CACHE PATH "udev rules directory (relative to the prefix)")
  set(LGRTK_SYSUSERSDIR lib/sysusers.d CACHE PATH "sysusers.d directory (relative to the prefix)")
  configure_file(packaging/lgrtkd.service.in ${CMAKE_CURRENT_BINARY_DIR}/lgrtkd.service @ONLY)
  configure_file(packaging/lgrtk.sysusers.in ${CMAKE_CURRENT_BINARY_DIR}/lgrtk.conf @ONLY)
  install(TARGETS lgrtkd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/lgrtkd.service DESTINATION ${LGRTK_SYSTEMD_UNITDIR})
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/lgrtk.conf DESTINATION ${LGRTK_SYSUSERSDIR})
  install(FILES packaging/99-lg290p.rules DESTINATION ${LGRTK_UDEV_RULESDIR})
  install(FILES packaging/lgrtkd.toml DESTINATION ${CMAKE_INSTALL_DATADIR}/lgrtk)
endif()

if(LGRTK_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
