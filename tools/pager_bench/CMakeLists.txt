# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
# Standalone synthetic hardware benchmarks, no renderer/engine dependency.
add_executable(PagerBench main.cpp)
target_link_libraries(PagerBench PRIVATE Vulkan::Vulkan)
target_compile_features(PagerBench PRIVATE cxx_std_20)
set_target_properties(PagerBench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
