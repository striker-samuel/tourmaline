# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(Import)

# import.exe: THE raw-3d -> engine-format converter (the asset
# creation). POSITIONAL grammar: import.exe <input.glb|gltf|gltf>
# [--settings] - parse + derive the Tourmaline artifact family next to
# the source (<stem>.geometry + .material/.anim + .gltf.meta, stable
# deterministic GUIDs; see source_import/asset_export.h). The ONE raw
# import surface: the editor's open/drop and the pak cook's
# derive-if-stale all run the SAME MeshImporter. import.exe is the
# headless/CLI form.
add_executable(Import main.cpp)

target_link_libraries(Import
PRIVATE
source_import
engine
)
# The USD adapter is a SEPARATE shared library (usd_import.dll): Import.exe
# loads it lazily through source_import::LoadUsdAdapter only when a .usd source
# is dispatched, so it does NOT link OpenUSD here. The define gates the
# .usd/.usda/.usdc/.usdz source-format recognition in source_formats.h.
if(TOURMALINE_WITH_USD)
    target_compile_definitions(Import PRIVATE TOURMALINE_WITH_USD=1)
    # Build the adapter DLL alongside (a build-order dependency, NOT a link):
    # Import.exe loads usd_import.dll lazily at runtime.
    add_dependencies(Import usd_import)
endif()
# Live at the build root like Cook/Pak (RUNTIME_OUTPUT_DIRECTORY), where
# the App's POST_BUILD staging already drops SDL3.dll: import.exe links
# the engine (SDL3), so it needs that DLL next to it to run from its own
# directory.
set_target_properties(Import PROPERTIES
RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
