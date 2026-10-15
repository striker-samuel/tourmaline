project(Cook)  # cook.exe: the COOK - artifacts + tour in, cooked artifacts out (GI/cubemaps/LODs/meshlets/de-instance). No pak write; pak.exe assembles.
# Cook as a library (the editor links it and calls cooktool::Run
# directly; the CLI stays a thin user interface over the same code).
# The lib build excludes main() via COOK_LIB_BUILD.
add_library(CookLib STATIC src/main.cpp src/lm_denoise.cpp src/lm_png.cpp src/cube_png.cpp)
target_compile_definitions(CookLib PRIVATE ENGINE_VULKAN COOK_LIB_BUILD)
target_include_directories(CookLib
PRIVATE
${CMAKE_SOURCE_DIR}/external/stb
${CMAKE_SOURCE_DIR}/tools/common
PUBLIC
${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(CookLib
PRIVATE
engine
gltf_import
artifact_cache
volk
OptickCore
)
add_executable(Cook src/main.cpp src/lm_denoise.cpp src/lm_png.cpp src/cube_png.cpp)
target_compile_definitions(Cook PRIVATE ENGINE_VULKAN)
# stb_image_write (cook-time atlas debug PNG, see lm_png.cpp; the same
# vendored header the editor uses for its PNG export).
target_include_directories(Cook PRIVATE ${CMAKE_SOURCE_DIR}/external/stb ${CMAKE_SOURCE_DIR}/tools/common)
target_link_libraries(Cook
PRIVATE
engine
gltf_import
artifact_cache
volk
OptickCore
)
set_target_properties(Cook PROPERTIES
RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
# ---- Intel Open Image Denoise (prebuilt, lightmap denoising) -----------
# Same prebuilt OIDN the pak cooker used: denoising is a cook-time pass
# over the fitted SG atlas amplitudes (see lm_denoise.cpp). Missing
# binaries only disable the feature (--lm-denoise prints a rebuild hint).
option(VULKANTOY_OIDN_COOK "Link prebuilt Intel OIDN into Cook (lightmap denoising)" ON)
set(OIDN_PREBUILT_ROOT ${CMAKE_SOURCE_DIR}/external/oidn)
if(VULKANTOY_OIDN_COOK AND EXISTS ${OIDN_PREBUILT_ROOT}/lib/OpenImageDenoise.lib
AND EXISTS ${OIDN_PREBUILT_ROOT}/include/OpenImageDenoise/oidn.h)
message(STATUS "Cook: OIDN lightmap denoising enabled (prebuilt external/oidn)")
target_compile_definitions(CookLib PRIVATE VULKANTOY_OIDN=1)
target_include_directories(CookLib PRIVATE ${OIDN_PREBUILT_ROOT}/include)
target_link_libraries(CookLib PRIVATE ${OIDN_PREBUILT_ROOT}/lib/OpenImageDenoise.lib)
target_compile_definitions(Cook PRIVATE VULKANTOY_OIDN=1)
target_include_directories(Cook PRIVATE ${OIDN_PREBUILT_ROOT}/include)
target_link_libraries(Cook PRIVATE ${OIDN_PREBUILT_ROOT}/lib/OpenImageDenoise.lib)
# Stage the CPU runtime set next to Cook.exe (OIDN discovers its
# device plugins relative to the loader DLL's directory).
foreach(OIDN_DLL OpenImageDenoise.dll OpenImageDenoise_core.dll
OpenImageDenoise_device_cpu.dll tbb12.dll)
add_custom_command(TARGET Cook POST_BUILD
COMMAND ${CMAKE_COMMAND} -E copy_if_different
"${OIDN_PREBUILT_ROOT}/bin/${OIDN_DLL}"
"$<TARGET_FILE_DIR:Cook>/${OIDN_DLL}"
COMMENT "Staging ${OIDN_DLL} next to Cook"
)
endforeach()
endif()
