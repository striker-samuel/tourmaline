# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(game_project_sample)

# ---------------------------------------------------------------------------
# 04_project_game: the CONTENT-AS-CODE sample — a standalone project exe.
# The project (raw sources + the recipe) is checked in under project/;
# the build generates the GENERATION-STAMPED project/project.h (content
# ids ensured in the project's asset database by the GameProject_ids
# target — it IMPORTS + resolves on the first run). The header appears
# in the IDE and can be committed like any other project source file;
# project_plans.h stays ignored as private build output.
# GameProject_stamp forces a new catalog
# generation on demand, and GameProject_paks cooks the full content
# (recipe build -> verified paks + the appmanifest + a project.h
# re-stamp whose pak records go state=verified) next to the exe ON
# DEMAND.
#
#   cmake --build ... --target GameProject        (exe + generated project.h)
#   cmake --build ... --target GameProject_ids    (refresh the catalog only)
#   cmake --build ... --target GameProject_stamp  (force a new generation)
#   cmake --build ... --target GameProject_paks   (the content cook)
#   .../GameProject.exe                           (runs the entry scene)
#
# NOTE: the ids generation writes the project's OWN state (the asset db
# under project/.tourmaline + .meta sidecars) — build-time writes into
# the checked-in project, exactly like a developer running the pipeline
# by hand (both are gitignored).
# ---------------------------------------------------------------------------
tourmaline_add_project(GameProject
	RECIPE ${CMAKE_CURRENT_SOURCE_DIR}/project/game.tourrecipe
)

# ---------------------------------------------------------------------------
# THE PROJECT LIT FAMILY (docs/shader_library_design.md Phase B): the
# project-authored lit shaders under project/content/shaders — the
# three entry wrappers (project_lit / project_lit_a2c /
# gfxmesh_project_lit) + the project's hook implementation
# (lit_hooks_project.hlsli + effects/). The wrappers are THREE-LINE
# files over the engine's extracted glue (lit_dependencies.hlsli /
# gfxmesh_lit_decl.hlsli) and the SAME shared lit body the engine's
# StandardLit family compiles; the include order puts THIS directory
# first so lit_hooks_project.hlsli resolves from the project (the
# engine's lit_hooks_default.hlsli is never reached).
#
# Outputs land in the build's content/shaders tree (the dev-override
# namespace the shader library serves exe-relative) under their OWN
# names — project_lit.*.spv, never the engine's default_lit.* — and
# are NOT part of engine-shaders.pak (the engine's declared list never
# sees this target's outputs). TOURMALINE_DEMO=0 (or editing the
# sample) removes the family: the exe still runs, every material
# simply stays StandardLit.
#
# HEADER_DIRS carries BOTH trees: DXC emits no include-dependency
# files, and the family's wrappers include the ENGINE's headers across
# the -I boundary — without this, editing default_lit_body.hlsli
# would not rebuild project_lit.*.spv (a stale family shading against
# a shifted layout, the design doc §7 dependency gap).
# ---------------------------------------------------------------------------
tourmaline_project_lit_variants(GAME_PROJECT_LIT_VARIANTS)
compile_shaders(
    "${CMAKE_CURRENT_SOURCE_DIR}/project/content/shaders"
    "${CMAKE_BINARY_DIR}/content/shaders"
    GameProjectLit
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/project/content/shaders"
        "${TOURMALINE_SOURCE_DIR}/engine/shaders"
    HEADER_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/project/content/shaders"
        "${TOURMALINE_SOURCE_DIR}/engine/shaders"
    VARIANTS ${GAME_PROJECT_LIT_VARIANTS}
    OUTPUT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/project_compiled_shaders.h"
    RUNTIME_ROOT "${CMAKE_BINARY_DIR}"
)

add_executable(GameProject
	main.cpp
)
# The family .spv must exist before the exe runs (the renderer's
# onAttach loads them best-effort); the exe sits in the build root, so
# the BINARY_DIR tree above is already exe-relative content/shaders.
add_dependencies(GameProject GameProjectLit_shaders)
target_include_directories(GameProject PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated")

# The full HOST setup (the same one App.exe gets): runtime output dir,
# the staged shared DLLs (KTX/OptickCore/SDL3 — this exe renders at
# runtime, its imports must resolve from a bare tree), fonts/luts,
# the engine+gltf_import link and the shader dependency. (The ad-hoc
# setup this replaces only staged SDL3: the exe would have failed to
# start anywhere but a warm build dir.)
tourmaline_host_setup(GameProject)

target_link_libraries(GameProject
	PRIVATE
		tourmaline_apphost
)

tourmaline_attach_project(GameProject GameProject)
