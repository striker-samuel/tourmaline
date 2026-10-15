project(Editor)

# EDITOR-ONLY fibers backend: Boost.Fibers needs compiled libs. The
# root CMakeLists BOOST_ROOT hint (env/-D) points CMake at the machine
# stage (C:/boost/boost_1_89_0). Nothing else in the tree links Boost.
find_package(Boost 1.89 REQUIRED COMPONENTS fiber context thread chrono atomic)

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
        # nanosvg (editor/icons/*.svg rasterization for the asset browser).
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
        # EDITOR-ONLY fibers backend (see editor_fibers.h): Boost.Fibers
        # from the machine-level stage at C:/boost (root CMakeLists
        # BOOST_ROOT hint). The engine, the App and the tools stay
        # Boost-free.
        Boost::fiber
        Boost::context
        Boost::thread
        Boost::chrono
        Boost::atomic
        KTX::ktx
        OptickCore
        # svg_icons.cpp: imgui_impl_vulkan.h pulls <vulkan/vulkan.h>
        # (the backend TU itself is compiled into the engine).
        Vulkan::Vulkan
        # main.cpp crash stack writer (SymFromAddr/SymGetLineFromAddr64).
        Dbghelp
)

set_target_properties(Editor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

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

vulkantoy_stage_sdl3(Editor)

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
