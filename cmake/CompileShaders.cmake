# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

if(DEFINED ENV{VULKAN_SDK})
  set(GLSLC "$ENV{VULKAN_SDK}/Bin/glslc.exe")
  set(DXC   "$ENV{VULKAN_SDK}/Bin/dxc.exe")
else()
  # No literal %VULKAN_SDK% — CMake never expands that. Fail fast at
  # configure time instead of emitting broken per-shader commands.
  message(FATAL_ERROR "[Shaders] VULKAN_SDK is not set: install the Vulkan SDK "
    "or set VULKAN_SDK to its root (contains Bin/dxc.exe).")
endif()

# Helper: map filename to stage + entry for HLSL
function(_detect_hlsl_stage_and_entry SHADER OUT_STAGE OUT_ENTRY OUT_PROFILE)
    get_filename_component(_name "${SHADER}" NAME)

    set(_stage "")
    if (_name MATCHES "\\.(vs|vert)\\.hlsl$")      # Vertex
        set(_stage "vs")
        set(_entry "mainVS")
        set(_profile "vs_6_7")
    elseif (_name MATCHES "\\.(ps|frag|fs)\\.hlsl$") # Pixel/Fragment
        set(_stage "ps")
        set(_entry "mainPS")
        set(_profile "ps_6_7")
    elseif (_name MATCHES "\\.(cs|comp)\\.hlsl$")  # Compute
        set(_stage "cs")
        set(_entry "mainCS")
        set(_profile "cs_6_7")
    elseif (_name MATCHES "\\.(gs|geom)\\.hlsl$")  # Geometry
        set(_stage "gs")
        set(_entry "mainGS")
        set(_profile "gs_6_7")
    elseif (_name MATCHES "\\.(hs|tesc)\\.hlsl$")  # Hull / tess control
        set(_stage "hs")
        set(_entry "mainHS")
        set(_profile "hs_6_7")
    elseif (_name MATCHES "\\.(ds|tese)\\.hlsl$")  # Domain / tess eval
        set(_stage "ds")
        set(_entry "mainDS")
        set(_profile "ds_6_7")
    elseif (_name MATCHES "\\.(rgen|rchit|rmiss|rahit|rint|rcall)\\.hlsl$") # Ray tracing
        set(_stage "rt")
        set(_entry "")              # entry via [shader("…")] attributes
        set(_profile "lib_6_7")
    elseif (_name MATCHES "\\.(as|task|amp)\\.hlsl$") # Amplification / task
        set(_stage "as")
        set(_entry "mainAS")
        set(_profile "as_6_7")
    elseif (_name MATCHES "\\.(ms|mesh)\\.hlsl$") # Mesh
        set(_stage "ms")
        set(_entry "mainMS")
        set(_profile "ms_6_7")
    else()
        # Fail closed: a mistyped suffix (e.g. *.mesh.hlsl vs *.ms.hlsl)
        # must not silently compile as the wrong stage.
        message(FATAL_ERROR "[Shaders] Unknown HLSL stage suffix for '${_name}': "
            "expected .vert/.frag/.cs/.gs/.hs/.ds/.rgen/.as/.ms.hlsl (or aliases).")
    endif()

    # Per-file overrides (optional)
    get_source_file_property(_entry_override   "${SHADER}" HLSL_ENTRY)
    if (_entry_override)
        set(_entry "${_entry_override}")
    endif()
    get_source_file_property(_profile_override "${SHADER}" HLSL_PROFILE)
    if (_profile_override)
        set(_profile "${_profile_override}")
    endif()

    set(${OUT_STAGE}   "${_stage}"   PARENT_SCOPE)
    set(${OUT_ENTRY}   "${_entry}"   PARENT_SCOPE)
    set(${OUT_PROFILE} "${_profile}" PARENT_SCOPE)
endfunction()

