# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
#
# TourmalineProject: content as code — the CMake layer over the
# project's recipe pipeline (Project.exe ids/build + the asset
# database's content-id registry in <project>/.tourmaline/assets.db).
#
#   tourmaline_add_project(GameProject
#       RECIPE  project/game.tourrecipe     # authored build definition
#       [PROJECT <dir>]               # default: the recipe's directory
#       [HEADER_DIR <dir>]            # default: ${CMAKE_CURRENT_BINARY_DIR}/<name>
#   )
#   tourmaline_attach_project(GameProject GameProject)
#
# add_project VALIDATES + REGISTERS (no targets — attach creates them,
# because attach is where the consuming exe's publish dir becomes
# known). attach_project wires the target's include path + the
# <Name>_ids dependency and registers THREE content targets:
#
#   * <Name>_ids — the build-time catalog refresh: Project.exe ids
#     resolves the recipe, ensures the content ids and regenerates the
#     GENERATION-STAMPED project.h (timestamp + generation counters +
#     a catalog-semantics content hash, all constexpr, plus the EOF
#     machine-readable metadata comment: the asset/pak fingerprints —
#     the generator's incremental cache, no side file). Runs on EVERY
#     build (a depfile cannot see a newly created glob match or a
#     sidecar GUID change) but the stamp moves ONLY on a generation
#     EVENT — any change to the catalog or the recorded fingerprints —
#     so an unchanged build re-emits byte-identical text and nothing
#     that includes project.h recompiles.
#   * <Name>_stamp — the manual refresh: the SAME generation with
#     --stamp — a NEW GENERATION every run, changed content or not
#     (the timestamp = the generation EVENT). The next exe build
#     recompiles project.h's includers; run it when you want the
#     catalog versioned forward.
#   * <Name>_paks — the ON-DEMAND full content build (Project.exe
#     build --publish next to the exe --header <the generated header>):
#     verified + policed paks + the appmanifest + a project.h re-stamp
#     whose EOF pak records go state=verified (the recipe build is the
#     one writer that knows the content). Not a default dependency —
#     cooking runs the bakes; run it when the content changes.
#
# The three operations are INDEPENDENT (the design contract — refresh
# the catalog without compiling, cook content without compiling, and
# the exe build pulls in the auto refresh only):
#   cmake --build ... --target <Name>_ids    # refresh the C++ catalog
#   cmake --build ... --target <Name>_stamp  # ... and force a generation
#   cmake --build ... --target <Name>_paks   # cook content (no C++ built)
#   cmake --build ... --target <Game>        # exe (+ the ids auto refresh)
#
# The generated exe runs against the packaged content: the apphost
# resolves the appmanifest + paks exe-relative (a moved exe+content
# pair keeps working — the manifest resolves its paks relative to
# itself). The ids pass fingerprints paks in the exe's dir — the same
# place <Name>_paks publishes them.

include_guard(GLOBAL)

