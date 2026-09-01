project(Engine)

set(ENGINE_BACKEND "Vulkan")



file(GLOB_RECURSE ENGINE_SRC CONFIGURE_DEPENDS
    src/*.cpp
    include/Engine/*.h
    ${CMAKE_SOURCE_DIR}/external/imgui/backends/imgui_impl_glfw.cpp
    ${CMAKE_SOURCE_DIR}/external/imgui/backends/imgui_impl_vulkan.cpp
)

add_library(engine STATIC ${ENGINE_SRC})

target_include_directories(engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/external/tiny_obj_loader
        ${CMAKE_SOURCE_DIR}/external/tinygltf
        ${nlohmann_json_SOURCE_DIR}/single_include/nlohmann
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Vulkan/detail
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RHI/detail
        ${CMAKE_SOURCE_DIR}/external/VulkanMemoryAllocator
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
        glm
        imgui_core
        imgui_backend_glfw
        meshoptimizer
    PRIVATE
        volk
        KTX::ktx
        imgui_backend_vulkan
        #OptickCore
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
