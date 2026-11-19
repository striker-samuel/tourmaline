# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT
#
# CONFIGURE-TIME STALENESS GATE for the vendored dependency trees.
#
# `external/` mixes two kinds of trees. The pinned git submodules (glm,
# SPIRV-Reflect, JoltPhysics, ...) move with `git pull`. Everything else is
# fetched OUT OF GIT by install_dependencies.ps1 / install_dependencies.sh:
# `git pull` never touches those trees, so a pull that bumps a pin leaves the
# OLD bytes on disk even though the engine source now requires the new API.
#
# The fetch is the only thing that knows the pin, and it records it in
# `<tree>/.depstamp-<name>` as "<url>|<sha256>". This module compares every
# PRESENT depstamp against the pin the platform's installer script declares
# TODAY and fails the CONFIGURE (not the compile) when they disagree.
#
# Why configure and not compile: the first symptom of a stale tree is a
# compile error inside engine code, which reads like a source bug. The ImGui
# docking repin is the recorded case - the overlay needs
# ImGuiConfigFlags_ViewportsEnable / ImGui::UpdatePlatformWindows, and a
# stale MAINLINE tree produced
#   imgui_overlay.cpp(84): error C2065: 'ImGuiConfigFlags_ViewportsEnable':
#                          undeclared identifier
# with no hint that the fix was "re-run the dependency installer".
#
# Policy: only trees that CARRY a stamp are policed. A tree fetched by hand
# (or a tree the installer never manages) has no promise to verify, and a
# missing stamp is also the fresh-clone state - never an error.
#
# Escape hatch: -DTOURMALINE_SKIP_DEP_STAMP_CHECK=ON (exotic setups that
# manage external/ themselves).
#
# Standalone (the regression test in tests/test_dependency_stamps.py):
#   cmake -DTOURMALINE_DEPS_ROOT=<repo> -DTOURMALINE_DEPS_PINS=<script> \
#         -P cmake/DependencyStamps.cmake

option(TOURMALINE_SKIP_DEP_STAMP_CHECK
    "Skip the vendored-dependency stamp check (external/ managed by hand)" OFF)

# Reads the installer script and returns the URL its pin for <dep> declares
# (empty when the dep is not pinned there, or the pin is a multi-URL raw dep
# whose stamp does not carry a single URL).
function(tourmaline_dependency_pin_url script dep out_var)
    set(_url "")
    if(EXISTS "${script}")
        file(READ "${script}" _text)
        set(_match "")
        if(script MATCHES "\\.ps1$")
            # PowerShell pin table row:
            #   @{ name='imgui'; type='tar'; target='imgui';
            #      url='https://...';   sha256='...' }
            # [^}] spans the row's newlines; `url='` deliberately does NOT
            # match the multi-URL `urls=@(...)` form (no single-URL stamp).
            string(REGEX MATCH "name='${dep}';[^}]*url='([^']+)'" _match "${_text}")
        else()
            # POSIX pin table row (URL on the following line, one URL each):
            #   install_archive imgui imgui tar \
            #       'https://...' \
            #   install_raw stb-image stb stb_image.h \
            #       'https://...' \
            # The trailing [ \t] keeps `stb` from matching `stb-image`.
            string(REGEX MATCH "install_archive[ \t]+${dep}[ \t][^']*'([^']+)'" _match "${_text}")
            if(NOT _match)
                string(REGEX MATCH "install_raw[ \t]+${dep}[ \t][^']*'([^']+)'" _match "${_text}")
            endif()
        endif()
        if(_match)
            set(_url "${CMAKE_MATCH_1}")
        endif()
    endif()
    set(${out_var} "${_url}" PARENT_SCOPE)
endfunction()

# The engine's ImGui overlay compiles against the DOCKING branch
# (multi-viewport: ImGuiConfigFlags_ViewportsEnable, DockBuilder,
# UpdatePlatformWindows). A mainline snapshot (or a tree left behind by a
# hand-managed checkout) turns the overlay into a wall of undeclared-identifier
# errors - name the real cause here instead.
function(tourmaline_check_imgui_docking root)
    set(_header "${root}/external/imgui/imgui.h")
    if(NOT EXISTS "${_header}")
        return()
    endif()
    file(READ "${_header}" _text)
    if(NOT _text MATCHES "#define IMGUI_HAS_DOCK")
        string(REGEX MATCH "#define IMGUI_VERSION[ \t]+\"([^\"]*)\"" _version "${_text}")
        set(_version_text "an unknown version")
        if(_version)
            set(_version_text "IMGUI_VERSION \"${CMAKE_MATCH_1}\"")
        endif()
        message(FATAL_ERROR
            "external/imgui is a MAINLINE ImGui snapshot (${_version_text}, no "
            "IMGUI_HAS_DOCK), but the engine's UI overlay is built against the "
            "DOCKING branch (multi-viewport: ImGuiConfigFlags_ViewportsEnable,\n"
            "  DockBuilder, ImGui::UpdatePlatformWindows). Building it produces\n"
            "  \"'ImGuiConfigFlags_ViewportsEnable': undeclared identifier\" in\n"
            "  engine/src/ImGui/imgui_overlay.cpp.\n"
            "\n"
            "Refresh the vendored tree:\n"
            "  Windows: powershell -NoProfile -ExecutionPolicy Bypass -File .\\install_dependencies.ps1\n"
            "  Linux:   ./install_dependencies.sh\n"
            "\n"
            "If the link step then reports unresolved ImGui:: symbols, delete the\n"
            "build directory's copy of the stale objects (e.g.\n"
            "out/build/<config>/external/imgui) and build again.")
    endif()
