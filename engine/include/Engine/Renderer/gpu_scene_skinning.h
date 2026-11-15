// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// CPU-only contract for frame-compact GPU-scene skinning work. This is a
// staging component: no draw is admitted to GPU-scene skinning by this file.
// The work table is rebuilt on a retired/acquired flight, then uploaded once.
// A future compute pass can dispatch this dense list and write one shared
// deformed vertex buffer (five uint words per vertex, the Northlight ABI).

#include <Engine/Renderer/gpu_scene.h>

#include <cstdint>
#include <limits>
#include <span>
#include <unordered_set>
#include <vector>

namespace tour::gpu_scene {

// Each input refers to ONE independently animated instance, not a mesh asset.
// Different poses of the same mesh require distinct output ranges.
struct SkinWorkSource {
    uint32_t instanceSlot = InvalidIndex;
    uint32_t instanceGeneration = 0;
    uint32_t paletteOffset = 0;
    uint32_t paletteCount = 0;

    uint32_t skinStreamOffset = 0;
    uint32_t skinStreamCount = 0;
    uint32_t bindVertexBase = 0;
    uint32_t vertexCount = 0;
};

// 48-byte GPU-visible record. Mirrored in gpu_scene_skinning.hlsli.
// identity: instance slot, generation, reserved, reserved
// palette: matrix offset, matrix count, skin-stream offset, vertex count
// vertices: bind-pose vertex base, compact output vertex base, reserved, reserved
struct alignas(16) SkinWorkRow {
    uint32_t identity[4] = {};
    uint32_t palette[4] = {};
    uint32_t vertices[4] = {};
};
static_assert(sizeof(SkinWorkRow) == 48);
static_assert(alignof(SkinWorkRow) == 16);

// The computed output is a dense span [0, vertexCount). Each work item owns
// a disjoint range; output addresses never depend on sparse bind-pose heap IDs.
// No partial publication on failure: the caller must keep legacy ownership.
struct PackedSkinWork {
    std::vector<SkinWorkRow> rows;
    uint32_t vertexCount = 0;
    [[nodiscard]] uint64_t DeformedBytes() const noexcept {
        return uint64_t(vertexCount) * 5u * sizeof(uint32_t);
    }
};

// Fails closed on invalid/duplicate instance generations, short skin streams,
// palette overflow, or output-capacity overflow. Zero-work is valid.
// maxVertices is the real per-flight output allocation capacity, not an HLSL
// declaration bound. Palette indexing is currently RGBA8_UINT (256 joints).
[[nodiscard]] inline bool PackSkinWork(std::span<const SkinWorkSource> sources,
                                       uint32_t maxVertices,
                                       PackedSkinWork& out) {
    PackedSkinWork next{};
    next.rows.reserve(sources.size());
    std::unordered_set<uint32_t> slots;
    slots.reserve(sources.size());
    for (const auto& src : sources) {
        if (src.instanceSlot == InvalidIndex || src.instanceGeneration == 0 ||
            !slots.insert(src.instanceSlot).second ||
            src.vertexCount == 0 || src.skinStreamCount != src.vertexCount ||
            src.paletteCount == 0 || src.paletteCount > 256 ||
            uint64_t(src.paletteOffset) + src.paletteCount > uint64_t(InvalidIndex) ||
            uint64_t(src.skinStreamOffset) + src.skinStreamCount > uint64_t(InvalidIndex) ||
            uint64_t(src.bindVertexBase) + src.vertexCount > uint64_t(InvalidIndex) ||
            src.vertexCount > maxVertices - next.vertexCount) {
            return false;
        }
        SkinWorkRow row{};
        row.identity[0] = src.instanceSlot;
        row.identity[1] = src.instanceGeneration;
        row.palette[0] = src.paletteOffset;
        row.palette[1] = src.paletteCount;
        row.palette[2] = src.skinStreamOffset;
        row.palette[3] = src.vertexCount;
        row.vertices[0] = src.bindVertexBase;
        row.vertices[1] = next.vertexCount;
        next.rows.push_back(row);
        next.vertexCount += src.vertexCount;
    }
    out = std::move(next);
    return true;
}

} // namespace tour::gpu_scene
