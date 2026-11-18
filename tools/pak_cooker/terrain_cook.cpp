// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// --terrain cook (see terrain_cook.h): stb_image PNG loads + strict
// validation + raw-RGBA8 Terrain blobs + meta blob with per-tile min/max.

#include "terrain_cook.h"
#include "world_height_bake.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include <Engine/Rendergraph/terrain_hfield.h>
#include <Engine/Rendergraph/terrain_meta.h>
#include <Engine/Rendergraph/terrain_occlusion.h>

// The standalone exe owns the stb_image implementation; the PakLib
// build linked into the editor skips its own copy (editor_branding.cpp
// provides it — one definition per binary still holds).
#ifndef PAK_LIB_BUILD
#define STB_IMAGE_IMPLEMENTATION
#endif
#include <stb_image.h>

namespace terraincook {
namespace {

bool ReadFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

} // namespace

bool CookTerrainBlobs(const std::string& terrainDir,
                      smvk::PakWriter& writer,
                      std::string& err,
                      const std::vector<const smvk::WorldPage*>& scenes) {
    // terrain.meta (the engine text convention; the .meta never reaches the
    // runtime).
    std::string metaText;
    if (!ReadFile(terrainDir + "/terrain.meta", metaText)) {
        err = "terrain: cannot read " + terrainDir + "/terrain.meta";
        return false;
    }
    smvk::terrain::TerrainMeta meta;
    if (!smvk::terrain::ParseTerrainMetaText(metaText, meta, err)) {
        err = "terrain: " + err;
        return false;
    }
    const uint32_t nH = smvk::terrain::TilesPerAxis(meta);
    const uint32_t nS = smvk::terrain::BlocksPerAxis(meta);

    std::vector<float> tileMinMax(size_t(nH) * nH * 2);
    uint64_t raisedSamples = 0;
    auto pyramid = smvk::terrain::NewOcclusionAtlas();
    // H-tiles: 1024 RGBA8, RG16 height.
    for (uint32_t tz = 0; tz < nH; ++tz) {
        for (uint32_t tx = 0; tx < nH; ++tx) {
            const std::string path = terrainDir + "/" + smvk::terrain::HTileName(tx, tz) + ".png";
            int w = 0, h = 0, comp = 0;
            stbi_uc* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
            if (!px) {
                err = "terrain: cannot decode " + path + " (" + stbi_failure_reason() + ")";
                return false;
            }
            struct Free {
                stbi_uc* p;
                ~Free() {
                    stbi_image_free(p);
                }
            } free{px};
            if (w != int(meta.hRes) || h != int(meta.hRes)) {
                err = "terrain: " + path + " is " + std::to_string(w) + "x" + std::to_string(h) +
                      " (want 1024x1024)";
                return false;
            }
            const size_t bytes = size_t(w) * size_t(h) * 4;
            std::vector<uint8_t> payload(px, px + bytes);
            // Per-tile world-Y bounds (G-block AABBs at T2 read these).
            {
                float mn = 0, mx = 0;
                smvk::terrain::TileMinMax(payload, meta.hRes, meta.baseY, meta.range, mn, mx);
                tileMinMax[(size_t(tz) * nH + tx) * 2] = mn;
                tileMinMax[(size_t(tz) * nH + tx) * 2 + 1] = mx;
            }
            // A distinct derived occlusion layer: terrain remains authoritative
            // for rendering, collision and placement. Scene meshes must never
            // alter the original RG16 H-tile or its terrain/meta min/max.
            std::vector<uint8_t> shadowHeights = payload;
            raisedSamples += RasterPlacedHeight(meta, tx, tz, scenes, shadowHeights);
            smvk::terrain::AccumulateOcclusionTile(pyramid, shadowHeights, tx, tz);
            if (!writer.AddBlob(smvk::PakAssetKind::Terrain,
                                "terrain/occlusion/" + smvk::terrain::HTileName(tx, tz),
                                std::move(shadowHeights))) {
                err = "terrain: occlusion tile AddBlob failed for " + path;
                return false;
            }
            if (!writer.AddBlob(smvk::PakAssetKind::Terrain,
                                smvk::terrain::HTileBlobPath(tx, tz),
                                std::move(payload))) {
                err = "terrain: AddBlob failed for " + path;
                return false;
            }
            std::cout << "Terrain H " << smvk::terrain::HTileName(tx, tz) << " (" << bytes
                      << " B raw, Y " << tileMinMax[(size_t(tz) * nH + tx) * 2] << ".."
                      << tileMinMax[(size_t(tz) * nH + tx) * 2 + 1] << ")\n";
        }
    }
    // One conservative hierarchy across ALL 16 tiles, including seams.
    smvk::terrain::FinishOcclusionAtlas(pyramid);
    if (!writer.AddBlob(smvk::PakAssetKind::Terrain,
                        smvk::terrain::kOcclusionPyramidPath,
                        std::move(pyramid))) {
        err = "terrain: occlusion maximum-height pyramid AddBlob failed";
        return false;
    }
    // S-tiles (test mask v1): 256 RGBA8, checker in R.
    for (uint32_t tz = 0; tz < nS; ++tz) {
        for (uint32_t tx = 0; tx < nS; ++tx) {
            const std::string path = terrainDir + "/" + smvk::terrain::STileName(tx, tz) + ".png";
            int w = 0, h = 0, comp = 0;
            stbi_uc* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
            if (!px) {
                err = "terrain: cannot decode " + path + " (" + stbi_failure_reason() + ")";
                return false;
            }
            struct Free {
                stbi_uc* p;
                ~Free() {
                    stbi_image_free(p);
                }
            } free{px};
            if (w != smvk::terrain::kMaskTileRes || h != smvk::terrain::kMaskTileRes) {
                err = "terrain: " + path + " is " + std::to_string(w) + "x" + std::to_string(h) +
                      " (want 256x256)";
                return false;
            }
            const size_t bytes = size_t(w) * size_t(h) * 4;
            std::vector<uint8_t> payload(px, px + bytes);
            if (!writer.AddBlob(smvk::PakAssetKind::Terrain,
                                smvk::terrain::STileBlobPath(tx, tz),
                                std::move(payload))) {
                err = "terrain: AddBlob failed for " + path;
                return false;
            }
        }
    }
    std::cout << "Terrain occlusion: " << raisedSamples
              << " raised texel write(s) from placed static meshes\\n";
    std::cout << "Terrain S: " << (nS * nS) << " mask tiles\n";
    // Meta blob (runtime form).
    {
        std::vector<uint8_t> metaBlob;
        smvk::terrain::SerializeTerrainMeta(meta, tileMinMax.data(), metaBlob);
        if (!writer.AddBlob(smvk::PakAssetKind::Terrain,
                            smvk::terrain::kTerrainMetaBlobPath,
                            std::move(metaBlob))) {
            err = "terrain: AddBlob failed for terrain/meta";
            return false;
        }
    }
    return true;
}

} // namespace terraincook