function(compile_shaders SOURCE_DIR BINARY_DIR TARGET_NAME)
  set(options)
  set(oneValueArgs)
  set(multiValueArgs INCLUDE_DIRS VARIANTS)
  cmake_parse_arguments(CS "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
  # Per-file -O3 pins for Debug builds (file names, no directory):
  #   set(HLSL_FORCE_O3_FILES "foo.cs.hlsl;bar.frag.hlsl")
  # before including this file to extend it. Defaults below cover shaders
  # whose UNOPTIMIZED (-Od) SPIR-V crashes a real driver compiler:
  #   - gfxfilter.cs.hlsl: the -Od blob (~155KB, heavy vk::BufferPointer
  #     traffic) AVs nvgpucomp64.dll inside vkCreateComputePipelines
  #     (Debug-only; the -O3 blob ~44KB compiles clean and Release runs
  #     fine). Pinned to the exact proven Release flags (-O3
  #     -Qstrip_debug), so this one shader is not RenderDoc-debuggable in
  #     Debug trees until the driver bug is fixed / worked around in-shader.
  #   - gfxbatchcull.cs.hlsl / gfxdrawcull.cs.hlsl: the GPU cull chain —
  #     the same BDA-heavy vk::BufferPointer traffic class (the batch cull
  #     also shares gfxfilter.hlsli's big struct bounds), pinned
  #     preemptively.
  if (NOT DEFINED HLSL_FORCE_O3_FILES)
    set(HLSL_FORCE_O3_FILES "gfxfilter.cs.hlsl;gfxbatchcull.cs.hlsl;gfxdrawcull.cs.hlsl;gfxskinbounds.cs.hlsl")
  endif()
  # Unpin escape hatch for driver re-tests: -DTOURMALINE_SHADER_OD_RETRY=ON
  # clears the -O3 pin so CI (tools/shader_od_retry.py) can retry the -Od
  # codegen that AVs nvgpucomp64.dll. Do NOT set this for normal builds —
  # a successful -Od pipeline compile still crashes affected drivers.
  # When the retry goes green on the pinned driver/DXC pair, delete the
  # file from HLSL_FORCE_O3_FILES (keep the comment trail).
  if (TOURMALINE_SHADER_OD_RETRY)
    set(HLSL_FORCE_O3_FILES "")
    message(STATUS "[Shaders] TOURMALINE_SHADER_OD_RETRY=ON: -O3 pin cleared, retrying -Od codegen")
  endif()
  # Normalize path (Windows-safe)
  file(TO_CMAKE_PATH "${SOURCE_DIR}" NORM_SOURCE_DIR)
  message(STATUS "NORM_SOURCE_DIR ${NORM_SOURCE_DIR}")

  if(NOT EXISTS "${NORM_SOURCE_DIR}")
    message(FATAL_ERROR "[Shaders] SOURCE_DIR does not exist: ${NORM_SOURCE_DIR}")
  endif()

  # Recurse and match HLSL/GLSL
  file(GLOB SHADERS CONFIGURE_DEPENDS 
    "${NORM_SOURCE_DIR}/*.hlsl"
    "${NORM_SOURCE_DIR}/*.vert" "${NORM_SOURCE_DIR}/*.frag" "${NORM_SOURCE_DIR}/*.comp"
    "${NORM_SOURCE_DIR}/*.geom" "${NORM_SOURCE_DIR}/*.tesc" "${NORM_SOURCE_DIR}/*.tese"
    "${NORM_SOURCE_DIR}/*.rgen" "${NORM_SOURCE_DIR}/*.rchit" "${NORM_SOURCE_DIR}/*.rmiss"
    "${NORM_SOURCE_DIR}/*.rahit" "${NORM_SOURCE_DIR}/*.rint" "${NORM_SOURCE_DIR}/*.rcall"
    "${NORM_SOURCE_DIR}/*.glsl"
    "${NORM_SOURCE_DIR}/*.vs.hlsl"   "${NORM_SOURCE_DIR}/*.vert.hlsl"
    "${NORM_SOURCE_DIR}/*.ps.hlsl"   "${NORM_SOURCE_DIR}/*.frag.hlsl" "${NORM_SOURCE_DIR}/*.fs.hlsl"
    "${NORM_SOURCE_DIR}/*.cs.hlsl"   "${NORM_SOURCE_DIR}/*.comp.hlsl"
    "${NORM_SOURCE_DIR}/*.gs.hlsl"   "${NORM_SOURCE_DIR}/*.geom.hlsl"
    "${NORM_SOURCE_DIR}/*.hs.hlsl"   "${NORM_SOURCE_DIR}/*.tesc.hlsl"
    "${NORM_SOURCE_DIR}/*.ds.hlsl"   "${NORM_SOURCE_DIR}/*.tese.hlsl"
    "${NORM_SOURCE_DIR}/*.rgen.hlsl" "${NORM_SOURCE_DIR}/*.rchit.hlsl"
    "${NORM_SOURCE_DIR}/*.rmiss.hlsl" "${NORM_SOURCE_DIR}/*.rahit.hlsl"
    "${NORM_SOURCE_DIR}/*.rint.hlsl" "${NORM_SOURCE_DIR}/*.rcall.hlsl"
    "${NORM_SOURCE_DIR}/*.as.hlsl"   "${NORM_SOURCE_DIR}/*.task.hlsl" "${NORM_SOURCE_DIR}/*.amp.hlsl"
    "${NORM_SOURCE_DIR}/*.ms.hlsl"   "${NORM_SOURCE_DIR}/*.mesh.hlsl"
  )

  message(STATUS "[Shaders] SOURCE_DIR = ${NORM_SOURCE_DIR}")

  # Include dependencies: DXC emits no .d include files, so track the shared
  # .hlsli headers coarsely – any header edit recompiles every shader that
  # could include it. Critical for common.hlsli struct layouts (FrameUniforms
  # / PushConstants C++-HLSL mirrors): stale SPIR-V silently reads shifted
  # offsets while the freshly-recompiled shaders use the new layout.
  file(GLOB_RECURSE SHADER_HEADERS CONFIGURE_DEPENDS
    "${NORM_SOURCE_DIR}/*.hlsli"
    "${NORM_SOURCE_DIR}/*.hlsl.h"
  )

  if(NOT SHADERS)
    message(WARNING "[Shaders] No shaders found to compile in: ${NORM_SOURCE_DIR}")
    # Print directory contents for diagnostics
    file(GLOB_RECURSE CONFIGURE_DEPENDS _ALL "${NORM_SOURCE_DIR}/*")
    if(_ALL)
      message(STATUS "[Shaders] Directory listing:")
      foreach(P ${_ALL})
        message(STATUS "  ${P}")
      endforeach()
    else()
      message(STATUS "[Shaders] Directory appears empty.")
    endif()
    return()
  endif()

  message(STATUS "[Shaders] Found shaders:")
  foreach(S ${SHADERS})
    message(STATUS "  ${S}")
  endforeach()

    foreach(SHADER ${SHADERS})
        get_filename_component(FILE_NAME "${SHADER}" NAME)
        set(OUTPUT_FILE "${BINARY_DIR}/${FILE_NAME}.spv")

        if (FILE_NAME MATCHES "\\.hlsl$")
            # --- HLSL via DXC → SPIR-V ---
            _detect_hlsl_stage_and_entry("${SHADER}" _STAGE _ENTRY _PROFILE)

            set(_COMMON
                -spirv
                -fspv-target-env=vulkan1.3   # bump to vulkan1.4 when you move SDK/driver
                -fvk-use-dx-layout           # packing contract: C++ mirrors assume DX layout (PushConstants tail, placement/sma pushes)
            )
            if (CMAKE_BUILD_TYPE MATCHES "Debug")
                # NOTE: do NOT use -fspv-debug=vulkan-with-source here. DXC
                # 1.9.5399 emits DebugLine/DebugLexicalBlock column ends past
                # the embedded source line length for some shaders (SMAA set),
                # and its internal spirv-val rejects it:
                # "generated SPIR-V is invalid ... Column End ... larger then
                # Line ... column length ... found in the DebugSource text"
                # (fatal error, no .spv emitted). -fspv-debug=vulkan keeps
                # line info without embedded source and validates clean.
                if (FILE_NAME IN_LIST HLSL_FORCE_O3_FILES)
                    # Driver-bug pin (see HLSL_FORCE_O3_FILES above): -Od
                    # output crashes the compiler, so take the proven
                    # Release codegen instead.
                    list(APPEND _COMMON -O3 -Qstrip_debug)
                else()
                    list(APPEND _COMMON -Od -Zi -Qembed_debug -fspv-debug=vulkan)
                endif()
            elseif (SHADER_DEBUG_ON)
                # Release + shader debug (x64-Release-ShaderDebug preset,
                # TOURMALINE_SHADER_DEBUG=ON): release -O3 shader codegen
                # with SPIR-V debug info instead of the stripped lean
                # modules — names/lines/source paths survive for RenderDoc
                # & friends. Same proven -Zi + -fspv-debug=vulkan pairing
                # as the Debug branch, just not -Od. (DXC forbids -Zi and
                # -Zs together; -Zi is the SPIR-V-embedding one.)
                list(APPEND _COMMON -O3 -Zi -fspv-debug=vulkan)
            else()
                list(APPEND _COMMON -O3 -Qstrip_debug)
            endif()

            # Deferred-shadow debug visualizers (paper 5.3/5.4): per-pixel
            # diagnostic paths, resolved by the root CMakeLists (auto = Debug
            # trees only; TOURMALINE_DS_VIZ=ON forces them into optimized
            # builds, e.g. the x64-Release-DSViz preset). Release presets
            # without the flag never see the code — zero shader cost.
            if (DS_VIZ_ON)
                list(APPEND _COMMON -DDS_DEBUG_VIZ=1)
            endif()

	        if (CS_INCLUDE_DIRS)
              foreach(INC ${CS_INCLUDE_DIRS})
                    list(APPEND _COMMON -I "${INC}")
                endforeach()
            endif()


            # Only pass -E if we actually have an entry (not for lib_6_7)
            set(_ENTRY_FLAGS "")
            if (NOT _ENTRY STREQUAL "")
                list(APPEND _ENTRY_FLAGS -E "${_ENTRY}")
            endif()

            add_custom_command(
                OUTPUT "${OUTPUT_FILE}"
                COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --cyan "⏳ DXC: ${FILE_NAME} [${_PROFILE}]"
                COMMAND "${DXC}" -T "${_PROFILE}" ${_ENTRY_FLAGS} ${_COMMON} -Fo "${OUTPUT_FILE}" "${SHADER}"
                COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --green "✅ DXC OK: ${FILE_NAME}"
                DEPENDS "${SHADER}" ${SHADER_HEADERS}
                COMMENT "Compiling HLSL ${FILE_NAME} → SPIR-V (Vulkan 1.3)"
                VERBATIM
            )
            # Fixed-count shader permutations (e.g. SG_LM_LOBE_COUNT):
            # CS_VARIANTS entries are "<basename>|<output suffix>|
            # DEFINE[=v][|...]", e.g.
            #   "default_lit.frag.hlsl|.sg6|SG_LM_LOBE_COUNT=6"
            # emits ${BINARY_DIR}/default_lit.frag.hlsl.sg6.spv compiled with
            # -DSG_LM_LOBE_COUNT=6 on top of the same flags/entry/profile as
            # the base output. Lets one source ship several baked-constant
            # programs the app binds per material range. HLSL/DXC only.
            foreach(_VSPEC ${CS_VARIANTS})
                string(REPLACE "|" ";" _VPARTS "${_VSPEC}")
                list(LENGTH _VPARTS _VLEN)
                if (_VLEN LESS 3)
                    message(FATAL_ERROR "[Shaders] malformed VARIANTS entry "
                        "(need file|suffix|DEFINE[=v]): ${_VSPEC}")
                endif()
                list(GET _VPARTS 0 _VFILE)
                list(GET _VPARTS 1 _VSUFFIX)
                if (NOT "${FILE_NAME}" STREQUAL "${_VFILE}")
                    continue()
                endif()
                set(_VARIANT_OUTPUT "${BINARY_DIR}/${FILE_NAME}${_VSUFFIX}.spv")
                set(_VARIANT_FLAGS "")
                math(EXPR _VLAST "${_VLEN} - 1")
                foreach(_VI RANGE 2 ${_VLAST})
                    list(GET _VPARTS ${_VI} _VDEF)
                    list(APPEND _VARIANT_FLAGS "-D${_VDEF}")
                endforeach()
                add_custom_command(
                    OUTPUT "${_VARIANT_OUTPUT}"
                    COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --cyan "⏳ DXC: ${FILE_NAME}${_VSUFFIX} [${_PROFILE}]"
                    COMMAND "${DXC}" -T "${_PROFILE}" ${_ENTRY_FLAGS} ${_COMMON} ${_VARIANT_FLAGS} -Fo "${_VARIANT_OUTPUT}" "${SHADER}"
                    COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --green "✅ DXC OK: ${FILE_NAME}${_VSUFFIX}"
                    DEPENDS "${SHADER}" ${SHADER_HEADERS}
                    COMMENT "Compiling HLSL ${FILE_NAME}${_VSUFFIX} → SPIR-V (Vulkan 1.3)"
                    VERBATIM
                )
                list(APPEND COMPILED_SHADERS "${_VARIANT_OUTPUT}")
            endforeach()
        else()
            # --- GLSL via glslc → SPIR-V ---
            set(_GLSLC_FLAGS --target-env=vulkan1.3)  # bump to vulkan1.4 when ready
            if (CMAKE_BUILD_TYPE MATCHES "Debug")
                list(APPEND _GLSLC_FLAGS -g)
            else()
                list(APPEND _GLSLC_FLAGS -O)
            endif()

            add_custom_command(
                OUTPUT "${OUTPUT_FILE}"
                COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --cyan "⏳ glslc: ${FILE_NAME}"
                COMMAND "${GLSLC}" ${_GLSLC_FLAGS} -o "${OUTPUT_FILE}" "${SHADER}"
                COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --green "✅ glslc OK: ${FILE_NAME}"
                DEPENDS "${SHADER}" ${SHADER_HEADERS}
                COMMENT "Compiling GLSL ${FILE_NAME} → SPIR-V (Vulkan 1.3)"
                VERBATIM
            )
        endif()

    list(APPEND COMPILED_SHADERS "${OUTPUT_FILE}")
    endforeach()

    add_custom_target(${TARGET_NAME}_shaders ALL DEPENDS ${COMPILED_SHADERS})
    set(COMPILED_SHADER_OUTPUTS ${COMPILED_SHADERS} PARENT_SCOPE)
endfunction()
