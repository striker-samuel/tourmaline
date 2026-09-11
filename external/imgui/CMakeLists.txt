# external/imgui/CMakeLists.txt
cmake_minimum_required(VERSION 3.16)

# Options so you can toggle pieces without touching callers
option(IMGUI_ENABLE_DEMO "Compile imgui_demo.cpp" OFF)
option(IMGUI_BACKEND_GLFW "Add the GLFW platform backend sources" ON)
option(IMGUI_BACKEND_SDL3 "Add the SDL3 platform backend sources" ON)
option(IMGUI_BACKEND_VULKAN "Add the Vulkan renderer backend sources" ON)
option(IMGUI_USE_VOLK "Define IMGUI_IMPL_VULKAN_NO_PROTOTYPES for volk" ON)


# Path to this folder (the ImGui repo root)
set(IMGUI_DIR ${CMAKE_CURRENT_LIST_DIR})

# ---- Core (no external deps) ----
set(IMGUI_CORE_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
)
if(IMGUI_ENABLE_DEMO)
    list(APPEND IMGUI_CORE_SOURCES ${IMGUI_DIR}/imgui_demo.cpp)
endif()

add_library(imgui_core STATIC ${IMGUI_CORE_SOURCES})
target_include_directories(imgui_core PUBLIC ${IMGUI_DIR})

# ---- Backends as INTERFACE (no forced link deps) ----
# GLFW backend just contributes sources/includes; you link GLFW yourself in your app/engine.
if(IMGUI_BACKEND_GLFW)
    add_library(imgui_backend_glfw INTERFACE)
    target_sources(imgui_backend_glfw INTERFACE
        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    )
    target_include_directories(imgui_backend_glfw INTERFACE
        ${IMGUI_DIR} ${IMGUI_DIR}/backends
    )
endif()

# SDL3 backend just contributes sources/includes; you link SDL3 yourself in your app/engine.
if(IMGUI_BACKEND_SDL3)
    add_library(imgui_backend_sdl3 INTERFACE)
    target_sources(imgui_backend_sdl3 INTERFACE
        ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    )
    target_include_directories(imgui_backend_sdl3 INTERFACE
        ${IMGUI_DIR} ${IMGUI_DIR}/backends
    )
endif()

# Vulkan backend contributes sources/includes; you link volk or Vulkan::Vulkan in your app/engine.
if(IMGUI_BACKEND_VULKAN)
     add_library(imgui_backend_vulkan INTERFACE)
     target_sources(imgui_backend_vulkan INTERFACE
         ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
     )
     target_include_directories(imgui_backend_vulkan INTERFACE
         ${IMGUI_DIR} ${IMGUI_DIR}/backends
     )
     if(IMGUI_USE_VOLK)
         # If you use volk, ImGui's Vulkan backend must not include prototypes
         target_compile_definitions(imgui_backend_vulkan INTERFACE IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
         target_compile_definitions(imgui_backend_vulkan INTERFACE IMGUI_IMPL_VULKAN_USE_VOLK)
     endif()

     target_compile_definitions(imgui_backend_vulkan INTERFACE IMGUI_IMPL_VULKAN_HAS_DYNAMIC_RENDERING)

 endif()

# Nice-to-have: hide these from install/export unless you want them
set_target_properties(imgui_core PROPERTIES EXPORT_NAME imgui_core)
if(TARGET imgui_backend_glfw)
    set_target_properties(imgui_backend_glfw PROPERTIES EXPORT_NAME imgui_backend_glfw)
endif()
if(TARGET imgui_backend_sdl3)
    set_target_properties(imgui_backend_sdl3 PROPERTIES EXPORT_NAME imgui_backend_sdl3)
endif()
if(TARGET imgui_backend_vulkan)
    set_target_properties(imgui_backend_vulkan PROPERTIES EXPORT_NAME imgui_backend_vulkan)
endif()
