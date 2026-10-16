# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.17)
# Build hygiene (see docs/project_guide.md): this repo is Ninja-only and
# out-of-source. In-source configures pollute the tree (CMakeCache.txt,
# *.vcxproj, Testing/, Debug/ next to the sources) and the Visual Studio
# generator breaks the Ninja-configured out/build/<config> dirs.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are forbidden: configure out-of-source "
    "with Ninja, e.g. cmake -B out/build/x64-Release -G Ninja "
    "-DCMAKE_BUILD_TYPE=Release . (If you already configured in-source, "
    "delete CMakeCache.txt + CMakeFiles/ + Testing/ from the source root.)")
endif()
if(CMAKE_GENERATOR MATCHES "Visual Studio")
  message(FATAL_ERROR "The Visual Studio generator is unsupported: use Ninja "
    "(-G Ninja). See AGENTS.md.")
endif()
project(Tourmaline LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Vulkan#
#--- Rendering backend ------------------------------------------------------

set(ENGINE_BACKEND "Vulkan" CACHE STRING "Rendering backend")
find_package(Vulkan REQUIRED)

include(FetchContent)

# MeshOptimizer (pinned git submodule)
add_subdirectory(external/meshoptimizer)

# xatlas (vendored chart + pack core for lightmap UV2 atlases)
add_subdirectory(external/xatlas)

# DirectXTex BC codecs (pinned git submodule; only the block
# codec translation units compile — the sglm BC6H payload uses
# D3DXEncodeBC6HU/D3DXDecodeBC6HU, cook + load-side only)
add_library(directxtex_bc STATIC
	external/DirectXTex/DirectXTex/BC.cpp
	external/DirectXTex/DirectXTex/BC4BC5.cpp
	external/DirectXTex/DirectXTex/BC6HBC7.cpp
)
target_include_directories(directxtex_bc PUBLIC external/DirectXTex)
target_compile_features(directxtex_bc PRIVATE cxx_std_20)
# Third-party TU: silence everything (the upstream source is unmodified).
if(MSVC)
	target_compile_options(directxtex_bc PRIVATE /W0)
else()
	target_compile_options(directxtex_bc PRIVATE -w)
endif()

# LZ4 (vendored block codec for cooked pak blobs)
add_subdirectory(external/lz4)

# Embree ray-tracing backend for the bake transport (vendored
# external/embree source — upstream master snapshot, 2026-10-01 era
# (still versioned 4.4.1), trimmed: no docs/tutorials/tests/superbuild).
# REQUIRED (not optional): the engine links it unconditionally and gates the
# Embree traversal path on SMVK_GI_EMBREE. Static lib, no tutorials,
# internal tasking (no TBB), triangle geometry (the bake only traces
# triangles — the rest of Embree's geometry types stay off to keep the
# build lean).
set(EMBREE_STATIC_LIB ON CACHE BOOL "" FORCE)
set(EMBREE_TUTORIALS OFF CACHE BOOL "" FORCE)
set(EMBREE_TASKING_SYSTEM "INTERNAL" CACHE STRING "" FORCE)
set(EMBREE_GEOMETRY_CURVE OFF CACHE BOOL "" FORCE)
set(EMBREE_GEOMETRY_SUBDIVISION OFF CACHE BOOL "" FORCE)
set(EMBREE_GEOMETRY_USER OFF CACHE BOOL "" FORCE)
set(EMBREE_GEOMETRY_INSTANCE OFF CACHE BOOL "" FORCE)
set(EMBREE_GEOMETRY_INSTANCE_ARRAY OFF CACHE BOOL "" FORCE)
set(EMBREE_RAY_MASK OFF CACHE BOOL "" FORCE)
set(EMBREE_FILTER_FUNCTION OFF CACHE BOOL "" FORCE)
add_subdirectory(external/embree)

# GLFW (legacy windowing; retained for samples compat — engine uses SDL3)
add_subdirectory(external/glfw)

# SDL3 (vendored prebuilt dev package: include/ + lib/<arch>/ + cmake/)
# Provides windowing, input (keyboard/mouse/gamepad) and audio.
list(PREPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/external/SDL3-3.4.16")
set(SDL3_DIR "${CMAKE_SOURCE_DIR}/external/SDL3-3.4.16/cmake" CACHE PATH "SDL3 config dir" FORCE)
find_package(SDL3 CONFIG REQUIRED)

# Stage SDL3.dll next to an executable (engine links SDL3 PUBLIC, so every
# consumer exe carries the import; Windows loads the DLL from the exe dir).
function(tourmaline_stage_sdl3 TARGET)
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL3::SDL3>"
            "$<TARGET_FILE_DIR:${TARGET}>"
        COMMENT "Staging SDL3.dll next to ${TARGET}"
    )
endfunction()

# Stage the NVIDIA Streamline runtime next to an executable (DLSS-SR).
# sl.interposer.dll is loaded dynamically from the exe dir (dlss.cpp);
# a missing set simply leaves DLSS unavailable - no hard dependency.
# Source of the binaries: external/streamline (Streamline SDK 2.14.1).
function(tourmaline_stage_streamline TARGET)
    set(_SL_BIN_DIR "${CMAKE_SOURCE_DIR}/external/streamline/bin/x64")
    foreach(_SL_DLL IN ITEMS
            sl.interposer.dll sl.common.dll sl.dlss.dll nvngx_dlss.dll
            NvLowLatencyVk.dll)
        if(EXISTS "${_SL_BIN_DIR}/${_SL_DLL}")
            add_custom_command(TARGET ${TARGET} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_SL_BIN_DIR}/${_SL_DLL}"
                    "$<TARGET_FILE_DIR:${TARGET}>/${_SL_DLL}"
                COMMENT "Staging ${_SL_DLL} next to ${TARGET}"
            )
        else()
            message(WARNING "Streamline binary '${_SL_DLL}' not found in ${_SL_BIN_DIR} - DLSS will be unavailable at runtime")
        endif()
    endforeach()
    if(EXISTS "${_SL_BIN_DIR}/nvngx_dlss.license.txt")
        add_custom_command(TARGET ${TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_SL_BIN_DIR}/nvngx_dlss.license.txt"
                "$<TARGET_FILE_DIR:${TARGET}>/nvngx_dlss.license.txt"
            COMMENT "Staging nvngx_dlss.license.txt next to ${TARGET}"
        )
    endif()
