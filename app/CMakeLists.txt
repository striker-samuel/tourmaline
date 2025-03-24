project(App)

file(GLOB_RECURSE APP_SRC CONFIGURE_DEPENDS
    src/*.cpp
    include/App/*.h
)

add_executable(App ${APP_SRC})
message(STATUS "App source files: ${APP_SRC}")


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
        ${CMAKE_SOURCE_DIR}/external/tiny_obj_loader
        ${CMAKE_SOURCE_DIR}/external/tinygltf
        ${CMAKE_SOURCE_DIR}/external/stb_image
        ${nlohmann_json_SOURCE_DIR}/single_include/nlohmann
)

target_link_libraries(App
    PRIVATE
        engine
        #nlohmann_json
)


