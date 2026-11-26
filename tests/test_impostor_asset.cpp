// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// CPU-only impostor ASSET integration: the .geometry MeshAsset round-trips
// the baked six-view proxy (impostorPositions/impostorIndices), the derive
// fingerprint gates re-derivation on the impostor settings, and the runtime
// selection + sparse cluster registration work offline. No Vulkan.
#include <source_import/asset_export.h>

#include <Engine/App/cluster.h>
#include <Engine/Renderer/static_impostor_gpu.h>

#include <cstdio>
#include <iostream>
#include <string>

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
    using namespace smvk::assets;
    namespace fs = std::filesystem;

    // A minimal single-blob .geometry with a baked impostor proxy: 4 verts /
    // 6 indices (two triangles) for the base, and a distinct proxy (8 verts /
    // 12 indices). The streams must be size-matched (positions == shading) for
    // the packed-SoA writer.
    MeshAsset m;
    MeshAsset::Geometry g;
    g.positions.resize(4);
    g.shading.resize(4);
    for (int v = 0; v < 4; ++v)
        g.positions[v].pos = glm::vec3(float(v & 1), float((v >> 1) & 1), 0.0f);
    g.indices = {0, 1, 2, 2, 1, 3};
    g.surfaces.push_back(smvk::Surface{0, 6, 0});
    g.aabbMin = glm::vec3(0.0f);
    g.aabbMax = glm::vec3(1.0f);
    g.uriFragment = "mesh.0/prim.0";
    // Impostor proxy: the packed heap layout the RT builder uploads verbatim.
    g.impostorPositions.resize(8);
    for (int v = 0; v < 8; ++v)
        g.impostorPositions[v].pos = glm::vec3(float(v & 1), float((v >> 1) & 1), float(v >> 2));
    g.impostorIndices.resize(12);
    for (uint32_t i = 0; i < 12; ++i)
        g.impostorIndices[i] = i % 8;
    m.geometry.push_back(g);
    MeshAsset::Instance inst;
    inst.node = 0;
    inst.geometry = 0;
    inst.materialGuid = "material-guid";
    m.instances.push_back(inst);

    // Round-trip through the .geometry serializer.
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "tourmaline_impostor_asset";
    fs::create_directories(dir, ec);
    const fs::path path = dir / "cube.geometry";
    std::string err;
    Check(SaveMeshAsset(path, m, err), "save mesh asset");
    MeshAsset loaded;
    Check(LoadMeshAsset(path, loaded, err), "load mesh asset");
    Check(loaded.geometry.size() == 1, "one geometry blob");
    Check(loaded.geometry[0].impostorPositions.size() == 8, "impostor positions count");
    Check(loaded.geometry[0].impostorIndices.size() == 12, "impostor indices count");
    bool identical = loaded.geometry[0].impostorPositions.size() == 8 &&
                     loaded.geometry[0].impostorIndices.size() == 12;
    for (size_t v = 0; identical && v < 8; ++v)
        identical = loaded.geometry[0].impostorPositions[v].pos == g.impostorPositions[v].pos;
    for (size_t i = 0; identical && i < 12; ++i)
        identical = loaded.geometry[0].impostorIndices[i] == g.impostorIndices[i];
    Check(identical, "impostor proxy round-trips");

    // A geometry without a proxy stays empty through the round-trip (the
    // count-gated section is absent on disk and reads back as empty).
    MeshAsset plain;
    MeshAsset::Geometry pg;
    pg.positions.resize(3);
    pg.shading.resize(3);
    pg.indices = {0, 1, 2};
    pg.surfaces.push_back(smvk::Surface{0, 3, 0});
    pg.aabbMin = glm::vec3(0.0f);
    pg.aabbMax = glm::vec3(1.0f);
    plain.geometry.push_back(pg);
    const fs::path plainPath = dir / "plain.geometry";
    Check(SaveMeshAsset(plainPath, plain, err), "save plain asset");
    MeshAsset plainLoaded;
    Check(LoadMeshAsset(plainPath, plainLoaded, err), "load plain asset");
    Check(plainLoaded.geometry[0].impostorPositions.empty() &&
              plainLoaded.geometry[0].impostorIndices.empty(),
          "plain geometry has no impostor");

    // The derive fingerprint gates re-derivation on the impostor settings.
    DeriveConfig offCfg;
    offCfg.impostorProxy = false;
    DeriveConfig onCfg;
    onCfg.impostorProxy = true;
    onCfg.impostorResolution = 48;
    Check(ImportFingerprintFor(offCfg) != ImportFingerprintFor(onCfg),
          "impostor flag changes the fingerprint");
    DeriveConfig sameRes;
    sameRes.impostorProxy = true;
    sameRes.impostorResolution = 48;
    Check(ImportFingerprintFor(onCfg) == ImportFingerprintFor(sameRes),
          "fingerprint is canonical (same settings, same bytes)");
    DeriveConfig otherRes;
    otherRes.impostorProxy = true;
    otherRes.impostorResolution = 64;
    Check(ImportFingerprintFor(onCfg) != ImportFingerprintFor(otherRes),
          "impostor resolution changes the fingerprint");

    // ---- runtime selection (pure, angular threshold) -------------------
    Check(!smvk::UseImpostorProxyAt(0.0f, 10.0f), "proxy.gate.zero-radius");
    Check(!smvk::UseImpostorProxyAt(1.0f, 0.0f), "proxy.gate.zero-distance");
    // radius/distance below the threshold -> proxy; above -> full mesh.
    Check(smvk::UseImpostorProxyAt(0.001f, 10.0f), "proxy.select.far");
    Check(!smvk::UseImpostorProxyAt(1.0f, 10.0f), "proxy.select.near");

    // ---- sparse cluster registration (offline, GPU-free) ----------------
    // Only a SMALL subset of meshes carry a proxy; the rest resolve to null.
    {
        auto cluster = std::make_unique<smvk::WorldPage>(nullptr);
        cluster->SetOfflineMode(true);
        smvk::MeshData base{};
        base.vertices.resize(3);
        base.vertices[0].pos = glm::vec3(0, 0, 0);
        base.vertices[1].pos = glm::vec3(1, 0, 0);
        base.vertices[2].pos = glm::vec3(0, 1, 0);
        base.indices = {0, 1, 2};
        base.aabbMin = glm::vec3(0);
        base.aabbMax = glm::vec3(1);
        const auto baseH = cluster->UploadMesh(base);

        smvk::MeshData proxy{};
        proxy.vertices.resize(4); // proxy soup: 4 verts, 2 tris
        proxy.indices = {0, 1, 2, 2, 1, 3};
        proxy.aabbMin = glm::vec3(0);
        proxy.aabbMax = glm::vec3(1);
        const auto proxyH = cluster->UploadMesh(proxy);

        // No proxy registered yet -> null.
        Check(cluster->GetImpostorProxy(baseH) == nullptr, "proxy.cluster.none");

        cluster->RegisterImpostorProxy(baseH, proxyH);
        const smvk::ImpostorProxy* entry = cluster->GetImpostorProxy(baseH);
        Check(entry != nullptr, "proxy.cluster.registered");
        Check(entry->indexCount == 6, "proxy.cluster.index-count");
        Check(entry->vertexCount == 4, "proxy.cluster.vertex-count");

        // A different mesh still has no proxy (sparse).
        smvk::MeshData other{};
        other.vertices.resize(3);
        other.indices = {0, 1, 2};
        other.aabbMin = glm::vec3(0);
        other.aabbMax = glm::vec3(1);
        const auto otherH = cluster->UploadMesh(other);
        Check(cluster->GetImpostorProxy(otherH) == nullptr, "proxy.cluster.sparse");
    }

    std::cout << "checks=" << checks << " failures=" << failures << "\n";
    return failures == 0 ? 0 : 1;
}
