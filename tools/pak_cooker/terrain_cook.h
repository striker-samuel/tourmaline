#pragma once
// --terrain cook: terrain_src dir (terrain.json + h_XX_ZZ.png + m_XX_ZZ.png)
// -> PakAssetKind::Terrain blobs (16x height RGBA8 raw, 256x mask RGBA8 raw,
// 1x terrain/meta). Host-path inputs, validated strictly; B/A channels of
// height tiles are preserved verbatim (free for future use).

#include <string>
#include <vector>

#include <Engine/Asset/pak.h>

namespace terraincook {

// Cooks every terrain blob into writer (compression follows the writer's
// mode). Returns false with err set on any validation/IO failure.
bool CookTerrainBlobs(const std::string& terrainDir,
	smvk::PakWriter& writer, std::string& err);

} // namespace terraincook
