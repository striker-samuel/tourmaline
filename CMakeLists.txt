cmake_minimum_required(VERSION 3.17)
project(VulkanToy LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Vulkan#
#--- Rendering backend ------------------------------------------------------

set(ENGINE_BACKEND "Vulkan" CACHE STRING "Rendering backend")
find_package(Vulkan REQUIRED)

include(FetchContent)

# MeshOptimizer
add_subdirectory(external/meshoptimizer)

# LZ4 (vendored block codec for cooked pak blobs)
add_subdirectory(external/lz4)

# GLFW (legacy windowing; retained for samples compat — engine uses SDL3)
add_subdirectory(external/glfw)

# SDL3 (vendored prebuilt dev package: include/ + lib/<arch>/ + cmake/)
# Provides windowing, input (keyboard/mouse/gamepad) and audio.
list(PREPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/external/SDL3-3.4.16")
set(SDL3_DIR "${CMAKE_SOURCE_DIR}/external/SDL3-3.4.16/cmake" CACHE PATH "SDL3 config dir" FORCE)
find_package(SDL3 CONFIG REQUIRED)

# Stage SDL3.dll next to an executable (engine links SDL3 PUBLIC, so every
# consumer exe carries the import; Windows loads the DLL from the exe dir).
function(vulkantoy_stage_sdl3 TARGET)
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL3::SDL3>"
            "$<TARGET_FILE_DIR:${TARGET}>"
        COMMENT "Staging SDL3.dll next to ${TARGET}"
    )
endfunction()

# Stage the NVIDIA Streamline runtime next to an executable (DLSS-SR).
# sl.interposer.dll is loaded dynamically from the exe dir (dlss.cpp);
# a missing set simply leaves DLSS unavailable - no hard dependency.
# Source of the binaries: external/streamline (Streamline SDK 2.14.1).
function(vulkantoy_stage_streamline TARGET)
    set(_SL_BIN_DIR "${CMAKE_SOURCE_DIR}/external/streamline/bin/x64")
    foreach(_SL_DLL IN ITEMS
            sl.interposer.dll sl.common.dll sl.dlss.dll nvngx_dlss.dll
            NvLowLatencyVk.dll)
        if(EXISTS "${_SL_BIN_DIR}/${_SL_DLL}")
            add_custom_command(TARGET ${TARGET} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_SL_BIN_DIR}/${_SL_DLL}"
                    "$<TARGET_FILE_DIR:${TARGET}>/${_SL_DLL}"
                COMMENT "Staging ${_SL_DLL} next to ${TARGET}"
            )
        else()
            message(WARNING "Streamline binary '${_SL_DLL}' not found in ${_SL_BIN_DIR} - DLSS will be unavailable at runtime")
        endif()
    endforeach()
    if(EXISTS "${_SL_BIN_DIR}/nvngx_dlss.license.txt")
        add_custom_command(TARGET ${TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_SL_BIN_DIR}/nvngx_dlss.license.txt"
                "$<TARGET_FILE_DIR:${TARGET}>/nvngx_dlss.license.txt"
            COMMENT "Staging nvngx_dlss.license.txt next to ${TARGET}"
        )
    endif()
endfunction()

# GLM (header-only)
add_subdirectory(external/glm)


# --- KTX hinting --------------------------------------------------------------
# Users can point CMake at KTX in several ways:
#  1) -Dktx_DIR=/path/to/lib/cmake/ktx
#  2) -DCMAKE_PREFIX_PATH=/path/to/prefix
#  3) Env var KTX_ROOT=/path/to/prefix  (we'll translate it to CMAKE_PREFIX_PATH)
#
# A "prefix" typically contains: include/, lib/, and lib/cmake/ktx/ktxConfig.cmake

if(NOT DEFINED KTX_ROOT AND DEFINED ENV{KTX_ROOT})
  set(KTX_ROOT "$ENV{KTX_ROOT}")
endif()

if(DEFINED KTX_ROOT)
  # Prepend so it wins over system installs
  list(PREPEND CMAKE_PREFIX_PATH "${KTX_ROOT}")
endif()

# Now try to locate the config package
find_package(ktx CONFIG REQUIRED)

if(NOT ktx_FOUND)
  message(FATAL_ERROR
    "KTX not found.\n"
    "Install KTX (KTX-Software) and make sure CMake can find ktxConfig.cmake.\n\n"
    "You can point CMake to it using ONE of:\n"
    "  - Set env var KTX_ROOT to the install prefix (contains include/ and lib/)\n"
    "  - Pass -DCMAKE_PREFIX_PATH=<prefix>\n"
    "  - Pass -Dktx_DIR=<prefix>/lib/cmake/ktx\n\n"
    "Example (PowerShell):\n"
    "  $env:KTX_ROOT='C:/SDKs/ktx'\n"
    "  cmake -S . -B build\n"
  )
