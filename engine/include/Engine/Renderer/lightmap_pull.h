// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Contract-only lightmap vertex-pulling helpers. No descriptor, pipeline,
// atlas sampling or draw producer is connected by this header.
#include <Engine/Renderer/vertex_pull.h>
#include <cstdint>

namespace tour::lightmappull {

inline constexpr uint32_t kNoChart = 0xFFFFFFFFu;
inline constexpr uint32_t kNoLobes = 0xFFFFFFFFu;

struct PackedChartVertex {
    uint32_t packedUv = 0;
    uint32_t chartId = kNoChart;
};
static_assert(sizeof(PackedChartVertex) == vertexpull::kLightmapStrideBytes);

// Explicit presence is necessary: a delta of 0xFFFFFFFF is VALID (-1).
struct ChartStream {
    vertexpull::VertexWindow vertices{};
    uint32_t chartTransformOffset = 0;
    uint32_t chartTransformCount = 0;
    uint32_t lobeCount = kNoLobes;
    bool resident = false;
};

[[nodiscard]] constexpr bool HasLightmap(const ChartStream& s) noexcept {
    return s.resident && s.vertices.vertexCount != 0 && s.chartTransformCount != 0 &&
           s.lobeCount != kNoLobes;
}

[[nodiscard]] constexpr bool ValidLobeCount(uint32_t lobes) noexcept {
    return lobes == 5u || lobes == 6u || lobes == 9u || lobes == 12u;
}

[[nodiscard]] constexpr bool TryChartTransformIndex(const ChartStream& stream,
                                                    uint32_t chartId,
                                                    uint32_t tableSize,
                                                    uint32_t& transformIndex) noexcept {
    if (!HasLightmap(stream) || chartId == kNoChart || chartId >= stream.chartTransformCount)
        return false;
    const uint64_t index = uint64_t(stream.chartTransformOffset) + chartId;
    if (index >= tableSize || index > UINT32_MAX)
        return false;
    transformIndex = static_cast<uint32_t>(index);
    return true;
}

[[nodiscard]] constexpr bool TryVertexByteOffset(const ChartStream& stream,
                                                 uint32_t globalVertexId,
                                                 uint64_t lightmapHeapBytes,
                                                 uint64_t& offset) noexcept {
    if (!HasLightmap(stream))
        return false;
    uint32_t streamVertex = 0;
    return vertexpull::TryMap(stream.vertices, globalVertexId, streamVertex) &&
           vertexpull::TryByteOffset(
               streamVertex, vertexpull::kLightmapStrideBytes, lightmapHeapBytes, offset);
}

[[nodiscard]] constexpr uint16_t UnpackU16(uint32_t packed, uint32_t component) noexcept {
    return static_cast<uint16_t>((packed >> (16u * component)) & 0xFFFFu);
}

} // namespace tour::lightmappull
