cmake_minimum_required(VERSION 3.17)
project(VulkanToy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Vulkan
find_package(Vulkan REQUIRED)

include(FetchContent)

# GLFW

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.3.8
)
FetchContent_MakeAvailable(glfw)

# GLM (header-only)
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        0.9.9.8  # or latest stable tag
)
FetchContent_MakeAvailable(glm)

# ASSIMP (compiled C++ lib)
FetchContent_Declare(
    assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG        v5.2.5  # or latest stable
)
FetchContent_MakeAvailable(assimp)


# nlohmann::json (required for tinygltf)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    #GIT_TAG        v3.11.2  # or latest stable
    GIT_TAG        master
)
FetchContent_MakeAvailable(nlohmann_json)

# Shader compile support
set(SHADER_SOURCE_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_BINARY_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_BINARY_DIR})

# Shader compilation rule (loaded from custom cmake file)
include(cmake/CompileShaders.cmake)

add_subdirectory(engine)
add_subdirectory(app)

# After App target exists
compile_shaders(${CMAKE_SOURCE_DIR}/App/shaders ${CMAKE_BINARY_DIR}/shaders compile_shaders)
add_custom_target(compile_shaders_target ALL DEPENDS ${COMPILED_SHADER_OUTPUTS})
add_dependencies(App compile_shaders_target)
