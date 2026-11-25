// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// CPU-only impostor depth-bake + proxy serialization tests. No Vulkan.
#include <Engine/Asset/impostor_depth_bake.h>

#include <cmath>
#include <iostream>
#include <vector>

namespace {
int checks = 0;
int failures = 0;
void Check(bool ok, const char* label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cerr << "FAIL " << label << "\n";
    }
}
} // namespace

int main() {
    using namespace tour::impostor;

    // Outward-facing closed cube (same fixture as the SDF test).
    const std::vector<glm::vec3> p = {{-1, -1, -1},
                                      {1, -1, -1},
                                      {1, 1, -1},
                                      {-1, 1, -1},
                                      {-1, -1, 1},
                                      {1, -1, 1},
                                      {1, 1, 1},
                                      {-1, 1, 1}};
    const std::vector<uint32_t> tris = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                                        1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};

    // ---- six-view atlas + combined proxy ----
    const DepthAtlas atlas = BakeDepthAtlas(p, tris, 32);
    Check(atlas.tileResolution == 32, "atlas resolution");
    Check(atlas.views.size() == 6, "six views");
    Check(atlas.depth.size() == size_t(32) * 32 * 6, "flattened depth size");
    Check(atlas.coverage.size() == atlas.depth.size(), "coverage parallel to depth");
    for (uint8_t c : atlas.coverage)
        Check(c <= 1, "coverage is 0/1");

    const TriangleProxy proxy = BakeImpostorProxy(p, tris, 32, 1.0f);
    Check(!proxy.positions.empty() && !proxy.indices.empty(), "proxy is non-empty");
    Check(proxy.indices.size() % 3 == 0, "proxy is indexed triangles");
    for (uint32_t idx : proxy.indices)
        Check(idx < proxy.positions.size(), "proxy index in range");

    // Deterministic: baking twice yields bit-identical output.
    const TriangleProxy again = BakeImpostorProxy(p, tris, 32, 1.0f);
    Check(again.positions.size() == proxy.positions.size() &&
              again.indices.size() == proxy.indices.size(),
          "deterministic size");
    bool identical = again.positions.size() == proxy.positions.size() &&
                     again.indices.size() == proxy.indices.size();
    for (size_t i = 0; identical && i < proxy.positions.size(); ++i)
        identical = proxy.positions[i] == again.positions[i];
    for (size_t i = 0; identical && i < proxy.indices.size(); ++i)
        identical = proxy.indices[i] == again.indices[i];
    Check(identical, "deterministic content");

    // Proxy stays within the object's extent (no runaway geometry).
    glm::vec3 mn(1e30f), mx(-1e30f);
    for (const glm::vec3& v : proxy.positions) {
        mn = glm::min(mn, v);
        mx = glm::max(mx, v);
    }
    Check(glm::all(glm::greaterThanEqual(mn, glm::vec3(-1.1f))), "proxy min bound");
    Check(glm::all(glm::lessThanEqual(mx, glm::vec3(1.1f))), "proxy max bound");

    // ---- invalid inputs fail closed ----
    Check(BakeDepthAtlas({}, tris, 32).tileResolution == 0, "empty positions rejected");
    Check(BakeDepthAtlas(p, {}, 32).tileResolution == 0, "empty indices rejected");
    Check(BakeDepthAtlas(p, tris, 1).tileResolution == 0, "resolution too small rejected");

    // ---- serialization round-trip ----
    const std::vector<uint8_t> bytes = WriteImpostorProxy(proxy);
    TriangleProxy decoded;
    Check(ParseImpostorProxy(bytes.data(), bytes.size(), decoded), "round-trip parse");
    Check(decoded.positions.size() == proxy.positions.size() &&
              decoded.indices.size() == proxy.indices.size(),
          "round-trip sizes");
    identical = decoded.positions.size() == proxy.positions.size() &&
                decoded.indices.size() == proxy.indices.size();
    for (size_t i = 0; identical && i < proxy.positions.size(); ++i)
        identical = decoded.positions[i] == proxy.positions[i];
    for (size_t i = 0; identical && i < proxy.indices.size(); ++i)
        identical = decoded.indices[i] == proxy.indices[i];
    Check(identical, "round-trip content");

    // Corruption / truncation is rejected transactionally.
    std::vector<uint8_t> bad = bytes;
    bad[0] = 'X';
    TriangleProxy scratch;
    Check(!ParseImpostorProxy(bad.data(), bad.size(), scratch), "reject corrupted magic");
    bad = bytes;
    bad.pop_back();
    Check(!ParseImpostorProxy(bad.data(), bad.size(), scratch), "reject truncated payload");

    std::cout << "checks=" << checks << " failures=" << failures << "\n";
    return failures == 0 ? 0 : 1;
}
