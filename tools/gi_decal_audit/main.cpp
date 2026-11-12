// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Diagnostic: classify every mesh in a cooked .geometry by name substring
// + material alphaMode/castsShadows, so we can see how many "decal"
// overlays enter the GI bake BVH as solid occluders (and would shorten
// probe visibility distances / push cells into the weighted path).
//
//   gi_decal_audit <content-dir> <geometry-vfs-path> [substring]
//
// Default substring is "decal" (case-insensitive).
#include <Engine/App/cluster.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Scene/scene.h>
#include <Engine/Scene/scene_tables.h>
#include <Engine/Vfs/vfs.h>
#include <gltf_import/asset_export.h>
#include <gltf_import/gltf.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::string lower(std::string s) {
    std::transform(
        s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static const char* modeName(tour::AlphaMode m) {
    switch (m) {
    case tour::AlphaMode::Opaque:
        return "Opaque";
    case tour::AlphaMode::Mask:
        return "Mask";
    case tour::AlphaMode::Blend:
        return "Blend";
    }
    return "?";
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: gi_decal_audit <content-dir> <geometry-vfs-path> "
                     "[substring]\n");
        return 2;
    }
    const std::string contentDir = argv[1];
    const std::string geometryVfs = argv[2];
    const std::string needle = lower(argc >= 4 ? argv[3] : "decal");

    smvk::OverlayVfs vfs;
    vfs.Mount(std::make_shared<smvk::LooseBackend>(contentDir), 0, "content");
    const std::string artPath = smvk::NormalizeAssetUri(geometryVfs).Str();
    auto artBytes = vfs.TryRead(artPath);
    if (!artBytes || artBytes->empty()) {
        std::fprintf(stderr, "artifact read failed: '%s'\n", geometryVfs.c_str());
        return 1;
    }
    smvk::assets::MeshAsset meshAsset;
    std::string aerr;
    if (!smvk::assets::LoadMeshAssetBytes(artBytes->data(), artBytes->size(), meshAsset, aerr)) {
        std::fprintf(stderr, "artifact parse failed: %s\n", aerr.c_str());
        return 1;
    }
    std::string stem = smvk::vfs::FileName(artPath);
    const size_t mdot = stem.rfind(".geometry");
    if (mdot != std::string::npos)
        stem = stem.substr(0, mdot);
    smvk::assets::AssetManifest meshManifest;
    if (auto metaBytes =
            vfs.TryRead(smvk::vfs::Join(smvk::vfs::BaseDir(artPath), stem + ".gltf.meta"))) {
        if (!metaBytes->empty()) {
            std::string merr;
            smvk::assets::LoadAssetManifestText(
                std::string(metaBytes->begin(), metaBytes->end()), meshManifest, merr);
        }
    }

    auto cluster = std::make_unique<smvk::Cluster>(nullptr);
    cluster->SetOfflineMode(true);
    addNode(cluster->scene, -1, 0);
    auto readSibling = [&](const std::string& file) -> std::optional<std::vector<uint8_t>> {
        const std::string p = smvk::vfs::Join(smvk::vfs::BaseDir(artPath), file);
        if (vfs.Exists(p))
            return vfs.TryRead(p);
        return std::nullopt;
    };
    auto texSrc = std::make_shared<VfsTextureSource>(&vfs, smvk::vfs::BaseDir(artPath));
    const size_t built = BuildFromMeshAsset(
        *cluster, nullptr, meshAsset, meshManifest, artPath, 0, texSrc, true, nullptr, readSibling);
    if (built == 0) {
        std::fprintf(stderr, "artifact built 0 nodes\n");
        return 1;
    }

    ::Scene& scene = cluster->scene;

    // Buckets: [match?] x [alphaMode] x [castsShadows].
    struct Row {
        uint64_t meshes = 0;
        uint64_t tris = 0;
    };
    Row bucket[2][3][2]{}; // [isNeedle][Opaque/Mask/Blend][casts]

    uint64_t totalMeshes = 0, totalTris = 0;
    uint64_t needleMeshes = 0, needleTris = 0;

    std::vector<std::string> needleNames;

    for (const MeshRow& row : scene.tables.meshes) {
        const std::string name = lower(getNodeName(scene, int(row.node)));
        const bool isNeedle = name.find(needle) != std::string::npos;
        ++totalMeshes;

        const MaterialRow* matRow = FindMaterial(scene, row.node);
        tour::AlphaMode am = tour::AlphaMode::Opaque;
        bool casts = true;
        if (matRow && matRow->material != 0xFFFFFFFFu) {
            const smvk::ClusterMaterialHandle h = cluster->materials.getHandle(matRow->material);
            if (h.valid()) {
                if (const tour::Material* m = cluster->materials.get(h)) {
                    am = m->alphaMode;
                    casts = m->castsShadows;
                }
            }
        }
        // Triangle count: resolve the mesh upload the trace build would use.
        uint64_t tris = 0;
        bool stale = false;
        const smvk::ClusterStaticMeshHandle mh = cluster->staticMeshes.getHandle(row.mesh);
        if (mh.valid())
            if (const tour::MeshData* up = cluster->GetRetainedUpload(mh))
                tris = up->indices.size() / 3;

        bucket[isNeedle ? 1 : 0][int(am)][casts ? 1 : 0].meshes += 1;
        bucket[isNeedle ? 1 : 0][int(am)][casts ? 1 : 0].tris += tris;
        totalTris += tris;
        if (isNeedle) {
            ++needleMeshes;
            needleTris += tris;
            if (needleNames.size() < 40)
                needleNames.push_back(getNodeName(scene, int(row.node)) + " [" + modeName(am) +
                                      (casts ? ", cast" : ", NOcast") + "]");
        }
    }

    std::printf("geometry: %s\n", geometryVfs.c_str());
    std::printf("substring: \"%s\"\n", needle.c_str());
    std::printf("total: %llu meshes, %llu triangles\n",
                (unsigned long long)totalMeshes,
                (unsigned long long)totalTris);
    std::printf("matching: %llu meshes, %llu triangles\n",
                (unsigned long long)needleMeshes,
                (unsigned long long)needleTris);

    static const char* modes[3] = {"Opaque", "Mask", "Blend"};
    std::printf("\n  %-8s | %-10s | %-8s | %12s | %12s\n",
                "class",
                "alphaMode",
                "casts",
                "meshes",
                "triangles");
    std::printf("  ---------+------------+----------+--------------+--------------\n");
    for (int m = 0; m < 2; ++m) {
        for (int a = 0; a < 3; ++a) {
            for (int c = 0; c < 2; ++c) {
                const Row& r = bucket[m][a][c];
                if (r.meshes == 0)
                    continue;
                std::printf("  %-8s | %-10s | %-8s | %12llu | %12llu\n",
                            m ? "MATCH" : "other",
                            modes[a],
                            c ? "yes" : "NO",
                            (unsigned long long)r.meshes,
                            (unsigned long long)r.tris);
            }
        }
    }

    std::printf("\n  sample matching nodes (up to 40):\n");
    for (const auto& n : needleNames)
        std::printf("    %s\n", n.c_str());
    return 0;
}
