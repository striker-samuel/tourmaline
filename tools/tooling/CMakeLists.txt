# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

# Public, linkable C++20 tooling core: no CLI processes at API call sites.
add_library(tourmaline_tooling STATIC
    src/project_init.cpp
    src/tooling.cpp
)
add_library(tourmaline::tooling ALIAS tourmaline_tooling)

target_include_directories(tourmaline_tooling PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(tourmaline_tooling PUBLIC cxx_std_20)
target_link_libraries(tourmaline_tooling PUBLIC
    project_pipeline
    engine
)
