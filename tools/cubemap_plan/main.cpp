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
#include <Engine/WorldPartition/CubemapCaptureDiagnostics.h>
#include <gltf_import/asset_export.h>
#include <gltf_import/gltf.h> // BuildFromMeshAsset + VfsTextureSource (global ns)

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
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
                 "[minX minY minZ maxX maxY maxZ] [--budget N] [--sweep]\n"
                 "  --budget N: default max probes per world cell and tier\n"
                 "  --tier-budgets near mid far: explicit per-cascade maxima\n"
                 "  --sweep: budgets 128, 256, 512 and 1024 (one BVH build)\n");
    return 2;
}

int main(int argc, char** argv) {
    if (argc < 3)
        return usage();
    const std::string contentDir = argv[1];
    const std::string geometryVfs = argv[2];

    wp::CubemapCaptureVolume volume{};
    // Bounds are an optional six-number group; a negative X is valid.
    int next = 3;
    if (next < argc && std::string(argv[next]) != "--budget" &&
        std::string(argv[next]) != "--sweep") {
        if (argc - next < 6)
            return usage();
        float values[6]{};
        for (int i = 0; i < 6; ++i) {
            char* tail = nullptr;
            errno = 0;
            values[i] = std::strtof(argv[next + i], &tail);
            if (errno == ERANGE || tail == argv[next + i] || *tail != '\0' ||
                !std::isfinite(values[i])) {
                std::fprintf(stderr, "invalid capture bound: %s\n", argv[next + i]);
                return 2;
            }
        }
        volume.min = {values[0], values[1], values[2]};
        volume.max = {values[3], values[4], values[5]};
        next += 6;
    } else {
        // Bistro fixture AABB (GiBaker --print-bounds trace bounds).
        volume.min = {-58.6003f, -4.72616f, -72.3586f};
        volume.max = {50.463f, 27.2062f, 42.9172f};
    }
    bool sweep = false;
    bool explicitBudget = false;
    bool explicitTiers = false;
    uint32_t tierBudgets[3]{};
    // No signed/zero/overflow budgets, regardless of host unsigned width.
    const auto parseBudget = [](const std::string& raw, uint32_t& value) {
        if (raw.empty() || raw[0] == '-')
            return false;
        char* tail = nullptr;
        errno = 0;
        const unsigned long parsed = std::strtoul(raw.c_str(), &tail, 10);
        if (errno == ERANGE || tail == raw.c_str() || *tail != '\0' ||
            parsed == 0 || parsed > std::numeric_limits<uint32_t>::max())
            return false;
        value = static_cast<uint32_t>(parsed);
        return true;
    };
    for (int i = next; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--sweep") {
            sweep = true;
        } else if (option == "--budget" && i + 1 < argc) {
            if (explicitBudget) {
                std::fprintf(stderr, "--budget specified more than once\n");
                return 2;
            }
            explicitBudget = true;
            const std::string raw = argv[++i];
            if (!parseBudget(raw, volume.maxProbesPerCell)) {
                std::fprintf(stderr, "invalid positive --budget: %s\n", raw.c_str());
                return 2;
            }
        } else if (option == "--tier-budgets" && i + 3 < argc) {
            if (explicitTiers) {
                std::fprintf(stderr, "--tier-budgets specified more than once\n");
                return 2;
            }
            explicitTiers = true;
            for (uint32_t tier = 0; tier < 3; ++tier) {
                const std::string raw = argv[++i];
                if (!parseBudget(raw, tierBudgets[tier])) {
                    std::fprintf(stderr, "invalid tier budget: %s\n", raw.c_str());
                    return 2;
                }
            }
        } else {
            std::fprintf(stderr, "unknown or incomplete option: %s\n", option.c_str());
            return usage();
        }
    }
    if (sweep && (explicitBudget || explicitTiers)) {
        std::fprintf(stderr, "--sweep cannot be combined with --budget\n");
        return 2;
    }
    volume.cascades = {{64.0f, 2.0f}, {256.0f, 3.0f}, {512.0f, 5.0f}};
    if (explicitTiers)
        for (uint32_t tier = 0; tier < 3; ++tier)
            volume.cascades[tier].maxProbesPerCell = tierBudgets[tier];

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

    // ---- Reuse the same geometry BVH for every budget. Report actual
    // overlap, not the misleading equality of per-tier probe counts.
    const std::vector<uint32_t> budgets = sweep
        ? std::vector<uint32_t>{128, 256, 512, 1024}
        : std::vector<uint32_t>{volume.maxProbesPerCell};
    for (const uint32_t budget : budgets) {
        volume.maxProbesPerCell = budget;
        std::vector<wp::CubemapTileCaptureSet> sets;
        std::string error;
        if (!wp::PlanCubemapTileCaptures(volume, occluded, sets, error)) {
            std::fprintf(stderr, "plan failed at budget %u: %s\n", budget, error.c_str());
            return 1;
        }
        std::printf("cubemap plan: budget=%u per cell/tier, %zu capture sets, "
                    "%zu tiers; BVH=%u triangles\n",
                    budget, sets.size(), volume.cascades.size(),
                    (unsigned)traceScene.Bvh().TriangleCount());
        size_t totalProbes = 0;
        size_t equivalentTiers = 0;
        std::map<std::pair<int64_t, int64_t>, const wp::CubemapTileCaptureSet*> previous;
        for (const auto& set : sets) {
            const auto key = std::make_pair(set.cell.x, set.cell.z);
            const auto it = previous.find(key);
            const auto* prior = it == previous.end() ? nullptr : it->second;
            const auto quality = wp::AnalyzeCubemapCaptureQuality(set, prior);
            const uint32_t tierBudget = volume.cascades[set.cascade].maxProbesPerCell
                ? volume.cascades[set.cascade].maxProbesPerCell : budget;
            totalProbes += set.probes.size();
            if (quality.equivalentToPrevious)
                ++equivalentTiers;
            std::printf("  cell (%lld,%lld) cascade %u target=%.1fm: "
                        "%zu probes (%.1f%% of %u budget); influence radius "
                        "min/median/max=%.2f/%.2f/%.2fm",
                        (long long)set.cell.x, (long long)set.cell.z,
                        set.cascade, (double)volume.cascades[set.cascade].densityMeters,
                        set.probes.size(), 100.0 * double(set.probes.size()) / double(tierBudget),
                        tierBudget,
                        (double)quality.minInfluenceMeters,
                        (double)quality.medianInfluenceMeters,
                        (double)quality.maxInfluenceMeters);
            if (quality.hasPrevious) {
                const double overlap = quality.probes
                    ? 100.0 * double(quality.matchingPreviousPositions) /
                        double(quality.probes)
                    : (prior->probes.empty() ? 100.0 : 0.0);
                std::printf("; prior-tier position overlap=%.1f%%, equivalent=%s",
                            overlap, quality.equivalentToPrevious ? "yes" : "no");
            }
            std::printf("\n");
            previous[key] = &set;
        }
        std::printf("  total probes=%zu; equivalent adjacent cell-tiers=%zu\n",
                    totalProbes, equivalentTiers);
        if (equivalentTiers)
            std::printf("  NOTE: matching placements do not justify distinct "
                        "capture tiers until bake/render quality is measured.\n");
    }
    return 0;
}
