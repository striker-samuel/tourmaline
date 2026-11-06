// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Cook-only rasterization of placed static mesh triangles into the maximum-Y
// height envelope of a terrain H-tile. The original H-tile is left untouched:
// this derived layer is for distant occlusion, NOT terrain collision/rendering.
// A 2.5D projection cannot express caves, underpasses or stacked surfaces.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <Engine/App/cluster.h>
#include <Engine/Rendergraph/terrain_meta.h>
#include <Engine/Rendergraph/terrain_pack.h>

namespace terraincook {

// Overlay every static, placed triangle on an RG16-in-RGBA8 terrain tile.
// Both sides of a triangle count; the highest intersection at each texel
// center wins. B/A are preserved. Returns the number of raised texels.
inline uint64_t RasterPlacedHeight(const smvk::terrain::TerrainMeta& meta,
    uint32_t tx, uint32_t tz, const std::vector<const smvk::Cluster*>& scenes,
    std::vector<uint8_t>& pixels) {
    const int res = int(meta.hRes);
    if (res <= 0 || pixels.size() != size_t(res) * size_t(res) * 4)
        return 0;
    const float step = meta.hTileSize / float(res);
    const float minX = meta.originX + float(tx) * meta.hTileSize;
    const float minZ = meta.originZ + float(tz) * meta.hTileSize;
    uint64_t raised = 0;
    for (const smvk::Cluster* cluster : scenes) {
        if (!cluster)
            continue;
        const auto& scene = cluster->scene;
        for (const MeshRow& row : scene.tables.meshes) {
            if (size_t(row.node) >= scene.globalTransform.size())
                continue;
            const auto handle = cluster->staticMeshes.getHandle(row.mesh);
            if (!handle.valid())
                continue;
            const auto* mesh = cluster->staticMeshes.get(handle);
            // Animated/deformed geometry is not a static bake input.
            if (!mesh || mesh->skinned)
                continue;
            const auto* upload = cluster->GetRetainedUpload(handle);
            if (!upload || upload->indices.size() % 3 != 0)
                continue;
            const glm::mat4& transform = scene.globalTransform[size_t(row.node)];
            const auto vertex = [&](uint32_t i) {
                return glm::vec3(transform * glm::vec4(upload->vertices[i].pos, 1.0f));
            };
            for (size_t tri = 0; tri < upload->indices.size(); tri += 3) {
                const uint32_t ia = upload->indices[tri];
                const uint32_t ib = upload->indices[tri + 1];
                const uint32_t ic = upload->indices[tri + 2];
                if (ia >= upload->vertices.size() || ib >= upload->vertices.size()
                    || ic >= upload->vertices.size())
                    continue;
                const glm::vec3 a = vertex(ia), b = vertex(ib), c = vertex(ic);
                if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(a.z)
                    || !std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(b.z)
                    || !std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z))
                    continue;
                const float abX = b.x - a.x, abZ = b.z - a.z;
                const float acX = c.x - a.x, acZ = c.z - a.z;
                const float det = abX * acZ - abZ * acX;
                // Zero XZ footprint (e.g. an infinitely thin vertical wall)
                // is not representable in a top-down heightfield.
                if (std::abs(det) < 1e-10f)
                    continue;
                const float loX = std::min({ a.x, b.x, c.x });
                const float hiX = std::max({ a.x, b.x, c.x });
                const float loZ = std::min({ a.z, b.z, c.z });
                const float hiZ = std::max({ a.z, b.z, c.z });
                if (hiX < minX || loX >= minX + meta.hTileSize
                    || hiZ < minZ || loZ >= minZ + meta.hTileSize)
                    continue;
                const int x0 = std::max(0, int(std::ceil((loX - minX) / step - 0.5f)));
                const int x1 = std::min(res - 1, int(std::floor((hiX - minX) / step - 0.5f)));
                const int z0 = std::max(0, int(std::ceil((loZ - minZ) / step - 0.5f)));
                const int z1 = std::min(res - 1, int(std::floor((hiZ - minZ) / step - 0.5f)));
                for (int z = z0; z <= z1; ++z) {
                    const float wz = minZ + (float(z) + 0.5f) * step;
                    for (int x = x0; x <= x1; ++x) {
                        const float wx = minX + (float(x) + 0.5f) * step;
                        const float dx = wx - a.x, dz = wz - a.z;
                        const float v = (dx * acZ - dz * acX) / det;
                        const float w = (abX * dz - abZ * dx) / det;
                        const float u = 1.0f - v - w;
                        if (u < -1e-5f || v < -1e-5f || w < -1e-5f)
                            continue;
                        const float y = u * a.y + v * b.y + w * c.y;
                        const float h01 = (y - meta.baseY) / meta.range;
                        if (h01 <= 0.0f)
                            continue;
                        const size_t offset = (size_t(z) * size_t(res) + size_t(x)) * 4;
                        const float old = smvk::terrain::DecodeH01(
                            pixels[offset], pixels[offset + 1]);
                        if (h01 <= old)
                            continue;
                        smvk::terrain::EncodeRG16(h01,
                            pixels[offset], pixels[offset + 1]);
                        ++raised;
                    }
                }
            }
        }
    }
    return raised;
}

} // namespace terraincook