endfunction()

# The gate. ROOT/PINS default to this repository's own layout.
function(tourmaline_check_vendored_dependencies)
    cmake_parse_arguments(ARG "" "ROOT;PINS" "" ${ARGN})
    if(NOT ARG_ROOT)
        set(ARG_ROOT "${TOURMALINE_SOURCE_DIR}")
    endif()
    if(NOT ARG_PINS)
        if(WIN32)
            set(ARG_PINS "${ARG_ROOT}/install_dependencies.ps1")
        else()
            set(ARG_PINS "${ARG_ROOT}/install_dependencies.sh")
        endif()
    endif()

    if(TOURMALINE_SKIP_DEP_STAMP_CHECK)
        message(STATUS "Dependency stamps: check skipped "
            "(TOURMALINE_SKIP_DEP_STAMP_CHECK=ON)")
        return()
    endif()

    tourmaline_check_imgui_docking("${ARG_ROOT}")

    file(GLOB _stamps "${ARG_ROOT}/external/*/.depstamp-*")
    set(_stale "")
    foreach(_stamp IN LISTS _stamps)
        get_filename_component(_file "${_stamp}" NAME)
        string(REGEX REPLACE "^\\.depstamp-" "" _dep "${_file}")
        file(READ "${_stamp}" _record)
        string(STRIP "${_record}" _record)
        # "<url>|<sha256>"; a stamp without the separator is not ours to judge
        string(FIND "${_record}" "|" _bar)
        if(_bar LESS 0)
            continue()
        endif()
        string(SUBSTRING "${_record}" 0 ${_bar} _stamp_url)
        tourmaline_dependency_pin_url("${ARG_PINS}" "${_dep}" _pin_url)
        if(NOT _pin_url)
            continue()   # not pinned by this platform's installer
        endif()
        if(NOT _stamp_url STREQUAL _pin_url)
            get_filename_component(_tree "${_stamp}" DIRECTORY)
            file(RELATIVE_PATH _tree_rel "${ARG_ROOT}" "${_tree}")
            list(APPEND _stale
                "  ${_tree_rel}\n"
                "    fetched from: ${_stamp_url}\n"
                "    pinned now:   ${_pin_url}")
        endif()
    endforeach()

    if(_stale)
        string(REPLACE ";" "\n" _stale_text "${_stale}")
        if(WIN32)
            set(_refresh "powershell -NoProfile -ExecutionPolicy Bypass -File .\\install_dependencies.ps1")
        else()
            set(_refresh "./install_dependencies.sh")
        endif()
        message(FATAL_ERROR
            "Stale vendored dependencies: the trees below were fetched from a "
            "different pin than the one install_dependencies declares today "
            "(usually a `git pull` that bumped a pin - git does not touch "
            "external/).\n"
            "\n"
            "${_stale_text}\n"
            "\n"
            "Refusing to configure: the build would fail with a compile error "
            "inside engine code that reads like a source bug.\n"
            "Refresh the trees, then re-run the configure:\n"
            "  ${_refresh}\n"
            "\n"
            "If the link step afterwards reports unresolved symbols from a "
            "refreshed tree, delete the build directory's copy of it (e.g. "
            "out/build/<config>/external/imgui) and build again.\n"
            "Approve an unmanaged external/ with -DTOURMALINE_SKIP_DEP_STAMP_CHECK=ON.")
    endif()
    list(LENGTH _stamps _stamp_count)
    message(STATUS "Dependency stamps: ${_stamp_count} vendored tree(s) match "
        "install_dependencies")
endfunction()

# Standalone driver (tests/test_dependency_stamps.py). Script mode only.
if(CMAKE_SCRIPT_MODE_FILE AND DEFINED TOURMALINE_DEPS_ROOT)
    tourmaline_check_vendored_dependencies(
        ROOT "${TOURMALINE_DEPS_ROOT}"
        PINS "${TOURMALINE_DEPS_PINS}")
endif()
