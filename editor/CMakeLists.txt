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
        # Reuses App's ABI-checked FrameUniforms + PushConstants
        # (common.hlsli layout, static_asserted in scene_renderer.h).
        ${CMAKE_SOURCE_DIR}/app/include
        # stb_image/stb_image_write (tourmaline logo generation + load).
        ${CMAKE_SOURCE_DIR}/app/vendor
)

target_link_libraries(Editor
    PRIVATE
        engine
        gltf_import
        KTX::ktx
        OptickCore
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

vulkantoy_stage_sdl3(Editor)

# The editor renders with the same compiled shaders as App
# (default.vert / default_unlit / default_lit / shadow.vert / ...).
# They are built by App's compile_shaders_target into
# ${CMAKE_BINARY_DIR}/content/shaders; sync them next to Editor.exe.
if(TARGET compile_shaders_target)
    add_dependencies(Editor compile_shaders_target)
endif()
add_custom_command(TARGET Editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Editor>/content/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_BINARY_DIR}/content/shaders"
            "$<TARGET_FILE_DIR:Editor>/content/shaders"
    COMMENT "Syncing compiled shaders next to Editor.exe"
)
