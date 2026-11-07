// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Packed conservative maximum-height pyramid, shared by cook and runtime.
// The 4096x4096 source samples map to a 2048x1024 R16_UNORM atlas:
// level 2 (4x4 texels -> one maximum) at x=0, size 1024;
// levels 3..12 are placed consecutively to its right at x=1024,
// 1536, 1792, ... ; y always starts at zero. No hardware mips are used.
// Height encoding is the SAME terrain baseY/range RG16 UNORM as the source.
// Layout is stable and explicit; a different terrain resolution needs a
// new format, not a silent change to this one.
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include <Engine/Rendergraph/terrain_pack.h>

namespace smvk::terrain {
inline constexpr uint32_t kOcclusionAtlasWidth = 2048;
inline constexpr uint32_t kOcclusionAtlasHeight = 1024;
inline constexpr uint32_t kOcclusionBaseWidth = 1024;
inline constexpr uint32_t kOcclusionFirstLevel = 2;
inline constexpr uint32_t kOcclusionLastLevel = 12;
inline constexpr const char* kOcclusionPyramidPath = "terrain/occlusion/max_pyramid";

// The atlas contains little-endian uint16 samples, unwrapped as R16_UNorm
// by the renderer. Do not assume host endian when emitting pack bytes.
inline void SetOcclusionSample(std::vector<uint8_t>& atlas,
                               uint32_t x,
                               uint32_t z,
                               uint16_t sample) {
    const size_t offset = (size_t(z) * kOcclusionAtlasWidth + x) * 2;
    atlas[offset] = uint8_t(sample & 255);
    atlas[offset + 1] = uint8_t(sample >> 8);
}
inline uint16_t GetOcclusionSample(std::span<const uint8_t> atlas, uint32_t x, uint32_t z) {
    const size_t offset = (size_t(z) * kOcclusionAtlasWidth + x) * 2;
    return uint16_t(atlas[offset]) | uint16_t(atlas[offset + 1]) << 8;
}
inline uint32_t OcclusionLevelX(uint32_t level) {
    if (level <= kOcclusionFirstLevel)
        return 0;
    uint32_t x = kOcclusionBaseWidth;
    for (uint32_t n = 3; n < level; ++n)
        x += (4096u >> n);
    return x;
}
inline std::vector<uint8_t> NewOcclusionAtlas() {
    return std::vector<uint8_t>(size_t(kOcclusionAtlasWidth) * kOcclusionAtlasHeight * 2, 0);
}
// Write one input 1024x1024 RG16/RGBA8 H-tile into the 4x4 max base.
// Values are MAX, never averages: safe for conservative ray skipping.
inline void AccumulateOcclusionTile(std::vector<uint8_t>& atlas,
                                    std::span<const uint8_t> tile,
                                    uint32_t tx,
                                    uint32_t tz) {
    for (uint32_t z = 0; z < 256; ++z)
        for (uint32_t x = 0; x < 256; ++x) {
            uint16_t maxH = 0;
            for (uint32_t dz = 0; dz < 4; ++dz)
                for (uint32_t dx = 0; dx < 4; ++dx) {
                    const size_t i = (size_t(z * 4 + dz) * 1024 + (x * 4 + dx)) * 4;
                    maxH = std::max(maxH, PackHeightR16(tile[i], tile[i + 1]));
                }
            SetOcclusionSample(atlas, tx * 256 + x, tz * 256 + z, maxH);
        }
}
// Build each higher level from the previous one. Every sample bounds
// ALL of its children, including adjacent original terrain H-tiles.
inline void FinishOcclusionAtlas(std::vector<uint8_t>& atlas) {
    for (uint32_t level = 3; level <= kOcclusionLastLevel; ++level) {
        const uint32_t size = 4096u >> level;
        const uint32_t prevX = OcclusionLevelX(level - 1);
        const uint32_t dstX = OcclusionLevelX(level);
        for (uint32_t z = 0; z < size; ++z)
            for (uint32_t x = 0; x < size; ++x) {
                uint16_t maxH = 0;
                for (uint32_t dz = 0; dz < 2; ++dz)
                    for (uint32_t dx = 0; dx < 2; ++dx)
                        maxH = std::max(maxH,
                                        GetOcclusionSample(atlas, prevX + 2 * x + dx, 2 * z + dz));
                SetOcclusionSample(atlas, dstX + x, z, maxH);
            }
    }
}
} // namespace smvk::terrain
