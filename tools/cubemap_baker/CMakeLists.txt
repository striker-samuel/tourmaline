project(CubemapBaker)

# cube_png.cpp is shared with Cook (same debug-PNG module, same stb
# declarations; the STB implementation lives in Cook's lm_png.cpp,
# so CubemapBaker needs its own TU with the implementation).
add_executable(CubemapBaker main.cpp ../cook/src/cube_png.cpp)

target_include_directories(CubemapBaker PRIVATE ../cook/src ${CMAKE_SOURCE_DIR}/external/stb)

target_compile_definitions(CubemapBaker PRIVATE ENGINE_VULKAN)

target_link_libraries(CubemapBaker
    PRIVATE
        engine
        gltf_import
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
