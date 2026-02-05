project(Engine)

file(GLOB_RECURSE ENGINE_SRC CONFIGURE_DEPENDS
    src/*.cpp
    include/Engine/*.h
    ${CMAKE_SOURCE_DIR}/external/imgui/backends/imgui_impl_glfw.cpp
    ${CMAKE_SOURCE_DIR}/external/imgui/backends/imgui_impl_vulkan.cpp
)

add_library(engine STATIC ${ENGINE_SRC} "include/Engine/platform.h" "include/Engine/Renderer/renderer.h")

target_include_directories(engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/external/tiny_obj_loader
        ${CMAKE_SOURCE_DIR}/external/tinygltf
        ${nlohmann_json_SOURCE_DIR}/single_include/nlohmann
        ${CMAKE_SOURCE_DIR}/external/VulkanMemoryAllocator
)


target_compile_definitions(engine 
    PUBLIC
        GLM_FORCE_RADIANS
        GLM_FORCE_DEPTH_ZERO_TO_ONE
        GLM_ENABLE_EXPERIMENTAL


        TINYGLTF_IMPLEMENTATION
        #STB_IMAGE_IMPLEMENTATION
        #STB_IMAGE_WRITE_IMPLEMENTATION

        TINYGLTF_NO_STB_IMAGE
        TINYGLTF_NO_STB_IMAGE_WRITE
        TINYGLTF_NO_EXTERNAL_IMAGE
        GLFW_INCLUDE_NONE
)

target_link_libraries(engine
    PUBLIC
        glfw
        glm
        volk
        KTX::ktx
        imgui_core
        imgui_backend_glfw
        imgui_backend_vulkan
        meshoptimizer
)





