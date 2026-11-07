// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Renderer/World2DRenderer.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <vector>
// House test convention (AGENTS.md): the CHECK macro ALWAYS evaluates its
// condition, in every build flavor. This file originally used CHECK(),
// whose NDEBUG no-op makes Release runs vacuous - and any side effect
// inside an assert (see test_gpu_scene_candidates.cpp:
// assert(leaves.Flush(db))) was silently skipped, crashing the run.
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                                          \
    } while (0)
namespace {
int g_checks = 0;
int g_failures = 0;
} // namespace

using namespace tour;

int main() {
    // A 6x6 atlas page fits exactly one padded 4x4 tile.
    SpriteAtlas2D atlas(6);
    const std::vector<uint8_t> red(4 * 4 * 4, 17);
    const uint32_t a = atlas.Insert(4, 4, red);
    const uint32_t b = atlas.Insert(4, 4, red);
    CHECK(a == 0 && b == 1);
    CHECK(atlas.PageCount() == 2);
    CHECK(atlas.Region(a)->page == 0);
    CHECK(atlas.Region(b)->page == 1);
    // The atlas stores UVs as N * (1/pageSize): NOT bit-identical to
    // N/pageSize (reciprocal-multiply rounds differently by 1 ulp) -
    // compare with an epsilon, never ==.
    CHECK(std::abs(atlas.Region(a)->uvMin.x - 1.0f / 6.0f) < 1e-6f);
    CHECK(std::abs(atlas.Region(a)->uvMax.x - 5.0f / 6.0f) < 1e-6f);
    CHECK(atlas.PagePixels(0)[0] == 17); // extruded top-left corner
    CHECK(atlas.PagePixels(0)[(5 * 6 + 5) * 4] == 17);
    CHECK(atlas.Insert(5, 4, red) == kInvalidSprite2D);
    CHECK(atlas.Insert(4, 4, std::span<const uint8_t>(red.data(), 1)) == kInvalidSprite2D);
    CHECK(atlas.PageCount() == 2);

    std::array<Sprite2D, 5> sprites{};
    sprites[0].region = a;
    sprites[0].z = 3.5f;
    sprites[1].region = b;
    sprites[1].z = -1.0f;
    sprites[2].region = a;
    sprites[2].z = 3.5f;
    sprites[3].region = b;
    sprites[3].z = 3.5f;
    sprites[4].region = kInvalidSprite2D;
    auto batch = BuildSpriteBatch2D(atlas, sprites);
    CHECK(batch.instances.size() == 4);
    CHECK(batch.minZ == -1.0f && batch.maxZ == 3.5f);
    CHECK(batch.runs.size() == 3);
    CHECK(batch.runs[0].page == 1 && batch.runs[0].firstInstance == 0);
    CHECK(batch.runs[1].page == 0 && batch.runs[1].count == 2);
    CHECK(batch.runs[2].page == 1 && batch.runs[2].firstInstance == 3);
    CHECK(batch.instances[1].z == 3.5f);
    // Stable sort keeps equal-Z insertion order. No texture sorting that
    // would put b between the two a sprites (or overdraw incorrectly).
    sprites[2].z = std::numeric_limits<float>::quiet_NaN();
    CHECK(BuildSpriteBatch2D(atlas, sprites).instances.size() == 3);
    const auto revision = atlas.Revision();
    atlas.Clear();
    CHECK(atlas.Revision() != revision && atlas.Region(a) == nullptr);
    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
