project(blur_sample)

set(SAMPLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

message(STATUS "BLUR_SAMPLE_DIR = ${SAMPLE_DIR}")

file(GLOB SAMPLE_SOURCES CONFIGURE_DEPENDS
    ${SAMPLE_DIR}/*.cpp
)

add_executable(02_blur ${SAMPLE_SOURCES})

add_custom_command(TARGET 02_blur POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:KTX::ktx>"
        "$<TARGET_FILE_DIR:02_blur>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SAMPLE_DIR}/shoes.png"
        "$<TARGET_FILE_DIR:02_blur>/shoes.png"
)

message(STATUS "Blur source files: ${SAMPLE_SOURCES}")
find_package(Vulkan REQUIRED)
message(STATUS "Vulkan_FOUND: ${Vulkan_FOUND}")

set_target_properties(02_blur PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

target_include_directories(02_blur
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(02_blur
    PRIVATE
        engine
        KTX::ktx
)
