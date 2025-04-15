

set(GLSLC "C:/VulkanSDK/1.3.283.0/Bin/glslc.exe")


function(compile_shaders SOURCE_DIR BINARY_DIR TARGET_NAME)
    file(GLOB SHADERS
        "${SOURCE_DIR}/*.vert"
        "${SOURCE_DIR}/*.frag"
        "${SOURCE_DIR}/*.comp"
        "${SOURCE_DIR}/*.geom"
        "${SOURCE_DIR}/*.tesc"
        "${SOURCE_DIR}/*.tese"
        "${SOURCE_DIR}/*.rgen"
        "${SOURCE_DIR}/*.rchit"
        "${SOURCE_DIR}/*.rmiss"
        "${SOURCE_DIR}/*.hlsl"
        "${SOURCE_DIR}/*.glsl"
    )

    set(COMPILED_SHADERS "")

    foreach(SHADER ${SHADERS})
        get_filename_component(FILE_NAME ${SHADER} NAME)
        set(OUTPUT_FILE "${BINARY_DIR}/${FILE_NAME}.spv")

        add_custom_command(
            OUTPUT ${OUTPUT_FILE}
            COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --cyan "⏳ Compiling shader: ${FILE_NAME}"
            COMMAND cmd /c "${GLSLC} -o ${OUTPUT_FILE} ${SHADER}"
            COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --green "✅ Compiled: ${FILE_NAME}"
            DEPENDS ${SHADER}
            COMMENT "Compiling shader ${FILE_NAME}"
            VERBATIM
        )



        list(APPEND COMPILED_SHADERS ${OUTPUT_FILE})
    endforeach()

    set(COMPILED_SHADER_OUTPUTS ${COMPILED_SHADERS} PARENT_SCOPE)
endfunction()
