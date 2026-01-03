cmake_minimum_required(VERSION 3.14)

#set(CMAKE_C_COMPILER_WORKS TRUE)
#set(CMAKE_CXX_COMPILER_WORKS TRUE)

project(SamRendererMayaPlugin)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set Maya location here!
set(MAYA_LOCATION "C:/Program Files/Autodesk/Maya2022")  # <-- Update this path for your system

# Set include and lib paths
link_directories("${MAYA_LOCATION}/lib")

file(GLOB_RECURSE PLUGIN_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

include_directories(
    "${MAYA_LOCATION}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

message(STATUS "Sources: ${PLUGIN_SRC}")


if (WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "You're building for x86 (32-bit). Maya only supports x64 plugins.")
endif()


if(WIN32)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
    add_definitions(-DNOMINMAX)
    set(MAYA_LIB_PATH "${MAYA_LOCATION}/lib")
    set(PLUGIN_EXTENSION ".mll")
elseif(APPLE)
    set(MAYA_LIB_PATH "${MAYA_LOCATION}/MacOS")
    set(PLUGIN_EXTENSION ".bundle")
elseif(UNIX)
    set(MAYA_LIB_PATH "${MAYA_LOCATION}/lib")
    set(PLUGIN_EXTENSION ".so")
endif()

# 👇 Aquí es donde definís tu plugin como biblioteca compartida
add_library(${PROJECT_NAME} SHARED ${PLUGIN_SRC})
link_directories("${MAYA_LIB_PATH}")

target_link_libraries(${PROJECT_NAME}
    OpenMaya
    OpenMayaAnim
    Foundation
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    SUFFIX ${PLUGIN_EXTENSION}
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plug-ins"
)
# Replace this with your desired output path
set(MAYA_PLUGIN_DEPLOY_DIR "C:/Users/smartin/Documents/DEVELOPMENT/maya/modules_content/core_mayakit/plug-ins")

add_custom_command(TARGET ${PROJECT_NAME}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:${PROJECT_NAME}>
        "${MAYA_PLUGIN_DEPLOY_DIR}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
    COMMENT "Copying ${PROJECT_NAME}${PLUGIN_EXTENSION} to Maya plug-ins folder"
)