endfunction()

# GLM (header-only, pinned git submodule)
add_subdirectory(external/glm)

# --- SQLite (vendored SQLiteCpp + sqlite3 amalgamation) ----------------------
# Backs the editor's artifact cache (registry). Plain static
# libs: sqlite3.c compiled once, the SQLiteCpp RAII wrapper on top. No
# shell/CLI pieces from the upstream project are built.
add_library(sqlite3_amalgamation STATIC external/SQLiteCpp/sqlite3/sqlite3.c)
target_include_directories(sqlite3_amalgamation
    PUBLIC external/SQLiteCpp/sqlite3)
# SQLiteCpp references the extension loader: the OMIT cuts break it.
target_compile_definitions(sqlite3_amalgamation PRIVATE
    SQLITE_ENABLE_COLUMN_METADATA)
if(MSVC)
    # Amalgamation is clean; keep the warning noise down.
    target_compile_options(sqlite3_amalgamation PRIVATE /W0)
endif()

add_library(SQLiteCpp STATIC
    external/SQLiteCpp/src/Backup.cpp
    external/SQLiteCpp/src/Column.cpp
    external/SQLiteCpp/src/Database.cpp
    external/SQLiteCpp/src/Exception.cpp
    external/SQLiteCpp/src/Savepoint.cpp
    external/SQLiteCpp/src/Statement.cpp
    external/SQLiteCpp/src/Transaction.cpp
)
target_include_directories(SQLiteCpp
    PUBLIC
        ${CMAKE_SOURCE_DIR}/external/SQLiteCpp/include
)
target_link_libraries(SQLiteCpp PUBLIC sqlite3_amalgamation)
if(MSVC)
    target_compile_options(SQLiteCpp PRIVATE /W3)
endif()


# --- KTX hinting --------------------------------------------------------------
# Users can point CMake at KTX in several ways:
#  1) -Dktx_DIR=/path/to/lib/cmake/ktx
#  2) -DCMAKE_PREFIX_PATH=/path/to/prefix
#  3) Env var KTX_ROOT=/path/to/prefix  (we'll translate it to CMAKE_PREFIX_PATH)
#
# A "prefix" typically contains: include/, lib/, and lib/cmake/ktx/ktxConfig.cmake

if(NOT DEFINED KTX_ROOT AND DEFINED ENV{KTX_ROOT})
  set(KTX_ROOT "$ENV{KTX_ROOT}")
endif()

if(DEFINED KTX_ROOT)
  # Prepend so it wins over system installs
  list(PREPEND CMAKE_PREFIX_PATH "${KTX_ROOT}")
endif()

# Now try to locate the config package
find_package(ktx CONFIG REQUIRED)

