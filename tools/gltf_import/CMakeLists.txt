# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

project(GltfImport)

# glTF import pipeline (moved out of the engine: the runtime works on
# cooked clusters/paks only; cookers, bakers and the editor import here).
# Static lib linked by PakCooker, GiBaker, CubemapBaker, Editor and App.
add_library(gltf_import STATIC
    src/gltf.cpp
    # The ONLY TU compiling the tinyobjloader implementation (the OBJ
    # source adapter; tinygltf's implementation lives in gltf.cpp).
    src/obj_import.cpp
    src/tex_transcode.cpp
    src/material_pack.cpp
    src/asset_export.cpp
    src/tour_world.cpp
)

target_include_directories(gltf_import
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    # tinyobjloader FIRST: xatlas's thirdparty dir bundles an older
    # tiny_obj_loader.h (its copy predates the v2 ObjReader API this
    # adapter uses), and include order decides which one
    # `#include <tiny_obj_loader.h>` resolves to. The dir carries only
    # that one header, so fronting it cannot shadow anything else.
    # stb_image.h / stb_image_resize.h for the in-process KTX
    # transcoder follow (xatlas thirdparty; implementations compile
    # once in tex_transcode). tools/common for tool_log.h
    # (tour_world.cpp definition lines).
    PRIVATE
        ${TOURMALINE_SOURCE_DIR}/external/tinyobjloader
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

# OpenUSD is an optional authoring-time dependency, never a runtime USD
# backend. Enabling it converts composed USD stages through the existing
# source model -> .geometry/.material/.meta derivation path.
option(TOURMALINE_WITH_USD "Enable native OpenUSD source importing" OFF)
if(TOURMALINE_WITH_USD)
    find_package(pxr CONFIG REQUIRED)
    target_sources(gltf_import PRIVATE src/usd_import.cpp)
    target_compile_definitions(gltf_import PUBLIC TOURMALINE_WITH_USD=1)
    if(TARGET usdGeom AND TARGET usdShade AND TARGET usd)
        target_link_libraries(gltf_import PRIVATE usdGeom usdShade usd)
    elseif(TARGET pxr::usdGeom AND TARGET pxr::usdShade AND TARGET pxr::usd)
        target_link_libraries(gltf_import PRIVATE pxr::usdGeom pxr::usdShade pxr::usd)
    else()
        message(FATAL_ERROR "OpenUSD CMake package lacks supported usd/usdGeom/usdShade targets")
    endif()
endif()
