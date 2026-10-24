# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(Editor)

# (The Boost.Fibers job backend was REFACTORED OUT: editor_fibers.cpp is
# a plain std::thread worker pool now — jobs never yielded, so the fiber
# layer carried zero concurrent benefit while its custom stacks caused
# the deep-job heap overruns; see the HISTORY note there. NOTHING in the
# tree links Boost anymore.)

file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS
    src/*.cpp
    src/*.h
)

add_executable(Editor ${EDITOR_SOURCES})

target_compile_definitions(Editor PRIVATE ENGINE_VULKAN)

# MUST match the engine's DS_DEBUG_VIZ gate (engine/CMakeLists.txt) — the
# DS_VIZ-gated FrameUniforms/WorldRenderer members shift the engine classes'
# LAYOUT, and the editor's TUs ALLOCATE those classes (MakeProcess<
# WorldRenderer>). Without the define the editor's sizeof(WorldRenderer)
# was 48 B SMALLER than the engine's member offsets: the hosted
# WorldRenderer's member writes landed PAST its arena allocation, into the
# next arena object (the hosted RenderGraph — topoHeap_._Myproxy stomped
# to 1, then the sort's wild-proxy crash took the device down; caught by a
# Dr0 hardware write watchpoint: UpdateGfxTextureDemand ->
# m_gfxMetaEpochUploaded = epoch at graph+32). Same DS_VIZ_ON gate as
# engine/App (root CMakeLists: Debug trees unless forced).
if(DS_VIZ_ON)
    target_compile_definitions(Editor PRIVATE DS_DEBUG_VIZ=1)
endif()

target_include_directories(Editor
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        # Reuses the engine's ABI-checked FrameUniforms + PushConstants
        # (common.hlsli layout, static_asserted in WorldRenderer.h).
        # (Was app/include + app/vendor before the renderer moved to engine;
        # stb headers now live in external/stb.)
        ${CMAKE_SOURCE_DIR}/external/stb
        # nanosvg (pinned git submodule; editor/icons/*.svg rasterization
        # for the asset browser).
        ${CMAKE_SOURCE_DIR}/external/nanosvg/src
        # ImGui Vulkan backend header (ImGui_ImplVulkan_AddTexture for the
        # rasterized SVG icons as ImGui user textures).
        ${CMAKE_SOURCE_DIR}/external/imgui
        ${CMAKE_SOURCE_DIR}/external/imgui/backends
        # In-process baker libs (the editor links the tools; their CLIs
        # stay thin user interfaces over the same Run entry points).
        ${CMAKE_SOURCE_DIR}/tools/gi_baker
        ${CMAKE_SOURCE_DIR}/tools/cook/src
        ${CMAKE_SOURCE_DIR}/tools/common
)

target_link_libraries(Editor
    PRIVATE
        engine
        gltf_import
        artifact_cache
        GiBakerLib
        CookLib
        PakLib
        KTX::ktx
        OptickCore
        # svg_icons.cpp: imgui_impl_vulkan.h pulls <vulkan/vulkan.h>
        # (the backend TU itself is compiled into the engine).
        Vulkan::Vulkan
)
if(WIN32)
    # main.cpp crash stack writer (SymFromAddr/SymGetLineFromAddr64);
    # the Linux counterpart uses glibc's execinfo backtrace (in libc).
    target_link_libraries(Editor PRIVATE Dbghelp)
endif()

set_target_properties(Editor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Linux: the staged .so's (SDL3 / OptickCore / KTX / libdxcompiler) sit
# next to the exe - $ORIGIN keeps them loadable (App gets this via
# cmake/HostSetup.cmake's tourmaline_host_setup; the editor wires its own
# properties).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_target_properties(Editor PROPERTIES
        BUILD_RPATH "\$ORIGIN"
        INSTALL_RPATH "\$ORIGIN"
    )
endif()

# KTX / OptickCore are SHARED: stage next to Editor.exe (same as App).
add_custom_command(TARGET Editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:KTX::ktx>"
        "$<TARGET_FILE_DIR:Editor>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OptickCore>"
        "$<TARGET_FILE_DIR:Editor>"
)

# SVG icon set (the asset browser grid): staged exe-relative as
# editor/icons/ next to Editor.exe (read at startup, rasterized once).
add_custom_command(TARGET Editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/icons"
            "$<TARGET_FILE_DIR:Editor>/editor/icons"
    COMMENT "Syncing SVG icons next to Editor.exe"
)

tourmaline_stage_sdl3(Editor)

# ---------------------------------------------------------------------------
# THE EDITOR LAUNCHER (the editor/launcher split): a standalone project
# picker (editor/launcher/main.cpp — console v1, zero engine deps) that
# owns PROJECT SELECTION and spawns Editor.exe --project <dir>. The editor
# REQUIRES the --project arg (see its main.cpp): no launcher, no launch.
# Shares the editor.prefs recents (the same [projects] rows).
# ---------------------------------------------------------------------------
add_executable(EditorLauncher launcher/main.cpp)
set_target_properties(EditorLauncher PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# The editor renders with the same compiled shaders as App
# (default.vert / default_unlit / default_lit / shadow.vert / ...).
# They are built by the engine's EngineScene_shaders target (compile_shaders()'s
# own ALL target) into ${CMAKE_BINARY_DIR}/content/shaders; sync them next to
# Editor.exe.
if(TARGET EngineScene_shaders)
    add_dependencies(Editor EngineScene_shaders)
endif()
add_custom_command(TARGET Editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Editor>/content/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_BINARY_DIR}/content/shaders"
            "$<TARGET_FILE_DIR:Editor>/content/shaders"
    COMMENT "Syncing compiled shaders next to Editor.exe"
)
