# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(App)

set(APP_DIR         "${CMAKE_CURRENT_SOURCE_DIR}")   # F:/REPOS/tourmaline/app

message(STATUS "APP_DIR = ${APP_DIR}")

# ---------------------------------------------------------------------------
# App.exe: the pak-viewer HOST — the PLAIN viewer (no game layer; the
# sponza invaders game lives in the top-level asteroids/ package). The
# engine renders worlds; this target owns everything app-specific — CLI
# parsing, pak preload + world source (PakApp.cpp / PakAppArgs.cpp /
# PakPreload.cpp; see src/Host.h). Content/shader staging lives in the
# shared cmake/HostSetup.cmake (see the root CMakeLists include).
# (appcore, the old WorldRenderer static lib, is long deleted;
# DecalAssets.cpp went with the demo-decal seeding — decals resolve from
# world content only now.)
# ---------------------------------------------------------------------------
add_executable(App
    src/main.cpp
    src/PakApp.cpp
    src/PakAppArgs.cpp
    src/PakPreload.cpp
)

find_package(Vulkan REQUIRED)
message(STATUS "Vulkan_FOUND: ${Vulkan_FOUND}")
message(STATUS "Vulkan_INCLUDE_DIRS: ${Vulkan_INCLUDE_DIRS}")
message(STATUS "Vulkan_LIBRARIES: ${Vulkan_LIBRARIES}")

tourmaline_host_setup(App)
