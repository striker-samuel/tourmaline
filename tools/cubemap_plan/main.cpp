// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Cascaded world-tile cubemap capture planner (the #338 planning API)
// applied to a scene's cooked geometry. Loads the .geometry artifact,
// builds the trace scene's triangle BVH, and uses it as the octree's
// occlusion predicate so probes land only in open cells (not inside
// geometry).
//
//   cubemap_plan <content-dir> <geometry-vfs-path> [minX minY minZ maxX maxY maxZ]
//
// Defaults to the bistro AABB when the bounds are omitted.
#include <Engine/App/cluster.h>
#include <Engine/GI/bake.h>
#include <Engine/Vfs/vfs.h>
#include <Engine/WorldPartition/CubemapCapturePlanning.h>
#include <gltf_import/asset_export.h>
#include <gltf_import/gltf.h> // BuildFromMeshAsset + VfsTextureSource (global ns)

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace wp = engine::worldpartition;
namespace fs = std::filesystem;

static int usage() {
    std::fprintf(stderr,
                 "usage: cubemap_plan <content-dir> <geometry-vfs-path> "
                 "[minX minY minZ maxX maxY maxZ]\n");
    return 2;
}

int main(int argc, char** argv) {
    if (argc < 3)
        return usage();
    const std::string contentDir = argv[1];
    const std::string geometryVfs = argv[2];

    wp::CubemapCaptureVolume volume{};
    if (argc >= 9) {
        volume.min = {std::strtof(argv[3], nullptr),
                      std::strtof(argv[4], nullptr),
                      std::strtof(argv[5], nullptr)};
        volume.max = {std::strtof(argv[6], nullptr),
                      std::strtof(argv[7], nullptr),
                      std::strtof(argv[8], nullptr)};
    } else {
        // The bistro scene AABB (GiBaker --print-bounds trace bounds).
        volume.min = {-58.6003f, -4.72616f, -72.3586f};
        volume.max = {50.463f, 27.2062f, 42.9172f};
    }
    volume.cascades = {{64.0f, 2.0f}, {256.0f, 3.0f}, {512.0f, 5.0f}};

    // ---- Build the offline cluster from the .geometry artifact (the
    // same derive path the GiBaker's --in-vfs lane uses).
    smvk::OverlayVfs vfs;
    vfs.Mount(std::make_shared<smvk::LooseBackend>(contentDir), 0, "content");
    const std::string artPath = smvk::NormalizeAssetUri(geometryVfs).Str();
    auto artBytes = vfs.TryRead(artPath);
    if (!artBytes || artBytes->empty()) {
        std::fprintf(
            stderr, "artifact read failed: '%s' not under the content root\n", geometryVfs.c_str());
        return 1;
    }
    smvk::assets::MeshAsset meshAsset;
    std::string aerr;
    if (!smvk::assets::LoadMeshAssetBytes(artBytes->data(), artBytes->size(), meshAsset, aerr)) {
        std::fprintf(stderr, "artifact parse failed: %s\n", aerr.c_str());
        return 1;
    }
    // Manifest (identity ledger) beside the artifact; empty = no ledger.
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
    addNode(cluster->scene, -1, 0); // empty scene root
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

    // ---- Build the trace scene + triangle BVH (the occlusion source).
    smvk::gi::GiSceneBuildOptions buildOpts{};
    smvk::gi::GiTraceScene traceScene;
    std::string berr;
    smvk::gi::BuildTraceSceneFromCluster(*cluster, buildOpts, traceScene, berr);
    if (!berr.empty()) {
        std::fprintf(stderr, "trace scene build failed: %s\n", berr.c_str());
        return 1;
    }

    const smvk::CubemapOcclusionRay occluded =
        [&](const glm::vec3& origin, const glm::vec3& dir, float maxDist) {
            return traceScene.Occluded(origin, dir, maxDist);
        };

    // ---- Plan the cascaded captures over the bistro AABB.
    std::vector<wp::CubemapTileCaptureSet> sets;
    std::string error;
    if (!wp::PlanCubemapTileCaptures(volume, occluded, sets, error)) {
        std::fprintf(stderr, "plan failed: %s\n", error.c_str());
        return 1;
    }

    std::printf("cascaded cubemap plan: %zu capture set(s) over %u cascade tiers\n"
                "  occlusion BVH: %u triangles\n",
                sets.size(),
                (unsigned)volume.cascades.size(),
                (unsigned)traceScene.Bvh().TriangleCount());
    size_t totalProbes = 0;
    for (const auto& s : sets) {
        totalProbes += s.probes.size();
        std::printf("  cell (%lld,%lld) cascade %u: %zu probes\n",
                    (long long)s.cell.x,
                    (long long)s.cell.z,
                    s.cascade,
                    s.probes.size());
        for (size_t i = 0; i < s.probes.size() && i < 4; ++i) {
            const auto& p = s.probes[i];
            std::printf(
                "      probe %zu: pos=(%.2f, %.2f, %.2f) influence=%.1f fade=%.1f depth=%u\n",
                i,
                p.position.x,
                p.position.y,
                p.position.z,
                p.influenceRadius,
                p.fadeRadius,
                p.depth);
        }
    }
    std::printf("total probes: %zu\n", totalProbes);
    return 0;
}
