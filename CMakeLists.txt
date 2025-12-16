cmake_minimum_required(VERSION 4.2)

project(order-matching-engine
    VERSION 1.0
    LANGUAGES CXX
)

add_executable(app
    src/main.cpp
)

# Enforce C++20 (hard requirement)
target_compile_features(app PUBLIC cxx_std_20)

# Enforce ISO C++ (no GNU / Apple extensions)
set_target_properties(app PROPERTIES
    CXX_EXTENSIONS OFF
)

target_compile_options(app PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

