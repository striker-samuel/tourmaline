// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Definition-tour -> frozen World (see tour_world.h): the shared
// GiBaker / Cook / PakCooker tour bootstrap (empty root + reference
// bind + texture/material asset imports + command replay, HOST-path
// resolution, editor parity). Definition lines go through tool::out()/
// tool::err() (console by default, editor feed when sunk); replay
// warnings append to the caller's vector; fatal details return in err.

#include <filesystem>

#include <Engine/App/cluster.h>
#include <Engine/App/world.h>
#include <Engine/Asset/tour.h>
#include <Engine/Vfs/vfs.h>

#include <source_import/asset_export.h>
#include <source_import/gltf.h>
#include <source_import/tour_world.h>

#include <tool_log.h>

namespace tourworld {

std::unique_ptr<BuiltWorld> BuildWorldFromDoc(const smvk::tour::Doc& doc,
                                              const BuildOptions& opt,
                                              std::vector<std::string>& warnings,
                                              std::string& err) {
    namespace fs = std::filesystem;
    auto world = std::make_unique<smvk::World>();
    auto cluster = std::make_unique<smvk::WorldPage>(nullptr);
    cluster->SetOfflineMode(true);
    addNode(cluster->m_hierarchy, -1, 0); // empty scene root
    size_t refsApplied = 0;
    std::vector<std::unique_ptr<smvk::WorldPage>> partPages;
    std::vector<std::string> partNames;
    if (!doc.references.empty()) {
        const std::string ownerBase = smvk::vfs::BaseDir(doc.references[0].source);
        refsApplied = ApplySceneReferences(*cluster,
                                           doc,
                                           ownerBase,
                                           nullptr,
                                           nullptr,
                                           nullptr /* HOST bind — editor parity */,
                                           opt.meshOpt,
                                           opt.meshlets,
                                           0,
                                           nullptr,
                                           nullptr,
                                           nullptr,
                                           opt.autoKtx,
                                           !doc.fileNodes.empty() ? &doc.fileNodes : nullptr,
                                           opt.artifactCache,
                                           nullptr /* guidResolver — path-only */,
                                           nullptr /* outImages */,
                                           opt.contentDir /* contentRoot — project-relative
                                                             sources resolve against the project
                                                             tree, not the tool's own CWD copy */
                                           ,
                                           &partPages,
                                           &partNames);
        tool::out() << "Definition: bound " << refsApplied << " reference subtree(s) ("
                    << doc.references.size() << " top-level, " << partPages.size()
                    << " partitioned part cluster(s))\n";
    }
    // Texture-asset POOL imports (ordered; see the tour's textureAsset
    // records): replayed BEFORE the material records (a material tex
    // ref or a light cookie naming a .texture must find its pool
    // entry). A failed import FAILS loudly (the cookie would otherwise
    // resolve empty and bake wrong textures).
    if (opt.importTextureAssets) {
        for (const auto& ta : doc.textureAssets) {
            // Resolve: project-relative records against contentDir
            // FIRST, then as-is for absolute/out-of-project records.
            fs::path hp;
            std::error_code ec;
            if (!opt.contentDir.empty())
                hp = fs::path(opt.contentDir) / ta.path;
            if (hp.empty() || !fs::exists(hp, ec))
                hp = fs::path(ta.path);
            if (!fs::exists(hp, ec)) {
                err = "Tour textureAsset not found: '" + ta.path + "' (under --content-dir '" +
                      opt.contentDir + "', then as-is)";
                tool::err() << err << "\n";
                return nullptr;
            }
            std::string imErr;
            if (smvk::assets::ImportTextureAssetToCluster(*cluster, hp, nullptr, imErr).empty()) {
                err = "Tour textureAsset import failed: '" + ta.path + "': " + imErr;
                tool::err() << err << "\n";
                return nullptr;
            }
        }
        if (!doc.textureAssets.empty())
            tool::out() << "Definition: imported " << doc.textureAssets.size()
                        << " texture asset(s)\n";
    }
    // Material-asset POOL imports (ordered; see the tour's
    // materialAsset records): the editor session minted these into the
    // cluster pool beyond the reference-bound materials, and the tour's
    // assignMaterial/-m indices were recorded against that pool —
    // replay the records in order (the record order IS the pool order)
    // so the indices resolve exactly as authored. A failed import
    // shifts every later index — FAIL loudly instead of baking
    // wrong-material content.
    if (opt.importMaterialAssets) {
        for (const auto& ma : doc.materialAssets) {
            if (ma.editorDefault) {
                smvk::assets::EnsureClusterDefaultMaterial(*cluster);
                continue;
            }
            fs::path hp;
            std::error_code ec;
            if (!opt.contentDir.empty())
                hp = fs::path(opt.contentDir) / ma.path;
            if (hp.empty() || !fs::exists(hp, ec))
                hp = fs::path(ma.path);
            if (!fs::exists(hp, ec)) {
                err = "Tour materialAsset not found: '" + ma.path + "' (under --content-dir '" +
                      opt.contentDir + "', then as-is)";
                tool::err() << err << "\n";
                return nullptr;
            }
            std::string imErr;
            if (smvk::assets::ImportMaterialAssetToCluster(*cluster, hp, nullptr, imErr).empty()) {
                err = "Tour materialAsset import failed: '" + ma.path + "': " + imErr;
                tool::err() << err << "\n";
                return nullptr;
            }
        }
        if (!doc.materialAssets.empty())
            tool::out() << "Definition: imported " << doc.materialAssets.size()
                        << " material asset(s)\n";
    }
    std::vector<std::string> rwarn;
    std::string rerr;
    if (!smvk::tour::ApplyTourToCluster(*cluster, doc, &rwarn, &rerr)) {
        err = "Tour replay failed: " + rerr;
        tool::err() << err << std::endl;
        return nullptr;
    }
    for (auto& w : rwarn)
        warnings.push_back(w);
    auto built = std::make_unique<BuiltWorld>();
    built->commands = doc.commands.size();
    built->refsApplied = refsApplied;
    built->refsTopLevel = doc.references.size();
    world->RegisterPage(std::move(cluster));
    // Partitioned-family part clusters (P1): registered BESIDE the main
    // cluster; the world folds them into one merged scene. The parts'
    // names ride the partNames vector (parallel) but the World merge does
    // not need them (registration order is the scene order).
    for (auto& part : partPages)
        world->RegisterPage(std::move(part));
    built->world = std::move(world);
    tool::out() << "Tour replayed: " << built->commands << " commands\n";
    return built;
}

} // namespace tourworld
