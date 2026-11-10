# Header-only dependencies of the daemon: system packages first, else fetched.
# The library (lgrtk::lgrtk) never links them.
include(FetchContent)

# Marks a fetched target's headers as system headers so -Werror ignores them.
function(_lgrtk_system_headers target)
  get_target_property(_inc ${target} INTERFACE_INCLUDE_DIRECTORIES)
  if(_inc)
    set_target_properties(${target} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_inc}")
  endif()
endfunction()

find_package(nlohmann_json 3.10 QUIET)
if(NOT nlohmann_json_FOUND)
  message(STATUS "lgrtk: fetching nlohmann/json 3.11.3")
  FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
  FetchContent_MakeAvailable(nlohmann_json)
  _lgrtk_system_headers(nlohmann_json)
endif()

find_package(tomlplusplus 3 QUIET)
if(NOT tomlplusplus_FOUND)
  message(STATUS "lgrtk: fetching toml++ 3.4.0")
  FetchContent_Declare(tomlplusplus
    URL https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
    URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155)
  FetchContent_MakeAvailable(tomlplusplus)
  _lgrtk_system_headers(tomlplusplus_tomlplusplus)
endif()
