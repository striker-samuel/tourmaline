project(App)

set(APP_DIR         "${CMAKE_CURRENT_SOURCE_DIR}")   # F:/REPOS/vulkantoy/app
set(ENGINE_SHADERS_DIR "${CMAKE_SOURCE_DIR}/engine/shaders")
set(APP_INCLUDE_DIR "${APP_DIR}/include")

message(STATUS "APP_DIR = ${APP_DIR}")
message(STATUS "ENGINE_SHADERS_DIR = ${ENGINE_SHADERS_DIR}")
message(STATUS "APP_INCLUDE_DIR = ${APP_INCLUDE_DIR}")

# ---------------------------------------------------------------------------
# appcore: the WorldRenderer ("classic world renderer pattern" — renders a
# host-provided world; no scene loading) as a static lib shared by App.exe
# and the Editor. The editor drives a HOSTED instance (offscreen render
# target composed into its WYSIWYG viewports — App::RequestHostedBoot);
# App installs a world source (SetWorldSource: main.cpp loads the pak)
# and runs it as its own window (Run()).
# ---------------------------------------------------------------------------
add_library(appcore STATIC
    src/world_renderer.cpp
    src/renderer.cpp
    src/forwardplus_renderer.cpp
    src/lut_io.cpp
)

target_include_directories(appcore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        # stb (lut_io PNG readback).
        ${CMAKE_SOURCE_DIR}/app/vendor
)

target_compile_definitions(appcore PRIVATE ENGINE_VULKAN)

if(DS_VIZ_ON)
    target_compile_definitions(appcore PRIVATE DS_DEBUG_VIZ=1)
endif()

target_link_libraries(appcore
    PUBLIC
        engine
    PRIVATE
        gltf_import
        KTX::ktx
        OptickCore
)

# ---------------------------------------------------------------------------
# Precompiled Header (PCH) for the App module.
# Uses CMake's built-in target_precompile_headers so it works with both
# MSBuild and Ninja (+ MSVC cl.exe, e.g. VS's x64-Debug Ninja config).
# No manual #include "pch.h" needed in the .cpps: CMake injects /Yu + /FI.
# Disable with -DVULKANTOY_PCH=OFF if the PCH ever masks a header bug.
# ---------------------------------------------------------------------------
option(VULKANTOY_PCH "Use precompiled headers for appcore/App" ON)
if(VULKANTOY_PCH)
    target_precompile_headers(appcore PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pch.h
    )
endif()

# ---------------------------------------------------------------------------
# App.exe: thin entry + appcore.
# ---------------------------------------------------------------------------
add_executable(App src/main.cpp)

set_target_properties(App PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)


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



# Runtime content layout next to App.exe is always:
#   content/models, content/paks, content/gi, content/fonts, content/textures, content/luts
# Large binaries (models/, paks/, gi/) live OUTSIDE git in VULKANTOY_CONTENT_DIR
# (default F:/content, overridable via -DVULKANTOY_CONTENT_DIR or the env var).
# Small tracked dirs (fonts/, ...) fall back to the legacy app/content tree.
if(DEFINED ENV{VULKANTOY_CONTENT_DIR})
    set(_VULKANTOY_CONTENT_DEFAULT "$ENV{VULKANTOY_CONTENT_DIR}")
else()
    set(_VULKANTOY_CONTENT_DEFAULT "F:/content")
endif()
set(VULKANTOY_CONTENT_DIR "${_VULKANTOY_CONTENT_DEFAULT}" CACHE PATH
    "External binary content root (models/, paks/, ...). Not stored in git; synced next to App.exe at build time.")
message(STATUS "VULKANTOY_CONTENT_DIR = ${VULKANTOY_CONTENT_DIR}")

function(_vulkantoy_resolve_content_dir NAME OUT_VAR)
    if(EXISTS "${VULKANTOY_CONTENT_DIR}/${NAME}")
        set(${OUT_VAR} "${VULKANTOY_CONTENT_DIR}/${NAME}" PARENT_SCOPE)
    elseif(EXISTS "${APP_DIR}/content/${NAME}")
        set(${OUT_VAR} "${APP_DIR}/content/${NAME}" PARENT_SCOPE)
    else()
        set(${OUT_VAR} "" PARENT_SCOPE)
    endif()
endfunction()

add_custom_command(TARGET App POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:App>/content/textures
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:App>/content/models

    COMMENT "📦 Copying resource folders (shaders, textures, models) to App runtime directory"
)

# Sync each content subdir from the external store (preferred) or the
# legacy in-repo tree (fallback). copy_directory creates the destination,
# so a missing source simply skips that subdir.
foreach(_CONTENT_SUB IN ITEMS models paks gi fonts textures luts)
    _vulkantoy_resolve_content_dir(${_CONTENT_SUB} _CONTENT_SRC)
    if(_CONTENT_SRC)
        message(STATUS "Content '${_CONTENT_SUB}' <- ${_CONTENT_SRC}")
        add_custom_command(TARGET App POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${_CONTENT_SRC}"
                    "$<TARGET_FILE_DIR:App>/content/${_CONTENT_SUB}"
        )
    elseif(_CONTENT_SUB STREQUAL "models" OR _CONTENT_SUB STREQUAL "paks")
        message(WARNING "Content '${_CONTENT_SUB}' not found in ${VULKANTOY_CONTENT_DIR} nor ${APP_DIR}/content - scenes needing it will fail at runtime.")
    endif()
