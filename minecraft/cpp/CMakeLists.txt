cmake_minimum_required(VERSION 3.16)
project(ck-mc-load CXX)

# With no build type CMake passes no -O flag, and an unoptimized worker spends several
# times as long on each tick. Optimize unless the caller says otherwise.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra -Wpedantic)

enable_testing()

add_library(ck_mc STATIC src/bot.cpp src/fleet.cpp src/logic.cpp src/journal.cpp src/control.cpp)
target_include_directories(ck_mc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ck_mc PUBLIC OpenSSL::Crypto ZLIB::ZLIB Threads::Threads)

add_executable(ck-mc-load src/main.cpp)
target_link_libraries(ck-mc-load PRIVATE ck_mc)

add_executable(protocol_test tests/protocol_test.cpp)
target_link_libraries(protocol_test PRIVATE ck_mc)
add_test(NAME protocol_test COMMAND protocol_test)

# Starts ck-mc-load with a control socket and no game server. Python 3, standard library only.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME control_smoke
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/control_smoke.py $<TARGET_FILE:ck-mc-load>)
endif()
