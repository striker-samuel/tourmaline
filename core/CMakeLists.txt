# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

# Shared, backend-agnostic memory API used by the engine, RHI, and hosts.
# Implementations currently link through engine until GPU flight-slot
# lifetime policy is separated from the allocator substrate.
add_library(tourmaline_core INTERFACE)
target_include_directories(tourmaline_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