endif()

# nlohmann::json (required for tinygltf)
add_subdirectory(external/json)
add_subdirectory(external/volk)

# --- Deferred-shadow debug visualizers (paper 5.3/5.4) -------------------------
# Per-pixel pipeline diagnostics: light culling steps, light-list membership,
# per-Shadow-Tile light counts (see engine/shaders/deferredshadow.hlsli +
# the "DS visualizers" ImGui block). Compiled in via DS_DEBUG_VIZ for the
# App target (app/CMakeLists.txt) and for DXC (CompileShaders.cmake).
#   unset/-D...="" -> auto: ON in Debug trees, OFF everywhere else
#                     (x64-Release stays clean, zero cost)
#   -DVULKANTOY_DS_VIZ=ON  -> force ON at any optimization level
#                     (the x64-Release-DSViz CMakeSettings preset: release
#                      -O2 C++ / -O3 shaders WITH visualizers)
#   -DVULKANTOY_DS_VIZ=OFF -> force OFF even in Debug trees
set(VULKANTOY_DS_VIZ "" CACHE BOOL
    "Deferred-shadow debug visualizers (empty = auto: Debug builds only)")
if(VULKANTOY_DS_VIZ)
  set(DS_VIZ_ON ON)
elseif(VULKANTOY_DS_VIZ STREQUAL "" AND CMAKE_BUILD_TYPE MATCHES "Debug")
  set(DS_VIZ_ON ON)
else()
  set(DS_VIZ_ON OFF)
endif()
message(STATUS "Deferred-shadow visualizers: ${DS_VIZ_ON}"
  " (VULKANTOY_DS_VIZ='${VULKANTOY_DS_VIZ}', build type '${CMAKE_BUILD_TYPE}')")

# --- Optick profiler ----------------------------------------------------------
option(ENABLE_OPTICK "Enable Optick CPU/GPU profiling" ON)
if(ENABLE_OPTICK)
  set(OPTICK_USE_VULKAN ON CACHE BOOL "" FORCE)
else()
  set(OPTICK_USE_VULKAN OFF CACHE BOOL "" FORCE)
endif()
set(OPTICK_USE_D3D12 OFF CACHE BOOL "" FORCE)
set(OPTICK_BUILD_GUI_APP OFF CACHE BOOL "" FORCE)
set(OPTICK_BUILD_CONSOLE_SAMPLE OFF CACHE BOOL "" FORCE)
set(OPTICK_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
if(NOT ENABLE_OPTICK)
  set(OPTICK_ENABLED OFF CACHE BOOL "" FORCE)
endif()
add_subdirectory(external/optick)

# Shader compile support
set(SHADER_SOURCE_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_BINARY_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_BINARY_DIR})

# Shader compilation rule (loaded from custom cmake file)
include(cmake/CompileShaders.cmake)

option(TRACE_INCLUDES "Print header include tree while compiling" OFF)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  if (TRACE_INCLUDES)
    add_compile_options(-H)        # GCC/Clang: show include stack
  endif()
elseif (MSVC)
  if (TRACE_INCLUDES)
    add_compile_options(/showIncludes)  # MSVC: show include stack
  endif()
endif()

# SPIRV-Reflect (vendored, CPU-only reflection for push/spec readback)
add_library(spirv_reflect STATIC external/SPIRV-Reflect/spirv_reflect.c)
target_include_directories(spirv_reflect PUBLIC
    ${CMAKE_SOURCE_DIR}/external/SPIRV-Reflect
    ${CMAKE_SOURCE_DIR}/external/SPIRV-Reflect/include
)
set_target_properties(spirv_reflect PROPERTIES C_STANDARD 11 LINKER_LANGUAGE C)

add_subdirectory(external/imgui)
add_subdirectory(engine)
add_subdirectory(app)
add_subdirectory(tools/pak_cooker)
add_subdirectory(tools/editor)
add_subdirectory(tools/cookie_baker)
add_subdirectory(tools/gi_baker)
add_subdirectory(tools/cubemap_baker)
add_subdirectory(samples/01_triangle)
add_subdirectory(samples/02_blur)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

set_property(CACHE ENGINE_BACKEND PROPERTY STRINGS
    "Vulkan"
)

if(ENGINE_BACKEND STREQUAL "Vulkan")
    target_compile_definitions(engine PRIVATE
        ENGINE_VULKAN=1
    )
else()
    message(FATAL_ERROR "Unsupported rendering backend: ${ENGINE_BACKEND}")
endif()

