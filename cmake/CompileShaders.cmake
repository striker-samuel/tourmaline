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
        "${SOURCE_DIR}/*.spv"
    )

    foreach(SHADER ${SHADERS})
        get_filename_component(FILE_NAME ${SHADER} NAME)
        set(OUTPUT_FILE "${BINARY_DIR}/${FILE_NAME}.spv")

        add_custom_command(
            OUTPUT ${OUTPUT_FILE}
            COMMAND ${Vulkan_GLSLC_EXECUTABLE} -o ${OUTPUT_FILE} ${SHADER}
            DEPENDS ${SHADER}
            COMMENT "Compiling shader ${FILE_NAME}"
            VERBATIM
        )

        list(APPEND COMPILED_SHADERS ${OUTPUT_FILE})
    endforeach()

    add_custom_target(${TARGET_NAME} DEPENDS ${COMPILED_SHADERS})
endfunction()
