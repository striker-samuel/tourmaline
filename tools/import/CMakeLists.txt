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
