// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// --terrain cook (see terrain_cook.h): stb_image PNG loads + strict
// validation + raw-RGBA8 Terrain blobs + meta blob with per-tile min/max.

#include "terrain_cook.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include <Engine/Rendergraph/terrain_hfield.h>
#include <Engine/Rendergraph/terrain_meta.h>

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
	smvk::PakWriter& writer, std::string& err) {
	// terrain.json (the flat schema; .json never reaches the runtime).
	std::string jsonText;
	if (!ReadFile(terrainDir + "/terrain.json", jsonText)) {
		err = "terrain: cannot read " + terrainDir + "/terrain.json";
		return false;
	}
	smvk::terrain::TerrainMeta meta;
	if (!smvk::terrain::ParseTerrainJson(jsonText, meta, err)) {
		err = "terrain: " + err;
		return false;
	}
	const uint32_t nH = static_cast<uint32_t>(
		meta.worldSize / meta.hTileSize + 0.5f);
	if (nH != 4) {
		err = "terrain: v1 supports exactly 4x4 H-tiles (worldSize/hTileSize)";
		return false;
	}
	const uint32_t nS = static_cast<uint32_t>(
		meta.worldSize / meta.gBlock + 0.5f);
	if (nS != 16) {
		err = "terrain: v1 expects 16x16 S-tiles (worldSize/gBlock)";
		return false;
	}

	float tileMinMax[16][2];
	// H-tiles: 1024 RGBA8, RG16 height.
	for (uint32_t tz = 0; tz < nH; ++tz) {
		for (uint32_t tx = 0; tx < nH; ++tx) {
			const std::string path = terrainDir + "/"
				+ smvk::terrain::HTileName(tx, tz) + ".png";
			int w = 0, h = 0, comp = 0;
			stbi_uc* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
			if (!px) {
				err = "terrain: cannot decode " + path + " ("
					+ stbi_failure_reason() + ")";
				return false;
			}
			struct Free {
				stbi_uc* p;
				~Free() { stbi_image_free(p); }
			} free{px};
			if (w != int(meta.hRes) || h != int(meta.hRes)) {
				err = "terrain: " + path + " is " + std::to_string(w) + "x"
					+ std::to_string(h) + " (want 1024x1024)";
				return false;
			}
			const size_t bytes = size_t(w) * size_t(h) * 4;
			std::vector<uint8_t> payload(px, px + bytes);
			// Per-tile world-Y bounds (G-block AABBs at T2 read these).
			{
				float mn = 0, mx = 0;
				smvk::terrain::TileMinMax(payload, meta.hRes, meta.baseY,
					meta.range, mn, mx);
				tileMinMax[tz * 4 + tx][0] = mn;
				tileMinMax[tz * 4 + tx][1] = mx;
			}
			if (!writer.AddBlob(smvk::PakAssetKind::Terrain,
				smvk::terrain::HTileBlobPath(tx, tz), std::move(payload))) {
				err = "terrain: AddBlob failed for " + path;
				return false;
			}
			std::cout << "Terrain H " << smvk::terrain::HTileName(tx, tz)
				<< " (" << bytes << " B raw, Y "
				<< tileMinMax[tz * 4 + tx][0] << ".."
				<< tileMinMax[tz * 4 + tx][1] << ")\n";
		}
	}
	// S-tiles (test mask v1): 256 RGBA8, checker in R.
	for (uint32_t tz = 0; tz < nS; ++tz) {
		for (uint32_t tx = 0; tx < nS; ++tx) {
			const std::string path = terrainDir + "/"
				+ smvk::terrain::STileName(tx, tz) + ".png";
			int w = 0, h = 0, comp = 0;
			stbi_uc* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
			if (!px) {
				err = "terrain: cannot decode " + path + " ("
					+ stbi_failure_reason() + ")";
				return false;
			}
			struct Free {
				stbi_uc* p;
				~Free() { stbi_image_free(p); }
			} free{px};
			if (w != smvk::terrain::kMaskTileRes
				|| h != smvk::terrain::kMaskTileRes) {
				err = "terrain: " + path + " is " + std::to_string(w) + "x"
					+ std::to_string(h) + " (want 256x256)";
				return false;
			}
			const size_t bytes = size_t(w) * size_t(h) * 4;
			std::vector<uint8_t> payload(px, px + bytes);
			if (!writer.AddBlob(smvk::PakAssetKind::Terrain,
				smvk::terrain::STileBlobPath(tx, tz), std::move(payload))) {
				err = "terrain: AddBlob failed for " + path;
				return false;
			}
		}
	}
	std::cout << "Terrain S: " << (nS * nS) << " mask tiles\n";
	// Meta blob (runtime form).
	{
		std::vector<uint8_t> metaBlob;
		smvk::terrain::SerializeTerrainMeta(meta, tileMinMax, metaBlob);
		if (!writer.AddBlob(smvk::PakAssetKind::Terrain,
			smvk::terrain::kTerrainMetaBlobPath, std::move(metaBlob))) {
			err = "terrain: AddBlob failed for terrain/meta";
			return false;
		}
	}
	return true;
}

} // namespace terraincook
