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

add_executable(worker_add_test tests/worker_add_test.cpp)
target_link_libraries(worker_add_test PRIVATE loadtest_core)
add_test(NAME worker_add_test COMMAND worker_add_test)

add_executable(pose_profile_test tests/pose_profile_test.cpp)
target_link_libraries(pose_profile_test PRIVATE loadtest_core)
add_test(NAME pose_profile_test COMMAND pose_profile_test)

add_executable(control_http_test tests/control_http_test.cpp)
target_link_libraries(control_http_test PRIVATE loadtest_core)
add_test(NAME control_http_test COMMAND control_http_test)

install(TARGETS cks-loadtest cks-loadtest-ctl RUNTIME DESTINATION bin)
