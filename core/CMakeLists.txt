# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

# Core owns the allocator implementation; Engine depends on Core.
add_library(tourmaline_core STATIC
    src/Memory/arena.cpp
    src/Memory/linear_allocator.cpp
)
target_compile_features(tourmaline_core PUBLIC cxx_std_20)
target_include_directories(tourmaline_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
