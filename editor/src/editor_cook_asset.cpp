// Cook-asset creation (see EditorApp::CreateCookAsset): fresh UNBAKED
// .gicook/.lmcook manifests in the project, adopted into the assetdb.

#include "editor_app.h"

#include <filesystem>

#include <Engine/Asset/bake_asset.h> // bake-asset GUID discovery (FindBakeAssetGuid)

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
	if (m_hasGltfModel || !m_pakPath.empty())
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
		a.cvol = m_cook.giCvol;
		a.world = m_cook.giWorld;
		ok = smvk::cook::SaveGiCookAsset(path.generic_string(), a,
			werr);
	}
	else {
		smvk::cook::LightmapCookAsset a =
			smvk::cook::DefaultLightmapCookAsset(
				path.stem().string(), scene);
		a.guid = guid;
		a.lmMetersPerTexel = m_cook.lmMetersPerTexel;
		a.lmLobes = m_cook.lmLobes;
		a.lmAtlasWidth = m_cook.lmAtlasWidth;
		a.lmRays = m_cook.lmRays;
		a.lmProxyRes = m_cook.lmProxyRes;
		a.lmExcludeSmall = m_cook.lmExcludeSmall;
		a.lmExcludeKw = m_cook.lmExcludeKw;
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

void EditorApp::RecordBakeAssetsFromCook() {
	// The cook's bake outputs become/refresh BAKE ASSETS tied to this
	// scene + tour (runs on the main thread from PollCookProcess after
	// a successful cook — the GiBaker-from-0 stage already wrote the
	// fresh sidecars + their .meta identities under the content root):
	//   - the GiCook manifest (settings + the fresh .sggi result pointer)
	//     updated in place when this scene already owns one, else minted
	//     beside the content root + adopted into the assetdb;
	//   - the cubemap capture-set manifest adopted when this cook baked
	//     cubemaps (the cook minted its identity + wrote it loose);
	//   - the bake GUID REFERENCES into the tour (m_bakeGiCookGuid /
	//     m_bakeCubemapGuid — the editorSet keys ride every autosave, so
	//     the .tour and the assetdb always agree on which bake belongs
	//     to which definition).
	if (!m_hasGltfModel)
		return;
	const std::string scene = SceneIdForHost();
	if (scene.empty() || m_cook.outPak.empty())
		return;
	namespace fs = std::filesystem;
	std::error_code ec;
	// The sidecar gi/ dir: the content root next to the out pak (the
	// same derivation as Bake GI + the cooker's discovery).
	const std::string contentRoot = fs::absolute(
		fs::path(m_cook.outPak).parent_path() / "..")
		.lexically_normal().generic_string();
	const std::string giDir = (fs::path(contentRoot) / "gi")
		.generic_string();
	// World-field result of this cook ("" = giWorld off / not baked).
	const std::string sggiGuid = smvk::bake::FindBakeAssetGuid(giDir,
		smvk::bake::kBakeAssetKindGi, std::string("probe:") + scene);

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
	// Settings + result snapshot (the blob pointer only when this cook
	// baked a world field — cvol-only cooks keep a previous result
	// alive if one exists; the hash covers whatever is stored).
	a.scene = scene;
	a.cvol = m_cook.giCvol;
	a.world = m_cook.giWorld;
	if (!sggiGuid.empty()) {
		a.blobGuid = sggiGuid;
		a.blobPath = "gi/" + sggiGuid + ".sggi";
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
	m_bakeCubemapGuid.clear();
	if (m_cook.cubemap) {
		// The capture-set base: the authored name, else the pak stem
		// (the cook's default) — the manifest identity the cook minted.
		const std::string base = m_cook.cubemapName.empty()
			? fs::path(m_cook.outPak).stem().generic_string()
			: m_cook.cubemapName;
		const std::string cubesDir =
			(fs::path(contentRoot) / "textures" / "cubemaps")
				.generic_string();
		const std::string manifestGuid =
			smvk::bake::FindBakeAssetGuid(cubesDir,
				smvk::bake::kBakeAssetKindCubemap,
				std::string("cubemanifest:") + base);
		if (!manifestGuid.empty()) {
			const std::string manifestPath =
				(fs::path(cubesDir)
					/ (manifestGuid + ".cubemanifest"))
					.generic_string();
			AdoptProjectAsset(manifestPath,
				smvk::bake::kBakeAssetKindCubemap);
			m_bakeCubemapGuid = manifestGuid;
		}
	}
	Log("cook assets: GiCook '" + a.name + "' " + (fresh ? "created"
			: "updated") + (a.blobGuid.empty() ? std::string(" (no world")
			: std::string(" -> " + a.blobPath + " ("))
		+ " field baked this cook); cubemap asset "
		+ (m_bakeCubemapGuid.empty() ? std::string("none")
			: m_bakeCubemapGuid) + "; tour bake refs updated");
}

} // namespace editor
