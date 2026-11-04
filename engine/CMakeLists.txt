# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(Engine)

set(ENGINE_BACKEND "Vulkan")

# --- vk_format_utils.h fallback ----------------------------------------------
# The engine includes <vulkan/utility/vk_format_utils.h> (impl.cpp), which
# ships with the Vulkan SDK - but several distro libvulkan-dev packages
# trim the utility headers (Ubuntu 24.04 does). Probe the system Vulkan
# headers; when the file is absent, append the tracked vendored copy
# (external/vk-headers-extra, the LunarG-generated SDK header, Apache-2.0
# - see THIRD_PARTY_NOTICES) to the include path. The probe keeps the
# system/SDK pair in charge wherever it exists (matching core/util
# versions), and the vendored file only fills the gap.
include(CheckIncludeFileCXX)
set(CMAKE_REQUIRED_INCLUDES ${Vulkan_INCLUDE_DIRS})
check_include_file_cxx("vulkan/utility/vk_format_utils.h" TOURMALINE_HAVE_VK_FORMAT_UTILS)
set(CMAKE_REQUIRED_INCLUDES "")
set(ENGINE_FALLBACK_INCS "")
if(NOT TOURMALINE_HAVE_VK_FORMAT_UTILS)
    message(STATUS "vk_format_utils.h missing from the system Vulkan headers - "
        "appending the vendored copy (external/vk-headers-extra)")
    set(ENGINE_FALLBACK_INCS "${TOURMALINE_SOURCE_DIR}/external/vk-headers-extra")
endif()



