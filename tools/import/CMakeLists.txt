# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(Import)

# import.exe: THE raw-3d -> engine-format converter (the asset
# creation). POSITIONAL grammar: import.exe <input.glb|gltf|gltf>
# [--settings] - parse + derive the Tourmaline artifact family next to
# the source (<stem>.geometry + .material/.anim + .gltf.meta, stable
# deterministic GUIDs; see gltf_import/asset_export.h). The ONE raw
# import surface: the editor's open/drop and the pak cook's
# derive-if-stale all run the SAME MeshImporter. import.exe is the
# headless/CLI form.
add_executable(Import main.cpp)

target_link_libraries(Import
PRIVATE
gltf_import
engine
)
# Live at the build root like Cook/Pak (RUNTIME_OUTPUT_DIRECTORY), where
# the App's POST_BUILD staging already drops SDL3.dll: import.exe links
# the engine (SDL3), so it needs that DLL next to it to run from its own
# directory.
set_target_properties(Import PROPERTIES
RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
