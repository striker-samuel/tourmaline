# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(App)

set(APP_DIR         "${CMAKE_CURRENT_SOURCE_DIR}")   # F:/REPOS/tourmaline/app

message(STATUS "APP_DIR = ${APP_DIR}")

# ---------------------------------------------------------------------------
# tourmaline_apphost: the pak-viewer HOST as a LIBRARY — the plain
# viewer's own layer (CLI parsing, pak preload/path resolution, world
# loading + scene prep, the viewer host wiring/lifecycle; see
# src/Host.h). App.exe is its thin main; PROJECT EXES (the content-as-
# code sample, samples/04_project_game — and any game built by
# tourmaline_attach_project) link the same host and drive it with their
# own PakAppConfig (mount the project's paks, load the entry scene by
# identity from the generated project.h).
# The engine renders worlds; this library owns everything app-specific.
# Content/shader staging lives in the shared cmake/HostSetup.cmake (see
# the root CMakeLists include).
# ---------------------------------------------------------------------------
add_library(tourmaline_apphost STATIC
    src/PakApp.cpp
    src/PakAppArgs.cpp
    src/PakPreload.cpp
    src/PakWorldLoader.cpp
    src/ProjectRuntime.cpp
    src/ProjectScenes.cpp
    src/AudioRuntime.cpp
    src/AudioGraphs.cpp
    src/AssetManager.cpp
    src/DeployCheck.cpp
    # THE APPLICATION-FACING RUNTIME (app::RunGame — Runtime.h): the
    # game-host convenience adapter (the context, the game element,
    # the packaged-default entry + the FATAL compiled-catalog gate).
    # App.exe stays its thin viewer main; PROJECT EXES drive THIS.
    src/AssetStore.cpp
    src/Runtime.cpp
)

target_include_directories(tourmaline_apphost
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(tourmaline_apphost
    PUBLIC
        engine
        gltf_import
    PRIVATE
        # DeployCheck's content-address digest (the pak name hash):
        # HashBytes. PRIVATE — the gate is host-internal.
        artifact_cache
)

# ---------------------------------------------------------------------------
# App.exe: the plain viewer main over the host library.
# ---------------------------------------------------------------------------
add_executable(App
    src/main.cpp
)

find_package(Vulkan REQUIRED)
message(STATUS "Vulkan_FOUND: ${Vulkan_FOUND}")
message(STATUS "Vulkan_INCLUDE_DIRS: ${Vulkan_INCLUDE_DIRS}")
message(STATUS "Vulkan_LIBRARIES: ${Vulkan_LIBRARIES}")

target_link_libraries(App
    PRIVATE
        tourmaline_apphost
)

tourmaline_host_setup(App)

# Dedicated profiling target: the same cooked-pak host, but no game
# sample/audio element. It requires a bounded --bench invocation so agents
# never accidentally launch an unbounded GUI workload.
add_executable(Benchmark src/benchmark_main.cpp)
target_link_libraries(Benchmark PRIVATE tourmaline_apphost)
tourmaline_host_setup(Benchmark)

# Convenience launcher for saved Nsight GPU Trace scenarios. It is a tiny
# Windows-only exe that invokes the existing stdlib-only Python harness,
# so we don't duplicate scene validation or link any GPU profiler SDK.
# Strictly excluded from shipping build graphs.
if(WIN32 AND NOT TOURMALINE_BUILD_PROFILE STREQUAL "shipping")
    add_executable(NsightTrace "${TOURMALINE_SOURCE_DIR}/tools/nsight_trace_cli.cpp")
    target_compile_features(NsightTrace PRIVATE cxx_std_20)
    set_target_properties(NsightTrace PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    add_dependencies(NsightTrace Benchmark)
endif()
