// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Renderer/vertex_pull.h>
#include <cstdint>
#include <iostream>
#include <limits>

using namespace tour::vertexpull;

static_assert(kPositionStrideBytes == 12);
static_assert(kShadingStrideBytes == 16);
static_assert(kLightmapStrideBytes == 8);
static_assert(Contains(VertexWindow{100, 4, 3}, 100));
static_assert(Contains(VertexWindow{100, 4, 3}, 102));
static_assert(!Contains(VertexWindow{100, 4, 3}, 103));
static_assert(StreamDelta(VertexWindow{100, 99, 2}) == 0xFFFFFFFFu);
static_assert(MapUnchecked(100, 0xFFFFFFFFu) == 99);
static_assert(ByteOffsetUnchecked(10, kPositionStrideBytes) == 120);

int main() {
    uint32_t mapped = 0;
    const VertexWindow lightmap{100, 4, 3};
    if (!TryMap(lightmap, 100, mapped) || mapped != 4 ||
        !TryMap(lightmap, 102, mapped) || mapped != 6 ||
        TryMap(lightmap, 103, mapped) ||
        TryMap(VertexWindow{std::numeric_limits<uint32_t>::max() - 1,
                            std::numeric_limits<uint32_t>::max(), 2},
               std::numeric_limits<uint32_t>::max(), mapped)) {
        std::cerr << "vertex window mapping failed\n";
        return 1;
    }
    uint64_t byteOffset = 0;
    if (!TryByteOffset(6, kLightmapStrideBytes, 56, byteOffset) ||
        byteOffset != 48 ||
        TryByteOffset(7, kLightmapStrideBytes, 56, byteOffset) ||
        TryByteOffset(0, 0, 56, byteOffset)) {
        std::cerr << "vertex byte range failed\n";
        return 1;
    }
    std::cout << "Vertex pulling contracts OK\n";
    return 0;
}
