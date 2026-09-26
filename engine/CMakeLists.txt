project(Engine)

set(ENGINE_BACKEND "Vulkan")



file(GLOB_RECURSE ENGINE_SRC CONFIGURE_DEPENDS
    src/*.cpp
    include/Engine/*.h
)

add_library(engine STATIC ${ENGINE_SRC})

target_include_directories(engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/external/tiny_obj_loader
        ${CMAKE_SOURCE_DIR}/external/tinygltf
        ${nlohmann_json_SOURCE_DIR}/single_include
        ${nlohmann_json_SOURCE_DIR}/single_include/nlohmann
        ${CMAKE_SOURCE_DIR}/external/SPIRV-Reflect
        ${CMAKE_SOURCE_DIR}/external/SPIRV-Reflect/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Vulkan/detail
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RHI/detail
        ${CMAKE_SOURCE_DIR}/external/VulkanMemoryAllocator
        ${CMAKE_SOURCE_DIR}/external/streamline/include
)


target_compile_definitions(engine 
    PUBLIC
        GLM_FORCE_RADIANS
        GLM_FORCE_DEPTH_ZERO_TO_ONE
        GLM_ENABLE_EXPERIMENTAL
        TINYGLTF_NO_STB_IMAGE
        TINYGLTF_NO_STB_IMAGE_WRITE
        TINYGLTF_NO_EXTERNAL_IMAGE
        GLFW_INCLUDE_NONE
)

if(ENGINE_BACKEND STREQUAL "Vulkan")
    target_compile_definitions(engine PRIVATE ENGINE_VULKAN)
endif()

target_link_libraries(engine
    PUBLIC
        glfw
        SDL3::SDL3
        glm
        imgui_core
        meshoptimizer
        xatlas
        OptickCore
        spirv_reflect
    PRIVATE
        directxtex_bc
        volk
        KTX::ktx
        imgui_backend_glfw
        imgui_backend_sdl3
        imgui_backend_vulkan
        lz4
        winhttp
        bcrypt
)

# DXC -- shader compiling HLSL->SPIRV
set(DXC_DIR "${CMAKE_SOURCE_DIR}/external/dxc")

target_include_directories(engine PRIVATE
    "${DXC_DIR}/include"
)

target_link_libraries(engine PRIVATE
    "${DXC_DIR}/lib/x64/dxcompiler.lib"
)


add_custom_command(TARGET engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${DXC_DIR}/bin/x64/dxcompiler.dll"
        "$<TARGET_FILE_DIR:engine>"

    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${DXC_DIR}/bin/x64/dxil.dll"
        "$<TARGET_FILE_DIR:engine>"
)

# OptickCore builds SHARED upstream -> its DLL must sit next to the exe.
# $TARGET_FILE:OptickCore resolves to the DLL on Windows.
add_custom_command(TARGET engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OptickCore>"
        "$<TARGET_FILE_DIR:engine>"
)

# SDL3 is SHARED (vendored prebuilt) -> its DLL must sit next to the exe.
add_custom_command(TARGET engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SDL3::SDL3>"
        "$<TARGET_FILE_DIR:engine>"
)

# ---------------------------------------------------------------------------
# Engine-shipped shaders (engine/resources/shaders): ENGINE/EDITOR
# concerns, DISTINCT from the App scene set (engine/shaders, which
# compiles into content/shaders and loads relative to the working dir
# alongside user content). Engine shaders stage next to the
# executables as engine_resources/shaders — first inhabitant: the
# WYSIWYG compose blit (the editor's hosted-preview viewport pass;
# see editor_app.h). App + Editor both stage the outputs POST_BUILD.
# ---------------------------------------------------------------------------
compile_shaders(
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/shaders"
    "${CMAKE_BINARY_DIR}/engine_resources/shaders"
    EngineRes
    INCLUDE_DIRS
        "${CMAKE_SOURCE_DIR}/engine/shaders"
)
