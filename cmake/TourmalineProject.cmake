# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
#
# TourmalineProject: content as code — the CMake layer over the
# project's recipe pipeline (Project.exe ids/build + the asset
# database's content-id registry in <project>/.tourmaline/assets.db).
#
#   tourmaline_add_project(Sponza
#       RECIPE  sponza.tourrecipe     # authored build definition
#       [PROJECT <dir>]               # default: the recipe's directory
#       [HEADER_DIR <dir>]            # default: ${CMAKE_CURRENT_BINARY_DIR}/<name>
#   )
#   tourmaline_attach_project(SponzaGame Sponza)
#
# add_project registers:
#   * <Name>_ids — the generation target: Project.exe ids resolves the
#     recipe, ensures the content ids and regenerates project.h
#     ONLY-ON-CHANGE (an unchanged catalog never recompiles includers).
#     The custom command carries a DEPFILE (the recipe + every content
#     file + the registry), so content changes regenerate at build time.
#     Requires the Project target (the host-tools guard at the root).
# attach_project wires a target:
#   * the include dir + the <Name>_ids dependency (the generated
#     project.h is includable as #include "project.h");
#   * <Name>_paks — the ON-DEMAND full build (Project.exe build: the
#     recipe build publishing the verified paks + the appmanifest
#     directly next to the exe). Not a default dependency — cooking
#     runs the bakes; run it when the content changes
#     (cmake --build ... --target <Name>_paks).
#
# The generated exe runs against the packaged content: the apphost
# resolves the appmanifest + paks exe-relative (a moved exe+content
# pair keeps working — the manifest resolves its paks relative to
# itself).

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

	# The generation runs on EVERY build (a custom target's command is
	# never skipped): a depfile could not see a NEWLY CREATED file
	# matching a recipe glob (it was absent when the deps were written)
	# or a sidecar GUID change — the always-run + the generator's
	# WRITE-ONLY-ON-CHANGE emission give the correct incrementality
	# instead: the scan is cheap, an unchanged catalog never touches
	# project.h's mtime, and nothing that includes it recompiles.
	add_custom_target(${NAME}_ids
		COMMAND $<TARGET_FILE:Project>
			ids
			--project "${TP_PROJECT}"
			--recipe "${TP_RECIPE}"
			--header "${TP_HEADER}"
		BYPRODUCTS "${TP_HEADER}"
		DEPENDS Project "${TP_RECIPE}"
		COMMENT "Generating ${NAME} project.h (content ids from the asset db)"
		VERBATIM
	)

	set_property(GLOBAL PROPERTY
		TOURMALINE_PROJECT_${NAME}_INCLUDE_DIR "${TP_HEADER_DIR}")
	set_property(GLOBAL PROPERTY
		TOURMALINE_PROJECT_${NAME}_RECIPE "${TP_RECIPE}")
	set_property(GLOBAL PROPERTY
		TOURMALINE_PROJECT_${NAME}_PROJECT "${TP_PROJECT}")
endfunction()

function(tourmaline_attach_project TARGET NAME)
	get_property(include_dir GLOBAL PROPERTY
		TOURMALINE_PROJECT_${NAME}_INCLUDE_DIR)
	get_property(recipe GLOBAL PROPERTY
		TOURMALINE_PROJECT_${NAME}_RECIPE)
	get_property(projdir GLOBAL PROPERTY
		TOURMALINE_PROJECT_${NAME}_PROJECT)
	if(NOT include_dir)
		message(FATAL_ERROR
			"tourmaline_attach_project(${TARGET}, ${NAME}): "
			"tourmaline_add_project(${NAME}) was not called")
	endif()
	target_include_directories(${TARGET} PRIVATE "${include_dir}")
	add_dependencies(${TARGET} ${NAME}_ids)
	# The on-demand full content build (bakes included — never a default
	# dependency): paks + the appmanifest publish next to the exe.
	add_custom_target(${NAME}_paks
		COMMAND $<TARGET_FILE:Project>
			build
			--project "${projdir}"
			--recipe "${recipe}"
			--publish "$<TARGET_FILE_DIR:${TARGET}>"
		DEPENDS Project
		COMMENT "Building ${NAME} content (recipe build -> paks + appmanifest)"
		VERBATIM
	)
endfunction()
