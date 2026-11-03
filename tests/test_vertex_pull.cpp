// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Renderer/vertex_pull.h>
#include <cstdint>
#include <iostream>
#include <limits>
#include <cstdio>
// House test convention (AGENTS.md): the CHECK macro ALWAYS evaluates its
// condition, in every build flavor. This file originally used CHECK(),
// whose NDEBUG no-op makes Release runs vacuous - and any side effect
// inside an assert (see test_gpu_scene_candidates.cpp:
// assert(leaves.Flush(db))) was silently skipped, crashing the run.
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } \
    while (0)
namespace { int g_checks = 0; int g_failures = 0; }

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
    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
