// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Standalone GPU-less terrain seam baker regression checks.
// Build in the same environment as other test_terrain_* targets.
#include <Engine/Rendergraph/terrain_seam_ribbon.h>
#include <cmath>
#include <iostream>
#include <vector>

int main() {
    using namespace smvk::terrain;
    TerrainHField f;
    f.meta.worldSize = 4096.0f;
    f.meta.hTileSize = 1024.0f;
    f.meta.hRes = 8;
    f.meta.originX = -2048.0f;
    f.meta.originZ = -2048.0f;
    f.meta.baseY = 0;
    f.meta.range = 1;
    f.tiles.resize(16, std::vector<uint8_t>(8 * 8 * 4, 0));
    const std::vector<glm::vec3> mesh = {
        {-2.0f, -1.0f, 0.0f}, {2.0f, 1.0f, 0.0f}, {2.0f, -1.0f, 2.0f}};
    SeamRibbonOptions options;
    options.width = 1.0f;
    options.sampleStep = 0.1f;
    const auto bake = BakeTerrainSeamRibbons(f, mesh, options);
    if (bake.contactSegments != 1 || bake.indices.size() != 12 || bake.vertices.size() != 6 ||
        bake.skippedTriangles != 0) {
        std::cerr << "Unexpected seam topology\n";
        return 1;
    }
    for (const auto& v : bake.vertices) {
        if (std::abs(v.position.y - options.lift) > 1e-4f || std::abs(v.normal.y - 1.0f) > 1e-4f) {
            std::cerr << "Ribbon fails to conform to terrain\n";
            return 2;
        }
    }
    const auto empty = BakeTerrainSeamRibbons(f, mesh, SeamRibbonOptions{0.0f});
    if (!empty.vertices.empty())
        return 3;
    std::cout << "Terrain seam ribbon bake OK\n";
    return 0;
}
