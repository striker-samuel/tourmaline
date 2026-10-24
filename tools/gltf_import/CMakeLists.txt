# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(GltfImport)

# glTF import pipeline (moved out of the engine: the runtime works on
# cooked clusters/paks only; cookers, bakers and the editor import here).
# Static lib linked by PakCooker, GiBaker, CubemapBaker, Editor and App.
add_library(gltf_import STATIC
    src/gltf.cpp
    src/tex_transcode.cpp
    src/asset_export.cpp
    src/tour_world.cpp
)

target_include_directories(gltf_import
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    # stb_image.h / stb_image_resize.h for the in-process KTX transcoder
    # (xatlas thirdparty; implementations compile once in tex_transcode).
    # tools/common for tool_log.h (tour_world.cpp definition lines).
    PRIVATE
        ${TOURMALINE_SOURCE_DIR}/external/xatlas/source/thirdparty
        ${TOURMALINE_SOURCE_DIR}/tools/common
)

# Engine types are the API (Cluster, VFS, GI bake options); tinygltf comes
# via engine's public includes. libktx is linked explicitly (engine keeps
# it PRIVATE): the transcoder writes KTX2, and consumers link it through.
# directxtex_bc: the texture-asset BCn encode targets (D3DXEncodeBC1/3/4/5/7
# per 4x4 block — the same vendored block-codec layer the engine's BC6H GI
# encode uses).
target_link_libraries(gltf_import
    PUBLIC
        engine
        KTX::ktx
    PRIVATE
        directxtex_bc
)

target_compile_definitions(gltf_import PRIVATE ENGINE_VULKAN)