if(NOT ktx_FOUND)
  message(FATAL_ERROR
    "KTX not found.\n"
    "Install KTX (KTX-Software) and make sure CMake can find ktxConfig.cmake.\n\n"
    "You can point CMake to it using ONE of:\n"
    "  - Set env var KTX_ROOT to the install prefix (contains include/ and lib/)\n"
    "  - Pass -DCMAKE_PREFIX_PATH=<prefix>\n"
    "  - Pass -Dktx_DIR=<prefix>/lib/cmake/ktx\n\n"
    "Example (PowerShell):\n"
    "  $env:KTX_ROOT='C:/SDKs/ktx'\n"
    "  cmake -S . -B build\n"
  )
endif()

# --- Boost hinting (the EDITOR-ONLY fibers backend) ----------------------------
# Boost.Fibers needs compiled libs (context/fiber/thread/chrono/atomic),
# staged at C:/boost/boost_1_89_0/stage (built with b2 toolset=msvc-14.3
# address-model=64 link=static runtime-link=shared threading=multi
# variant=release,debug -j 8 stage). Only the Editor links it — the engine,
# the App and every tool stay Boost-free.
#
# Users can point CMake at it in several ways:
#  1) -DBoost_DIR=/path/to/stage/lib/cmake/Boost-1.89.0
#  2) -DBoost_ROOT=/path/to/boost_1_89_0
#  3) Env var BOOST_ROOT=/path/to/boost_1_89_0  (translated below)

if(NOT DEFINED Boost_ROOT AND DEFINED ENV{BOOST_ROOT})
  set(Boost_ROOT "$ENV{BOOST_ROOT}")
endif()

if(DEFINED Boost_ROOT)
  # Prepend the staged cmake config dir so it wins over any system install
  list(PREPEND CMAKE_PREFIX_PATH "${Boost_ROOT}/stage/lib/cmake")
endif()

# nlohmann::json (pinned git submodule; required for tinygltf)
add_subdirectory(external/json)
add_subdirectory(external/volk)  # Vulkan loader (pinned git submodule)

# --- Deferred-shadow debug visualizers (paper 5.3/5.4) -------------------------
# Per-pixel pipeline diagnostics: light culling steps, light-list membership,
# per-Shadow-Tile light counts (see engine/shaders/deferredshadow.hlsli +
# the "DS visualizers" ImGui block). Compiled in via DS_DEBUG_VIZ for the
# App target (app/CMakeLists.txt) and for DXC (CompileShaders.cmake).
#   unset/-D...="" -> auto: ON in Debug trees, OFF everywhere else
#                     (x64-Release stays clean, zero cost)
#   -DTOURMALINE_DS_VIZ=ON  -> force ON at any optimization level
#                     (the x64-Release-DSViz CMakeSettings preset: release
#                      -O2 C++ / -O3 shaders WITH visualizers)
#   -DTOURMALINE_DS_VIZ=OFF -> force OFF even in Debug trees
set(TOURMALINE_DS_VIZ "" CACHE BOOL
    "Deferred-shadow debug visualizers (empty = auto: Debug builds only)")
if(TOURMALINE_DS_VIZ)
  set(DS_VIZ_ON ON)
elseif(TOURMALINE_DS_VIZ STREQUAL "" AND CMAKE_BUILD_TYPE MATCHES "Debug")
  set(DS_VIZ_ON ON)
else()
  set(DS_VIZ_ON OFF)
endif()
message(STATUS "Deferred-shadow visualizers: ${DS_VIZ_ON}"
  " (TOURMALINE_DS_VIZ='${TOURMALINE_DS_VIZ}', build type '${CMAKE_BUILD_TYPE}')")

# --- Shader debug information (-Zi) ---------------------------------------------
# Release tree whose shaders carry SPIR-V debug info (names/lines/source
# paths via NonSemantic.Shader.DebugInfo.100 — RenderDoc & friends) instead
# of the lean -Qstrip_debug modules. C++ codegen stays Release (-O2).
#   unset/-DTOURMALINE_SHADER_DEBUG=OFF -> OFF (Release keeps -O3 + strip)
#   Debug trees already carry debug info via their own CompileShaders branch
#   -DTOURMALINE_SHADER_DEBUG=ON -> shaders compile -O3 -Zi -fspv-debug=vulkan
#     in a Release tree (the x64-Release-ShaderDebug CMakeSettings preset).
# NOTE: DXC's -Zi and -Zs are mutually exclusive ("Cannot specify both");
# -Zi is the flag that embeds debug info in the SPIR-V itself, so that is
# what this flavor uses.
set(TOURMALINE_SHADER_DEBUG OFF CACHE BOOL
    "Compile shaders with -Zi SPIR-V debug info in Release trees")
