project(Engine)

file(GLOB_RECURSE ENGINE_SRC CONFIGURE_DEPENDS
    src/*.cpp
    include/Engine/*.h
)

add_library(engine STATIC ${ENGINE_SRC})

add_compile_definitions(
    GLFW_INCLUDE_VULKAN,
    GLM_FORCE_RADIANS,
    GLM_ENABLE_EXPERIMENTAL
)

target_include_directories(engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(engine
    PUBLIC
        Vulkan::Vulkan
        glfw
        glm
)





