project(PakCooker)

add_executable(PakCooker main.cpp)

target_compile_definitions(PakCooker PRIVATE ENGINE_VULKAN)

target_link_libraries(PakCooker
    PRIVATE
        engine
)

set_target_properties(PakCooker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# OptickCore is SHARED: stage its DLL next to PakCooker.exe
add_custom_command(TARGET PakCooker POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OptickCore>"
        "$<TARGET_FILE_DIR:PakCooker>"
)

vulkantoy_stage_sdl3(PakCooker)
