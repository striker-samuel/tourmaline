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

# Stage the SDL3 runtime next to an executable. Windows: the vendored
# prebuilt SDL3.dll (engine links SDL3 PUBLIC, so every consumer exe
# carries the import; Windows loads the DLL from the exe dir). Linux:
# the built .so (vendored source build or system install) - staging it
# next to the exe + the $ORIGIN rpath below keeps the vendored build
# loadable without a system install.
# The genex names the REAL imported target (SDL3::SDL3-shared), not the
# config's SDL3::SDL3 ALIAS: aliases stay directory-scoped forever, and this
# staging runs in GAME-HOST scopes too (this file's dual-include contract)
# where only the promoted IMPORTED_GLOBAL target is visible. When the
# shared target is not exposed (source-build configs name it differently,
# static-only installs), fall back to the SDL3::SDL3 alias.
function(tourmaline_stage_sdl3 TARGET)
    if(TARGET SDL3::SDL3-shared)
        set(_SDL3_STAGE_FILE "$<TARGET_FILE:SDL3::SDL3-shared>")
    elseif(TARGET SDL3::SDL3)
        set(_SDL3_STAGE_FILE "$<TARGET_FILE:SDL3::SDL3>")
    else()
        message(WARNING "tourmaline_stage_sdl3: no SDL3 target to stage")
        return()
    endif()
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_SDL3_STAGE_FILE}"
            "$<TARGET_FILE_DIR:${TARGET}>"
        COMMENT "Staging the SDL3 runtime next to ${TARGET}"
    )
endfunction()

# Stage the NVIDIA Streamline runtime next to an executable (DLSS-SR).
# sl.interposer.dll is loaded dynamically from the exe dir (dlss.cpp);
# a missing set simply leaves DLSS unavailable - no hard dependency.
# Source of the binaries: external/streamline (Streamline SDK 2.14.1).
# nvngx_dlss.dll is fetched separately (never in git) - see the README in
# external/streamline/bin/x64; the per-DLL EXISTS guard below warns + skips.
function(tourmaline_stage_streamline TARGET)
    if(NOT WIN32)
        # The Streamline runtime set (sl.interposer.dll + nvngx_*.dll) is
        # Windows-only; the engine's dlss backend reports unavailable on
        # Linux by design (see engine/src/Renderer/dlss.cpp). Nothing to
        # stage here.
        message(STATUS "[${TARGET}] Streamline staging skipped (Windows-only runtime; DLSS stays unavailable)")
        return()
    endif()
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

    # Linux: staged .so's (SDL3 / OptickCore / libdxcompiler / libktx) live
    # next to the exe, so the exe needs $ORIGIN in its rpath to find them
    # without LD_LIBRARY_PATH. (Windows resolves staged DLLs from the exe
    # dir by default - no equivalent needed.)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_target_properties(${TGT} PROPERTIES
            BUILD_RPATH "\$ORIGIN"
            INSTALL_RPATH "\$ORIGIN"
        )
    endif()

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

    # Runtime content next to the exe: ONLY the small engine-read
    # resources — content/fonts (the UI font, Roboto) + content/luts
    # (the tonemap LUT strip) — plus the compiled content/shaders (the
    # DXC sync below). The BIG binary store (models/, paks/, gi/,
    # textures/) is deliberately NOT synced: the entry-point pak is
    # EXPLICIT (App --pak <path> is REQUIRED — no default, nothing
    # auto-runs) and everything a scene needs ships inside the pak
    # (meshes, materials, textures, the giAsset-bound GI payloads).
    # Populating content/paks with every pak from an external store
    # just re-materialized 500+ MB of stale content into every build
    # dir (knight/asteroids/banner paks that had nothing to do with the
    # tree being built). The resources live IN THE REPO
    # (app/content/fonts + app/content/luts — small, tracked, licensed
    # — see THIRD_PARTY_NOTICES.md); there is no external content-root
    # knob anymore (TOURMALINE_CONTENT_DIR is gone: it only ever fed
    # this sync, and the store never carried fonts/luts anyway).
    # copy_directory creates the destination.
    foreach(_CONTENT_SUB IN ITEMS fonts luts)
        if(EXISTS "${TOURMALINE_SOURCE_DIR}/app/content/${_CONTENT_SUB}")
            set(_CONTENT_SRC "${TOURMALINE_SOURCE_DIR}/app/content/${_CONTENT_SUB}")
        else()
            set(_CONTENT_SRC "")
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
        # THE ENGINE SHADER PACKAGE (engine-shaders.pak, built by
        # EngineScene_shaderpak over the declared compiled-output list
        # - see tools/pak_cooker/CMakeLists.txt): deployed next to
        # every host; the engine shader library mounts it at boot as
        # the MANDATORY production shader source. The loose sync above
        # stays the DEV override (a recompiled shader serves
        # immediately); the package is what a deployed tree ships.
        if(TARGET EngineScene_shaderpak)
            add_dependencies(${TGT} EngineScene_shaderpak)
            add_custom_command(TARGET ${TGT} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${ENGINE_SHADER_PAK}"
                        "$<TARGET_FILE_DIR:${TGT}>/engine-shaders.pak"
                COMMENT "[${TGT}] Deploying engine-shaders.pak next to the exe"
            )
        endif()
    else()
        message(WARNING "[${TGT}] EngineScene_shaders target missing - scene shaders will not build!")
    endif()
endfunction()
