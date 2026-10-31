# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

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

tourmaline_stage_sdl3(CubemapBaker)

# Vulkan headers opt-in (the namespace/RHI boundary audit): this target
# includes <volk.h> / Vulkan headers directly - link the Linux
# full-header iface EXPLICITLY instead of relying on the engine's (now
# PRIVATE) propagation. Windows: the SDK headers via find_package.
if(TARGET vulkan_headers_iface)
    target_link_libraries(CubemapBaker PRIVATE vulkan_headers_iface)
endif()