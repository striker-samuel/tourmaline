# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(game_project_sample)

# ---------------------------------------------------------------------------
# 04_project_game: the CONTENT-AS-CODE sample — a standalone project exe.
# The project (raw sources + the recipe) is checked in under project/;
# the build generates the GENERATION-STAMPED project.h (content ids
# ensured in the project's asset database by the GameProject_ids target
# — it IMPORTS + resolves on the first run, so the checked-in tree
# needs no derived files), GameProject_stamp forces a new catalog
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

add_executable(GameProject
	main.cpp
)

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
