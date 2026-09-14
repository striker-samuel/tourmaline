project(CubemapBaker)

add_executable(CubemapBaker main.cpp)

target_compile_definitions(CubemapBaker PRIVATE ENGINE_VULKAN)

target_link_libraries(CubemapBaker
    PRIVATE
        engine
        volk
        KTX::ktx
)

set_target_properties(CubemapBaker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# KTX is a shared lib on this setup: stage it next to CubemapBaker.exe
# (same as App). OptickCore is SHARED via engine: stage it too.
add_custom_command(TARGET CubemapBaker POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:KTX::ktx>"
        "$<TARGET_FILE_DIR:CubemapBaker>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OptickCore>"
        "$<TARGET_FILE_DIR:CubemapBaker>"
)

vulkantoy_stage_sdl3(CubemapBaker)
