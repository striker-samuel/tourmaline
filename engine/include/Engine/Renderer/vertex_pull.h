// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// CPU-side value contract for future raster vertex pulling. This header does
// not select a renderer, allocate descriptors, or change draw submission.
#include <cstdint>
#include <limits>

namespace tour::vertexpull {

inline constexpr uint32_t kPositionStrideBytes = 12u;
inline constexpr uint32_t kShadingStrideBytes = 16u;
inline constexpr uint32_t kLightmapStrideBytes = 8u;

// sourceBase is in the global vertex/index space; streamBase is in an
// independently packed stream's VERTEX units. The window is half-open.
struct VertexWindow {
    uint32_t sourceBase = 0;
    uint32_t streamBase = 0;
    uint32_t vertexCount = 0;
};

[[nodiscard]] constexpr bool Contains(VertexWindow w, uint32_t vertexId) noexcept {
    return vertexId >= w.sourceBase
        && uint64_t(vertexId) - uint64_t(w.sourceBase) < w.vertexCount;
}

// This deliberately uses uint32 wrapping arithmetic, matching the GPU draw
// ring's additive delta. A delta of UINT32_MAX can be a VALID -1 offset;
// presence must be tracked separately from the delta value.
[[nodiscard]] constexpr uint32_t StreamDelta(VertexWindow w) noexcept {
    return w.streamBase - w.sourceBase;
}
[[nodiscard]] constexpr uint32_t MapUnchecked(uint32_t vertexId,
    uint32_t delta) noexcept {
    return vertexId + delta;
}

[[nodiscard]] constexpr bool TryMap(VertexWindow w, uint32_t vertexId,
    uint32_t& streamVertex) noexcept {
    if (!Contains(w, vertexId))
        return false;
    const uint64_t mapped = uint64_t(w.streamBase)
        + (uint64_t(vertexId) - w.sourceBase);
    if (mapped > std::numeric_limits<uint32_t>::max())
        return false;
    streamVertex = static_cast<uint32_t>(mapped);
    return true;
}

// The checked CPU form can validate upload ranges; the shader fast path
// uses ByteOffsetUnchecked only after the window has been validated.
[[nodiscard]] constexpr uint64_t ByteOffsetUnchecked(uint32_t streamVertex,
    uint32_t strideBytes) noexcept {
    return uint64_t(streamVertex) * strideBytes;
}
[[nodiscard]] constexpr bool TryByteOffset(uint32_t streamVertex,
    uint32_t strideBytes, uint64_t bufferBytes, uint64_t& offset) noexcept {
    if (strideBytes == 0)
        return false;
    const uint64_t value = ByteOffsetUnchecked(streamVertex, strideBytes);
    if (value > bufferBytes || bufferBytes - value < strideBytes)
        return false;
    offset = value;
    return true;
}

} // namespace tour::vertexpull
