project(Engine)

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
        ${CMAKE_SOURCE_DIR}/external/stb_image
        ${nlohmann_json_SOURCE_DIR}/single_include/nlohmann
        ${CMAKE_SOURCE_DIR}/external/VulkanMemoryAllocator
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
)





