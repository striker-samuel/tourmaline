# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(ProjectCli)

# Project.exe: the project-pipeline CLI — a THIN wrapper over the shared
# project_pipeline library (the editor uses the same implementation).
add_executable(Project
    main.cpp
)

target_link_libraries(Project
    PRIVATE
        project_pipeline
)

set_target_properties(Project PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

tourmaline_stage_sdl3(Project)

# KTX + OptickCore are SHARED: stage their DLLs next to Project.exe.
# Project.exe is not just a user tool anymore — the BUILD runs it
# (tourmaline_add_project's <Name>_ids generation step), so it must
# START from a bare build tree where nothing else has staged the DLLs
# yet (the CI Windows build failed at exactly that: GameProject_ids ->
# Project.exe -> 0xC0000135 DLL_NOT_FOUND, ktx.dll/OptickCore.dll not
# yet copied by any other target's POST_BUILD). Stage them as part of
# THIS target's build edge — dependents (the ids generation) can only
# run after the POST_BUILD steps complete.
add_custom_command(TARGET Project POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:KTX::ktx>"
        "$<TARGET_FILE_DIR:Project>"
)
add_custom_command(TARGET Project POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OptickCore>"
        "$<TARGET_FILE_DIR:Project>"
)
