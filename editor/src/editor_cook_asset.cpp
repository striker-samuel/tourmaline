// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Cook-asset creation (see EditorApp::CreateCookAsset): fresh UNBAKED
// .gicook/.lmcook manifests in the project, adopted into the assetdb.

#include "editor_app.h"

#include <filesystem>
#include <fstream>
#include <vector>

#include <Engine/Asset/bake_asset.h> // kBakeAssetKindCubemap + SplitManifestRow

namespace editor {

std::string EditorApp::CreateCookAsset(bool gi, const std::string& dir) {
	namespace fs = std::filesystem;
	if (m_projectRoot.empty()) {
		Log("New cook asset: open or create a project first (File > New / "
			"Open Project...) — cook assets live inside the project.");
		return {};
	}
	// Placement dir: the browser dir when it is a live project dir,
	// else the content home (same healing as DrawAssetsPanel).
	std::error_code ec;
	std::string target = dir;
	if (target.empty() || !fs::is_directory(fs::path(target), ec)
		|| !InProjectDir(target)) {
		target = (fs::path(m_projectRoot) / "content").generic_string();
		fs::create_directories(fs::path(target), ec);
	}
	const char* ext = gi ? smvk::cook::kGiCookExt
		: smvk::cook::kLightmapCookExt;
	const char* kind = gi ? smvk::cook::kCookAssetKindGiCook
		: smvk::cook::kCookAssetKindLightmapCook;
	const std::string base = gi ? "NewGiCook" : "NewLightmapCook";
	// Unique file name (never overwrite: NewGiCook.gicook, NewGiCook1…).
	fs::path path;
	for (int i = 0; i < 10000; ++i) {
		const std::string stem = base
			+ (i == 0 ? std::string() : std::to_string(i));
		path = fs::path(target) / (stem + ext);
		if (!fs::exists(path, ec))
			break;
		path.clear();
	}
	if (path.empty()) {
		Log("New cook asset: no free file name under '" + target + "'.");
		return {};
	}
	// Scene seed: the open scene's stem when one is open ("" = unbound —
	// the asset still creates; the scene binds at bake time).
	std::string scene;
	if (!SceneEmpty())
		scene = SceneIdForHost();
	const std::string guid = smvk::tour::GenerateUuid();
	std::string werr;
	bool ok = false;
	if (gi) {
		// Settings ride the CURRENT cook dialog (a fresh asset bakes
		// what the dialog would bake today).
		smvk::cook::GiCookAsset a =
			smvk::cook::DefaultGiCookAsset(
				path.stem().string(), scene);
		a.guid = guid;
		a.giVolumes = ExportCfg().giVolumes;
		// (The world-field flag is retired with the octree bake.)
		ok = smvk::cook::SaveGiCookAsset(path.generic_string(), a,
			werr);
	}
	else {
		smvk::cook::LightmapCookAsset a =
			smvk::cook::DefaultLightmapCookAsset(
				path.stem().string(), scene);
		a.guid = guid;
		a.lmMetersPerTexel = ExportCfg().lmMetersPerTexel;
		a.lmLobes = ExportCfg().lmLobes;
		a.lmAtlasWidth = ExportCfg().lmAtlasWidth;
		a.lmRays = ExportCfg().lmRays;
		a.lmProxyRes = ExportCfg().lmProxyRes;
		a.lmExcludeSmall = ExportCfg().lmExcludeSmall;
		a.lmExcludeKw = ExportCfg().lmExcludeKw;
		// Sun/sky snapshot from the ENV BLOCK (lux-native, one authored
		// sun — m_cook no longer carries them; the bake reads the same
		// env through the cooker's EnvForBake).
		const EditorEnv& env = CurrentEnv();
		a.sunAzimuth = env.sunAzimuth;
		a.sunElevation = env.sunElevation;
		a.sunIntensity = env.sunIntensity;
		a.skyIntensity = env.skyIntensity;
		a.sunColor = env.sunColor;
		ok = smvk::cook::SaveLightmapCookAsset(path.generic_string(),
			a, werr);
	}
	if (!ok) {
		Log("New cook asset: cannot write '" + path.generic_string()
			+ "': " + werr);
		return {};
	}
	// Identity: the .meta sidecar + assetdb row (kind gicook/lmcook, so
	// the rescan keeps the collection kinds — see BakeSidecarKind's
	// importer fallback).
	AdoptProjectAsset(path.generic_string(), kind);
	m_assetsDir = target;
	m_assetsSel = path.generic_string();
	Log(std::string(gi ? "GI" : "Lightmap") + " cook asset created: '"
		+ path.generic_string() + "' (unbaked — bake execution lands "
		"the blob result).");
	return path.generic_string();
}

namespace {
// Typed-asset placement: the GI folder (<project>/content/gi/) owns
// .cubemap/.givolume manifests (no loose bake files anywhere else).
std::string GiTypedDir(const std::string& projectRoot) {
	namespace fs = std::filesystem;
	std::error_code ec;
	fs::path d = projectRoot.empty()
		? fs::path("content") / "gi"
		: fs::path(projectRoot) / "content" / "gi";
	fs::create_directories(d, ec);
	return d.generic_string();
}
} // namespace

std::string EditorApp::CreateCubemapAsset(const std::string& dir) {
	namespace fs = std::filesystem;
	if (m_projectRoot.empty()) {
		Log("New cubemap asset: open or create a project first.");
		return {};
	}
	std::error_code ec;
	std::string target = dir;
	if (target.empty() || !fs::is_directory(fs::path(target), ec)
		|| !InProjectDir(target))
		target = GiTypedDir(m_projectRoot);
	const std::string base = "NewCapture";
	fs::path path;
	for (int i = 0; i < 10000; ++i) {
		const std::string stem = base
			+ (i == 0 ? std::string() : std::to_string(i));
		path = fs::path(target) / (stem + smvk::cook::kCubemapExt);
		if (!fs::exists(path, ec))
			break;
		path.clear();
	}
	if (path.empty()) {
		Log("New cubemap asset: no free file name under '" + target + "'.");
		return {};
	}
	std::string scene;
	if (!SceneEmpty())
		scene = SceneIdForHost();
	smvk::cook::CubemapAsset a =
		smvk::cook::DefaultCubemapAsset(path.stem().string(), scene);
	a.guid = smvk::tour::GenerateUuid();
	a.size = ExportCfg().cubemapSize;
	a.rays = ExportCfg().cubemapRays;
	a.capture = scene; // capture sets are NAMELESS (scene stem)
	std::string werr;
	if (!smvk::cook::SaveCubemapAsset(path.generic_string(), a, werr)) {
		Log("New cubemap asset: cannot write '" + path.generic_string()
			+ "': " + werr);
		return {};
	}
	std::string merr;
	if (!smvk::cache::WriteAssetMeta(path.generic_string(), a.guid,
			smvk::cook::kCookAssetKindCubemap, merr)) {
		Log("New cubemap asset: .meta write failed: " + merr);
		return {};
	}
	AdoptProjectAsset(path.generic_string(),
		smvk::cook::kCookAssetKindCubemap);
	m_assetsDir = target;
	m_assetsSel = path.generic_string();
	Log("Cubemap asset created: '" + path.generic_string()
		+ "' (unbaked).");
	return path.generic_string();
}

std::string EditorApp::CreateGivolumeAsset(const std::string& dir,
	uint32_t node) {
	namespace fs = std::filesystem;
	if (m_projectRoot.empty()) {
		Log("New GI volume asset: open or create a project first.");
		return {};
	}
	std::error_code ec;
	std::string target = dir;
	if (target.empty() || !fs::is_directory(fs::path(target), ec)
		|| !InProjectDir(target))
		target = GiTypedDir(m_projectRoot);
	const std::string base = "NewVolume";
	fs::path path;
	for (int i = 0; i < 10000; ++i) {
		const std::string stem = base
			+ (i == 0 ? std::string() : std::to_string(i));
		path = fs::path(target) / (stem + smvk::cook::kGivolumeExt);
		if (!fs::exists(path, ec))
			break;
		path.clear();
	}
	if (path.empty()) {
		Log("New GI volume asset: no free file name under '" + target + "'.");
		return {};
	}
	std::string scene;
	if (!SceneEmpty())
		scene = SceneIdForHost();
	uint32_t useNode = node;
	uint32_t priority = 0;
	if (m_world && useNode != UINT32_MAX) {
		auto it = m_world->scene.giVolumeForNode.find(useNode);
		if (it != m_world->scene.giVolumeForNode.end())
			priority = it->second.priority;
	}
	smvk::cook::GivolumeAsset a = smvk::cook::DefaultGivolumeAsset(
		path.stem().string(), scene, useNode);
	a.guid = smvk::tour::GenerateUuid();
	a.priority = priority;
	std::string werr;
	if (!smvk::cook::SaveGivolumeAsset(path.generic_string(), a, werr)) {
		Log("New GI volume asset: cannot write '" + path.generic_string()
			+ "': " + werr);
		return {};
	}
	std::string merr;
	if (!smvk::cache::WriteAssetMeta(path.generic_string(), a.guid,
			smvk::cook::kCookAssetKindGivolume, merr)) {
		Log("New GI volume asset: .meta write failed: " + merr);
		return {};
	}
	AdoptProjectAsset(path.generic_string(),
		smvk::cook::kCookAssetKindGivolume);
	m_assetsDir = target;
	m_assetsSel = path.generic_string();
	Log("GI volume asset created: '" + path.generic_string()
		+ "' (unbaked).");
	return path.generic_string();
}

void EditorApp::RecordBakeAssetsFromCook() {
	// The cook's bake outputs become/refresh BAKE ASSETS tied to this
	// scene + tour (runs on the main thread from PollCookProcess after
	// a successful cook — the GiBaker-from-0 stage already wrote the
	// fresh bake files + their .meta identities under the content root):
	//   - the GiCook manifest (settings + the result pointer for any
	//     legacy world record) updated in place when this scene already
	//     owns one, else minted beside the content root + adopted into
	//     the assetdb;
	//   - the cubemap capture-set manifest adopted when this cook baked
	//     cubemaps (the cook minted its identity + wrote it loose);
	//   - the bake GUID REFERENCES into the tour (m_bakeGiCookGuid /
	//     m_bakeCubemapGuid — the editorSet keys ride every autosave, so
	//     the .tour and the assetdb always agree on which bake belongs
	//     to which definition).
	// Runs only after a successful CookToPak (itself gated on a
	// non-empty definition scene); the scene + outPak checks below carry
	// every other caller path.
	const std::string scene = SceneIdForHost();
	if (scene.empty() || m_cook.outPak.empty())
		return;
	namespace fs = std::filesystem;
	std::error_code ec;
	// The bake gi/ dir: the content root next to the out pak (the
	// same derivation as Bake GI + the cooker's discovery).
	const std::string contentRoot = fs::absolute(
		fs::path(m_cook.outPak).parent_path() / "..")
		.lexically_normal().generic_string();
	// The bake artifacts carry RANDOM minted names — the tour's giAsset
	// records (the live-doc mirror, updated from the bake's stamp by the
	// completion handler BEFORE this runs) are the ONLY identity. The
	// registry's blob pointers read straight off them; the superseded
	// old artifacts were already deleted BY RECORD (the bake owner's
	// stamp did it — nothing left to derive here, no stems anywhere).
	const std::string worldBlob = [&] {
		for (const smvk::tour::GiAssetCmd& ga : m_giAssets)
			if (ga.kind == smvk::tour::GiAssetCmd::Kind::World)
				return ga.source;
		return std::string();
	}();
	const bool hasSggi = !worldBlob.empty()
		&& fs::is_regular_file(fs::path(contentRoot) / worldBlob, ec);

	// ---- GiCook manifest: this scene's asset, else a fresh mint ----
	smvk::cook::GiCookAsset a{};
	std::string aPath;
	bool fresh = true;
	{
		// Search the project content tree (the cook assets' home — see
		// CreateCookAsset) for this scene's .gicook.
		const fs::path contentDir = m_projectRoot.empty()
			? fs::path("content") : fs::path(m_projectRoot) / "content";
		std::vector<fs::path> stack;
		if (fs::is_directory(contentDir, ec))
			stack.push_back(contentDir);
		while (!stack.empty()) {
			const fs::path dir = std::move(stack.back());
			stack.pop_back();
			for (fs::directory_iterator it(dir, ec), end; it != end; ++it) {
				const fs::path& p = it->path();
				if (it->is_directory(ec)) {
					stack.push_back(p);
					continue;
				}
				if (p.extension().generic_string()
					!= smvk::cook::kGiCookExt)
					continue;
				smvk::cook::GiCookAsset t{};
				std::string lerr;
				if (smvk::cook::LoadGiCookAsset(p.generic_string(), t,
						lerr) && t.scene == scene) {
					a = std::move(t);
					aPath = p.generic_string();
					fresh = false;
					break;
				}
			}
			if (!fresh)
				break;
		}
		// Fresh mint: <content>/<scene>.gicook (suffixed until free).
		if (fresh) {
			a = smvk::cook::DefaultGiCookAsset(scene, scene);
			a.guid = smvk::tour::GenerateUuid();
			for (int i = 0; i < 10000; ++i) {
				const std::string stem = scene
					+ (i == 0 ? std::string()
						: std::to_string(i));
				const fs::path p = fs::path(contentDir)
					/ (stem + smvk::cook::kGiCookExt);
				if (!fs::exists(p, ec)) {
					a.name = stem;
					aPath = p.generic_string();
					break;
				}
			}
			if (aPath.empty()) {
				Log("cook assets: no free .gicook name under '"
					+ contentDir.generic_string() + "'");
				return;
			}
		}
	}
	// Settings + result snapshot (the blob pointer only while the tour
	// still carries a LEGACY world record from a pre-retirement bake —
	// the world bake is gone, so new cooks never mint one; a volume-only
	// cook keeps a previous record alive if it exists, and the hash
	// covers whatever is stored). The result pointer = the tour's world
	// giAsset record (the RANDOM minted artifact path; blobGuid mirrors
	// the record's artifact guid, empty = path-only bind). NO supersede
	// deletion here — the bake owner's stamp already retired the
	// replaced artifacts BY RECORD (exact set-difference; nothing to
	// re-derive).
	a.scene = scene;
	a.giVolumes = ExportCfg().giVolumes;
	if (hasSggi) {
		a.blobGuid.clear(); // path-only bind (the artifact guid)
		a.blobPath = worldBlob;
	}
	a.bakedHash = smvk::cook::GiCookSettingsHash(a);
	{
		std::string werr;
		if (!smvk::cook::SaveGiCookAsset(aPath, a, werr)) {
			Log("cook assets: GiCook save failed ('" + aPath + "'): "
				+ werr);
			return;
		}
	}
	// Identity + assetdb: the .meta carries the manifest's guid so
	// AdoptProjectAsset keeps it stable across rebakes (fresh mints
	// write it; updates find the existing one in place).
	if (fresh) {
		std::string merr;
		if (!smvk::cache::WriteAssetMeta(aPath, a.guid,
				smvk::cook::kCookAssetKindGiCook, merr)) {
			Log("cook assets: .meta write failed ('" + aPath + "'): "
				+ merr);
			return;
		}
	}
	AdoptProjectAsset(aPath, smvk::cook::kCookAssetKindGiCook);
	m_bakeGiCookGuid = a.guid;

	// ---- cubemap capture set (only when this cook baked cubemaps) ----
	// Typed asset (content/gi/<scene>.cubemap): no loose bake files are
	// written by the editor anymore. The legacy loose manifest
	// (textures/cubemaps/<base>.cubemanifest) is still adopted when
	// present (migration) so old cooks keep resolving.
	m_bakeCubemapGuid.clear();
	if (ExportCfg().cubemap) {
		// Legacy-manifest migration base: the old naming rule was the
		// out-pak stem (capture sets are NAMELESS now - scene stem).
		const std::string base =
			fs::path(m_cook.outPak).stem().generic_string();
		// Migrate/read the legacy loose manifest when the cooker wrote one.
		const std::string legacyCubesDir =
			(fs::path(contentRoot) / "textures" / "cubemaps")
				.generic_string();
		const fs::path legacyManifest =
			fs::path(legacyCubesDir) / (base + ".cubemanifest");
		if (fs::is_regular_file(legacyManifest, ec)) {
			AdoptProjectAsset(legacyManifest.generic_string(),
				smvk::bake::kBakeAssetKindCubemap);
		}
		// Typed asset: content/gi/<scene>.cubemap (one per scene).
		const fs::path giDir = m_projectRoot.empty()
			? fs::path("content") / "gi"
			: fs::path(m_projectRoot) / "content" / "gi";
		std::error_code gec;
		fs::create_directories(giDir, gec);
		const fs::path cmPath = giDir / (scene + smvk::cook::kCubemapExt);
		smvk::cook::CubemapAsset cm;
		bool cmFresh = true;
		{
			std::string lerr;
			if (fs::is_regular_file(cmPath, ec)
				&& smvk::cook::LoadCubemapAsset(cmPath.generic_string(),
					cm, lerr)
				&& cm.scene == scene)
				cmFresh = false;
			else {
				cm = smvk::cook::DefaultCubemapAsset(scene, scene);
				cm.guid = smvk::tour::GenerateUuid();
			}
		}
		cm.scene = scene;
		cm.size = ExportCfg().cubemapSize;
		cm.rays = ExportCfg().cubemapRays;
		cm.capture = base;
		// Payload pointer: the baked set for this scene (registry-owned
		// path under gi/; the loose ktx2 rows stay readable via VFS).
		cm.blobGuid = cm.guid;
		cm.blobPath = std::string("gi/") + scene + ".cubedata";
		cm.bakedHash = smvk::cook::CubemapSettingsHash(cm);
		{
			std::string werr;
			if (smvk::cook::SaveCubemapAsset(cmPath.generic_string(), cm,
					werr)) {
				if (cmFresh) {
					std::string merr;
					smvk::cache::WriteAssetMeta(cmPath.generic_string(),
						cm.guid, smvk::cook::kCookAssetKindCubemap, merr);
				}
				AdoptProjectAsset(cmPath.generic_string(),
					smvk::cook::kCookAssetKindCubemap);
				m_bakeCubemapGuid = cm.guid;
			}
			else {
				Log("cook assets: Cubemap save failed ('"
					+ cmPath.generic_string() + "'): " + werr);
			}
		}
	}
	// ---- GI-volume assets (one typed .givolume per authored node) ----
	// Each GI volume node owns its bake pointer; the cook refreshes the
	// settings snapshot + baked hash (payload e.g. gi/<scene>_<i>.givolume).
	if (m_world && !m_world->scene.giVolumeForNode.empty()) {
		const fs::path giDir = m_projectRoot.empty()
			? fs::path("content") / "gi"
			: fs::path(m_projectRoot) / "content" / "gi";
		std::error_code gec;
		fs::create_directories(giDir, gec);
		for (const auto& [node, def] : m_world->scene.giVolumeForNode) {
			const std::string stem =
				scene + "_givolume" + std::to_string(node);
			const fs::path cvPath =
				giDir / (stem + smvk::cook::kGivolumeExt);
			smvk::cook::GivolumeAsset cv;
			bool cvFresh = true;
			{
				std::string lerr;
				if (fs::is_regular_file(cvPath, ec)
					&& smvk::cook::LoadGivolumeAsset(
						cvPath.generic_string(), cv, lerr)
					&& cv.scene == scene)
					cvFresh = false;
				else {
					cv = smvk::cook::DefaultGivolumeAsset(
						stem, scene, node);
					cv.guid = smvk::tour::GenerateUuid();
				}
			}
			cv.scene = scene;
			cv.node = node;
			cv.priority = def.priority;
			// The bake pointer = the LIVE binding (the scene's
			// authoritative giAsset record — the same path the pak
			// binds + the preview reads; empty = never baked).
			cv.blobGuid = def.bakeGuid;
			cv.blobPath = def.bakeSource;
			cv.bakedHash = smvk::cook::GivolumeSettingsHash(cv);
			std::string werr;
			if (smvk::cook::SaveGivolumeAsset(cvPath.generic_string(),
					cv, werr)) {
				if (cvFresh) {
					std::string merr;
					smvk::cache::WriteAssetMeta(cvPath.generic_string(),
						cv.guid,
						smvk::cook::kCookAssetKindGivolume, merr);
				}
				AdoptProjectAsset(cvPath.generic_string(),
					smvk::cook::kCookAssetKindGivolume);
			}
		}
		MarkOctreeDirty(); // baked output moved -> precook grid is stale
	}
	Log("cook assets: GiCook '" + a.name + "' " + (fresh ? "created"
			: "updated") + (a.blobGuid.empty() ? std::string(" (no world")
			: std::string(" -> " + a.blobPath + " ("))
		+ " field baked this cook); cubemap asset "
		+ (m_bakeCubemapGuid.empty() ? std::string("none")
			: m_bakeCubemapGuid) + "; tour bake refs updated");
}

} // namespace editor
