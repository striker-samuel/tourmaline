project(GltfImport)

# glTF import pipeline (moved out of the engine: the runtime works on
# cooked clusters/paks only; cookers, bakers and the editor import here).
# Static lib linked by PakCooker, GiBaker, CubemapBaker, Editor and App.
add_library(gltf_import STATIC
    src/gltf.cpp
)

target_include_directories(gltf_import
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Engine types are the API (Cluster, VFS, GI bake options); tinygltf comes
# via engine's public includes.
target_link_libraries(gltf_import
    PUBLIC
        engine
)

target_compile_definitions(gltf_import PRIVATE ENGINE_VULKAN)
