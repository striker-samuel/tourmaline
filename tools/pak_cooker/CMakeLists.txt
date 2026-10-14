project(Pak)  # pak.exe: the PAK COOK - consumed artifacts only (raw glTF never cooks; import.exe / the MeshImporter produce the artifacts)

add_executable(Pak main.cpp lm_denoise.cpp lm_png.cpp cube_png.cpp)

target_compile_definitions(Pak PRIVATE ENGINE_VULKAN)

# stb_image_write (cook-time atlas debug PNG, see lm_png.cpp; the same
# vendored header the editor uses for its PNG export).
target_include_directories(Pak PRIVATE ${CMAKE_SOURCE_DIR}/external/stb)

target_link_libraries(Pak
    PRIVATE
        engine
        gltf_import
        artifact_cache
        volk
        OptickCore
)

set_target_properties(Pak PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ---- Intel Open Image Denoise (prebuilt, lightmap denoising) -----------
# Links the prebuilt OIDN release living in external/oidn (bin/ + lib/ +
# include/; the source tree next to them is the reference checkout). The
# engine and App NEVER link OIDN: denoising is a cook-time pass over the
# fitted SG atlas amplitudes (see lm_denoise.cpp). Missing binaries only
# disable the feature (--lm-denoise prints a rebuild hint).
option(VULKANTOY_OIDN "Link prebuilt Intel OIDN into Pak (lightmap denoising)" ON)
set(OIDN_PREBUILT_ROOT ${CMAKE_SOURCE_DIR}/external/oidn)
if(VULKANTOY_OIDN AND EXISTS ${OIDN_PREBUILT_ROOT}/lib/OpenImageDenoise.lib
                  AND EXISTS ${OIDN_PREBUILT_ROOT}/include/OpenImageDenoise/oidn.h)
    message(STATUS "Pak: OIDN lightmap denoising enabled (prebuilt external/oidn)")
    target_compile_definitions(Pak PRIVATE VULKANTOY_OIDN=1)
    target_include_directories(Pak PRIVATE ${OIDN_PREBUILT_ROOT}/include)
    target_link_libraries(Pak PRIVATE ${OIDN_PREBUILT_ROOT}/lib/OpenImageDenoise.lib)
    # Stage the CPU runtime set next to Pak.exe (OIDN discovers its
    # device plugins relative to the loader DLL's directory).
    foreach(OIDN_DLL OpenImageDenoise.dll OpenImageDenoise_core.dll
                     OpenImageDenoise_device_cpu.dll tbb12.dll)
        add_custom_command(TARGET Pak POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${OIDN_PREBUILT_ROOT}/bin/${OIDN_DLL}"
                "$<TARGET_FILE_DIR:Pak>/${OIDN_DLL}"
            COMMENT "Staging ${OIDN_DLL} next to Pak"
        )
    endforeach()
elseif(VULKANTOY_OIDN)
    message(WARNING
        "Pak: prebuilt OIDN not found under external/oidn "
        "(want bin/OpenImageDenoise*.dll + lib/OpenImageDenoise.lib + include/) "
        "- lightmap denoising is DISABLED. Extract the oidn-<ver>.x64.windows "
        "release zip into external/oidn and reconfigure.")
endif()

# KTX is a shared lib on this setup: stage it next to Pak.exe
# (the --cubemap stage encodes KTX2 in-process, like CubemapBaker).
add_custom_command(TARGET Pak POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:KTX::ktx>"
        "$<TARGET_FILE_DIR:Pak>"
)

# OptickCore is SHARED: stage its DLL next to Pak.exe
add_custom_command(TARGET Pak POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OptickCore>"
        "$<TARGET_FILE_DIR:Pak>"
)

vulkantoy_stage_sdl3(Pak)
