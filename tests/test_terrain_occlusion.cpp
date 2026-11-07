// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// GPU-free regressions for packed conservative maximum-height hierarchy.
#include <Engine/Rendergraph/terrain_occlusion.h>
#include <cstdint>
#include <cstdio>
#include <vector>

int main() {
    using namespace smvk::terrain;
    auto atlas = NewOcclusionAtlas();
    auto check = [](bool yes, const char* msg) {
        if (!yes)
            std::fprintf(stderr, "FAIL %s\\n", msg);
        return yes;
    };
    bool ok = true;
    ok &= check(atlas.size() == size_t(2048) * 1024 * 2, "packed atlas dimensions");
    ok &=
        check(OcclusionLevelX(2) == 0 && OcclusionLevelX(3) == 1024 && OcclusionLevelX(4) == 1536 &&
                  OcclusionLevelX(6) == 1920 && OcclusionLevelX(12) == 2046,
              "packed level offsets");
    // Two adjacent source H-tiles. A 2x2 parent covering their shared
    // boundary MUST preserve a peak from either side, never average it.
    std::vector<uint8_t> left(1024u * 1024u * 4u, 0);
    std::vector<uint8_t> right(left.size(), 0);
    // Last 4x4 of tile 0: tile-local (1020..1023, 0..3).
    size_t leftPeak = (size_t(2) * 1024 + 1023) * 4;
    left[leftPeak] = 0x12;
    left[leftPeak + 1] = 0x34;
    // First 4x4 of tile 1, larger peak.
    size_t rightPeak = (size_t(3) * 1024 + 1) * 4;
    right[rightPeak] = 0xFE;
    right[rightPeak + 1] = 0xDC;
    // Attribute channels are intentionally irrelevant to max-height.
    right[rightPeak + 2] = 255;
    right[rightPeak + 3] = 200;
    AccumulateOcclusionTile(atlas, left, 0, 0);
    AccumulateOcclusionTile(atlas, right, 1, 0);
    ok &= check(GetOcclusionSample(atlas, 255, 0) == 0x1234, "left tile peak retained");
    ok &= check(GetOcclusionSample(atlas, 256, 0) == 0xFEDC, "right tile peak retained");
    FinishOcclusionAtlas(atlas);
    ok &= check(GetOcclusionSample(atlas, OcclusionLevelX(3) + 127, 0) == 0x1234,
                "left L3 max retained");
    ok &= check(GetOcclusionSample(atlas, OcclusionLevelX(3) + 128, 0) == 0xFEDC,
                "right L3 max retained");
    ok &= check(GetOcclusionSample(atlas, OcclusionLevelX(12), 0) == 0xFEDC,
                "root max includes both tiles");

    // General parent invariant: the encoded parent is >= all four
    // children, across every level and every valid cell.
    for (uint32_t lvl = 3; lvl <= kOcclusionLastLevel; ++lvl) {
        const uint32_t dim = 4096u >> lvl;
        const uint32_t parentX = OcclusionLevelX(lvl);
        const uint32_t childX = OcclusionLevelX(lvl - 1);
        for (uint32_t z = 0; z < dim; ++z)
            for (uint32_t x = 0; x < dim; ++x) {
                uint16_t parent = GetOcclusionSample(atlas, parentX + x, z);
                for (uint32_t dz = 0; dz < 2; ++dz)
                    for (uint32_t dx = 0; dx < 2; ++dx)
                        if (parent < GetOcclusionSample(atlas, childX + 2 * x + dx, 2 * z + dz))
                            ok = false;
            }
    }
    ok &= check(ok, "max hierarchy never underestimates children");
    return ok ? 0 : 1;
}
