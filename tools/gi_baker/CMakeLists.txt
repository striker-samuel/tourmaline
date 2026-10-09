project(GiBaker)

add_executable(GiBaker main.cpp)

target_compile_definitions(GiBaker PRIVATE ENGINE_VULKAN)

target_link_libraries(GiBaker
    PRIVATE
        engine
        gltf_import
        volk
        KTX::ktx
)

set_target_properties(GiBaker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# KTX is a shared lib on this setup: stage it next to GiBaker.exe.
# OptickCore is SHARED via engine: stage it too.
add_custom_command(TARGET GiBaker POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:KTX::ktx>"
        "$<TARGET_FILE_DIR:GiBaker>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OptickCore>"
        "$<TARGET_FILE_DIR:GiBaker>"
)

vulkantoy_stage_sdl3(GiBaker)
