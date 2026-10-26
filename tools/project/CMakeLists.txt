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