endforeach()

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

# Engine-shipped SHADERS (engine/resources/shaders, compiled by the
# engine target into ${CMAKE_BINARY_DIR}/engine_resources/shaders):
# the WYSIWYG compose blit first. Stage next to App.exe like the
# engine resources above; the ALL target guarantees the spvs exist.
if(TARGET EngineRes_shaders)
    add_dependencies(App EngineRes_shaders)
    add_custom_command(TARGET App POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:App>/engine_resources/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_BINARY_DIR}/engine_resources/shaders"
                "$<TARGET_FILE_DIR:App>/engine_resources/shaders"
        COMMENT "Syncing engine-shipped shaders next to App.exe"
    )
endif()

target_include_directories(App
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include

)

target_compile_definitions(App PRIVATE ENGINE_VULKAN)

# Deferred-shadow debug visualizers (paper 5.3/5.4): resolved by the root
# CMakeLists (auto = Debug trees; VULKANTOY_DS_VIZ=ON forces them into
# optimized builds — see the x64-Release-DSViz CMakeSettings preset). The
# same define is passed to DXC (CompileShaders.cmake); clean release trees
# compile the visualizer UI, uniform fields and shader paths out entirely.
if(DS_VIZ_ON)
    target_compile_definitions(App PRIVATE DS_DEBUG_VIZ=1)
endif()

target_link_libraries(App
    PRIVATE
        appcore
        engine
        gltf_import
        KTX::ktx
        OptickCore
)

if(VULKANTOY_PCH)
    target_precompile_headers(App PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pch.h
    )
endif()

# OptickCore is SHARED: stage its DLL next to App.exe
add_custom_command(TARGET App POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OptickCore>"
        "$<TARGET_FILE_DIR:App>"
)

# SDL3 is SHARED (vendored prebuilt): stage its DLL next to App.exe
vulkantoy_stage_sdl3(App)

# NVIDIA Streamline runtime (DLSS-SR): staged from external/streamline.
# Optional at runtime - missing DLLs just disable the DLSS feature.
vulkantoy_stage_streamline(App)


# Shader compile outputs will be collected in this list
set(COMPILED_SHADER_OUTPUTS)

compile_shaders(
  "${ENGINE_SHADERS_DIR}"
  "${CMAKE_BINARY_DIR}/content/shaders"
  App
  INCLUDE_DIRS
    "${ENGINE_SHADERS_DIR}"
    "${APP_INCLUDE_DIR}"
    "${APP_DIR}"
  # Fixed-count lit permutations: the Main filtered leg binds the lit FS
  # matching each material range's cooked lobe count (see GfxRenderer::
  # FilteredLitProgram), so the shader's SG evaluation folds to literals.
  # Output: default_lit.frag.hlsl.sg{5,6,9,12}.spv next to the base module.
  VARIANTS
    "default_lit.frag.hlsl|.sg5|SG_LM_LOBE_COUNT=5"
    "default_lit.frag.hlsl|.sg6|SG_LM_LOBE_COUNT=6"
    "default_lit.frag.hlsl|.sg9|SG_LM_LOBE_COUNT=9"
    "default_lit.frag.hlsl|.sg12|SG_LM_LOBE_COUNT=12"
    # GI permutations (SPEC_GI_VARIANT specialization constant)
    "default_lit.frag.hlsl|.gi0|GI_NONE=1"
    "default_lit.frag.hlsl|.gi1|GI_OCTREE=1"
    "default_lit.frag.hlsl|.gi2|GI_REGULAR=1"
    "default_lit.frag.hlsl|.gi3|GI_FULL=1"
)

# compile_shaders() already emits the ALL target "App_shaders" over these
# outputs. Do NOT add a second ALL target over the same .spv files — two
# independent custom targets writing identical outputs race in MSBuild
# (MSB8066 "exited with code 1" on one of them, ~every shader change).
if(COMPILED_SHADER_OUTPUTS)
    add_dependencies(App App_shaders)
else()
    message(WARNING "No shaders found to compile!")
endif()

# The app loads "content/shaders/*.spv" relative to its working directory,
# so the DXC outputs (in ${CMAKE_BINARY_DIR}/content/shaders) must be synced
# next to the exe like the other content dirs above. Without this, newly
# added shaders silently fail to load at runtime (readFile throws inside
# EnsureShaders -> builder stays invalid -> feature dead with no crash).
if(COMPILED_SHADER_OUTPUTS)
    # Shader sync as an App POST_BUILD (per-config, like the content-dir
    # copies above): a separate ALL target would only run for whichever
    # config built it, so a fresh tree of a NEW configuration (e.g. the
    # first Release build) compiled the SPVs but never copied them next
    # to that config's App.exe — every shader-referencing builder then
    # failed at runtime and the feature silently died (EVSM fell back to
    # PCF in Release exactly this way). POST_BUILD with the generator
    # expression resolves to each built config's output dir.
    add_custom_command(TARGET App POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:App>/content/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_BINARY_DIR}/content/shaders"
                "$<TARGET_FILE_DIR:App>/content/shaders"
        COMMENT "Syncing compiled shaders next to App.exe"
    )
endif()
