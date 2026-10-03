project(Editor)

file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS
    src/*.cpp
    src/*.h
)

add_executable(Editor ${EDITOR_SOURCES})

target_compile_definitions(Editor PRIVATE ENGINE_VULKAN)

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
)

target_link_libraries(Editor
    PRIVATE
        engine
        gltf_import
        artifact_cache
        KTX::ktx
        OptickCore
        # svg_icons.cpp: imgui_impl_vulkan.h pulls <vulkan/vulkan.h>
        # (the backend TU itself is compiled into the engine).
        Vulkan::Vulkan
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

# Engine-shipped shaders (WYSIWYG compose blit lives in
# engine/resources/shaders): same staging as App — engine_resources/
# shaders next to the exe. The hosted WorldRenderer loads the scene
# set from content/shaders; the editor's OWN passes (compose) load
# from engine_resources/shaders.
if(TARGET EngineRes_shaders)
    add_dependencies(Editor EngineRes_shaders)
    add_custom_command(TARGET Editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:Editor>/engine_resources/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_BINARY_DIR}/engine_resources/shaders"
                "$<TARGET_FILE_DIR:Editor>/engine_resources/shaders"
        COMMENT "Syncing engine-shipped shaders next to Editor.exe"
    )
endif()
