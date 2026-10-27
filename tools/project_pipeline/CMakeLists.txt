# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(ProjectPipeline)

# The project-based content pipeline (the canonical-flow spec): the
# project model (project.tourproj marker), the import registry and the
# canonical ImportProjectAsset flow — a THIN layer over the single
# existing importer (gltf_import's ImportMeshSource) + the engine's
# artifact formats. Linked by Project.exe (the CLI), tests, and (next
# migration step) the editor's import UI.
add_library(project_pipeline STATIC
    src/project.cpp
    src/registry.cpp
    src/import_pipeline.cpp
    src/build_pipeline.cpp
)

target_include_directories(project_pipeline
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(project_pipeline
    PUBLIC
        gltf_import
        engine
)
