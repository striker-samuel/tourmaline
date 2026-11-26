// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// test_light_cookie: end-to-end fixture light-cookie baking. Proves the
// recipe -> Embree ray-sampled spot cookie -> .texture asset -> cluster pool
// -> cookie-uri resolution chain (the renderer's FindTextureByUri(cookieUri)
// lookup) without the editor UI. No GPU required.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/App/cluster.h>
#include <Engine/App/world.h>
#include <Engine/Asset/tour.h>
#include <Engine/GI/light_cookie_bake.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Scene/scene.h>
#include <Engine/Scene/scene_tables.h>
#include <Engine/Utils/LutIo.h>

#include <source_import/asset_export.h>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
    ++g_checks;
    if (cond) {
        std::cout << "PASS " << name << "\n";
    } else {
        ++g_failures;
        std::cout << "FAIL " << name << "\n";
    }
}

// A spot at the origin aiming -Z (45-degree outer cone) plus one occluder:
// a filled 0.8m square at z=-2 centered on the aim axis. The square blocks
// rays with |ndcS| <= 0.2 and |ndcU| <= 0.2 (the z=-2 plane is hit at
// x = 2*ndcS, y = 2*ndcU for a 45-degree cone).
struct Fixture {
    smvk::World world;
    std::string lightUuid = "0000000000000000000000000000ca11";
    std::string occluderUuid = "0000000000000000000000000000dead";
};

void BuildFixture(Fixture& f, SceneLight::Type lightType = SceneLight::Type::Spot) {
    auto cluster = std::make_unique<smvk::WorldPage>(nullptr);
    cluster->SetOfflineMode(true);
    ::TransformHierarchy& cs = cluster->scene;
    const int root = addNode(cs, -1, 0);

    // Occluder quad (two triangles) at z=-2.
    smvk::MeshData m;
    const glm::vec3 p[4] = {
        {-0.4f, -0.4f, -2.0f},
        {0.4f, -0.4f, -2.0f},
        {0.4f, 0.4f, -2.0f},
        {-0.4f, 0.4f, -2.0f},
    };
    for (const auto& q : p) {
        smvk::MeshVertex iv{};
        iv.pos = q;
        iv.color = {1.0f, 1.0f, 1.0f, 1.0f};
        m.vertices.push_back(iv);
    }
    const uint32_t tris[6] = {0, 1, 2, 0, 2, 3};
    m.indices.insert(m.indices.end(), tris, tris + 6);
    m.aabbMin = {-0.4f, -0.4f, -2.0f};
    m.aabbMax = {0.4f, 0.4f, -2.0f};
    const auto mh = cluster->UploadMesh(m);
    const int ocNode = addNode(cs, root, 1);
    SetMesh(cs, uint32_t(ocNode), mh.index());
    SetNodeUuidRaw(cs, uint32_t(ocNode), f.occluderUuid);
    cs.localTransform[ocNode] = glm::mat4(1.0f);

    // Spot at the origin, aim -Z.
    SceneLight light;
    light.type = lightType;
    light.color = glm::vec3(1.0f);
    light.intensity = 100.0f;
    light.range = 20.0f;
    light.innerConeAngle = 0.4f;
    light.outerConeAngle = 0.785398163f; // 45 deg
    const auto lh = cluster->CreateLight(light);
    const int lNode = addNode(cs, root, 1);
    SetLight(cs, uint32_t(lNode), lh.index());
    SetNodeUuidRaw(cs, uint32_t(lNode), f.lightUuid);
    cs.localTransform[lNode] = glm::mat4(1.0f);

    markAsChanged(cs, root);
    recalculateGlobalTransforms(cs);

    f.world.RegisterPage(std::move(cluster));
    f.world.ResolveAndFixup_pages();
}

void TestSpotCookieBake() {
    using namespace smvk;
    Fixture f;
    BuildFixture(f);

    smvk::tour::LightCookieBake recipe;
    recipe.lightUuid = f.lightUuid;
    recipe.occluderUuids = {f.occluderUuid};
    recipe.output = "content/textures/fixture_cookie.texture";
    recipe.resolution = 64;
    recipe.samples = 8;
    recipe.emitterRadius = 0.0f; // crisp edges for exact assertions
    recipe.captureDistance = 5.0f;

    gi::LightCookieBakeResult out;
    std::string err;
    Check(gi::BakeSpotLightCookie(f.world, recipe, out, err), "cookie.bake-ok");
    Check(err.empty(), "cookie.bake-no-error");
    Check(out.resolution == 64, "cookie.resolution");
    Check(out.occluderTriangles == 2, "cookie.occluder-tris");
    Check(out.rgba.size() == size_t(64) * 64 * 4, "cookie.rgba-size");
    // Aim -Z (the spot's -Z node axis), s/u the identity basis.
    Check(out.direction.z < -0.99f && std::abs(out.direction.x) < 1e-4f &&
              std::abs(out.direction.y) < 1e-4f,
          "cookie.aim");

    auto coverage = [&](uint32_t px, uint32_t py) {
        const size_t o = (size_t(py) * 64 + px) * 4;
        return int(out.rgba[o]);
    };
    // The aim ray is blocked by the quad -> black.
    Check(coverage(32, 32) == 0, "cookie.center-blocked");
    // An off-axis ray (ndcS ~ 0.52) clears the quad -> white.
    Check(coverage(48, 32) == 255, "cookie.offaxis-open");
    // The blocked region is the |ndc| <= 0.2 square: ~12x12 px (144), a
    // localized shadow, not a full-frame blackout.
    int dark = 0;
    for (uint32_t py = 0; py < 64; ++py)
        for (uint32_t px = 0; px < 64; ++px)
            if (coverage(px, py) < 128)
                ++dark;
    Check(dark >= 100 && dark <= 220, "cookie.shadow-localized");
}

