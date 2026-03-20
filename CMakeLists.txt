cmake_minimum_required(VERSION 3.20)
project(OrderMatchingEngine CXX)

# C++20 is required — coroutines, std::variant, concepts
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Collect all .cpp files in src/
file(GLOB_RECURSE SOURCES "src/*.cpp")

add_executable(app ${SOURCES})

# Tell the compiler where your headers live
target_include_directories(app PRIVATE include)

# Compiler flags
target_compile_options(app PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -O2
    -march=native
)

# Export compile commands — this is what VS Code's IntelliSense reads
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)