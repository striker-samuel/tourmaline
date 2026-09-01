if(DEFINED ENV{VULKAN_SDK})
  set(GLSLC "$ENV{VULKAN_SDK}/Bin/glslc.exe")
  set(DXC   "$ENV{VULKAN_SDK}/Bin/dxc.exe")
else()
  set(GLSLC "%VULKAN_SDK%/Bin/glslc.exe")
  set(DXC   "%VULKAN_SDK%/Bin/dxc.exe")
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
    else()
        # Fallback: assume vertex
        set(_stage "vs")
        set(_entry "mainVS")
        set(_profile "vs_6_7")
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
  set(multiValueArgs INCLUDE_DIRS)
  cmake_parse_arguments(CS "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
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
  )

  message(STATUS "[Shaders] SOURCE_DIR = ${NORM_SOURCE_DIR}")

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
                -fvk-use-dx-layout
            )
            if (CMAKE_BUILD_TYPE MATCHES "Debug")
                list(APPEND _COMMON -Od -Zi -Qembed_debug -fspv-debug=vulkan-with-source)
            else()
                list(APPEND _COMMON -O3 -Qstrip_debug)
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
                DEPENDS "${SHADER}"
                COMMENT "Compiling HLSL ${FILE_NAME} → SPIR-V (Vulkan 1.3)"
                VERBATIM
            )
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
                DEPENDS "${SHADER}"
                COMMENT "Compiling GLSL ${FILE_NAME} → SPIR-V (Vulkan 1.3)"
                VERBATIM
            )
        endif()

        list(APPEND COMPILED_SHADERS "${OUTPUT_FILE}")
    endforeach()

    add_custom_target(${TARGET_NAME}_shaders ALL DEPENDS ${COMPILED_SHADERS})
    set(COMPILED_SHADER_OUTPUTS ${COMPILED_SHADERS} PARENT_SCOPE)
endfunction()
