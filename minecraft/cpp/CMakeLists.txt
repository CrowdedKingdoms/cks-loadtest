cmake_minimum_required(VERSION 3.16)
project(ck-mc-load CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra -Wpedantic)

enable_testing()

add_library(ck_mc STATIC src/bot.cpp src/fleet.cpp)
target_include_directories(ck_mc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ck_mc PUBLIC OpenSSL::Crypto ZLIB::ZLIB Threads::Threads)

add_executable(ck-mc-load src/main.cpp)
target_link_libraries(ck-mc-load PRIVATE ck_mc)

add_executable(protocol_test tests/protocol_test.cpp)
target_link_libraries(protocol_test PRIVATE ck_mc)
add_test(NAME protocol_test COMMAND protocol_test)
