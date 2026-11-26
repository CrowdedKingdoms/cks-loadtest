cmake_minimum_required(VERSION 3.16)
project(cks-loadtest CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra)

add_library(loadtest_core STATIC
  src/Config.cpp
  src/GraphQLClient.cpp
  src/Provisioner.cpp
  src/Stats.cpp
  src/Worker.cpp
  src/Harness.cpp
  src/ControlServer.cpp
)
target_include_directories(loadtest_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cxxopts
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib
)
target_link_libraries(loadtest_core PUBLIC
  CURL::libcurl
  OpenSSL::Crypto
  Threads::Threads
)

add_executable(cks-loadtest src/main.cpp)
target_link_libraries(cks-loadtest PRIVATE loadtest_core)

add_executable(cks-loadtest-ctl src/ctl.cpp)
target_link_libraries(cks-loadtest-ctl PRIVATE loadtest_core)

# ck-fleet-load: the fleet session load client (docs/FLEET-LOAD.md). It reuses
# the harness's wire, HMAC, GraphQL and Provisioner code from loadtest_core.
add_library(fleet_load_core STATIC
  src/fleet/FleetStats.cpp
  src/fleet/FleetConfig.cpp
  src/fleet/FleetApi.cpp
  src/fleet/Roster.cpp
  src/fleet/FleetWorker.cpp
  src/fleet/FakeRealtime.cpp
  src/fleet/FleetRun.cpp
  src/fleet/DryRun.cpp
  src/fleet/Modes.cpp
  src/fleet/RulesCheck.cpp
  src/fleet/SelfTest.cpp
)
target_link_libraries(fleet_load_core PUBLIC loadtest_core)

add_executable(ck-fleet-load src/fleet/main.cpp)
target_link_libraries(ck-fleet-load PRIVATE fleet_load_core)

enable_testing()
add_executable(wire_test tests/wire_test.cpp)
target_link_libraries(wire_test PRIVATE loadtest_core)
add_test(NAME wire_test COMMAND wire_test)

add_executable(config_test tests/config_test.cpp)
target_link_libraries(config_test PRIVATE loadtest_core)
add_test(NAME config_test COMMAND config_test)

add_executable(stats_test tests/stats_test.cpp)
target_link_libraries(stats_test PRIVATE loadtest_core)
add_test(NAME stats_test COMMAND stats_test)

add_executable(harness_rung_test tests/harness_rung_test.cpp)
target_link_libraries(harness_rung_test PRIVATE loadtest_core)
add_test(NAME harness_rung_test COMMAND harness_rung_test)

add_executable(worker_add_test tests/worker_add_test.cpp)
target_link_libraries(worker_add_test PRIVATE loadtest_core)
add_test(NAME worker_add_test COMMAND worker_add_test)

add_executable(pose_profile_test tests/pose_profile_test.cpp)
target_link_libraries(pose_profile_test PRIVATE loadtest_core)
add_test(NAME pose_profile_test COMMAND pose_profile_test)

add_executable(control_http_test tests/control_http_test.cpp)
target_link_libraries(control_http_test PRIVATE loadtest_core)
add_test(NAME control_http_test COMMAND control_http_test)

# The fleet load client's tests. The last four run the binary itself: its
# built-in checks, a dry run of the load and of the rule checks (no network),
# and a live run refused for want of the org token.
foreach(fleet_test fleet_wire_test fleet_world_test fleet_api_test fleet_run_test)
  add_executable(${fleet_test} tests/${fleet_test}.cpp)
  target_link_libraries(${fleet_test} PRIVATE fleet_load_core)
  add_test(NAME ${fleet_test} COMMAND ${fleet_test})
endforeach()
add_test(NAME fleet_self_test COMMAND ck-fleet-load --self-test)
add_test(NAME fleet_dry_run
         COMMAND ck-fleet-load --dry-run --servers 16 --players 320 --cross-rate 40 --dry-run-s 3)
add_test(NAME fleet_rules_dry_run COMMAND ck-fleet-load --rules-check --dry-run)
add_test(NAME fleet_needs_org_token
         COMMAND ck-fleet-load --api https://api.example.invalid/graphql --app 1 --servers 1 --players 1)
set_tests_properties(fleet_needs_org_token PROPERTIES
  ENVIRONMENT "CK_ORG_TOKEN="
  WILL_FAIL TRUE)

install(TARGETS cks-loadtest cks-loadtest-ctl ck-fleet-load RUNTIME DESTINATION bin)

add_subdirectory(minecraft/cpp)
