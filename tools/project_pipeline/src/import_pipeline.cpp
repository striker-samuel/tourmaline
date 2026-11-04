// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// The canonical project import (see import_pipeline.h). Flow:
//   validate -> copy the source + deps into a STAGING dir under
//   .tourmaline/ -> derive (ImportMeshSource — the single glTF-aware
//   importer, with the request's settings) -> VALIDATE the staged family
//   (round-trip + the requested preprocessing actually happened) ->
//   PUBLISH into content/models/<stem>/ (remove the previous derived
//   family, move the staged one in; user-authored .texture files are
//   NEVER touched) -> update the registry. A failure at any point leaves
//   the previous live generation untouched.

#include <project_pipeline/import_pipeline.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <system_error>

#include <gltf_import/asset_export.h>
#include <gltf_import/source_formats.h>

#include "registry_detail.h"

namespace smvk::project {

namespace {

// The mesh-source gate (one shared place — see source_formats.h).
bool IsModelSource(const std::filesystem::path& p) {
	return smvk::assets::IsMeshSourceFile(p);
}

// The derived-family file patterns (the publish step replaces exactly
// these; everything else in the destination — user-authored .texture
// settings, hand-placed files — survives a reimport).
bool IsDerivedFamilyFile(const std::string& name) {
	auto endsWith = [&](const char* suffix) {
		const size_t n = std::strlen(suffix);
		return name.size() >= n
			&& name.compare(name.size() - n, n, suffix) == 0;
	};
	// The identity ledger sidecar of ANY mesh source format
	// (<stem>.gltf.meta / .glb.meta / .obj.meta — see
	// IsMeshSourceMetaSidecar), not just the glTF spelling.
	return endsWith(".geometry")
		|| smvk::assets::IsMeshSourceMetaSidecar(name)
		|| endsWith(".material") || endsWith(".anim")
        || endsWith(".packed.png");
}

bool CopyFileTo(const std::filesystem::path& from,
	const std::filesystem::path& to, std::string& err) {
	std::error_code ec;
	std::filesystem::create_directories(to.parent_path(), ec);
	std::filesystem::copy_file(from, to,
		std::filesystem::copy_options::overwrite_existing, ec);
	if (ec) {
		err = "copy failed '" + from.generic_string() + "' -> '"
			+ to.generic_string() + "': " + ec.message();
		return false;
	}
	return true;
}

} // namespace

// The authored recipe specifies every output channel, including color,
// opacity, normal and emissive. Canonical output channels keep the
// shader's baseColor/normal/emissive sample paths unchanged and use the
// existing channel-select word for the scalar map.
bool ComposeMaterialFromRecipe(const MaterialPackingRule& rule,
    smvk::assets::MaterialAsset& material,
    const std::filesystem::path& outputDir, std::string& err) {
    if (rule.channels.empty())
        return true;
    using smvk::assets::MaterialAsset;
    using smvk::assets::PackedImageChannel;
    const MaterialAsset original = material;

    struct Group {
        const char* name;
        std::array<const char*, 4> targets;
        int count;
        bool srgb;
    };
    const Group groups[] = {
        { "baseColor", { "baseColor.r", "baseColor.g", "baseColor.b", "baseColor.a" }, 4, true },
        { "normal", { "normal.r", "normal.g", "normal.b", "" }, 3, false },
        { "emissive", { "emissive.r", "emissive.g", "emissive.b", "" }, 3, true },
        { "scalar", { "ao", "cavity", "roughness", "metallic" }, 4, false }
    };
    const auto slot = [&](const std::string& name) -> const MaterialAsset::TexRef* {
        if (name == "baseColor") return &original.baseColor;
        if (name == "normal") return &original.normalTex;
        if (name == "emissive") return &original.emissiveTex;
        if (name == "occlusion") return &original.occlusion;
        if (name == "cavity") return &original.cavity;
        if (name == "metallicRoughness") return &original.metallicRoughness;
        return nullptr;
    };
    for (const Group& group : groups) {
        bool changed = false;
        for (int i = 0; i < group.count; ++i)
            for (const auto& ruleChannel : rule.channels)
                changed |= ruleChannel.target == group.targets[i];
        if (!changed)
            continue;
        std::array<PackedImageChannel, 4> inputs{};
        for (int i = 0; i < 4; ++i) {
            inputs[i].constant = group.name == std::string("normal")
                ? uint8_t(i == 0 || i == 1 ? 128 : 255) : uint8_t(255);
        }
        // Native glTF defaults for scalar texture lanes: occlusion R,
        // cavity absent=1, roughness G, metallic B.
        const MaterialAsset::TexRef* defaults[4] = {
            &original.occlusion, &original.cavity,
            &original.metallicRoughness, &original.metallicRoughness };
        if (std::string(group.name) == "scalar") {
            for (int i = 0; i < 4; ++i) {
                inputs[i].path = defaults[i]->path.empty()
                    ? std::filesystem::path{} : outputDir / defaults[i]->path;
                inputs[i].component = i == 2 ? 1 : i == 3 ? 2 : 0;
            }
        } else {
            const auto* tex = slot(group.name);
            for (int i = 0; i < group.count; ++i) {
                inputs[i].path = tex->path.empty()
                    ? std::filesystem::path{} : outputDir / tex->path;
                inputs[i].component = i;
            }
        }
        for (const auto& channel : rule.channels) {
            for (int i = 0; i < group.count; ++i) {
                if (channel.target != group.targets[i])
                    continue;
                if (channel.source == "one" || channel.source == "zero") {
                    inputs[i].path.clear();
                    inputs[i].constant = channel.source == "one" ? 255 : 0;
                } else {
                    const auto* tex = slot(channel.source);
                    if (!tex || tex->path.empty()) {
                        err = "material '" + material.name + "' has no source texture for " +
                            channel.target + ": " + channel.source;
                        return false;
                    }
                    inputs[i].path = outputDir / tex->path;
                    const auto index = std::string("rgba").find(channel.channel);
                    inputs[i].component = static_cast<int>(index);
                }
            }
        }
        const std::filesystem::path file =
            material.name.empty() ? std::filesystem::path("material") :
            std::filesystem::path(material.guid);
        const std::filesystem::path result = outputDir /
            (file.string() + "." + group.name + ".packed.png");
        if (!smvk::assets::ComposeMaterialImage(result, inputs, err)) {
            err = "material '" + material.name + "' output " + group.name +
                ": " + err;
            return false;
        }
        MaterialAsset::TexRef output;
        output.path = result.filename().generic_string();
        output.srgb = group.srgb;
        if (std::string(group.name) == "scalar") {
            material.occlusion = output;
            material.cavity = output;
            material.metallicRoughness = output;
            // Canonical packed RGBA = AO, cavity, roughness, metallic.
            material.channelSelectors = (1 << 2) | (2 << 4) | (3 << 6);
        } else if (std::string(group.name) == "baseColor") {
            material.baseColor = output;
        } else if (std::string(group.name) == "normal") {
            material.normalTex = output;
        } else {
            material.emissiveTex = output;
        }
    }
    material.packingSource = "compose";
    material.packingSignature = rule.Signature();
    return true;
}

bool ApplyMaterialPackingRule(const MaterialPackingRule& rule,
    smvk::assets::MaterialAsset& material) {
    if (rule.source.empty())
        return false;
    const auto source = rule.source == "metallicRoughness"
        ? material.metallicRoughness : material.occlusion;
    if (source.path.empty())
        return false;
    material.metallicRoughness = source;
    material.occlusion = source;
    material.cavity = source;
    material.packingSource = rule.source;
    material.channelSelectors = rule.PackedWord();
    return true;
}

bool MaterialPackingRuleMatches(const MaterialPackingRule& rule,
    const smvk::assets::MaterialAsset& material) {
    if (!rule.channels.empty())
        return material.packingSource == "compose" &&
            material.packingSignature == rule.Signature();
    if (rule.source.empty())
        return material.packingSource.empty() && material.channelSelectors == -1;
    if (!material.packingSource.empty())
        return material.packingSource == rule.source &&
            material.channelSelectors == rule.PackedWord();
    const bool available = rule.source == "metallicRoughness"
        ? !material.metallicRoughness.path.empty() : !material.occlusion.path.empty();
    return !available && material.channelSelectors == -1;
}

bool ImportProjectAsset(const ImportRequest& request,
	ImportResult& out, std::string& err) {
	err.clear();
	out = ImportResult{};
	if (!request.project.Valid()) {
		err = "invalid project (open/init first)";
		return false;
	}
	if (request.source.empty() || !IsModelSource(request.source)) {
		err = "the source must be a .gltf/.glb/.obj: "
			+ request.source.generic_string();
		return false;
	}
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path src = fs::absolute(request.source, ec);
	if (ec || !fs::is_regular_file(src, ec)) {
		err = "source not found: " + request.source.generic_string();
		return false;
	}
	const std::string stem = src.stem().string();
	const fs::path destDir =
		fs::path(request.project.ImportsDir()) / stem;
	const fs::path projectSource = destDir / src.filename();

	// ---- idempotence: unchanged content + unchanged settings = a no-op --
	// (The project copy is the import source of record; when it exists
	// and its family is current under THIS request's settings, there is
	// nothing to write. The external source may have changed — that only
	// matters when it differs from the project copy, checked below.)
	if (!request.force && fs::is_regular_file(projectSource, ec)) {
		if (!smvk::assets::MeshArtifactStale(projectSource,
				request.settings) &&
                [&]() {
                    smvk::assets::AssetManifest current;
                    std::string error;
                    if (!smvk::assets::LoadAssetManifest(projectSource, current, error))
                        return false;
                    for (const auto& entry : current.entries) {
                        if (entry.kind != smvk::assets::AssetKind::Material)
                            continue;
                        smvk::assets::MaterialAsset material;
                        if (!smvk::assets::LoadMaterialAsset(
                                projectSource.parent_path() / entry.file, material, error) ||
                                !MaterialPackingRuleMatches(request.materialPacking, material))
                            return false;
                    }
                    return true;
                }()) {
			out.projectSource = projectSource;
			out.geometry =
				fs::path(projectSource).replace_extension(".geometry");
			smvk::assets::AssetManifest m;
			std::string merr;
			if (smvk::assets::LoadAssetManifest(projectSource, m, merr))
				out.guid = m.guid;
			out.derived = false;
			return true; // current — nothing written
		}
	}

	// ---- stage: a full working copy under .tourmaline/stage/<stem>/ ----
	// (Deterministic derivation, so the staged family is byte-identical
	// to what a direct derivation beside the source would produce.)
	const fs::path stageRoot =
		fs::path(request.project.DerivedDataDir()) / "stage" / stem;
	std::filesystem::remove_all(stageRoot, ec);
	fs::create_directories(stageRoot, ec);
	const fs::path stageSource = stageRoot / src.filename();
	if (!CopyFileTo(src, stageSource, err))
		return false;
	// The dependency closure (the single glTF-aware enumeration — this
	// module parses through the importer's own tinygltf path).
	for (const std::string& dep :
			smvk::assets::GltfExternalDependencies(src)) {
		const fs::path depFrom = fs::path(dep).is_absolute()
			? fs::path(dep) : src.parent_path() / fs::path(dep);
		if (!fs::is_regular_file(depFrom, ec)) {
			err = "source dependency missing: '" + dep + "' (beside "
				+ src.generic_string() + ")";
			return false;
		}
		if (!CopyFileTo(depFrom, stageRoot / fs::path(dep), err))
			return false;
	}

	// ---- resolve the family identity BEFORE importing (the explicit
	// .meta resolution the derivation consumes): the PUBLISHED
	// manifest beside the project copy is the identity ledger — its
	// family guid + its subasset rows carry across the content edit
	// (the staged derive cannot see it: the stage dir is fresh).
	smvk::assets::AssetManifest prevLedger;
	bool haveLedger = false;
	{
		smvk::assets::AssetManifest m;
		std::string merr;
		if (smvk::assets::LoadAssetManifest(projectSource, m, merr)) {
			prevLedger = std::move(m);
			haveLedger = true;
		}
	}

	// ---- derive (the ONE importer; the request's settings) -----------
	smvk::assets::AssetManifest manifest;
	smvk::assets::DeriveStats stats;
	if (!smvk::assets::ImportMeshSource(stageSource,
			haveLedger ? prevLedger.guid : "",
			haveLedger ? &prevLedger : nullptr,
			request.settings, manifest, &stats, &err)) {
		err = "import failed: " + err;
		return false; // stage stays for diagnostics? no — cleaned below
	}

    // Resolve the .tourrecipe's texture-packing rule during import.
    // Store channels and texture bindings with the derived material, not
    // in .tour scenes. Cooker and editor then consume identical metadata.
    for (const auto& entry : manifest.entries) {
        if (entry.kind != smvk::assets::AssetKind::Material)
            continue;
        const fs::path materialPath = stageRoot / entry.file;
        smvk::assets::MaterialAsset mat;
        if (!smvk::assets::LoadMaterialAsset(materialPath, mat, err))
            return false;
        if (request.materialPacking.channels.empty()) {
            ApplyMaterialPackingRule(request.materialPacking, mat);
        } else if (!ComposeMaterialFromRecipe(request.materialPacking,
                mat, stageRoot, err)) {
            return false;
        }
        if (!smvk::assets::SaveMaterialAsset(materialPath, mat, err))
            return false;
    }

	// ---- validate the staged family BEFORE publishing ---------------------
	// (a) Round-trip: the written .geometry parses back.
	const fs::path stageGeometry =
		fs::path(stageSource).replace_extension(".geometry");
	smvk::assets::MeshAsset asset;
	std::string verr;
	if (!smvk::assets::LoadMeshAsset(stageGeometry, asset, verr)) {
		err = "validation failed — the staged .geometry does not parse: "
			+ verr;
		return false;
	}
	// (b) The requested preprocessing ACTUALLY happened: a requested
	// meshlet/LOD pass that produced nothing is an error, never a silent
	// success (the runtime would fall back to direct draws and the
	// renderer's mesh-shader path would silently vanish). Tiny meshes
	// legitimately produce no siblings (lodMinTriangles/minReduction) —
	// the LOD rule fires only when CHAINABLE geometry (enough triangles
	// to simplify) produced no chain at all.
	uint32_t blobsWithMeshlets = 0, lodLevels = 0, chainableBlobs = 0;
	for (const auto& g : asset.geometry) {
		if (!g.meshlets.empty())
			++blobsWithMeshlets;
		if (g.indices.size() / 3 > 100)
			++chainableBlobs;
	}
	for (const auto& chain : asset.lodChains)
		lodLevels += uint32_t(chain.size());
	if (request.settings.meshlets && !asset.geometry.empty()
			&& blobsWithMeshlets == 0) {
		err = "validation failed — meshlets were requested but no cooked"
			" set derived (the importer's meshlet pass produced nothing)";
		return false;
	}
	if (request.settings.lods && chainableBlobs > 0 && lodLevels == 0) {
		err = "validation failed — an LOD chain was requested but no"
			" sibling level derived despite chainable geometry (the"
			" simplifier produced nothing)";
		return false;
	}
	// (c) Manifest present + parseable beside the staged source.
	{
		smvk::assets::AssetManifest check;
		std::string cerr;
		if (!smvk::assets::LoadAssetManifest(stageSource, check, cerr)) {
			err = "validation failed — the staged manifest is missing or"
				" unparseable: " + cerr;
			return false;
		}
	}

	// ---- publish ------------------------------------------------------------
	// The derived family is REPLACED (a changed source may drop or rename
	// .material/.anim files — stale ones must not linger for cooks to
	// pick up); the source + dependency copies overwrite in place;
	// user-authored .texture files and anything unrecognized survive.
	fs::create_directories(destDir, ec);
	for (const fs::directory_entry& e :
			fs::directory_iterator(destDir, ec)) {
		if (ec || !e.is_regular_file(ec))
			continue;
		const std::string name = e.path().filename().generic_string();
		if (IsDerivedFamilyFile(name))
			fs::remove(e.path(), ec);
	}
	// RECURSIVE: dependency copies keep their relative subpaths
	// (a glTF referencing "shared/stone.png" stages it under
	// stage/<stem>/shared/ — the old flat iterator silently dropped
	// every subdirectory dep, leaving the published family with a
	// missing source dependency: permanently "stale" freshness + a
	// reimport that refuses on the absent file).
	for (const fs::directory_entry& e :
			fs::recursive_directory_iterator(stageRoot, ec)) {
		if (ec || !e.is_regular_file(ec))
			continue;
		const fs::path rel = fs::relative(e.path(), stageRoot, ec);
		if (ec || rel.empty())
			continue;
		const fs::path dest = destDir / rel;
		fs::create_directories(dest.parent_path(), ec);
		std::error_code moveEc;
		fs::rename(e.path(), dest, moveEc);
		if (moveEc) {
			// Cross-device or locked: fall back to copy+remove (per-file;
			// the stage retains a complete copy until the loop ends).
			if (!CopyFileTo(e.path(), dest, err))
				return false;
			fs::remove(e.path(), moveEc);
		}
	}
	std::filesystem::remove_all(stageRoot, ec);

	// ---- the registry (only after a successful publication) ---------------
	out.projectSource = projectSource;
	out.geometry = fs::path(projectSource).replace_extension(".geometry");
	out.guid = manifest.guid;
	out.geometryBlobs = stats.geometryBlobs;
	out.lodLevels = lodLevels;
	out.meshletSets = blobsWithMeshlets;
	out.derived = true;
	const std::string rel =
		request.project.RelativeTo(projectSource.generic_string());
	if (rel.empty()) {
		err = "internal: the published source is not project-relative: "
			+ projectSource.generic_string();
		return false;
	}
	if (!registry::UpdateRow(request.project, out, rel,
			src.generic_string(),
			smvk::assets::ImportFingerprintFor(request.settings))) {
		err = "import published but the registry write failed: "
			+ request.project.RegistryPath();
		return false;
	}
	return true;
}

std::string SeedProjectScene(const Project& project,
	const std::string& externalTour, const std::string& importedStem,
	std::string& err) {
	err.clear();
	if (!project.Valid()) {
		err = "invalid project";
		return {};
	}
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path src = fs::path(externalTour);
	if (!fs::is_regular_file(src, ec)) {
		err = "scene tour not found: " + externalTour;
		return {};
	}
	const fs::path destDir =
		fs::path(project.root) / "content" / "scenes";
	const fs::path dest = destDir / src.filename();
	if (fs::is_regular_file(dest, ec)) {
		// Authored scenes are never regenerated: an existing project
		// scene wins (the caller opens/builds it as-is).
		return project.RelativeTo(dest.generic_string());
	}
	// Read + re-point the root reference #0 at the project's imported
	// .geometry (content/models/<stem>/<stem>.geometry). The reference
	// must end up PROJECT-RELATIVE (an absolute path breaks the mesh-URI
	// machinery the pak serializer reads).
	const fs::path projectGeometry =
		fs::path(project.ImportsDir()) / importedStem
		/ (importedStem + ".geometry");
	const std::string projectRef =
		project.RelativeTo(projectGeometry.generic_string());
	std::ifstream in(src, std::ios::binary);
	std::ostringstream text;
	text << in.rdbuf();
	std::istringstream lines(text.str());
	std::ostringstream out;
	std::string line;
	bool repointed = false;
	while (std::getline(lines, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		// reference ... -source "<path>" ... (the root reference #0 is
		// the first `reference` line).
		if (!repointed
				&& line.compare(0, 10, "reference ") == 0) {
			const size_t at = line.find("-source \"");
			if (at != std::string::npos) {
				const size_t end = line.find('"', at + 9);
				if (end != std::string::npos) {
					// Same-stem .geometry reference -> the project copy;
					// a different target keeps its recorded form (it must
					// already be project-relative or the build reports
					// it).
					const std::string path =
						line.substr(at + 9, end - (at + 9));
					const std::string file =
						fs::path(path).filename().generic_string();
					if (file == importedStem + ".geometry")
						line = line.substr(0, at + 9) + projectRef
							+ line.substr(end);
					repointed = true;
				}
			}
		}
		out << line << "\n";
	}
	fs::create_directories(destDir, ec);
	std::ofstream of(dest, std::ios::binary | std::ios::trunc);
	if (!of) {
		err = "cannot write '" + dest.generic_string() + "'";
		return {};
	}
	of << out.str();
	if (!of.good()) {
		err = "failed writing '" + dest.generic_string() + "'";
		return {};
	}
	return project.RelativeTo(dest.generic_string());
}

} // namespace smvk::project
