# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(ProjectPipeline)

# The project-based content pipeline (the canonical-flow spec): the
# project model (project.tourproj marker), the import registry and the
# canonical ImportProjectAsset flow — a THIN layer over the single
# existing importer (gltf_import's ImportMeshSource) + the engine's
# artifact formats. Linked by Project.exe (the CLI), tests, and (next
# migration step) the editor's import UI.
#
# The RECIPE layer (the .tourrecipe spec): project_recipe.cpp parses
# the authored build definition (syntax + schema only), build_plan.cpp
# + recipe_resolver.cpp turn it into the deterministic, fully-resolved
# BuildPlan (ownership + topological pak order) that the shared
# executor consumes.
add_library(project_pipeline STATIC
    src/project.cpp
    src/registry.cpp
    src/import_pipeline.cpp
    src/build_pipeline.cpp
    src/project_recipe.cpp
    src/build_plan.cpp
    src/recipe_resolver.cpp
    src/project_catalog.cpp
)

target_include_directories(project_pipeline
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(project_pipeline
    PUBLIC
        gltf_import
        engine
        artifact_cache
)
