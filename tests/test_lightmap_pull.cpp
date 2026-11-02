// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Renderer/lightmap_pull.h>
#include <cstdint>
#include <iostream>

using namespace tour::lightmappull;

static_assert(sizeof(PackedChartVertex) == 8);
static_assert(ValidLobeCount(5) && ValidLobeCount(6)
    && ValidLobeCount(9) && ValidLobeCount(12));
static_assert(!ValidLobeCount(7));
static_assert(UnpackU16(0xABCD1234u, 0) == 0x1234u);
static_assert(UnpackU16(0xABCD1234u, 1) == 0xABCDu);

int main() {
    // Valid delta == UINT32_MAX; absence must be a separate flag.
    const ChartStream live{{100, 99, 2}, 8, 3, 6, true};
    const ChartStream offline{{100, 99, 2}, 8, 3, 6, false};
    if (!HasLightmap(live) || HasLightmap(offline) ||
        tour::vertexpull::StreamDelta(live.vertices) != UINT32_MAX) {
        std::cerr << "lightmap residency contract failed\n";
        return 1;
    }
    uint64_t byteOffset = 0;
    if (!TryVertexByteOffset(live, 100, 808, byteOffset) ||
        byteOffset != 99 * 8 ||
        TryVertexByteOffset(live, 102, 808, byteOffset) ||
        TryVertexByteOffset(offline, 100, 808, byteOffset) ||
        TryVertexByteOffset(live, 101, 800, byteOffset)) {
        std::cerr << "lightmap byte addressing failed\n";
        return 1;
    }
    uint32_t index = 0;
    if (!TryChartTransformIndex(live, 0, 16, index) || index != 8 ||
        !TryChartTransformIndex(live, 2, 16, index) || index != 10 ||
        TryChartTransformIndex(live, 3, 16, index) ||
        TryChartTransformIndex(live, kNoChart, 16, index) ||
        TryChartTransformIndex(live, 2, 10, index) ||
        TryChartTransformIndex(offline, 0, 16, index)) {
        std::cerr << "chart table addressing failed\n";
        return 1;
    }
    std::cout << "Lightmap pulling contracts OK\n";
    return 0;
}
