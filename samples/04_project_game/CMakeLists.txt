# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(game_project_sample)

# ---------------------------------------------------------------------------
# 04_project_game: the CONTENT-AS-CODE sample — a standalone project exe.
# The project (raw sources + the recipe) is checked in under project/;
# the build generates project.h (content ids ensured in the project's
# asset database by the GameProject_ids target — it IMPORTS + resolves
# on the first run, so the checked-in tree needs no derived files),
# and GameProject_paks cooks the full content (recipe build -> verified
# paks + the appmanifest) next to the exe ON DEMAND.
#
#   cmake --build ... --target GameProject        (exe + generated project.h)
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

target_link_libraries(GameProject
	PRIVATE
		tourmaline_apphost
)

tourmaline_attach_project(GameProject GameProject)

set_target_properties(GameProject PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

tourmaline_stage_sdl3(GameProject)
