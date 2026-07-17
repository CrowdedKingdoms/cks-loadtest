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
)
target_include_directories(loadtest_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cxxopts
)
target_link_libraries(loadtest_core PUBLIC
  CURL::libcurl
  OpenSSL::Crypto
  Threads::Threads
)

add_executable(cks-loadtest src/main.cpp)
target_link_libraries(cks-loadtest PRIVATE loadtest_core)

enable_testing()
add_executable(wire_test tests/wire_test.cpp)
target_link_libraries(wire_test PRIVATE loadtest_core)
add_test(NAME wire_test COMMAND wire_test)

install(TARGETS cks-loadtest RUNTIME DESTINATION bin)