void TestCookieAssetPipeline() {
    using namespace smvk;
    namespace fs = std::filesystem;
    Fixture f;
    BuildFixture(f);

    smvk::tour::LightCookieBake recipe;
    recipe.lightUuid = f.lightUuid;
    recipe.occluderUuids = {f.occluderUuid};
    recipe.output = "content/textures/fixture_cookie.texture";
    recipe.resolution = 32;
    recipe.samples = 4;
    recipe.emitterRadius = 0.0f;
    recipe.captureDistance = 5.0f;

    gi::LightCookieBakeResult out;
    std::string err;
    Check(gi::BakeSpotLightCookie(f.world, recipe, out, err), "cookie.pipe-bake-ok");

    // Write the source PNG + the .texture descriptor through the shared
    // asset library, then import into the cluster pool — the exact
    // FindTextureByUri(cookieUri) resolution the renderer performs. The
    // scratch tree sits at a RELATIVE path under the CWD: asset URIs are
    // VFS-relative by contract (NormalizeAssetUri rejects absolute/..
    // paths), so an absolute temp dir would never register.
    const fs::path dir = "tourmaline_cookie_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path png = dir / "fixture_cookie.png";
    const fs::path tex = dir / "fixture_cookie.texture";

    Check(engine::LutWritePng(
              png.string().c_str(), int(out.resolution), int(out.resolution), out.rgba.data()),
          "cookie.png-written");

    assets::TextureAsset ta;
    ta.guid = "5c8f1a2b3c4d5e6f708192a3b4c5d6e7";
    ta.name = "fixture_cookie";
    ta.source = "fixture_cookie.png"; // beside the .texture
    ta.colorspace = 1;                // linear: coverage is data, not color
    ta.format = 0;                    // rgba8
    ta.wrap = 1;                      // clamp-edge
    ta.filter = 0;                    // linear
    std::string aerr;
    Check(assets::SaveTextureAsset(tex, ta, aerr), "cookie.texture-written");
    Check(aerr.empty(), "cookie.texture-no-error");

    const TextureHandle h =
        assets::ImportTextureAssetToCluster(*f.world.pages[0], tex, nullptr, aerr);
    Check(h.valid(), "cookie.import-ok");
    Check(f.world.pages[0]->FindTextureByUri(tex.generic_string()).has_value(),
          "cookie.uri-resolves");
    fs::remove_all(dir, ec);
}

void TestCookieRejects() {
    using namespace smvk;
    gi::LightCookieBakeResult out;
    std::string err;

    // A point light is rejected (spot cookies only).
    {
        Fixture f;
        BuildFixture(f, SceneLight::Type::Point);
        smvk::tour::LightCookieBake recipe;
        recipe.lightUuid = f.lightUuid;
        recipe.occluderUuids = {f.occluderUuid};
        recipe.resolution = 32;
        recipe.samples = 4;
        recipe.captureDistance = 5.0f;
        Check(!gi::BakeSpotLightCookie(f.world, recipe, out, err), "cookie.reject-point");
        Check(err.find("not a spot") != std::string::npos, "cookie.reject-point-reason");
    }
    // A stale occluder uuid fails loudly rather than baking a partial cookie.
    {
        Fixture f;
        BuildFixture(f);
        smvk::tour::LightCookieBake recipe;
        recipe.lightUuid = f.lightUuid;
        recipe.occluderUuids = {"00000000000000000000000000000000"};
        recipe.resolution = 32;
        recipe.samples = 4;
        recipe.captureDistance = 5.0f;
        Check(!gi::BakeSpotLightCookie(f.world, recipe, out, err), "cookie.reject-stale-occluder");
        Check(err.find("not found") != std::string::npos, "cookie.reject-stale-occluder-reason");
    }
    // A missing light uuid is rejected.
    {
        Fixture f;
        BuildFixture(f);
        smvk::tour::LightCookieBake recipe;
        recipe.lightUuid = "00000000000000000000000000000001";
        recipe.occluderUuids = {f.occluderUuid};
        recipe.resolution = 32;
        recipe.samples = 4;
        recipe.captureDistance = 5.0f;
        Check(!gi::BakeSpotLightCookie(f.world, recipe, out, err), "cookie.reject-missing-light");
        Check(err.find("not found") != std::string::npos, "cookie.reject-missing-light-reason");
    }
}

} // namespace

int main() {
    TestSpotCookieBake();
    TestCookieAssetPipeline();
    TestCookieRejects();
    std::cout << "light_cookie_tests: " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
