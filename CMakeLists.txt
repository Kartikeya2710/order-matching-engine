cmake_minimum_required(VERSION 3.20)
project(OrderMatchingEngine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(COMMON_FLAGS
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -O2 -march=native
)

file(GLOB ENGINE_SOURCES "src/*.cpp")
list(REMOVE_ITEM ENGINE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

add_library(engine_lib STATIC ${ENGINE_SOURCES})
target_include_directories(engine_lib PUBLIC include)
target_compile_options(engine_lib PRIVATE ${COMMON_FLAGS})

add_executable(app src/main.cpp)
target_include_directories(app PRIVATE include)
target_link_libraries(app PRIVATE engine_lib)
target_compile_options(app PRIVATE ${COMMON_FLAGS})

add_executable(tests tests/TestMain.cpp)
target_include_directories(tests PRIVATE include tests)
target_link_libraries(tests PRIVATE engine_lib)
target_compile_options(tests PRIVATE ${COMMON_FLAGS})

enable_testing()
add_test(NAME AllTests COMMAND tests)