function(tourmaline_add_project NAME)
	cmake_parse_arguments(TP "" "RECIPE;PROJECT;HEADER_DIR" "" ${ARGN})
	if(NOT TP_RECIPE)
		message(FATAL_ERROR
			"tourmaline_add_project(${NAME}): RECIPE is required")
	endif()
	# Absolutize the recipe FIRST — the default PROJECT derives from the
	# RESOLVED recipe path (deriving before absolutizing turned a
	# relative recipe into a wrong --project argument; the commit
	# review caught it).
	get_filename_component(TP_RECIPE "${TP_RECIPE}" ABSOLUTE)
	if(NOT EXISTS "${TP_RECIPE}")
		message(FATAL_ERROR
			"tourmaline_add_project(${NAME}): recipe not found: ${TP_RECIPE}")
	endif()
	if(NOT TP_PROJECT)
		get_filename_component(TP_PROJECT "${TP_RECIPE}" DIRECTORY)
	endif()
	if(NOT TP_HEADER_DIR)
		set(TP_HEADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/${NAME}")
	endif()
	set(TP_HEADER "${TP_HEADER_DIR}/project.h")

	if(NOT TARGET Project)
		message(FATAL_ERROR
			"tourmaline_add_project(${NAME}): the Project target is not "
			"built in this configure (host tools are required for the "
			"content-id generation)")
	endif()

	set_property(GLOBAL PROPERTY
		TOURMALINE_PROJECT_${NAME}_INCLUDE_DIR "${TP_HEADER_DIR}")
	set_property(GLOBAL PROPERTY
		TOURMALINE_PROJECT_${NAME}_RECIPE "${TP_RECIPE}")
	set_property(GLOBAL PROPERTY
		TOURMALINE_PROJECT_${NAME}_PROJECT "${TP_PROJECT}")
	set_property(GLOBAL PROPERTY
		TOURMALINE_PROJECT_${NAME}_HEADER "${TP_HEADER}")
	# (No targets here — attach_project creates them: it knows the
	# consuming exe, whose $<TARGET_FILE_DIR> is the publish dir the
	# catalog pass fingerprints the paks in.)
endfunction()

function(tourmaline_attach_project TARGET NAME)
	get_property(include_dir GLOBAL PROPERTY
		TOURMALINE_PROJECT_${NAME}_INCLUDE_DIR)
	get_property(recipe GLOBAL PROPERTY
		TOURMALINE_PROJECT_${NAME}_RECIPE)
	get_property(projdir GLOBAL PROPERTY
		TOURMALINE_PROJECT_${NAME}_PROJECT)
	get_property(header GLOBAL PROPERTY
		TOURMALINE_PROJECT_${NAME}_HEADER)
	if(NOT include_dir)
		message(FATAL_ERROR
			"tourmaline_attach_project(${TARGET}, ${NAME}): "
			"tourmaline_add_project(${NAME}) was not called")
	endif()
	target_include_directories(${TARGET} PRIVATE "${include_dir}")
	# The publish dir = the consuming exe's dir (where <Name>_paks
	# lands the paks and the runtime loads them exe-relative).
	set(TP_PAK_DIR "$<TARGET_FILE_DIR:${TARGET}>")

	if(NOT TARGET ${NAME}_ids)
		# The generation runs on EVERY build (a custom target's command
		# is never skipped): a depfile could not see a NEWLY CREATED
		# file matching a recipe glob (it was absent when the deps were
		# written) or a sidecar GUID change — the always-run scan +
		# the GENERATION-STAMPED write-only-on-change emission give the
		# correct incrementality instead: the scan is cheap, an
		# unchanged generation never touches project.h's mtime, and
		# nothing that includes it recompiles.
		add_custom_target(${NAME}_ids
			COMMAND $<TARGET_FILE:Project>
				ids
				--project "${projdir}"
				--recipe "${recipe}"
				--header "${header}"
				--pak-dir "${TP_PAK_DIR}"
			BYPRODUCTS "${header}"
			DEPENDS Project "${recipe}"
			COMMENT "Generating ${NAME} project.h (content ids from the asset db)"
			VERBATIM
		)
	endif()
	if(NOT TARGET ${NAME}_stamp)
		# The forced generation (--stamp): a NEW generation every run,
		# changed content or not. Deliberately NO BYPRODUCTS — ninja
		# allows exactly one rule per output file, and <Name>_ids owns
		# the header's build-graph slot; the always-run ids refresh
		# heals any drift a stamp run leaves.
		add_custom_target(${NAME}_stamp
			COMMAND $<TARGET_FILE:Project>
				ids
				--project "${projdir}"
				--recipe "${recipe}"
				--header "${header}"
				--pak-dir "${TP_PAK_DIR}"
				--stamp
			DEPENDS Project "${recipe}"
			COMMENT "Forcing a new ${NAME} catalog generation (the stamp always bumps)"
			VERBATIM
		)
	endif()
	add_dependencies(${TARGET} ${NAME}_ids)
	if(NOT TARGET ${NAME}_paks)
		# The on-demand full content build (bakes included — never a
		# default dependency): paks + the appmanifest publish next to
		# the exe, and the generated project.h re-stamps with the
		# paks' fingerprints at state=verified. NO BYPRODUCTS on the
		# header (see <Name>_stamp) — <Name>_ids owns the slot.
		add_custom_target(${NAME}_paks
			COMMAND $<TARGET_FILE:Project>
				build
				--project "${projdir}"
				--recipe "${recipe}"
				--publish "${TP_PAK_DIR}"
				--header "${header}"
			DEPENDS Project ${NAME}_ids
			COMMENT "Building ${NAME} content (recipe build -> paks + appmanifest + project.h re-stamp)"
			VERBATIM
		)
	endif()
endfunction()
