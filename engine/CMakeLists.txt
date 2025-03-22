project(Engine)

file(GLOB_RECURSE ENGINE_SRC CONFIGURE_DEPENDS
    src/*.cpp
    include/Engine/*.h
)

add_library(engine STATIC ${ENGINE_SRC})

target_include_directories(engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(engine
    PUBLIC
        Vulkan::Vulkan
        glfw
)

# Add shaders as a dependency
add_custom_target(compile_shaders ALL)
compile_shaders(${SHADER_SOURCE_DIR} ${SHADER_BINARY_DIR} compile_shaders)
add_dependencies(engine compile_shaders)
