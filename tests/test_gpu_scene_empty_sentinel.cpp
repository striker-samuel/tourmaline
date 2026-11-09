// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Renderer/gpu_scene_raster.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "%s:%d failed: %s\n", __FILE__, __LINE__, #x); \
    std::abort(); \
} } while (false)

int main() {
    using namespace tour::gpu_scene;
    const auto empty = EmptyRasterIndirectCommand();
    CHECK(empty.indexCount == 0);
    CHECK(empty.instanceCount == 0);
#if defined(GS_RASTER_DEBUG_EMPTY_COMMAND_SENTINEL) && GS_RASTER_DEBUG_EMPTY_COMMAND_SENTINEL
    CHECK(empty.firstIndex == 0xDEADBEEFu);
    CHECK(empty.vertexOffset == 0x7BADBEEF);
    CHECK(empty.firstInstance == 0xDEADBEEFu);
#else
    CHECK(empty.firstIndex == 0);
    CHECK(empty.vertexOffset == 0);
    CHECK(empty.firstInstance == 0);
#endif

    const IndexedGeometry geometry{42, 7, -3, 0};
    const auto wrongClass = MakeRasterBinnedCommand(geometry, 19, 1, 0);
    CHECK(wrongClass.indexCount == empty.indexCount);
    CHECK(wrongClass.instanceCount == empty.instanceCount);
    CHECK(wrongClass.firstIndex == empty.firstIndex);
    CHECK(wrongClass.vertexOffset == empty.vertexOffset);
    CHECK(wrongClass.firstInstance == empty.firstInstance);

    const auto valid = MakeRasterBinnedCommand(geometry, 19, 1, 1);
    CHECK(valid.indexCount == 42);
    CHECK(valid.instanceCount == 1);
    CHECK(valid.firstIndex == 7);
    CHECK(valid.vertexOffset == -3);
    CHECK(valid.firstInstance == 19);

    const auto invalidGeometry = MakeRasterBinnedCommand({}, 19, 1, 1);
    CHECK(invalidGeometry.instanceCount == 0);
    CHECK(invalidGeometry.firstInstance == empty.firstInstance);
    return 0;
}
