cmake_minimum_required(VERSION 3.17)
project(VulkanToy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Vulkan
find_package(Vulkan REQUIRED)

include(FetchContent)

# GLFW
add_subdirectory(external/glfw)


# GLM (header-only)
add_subdirectory(external/glm)


# ASSIMP from local source
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)  # Let it use bundled zlib
# Or: OFF, if you provide your own ZLIB

add_subdirectory(external/assimp)

# nlohmann::json (required for tinygltf)
add_subdirectory(external/json)


# Shader compile support
set(SHADER_SOURCE_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_BINARY_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_BINARY_DIR})

# Shader compilation rule (loaded from custom cmake file)
include(cmake/CompileShaders.cmake)

add_subdirectory(engine)
add_subdirectory(app)
# Shader compile outputs will be collected in this list
set(COMPILED_SHADER_OUTPUTS)

# Call after all targets exist
compile_shaders(${CMAKE_SOURCE_DIR}/app/shaders ${CMAKE_BINARY_DIR}/shaders compile_shaders)

# Ensure shaders are actually part of the build
if(COMPILED_SHADER_OUTPUTS)
    add_custom_target(compile_shaders_target ALL DEPENDS ${COMPILED_SHADER_OUTPUTS})
    add_dependencies(App compile_shaders_target)
else()
    message(WARNING "No shaders found to compile!")
endif()
