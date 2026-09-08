project(App)

file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
    src/*.cpp
    include/App/*.h
)

add_executable(App ${APP_SOURCES})

set(APP_DIR         "${CMAKE_CURRENT_SOURCE_DIR}")   # F:/REPOS/vulkantoy/app
set(APP_SHADERS_DIR "${APP_DIR}/content/shaders")
set(APP_INCLUDE_DIR "${APP_DIR}/include")

message(STATUS "APP_DIR = ${APP_DIR}")
message(STATUS "APP_SHADERS_DIR = ${APP_SHADERS_DIR}")
message(STATUS "APP_INCLUDE_DIR = ${APP_INCLUDE_DIR}")


add_custom_command(TARGET App POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:KTX::ktx>"
        "$<TARGET_FILE_DIR:App>"
)

message(STATUS "App source files: ${APP_SRC}")
find_package(Vulkan REQUIRED)
message(STATUS "Vulkan_FOUND: ${Vulkan_FOUND}")
message(STATUS "Vulkan_INCLUDE_DIRS: ${Vulkan_INCLUDE_DIRS}")
message(STATUS "Vulkan_LIBRARIES: ${Vulkan_LIBRARIES}")

set_target_properties(App PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)



add_custom_command(TARGET App POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:App>/content/textures
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:App>/content/models

    COMMENT "📦 Copying resource folders (shaders, textures, models) to App runtime directory"
)

# Copy resource dirs only if they exist in the source tree.
# (cmake -E copy_directory errors out when the source dir is missing,
# which would fail the whole POST_BUILD step.)
if(EXISTS "${APP_DIR}/content/textures")
    add_custom_command(TARGET App POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${APP_DIR}/content/textures"
                "$<TARGET_FILE_DIR:App>/content/textures"
    )
endif()

if(EXISTS "${APP_DIR}/content/models")
    add_custom_command(TARGET App POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${APP_DIR}/content/models"
                "$<TARGET_FILE_DIR:App>/content/models"
    )
endif()

# UI fonts (ImGui DPI-scaled TTF, see ImGuiOverlayDesc)
if(EXISTS "${APP_DIR}/content/fonts")
    add_custom_command(TARGET App POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${APP_DIR}/content/fonts"
                "$<TARGET_FILE_DIR:App>/content/fonts"
    )
endif()

# Cooked paks (PakCooker output; App prefers these over raw glTF)
if(EXISTS "${APP_DIR}/content/paks")
    add_custom_command(TARGET App POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${APP_DIR}/content/paks"
                "$<TARGET_FILE_DIR:App>/content/paks"
    )
endif()

# Engine-owned baked resources (light cookies, see tools/cookie_baker).
# Staged next to the executables' working dir; the app mounts it as the
# lowest-priority VFS backend (root dir name: "engine_resources").
if(EXISTS "${CMAKE_SOURCE_DIR}/engine/resources")
    add_custom_command(TARGET App POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/engine/resources"
                "$<TARGET_FILE_DIR:App>/engine_resources"
        COMMENT "Copying engine resources to $<TARGET_FILE_DIR:App>/engine_resources"
    )
endif()

target_include_directories(App
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include

)

target_compile_definitions(App PRIVATE ENGINE_VULKAN)

target_link_libraries(App
    PRIVATE
        engine
        engine
        KTX::ktx
        OptickCore
        #meshoptimizer
        #imgui_core
        #imgui_backend_glfw
        #imgui_backend_vulkan
        #Vulkan::Vulkan
        #nlohmann_json
)

# OptickCore is SHARED: stage its DLL next to App.exe
add_custom_command(TARGET App POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OptickCore>"
        "$<TARGET_FILE_DIR:App>"
)


# Shader compile outputs will be collected in this list
set(COMPILED_SHADER_OUTPUTS)

compile_shaders(
  "${APP_SHADERS_DIR}"
  "${CMAKE_BINARY_DIR}/content/shaders"
  App
  INCLUDE_DIRS
    "${APP_SHADERS_DIR}"
    "${APP_INCLUDE_DIR}"
    "${APP_DIR}"
)

# Ensure shaders are actually part of the build
if(COMPILED_SHADER_OUTPUTS)
    add_custom_target(compile_shaders_target ALL DEPENDS ${COMPILED_SHADER_OUTPUTS})
    add_dependencies(App compile_shaders_target)
else()
    message(WARNING "No shaders found to compile!")
endif()

# The app loads "content/shaders/*.spv" relative to its working directory,
# so the DXC outputs (in ${CMAKE_BINARY_DIR}/content/shaders) must be synced
# next to the exe like the other content dirs above. Without this, newly
# added shaders silently fail to load at runtime (readFile throws inside
# EnsureShaders -> builder stays invalid -> feature dead with no crash).
if(COMPILED_SHADER_OUTPUTS)
    add_custom_target(AppSyncShaders ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/content/shaders"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:App>/content/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_BINARY_DIR}/content/shaders"
                "$<TARGET_FILE_DIR:App>/content/shaders"
        COMMENT "Syncing compiled shaders next to App.exe"
    )
    add_dependencies(AppSyncShaders compile_shaders_target)
endif() 