file(GLOB_RECURSE ENGINE_SRC CONFIGURE_DEPENDS
    src/*.cpp
    include/Engine/*.h
)

add_library(engine STATIC ${ENGINE_SRC})

target_include_directories(engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${TOURMALINE_SOURCE_DIR}/external/tinygltf
        ${nlohmann_json_SOURCE_DIR}/single_include
        ${nlohmann_json_SOURCE_DIR}/single_include/nlohmann
        ${TOURMALINE_SOURCE_DIR}/external/SPIRV-Reflect
        ${TOURMALINE_SOURCE_DIR}/external/SPIRV-Reflect/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Vulkan/detail
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RHI/detail
        ${TOURMALINE_SOURCE_DIR}/external/VulkanMemoryAllocator
        ${TOURMALINE_SOURCE_DIR}/external/streamline/include
        # stb_image/stb_image_write (LutIo PNG readback).
        ${TOURMALINE_SOURCE_DIR}/external/stb
        # gltf_import helper used by the world renderer
        # (GltfPunctualAimAxis): headers only — the lib itself stays with
        # the executables (gltf_import PUBLIC-links engine, so linking it
        # here would be a dependency cycle).
        ${TOURMALINE_SOURCE_DIR}/tools/gltf_import/include
        # miniaudio (vendored single-header, device-less configuration):
        # the audio DSP/node-graph core — include path is PRIVATE (the
        # engine's public headers never mention ma_* types; all miniaudio
        # use lives in engine/src/Audio), and every TU includes it through
        # src/Audio/miniaudio_config.h so the feature macros are uniform.
        ${TOURMALINE_SOURCE_DIR}/external/miniaudio
        ${ENGINE_FALLBACK_INCS}
)

# The miniaudio implementation TU skips the engine PCH: a 4 MB
# self-contained C header gains nothing from the force-injected engine
# includes (and would recompile them pointlessly into its prefix).
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Audio/miniaudio_impl.cpp"
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON
)


target_compile_definitions(engine 
    PUBLIC
        GLM_FORCE_RADIANS
        GLM_FORCE_DEPTH_ZERO_TO_ONE
        GLM_ENABLE_EXPERIMENTAL
        TINYGLTF_NO_STB_IMAGE
        TINYGLTF_NO_STB_IMAGE_WRITE
        TINYGLTF_NO_EXTERNAL_IMAGE
)

if(ENGINE_BACKEND STREQUAL "Vulkan")
    target_compile_definitions(engine PRIVATE ENGINE_VULKAN)
endif()

# Full Vulkan headers (Linux; see the root CMakeLists block): PRIVATE
# since the namespace/RHI boundary audit - the engine's PUBLIC headers
# include no Vulkan (only engine/src/Vulkan impl TUs do), so the
# include path must NOT propagate to consumers. The Linux consumers
# that genuinely include <volk.h>/Vulkan headers directly (the cook
# tools' VkFormat enums, the GPU window tests) opt in themselves - the
# boundary rule: RHI/Vulkan access is an explicit opt-in, never a
# transitive default. (Windows: the SDK headers arrive via
# find_package(Vulkan) at the consumers that need them - unchanged.)
if(TARGET vulkan_headers_iface)
    target_link_libraries(engine PRIVATE vulkan_headers_iface)
endif()

# ---------------------------------------------------------------------------
# Precompiled Header for the engine (moved with the world renderer from
# appcore). CMake's built-in target_precompile_headers works with both
# MSBuild and Ninja (+ MSVC cl.exe). No manual #include "pch.h" needed:
# CMake injects /Yu + /FI. Disable with -DTOURMALINE_PCH=OFF if the PCH
# ever masks a header bug.
# ---------------------------------------------------------------------------
option(TOURMALINE_PCH "Use precompiled headers for engine" ON)
if(TOURMALINE_PCH AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC's PCH does not carry #pragma-once state across the .gch boundary:
    # every header reachable through pch.h AND the TU's own include chain
    # redefines its contents (redefinition errors across Engine/*.h). The
    # headers are #pragma-once style (no classic include guards), so GCC
    # instead gets the pch.h FORCE-INCLUDED as a plain -include (identical
    # header availability - many TUs rely on pch.h's transitive includes -
    # at the cost of re-parsing it per TU, with fully normal pragma-once
    # semantics). MSVC/Clang keep the real PCH.
    message(STATUS "TOURMALINE_PCH: GCC uses -include pch.h instead of a .gch (PCH + #pragma once redefinition breakage)")
    set(TOURMALINE_PCH OFF)
    set(TOURMALINE_GCC_FORCE_INCLUDE_PCH ON)
endif()
if(TOURMALINE_PCH)
    target_precompile_headers(engine PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pch.h
    )
elseif(TOURMALINE_GCC_FORCE_INCLUDE_PCH)
    target_compile_options(engine PRIVATE
        "-include${CMAKE_CURRENT_SOURCE_DIR}/pch.h"
    )
endif()

# Deferred-shadow debug visualizers (paper 5.3/5.4): the world renderer
# moved here from appcore, taking its DS_DEBUG_VIZ-gated FrameUniforms
# tail with it. Same auto logic as before (root CMakeLists resolves
# DS_VIZ_ON: Debug trees unless forced); must match the DXC define or
# the CPU/GPU layout diverges.
if(DS_VIZ_ON)
    target_compile_definitions(engine PRIVATE DS_DEBUG_VIZ=1)
endif()

if(TOURMALINE_NSIGHT_MARKERS)
    target_compile_definitions(engine PRIVATE TOURMALINE_NSIGHT_MARKERS=1)
endif()

# Nsight Aftermath shader debug tables (crash symbolication): when this
# tree's SPIR-V carries DXC -Zi debug info (root CMakeLists resolves
# SHADER_DEBUG_INFO_ON: Debug trees / TOURMALINE_SHADER_DEBUG=ON preset),
# the engine's device init additionally probes + enables
# VK_NV_device_diagnostics_config with the shader-debug-info bit chained
# into VkDeviceCreateInfo — the NVIDIA-driver half that maps GPU machine
# code back to SPIR-V so Aftermath crash dumps resolve to HLSL source
# lines. Runtime kill switch: TOURMALINE_AFTERMATH=0.
if(SHADER_DEBUG_INFO_ON)
    target_compile_definitions(engine PRIVATE TOURMALINE_SHADER_DEBUG_INFO=1)
endif()

# Proprietary NVIDIA SDK is installed locally, not vendored or packaged.
option(TOURMALINE_AFTERMATH_SDK "Private Aftermath SDK crash capture" OFF)
if(TOURMALINE_AFTERMATH_SDK)
    if(NOT WIN32 OR NOT TOURMALINE_BUILD_PROFILE STREQUAL "diagnostics")
        message(FATAL_ERROR "Aftermath SDK requires the private Windows diagnostics profile")
    endif()
    set(TOURMALINE_AFTERMATH_SDK_ROOT "" CACHE PATH "Local Nsight Aftermath SDK root")
    find_path(AFTERMATH_INCLUDE_DIR GFSDK_Aftermath_GpuCrashDump.h
        HINTS "${TOURMALINE_AFTERMATH_SDK_ROOT}" PATH_SUFFIXES include)
    find_library(AFTERMATH_LIBRARY NAMES GFSDK_Aftermath_Lib.x64
        HINTS "${TOURMALINE_AFTERMATH_SDK_ROOT}" PATH_SUFFIXES lib/x64 lib)
    if(NOT AFTERMATH_INCLUDE_DIR OR NOT AFTERMATH_LIBRARY)
        message(FATAL_ERROR "Missing Aftermath SDK; set TOURMALINE_AFTERMATH_SDK_ROOT")
    endif()
    target_include_directories(engine PRIVATE "${AFTERMATH_INCLUDE_DIR}")
    target_link_libraries(engine PRIVATE "${AFTERMATH_LIBRARY}")
    target_compile_definitions(engine PRIVATE TOURMALINE_USE_AFTERMATH_SDK=1)
    find_file(AFTERMATH_DLL GFSDK_Aftermath_Lib.x64.dll
        HINTS "${TOURMALINE_AFTERMATH_SDK_ROOT}" PATH_SUFFIXES lib/x64 bin/x64 bin)
    if(NOT AFTERMATH_DLL)
        message(FATAL_ERROR "Missing Aftermath SDK runtime DLL")
    endif()
    add_custom_command(TARGET engine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${AFTERMATH_DLL}" "${TOURMALINE_RUNTIME_DIR}")
endif()

target_link_libraries(engine
    PUBLIC
        SDL3::SDL3
        glm
        imgui_core
        meshoptimizer
        xatlas
        spirv_reflect
    PRIVATE
        directxtex_bc
        volk
        KTX::ktx
        imgui_backend_sdl3
        imgui_backend_vulkan
        lz4
)
if(ENABLE_OPTICK)
    target_link_libraries(engine PUBLIC OptickCore)
else()
    target_include_directories(engine PUBLIC "${TOURMALINE_SOURCE_DIR}/external/optick/src")
    target_compile_definitions(engine PUBLIC USE_OPTICK=0)
endif()
if(WIN32)
    # Windows system libs. (winhttp/bcrypt were linked historically; the
    # tree no longer references them, but the link stays to keep the
    # proven Windows link line untouched.)
    target_link_libraries(engine PRIVATE winhttp bcrypt)
else()
    # std::thread and the worker backends need -pthread; dl for the
    # runtime module loads.
    find_package(Threads REQUIRED)
    target_link_libraries(engine PUBLIC Threads::Threads)
    target_link_libraries(engine PRIVATE ${CMAKE_DL_LIBS})
endif()

# DXC -- shader compiling HLSL->SPIRV
# Windows: the vendored external/dxc MSVC package (dxcompiler.lib import
# lib; dxcompiler.dll/dxil.dll staged at the build root POST_BUILD).
# Linux: the vendored external/dxc linux package (lib/libdxcompiler.so +
# libdxil.so), staged at the build root too; the exes' $ORIGIN rpath
# (cmake/HostSetup.cmake + editor/CMakeLists) keeps them loadable.
# Staging targets ${TOURMALINE_RUNTIME_DIR} (the TOP-LEVEL build root,
# CMAKE_BINARY_DIR), where every host exe runs from (the
# RUNTIME_OUTPUT_DIRECTORY convention in cmake/HostSetup.cmake + the
# game-host cook tools) - NOT the static lib's own dir
# ($<TARGET_FILE_DIR:engine> would drop the DLLs in engine/ where no
# loader searches) and NOT this tree's own binary dir
# (${TOURMALINE_BINARY_DIR}): a game repo embedding this tree through a
# submodule places the exes in the GAME's build root, one level above
# this tree's dir, and Windows loads staged DLLs from the exe dir only.
set(DXC_DIR "${TOURMALINE_SOURCE_DIR}/external/dxc")

target_include_directories(engine PRIVATE
    "${DXC_DIR}/include"
)

if(WIN32)
    target_link_libraries(engine PRIVATE
        "${DXC_DIR}/lib/x64/dxcompiler.lib"
    )

    add_custom_command(TARGET engine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DXC_DIR}/bin/x64/dxcompiler.dll"
            "${TOURMALINE_RUNTIME_DIR}"

        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DXC_DIR}/bin/x64/dxil.dll"
            "${TOURMALINE_RUNTIME_DIR}"
    )
else()
    target_link_libraries(engine PRIVATE
        "${DXC_DIR}/lib/libdxcompiler.so"
    )

    add_custom_command(TARGET engine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DXC_DIR}/lib/libdxcompiler.so"
            "${TOURMALINE_RUNTIME_DIR}"

        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DXC_DIR}/lib/libdxil.so"
            "${TOURMALINE_RUNTIME_DIR}"
    )
endif()

# OptickCore builds SHARED upstream -> its runtime (DLL on Windows, .so
# on Linux) must sit next to the exes (build root, same staging target
# as DXC above - the host-staging in cmake/HostSetup.cmake mirrors it).
if(ENABLE_OPTICK)
add_custom_command(TARGET engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OptickCore>"
        "${TOURMALINE_RUNTIME_DIR}"
)
endif()

# SDL3 is SHARED (vendored prebuilt on Windows / built as a subproject or
# system on Linux) -> same build-root staging.
add_custom_command(TARGET engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SDL3::SDL3>"
        "${TOURMALINE_RUNTIME_DIR}"
)

# Prebuilt-Embree route (root CMakeLists find_package): the shared
# embree4.dll + its bundled TBB runtime must sit next to the exes (build
# root) or the loader finds neither; the source route is a static lib and
# stages nothing. embree_DIR points into <pkg>/lib/cmake/embree* - the
# runtime DLLs sit in <pkg>/bin.
if(embree_FOUND)
    set(_EMBREE_BIN_DIR "${embree_DIR}/../../../bin")
    foreach(_embree_dll IN ITEMS embree4.dll tbb12.dll tbbmalloc.dll)
        if(EXISTS "${_EMBREE_BIN_DIR}/${_embree_dll}")
            add_custom_command(TARGET engine POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_EMBREE_BIN_DIR}/${_embree_dll}"
                    "${TOURMALINE_RUNTIME_DIR}"
                COMMENT "Staging the prebuilt Embree runtime ${_embree_dll} at the build root"
            )
        else()
            message(WARNING "prebuilt Embree runtime '${_embree_dll}' not found in "
                "${_EMBREE_BIN_DIR} - executables may fail to load Embree")
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Scene shaders (engine/shaders -> content/shaders/*.spv): the world
# renderer's pipeline set, loaded relative to the working directory
# alongside user content. Owned here since the renderer moved into the
# engine (was App's App_shaders target). Emits the ALL target
# "EngineScene_shaders" over the outputs; App + Editor sync them next to
# their exes POST_BUILD (see app/editor CMakeLists). Do NOT add a second
# ALL target over the same .spv files — two independent custom targets
# writing identical outputs race in MSBuild (MSB8066 "exited with code 1"
# on one of them, ~every shader change).
# ---------------------------------------------------------------------------
# One declarative list is shared by shader compilation and the C++ hot-reload
# registry. Configure-time generation avoids filesystem reads at runtime.
set(TOURMALINE_SHADER_VARIANT_SPEC "${CMAKE_CURRENT_SOURCE_DIR}/shaders/variants.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${TOURMALINE_SHADER_VARIANT_SPEC}")
file(STRINGS "${TOURMALINE_SHADER_VARIANT_SPEC}" TOURMALINE_SHADER_VARIANTS
    REGEX "^[^#][^|]*\\|\\.[^|]*\\|")
set(_variant_cpp "// Generated by engine/CMakeLists.txt from shaders/variants.txt. Do not edit.\n")
foreach(_entry IN LISTS TOURMALINE_SHADER_VARIANTS)
    string(REPLACE "|" ";" _fields "${_entry}")
    list(LENGTH _fields _count)
    if(_count LESS 3)
        message(FATAL_ERROR "Invalid shader variant specification: ${_entry}")
    endif()
    list(GET _fields 0 _source)
    list(GET _fields 1 _suffix)
    string(APPEND _variant_cpp "    table[\"${_source}\"].push_back({\"${_suffix}\", {")
    math(EXPR _last "${_count} - 1")
    foreach(_index RANGE 2 ${_last})
        list(GET _fields ${_index} _define)
        if(NOT _define MATCHES "^[A-Za-z_][A-Za-z0-9_]*=[A-Za-z0-9_+-]+$")
            message(FATAL_ERROR "Invalid shader variant define: ${_define}")
        endif()
        string(APPEND _variant_cpp "\"${_define}\", ")
    endforeach()
    string(APPEND _variant_cpp "}});\n")
endforeach()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/shader_variants.inc" "${_variant_cpp}")
target_include_directories(engine PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

compile_shaders(
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    "${CMAKE_BINARY_DIR}/content/shaders"
    EngineScene
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    # Fixed-count lit permutations: the Main filtered leg binds the lit FS
    # matching each material range's cooked lobe count (see GfxRenderer::
    # FilteredLitProgram), so the shader's SG evaluation folds to literals.
    # Output: default_lit.frag.hlsl.sg{5,6,9,12}.spv next to the base module.
    VARIANTS
        ${TOURMALINE_SHADER_VARIANTS}
)

# ---------------------------------------------------------------------------
# THE ENGINE SHADER PACKAGE (engine-shaders.pak): the DECLARED compiled-
# output list (compile_shaders' exact .spv set — base outputs + every
# configured variant) is the authoritative packaging feed. The list file
# + the export land here; tools/pak_cooker/CMakeLists.txt (the first
# scope where the Pak tool exists) owns the packaging command, and
# HostSetup deploys the package next to every host. The list (not a
# directory walk) is what keeps a retired variant's stale .spv in an
# old build tree OUT of the package.
# ---------------------------------------------------------------------------
set(ENGINE_SHADER_OUTPUTS ${COMPILED_SHADER_OUTPUTS})
set(ENGINE_SHADER_OUTPUTS ${ENGINE_SHADER_OUTPUTS} PARENT_SCOPE)
list(SORT ENGINE_SHADER_OUTPUTS)
string(REPLACE ";" "\n" _ENGINE_SHADER_LIST "${ENGINE_SHADER_OUTPUTS}")
file(WRITE "${CMAKE_BINARY_DIR}/engine-shaders.list"
    "# The DECLARED engine compiled-output list (generated by engine/"
    "CMakeLists.txt - the engine-shaders.pak feed). One host path per"
    " line.\n${_ENGINE_SHADER_LIST}\n")
