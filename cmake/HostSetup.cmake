# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

# Shared HOST-EXE plumbing (App.exe in this repo + the game hosts that pin
# this tree as a git submodule): runtime DLL staging (SDL3, Streamline),
# the content-dir sync, the engine link + the scene-shader dependency.
#
# DUAL-INCLUDE CONTRACT: tourmaline's root includes this file once (all
# three functions land in the root scope, visible to app/); a GAME repo
# (F:/REPOS/asteroids) includes it from its submodule copy —
#   set(TOURMALINE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tourmaline)
#   add_subdirectory(${TOURMALINE_SOURCE_DIR} tourmaline)
#   include(${TOURMALINE_SOURCE_DIR}/cmake/HostSetup.cmake)
# — so the SAME definitions serve both (the functions resolve every path
# through TOURMALINE_SOURCE_DIR at CALL time in the caller's scope).

# Stage SDL3.dll next to an executable (engine links SDL3 PUBLIC, so every
# consumer exe carries the import; Windows loads the DLL from the exe dir).
function(tourmaline_stage_sdl3 TARGET)
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
# nvngx_dlss.dll is fetched separately (never in git) - see the README in
# external/streamline/bin/x64; the per-DLL EXISTS guard below warns + skips.
function(tourmaline_stage_streamline TARGET)
    set(_SL_BIN_DIR "${TOURMALINE_SOURCE_DIR}/external/streamline/bin/x64")
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

# The host-exe setup itself: runtime output dir, the staged DLLs above, the
# content-dir sync, compile defines, the engine link + the shader
# dependency. Per-host concerns (sources) stay in the caller's CMakeLists.
function(tourmaline_host_setup TGT)
    set_target_properties(${TGT} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )

    # KTX + OptickCore are SHARED: stage their DLLs next to the exe.
    add_custom_command(TARGET ${TGT} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:KTX::ktx>"
            "$<TARGET_FILE_DIR:${TGT}>"
    )
    add_custom_command(TARGET ${TGT} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:OptickCore>"
            "$<TARGET_FILE_DIR:${TGT}>"
    )

    # Runtime content layout next to the exe is always:
    #   content/models, content/paks, content/gi, content/fonts, content/textures, content/luts
    # Large binaries (models/, paks/, gi/) live OUTSIDE git in TOURMALINE_CONTENT_DIR
    # (default F:/content, overridable via -DTOURMALINE_CONTENT_DIR or the env var).
    # Small tracked dirs (fonts/, ...) fall back to the legacy app/content tree.
    if(DEFINED ENV{TOURMALINE_CONTENT_DIR})
        set(_TOURMALINE_CONTENT_DEFAULT "$ENV{TOURMALINE_CONTENT_DIR}")
    else()
        set(_TOURMALINE_CONTENT_DEFAULT "F:/content")
    endif()
    set(TOURMALINE_CONTENT_DIR "${_TOURMALINE_CONTENT_DEFAULT}" CACHE PATH
        "External binary content root (models/, paks/, ...). Not stored in git; synced next to the exe at build time.")
    message(STATUS "[${TGT}] TOURMALINE_CONTENT_DIR = ${TOURMALINE_CONTENT_DIR}")

    add_custom_command(TARGET ${TGT} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${TGT}>/content/textures
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${TGT}>/content/models
        COMMENT "[${TGT}] Copying resource folders (shaders, textures, models) to the runtime directory"
    )

    # Sync each content subdir from the external store (preferred) or the
    # legacy in-repo app/content tree (fallback). copy_directory creates
    # the destination, so a missing source simply skips that subdir.
    foreach(_CONTENT_SUB IN ITEMS models paks gi fonts textures luts)
        if(EXISTS "${TOURMALINE_CONTENT_DIR}/${_CONTENT_SUB}")
            set(_CONTENT_SRC "${TOURMALINE_CONTENT_DIR}/${_CONTENT_SUB}")
        elseif(EXISTS "${TOURMALINE_SOURCE_DIR}/app/content/${_CONTENT_SUB}")
            set(_CONTENT_SRC "${TOURMALINE_SOURCE_DIR}/app/content/${_CONTENT_SUB}")
        else()
            set(_CONTENT_SRC "")
            if(_CONTENT_SUB STREQUAL "models" OR _CONTENT_SUB STREQUAL "paks")
                message(WARNING "[${TGT}] Content '${_CONTENT_SUB}' not found in ${TOURMALINE_CONTENT_DIR} nor app/content - scenes needing it will fail at runtime.")
            endif()
        endif()
        if(_CONTENT_SRC)
            message(STATUS "[${TGT}] Content '${_CONTENT_SUB}' <- ${_CONTENT_SRC}")
            add_custom_command(TARGET ${TGT} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${_CONTENT_SRC}"
                        "$<TARGET_FILE_DIR:${TGT}>/content/${_CONTENT_SUB}"
            )
        endif()
    endforeach()

    # Deferred-shadow debug visualizers (paper 5.3/5.4): resolved by the root
    # CMakeLists (auto = Debug trees; TOURMALINE_DS_VIZ=ON forces them into
    # optimized builds — see the x64-Release-DSViz CMakeSettings preset). The
    # same define is passed to DXC (CompileShaders.cmake); clean release trees
    # compile the visualizer UI, uniform fields and shader paths out entirely.
    # Host TUs (PakApp.cpp) include the engine WorldRenderer header, so they
    # must see the same layout as the engine TUs.
    if(DS_VIZ_ON)
        target_compile_definitions(${TGT} PRIVATE DS_DEBUG_VIZ=1)
    endif()

    target_link_libraries(${TGT}
        PRIVATE
            engine
            gltf_import
            KTX::ktx
            OptickCore
    )

    # SDL3 is SHARED (vendored prebuilt): stage its DLL next to the exe.
    tourmaline_stage_sdl3(${TGT})

    # NVIDIA Streamline runtime (DLSS-SR): staged from external/streamline.
    # Optional at runtime - missing DLLs just disable the DLSS feature.
    tourmaline_stage_streamline(${TGT})

    # Scene shaders (engine/shaders -> content/shaders) are compiled by the
    # engine's EngineScene_shaders ALL target (see engine/CMakeLists.txt);
    # depend on it here so a shader edit rebuilds through the host targets.
    if(TARGET EngineScene_shaders)
        add_dependencies(${TGT} EngineScene_shaders)
        # Hosts load "content/shaders/*.spv" relative to their working
        # directory, so the DXC outputs (in ${CMAKE_BINARY_DIR}/content/
        # shaders) must be synced next to the exe like the other content
        # dirs above. Without this, newly added shaders silently fail to
        # load at runtime (readFile throws inside EnsureShaders -> builder
        # stays invalid -> feature dead with no crash).
        #
        # POST_BUILD (per-config, like the content-dir copies above): a
        # separate ALL target would only run for whichever config built it,
        # so a fresh tree of a NEW configuration (e.g. the first Release
        # build) compiled the SPVs but never copied them next to that
        # config's exe — every shader-referencing builder then failed at
        # runtime and the feature silently died (EVSM fell back to PCF in
        # Release exactly this way). POST_BUILD with the generator
        # expression resolves to each built config's output dir.
        add_custom_command(TARGET ${TGT} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${TGT}>/content/shaders"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_BINARY_DIR}/content/shaders"
                    "$<TARGET_FILE_DIR:${TGT}>/content/shaders"
            COMMENT "[${TGT}] Syncing compiled shaders next to the exe"
        )
    else()
        message(WARNING "[${TGT}] EngineScene_shaders target missing - scene shaders will not build!")
    endif()
endfunction()
