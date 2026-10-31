# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(Pak)  # pak.exe: the PAK COOK - consumed artifacts only (raw glTF never cooks; import.exe / the MeshImporter produce the artifacts)

# Pak as a library (the editor links it and calls paktool::Run
# directly; the CLI stays a thin user interface over the same code).
# The lib build excludes main() via PAK_LIB_BUILD.
add_library(PakLib STATIC main.cpp terrain_cook.cpp)
target_compile_definitions(PakLib PRIVATE ENGINE_VULKAN PAK_LIB_BUILD)
target_include_directories(PakLib
PRIVATE
${CMAKE_SOURCE_DIR}/external/stb
${CMAKE_SOURCE_DIR}/tools/common
PUBLIC
${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(PakLib
PRIVATE
engine
gltf_import
artifact_cache
volk
OptickCore
)
add_executable(Pak main.cpp terrain_cook.cpp)

target_compile_definitions(Pak PRIVATE ENGINE_VULKAN)

# stb_image_write (cook-time atlas debug PNG, see lm_png.cpp; the same
# vendored header the editor uses for its PNG export).
target_include_directories(Pak PRIVATE ${CMAKE_SOURCE_DIR}/external/stb ${CMAKE_SOURCE_DIR}/tools/common)

target_link_libraries(Pak
    PRIVATE
        engine
        gltf_import
        artifact_cache
        volk
        OptickCore
)

set_target_properties(Pak PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# KTX is a shared lib on this setup: stage it next to Pak.exe
# (the --cubemap stage encodes KTX2 in-process, like CubemapBaker).
add_custom_command(TARGET Pak POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:KTX::ktx>"
        "$<TARGET_FILE_DIR:Pak>"
)

# OptickCore is SHARED: stage its DLL next to Pak.exe
add_custom_command(TARGET Pak POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OptickCore>"
        "$<TARGET_FILE_DIR:Pak>"
)

tourmaline_stage_sdl3(Pak)

# Vulkan headers opt-in (the namespace/RHI boundary audit): this target
# includes <volk.h> / Vulkan headers directly - link the Linux
# full-header iface EXPLICITLY instead of relying on the engine's (now
# PRIVATE) propagation. Windows: the SDK headers via find_package.
if(TARGET vulkan_headers_iface)
    target_link_libraries(PakLib PRIVATE vulkan_headers_iface)
endif()
# Vulkan headers opt-in (the namespace/RHI boundary audit): this target
# includes <volk.h> / Vulkan headers directly - link the Linux
# full-header iface EXPLICITLY instead of relying on the engine's (now
# PRIVATE) propagation. Windows: the SDK headers via find_package.
if(TARGET vulkan_headers_iface)
    target_link_libraries(Pak PRIVATE vulkan_headers_iface)
endif()

# ---------------------------------------------------------------------------
# THE ENGINE SHADER PACKAGE (engine-shaders.pak): one ENGINE-OWNED
# package over the DECLARED compiled-output list (engine/CMakeLists.txt
# writes ${CMAKE_BINARY_DIR}/engine-shaders.list from compile_shaders'
# exact .spv set). Pak.exe's --shaders-list mode validates every blob's
# SPIR-V magic and packs the list EXACTLY (a retired variant's stale
# .spv in an old build tree never ships). This is the FIRST scope where
# both EngineScene_shaders and the Pak tool exist (engine is
# add_subdirectory'd before tools). Hosts deploy the package next to
# their exe (HostSetup) and the engine shader library
# (Engine/Utils/shader_library.h) mounts it at boot — a MANDATORY
# runtime dependency of every graphics host, built independent of any
# scene or project recipe.
# ---------------------------------------------------------------------------
if(TARGET EngineScene_shaders AND DEFINED ENGINE_SHADER_OUTPUTS)
    set(ENGINE_SHADER_PAK "${CMAKE_BINARY_DIR}/content/paks/engine-shaders.pak"
        CACHE INTERNAL "the engine-owned shader package")
    add_custom_command(
        OUTPUT "${ENGINE_SHADER_PAK}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/content/paks"
        COMMAND $<TARGET_FILE:Pak> --shaders-list "${CMAKE_BINARY_DIR}/engine-shaders.list"
                --out-pak "${ENGINE_SHADER_PAK}" --compress on
        # The DECLARED list is the feed; the .spv outputs are the FILE
        # dependencies (a target DEPENDS alone would only order, not
        # rebuild, on a recompiled shader).
        DEPENDS "${CMAKE_BINARY_DIR}/engine-shaders.list" ${ENGINE_SHADER_OUTPUTS}
        COMMENT "Packing engine-shaders.pak (the declared engine shader set)"
        VERBATIM
    )
    add_custom_target(EngineScene_shaderpak ALL DEPENDS "${ENGINE_SHADER_PAK}")
    add_dependencies(EngineScene_shaderpak Pak)
endif()