if(TOURMALINE_SHADER_DEBUG AND NOT CMAKE_BUILD_TYPE MATCHES "Debug")
  set(SHADER_DEBUG_ON ON)
else()
  set(SHADER_DEBUG_ON OFF)
endif()
message(STATUS "Shader debug info (-Zi): ${SHADER_DEBUG_ON}"
  " (TOURMALINE_SHADER_DEBUG='${TOURMALINE_SHADER_DEBUG}', build type '${CMAKE_BUILD_TYPE}')")
# True when the SPIR-V this tree ships actually CARRIES that debug info:
# Debug trees do via their own CompileShaders branch (-Od -Zi
# -fspv-debug=vulkan), Release only via SHADER_DEBUG_ON above. Gates the
# driver-side Nsight Aftermath half of crash symbolication: with debug
# info present, the engine asks the NVIDIA driver to generate the
# GPU-machine-code -> SPIR-V line tables at device creation
# (VK_NV_device_diagnostics_config + shader debug info bit, see
# engine/src/Vulkan/impl.cpp device init + TOURMALINE_AFTERMATH=0 kill
# switch). Lean Release trees stay off — driver tables over stripped
# SPIR-V map nothing and only cost pipeline-compile time/memory.
if(SHADER_DEBUG_ON OR CMAKE_BUILD_TYPE MATCHES "Debug")
  set(SHADER_DEBUG_INFO_ON ON)
else()
  set(SHADER_DEBUG_INFO_ON OFF)
endif()

# --- Optick profiler ----------------------------------------------------------
option(ENABLE_OPTICK "Enable Optick CPU/GPU profiling" ON)
if(ENABLE_OPTICK)
  set(OPTICK_USE_VULKAN ON CACHE BOOL "" FORCE)
else()
  set(OPTICK_USE_VULKAN OFF CACHE BOOL "" FORCE)
endif()
set(OPTICK_USE_D3D12 OFF CACHE BOOL "" FORCE)
set(OPTICK_BUILD_GUI_APP OFF CACHE BOOL "" FORCE)
set(OPTICK_BUILD_CONSOLE_SAMPLE OFF CACHE BOOL "" FORCE)
set(OPTICK_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
if(NOT ENABLE_OPTICK)
  set(OPTICK_ENABLED OFF CACHE BOOL "" FORCE)
endif()
add_subdirectory(external/optick)

# Shader compile support
set(SHADER_SOURCE_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_BINARY_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_BINARY_DIR})

# Shader compilation rule (loaded from custom cmake file)
include(cmake/CompileShaders.cmake)

option(TRACE_INCLUDES "Print header include tree while compiling" OFF)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  if (TRACE_INCLUDES)
    add_compile_options(-H)        # GCC/Clang: show include stack
  endif()
elseif (MSVC)
  if (TRACE_INCLUDES)
    add_compile_options(/showIncludes)  # MSVC: show include stack
  endif()
endif()

# SPIRV-Reflect (pinned git submodule, CPU-only reflection for push/spec readback)
add_library(spirv_reflect STATIC external/SPIRV-Reflect/spirv_reflect.c)
target_include_directories(spirv_reflect PUBLIC
    ${CMAKE_SOURCE_DIR}/external/SPIRV-Reflect
    ${CMAKE_SOURCE_DIR}/external/SPIRV-Reflect/include
)
set_target_properties(spirv_reflect PROPERTIES C_STANDARD 11 LINKER_LANGUAGE C)

add_subdirectory(external/imgui)
add_subdirectory(engine)
# Embree backend wiring (see the options block above): the engine links
# the static lib (the traversal path is unconditional in the source —
# the builtin stays as the deterministic reference backend + the
# ClosestDistance path).
target_link_libraries(engine PRIVATE embree)
add_subdirectory(tools/gltf_import)
add_subdirectory(tools/import)
add_subdirectory(tools/artifact_cache)
add_subdirectory(tools/cook)
add_subdirectory(app)
add_subdirectory(tools/pak_cooker)
add_subdirectory(editor)
add_subdirectory(tools/gi_baker)
add_subdirectory(tools/cubemap_baker)
add_subdirectory(samples/01_triangle)
add_subdirectory(samples/02_blur)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

set_property(CACHE ENGINE_BACKEND PROPERTY STRINGS
    "Vulkan"
)

if(ENGINE_BACKEND STREQUAL "Vulkan")
    target_compile_definitions(engine PRIVATE
        ENGINE_VULKAN=1
    )
else()
    message(FATAL_ERROR "Unsupported rendering backend: ${ENGINE_BACKEND}")
endif()

