project(App)

file(GLOB_RECURSE APP_SRC CONFIGURE_DEPENDS
    src/*.cpp
    include/App/*.h
)

add_executable(App ${APP_SRC})

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
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:App>/textures
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:App>/models


    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/App/textures
            $<TARGET_FILE_DIR:App>/textures

    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/App/models
            $<TARGET_FILE_DIR:App>/models

    COMMENT "📦 Copying resource folders (shaders, textures, models) to App runtime directory"
)

target_include_directories(App
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(App
    PRIVATE
        engine
        KTX::ktx
        #Vulkan::Vulkan
        #nlohmann_json
)


