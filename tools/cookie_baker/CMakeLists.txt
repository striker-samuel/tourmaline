project(CookieBaker)

add_executable(CookieBaker main.cpp)

target_compile_definitions(CookieBaker PRIVATE ENGINE_VULKAN)

target_link_libraries(CookieBaker
    PRIVATE
        engine
        volk
        KTX::ktx
)

set_target_properties(CookieBaker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# KTX is a shared lib on this setup: stage it next to CookieBaker.exe
# (same as App). OptickCore is SHARED via engine: stage it too.
add_custom_command(TARGET CookieBaker POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:KTX::ktx>"
        "$<TARGET_FILE_DIR:CookieBaker>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OptickCore>"
        "$<TARGET_FILE_DIR:CookieBaker>"
)

vulkantoy_stage_sdl3(CookieBaker)
