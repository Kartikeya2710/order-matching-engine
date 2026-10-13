cmake_minimum_required(VERSION 3.21)
project(OrderMatchingEngine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to Debug if nobody chose a build type
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(Threads REQUIRED)

set(COMMON_FLAGS -Wall -Wextra -Wpedantic -Wno-unused-parameter)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND COMMON_FLAGS -Wno-psabi)
endif()

list(APPEND COMMON_FLAGS "$<$<CONFIG:Release>:-O2>")
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
    list(APPEND COMMON_FLAGS "$<$<CONFIG:Release>:-mcpu=native>")
else()
    list(APPEND COMMON_FLAGS "$<$<CONFIG:Release>:-march=native>")
endif()

file(GLOB ENGINE_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
list(REMOVE_ITEM ENGINE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/sim_runner.cpp")

add_library(engine_lib STATIC ${ENGINE_SOURCES})
target_include_directories(engine_lib PUBLIC include)
target_compile_options(engine_lib PRIVATE ${COMMON_FLAGS})
target_link_libraries(engine_lib PUBLIC Threads::Threads)

add_executable(sim_runner src/sim_runner.cpp)
target_link_libraries(sim_runner PRIVATE engine_lib)
target_compile_options(sim_runner PRIVATE ${COMMON_FLAGS})

enable_testing()

# GoogleTest is downloaded at configure time and built with the project
include(FetchContent)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.18.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

# Every tests/*Test.cpp is part of the test binary
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "tests/*Test.cpp")

add_executable(tests ${TEST_SOURCES})
target_link_libraries(tests PRIVATE engine_lib GTest::gtest_main)
target_compile_options(tests PRIVATE ${COMMON_FLAGS})

# Registers each TEST as its own CTest entry
gtest_discover_tests(tests)