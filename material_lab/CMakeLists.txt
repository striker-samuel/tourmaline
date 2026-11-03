# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

# ProjectLit uses the same filenames as the game sample. Keep its binaries
# in the MaterialLab runtime rather than the shared shader tree.
tourmaline_project_lit_variants(MATERIAL_LAB_LIT_VARIANTS)
compile_shaders(
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    "${CMAKE_CURRENT_BINARY_DIR}/compiled_shaders"
    MaterialLabLit
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        "${TOURMALINE_SOURCE_DIR}/engine/shaders"
    HEADER_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        "${TOURMALINE_SOURCE_DIR}/engine/shaders"
    VARIANTS ${MATERIAL_LAB_LIT_VARIANTS}
)

add_executable(MaterialLab main.cpp)
target_link_libraries(MaterialLab PRIVATE tourmaline_apphost)
tourmaline_host_setup(MaterialLab)
set_target_properties(MaterialLab PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/material-lab-runtime"
)
add_dependencies(MaterialLab MaterialLabLit_shaders)
# HostSetup first stages engine shaders. Overlay this lab's family last.
add_custom_command(TARGET MaterialLab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_BINARY_DIR}/compiled_shaders"
        "$<TARGET_FILE_DIR:MaterialLab>/content/shaders"
    COMMENT "[MaterialLab] Staging isolated toon ProjectLit modules"
)
